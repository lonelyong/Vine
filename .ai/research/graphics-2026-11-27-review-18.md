# graphics review 18: navigation, the mirrored face, and a second look at reviews 11 and 17

> **Status**: draft, non-authoritative - a working record. §2 is what the tree states, §3 is the new findings, §4 is the
> re-review of the two earlier three-dimensional rounds, §5 is the peer material not to copy, §6 proposes an order, §7
> lists the verification commands. This round changed no file outside this page and its index entry; every finding is a
> decision left to the owner.
> **Last verified**: 2026-11-27, tree `a546323c` on `dev`.
> **Scope**: review 11 walked the shading chain (`V1`-`V10`), review 17 the surfaces it did not reach (`W1`-`W9`), and
> the owner landed `W1`-`W9` plus the front-face correction. This round does three things: it **re-reviews** both sets on
> the current tree (which landed, which is still there, and what the landings changed - `Y1`, `Y8` and most of §4 are
> consequences of landings); it takes the one three-dimensional surface neither round opened, **navigation** (the
> manipulator: the wheel, the orthographic picture, the first-person mode - `Y2`-`Y5`, `Y10`); and it re-walks the
> **concept, naming and framework** of the three-dimensional vocabulary a host meets first (`Y6`, `Y8`, `Y12`), the
> shadow caster's contract (`Y7`) and the sky's (`Y9`). It does not re-walk the frame machinery, the threading model or
> the backend's memory tiers.
> **Sources**: `src/viz/graphics/sdk/vine/graphics/{Camera,CameraSnapshot,CameraManipulator,OrbitCameraManipulator,
> AutoTransform,Light,RenderState,ShaderAbi,Topology,PositionAttitudeTransform}.hpp`, `src/viz/graphics/src/{Camera,
> OrbitCameraManipulator,AutoTransform,LightAbi,BuiltinShaders,ChainPresets,ChainBuilder,Frustum}.{cpp,hpp}`,
> `src/viz/graphics/shaders/**`, `src/plugins/gfx_backend_vk/src/internal/{ContentDraw,StateMapping}.{cpp,hpp}`,
> `src/plugins/{demo_plugin,model_viewer}/**`, `tests/test_graphics/GraphicsTest.cpp`, `.ai/design/graphics-*.md`,
> `.ai/decisions/ADR-2026-11-26-*.md`.
> **Backlog**: `../backlog/graphics.md` - this round adds no entry; §6 lists what it would add, for approval.
> **Numbering**: `Y#` is this page's own set (review 11 used `V#`, review 17 `W#`). The module's `F#` / `C#` / `R#` /
> `N#` / `P#` / `D#` / `T#` / `G#` remain the design pages'.
> **Rule of evidence**: every finding names a `file:line`, a test, or a number produced by a command that is quoted
> beside it. A peer statement is a statement about that peer's public source, quoted from the file read this round,
> not a benchmark. Where a peer's source could not be read, it is not cited.

## 1. Why this round is needed

Review 11 asked whether each quantity along the shading chain has the right mathematical meaning; review 17 whether
picking and the picture read the same facts. Neither asked the question a user meets before either: **does the camera
go where the wheel says, in the picture type I am in?** And both were written against a tree that has since moved -
nine findings landed, and one landing (`W3`, the back-face normal) changed what an older finding (`V3`, the mirror)
means.

Three questions:

1. **Re-review.** Of `V1`-`V10` and `W1`-`W9`, what is landed, what is still in the tree, and which landing changed
   the meaning of another finding? (§4, `Y1`, `Y8`)
2. **Semantics.** In an orthographic picture, on the wheel, in first-person, under a mirror: does the camera and the
   shading do what their own headers say? (`Y1`-`Y5`, `Y10`)
3. **Concept and framework.** Which convention has no owner (the world's up axis, the orthographic scale, the
   caster's contract, the sky's depth), and which name describes its neighbour? (`Y6`-`Y9`, `Y12`)

### 1.1 Which peers, and why

| Peer | Source read this round | Why it is a suitable reference |
|---|---|---|
| three.js | `examples/jsm/controls/OrbitControls.js` (`update`, `_getZoomScale`, `_dollyIn` / `_dollyOut`) | The public reference for what a wheel does to each camera type, and for a proportional dolly |
| three.js | `src/renderers/webgl/WebGLShadowMap.js` (`getDepthMaterial`) | What a shadow depth pass copies from the visible material - the caster's contract |
| three.js | `src/renderers/shaders/ShaderLib/backgroundCube.glsl.js` | How a background is placed at infinity rather than at a world distance |
| Vulkan | `VkFrontFace` reference page (the area formula and the two enumerators) | To DERIVE the front-face constant review 17's follow-up measured, so the measurement and the specification agree |
| glTF 2.0 | The specification's coordinate system (right-handed, +Y up) | The import format the material packing already claims, and the axis a Z-up world has to answer for |

OSG's own sources could not be read this round, so OSG is cited only for the names this tree inherited from it and
nothing is asserted about its behaviour. The comparison is not a dependency and copies no code.

