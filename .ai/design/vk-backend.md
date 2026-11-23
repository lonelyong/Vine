# the vk backend: a render backend straight onto Vulkan

> **Status**: current
> **Last verified**: 2026-11-23
> **Scope**: the `gfx_backend_vk` plugin (registered as `"vk"`) — a **straight-to-Vulkan** implementation of `RenderBackend`: device and queues,
> targets and windows, content drawing, the device memory pool, the pipeline cache, reports and diagnostics. It does **not** cover the SDK seam itself (see `graphics-*`)
> or `gfx_backend_vsg` (see `vsg-backend.md`).
> **Sources**: `src/plugins/gfx_backend_vk/include/vine/backend_vk/**` (the contract), `tests/test_vk/**` (behaviour),
> `scripts/check_vk_exposure.py` (the exposure boundary), `scripts/measure_graphics_claims.py` (re-derives the case / suite counts quoted here)
> **Backlog**: `../backlog/graphics.md`
>
> **Numbering**: this page's `F#` / `C#` / `R#` / `N#` / `P#` / `D#` / `T#` are **this module's** stable ids; an audit finding is written
> "audit `F#`" and a backlog entry "backlog `T#`" — the three sets never stand in for each other.

## 1. Overview

### 1.1 Problem and background

- The SDK's shared mechanisms (`vine/graphics/`) need a **second implementation** to be judged: with one backend alone, "the seam requires this" and
  "vsg happens to do this" cannot be told apart. This plugin is that second implementation, and its path is **straight onto Vulkan** (its own `VkInstance` /
  `VkDevice` / pipelines), without VulkanSceneGraph.
- It consumes the seam, not the engine: what to draw comes from `FrameDescription`, how to draw it from this backend. That is what lets `test_vk` drive it directly
  (the plugin is a MODULE and the test compiles its sources) **with no window**.
- Status: every milestone is landed; among the industry-standard audit's findings (F1–F13, 2026-11-14) the resource and
  cost-model items (audit F1 / F2 / F4 / F7 / F8 / F12) are landed. Behaviour is pinned by `test_vk` **178 cases / 42 suites** (2026-11-23), and the comparison against vsg by
  `BackendParityTest` (15 cases, inside `test_vsg`).

### 1.2 Goals

1. **Specification compliance**: the validation layer is silent (0 VUIDs); every Vulkan call's failure shape is classified, not "it does not draw".
2. **Comparable**: the same frame renders **byte for byte identically** on vk and vsg — the evidence that the seam is implemented rather than a coincidence.
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
- **Real-hardware evidence for Wayland** (backlog T13): both platform branches are implemented and both are measured on hardware - X11 through `XGetImage` and
  Win32 through a `PrintWindow` compositor read, one shared instrument for `WindowPresentTest` (2026-11-23). Wayland is implemented against the WSI contract only.
- **Becoming the default backend**: the gate is in `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`. **This backend is the one the application names
  today** — `app_shell`'s `chooseRenderBackend()` states `"vk"` (`VN_RENDER_BACKEND` overrides it) — and that is a host decision, not a verdict from the switch gate:
  the registry has **no** default to switch (an unknown name answers null, and several registered with none stated is a refusal that lists them —
  `appfw-render-surface.md` 2.4 and its `D7`). What the gate still wants is a measured Wayland fallback and a performance reading it
  accepts (backlog T35), so Windows does not ship before them. Retiring `vsg` is a separate trigger-gated item (backlog T60), not this gate.

### 1.4 One-paragraph conclusion

**A session = one device + one memory pool + one pipeline cache; a frame = one command buffer + one submission + one fence wait; and every capability, every refusal
and every cache has a witness a test can read.**

## 2. Design Points

### 2.1 The two halves: the shell exposes only the seam, Vulkan lives only in internal

- **What it is**: `shell/` (`Plugin`, `BackendFactory`) names `RenderBackend` / `RenderBackendFactory` only; `internal/` is where Vulkan lives; `support/Handles.hpp` is
  the only door that hands `Vk*` out (to tests / tools).
- **Why**: `G1` (each concept in its place) + `G2` (dependencies point one way: `internal` → SDK → `base`, and `shell` does not depend back on Vulkan types). It also makes
  "replace vk with another implementation" a plugin swap rather than an SDK change.
