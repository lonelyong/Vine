# ADR-2026-11-15: the device is a resource pool and a session owns a frame - two engines share content and draw separately

- Status: Accepted (this document fixes the **contract and the shape**; the landing takes two steps, see §Decision item 9. The first step, `T47`, landed on 2026-11-15; the second step, `T48`, landed its vk side S1-S7 / S9 on 2026-11-16 - see "Implementation status" at the end)
- Date: 2026-11-15
- Scope: `../design/graphics-threading.md` (new §2.16 / `C21` / `R18` / `N18` / `D13`),
  `../design/vk-backend.md` (new §2.11 / the `§9.2` gap), `../backlog/graphics.md` (`T47` / `T48`),
  the contract text and implementations of `RenderDevice.hpp`, `RenderFrameExecutor.hpp` and `RenderEngine.hpp`
- Related: `ADR-2026-11-13-one-engine-one-surface.md` (one engine, one surface - this document is its next step, not its opposite),
  `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` (one frame in flight; this document **shares** the retirement mechanism with it)

## Background

The requirement (2026-11-15, the owner's words): **geometry / shader / texture resources are shared, frames are not; once shared, it has to be thread-safe;
two engines may frame at the same time.**

The half that already holds is "sharing":

- One `RenderDevice` being borrowed by several `RenderEngine`s **is already supported**: the device is `initialize()`d by its creator and an engine only borrows it
  (see the contract of `RenderEngine::setDevice`); `tests/test_graphics/HeadlessBackendTest.cpp`'s
  `OneDeviceServesSeveralAreasWithoutCrossingThem` is exactly two engines, one device, two areas and two targets.
- But that case's two frames run **one after the other** (`engine_a->frame(); engine_b->frame();`) - the "without crossing them" in its name says exactly this:
  **sharing is proven, concurrency is not.**
- Content residency was already cached by **identity** (geometry by a `GeometryDescription`'s address + revision, programs by description + variant bits,
  textures by a `Texture`'s address + revision), so the same geometry is **the same key** in two frames: sharing was already one concept at the key layer.

The half that does not hold is the **frame**: a device has one `RenderFrameExecutor` (whose contract says outright it "is a property, not state"),
and beneath it there is **one command pool, one fence, one constant ring, one descriptor pool**. Two engines framing at once means two threads writing the same frame state.

### The concrete facts in the way (all checkable in the code)

| # | Fact | Where |
|---|---|---|
| 1 | A device answers one executor, and the contract says it "does not change with the session" | `executor()` in `RenderDevice.hpp` |
| 2 | A frame's machinery is **device-level**: one `command_pool` + one `completion` fence, shared by the frame path **and** the two read-back paths | `Device.cpp` (`Impl`'s `command_pool` / `completion`; submission and waiting are inside `executeFrame`, read-back passes the same `impl->completion` at `:2434` / `:2566`) |
| 3 | One `ContentDraw` object holds both the **caches** (modules / streams / pipelines / layouts) and the **frame state** (constant ring, descriptor pool, per-draw UBOs) | `Impl` in `ContentDraw.hpp` |
| 4 | The frame boundary resets **device-level** state: `beginFrame()` resets the descriptor pool and `endFrame()` releases the per-draw UBOs | the "FRAME BOUNDARY" section of `ContentDraw.hpp` |
| 5 | A content cache has **no "uploading" state**: on a miss the copy is recorded into the current frame's command buffer and the entry appears in the table straight away | `resolve*` / `prepare*` in `ContentDraw.cpp` |
| 6 | The default content program is a **device-level single slot**: a second engine's `setDevice()` replaces the first one's | `setDefaultContentProgram` in `RenderDevice.hpp`, `setDevice` in `RenderEngine.cpp` |
| 7 | The notes on `releaseContent` / `releaseRenderTarget` say "call between frames" - which holds today **only because** one frame is in flight | `Device.hpp` |
| 8 | Three pieces of contract text say "one frame at a time, one thread": device, executor, plugin device | `RenderDevice.hpp`, `RenderFrameExecutor.hpp`, `Device.hpp` |

**One correction**: an earlier assessment said "diagnostic routing is also a device-level single slot", which is wrong. `setDiagnosticsRoute` states explicitly that every report reaches
**every** installed route, so a second engine does not take the first one's counts, and the accessor answers from the device's own routes - **routing is already multi-route**.
The only real single slot left is item 6 (the default content program).

## The problem has to be split into three, because the three answers differ

| Question | Answer | Basis |
|---|---|---|
| **What can be shared?** | GPU resources that **do not change after upload**: vertex/index streams, shader modules and pipelines, textures and samplers, plus the device itself (instance / physical / logical / queues / memory pools / pipeline cache file) | A cache key contains the revision ⇒ the entry is **immutable**; an immutable thing has no "whose" question |
| **What must not be shared?** | Anything **with a layout** or **with a mutable host-visible range**: targets and their attachments, swap chains, command pools and buffers, fences, descriptor pools, constant rings | An image has exactly one layout at any moment; the constant ring is "written inside a frame, read inside a frame" scratch |
| **Who may touch the shared part at once?** | **Limit by resource, not by thread**: immutable resources are read concurrently by any session; mutable shared state (pipeline cache, queues, allocator, cache tables) gets one lock each; per-session things are thread-confined | See §Vulkan's hard constraints and §The lock table |

This split is `G1`: three questions, each with one concept, one owner and one boundary. Adding locks with "thread safety" as one whole
is the reverse of `G8` (coordinating writers that do not exist) and the reverse of `G10` (putting the cost on the hot path).

## Vulkan's hard constraints (not our taste - the specification's)

A Vulkan device is **itself** not "one thread at a time": most entry points may be called concurrently, and the specification requires external synchronisation only for **specific objects**.
That decides the shape - sharing is legal, and only a few objects need locks:

| Object | What the specification requires | What we do |
|---|---|---|
| `VkDevice` (general entry points) | Concurrent calls are legal | No global lock (**rejecting D13**) |
| `VkQueue` | External synchronisation (submissions to one queue must be serialised) | One **submission lock**; a submission is an O(1) driver call and the device executes outside the lock |
| `VkPipelineCache` | External synchronisation (when passed to `vkCreateGraphicsPipelines`) | One **pipeline lock**; taken only on a miss, zero times in steady state |
| `VkCommandPool` / command buffer recording | External synchronisation (per pool) | **One pool per session**, lock-free |
| `VkDescriptorPool` | External synchronisation (per pool) | **One pool per session**, lock-free |
| `VkFence` / semaphores | External synchronisation (per object) | **One per session**, lock-free |
| `VkPipeline` / `VkPipelineLayout` / `VkDescriptorSetLayout` / `VkSampler` / `VkImageView` | Read-only after creation, usable concurrently | Shared, lock-free |
| `VkBuffer` / `VkImage` / `VkDeviceMemory` | Concurrent use is fine as long as ranges do not overlap; **the layout is the image's state** | Read-only after upload; a target is claimed by one frame (see §Decision item 4) |

The `VkDeviceMemory` row is the fulcrum of this design: **memory can be shared, as long as two users do not write the same bytes**.
A residency entry is no longer written once uploaded, so "sharing video memory" and "recording concurrently" do not conflict.

## How the industry splits it (the parts worth copying, marked as checked here or not)

| System | Device | Frame / commands | The sharing boundary | Checked |
|---|---|---|---|---|
| **VSG 1.1.16** (this repository's reference backend) | `vsg::Device`; `Context` builds **two device-level** `MemoryBufferPools` on it (device / staging), each with `mutable std::mutex _mutex` | Each `RecordAndSubmitTask` has its own (`numBuffers = 3`, one `_fences` per slot) | **A device-level pool + per-task frame state** - the same shape as here | Checked: `mutable std::mutex _mutex;` at line 69 of `MemoryBufferPools.h`; `Context.cpp` hangs the two pools on `device->...BufferPools`; `RecordAndSubmitTask.h` holds `std::vector<ref_ptr<Fence>> _fences` |
| **O3DE RHI** | `Device` is thread-safe; `PipelineLibrary` is shared and locked | One `CommandList` per thread | As above | External material, not checked here |
| **wgpu** | `Device` / `Queue` are `Send + Sync` | One `CommandEncoder` per thread | As above | External material, not checked here |
| **Filament** | `Engine` is explicitly **not thread-safe**, `Driver` is single-threaded | Everything on the render thread | Not shared | External material; a **counter-example**: the cost is one engine per device |
| **bgfx** | `bgfx::frame()` is a single-threaded API with an internal render thread | Internal | Not shared | External material; a counter-example too |

The conclusion is not "everybody does this" but that the **divide agrees**: **the device is a pool (with locks), a frame is per-thread state.**
The difference is only whether the API exposes a second frame driver - Filament and bgfx choose not to (one device per engine),
we choose to, because the requirement is precisely "two engines, one device, and two copies of content uploaded once".

**One difference that must be stated**: VSG's `MemoryBufferPools` are **device-level**, but it is common for a `Viewer` to build its own; that is,
VSG shares the **device** but not necessarily the **residency**. What this repository wants is stronger: one piece of geometry **uploaded once** across two engines.
That is exactly why this document is a framework decision rather than "copying VSG".

## Options

| Option | Benefit | Cost / violation |
|---|---|---|
| **A Write the specification only, change no code** ("two engines may share a device, but frames must be serial") | No change; contract and code agree (`G6`) | The requirement is not met; and **not even seriality is guaranteed today** - two threads calling `executeFrame` at once is a data race, not "a bit slower" |
| **B A device-level frame lock** (one `std::mutex` around `executeFrame`) | A small change (one lock + contract + one case); turns a silent race into correct serialisation; no public contract changes (`G4` / `G11`) | The two engines wait a whole frame for each other (about 8.6 ms here); `G8` says this is a **transition**, not the endpoint |
| **C Device = pool, session owns the frame** (this document) | The frame path takes **zero locks**; sharing is paid for only on a miss; the two engines really run in parallel; the shape matches VSG / O3DE / wgpu (`G1` / `G5` / `G9`) | Requires splitting `ContentDraw`, adding a residency state machine, adding three locks and target claims; about 1350-2100 lines, 12-18 files |
| **D One device per engine** (no sharing) | Zero risk; doable today (`SurfaceWindow` already does exactly this: one device per window) | The opposite of the requirement: the same geometry uploaded twice, pipelines compiled twice, video memory twice; `G5` (what should be shared is split apart) |
| **E A global lock around every device verb** | The least effort to write | Reverses `G7`: the complexity is not absorbed but **transferred** into "every call may block"; and the lock granularity has nothing to do with resources, failing `G1` ("thread safety" treated as one concept). **Rejecting D13** |
| **F A per-resource read-write lock** (one `shared_mutex` per entry) | Sounds the finest-grained | Today's shared resources are **immutable after creation**, so there is no writer to coordinate; a per-entry lock would only pay 2000 atomics on a 2000-draw path (`G10`), solving a problem that does not exist (`G8`) |

## Decision

**Adopt C, with B as the shape of the first landing step** (make the status quo correct first, then make concurrency hold).

### 1. Nouns: the device is a pool, a session owns a frame

| Noun | What it is | Who owns it | Concurrency |
|---|---|---|---|
| `RenderDevice` | The **pool**: GPU, memory allocator, content residency caches, targets, pipeline cache, diagnostic routes | Its creator (the host) | Shared; internally locked **per resource** |
| `Session` (called `RenderFrameExecutor` today) | **One frame's executor**: command pool + command buffer + fence + constant ring + descriptor pool + frame accounting | One engine | **Thread-confined**: one thread at a time |
| A residency entry | One content copy on the GPU, key = identity + revision | The device | **Immutable** after upload; a `Resident` entry is read concurrently by any session |
| A target | A set of images with a layout plus its render pass / framebuffer | Built by the device, borrowed by a session | Claimed by **one** session within a frame |

`RenderFrameExecutor` **gains no new type**: it is the existing name of the session concept, and what changes is "who owns it and how many there are".
`RenderDevice::executor()` stays, its meaning narrowed to "the device's own session **when no engine drives it**" - the same convention
`setDiagnosticsRoute` already uses ("a device with no engine driving it answers from its own private routes"), `G6`.

### 2. The shared and unshared surfaces (one decidable sentence each)

- **Shareable** = GPU resources that do not change after upload + the device object. Criterion: **its key contains the revision, and its layout no longer changes.**
- **Not shareable** = targets with a layout, swap chains, command pools/buffers, fences, descriptor pools, constant rings. Criterion: **it is written during a frame's lifetime.**
- **A window is still one window, one target** (`createSurfaceTarget`'s `WindowOccupied` rule is unchanged): two engines cannot draw the same window;
  that is a platform constraint, not a trade-off of this ADR.

### 3. The residency state machine: `Absent → Uploading(owner, frame) → Resident`, and only `Resident` may be drawn

Today's cache has only two states, "in the table / not in the table", because filling happens inside **the one and only** frame's command buffer. After two sessions there must be a third state:

```
Absent ──the first session that needs it claims──▶ Uploading(owner, frame)
                                        │ the owner's frame completes
                                        │ (today's synchronous model: executeFrame has waited on the fence when it returns)
                                        ▼
                                     Resident ──releaseContent──▶ Retiring(epoch) ──every session completed ≥ epoch──▶ released
                                        ▲
                     a contender that sees Uploading builds a **temporary copy** in its own frame (not published, dropped at the frame's end) and draws as usual
```

Three rules, and it does not hold without any one of them:

1. **One key has exactly one publisher.** The claim is atomic; the loser **does not publish** and uses its own copy only within this frame.
2. **A `Resident` entry is published only once the fill can be proven complete** - today that is `executeFrame` returning (it runs `vkWaitForFences` internally),
   and the warm-up path is that transfer-only submission returning. Once several frames are in flight (F5), the publish point becomes "that frame completes",
   the same mechanism as retirement.
3. **Entries are immutable**: the key contains the revision, so a `Resident` entry is never rewritten; a new revision is **a new entry**.
   This is an existing fact (`ContentDraw`'s keys); this ADR only writes it down as a premise.

**Why the contender "copies" rather than "waits"**: waiting is blocking between sessions, the one thing that can **deadlock** two sessions
(A waits for B's frame, B waits for A's frame), whereas copying is bounded - each identity has at most `N−1` temporary copies, only on the frame where
"two engines first draw it at the same time", and after that everything is a hit. `prepare()` does this during loading, so a real host usually never hits it at all.

### 4. Targets: claimed per frame, and a conflict skips only that pass

The `FrameDescription` that `executeFrame` receives already lists each pass's target, so a claim needs no cooperation from the host:
the frame head claims **the set of targets it will draw** and releases it at the frame's end. On a conflict (another session is in flight and its name set contains the same target)
**only that pass is skipped**, with one report keyed on `(target, episode)` - **the same shape** as the existing "this device cannot build this target, report once and skip"
(`G6`: no new failure shape). The whole frame does not fail: the other passes have nothing to do with it.

Targets are built by the **device** and cached by shape (`planTarget`'s Rebuild / ResizeInPlace / Repair unchanged),
so when two engines draw **different** targets, the attachments, render pass and framebuffer are still built once.

### 5. Release: park + every session's `completed ≥ epoch`

`releaseContent` / `releaseRenderTarget` say "call between frames" today because one frame is in flight. With two sessions, "between frames"
is no longer a moment. The approach follows `RetirementQueue`'s existing shape (it is already the queue that decides "when may this be destroyed"):

- a release request records **every session's `submitted` at that moment** (`FrameTimeline`'s existing counter);
- the object is really destroyed only when **every** session reports `completed ≥` its recorded value;
- so `releaseContent` may be called while another engine is framing - exactly the scenario this ADR wants (the host edits while the background bakes),
  rather than a verb whose timing the host has to police.

### 6. The default content program belongs to the session; the device keeps only the "no engine" fallback

The direct consequence of item 6's single slot is that program-less content of two engines gets shaded by **the other engine's** program, with no report at all.
The fix has the same shape as diagnostic routing: the program is **the session's** (one per engine), and the device keeps one of its own that serves only callers
**driving the device directly** (that is how `test_vk` uses it). `RenderEngine::setDevice()` no longer announces it to the device but hands it to its own session in `initialize()`;
`setDefaultContentProgram` at run time affects only its own session.

### 7. The lock table (which lock, what it guards, why it is cheap)

| Shared thing | Protection | Taken when | Cost |
|---|---|---|---|
| `VkQueue` submission | Submission lock | Once per frame | Driver-call level; the device executes outside the lock |
| `VkPipelineCache` | Pipeline lock | Only on a pipeline miss | Zero times in steady state |
| The memory allocator | Allocation lock | Only on an upload miss | Zero times in steady state |
| The residency cache tables (programs / streams / pipelines / images) | Cache lock (read-write) | **Once at the frame head**: resolving this frame's identities into a session-local `Resolved` table | Once per frame, not once per draw |
| The target table / each target's attachments | Target lock + a per-target `building` flag | On build and lookup | Once at the frame head |
| The upload staging block | Staging lock | Only on a miss | Zero times in steady state |
| Per session: command pool / fence / descriptor pool / constant ring / frame accounting | **No lock** (thread-confined) | — | Zero atomics on the frame path |
| Per session: `submitted` / `completed` | Atomic | Write: the session thread; read: the retirement decision | Read only when releasing |

**The record path (per draw) takes no lock**, which is the condition for the whole design to hold, and also `G10`'s account: a frame with 2000 draws,
at 20-40 ns per lock acquisition, would be 40-80 µs, 5-9% of this machine's ~0.9 ms host time - unacceptable, hence one snapshot at the frame head.

### 8. Three pieces of contract text to change

"One frame at a time, one thread" in `RenderDevice.hpp`, `RenderFrameExecutor.hpp` and `Device.hpp` becomes:

- **The device**: borrowable by several engines; content residency shared (one upload, one compile); **host verbs that change shared state fall into two classes**:
  - **per-session** (`setRenderTarget` / `releaseRenderTarget` / `setDefaultContentProgram`) may not run concurrently with **their own session's**
    frame, but are unaffected by other sessions;
  - **device-level** (`releaseContent`) may be called while any session is in flight - it only parks, it does not destroy immediately (§Decision item 5).
- **A session**: thread-confined; two threads entering **the same** session at once is a contract violation (guarded, one report and a refusal).
- **Two sessions may frame at the same time**: all they share is read-only content; what they must not share is one target and one window.

**The difference between the first and the second step has to be written into the same paragraph**: the first step's implementation **serialises** the two sessions inside
the device (the frame gate), and only the second step lets them run in parallel - so the contract may already say "may be called at the same time", while the cost of "at the same time"
disappears with the second step (`G4`: state which version a cost belongs to).

### 9. Land in two steps

**Step one (`T47`, low risk, lands first)** - make the status quo **correct** and the contract **honest**:

1. A device-level frame gate (`std::mutex` + recording the holding thread): **serialise** across threads; a same-thread re-entry (a `before_draw` hook
   driving another engine) reports once and is skipped, with a Debug assertion. Today such a re-entry corrupts the same command pool outright.
   **The gate also guards `prepareFrame`** (it submits a transfer-only batch too, see §Decision item 10).
2. Item 6's single slot is **written into the documentation** ("one device, one default program; when two engines declare different defaults they do not compose, and the later one wins"),
   not half-fixed in step one - the structural fix belongs to step two, and a half-fix would create a concept step two has to tear down (`G8`).
3. The three pieces of contract text (§Decision item 8).
4. Cases: two engines, one device, two threads, N frames each ⇒ no crash, each picture correct, at most 1 diagnostic;
   mutation: remove the frame gate ⇒ the case must go red (at least one of ASan / TSan).

**Step two (`T48`)** - make concurrency **hold**:

| Change | Files | Size |
|---|---|---|
| A `createSession()` verb + the engine holding its own session | `RenderDevice.hpp`, `RenderEngine.cpp/hpp`, `RenderBackend.hpp` | 200-350 lines |
| `ContentDraw` split into `ContentCaches` (device) + `FrameScratch` (session) | `ContentDraw.hpp/.cpp`, `Device.hpp/.cpp` | 500-700 lines (mostly moves) |
| The residency state machine + the frame-head snapshot | the same + `Keys.hpp` | 200-300 lines | <!-- drift-ok -->
| Three locks (submission / pipeline / allocation) + target claims | `Device.cpp`, `MemoryAllocator.cpp` | 150-250 lines |
| Multi-session retirement (§Decision item 5) | `RetirementQueue.hpp`, `FrameTimeline.hpp` | 100-200 lines |
| Cases (`test_vk` two sessions / `test_graphics` two engines / TSan) | `tests/test_vk`, `tests/test_graphics` | 200-300 lines |

**About 1350-2100 lines across 12-18 files** in total. The vsg backend is **closer**: it already is "a device + a per-session command graph"
(`VsgBackend`'s `Session` / `OffscreenSession`); what it lacks is device-level residency sharing and cross-session retirement.

### 10. Three device-level facts that the split does not change

- **Device loss is device-level**: when one session sees `VK_ERROR_DEVICE_LOST`, `initialized()` turns false for **all** sessions,
  and the rebuild path is still `shutdown()` + `initialize()`. Two engines do not end up "one dead, one alive" - they share the same `VkDevice`,
  and that is not something a failure classification can separate.
- **Capabilities are device-level**: `capabilities()` is read-only and may be asked by any session at any time (it decides how a frame is driven, see `RenderEngine::frame`).
- **Warm-up and frame go through the same door**: `prepareFrame` also submits a transfer-only batch (`ADR-2026-11-14-prepare-may-submit-transfers.md`),
  so step one's frame gate must **guard it as well**, or one thread's warm-up collides with another thread's frame on the same command pool; in step two
  it naturally becomes a session verb.

## Consequences

- **Good**:
  - The frame path takes **zero locks**, and that is not an optimisation but the structure: shared resources are immutable after upload, so "concurrency" needs no
    coordination on the hot path (`G1` / `G10`).
  - The nouns agree with the industry's divide (device = pool, frame = per-thread state), which survives a backend change (`G9`).
  - Step one alone turns today's **silent data race** into correct serialisation plus one diagnostic (`G4` / `G11`).
  - It **shares** the same retirement mechanism with F5 (several frames in flight): doing either first causes no rework.
- **Bad / cost**:
  - Step two crosses 5 modules (SDK shape, engine, vk plugin, retirement mechanism, cases), so `G5`'s account is per module.
  - The constant ring goes from "one per device" to "one per session": video memory multiplies by the session count (each session grows with that session's
    draws in one frame). That is the direct price of "frames are not shared", booked under `G10` rather than hidden.
  - On the frame where two sessions draw the same content at once for the first time, the contending session uploads it one extra time
    (bounded, see §Decision item 3).
  - On this machine (lavapipe) the **gain is nearly unmeasurable**: in software rasterisation "device time" is CPU time, and two sessions compete for the same CPUs.
    The real benefit lies on a real GPU where "the device is fast and the host is slow" - the same coin as the numbers in
    `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` (there 8.63-8.94 ms wall clock / 7.95 ms device ⇒ the device is ~90% here).
    **So this document cannot prove a benefit with a local benchmark**; it can only prove the cost structurally (no lock on the hot path, paid only on a miss).
- **Trigger conditions (when to reopen)**:
  1. a real host "edits while baking" (two engines wanting pictures at once) and step one's serialised wait enters the frame budget;
  2. F5's trigger is hit (device time < half the wall clock) - then several frames in flight and several sessions share the retirement mechanism and are done together;
  3. a backend of a second API family (D3D12 / Metal) lands, forcing "the device is a pool" to become an official noun of the seam.
- **Explicitly not done**:
  - Not "one target drawn by two sessions at once" - an image has one layout, and no lock can change that (`G4`: saying "no" beats saying "maybe").
  - No global device lock (`D13`).
  - No per-entry read-write locks (option F).
  - No change to "one engine, one surface" (`ADR-2026-11-13`) - the two engines each drawing their own surface is exactly this ADR's premise.

## Evidence

- **Code facts**: this document's "facts in the way" table (8 items, each with its file and member); `Device.cpp`'s `:2434` / `:2566`
  passing `impl->completion` proves read-back and the frame share one fence; `setDiagnosticsRoute` in `RenderDevice.hpp` proves
  multi-route **already** landed (so the only single slot left is the default program).
- **Existing cases**: `OneDeviceServesSeveralAreasWithoutCrossingThem` in `tests/test_graphics/HeadlessBackendTest.cpp`
  (sharing proven, concurrency not); `tests/test_vk` 89 cases
  (the whole suite implicitly depends on single-session semantics); `tests/test_vsg/BackendParityTest.cpp` (both backends compared byte for byte under the same frame semantics).
- **Precedents**: VSG's device-level pool + per-task frame state (`mutable std::mutex` at line 69 of `MemoryBufferPools.h`,
  `Context.cpp` hanging the pools on `device`, `_fences` in `RecordAndSubmitTask.h`).
- **The specification**: Vulkan's external-synchronisation requirements are per object (queue / command pool / descriptor pool / pipeline cache / fence),
  and the device itself allows concurrent entry points - see §Vulkan's hard constraints.
- **Numbers**: 8.63-8.94 ms / 7.95 ms in `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`;
  a local machine cannot measure a concurrency gain, for the reason in §Consequences.
- **Gates**: `scripts/tsan_check.sh` (step two's race evidence); `scripts/check_vk_exposure.py` (the shape changed, the boundary did not).

## Implementation status (appended, 2026-11-16)

- **Step one (`T47`): landed 2026-11-15** - the device-level frame gate (`backend/FrameGate.hpp`, one implementation for both backends), the three pieces of contract text,
  and the concurrency cases.
- **Step two (`T48`): the vk side landed** - S1 (release goes through `RetirementQueue`), S2a/S2b (the frame machine and frame body move into `Device::Session`),
  S3 (`ContentCaches` + `FrameScratch`), S4 (the `ResidenceTable` three-state machine), S5 (`TargetClaims` leases), S6a/S6b
  (`createSession()` + the engine holding its own driver + the default program belonging to the session), S6c-1 (the frame-head snapshot), S6c-2a (the frame bodies really in parallel), S6c-2b (release does not wait
  for a frame), S7 (read-back independent: its own pool/fence + counted waiting), S9 (contract/documentation close-out + TSan).
  **Still not done**: S8 (moving vsg's collection scratch into the session) and wiring `ResidenceTable` into the device - the state machine is in the tree, the caches still take the old path, and when two
  drivers build the same content at the same time for the first time they **serialise** on the cache lock (`graphics-sessions.md` §9.2).
- **The contract text has been rewritten to match the implementation**: `C21` / `R18` / `N18` in `graphics-threading.md` (frames run concurrently per session; the default program belongs to the session),
  the threading paragraphs of `RenderDevice.hpp` / `RenderFrameExecutor.hpp`, and the `@warning` in vk `Device.hpp`.
- **Race evidence (TSan, 2026-11-16)**: `test_vk` 133/133, `test_graphics` 587/587, and `test_vsg` (excluding the window / probe / plugin
  suites): **0 race reports** each. The tool found and fixed one **real defect**: `Geometry::description()`'s lazy cache had no protection (two engines'
  content threads describing the same geometry at once raced) - readers now take a shared lock and a single builder takes an exclusive one.
- **Platforms**: Linux/X11; real-machine evidence for Win32 / Wayland still fails the switch gate (`T35`).
- **Appended (2026-11-17)**: the previous item's "still not done" is out of date - S8a / S8b / S8c-1 / S8c-2 and `ResidenceTable`'s
  device wiring (`F4`) all landed on 2026-11-16, so this ADR's nine migration steps are complete. Today's shape is in
  `graphics-sessions.md` §2.9 / §3.1.9 and the threading contract is in `graphics-threading.md` `C21`; each step's cases and mutation evidence
  stay in the git history (the original record is not rewritten).
- **Correction (2026-11-18, audit-4 B13)**: the three-state machine this ADR calls `ResidenceTable` is
  `ResidencyClaims` today (`vn::backend_vk`); the states (`Absent` / `Uploading` / `Resident`) and every
  argument above are unchanged.
