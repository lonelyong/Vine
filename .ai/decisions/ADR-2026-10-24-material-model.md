# ADR-2026-10-24: materials become one metallic-roughness PBR model plus sparse instances

- Status: Accepted
- Date: 2026-10-24
- Related: `../design/graphics-material.md`, `../backlog/graphics.md` (T16)

## Background

The engine's material was "tiled Phong plus one texture": `ambient / diffuse / specular / shininess`, with the material alpha carried but never read; an imported PBR asset was approximated as Phong in `ModelScene.cpp::materialFrom` (metallic folded into diffuse, roughness into shininess and specular intensity). With no instances, per-object highlight and tint could only clone the whole material (`ModelRenderView::selectAt`).

Both triggers of backlog T16 had appeared:

1. a real PBR need: imported assets carry base colour / metallic / roughness / emissive, and the approximation is visible on metals and rough surfaces;
2. a second call site for per-object parameters: the model viewer's selection highlight and test_plugin's per-frame animation colour.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep Phong, add only `MaterialInstance` | The smallest change; the G-buffer and both paths stay | Real PBR still cannot be expressed; the approximation stays in the importer forever; two material semantics |
| B. Two models (a material is either Phong or PBR) | Old assets need no migration | Two blocks, two G-buffer interpretations, two paths x two models; `G1` and `G6` suffer directly |
| C. One metallic-roughness PBR model plus `MaterialInstance` and an alpha mode | One semantics, one BRDF for both paths; instances remove the cloning | A large public API change; the transparency rules are rewritten; the G-buffer is reordered |
| D. One `Material` class with an optional parent node (no instance type) | One type fewer | "Which items are overridden" and "who is affected by an asset edit" become runtime data; `G1`, `G4` |
| E. A variant bit per texture slot | No texture slot costs nothing | 2^5 combinations x path x shadow, which explodes the pipeline cache and the gate matrix; `G10` |

## Decision

Chose **C**, with three accompanying decisions:

1. **One shading model**: metallic-roughness PBR is the engine's only material semantics. `vn::geometry::PhongMaterial` is converted through a fixed table at the import boundary, and there is no Phong branch inside the engine.
2. **Two levels, asset and instance**: `Material` (the asset) and `MaterialInstance` (a sparse override) share `MaterialBase`; `RenderCommand` holds a `MaterialBasePtr`. A resolved instance is cached by (parent revision, own revision).
3. **A texture set plus one binary variant**: five semantic slots (base_color / metallic_roughness / normal / occlusion / emissive), each with a `Sampler`; the single `VN_TEXTURED` bit decides whether the five samplers are declared, and a missing slot binds a typed constant fallback.
4. **Transparency**: `AlphaMode::Opaque | Mask | Blend` lives on the material; the magnitude is still the drawable's opacity; the effective alpha is the product of the two and the base texture's alpha; `isTransparent()` reads both inputs. This replaces the old rule "a material contributes no alpha".
5. **G-buffer**: the four attachments are reordered to albedo / octahedral normal plus roughness and metallic / emissive plus occlusion / view position plus mask; A2 moves to RGBA16F.

## Consequences

- Good:
  - One material semantics runs through import, collection, both shading paths and the tests (`G1`, `G6`).
  - Per-object highlight and tint no longer clone the material, and a texture is stored once (`G10`).
  - The number of variants does not grow with the number of texture slots (`G10`), and untextured content keeps the zero-sampler fast path.
  - Sorting and shading read the same pair of transparency inputs, removing the latent "transparent but unsorted" (`G4`).
- Bad / cost:
  - `setDiffuse / setAmbient / setSpecular / setShininess` are deleted, so the demo, the tests and model_viewer have to migrate.
  - The deferred G-buffer layout changes, so `builtin_gbuffer.frag`, `builtin_deferred_lighting.frag` and the binding counts in `ProgramAbi` all change with it.
  - A textured material always occupies five samplers (a missing slot binds the fallback), four bindings more than today's "only the base slot declares a sampler".
  - The normal map relies on a screen-space derivative tangent basis, which is a known approximation under mirrored UVs.
- Trigger (when to reopen):
  - descriptor pressure or binding count of textured materials is measured to be the bottleneck -> reconsider per-slot variants or bindless;
  - IBL or environment probes are needed -> the occlusion/emissive G-buffer channels may need another pass;
  - a real consumer of KHR_materials_unlit assets appears -> discuss again whether the shading model is a material property.

## Evidence

- Today's approximation: the `MaterialType::Pbr` branch of `src/viz/graphics/src/ModelScene.cpp` (metallic -> a reduced diffuse, roughness -> shininess and specular intensity).
- The cloning site for per-object highlight: `selectAt` in `src/plugins/model_viewer/src/ModelRenderView.hpp`.
- One texture and a 64 B block: `src/viz/graphics/sdk/vine/graphics/Material.hpp`, `VineMaterialBlock` in `ShaderAbi.hpp`, `shaders/vine_params.glsl`.
- The variant mechanism and its gate: `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/ProgramVariant.hpp`, `scripts/vn_shader_check.sh`.
- Caching a sampler by value already exists: `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/MaterialImages.hpp` (`acquire(texture, sampler, why)`).