- **Boundary**: `scripts/check_vk_exposure.py` is the gate (V2 the SDK's backend face, V4 the plugin shell, E1 the frame path, E2 the seam-free low-level units) and it
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
  `releaseContent` only empties the matching streams (leaving a hole) and the next frame fills the holes only.
- **Why**: `G5` (the cache key is identity, release leaves a hole, no traversal to compare) + `G10` (one release, one upload, not one rebuild).
- **Boundary**: the key holds an `intrusive_ptr`, so an address is never reused by a later object; that is the premise that lets identity be a key.

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
  it resolves into carry separate records (`depth_from` / `depth_picture_from`), because `depth_sampled` moves only the
  picture - a multisampled image may not be sampled at all, so stating the picture's layout for the sample buffer is an illegal
  barrier (`VUID-VkImageMemoryBarrier2-oldLayout-01211`, measured). A `shareDepth` target states the LENDER's picture and the
  depth attachment layout it is in; the barrier that follows transitions nothing and is recorded for exactly that reason - it is
  the execution dependency between the lender's pass and the borrower's LOAD.
- **Boundary**: the table covers the purposes this backend produces (Undefined / ColorAttachment / DepthAttachment / ShaderReadOnly / TransferSrc / TransferDst / PresentSrc);
  a new purpose is one more row, not one more inline barrier. An ATTACHMENT row states the read side of its access as well as
  the write side, because under dynamic rendering the LOAD (and a blend's destination read) has no implicit ordering.

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

- **What it is**: `test_vk` compiles the plugin's sources and drives the device with no window; `BackendParityTest` (inside `test_vsg`) hands the same frame to both
  backends and compares the rendered region **byte for byte**; key guards are proven load-bearing by **mutation batteries** (turn it off and the matching case must go red).
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
  their driver by `ContentImages::BatchId`, and a replaced image parks to the watermark. Sampled targets (a screen source / a pass input) also take a target lease.
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
  `RenderDiagnostic`. vsg's readback refusals (the `HostReadbackRefusal` enum) produce the same type: a refusal has one shape only (ADR-2026-11-16 §4).
- **Why**: this path used to be two out-parameters, `(vn::String* why, DiagnosticCategory* category)` — fill one and forget the other, and of the seven reasons for
  "skip one draw" only one `ContentSkipped` reached the host. Putting the code on **the side that knows the reason** is what first lets the host and the cases say "this
  draw has no program" rather than "some draw was skipped" (`R28`'s landing point).
- **Boundary**: `refuse()` is the only writer and **first-writer-wins** — a second reason ("no geometry, so no pipeline either") does not overwrite the first, a rule that
  used to be repeated three times as `if (why.empty())` at the call sites. An empty sentence with a code is still a refusal: whether a sentence exists was never the
  criterion. `ContentRefusal` is an **internal header** (`internal/ContentRefusal.hpp`) and does not enter the SDK; the shape itself lives in the SDK's backend half.
- **The reason is an enum and the code is one `switch` with no `default`** (`ContentRefusalCause` / `contentRefusalCode`): `refuse`'s call sites used to write string
  literals directly, so a new cause could compile without a code (D-22). Now adding an enum value fails to compile without its `case` (copying vsg's
  `HostReadbackRefusal` mapping); `VkContentRefusalTest.EveryCauseNamesAStableCode…` additionally checks that every value has a code and that no two share one.
- **The two halves of the gate**: a report call that **constructs** a diagnostic (≥3 arguments) must write a code; one that **forwards** a `RenderDiagnostic`
  (`reportDiagnostic(diagnostic)`) does not count — the code travels inside the value and a sink cannot drop it. `check_diagnostic_formats.py` checks the registry both
  ways at once: what is used must be registered, and what is registered must be used by some builder / code table / report.

### 2.15 Release is handed to the queue, not destroyed at once (`G4` / `G10`)

- **What it is**: `releaseContent` / `releaseRenderTarget` **forget the cache entry on the spot** (the residency count drops immediately and the next frame uploads again),
  while the Vulkan object behind it goes to `graphics/RetirementQueue.hpp` and is destroyed only once the device's completion watermark has passed that frame. The
  completion evidence comes from `graphics/DeviceClock.hpp`: this backend registers **one** clock window (one frame in flight, `slots = 1`).
- **Why**: release has to say two things — "is this content still wanted" and "may this object be destroyed now" — and putting them together is what forces the host
  to keep the ordering "release only between frames". Split apart, the mechanism vsg already uses carries two implementations (`G6`), and it is the premise of several
  sessions (`.ai/design/graphics-sessions.md` §2.6).
- **Cost (written here rather than hidden)**: a released object lives to `submitted + slots + 1`, i.e. is destroyed **at most two frames later** (the conservative price of the
  evidence "the slot has been reclaimed"). Two frames of memory latency is cheap against one device stall, and the waiting becomes a background event timed in frames.
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
- **Lock and frame-head snapshot**: `ContentCaches` has **its own lock** (`ContentCaches::Lock`, non-reentrant) and a frame takes it **once, at the frame head**: `planDraw`
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
  `FrameScratch`, the rest from `caches.numbers()`. The single-session semantics are value for value what they were before the split (`test_vk` green, `BackendParityTest`
  12/12).
- **Frame-head storage reuse and its witness (I-02)**: `FrameHead`'s pass / call slots are `clear()`ed rather than rebuilt (`storageGrows()` increases only when capacity is
  short); `Device::frameHeadStorageGrows()` exposes it to tests, and `VkFrameHeadTest.ASteadyFrameDoesNotGrowTheHeadsStorage` pins it with "3 warm-up frames, then 5
  unchanged". **It proves the head's storage reuse only**, not a whole frame allocating nothing (pipeline keys and content caches have their own allocations, which this page
  neither claims nor measures). `planDraw` clears with `plan.clear()` rather than releasing and rebuilding the reused vector per command.

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
- **Boundary**: the table **destroys nothing** and **retires nothing**: `forget` only forgets the key, and the Vulkan object still goes to `RetirementQueue` (§2.15). It is not
  vsg's `PinnedResources` either (`vn::backend_vsg::core`) — that table answers "may this resident value be released now" (single-threaded, retired by pin), this one answers "who may build this key"
  (two drivers, up to publication only).