## 2. What the tree states today

| Topic | Current meaning | Home |
|---|---|---|
| World up | Z-up, as one private constant of the manipulator, one default light direction and one default billboard axis; stated in no header, guide or design page | `OrbitCameraManipulator.cpp:30-32`, `Light.hpp:111`, `AutoTransform.hpp:138`, `ChainPresets.cpp:44` |
| Camera defaults | eye `(0,0,5)`, up `(0,1,0)`, 45 deg, 0.1..1000, aspect 1; `fieldOfView` "perspective only" | `Camera.hpp:173-177, 250-258` |
| Orthographic scale | Not a named quantity: the window is `distance * tan(fov / 2)` whenever the manipulator applies | `OrbitCameraManipulator.cpp:742-752` |
| The wheel | Translates the orbit CENTRE (and with it the eye) by `distance * (1 - 1/1.15)` toward the anchor; perspective and orthographic alike | `OrbitCameraManipulator.cpp:490-495` |
| `dolly()` | Multiplies the distance; reads the dolly-to-cursor switch; called by tests only | `OrbitCameraManipulator.cpp:169-207`, `GraphicsTest.cpp:1076, 1327, 1383` |
| `Mode::FirstPerson` | Left-drag calls `orbit()`; WASD moves the eye; `Mode::Pan` / `Mode::Zoom` are compared nowhere | `OrbitCameraManipulator.cpp:425-427, 506` |
| Front face | SDK counter-clockwise; vk `COUNTER_CLOCKWISE`, measured; a mirrored placement reads BACK | `StateMapping.hpp:36-51`, `vine_material.glsl:86-89` |
| Shadow casters | Every drawable of the content, through the depth-only program; neither the alpha mode nor the opacity is read | `ChainBuilder.cpp:281`, `builtin_depth.frag:12-14` |
| Sky | A world-sized box, transformed like any geometry; the host sizes it against the far plane | `builtin_skybox.vert:33`, `DemoScene.cpp:385-390, 408-412` |
| Per-draw data | The push range (projection, model-view) and an 80-byte draw block whose `model` is written for every draw and read by no shipped stage | `ShaderAbi.hpp:135-138`, `ContentDraw.cpp:488` |

What is right and should stay: the presented-picture pick ray now unprojects at the projection's own two ends
(`Camera.cpp:227-228`) and answers the same for an off-centre window; the pointer names a pixel's centre
(`OrbitCameraManipulator.cpp:563-564`); the cull default is a stated decision; the frustum is extracted from the SDK's
own clip range with the p-vertex test; the spot's cone and the directional's sign agree with the block's packing
(`vine_params.glsl:101-152`, `LightAbi.cpp:156-181`); and the flat preset's derivative normal was oriented by
measurement (`builtin_forward.frag:54-58`).

## 3. Findings

### Y1 (face orientation x lighting / high): a mirrored subtree is now lit from behind on the DEFAULT path - `W3` changed what `V3` costs

**Evidence.** `vnFacingNormal` turns the interpolated normal on a back face (`vine_material.glsl:86-89`), both shading
stages call it (`builtin_forward.frag:63`, `builtin_gbuffer.frag:49`), and `gl_FrontFacing` follows the winding: the
pixel case pins "a mirrored placement reads back" (`.ai/memory/graphics.md`, "review 17's follow-up"). A negative
scale is legal (`PositionAttitudeTransform.hpp:28-29`, `AutoTransform.hpp:108`) and so is a mirror inside an instance
matrix (`builtin_forward.vert:21`, a `mat4` per instance).

**The geometry.** A mirror is an orthogonal matrix, so a vertex normal transforms with it and still points outward:
for `S = diag(-1, 1, 1)` and an outward normal `+z`, `(S^-1)^T n = S n = +z`, toward the viewer. The winding flips.
So on a mirrored solid the rasteriser says BACK, `vnFacingNormal` negates a normal that already faces the viewer, and
every visible face is shaded as the solid's far side. Replayed for the triangle `(0,0,0), (1,0,0), (0,1,0)` seen from
`+z`, with Vulkan's own area formula in framebuffer coordinates:

```
authored: framebuffer area +0.50 -> front face; true outward normal (0,0,1); vnFacingNormal answers (0,0,1)
mirrored: framebuffer area -0.50 -> BACK   face; true outward normal (0,0,1); vnFacingNormal answers (0,0,-1)
```

**What changed.** Before `W3` a mirror cost culling only, which is what §9.2 still records: "with culling on, a
mirrored solid's outer surface is culled as a back face ... (trigger: the first mirrored part that must be culled)"
(`graphics-scene.md:291-294`). The engine default is `CullMode::None`, so today the cost is SHADING with nothing
enabled, and the recorded trigger never fires: a mirrored part is lit wrongly whether or not anyone culls it. The FLAT
preset does not have the defect (its derivative normal always faces the viewer, `builtin_forward.frag:58`), so the two
presets disagree about one object.

