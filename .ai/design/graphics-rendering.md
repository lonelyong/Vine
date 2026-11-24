# graphics rendering: targets, MSAA, image sources, colour, shading, shadows

> **Status**: current
> **Last verified**: 2026-11-23
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
2.2 **The sample count is a target property**: `setSamples` → the target's `RenderPassCompatibility` → pipeline key → pass and image.
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

2.8 **Dynamic state is a single list, NEXT TO ITS WRITER**: the dynamic state the pipeline **declares** (`DynamicState`) and the dynamic state the commands **write** (the `vkCmdSet*` calls in `recordRasterState`) must be two faces of the same list, pinned by one test "every item the commands write is declared in the pipeline, and vice versa". Miss one declaration and the driver takes the create-info
baked value — the half the commands wrote is silently dropped, and the picture is wrong without an error. This also governs the **switches** in create-info: dynamic state can only change values, not
enables such as `depthBiasEnable`, so an enable must be fixed together with the list.
There is **one** list, declared where the pipeline is built (`ContentCaches`'s `buildPipeline`) and issued by one recorder helper (`ContentDraw::recordRasterState`): the floor requires `VK_EXT_extended_dynamic_state3`, so every item a pipeline key used to bake is a command today (`T54`).
The witness that the rule has teeth is `../bugs/vsg-depth-bias-not-declared-dynamic.md`: the content pipeline carried a second copy of the list without `DEPTH_BIAS` and without the enable, so `setPolygonOffset` was a silent no-op until 2026-11-21.

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
| `screenCopyProgram` / `ScreenCopyChannels` / `drawScreenProgram` / `buildScreenProgramFacts` / `ContentPipeline::createScreen`; `builtin_screen_copy.frag` | `fullscreenCopyProgram` / `FullscreenCopyChannels` / `drawFullscreenProgram` / `buildFullscreenProgramFacts` / `ContentPipeline::createFullscreen`; `builtin_fullscreen_copy.frag` | the PROGRAM path kept the word the DRAW path had retired in `BC-30`; the file rename regenerates the embedded constant (`kBuiltinFullscreenCopyFrag`) | `BC-33` |

- What it is: names plus ONE meaning. `DrawPacket` is the per-view request (`graphics-frame-pipeline.md` 2.2 / 2.8) and
  `DrawItem` is still what it submits; `draws` now counts `DrawItem`s and `draw_packets` counts requests, so the bench log, the
  HUD and a profiler agree; `Fullscreen` describes the draw, not the target, so a pip-rectangle pass and a window pass are the
  same kind.
- Why: the previous batch had to write the distinction into the header ("ONE DRAWING CALL IS A REQUEST, NOT A SUBMISSION") and to
  keep two count fields apart by hand; the rename removes the sentence the name made necessary (`G7`).
- Landed later, as `T49` (2026-11-22): the PROGRAM path says `Fullscreen` too - `fullscreenCopyProgram`,
  `FullscreenCopyChannels`, `drawFullscreenProgram`, `buildFullscreenProgramFacts`, `ContentPipeline::createFullscreen`, the
  shader file `builtin_fullscreen_copy.frag` and its generated `kBuiltinFullscreenCopyFrag` (`BC-33`). `screen` now names only
  the WINDOW or screen-space COORDINATES (`Camera::screenToWorldRay`, `fitToScreen`, `RotateToScreen` / `autoScaleToScreen`,
  post-process effects), which is what the glossary's `screen` row states. What still says "screen" for a full-screen program
  is backend-private: the vk plugin's pipeline/set plumbing plus its `vk.content.screen_*` diagnostic codes - a diagnostic
  code is identity, so that half is a batch of its own (`T57` in the backlog).
- Rejected: keeping the `Capabilities` / `WindowId` forwarders (the glossary had already scheduled them for removal, and the
  module promises no source or ABI stability); renaming `FrameResult` instead of `FrameCallOutcome` ("result" is the word a host
  arrives with, and the odd one out was the CALL's value); renaming `PassKind::Scene` to `Content` (a scene pass draws lines and
  impostors too, and `Content` would read as "anything the engine owns", the full-screen program included).

**The 2026-11-22 completion: one kind, three levels** (`BC-33`;
[`ADR-2026-11-20`](../decisions/ADR-2026-11-20-draw-packet-and-fullscreen-vocabulary.md), amendment). The batch above renamed the
`Screen` enumerator in four types and kept the four types; the fifth review round named that mixture as the defect it was, so the
TYPE count went down while the values stayed `Scene` / `Fullscreen`.

| Before | Now | What the old spelling got wrong | Note |
|---|---|---|---|
| `PassKind` / `DrawPacket::Kind` / vk `FrameHead::Kind` / vsg `bool fullscreen`; `DrawKind{Content,Fullscreen}` | `DrawKind{Scene,Fullscreen}` in `DrawKind.hpp`, taken by `PassDesc::kind` / `DrawPacket::kind` / `PipelineKey::kind` and by both backends' frame heads | four types for one fact; the head's `bool` could not be compared with the key's enumeration, so "the plan's kind and the pipeline's kind agree" was a rule instead of a property - and the two ABIs bind different descriptor sets, so a mismatch binds a pipeline for the wrong layout | `BC-33` <!-- drift-ok --> |

- What it is: a type deletion, not a rename. `PassDesc::kind`, `DrawPacket::kind` and `PipelineKey::kind` are one enumeration, so
  a pipeline cannot be compiled for one kind and bound for the other.
- Kept on purpose: the VALUE `Scene` (a scene pass draws lines, impostors, gizmos and overlays; `Content` would read as
  "anything the engine owns"), and the reopen trigger - a third execution shape redesigns the SET as a whole.
- Rejected: `PassKind` kept as an alias of `DrawKind` (the module promises no source stability, every in-tree consumer moved in
  the same commit, and an alias leaves two names in every search); a `std::variant` of two packet types (the mismatch it would
  prevent is answered by one predicate, `packetDraws`, and the shape would pierce `OwnedFrame` and both recorders).

**The 2026-11-23 batch: a policy is not a state, and a build is not a scene** (`BC-34` / `BC-35`; backlog `T53`). Both names
described a NEIGHBOUR rather than their own value; the change is vocabulary only - every enumerator, member and behaviour is
unchanged.

| Before | Now | What the old name got wrong | Note |
|---|---|---|---|
| `DepthMode` / `PassDesc::depth_mode` / `RenderPass::depthMode()`; `DepthMode.hpp` | `DepthPolicy` / `PassDesc::depth_policy` / `RenderPass::depthPolicy()`; `DepthPolicy.hpp` | a POLICY, not a state: it says what a pass decides for content that authored no depth state, and the old name sat beside the real `DepthState` (a draw's own test/write/compare value) as a false friend - the header itself called it a policy | `BC-34` <!-- drift-ok --> |
| `ModelScene` / `ModelScene.hpp` / `ModelScene.cpp` / `ModelSceneTest` | `SceneBuild` / `SceneBuild.hpp` / `SceneBuild.cpp` / `SceneBuildTest` | the RESULT of `sceneFromModel()`: a node tree plus the conversion's counts, which the caller attaches to a scene of its choosing - the old name read as "the model's scene" | `BC-35` <!-- drift-ok --> |

- What it is: names only. The three `DepthPolicy` values keep their meaning, `sceneFromModel()` keeps its name, and
  `SceneBuild` keeps its four members (`root`, `geometries`, `skipped_meshes`, `skipped_nodes`).
- Why: the trigger was "the next batch that touches either type for another reason" (a word sweep alone has no witness,
  `G8`); it was taken on an explicit go because a reader hits `DepthMode` beside `DepthState` and `ModelScene` meaning
  "not a scene" and has to re-learn both - the mixture the names create is the witness.
- Rejected: keeping `depth_mode` as a field and renaming only the type (two spellings for one concept); renaming
  `sceneFromModel` too (it is a verb phrase and reads correctly); the meshio exporter's `ModelSceneWriter` (a different
  concept - it writes an Assimp scene and never touches this type).

**The same day, T15 closed: the layout vocabulary left the SDK** (`BC-36`; review 8 §4.7/§4.8). `DrawState::ImageLayout`
and `DrawState::LoadOpVariantKey` are DELETED from `DrawState.hpp`; the SDK keeps `LoadOp` / `StoreOp`, which every
family answers.

| Before | Now | Why |
|---|---|---|
| `vn::graphics::ImageLayout` | `vn::backend_vk::ImageLayout` (`gfx_backend_vk/internal/ImageLayout.hpp`); the retired backend had its own copy | a layout is one family's spelling of "this image is being written as colour" - D3D12 states a resource state, Metal tracks hazards itself - so it belongs beside the table that consumes it, not on the seam |
| `vn::graphics::LoadOpVariantKey` | the backend that builds pass objects keys its own (vk builds variants by hand) | the key of a render-pass OBJECT; a D3D12 backend keys PSOs, Metal has no pass object at all |

- What a host must change: nothing - neither type was ever a parameter of a host call (the load/store DECISION reaches a
  backend through `PassDesc`'s `ClearPolicy`).
- What a backend must do: include its own header; `keyAuditTable()` now has six rows, because the variant key cannot
  enter `PipelineKey` when it is not an SDK type (the seam test states that as the stronger fact).
- The rule that decided it is in `graphics-architecture.md` 2.15: a fact only one API family can answer stays off the
  public surface, and the other half of the rule is that a promise a host can observe keeps ONE implementation.

**2026-11-23, `T54`: vk's floor rose, and the pipeline key lost its state half** (`BC-37`; `ADR-2026-11-23` D4).
`gfx_backend_vk` now REQUIRES `VK_EXT_extended_dynamic_state` / `_state2` / `_state3` (polygon mode and colour blend
included) and refuses `initialize` with `vk.init.dynamic_state_missing` without them; its private
`ContentPipelineKey` is the seam's `PipelineKey` alone, and cull / polygon / depth test / write / compare / blend are
commands issued by `recordRasterState`.

- What a host must change: nothing on a driver that offers the three extensions. On a Vulkan 1.2 device without them
  the backend now answers a diagnostic at `initialize` instead of drawing - the alternative was a silent wrong
  picture, because there is no second (baking) path any more.
- What a backend author must know: identity holds the PROGRAM and the pass SHAPE, never a state a `vkCmdSet*` can move.
  The rule now has no exception, because `RasterState` was the only one.

**The same day, step 1b: the API floor is 1.3 and barriers are synchronisation 2** (`BC-38`; `ADR-2026-11-23` D4).
`gfx_backend_vk` requires a Vulkan 1.3 device with the `synchronization2` and `dynamicRendering` features enabled, and
`BarrierPlan` emits `VkImageMemoryBarrier2` recorded by the one `recordBarrier` (`vkCmdPipelineBarrier2`). The
Vulkan-1.0 barrier form is not kept.

- What a host must change: nothing on a 1.3 driver - a device below it answers `vk.init.api_version_low`, and a 1.3
device without the two features answers `vk.init.device_features_missing`.
- What a backend author must know: the stage masks belong to the table rows, so a call site cannot pair a layout with
another row's stages; `recordBarrier` is the single place the sync2 command is named.
- Dynamic rendering is the next step of the same batch (`T11`); until it lands, pass objects are still `VkRenderPass`,
  which the 1.3 floor permits.

**The same day, step 2: vk draws through dynamic rendering** (`BC-39`; `ADR-2026-11-23` D4, backlog `T11`).
`gfx_backend_vk` has no `VkRenderPass` and no `VkFramebuffer` any more. A pass opens a dynamic-rendering instance
(`vkCmdBeginRendering`) over its attachment set; the load ops, the clear values and the MSAA resolves (colour
`AVERAGE`, depth `SAMPLE_ZERO`) are fields of the per-attachment `VkRenderingAttachmentInfo`, and a pipeline is built
with `VkPipelineRenderingCreateInfo` + `renderPass = VK_NULL_HANDLE` from the attachment FORMATS of its compatibility.

- What a host must change: nothing. The SDK never named a render pass; `RenderPassKey` / `RenderPassCompatibility`
  keep their names and meaning - they are the pipeline's shape identity, not a Vulkan handle.
- What a backend author must know: dynamic rendering inserts NO dependency and states NO layout. Every pass records its
  own attachment transitions (one barrier per attachment, from the layout its owner tracks to the rendering layout),
  and a barrier between two identical states is deliberate - it is the execution dependency a render pass used to
  provide. The trap, its VUID and the mutation that proves the transitions load-bearing are `vk-backend.md` `P15`.
- What it bought: one load/store pair per attachment instead of a render-pass variant, so the HUD case (keep the
  picture, clear only the depth inside its rectangle), the "clear one attachment, load another, discard a third" case
  (2026-11-24: `ClearPolicy::color_ops` / `depth_op`, authored through `PassDesc`) and the MSAA resolve pair are values
  rather than structures - and the last legacy creation call in the backend is gone. The backend resolves the one fact
  the engine cannot know: a stated `Load` on a set with no picture becomes `Clear` (`decidePassLoad`).

2.11 **The pipeline identity is one value, and a backend adds only what its device cannot move** (`G1` / `G6`;
[`ADR-2026-11-21`](../decisions/ADR-2026-11-21-state-identity-and-vertex-format.md)):

- `vn::graphics::PipelineKey` is the PORTABLE identity both backends key a compiled pipeline by: draw kind, program
  identity + revision, the vertex layout, render-pass compatibility, the sampled depth/colour counts, the program
  variant and the topology class. Its `vertex_layout` is **one slot per declared attribute, in binding order, with
  the slot's component count** - a locations-only key shared one compiled pipeline between a 2-component and a
  4-component custom channel, i.e. between two vertex FORMATS.
- `DynamicState` is the maximum the ENGINE can move off identity (`graphics-rendering.md` 2.8 lists how it is
  delivered). `DrawState.hpp` states the rule the two layers follow: a backend must move every item its device CAN
  off identity and must say which ones it cannot.
- **vk (the host's default)** moves every one of them too: its key is `ContentPipelineKey` and nothing else, and the
  commands live in one recorder helper (`recordRasterState`). It got there on 2026-11-23 (`T54`): the floor rose to
  `VK_EXT_extended_dynamic_state`/`_state3`, which is what makes a separate raster half disappear.
- The witness that "one list" has teeth is `../bugs/vsg-depth-bias-not-declared-dynamic.md`: a second copy of the
  list without `DEPTH_BIAS`, plus a pipeline without the `depthBiasEnable` switch, made `setPolygonOffset` a silent
  no-op (fixed 2026-11-21).
- The price of `T54`, stated: no in-tree backend exercises a Vulkan 1.2-class device any more, so the seam is proven on
  devices that can move these states. The next cross-family reader (D3D12) replaces that reading, and a 1.2 device
  without the extensions is refused rather than served a different picture (`BC-37`).

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
| F23 | Transparency | TWO INPUTS, ONE RULE: the magnitude is the drawable's `opacity` × the material's own alpha × the base map's alpha, and the material's `AlphaMode` says what that product means (Opaque ignores it, Mask thresholds it, Blend uses it as the output alpha) - sorting and shading read the same pair (`graphics-material.md` §2.7). Blending is not a pipeline constant: `blendEnable` is baked false and switched per draw through `VK_EXT_extended_dynamic_state3` (`StateCommands`), and a draw whose resolved `BlendState` is enabled - which is what an effective opacity below 1 resolves to - is the one that turns it on. |
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
| C1 | The sample count is a target property; image / pass / pipeline are all driven by one `RenderPassCompatibility` (the value a target's `shape()` answers). |
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
| N6 | The material's alpha mode and the drawable's opacity are the two halves of one rule: the mode says how alpha is USED, the opacity is its magnitude - never substitute one for the other (`graphics-material.md` N5), and do not read the mode as "the material's own alpha": Opaque ignores the material's alphas, Mask thresholds them, Blend multiplies them into the output. |
| N7 | Changing one `ShaderAbi` block layout / location means changing C++ and GLSL together. |
| N8 | Shadow-casting lights are directional and spot (orthographic / perspective light cameras, the same 2D depth map, the same `VineShadowBlock`); **each chain has only one map** — a second casting light gets no map; a point light needs a six-face cube, and `setCastShadow` on a point light builds no pass. The three reasons for not building (second casting light / point light / this path produces no lighting) are reported separately by `ChainBuilder::reportRequestedShadows`, see 2.7 and backlog S6. CSM / PCSS are not done. |
| N9 | The dynamic state a pipeline declares must cover every item the commands write; an item written but not declared takes the create-info baked value (silently). |
| N10 | Dynamic state cannot change an enable: switches such as `depthBiasEnable` stay in the pipeline and must be fixed together with the state list, not left to the commands alone. |
| N11 | Alpha-to-coverage is not stated: a Mask fragment on a multisampled target keeps binary edges. On this backend it is ONE switch (`vkCmdSetAlphaToCoverageEnableEXT`, dynamic under the EDS3 floor), so the obstacle is not the backend - coverage is derived from the fragment's first colour output, which is the material's fact (`graphics-material.md` N6, backlog `T58`). |

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
- How to detect it: the image fed to the shader is not the target whose `shadowOf()` says it is a map; or the map's owner is not in this call's light list.
- How to prevent it: identity comes only from the target (`setShadowOf` + `setProducerViewProjection`); nothing derives it from input order.

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
| T1 | `RenderTargetTest` / `VkMsaaTest` / `VkContentDrawTest` / `VkContentTargetTest`. |
| T2 | A 1-sample triangle edge has 0 blended pixels; 4 samples > 0; the interior is still a solid colour. |
| T3 | A 4-sample depth is readable and has no intermediate values; after resolve it matches the 1-sample one point by point. |
| T4 | `VkContentDrawTest.AFullscreenCopySamplesTheSourceTarget` / `AFullscreenCopyIsDrawnWhateverThePassDepthPolicySays` / `ABaseColorTextureIsUploadedAndSampled` / `ACubeMapIsUploadedWithEveryFaceAndSampled` / `ColorTest` / `ColorfTest` / `ShaderAbiTest`. |
| T5 | Shadows: the ground darkens with a casting light; without one the evidence is byte-identical. |
| T6 | The dynamic-state list: every item the command's `record()` writes is declared in the pipeline (witness: `../bugs/vsg-depth-bias-not-declared-dynamic.md`). |

## 9. Status and Evolution

### 9.1 Current status

Current design; the feature points and interfaces are the tables above.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items stay by trigger condition.

### 9.3 Compatibility / migration

Old names/paths appear only in §7's rejected options or in git history; new code follows §4's interfaces.

## 10. References

`graphics-architecture.md` · `graphics-scene.md` · `graphics-geometry.md` · `vk-backend.md` ·
`../backlog/graphics.md`
