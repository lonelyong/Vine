# graphics material: one PBR model, a texture set, instances

> **Status**: current (M1–M3 landed, M4 not done, see §9.1/§9.2)
> **Last verified**: 2026-11-23
> **Scope**: the material data model, the texture set and sampling, instance overrides, upload dedup, forward/deferred consistency;
> not the material DSL, runtime name reflection, bindless or IBL.
> **Sources**: `Material.hpp` / `ShaderAbi.hpp` / `shaders/vine_params.glsl` (the material block), `vine_material.glsl`
> (slot → surface), `vine_brdf.glsl` (BSDF), `vine_gbuffer.glsl` (G-buffer packing); the instance types land with M4.
> **Backlog**: `../backlog/graphics.md` (M4)

## 1. Overview

### 1.1 Problem and background

- Before this design `vn::graphics::Material` was "flat Phong + one texture": ambient / diffuse / specular / shininess,
  material alpha was only carried and never read; imported PBR assets were approximated as Phong in `materialFrom`.
- No instances: per-object highlighting / tint could only replace the whole material (`ModelRenderView::selectAt` clones one),
  and a change on a shared material changed every user at once.
- Both of backlog T16's triggers have appeared: a real PBR need (imported assets carrying metallic/roughness),
  and a second call site for per-object parameters (model_viewer's highlight, test_plugin's animated colour).

### 1.2 Goals

- One shading model: metallic-roughness PBR; Phong exists only at the import boundary.
- Two levels, asset / instance: textures and factors exist once, an instance overrides only the items it changes.
- A texture set: five semantic slots, each with sampling state and a colour-space contract.
- forward and deferred read the same material bytes and run the same BRDF.
- Transparency has one model: the mode on the material, the magnitude on the drawable; sorting and shading read the same pair of inputs.

### 1.3 Non-goals

