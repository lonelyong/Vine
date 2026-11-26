# graphics review 17: what is picked, what is blended, what a host program may assume

> **Status**: draft, non-authoritative - a working record. §2 is what the tree states, §3 is the findings, §4 is the
> peer material not to copy, §5 proposes an order, §6 lists the verification commands. This round changed no file
> outside this page and its index entry; every finding is a decision left to the owner.
> **Last verified**: 2026-11-26, tree `a88c6cce` on `dev` (clean working tree).
> **Scope**: review 11 walked the three-dimensional SEMANTICS of transform -> view -> clip -> shading (`V1`-`V10`).
> Rounds 12-16 (recorded in `../memory/graphics.md`, no research page) walked the frame interval, the frame answer and
> the vocabulary. This round takes the three-dimensional surfaces neither set reached: **picking** (does a pick see
> what the picture shows), **transparency** (does the sort and the rasteriser read one classification), **face
> orientation** (which faces exist, and how a back face is lit), the **host-program contract** (what a user shader may
> assume), the **display pipeline** (where linear becomes encoded) and the **geometry capability boundary**. It does
> not re-walk the frame machinery, the threading model or the backend's memory tiers.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/*.hpp`, `src/viz/graphics/src/**`, `src/viz/graphics/shaders/**`,
> `src/plugins/gfx_backend_vk/**`, `src/base/geom/sdk/vine/geom/spatial/{Ray,Aabb}.hpp`, `src/loaders/meshio/**`,
> `src/plugins/{demo_plugin,test_plugin,model_viewer}/**`, `tests/test_graphics/**`, `tests/test_vk/**`,
> `docs/guides/graphics.md`, `.ai/design/graphics-*.md`.
> **Backlog**: `../backlog/graphics.md` - this round adds no entry; §5 lists what it would add, for approval.
> **Numbering**: `W#` is this page's own set (review 11 used `V#`). The module's `F#` / `C#` / `R#` / `N#` / `P#` /
> `D#` / `T#` / `G#` remain the design pages'.
> **Rule of evidence**: every finding names a `file:line`, a test, or a number produced by a command that is quoted
> beside it. A peer claim is a statement about that peer's documented design, not a benchmark.

## 1. Why this round is needed

Review 11 asked whether each quantity along the shading chain has the right mathematical meaning. It did not ask
the question a user of a CAD viewer meets first: **is the thing I clicked the thing I see, and is the thing I faded
actually faded?** Those are answered by agreements between parts that never share a file - the pick query and the
draw list, the sort and the rasteriser, the importer and the cull state, the SDK header and the backend's private
layout. An agreement that lives in no single place is exactly where this module's discipline ("one concept, one
owner") has nothing to check.

Three questions:

1. **Semantics.** Do picking and drawing read the same pose, topology and visibility? Do the sort and the blend
   read the same classification? (`W1`-`W4`)
2. **Framework.** What may a host-authored shader assume, and where is that written? Where does linear light become
   an encoded pixel? (`W5`, `W6`)
3. **Concept.** Which geometry facts are a closed capability boundary rather than a default? (`W7`, `W8`)

### 1.1 Which peers, and why

| Peer | Mechanism read | Why it is a suitable reference |
|---|---|---|
| three.js | `Raycaster.setFromCamera` branches on the camera type; `Raycaster.params.Points/Line.threshold`; raycast reads the rendered `matrixWorld`; `DoubleSide` flips the normal on back faces | The most direct public reference for pick/draw agreement and for double-sided shading |
| glTF 2.0 | `material.doubleSided` (default `false`); accessor `componentType` 5121/5123/5125 | The importer already claims glTF for the material's channel packing; face orientation and index width belong to the same claim |
| Filament | Its published feature list: "HDR/linear lighting", tone mappers, colour and tone management as post-processing | A modern engine's stated display pipeline |
| WebGPU | `indexFormat: uint16 | uint32`; explicit bind-group layouts | The closed-API / open-data split for the two geometry facts |

The comparison is not a dependency and copies no code.

## 2. What the tree states today

