# graphics rendering: targets, MSAA, image sources, colour, shading, shadows, background

> **Status**: current
> **Last verified**: 2026-11-27
> **Scope**: targets, MSAA, image sources, colour space, shading, shadows and the background.
> **Sources**: `RenderTarget.hpp` / `ImageInput.hpp` / `Sampler.hpp` / `ShaderProgram.hpp` /
> `ShaderAbi.hpp` / `Light.hpp` / `BuiltinShaders.hpp` / `PassChain.hpp`.
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
2.3 **Two image-source lifetimes, ONE declaration list**: produced (`ImageRef`) and imported (`Texture`) are not merged; `ImageSource` resolves the shape uniformly, and `PassDesc::inputs` is the one ordered list every reader of a pass walks (2.13).
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
  - **The fragment stage still CUTS A SILHOUETTE** (`G1` `G5`; review 18 `Y7`, fixed 2026-11-27): it applies the material's alpha rule (`vine_alpha.glsl`, the module the forward and G-buffer stages call) before writing no colour, so a `Mask` material's cut-out leaves the same holes in the map that it leaves in the picture - the shadow of a leaf is a leaf. A depth texel holds one DISTANCE, so this is the only alpha rule a depth pass can honour: `Blend` and a faded drawable cast nothing (see the next bullet), which is a policy rather than a texel. Cost: one base-map fetch per depth texel for a textured `Mask` caster, and nothing for an `Opaque` one (the mode is read before the fetch).
  - **What casts is the sort's own answer** (`PassDesc::selection = PassSelection::Opaque`, `G1` `G4` `G10`): a depth pass records only the commands `DrawItem::isTransparent()` leaves in the opaque queue, so the queue a drawable lands in, the blend the backend enables for it and whether a depth map records it are three readings of one value. `Mask` is opaque here (its cut-out is a shader threshold, handled by the bullet above), while `Blend` and an `Opaque` material at opacity < 1 are not: one distance cannot describe partial coverage, and a map written for such a drawable would shadow everything its colour only tints. The pass STATES it and the backend applies it per command, so the collected list stays the shared, immutable one every pass of the frame draws (`G10`).