**Peer.** three.js sets `frontFaceCW` from the world matrix's determinant so that a mirrored object is still the
AUTHORED front to the shading (cited in review 11 `V3` from its source); that is the missing half.

**Fix.** Three ways, for the owner: (a) state the mirror in the draw - `vn_draw.params.z` is reserved and packed as
zero today (`vine_params.glsl:58-61`, `ShaderAbi.hpp:137`); one bit there, and for an instanced draw
`determinant(mat3(vn_InstanceModel)) < 0` in the shader, let `vnFacingNormal` flip on `gl_FrontFacing != mirrored`;
(b) a dynamic front face from the determinant per draw (needs `VK_EXT_extended_dynamic_state`, and a per-instance
mirror still needs batching); (c) refuse a negative scale at the setter and report, as non-power-of-two sample counts
are refused. In all three, §9.2's sentence and trigger are rewritten. **code + doc + test** (a mirrored box, pixels
lit as its unmirrored twin).

---

### Y2 (navigation / high on a supported path): in an orthographic picture the wheel moves the eye and changes nothing the picture shows

**Evidence.** `onScroll` adds `dir * step` to `center_` and calls `apply()` (`OrbitCameraManipulator.cpp:494-495`); the
eye follows the centre (`:377`) and `distance_` is not touched. The orthographic window is a function of `distance_`
alone (`updateOrthoProjection`, `:742-752`), so an orthographic picture - which does not depend on where the eye is
along the view axis - does not change. The one function that scales an orthographic window, `dolly()`, is called by
tests only. When the travelling eye reaches the scene's box, `clampEyeOutsideBounds` pins it just outside
(`:728-730`) by RAISING `distance_`, and from then on every further notch grows the window and carries its centre -
the look-at target - away from the model.

**Measured**, replaying `onScroll` and `apply()` on a box `y in [-1, 1]`, a side view along `-y`, centre at the
origin, `distance_ = 10`, fov 45 deg:

```
notch  0: centre y   0.000  eye y 10.000  window half-height 4.142
notch  1: centre y  -1.304  eye y  8.696  window half-height 4.142
notch  7: centre y  -9.130  eye y  1.203  window half-height 4.280
notch 10: centre y -13.736  eye y  1.295  window half-height 6.226
notch 15: centre y -26.517  eye y  1.550  window half-height 11.626
```

Scrolling IN: six notches of nothing, then the window grows by about 13 per cent a notch (the picture zooms OUT)
while the model slides out of the frame.

