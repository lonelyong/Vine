# graphics rendering: targets, MSAA, image sources, colour, shading, shadows

> **Status**: current
> **Last verified**: 2026-11-02
> **Scope**: targets, MSAA, image sources, colour space, shading and shadows.
> **Sources**: `RenderTarget.hpp` / `ImageSource.hpp` / `Sampler.hpp` / `ShaderProgram.hpp` /
> `ShaderAbi.hpp` / `Light.hpp`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

This file defines the current design of graphics rendering: targets, MSAA, image sources, colour, shading and shadows: first the objects and their boundaries, then features, interfaces, constraints and acceptance.

### 1.2 Goals

- Turn §2's design points into §3's features, §4's interfaces and §8's acceptance.
- Keep "what it is now" separate from "why it was decided that way": the reasons are in §2/§7, history is in git.

### 1.3 Non-goals

- This file does not repeat the full signatures from the headers; an interface gives the contract and a pointer only.
- This file keeps no closed history or per-round review records.

### 1.4 One-paragraph conclusion

> `RenderTarget` is a description; the sample count is a target property; produced and imported image sources are not merged;  colour splits into linear light and sRGB encoding; shading goes through `ShaderProgram` + `ShaderAbi`; shadows are declared by lights.

## 2. Design Points

2.1 **Description separated from GPU resources**: `RenderTarget` is only a description; the backend builds the image from the description.
2.2 **The sample count is a target property**: `setSamples` → `TargetShape` → pipeline key → pass and image.
2.3 **Two image-source lifetimes**: produced (`ImageRef`) and imported (`Texture`) are not merged; only `ImageSource` resolves the shape uniformly.
2.4 **Colour goes into types**: `Colorf` linear light, `Color` sRGB encoding, one named pair of conversions.
2.5 **Shading is a thin interface**: GLSL source + typed entry points; `ShaderAbi` is the only contract between C++ and GLSL.
2.6 **A shadow is an ordinary pass**: the light declares it, the chain builds a depth-only pass, there is no engine-level shadow subsystem.
2.7 **Four homes for the shadow**: the request is on the light (`Light::setCastShadow` + `ShadowSettings`), the identity on the image (`RenderTarget::setShadowOf` and `setProducerViewProjection`, the plan only copies it into `CompiledPass::shadow`), the convention in the built-in ABI (`VineShadowBlock` and the shadow terms, an optional variant), the composition in the preset (`ChainBuilder`'s recipe). The engine core's `PassDesc` has no shadow field (see D5).

  **The built-in shadow pipeline's capability boundary** (the one `PassChain::build` builds, not the pass model's ceiling):
  - **One image**: each chain builds one shadow map, serving **one light** — the first enabled, `isCastingShadow`, buildable directional or spot light in the content. A second casting light gets no map.
  - **Two light cameras**: a directional light uses an orthographic fit (`directionalShadowMatrix`), a spot light a perspective fit (eye at the light position, cone angle = outer cone angle × 1.02, `spotShadowMatrix`); both exits are the same `VineShadowBlock` (one matrix), shading does not branch.
  - **Point lights unsupported**: a point light's shadow is six faces of the same map, and today's ABI has no multi-matrix / cube, so `setCastShadow` on a point light never builds a pass in this pipeline (backlog S6).
  - **Three reasons for not building are reported separately** (once per build, `UnsupportedRequest`): a second casting light / a point light that cannot be represented / this path produces no lighting at all (the host's own lighting program, or a mode that does not need shading). Counting them separately is what lets "N lights cast no shadow" be located instead of guessed.
  - **The pass names a colour-less program** (`BuiltinShaders::depthOnlyProgram()`, `program_override`): a depth map has no colour attachment, so the pass draws with a fragment stage that declares no output instead of each drawable's own program. The vertex stage stays the content's (`builtin_forward.vert`) - same push constants, same instancing, same position - which is why a triangle's depth and its colour cannot disagree; the price is that the stage still transforms a normal no fragment reads, and the alternative (a position-only stage) would declare inputs the geometry is not required to carry. Reportable effect: the shadow pass does no per-fragment material/lights/shadow work, and validation no longer says the fragment output it used to write was unused.

2.8 **Dynamic state is a single list**: the dynamic state the pipeline **declares** (`DynamicState`) and the dynamic state the commands **write** (`SetDynamicState`)
must be two faces of the same list, pinned by one test "every item the commands write is declared in the pipeline, and vice versa". Miss one declaration and the driver takes the create-info
baked value — the half the commands wrote is silently dropped, and the picture is wrong without an error. This also governs the **switches** in create-info: dynamic state can only change values, not
enables such as `depthBiasEnable`, so an enable must be fixed together with the list. Known violation: `../bugs/vsg-depth-bias-not-declared-dynamic.md`.

2.9 **Capabilities are two questions, readback has one "which image"**: `Capabilities` consists of two values, `Features` (is it supported) and `Limits` (how large is the maximum);
the limit fields are `Capabilities::limits.*` (source-level, BC-11), and the feature questions are `Features::supports` / `Features::bit` with `Features::Feature` as the enum.
The transitive bit-set spelling on `Capabilities` was deleted once its migration was done (BC-32): one question has one spelling. Both readback verbs `readColorBuffer` / `readDepthBuffer` take one `ImageRef` — the same vocabulary as a pass input's "which image";
the old `(target, attachment)` spelling is kept for one release and forwards (BC-16),
and so is the old answer shape (`bool` + `ReadbackResult*`): the answer is now the return value,
`ReadbackOutcome` (its `result` is the four-state `ReadbackResult`), and the two-answer shims are
`[[deprecated]]` for one release. Why: `G4` (one question, one answer) and `G6` (one "which image" does not get
two spellings).

2.10 **A public name states the thing, not the artifact that produced it** (naming, `G1` / `G4` / `G12`; audit-4 `T-12`, 2026-11-18):
five spellings changed, one migration note each, and **no `[[deprecated]]` alias was kept** - the module promises no source or ABI
stability (`graphics-frame-pipeline.md` 1.3), every in-tree consumer moved in the same commit, and an alias would leave two names in
every search, every guide table and every reader's head, which is the cost this batch removes (`G1`).

| Before | Now | What the old name got wrong | Note |
|---|---|---|---|
| `RenderCommand` | `DrawItem` | named the engine's artifact instead of the scene's one drawable; the industry order is item, then call | `BC-17` |
| `DrawDescription` | `DrawCall` (then `DrawPacket`, see the 2026-11-20 table) | "description" hid that it is *one drawing call* (a pass may issue several) | `BC-18` |
| `MaterialBase` | `MaterialValues` | a base-class name on the interface that is the **read face of a material's values** (`Material` is the asset, `MaterialInstance` the instance) | `BC-19` |
| `EngineNotReady` | `EngineNotReadyReason` | the type is a reason (`FrameCallOutcome::reason`); the name read as a state | `BC-20` |
| `createSession` / `releaseSession` | `createFrameDriver` / `releaseFrameDriver` | the noun became *frame driver* in S2 and the verbs stayed behind | `BC-21` |

- What it is: names only. `DrawItem` is still the per-frame value and `DrawPacket` still the per-pass request (`graphics-frame-pipeline.md`
  2.2 / 2.8); `MaterialValues` is still what a description publishes; `EngineNotReadyReason` still shares its enumerators with
  `prepare()`; and `createFrameDriver` still hands out a `RenderFrameExecutor` (that type's name did not change).
- Why now: the third round registered all five (`C-A3-02` / `C-A3-03` / `C-A3-08`, `A3-17`) and the fourth round corrected the
  register first - two of the words it had listed (`releaseResidency`, `SessionId`) no longer existed, so the batch was smaller than
  recorded and could be taken in one go.
- Rejected: keeping the old spellings as deprecated aliases (the shape B4 used for the readback answer). B4 migrated a *shape*
  whose in-tree callers still existed; a migrated *word* has no such callers left after the sweep, and the alias is exactly the
  double spelling the guide's 5.2 table warns about (`G1`). Also rejected: renaming only the cheap words - the register is read as
  one vocabulary, and a half-renamed pair (`DrawItem` beside `DrawDescription`) would be worse than either end.

**The 2026-11-20 batch: a draw packet is not a draw call, and a full-screen pass is not a screen** (`G1` / `G4` / `G12`;
[`ADR-2026-11-20-draw-packet-and-fullscreen-vocabulary`](../decisions/ADR-2026-11-20-draw-packet-and-fullscreen-vocabulary.md)).
The same rule as above, applied where the first batch stopped: the value was renamed and the two counts it had forced apart were
realigned to the industry's meaning.

| Before | Now | What the old name got wrong | Note |
|---|---|---|---|
| `DrawCall` / `OwnedDraw` | `DrawPacket` / `OwnedDrawPacket` | the value is one view's whole PACKET of `DrawItem`s; every profiler calls one submitted item a draw call, and the old name had to carry a warning saying so | `BC-28` |
| `isDrawingCall()` / `drawingCallItems()` / `PassDraw::draws` | `packetDraws()` / `drawItemsOf()` / `PassDraw::packets` | three more spellings of the same retired word (a predicate, a count and the container) | `BC-28` |
| `draws` (packets) + `draw_items` (items) in `FrameCounts` / `FrameCounters` / `PrepareReport` / `FrameResult` | `draw_packets` + `draws` (items) | a field called `draws` in a frame report is read against RenderDoc; it counts what a profiler calls draw calls | `BC-29` |
| `PassKind::Screen` / `DrawPacket::Kind::Screen` / `DrawKind::Screen` / `FrameHead::Kind::Screen`; `ScreenPass` | `Fullscreen`; `FullscreenPass` | "screen" is the window in this module, and the presenting pass is itself one of these - the kind fills its VIEW, wherever that lands | `BC-30` |
| `FrameOutcome` | `FrameCallOutcome` | two English near-synonyms for two subjects; this one is what one `frame()` CALL did (a `NotReady` means no frame happened) | `BC-31` |
| `Capabilities::Feature` / `supports` / `bit`; `DeviceClock::WindowId`; `FrameStats.hpp` | `Features::Feature` / `features.supports` / `Features::bit`; `FrameDriverId`; `FrameReport.hpp` | two forwarders the glossary had registered for removal, and a header named after a type that does not exist | `BC-32` <!-- drift-ok --> |

- What it is: names plus ONE meaning. `DrawPacket` is the per-view request (`graphics-frame-pipeline.md` 2.2 / 2.8) and
  `DrawItem` is still what it submits; `draws` now counts `DrawItem`s and `draw_packets` counts requests, so the bench log, the
  HUD and a profiler agree; `Fullscreen` describes the draw, not the target, so a pip-rectangle pass and a window pass are the
  same kind.
- Why: the previous batch had to write the distinction into the header ("ONE DRAWING CALL IS A REQUEST, NOT A SUBMISSION") and to
  keep two count fields apart by hand; the rename removes the sentence the name made necessary (`G7`).
- Kept on purpose: `drawScreenProgram` / `screenCopyProgram` / `kScreenFragmentSource` and the vsg plugin's screen-program layer
  name a **screen-space program** (a fragment stage run over a full-screen triangle), an industry term that does not mean the
  window; `Camera::screenToWorldRay` and the screen-space scaling helpers name coordinates. The backlog records the shader-side
  rename with its trigger (it changes a build input and the generated constant).
- Rejected: keeping the `Capabilities` / `WindowId` forwarders (the glossary had already scheduled them for removal, and the
  module promises no source or ABI stability); renaming `FrameResult` instead of `FrameCallOutcome` ("result" is the word a host
  arrives with, and the odd one out was the CALL's value); renaming `PassKind::Scene` to `Content` (a scene pass draws lines and
  impostors too, and `Content` would read as "anything the engine owns", the full-screen program included).

## 3. Features

### 3.1 Targets

| # | Feature | Notes |
|---|---|---|
| F1 | Colour attachments | `attachColor(format)`; may be several (MRT / G-buffer). |
| F2 | Depth attachment | `attachDepth(D16/D24/D32/D32F)`. |
| F3 | Shared depth | `setDepthPromotion(false)` + `shareDepth(DepthView)`; the lender draws first, the borrower LOADs. |
| F4 | Validity | At least one own attachment + a positive size; borrowed depth does not count as its own. |
| F5 | Readback | `readColorBuffer` / `readDepthBuffer`, both take one `ImageRef` (it names "which image") and return `ReadbackOutcome` (the answer is the return value; `result` carries `Ok` / `Unsupported` / `NotReady` / `Invalid` / `Failed`). The old `(target, index)` spelling and the old `bool` + `ReadbackResult*` shape are deprecated shims for one release (BC-16). |

### 3.2 MSAA

| # | Feature | Notes |
|---|---|---|
| F6 | Sample count | `RenderTarget::setSamples(n)`; 0 is treated as 1. |
| F7 | Colour resolve | The MS image is an attachment only; the resolve image is single-sampled, sampleable, copyable. |
| F8 | Depth resolve | The MS image is a depth attachment only; the resolve image is single-sampled, `sample0` is not averaged; a backend that cannot resolve depth refuses "multisampled + sampleable depth", and **borrowed depth + multisampling** is refused by every backend. |
| F9 | Picture definition | `picture()` / `depthPicture()`: readback, sampling, lending and promotion all go through it. |
| F10 | Limit | `Capabilities::limits.max_samples` is a **count** (a legal count is a power of two); a count the device cannot build is **refused** (`SamplesUnsupported`, reported once), never lowered. |

### 3.3 Image sources

| # | Feature | Notes |
|---|---|---|
| F11 | produced | `ImageRef` = target + attachment (+ depth); one producer per frame. |
| F12 | imported | `Texture` + revision + an optional `Sampler`; the upload key is `(Texture*, revision)`. |
| F13 | Narrow binding | `ProducedImage` binds only the declared attachment; `ProducedTarget` keeps the whole target. |
| F14 | Full-screen ABI | Source colour attachments occupy 0..C-1, depth follows, imported sources fill the remaining bindings in declaration order. |
| F15 | Sampler | min/mag, mipmap, address, border, anisotropy, lod bias, LOD range; cached by value. |

### 3.4 Colour

| # | Feature | Notes |
|---|---|---|
| F16 | `Colorf` | Linear-light float; clear / pass colour / default colours use it. |
| F17 | `Color` | sRGB-encoded bytes; `toLinear()` / `toColor()` are one pair of conversions. |
| F18 | Text and byte order | CSS hex read/write; `fromArgb` / `fromBgr` ordered construction. |
| F19 | Interpolation | `Colorf::lerp` interpolates in linear light. |

### 3.5 Shading and shadows

| # | Feature | Notes |
|---|---|---|
| F20 | `ShaderProgram` | Vertex / Fragment stages + entry points; `addStage` / `setStage` / `clearStages` / `replaceStages` bump the revision. |
| F21 | Built-in programs | forward / flat / gbuffer / deferred lighting / shadowed; `builtin_<role>.<stage>`. |
| F22 | ABI | Vertex locations, `VineViewBlock` / `VineDrawBlock` / `VineMaterialBlock` / `VineShadowBlock`, 128 B push. |
| F23 | Transparency | The only channel is `Node::opacity`; materials contribute no alpha; blend is always on. |
| F24 | Shadows | `Light::setCastShadow` + `ShadowSettings{resolution, bias}`; the light camera is derived by `directionalShadowMatrix` (orthographic) / `spotShadowMatrix` (perspective). **One map per chain, one light (the first buildable directional or spot light)**, point lights unsupported (see 2.7). |
| F26 | Line width | `StateNode::setLineWidth` / `RenderPass::setLineWidthOverride`: the width of **line primitives** (`Topology::Lines`), framebuffer pixels, dynamic state (`vkCmdSetLineWidth`) so no rebuild; `wideLines` is a required device feature, the limit is the device's `lineWidthRange` (`Capabilities::limits.max_line_width`), the floor 1.0. It does **not** widen `PolygonMode::Line` wireframe edges. |


## 4. Public Interface

### 4.1 Targets

| Interface | Purpose |
|---|---|
| `attachColor()` / `attachDepth()` | Attachments |
| `setSamples()` / `samples()` | Sample count |
| `setDepthPromotion()` / `shareDepth()` / `sharedDepth()` | Depth sharing |
| `hasColor()` / `hasDepth()` / `colorCount()` / `colorFormat()` / `depthFormat()` | Queries |
| `setSize()` / `width()` / `height()` | Size |

### 4.2 Image sources and sampling

| Interface | Purpose |
|---|---|
| `ImageInput(ImageRef)` / `ImageInput(Texture[, Sampler])` | Declaration |
| `ImageSource::producedImage()` / `producedTarget()` / `imported()` | Resolution |
| `Sampler` fields | filter / mipmap / address / border / anisotropy / lod |
| `RenderPass::addInput(Texture[, Sampler])` / `PassDesc::inputs` | Pass inputs |

### 4.3 Shading and shadows

| Interface | Purpose |
|---|---|
| `ShaderProgram::addStage()` / `setStage()` / `description()` / `revision()`, `ShaderProgramDescription::stages()` | Program |
| `ShaderStage{type, source, entryPoint}` | Stage |
| `attributeLocation()` / `Vine*Block` | ABI |
| `Light::setCastShadow()` / `isCastingShadow()` / `shadowSettings()` / `setShadowResolution()` / `setShadowBias()` | Light |
| `directionalShadowMatrix(light, box, ...)` | Light camera (directional, orthographic) |
| `spotShadowMatrix(light, box, ...)` | Light camera (spot, perspective, eye at the light position, cone angle = outer cone angle × 1.02) |
| `RenderTarget::shadowOf()` / `setShadowOf()` | Shadow-target declaration |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | The sample count is a target property; image / pass / pipeline are all driven by one `TargetShape`. |
| C2 | produced and imported image sources are not merged; their lifetimes differ. |
| C3 | Colour splits into linear-light `Colorf` and sRGB-encoded `Color`. |
| C4 | The engine core's pass description has no shadow field; "this image is a shadow" is declared by the target itself, consumers do not guess from "the first input with depth". |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | A shadow is an ordinary pass: the light declares it, the chain builds the pass, no engine built-in shadow subsystem. |
| R2 | A multisampled target's depth resolve takes `sample0`, no averaging. |
| R3 | Target formats gain no second colour semantics. |
| R4 | The shadow convention lives only in the built-in shaders: a pass that does not use the built-in programs has nothing to do with `VineShadowBlock`; sample the map as an ordinary produced image. |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | A multisampled target cannot **borrow** depth; the lender / promotion may be MS. |
| N2 | The depth resolve uses `sample0`; averaging would make "turning MSAA on" change depth values. |
| N3 | The default window / default framebuffer stays single-sampled; anti-aliasing draws into an MS offscreen target and then goes to the screen. |
| N4 | A produced input binds only the declared attachment in a content pass; do not assume the whole target. |
| N5 | `Sampler` is a cache key: its fields must be finite; NaN makes the cache miss every frame. |
| N6 | Materials contribute no alpha; transparency comes only from `Node::opacity`. |
| N7 | Changing one `ShaderAbi` block layout / location means changing C++ and GLSL together. |
| N8 | Shadow-casting lights are directional and spot (orthographic / perspective light cameras, the same 2D depth map, the same `VineShadowBlock`); **each chain has only one map** — a second casting light gets no map; a point light needs a six-face cube, and `setCastShadow` on a point light builds no pass. The three reasons for not building (second casting light / point light / this path produces no lighting) are reported separately by `ChainBuilder::reportRequestedShadows`, see 2.7 and backlog S6. CSM / PCSS are not done. |
| N9 | The dynamic state a pipeline declares must cover every item the commands write; an item written but not declared takes the create-info baked value (silently). |
| N10 | Dynamic state cannot change an enable: switches such as `depthBiasEnable` stay in the pipeline and must be fixed together with the state list, not left to the commands alone. |

## 6. Caveats and Traps

### 6.1 One fact, one home

- How it fails: the same state is kept in two places, change one and the other goes stale.
- How to detect it: two writable values / two writable registries appear.
- How to prevent it: go back to §5's N#; keep one owner, everything else read-only.

### 6.2 Backend/platform boundary

- How it fails: platform differences are written into the engine/application layer, and every platform change forks everywhere.
- How to detect it: an "if Windows/X11 then …" branch appears.
- How to prevent it: platform facts stay in the backend, the engine consumes only compiled frame values.

### 6.3 A shadow map's identity guessed

- How it fails: a consumer takes "the first input with sampleable depth" as the shadow map — the G-buffer has depth too, so the sun's shadow reads the G-buffer's depth and
  only half the picture is right (this is the defect the reference backend measured).
- How to detect it: the image fed to the shader and the one declared by `CompiledPass::shadow` are not the same; or the map's owner is not in this call's light list.
- How to prevent it: identity comes only from the target (`setShadowOf` + `setProducerViewProjection`), the plan copies and never derives; `ShadowFacts` is the only source.

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Hardware compare sampler | This release's pass depth path reads exact depth; a compare sampler needs its own ABI. |
| D2 | Array / 3D textures | `Texture::Kind` has only D2 / Cube; layered targets are a trigger-gated design. |
| D3 | A multisampled target borrowing depth | What is borrowed is the single-sampled resolve image, and one framebuffer attachment shares one sample count. See [ADR](../decisions/ADR-2026-10-22-msaa-target-property.md). |
| D4 | sRGB variants of target formats | Colour space is already solved in the type layer; targets gain no second semantics. See [ADR](../decisions/ADR-2026-10-21-colour-space-in-types.md). |
| D5 | An engine-automatic shadow subsystem, or a shadow field / `PassStage::Shadow` on `PassDesc` | A shadow is the composition of one or more ordinary passes; the engine only provides the generic `Depth` slot, the image identity and the resolution facts. A field would copy "who casts, how it is read" into the pass model (see §2.7). |
| D6 | Point/spot shadows / CSM / PCSS | No consumer; point/spot `setCastShadow` builds no pass today and is reported (backlog S6 waits for a real need). |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `RenderTargetTest` / `MsaaTargetTest` / `SdkPixelTest` / `VsgBackendTest`. |
| T2 | A 1-sample triangle edge has 0 blended pixels; 4 samples > 0; the interior is still a solid colour. |
| T3 | A 4-sample depth is readable and has no intermediate values; after resolve it matches the 1-sample one point by point. |
| T4 | `FullscreenDrawTest` / `ImageSourceTest` / `SamplerTest` / `ColorTest` / `ColorfTest` / `ShaderAbiTest`. |
| T5 | Shadows: the ground darkens with a casting light; without one the evidence is byte-identical. |
| T6 | The dynamic-state list: every item the command's `record()` writes is declared in the pipeline (the current gap is `../bugs/vsg-depth-bias-not-declared-dynamic.md`). |

## 9. Status and Evolution

### 9.1 Current status

Current design; the feature points and interfaces are the tables above.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items stay by trigger condition.

### 9.3 Compatibility / migration

Old names/paths appear only in §7's rejected options or in git history; new code follows §4's interfaces.

## 10. References

`graphics-architecture.md` · `graphics-scene.md` · `graphics-geometry.md` · `vsg-backend.md` ·
`../backlog/graphics.md`
