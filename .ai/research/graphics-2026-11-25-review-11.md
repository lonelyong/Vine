# graphics review 11: three-dimensional concepts, conventions and semantics

> **Status**: draft, non-authoritative - a working record. §2 is what the source actually says about the three-dimensional conventions, §3 is the findings, §4 is the peer material not to copy, §5 is the proposed order, §6 is the verification commands. This round changed no file outside this page and its index entry; every finding is left to the owner.
> **Last verified**: 2026-11-26, tree `9829e682` on `dev` (V5 rechecked: `return {}` is the identity matrix, not a zero matrix).
> **Scope**: the previous ten rounds covered implementation quality (7), incremental editing/threading (6), seam concepts (8), the vk backend as a system (9), and the design corpus / concept inventory / extension surface (10); **none took three-dimensional semantics itself as the dimension**. This round looks only at: space and matrix conventions, camera/projection/picking, the transform hierarchy, and the three-dimensional semantics of geometry, lighting and materials. It does not re-walk the threading, frame machinery, backend system or concept inventory of rounds 6-10.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/*.hpp`, `src/viz/graphics/src/**`, `src/viz/graphics/shaders/**`, `src/plugins/gfx_backend_vk/src/internal/**`, `src/base/math/**`, `tests/test_graphics/**`, `tests/test_vk/**`, `.ai/design/graphics-*.md`, `.ai/decisions/**`.
> **Backlog**: no entry is added without the owner; §5 lists candidates, and the `T#` numbering is the owner's.
> **Numbering**: `V#` is this round's own numbering (rounds 8/9/10 used `C#`/`K#`/`L#`); the module's `F#`/`C#`/`R#`/`N#`/`P#`/`D#`/`T#`/`G#` remain the design pages' numbering.
> **Rule of evidence**: every finding gives a `file:line`, a test name or a re-runnable command. A peer statement describes the peer's public design, not a performance benchmark.

## 1. Why this round is needed

A graphics module can be completely self-consistent as engineering - a clean seam, immutable frame values, complete diagnostics, one ABI declaration - while being wrong in **three-dimensional semantics**: normals transformed with the wrong matrix, mirrored winding culled as back faces, lit shaders reading an undefined attribute when a normal channel is missing, a degenerate camera silently falling back to the identity view, and light units that do not match the import format. Rounds 9/10 cannot find these, because the owner exists and the spelling is consistent; what is wrong is the **mathematical meaning**.

This round asks three questions:

1. **Concept**: does the module have one three-dimensional convention (handedness, up axis, world units, space flow), or are the facts scattered across file comments? (§3 `V1`)
2. **Semantics**: along transform -> view -> clip -> shading, does each quantity's mathematical meaning agree with the documentation, the tests and the peer convention? (§3 `V2`-`V8`)
3. **Framework**: where should a convention change (normal matrix, front-face, units) land so it does not become a second source of truth? (the fix column of each finding in §3)

### 1.1 Which peers, and why

| Peer | Mechanism read | Why it is a suitable reference |
|---|---|---|
| three.js | `object.normalMatrix.getNormalMatrix(modelView)`; `frontFaceCW = matrixWorld.determinantAffine() < 0` | The most direct reference for the normal matrix / negative-determinant front-face decision, and its source is public |
| OSG | The naming of `PositionAttitudeTransform` / `AutoTransform`, and the "parent transform cancelled by the automatic rotation" semantics | This module's node names and behaviour are inherited directly from OSG; check whether the inheritance is complete |
| glTF 2.0 + `KHR_lights_punctual` | Right-handed Y-up, metres; OpenGL orientation for normal maps; cd/lux light intensity, `1/d^2` attenuation | The material channel packing and import table already claim glTF compatibility; the lighting side should use the same reference |
| Vulkan / D3D12 | NDC depth [0,1], framebuffer Y down; reverse-Z is an optional convention, not an API rule | This module keeps the GL convention in the SDK and folds it at the vk boundary; the statements about that boundary need checking |
| Unity / Unreal / Filament | Explicit world units, inverse-transpose normals, PCF shadows, IBL as a separate environment term | Distinguish "deliberately not done" from "not done yet" |

The comparison is not a dependency and copies no code; it only answers "how does the peer define this quantity, and where is the definition written".

## 2. The three-dimensional conventions the module currently states

Putting the source comments, the implementation and the tests together gives the conventions the module **actually** holds. The table is itself one output of this round: no single normative page carries these sentences today.

| Space / quantity | Current meaning | Home |
|---|---|---|
| World | Right-handed, default up `(0,1,0)`, default camera eye `(0,0,5)` looking at `(0,0,0)`; **no world-unit declaration** | `Camera.hpp:250-252`, `Camera.cpp:11-15` |
| World CCW | Front faces are counter-clockwise in world space; after the vk framebuffer Y flip -> `frontFace = CLOCKWISE` | `StateMapping.hpp:32,44`, `ContentCaches.cpp:780` |
| View | `lookAt` row basis (right/up/backward); the camera looks down view -Z; `eye == target` returns the **identity matrix** (the camera silently moves to the origin looking down -Z) | `Transform3.cpp:61-115` (the degenerate path at `68-70`) |
| Clip (SDK) | Classic GL: x/y in [-1,1], z in [-1,1], near = -1, far = +1 | `Camera.hpp:38-43`, `Transform3.hpp:280-296` |
| Clip (vk device) | `fold = diag(1,-1,-0.5)` + `z' = 0.5 - 0.5z`: Y flipped, reverse-Z, near = 1, far = 0 | `ContentDraw.cpp:87-93` |
| Frustum culling | Six planes extracted from the SDK `P*V` with Gribb-Hartmann rows; p-vertex AABB test | `Frustum.hpp:39-82` |
| Geometry channels | location 0 position / 1 normal / 2 colour / 8 UV / 12-15 instance matrix, column-major | `ShaderAbi.hpp`, `Geometry.hpp`, `Primitive.hpp` |
| Shading | All in **view space**: lights are packed after the camera's rotation and translation; one GGX/Smith/Schlick BRDF; the diffuse lobe has no `1/pi` | `LightAbi.cpp:117-191`, `vine_params.glsl:100-151`, `vine_brdf.glsl:58-80` |
| Transparency | Sort key = draw order -> layer -> opaque/transparent -> quantized camera-space depth -> tie; the representative point is the world-box centre | `DrawListBake.hpp:425-460`, `DrawListBake.cpp:525-545` |

What the module **gets right** should also be stated: reverse-Z is folded at one boundary and has a pixel-level diagnostic record (not implemented twice); forward and deferred share one BRDF, one material sampling and one light evaluation; the instance matrix is column-major and has the same shape in C++, GLSL and the tests; transparent sorting uses camera-space depth and the box centre rather than the node origin. The problems in this round are not in those mechanisms but in the **landing of the semantics** in the table above.

## 3. Findings

### V1 (concept / medium-high): the three-dimensional convention has no single owner; handedness, up axis and world units are defaults, not declarations

**Evidence**: `Camera.hpp:250-252`'s `(0,0,5)/(0,0,0)/(0,1,0)` implies right-handed Y-up; the note at `Transform3.hpp:220-225` mentions right-handed -Z only at `lookAt`; `Light.hpp:243` says the range is in "world units"; `Geometry.hpp:489` says the renderer derives normals when none are authored; `ADR-2026-11-04-surface-placement-value-type.md` only states that `geometry::Placement<T>` is always right-handed and is explicitly a local value type. `src/loaders` contains no `up_axis` at all (`tests/test_meshio/ModelLoadTest.cpp` has a `Y_UP` sample, but no consumer conversion).

**Why it matters**: `Placement<T>` always being right-handed is a CAD/geometry decision and cannot automatically become the graphics world's convention; the importer keeps the file's up axis while the renderer defaults to Y-up, so a Z-up asset "lies down" unless it is explicitly rotated. A host cannot ask the API for handedness/up/units and has to repeat the assumption in several places - while this project's discipline is exactly "one concept, one owner, no second spelling".

**Peer comparison**: glTF writes right-handed Y-up and metres into its specification; Unity/Unreal put the coordinate system and units into project settings; OSG makes up an explicit default of the camera variable.

**fix**: add an ADR as the one normative page, stating world handedness, up axis and world units (and whether glTF light intensity is interpreted in metres); assign ownership of the up-axis conversion at the import boundary; expose a queryable constant or type in `docs/guides/graphics.md` instead of letting every call site infer it. **ADR + doc + test** (import a Z-up sample and assert the pose).

---

### V2 (rendering semantics / high): under non-uniform scale the normal is transformed by `mat3(modelView)`, not the inverse transpose

**Evidence**: `builtin_forward.vert:68` and `builtin_gbuffer.vert:60` both use `vn_view_normal = mat3(vn_model) * vn_Normal;`; `PositionAttitudeTransform.hpp:28-29` states that non-uniform scale is legal, and `PositionAttitudeTransform.cpp:81-82` actually builds `T*R*S*T(-pivot)`; `ShaderAbi.hpp:131-134`'s `VineDrawBlock` has only `mat4 model` + `vec4 params`, and the module has no `normalMatrix` or equivalent channel.

**Mathematical consequence**: multiplying a normal directly by `M` as if it were a direction equals the correct `det(M) * (M^-1)^T n` only for an orthogonal transform (pure rotation + uniform scale). For `n=(1,1,0)/sqrt(2)` and `S=(10,1,1)`, the current implementation gives `(0.995,0.0995,0)`, while the correct value is `(0.0995,0.995,0)` - the two directions are nearly perpendicular. A CAD part stretched along one axis or a robot-arm link will be lit incorrectly over a whole surface; a normal map is wrong in the same way, because the tangent basis is built on the wrong normal.

**Peer comparison**: three.js calls `object.normalMatrix.getNormalMatrix(object.modelViewMatrix)` (the inverse transpose) every render and uploads it as a uniform; Unity/Unreal/Filament do the same.

**fix**: add a normal matrix (3x4 or mat4, std140-aligned) to each draw at the SDK ABI layer, derived by the one packing function from the world/view matrix, read by both the forward and G-buffer vertex stages; or restrict scale to positive uniform scale and **change `PositionAttitudeTransform`'s public contract** (the current documentation explicitly allows non-uniform scale, so it cannot be treated as already restricted). **code + ABI + test** (pixel/numeric assertion for a plane normal under non-uniform scale).

---

### V3 (geometry semantics / high): negative scale flips winding, but front-face is fixed to CLOCKWISE

**Evidence**: `PositionAttitudeTransform.hpp:29` states that negative scale mirrors and flips the winding of its triangles; `AutoTransform.hpp:108` allows the same; `StateMapping.hpp:44` fixes `VK_FRONT_FACE_CLOCKWISE`, and `ContentCaches.cpp:780` writes it into the pipeline unconditionally; no backend code flips by determinant (`grep frontFace/kFrontFace` finds only those sites).

**Consequence**: with back-face culling enabled, the outer surface of a mirrored subtree is culled as a back face and the inner surface becomes visible; `CullMode` gives inside/outside reversed whether it is Back or Front. three.js's `frontFaceCW = matrixWorld.determinantAffine() < 0` is the fix for this common case.

**fix**: add a "negative determinant" fact to the draw's resolved state and consume it in the pipeline key or in dynamic front-face state; it must be decided per draw for instance matrices (`Primitive::instance_transforms` can give each instance a different determinant, so a static pipeline flip would also need to batch by instance, or switch to two-sided/custom culling). First decide whether mirroring is officially supported: if not, make the setter reject negative scale and report a diagnostic, matching the existing "reject non-power-of-two sample counts" style. **code + ABI + test** (pixel culling assertion for a mirrored box).

---

### V4 (geometry semantics / high): geometry without vertex normals reads an undefined input under the default smooth program; the documentation promises a derivation

**Evidence**: `Geometry.hpp:489` says "when unset the renderer derives normals from the positions"; in practice `ContentCaches.cpp:964-987` writes only the channels the geometry **has** into the vertex input, so a missing location 1 is not bound; `builtin_forward.vert:16-17,68` reads `vn_Normal` unconditionally and multiplies it into `vn_view_normal`; `builtin_forward.frag:60` calls `normalize(vn_view_normal)` directly; `builtin_gbuffer.frag:46` does the same. No implementation anywhere derives normals from positions (`grep hasNormals` only hits container queries). The `VN_FLAT` variant uses `dFdx/dFdy` for the face normal (`builtin_forward.frag:48-58`), so it is correct only when the flat program happens to be selected.

**Consequence**: a Mesh without normals (the importer produces one when the source mesh has none; `tests/test_graphics/GraphicsTest.cpp:2421` asserts exactly such geometry) drawn with the default lit program reads an undefined Vulkan vertex input; `normalize` can produce NaN -> whole surfaces of wrong colour or blotches. This is not about "DSL/value-semantics" engineering correctness; it is the three-dimensional data contract failing.

**Peer comparison**: glTF/three.js allow geometry without normals and the renderer uses a flat/face-normal fallback; OSG also treats normal generation as an explicit local policy.

**fix**: choose one and write it into the convention: (a) generate face normals in the content bridge `geometryFromShape()` and the `setPositions()` path (data side), or (b) make the default program use the `VN_FLAT` vertex/fragment path when the channel is missing (render side). Also correct the sentence at `Geometry.hpp:489` and add an end-to-end pixel test. **code + doc + test**.

---

### V5 (camera semantics / medium-high): a degenerate camera silently falls back to the identity view, frustum parameters are unvalidated, and the stored up is not the actual basis

**Evidence**: in `Transform3.cpp:68-70`, `eye == target` returns `{}` directly; `Matrix4x4`'s default constructor is the identity matrix (`Matrix4x4.hpp:60-66`), so this is the identity view, not a zero matrix; `Camera.cpp:34-40` accepts it unchanged; `setPerspective()` (`Camera.cpp:42-48`) does not check `fovy`, `zNear>0` or `zFar>zNear`, and `setOrthographic()` (`:50-62`) does not check the left/right/bottom/top/near/far order. By contrast `setAspect()` (`:129-134`) explicitly ignores non-positive values and has a test (`GraphicsTest.cpp:1033-1036`) - the same validation style is missing from the frustum parameters. `Camera.cpp:38` stores only the normalized `up`, but `lookAt` **picks a different reference axis** when up is parallel to the view direction (`Transform3.cpp:76-96`), so `up()` reports an up that the view matrix does not actually use; `CameraSnapshot::up` carries that value into the billboard orientation at `AutoTransform.cpp:250`.

**Consequence**: when `eye == target`, the view silently becomes the identity matrix (not all zero), and the camera returns to the world origin looking down -Z; the frustum can still be extracted, so there is no "six degenerate planes / nothing culled", but the picture silently switches to a pose nobody asked for and there is no diagnostic. Illegal near/far/fov values produce NaN or a negative depth range in the projection, again with no diagnostic beyond a wrong picture. A parallel up makes the billboard orientation disagree with the picture.

**Peer comparison**: three.js/OSG/Unity treat a degenerate lookAt and an illegal frustum as call errors or an explicit fallback; this project itself "rejects and reports" non-power-of-two sample counts.

**fix**: give `Camera` a named refusal/fallback answer (following the `PlanRefusal`/`EngineNotReadyReason` style); a degenerate lookAt keeps the last valid view and reports; set up to the basis `lookAt` actually resolved, or make `up()` recover it from `view_`; add four parameter-validation tests. **code + test**.

---

### V6 (convention boundary / medium): the SDK and device clip conventions split, and a few documentation sentences state the direction backwards

**Evidence**: `Camera.hpp:38-43` correctly states that the SDK projection is GL z in [-1,1], and `ContentDraw.cpp:87-93` folds it at the one boundary into reverse-Z [0,1] with Y flipped. But `CameraSnapshot.hpp:58-60` says "the projection carries which clip depth is NEAR (this tree renders Reverse-Z)", while `CameraSnapshot::of()` (`Camera.cpp:188`) writes the GL projection; `CameraSnapshot::screenRay()` (`Camera.cpp:204-206`) actually decides which end is near by "distance to the eye", so the code is right and **the sentence is wrong**. `BuiltinShaders.cpp:125-126` says the light clip z is "0 at near, 1 at far", while the same block's code at `:137` uses `1 - (z*0.5+0.5)` to map GL -1 to 1 (near) and +1 to 0 (far) - that comment states the same thing backwards.

**Consequence**: a host that treats the SDK `projectionMatrix()` as a device projection (writing its own depth effect, GPU-side unproject, depth reconstruction) gets a silently wrong picture; the wrong comments make a maintainer reason in the opposite depth direction when changing shadows or picking. The code currently avoids the bug through the "distance decides" rule and the single fold point, but it will not save the next reader.

**fix**: keep the two-layer convention (it is a reasonable design), but promote the convention table to a normative page (it can merge with `V1`'s ADR), state "who produces the GL projection, who folds it, who must not fold twice", and correct the two comments above; add a test asserting that `CameraSnapshot::projection`'s near is GL -1, not the device 1. **doc + test**.

---

### V7 (lighting semantics / medium): light units do not match glTF/physical conventions, and the "PBR" label promises more than the units deliver

**Evidence**: the attenuation at `vine_params.glsl:127` is `1/(d^2+1)` (then multiplied by the range window at `:133-134`), while `KHR_lights_punctual` specifies candela for point/spot and lux for directional, attenuation `~1/d^2`, recommending `window / d^2`; `Light.hpp:67,182`'s intensity is only a unitless multiplier; `vine_brdf.glsl:15-18` explicitly states that the diffuse lobe carries no `1/pi` (to preserve the old brightness), while the specular lobe is a physically shaped GGX; `LightAbi.cpp:185-189` fills a 0.15 ambient fill when there is no light. On the material side `graphics-material.md:179` already admits that metal is nearly black without an environment map.

**Consequence**: a glTF `KHR_lights_punctual` intensity imports to a brightness that is not comparable with a reference renderer; mixing the dimensions of the diffuse and specular lobes means "raise roughness" changes the ratio of two differently-unitised terms, so PBR parameters cannot be reused across tools; metal objects in a scene without IBL are almost only specular. This is a **deliberately chosen unit convention** (the documentation says so), but a claim of metallic-roughness PBR / glTF compatibility should state the unit exception at the same time.

**Peer comparison**: the glTF specification above; Filament/Unity treat IBL as a separate environment contribution and distinguish directional lux from point cd in their types.

**fix**: write an ADR/guide entry for "the engine's own intensity unit" and state the conversion to candela/lux or "not directly interchangeable"; for strict compatibility, change the attenuation to `1/d^2` (with a softening floor), add `1/pi` to the diffuse lobe and rebalance, then update the import table and pixel baselines. This is a semantic decision and should not be a one-line shader edit. **decision + doc (+ code depending on the choice)**.

---

### V8 (shadows / medium): one depth compare, only the first shadow-casting light, fixed bias - deliberate simplifications that need to be stated as capability boundaries

**Evidence**: `BuiltinShaders.cpp:143` does one `texture().r` compare, with no PCF; `Light.hpp:39-41` explicitly has no filter knob; `Light.hpp:284-288` and `ChainPresets.cpp` build a depth pass only for the first enabled directional/spot light that casts; `ShadowSettings::bias` is a normalized depth scalar (`BuiltinShaders.cpp:143` adds it directly to the reverse-Z depth), with no slope-scale or world-unit-based offset. Backlog `S6` (per-light shadows / cube shadows) and `S7` (per-object light selection) already track the first two.

**Assessment**: this is consistent with the module's "only declare switches that are implemented" discipline and is not a bug; but from a three-dimensional experience, hard-edged shadows, shadows following only the first light, and grazing-angle acne/detach are the differences a user notices first.

**fix**: do not rush to implement; write it into the guide's **capability matrix** (one shadow-casting light per scene, no PCF, bias in depth units) so a host can choose to implement its own. If PCF is added together with alpha-to-coverage, decide it in the same batch as `T58`. **doc + backlog (S6/S7 already exist)**.

---

### V9 (transparency semantics / medium): under Mask, opacity feeds the cutout threshold, yet the object is sorted into the opaque queue; design page 2.7 disagrees with the code

**Evidence**: `Material.cpp:23-31`'s `alphaModeSortsTransparent()` always returns false for Mask; the threshold at `builtin_forward.frag:89` and `builtin_gbuffer.frag:62` is `vn_draw.params.x * vn_surface.alpha < cutoff`, i.e. the drawable's opacity changes the cutout shape. `.ai/design/graphics-material.md:70-73` instead says "`isTransparent()` becomes opacity<1 or Blend", which is the opposite of the code semantics in §2.7; the same page's §3.5/C4 also says Mask does not enter the transparent queue.

**Consequence**: when a host fades a scene with one `setOpacity()`, a Mask object does not become fainter - it gradually disappears / develops holes - and because it is in the opaque queue it cannot blend to a translucent result. glTF's alphaMask threshold comes from material alpha (texture x factor); object opacity is a different quantity.

**fix**: decide the semantics: (a) the threshold uses material alpha only, and opacity is used only for Opaque/Blend output/sorting (matching glTF); or (b) keep the current behaviour but remove opacity from the `alphaModeSortsTransparent` documentation sentence, and warn in the guide that "fading a Mask erodes its outline". Either way, the sentence in §2.7 must have the same source as the code. **code or doc + test**.

---

### V10 (documentation consistency / low): the sort-key comment states the depth direction backwards

**Evidence**: `DrawListBake.hpp:429` says "Quantized camera-space depth (near = larger)", but `DrawListBake.cpp:541-544` computes `depth = -(view*point).z` (near z ~ -1 -> depth ~ 1, far z ~ -10 -> depth ~ 10), `SelectTest.cpp:199` asserts "camera-space depth grows away from the eye", and `operator<` (`DrawListBake.hpp:456-457`) uses that to draw opaque from near to far and transparent from far to near. The implementation and the test are right; the comment is backwards.

**fix**: change the comment to "depth grows away from the eye"; doc-only. **doc**.

## 4. Peer material not to copy

| Peer mechanism | This module's judgement |
|---|---|
| three.js's single-header JS API / dynamic material compilation | This module is a cross-DLL C++ ABI; the normal matrix belongs in the ABI, not in an implicit field on an object |
| Unity/Unreal fixing world units to metres/centimetres | This module serves CAD and robotics geometry; leaving units to the layer above fits the existing neutral `Placement<T>` design better. What is needed is a **declaration and boundary conversion**, not hard-coded metres |
| glTF's full IBL / physical light units | This round only recommends declaring the difference; forcing a unit switch would break the existing brightness baseline and the migration record in `graphics-material.md` |
| WebGPU's fully explicit barriers / bind groups | Already handled by the vk backend under this project's ADR; unrelated to three-dimensional semantics |
| OSG's multi-parent DAG / node-level lights | Already rejected/tracked by `ADR-2026-10-23` and backlog T7; do not reopen |
| wgpu/Filament's bindless shadows and many lights | This module's 8 slots + first-light shadow is a capacity decision (`S6/S7/T69`), not a semantic error |

## 5. Proposed order (for the owner to approve; not automatically added to the backlog)

1. **Settle the convention first** (`V1` + `V6`): one ADR declares handedness/up/units/space flow and corrects the two backwards depth comments. Every later fix then has one reference point.
2. **Then fix the high-risk three-dimensional maths** (`V2`, `V3`, `V4`): normal matrix into the ABI -> negative-determinant front-face policy -> missing-normal fallback. All three involve ABI or program variants and should be planned as one batch to avoid repeated ABI bumps; each gets a pixel/numeric test.
3. **Camera robustness** (`V5`): follow the existing "named refusal" style and introduce no new mechanism; change it together with the `CameraSnapshot`/`AutoTransform` up semantics.
4. **Semantic clarification** (`V7`, `V8`, `V9`): state the units and capability boundaries first, then decide whether to change the numbers; `V9` can go with the existing drift correction in the material guide.
5. **Single-line correction** (`V10`).

Candidate backlog entries (**not added**): normal matrix and negative determinant (can be one entry; trigger: the first non-uniformly scaled/mirrored asset enters rendering); no-normal fallback (trigger: the first imported model without a normal channel drawn with the default lit program); camera degenerate/illegal frustum refusal (trigger: a host observes NaN pixels or needs a diagnostic); light units and glTF conversion (trigger: the first `KHR_lights_punctual` import requirement).

## 6. Verification commands

```bash
# the entry points for this round's evidence
grep -rn "mat3(vn_model) \* vn_Normal" src/viz/graphics/shaders
grep -rn "kFrontFace\|frontFace" src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/StateMapping.hpp src/plugins/gfx_backend_vk/src/internal/ContentCaches.cpp
grep -rn "derives normals from the positions\|hasNormals" src/viz/graphics
grep -rn "1.0 / (vn_distance \* vn_distance + 1.0)" src/viz/graphics/shaders/vine_params.glsl
grep -rn "near = larger\|depth grows away" src/viz/graphics/src/DrawListBake.hpp tests/test_graphics/SelectTest.cpp
grep -rn "up_axis" src/loaders tests/test_meshio | head
```
