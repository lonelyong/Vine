# the vk backend: a render backend straight onto Vulkan

> **Status**: current
> **Last verified**: 2026-11-23
> **Scope**: the `gfx_backend_vk` plugin (registered as `"vk"`) — a **straight-to-Vulkan** implementation of `RenderBackend`: device and queues,
> targets and windows, content drawing, the device memory pool, the pipeline cache, reports and diagnostics. It does **not** cover the SDK seam itself (see `graphics-*`)
> or the retired reference backend (record: `vsg-backend.md`).
> **Sources**: `src/plugins/gfx_backend_vk/include/vine/backend_vk/**` (the contract), `tests/test_vk/**` (behaviour),
> `scripts/check_vk_exposure.py` (the exposure boundary), `scripts/measure_graphics_claims.py` (re-derives the case / suite counts quoted here)
> **Backlog**: `../backlog/graphics.md`
>
> **Numbering**: this page's `F#` / `C#` / `R#` / `N#` / `P#` / `D#` / `T#` are **this module's** stable ids; an audit finding is written
> "audit `F#`" and a backlog entry "backlog `T#`" — the three sets never stand in for each other.

## 1. Overview

### 1.1 Problem and background

- The SDK's shared mechanisms (`vine/graphics/`) are written for an implementation that does not share the engine's machinery: this plugin's path is **straight onto
  Vulkan** (its own `VkInstance` / `VkDevice` / pipelines), with no scene graph between it and the driver. That path is what the seam's neutrality is judged against
  (`graphics-architecture.md` 2.9, `check_vk_exposure.py`); the second **family** (D3D12) is the next reader and is a plan with a directory, not a wish.
- It consumes the seam, not the engine: what to draw comes from `FrameDescription`, how to draw it from this backend. That is what lets `test_vk` drive it directly
  (the plugin is a MODULE and the test compiles its sources) **with no window**.
- Status: every milestone is landed; among the industry-standard audit's findings (F1–F13, 2026-11-14) the resource and
  cost-model items (audit F1 / F2 / F4 / F7 / F8 / F12) are landed. Behaviour is pinned by `test_vk` **197 cases / 44 suites** (2026-11-24).

### 1.2 Goals

1. **Specification compliance**: the validation layer is silent (0 VUIDs); every Vulkan call's failure shape is classified, not "it does not draw".
2. **Comparable**: a frame's rendered region is read back and compared exactly (`Probe` cases in `test_vk`), so "the seam is implemented" is a measurement rather than a
   coincidence.
3. **Diagnosable**: capabilities report only what is actually enabled; every refusal is "one report per episode" (`ReportOnce`); a missing report field is **not 0**.
4. **Industrial resource and cost model**: a device memory pool (one session, one block, one pool), a descriptor-set cache, a constant ring (dynamic offsets),
   a per-session and a cross-session pipeline cache, and a device clock.
5. **Checkable boundary**: no `Vk*` appears in the plugin shell; Vulkan shapes exist only in `internal/`; the only escape hatch is
   `support/Handles.hpp` (`vkHandles()`), and the frame path does not use it.

### 1.3 Non-goals

- **Several frames in flight** (audit F5): one `submit` + `fence wait` per frame is the **current contract choice** (§2.3); changing it would touch the SDK's "read when drawn"
  and needs an ADR; it is recorded in the backlog, not in the code.
- **A dedicated transfer queue / queue-family ownership transfer** (audit F11): the gain under the synchronous submission model is tiny (`G8`).
- **`VMA`**: no third-party dependency; the hand-written pool already covers today's consumers (§2.4, `MemoryAllocator`).
- **Wayland** (backlog T13): **NOT served** - there is no Wayland branch (`instanceExtension()` answers
  `VK_KHR_xcb_surface` on Linux, and `WindowSystem` has one file per served platform: X11 and Win32), so a Wayland-only
  host gets `UnsupportedSurface` from every window target and the session runs off-screen. Measured 2026-11-28 on a
  host that has both `DISPLAY` and `WAYLAND_DISPLAY`: with `DISPLAY` unset `WindowSystem::open()` answers "no X display
  is reachable (is DISPLAY set?)" and all eight `test_vk` window/probe cases SKIP, because the instrument connects with
  `xcb_connect` - so the off-screen path itself is covered and the TRANSITION (a window refused, then frames on) is not.
  The two SERVED branches are measured on hardware - X11 through `XGetImage` and Win32 through a `PrintWindow`
  compositor read, one shared instrument for `WindowPresentTest` (2026-11-23).
- **Becoming the default backend**: the gate is in `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`. **This backend is the one the application names
  today** — `app_shell`'s `chooseRenderBackend()` states `"vk"` (`VN_RENDER_BACKEND` overrides it) — and that is a host decision, not a verdict from the switch gate:
  the registry has **no** default to switch (an unknown name answers null, and several registered with none stated is a refusal that lists them —
  `appfw-render-surface.md` 2.4 and its `D7`). What the gate still wants is the Wayland ANSWER (serve it, or record that a
  Wayland-only host runs off-screen - backlog T13) and a performance reading it accepts (backlog T35), so Windows does not
  ship before them.

### 1.4 One-paragraph conclusion

**A session = one device + one memory pool + one pipeline cache; a frame = one command buffer + one submission + one fence wait; and every capability, every refusal
and every cache has a witness a test can read.**

## 2. Design Points

### 2.1 The two halves: the shell exposes only the seam, Vulkan lives only in internal

- **What it is**: `shell/` (`Plugin`, `BackendFactory`) names `RenderBackend` / `RenderBackendFactory` only; `internal/` is where Vulkan lives; `support/Handles.hpp` is
  the only door that hands `Vk*` out (to tests / tools).
- **Why**: `G1` (each concept in its place) + `G2` (dependencies point one way: `internal` → SDK → `base`, and `shell` does not depend back on Vulkan types). It also makes
  "replace vk with another implementation" a plugin swap rather than an SDK change.
- **Boundary**: `scripts/check_vk_exposure.py` is the gate (V2 the SDK's backend face, V4 the plugin shell, V5 the SDK's prose, V6 the
  plugin-internal unit names, E1 the frame path, E2 the seam-free low-level units) and it
  runs with ctest.
- **The low-level half**: `MemoryAllocator`, `PipelineCacheFile`, `WindowSystem`, `ReadbackStaging` and `Handles` **do not include `vine/graphics` at all**
  (~2.0k lines) — they are candidates for "a tool may use them without the seam", and E2 pins exactly that. No **external** consumer uses them today (`G8`), so they are
  **not pulled out into a library**: pulling them out would only move code, the boundary is already here; once a consumer that bypasses `graphics` exists, cutting them out
  is a move rather than an excavation. `BarrierPlan` / `ShaderCompiler` each include one seam header (`DrawState`
  for `LoadOp` / `StoreOp` / `ShaderAbi` for `ShaderStage`), so they are not in this group - and `ImageLayout`,
  the one Vulkan-flavoured input `BarrierPlan` used to take from the SDK, is **this module's own type** since
  2026-11-23 (`internal/ImageLayout.hpp`, backlog T15 closed).

### 2.2 The device is session semantics, and loss is the first kind of failure

- **What it is**: `initialize()` / `shutdown()` are idempotent; `initialized()` turns `false` after `VK_ERROR_DEVICE_LOST` and frames are skipped instead of touching the
  device; the rebuild path is fixed at `shutdown()` + `initialize()`.
- **Why**: `G4` (failure modes written into the type / contract) + `G11` (one failure can be located). Device loss is not "a frame failed" but **the session is over** —
  treating it as a frame error makes the host retry on a dead device until the process ends.
- **Boundary**: `noteDeviceLost` is the single classification point (queue submission, acquire and present all go through it), one report per episode.

### 2.3 One submission per frame: synchronisation bought for determinism

- **What it is**: `executeFrame` records one command buffer → submits → waits on its own fence → readback / release can happen right after that call; `prepareFrame`
  exceptionally allows **one transfer-only** submission (ADR-2026-11-14-prepare-may-submit-transfers).
- **Why**: `G3` (the flow reads in one pass) + `G11` ("read when drawn" means neither the tests nor the host has to guess the ordering). The price is written here: the
  **host and the device are serialised**. Measured (lavapipe, 60-frame benchmark): one `renderFrame()`'s wall-clock median is **8.63–8.94 ms** and the same frame's device
  time **7.95 ms**, so the difference of **≈0.7–0.9 ms is the host side** ⇒ **the device is the bottleneck (about 90%)**, not the host. The ceiling on the gain from
  several frames in flight is therefore `max(host, device) ≈ 7.95 ms` (about 10%), and the price is changing the "read when drawn" contract — the decision and the
  arithmetic are in `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`.
- **Boundary**: a synchronous frame is **one reason release became cheap**, but no longer the only one: release now goes to the retirement queue (§2.15), so "no idle
  needed" is the queue's guarantee (witnessed by `device_waits`), not an accident of timing. Breaking "one submission per frame" would still change release and readback
  together, so it needs an ADR rather than deleting one `waitForFence`.

### 2.4 The cost model: four items per frame, each with a witness

- **What it is**: a frame's constants go into **one** persistently mapped ring (`MemorySlice` + `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`); descriptor sets are cached
  by **the resources they bind**; geometry / instance streams, programs and pipelines are cached by **identity**; device memory comes from **one session-level pool**.
- **Why**: `G5` (a change stays inside the module) + `G7` (the complexity is absorbed, not pushed to the caller) + `G10` (counts, lifetimes and layouts are measured).
  The old shape built and destroyed a buffer and its memory per draw per frame and allocated a set per draw per frame; measured, it moved the steady-state median from
  9.03–9.23 ms to 8.16–8.30 ms (−10%).
- **Boundary**: each of the four has a readable witness — `Device::contentCacheCounters()` (`ring_builds` / `set_builds` / `sets_live`), `Device::contentPipelines()`,
  `Device::memoryStats()`, `Device::pipelineCacheReport()`. **An optimisation without a witness has not landed.**

### 2.5 Content identity, never content comparison

- **What it is**: geometry by `GeometryDescription`'s address + revision, textures by `Texture`'s address + revision, programs by description + variant bits;
  `releaseContent` only empties the matching streams (leaving a hole) and the next frame fills the holes only. The instance stream is keyed by its
  `Buffer`'s address **and holds that buffer** (`InstanceStream::source`), which is what makes the key an identity in both directions: no later object can
  be allocated at an address the entry still names, and `revision` compares versions of the one object the bytes came from.
- **Why**: `G5` (the cache key is identity, release leaves a hole, no traversal to compare) + `G10` (one release, one upload, not one rebuild).
- **Boundary**: the key holds an `intrusive_ptr`, so an address is never reused by a later object; that is the premise that lets identity be a key
  (`../decisions/ADR-2026-11-09-address-keys-hold-the-object.md`). The geometry and program entries already held what they were keyed by; the instance
  stream did not, and because a fresh `Buffer` starts at revision 0, an object allocated at a withdrawn entry's address was served the previous object's
  bytes **silently** - the revision check passed (review 19 `Z12`).
- **Withdrawal**: holding the key's buffer makes the cache an owner, so an entry ends two ways - the host's announcement (`releaseContent(Buffer)`),
  and the frame end at which the cache's share is the last one (`sweepUnheldStreams`, called from `endFrame`). The second arm exists because the shape
  this table serves - a host that rebuilds its instance buffer per frame - has no way to announce a buffer it no longer holds, and because a share kept
  for the session's life would trade a stale `VkBuffer` for a stale `VkBuffer` **plus** the host's bytes (`G10`). Cost: one walk of the instance-stream
  table per frame, O(entries), under the lock `endFrame` already takes (`../decisions/ADR-2026-11-27-a-held-entry-is-withdrawn-at-the-frames-end.md`).
- **A refused fill is remembered** (`G11`, review 19 `Z13`): `InstanceStream::failed` records the revision an upload failed
  at, so a broken stream answers null from then on - one report, no second allocation or copy - exactly as
  `ProgramEntry::failed` and `GeometryEntry::failed` already behave. A NEW revision is new bytes, so it retries there.

### 2.6 Shape vs extent: a resize does not recompile a pipeline