- **Wiring**: `ContentCaches::ensureGeometry` asks the table first (`Use` / `Build` / `Copy`), the frame-scoped `ContentCaches::FrameBuilds` `publishAll()`s after the frame
  fence was waited on (skipping failed fills), and `releaseGeometry` also `forget`s the key; a competitor takes `Copy`, builds a **detached** temporary entry and destroys it
  when the frame ends.

### 2.18 The target table: objects, shapes and one holder per frame (`G1` / `G6` / `G7`)

- **What it is**: the device holds one `TargetTable` — from the host's `RenderTarget*` to the `TargetObjects` it becomes (map + shape cache + build / rebuild / repair), the
  six target-refusal episodes, and the per-frame leases (`RenderTarget* → FrameDriverId`) **are three questions of one table**; `lease(driver)` issues a frame-scoped `Lease`;
  `executeFrame` and `prepareFrame` `claim` each pass' targets **before `ensure`**. On a conflict that pass **is skipped** (the frame's other passes still run) and
  `vk.target.claim_conflict` is reported — **one per (target, episode)**, the episode ending after a frame or a preparation without a conflict.
- **Why per pass rather than per frame**: one local conflict must not be amplified into "the frame failed"; the conflict takes the existing "this device cannot build this
  target" report path (`G6`), with one more code.
- **Why the claim comes before `ensure`**: building / rebuilding attachments is **the holder's verb** (`R5`), so "who may rebuild" and "who may draw" are the same answer; a
  claim one step later would let two sessions interleave rebuilds on one target.