**Reach.** The class documents both projection types as supported through one distance ("which drives the perspective
dolly step and the orthographic frustum height", `OrbitCameraManipulator.hpp:32-34`) and `fitToScreen` frames an
orthographic camera (`:263-269, 284-289`); the model viewer forwards the wheel to the manipulator
(`ModelRenderView.hpp:117`). No shipped host switches to orthographic today, and no test sends a scroll to the
manipulator at all (§`Y4`), which is how the path stayed green.

**Peer.** three.js `OrbitControls`: for an orthographic camera the wheel sets `object.zoom` and calls
`updateProjectionMatrix()`; with `zoomToCursor` it then moves the camera so the point under the cursor stays put
(`update`, the `isOrthographicCamera` branches, read this round). The picture changes; the eye is the afterthought.

**Fix.** The wheel in an orthographic picture is a ZOOM: scale the window (or the named quantity `Y5` asks for) by
`wheel_factor_` per notch about the anchor, as `dolly()` already does; moving the eye is worth nothing there. Add a
case: an orthographic camera, one wheel notch, the window's height asserted. **code + test.**

---

### Y3 (navigation / medium-high): the wheel's step does not track the zoom level - it is a constant world length until the centre is re-aimed

**Evidence.** `step = distance_ * (1 - 1 / wheel_factor_)` (`OrbitCameraManipulator.cpp:492-493`), and the same step
is added to `center_` (`:494`), so `distance_` - the eye-to-CENTRE distance - is invariant under the wheel. The comment
says the step is "a fraction of the current eye-to-centre distance per notch so the dolly speed tracks the zoom level"
(`:490-491`): that distance does not change while the wheel is used, so the speed is constant. The release rule that
follows (`:479-487`, "once the eye is essentially at the surface ... release the anchor") exists because the step does
not shrink.

**Measured** from a fitted orbit of 10 units, `wheel_factor_ = 1.15`:

```
 1 notch: eye travelled  1.30 units, eye-to-centre distance still 10.00; three.js radius = 9.50
 8 notches: eye travelled 10.43 units, eye-to-centre distance still 10.00; three.js radius = 6.63
20 notches: eye travelled 26.09 units, eye-to-centre distance still 10.00; three.js radius = 3.58
```

Eight notches carry the eye to the centre of a model that `fitToScreen` just framed (its near surface is about four
notches away), at a speed that never eases.

**Peer.** three.js `OrbitControls` multiplies the radius every notch: `_getZoomScale` is `Math.pow(0.95, zoomSpeed *
normalizedDelta)` and `update` does `spherical.radius = clamp(spherical.radius * _scale)` (read this round). With
`zoomToCursor` the camera moves down the pointer ray by the radius difference - proportional to the distance to the
target, not to a picked surface.

**In the tree.** The demo's sky relies on the opposite premise: "The manipulator zooms multiplicatively from a ~10-unit
orbit, so that is a long way out" (`DemoScene.cpp:411-412`). It does not.

**Fix.** Scale the notch by the distance to the anchor while the anchor is on the ray (`d * (1 - 1 / wheel_factor_)`,
a geometric approach that cannot overshoot), or keep the eye-to-centre distance as the radius and move the eye only.
Either is local; `DemoScene.cpp:411` is then true again or is corrected. **code + test.**

---

### Y4 (navigation / medium): `setDollyToCursor` has no reader on the wheel path, and the wheel path has no test

**Evidence.** `dolly_to_cursor_` is read in one place, `dolly()` (`OrbitCameraManipulator.cpp:183`); `dolly()` has no
caller outside the tests (`GraphicsTest.cpp:1076, 1327, 1383`). The header documents the switch as "whether a wheel
dolly keeps the cursor anchor fixed on screen" (`OrbitCameraManipulator.hpp:136-144`), and the wheel
(`onScroll`, `:449-496`) dives at the anchor unconditionally. No host calls `setDollyToCursor`. The only scroll a test
sends goes to a recording fake (`GraphicsTest.cpp:4715, 4778`); `OrbitCameraManipulator::onScroll` itself has no case,
so `Y2`, `Y3` and the release rule are all untested.

**Consequence.** A host that turns the switch off gets a wheel that still dives at the cursor; a host that calls
`dolly()` gets the documented behaviour on a path nothing drives.

**Fix.** Decide which path owns the switch: read it in `onScroll` (when off, step along the view axis and slide the
centre by the anchor rule `dolly()` already has), or delete it with `dolly()`'s branch and say the wheel always
anchors. In both, one scroll test per projection type. **code + doc + test.**

---

### Y5 (camera concept / medium): the orthographic scale has no name - it is `distance * tan(fieldOfView / 2)` - and the manipulator overwrites the window the camera keeps whole

**Evidence.** `Camera::fieldOfView` is documented "perspective only" (`Camera.hpp:173-177`); the manipulator's
orthographic paths read it three times as the zoom coupling (`OrbitCameraManipulator.cpp:267, 285, 748`), and `pan`'s
orthographic branch rebuilds the window from it (`:154-158`) instead of reading `Camera::orthographicHeight()`
(`Camera.hpp:179-183`), which exists for that. `Camera::setOrthographic` keeps all four bounds so that "an off-centre
window - a tiled view, one eye of a stereo pair, a shifted shadow frustum - survives the round trip"
(`Camera.hpp:130-132`; `CameraTest.TheOrthographicWindowIsKeptWhole`, `GraphicsTest.cpp:924`) - but every `apply()`
replaces the window with a centred one (`:742-752`).

**Measured** for the window that case uses, `(-2, 6, -1, 3)` with a 600-pixel picture and `distance_ = 10`:

```
authored window height 4.000 -> 0.00667 world units/pixel; manipulator assumes 0.01381 (2.07x too fast);
apply() rewrites the window to height 8.284
```

A pan on such a camera moves at twice the grabbed point's speed, and the first gesture erases the window.

**The header disagrees with itself.** `dolly()`: "the projection is untouched, which is what keeps an orthographic
view's extent unchanged" (`OrbitCameraManipulator.hpp:95-96`); the class note: the distance "drives ... the
orthographic frustum height" (`:32-34`); the code scales the window (`:742-752`, and the cases at
`GraphicsTest.cpp:1368-1373` assert it follows the aspect). In an orthographic camera `dolly` IS a zoom.

**Peer.** three.js `OrthographicCamera.zoom` is a named property and the window is the camera's own state; nothing
re-derives it from a perspective parameter.