- **What it is**: the ATTACHMENT SET is decided by the **shape** (colour / depth formats, sample count); images and views by the **extent**. `planTarget`
  therefore answers Rebuild / ResizeInPlace / Repair (the SDK's `TargetDecision`; `TargetPlan.hpp` is the unit, `planTarget` the verb).
- **Why**: `G1` (shape and size are two concepts) + `G9` (a new format or sample count only extends the shape table) + `G10` (a resize pays no recompile).
- **Boundary**: `ResizeInPlace` keeps the shape, and the shape is the WHOLE pipeline key half that describes the attachments; `Rebuild` is the only path that changes
  it. Nothing is created per attachment set any more, so a load op cannot split the set into variants (see §2.7 and `F4`).

### 2.7 Barriers are one table, not scattered calls

- **What it is**: `BarrierPlan.hpp` (`stateOf` / `barrierBetween` / `recordBarrier` / `loadOpOf` / `storeOpOf`) maps this
  module's `ImageLayout` (a "purpose") into Vulkan's layout + stage + access; every transition goes through
  `barrierBetween`, and `recordBarrier` is the ONE place that names `vkCmdPipelineBarrier2`.
- **Why**: `G1` (one definition, one owner) + `G11` (a synchronisation mistake has exactly one place it can be made, and both the tests and validation point there).
- **Synchronisation 2**: the stage masks travel ON the barrier (they are two rows of the table), so no call site passes a
  stage pair of its own and no site can pair a layout with another row's stages. The Vulkan-1.0 form is not kept - the
  floor is 1.3 and the `synchronization2` feature is enabled and required (`vk.init.device_features_missing` otherwise).
- **Dynamic rendering, and the dependency it does not insert**: since the attachments are stated on the command buffer
  (`vkCmdBeginRendering`), there is no render pass to move them from `initialLayout` to the subpass layout and no subpass
  dependency to order two passes into one image. `DynamicRendering::beginRendering` does both: one barrier per attachment
  from the layout its owner records to the rendering layout, INCLUDING a barrier between two identical states (that is the
  execution dependency), then the instance. `TargetObjects::beginPass` / `WindowTarget::beginPass` are the two call sites; the
  layout bookkeeping is theirs (see `F4` and `P15`).
- **Two depth images, two layouts; a borrowed depth is a dependency, not an exception**: the depth ATTACHMENT and the picture
  it resolves into are two ROLES (`PassAttachmentSet::depth` / `depth_picture`), because a reader sampling the picture moves only
  the picture - a multisampled image may not be sampled at all, so stating the picture's layout for the sample buffer is an illegal
  barrier (`VUID-VkImageMemoryBarrier2-oldLayout-01211`, measured). On a SINGLE-SAMPLED set the two names resolve to ONE
  image, which therefore has ONE record (`TargetObjects::depthPictureLayout`) and the `depth_picture` role is simply
  absent, so the next pass transitions it back (`VkShadowTest.ASingleSampledMapDrawnAgainStatesItsDepthAgain`). A `shareDepth`
  target states the LENDER's record for that image - one image, one layout record, held in the lender
  (`TargetObjects::depth_lender`), with the borrow indirection inside the record's own accessors - and the barrier that follows
  transitions nothing and is recorded for exactly that reason: it is the execution dependency between the lender's pass and the
  borrower's LOAD.
- **Layouts are RECORDS, not derivations**: every image of an attachment set has one `ImageLayout`, written by the command that
  moves it (`TargetObjects::color_layouts` / `multisample_color_layouts` / `depth_layout` / `depth_picture_layout`, and
  `WindowTarget`'s per-image record) and read by the barrier that states where it starts. `TargetObjects::beginPass` only picks
  WHICH record belongs to which role - it computes nothing - so the two answers an image used to be able to acquire ("the
  attachment is never sampled", "the picture was sampled") cannot disagree, and the shape that produced both halves of `P15`
  is no longer expressible. The two READBACK verbs are deliberately not movers: each reads one image and puts it back in the
  layout it found, so a device verb changes no record. `fresh` stays as the one SET-level, atomic MIRROR of "no record
  differs from Undefined", because it is the only form of that fact a reader outside the frame gate may take.
- **A MOVER IS ONE CALL, NOT TWO**: `BarrierPlan::transitionImage` records the barrier AND writes the record it read it
  from, and `TargetObjects::transitionDepthPicture` is the same verb where the record is reached through the
  borrow/resolve indirection. Before this, every site assembled the pair by hand (`barrierBetween(stateOf(record), …)`
  then `record = to`), which is the shape that shipped VUID 09588 and 09592: the barrier and the record drifted and the
  next pass stated a layout the image had never been in. A new mover calls one of those two verbs; a caller that needs a
  same-layout dependency (two passes into one attachment) is `beginRendering`'s job, not theirs.
- **LOAD AND STORE ARE PER ATTACHMENT, AND THE STORE IS DERIVED WHERE THE RESOLVE IS KNOWN**: `PassAttachmentOps` carries one
  `ResolvedAttachment` (the seam's `LoadOp` / `StoreOp` plus the clear value) for EACH colour attachment and one for depth,
  and `beginRendering` DOWNGRADES a resolved role to `DontCare` - a multisampled pass' sample buffers, and a depth that
  resolves, are content nobody can read afterwards, so keeping them is bandwidth nobody buys. `resolveAttachmentOps` is the
  one rule that turns `AttachmentPolicy` (the group flags, plus the per-attachment `color_ops` / `depth_op` an author states)
  into those entries; the neutral ops are mapped by the table's `loadOpOf` / `storeOpOf`, which is what makes those two
  rows live instead of a second conditional inside the recorder. **A pass that announces NO policy clears NOTHING**: a
  later pass into a set that already holds a picture LOADs it, and only a fresh set falls back to `Clear` (there is
  nothing to keep). Reading nullopt as "clear both" wiped the demo's deferred result and every later window pass.
- **A pass states four ROLES, not thirteen fields**: `PassAttachmentSet` is `color` (the images rendering writes), `picture`
  (their resolve targets), `depth` and `depth_picture` (`DynamicRendering.hpp`). A role with one image is a `PassImage`
  value (image + view + its record), a role with one image per colour attachment is a `PassRole` of three parallel spans -
  three and not one array of structs because a full-screen draw binds the views as ONE contiguous `VkImageView` span, and
  converting per pass would allocate on the recording path.
- **Boundary**: the table covers the FIVE purposes `ImageLayout` states (Undefined / ColorAttachment / DepthAttachment /
  ShaderReadOnly / Present) plus the two a device verb needs (`transferReadState()` / `transferWriteState()`, kept out of the
  enum because a readback or an upload is not something a pass declares); a new purpose is one more row, not one more inline barrier.
  An ATTACHMENT row states the read side of its access as well as the write side, because under dynamic rendering the
  LOAD (and a blend's destination read) has no implicit ordering.
- **ONE DEPENDENCY PER PASS ENTRY, AND THE SUBMISSION IS 1.3 TOO**: every attachment barrier a pass needs is stated in a
  single `VkDependencyInfo` (`recordBarriers`), so a seven-attachment multisampled set is one command with seven barriers
  (`BarrierBatch` is the same batching for a pass' sampled-source moves). The queue follows the same rule: the frame
  submits through `vkQueueSubmit2`, the wait stage travels on the semaphore, and no 1.0 stage mask is paired with a 1.3
  command buffer. The clock is the one place where a stage is chosen for a REASON rather than by the table: a frame's
  start takes no wait, every other boundary waits for `ALL_COMMANDS` (see F13 and P18).

### 2.8 Honest reporting: absent is not zero, one report per episode

- **What it is**: a missing `timing` in `FrameReport` means "this card does not report a clock", not 0; the same holds for `PrepareReport`, `FrameCounters`,
  `memoryStats` and `pipelineCacheReport`; every "cannot" has its own `ReportOnce`.
- **Why**: `G4` (a field says what it is) + `G11` (the host can tell "did not happen" from "happened and was 0").
- **Boundary**: `ReportOnce`'s `rearm` is called at the place where the condition ends, not cleared wholesale at shutdown (otherwise the next session's first refusal
  would be swallowed by the previous session's silence).

### 2.9 The location is the host's, the format is the backend's

- **What it is**: `DeviceDesc::cache_directory` is a **creation input** (the host says where files may go); `PipelineCacheFile` says what the file looks like
  (`[magic][format][key][size][blob]`, the key = vendor / device / driver / API / `pipelineCacheUUID`). When the host passes nothing, the plugin uses its own application
  data directory as the default in `Plugin::load`; neither = no persistence.
- **Why**: `G1` (ownership) + `G2` (the SDK does not know the cache format and the backend does not know the user's paths). Putting "where user files go" into the backend
  makes the render layer take a product decision for the application; putting the cache format into the SDK makes the seam know about Vulkan.
- **Boundary**: the cache is an optimisation, not correctness: an unreadable file, a driver rejection and a missing directory all mean a cold start only.

### 2.10 The test is half the contract

- **What it is**: `test_vk` compiles the plugin's sources and drives the device with no window; `tests/BackendSeamTest.cpp` is the conformance suite every backend's
  build carries; and key guards are proven load-bearing by **mutation batteries** (turn it off and the matching case must go red).
- **Why**: `G11` (the module is testable on its own and a failure can be located). A windowless test puts CI and the desktop on the same path.
- **Boundary**: `test_vk`'s device cases **skip** on a machine without a Vulkan device instead of failing (the environment is not the contract).

### 2.11 One `Device` object = one session pool (`G1` / `G8`)

- **What it is**: this plugin's `Device` is both the **device pool** and **its own session**: one `VkInstance` / `VkPhysicalDevice` / `VkDevice` / graphics queue, one
  device-level content cache and target table, one `DeviceClock` + `RetirementQueue`; `createFrameDriver()` lends an additional `FrameDriver`.
  `RenderDevice::executor()` answers the device's own driver.
- **The frame machinery** lives in the driver — command pool / fence / timestamp pool / frame books / `FrameGate` / clock window; the device owns it and destroys it in
  `shutdown()`; the frame body (`executeFrame` / `prepareFrame` / `frameReport`) is in the driver too, and `Device` keeps three one-line forwards.
- **The session seam**: `createFrameDriver()` builds a `FrameDriver` (allowed before `initialize()`, which then starts it; built on the spot on a started device), and
  `releaseFrameDriver()` destroys and forgets it (the device's own driver cannot be given back). The default content program belongs to `RenderFrameExecutor`: each of this
  device's `FrameDriver`s holds its own, and `Device` overrides the call to forward to its own driver.
- **Concurrency**: the device's big lock is `graphics::FrameGate body_gate`: a (non-window) frame body takes SHARED, so two drivers on two threads record and submit at
  the same time; a frame outside a window and every host verb (readback / release / default program / release target / create window / diagnostic reads) take EXCLUSIVE
  (they may wait for a frame, they do not race it); `shutdown` takes EXCLUSIVE too. **A host verb called from a frame-body thread gets an empty lease**: it answers its own
  refusal value (a read answers `Failed`, a target creation answers `UnsupportedSurface`, a counter answers 0) and reports **one** `vk.device.reentry` (`G4`: a refusal
  must be heard rather than silently continuing). The report sink's lock is **recursive**, because a sink may itself call back (a second report from the same thread is
  that `vk.device.reentry`). Around it sits a set of small locks: `clock_mutex` (`DeviceClock` + `RetirementQueue`, both SDK value types still documented as not
  thread-safe, a multi-threaded device supplying its own lock, `P4`), one for the allocator, one for the target map, one for the report sink, one for each session's default
  program, one for the surface list. `FrameTimeline` supports several open frames (a number is minted when a frame opens). Texture staging and replaced images belong to
  their driver by `ContentImages::BatchId`, and a replaced image parks to the watermark. Sampled targets (a full-screen source / a pass input) also take a target lease.
  **Cases**: `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame`,
  `VkConcurrentFrameTest.AHostVerbCalledFromInsideAFrameIsRefusedInsteadOfHanging`.
- **Release**: `releaseContent` (three overloads) and `releaseRenderTarget` do not take the body gate: they wait for one cache operation only (the lock the frame head's
  resolution holds), and safety comes from the cache lock + park + the watermark's open-frame rule. An entry in `targets` is a `shared_ptr<TargetObjects>` (a release hands
  the reference to the park, and a frame that already resolved it still holds a valid pointer), `released_nodes` is atomic, and the queue's readers
  (`retirementNumbers` / `frameReport`) take `clock_mutex` too.
- **Three pieces of frame scratch**: `ContentDraw` is not one object that holds everything but three: `ContentCaches` (**the device**: programs / geometries / instance
  streams / pipelines / layouts / images / the pipeline cache), `FrameScratch` (**the driver**: the constant ring + the descriptor pool + the set tables +
  `frame_index`), and `ContentDraw` itself (**the recorder**: it borrows the first two and has only `prepare` / `prepareScreen` / `record` / `recordFullscreen`).
  `Device::FrameDriver` owns its `scratch` and `recorder`; `Device::Impl` keeps only `caches`; `createScratch()` is called by `initialize()` once the layouts exist, and
  on teardown the scratch goes before the caches (the sets' layouts are borrowed).
- **Why this shape**: one frame in flight per driver (§2.3) makes "between frames" an **instant**, so release, readback and cache filling can share one machine;
  `ContentDraw`'s `beginFrame` / `endFrame` resetting device-level state is a direct consequence. Under that premise, splitting it into two objects would only add a layer
  of forwarding (`G8`).
- **Boundary**: two drivers' frame bodies may run at the same time; two frames of the same driver are still serialised by its `FrameGate` (another thread **waits**, a
  same-thread reentry is **refused**, `vk.session.frame_reentry` / `PresentOutcome::Refused`). A window frame takes the body gate exclusively (the swap chain is one device
  resource). Each frame reports what it spent on the cache lock (`FrameReport::locking`: one hold at the frame head + one sample at the frame tail, a constant in the
  steady state with `waits` 0, see `VkFrameHeadTest.AFrameReportsWhatItSpentOnTheCacheLock`). Readback still takes the body gate EXCLUSIVE, so it **waits** for an in-flight
  frame; release does not (R6). The shape and its argument are in `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`.
- **What would move if the split were taken further** (a starting list, not the split itself): half of `ContentDraw`'s `Impl` is **cache** (modules / streams / pipelines /
  layouts, the device's) and half is **frame state** (the constant ring / the descriptor pool / per-draw UBOs, the session's); `command_pool` and `completion` belong to
  the session; `allocator`, `targets`, `surfaces`, the pipeline cache and `default_program` belong to the device.

### 2.12 Allocation failure is classified in one place, the device (`G1` / `G4`)

- **What it is**: `MemoryAllocator` answers **facts** only — a `MemoryRefusal` (`NoType` / `DeviceLost` / `OutOfMemory` / `DriverRefused` / `InvalidRequest` / `NotOpen` /
  `Internal`) + the driver's raw `VkResult` + the requested bytes / type / pool's current numbers, in a `MemoryRefusalReport`, handed to a sink on **every allocation,
  including a successful one**. The sink is installed by `Device` in `initialize()` and is its only consumer; `Device::noteMemoryRefusal` is the only classification
  point: `DeviceLost` goes through `noteDeviceLost` (the session ends, one report), `OutOfMemory` is a warning that the host may release content, `DriverRefused` is an
  error to report, and the two wiring defects each have their own.
- **Why**: `allocate` has six call sites and five of them need only "did it work" — handing the reason back means asking every call site to remember to use it, and they
  will not. That is exactly the defect that was fixed: `vkAllocateMemory`'s `VkResult` was dropped and **device loss was read all the way through as "out of video
  memory"**, with the host retrying on a dead device (audit `A1`).
- **Boundary**: the pool does not include the seam (`C10`), so it does not know `DiagnosticCategory` and should not; the sentence (with its numbers) is written by the
  device. A success is reported as well, because a `ReportOnce` episode must re-arm where its condition ends.
- **Cost**: one `std::function` call and one stack report value per allocation; allocation is a miss-path event (`R24`: a steady-state frame allocates nothing), so the hot
  path is unaffected.
- **An actionable refusal** (audit `A2`): `MemoryStats` carries `free_bytes` / `largest_free_run`, and **only on a refusal** does it walk the block table to fill them
  (the success path takes the existing counters). So "the heap refused a new block while the pool still has 12 MiB free (largest contiguous run 1 MiB)" becomes one
  sentence, and the host knows from it that releasing one large resource helps and releasing small ones may not. **There is no reclamation ladder**: nothing in the pool
  can be evicted (a release already destroys wholly empty blocks, keeping at most one as a cache), and what really frees video memory is the host's `releaseContent`, so
  the honest move is to say why it is not enough rather than to grow a mechanism that pretends it can make room (`G8`).

### 2.13 Diagnostics carry a code, a subject and a limit: the sentence is for people, the values are for the host (`G4` / `G11`)

- **What it is**: every report carries a **stable code** (`vk.<class>.<cause>`, registered in `scripts/diagnostic_codes.txt`), a **subject** (which target this report is
  about) and an optional **limit** (a `requested` / `supported` pair). The `message` still carries every number, but **it may be reworded** and the code may not.
- **Why**: the host and the cases must be able to act **without parsing the sentence** (`R28`: assert by code). Two causes under one category ("over the limit" and "not a
  power of two") are two codes in value and two sentences in prose — with sentences only, a case can do nothing but `find(u8" MiB")`.
- **Boundary**: the code vocabulary belongs to **the module that reports it** (the SDK does not know `Vk*`, `C3` / `C10`), so it is a string rather than a central enum;
  `check_diagnostic_formats.py` checks it both ways (what is used must be registered, what is registered must be used) and uses `scripts/diagnostic_code_baseline.txt` to
  put a **ceiling** on call sites that have not been converted — a new report must carry a code.
- **Converted**: **every report** in this backend — memory, session, targets, windows, initialisation, frames and preparation, content: seven families, 65 codes in all;
  `scripts/diagnostic_code_baseline.txt` no longer lists any of this plugin's files (audit `A3`), and that file has been **empty** since batch 4.
- **Cost**: one `String` field on the report path (the episode is already deduplicated, so it is not on the hot path); `limit` is a `std::optional` and costs nothing when
  no limit is reported.

### 2.14 The cause is named where it is known: a content refusal is one value, not two out-parameters (`G1` / `G4` / `G7`)

- **What it is**: a refusal on the content path (why this draw did not come out) is produced by **the recorder** — only it knows which level refused (program, stream,
  instance stream, pipeline, ring buffer, descriptor set). It fills one `ContentRefusal` — **it is an alias of the seam's `graphics::DiagnosticDraft`** (`category` +
  **code** + subject + limit + sentence, with `refuse()` as the only writer and first-writer-wins); `executeFrame` / `prepareFrame` only **forward** it as a
  `RenderDiagnostic`. A readback refusal produces the same type: a refusal has one shape only (`ADR-2026-11-16` §4).
- **Why**: this path used to be two out-parameters, `(vn::String* why, DiagnosticCategory* category)` — fill one and forget the other, and of the seven reasons for
  "skip one draw" only one `ContentSkipped` reached the host. Putting the code on **the side that knows the reason** is what first lets the host and the cases say "this
  draw has no program" rather than "some draw was skipped" (`R28`'s landing point).
- **Boundary**: `refuse()` is the only writer and **first-writer-wins** — a second reason ("no geometry, so no pipeline either") does not overwrite the first, a rule that
  used to be repeated three times as `if (why.empty())` at the call sites. An empty sentence with a code is still a refusal: whether a sentence exists was never the
  criterion. `ContentRefusal` is an **internal header** (`internal/ContentRefusal.hpp`) and does not enter the SDK; the shape itself lives in the SDK's backend half.
- **The reason is an enum and the code is one `switch` with no `default`** (`ContentRefusalCause` / `contentRefusalCode`): `refuse`'s call sites used to write string
  literals directly, so a new cause could compile without a code (D-22). Now adding an enum value fails to compile without its `case` (the same shape a readback
  refusal's mapping uses); `VkContentRefusalTest.EveryCauseNamesAStableCode…` additionally checks that every value has a code and that no two share one.
- **The two halves of the gate**: a report call that **constructs** a diagnostic (≥3 arguments) must write a code; one that **forwards** a `RenderDiagnostic`
  (`reportDiagnostic(diagnostic)`) does not count — the code travels inside the value and a sink cannot drop it. `check_diagnostic_formats.py` checks the registry both
  ways at once: what is used must be registered, and what is registered must be used by some builder / code table / report.

### 2.15 Release is handed to the queue, not destroyed at once (`G4` / `G10`)

- **What it is**: `releaseContent` / `releaseRenderTarget` **forget the cache entry on the spot** (the residency count drops immediately and the next frame uploads again),
  while the Vulkan object behind it goes to `graphics/RetirementQueue.hpp` and is destroyed only once the device's completion watermark has passed that
  submission. The evidence is `graphics/DeviceClock.hpp`'s SUBMISSION SEQUENCE (`ADR-2026-11-24-completion-belongs-to-the-submission`): every
  `vkQueueSubmit2` takes the next number inside the queue's lock and reports that number's proof after its fence, and the watermark is the oldest
  unproven submission minus one. Driver registration (`addDriver` / `removeDriver`) is identity only - the watermark never consults it, which is what
  keeps an idle engine from freezing every parked release.
- **Why**: release has to say two things — "is this content still wanted" and "may this object be destroyed now" — and putting them together is what forces the host
  to keep the ordering "release only between frames". Split apart, each answer has one mechanism (`G6`), and the split is the premise of several sessions
  (`.ai/design/graphics-sessions.md` §2.6).
- **Cost (written here rather than hidden)**: a released object lives to `submissions + slots + 1`, i.e. is destroyed **at most two submissions later** (the conservative
  price of the evidence "the slot that recorded it has been reclaimed"). Two frames of memory latency is cheap against one device stall, and the waiting becomes a background
  event timed in submissions.
- **Boundary**: **teardown does not park**. `shutdown()` calls `vkDeviceWaitIdle` first, then clears the seam and calls `releaseAll()` (it is the verb for "arbitration has
  already happened"); otherwise parked objects leak with the session. The **replacement path** (an old buffer on re-upload, a ring rebuild) still destroys immediately, because
  it happens while this frame is recorded and before submission; with several sessions they must park together.
- **Observation point**: `Device::retirementNumbers()` (`pending` / `released` / `device_waits`). `device_waits` records "the host stopped the device": destructive paths that
  cannot park, and **readback's wait** (readback waits for the frame it reads, one count per wait). The frame's assembly path (record / submit / fence wait) never raises it —
  that is the invariant this number guards.

### 2.16 The device is a pool, drivers are frames: caches and frame scratch split (`G1` / `G4` / `G10`)

- **What it is**: the content path is split into `ContentCaches` (the device) and `FrameScratch` (the driver), and `ContentDraw` degrades to a recorder over both.
  `ContentCaches` holds programs / geometries / instance streams / pipelines / the three set layouts and two pipeline layouts / the pipeline cache / `ContentImages`, plus
  release (`release*`) and the residency account (`Progress`); `FrameScratch` holds the constant ring (one host-visible buffer + a slice cursor), the descriptor pool, the set
  cache, `draw_set`, `frame_index`, `ring_builds` / `set_builds`.
- **Why**: the caches are keyed by **host identity** (program revision / geometry revision / buffer address) and rebuilding them is expensive — a second driver wants to
  **find** them, not to copy them; while the ring and the set cache are written **per frame** (`beginFrame` hands out slices from zero and they grow inside a frame), so two
  frames interleaving writes to one ring overwrite constants the other's submitted frame still reads. Cutting "expensive and shared by identity" from "written and broken
  every frame" along that line is what makes `createFrameDriver()` "one more driver" rather than "one more thing that can destroy the first" (`G5`).
- **Boundary**: layouts belong to the caches (pipeline state needs them), the pool and the ring to the scratch (allocation happens inside a frame); the scratch **borrows** the
  layouts and the device entry, so the teardown order is scratch → caches → allocator → device (the same family of rule as `P8`). The set cache is evicted by `frame_index`
  rather than by a lock: it belongs to its own driver.
- **Cost (honestly)**: `ContentDraw`'s `record` pays one more pointer indirection per frame (two borrowed references), and what it buys is that "who owns in-frame state" can
  point at one object in one line.
- **Lock and frame-head snapshot**: `ContentCaches` has **its own lock** (`ContentCaches::Lock`, non-reentrant) and a frame takes it **once, at the frame head**: `planCommand` / `planScreen`
  (both overloads) / `resolveTextures` write what each draw call needs (pipeline, vertex / index / instance buffer handles, material, texture view / sampler, shadow sampler)
  **by value** into `FrameHead::DrawPlan` (the frame-level texture / sampler facts hang off `FrameHead`), and texture uploads are recorded into the frame's own command buffer
  at the frame head (outside a render pass, the only legal place). **The recording path afterwards takes no lock and touches no cache entry**; when an entry is replaced or
  released the old object **parks to the watermark**, so "the values resolved at the frame head" stay valid for the whole frame (`G10`). The `Lock&` parameter turns "I hold
  the lock" into compile-time evidence (`G4`).
- **Observation point (T3's exact form)**: `Device::contentCacheCounters().cache_lock_acquisitions` is a cumulative count. A whole frame is a fixed **4** (1 at the frame head +
  1 each for the frame head's and the frame tail's content bookkeeping (`ContentCaches::numbers()` reads both halves once) + 1 in `endFrame`) and **does not grow with the draw
  count**; "a steady-state frame takes 0" means **the recording path**, and the frame head's 1 is the mechanism §2.3 describes (see `graphics-sessions.md` §8 T3's correction).
  `VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls` compares the same number with 1 vs 6 commands; putting per-draw locks back on the recording path turns it red
  (measured 6 → 10).
- **Observation point (the other half)**: `Device::contentCacheCounters()` is assembled from two halves — the ring / set numbers come from the device's **own driver**'s
  `FrameScratch`, the rest from `caches.numbers()`. The single-session semantics are value for value what they were before the split (`test_vk` green).
- **Frame-head storage reuse and its witness (I-02)**: `FrameHead`'s pass / packet slots are `clear()`ed rather than rebuilt (`storageGrows()` increases only when capacity is
  short); `Device::frameHeadStorageGrows()` exposes it to tests, and `VkFrameHeadTest.ASteadyFrameDoesNotGrowTheHeadsStorage` pins it with "3 warm-up frames, then 5
  unchanged". **It proves the head's storage reuse only**, not a whole frame allocating nothing (pipeline keys and content caches have their own allocations, which this page
  neither claims nor measures). `planCommand` clears with `plan.clear()` rather than releasing and rebuilding the reused vector per command.
- **A pass' SELECTION is applied at the same place as its state and its program override** (`G1` `G10`, 2026-11-27): a
  `PassSelection::Opaque` pass (a depth map) answers "not recorded" for a command the sort calls transparent, so the plan
  stays empty and the recorder skips exactly that command - the pass' other commands keep theirs. The command list is never
  filtered: it is the shared, immutable list every pass of the frame draws, and rewriting it per pass would cost a
  command-sized copy on every frame (the same trade `program_override` documents). The empty plan carries no `ContentRefusal`
  - the pass asked for the content's opaque set and this command is not in it, which is an answer, not a failure.
  Witness: `VkShadowTest.ADepthPassRecordsOnlyWhatCanOcclude` runs one blended caster twice (selection `Opaque` vs `All`) and
  reads the map's texels.

### 2.17 Residency's three states: one key has one publisher (`G1` / `G4` / `G10`)

- **What it is**: `internal/ResidencyClaims.hpp` is a state machine with one entry per key: `Absent → Uploading(owner, token) → Resident`. `admit(key, session)` answers one of
  three things — `Use` (already published, use it), `Build` (I won the compare-and-set and hold a move-only `Claim`), `Copy(owner)` (someone else is filling it, build an
  unpublished temporary copy). `Resident` may only be declared by the token-holding owner through `Claim::publish()` ("the fill is complete and the evidence is mine"); an
  unresolved `Claim` returns the key to `Absent` when it is destroyed.
- **Why not "wait"**: waiting is the one shape that can lock two drivers against each other (design §2.4 `P2` / `D3`), while copying is bounded — at most one copy per key per
  competitor, in the frame where "two engines first draw it at the same time". An `Uploading` entry equals `Absent` for everyone else, so a half-built entry is never used as
  `Resident`.
- **Why a token rather than "whoever built it publishes"**: tokens are monotonic and never reused, so three real orderings can be answered honestly: `forget`
  (`releaseContent`) happening mid-fill, the claim already withdrawn, and the key taken by a new claim after a release — an old token's `publish` always fails and counts in
  `Numbers::stale` ("the caller believed it owned it").
- **Boundary**: the table **destroys nothing** and **retires nothing**: `forget` only forgets the key, and the Vulkan object still goes to `RetirementQueue` (§2.15).
  The table answers one question only — "who may build this key" (two drivers, up to publication only) — and never "may this value be released now"; that is
  `RetirementQueue`'s sentence (§2.15).
- **Wiring**: `ContentCaches::ensureGeometry` asks the table first (`Use` / `Build` / `Copy`), the frame-scoped `ContentCaches::FrameBuilds` `publishAll()`s after the frame
  fence was waited on (skipping failed fills), and `releaseGeometry` also `forget`s the key; a competitor takes `Copy`, builds a **detached** temporary entry and destroys it
  when the frame ends.

### 2.18 The target table: objects, shapes and one holder per frame (`G1` / `G6` / `G7`)
- **What it is**: the device holds one `TargetTable` — from the host's `RenderTarget*` to the `TargetObjects` it becomes (map + shape cache + build / rebuild / repair), the
  seven target-refusal episodes, and the per-frame leases (`RenderTarget* → FrameDriverId`) **are three questions of one table**; `lease(driver)` issues a frame-scoped `Lease`;
  `executeFrame` and `prepareFrame` `claim` each pass' targets **before `ensure`**. On a conflict that pass **is skipped** (the frame's other passes still run) and
  `vk.target.claim_conflict` is reported — **one per (target, episode)**, the episode ending after a frame or a preparation without a conflict.
- **Why per pass rather than per frame**: one local conflict must not be amplified into "the frame failed"; the conflict takes the existing "this device cannot build this
  target" report path (`G6`), with one more code.
- **Why the claim comes before `ensure`**: building / rebuilding attachments is **the holder's verb** (`R5`), so "who may rebuild" and "who may draw" are the same answer; a
  claim one step later would let two sessions interleave rebuilds on one target.
- **Residency borrowing: a release ends residency, not the objects** (`ADR-2026-11-24-residency-borrowing`). A target's
  or a content entry's GPU objects have one owner (the device's residency) and are LENT to a frame for that frame's
  life and to a host verb for that call's life; the destruction waits for two windows — no submitted work names them
  (the park's timeline) and no value the device lent still names them (a `use_count` probe the park re-asks at a later
  frame end). Neither window ever makes a release wait (`R6`). The second window is not academic: off-screen frame
  bodies overlap (`executeFrame` takes the body gate SHARED), so one engine's frame end can run the park while another
  engine is still recording. Values that name a record keep it alive — the pass handle for what a frame DRAWS INTO
  (§2.20), `FrameRecording::read_records` for what a frame READS — and both are dropped when the frame ends.
  **Content obeys the same rule** (`G12`: one rule, two tables). Each table holds `shared_ptr` entries
  (`GeometryEntry`, `InstanceStream`, `PipelineHandle`, `ContentImages::Impl::Entry`), `FrameBuilds::borrow` keeps every
  entry a head resolved until the frame ends, and every release path erases or REPLACES the table's share before it
  parks - replacing rather than mutating, because a table's own share inside a parked entry would make the `use_count`
  probe count it and never destroy anything. Park witnesses: `VkMultiSessionTest.AFrameKeepsTheContentItResolvedUntilTheFrameEnds`
  (a release announced while another engine records: three frame ends run the park, and the frame still draws its
  picture; with `borrow` made a no-op the pool loses the streams and the picture is black),
  `VkMultiSessionTest.AReleaseRacingAFramesContentNeverLeavesACommandBufferNamingAFreedBuffer` (the interleaving is not
  forced; every interleaving must have a legal answer), `CoreRetirementQueueTest.AReleaseCallbackMayParkSomethingNew`
  (a release act is handed the clock, which is what lets it ask for another window without the lock it runs under).
- **Why `ensure` answers the record, not a boolean**: the borrowed pointer a pass keeps must be the record the build left behind, taken under the same map lock. A release
  does **not** wait for the frames in flight (`R6`), so it can erase the entry at any moment — a boolean answer resolved by a second `lookup` was one lock late, and that
  call answered null for exactly that release (`G4`: one call, one answer; `G10`: the window was a lifetime the caller had to re-prove). The answer is a `shared_ptr`, so a
  release announced right after the call parks the record instead of destroying it — **and the pass handle keeps it** (§2.20), so the record cannot be destroyed while a
  frame that already resolved it is still recording.
  Witnesses: `TargetTableTest.EnsureWithoutASessionAnswersNoRecord` (the refusal is the same spelling — a null record, no entry),
  `VkMultiSessionTest.AReleaseCannotTakeAwayTheRecordAPassHasJustResolved` (the answer is whole after an erasing release),
  `VkMultiSessionTest.AReleaseRacingTheFramesClaimsNeverLeavesAPassWithoutATarget` (the interleaving is not forced; every interleaving must have a legal answer) and
  `VkMultiSessionTest.APassHandleKeepsItsTargetAliveAfterARelease` (the park's release act runs first, then the handle still names a live record — ASan reports a
  heap-use-after-free with the old borrowing handle).
- **Why one table**: the map and the lease answer the same question — "what is this target now, and who may touch it". With two tables a release had to do half in each place
  and a test had to drive two objects at once (`G1`); merged, `release(target)` is one call ("the entry leaves the map + the record goes to the park") and `drain()` clears
  records and leases together. The lease still holds its own lock (another driver's per-pass claim must not queue behind one build).
- **Boundary**: a lease is not a lock (`C5`) — waiting would tie two engines' frame rates together; a conflict destroys nothing and the target is retried next frame. `Lease`'s
  destructor is the **only release point** (`P3`), so no early return in a frame can pin a target. A window target needs no claim: `createSurfaceTarget` already guarantees one
  window belongs to one session (`WindowOccupied`).
- **When a conflict happens**: once two drivers are lent out by `RenderDevice::createFrameDriver()`, the conflict is exercised by a lease impersonating "another driver" and by
  `VkMultiSessionTest`'s concurrent cases; a single driver never conflicts.

### 2.19 The recording path is batched, shadowed and named (`G10` / `G11`)

- **What it is**: three facts that used to be conventions are mechanisms with numbers.
  * **ONE DEPENDENCY PER PASS ENTRY** (`BarrierPlan::recordBarriers` + `BarrierBatch`): `DynamicRendering::beginRendering`
    states every attachment barrier of a pass in ONE `VkDependencyInfo`, and a pass' sampled-source moves are one
    dependency per source. Before this, a seven-attachment multisampled pass issued seven `vkCmdPipelineBarrier2` calls
    that each said the same thing about a different image - the same ordering, stated expensively.
  * **A STATE COMMAND IS ISSUED WHEN THE STATE CHANGES** (`ContentDraw::RecordedState`): viewport, scissor, line width,
    depth bias, cull, front face, polygon mode, depth test/write/compare and blend are diffed against what this frame's
    command buffer already holds. The viewport and the scissor are PASS facts (they are computed from the pass'
    rectangle), so a pass with ten draws used to state the same rectangle ten times. The shadow SURVIVES a pipeline
    bind, and that is a property of THIS backend rather than of Vulkan: a bind resets no dynamic state, and every
    pipeline here comes from `buildPipeline`, which declares the same twelve dynamic states - so the value the buffer
    holds is still one the new pipeline reads, and a pass that alternates two pipelines pays one command per bind
    instead of re-stating everything
    (review 9 `K6`). It is reset by `beginFrame()` alone, because every frame begins a new command buffer.
  * **THE FRAME NAMES ITSELF** (`Device::debugMarkers`): one `VK_EXT_debug_utils` region around the frame and one around
    each pass, named from `RenderPass::name` - so a capture tool's tree, the host's `PrepareReport` and
    `FrameReport::timing`'s per-pass entries all use the same names.
- **Why**: `G10` (a command is a cost, and repeating a state the buffer already holds is a cost with no meaning) + `G11`
  (a capture without labels is one anonymous command stream, and a pass timing nobody can match to commands is a number
  without a consumer). Both are recorded as NUMBERS rather than as claims: `Device::recordingNumbers()` answers
  `barrier_dependencies` / `barriers` / `state_commands` / `debug_labels`, cumulative, and each case takes its delta -
  the same rule §2.4 lays down for the ring, the set cache and the pool.
- **Boundary**: the shadow is the recorder's and lives for one frame; a second pipeline BUILDER with a narrower dynamic
  list would have to invalidate it where it binds, which is why the list is one constant inside `buildPipeline` rather
  than a property each call site restates. A session whose
  loader has no `VK_EXT_debug_utils` names nothing (`debugMarkers()` answers false) rather than failing a frame, and its
  `debug_labels` stays 0.
- **Cases**: `VkContentDrawTest.APassEntryIsOneDependencyWhateverItWrites` (a three-attachment pass = one dependency
  carrying three barriers), `ARepeatedStateIsNotARepeatedCommand` (two identical commands cost what one does),
  `TwoAlternatingPipelinesRestateNoState` (two pipelines that differ only by program cost one command - the bind - per
  switch) and `AFrameAndItsPassesAreNamedToACaptureTool` (passes + 1 regions).

### 2.20 One pass is one value; one frame is one command buffer (`G1` / `G5` / `G6`)

- **What it is**: the frame body used to keep four descriptions of "the pass about to run" - a local `Runnable` list,
  `FrameHead::Pass`, `PassAttachmentSet` and `PassAttachmentOps` - plus the `pass_position` / `pass_index` arithmetic that kept the
  indexings in step. `FrameHead::Pass` now carries the frame facts (`entry`, `draw`, `target`, and - since review 9 -
  what the pass READS): the walk claims them, the head resolves the plans into the same slots while it holds the cache
  lock, and the recorder walks that one array. **A
  pass' attachment set is itself a value** (`PassTarget`): the off-screen set and the window's acquired image answer the
  same four questions - extent, pipeline compatibility, what is already in it, open and close it - through ONE handle,
  so the frame body has no `if (window) A else B` pair left (it used to have four). **The handle also states who keeps
  the set alive** (`G4` / `G12`): the window is the device's object and is borrowed for the session, while an off-screen
  set belongs to the target table and a host may release it mid-frame (`R6`: a release does not wait), so the off-screen
  handle SHARES OWNERSHIP of the record and the record lives as long as the head's pass slot does. The park then dates
  only the images' destruction against the frames that were **submitted** - the one window a handle cannot cover. Before
  this the handle borrowed both halves and a record survived a frame only because a frame body never runs beside a
  submission: an implicit mechanism no type stated. The pass' RECTANGLE is a pass fact
  too, so it is resolved once (`ContentDraw::DrawScope`) instead of in every draw call's argument list.
- **ONE WALKER FOR BOTH FRAME VERBS** (`FrameDriver::claimPasses`): "which passes can this device run, and which claims
  does each one take" was answered once for a recorded frame and written out again for a warm-up - and the second copy
  had already lost three of the four claims (the depth LENDER, a full-screen source, a declared input; §2.18). It is one
  walk with one rule, and `PassMode` states only what the walker does with its answer: a recorded frame acquires the
  window's image and counts what it ran, a warm-up does neither. The resolution is one function too
  (`resolveFrameHead`), so the warm-up's separate texture path is gone and a prepared texture is found resident by the
  frame that samples it (§2.21).
- **The frame's own state has an owner** (`FrameRecording`, the driver's): what it claimed, the depths it lends, the
  records it moved, the clock's sample buffer, its counters and its labels. Those were a dozen locals in one long
  function and, in part, stack containers rebuilt per frame; they are one value the frame's phases read and write,
  `clear()`ed per frame with its capacity kept.
- **A PASS ALSO OWNS WHAT IT READS** (`FrameHead::Pass::sources` / `shadow_map`, review 9 `K7`): the walk already
  CLAIMED every sampled target (a full-screen source, a declared input, a depth lender), but the RECORD was looked up
  again in the record loop - per pass and per source, under the target map's lock - so a release landing between the
  claim and that second call took the picture away from a frame that had claimed it (a full-screen copy refused with
  `ScreenNoSource`, a shadowed pass drawn unshadowed with no report), and the recording path took the one lock the
  head/record split exists to keep off it. `FrameDriver::resolveReadSet` resolves the records AFTER the whole walk - so a
  record a LATER pass builds, or a depth a LATER pass lends, is still seen - and the pass slot keeps them, which is the
  same "a frame owns what it names" rule the pass handle already followed (`N10`).
- **And so does the command buffer** (`FrameDriver::commands`): the command pool, the fence and the query pool were the
  driver's; the buffer was allocated and freed per frame, with a failure branch (`vk.frame.command_buffer_missing`) a
  host could do nothing about. It is created with the pool and reset by `vkBeginCommandBuffer`, and the warm-up records
  into it under the same exclusive gate - so both frame bodies spend no allocation on it.
- **Why**: `G1` (one concept, one definition, one owner), `G5` (the expected change - "the warm-up must prepare what the
  frame will run" - stays inside the walker rather than being re-derived in a second function) and `G6` (the frame
  machinery is listed in one object). Three indexings over one list is exactly the shape that lets a report name the
  wrong pass, and a per-frame allocate/free pair is the frame machinery's own cost paid outside the machine.
- **Boundary**: the PASS VALUE is a structure change with no behaviour - the 197 `test_vk` cases did not move,
  `FrameHead::storageGrows` still answers what it did, and the clock's per-pass spans are what F13 says they are. The
  read-set half is the one behaviour change here: it is stated as `N10` and pinned by its own case, and the rest of the
  suite was unchanged by it.

### 2.21 Content becomes resident when its fill is PROVEN (`G6` / `G10`)

- **What it is**: this backend fills content from two sources, and only one of them used to follow the rule. A geometry
  stream is written by the HOST into host-visible memory, so its bytes exist the moment they are copied - the fence
  decides who may RELY on them (`ContentCaches::FrameBuilds`, §2.17). A texture's image is filled by a COPY THE GPU
  records, so between "recorded" and "the fence reported" it is in `UNDEFINED` and no descriptor may claim it.
  `ContentImages` therefore keeps a recorded copy PENDING in the batch that recorded it and promotes it into the table in
  `batchCompleted`, which runs exactly where the fence was waited; `batchAbandoned` parks it instead, so a frame that
  never reached the queue leaves the LAST PROVEN revision in place. A batch is a DRIVER (`BatchId`), because a frame and
  its warm-up share one command buffer and one fence.
- **What that fixes**: `T66` - a failed submit used to leave a resident entry whose image nobody had filled, and the
  next frame found its revision, bound it and never uploaded it again. The same rule closes the half nobody had written
  down: another driver's frame head used to find that entry before its copy was even submitted.
- **Why**: `G6` (one resource-lifecycle rule for both kinds of content, stated once) + `G10` (a competitor that wants the
  texture meanwhile records its own copy, which is the bounded cost the alternative - waiting for another driver's frame -
  would charge every time).
- **Boundary**: the 1x1 stand-in is a SHARED CONSTANT, not an asset (§6.19 P19): a frame that finds it mid-fill binds it
  rather than dropping every draw that needs it, and the one thing not shared is its failure - a batch that never ran
  drops it, so the next frame makes a fresh one. `ContentImages::Progress::pending` / `pending_bytes` say what is
  recorded and not yet proven, and they are deliberately NOT part of `live`.
- **Cases**: `VkPrepareUploadTest.APrepareWhoseSubmitFailsPublishesNothingAndTheNextFrameUploadsAgain` (§8 T10).

### 2.22 A graphics pipeline states two stages, and the builder is where that is required (`G1` / `G4` / `G10`)

- **What it is**: `vkCreateGraphicsPipelines` needs a module for every stage a pipeline states - a vertex stage always,
  and a fragment stage unless rasterisation is discarded (this backend never discards it). A `ShaderProgram` is editable
  to ANY stage list (`clearStages`, a fragment-only file, a compute stage), so "the program is complete" is not a
  property of the program: `buildPipeline` asks for the two modules it is handed and refuses a pair with a hole, naming
  the stage, before the driver sees it (review 19 `Z16`).
- **Why the builder and not the program entry**: one description can be complete for one use and incomplete for another
  - a fragment-only program IS a whole full-screen program (that path states the engine's canonical vertex stage) and is
  no content program at all. A refusal remembered on the program entry would refuse the legal draw for the rest of the
  session, and a refusal per (program, need) would be two answers under one key. So the requirement is stated where the
  stages are stated to the API, and the two guards that used to sit in `ensureContentPipeline` /
  `ensureFullscreenPipeline` (two sentences, neither naming the stage) are gone.
- **Two failures, two codes**: the builder answers a `PipelineFailure`, so a hole in the program is reported as
  `vk.content.program_stage_missing` (the repair is the program's stage list) while a state the driver refused keeps
  `vk.content.pipeline_refused` / `vk.content.screen_pipeline_refused` (the repair is the state, and the loader's report
  says what). Reporting both under one code sent a host to the blend/depth state for a defect in the program (§2.14).
- **No stage is dropped silently**: a `Compute` stage used to be `continue`d past, so an entry that resolved NOTHING was
  cached as compiled and the draw was reported as a missing *graphics* stage. This backend stages no compute program,
  and that is the entry's own refusal now (`vk.content.program_refused`, a sentence that says so), cached like every
  other failure.
- **Boundary**: the rule is the GRAPHICS pipeline's. A compute pipeline (M2) states its own requirement, and the SDK keeps
  allowing any stage list - an empty program is a legal editing state (`clearStages()` then `addStage()`) and a value,
  not a claim about how it will be used (`graphics-rendering.md` F20).
- **Cost, stated**: the refusal lands after the geometry upload, because the pipeline is resolved last (program ->
  streams -> instance stream -> pipeline). That is one upload on the first frame that names such a program and nothing
  after it; moving the check earlier would state the rule in a second place, which is the defect it removes.
- **Cases**: `VkContentDrawTest.AProgramWithNoVertexStageIsRefusedAsAProgramFault`,
  `VkContentDrawTest.AFragmentOnlyProgramIsAWholeFullScreenProgram`,
  `VkContentDrawTest.AComputeStageIsRefusedRatherThanDropped` (§8 T12).

### 2.23 An upload is prepared, then recorded (`G3` / `G4` / `G10`)

- **What it was**: `ContentImages`' texture upload was ONE function that both allocated and recorded. Each layer's
  staging buffer was created inside the copy loop, so the first faces were already recorded when a later allocation
  failed - and the failure path then ran `destroyEntry`, an immediate `vkDestroyImage`, while the caller's command buffer
  held the barrier into `TRANSFER_DST` and the copies of the faces built so far. The frame went on to submit that
  command buffer, which names a destroyed image (review 19 `Z15`). `ensureFallback` had the same shape one step further
  along: its `vkCreateImageView` ran AFTER the barrier and the copy, so a refused view destroyed the image those
  commands named.
- **What it is**: two halves with different contracts. The **prepare** half builds every object the copy names - the
  image, its memory, its view, ONE host-visible staging buffer holding every layer and mip, and the region array that
  reads it - and may fail: it records no command, so its failure path destroys what it built and leaves the caller's
  command buffer exactly as it found it. The **record** half states `UNDEFINED -> TRANSFER_DST`, one
  `vkCmdCopyBufferToImage` carrying every region, `TRANSFER_DST -> SHADER_READ_ONLY`, hands the staging buffer to the
  batch, and ANSWERS NOTHING. The stand-in takes the same order (image, memory, view, 4-byte buffer, then the record
  verb), so there is one copy path rather than a second one for the 1x1.
- **Why**: `G3` (prepare -> record -> commit reads in one pass) + `G4` (the SIGNATURE carries the rule: the record half
  has no failure to report, so no caller can believe it may "undo" a recorded command) + `G10` (one allocation, one
  mapping and one copy command per upload instead of one per layer). The alternative the review offered - park the
  failed entry against the batch instead of destroying it - buys a correct destroy for a state that should not exist,
  and leaves a batch holding staging for a copy whose destination nobody publishes (`D13`).
- **Boundary**: the split is the rule for anything this backend records from host bytes; a new fill (an array texture, a
  compute writeback) is prepared first and recorded second. A destroy that follows a recorded command is a RELEASE
  through the batch's books (`batchAbandoned`) or the park queue (`releaseLater`), never an immediate `vkDestroy*` -
  which is what those books exist for (`P17`, `P19`).
- **What it costs** (`G10`): the staging is ONE contiguous range, so an upload's atomicity is "one free run of this
  size" instead of "N holes of this size", and on a fragmented host-visible heap a large upload can be refused where
  the per-layer shape would have found room in pieces. The bytes are unchanged and the pool states the evidence when it
  refuses (`MemoryStats::largest_free_run`, reported in the `vk.memory.out_of_memory` sentence), and the property bought
  in exchange is the one this point exists for: N separate allocations cannot be atomic, so the shape that can fail in
  pieces IS the shape whose failure lands mid-recording (`P21` keeps the recognition test).
- **A witness, not a comment**: the upload's barriers carry the frame's `RecordingTally` like every other frame-path
  barrier, so "a refused upload records nothing" is a number `Device::recordingNumbers()` answers. Before this the
  upload was the one recording path whose barriers no report counted.
- **Cases**: `VkResidencyTest.ARefusedTextureUploadRecordsNothingSoTheFrameNamesNoDestroyedImage` (§8 T15).

### 2.24 A frame answers for the picture it promised (`G1` / `G3` / `G4`)

- **What it was**: two answers a frame that did not run gave were not its own. A window frame whose submit or fence
  failed answered `Skipped`: `rewindUnrunFrame` gave the acquired image back (clearing `window_acquired`) before the
  present block read that same flag, so the `Lost` branch was UNREACHABLE - and the two consumers that act only on `Lost`
  (the engine's size re-announcement, the surface's lost-picture line) never ran for the one failure a host must act on
  (review 19 `Z19`). And a frame that did not complete answered with the PREVIOUS sampled frame's GPU time and per-pass
  costs: every `books.timing` reset sat inside `if (completed)`, so the failing path carried the last number forward
  (review 19 `Z20` - which `N7` already forbade, so the second was code behind its own contract).
- **What it is**: a frame keeps the two facts apart and answers from the right one. `FrameRecording::window_acquired` is
  the HOLD - "the presentation engine's image is still owed back" - and the one give-back act clears it
  (`FrameRecording::giveBackWindowImage`, which also replaced the copy of that act `claimPasses` kept).
  `FrameRecording::picture_owed` is the PROMISE - "this frame took the surface's image, so a picture is coming" - set
  with the acquire and cleared only by the next frame's `clear()`. The answer is `FrameRecording::unrunOutcome()`: `Lost`
  when the promise was made, `Skipped` when there was no surface or nothing to present. And the books OPEN DISENGAGED:
  `books.timing.reset()` sits at the frame's start, beside the frame's own state reset, so a read of THIS frame's pair is
  the only thing that can engage the device time.
- **Why**: `G1` (one name, one meaning - a bool that answered both "is the image held" and "was a picture promised" is
  what let the give-back erase the answer) + `G3` (the answer is computed once instead of re-derived at each failing
  exit) + `G4` (`unrunOutcome` names a QUESTION, so the three exits that do not present - the buffer that will not
  begin, the buffer that will not end, the submission the fence never confirmed - all read the same statement). The
  timing half is the same rule on a different field: an answer that only its own measurement can engage cannot inherit
  its predecessor's, whatever path the frame leaves by.
- **Boundary**: `Skipped` keeps two jobs, both "there was nothing to present" - a frame with no presenter at all (an
  off-screen target), and one whose walk claimed no pass: there the walk withdraws the promise when it gives the image
  back, because no picture was ever recorded and a swapchain rebuild cannot fix a refused pass (answering `Lost` would
  buy a device wait per frame for a content defect). A frame that failed BEFORE the acquire (`Refused`, a device that is
  not up, a lost device) never made a promise and keeps its own answer, and a frame that did run keeps the platform's
  answer from `WindowTarget::present` - a third fact, unchanged.
- **Cases**: `VkWindowPresentTest.AFrameWhoseSubmitIsRefusedAnswersLostAndTheNextOnePresents` (§8 T16) - the assertion
  used to read `Skipped`, which is the defect itself - and `VkDeviceClockTest.AFrameReportsItsDeviceTime` (§8 T16).
  Both mutations are recorded: answering from `window_acquired` again turns the first red, and putting the timing resets
  back inside `if (completed)` turns only the second's new assertion red.

### 2.25 An entry that holds its key ends when nobody else names the key (`G1` / `G6` / `G10`)

- **What it is**: the caches' four tables keep entries between frames, and every one of them holds the object its key is -
  an instance stream holds the `Buffer` of its address, a geometry entry the `GeometryDescription` of its shape, a program
  entry the `ShaderProgramDescription` (with the pipelines compiled for it), a texture entry the `Texture`. At a frame's end
  `ContentCaches::endFrame` ends every entry whose key has **no holder other than the entry itself**
  (`sweepUnheldStreams` / `sweepUnheldGeometries` / `sweepUnheldPrograms` / `ContentImages::sweepUnheldTextures`).
  `useCount()` is the whole probe: naming a value requires a share, so the last share outside the cache is exactly "nobody
  can ask for these bytes again". One withdrawal body per table serves both the host's announcement (`releaseContent`) and
  the sweep, so "when does this entry end" has one answer.
- **Why holdings and not a policy**: a time window picks a number nobody can defend, and re-deriving "is this content alive"
  from the scene graph would put a second answer beside the host's own reference count (`G1`). The pin ADR
  (`ADR-2026-11-10-gpu-residency-pinned-by-content`) already says residency follows live content; this is that sentence
  implemented rather than approximated.
- **Why the sweep had to become a rule, not a case**: a shape revision is a NEW IDENTITY (`Geometry::bumpRevision` builds the
  next description, `DescriptionCache` keeps one), so `releaseContent(Geometry)` can only ever name the CURRENT revision - a
  superseded one is unreachable from the host, and a DROPPED geometry has no object left to hand the verb at all. Ending them
  by holdings is the only spelling that a host does not have to participate in; without it, a host that regenerates a shape
  (the in-tree one is `FpsOverlay::writePattern`) keeps one entry's streams, the description and its own `Buffer<float>` per
  regeneration for the session's life.
- **What it deliberately does not end**: culling and hiding do not move a revision, so the geometry still holds its
  description and the entry stays (F27 / F28 - becoming visible again retransmits nothing). A frame that is still recording,
  a cached publication and a host-held command list all hold the description, so a sweep can never take what a submission may
  still name - it is a stronger guard than the entry-share the park uses, and the park still decides *when* the objects go.
- **Cost**: O(#resident entries) once per frame - one share count per entry and, for programs, a reused census vector (the
  table is keyed by `(description, variant)`, so one revision holds one share per variant). It is the same order the stream
  sweep already paid: a frame walks the entries that exist, never the content (§2.5's "holding is identity" argument).
- **Boundary**: the sweep ends entries, it does not decide residency POLICY. `releaseContent` still exists for a host under
  memory pressure that wants to keep its objects, and nothing here reintroduces a cap or a window (§2.15, `T27`).
- **Cases**: `VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced` (a kept command list holds the old shape, so the
  entry stays; dropped, the next frame's end releases it) and `VkResidencyTest.ContentTheHostNoLongerHoldsLeavesResidencyBehind`
  (a closed document's geometry, texture and pipeline leave while the open one's stay). Both are red with the sweep arms off,
  and each arm restores exactly its own assertion (review 21 §2.4).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Device lifecycle and loss | instance / device / queue / command pool / fence; loss ends the session | landed |
| F2 | Physical device selection | ordered by `VkPhysicalDeviceType`, ties keep the first seen | landed |
| F3 | Window and swap chain | surface, FIFO, one present semaphore per image, a 1 s acquire timeout, rebuild | landed (Linux/X11) |
| F4 | Offscreen targets | dynamic-rendering attachment sets, MRT, MSAA + depth resolve, borrowed depth, resize keeping the pipelines | landed |
| F5 | Content drawing | program → SPIR-V → pipeline (the pipeline states a vertex AND a fragment module, §2.22); state mapping; vertex / index / instance streams | landed |
| F6 | Fullscreen drawing | the screen ABI; depth forced to Disabled; binds the shadow map the pass resolved | landed |
| F7 | Textures and samplers | revision cache, the 1×1 stand-in, uploads before the rendering instance | landed |
| F8 | The constant ring | one persistently mapped buffer per frame, slices + dynamic offsets | landed |
| F9 | Descriptor-set cache | cached by (the view set, the shadow map); released entry by entry, swept periodically | landed |
| F10 | Device memory pool | blocks + slices, usage / kind, granularity, whole-block mapping | landed |
| F11 | Readback | colour (RGBA8) and depth (D32); its own pool and fence; one counted wait per readback | landed |
| F12 | Pipeline cache | a session `VkPipelineCache` + a device-keyed cross-session file | landed |
| F13 | Device clock | `FrameTiming` (sampled every 30 frames) | landed |
| F14 | Reports and diagnostics | `FrameReport`, `PrepareReport`, driver messages, `ReportOnce` | landed |
| F15 | The exposure boundary | zero `Vk*` in the shell; the `Handles` escape hatch; the gate script | landed |

### 3.1.1 F1 Device lifecycle and loss

- **Behaviour**: `initialize()` creates the instance, picks the physical device, creates the device / queue / command pool / fence, then the window surface and the
  content path; `shutdown()` releases in the order "content → targets → windows → pool → device → instance" and is idempotent.
- **Constraints**: `C1`, `C2`, `C6`.
- **Acceptance**: `T1` (the whole set), `BackendTest`'s initialise / shut-down round trip.
- **Caveat**: `P14` (the shut-down order: content → targets → windows → pool → device → instance).

### 3.1.2 F2 Physical device selection

- **Behaviour**: enumerate every physical device; FILTER each candidate by the session's floor (the Vulkan 1.3 version,
  the `synchronization2` / `dynamicRendering` features, and `VK_EXT_extended_dynamic_state3` with all three switches),
  then order the SURVIVORS by type (discrete 4 / integrated 3 / virtual 2 / cpu 1 / other 0), keeping the first seen on a
  tie; with a window, the family's present support is required as well. A device that misses a floor is not ranked at
  all, and when NO candidate serves the refusal names the floor the highest-ranked device missed - a fact about the
  machine, not about a device this backend picked (`DeviceChoice.hpp`'s `candidateRefusal`, review 9 `K3`; the peers
  filter the same way: `vk-bootstrap`'s `PhysicalDeviceSelector`, Diligent's adapter enumeration).
- **Constraints**: `C7`.
- **Acceptance**: `T1`; `VkDeviceChoiceTest` (the predicate driven with fabricated offers - two real GPUs are not in CI);
  the choice is readable through `handles()`.
- **Caveat**: `P10` (device identity is half of the cache key).

### 3.1.3 F3 Window and swap chain

- **Behaviour**: `WindowSystem` creates the surface (X11 / xcb); the swap chain is FIFO with `minImageCount + 1`, `currentTransform`, a composite-alpha fallback, and
  prefers BGRA8 / RGBA8 sRGB; **one present-wait semaphore per swap-chain image**; `vkAcquireNextImageKHR` has a 1 s timeout, and a timeout skips "this frame got no
  image" and reports once per episode; `OUT_OF_DATE` rebuilds and asks once more, while `SUBOPTIMAL` is an image WITH A REBUILD
  OWED - the meaning `present` gives it, so the frame draws and presents from the chain it has and the next acquire replaces it
  (review 20 `Z24`).
- **A failed rebuild is a state, not an ending** (`G1`, review 20 `Z23`): a target that was ATTACHED owes a chain, and
  `destroySwapchain` - the one function that removes one - raises `needs_recreate`, so an acquire that finds no chain builds it
  instead of refusing. THE FRAME WALK ASKS THE QUESTION EACH MODE NEEDS: a recorded frame asks for its IMAGE (the acquire, which
  is what repairs), a warm-up asks for the CHAIN (it acquires nothing, `P11`). Both used to ask `ready()` first, and because the
  rebuild destroys the old chain BEFORE it builds the new one, one failed rebuild - a zero extent, a refused
  `vkCreateSwapchainKHR`, a refused depth-image allocation - ended the window for the session.
  Case: `VkWindowPresentTest.ARebuildThatFailsIsRetriedByTheNextFrame` (the refusal injected through
  `Device::setMemoryBlockCreator`, the repair required of the next frame).
- **An unready presenter names its own cause** (`G4`, review 19 `Z14`): a window pass whose presenter exists but cannot present
  answers `vk.window.not_ready` with the window's own state (`WindowTarget::presentationState`: `Detached` / `NotAttached`, and
  `NoSwapchain` for a warm-up - a recorded frame asks the acquire, whose failure is `vk.window.acquire_failed` with the target's
  own sentence), while `vk.window.presenter_missing` is kept for "no presenter this device handed out" - the sentence used to
  accuse the host of a foreign presenter when the target's own rebuild had failed.
- **Constraints**: `C1`, `C2`, `R3`.
- **Acceptance**: `WindowPresentTest`, `ProbeTest`; `T6` (a real host).
- **Caveat**: `P13` (suspect the environment first when a window test goes red; the swap-chain semaphores are one per image and cannot be shared).

### 3.1.4 F4 Offscreen targets

- **Behaviour**: `TargetObjects` holds the colour / sample buffer / depth / depth-resolve images and views, and the layout each
  one is in; a pass opens a **dynamic-rendering instance** over them (`beginPass` / `endPass`, and `DynamicRendering`), stating
  ONE load/store answer PER ATTACHMENT (`AttachmentPolicy::color_ops` / `depth_op`; a stated op overrides the group spelling) - so the HUD case (keep the picture, own the depth inside its rectangle) is two fields of one
  `VkRenderingAttachmentInfo` instead of a fourth render pass. The WINDOW owns the same pair (`WindowTarget::beginPass`), with
  a PER-IMAGE layout record: a swapchain image is `UNDEFINED` on its first use, `PRESENT_SRC_KHR` after it was presented, and
  the colour layout between two passes of one frame. A PRESENTED COLOUR ATTACHMENT IS ALWAYS `Store`d: `beginPass` overrides a
  stated `DontCare` (the compositor reads that image), while the depth is not forced, because it is not presented. An MSAA target draws into the sample buffers and resolves colour with
  `AVERAGE` and depth with `SAMPLE_ZERO` in the attachment info (`resolveMode` / `resolveImageView`, core 1.3 - no
  `vkCreateRenderPass2`); a depth-only target is a shadow map; a `shareDepth` target attaches the lender's depth picture and
  ALWAYS loads AND stores it (`TargetObjects::beginPass` overrides both halves - another target's image may never be
  cleared or discarded), and its
  barrier is recorded against that LENDER's image as a same-layout barrier: the dependency, not a transition. The borrow
  edge is also a CLAIM: a frame takes the LENDER as well as the borrower, so two drivers cannot write one depth image (and
  its ONE record) at once - a contended lender skips the pass and reports `vk.target.claim_conflict`, exactly like a
  contended target. The depth
  ATTACHMENT and the depth RESOLVE PICTURE keep separate layout records, because sampling the picture does not move the sample
  buffer - and every other image has its own record too (`color_layouts` per picture, `multisample_color_layouts` per sample buffer), which
  is what lets a colour picture a full-screen draw sampled return to the attachment layout on its own.
- **A target it cannot build is named at BUILD time**: a target with no usable extent answers `vk.target.size_unknown` and a borrowed
  depth of another extent answers `vk.target.borrow_extent_mismatch` when the host announces it, so the reason arrives with the
  announcement rather than at the first resize (a capability, not a difference - review 8 `K6`).
- **Constraints**: `C5`, `C8`, `N2`.
- **Acceptance**: `MsaaTest`, `ContentTargetTest`, `OffscreenClearTest`, `DeferredTest`,
  `TargetClaimFrameTest.ABorrowedDepthIsTheLendersToHold` (the borrow edge is a claim) and
  `VkFrameRecordTest.ADepthOnlyClearKeepsTheDrawnPicture` (a depth-only clear keeps the picture) plus
  `VkWindowPresentTest.AWindowFrameIsDrawnAndPresented`'s frame loop, which forces a presented image to be acquired again and is
  therefore the witness for the per-image layout record.
- **Caveat**: `P4`, `P15` (dynamic rendering inserts no dependency between two passes into one attachment).

### 3.1.5 F5 Content drawing

- **Behaviour**: `ShaderProgramDescription` → glslang → SPIR-V → `VkShaderModule`; the pipeline key is the
  SEAM's `vn::graphics::PipelineKey` and NOTHING ELSE (kind, program identity + revision, the vertex layout slots,
  attachment-set compatibility, variant, topology class - see `ContentPipelineKey`); one `DrawItem` = one
  `vkCmdDrawIndexed`. Instance streams are cached by buffer address + revision, and the entry holds that buffer (§2.5). The pipeline states NO render pass: it carries
  the attachment FORMATS through `VkPipelineRenderingCreateInfo`, so its blend-array length and the rendering instance's colour
  count are the same number derived from one value.
- **EVERY state item is a command** (`T54`, `ADR-2026-11-23` D4): viewport, scissor, line width and the depth-bias
  values always were; cull mode, polygon mode, depth test/write/compare and blend enable/factors joined them when the
  floor rose to `VK_EXT_extended_dynamic_state` / `_state2` / `_state3` (REQUIRED - a device without them is refused
  at `initialize` with `vk.init.dynamic_state_missing`). The commands live in ONE recorder helper
  (`recordRasterState`), so a state change compiles nothing: `VkContentTargetTest` pins that a wireframe paints fewer
  pixels than the filled frame and creates no pipeline, and `VkContentDrawTest.AStateChangeIsACommandNotAPipeline`
  pins the same for blend - with the pixel as the proof the command ran. The one EDS3 switch this helper does NOT state
  is alpha-to-coverage (`vkCmdSetAlphaToCoverageEnableEXT`): stating it is one command, but coverage is derived from the
  fragment's first colour output, which the forward program fills with the drawable opacity - the feature needs a shader
  branch and one seam fact as well, and nothing draws Mask content into a multisampled target yet
  (`graphics-material.md` F7 N6, backlog `T58`).
- **The declared vertex input is the vertex stage's own set**: the geometry contributes channels, but the pipeline declares only the locations the compiled vertex
  stage reads (read back from the module's `OpVariable`/`Location` interface, one location per matrix column, so the instance stream keeps 12-15). A geometry channel the
  stage does not read is neither declared nor fetched - it would otherwise be a validation warning ("attribute at location N not consumed by vertex shader") and a
  pipeline nobody wrote. When the module cannot be read the answer is "declares everything", so narrowing only ever happens on proof (`G10`).
- **Constraints**: `C4`, `C7`, `N3`.
- **Acceptance**: `ContentDrawTest`, `VariantTest`, `VertexInputTest`, `BuiltinProgramTest`, `BarrierPlanTest`.
- **Caveat**: `P6` (the ring only grows between frames).

### 3.1.6 F6 Fullscreen drawing

- **Behaviour**: the vertex stage from `fullscreenVertexProgram()` + the caller's fragment stage; the source's colour attachments bind to set 0's 0..N-1, the shadow map
  and the light block to 7 / 8; `planScreen()` **forces** `DepthPolicy::Disabled`.
- **Constraints**: `C7`, `N3`.
- **Acceptance**: `ContentDrawTest`'s `AFullscreenCopyIsDrawnWhateverThePassDepthPolicySays`, `ShadowTest`'s
  `AFullscreenPassShadesWithTheMapItResolved`, `DeferredTest`.
- **Caveat**: `P1`, `P2`.

### 3.1.7 F7 Textures and samplers

- **Behaviour**: `ContentImages` caches the image / view by `Texture` address + revision; samplers are cached by `Sampler` value; a missing slot binds the 1×1 white
  stand-in and clears the material's slot; an upload (barrier + `vkCmdCopyBufferToImage`) may only be recorded **before** the render pass.
- **Constraints**: `C5`, `N1`.
- **Acceptance**: `ResidencyTest`, `ContentDrawTest`'s texture-sampling cases, `PrepareUploadTest`.
- **Caveat**: `P5`.

### 3.1.8 F8 The constant ring

- **Behaviour**: `beginFrame()` resets the cursor to zero; each draw takes one `sliceSize()` slice and the four blocks (material / lights / draw / shadow) are written at
  fixed offsets; every uniform binding is DYNAMIC and the offset travels with `vkCmdBindDescriptorSets`.
- **Constraints**: `C5`, `N4`.
- **Acceptance**: `VkUniformRingTest.ASteadyFrameCreatesNoRingAndBuildsNoSet`,
  `VkUniformRingTest.TwoDrawablesWithDifferentMaterialsShareTheRing`.
- **Caveat**: `P6`.

### 3.1.9 F9 Descriptor-set cache

- **Behaviour**: content sets are cached by (the five texture views + the shadow map), full-screen sets by (the source view + the shadow map), and the per-draw set is one per
  ring generation; the pool is created with `FREE_DESCRIPTOR_SET`, entries are released one by one, and one unused for 600 frames is swept every 120 frames.
- **Constraints**: `C5`, `N1`.
- **Acceptance**: `VkUniformRingTest.*`; the mutation that turns the cache off makes both cost-model cases red at once.
- **Caveat**: `N1` (only a new ring generation empties it).

### 3.1.10 F10 Device memory pool

- **Behaviour**: `MemoryAllocator` creates `VkDeviceMemory` blocks per memory type, resources hold a `MemorySlice{memory, offset, size, block}` and bind by offset;
  `MemoryUsage` decides the type (host-visible forces coherent), `MemoryKind` carries `bufferImageGranularity`; a release merges with the adjacent free run; a wholly
  empty block is destroyed when another block of the same kind exists; a host-visible block is mapped once as a whole.
- **The pool's byte account is the host's memory figure**: five numbers from `MemoryStats` (`used_bytes` / `reserved_bytes` / `peak_used_bytes` / `free_bytes` /
  `largest_free_run`) travel in `FrameReport::memory` (`DeviceMemory`) whenever the device is up, gated by `Features::Feature::MemoryUsage`; they are the pool's own
  bookkeeping - **not** the whole VRAM as the driver sees it, and **not** the frame's traffic (`FrameUploads`) - while the block / slice counters stay the snapshot's
  witness in `Device::memoryStats()`. A device that is down leaves the part disengaged rather than answering zeros.
- **Constraints**: `C6`, `N4`.
- **Acceptance**: `VkMemoryPoolTest.*` (6 targets = 12 slices ≤ 2 blocks; a steady-state frame allocates nothing; a readback gives its bytes back; shutdown reaches zero;
  `ThePoolsByteFiguresReachTheHostsFrameReport` - the five figures cross into `FrameReport::memory` and agree with `memoryStats()` on every one),
  `VkMemoryAllocatorTest.*` (adjacent merging; linear / optimal do not share a page).
- **Caveat**: `P7` (whether it can be mapped depends on the memory type; the pool opens before the first consumer of memory).

### 3.1.11 F11 Readback

- **Behaviour**: `ReadbackStaging` is one per call and **carries its own command pool and fence** (`C7`: readback borrows no session's frame machinery); a colour
  readback serves the attachment's own 8-bit format (one byte per channel), and a depth readback turns D16 / D32 / D32F into normalised floats; the answer is ONE VALUE -
  `ColorReadback` / `DepthReadback` (`ReadbackResult<T>`): the `ReadbackStatus` (`Ok` / `NotReady` / `Unsupported` / `Invalid` / `Failed`) together with
  the picture it classifies, `values` empty on every refusal (`BC-41`). Readback takes the body gate EXCLUSIVE (the device's "between frames")
  and **counts one** `deviceWaits()`; a refusal answerable before waiting (unknown / unbuilt target, out-of-range attachment, unsupported format,
  borrowed depth, no host-visible coherent memory type) does not stop the device; a read called back from inside a frame body answers `Failed` (`FrameBodyDepth`, never waiting for itself).
  Both verbs share ONE copy sequence (`Device::copyImageToHost`), so the barriers, the copy and the layout restore cannot drift between colour and depth (`BC-42`).
- **Which formats can be read is a classification, not a surprise**: pure depth formats (D16 / D32 / D32F, MSAA resolving its
  resolve image) are read, and D24 answers `Unsupported` through the SDK's `depthBytesPerTexel` (`Readback.hpp`) - "this device never will", the answer a
  host gates its feature on (review 8 `K6`). A BORROWED depth answers `Invalid`, because the request names an image this target does not have - the caller reads
  the SOURCE target. The depth format always comes from the record (`TargetShape::depth_format`), never the host's current `RenderTarget::depthFormat()`.
- **Constraints**: `C1`, `R3`, `R28`.
- **Phase one reads values, not records** (A3-01): a refusal answerable before waiting takes a snapshot through `TargetTable::peek` under the map lock (`built` / `fresh` /
  the engine shape); the record itself is rebuilt in place by `ensure` and handed to the park by `release` to be destroyed outside the table, so reading its fields again
  through `lookup` has no synchronisation to point at (TSan once reported a real race here). The answer may be **one pass old**: `TargetObjects::fresh` is written by
  the frame body on the recording path (`C3` / `C5` forbid taking the lock), so it is an atomic bit; phase one uses it as a hint and phase two re-checks inside the gate.
- **Acceptance**: `VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack`, `OffscreenClearTest`, `ContentTargetTest`, and `VkMultiSessionTest`'s three cases (interleaved
  readbacks each get their own last frame / a wait that counts / a refusal from inside a frame body).
- **Caveat**: `P9`, `P7` (readback is not in the frame budget).

### 3.1.12 F12 Pipeline cache

- **Behaviour**: one `VkPipelineCache` per session is passed to both `vkCreateGraphicsPipelines` sites; `shutdown()` writes the blob from `vkGetPipelineCacheData` into
  a device-keyed file (strictly validated, written atomically); the next `initialize()` loads it through `pInitialData`; when the driver refuses it, the plugin **retries
  once without the data**.
- **Constraints**: `C9`, `N5`.
- **Acceptance**: `VkPipelineCacheFileTest.*` (5 cases, no device needed), `VkPipelineCacheSessionTest.*` (4 cases).
- **Caveat**: `P9`, `P10`, `P11`.

### 3.1.13 F13 Device clock

- **Behaviour**: a pair of `VK_QUERY_TYPE_TIMESTAMP`s wraps a frame's command buffer, is read back after the fence and written into `FrameReport::timing`
  (`FrameTiming::gpu_micros`); **every pass has one boundary, written at `ALL_COMMANDS` after it** (so a pass' span is the time until ITS work finished,
  uploads and transitions included, and segment *i* is boundary *i* minus boundary *i-1*), read back into `FrameTiming::passes` (in execution order, named from
  `RenderPass::name`, so the host can hold it against the plan it read); it is sampled every 30 frames (measured, this pair costs ~0.8 ms per frame on lavapipe),
  and an unsampled frame is **absent** rather than repeating the previous value. The query pool holds `2 + passes` queries: the frame's start (the one boundary
  that takes no wait), its end, and one per pass.
- **Constraints**: `C7`, `C13`, `C14`.
- **Acceptance**: `VkDeviceClockTest.AFrameReportsItsDeviceTime`,
  `VkPassTimingTest.TheReportAttributesTheFrameToThePassesThatSpentIt` (two passes, two entries, the names match, and the sum of the segments does not exceed the
  whole frame) and `VkPassTimingTest.AHeavyPassReportsMoreTimeThanACheapOne` (a 512x512 clear reports more time than a 4x4 one - the property a boundary that
  does not wait cannot state).
- **Caveat**: `P12`.

### 3.1.14 F14 Reports and diagnostics

- **Behaviour**: `frameReport()` gathers one frame's counters, progress, uploads and timings, plus the pool's byte account (a now-fact, not a frame fact);
  `prepareFrame` answers a `PrepareReport`; driver messages and backend
  refusals take the same diagnostic route; every condition has one `ReportOnce`.
- **Frozen code spellings** (`BC-40`): the four `vk.content.screen_*` codes keep the word `screen` although the
  internal vocabulary says `Fullscreen` since `T57`. A code is what a HOST matches, so it keeps the spelling it
  shipped with; only the enumerator (`ContentRefusalCause::Fullscreen*`) is internal and renamable.
- **Two window codes, two facts** (`BC-40`): `vk.window.presenter_missing` means exactly "the pass named no presenter
  this device handed out"; an unready presenter has `vk.window.not_ready` (F3), so a host that matches a code is not
  sent to the wrong repair (review 19 `Z14`).
- **Constraints**: `R3`, `R4`.
- **Acceptance**: `BackendTest`, `ContentDrawTest`'s diagnostic-count assertions.
- **Caveat**: `P8` (a report field must not live in an object that gets destroyed).

### 3.1.15 F15 The exposure boundary

- **Behaviour**: `vkHandles()` in `support/Handles.hpp` hands out `Handles` (instance / physical / device / queue / family / validation); `Device::handles()` answers
  the same value inside the plugin; the frame path (`src/internal/`) does not include `support/`.
- **Constraints**: `C4`.
- **Acceptance**: `test_vk`'s `vk_exposure` (inside ctest).
- **Caveat**: none — the gate itself is the guard against drift.

## 4. Public Interface

### 4.1 `Device` (the plugin's internal face)

| Interface | Contract | Notes |
|---|---|---|
| `initialize()` / `shutdown()` / `initialized()` | idempotent; `initialized()` is false after a loss | §2.2 |
| `executeFrame(frame, present_to)` | one submission + one wait; `Presented` / `Skipped` / `Lost` | §2.3 |
| `prepareFrame(frame, present_to)` | records content and transfers only, draws nothing and presents nothing; may submit one transfer-only batch | ADR-2026-11-14-prepare-may-submit-transfers |
| `readColorImage` / `readDepthImage` | readable as soon as it is drawn (guaranteed by `C1`) | F11 |
| `setRenderTarget` / `releaseRenderTarget` | targets are held by the device; a release gives the slices back | F4 / F10 |
| `releaseContent(...)` | leaves a hole by identity and fills it next frame | §2.5 |
| `createSurfaceTarget(handle, why)` | one live target per window | F3 |
| `capabilities()` | reports only what is actually enabled | §2.8 |
| `handles()` | the escape hatch's value (inside the plugin) | F15 |
| `contentPipelines()` / `contentCacheCounters()` | the cost model's witnesses (pipeline count, ring / set) | §2.4 |
| `recordingNumbers()` / `debugMarkers()` | the recording path's witnesses (dependencies, barriers, state commands, labels) and whether this session can label at all | §2.19 |
| `memoryStats()` / `pipelineCacheReport()` | the pool's and the cache's witnesses; the pool's five byte figures also reach the host in `FrameReport::memory` | F10 / F12 |
| `setMemoryBlockCreator(...)` / `refuseNextSubmit()` | the two TEST DOORS: make an allocation, or the next queue submission, answer a failure the failure contracts are about (§6.16 / §6.19) | §2.12 / §2.21 |

The full signatures and per-parameter contracts are in `include/vine/backend_vk/internal/Device.hpp`.

### 4.2 `BackendFactory` / `Plugin`

| Interface | Contract | Notes |
|---|---|---|
| `BackendFactory::create(desc)` | builds the device (lazily); an empty `desc.cache_directory` takes the default directory | §2.9 |
| `BackendFactory::setDefaultCacheDirectory` | set by `Plugin::load` from `PluginLoadContext::ensureDataDirectory` | §2.9 |
| `Plugin::load` / `unload` | registers / unregisters the factory; loading takes the plugin's data directory | `shell/Plugin.cpp` |

### 4.3 The internal units

| Unit | Responsibility | Key contract |
|---|---|---|
| `Device` | session, frames, the window table, reports (the target table is `TargetTable`) | 4.1 above |
| `ContentDraw` | content / fullscreen recording, programs and pipelines, the ring, the set cache, the pipeline cache object | F5 / F6 / F8 / F9 / F12; the state shadow and `stateCommands()` (the recording path's witness) are its; `DrawScope` is what one pass records with (§2.20) |
| `ContentImages` | texture revisions, samplers, the stand-in, the per-driver upload batch and its two outcomes (`batchCompleted` / `batchAbandoned`) | F7 / §2.21 |
| `PassTarget` | WHERE one pass draws: an off-screen set or the window's acquired image, as one value with the facts and the verbs a pass needs | §2.20 |
| `TargetTable` | host target → object (map + shape + build / rebuild / repair) + the per-frame lease + release / park + the wait-free value snapshot (`peek`) | F4 / §2.18 |
| `TargetObjects` | an offscreen target's attachment set: images, views and the layout each one is in (`beginPass` / `endPass`) | F4 |
| `DynamicRendering` | one dynamic-rendering instance: the attachment transitions, `vkCmdBeginRendering` / `vkCmdEndRendering`, the resolve fields, and `resolveAttachmentOps` (the per-attachment load/store rule, one place) | F4 / §2.7 |
| `WindowTarget` / `WindowSystem` | the swap chain and the surface (per-image layout record, `beginPass` / `endPass`) | F3 |
| `MemoryAllocator` | the device memory pool (blocks / slices / mapping) | F10 |
| `PipelineCacheFile` | the cache file format, validation, the atomic write | F12 |
| `ReadbackStaging` | one readback's staging sequence | F11 |
| `BarrierPlan.hpp` / `StateMapping` | `stateOf` / `barrierBetween` / `loadOpOf` / `storeOpOf` / the clock's boundary stages; `recordBarriers` and `BarrierBatch` state a pass' moves as ONE dependency | §2.7 / §2.19 |
| `ShaderCompiler.hpp` | `compileStage` + `SpirvModule` (glslang); the variant's define names (the variant VALUE itself is the SDK's `ProgramVariant`) | F5 |
| `Handles` (`internal/` + `support/`) | the escape hatch | F15 |
| `ContentAbi.hpp` | the content block layout this backend packs (offsets are the SDK's ABI) | F5, `graphics-material.md` |
| `SampleCounts.hpp` | which sample counts this device builds, and the refusal when one is asked for | F4, `ADR-2026-11-14` |
| `NumberText.hpp` | the number spelling every refusal uses, so a report's digits read the same everywhere | §2.13 |
| `backend_vk_global.hpp` | the export macro and the namespace the plugin is built under | F15 |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

- **C1 One frame in flight**: the queue is idle when `executeFrame` returns; readback, release and ring growth all rest on that (§2.3).
- **C2 A submission is a wait**: every submission has its paired fence wait; `prepareFrame`'s transfer batch is the only exception, and it waits for itself.
- **C3 The plan does not change during recording**: `applyPlan` refuses while a frame is recording (the SDK's `PlanResult::Busy`); this backend caches no "this will change next frame" state.
- **C4 Boundary**: `Vk*` never appears in `shell/`; `internal/` is not depended on back by `support/`; the frame path does not use `vkHandles()`.
- **C5 Growth happens between frames**: the ring, pool blocks and pipeline-cache object are replaced or grown only while no command buffer references them.
- **C6 Binding offsets**: every `vkBind*Memory` offset satisfies the resource's alignment, and slices of different `MemoryKind`s do not share one
  `bufferImageGranularity` page.
- **C7 Capability = actual**: every bit of `capabilities()` comes from a feature / limit actually enabled, not from "the device may support it".
- **C8 Two trees**: Debug and Release both build and both run `test_vk`; Release's device cases are the same set as Debug's.
- **C9 The cache trusts the same device only**: the cache file's key must match the current device; a mismatch is refused, never "used as far as possible".
- **C10 The low-level units do not depend on the seam**: `MemoryAllocator`, `PipelineCacheFile`, `WindowSystem`, `ReadbackStaging` and `Handles` must not
  `#include <vine/graphics/...>`; `check_vk_exposure.py`'s E2 checks it (a new unit is added to the list explicitly, which is exactly the step that decides which half
  it belongs to).
- **C11 Allocation failure is classified in one place**: `MemoryAllocator` reports facts only (`MemoryRefusal` + `VkResult` + numbers), `Device::noteMemoryRefusal` is the
  only classification point, and no call site may classify a failure as "out of video memory" on its own (§2.12).
- **C12 Every report carries a stable code**: a newly written `reportDiagnostic` must carry a `code` (`check_diagnostic_formats.py` sets a ceiling with the baseline and
  goes red above it); once published, a `code` is never reworded or reused (`scripts/diagnostic_codes.txt`).
- **C13 Attribution is segmented, not the whole frame repeated**: a pass' time is the segment between its boundary and the previous one (including the uploads and
  transitions it asks for), so the segments sum to ≤ the whole frame; attributing the whole frame to every pass is wrong (`VkPassTimingTest` pins it).
- **C14 A boundary is a wait**: a timestamp answers "when did everything before it finish", so the frame's START takes no wait (`kFrameClockStartStage`, nothing  before it belongs to the frame) and every other boundary - each pass' end and the frame's end - waits for `ALL_COMMANDS` (`kPassClockBoundaryStage`). A boundary
  that does not wait measures when the command processor ARRIVED, not what the pass spent (see `P18`).
- **C15 A graphics pipeline states no stage without a module**: no `VkShaderModule` handed to
  `vkCreateGraphicsPipelines` is `VK_NULL_HANDLE` - `buildPipeline` refuses the pair and names the stage that is absent,
  and the refusal is reported as a program fault, never as a state the driver refused (§2.22).
- **C16 An upload is prepared before it is recorded**: a command that names an object is recorded only after every
  object it names exists. The prepare half may fail and answers what it built (no command names it, so it dies at
  once); the record half answers nothing at all. Nothing a recorded command names is destroyed before its submission -
  such an object is released through the batch's books or the park queue (§2.23).
- **C17 A frame's answer describes that frame**: a frame that owed the surface a picture and did not present answers
  `Lost` (`FrameRecording::unrunOutcome`), never `Skipped` - the give-back returns the image, not the promise; and a
  frame that did not measure its device time answers it ABSENT, never the previous frame's number (§2.24, `N7`).

### 5.2 Requirements (R#)

- **R1** The validation layer is silent: neither `test_vk` nor a real host produces a VUID - measured 2026-11-27 with
  `VK_LAYER_KHRONOS_validation` 1.4.341: `test_vk` 237/237 and **0** driver messages (warning or error) across the suite.
- **R2** The rendered region is read back and compared exactly: a frame's pixels are the ones the recorded commands promise (`Probe` cases), not "something was drawn".
- **R3** One report per episode: every "cannot" has one `ReportOnce`, and when the condition ends `rearm` happens where it ended.
- **R4** The witnesses are readable numbers: every conclusion about the cost model and the caches has a number on `Device`, and a test reads it instead of counting
  driver calls.
- **R5** Every gate green: `check_ai_docs --strict`, `check_design_docs`, `check_cross_refs`, `check_doc_symbols`, `check_include_hygiene`, `check_api_style`,
  `check_public_docs`, `check_export_annotations`, `check_vk_exposure`, `check_diagnostic_formats`.
- **R6** Mutations are load-bearing: every guard has a record that "remove it and the matching case goes red".

### 5.3 Invariants (N#)

- **N1** A descriptor set is emptied only when the ring changes generation; a steady frame builds and destroys nothing.
- **N2** A resize with an unchanged shape recompiles no pipeline (a pipeline is keyed by the shape alone and states no extent).
- **N3** A frame's command buffer is reset / released only after its `fence` has signalled; nothing is destroyed while it is still referenced.
- **N4** Pool slices: the ranges inside one block are ordered and alternate live / free; a release merges with its two neighbours only.
- **N5** A cache file is either read back whole (magic / version / key / length all match) or refused; there is no half parse.
- **N6** After a device loss `initialized()` is false, frames are skipped and one report is made — **`VK_ERROR_DEVICE_LOST` on the allocation path takes the same route as
  on the submission path** (`noteDeviceLost`, §2.12).
- **N7** A frame's `FrameReport.timing` either belongs to that frame or is absent; the previous frame's value is never
  reused - **by construction**: the books open disengaged at the frame's start and only a read of that frame's own
  timestamp pair engages them, so no exit has to remember to clear it (`VkDeviceClockTest.AFrameReportsItsDeviceTime`
  drives a refused submit between two frames; review 19 `Z20`, §2.24).
- **N8 One pass, one dependency**: a pass' entry states every attachment barrier in one `VkDependencyInfo`, so
  `RecordingNumbers::barrier_dependencies` grows by one per pass entry while `barriers` grows by the attachment count
  (`VkContentDrawTest.APassEntryIsOneDependencyWhateverItWrites`).
- **N9 A state command is issued only when the state changes**: within one frame, a draw that repeats the previous
  draw's pipeline, rectangle and raster state issues no state command
  (`VkContentDrawTest.ARepeatedStateIsNotARepeatedCommand`), and a draw that only changes the PIPELINE issues exactly one
  - the bind - because every pipeline declares the same dynamic set
  (`TwoAlternatingPipelinesRestateNoState`).
- **N10 A frame owns what it reads**: a pass' sampled sources and its shadow map are resolved where the frame claims the
  pass, and the pass slot keeps them - so a release announced after the claim parks the record instead of emptying the
  frame's answer, and the record loop of `Device::executeFrame` takes no lock
  (`VkMultiSessionTest.AReleaseBetweenTheClaimAndTheRecordStillDrawsTheSampledSource`,
  `AFrameKeepsTheSourceItReadsUntilTheFrameEnds`).
- **N11 A table entry holds its key, and ends with it**: every cache table whose key is an object holds that object, because
  naming an entry must be impossible once nobody else can name what it describes - an instance stream holds the `Buffer` of its
  address, a geometry entry the shape it describes, a program entry its revision (with the pipelines compiled for it), a
  texture entry its texture. The entry ends either on the host's announcement (`releaseContent`) or **at a frame's end once
  the key has no other holder** (§2.25; the rule used to be implemented for the instance stream alone, which is `review 19`'s
  `Z13` half). The property both arms protect is the same one: an entry is never left keyed by an address or an identity the
  caller may hand to a later object, and nothing resident outlives the content that asked for it
  (`VkBuiltinProgramTest.AnInstanceStreamHoldsItsBufferAndEndsWhenNobodyElseDoes`,
  `ARecycledAddressDoesNotInheritThePreviousInstanceBytes`, `VkResidencyTest.ASupersededShapeLeavesTheResidencyItReplaced`,
  `ContentTheHostNoLongerHoldsLeavesResidencyBehind`). What the sweep does NOT cover: a geometry whose shape moved still
  uploads every channel of the new revision, changed or not (`ContentCaches`' streams are per entry, not shared by
  `StreamKey`; backlog `T74`), and a sampler cached by VALUE has no holder to ask (`Z26`, review 21 §4).

## 6. Caveats and Traps

### 6.1 P1 A fullscreen call must have depth Disabled

- How it fails: the fullscreen triangle sits at z = 0 (the Reverse-Z far plane); inheriting the pass' `TestAndWrite` discards the whole block with **zero VUIDs**.
- How to detect it: the recorder says `recorded=1` per call but the picture is the clear colour; `readColorImage` equals the background everywhere.
- How to prevent it: `planFullscreen()` forces `DepthPolicy::Disabled`; the case `AFullscreenCopyIsDrawnWhateverThePassDepthPolicySays` pins it with a fixture of "default pass
  state + a target with depth" (the old fixture set Disabled explicitly, which is exactly why it did not catch this).

### 6.2 P2 The full-screen set must bind the shadow map the pass resolved

- How it fails: binding 7/8 existing ≠ binding the right map; a white stand-in + a zero block makes the deferred path "shaded in forward, unshaded in deferred", with no
  diagnostic.
- How to detect it: the forward / deferred pictures differ; `ShadowTest`'s sampling case reads a depth of 255 (the stand-in) instead of the map's value.
- How to prevent it: `recordFullscreen` takes a `ShadowSource` and binds the real depth + a per-call `VineShadowBlock` when there is a map, falling back to the stand-in +
  `params.x = 0` only when there is none.

### 6.3 P3 A plan row is unique by `(owner, name)`

- How it fails: declarations with the same name overwrite each other and only the last survives ("only the fourth preview shows").
- How to detect it: duplicate names in the frame table; `VN_FRAME_PLAN=1` shows them directly.
- How to prevent it: previews / declarations use unique names; after changing an object it must be **declared again** (the plan reads declarations, not objects).

### 6.4 P4 Depth is sampleable in three shapes only

- How it fails: MSAA depth, borrowed depth and a fresh target's depth can all be mistaken for a "sampleable image"; a borrowing target does not own the image itself.
- How to detect it: validation reports a layout / usage violation; or the picture is black.
- How to prevent it: `depthPicture()` / `depthPictureView()` are the only entry points; a borrowing target answers `VK_NULL_HANDLE` and the read goes to the lender.

### 6.5 P5 An upload may only be recorded before the rendering instance

- How it fails: `vkCmdCopyBufferToImage` and some barriers are illegal inside a rendering instance (and `vkCmdBeginRendering`)
  may not be followed by one).
- How to detect it: validation reports it directly; or the texture stays in TRANSFER_DST for ever.
- How to prevent it: the head records every upload before the first rendering instance begins (`resolveTextures`, called from `resolveFrameHead`); `ContentImages`' "not prepared" gives an explicit `why`.

### 6.6 P6 The ring grows only between frames

- How it fails: destroying the ring inside a frame steps on a buffer a recorded command buffer already names.
- How to detect it: validation's "destroyed while in use"; or a crash.
- How to prevent it: a shortage inside a frame records `demand` and skips that draw (reporting once), and `beginFrame` grows it.

### 6.7 P7 Whether the pool can map is a property of the memory type

- How to fail: deciding mappability by "why the block was originally created"; on a device where device-local and host-visible share one type (lavapipe, ReBAR) a legal
  host-visible slice is refused and the device does not start (reporting only "no Vulkan device").
- How to detect it: `ensureRing` fails; `map()` returns null.
- How to prevent it: `map()` checks `memoryTypes[type].propertyFlags & HOST_VISIBLE`; the pool must open before **the first consumer of memory** (a window swap chain's
  depth image is created at `attach`).

### 6.8 P8 A cache report needs a snapshot, not a destroyed object

- How it fails: a report field lives in `ContentDraw`, and `shutdown()` calls `content.destroy()` before reading it — everything turns 0.
- How to detect it: the report reads 0 after shutdown although the file was written.
- How to prevent it: `Device::Impl::CacheBooks` snapshots after `create()`; only cumulative counters (`cache_hits`) are read live while the session is up.

### 6.9 P9 A driver may validate nothing at all

- How to fail: lavapipe advertises `VK_EXT_pipeline_creation_feedback` yet **never reports a hit** and does **not validate** `pInitialData` (a blob with the vendor changed
  is accepted). Reading the driver's silence as "the cache is not working" is a misjudgement.
- How to detect it: `hits_measured == true` while `cache_hits` stays 0.
- How to prevent it: the backend declares only the half it can prove — `preload_handed` (the blob really reached `vkCreatePipelineCache`); hits are reported but not
  demanded; the case pins the hand-over, not the driver's silence.

### 6.10 P10 A cache file's key must belong to the device

- How it fails: handing another driver's / device's cache to `vkCreatePipelineCache` is **undefined behaviour**, not just slower.
- How to detect it: the symptoms are random (a driver-internal crash, wrong pipelines).
- How to prevent it: `PipelineCacheFile`'s key (vendor / device / driver / api / `pipelineCacheUUID`) + a strict read; a mismatched key is refused outright.

### 6.11 P11 The shutdown path must be able to write

- How it fails: a process killed by `SIGKILL` / `SIGTERM` never runs `shutdown()`, so the cache file never appears.
- How to detect it: a host run under `timeout` never has a cache file, while a test does.
- How to prevent it: "write the cache" lives in `shutdown()` and runs before the cache object is destroyed; a test uses the session semantics of an explicit `shutdown()`.

### 6.12 P12 The clock is not free

- How it fails: one timestamp pair per frame costs ~0.8 ms per frame on a software rasteriser (8.30 off / 9.01–9.20 on, three runs each).
- How to detect it: the steady-state median moves up noticeably once the clock is on.
- How to prevent it: sample every 30 frames; an unsampled frame answers absent.

### 6.13 P13 When a window test goes red, suspect the environment first

- How it fails: a leftover window covers the test window ⇒ X11 pixel reads return someone else's picture; several window tests in parallel fight over X.
- How to detect it: a single case run on its own is green; `read-error 8` (`BadMatch`).
- How to prevent it: `pgrep` for leftover processes first; run GUI tests serially; the last `[ RUN ]` line is not the crash point (gtest fully buffers its stdout when
  redirected — use `stdbuf -oL`).

### 6.14 P14 The shut-down order: a resource goes before what it borrows

- How it fails: `vkDestroyDevice` first and a resource's destructor afterwards — the destructor uses a stale `VkDevice` and device table, and the crash lands somewhere
  that looks unrelated; or `allocator.close()` before releasing a target, so returning slices writes into a freed block.
- How to detect it: a crash at shutdown / validation's "destroyed while in use"; a report field turning 0 after shutdown (another form of P8).
- How to prevent it: `shutdown()` has a fixed order — `vkDeviceWaitIdle` → write the cache → content → targets → windows → **pool** → command pool / fence / query
  pool → device → instance; every step is idempotent and a failure in one does not skip the rest.

### 6.15 P15 Dynamic rendering inserts no dependency, and states no layout

- How it fails: a render pass moved every attachment from `initialLayout` to the subpass layout and ordered the pass' accesses
  after what an earlier submission did. `vkCmdBeginRendering` does NEITHER: an image whose layout differs from the one its
  `VkRenderingAttachmentInfo` states is an invalid command (`VUID-vkCmdBeginRendering-pRenderingInfo-09592`), and two passes
  into one attachment have no execution dependency between them unless the recorder writes one.
- How to detect it: validation names 09592 at the pass' first draw (MEASURED: skipping `beginRendering`'s transitions turns
  seventeen `test_vk` cases red, each pointing at that VUID); a second pass that LOADs what the first wrote reads stale data
  with no message at all on a driver that does not hazard-check.
- How to prevent it: `DynamicRendering::beginRendering` records one barrier per attachment before the instance - including one
  between two identical states, which IS the dependency - and the LAYOUT it starts from is the record of THAT image
  (`TargetObjects::color_layouts` / `multisample_color_layouts` / `depth_layout` / `depth_picture_layout`, `WindowTarget`'s
  per-image record), written by whichever command moved it. A new caller must state its layouts instead of assuming a render pass
  would move them, and a new MOVER must write the record of the image it moved.
- Two IMAGES are not one layout: a multisampled target's depth ATTACHMENT and the picture it resolves into part company as soon
  as the picture is sampled - the sample buffer stays an attachment (it may not be sampled at all) and the picture moves to
  `SHADER_READ_ONLY_OPTIMAL`. One record for both states a layout an image never had. How to detect: a second frame over the same
  multisampled map reports `VUID-VkImageMemoryBarrier2-oldLayout-01211` (MEASURED; `VkShadowTest.AMultisampledMapDrawnAgainStatesBothDepthImages`
  is red without the split). AND ONE IMAGE CAN BE BOTH: on a single-sampled set the attachment is the picture, so a recorder
  that says "the attachment is never sampled" drops the SHADER_READ_ONLY -> depth attachment barrier, and the next frame reports
  `VUID-vkCmdBeginRendering-pRenderingInfo-09588` (MEASURED on the application's `open_demo`, 2026-11-24;
  `VkShadowTest.ASingleSampledMapDrawnAgainStatesItsDepthAgain` is red without it). The COLOUR HALF is the same rule on the other
  attachment: a picture a full-screen draw sampled must return to the attachment layout before the next pass writes it, and answering
  "colour attachment" for every picture reports `VUID-vkCmdBeginRendering-pRenderingInfo-09592` plus `vkCmdDraw-None-09600`
  (MEASURED; `VkDeferredTest.TheDeferredFrameIsDrawnAgainAfterItsGbufferWasSampled` is red, at one sample and at the device's
  maximum). One record per image is what makes both halves unrepresentable rather than fixed.
- A BORROWED depth is a dependency, not an exception: `shareDepth`'s image belongs to the lender, its layout does not move, and
  the barrier must still be recorded - it orders the lender's write before the borrower's LOAD and depth test. NOT measured: an
  ordered implementation shows no pixel difference and this environment's validation does not report the missing hazard, so the
  rule is pinned by `BarrierPlanTest.ASameStateBarrierStillCarriesTheDependency` (the table) and by the comment at the call
  site, not by a device case.

### 6.16 P16 An allocation failure classified across kinds

- How it fails: treating any `vkAllocateMemory` failure as "out of video memory" — a session whose device is lost keeps running, the host is told "the target cannot be
  built", it goes off to release content (uselessly), and it retries on a dead device until the process ends.
- How to detect it: `test_vk`'s `VkMemoryFailureTest.ALostDeviceDuringAnAllocationEndsTheSession` (inject `VK_ERROR_DEVICE_LOST`: `initialized()` must turn false) and
  `VkMemoryAllocatorTest.ARefusedBlockSaysWhichKindOfFailureItIs` (the four kinds are four values, and the driver's raw result travels with the report).
- How to prevent it: classification lives in one place (§2.12) and the pool reports facts only; `MemoryBlockCreator` is the failure path's injection point —
  `vkAllocateMemory` does not fail on a healthy machine, and without it these cases cannot be written.

### 6.17 P17 A frame that never reached the queue moved nothing
- How it fails: the layout records are written WHILE RECORDING (that is what lets the next pass state the right
  `oldLayout`), on the premise of `C1` - one submission per frame, one fence wait. A frame that dies between its first
  `beginPass` and its `vkQueueSubmit` (a command buffer that will not end, a submit the driver refuses) has advanced
  records for commands the GPU never executed, so the next frame states layouts no image was ever in (validation's
  `oldLayout` mismatch, or a picture nobody wrote read as if it had been). The window half has a second form: an ACQUIRED
  swapchain image was taken and its `image_available` semaphore signalled, and a frame that never presented leaves both
  behind - the next acquire may not even hand that signalled semaphore to the driver again.
- How to detect it: `vkQueueSubmit` does not fail on a healthy machine, so the witness is the code path itself -
  `Device::FrameDriver::executeFrame` rewinds on EVERY return after the first pass was recorded
  (`rewindUnrunFrame()`), and gives the acquired image back on the paths that fail after the acquire
  (`giveBackWindowImage()`). The memory-failure injection (`MemoryBlockCreator`) is how to reach it on purpose.
- How to prevent it: `TargetObjects::resetLayouts()` puts THIS frame's own attachment sets back to `Undefined` (content the
  GPU never wrote, so the next pass clears rather than loads); a **rewind log** (`MovedLayout`) puts a sampled source's or a
  shadow map's record back to the EXACT value it had, because those images may hold a picture an earlier frame wrote and
  nothing can reproduce (a wholesale reset there would discard a baked target); and `WindowTarget::abandonFrame()` rebuilds
  the swapchain, which returns the acquired image and starts every window record at `Undefined`. Only what THIS frame moved
  is touched, so another driver's targets are never rewritten.
- NOW COVERED, including the texture half: a recorded texture copy is PENDING in its batch and is promoted only by
  `ContentImages::batchCompleted` (the fence was waited) or parked by `batchAbandoned` - so a frame that never reached
  the queue publishes no image nobody filled (§2.21, §6.19).

### 6.18 P18 A boundary that does not wait measures arrival

- How it fails: a per-pass clock boundary written at `TOP_OF_PIPE` takes its timestamp when the command processor REACHES it, so a pass' span is queue latency
  rather than the pass' work. Two passes with the same command count then report the same near-zero time whatever each one drew, and a host optimising "the slowest
  pass" optimises nothing. The first version of F13 had exactly this shape, and the case that existed could not tell: its only quantitative assertion is "the
  segments do not exceed the frame", which a report of zeros satisfies.
- How to detect it: `VkPassTimingTest.AHeavyPassReportsMoreTimeThanACheapOne` - a 512x512 clear must report more time than a 4x4 one once the frame is long enough to
  divide. **NOT distinguishable on a software rasteriser**, which executes commands in order and makes the old stage look right; the mutation that reverts the
  boundary to `TOP_OF_PIPE` leaves every timing case green here. That is why the STAGE is pinned as a value:
  `BarrierPlanTest.APassClockBoundaryWaitsForEveryCommand` asserts `kPassClockBoundaryStage == ALL_COMMANDS` and
  `kFrameClockStartStage == TOP_OF_PIPE`, so the meaning does not depend on one device's execution model.
- How to prevent it: the two stages are named constants in `BarrierPlan.hpp` (the table that owns stage vocabulary), and no call site spells a stage of its own.

### 6.19 P19 A copy that never ran must not become resident

- How it fails: an image published at RECORD time is resident while still `UNDEFINED`. A later frame finds its revision,
  binds it (its descriptor names `SHADER_READ_ONLY_OPTIMAL`) and samples an image the driver never wrote: black pixels, a
  layout VUID, and no re-upload, because the revision matches. The second form is quieter: with two drivers, the other
  one's frame head finds the entry before its copy was even submitted.
- How to detect it: `ContentImages::Progress::live` must not move until the batch's fence was waited, and
  `Progress::pending` must be back to 0 once the verb that recorded it returned. A refusal the device honours
  (`Device::refuseNextSubmit`, a test door) makes the failure reachable: the case then asserts that the NEXT frame
  uploads the texture itself and draws its colour, instead of binding the entry the failed batch left behind.
- How to prevent it: publish in `batchCompleted`, abandon in `batchAbandoned`, and keep the pending entry in the batch's
  own book (`ContentImages`). A HOST-FILLED stream is the other kind and keeps the `FrameBuilds` rule (§2.17): its bytes
  exist immediately, so only the claim/publish half applies to it.

### 6.20 P20 A program is a value, not a verdict

- How it fails: a program is editable to any stage list, so a description that is complete for a full-screen pass is
  incomplete for a content draw - and a refusal remembered on the PROGRAM then refuses the legal use too, for the rest of
  the session (the shape review 19 `Z16` proposed, and the one this design rejects).
- How to detect it: a full-screen draw whose fragment-only program compiles fine draws nothing, and the diagnostic says
  the program is broken rather than the use.
- How to prevent it: the requirement lives in `buildPipeline` (the pipeline that states the stages), the refusal is per
  DRAW with `vk.content.program_stage_missing`, and nothing about stage completeness is cached on `ProgramEntry`
  (`VkContentDrawTest.AFragmentOnlyProgramIsAWholeFullScreenProgram`).

### 6.21 P21 A recorded command is a lifetime, not a step

- How it fails: an image destroyed while the command buffer that copies into it is still to be submitted. The call is
  legal to the API - the image is no longer in use by a SUBMITTED command buffer - and invalid to the submission: this
  is `VUID-vkDestroyImage-image-01000`, and a driver that does not check reads freed memory when the barrier and the
  copies execute. The form review 19 found: a staging allocation inside the recording loop, so the failure came AFTER
  the transition had been recorded, and the failure path destroyed the image that transition named.
- How to detect it: NOT by the validation layer, measured - with `VK_LAYER_KHRONOS_validation` 1.4.341 installed the
  `Z15` shape produces no message at all, because the destroy happens before the submission and the layer's in-use
  check sees only submitted references. The ordering is pinned by numbers instead: `Device::recordingNumbers()` across a
  frame whose upload is refused must spend exactly what a frame with no upload spends
  (`VkResidencyTest.ARefusedTextureUploadRecordsNothingSoTheFrameNamesNoDestroyedImage`).
- How to prevent it: the two halves of §2.23 - prepare (fallible, records nothing) then record (records, fails nowhere) -
  and the rule that a destroy after a recorded command is a RELEASE through the batch's books or the park queue, never
  an immediate `vkDestroy*`. The failure has to be injected to be reached at all (`vkAllocateMemory` does not fail on a
  healthy machine): `Device::setMemoryBlockCreator` plus a texture whose staging exceeds the first host-visible block
  while its image still fits the first device-local one refuses the staging and nothing else.
- **This rule's own cost, which is not a second defect**: staging is one contiguous range now (§2.23), so a large upload
  needs one free run of its size - on a fragmented host-visible heap it is refused where per-layer requests would each
  have found a hole. The refusal says which case it was: `vk.memory.out_of_memory` names the bytes asked for and, when
  the pool still holds free bytes, "N free (largest run M)" beside the reserved/in-use totals - so "a large upload
  refused while small ones succeed" is this trade, and "nothing free at all" is a real shortage.
- **And the retry is per frame, which this path does not remember**: a refused entry was never published, so nothing is
  left behind - but `ContentImages::viewFor` has no "this revision was refused" fact (the instance stream's `Failed`
  revision, §2.5, is the shape that has one), so a texture that cannot be uploaded is attempted again on every frame it
  is bound: the image, its device-local memory and its view are built and destroyed again, plus one created-then-destroyed
  `VkBuffer` and the refused `vkAllocateMemory` - all of it per frame and per texture. The episode rule keeps the report
  to one line, so the cost is the device calls, not the log.

### 6.22 P22 A give-back must not erase the answer

- How it fails: the acquisition is two facts stated in one flag. `window_acquired` answers "the presentation engine's
  image is still owed back" and, in the same bool, "this frame promised the surface a picture". A failure path MUST give
  the image back, so it clears the flag - and the answer that asks "did this frame promise a picture" then reads false
  on exactly the paths it exists for. Nothing crashes and no VUID fires: the frame answers `Skipped`, the host reads it
  as "nothing to present", and neither the size re-announcement nor the lost-picture line runs for a picture that really
  was lost (review 19 `Z19`).
- How to detect it: a real window frame whose submit is refused (`Device::refuseNextSubmit`) must answer `Lost`, and
  `VkWindowPresentTest.AFrameWhoseSubmitIsRefusedAnswersLostAndTheNextOnePresents` read `Skipped` for as long as the
  defect lived. The shape to watch for is a comment that states the intended answer directly above an assertion pinning
  the actual one; reading the branch shows nothing, because the branch is well formed and UNREACHABLE.
- How to prevent it: the fact that must survive an act is a field beside the one the act clears (`picture_owed` /
  `window_acquired`), the answer is one function every non-presenting exit calls (`unrunOutcome`), and the give-back is
  one method on the value that owns both facts - so no call site can return the image while dropping the promise.

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | using `VMA` for memory | `G8`: today's consumers (targets, textures, streams, the ring, staging) are covered by the hand-written pool; a new dependency has to be paid for by someone | — |
| D2 | one `vkAllocateMemory` per resource | `G10`: allocations grow with the scene, and `maxMemoryAllocationCount`'s failure shape is "it looks like out of video memory" | — (changed, see F10) |
| D3 | compiling SPIR-V inside the plugin (a backend-local `ShaderCompiler`) | `G2`: that would point a dependency at a plugin that is not the compiler's owner; glslang is supplied centrally by `cmake/VineGlslang.cmake` | — |
| D4 | synchronization2 / dynamic rendering "later" | `G8` said the target was API 1.2; the 1.3 floor and the retirement's ordered batch made both the floor move and the legacy path go - D4 is LANDED as steps 1/1b/`T11` (2026-11-23) | `../decisions/ADR-2026-11-23-retiring-the-reference-backend.md` |
| D5 | putting the cache file format into the SDK | `G1`: the SDK does not know Vulkan; the right seam is "the host gives the location, the backend the format" (§2.9) | — |
| D6 | implementing several frames in flight directly | `G4`: changing the "read when drawn" contract needs a decision document first; the measured ceiling is about 10% (the device is 90%) | `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` |
| D7 | splitting the device and the session objects right away | `G8`: the contract is one frame in flight, one at a time, and a split would only add a layer of forwarding; because concurrent frames are clearly wanted it lands in **two steps** (the gate + the contract first, then the split) rather than in one go | `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` |
| D8 | an attachment-set base class for `TargetObjects` / `WindowTarget` | `G10` / `G12`: a vtable in a value type, and one header that would have to know both; a `PassTarget` VALUE (owner pointer + four answers) gives the frame body one shape without either cost (§2.20) | — |
| D9 | giving `Buffer` a unique id, or a weak handle, so an entry can outlive its key's address | `G10` / `G1`: a weak handle needs a control block beside `RefCounted`'s in-object counter - a redesign of the primitive every holder uses, for one table - and an id neither ends the entry nor answers the multi-session case (two caches would hold it for ever) | `../decisions/ADR-2026-11-09-address-keys-hold-the-object.md` (option B, rejected there) / `ADR-2026-11-27-a-held-entry-is-withdrawn-at-the-frames-end.md` |
| D10 | ending an unheld entry by a TTL, an LRU or a byte cap | `G1`: the SDK states the opposite rule (`graphics-content-lifecycle.md` 2.3: no cap, no LRU, no pressure eviction), and "the host stopped holding it" is already observable exactly - the cache's share is the last one | `ADR-2026-11-27-a-held-entry-is-withdrawn-at-the-frames-end.md` |
| D11 | retrying a refused instance upload every frame (the report saved by the episode rule) | `G11` / `G10`: the message was deduplicated but the allocation, the copy and the refusal were not - a broken stream would ask the pool for a block every frame. `Z13` closed it with the flag the other two fills already carry | — |
| D12 | reporting an unready presenter as `vk.window.presenter_missing` | `G4`: a code is an identity a host matches, and the second condition is not the first fact - the window's own state was never said. `Z14` split the codes and made the window answer `presentationState()` | — |
| D13 | PARKING a texture entry whose upload was refused, instead of removing the failure from the recording phase | `G1` / `G3`: the interleaving was the defect. Parking keeps a reader of `recordUpload` believing it may fail halfway, and leaves the batch's staging describing a copy whose destination nobody publishes - two answers for "what this batch recorded". One staging buffer per upload plus a prepare/record split removes the state instead (§2.23) | `../decisions/ADR-2026-11-27-an-upload-is-prepared-then-recorded.md` |
| D14 | answering `Lost` from a local captured at the failure site, or resetting `books.timing` on the failure path | `G1` / `G3`: a local in `executeFrame` is not reachable from `claimPasses`, which is where the walk gives the image back, so the two answers would drift apart again; and "reset the device time where the frame failed" is one more exit to remember - the exit that was forgotten IS `Z20`. One field beside the hold, one answer function, and one reset before any exit can return (§2.24) | — |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | the whole behaviour | `build/bin/test_vk` (the whole suite — 237 cases / 47 suites on 2026-11-27); `ctest -R test_vk` |
| T2 | the rendered region is exactly what the commands promise | the `Probe` cases in `test_vk` (`VkContentDrawTest`, `VkContentTargetTest`, `VkShadowTest` compare read-back pixels) |
| T3 | the exposure boundary and the seam-free low level | `ctest -R vk_exposure` (V2 / V4 / V5 / V6 / E1 / E2, `scripts/check_vk_exposure.py`) |
| T4 | the cost model does not regress | `VkUniformRingTest.*`, `VkMemoryPoolTest.ASteadyFrameAllocatesNothing` (turning the cache off must go red) |
| T5 | validation is silent | in those runs `diagnosticCount(InitFailed)` is 0, and with the layer installed the whole suite produces **0** driver messages (warning or error) - measured 2026-11-27, `VK_LAYER_KHRONOS_validation` 1.4.341 |
| T6 | a real host | `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=48 ./Vine --open demo`: median ≈ 8.2 ms, 11 passes / 10 draws, no `vk:` diagnostic |
| T7 | documentation and gates | `python3 scripts/check_ai_docs.py --strict` + §5.2 R5's scripts |
| T8 | mutations are load-bearing | recorded in `../memory/graphics.md`: the ring / set, pool and cache-file mutations each turn only their own cases red |
| T9 | readback | `OffscreenClearTest`, `ContentTargetTest`, `VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack` |
| T10 | content is resident only when its fill is proven | `VkPrepareUploadTest.APrepareWhoseSubmitFailsPublishesNothingAndTheNextFrameUploadsAgain` (a refused submit through `Device::refuseNextSubmit`; the mutation that publishes at record time turns it red - §2.21, §6.19) |
| T11 | a table entry holds its key and ends when nobody else holds it | `VkBuiltinProgramTest.AnInstanceStreamHoldsItsBufferAndEndsWhenNobodyElseDoes` (the cache's share shows in `useCount`; the stream leaves at a frame end with no announcement) and `ARecycledAddressDoesNotInheritThePreviousInstanceBytes` (the picture is the fresh buffer's; under the old rule the previous matrices were drawn at the previous position - verified by reverting the share and the sweep) |
| T12 | a pipeline never states a stage without a module, and an incomplete program is reported as a program fault (never as a state the driver refused) | `VkContentDrawTest.AProgramWithNoVertexStageIsRefusedAsAProgramFault` (the code, the sentence naming the stage, the clear picture), `AComputeStageIsRefusedRatherThanDropped`, `AFragmentOnlyProgramIsAWholeFullScreenProgram` (the same program is complete for a full-screen pass - §2.22) |
| T13 | a refused fill is not retried | `VkResidenceWireTest.ARefusedInstanceUploadIsRememberedInsteadOfRetriedEveryFrame` (with block creation refused, the second frame does not reach `vkAllocateMemory`, and a new revision retries) |
| T14 | an unready presenter names its own cause | `VkWindowPresentTest.ADetachedPresenterAnswersItsOwnReasonInsteadOfAccusingTheHost` (`presentationState() == Detached`; the frame reports `vk.window.not_ready`, never `presenter_missing`) |
| T15 | an upload records nothing when it is refused, and everything it names outlives the recording | `VkResidencyTest.ARefusedTextureUploadRecordsNothingSoTheFrameNamesNoDestroyedImage` (a staging block refused through `Device::setMemoryBlockCreator`: the frame spends exactly what an upload-free frame spends - `Device::recordingNumbers()` - the picture is the stand-in, and the frame after it uploads and samples the texture; the mutation that records the transition before the allocation turns those numbers red - §2.23, §6.21) |
| T16 | a frame that did not run answers for itself: `Lost` when it owed the surface a picture, and no device time | `VkWindowPresentTest.AFrameWhoseSubmitIsRefusedAnswersLostAndTheNextOnePresents` (a refused submit through `Device::refuseNextSubmit` on a real window: `Lost`, the image given back, and the NEXT frame presents - the mutation that answers from `window_acquired` again turns it red) and `VkDeviceClockTest.AFrameReportsItsDeviceTime` (a refused submit between two frames answers DISENGAGED, not the sampled frame's number; the mutation that puts the timing resets back inside `if (completed)` turns it red - §2.24, §6.22, `N7`) |

## 9. Status and Evolution

### 9.1 Current status

- Audit items F1 / F2 / F4 / F7 / F8 / F9 / F10 / F12 are landed (F9 / F10 as of the 1.3 floor and `T11`); F5 / F11 / F13 are kept as
  follow-up per §1.3. Several drivers are entirely landed, with the shape in §2.11 and `graphics-sessions.md`; **today's three TSan
  readings are in `graphics-threading.md` R18.**
- `test_vk` **237 cases / 47 suites**, `test_graphics` **710 / 87** (2026-11-27, `--gtest_list_tests`). The target map, the shapes and the leases are one
  `TargetTable` (`TargetTableTest`); the frame head has a storage witness, and the light vector is not one copy per pass.
- **One build lesson**: after a change to an SDK vtable the **dynamic plugin must be rebuilt too** — an old `.so` loads, then fails inside the first virtual call,
  and only a rebuild tells that apart from a regression (backlog `T65`, `ADR-2026-11-23` D5).
- **The validation layer runs here since 2026-11-27** (`vulkan-validationlayers` 1.4.341, installed user-locally under
  `~/.local/share/vulkan`): `validation=on`, `test_vk` 237/237 with the `VK_EXT_debug_utils` case no longer skipping, and
  **0 driver messages** (warning or error) across the suite - which is `R1` / `T5` measured rather than asserted. What it
  found was a FIXTURE defect, not a backend one: `VkShadowTest`'s map passes built their `PassDraw` without
  `PassDraw::program_override`, so a depth-only pass drew the content's colour-writing program (23 layer warnings, one
  per draw). The product states the override (`ChainBuilder::buildShadowPass`); the fixture now does too, through one
  helper (`ShadowTest::useDepthProgram`), and the case that collects driver messages asserts none arrive instead of
  tolerating warnings.
- Measured (lavapipe, a real host): 11 passes / 10 draws, steady-state median **8.24 ms**, GPU **7.15–7.74 ms**; the ring + set cache change is −10% (9.03–9.23 →
  8.16–8.30 ms).
- **Host allocations, measured once and kept apart** (2026-11-18, Debug + lavapipe, `operator new` counted for one measurement): the backend's
  `executeFrame` asks for **85 allocations / 10.6 KB per steady frame with one draw packet** and **133 / 12.4 KB with four** - stable frame to frame, so it is churn,
  not growth; that is ≈**16 per further draw packet** and **69 fixed** (mostly small vectors: the pipeline key's channel/binding/attribute lists and the plan's
  per-draw scratch). The recorder's two per-command VERTEX-BIND vectors were removed afterwards (review 9 `K6`: they are the recorder's, allocated once), so the
  per-packet share is now an upper bound - the figure is kept as measured and dated rather than restated without a run. The **whole chain frame** (engine + backend, the demo's deferred path: 10 passes / 10 draws with shadows, sky and HUD) is ≈**1966 allocations /
  ≈2.6 MB per frame** (`VN_BENCH_FRAMES` 8 vs 120 on one build, 2026-11-18). The two numbers answer different questions and are not interchangeable: the smaller one is
  the backend's own recording window, the larger one is what a host's frame costs. GPU-side builds and uploads are **0** in steady state (`VkResidencyTest`), so none
  of this is device memory.

### 9.2 Known gaps / backlog

- **Audit F5 (several frames in flight)**: **decided to keep one frame in flight** (`../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`), with the trigger
  conditions written in the ADR (device time under half the wall clock, an artefact shape that must overlap the GPU, or a second backend needing the same semantics).
- **Audit F9 / F10**: LANDED. `vkCmdPipelineBarrier2` (step 1b, `BC-38`) and dynamic rendering (`T11`, `BC-39`) are the only
  spellings now - no `VkRenderPass`, no `VkFramebuffer`, no 1.0 `vkCmdPipelineBarrier` or `vkCmdBeginRenderPass` in the
  backend. The 1.3 floor with the `synchronization2` and `dynamicRendering` features is the device requirement.
- **Backlog `T35` (becoming the default backend)**'s gate (`../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`): `releaseRenderTarget` really frees the
  attachments, `readDepthImage` and the device-loss report are landed; `releasePass` is a documented default no-op (`RenderDevice.hpp`), and the user page is
  `docs/guides/graphics-backends.md`; the Win32 branch is implemented and its presented pixels are asserted there (`../memory/graphics.md` 2026-11-23: `test_vk`
  174/174, no skips, the window cases reading the compositor); **still missing** are the Wayland answer (T13: serve it, or
  record "a Wayland-only host runs off-screen" and pin it with a case that needs no X - see 1.3) and a performance reading
  the gate accepts.
- **Backlog `T12` / `T13`**: `T12` landed 2026-11-23 (the Win32 pixel read through `PrintWindow`); `T13` is the Wayland
  question - there is no Wayland branch in this backend (`instanceExtension()` is xcb on Linux), so what a Wayland-only
  host gets is the off-screen session (1.3), measured 2026-11-28.
- **`T60` (the retirement) landed 2026-11-23**: this backend is the one the retirement leaves in place. The floor is Vulkan 1.3 with `synchronization2` +
  `dynamicRendering`, the pipeline key has no state half, and the backend has no render-pass or framebuffer object left; the retired plugin's codes and the comparison
  documents went with it ([ADR-2026-11-23](../decisions/ADR-2026-11-23-retiring-the-reference-backend.md)).
- **Review 9's remaining findings are backlog items**: `T67` - content STREAMS still live in host-visible memory while images and targets are device-local
  (`K1`; the trigger is a measurement on a device whose memory types differ, which a software rasteriser cannot be); `T68` - the shader-variant RULE has no SDK
  owner until a second API family starts (`K2`); `T69` - the descriptor-set cache's ceiling is combinatorial and its trigger is unstated (`K8`). `T17`'s text now
  names the cross-driver stall a cold build causes under the device-wide cache lock (`K5`), and `K4`'s one build-admission vocabulary rides with that batch.

### 9.3 Compatibility / migration

- **SDK**: `DeviceDesc` gains `cache_directory` (it has a default, so the old two-argument aggregate initialisation is unchanged).
- **Plugin ABI**: only `RenderBackend` / `RenderBackendFactory` face the host; the internal headers (`internal/`) promise no stability.
- **Residency, not ABI**: a host that drops its last reference to a `Buffer` without announcing a release now sees that stream leave
  residency at the next frame end (§2.5), where it previously stayed for the session. Nothing about the API changed, and a host that
  keeps the buffer and calls `releaseContent` sees exactly what it saw before.
- **Cache file**: format version `1`; a mismatched version is refused (no migration, no guessing).
- **Texture uploads are internal**: `ContentImages::viewFor` / `ensureFallback` and `ContentCaches::resolveTextures` take
  an optional `RecordingTally*` so a refused upload is observable as a number (§2.23). The headers are `internal/`, which
  promises no stability; nothing a host sees changed, and the sentence a refused staging reports is the one it reported
  before.
- **Codes**: `vk.content.program_stage_missing` is new. No code was renamed or reused: a program whose stage list cannot
  fill a graphics pipeline keeps its own cause, and the pipeline codes keep meaning "the driver refused this state".
- **Device requirements**: a Vulkan **1.3** device (step 2 of `T60`'s batch, `ADR-2026-11-23` D4) with the
  `synchronization2` and `dynamicRendering` FEATURES enabled - a 1.3 device that does not offer them is refused with
  `vk.init.device_features_missing`, and an older one with `vk.init.api_version_low` - PLUS
  `VK_EXT_extended_dynamic_state3` with the polygon-mode and colour-blend features (`T54`); a device without that
  extension is refused with `vk.init.dynamic_state_missing`. Multisample depth resolve is one attachment field of dynamic
  rendering (the 1.3 floor guarantees it), and creation feedback is core 1.3, so neither needs an extension or a version
  check any more.
- **Platforms**: Linux/X11 and Win32 both have a `WindowSystem` implementation and both are measured with a pixel instrument; Wayland is implemented against the WSI
  contract only.

## 10. References

### 10.1 Code

- `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/**`: every contract (`Device`, `ContentDraw`, `TargetObjects`, `ContentImages`, `WindowTarget`,
  `MemoryAllocator`, `PipelineCacheFile`, `ReadbackStaging`, `BarrierPlan`, `DynamicRendering`, `StateMapping`, `Handles`).
- `src/plugins/gfx_backend_vk/src/shell/**`: `Plugin`, `BackendFactory`.
- `tests/test_vk/**`: behaviour; `tests/test_vk/FrameRecordTest.cpp`: the five frame-shape facts the retired parity
  suite had no successor for (the other ten already had a `test_vk` case; see `ADR-2026-11-23` D4 step 4).
- `scripts/check_vk_exposure.py`: the exposure-boundary gate.

### 10.2 Documents and ADRs

- `graphics-architecture.md`, `graphics-rendering.md`, `graphics-frame-pipeline.md`, `graphics-content-lifecycle.md`: the SDK's concepts and one frame.
- `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`: the gate for becoming the default backend.
- `../decisions/ADR-2026-11-14-prepare-may-submit-transfers.md`: why `prepareFrame` may submit one transfer batch.
- `../decisions/ADR-2026-11-14-sample-count-refused-not-lowered.md`: sample counts are "refused, not lowered".
- `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`: residency is pinned by content.
- `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`: the exposure boundary (V1–V4) and the switch gate.
- `../decisions/ADR-2026-11-23-retiring-the-reference-backend.md`: the seam's promise rule and the retirement's terms
  (which is what makes this backend the only implementation).

### 10.3 External material

- the Vulkan specification: *Memory Allocation* (`bufferImageGranularity`, `maxMemoryAllocationCount`), *Synchronization and Cache Control* (the barrier rules inside
  and outside a render pass), *Pipeline Cache* (`pInitialData` must come from the same implementation), *Pipelines* (`VkPipelineCreationFeedback`).
- the Vulkan Validation Layers' VUID text: this backend's "zero VUIDs" goal is checked against it.
- the Vulkan Memory Allocator's design notes (a reference for "blocks + suballocation + alignment + free lists" only; not taken as a dependency).