2.8 **Dynamic state is a single list, NEXT TO ITS WRITER**: the dynamic state the pipeline **declares** (`DynamicState`) and the dynamic state the commands **write** (the `vkCmdSet*` calls in `recordRasterState`) must be two faces of the same list, pinned by one test "every item the commands write is declared in the pipeline, and vice versa". Miss one declaration and the driver takes the create-info
baked value — the half the commands wrote is silently dropped, and the picture is wrong without an error. This also governs the **switches** in create-info: dynamic state can only change values, not
enables such as `depthBiasEnable`, so an enable must be fixed together with the list.
There is **one** list, declared where the pipeline is built (`ContentCaches`'s `buildPipeline`) and issued by one recorder helper (`ContentDraw::recordRasterState`): the floor requires `VK_EXT_extended_dynamic_state3`, so every item a pipeline key used to bake is a command today (`T54`).
The witness that the rule has teeth is `../bugs/vsg-depth-bias-not-declared-dynamic.md`: the content pipeline carried a second copy of the list without `DEPTH_BIAS` and without the enable, so `setPolygonOffset` was a silent no-op until 2026-11-21.

2.9 **Capabilities are two questions, readback has one "which image" and one answer**: `Capabilities` consists of two values, `Features` (is it supported) and `Limits` (how large is the maximum);
the limit fields are `Capabilities::limits.*` (source-level, BC-11), and the feature questions are `Features::supports` / `Features::bit` with `Features::Feature` as the enum.
The transitive bit-set spelling on `Capabilities` was deleted once its migration was done (BC-32): one question has one spelling. Both readback verbs `readColorImage` / `readDepthImage` take one `ImageRef` - the same vocabulary as a pass input's "which image" (BC-16); `readColorImage(target, attachment = 0)` and `readDepthImage(target)` are short spellings of the same verbs, not a second readback path.

**The answer is one value** (`BC-41`, 2026-11-25): a verb returns `ColorReadback` / `DepthReadback` - `ReadbackResult<T>`, the classification (`ReadbackStatus`: `Ok` / `Unsupported` / `NotReady` / `Invalid` / `Failed`) together with the picture it classifies, `values` empty on every refusal. Nothing is an out-parameter any more: the `bool` + `std::vector&` + `ReadbackStatus*` shapes and the payload-less `ReadbackOutcome` wrapper are deleted, not deprecated (the batch above had kept them "for one release"; nothing shipped with them, and a second spelling would have survived in every caller and every guide table - `G1`).
Why: `G3` (ask, configure, answer readable in one pass), `G4` (a refused read cannot leave a vector a caller reads as this frame's picture), `G6` (`PlanResult` and `FrameCallOutcome` are the same shape: an answer carries its status and its product).
The naming rule it completes: a verb with NO product returns a `*Result` code (`SurfaceTargetResult`, `BackendRegistrationResult`); a verb that HAS a product returns a `*Result` carrying its `Status` (`PlanResult`, `FrameResult`, `ReadbackResult<T>`) - which is why the code itself is `ReadbackStatus`.

**The names state the image, and the classification has one home** (`BC-42`, 2026-11-25): the verbs are `readColorImage` / `readDepthImage`, because a target holds IMAGES (attachments) and a buffer is a different GPU resource - a verb named `readColorBuffer` names the wrong object (`G12`; the helper that builds the image for the short spelling is `makeReadbackImage`).
The classification is `ReadbackStatus` and nothing else. The seam's second vocabulary (`readbackOf` / `ReadbackState` / `ReadbackRequest` / `ReadbackRefusal` / `readbackStatusOf` / `detail::refusalName` / `ReadbackKind`) is DELETED, not deprecated: it described a copy-back-node model neither backend has, its only consumer was `BackendSeamTest`, and the repo already demotes test-only exports (audit-4 `B5`) - keeping it would be two answers to one question (`T70`, `G1`, `G8`). Which answer wins is stated ONCE, on `ReadbackStatus`: a request that can never be served answers permanently first (`Invalid` - it names nothing; then `Unsupported` - this device has no packing for it), and a target this device has not built yet answers `NotReady` before the request is judged, because until a frame announces it there is no shape to judge against.
The two tables answer one fact each: `colorBytesPerTexel` / `depthBytesPerTexel` return `std::optional<std::uint32_t>`, and `std::nullopt` IS "no packing here". The deleted `ReadbackFormat{bytes, readable}` pair could state a width and a refusal that disagreed, and one field was a copy of the other.
A depth readback describes the image the device BUILT (the record's `TargetShape::depth_format`), never `RenderTarget::depthFormat()`: that is the host's current declaration, and a host that changes it before the next frame announces would otherwise size the copy from a format the image does not have (`VkContentTargetTest.ADepthReadbackAnswersAboutTheImageThatExistsNotALaterDeclaration`).
The device also answers its capabilities before it waits: a device with no host-visible coherent memory type refuses `Unsupported` without stopping the device (`MemoryAllocator::supportsHostVisible`), which is the rule a refusal owes a caller.

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
- Rejected: keeping the old spellings as deprecated aliases (the shape B4 first used for the readback answer, and BC-41 later deleted). B4 migrated a *shape*
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
  post-process effects), which is what the glossary's `screen` row states. The backend half landed as `T57`
  (2026-11-25): the vk plugin's pipeline/set/planner plumbing says `Fullscreen` too (`kFullscreenSamplerBindings`,
  `ContentDraw::FullscreenSource`, `ContentCaches::ensureFullscreenPipeline` / `planFullscreen`,
  `FrameScratch::fullscreenSetFor`). The four `vk.content.screen_*` diagnostic codes KEEP their spelling - a code is
  identity a host matches, so the internal name moved and the string did not (`BC-40`).
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
  backend through `PassDesc`'s `AttachmentPolicy`).
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
  (2026-11-24: `AttachmentPolicy::color_ops` / `depth_op`, authored through `PassDesc`) and the MSAA resolve pair are values
  rather than structures - and the last legacy creation call in the backend is gone. The backend resolves the one fact
  the engine cannot know: a stated `Load` on a set with no picture becomes `Clear` (`resolveAttachmentOps`).

2.11 **The pipeline identity is one value, and a backend adds only what its device cannot move** (`G1` / `G6`;
[`ADR-2026-11-21`](../decisions/ADR-2026-11-21-state-identity-and-vertex-format.md)):

- `vn::graphics::PipelineKey` is the PORTABLE identity a backend keys a compiled pipeline by: draw kind, program
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

2.12 **A colour format is `channels x family`, and an 8-bit readback is a byte copy of it** (`G1` / `G4` / `G6` / `G9`;
backlog `T59`): `RenderTarget::ColorFormat` names nine values - 1, 2 or 4 channels of three numeric families (8-bit
UNORM, 16-bit float, 32-bit float) - because a post-process product is not always a four-channel colour: a coverage or
SSAO mask is one channel, a luminance or a depth-like slice is `R16F` / `R32F`, a velocity buffer or an octahedral
normal is two channels.

- The vocabulary is a **CROSS PRODUCT**, not a list of today's targets, so no value is an arbitrary exception. What is
  deliberately absent says as much as what is present: no three-channel value (`D7`), no integer family (`D8`), no
  packed 10/11-bit (`D9`), no sRGB spelling (`D4`).
- The families are two CONTRACTS, not two precisions:
  - **8-bit** is one byte per channel, so a texel is 1, 2 or 4 bytes and `colorBytesPerTexel` states exactly that width
    (`C5`). A mask reads back like a colour does, because it is the same rule - which is what lets a host verify a
    post-process product rather than only draw it.
  - **float** renders and samples (an HDR product, a depth-like slice) but has no readback packing: `colorBytesPerTexel`
    answers `std::nullopt` and a read answers `Unsupported`, because the verb copies texels and converts nothing, and a
    byte count alone does not describe a half or a full float to a caller decoding bytes (`D10`).
- **The mapping is the rule**: `R8` is `R8_UNORM`, `RG16F` is `R16G16_SFLOAT`. A value added to the enum is added to
  the backend's "every value" list (`kColorFormats`) and to `colorBytesPerTexel`; the two switches report an omission.
- **A format is a target only when the device backs every use a target has** (`C6`): drawn into, sampled by a later
  pass and copied by a readback. That is the DEVICE's answer (`vkGetPhysicalDeviceFormatProperties`, asked once at
  session open), and a target asking for a format the device does not back is refused BY NAME
  (`vk.target.color_format_unsupported`) instead of being attempted and failing inside image or pipeline creation - the
  sample-count rule of [`ADR-2026-11-14`](../decisions/ADR-2026-11-14-sample-count-refused-not-lowered.md) applied to a
  format.