**Fix.** Give the scale a name on the camera (the window's height, or a `zoom`), have the manipulator edit that and
never infer it from `fieldOfView`; `pan` reads `(top - bottom) / picture height`; correct the two header sentences.
**code + doc + test** (a pan on an authored off-centre window; a `dolly` on an orthographic camera asserting the
window).

---

### Y6 (concept / medium): Z-up is the engine's convention by its defaults and by one private constant, and it is declared nowhere a host reads (review 11 `V1`, sharpened)

**Evidence for Z-up.** `kWorldUp{0,0,1}` - "Vine scenes are Z-up (robotics convention: X forward, Z up)"
(`OrbitCameraManipulator.cpp:30-32`); `Light::createDirectional` defaults to `(0,0,-1)`, straight down in a Z-up world
(`Light.hpp:111, 353`); `AutoTransform::axis` defaults to `(0,0,1)`, "Z by default" (`AutoTransform.hpp:138, 257`); the
shadow camera's reference axis is Z unless the light is near it (`ChainPresets.cpp:44`); the demo says so three times
(`DemoScene.cpp:557, 924, 966`).

**Evidence against a declaration.** `Camera`'s defaults `(0,0,5) / (0,1,0)` (`Camera.hpp:250-252`) state nothing;
`grep -i "z-up"` over `docs/guides/graphics.md`, the eight `graphics-*.md` design pages and the ADRs is empty;
`src/loaders` has no `up_axis`. Review 11's own table read the camera default as "implies right-handed Y-up" - the
wrong inference: an eye on `+z` with an up of `+y` IS the Z-up top view.

**Consequence.** The world's up axis is not a parameter of anything: a host with Y-up content (glTF is Y-up by
specification, and the importer keeps the file's axes) gets a manipulator whose horizon is Z - `fitToScreen` "Frame the
model level: drop any accumulated roll" re-levels to Z (`:293-295`), `syncFromCamera` reads pitch as `asin(dir.z)`
(`:676`), `pivotRotate` yaws about Z (`:646`) - a sun whose default shines down the model's up, and a glTF part lying
on its back. The demo reorients its sky by hand (`DemoScene.cpp:269-270`).

**Fix.** The ADR review 11 asked for, now with its evidence in one place: right-handed, Z-up, the world unit is the
content's. Then `OrbitCameraManipulator::setWorldUp` (a host states its own horizon), the convention in
`docs/guides/graphics.md`, and a decision for the import boundary - a root rotation for a format that states Y-up,
reported, or nothing. Trigger for the import half: the first glTF asset in a Z-up scene. **ADR + doc (+ code).**

---

### Y7 (shadows / medium): a Mask caster casts its quad and a faded part casts in full - the shadow pass reads neither the alpha mode nor the opacity

**Evidence.** The shadow pass overrides every drawable's program with the depth-only one
(`desc.program_override = depthOnlyProgram()`, `ChainBuilder.cpp:281`), whose fragment stage is empty
(`builtin_depth.frag:12-14`): there is no discard and no texture read. The opacity (`vn_draw.params.x`) is not
consulted either. Nothing in `graphics-rendering.md` 2.7 states what casts.

**Consequence.** A glTF `alphaMask` leaf, fence or grate casts a solid quad - the cut-out the colour pass draws is not
the shape the shadow has. And a part ghosted with `setOpacity(0.1)` - the engine's stated CAD use of the opacity channel
(`RenderState.hpp:90-96`) - throws a full shadow, which contradicts the picture it is in.

**Peer.** three.js copies the visible material onto the depth material before the shadow pass: `result.alphaMap =
material.alphaMap; result.alphaTest = ...; result.map = material.map;` (`WebGLShadowMap.js`, `getDepthMaterial`, read
this round), so an alpha-tested surface casts its holes.

**Fix.** Two decisions: (a) a Mask caster's depth stage is the textured variant with the SAME threshold text the two
shading stages share (`builtin_forward.frag:88-96`); (b) a drawable below an opacity does not cast - or casts dithered,
which is a technique and not a switch. State both in 2.7's shadow row; (a) is a program variant, (b) is one comparison
where the caster list is collected. **code + doc + test** (a masked quad casts holes, in pixels).

---

### Y8 (host-program contract / medium-low): the draw block's `model` is written for every draw and read by no stage, and the GLSL names the model-VIEW `vn_model`

**Evidence.** `VineDrawBlock` is `model` (64 bytes, "Object -> world") plus `params` (16): 80 bytes
(`ShaderAbi.hpp:135-138, 267`), packed at every recorded draw (`ContentDraw.cpp:488`, `DrawAbi.cpp:7`).
`grep "vn_draw\.model"` over `src` and `tests` - GLSL included - is empty. The host-program ADR says the RANGE "carries
no separate model matrix"; a host program can in fact read the world model here, and no sentence says so. In the vertex
stages a local is called `vn_model` and holds the model-VIEW (`builtin_forward.vert:62`, `builtin_gbuffer.vert:54`),
beside a block member `model` that is a different matrix: three names for two matrices, and `vine_params.glsl:58`
calls the block "the model matrix and four scalars".

**Fix.** Decide with review 11 `V2`: the normal matrix `V2` asks for is the first real consumer a draw block's matrix
half would have. Either `model` goes (80 -> 16 bytes, one ABI bump, in the same batch as `V2`), or the contract states
it. Rename the local `vn_model` to `vn_model_view` in both stages. **ADR + ABI (+ doc).**

---

### Y9 (sky / medium-low): the sky has no depth contract - it is a world-sized box the host must keep the camera inside of and size against the far plane

**Evidence.** `builtin_skybox.vert:33` transforms the box like any geometry (`projection * modelView * vertex`); the
direction comes from an authored channel. The demo states the consequences itself: the half extent is chosen "inside
the camera's far plane (1000) even at its corners" (`DemoScene.cpp:385-390`), and "a viewer that flies outside the box
would see it from the outside" (`:408-412`). `fitToScreen` sets the far plane to `100 * distance`
(`OrbitCameraManipulator.cpp:282-283`), so a static box is right for one content scale only.

**Peer.** three.js `backgroundCube`: `vWorldDirection = transformDirection( position, modelMatrix ); ...
gl_Position.z = gl_Position.w; // set z to camera.far` (`backgroundCube.glsl.js`, read this round): the background is
a DIRECTION, placed at the far plane, at no world distance.

**Fix.** The skybox vertex stage writes `gl_Position.z = gl_Position.w` (the SDK clip far plane; after the fold it is
depth 0) and passes the view direction, and the pass that draws a sky states its depth policy for it (a fragment AT the
far plane does not pass a strict compare against a buffer cleared to 0, so the sky's pass tests `GreaterEqual` or
draws first). The host's box then carries no size. **code + doc + test.**

---

### Y10 (navigation / medium-low): a first-person look is an orbit, and two of the four modes are compared nowhere

**Evidence.** In `Mode::FirstPerson` a left-drag calls `orbit()` (`OrbitCameraManipulator.cpp:425-427`), whose `apply()`
puts the eye at `center + eyeDirection * distance_` (`:376-377`): the eye travels an arc of radius `distance_` around
the centre while the user looks. Measured for `distance_ = 10`: a 0.5 rad look moves the eye 4.95 units. `Mode::Pan` and
`Mode::Zoom` are never read - the two comparisons are `== Mode::FirstPerson` and `!= Mode::FirstPerson` (`:425, 506`) -
so they behave as `Orbit`; no test sets a mode and no host calls `setMode`.

**Fix.** A first-person look rotates forward and up about the EYE (what `pivotRotate` does with the pivot at the eye)
and keeps the target `distance_` ahead; delete `Pan` and `Zoom` from `CameraManipulator::Mode`
(`CameraManipulator.hpp:36-42`) or define them. **code + test.**

---

### Y11 (documentation / low): sentences that the review-17 landings left behind

| Where | Says | The tree |
|---|---|---|
| `Camera.hpp:25`, `Camera.cpp:181` | "the view block that builds it" / "the view block ... reads" | The view block was deleted (`ADR-2026-11-26-host-program-contract-is-the-push-range`) |
| `BuiltinShaders.cpp:125-126` | light clip "z 0 at the near plane to 1 at the far one" | The SDK's range is -1..+1; the code at `:137` is right (review 11 `V6`, still open) |
| `BuiltinShaders.cpp:196-198` | shadow bindings "set 0 / 3 ... 4 ... the lights take 5" | The code at `:205` uses 7 / 8 and the lights are at 6 (`vine_params.glsl:84`) |
| `builtin_forward.vert:29, 77` | "see SceneBridgePipeline" | The retired backend's file; the fold is `ContentDraw::foldToDeviceClip` |
| `DrawListBake.hpp:429` | "Quantized camera-space depth (near = larger)" | Depth grows away from the eye (review 11 `V10`, still open) |
| `Light.hpp:199` vs `:192` | direction "only meaningful for directional lights" / "true for directional and spot" | A spot has one (`has_direction`, `LightAbi.cpp:165`) |
| `CameraManipulator.hpp:127-128` | `onResize` refreshes "typically the camera projection aspect ratio" | The manipulator deliberately does not (`OrbitCameraManipulator.cpp:541-543`) |
| `DemoScene.cpp:411` | "The manipulator zooms multiplicatively" | It translates by a constant step (`Y3`) |

**doc.**

---

### Y12 (naming / low): one property, two names, in two classes the header calls counterparts

`PositionAttitudeTransform::attitude()` / `setAttitude()` (`PositionAttitudeTransform.hpp:59-69`) and
`AutoTransform::rotation()` / `setRotation()` (`AutoTransform.hpp:84-94`) are the same quaternion in the same
composed matrix `T * R * S * T(-pivot)`; the class note says "scale and pivot are the same as
PositionAttitudeTransform's" (`:38-39`) and does not say why the rotation is not. The split is inherited from OSG
(`osg::PositionAttitudeTransform::setAttitude`, `osg::AutoTransform::setRotation`). A reader who looks for the
counterpart's setter finds nothing. **doc, or one rename at the next ABI touch.**

## 4. Re-review of reviews 11 and 17 on `a546323c`

### 4.1 Review 11 (`V#`)

| # | Status | What the re-review found |
|---|---|---|
| `V1` | OPEN, sharpened | No owner for handedness / up / units. The convention IS Z-up - by five defaults and one private constant (`Y6`) - and review 11's "implies Y-up" reading of the camera default was itself the wrong inference |
| `V2` | OPEN | `mat3(vn_model) * vn_Normal` at `builtin_forward.vert:68` and `builtin_gbuffer.vert:60`; the instance matrix makes a non-uniform scale reachable without any node stating one. `Y8` names the batch it belongs to |
| `V3` | **CHANGED** | The consequence grew: `W3` made a mirror shade from behind with culling off, and §9.2's recorded trigger ("the first mirrored part that must be culled") no longer describes when it fires (`Y1`) |
| `V4` | OPEN | `Geometry.hpp:489` still promises a derivation no code performs |
| `V5` | OPEN, one more reader | `setPerspective` / `setOrthographic` still validate nothing, `eye == target` still answers the identity view, and `up()` still reports the stored up rather than the basis. New: `pan` builds its axes from `cam->up()` (`OrbitCameraManipulator.cpp:151-152`), so on a camera whose authored up is zero or parallel to the view - a case `lookAt` resolves - a pan does nothing, silently (`Vec3::normalize` keeps a zero vector zero, `Vector3.hpp:367-375`, so it is not NaN) |
| `V6` | OPEN (doc) | The two wrong sentences: `BuiltinShaders.cpp:125-126` is still there; `CameraSnapshot.hpp` was corrected by `W1` |
| `V7` | OPEN, sharpened | The `+ 1` in `1 / (d * d + 1)` (`vine_params.glsl:128`) has the unit of a length squared: the engine's light fixes a world unit of its own, against a camera `fitToScreen` scales to the content (`OrbitCameraManipulator.cpp:279-283`) and a `createPoint` default range of `10` (`Light.hpp:119`). Also unstated in `Light.hpp`: with no ambient light announced, a 0.15 fill is applied (`LightAbi.cpp:185-189`) |
| `V8` | OPEN | One compare, the first caster, a normalised bias. `Y7` adds what the depth pass does not read |
| `V9` | OPEN | The Mask threshold is still `params.x * alpha < cutoff` (`builtin_forward.frag:92`, `builtin_gbuffer.frag:65`) |
| `V10` | OPEN | `DrawListBake.hpp:429` |

### 4.2 Review 17 (`W#`)

| # | Status | What the re-review found |
|---|---|---|
| `W1` | LANDED, correct | `Camera.cpp:227-228` unprojects at +1 and -1; the near end is chosen by distance to the eye; the off-centre case exists (`GraphicsTest.cpp:12307`). One residue: the two producers now answer different ORIGINS for an orthographic camera - the live camera places it on the EYE plane (`Camera.cpp:167`), the snapshot on the NEAR plane (`:256`) - and a hit's `distance` is measured from the ray's origin (`RayIntersection.cpp:218`), so the same pick reports two distances. Only their ORDER is read in the tree; the header of `RayIntersectionResult` should say the origin is the producer's |
| `W2` | LANDED | `applyStateDefaults` / `resolveRenderState` take `transparent` (`StateResolution.cpp:57-70, 90-104`); `BlendState.hpp`'s rule at `RenderState.hpp:88-98` now matches |
| `W3` | LANDED, with a consequence | The normal turn (`vine_material.glsl:86-89`), the import (`MeshLoader.cpp:518-541`, `SceneBuild.cpp:110-117`), the stated default (`graphics-scene.md:96-99`). Its interaction with the mirror is `Y1` |
| `W3` follow-up | VERIFIED by derivation | `kFrontFace = COUNTER_CLOCKWISE` (`StateMapping.hpp:51`) was reached by a pixel probe. The specification agrees: Vulkan computes the area in FRAMEBUFFER coordinates as `a = -1/2 * sum(x_i * y_(i+1) - x_(i+1) * y_i)` and `COUNTER_CLOCKWISE` means a positive area is front-facing (`VkFrontFace`, read this round). The Y fold negates clip y; the framebuffer's y grows down; two reflections leave the picture's counter-clockwise polygon counter-clockwise - the note at `StateMapping.hpp:29-38` states exactly this.
Measured AND derived: nothing left to pin |
| `W4` | LANDED | `(index + 0.5)` (`OrbitCameraManipulator.cpp:563-564`); the hidden scene and the zero-opacity subtree are not picked; what a pick does not read is stated (`OrbitCameraManipulator.hpp:181-184`, `RayIntersection.hpp:65-71`) |
| `W5` | LANDED, one channel left | `VinePushRange` is the contract (`ShaderAbi.hpp`); the draw block's `model` is a second, unstated one (`Y8`) |
| `W6` | LANDED | `ADR-2026-11-26-linear-light-is-encoded-by-the-target-format` |
| `W7`, `W8` | LANDED | `graphics-geometry.md` 2.11; `PassDesc::clear_depth` |
| `W9` | LANDED | `Node::setOpacity` clamps (`Node.cpp:197-205`); a zero fold emits no command |

## 5. Peer material not to copy

| Peer mechanism | This module's judgement |
|---|---|
| three.js `OrbitControls`' fixed `target` and `spherical.radius` model | The anchor pick IS the CAD feature to keep: a rotate about the grabbed point and a wheel that dives at it are what this manipulator does better. What transfers is the two arithmetic facts (`Y2`, `Y3`): the orthographic wheel is a zoom, and the step is multiplicative |
| three.js `zoomToCursor` without a pick | It moves down the pointer ray by a radius difference and never asks what is under the cursor; this engine's anchor is the better answer for "zoom to the part", and the release rule should shrink with it rather than be a fixed three near planes |
| three.js `MeshDepthMaterial` copies (`alphaTest`, `clipShadows`, `displacementMap`) | Only `map` / `alphaTest` transfer (`Y7`); clipping planes are not a pick fact in this engine either (review 17 `W4`) |
| three.js `backgroundCube` at `gl_Position.z = gl_Position.w` | Transfers with one addition (`Y9`): a reverse-Z buffer cleared to 0 needs the sky's pass to compare `GreaterEqual`, which three.js's depth range does not |
| An engine-fixed world unit (metres) | Still rejected (review 11 §4). What `Y6` and `V7` add is that the engine ALREADY fixes one where nobody reads it - the `+ 1` of the attenuation and the `1.0` margins of the light cameras (`ChainPresets.cpp:104-106`) |
| OSG's `OrthoManipulator` beside the orbit manipulator | Two manipulators for one camera is the shape this engine's "one camera, one picture shape" rule exists to avoid; the answer is one manipulator that knows the projection type (`Y2`, `Y5`) |

## 6. Proposed order (for the owner to approve; not automatically added to the backlog)

1. **`Y1`** - `W3`'s landing made an existing limit visible on the default path; one reserved bit and one shader
   line, with `graphics-scene.md` §9.2 rewritten. The only finding that changes a shipped picture today.
2. **`Y2` + `Y3` + `Y4`** - one batch on the wheel, with the first tests that send a scroll to the manipulator: the
   orthographic zoom, the proportional step, and the switch's owner decided.
3. **`Y5`** - a named orthographic scale; `pan` reads the camera's own window. Same batch as `Y2`, which needs the
   name to zoom.
4. **`Y6`** - the convention ADR (with `V1`, `V7`'s unit sentence and `V6`'s two corrections), then `setWorldUp`.
5. **`Y7`** - the caster's two decisions, stated in 2.7; the masked depth variant when a Mask asset casts.
6. **`Y8` with `V2`**, **`Y9`, `Y10`, `Y11`, `Y12`** - decisions and sentences.

Backlog candidates (**not added**): the mirrored face (`Y1`; trigger: none - present now); the orthographic wheel
(`Y2`; trigger: the first host that offers an orthographic view); the wheel step and its tests (`Y3` / `Y4`; trigger:
none - the model viewer uses this path); the caster's contract (`Y7`; trigger: the first `alphaMask` asset that
casts).