| Topic | Current meaning | Home |
|---|---|---|
| Pick query | Two producers: `Camera::screenToWorldRay` (live camera, own basis) and `CameraSnapshot::screenRay` (the presented picture, matrix inverse). The manipulator prefers the snapshot | `Camera.cpp:146-177`, `Camera.cpp:195-252`, `OrbitCameraManipulator.cpp:548-572` |
| Pick hit | Moller-Trumbore, double-sided, `t >= 0`; world-space face normal; distance from the ray origin; triangles only | `Ray.hpp:140-183`, `RayIntersection.cpp:147-218` |
| Transparency | Sort: `isTransparent()` = `Blend` mode, or opacity below 1. Raster: blend on only when the resolved `BlendState::enabled` is set | `Material.cpp:23-31`, `ContentDraw.cpp:284` |
| Face orientation | SDK front = world CCW; vk `frontFace = CLOCKWISE` (the Y fold); default `CullMode::None` | `RenderState.hpp:39-44`, `StateMapping.hpp:44`, `StateResolution.hpp:29` |
| Depth compare | Distance semantics (`Less` = closer passes); the inversion to `GREATER` is the backend's | `RenderState.hpp:13-23`, `StateMapping.hpp:52-66` |
| Depth range | `min` is the FAR end, `max` the NEAR end; passed to `minDepth/maxDepth` unchanged | `DepthRange.hpp:30-52`, `StateMapping.hpp:187-193` |
| Colour targets | Offscreen 8-bit is `*_UNORM` (linear); sRGB exists only as a swap-chain candidate; no tonemap, no exposure | `TargetObjects.cpp:107-122`, `WindowTarget.cpp:37-51`, `BuiltinShaders.hpp:194` |
| Indices / instances | `uint32_t` indices only; no primitive restart; the only per-instance input is the `mat4` at 12-15 | `Primitive.hpp:57`, `ContentDraw.cpp:562`, `ContentCaches.cpp:865-867`, `Geometry.hpp:315-320` |
| Host program | A host supplies GLSL; the push range is two `mat4` (projection folded into device clip, model-view); `VineViewBlock` is declared and bound by nobody | `ShaderProgram.hpp:136-169`, `ContentAbi.hpp:84-101`, `ShaderAbi.hpp:104-120` |

What is right and should stay: the instance matrices are folded into the world box (`Geometry.cpp:551-563`) and the
pick returns the instance index (`RayIntersection.cpp:163-184`); strip and fan are unwound exactly as Vulkan assembles
them (`TriangleStream.hpp:154-168`); all eight `CompareOp` values are mapped and the depth-range ends are named for
what they are under reverse-Z.

## 3. Findings

### W1 (picking / high): an orthographic pick ray starts in the middle of the depth range, so everything nearer is unpickable

**Evidence.** `CameraSnapshot::screenRay` unprojects the pixel at two clip depths, `1.0` and `0.0` (`Camera.cpp:220-221`),
because the comment believes the snapshot's projection is reverse-Z (`:204-206`). It is not: `CameraSnapshot::of`
stores `camera.projectionMatrix()` (`Camera.cpp:188`), which is the OpenGL-range projection, near `-1`, far `+1`
(`Camera.hpp:38-43`, `Transform3.hpp:280-296`). For a perspective camera both points lie on the eye ray, so only the
origin differs (it is replaced by the eye, `:249`). For an orthographic camera the two points are the FAR plane and the
MIDDLE of the range, and `Ray::intersectTriangle` rejects `t < 0` (`Ray.hpp:170-172`).

**Measured**, with the camera its own test builds (`GraphicsTest.cpp:12210`: eye `(0,0,5)`, window `-4..4 x -3..3`,
near `0.1`, far `100`), by replaying `screenRay`'s arithmetic in a script:

```
clip z=1 -> depth 100.0000; clip z=0 -> depth 50.0500
ray origin depth = 50.0500 (the header promises the near plane, 0.1)
  the camera's own look-at target: t = -45.0500 -> REJECTED (Ray.hpp: t < 0)
  a part at depth 90:              t = +39.9500 -> HIT
```

The presented-picture path is the default in the GUI (`SurfaceWindow.cpp:938-940`), and `fitToScreen` gives an
orthographic camera `near = 0.01 d`, `far = 100 d` (`OrbitCameraManipulator.cpp:281-287`), so the origin lands about
`50 d` from the eye: the model, fitted at distance `d`, sits behind the ray. The one orthographic test states the
line only - "Same LINE, not the same starting point" (`GraphicsTest.cpp:12208-12225`) - which is how the defect
survived it.