- A shader still writes `vec4` whatever the attachment keeps, so an effect pass needs no second fragment-output
  contract; the price of that convenience is `P4`.

2.13 **One list of declared inputs per pass** (`T56`, 2026-11-25; `G1` / `G4` / `G6` / `G11`): what a pass reads is
`PassDesc::inputs`, one ordered `ImageInput` list, and each entry names ONE identity - `byImage` (one attachment a
producer binds), `byTarget` (a whole target), `byName` (a late-bound published name) or `byTexture` (an imported
texture). Resolution, scheduling edges, the wiring checks, the frame table and `Plan::describe` read that same list in
that same order, so a wire is resolved once and reported once.

- Why the identities stay four: each is the identity SOME author holds. A transient target is created by the engine
  while it compiles (the author cannot hold it), a host publishes a target under a name, and an imported texture has
  no producer at all - folding any of them into "a pointer" would delete the only spelling that case has (`G9`).
- Why one list and not "objects win, names are ignored": the removed rule let a declaration be silently discarded, so
  the same wire could be resolved from one field and validated, ordered and reported from another - the built-in chain
  stated the G-buffer twice to compensate, and a shadow map stated by object never appeared in the plan at all. One
  list makes "stated twice" impossible instead of documented (`G7`).
- Why named factories and no implicit conversions: the industry's version of this value (Unity's
  `RenderTargetIdentifier`) accepts its sources through implicit conversions, which is why "which identity did this
  become" is a known debugging trap; `byImage` / `byTarget` / `byName` / `byTexture` say it at the call site (`G4`).
  What the entry costs is length, not ambiguity.
- The declaration is a VALUE (owning handles, private fields, `kind()`); the resolved form is `ImageSource` (three
  kinds, borrowed). A name is resolved where it is used and does not survive into the frame: the backend answers "what
  did I get", never "how was it found".
- Boundary: WHICH identity each rule can judge is a property of the identity, not a convenience - `validatePlan` can
  only judge a name (a plan has no frame), and the compile judges an object (whether a target was produced is a fact
  of the frame). Neither rule is judged twice (`G5`), and both read the same list (`N13`).
- Cost, stated: `ImageInput` is a fat value (one of four handles per entry); a `std::variant` would be smaller, but it
  would stop mirroring `ImageSource`'s tagged shape for a few dozen bytes per declaration (`G8`, `D13`).
- The rejected shapes are `D11`-`D16`; the decision record is
  [`ADR-2026-11-25`](../decisions/ADR-2026-11-25-one-input-list-per-pass.md).

2.14 **Linear light becomes an encoded pixel at the swap-chain write, by the target's format** (`G1` / `G4` / `G9`;
review 17 `W6`): every shading stage produces LINEAR values (the material block, the BRDF and the emissive term are all
linear and may exceed 1), and exactly one step in the module encodes them - the store into a presenting target, whose
`ColorFormat` is an sRGB one and whose hardware applies the transfer. No shader and no pass encodes, and there is no
tone curve: a lit value above 1 clips there.

- The transfer belongs to the FORMAT because a format is what a texel MEANS: two targets with different formats hold
  different kinds of value, and a pass that wrote an sRGB target would be writing encoded numbers into a type whose
  arithmetic (a blend, a filter, a downsample) assumes linear light.
- An offscreen 8-bit format is therefore a DATA format (a mask, a weight, an occlusion factor, the exact bytes a test
  compares); a lit picture that a human will look at, and that must survive an offscreen hop, is `RGBA16F` until the
  window write - which is what the Deferred preset's G-buffer already does. The dark half of the 8-bit range is where a
  linear value loses precision (linear 0.5 is about 188 encoded), so an 8-bit lit intermediate bands in shadow.
- A colour readback answers the STORED bytes; the difference between it and the window is exactly this transfer, which is
  why a pixel test on an offscreen 8-bit target is a test of the data and not of the picture a user sees.