- **Why one table**: the map and the lease answer the same question — "what is this target now, and who may touch it". With two tables a release had to do half in each place
  and a test had to drive two objects at once (`G1`); merged, `release(target)` is one call ("the entry leaves the map + the record goes to the park") and `drain()` clears
  records and leases together. The lease still holds its own lock (another driver's per-pass claim must not queue behind one build).
- **Boundary**: a lease is not a lock (`C5`) — waiting would tie two engines' frame rates together; a conflict destroys nothing and the target is retried next frame. `Lease`'s
  destructor is the **only release point** (`P3`), so no early return in a frame can pin a target. A window target needs no claim: `createSurfaceTarget` already guarantees one
  window belongs to one session (`WindowOccupied`).
- **When a conflict happens**: once two drivers are lent out by `RenderDevice::createFrameDriver()`, the conflict is exercised by a lease impersonating "another driver" and by
  `VkMultiSessionTest`'s concurrent cases; a single driver never conflicts.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Device lifecycle and loss | instance / device / queue / command pool / fence; loss ends the session | landed |
| F2 | Physical device selection | ordered by `VkPhysicalDeviceType`, ties keep the first seen | landed |
| F3 | Window and swap chain | surface, FIFO, one present semaphore per image, a 1 s acquire timeout, rebuild | landed (Linux/X11) |
| F4 | Offscreen targets | dynamic-rendering attachment sets, MRT, MSAA + depth resolve, borrowed depth, resize keeping the pipelines | landed |
| F5 | Content drawing | program → SPIR-V → pipeline; state mapping; vertex / index / instance streams | landed |
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

- **Behaviour**: enumerate every physical device and order by type (discrete 4 / integrated 3 / virtual 2 / cpu 1 / other 0), keeping the first seen on a tie; with a
  window, the family's present support is required as well.
- **Constraints**: `C7`.
- **Acceptance**: `T1`; the choice is readable through `handles()`.
- **Caveat**: `P10` (device identity is half of the cache key).

### 3.1.3 F3 Window and swap chain

- **Behaviour**: `WindowSystem` creates the surface (X11 / xcb); the swap chain is FIFO with `minImageCount + 1`, `currentTransform`, a composite-alpha fallback, and
  prefers BGRA8 / RGBA8 sRGB; **one present-wait semaphore per swap-chain image**; `vkAcquireNextImageKHR` has a 1 s timeout, and a timeout skips "this frame got no
  image" and reports once per episode; `OUT_OF_DATE` / `SUBOPTIMAL` rebuild.
- **Constraints**: `C1`, `C2`, `R3`.
- **Acceptance**: `WindowPresentTest`, `ProbeTest`; `T6` (a real host).
- **Caveat**: `P13` (suspect the environment first when a window test goes red; the swap-chain semaphores are one per image and cannot be shared).

### 3.1.4 F4 Offscreen targets

- **Behaviour**: `TargetObjects` holds the colour / sample buffer / depth / depth-resolve images and views, and the layout each
  one is in; a pass opens a **dynamic-rendering instance** over them (`beginPass` / `endPass`, and `DynamicRendering`), stating
  ONE load answer per attachment - so the HUD case (keep the picture, own the depth inside its rectangle) is two fields of one
  `VkRenderingAttachmentInfo` instead of a fourth render pass. The WINDOW owns the same pair (`WindowTarget::beginPass`), with
  a PER-IMAGE layout record: a swapchain image is `UNDEFINED` on its first use, `PRESENT_SRC_KHR` after it was presented, and
  the colour layout between two passes of one frame. An MSAA target draws into the sample buffers and resolves colour with
  `AVERAGE` and depth with `SAMPLE_ZERO` in the attachment info (`resolveMode` / `resolveImageView`, core 1.3 - no
  `vkCreateRenderPass2`); a depth-only target is a shadow map; a `shareDepth` target attaches the lender's depth picture and
  ALWAYS loads it (`TargetObjects::beginPass` overrides the policy - another target's image may never be cleared), and its
  barrier is recorded against that LENDER's image as a same-layout barrier: the dependency, not a transition. The depth
  ATTACHMENT and the depth RESOLVE PICTURE keep separate layout records, because sampling the picture does not move the sample
  buffer.
- **A target it cannot build is named at BUILD time**: a target with no usable extent answers `vk.target.size_unknown` and a borrowed
  depth of another extent answers `vk.target.borrow_extent_mismatch` when the host announces it, so the reason arrives with the
  announcement rather than at the first resize (a capability, not a difference - review 8 `K6`).
- **Constraints**: `C5`, `C8`, `N2`.
- **Acceptance**: `MsaaTest`, `ContentTargetTest`, `OffscreenClearTest`, `DeferredTest`, and
  `BackendParityTest.ADepthOnlyClearKeepsThePictureOnBothBackends` (a depth-only clear keeps the picture on both backends) plus
  `VkWindowPresentTest.AWindowFrameIsDrawnAndPresented`'s frame loop, which forces a presented image to be acquired again and is
  therefore the witness for the per-image layout record.
- **Caveat**: `P4`, `P15` (dynamic rendering inserts no dependency between two passes into one attachment).

### 3.1.5 F5 Content drawing

- **Behaviour**: `ShaderProgramDescription` → glslang → SPIR-V → `VkShaderModule`; the pipeline key is the
  SEAM's `vn::graphics::PipelineKey` and NOTHING ELSE (kind, program identity + revision, the vertex layout slots,
  attachment-set compatibility, variant, topology class - see `ContentPipelineKey`); one `DrawItem` = one
  `vkCmdDrawIndexed`. Instance streams are cached by buffer address + revision. The pipeline states NO render pass: it carries
  the attachment FORMATS through `VkPipelineRenderingCreateInfo`, so its blend-array length and the rendering instance's colour
  count are the same number derived from one value.
- **EVERY state item is a command** (`T54`, `ADR-2026-11-23` D4): viewport, scissor, line width and the depth-bias
  values always were; cull mode, polygon mode, depth test/write/compare and blend enable/factors joined them when the
  floor rose to `VK_EXT_extended_dynamic_state` / `_state2` / `_state3` (REQUIRED - a device without them is refused
  at `initialize` with `vk.init.dynamic_state_missing`). The commands live in ONE recorder helper
  (`recordRasterState`), so a state change compiles nothing: `VkContentTargetTest` pins that a wireframe paints fewer
  pixels than the filled frame and creates no pipeline, and `VkContentDrawTest.AStateChangeIsACommandNotAPipeline`
  pins the same for blend - with the pixel as the proof the command ran.
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
- **Acceptance**: `ContentDrawTest`'s `AScreenCopyIsDrawnWhateverThePassDepthPolicySays`, `ShadowTest`'s
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

- **Behaviour**: content sets are cached by (the five texture views + the shadow map), screen sets by (the source view + the shadow map), and the per-draw set is one per
  ring generation; the pool is created with `FREE_DESCRIPTOR_SET`, entries are released one by one, and one unused for 600 frames is swept every 120 frames.
- **Constraints**: `C5`, `N1`.
- **Acceptance**: `VkUniformRingTest.*`; the mutation that turns the cache off makes both cost-model cases red at once.
- **Caveat**: `N1` (only a new ring generation empties it).

### 3.1.10 F10 Device memory pool

- **Behaviour**: `MemoryAllocator` creates `VkDeviceMemory` blocks per memory type, resources hold a `MemorySlice{memory, offset, size, block}` and bind by offset;
  `MemoryUsage` decides the type (host-visible forces coherent), `MemoryKind` carries `bufferImageGranularity`; a release merges with the adjacent free run; a wholly
  empty block is destroyed when another block of the same kind exists; a host-visible block is mapped once as a whole.
- **Constraints**: `C6`, `N4`.
- **Acceptance**: `VkMemoryPoolTest.*` (6 targets = 12 slices ≤ 2 blocks; a steady-state frame allocates nothing; a readback gives its bytes back; shutdown reaches zero),
  `VkMemoryAllocatorTest.*` (adjacent merging; linear / optimal do not share a page).
- **Caveat**: `P7` (whether it can be mapped depends on the memory type; the pool opens before the first consumer of memory).

### 3.1.11 F11 Readback

- **Behaviour**: `ReadbackStaging` is one per call and **carries its own command pool and fence** (`C7`: readback borrows no session's frame machinery); a colour
  readback requires the target attachment to be `R8G8B8A8_UNORM`, and a depth readback turns D32 into a float array; the answer is the
  four-state `ReadbackResult` (`NotReady` / `Unsupported` / `Invalid` / `Failed`) returned as `ReadbackOutcome` (the deprecated
  `bool` + out-parameter shim forwards to it, BC-16). Readback takes the body gate EXCLUSIVE (the device's "between frames")
  and **counts one** `deviceWaits()`; a refusal answerable before waiting (unknown / unbuilt target, out-of-range attachment, unsupported format, borrowed depth) does
  not stop the device; a read called back from inside a frame body answers `Failed` (`FrameBodyDepth`, never waiting for itself).
- **Which formats can be read is a classification, not a surprise**: pure depth formats (D16 / D32 / D32F, MSAA resolving its
  resolve image) are read, and D24 or a borrowed depth answers `Unsupported` through the SDK's `depthReadbackOf` (`Readback.hpp`) -
  "this device never will", the answer a host gates its feature on (review 8 `K6`).
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
  (`FrameTiming::gpu_micros`); **every pass has its own pair of boundaries** (written before its uploads, so the transitions and copies it asks for count against it),
  read back into `FrameTiming::passes` (in execution order, named from `RenderPass::name`, so the host can hold it against the plan it read); it is sampled every 30
  frames (measured, this pair costs ~0.8 ms per frame on lavapipe), and an unsampled frame is **absent** rather than repeating the previous value.
- **Constraints**: `C7`, `C13`.
- **Acceptance**: `VkDeviceClockTest.AFrameReportsItsDeviceTime`,
  `VkPassTimingTest.TheReportAttributesTheFrameToThePassesThatSpentIt` (two passes, two entries, the names match, and the sum of the segments does not exceed the
  whole frame).
- **Caveat**: `P12`.

### 3.1.14 F14 Reports and diagnostics

- **Behaviour**: `frameReport()` gathers one frame's counters, progress, uploads and timings; `prepareFrame` answers a `PrepareReport`; driver messages and backend
  refusals take the same diagnostic route; every condition has one `ReportOnce`.
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
| `readColorBuffer` / `readDepthBuffer` | readable as soon as it is drawn (guaranteed by `C1`) | F11 |
| `setRenderTarget` / `releaseRenderTarget` | targets are held by the device; a release gives the slices back | F4 / F10 |
| `releaseContent(...)` | leaves a hole by identity and fills it next frame | §2.5 |
| `createSurfaceTarget(handle, why)` | one live target per window | F3 |
| `capabilities()` | reports only what is actually enabled | §2.8 |
| `handles()` | the escape hatch's value (inside the plugin) | F15 |
| `contentPipelines()` / `contentCacheCounters()` | the cost model's witnesses (pipeline count, ring / set) | §2.4 |
| `memoryStats()` / `pipelineCacheReport()` | the pool's and the cache's witnesses | F10 / F12 |

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
| `ContentDraw` | content / fullscreen recording, programs and pipelines, the ring, the set cache, the pipeline cache object | F5 / F6 / F8 / F9 / F12 |
| `ContentImages` | texture revisions, samplers, the stand-in | F7 |
| `TargetTable` | host target → object (map + shape + build / rebuild / repair) + the per-frame lease + release / park + the wait-free value snapshot (`peek`) | F4 / §2.18 |
| `TargetObjects` | an offscreen target's attachment set: images, views and the layout each one is in (`beginPass` / `endPass`) | F4 |
| `DynamicRendering` | one dynamic-rendering instance: the attachment transitions, `vkCmdBeginRendering` / `vkCmdEndRendering`, the resolve fields | F4 / §2.7 |
| `WindowTarget` / `WindowSystem` | the swap chain and the surface (per-image layout record, `beginPass` / `endPass`) | F3 |
| `MemoryAllocator` | the device memory pool (blocks / slices / mapping) | F10 |
| `PipelineCacheFile` | the cache file format, validation, the atomic write | F12 |
| `ReadbackStaging` | one readback's staging sequence | F11 |
| `BarrierPlan.hpp` / `StateMapping` | `stateOf` / `barrierBetween` / `loadOpOf` / `storeOpOf`; `StateMapping` turns SDK state into Vulkan enums | §2.7 |
| `ShaderCompiler.hpp` / `ProgramVariant` | `compileStage` + `SpirvModule` (glslang); the variant bits | F5 |
| `Handles` (`internal/` + `support/`) | the escape hatch | F15 |

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
- **C13 Attribution is segmented, not the whole frame repeated**: a pass' time is the segment between it and the next boundary (including the uploads and transitions it
  asks for), so the segments sum to ≤ the whole frame; attributing the whole frame to every pass is wrong (`VkPassTimingTest` pins it).

### 5.2 Requirements (R#)

- **R1** The validation layer is silent: neither `test_vk` nor a real host produces a VUID.
- **R2** Comparable with vsg: the same frame's rendered region is byte for byte identical (`BackendParityTest`).
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
- **N7** A frame's `FrameReport.timing` either belongs to that frame or is absent; the previous frame's value is never reused.

## 6. Caveats and Traps

### 6.1 P1 A fullscreen call must have depth Disabled

- How it fails: the fullscreen triangle sits at z = 0 (the Reverse-Z far plane); inheriting the pass' `TestAndWrite` discards the whole block with **zero VUIDs**.
- How to detect it: the recorder says `recorded=1` per call but the picture is the clear colour; `readColorBuffer` equals the background everywhere.
- How to prevent it: `planScreen()` forces `DepthPolicy::Disabled`; the case `AScreenCopyIsDrawnWhateverThePassDepthPolicySays` pins it with a fixture of "default pass
  state + a target with depth" (the old fixture set Disabled explicitly, which is exactly why it did not catch this).

### 6.2 P2 The screen set must bind the shadow map the pass resolved

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
- How to prevent it: `preparePass` runs before `vkCmdBeginRenderPass`; `ContentImages`' "not prepared" gives an explicit `why`.

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
  between two identical states, which IS the dependency - and the LAYOUT it starts from is the owner's own record
  (`TargetObjects::fresh` / `color_sampled` / `depth_sampled`, `WindowTarget`'s per-image record). A new caller must state its
  layouts instead of assuming a render pass would move them.
- Two IMAGES are not one layout: a multisampled target's depth ATTACHMENT and the picture it resolves into part company as soon
  as the picture is sampled - the sample buffer stays an attachment (it may not be sampled at all) and the picture moves to
  `SHADER_READ_ONLY_OPTIMAL`. One record for both states a layout an image never had. How to detect: a second frame over the same
  multisampled map reports `VUID-VkImageMemoryBarrier2-oldLayout-01211` (MEASURED; `VkShadowTest.AMultisampledMapDrawnAgainStatesBothDepthImages`
  is red without the split).
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

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | using `VMA` for memory | `G8`: today's consumers (targets, textures, streams, the ring, staging) are covered by the hand-written pool; a new dependency has to be paid for by someone | — |
| D2 | one `vkAllocateMemory` per resource | `G10`: allocations grow with the scene, and `maxMemoryAllocationCount`'s failure shape is "it looks like out of video memory" | — (changed, see F10) |
| D3 | compiling SPIR-V with vsg's `ShaderCompiler` | `G2`: that would point a dependency at another backend plugin; glslang is supplied centrally by `cmake/VineGlslang.cmake` | — |
| D4 | synchronization2 / dynamic rendering "later" | `G8` said the target was API 1.2; the 1.3 floor and the retirement's ordered batch made both the floor move and the legacy path go - D4 is LANDED as steps 1/1b/`T11` (2026-11-23) | `../decisions/ADR-2026-11-23-retiring-the-reference-backend.md` |
| D5 | putting the cache file format into the SDK | `G1`: the SDK does not know Vulkan; the right seam is "the host gives the location, the backend the format" (§2.9) | — |
| D6 | implementing several frames in flight directly | `G4`: changing the "read when drawn" contract needs a decision document first; the measured ceiling is about 10% (the device is 90%) | `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` |
| D7 | splitting the device and the session objects right away | `G8`: the contract is one frame in flight, one at a time, and a split would only add a layer of forwarding; because concurrent frames are clearly wanted it lands in **two steps** (the gate + the contract first, then the split) rather than in one go | `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | the whole behaviour | `build/bin/test_vk` (the whole suite — 174 cases / 42 suites on 2026-11-23); `ctest -R test_vk` |
| T2 | byte for byte identical with vsg | `test_vsg`'s `BackendParityTest` (13 cases) |
| T3 | the exposure boundary and the seam-free low level | `ctest -R vk_exposure` (V2 / V4 / E1 / E2, `scripts/check_vk_exposure.py`) |
| T4 | the cost model does not regress | `VkUniformRingTest.*`, `VkMemoryPoolTest.ASteadyFrameAllocatesNothing` (turning the cache off must go red) |
| T5 | validation is silent | in those runs `diagnosticCount(InitFailed)` and the layer count are 0 |
| T6 | a real host | `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=48 ./Vine --open demo`: median ≈ 8.2 ms, 11 passes / 10 draws, no `vk:` diagnostic |
| T7 | documentation and gates | `python3 scripts/check_ai_docs.py --strict` + §5.2 R5's scripts |
| T8 | mutations are load-bearing | recorded in `../memory/graphics.md`: the ring / set, pool and cache-file mutations each turn only their own cases red |
| T9 | readback | `OffscreenClearTest`, `ContentTargetTest`, `VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack` |

## 9. Status and Evolution

### 9.1 Current status

- Audit items F1 / F2 / F4 / F7 / F8 / F9 / F10 / F12 are landed (F9 / F10 as of the 1.3 floor and `T11`); F5 / F11 / F13 are kept as
  follow-up per §1.3. Several drivers are entirely landed, with the shape in §2.11 and `graphics-sessions.md`; **today's three TSan
  readings are in `graphics-threading.md` R18.**
- `test_vk` **176 cases / 42 suites** (2026-11-23, Linux: 176/176; the Windows reading the same day was 174/174 with no skips,
  before the two-window case and the `T54` witnesses landed); `test_vsg` **576**, `test_graphics` **639** (same build,
  `--gtest_list_tests`); `BackendParityTest` **15/15**. The target map, the shapes and the leases are one
  `TargetTable` (`TargetTableTest`); the frame head has a storage witness, and the light vector is not one copy per pass.
- **One build lesson**: after a change to an SDK vtable the **dynamic plugin must be rebuilt too** (an old `.so` makes `VsgBackendPluginTest` segfault).
- Measured (lavapipe, a real host): 11 passes / 10 draws, steady-state median **8.24 ms**, GPU **7.15–7.74 ms**; the ring + set cache change is −10% (9.03–9.23 →
  8.16–8.30 ms).
- **Host allocations, measured once and kept apart** (2026-11-18, Debug + lavapipe, `operator new` counted for one measurement): the backend's
  `executeFrame` asks for **85 allocations / 10.6 KB per steady frame with one draw packet** and **133 / 12.4 KB with four** - stable frame to frame, so it is churn,
  not growth; that is ≈**16 per further draw packet** and **69 fixed** (mostly small vectors: the pipeline key's channel/binding/attribute lists and the plan's
  per-draw scratch). The **whole chain frame** (engine + backend, the demo's deferred path: 10 passes / 10 draws with shadows, sky and HUD) is ≈**1966 allocations /
  ≈2.6 MB per frame** (`VN_BENCH_FRAMES` 8 vs 120 on one build, 2026-11-18). The two numbers answer different questions and are not interchangeable: the smaller one is
  the backend's own recording window, the larger one is what a host's frame costs. GPU-side builds and uploads are **0** in steady state (`VkResidencyTest`), so none
  of this is device memory.
- `test_vsg` (Release) still shows `T36`'s `EachAttachmentClearsToItsOwnColour` occasionally under WSLg (window / device creation, see the backlog); the three
  "released before submission completed" defects are fixed, evidence in `../bugs/vsg-teardown-frees-in-flight-resources.md`.

### 9.2 Known gaps / backlog

- **Audit F5 (several frames in flight)**: **decided to keep one frame in flight** (`../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`), with the trigger
  conditions written in the ADR (device time under half the wall clock, an artefact shape that must overlap the GPU, or a second backend needing the same semantics).
- **Audit F9 / F10**: LANDED. `vkCmdPipelineBarrier2` (step 1b, `BC-38`) and dynamic rendering (`T11`, `BC-39`) are the only
  spellings now - no `VkRenderPass`, no `VkFramebuffer`, no 1.0 `vkCmdPipelineBarrier` or `vkCmdBeginRenderPass` in the
  backend. The 1.3 floor with the `synchronization2` and `dynamicRendering` features is the device requirement.
- **Backlog `T35` (becoming the default backend)**'s gate (`../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`): `releaseRenderTarget` really frees the
  attachments, `readDepthBuffer` and the device-loss report are landed; `releasePass` is a documented default no-op (`RenderDevice.hpp`), and the user page is
  `docs/guides/graphics-backends.md`; the Win32 branch is implemented and its presented pixels are asserted there (`../memory/graphics.md` 2026-11-23: `test_vk`
  174/174, no skips, the window cases reading the compositor); **still missing** are a measured Wayland fallback and a performance reading the gate accepts.
- **Backlog `T12` / `T13`**: reading pixels from a Windows window, and the Wayland fallback.
- **Backlog `T60`**: retiring `vsg` after this gate - trigger, rides and reasoning are in the item; the terms (mechanism
  ownership, the 24 `vsg.*` codes, the document collapse and the batch order `T54` → `T11` → `T58` → delete) are
  [ADR-2026-11-23](../decisions/ADR-2026-11-23-retiring-the-reference-backend.md), and this backend is the one the
  retirement leaves in place. **Steps 1 (`T54`), 1b (`BC-38`) and 2 (`T11`, `BC-39`) landed 2026-11-23**: the floor is
  Vulkan 1.3 with `synchronization2` + `dynamicRendering`, the pipeline key has no state half, and the backend has no
  render-pass or framebuffer object left; the remaining steps are the alpha-to-coverage bit (`T58`) and the deletion.

### 9.3 Compatibility / migration

- **SDK**: `DeviceDesc` gains `cache_directory` (it has a default, so the old two-argument aggregate initialisation is unchanged).
- **Plugin ABI**: only `RenderBackend` / `RenderBackendFactory` face the host; the internal headers (`internal/`) promise no stability.
- **Cache file**: format version `1`; a mismatched version is refused (no migration, no guessing).
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
- `tests/test_vk/**`: behaviour; `tests/test_vsg/BackendParityTest.cpp`: the comparison.
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