**This corrects review 11 `V6`.** That finding said the code was right and the sentence wrong, because the two ends
are compared by distance to the eye. That holds for perspective only.

**Peer.** three.js `Raycaster.setFromCamera` branches on the camera type and places the orthographic origin in the
camera's own plane (it unprojects the projection's z translation and comments "set origin in plane of camera"); it
never infers an end from a depth convention. That is also what this module's OTHER producer, `Camera::screenToWorldRay`,
already does (`Camera.cpp:161-169`: the origin travels across the window at the eye).

**Fix.** Unproject at the projection's own two ends (`-1` and `+1`) - the same two points for a perspective camera,
which keeps that path unchanged - or, like the live producer, place an orthographic origin on the eye plane. Add a case
that intersects real geometry through `CameraSnapshot::screenRay` with an orthographic camera. **code + test.**

---

### W2 (transparency / high): opacity below 1 moves a draw to the transparent queue but does not turn blending on

**Evidence.** `BlendState`'s header: "a command whose effective opacity is below 1 ... is drawn with blending on and
the pair below" (`RenderState.hpp:90-92`). The guide says the classification "decides whether blending is opened"
(`docs/guides/graphics.md:82`) and that the sort and the blend switch "read the same classification" (`:73`). The
code: `resolveRenderState` copies `authored.blend` and never reads opacity (`StateResolution.cpp:91`), the vk recorder
enables blend from `resolved.state.blend.enabled` alone (`ContentDraw.cpp:284`), and the only setter in the tree is
`StateNode::setBlend` (`StateNode.cpp:50`). The demo works around it by hanging its half-opacity box under a blend
`StateNode` (`DemoScene.cpp:776-788`); `TestRenderLiveCommand.cpp:110` animates `setOpacity` with no such node. The
pixel test sets the blend state explicitly (`ContentDrawTest.cpp:793`), so no case covers the documented rule.

**Consequence.** A host that fades a part with `setOpacity(0.5)` gets a draw sorted last, depth-tested read-only, and
written opaque. The two halves of "one transparency channel" disagree, which is precisely the trap the design page
records as `P1`.

**Peer.** three.js and Filament make blending a material fact and never sort by a magnitude alone. Whatever this
module decides, the two readers must ask one question.

**Fix.** Decide which is the fact: (a) the recorder enables the default pair when `command.isTransparent()` and the
content authored no blend (the documented rule), or (b) the sort follows `blend.enabled` / `AlphaMode::Blend` and the
opacity stays a magnitude for those two only. Either is a local change; the three sentences must then say the same
thing. **code or doc + test** (a half-opacity draw with no `StateNode`, pixel-checked).

---

### W3 (face orientation / medium-high): everything is double-sided by default, a back face is lit with its away-facing normal, and glTF `doubleSided` is not imported

**Evidence.** `ResolvedRenderState::cullMode` defaults to `None` (`StateResolution.hpp:29`); no shipped shader reads
`gl_FrontFacing` (`grep -rn gl_FrontFacing src/viz/graphics src/plugins/gfx_backend_vk` is empty); `materialFrom`
sets base colour, metallic, roughness and emissive only (`SceneBuild.cpp:153-203`); the loader reads no two-sided key
(`MeshLoader.cpp:511-517`), and `grep -rni doubleSided src` is empty. No design page records the `None` default as a
decision (`graphics-scene.md:165` lists the setter only).

**Consequence.** A closed solid pays every back face's fragment work; a face seen from behind shades with `n.l <= 0`
and falls to ambient, so an open shell (a tessellated sheet, a trimmed face) is dark from one side; and a glTF asset
whose material says `doubleSided: false` (the default) is drawn as if it said true - an import conformance gap beside
the channel packing the same importer does honour.

**Peer.** glTF's `doubleSided` defaults to `false` (the material schema), so a single-sided surface is the format's
default; three.js's shared normal chunk reads `gl_FrontFacing` and multiplies the normal - and the tangent frame - by
`faceDirection` under `DOUBLE_SIDED`, so a back face is lit as the same surface seen from behind.

**Fix.** Three steps, each small: (1) `gl_FrontFacing ? n : -n` in the shared surface fragment, so a back face is lit
as the same surface from behind; (2) map `doubleSided` to `CullMode::None` / `Back` at import (`SceneBuild.cpp`, the
one place the design already names as `C5`); (3) decide the engine default for geometry that states nothing - for
tessellated B-rep `None` is defensible, but it should be a sentence in a design page, not an unset optional.
**code + doc + test.**