- No material DSL, no runtime name reflection (`graphics-shader-params.md`'s S2/S3 stay separate triggers).
- No bindless / texture arrays / descriptor indexing.
- No per-material program: program resolution stays "primitive > geometry default > nearest StateNode > pass".
- No second UV set, no tangent vertex channel (normal mapping builds the tangent basis from screen-space derivatives).
- No IBL / environment probes: the output is still analytic lights + ambient.

### 1.4 One-paragraph conclusion

> A material is a set of PBR factors + a texture set; an instance is a sparse override on top of it; both paths read the same bytes and run the same BRDF.

## 2. Design Points

2.1 **One shading model** (`G1` `G6`): metallic-roughness (glTF 2.0's core vocabulary: base color / metallic / roughness /
emissive / occlusion / normal). Phong is no longer the engine model; `vn::geom::spatial::PhongMaterial` is converted to
PBR at the import boundary by a fixed table, and the engine has one semantics inside. Both paths, the importers and the tests only need to agree on one definition; the price is that the old setters disappear (see §9.3).

2.2 **Asset and instance are two types** (`G1` `G4`): `Material` is the asset — shareable, editable, a change propagates to every instance
that does not override it; `MaterialInstance` references an asset and overrides only the items it changes. They share the base class `MaterialValues`,
and `DrawItem` / `Geometry` / `Primitive` / `StateNode` hold a `MaterialValuesPtr`, so "what can shade one draw" is one type. "One class + an optional parent" (D2) is rejected: it makes "does changing the asset affect whom" runtime data instead of a type.

2.3 **Overrides are sparse and explicit** (`G4` `G8`): every item of an instance is optional, `clearOverride(x)` restores inheritance; the resolution result
is cached in the instance, keyed by (parent revision, own revision). Resolve once, read many times, an overridden item is not copied.

2.4 **The texture set is five semantic slots** (`G1` `G9`): base_color / metallic_roughness / normal / occlusion / emissive,
each slot = (`Texture` + `Sampler` + present or not). The slot's identity decides binding and colour space; the host may not invent names. Only
base_color may be Cube (following "the sampler kind follows the geometry texcoord width"); the other four take D2 only. The UV set is fixed at 0,
a second UV is a geometry-format change, not a material change.

2.5 **The variant is "is there a texture set", not "which slots are there"** (`G10`): one boolean bit `VN_TEXTURED`. With any slot present the program
declares all five samplers, **whether a slot exists is short-circuited per slot by the flags in the block**: a missing slot is not sampled, a constant is used per slot (base / MR / AO multiply 1,
normal flat, emissive black). The bound stand-in exists only for "the descriptor is legal + the kind is right"; its **value** takes no part in the picture. A per-slot bit would give
2^5 combinations × two paths × shadow variants, blowing up the pipeline cache and `vn_shader_check.sh`'s combination gate; the binary bit keeps "textureless content has zero samplers".

2.6 **One BRDF, two paths** (`G1` `G5`): `shaders/vine_brdf.glsl` is the only implementation of GGX / Smith / Schlick:
forward calls it directly; deferred first reconstructs the same inputs from the G-buffer and then calls it. `vnEvaluateLight` stays the only light implementation.
The translation from slots to surface inputs (channel packing, missing-slot constants, normal tangent basis) is one contract too, its one home is `vine_material.glsl`,
and both surface-writing stages (forward / G-buffer) include it.

2.7 **Transparency: the mode on the material, the magnitude on the drawable** (`G1` `G4`): `AlphaMode::Opaque | Mask | Blend`; effective alpha =
`vn_draw.params.x` (drawable opacity) × `vn_material.base_color.a` × the optional base texture alpha.
`DrawItem::isTransparent()` becomes "opacity < 1 **or** the material is Blend"; Mask discards by cutoff in the shading stage,
takes no part in sorting and stays in the opaque pass. This replaces the old "materials contribute no alpha" rule, so sorting and shading read the same pair of inputs (see N5).
Blend content goes through the forward overlay; the deferred G-buffer takes opaque and Mask only.

2.8 **The G-buffer is a projection of the material** (`G10`): the four attachments hold exactly the per-pixel quantities — A0 base colour.rgb (a writes 1),
A1 octahedral normal.rg + roughness + metallic, A2 emissive.rgb + occlusion, A3 view position.xyz + write mask.
Octahedral encoding squeezes a 3-component normal into 2 channels, and the two freed slots hold exactly the two factors; f0 is **not stored**, both paths derive it from base and metallic.
The single home of the packing contract is `shaders/vine_gbuffer.glsl` (the write side encodes and the read side decodes in the same file). A2 is RGBA16F,
because emissive is HDR. Ambient is scaled only by occlusion (direct light is not), emissive is added once after the lights and is not shadowed.

2.9 **Colour space on the slot** (`G4`): base_color / emissive must be sRGB formats (hardware decode),
metallic_roughness / normal / occlusion must be linear formats; a slot with the wrong format is reported once per episode, no implicit gamma.
This follows `Colorf`'s global linear-light convention.

2.10 **A material carries no program** (`G5`): program resolution stays as it is; a material only provides parameters and a texture set. A user program reads the same
`VineMaterialBlock`, no second material interface is introduced.

2.11 **Upload by identity + composite revision** (`G10`): the material arena still deduplicates by object address + revision (`block_bytes` is still
64 B, see F4); an instance's revision = the parent revision combined with its own revision, so a parent asset change automatically rewrites every instance once.
An instance copies neither textures nor bytes beyond the block.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | `Material` asset | PBR factors + texture set + alpha mode; setters only move the revision | done |
| F2 | `MaterialInstance` | References one asset, sparsely overrides factors / textures / alpha | done |
| F3 | Texture set | Five semantic slots + a `Sampler` per slot + kind/format validation | done (format checks with F11) |
| F4 | Material block v2 | Still 64 B: base_color / emissive / params / flags | done |
| F5 | forward PBR | GGX direct light + ambient, including normal/ORM/emissive slots | done |
| F6 | deferred PBR | G-buffer reconstruction + the same BRDF + emissive/occlusion | done |
| F7 | alpha modes | Sorting, discard and output alpha for Opaque / Mask / Blend | done |
| F8 | import conversion | asset `Color/Phong/Pbr` → engine PBR, approximations removed | done |
| F9 | variant | `VN_TEXTURED` binary bit + constant fallbacks | done |
| F10 | upload dedup | composite instance revision; changing the asset rewrites instances | done |
| F11 | diagnostics | missing slot / wrong format / Cube in a non-base slot / invalid override | done (missing-slot count; the rest once per material) |

### 3.2 F1–F2 asset and instance

- **Behaviour**: `Material`'s fields = base_color (linear RGBA), emissive (RGB + intensity), metallic, roughness,
  occlusion, alpha_mode, alpha_cutoff, five texture slots; `MaterialInstance` = one `Material` parent + optional versions of the same fields
  (`MaterialItem` one bit per item, one bit per texture slot). Both answer through `MaterialValues::resolved()`.
- **Constraint** (C1): an instance has one parent, and the parent cannot be an instance (the parent's type is `Material`); `G1` one definition.
- **Requirement** (R1): the resolution result = the parent's values replaced item by item, giving one `MaterialParameters`; the resolution cache is keyed by
  (parent revision, own revision), a hit compares two numbers only.
- **Acceptance** (T1): the parent changes the base colour and the instance overrides roughness — the instance is the combination of the two, the non-overridden items follow the parent.
- **Caveat** (N1): an override must not hold a second texture reference without entering the revision; `setTexture(slot, ...)` and
  `clearOverride` both move the revision.

### 3.3 F3 texture set

- **Behaviour**: `enum class TextureSlot` (five slots) + `TextureSlotBinding { TexturePtr texture; Sampler sampler{}; }`; bindings fixed
  (content set 0: material 0, five slots 1–5, lights 6, shadow 7/8); `metallic_roughness` packed per glTF (G = roughness,
  B = metallic), `occlusion` takes R.
- **Constraint** (C2): a slot's kind and format follow §2.9; when illegal, report once per F11 and **do not** silently substitute a fallback image (the fallback is only for
  "the slot is absent", never for "the slot is wrong").
- **Requirement** (R2): each slot's `Sampler` enters `MaterialImages::acquire(texture, sampler, why)`'s by-value cache, equal state shares the
  sampler object; a `Sampler` containing NaN misses every frame (materials do no numeric cleaning, `Sampler.hpp`).

### 3.4 F4–F6 block and the two paths

- **Behaviour**: the block is still 64 B, pinned by `static_assert` on offsets:

  | Offset | Member | Contents |
  |---|---|---|
  | 0 | `base_color` | rgb linear + a = material alpha |
  | 16 | `emissive` | rgb + a = intensity |
  | 32 | `params` | x metallic, y roughness, z occlusion, w alpha_cutoff |
  | 48 | `flags` | `uvec4`: bit0–4 = slot present; bit8–9 = alpha mode |

- **Constraint** (C3): the block is packed by `blockOfMaterial` in one place; an instance resolves before packing (R1).
- **Requirement** (R3): forward and deferred get the same picture from the same block; there is one GLSL each for the BRDF and for light evaluation.
- **Acceptance** (T3): with the same material, lights and camera, forward and deferred differ by ≤ quantisation error at the same pixel (read-back comparison).
- **Behaviour** (F5/F6): the diffuse term carries no 1/π — light intensity is the engine's own unit, and swapping the BRDF should not darken existing scenes by a factor of π (`G6`).
  roughness is a **perceptual value**, alpha = roughness²; the normal slot builds its tangent basis from screen-space derivatives in the UV channel and flips green after sampling (glTF is
  OpenGL-oriented, this engine's v points down); a Cube material has no UV and samples the base slot only; ambient is scaled only by occlusion, emissive is not shadowed.

### 3.5 F7 alpha modes

- **Behaviour**: Opaque ignores alpha; Mask `discard`s after the base sample; Blend outputs a and enters the transparent queue. The sort key
  = opacity < 1 or the mode is Blend (§2.7).
- **Constraint** (C4): Mask cannot enter the transparent queue (otherwise sorting writes into the overlay what should be opaque, losing self-occlusion).
- **Requirement** (R4): `vn_draw.params.x` is still the only per-drawable magnitude; the material does not copy it.
- **Acceptance** (T4): a Mask leaf discards correctly on both sides of the cutoff; a Blend leaf blends by opacity × material alpha.
- **Caveat** (N4): the deferred G-buffer does not write Blend content; it can only appear in the forward overlay.
- **Caveat** (N6): on a MULTISAMPLED target a Mask fragment still discards whole fragments, so its cutout edges keep
  their pixel steps. The mechanism is alpha-to-coverage - one switch (`VkPipelineMultisampleStateCreateInfo::
  alphaToCoverageEnable`, dynamic under the EDS3 floor this tree already requires) - and **the switch alone is not the
  feature**: coverage is derived from the alpha of the fragment's FIRST colour output, and `builtin_forward.frag` writes
  the drawable's opacity there, not the masked alpha. Landing it is therefore three parts: the forward program's Mask
  branch writes `opacity × surface alpha` as its output alpha when `gl_NumSamples > 1`; the seam carries "this draw's
  alpha is a coverage fraction" beside `blend`; and the backend states the switch per draw, where it states
  `blendEnable`. **It cannot serve the deferred G-buffer**, whose attachment 0 alpha is the occlusion the lighting pass
  scales - Mask content on a multisampled G-buffer keeps binary edges by construction. The shader-side `gl_SampleMask`
  sketch is WITHDRAWN: it existed only because the retired vsg path overrode `MultisampleState` after the application, <!-- drift-ok -->
  it costs a variant bit plus a per-sample write, and every API family this seam serves has the switch - a workaround no
  second family would spell (`G6` / `G9`). Trigger unchanged: the first Mask-on-MSAA workload; backlog `T58` keeps it,
  and `ADR-2026-11-23` D4's amendment records the decision.

### 3.6 F8 import conversion

- **Behaviour**: `ColorMaterial` → base=color, metallic 0, roughness 1; `PhongMaterial` → base=diffuse,
  `roughness = sqrt(2 / (shininess + 2))` (the standard Blinn-Phong to GGX mapping), specular folded into metallic;
  `PbrMaterial` → field by field, no approximation any more.
- **Specification**: `metallic = clamp(max(specular.rgb) × clamp(specular.a, 0, 1), 0, 1)` — metallic-roughness has no separate reflectance knob, a strong or coloured specular is exactly what "metal" means (the strongest channel = the reflection peak, alpha = intensity); the price is in D7.
- **Caveat** (N5): this mapping assumes Phong's specular is **authored**. The default material an importer fabricates for "the file has no material" (assimp's `AI_DEFAULT_MATERIAL_NAME`, white specular) is treated as "no material" on the loading side (`meshio-brepio-design.md` F2), so it never reaches here; otherwise white specular would fold to metallic 1, and `builtin_forward.frag` multiplies the ambient term by `(1 - metallic)`, leaving the model with specular only and looking black in a scene without an environment map.
- **Constraint** (C5): the conversion happens in one place, `SceneBuild.cpp`; the engine side has no "the source was Phong" branch.
- **Acceptance** (T5): import a glTF PBR asset, base/metallic/roughness/emissive match the source file field by field.

### 3.7 F9–F11 variants, upload and diagnostics

- **Behaviour**: `ProgramVariant` gains `textured` (any slot present); the `diffuse_map` bit is replaced by `textured`.
  An instance's composite revision makes the arena rewrite; diagnostics follow `DiagnosticsHandle`'s episode rule.
- **Constraint** (C6): `vn_shader_check.sh`'s combination count = existing bits × 1, it does not grow with the slot count;
  the five sampler declarations `ProgramAbi` scans appear with `VN_TEXTURED`.
- **Acceptance** (T6): `EmbeddedShadersTest`'s variant matrix and `ProgramAbiTest`'s binding counts are updated together.

## 4. Public Interface

### 4.1 `MaterialValues` / `Material` / `MaterialInstance`

| Interface | Contract |
|---|---|
| `Material::setBaseColor` / `setEmissive` / `setMetallic` / `setRoughness` / `setOcclusion` | Writes and moves the revision; equal values do not move it |
| `Material::setAlphaMode` / `setAlphaCutoff` | Mode and cutoff; affects Mask/Blend only |
| `Material::setTexture(slot, texture, sampler)` / `clearTexture(slot)` / `texture(slot)` | Texture set |
| `MaterialInstance::parent()` / `overrideMask()` / `clearOverride(item)` | Sparse override |
| `MaterialValues::revision()` / `name()` / `resolved()` | The asset's own revision; an instance's composite revision and resolved values |
| `MaterialValuesPtr` | The unified type held by `DrawItem` / `Geometry` / `Primitive` / `StateNode` |

### 4.2 Shading side

| Interface | Contract |
|---|---|
| `VineMaterialBlock` (64 B) | See F4's offset table; `operator==` stays default |
| `vine_material.glsl` | `VineSurface vnSampleMaterial(vn_n, vn_position, vn_uv)`: five slots → surface; a missing slot short-circuits by flags |
| `vine_brdf.glsl` | `vnEvaluateBRDF(vn_base_color, vn_metallic, vn_roughness, vn_n, vn_v, vn_l)`: no radiance and no cosine (the caller multiplies) |
| `vine_gbuffer.glsl` | `vnOctahedralEncode` / `vnOctahedralDecode` and the four attachments' packing comments; `VN_TEXTURED` declares the five slot samplers |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | An instance has one parent and the parent cannot be an instance; resolution is one `MaterialParameters`. |
| C2 | The texture slot's kind / format contract (§2.9) is checked by the material, an error does not fall back to a fallback image. |
| C3 | The block is packed only by `blockOfMaterial`, an instance resolves first. |
| C4 | Mask does not enter the transparent queue; Blend goes through forward only. |
| C5 | The import conversion happens in one place, `SceneBuild.cpp`. |
| C6 | The variant count does not grow with the texture-slot count. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | The instance resolution cache is keyed by (parent revision, own revision); a parent change invalidates it automatically. |
| R2 | Samplers are cached by `Sampler` value; equal state shares one sampler object. |
| R3 | forward and deferred share one BRDF and one light evaluation. |
| R4 | The per-drawable transparency magnitude has one source, `vn_draw.params.x`; sorting and the blend switch read **the same** classification result. |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | Override items and texture slots must enter the revision, otherwise an instance change is not uploaded. |
| N2 | A `Sampler` containing NaN breaks the by-value cache; materials do no numeric cleaning. |
| N3 | The G-buffer's colour attachments are HDR; 8-bit would clip emissive. |
| N4 | deferred takes Opaque / Mask only; Blend can appear only in the forward overlay. |
| N5 | The split between material alpha and drawable opacity is "mode / magnitude", do not substitute one for the other. |
| N6 | base_color may be Cube, the other four slots may be D2 only; this is a kind contract, not an option. |

## 6. Caveats and Traps

| # | Trap | How it fails | How to detect it | How to prevent it |
|---|---|---|---|---|
| P1 | Material alpha and drawable opacity written twice | The host treats material alpha as opacity, sorting and shading each read one | Transparent in forward, opaque in deferred, or an object with alpha = 1 in the transparent queue | §2.7's formula is the only entry, `isTransparent()` reads both inputs |
| P3 | Instance revision not following the parent | After an asset change the instance still draws the old values, with no log | `resolved()`'s cache key misses the parent revision | R1; a test pins "parent changes → instance rewrites once" |
| P4 | Normal map without a tangent | The tangent basis is wrong, the normal map's direction rotates as a whole and the lighting looks "off" | A flat normal map should not change the appearance; a change means a wrong basis | Build the basis from screen-space derivatives (dFdx/dFdy of position and UV), one GLSL implementation |

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Keep Phong and only add instances | The real PBR need has appeared and Phong cannot express metallic/roughness; see [ADR](../decisions/ADR-2026-10-24-material-model.md). |
| D2 | One class + an optional parent node (no `MaterialInstance` type) | Override semantics become runtime data and the host cannot answer "which items did I change"; `G1` `G4`. |
| D3 | Per-slot variant bits | 2^5 combinations × paths × shadows; `G10`. |
| D4 | Tint through the draw block's user slot only, no instances | No texture/alpha override, so after the second call site the material still gets cloned; `G8` counter-evidence. |
| D5 | bindless / texture arrays / descriptor indexing | No consumer today; `G8`. |
| D6 | A name table as the material core (`set("BaseColor", ...)`) | Runtime lookup, no type safety; S2/S3 stay separate; `G4` `G8`. || D7 | Keep Phong's specular as F0 (Blender / Godot's approach) | It needs one more reflectance input on the material and in the block, while the engine has one reflection path today; the cost of folding into metallic (a typical `Ks 0.5` becomes half metal) is carried by F8's fixed table; `G8`. |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | Instance resolution: overridden and inherited items combine correctly | `MaterialInstanceTest.*` |
| T2 | Samplers shared by value, a texture uploaded once | `VkContentDrawTest.ABaseColorTextureIsUploadedAndSampled` / `ACubeMapIsUploadedWithEveryFaceAndSampled` |
| T3 | The two paths shade one block (each path's pixels are checked; the same-pixel comparison itself has no case) | `ShaderAbiTest.TheMaterialBlockIsThePbrRowsAndEqualityCoversThem` + `VkContentDrawTest.ATriangleIsDrawnIntoTheTargetAndProbed` (forward) and `VkDeferredTest.TheDeferredFrameLightsTheGbuffer` (deferred) |
| T4 | Mask discard and Blend sorting | `ShaderAbiTest.TheAlphaModeDecidesWhatTheMaterialsAlphaMeans` (the ABI) + `VkContentDrawTest.ABlendedDrawFollowsTheEngineOrder` (blend, pixel-checked) |
| T5 | Import matches field by field | `SceneBuildTest.TheThreeSourceMaterialsBecomePbrFactorsAndPbrOpacityBecomesLeafOpacity` |
| T6 | Variant matrix and binding counts in step | `EmbeddedShadersTest` / `ShaderAbiTest` |
| T7 | The slot contract's decisions and reporting rhythm | `MaterialSlotCheckTest.*` / `ReportOnceTest.OnlyTheFirstCallOfAnEpisodeReports` |

## 9. Status and Evolution

### 9.1 Current status

**M1–M4 have all landed** (M4 = instances + alpha modes), only F11's per-material diagnostics are left: `Material` /`MaterialInstance`
share `MaterialValues` and `MaterialParameters`, every holder uses `MaterialValuesPtr`; the block and the content-set bindings are unchanged since M1–M3 (material 0 /
slots 1–5 / lights 6 / shadow 7–8, `VN_TEXTURED` one bit), shading is GGX / Smith / Schlick (`vine_brdf.glsl`),
forward and deferred reconstruct the same set of surfaces from their own inputs and then call the same function. All three alpha branches are in the shading: Opaque ignores material alpha, Mask clips by
`opacity × material α × texture α` and is then treated as opaque, Blend uses that product as output alpha, and sorting and the blend switch read the same classification result.

### 9.2 Known gaps / backlog

- The landing took four steps: M1–M4 have all landed (F1/F3–F10); F11's per-material diagnostics are wired too (the decision is in `MaterialSlotCheck`,
  the episode is one record per material in `ContentStore`, re-armed after a change).