- Tone mapping and exposure are NOT part of the engine: a host that wants a look names a full-screen program
  (`fullscreenCopyProgram`'s note says so) and the pass draws it. `D4` already rejected sRGB variants of target
  formats; encoding inside a copy program and a tonemap stage are rejected with their reasons in the decision record,
  [`ADR-2026-11-26`](../decisions/ADR-2026-11-26-linear-light-is-encoded-by-the-target-format.md).

2.15 **A background is not a surface, and its four statements live in ONE pair** (`G1` / `G4` / `G7` / `G12`; review 18
`Y9`):

A picture needs an answer for "what does a ray that reached nothing see", and that answer is not geometry: a sky is not
lit, not shadowed, not measured and not occluded. The SDK states it as a pair - `skyboxProgram()` for the text and
`skyboxRenderState()` for the state - and a chain declares it (`ChainDesc::background`), so no host has to remember the
invariants between the two halves.

- **The camera decides the picture, not the box.** The vertex stage removes the camera's translation from
  `view * model`, so the unit box is centred on the eye wherever the eye is: its radius encloses nothing, a camera that
  flies out of the authored box still stands inside the sky, and no per-frame work positions it. What remains is the
  window's own scale (`1 / projection[0][0]`, `1 / projection[1][1]`), which is a no-op for a perspective projection
  (coverage there is angular) and exactly the lateral coverage an orthographic one needs.
- **The depth is the far plane, not the geometry's.** The stage writes `kFarClipZ` (`ShaderAbi.hpp`) as clip z and keeps
  the fragment's `w`, so a sky fragment holds exactly the depth an empty pixel holds: the test passes only where the
  picture wrote nothing, and neither the camera's far plane nor the size of the scene has to agree with the box. That
  is the gap review 18 `Y9` found - a host-sized box had to stay inside the far plane AND enclose the camera, and
  neither requirement was written where a host would read it.
- **The cull is part of the contract, not an optimisation.** Both shells of the box sit at the far plane, so the depth
  test cannot choose between them; under an orthographic projection there is no `w` to clip the near shell away with,
  and that shell's direction is the ANTIPODAL one. `CullMode::Front` keeps the shell each ray leaves through - which is
  why the program and the state cannot be applied separately.
- **The background is the chain's, not the content's.** It is a scene of its own drawn by a pass of its own, after the
  shading and before every other forward-only pass. As content it would be re-programmed by the G-buffer pass, framed
  by the shadow fit and measured into the content's bounding box - three wrong answers no per-container flag separates
  (`G7`).
- **A declared background is a structural choice on the Deferred path**: its test needs the G-buffer's depth, so the
  lit result is baked into the off-screen composite first - the same rule transparent content already follows.
- **The environment may arrive late.** The map and the pass' enabled flag are one answer (`PassChain::setBackgroundMap`),
  so installing a streamed cube map costs one call and re-registers nothing.
- Boundary: the pair states what a background IS; where it enters the frame, and which depth it tests against, is the
  chain's (stage `Transparent`, one order step earlier than any other pass of it). A host drawing its own background
  uses the pair and states its own placement. `D17` records the rejected alternatives.

## 3. Features

### 3.1 Targets

| # | Feature | Notes |
|---|---|---|
| F1 | Colour attachments | `attachColor(format)`; may be several (MRT / G-buffer). `format` is `channels x family` (nine values, 2.12): the 8-bit family reads back at one byte per channel, the float family renders and samples but is not read back yet. |
| F2 | Depth attachment | `attachDepth(D16/D24/D32/D32F)`. |
| F3 | Shared depth | `setDepthPromotion(false)` + `shareDepth(DepthView)`; the lender draws first, the borrower LOADs. |
| F4 | Validity | At least one own attachment + a positive size; borrowed depth does not count as its own. |
| F5 | Readback | `readColorImage` / `readDepthImage`, both take one `ImageRef` (it names "which image") and return `ColorReadback` / `DepthReadback` = `ReadbackResult<T>`: `status` (`Ok` / `Unsupported` / `NotReady` / `Invalid` / `Failed`) together with `values`, populated exactly on `Ok` (BC-41). `readColorImage(target, attachment = 0)` / `readDepthImage(target)` are short spellings of the same verbs (BC-16), and the byte-width tables answer a texel width or nothing (`colorBytesPerTexel` / `depthBytesPerTexel`; BC-42). |

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
| F27 | One input list | `PassDesc::inputs`: one `ImageInput` per declaration, each naming one identity (`byImage` / `byTarget` / `byName` / `byTexture`); resolution, edges, wiring checks, the frame table and `Plan::describe` read that one list in declaration order (2.13). |

### 3.4 Colour

| # | Feature | Notes |
|---|---|---|
| F16 | `Colorf` | Linear-light float; clear / pass colour / default colours use it. |
| F17 | `Color` | sRGB-encoded bytes; `toLinear()` / `toColor()` are one pair of conversions. |
| F18 | Text and byte order | CSS hex read/write; `fromArgb` / `fromBgr` ordered construction. |
| F19 | Interpolation | `Colorf::lerp` interpolates in linear light. |
| F28 | Display pipeline | Linear shading → an optional `RGBA16F` intermediate → the presenting target's sRGB format encodes at the store (2.14). No tonemap, no exposure: a value above 1 clips at the window. A readback is the stored bytes, not the encoded picture. |

### 3.5 Shading and shadows

| # | Feature | Notes |
|---|---|---|
| F20 | `ShaderProgram` | Vertex / Fragment stages + entry points; `addStage` / `setStage` / `clearStages` / `replaceStages` bump the revision. |
| F21 | Built-in programs | forward / flat / gbuffer / deferred lighting / shadowed; `builtin_<role>.<stage>`. |
| F22 | ABI | Vertex locations, `VinePushRange` (the 128 B projection + model-view a content draw hands its program, and the whole host-program contract - see `graphics-shader-params.md` §2.8) / `VineDrawBlock` / `VineMaterialBlock` / `VineShadowBlock`. |
| F23 | Transparency | TWO INPUTS, ONE RULE: the magnitude is the drawable's `opacity` × the material's own alpha × the base map's alpha, and the material's `AlphaMode` says what that product means (Opaque ignores it, Mask thresholds it, Blend uses it as the output alpha) - sorting and shading read the same pair (`graphics-material.md` §2.7). Blending is not a pipeline constant: `blendEnable` is baked false and switched per draw through `VK_EXT_extended_dynamic_state3` (`StateCommands`), and a draw in the transparent queue that authored no blend resolves to the default pair ENABLED (`resolveRenderState(..., transparent)`) - so the classification the sort used is the one that opens the blend. |
| F24 | Shadows | `Light::setCastShadow` + `ShadowSettings{resolution, bias}`; the light camera is derived by `directionalShadowMatrix` (orthographic) / `spotShadowMatrix` (perspective). **One map per chain, one light (the first buildable directional or spot light)**, point lights unsupported (see 2.7). |
| F26 | Line width | `StateNode::setLineWidth` / `RenderPass::setLineWidthOverride`: the width of **line primitives** (`Topology::Lines`), framebuffer pixels, dynamic state (`vkCmdSetLineWidth`) so no rebuild; `wideLines` is a required device feature, the limit is the device's `lineWidthRange` (`Capabilities::limits.max_line_width`), the floor 1.0. It does **not** widen `PolygonMode::Line` wireframe edges. |
| F29 | Background | `ChainDesc::background` (`BackgroundOptions{map, world_to_map}`): the chain builds the SDK's unit direction box, its material, its scene and its pass; the pass is `skybox` at stage `Transparent` order `-1` (after the shading, before the transparent content), keeps its target, tests depth and writes none. A null map builds the row DISABLED, and `PassChain::setBackgroundMap(map)` / `backgroundPass()` are the install and the switch. |
| F30 | Background program + state | `skyboxProgram()` (direction lookup at the far plane, centred on the eye, scaled to an orthographic window) and `skyboxRenderState()` (`DepthState{test, !write, LessEqual}` + `CullMode::Front`). A host that draws its own sky states both; the chain's background states them for it. |


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
| `ImageInput::byImage()` / `byTarget()` / `byName()` / `byTexture()` | Declaration (one identity each) |
| `ImageInput::kind()` / `description()` | Which identity an entry names; the declaration as text |
| `ImageSource::producedImage()` / `producedTarget()` / `imported()` | Resolution |
| `Sampler` fields | filter / mipmap / address / border / anisotropy / lod |
| `PassDesc::inputs` / `RenderPass::inputs()` | Pass inputs (the one list) |

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

### 4.4 Background

| Interface | Purpose |
|---|---|
| `ChainDesc::background` (`std::optional<BackgroundOptions>`) | Declares the picture's background; unset draws none |
| `BackgroundOptions::map` / `world_to_map` | The environment, and the rotation from world directions to the frame it is authored in |
| `PassChain::backgroundPass()` / `setBackgroundMap()` | The pass (its enabled flag IS "drawn"), and the one live switch (map + enable together) |
| `skyboxProgram()` / `skyboxRenderState()` | The program and the state a background is drawn with - one pair, never one half |
| `kFarClipZ` (`ShaderAbi.hpp`) | The clip z that means "behind everything" |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | The sample count is a target property; image / pass / pipeline are all driven by one `RenderPassCompatibility` (the value a target's `shape()` answers). |
| C2 | produced and imported image sources are not merged; their lifetimes differ. |
| C3 | Colour splits into linear-light `Colorf` and sRGB-encoded `Color`. |
| C4 | The engine core's pass description has no shadow field; "this image is a shadow" is declared by the target itself, consumers do not guess from "the first input with depth". |
| C5 | A colour texel's readback width is its channel count: the 8-bit family is one byte per channel (1 / 2 / 4 bytes); the float family answers `std::nullopt` from `colorBytesPerTexel` and `Unsupported` from a read. |
| C6 | A format is a target only when the device backs colour attachment, sampling AND transfer-source for it; a target asking for anything less is refused by name, never attempted. |
| C7 | A pass' inputs are ONE ordered list of `ImageInput`; each entry names one identity, and every reader (resolution, edges, wiring checks, frame table, plan description) reads that list in declaration order. |
| C8 | A background is NOT content: the chain draws it from a scene of its own, and no pass that walks the picture's content (G-buffer, shadow, content bounds) sees it. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | A shadow is an ordinary pass: the light declares it, the chain builds the pass, no engine built-in shadow subsystem. |
| R2 | A multisampled target's depth resolve takes `sample0`, no averaging. |
| R3 | Target formats gain no second colour semantics. |
| R4 | The shadow convention lives only in the built-in shaders: a pass that does not use the built-in programs has nothing to do with `VineShadowBlock`; sample the map as an ordinary produced image. |
| R5 | A background drawn with the SDK's program states the SDK's state too (`skyboxRenderState()`); one half without the other is a defect, not a style choice (2.15). |

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
| N12 | A shader writes `vec4` into any colour format; the attachment keeps its own channels and drops the rest, so a value added to the vocabulary needs no second fragment-output contract - and a product narrower than four channels is a mask, not a picture with empty channels (`P4`). |
| N13 | `validatePlan` judges only `ImageInput::byName` entries (a plan has no frame); a target/image identity is judged by the compile that resolves it. Both walk the same `PassDesc::inputs` list, so no input is invisible to one reader and visible to another. |
| N14 | A background fragment's depth IS the cleared value (`kFarClipZ`) and it writes none: it therefore passes exactly where an empty pixel is still stored, whatever the scene's size or the camera's far plane. |

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

### 6.4 A narrow attachment read as if it were a picture

- How it fails: `readColorImage` on an `R8` target answers one byte per texel, and a caller indexing it as RGBA8
  (`(y * width + x) * 4`) reads a quarter of the picture at the wrong stride; a fragment writing `vec4` into `R8` keeps
  only `.r`, so a reader of `.g` sees zero and calls the product empty.
- How to detect it: `pixels.size()` is not `width * height * 4`; a mask reads back all-zero except its first channel; the
  target's `colorFormat()` differs from the format the probe assumed.
- How to prevent it: size the read from `colorBytesPerTexel(format)` (never a literal 4; `std::nullopt` means this
  format cannot be read back at all), and name the channel the product lives in - `R8` is a mask, not a grey picture
  with three empty channels (`N12`).

### 6.5 An empty input, or a name that lost

- How it fails: `ImageInput::byName(u8"")` declares nothing (`Kind::None`); it keeps its slot in the binding order
  but resolves to an empty source, so a pass whose only "input" was that entry is a pass that draws nothing
  (`engine.screen.no_input`). Several name entries are a FALLBACK list: the first that resolved is sampled and
  the rest stay quiet.
- How to detect it: `inputs[i].kind() == ImageInput::Kind::None`; `Plan::describe()` and the frame table spell
  such an entry `(empty)`.
- How to prevent it: state the identity you hold - `byTarget` for a target you own, `byName` for a name you can
  publish. A name list is a fallback list by design, not an "all of these" list.

### 6.6 A background drawn as content, or with half its pair

- How it fails: a sky that is CONTENT is re-programmed by a G-buffer pass' override, framed by the shadow fit and
  counted into the content's bounding box - so the sky loses its program, the shadow map's texels line up with a
  world-sized box, and a "fit to content" view flies out to the sky. A sky drawn with `skyboxProgram()` but without
  `skyboxRenderState()` writes the far plane over every pixel it covers (depth write on) or samples the hemisphere
  facing AWAY from the eye (no cull) - and both look right from a perspective camera pointed along an axis.
- How to detect it: the background pass' `content` is the picture's scene; the scene root's `depth()` / `cullMode()`
  are not `skyboxRenderState()`'s; or a pass that walks the picture's content lists an object thousands of units
  across.
- How to prevent it: declare it (`ChainDesc::background`) instead of drawing it, and when a host must draw its own,
  apply the program and the state together (`R5`).

## 7. Rejected Options

| # | Rejected | Reason |
|---|---|---|
| D1 | Hardware compare sampler | This release's pass depth path reads exact depth; a compare sampler needs its own ABI. |
| D2 | Array / 3D textures | `Texture::Kind` has only D2 / Cube; layered targets are a trigger-gated design. |
| D3 | A multisampled target borrowing depth | What is borrowed is the single-sampled resolve image, and one framebuffer attachment shares one sample count. See [ADR](../decisions/ADR-2026-10-22-msaa-target-property.md). |
| D4 | sRGB variants of target formats | Colour space is already solved in the type layer; targets gain no second semantics. See [ADR](../decisions/ADR-2026-10-21-colour-space-in-types.md). |
| D5 | An engine-automatic shadow subsystem, or a shadow field / `PassStage::Shadow` on `PassDesc` | A shadow is the composition of one or more ordinary passes; the engine only provides the generic `Depth` slot, the image identity and the resolution facts. A field would copy "who casts, how it is read" into the pass model (see §2.7). |
| D6 | Point/spot shadows / CSM / PCSS | No consumer; point/spot `setCastShadow` builds no pass today and is reported (backlog S6 waits for a real need). |
| D7 | A three-channel colour format (`RGB8`) | No API family has a three-channel colour ATTACHMENT format (Vulkan's `R8G8B8_*` values are not colour-attachment), so the value would name a product no device can serve. |
| D8 | An integer colour family (`R8_UINT`, `R32_UINT`) | An integer attachment requires an integer fragment output (`uvec4`) - a different shader-output contract - so it is a shader-ABI batch, not a format-table one. |
| D9 | A packed 10/11-bit value (`R11G11B10F`, `A2B10G10R10`) | No product here needs the bandwidth, and `channels x family` is what keeps the vocabulary small (`G8`); a real product is the trigger. |
| D10 | Reading a float colour attachment back as bytes | The verb copies texels and converts nothing; under a byte-count contract a half float misreads every texel after the first. A float readback is a DECODER of its own; `R16F` / `R32F` are drawn and sampled today, and the trigger is a host asking for its product's values. |
| D11 | Keep the three input vectors and document the precedence | `G4`/`G1`: a field that can be stated and silently ignored is not a contract, and one input concept with three storages is the defect 2.13 removes. See [ADR](../decisions/ADR-2026-11-25-one-input-list-per-pass.md). |
| D12 | Resolve names at authoring time and keep objects only | `G9`/`G4`: a transient target exists only after the compile, and a host binding is keyed by name - the object cannot be held when the declaration is written. |
| D13 | `std::variant` for the four identities | `G6`: `ImageSource` is a tagged struct and the declaration must mirror it; private fields plus factories already make an illegal entry unrepresentable. |
| D14 | A separate `InputTarget` type beside `ImageInput` | `G1`: one input, two types; the binding order would need two lists again. |
| D15 | `validatePlan` also checking object identities | `G7`: it would need the host's publication set and the transient identities inside the plan layer, duplicating the compile's wiring rules (`N13`). |
| D16 | Rename the pair to `PassInput` / `ResolvedInput` | `G1`: the existing pair already says declaration/resolution; renaming buys no distinction and costs every search. |
| D17 | A sky as ordinary content; a `DepthRange` band for the sky; a fullscreen triangle + `inverse(projection)`; the shader-only `z = w` fix | The first is `C8`'s defect (G-buffer override, shadow fit, content bounds). A depth band declares a LAYER and does not remove clip-space clipping, so it cannot say "infinitely far". A fullscreen program receives no camera matrices (`VinePushRange` is the content ABI), so the ray reconstruction would need a second ABI for one program. And `z = w` alone leaves the near shell on screen under an orthographic camera - the depth test cannot reject it (2.15). |

## 8. Tests and Acceptance

| # | Criterion |
|---|---|
| T1 | `RenderTargetTest` / `VkMsaaTest` / `VkContentDrawTest` / `VkContentTargetTest`. |
| T2 | A 1-sample triangle edge has 0 blended pixels; 4 samples > 0; the interior is still a solid colour. |
| T3 | A 4-sample depth is readable and has no intermediate values; after resolve it matches the 1-sample one point by point. |
| T4 | `VkContentDrawTest.AFullscreenCopySamplesTheSourceTarget` / `AFullscreenCopyIsDrawnWhateverThePassDepthPolicySays` / `ABaseColorTextureIsUploadedAndSampled` / `ACubeMapIsUploadedWithEveryFaceAndSampled` / `ColorTest` / `ColorfTest` / `ShaderAbiTest`. |
| T5 | Shadows: the ground darkens with a casting light; without one the evidence is byte-identical. |
| T6 | The dynamic-state list: every item the command's `record()` writes is declared in the pipeline (witness: `../bugs/vsg-depth-bias-not-declared-dynamic.md`). |
| T7 | Every colour format: `VkContentTargetTest.AColourFormatBuildsExactlyWhenTheDeviceBacksItAndTheEightBitFamilyReadsBack` (agreement between the device mask and the build path, plus the 8-bit readback values), `VkBackendTest.AColourFormatIsATargetOnlyWhenTheDeviceBacksEveryUse` (mapping, names, all-or-nothing feature rule), `CoreReadbackTest.TheTablesAnswerATexelWidthOrNothingAndCoverBothSides`, `RenderTargetTest.MultipleColorAttachments`. |
| T8 | One input list: `PlanTest.TheDescriptionSpellsEveryDeclaredInput` (every identity spelled in one order), `RenderTest.NamedOutputAndInputSlots` (an empty name declares nothing), `RenderPassTest.ObjectTypedOutputAndInputImages` (kinds and declaration order), `OwnedFrameTest.TheOwnedFrameKeepsWhatItsProducersReleased` (a whole target resolves like an imported texture does), `PassChainTest.AShadowThisChainCannotShadeIsReported` (the shadow map appears in the frame table), `PassScheduleTest.AProducerRunsBeforeAConsumerAnnouncedEarlier` (the edge comes from a target identity). |
| T9 | The background: `VkBackgroundTest.TheBackgroundFollowsTheCameraAndHoldsTheFarPlane` (content 50 units away beats a unit box, and the frame is the map five units from the box's origin), `VkBackgroundTest.AnOrthographicViewKeepsTheShellItsRaysLeaveThrough` (an identity camera never samples the antipodal face), `PassChainTest.ABackgroundIsAChainOwnedPassAboveTheShading` (own scene, own state, row order in the frame table, published by the chain), `PassChainTest.ABackgroundMakesTheDeferredChainCompositeAndInstallsItsMap` (the composite, the borrowed depth, the live map switch, the refusal). |

## 9. Status and Evolution

### 9.1 Current status

Current design; the feature points and interfaces are the tables above.

### 9.2 Known gaps / backlog

See `../backlog/graphics.md`; open items stay by trigger condition.

### 9.3 Compatibility / migration

Old names/paths appear only in §7's rejected options or in git history; new code follows §4's interfaces.

2026-11-26 - the frame's three workload counts have one width (`BC-45`): `FrameCounts`, `PrepareReport`, `FrameResult`
and `FrameCounters` all use `std::uint64_t` for `passes` / `draw_packets` / `draws`, and `drawItemsOf` no longer clamps a
packet's draw count to 32 bits. Why: the asked workload and the recorded frame described the same three numbers in two
widths, so a host comparing them needed a cast and an oversized packet could saturate silently (`G4` / `G6` / `G10`).
What a host must change: recompile every backend module - `PrepareReport` crosses the seam, so
`VN_RENDER_BACKEND_ABI_VERSION` moved to `3u`; a host that stored these fields in a 32-bit variable widens it.

2026-11-25 - the pass-input header is named after what a host writes (`BC-44`): `ImageSource.hpp` is <!-- drift-ok -->
`ImageInput.hpp`, and the include path moves with it (`<vine/graphics/ImageInput.hpp>`). Why: the file
declared the host-written `ImageInput` beside the engine-resolved `ImageSource`, so its name stated the half a host never
constructs, and the guide had to spell `ImageInput::byTarget` under a header named after the other type (`G12`). What
a host must change: one include line. The types, their members and the two lifetimes are unchanged.

2026-11-25 - the state-provenance table is deleted (`BC-43`): `StateOrigin`, `RenderStateOrigin`,
`ResolvedCommandState::origin` and `ResolvedCommandState::topology_origin` are gone, and `resolveRenderState` now answers
"the final state and the effective assembly" only. Why: the seven-field provenance value was written for every command in
every frame and read by nothing outside the tests - no report part, no `FrameTable` row and no host accessor exposed it, so
its own claim ("why is this drawn in Line?") had no reader (`G8` / `G10`, the class `BC-26` / `BC-27` closed the same way).
What a host must change: read `state` and `topology` only; a tool that wants the priority table reads `graphics-scene.md`
2.12/§4.1, which states the layering once. `ResolvedCommandState` crosses the backend seam, so
`VN_RENDER_BACKEND_ABI_VERSION` moved to `2u` and every module that builds that value must be rebuilt.

2026-11-25 - the readback vocabulary has one classifier and names the image (`BC-42`): `readColorBuffer` / `readDepthBuffer`
become `readColorImage` / `readDepthImage` (a target holds images, not buffers), `colorReadbackOf` / `depthReadbackOf`
become `colorBytesPerTexel` / `depthBytesPerTexel` returning `std::optional<std::uint32_t>` (`std::nullopt` = no packing),
`ReadbackFormat` is gone, and the seam's second classification (`readbackOf` / `ReadbackState` / `ReadbackRequest` /
`ReadbackRefusal` / `readbackStatusOf` / `detail::refusalName` / `ReadbackKind`) is deleted. What a host must change: the two
verb names, the two table names (`.readable` -> `.has_value()`), and the answer for a borrowed depth, which is now `Invalid`
(the request names an image that target does not have) instead of `Unsupported`. `ReadbackResult` gained a constructor, so
there is no default answer: every construction states the classification it measured.

2026-11-25 - the readback answer is one value (`BC-41`): `readColorBuffer` / `readDepthBuffer` return
`ColorReadback` / `DepthReadback` (`ReadbackResult<T>` - the status together with the picture it classifies). What a host must
change: drop the out-vector and the `why` pointer - `const ColorReadback read = device->readColorBuffer(target, 0);` then
`read.ok()`, `read.status`, `read.values` (the two verbs were renamed `readColorImage` / `readDepthImage` the same day, see
the `BC-42` note above). `ReadbackOutcome`, `ReadbackResult` (the code, now `ReadbackStatus`) and every
`bool` + out-parameter overload are deleted, not deprecated: every in-tree consumer moved in the same commit and the module
promises no source stability.

2026-11-25 - `T59` landed: the colour vocabulary is `channels x family` (nine values, 2.12), the 8-bit family reads back at one
byte per channel, and a colour format the device does not back is refused by name. What a host must change: nothing that used
`RGBA8` / `RGBA16F` / `RGBA32F` - their meanings and their readback answers are unchanged (the floats are still
`UnreadableFormat`); what is new is that `R8` / `RG8` read back, and that a narrow product no longer needs a four-channel
target to exist.

2026-11-25 - `T56` landed: `PassDesc` states its inputs ONCE (`inputs`, 2.13). What a host must change:
`input_names` / `input_targets` and `RenderPass::inputNames()` / `inputTargets()` are gone - declare
`ImageInput::byName` / `byTarget` / `byImage` / `byTexture` in `inputs`, in binding order. `FrameTable::PassRow::inputs`
still carries `String`s but now spells the whole declaration (`name:X`, `target:X`, `image:label#N`, `texture:D2 WxH`),
so an input stated by object is visible for the first time. No bootstrap (BC-) is needed: every in-tree consumer moved in
the same commit and the module promises no source stability.

## 10. References

`graphics-architecture.md` · `graphics-scene.md` · `graphics-geometry.md` · `vk-backend.md` ·
`../backlog/graphics.md`