---

### W4 (picking / medium): the pick and the picture read different facts

Six smaller disagreements, all in the direction "the picture is not what is tested":

| Fact | The picture uses | The pick uses | Evidence |
|---|---|---|---|
| A billboard's pose | the per-view placement | the `NoRotation` pose | `AutoTransform.hpp:40-41`, `RayIntersection.cpp:370` |
| Topology | the pass override (`show as points`) | the primitive's own topology | `ContentCaches.cpp:1256`, `RayIntersection.cpp:149` |
| Lines and points | drawn | skipped, no pixel tolerance | `RayIntersection.cpp:149-151` |
| Scene visibility | `Scene::isVisible` prunes | not consulted | `Scene.hpp:136`, `RayIntersection.cpp:506-534` |
| The pixel | its centre | its top-left corner (`x / w`, no `+0.5`) | `OrbitCameraManipulator.cpp:563` |
| Clip planes, cull mode | applied | not applied | `RenderState.hpp:127-129` |

Each is individually defensible; together they mean a selection can answer "nothing" for a billboard, a point cloud or
an invisible scene, and "this" for a back face the picture culled. `AutoTransform.hpp:40-41` states the first as
intended, the other five are stated nowhere.

**Peer.** three.js's raycast reads the rendered `matrixWorld`, takes an explicit `Line.threshold` /
`Points.threshold` (world units), and the raycaster carries the camera into the query for "view-dependent objects
such as billboarded objects" - the three facts `W4` says this module's query is missing.

**Fix.** One concept: a pick query is built from the same `View` value the frame drew with (`CameraSnapshot`, the
resolved topology, the view placement) and answers through one entry. The cheap first step is a pixel radius in the
query (lines and points) and `+0.5` in the pointer normalisation; the billboard pose needs the presented view and is
the same value `N17` already carries. **design + code + test.**

---

### W5 (framework / medium): the host-program contract has no owner - the layout is private, the projection is folded undocumented, and the view block is a dead shape

**Evidence.** A host may supply GLSL (`ShaderProgram.hpp:136-169`). What it must assume:
- The push range's member layout (`projection`, then `modelView`) is declared in the backend's PRIVATE
  `ContentAbi.hpp:84-101`; the public `ShaderAbi.hpp:104-107` says only "the proj/model subset".
- That `projection` is already folded into the device's clip convention (Y down, reverse-Z) - `ContentDraw.cpp:103-109`.
  `ShaderAbi.hpp:113-120` documents `VineViewBlock::proj` as view -> clip with no such word.
- `VineViewBlock` is declared (`ShaderAbi.hpp:113`) and bound by nobody: the vk device never uploads it
  (`grep -rn VineViewBlock src` reaches comments only), `vine_params.glsl:24-26` says it has no shipped GLSL
  declaration, while `Device.hpp:25` says it is "bound as uniforms" and `CameraSnapshot.hpp:23` /
  `StateMapping.hpp:31` refer to a `buildViewBlock` that does not exist.
- A full-screen draw receives no camera data at all (no push range, an empty draw block: `ContentDraw.cpp:652`), and
  there is no channel for host parameters (`ShaderAbi.hpp:128-129`, `params.z/w` reserved and packed as zero).
- The engine's variant defines are injected into a host program too (`ContentCaches.cpp:1218`), with no entry for the
  host's own.

**Consequence.** A host program that wants the camera position, an inverse projection or an unfolded clip space (an
outline, a depth-reconstruct, a fog) can write the GLSL and cannot be handed the numbers. The next backend must
rediscover the push range from this one's private header. This is the one extension surface review 10's `L1` table
did not list.

**Peer.** three.js's `ShaderMaterial` documents the contract on the page that teaches how to write the shader: "Built
in attributes and uniforms are passed to the shaders along with your code" (the projection, the model-view, the
normal matrix, the camera position), and the MATERIAL carries its own `defines` - the two things this module's host
program has no public statement for and no entry for.

**Fix.** Decide the contract's home and put all of it there: either (a) the SDK owns a `PushRange` mirror of
`ContentAbi.hpp`'s struct plus a public sentence per convention, or (b) `VineViewBlock` becomes real - bound by the
backend, declared once in `vine_params.glsl`, with `proj` defined as the SDK's clip space - and the push range becomes
the built-in stages' optimisation of it. In both, delete or correct the `buildViewBlock` references. **ADR + doc
(+ code for (b)).**