- The derivative tangent basis under mirrored UVs is a known approximation, not a bug; introduce a tangent channel when needed.
- IBL, SSAO and transmission (KHR_materials_transmission) are outside this design.

### 9.3 Compatibility / migration

- `setDiffuse` → `setBaseColor`, `setAmbient` / `setSpecular` / `setShininess` deleted; `setTexture(t)` → `setTexture(TextureSlot::BaseColor, t, {})`, `texture()` → `texture(slot)`.
- `MaterialPtr` becomes `MaterialValuesPtr` at every holder (already landed: `DrawItem` / `Geometry` / `Primitive` / `StateNode`
  and `effectiveMaterial`); the `vn::geometry` side is unchanged. An instance uses `MaterialInstance(parent)` + `setX` / `clearOverride`.

## 10. References

### 10.1 Code

- `src/viz/graphics/sdk/vine/graphics/MaterialValues.hpp` · `Material.hpp` · `MaterialInstance.hpp` · `ShaderAbi.hpp`
- `shaders/vine_params.glsl` · `vine_material.glsl` · `vine_brdf.glsl` · `vine_gbuffer.glsl`
- `shaders/builtin_forward.frag` · `builtin_gbuffer.frag` · `builtin_deferred_lighting.frag`
- `MaterialAbi.hpp` (`blockOfMaterial` and the block's offsets) · `ContentImages.hpp` (F11's slot decision) · `ProgramVariant.hpp` (the SDK's variant rule) · `SceneBuild.cpp` (`materialFrom`)

### 10.2 Documents, ADRs and external sources

- `graphics-shader-params.md` · `graphics-rendering.md` · `graphics-scene.md` · `graphics-content-lifecycle.md`
- [ADR-2026-10-24-material-model](../decisions/ADR-2026-10-24-material-model.md)
- glTF 2.0's material (metallic-roughness, textureInfo's texCoord/sampler); Filament's material/instance split, sampler parameters and the Smith/Schlick form of Cook-Torrance GGX.