## 7. Verification commands

```bash
# Y1: the turn, and where the mirror is legal
sed -n '86,89p' src/viz/graphics/shaders/vine_material.glsl
sed -n '291,294p' .ai/design/graphics-scene.md
# Y2 / Y3 / Y4: the wheel, the step, the switch's one reader, and the tests that call dolly
sed -n '449,496p' src/viz/graphics/src/OrbitCameraManipulator.cpp
grep -n "dolly_to_cursor_" src/viz/graphics/src/OrbitCameraManipulator.cpp
grep -rn "onScroll(" tests src --include=*.cpp --include=*.hpp | grep -v "void onScroll"
# Y5: the parameter documented "perspective only", read on the orthographic paths
sed -n '173,183p' src/viz/graphics/sdk/vine/graphics/Camera.hpp
grep -n "fieldOfView()" src/viz/graphics/src/OrbitCameraManipulator.cpp
grep -n "orthographicHeight" src/viz/graphics/src/OrbitCameraManipulator.cpp   # (no hit)
# Y6: the convention's five homes, and the pages that do not carry it
grep -rn "kWorldUp" src/viz/graphics/src/OrbitCameraManipulator.cpp
grep -rniI "z-up" docs/guides/graphics.md .ai/design/graphics-*.md .ai/decisions   # (no hit)
grep -rn "up_axis" src/loaders                                                            # (no hit)
# Y7: the caster's program and its fragment
sed -n '276,283p' src/viz/graphics/src/ChainBuilder.cpp
cat src/viz/graphics/shaders/builtin_depth.frag
# Y8: the member no stage reads
grep -rn "vn_draw\.model" src tests    # (no hit)
sed -n '135,138p' src/viz/graphics/sdk/vine/graphics/ShaderAbi.hpp
# Y9: the sky's placement
sed -n '33p' src/viz/graphics/shaders/builtin_skybox.vert
# Y10: the modes that are compared
grep -rn "Mode::Pan\|Mode::Zoom\|Mode::Orbit\|Mode::FirstPerson" src --include=*.cpp --include=*.hpp | grep -v AutoRotateMode
# §4: review 11's open sentences
grep -rn "mat3(vn_model)" src/viz/graphics/shaders
grep -rn "derives normals" src/viz/graphics
grep -n "near = larger" src/viz/graphics/src/DrawListBake.hpp
sed -n '125,126p' src/viz/graphics/src/BuiltinShaders.cpp
```