---

### W6 (display pipeline / medium): linear light is quantised to 8 bits before it is encoded, and nothing owns the transform

**Evidence.** Every offscreen 8-bit format maps to `*_UNORM` (`TargetObjects.cpp:107-122`); sRGB is chosen only for the
swap chain (`WindowTarget.cpp:37-51`); `ColorFormat` has no sRGB spelling (`RenderTarget.hpp:53-66`) although its
comment calls `RGBA8` a projection over both; the full-screen copy is one `texture()` (`builtin_fullscreen_copy.frag:19`);
tonemapping and exposure do not exist and the header says a program that wants one "is a different program"
(`BuiltinShaders.hpp:194`). A readback is the stored bytes (`ReadbackStaging.cpp:96-103`).

**Consequence.** A lit value passes an offscreen target as linear 8-bit, where the dark half of the range has most of
the code values (`0.5` linear is about `188` sRGB): banding in shadows, and any value above 1 clips before the window
when the target is `RGBA8`. A readback and the same frame on screen differ by the transfer function, so a
pixel-comparison test proves something other than what the user sees.

**Peer.** Filament's published feature list names the stages this module has no owner for: "HDR/linear lighting",
several tone mappers, and colour and tone management as post-processing (`README.md`, Rendering / Post processing).
The transfer is not one shader; it is a stated order.

**Fix.** Not a rewrite - two decisions with one owner: which transfer function each `ColorFormat` carries (a linear
8-bit intermediate should be the exception, not the default), and where the encode happens (the swap-chain write, as
now, stated once). State both in the display section of the guide and in `RenderTarget.hpp`. **ADR + doc.**

---

### W7 (concept / medium-low): the geometry boundary is closed but undocumented - 32-bit indices, no restart, no per-instance attribute

**Evidence.** `Primitive::IndexSpan` is `BufferSlice<std::uint32_t>` (`Primitive.hpp:57`) and the backend binds
`VK_INDEX_TYPE_UINT32` unconditionally (`ContentDraw.cpp:562`); the input-assembly state sets no
`primitiveRestartEnable` (`ContentCaches.cpp:865-867`); `GeometryChannel` has a location and a per-vertex segment only
(`Geometry.hpp:315-320`), so the instance rate exists for the `mat4` at 12-15 and for nothing else.

**Consequence.** A glTF mesh with 16-bit indices (`componentType` 5123, the common case) is doubled on import; a set
of strips must be split into draws; per-instance colour, ID or scale needs one draw per distinct value - which is
what a selection highlight over an instanced part has no cheap spelling for.

**Peer.** glTF, WebGPU (`indexFormat: uint16 | uint32`) and three.js (`InstancedBufferAttribute`) all leave index width
and per-instance data open, as data.

**Fix.** No code is asked for. Record the three as a closed boundary in the geometry design page with a trigger each
(first asset whose index buffer is the memory peak; first multi-strip geometry; first per-instance highlight), so the
next reader does not take a missing feature for an oversight. **doc + backlog candidate.**

---

### W8 (documentation / low): a depth-only clear is not "applied inside the pass' viewport"

`PassDesc.hpp:110-111` says it is. The backend's render area is the whole target (`DynamicRendering.cpp:179-180`) and a
`loadOp = CLEAR` ignores viewport and scissor, so `axis_gizmo`'s `clear_depth = true` (`ChainBuilder.cpp:108`) erases
the scene's depth for the whole picture. No shipped pass tests depth after `PassStage::Overlay`, so nothing is wrong
today; a host `Preview`-stage pass that does will be. Correct the sentence to say "the whole attachment", or narrow
the clear to the rectangle if that was the intent. **doc (or code).**

---

### W9 (documentation / low): the opacity setters accept anything, and zero still draws and still picks

`Node::setOpacity` stores its argument (`Node.cpp:196-200`); the product of ancestors is clamped once, at the leaf
(`DrawListBake.hpp:586`). Two negative opacities multiply back to a positive one, and a node at opacity `0` produces
a draw and a pick hit. The alpha-mode rule (`C4`) explains why `Mask` must stay in the opaque queue; it does not
explain why nothing is culled at exactly zero. Decide and state: clamp at the setter (the `setAspect` precedent,
`Camera.cpp:129-134`) and prune at zero, or document both as intended. **code or doc.**

---

### Still in the tree from review 11

Verified on `a88c6cce`: `V2` (`builtin_forward.vert:68`, `builtin_gbuffer.vert:60`), `V3` (`StateMapping.hpp:44`),
`V4` (`Geometry.hpp:489`; the loader header repeats the promise at `MeshLoader.hpp:198-199`) and `V10`
(`DrawListBake.hpp:429`) are unchanged. `W3` and `V3` share one mechanism (the face orientation a draw assumes) and
belong in the same batch. For `V4` a peer spelling exists: three.js's `ShaderMaterial.defaultAttributeValues` passes a
default "when the rendered geometry doesn't include these attributes but the material does ... This avoids errors
when buffer data is missing" - the render-side answer to a channel the geometry does not carry.

## 4. Peer material not to copy

| Peer mechanism | This module's judgement |
|---|---|
| three.js `material.transparent` as the ONLY source of blending | The opacity channel is a CAD-review need (ghost a whole part); what transfers is "one question for the sort and the rasteriser", not the removal of the channel |
| OSG `StateAttribute::OVERRIDE / PROTECTED` | The subtree fold's "deepest wins" is simpler and matches the tree-only scene (`ADR-2026-10-23`); the cost is that a descendant cannot reset a material to the default, worth a sentence, not a mechanism |
| An ID-buffer (GPU) pick | CPU picking is the right answer for exact CAD selection (no readback latency, sub-triangle precision); what transfers from it is that the pick pass draws the SAME transforms as the picture (`W4`) |
| A render graph with declared resource transitions | Rejected by the standing ADRs (review 8); nothing here reopens it |
| Filament's full tonemapping stack | Two decisions transfer (`W6`): which transfer function a format carries, and who encodes |
| Bindless per-instance data | Closed by `D5` and `T69`; `W7` asks only that the boundary be written down |

## 5. Proposed order (for the owner to approve; not automatically added to the backlog)

1. **`W1`** - one function, one new test with real geometry. It is the only finding that makes a shipped interaction
   (orthographic pivot / recentre) answer "nothing" on the presented-picture path.
2. **`W2`** - decide which half is the fact, land the local change, make the three sentences agree. Same batch as a
   pixel case for an opacity-only draw.
3. **`W3` + review 11 `V3`** - one batch on face orientation: the `gl_FrontFacing` flip, the `doubleSided` import, the
   determinant decision, and the default stated in a design page.
4. **`W5`** - an ADR naming the host-program contract's home; delete the `buildViewBlock` references now, whatever
   the ADR decides.
5. **`W4`** - the cheap half first (pixel radius, `+ 0.5`, scene visibility), then the view-carried billboard pose.
6. **`W6`, `W7`, `W8`, `W9`** - decisions and sentences.

Backlog candidates (**not added**): orthographic pick origin (trigger: none - present now); opacity-only blending
(present now); double-sided import and back-face lighting (trigger: first glTF asset with open shells);
the host-program contract (trigger: the first host post-processing program, or the second backend).

## 6. Verification commands

```bash
# W1: the two clip depths and the rejection
sed -n '204,222p' src/viz/graphics/src/Camera.cpp
sed -n '165,172p' src/base/geom/sdk/vine/geom/spatial/Ray.hpp
# W2: the promise, and the only reader
sed -n '88,93p' src/viz/graphics/sdk/vine/graphics/RenderState.hpp
grep -rn "blend\.enabled\|setBlend" src/viz/graphics src/plugins/gfx_backend_vk
# W3: no back-face branch, no import
grep -rn "gl_FrontFacing" src/viz/graphics src/plugins/gfx_backend_vk
grep -rni "doubleSided\|TWOSIDED" src
# W4: the two topology sources
grep -n "primitive->topology" src/viz/graphics/src/RayIntersection.cpp
grep -n "resolveRenderState" src/plugins/gfx_backend_vk/src/internal/ContentCaches.cpp
# W5: the shape nobody binds
grep -rn "VineViewBlock\|buildViewBlock" src | grep -v "ShaderAbi.hpp"
# W7: the index width and the assembly state
grep -n "VK_INDEX_TYPE" src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp
grep -rn "primitiveRestartEnable" src/plugins/gfx_backend_vk
```
