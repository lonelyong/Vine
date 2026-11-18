# the device is a pool and sessions own frames: types, ownership and concurrency rules

> **Status**: current — the shape below is landed in both backends (§9.1).
> **Last verified**: 2026-11-17
> **Scope**: the **implementation shape** of what is shared, who owns a frame, how frames run concurrently and how retirement works when one `RenderDevice` is borrowed by
> several engines; the contract's **why** and its landing record are in the ADR, and the threading model's **present** is in `graphics-threading.md`.
> **Sources**: `RenderDevice.hpp` / `RenderFrameExecutor.hpp` / `RenderBackend.hpp` (the contract),
> `graphics/FrameGate.hpp` / `graphics/DeviceClock.hpp` / `graphics/RetirementQueue.hpp` (the mechanisms),
> `src/plugins/gfx_backend_vk/src/internal/Device.cpp` and `ContentDraw.cpp` (the device-level frame machinery),
> `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp` (the half that is already "device + per window").
> **Backlog**: no session migration step is unlanded; trigger-gated work is in §9.2 and `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

The requirement, in the user's words: **geometry / shader / texture resources are shared, frames are not; sharing must stay thread-safe; two engines may frame at the same time.**

`ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` fixed the **contract and the shape** (the device is a pool, sessions own frames, residency's three states, target
claims, cross-session retirement, the three locks). This page records the shape that resulted: which types exist, who owns what, where a frame takes each lock, and where
state falls back on failure. It does not repeat the ADR's argument (the options, the industry comparison, Vulkan's external-synchronisation table) and only refers to it;
it does not repeat `graphics-threading.md`'s `C21` either.

### 1.2 Goals

1. **Two sessions drive one device at the same time**: content is uploaded and compiled once; two frames record in parallel and submit on their own.
2. **The recording path takes no lock**: a steady-state frame takes no device-level lock (`G10`'s account, see §2.3).
3. **Every failure has one owner**: device loss, allocation failure, target conflict, upload race, release overlapping a frame in flight — each of the five has one
   sentence of contract, one state and one report, and the host does not have to guess the ordering (`G4` / `G11`).
4. **Migration is acceptable stage by stage**: at the end of each stage the existing `test_graphics` / `test_vk` / `test_vsg` are green, and the invariants that stage adds have
   **a case that can go red** (§8).

### 1.3 Non-goals

- **No "one target drawn by two sessions at once"**: an image has one layout at any instant, and no lock can fix that (`G4`: saying "no" beats saying "maybe"). A conflict
  **skips that pass only**, see §2.5.
- **No global device lock** (`D1`) and **no per-entry read/write lock** (`D2`) — the ADR rejected both.
- **No several frames in flight** (`ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`): that is that ADR's topic. This page only guarantees that the retirement mechanism is
  **shared** with it (§2.6), so either order needs no rework.
- **No turning "one window, one target" into something shareable**: that is a platform constraint (`createSurfaceTarget`'s `WindowOccupied`).
- **No change to appfw's usage**: `SurfaceWindow` builds one device per window, and sharing has no consumer there yet (`G8`).

### 1.4 One-paragraph conclusion

**A session is the "frame driver" (not the window); it owns the frame machinery and the frame scratch, while the device owns the GPU, the content caches and the targets and
resolves those caches into session-local tables once at the frame head** — so concurrency needs no coordination on the recording path, only at "once per frame head" and at
"a residency miss".

## 2. Design Points

### 2.1 A session is a "frame driver", not a "window" (`G1`)

- **What it is**: `FrameDriver` = who drives frames: the command pool, command buffers, fence, constant ring, descriptor pool, the frame books, and the target it claims.
  The type name stays `RenderFrameExecutor` (a public contract, the vocabulary of `ADR-2026-11-16`); a driver presents to **at most** one window (the window is its target,
  not its identity).
- **Why it needs a name**: `gfx_backend_vsg`'s `api::WindowSession` is **one window, one device, one swap chain** (`WindowSession.hpp`), while the
  **collection / compilation / execution** frame pipeline is a **device-level singleton** in `VsgBackend::Data` (`recorder` / `compiler` / `arena` / `facts` / `packets` /
  `frame` / `driving`, see `VsgBackend.cpp:444`–`534`). That is: **vsg's "session" is a window session, not a frame driver** — the ADR's "vsg is closer" was only half right.
  That correction decides the division of work: what vsg has to move is the **collection scratch**, not the swap chain.
- **Boundary**: a window is still "one live swap chain per platform surface"; a session may have no window (offscreen), in which case it presents to nothing.

### 2.2 Shareable = immutable; not shareable = has a layout or is written inside a frame (`G1` / `G4`)

| Category | Criterion (one sentence decides it) | Example (vk) |
|---|---|---|
| shareable | the key carries a revision and it is **never rewritten** after publication | program modules, pipelines, geometry streams, textures, samplers, layouts |
| not shareable | it is **written** during a frame's lifetime | command pools, fences, constant rings, descriptor pools, target attachments, swap chains |

Everything not shareable is **one copy per session**, so it needs no lock: ownership turns concurrency into "two threads never touch the same memory".

### 2.3 Frame-head snapshot: the caches are read once at the frame head, and the recording path takes no lock (`G10`)

- **What it is**: at the start of a frame the session takes the device caches' **exclusive lock once** and resolves the identities this frame will use — the frame value already
  lists them: each `DrawItem`'s geometry / program / material description and each pass' target — into **session-local tables** (`FrameSnapshot`: identity → handle +
  state). Recording then reads only the session's own tables and **never touches the device caches again**.
- **Why not "take a read lock per lookup"**: a 2000-draw frame at 20–40 ns per `std::shared_mutex` read lock is 40–80 µs, 5–9% of this machine's host-side frame time
  (~0.9 ms), and all that buys is "reading a table nobody writes" — `G10`'s textbook waste, and the same reason `D2` was rejected.
- **Why the snapshot is not expensive**: its cost is O(identities in this frame), not O(the cache); in the steady state it only does hash lookups and allocates nothing
  (the tables are reserved at the previous size).
- **Why the snapshot takes the exclusive lock rather than a read lock**: a snapshot **writes** (a miss publishes `Uploading`), and `std::shared_mutex` cannot be upgraded;
  "scan under a read lock, drop it, scan again under the exclusive lock" is two traversals, two locks and two race windows (`G3`: a flow that reads in one pass is not split
  into two). One exclusive lock per frame costs two sessions' snapshots ~50 ns of serialisation, a hundred-thousandth of the frame budget — far cheaper than a read-lock state
  machine.
- **Boundary: a miss still takes the lock.** An entry whose state is `Absent` has to be built inside the frame: building **must** publish the entry as `Uploading(owner, token)`
  under the exclusive lock (§2.4) and record the handle back into the session-local table. So "no lock" means exactly: **no lock on the recording path; one at the frame head**.
  The latter is the one at the start of this section and the former is its consequence — the two numbers are stated separately, never merged into "a steady-state frame takes
  0" (that is the recording path's claim, not the whole frame's).
- **Shape**: one `ContentCaches` lock is taken **at the frame head**, and `planDraw` (both overloads) / `resolveTextures` hand the result **by value** to the driver
  (`FrameHead`: one `FrameHead::DrawPlan` per draw call, with the frame-level texture / sampler facts hanging off the head); texture uploads are recorded into the frame's
  command buffer at the frame head as well; `record` / `recordScreen` read only those values. A whole frame's acquisition count is a constant **4** (1 at the frame head +
  1 each for the frame head's and the frame tail's bookkeeping + 1 in `endFrame`) and **does not grow with the draw count** (`VkFrameHeadTest`).
- **vsg's shape**: the same principle lands on vsg's two halves. The **frame head** (`openFrameHead`) resolves, under one content lock, the pass numbering / targets /
  sampled targets / content tracking / target fact tables / storage growth and records the upload baseline; **collection and plan compilation** read only the session scratch
  and take no device lock; the **content phase** is split again: **resolution** (`resolveContent`, one content lock) writes halves / declared sets / stream bind / material
  slot / texture view / block offset / pipeline identity as the driver `FrameHead`'s **values**, **recording** (`FrameHead::record`) only reads those values and takes no
  device lock, and **submission** (`executor.record` + sweep + session submit) takes the lock once more. What remains of the difference from vk is shape (vsg's "values" are a
  group of vsg nodes + offsets, vk's are a group of `FrameHead::DrawPlan`s), and `C3` holds for vsg (see §3.1.9).
- **Recording is owned per driver too**: texture-upload staging and replaced images are grouped by `ContentImages::BatchId` (= `clock_window + 1`), and the frame tail
  releases **only its own** batch; a replaced image is parked to the watermark, because another driver's in-flight frame may still hold the old view.
- **Pipelines are the exception, and they were always on a miss**: a pipeline key includes state (topology / blend / depth), state is resolved per command, and the frame head
  cannot compute it — but the pipeline table is written only on **creation**, which is an ms-scale miss anyway, so one exclusive lock there does not change the conclusion.

### 2.4 Residency's three states: publish or withdraw, and a race means a copy (`G3` / `G4`)

```
Absent ──frame-head miss, claim won──▶ Uploading(owner, token)
                                   │  the owner's frame completes (today: executeFrame returns after the fence was waited)
                                   ├──▶ Resident          (publish: FrameBuilds::publishAll)
                                   └──▶ Absent            (withdraw: the fill or the frame failed, done by the guard)
Resident ──releaseContent──▶ Retiring(epoch) ──device watermark ≥ epoch──▶ release
```

Three rules, and it does not hold without any one of them:

1. **One key has one publisher**: publication is a compare-and-set (`Absent → Uploading(me)`), and the loser **does not publish**.
2. **`Resident` is published only once the fill is provably complete**: today that is "this frame's fence has been waited on"; **preparation's (`prepareFrame`) publication point
   is the return of its transfer-only submission** (`ADR-2026-11-14-prepare-may-submit-transfers.md`); with several frames in flight it is "the retirement queue says that
   frame completed" — the same signal (§2.6).
3. **Publish or withdraw, one of the two**: at the frame tail the owner sets **every** entry this frame built to `Resident` or every one back to `Absent`, through a scope
   guard. There is no third way — an entry stuck in `Uploading` makes **every** session miss for ever (`P1`).

A race (another session sees `Uploading` at its frame head): **build an unpublished temporary copy** and discard it when this frame ends.
`P2` explains why this is not "wait": waiting is the one shape that can lock two sessions **against each other**; copying is bounded (at most `N−1` copies per identity,
and only in the frame where two engines first draw it at the same time). `prepare()` does it during loading, so a real host usually never hits it.

**The three rules are enforced by the types and the wiring** — `ContentCaches::ensureGeometry` + `ContentCaches::FrameBuilds` — not by call sites remembering them. The
frame head asks the table and dispatches on the answer (`Use` takes the cached entry and fills the hole a release punched / `Build` fills and leaves a claim / `Copy` builds
this frame's own copy); `publishAll()` publishes this frame's claims only after the frame fence was waited on, a failed fill is never registered and the guard returns it to
`Absent`, and the destructor collects unpublished claims and copies together. Two drivers' **frame-head resolution** still sits on the caches' one lock (`C3` is about the
recording path); what the wiring decides is "who may build and who copies".

### 2.5 Target claims: one claim per frame, a conflict skips only that pass (`G6`)

- **What it is**: the device holds a `TargetTable` (`RenderTarget*` → `FrameDriverId`). At the frame head each target in **this frame's set** is try-claimed; a conflicting
  target is **not claimed**, the pass that would use it is **skipped**, and one report is made (deduplicated by `(target, episode)`).
- **Why per pass rather than per frame**: the frame's other passes have nothing to do with the conflicting target, and failing the whole frame would amplify a local conflict
  (`G7`: absorb the complexity locally).
- **Why it is not a new failure shape**: the existing semantics already have "the device cannot build this target ⇒ report once and skip its pass"; a claim conflict goes
  through the same report path and the same category (`G6`).
- **Boundary**: a window target needs no claim — a window belongs to one session (`WindowOccupied` / a foreign target is already there). Claims are all released **at the
  frame tail**; the attention goes to `P3`: any early return must pass through that one release point.
- **Rebuild and claims**: a target's rebuild (resize / repair) **may only be done by the session holding the claim**; the other session sees "this frame skips it", not "an image
  half rebuilt".

### 2.6 Cross-session retirement: reuse `DeviceClock` + `RetirementQueue`, invent nothing (`G6` / `G9`)

- **What it is**: vk today waits inside `executeFrame`, so "everything before the next frame can be destroyed at once" and `releaseContent` may only be called between
  frames. With two sessions "between frames" is no longer one instant, and a **device watermark** is needed.
- **What is reused**: `backend/DeviceClock.hpp` is already "each window reports its own completion evidence, and the device watermark is the **minimum**", and
  `backend/RetirementQueue.hpp` is already "park + release when due"; **vsg already uses both** (`d->clock` / `d->retirement`, borrowed per session). vk adopting them means the
  two backends share one completion evidence (`G6`) instead of vk growing a second counter (`G7`).
- **Who is who**: **a frame driver is a clock window** (what `DeviceClock` issues is a `graphics::FrameDriverId`, a u32; an offscreen session registers one too). The three
  spellings are already unified on it: `DeviceClock::WindowId` survives only as one `@deprecated` alias.
- **What release means**: `releaseContent` / `releaseRenderTarget` record **each session's `submitted`** at the time of the call, and the object is destroyed only once the
  device watermark passes it; so those two verbs **may be called while another session is framing**. The contract text at `RenderDevice.hpp:302`–`308` already says exactly
  this ("a device that retires asynchronously parks the free until every session is past it") — this page only makes vk such a device.
- **Threads**: `DeviceClock` and `RetirementQueue` are contracted as "the caller serialises; a multi-threaded device supplies its own lock" (`P4`). The lock lives on the
  **device** (`Impl::clock_mutex`), not inside the two SDK value types: the reference backend (vsg) is single-threaded, and a lock there would be a mechanism with no consumer
  (`G8`). Every place that takes a `clock.timeline()` reference is inside the lock, and the reference does not leave the critical section.
- **Several open frames**: `FrameTimeline` is "one per driver", not "one open frame slot": `begin()` mints a new number each time and `submitted` / `abandoned` close it by token.
  Otherwise the second driver would get the first one's token and the two frames would collapse into one in the watermark comparison.
- **Release is two things**: the cache table **forgets the entry at once** (the residency account's `progress().streams_live` drops immediately and the next frame uploads
  again), while the GPU object waits in the retirement queue for the device watermark — with one slot that is `submitted + 2`, i.e. destroyed at most two frames later. The
  observation point is `Device::retirementNumbers()` (`pending` / `released` / `device_waits`), not the residency account; those two frames are not a defect, they are the
  price of the evidence "the slot has been reclaimed", and a conservatism that several sessions cannot do without.

### 2.7 Readback is the device's verb, not a session's (`G4` / `G6`)

- **The problem**: readback today borrows the frame's `command_pool` + `completion` fence (the ADR's "the fact that blocks" item 2). Once sessions exist it may borrow no
  session's — that would make readback and that session's frame wait for each other.
- **What is done**: readback has **its own** pool and fence (device-level, one lock guarding "one readback at a time") and **waits for the device watermark** (every session's
  `submitted` completed) before copying. This is exactly what vsg does today (`waitDeviceIdle` for **every window** inside `readColorBuffer`, counted in `deviceWaits()`), and vk
  takes the same path (`G6`).
- **Why it is honest**: readback reads "what has already completed", not "what some session just submitted"; waiting for the watermark turns that sentence into a mechanism
  instead of making "a frame in flight" an implicit premise.

### 2.8 The shape of a session's interface: the device lends an executor it owns (`G4` / `G8`)

```cpp
// RenderDevice
virtual RenderFrameExecutor* createFrameDriver();                 // default: return &executor(); (one session per device)
virtual void                 releaseFrameDriver(RenderFrameExecutor& session);  // default: no-op
// RenderFrameExecutor (one new optional entry point, not a requirement)
virtual void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program);  // default: no-op
```

- **The default implementation is "today"**: `createFrameDriver()` returns `&executor()` by default, so **existing backends and every case change not one line**; only vk and vsg
  override it. That makes "concurrent sessions" a **seam** rather than a repository-wide rewrite (`G9`), and does not force a shape a third backend does not need today (`G8`).
- **Ownership**: **the device owns the session**; `createFrameDriver()` lends a reference, contracted as "until `releaseFrameDriver` or the device's `shutdown()`". An engine creates one
  in `setDevice` and releases it when the device changes or it is destroyed. An engine that frames after `shutdown()` asks `initialized()` first (as it already does) and
  reports "the device is not up", **without dereferencing** the reference — `RenderDevice`'s existing "the creator guarantees no frame in flight" is the same ordering
  guarantee (`P5`).
- **The default content program belongs to the session, and its declaration moves** from `RenderDevice` to `RenderFrameExecutor`. The program is the engine's
  (`RenderEngine::forwardProgram()`), it shades a **frame**, and resolution happens at record time — so it is a session setting. vsg has modelled it that way for a while
  (`FrameRecorder::setDefaultContentProgram` belongs to the **recorder**, and the device-level field is only what feeds it). **The second reason to move the declaration is
  the implementation**: `RenderBackend` inherits both `RenderDevice` and `RenderFrameExecutor`, and two virtuals with the same name and signature would make
  `backend->setDefaultContentProgram(...)` ambiguous (`G4`: an interface must not require the caller to write a qualified name). `Device` overrides it and forwards to **its
  own session**, so a caller driving the device directly (`test_vk`) behaves unchanged; `RenderEngine` declares it on **its own session** inside `setDevice`.
- **Windows**: `createSurfaceTarget` stays on the device (a platform surface is not a session's property); the **first** session to claim it becomes its owner, and a second
  session gets the existing "foreign target" refusal. No new parameter is needed.

### 2.9 How the two backends differ in shape (`G5` / `G10`)

| | vk (`gfx_backend_vk`) | vsg (`gfx_backend_vsg`) |
|---|---|---|
| where the frame machinery is | the session's `FrameDriver`: `command_pool` / `completion` / timestamp pool / `Books` / `FrameGate` | the command graph and submission per window (`api::WindowSession` holds the `vsg::Viewer`); the collection scratch on the session's `FrameScratch` (arena / recorder / compiler / facts / input tables / packets / frame), one copy per window and offscreen session |
| where the frame scratch is | `ContentCaches` (the device) holds programs / geometries / pipelines / layouts / images; `FrameScratch` (the session) holds the ring / descriptor pool / set tables / `frame_index`; `ContentDraw` is only the recorder that turns one draw into commands | `BlockStorage` (a slab rotation per frame) + `ContentAssembly::beginFrame`'s sweep (device cache, read-only after the frame-head snapshot); the device gate split into two sides, `FrameDrive`, `FrameHead` (the values, one per driver) + the three phases (resolution takes the lock / recording takes none / submission takes it) |
| completion evidence | `DeviceClock` + `RetirementQueue` (the same pair vsg uses) | `d->clock` / `d->retirement`, borrowed per session |

**Both backends walk the same path**: each session holds its own frame machinery and frame scratch, the device holds the content caches and the targets, and the frame head
resolves them into session-local values; the only difference is where the completion evidence comes from (vk adopts `DeviceClock`, vsg always used it).

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
|---|---|---|---|
| F1 | Session type and creation | the device owns sessions; `createFrameDriver()` lends one and answers `&executor()` by default | landed: the SDK's default seam + the engine holding it; vk has several drivers (each with its own command pool / fence / scratch / default program), **frame bodies really run in parallel**, and a window frame takes the body gate exclusively |
| F2 | Per-session frame machinery | command pool / fence / timestamp pool / frame books / frame gate, one per session | landed: `Device::FrameDriver` owns the frame machinery and the frame body, and `Device` only forwards |
| F3 | Device caches + frame-head snapshot | the caches are read once at the frame head; the recording path takes no lock | landed: `ContentCaches` / `FrameScratch` are separate; plans go by value; the observation point is `contentCacheCounters().cache_lock_acquisitions` (a constant 4 per frame) |
| F4 | Residency's three states | `Absent → Uploading → Resident`, publish or withdraw, a race means a copy | landed: the state machine + `ensureGeometry` asking the table (`Use`/`Build`/`Copy`); `FrameBuilds` publishes when the frame completes, a racing frame builds a temporary copy, and failure and release each have an owner (see §3.1.4) |
| F5 | Target claims | one claim per frame; a conflict skips only that pass | landed: the `TargetTable` lease + the frame/preparation wiring; a conflict between two sessions really happens (that pass alone is skipped, reported as `vk.target.claim_conflict`) |
| F6 | Cross-session retirement | vk adopts `DeviceClock` + `RetirementQueue`; a release may be called while another session frames | landed: the clock / the park queue / the watermark / the open-frame rule are all in the tree; release does not wait for a frame (R6) |
| F7 | Device-level readback | its own pool and fence, waits for the device watermark before copying, counted in `deviceWaits()` | landed: `ReadbackStaging` carries its own command pool and fence; the wait is the body gate's "between frames", and each readback counts one `deviceWaits()`; a call from inside a frame body answers `Failed` (see §3.1.7) |
| F8 | The default content program belongs to the session | settable on a session; the device level serves direct callers only; single-session backends behave unchanged | landed: the declaration is on `RenderFrameExecutor`; each of vk's `FrameDriver`s holds its own, and two drivers each draw with their own (the T8 case) |
| F9 | vsg's corresponding shape | the collection scratch moves into the session; the caches get a lock and a snapshot; the completion evidence is already there | landed: the collection scratch is on the session (`FrameScratch`); the executor's frame state belongs to the driver (`VsgDriver` + a per-driver default program); frame-head snapshot + content lock + the device gate split into two sides; the recording path takes no lock (`FrameHead`) |
| F10 | Closing out the contract and the docs | `C21` / `N18` rewritten, three headers' text, the ADR's status, the audit rows | landed: `C21` / `R18` / `N18` say "frames are concurrent per session"; the threading sections of `RenderDevice` / `RenderFrameExecutor` / vk's `Device` agree |

### 3.1.1 F1 Session type and creation

- **Behaviour**: `RenderDevice::createFrameDriver()` returns `&executor()` by default; vk / vsg override it to "build a session". `releaseFrameDriver(RenderFrameExecutor&)` is a no-op by
  default. `RenderEngine` holds a `RenderFrameExecutor*`, creates it in `setDevice` and releases it on a device change or destruction.
- **Constraints** (C1): the **device** owns a session; an engine only borrows it.
- **Requirements** (R1): `createFrameDriver()` may be called before `initialize()` (the creator initializes the device and the engine attaches first).
- **Acceptance** (T1): a single-session backend (the default implementation) runs `test_graphics` with no behaviour change.
- **Caveat** (N1): `shutdown()` leaves no dangling session behind — see `P5`.

### 3.1.2 F2 Per-session frame machinery

- **Behaviour**: `FrameDriver` holds `command_pool` / the `completion` fence / the timestamp pool / `Books` / `FrameGate` / its own set of `ReportOnce`s; `Device::executeFrame`
  becomes "forward to the target session".
- **Constraints** (C2): the frame machinery exists **only** in a session; the device has no second `command_pool`.
- **Requirements** (R2): `executor()` stays, with its meaning narrowed to "the device's own session" (`G6`, the same shape as `setDiagnosticsRoute`).
- **Acceptance** (T2): `test_vk`'s 102 cases are green on a tree with "one session only" (a pure move, zero behaviour change).
- **Caveat** (N2): `PrepareReport` / `FrameReport` are a session's answers; the device-level one answers for its own session.

### 3.1.3 F3 Device caches + frame-head snapshot

- **Behaviour**: `ContentCaches` (the device) holds programs / geometries / pipelines / layouts / the image cache / the allocator; `FrameScratch` (the session) holds the ring,
  descriptor pools, set tables, per-frame counters and the `FrameSnapshot`.
- **Constraints** (C3): the recording path **must not** take the device caches' lock; every device-cache read happens at the frame-head snapshot.
- **Requirements** (R3): a steady-state frame's device-cache lock acquisition count is **0** (observable: one counter + one case).
- **Acceptance** (T3): a 2000-draw frame has a lock count of 0; a frame with misses has a count ≤ the miss count + 1.
- **Caveat** (P6): a snapshot that misses an identity = that draw misses inside the frame; **correctness is unaffected**, it just takes the lock once more.

### 3.1.4 F4 Residency's three states

- **Behaviour**: an entry's state is `Absent` / `Uploading(owner, token)` / `Resident` / `Retiring(epoch)`; publication is a compare-and-set; a racing session builds a
  temporary copy; the owner **publishes or withdraws** at the frame tail.
- **Constraints** (C4): only a `Resident` entry may be used by a non-owner; an `Uploading` entry equals `Absent` + a copy for everyone else.
- **Requirements** (R4): entries are immutable (the key carries a revision); no `Uploading` may persist.
- **Acceptance** (T4): two sessions first-drawing the same geometry ⇒ no crash, both pictures right, uploads ≤ 2; a mutation (removing the withdrawal) ⇒ the case goes red.
- **Caveat** (P1): an entry stuck in `Uploading` is a **device-wide** scar, not one draw's failure.

**The implementation shape**: the state machine is `internal/ResidencyClaims.hpp` (no Vulkan type), one entry per key: `admit(key, session)` answers `Use` / `Build` (with a
move-only `Claim`) / `Copy(owner)`; `Claim::publish()` is the owner's declaration that "the fill is complete", and an unresolved entry falls back to `Absent` when the `Claim` is
destroyed (rule 3 is enforced by the type, not by call sites remembering it); tokens increase monotonically and are never reused, so an old claim cannot resurrect an entry
after `forget` (`releaseContent`).
**The wiring lands the three rules on the cache**: `ContentCaches::ensureGeometry` asks the table before uploading — `Use` takes the cached entry (filling the hole
`releaseStreams` punched), `Build` fills the cached entry and hands the `Claim` to this frame's `ContentCaches::FrameBuilds`, and `Copy` builds a **detached** geometry outside
the table that serves this frame only. `FrameBuilds` is a frame-scoped value: `publishAll()` publishes the fills that worked **after the frame fence was waited on** (a failed
fill **never registers a claim**: the failure site lets the guard return it to `Absent`, so `Resident` can never be reached by a failed fill), and the destructor withdraws
unpublished claims and destroys the temporary copies **immediately** (a copy never enters the table, only this frame could name it, and this frame is over; its upload counts in
`stream_builds`, and destroying it is not a release, so it does not count in `stream_releases`). Preparation's publication point is the return of its transfer-only submission;
`releaseGeometry` also `forget`s the key, and `destroy` clears the table.
**Boundary**: two drivers' frame-head resolution still sits on the caches' one lock (`C3` asks for a lock-free recording path); vk's fill today is a host-visible memcpy, so the
fill is provably complete when `fillStreams` returns — when uploads move to device-local memory the publication point stays the same `publishAll`, only the evidence becomes a
fence.

### 3.1.5 F5 Target claims

- **Behaviour**: the frame head claims this frame's targets; a conflict ⇒ skip that pass and report once; the frame tail releases them all.
- **Constraints** (C5): one target has one session at a time; a claim is a **lease**, not a lock.
- **Requirements** (R5): a target's rebuild is done by the claim holder only.
- **Acceptance** (T5): two sessions drawing one target ⇒ one session's pass is skipped and reported once (and only once).
- **Caveat** (P3): any early return must pass through that one release point (a scope guard).

**The implementation shape**: `internal/TargetTable` is one table — the map, the shape/build and the claim bookkeeping all belong to it; `lease(driver)` issues a frame-scoped
`Lease`, `Lease::claim(target)` answers false on a conflict, and the destructor returns every claim this frame took (`P3` enforced by the type); `release(target)` does "the entry
leaves the map + the record goes to the park" in one go, and `drain()` clears records and leases in one go. `FrameDriver::executeFrame` and `prepareFrame` both claim **before
`ensure`** (a rebuild is the holder's verb, `R5`); a conflict skips that pass only and reports `vk.target.claim_conflict` **once per (target, episode)**, where the episode ends
when the frame or preparation finishes without a conflict. A window target is not claimed (`createSurfaceTarget` already guarantees one window, one session).

### 3.1.6 F6 Cross-session retirement

- **Behaviour**: vk adopts `DeviceClock` (a session = a clock window) + `RetirementQueue` (device level); a release records an epoch, and destruction happens once the
  watermark passes it.
- **Constraints** (C6): destruction may only happen after the device watermark; `vkDeviceWaitIdle` must not be traded for correctness (`G10`: that moves the cost from the
  design to every frame).
- **Requirements** (R6): `releaseContent` called while another session is framing is **safe and does not block until that frame ends** (it only parks).
- **Acceptance** (T6): a release overlapping an in-flight frame ⇒ nothing is destroyed before the copy completes (a counter + one case); `deviceWaits()` does not rise.
- **Caveat** (P4): `DeviceClock` / `RetirementQueue` each need a small lock, taken only at the frame head / frame tail.

### 3.1.7 F7 Device-level readback

- **Behaviour**: readback uses its own pool and fence; it waits for the device watermark before copying; each wait counts one `deviceWaits()`.
- **Constraints** (C7): readback must not borrow any session's frame machinery.
- **Requirements** (R7): after two sessions have each run several frames, readback reads the **latest completed** frame (the same semantics as today's single session).
- **Acceptance** (T7): two sessions alternately drawing different targets with readbacks interleaved ⇒ each picture is its own last completed frame.
- **Caveat** (P7): readback is "the slowest verb"; do not put it in the frame budget, and its wait must be observable.

**The implementation shape**:

- **Its own machinery**: `ReadbackStaging` owns **its own command pool and fence** (one set per copy; creating and destroying them is noise against a GPU round trip, `G10`),
  and the readback path never touches `FrameDriver`'s command pool or fence (C7 holds structurally).
- **The wait is the device's "between frames"**: it is expressed as the body gate's EXCLUSIVE side — on the wall it still waits for the **in-flight frame** (that is the
  completion evidence), but it is not some session's machinery; **each readback counts one** `deviceWaits()`, the same reading as the reference backend's `waitDeviceIdle` (`G6`).
- **A refusal does not stop the device**: refusals that can be answered before waiting (unknown / unbuilt target, out-of-range attachment, unsupported format, borrowed depth)
  are answered in phase one; phase two **re-resolves** inside the gate, because a frame may have rebuilt or replaced the target while waiting.
- **A call from inside a frame body**: when a diagnostic sink / hook calls back into this device from inside a frame body, the read answers `Failed` instead of waiting for
  itself — `FrameBodyDepth` (the in-thread frame-body depth) turns that class of deadlock into an answerable refusal; `Device.hpp` states this contract.
- **External synchronisation**: `vkQueueSubmit` / `vkDeviceWaitIdle` go through the device's `queue_submit` lock (the ADR's hard-constraint table requires it; two drivers
  submitting at once is a Vulkan external-synchronisation violation).

**Cases**: `VkMultiSessionTest` — two drivers alternately draw, and interleaved readbacks each get their own last frame (T7); a readback while a frame is stopped by a sink
must **wait**, then read that frame's pixels and count +1; a read from inside a frame body answers `Failed` and touches no host vector.

### 3.1.8 F8 The default content program belongs to the session

- **Behaviour**: the `setDefaultContentProgram` declaration moves from `RenderDevice` to `RenderFrameExecutor` (default no-op; `Device` overrides it and forwards to the
  device's own session), and the engine declares it on **its own session** in `setDevice`; the device-level one serves only a caller that drives the device directly.
- **Constraints** (C8): when two engines declare different default programs, **each draws with its own** (no more "the last declaration wins").
- **Requirements** (R8): a single-session backend behaves byte for byte as today.
- **Acceptance** (T8): two sessions declaring different default programs ⇒ each frame's pixels come from its own program; a mutation (sharing one device-level slot) ⇒ red.
- **Caveat** (N3): this replaces `graphics-threading.md`'s `N18` (one device-level slot), see F10.

### 3.1.9 F9 vsg's corresponding shape

- **Behaviour**: `recorder` / `compiler` / `arena` / `facts` / `light_scratch` / `input_*` / `packets` / `frame` / `driving` move from `VsgBackend::Data` into the
  session; the device caches (`store` / `halves` / `images` / `uploads` / `targets` / `passes` / `pool` / `storage`) get a lock + snapshot per F3.
- **Constraints** (C9): `api::WindowSession`'s **window** duties do not move (the swap chain, the `Viewer` and `WindowTarget` are already per window).
- **Requirements** (R9): `BackendParityTest`'s byte-for-byte comparison still holds with two sessions.
- **Acceptance** (T9): `test_vsg` green + the two-session cases; the comparison unchanged.
- **Caveat** (P8): `d->driving` is the **only** record of "which picture belongs to which session"; once it moves there may be no second copy.

**vsg's collection scratch**:

- **`FrameScratch`** (`internal/FrameScratch.hpp`): arena / recorder / compiler / `facts` / `light_scratch` / `input_*` / `packets` / `frame` collected into one type; it
  borrows the device's `Diagnostics` and `Observe` (both outlive any session).
- **Ownership**: `api::WindowSession` (a window) and `api::OffscreenSession` each hold one, created in `initialize()` and destroyed with the session;
  `WindowSessionOptions` / `OffscreenSessionOptions`' `observe` (null = the session brings its own, so a test that builds a session on its own still works).
- **The device lends one handle only**: `Data::scratch` points at the driving session's copy while a frame is open, and `ScratchCloser` clears it **at every exit**;
  `commitFrameThrough` takes the same copy back from the session parameter — no copy of the collection scratch lives on the device.
- **A device-level setting covers every session**: `setDefaultContentProgram` and `releaseRenderTarget` apply to every session's scratch; opening a frame tops up the default
  program once more (for sessions that started after the setting), which is one pointer copy.

**vsg's driver**:

- **`TargetTable`**: the target table and identity resolution are one — the device's single table (one row per identity: description, object, the owner's tag, and the
  executor's "not applied" episode); each driver's executor borrows it, so two drivers resolve against the same identity table (`P8`: there is no second copy). A
  separately constructed executor (a test) brings its own, and `adopt` registers an object someone else built (non-owning).
- **`VsgDriver`** (`internal/VsgDriver.hpp`): the frame driver `createFrameDriver()` hands out, and the `RenderFrameExecutor` implementation; it owns a `VsgExecutor`
  (`window_` / `window_recorded_` / `recorded_` / `profile_entries_` / `skipped_`) and its own default content program; the device owns it, `releaseFrameDriver` destroys it, and
  `shutdown` takes it away.
- **The device itself uses one path too**: `VsgBackend::executeFrame` / `prepareFrame` are two lines of forwarding, and the truth is in
  `executeFrameFor(executor, ...)` / `prepareFrameFor(executor, ...)`; `Data::driving_executor` is set when a frame opens and cleared at every exit, like `Data::scratch`, and
  `presentWindow` takes this frame's driver back from it.
- **The default program belongs to the driver (C8)**: `VsgExecutor` holds the object and the text; the device-level `setDefaultContentProgram` sets the device's own driver
  only, and each frame hands **the driver's own** text to the driving session's recorder when it opens; the program is still registered in the device store (the content tables
  are the device's).
- **Cases**: `VsgBackendTest.TwoDriversKeepTheirOwnDefaultProgram` — two drivers' default programs do not overwrite each other and the device's own driver is not polluted;
  on one offscreen target A draws red and B draws green; after A is released B still draws its own green.

**vsg's frame-head snapshot and content lock**

- **The device gate**: vsg's device gate is **`graphics::FrameGate`** (the same type vk uses, one instance per device) — a frame body takes the **shared** side (two drivers'
  frame bodies may be inside at once) and a host verb takes the **exclusive** side (initialize / shutdown / readback / `frameReport` / window sets / resize). A host verb called
  from a frame-body thread gets an **empty lease** and answers refused (readback answers `InsideFrame`; a content verb answers false / does nothing and reports one
  `vsg.device.reentry`), and never waits for itself. Diagnostic routing and the episode locks are **recursive**, because a sink may itself call back.
- **Frame state belongs to the driver**: `VsgExecutor::FrameDrive` (scratch / this frame's window index / the upload baseline / the previous frame's delta) is the only home
  of frame state; the device's `Data::last_uploads` keeps a copy of "the last frame" for `frameReport()` only. `presentWindow` gets this frame's driver back through the
  **thread-local in-flight frame** (`FrameInFlight`) — a window target's `present()` has no frame parameter, and a single device-level copy would hand one frame's plan to
  another driver's present.
- **The frame-head snapshot (vsg's shape)**: `openFrameHead` resolves, under **one** content lock, every **device fact** this frame will read — each pass' number
  (`PassRegistry::adopt` writes `FrameScratch::pass_ids`), every host target it draws (`adoptHostTarget`), every target it samples (`TargetTable::observe`), the content its
  commands name (`ContentStore::track`), a copy of the target fact tables (`FrameScratch::facts`), the staging sweep and the block storage's growth. **Collection**
  (`collectFrame`) and **plan compilation** (`closeFrameBooks`) then read only the session's own scratch, so the reports inside them (the plan compiler's "this input / target
  cannot be resolved") reach the sink **holding no device lock**.
- **The content phase is the device's only serial section**: `commitFrameThrough` does `applyTargetPlans` / material arena growth / `recordContent` (assembly opens the frame and
  records) / `executor.record` / the sweep / the session submission under **one** content lock, and publishes this frame's upload delta and counters at every exit
  (`Data::last_uploads` / `last_counters`).
- **One frame claim per session**: a window slot and an offscreen session each have a `frame` mutex, held by the frame body until it ends — two frames of the same window /
  offscreen session **wait** (the same semantics as the old device gate: two engines both drawing offscreen is a supported shape and cannot be refused), while **different
  windows never wait for each other**.
- **Counters belong to the session** (`WindowSessionOptions::observe` no longer borrows the device's `Observe`): the recorder **resets** the counters when a frame opens
  (`FrameRecorder::beginFrame`), and a shared copy would let two drivers overwrite each other; `frameReport()` refers to the copy of the frame that ended **last**
  (`Data::last_counters`), while retention stays the device's.
- **A lazily built device piece takes the same lock as the frame head**: `ensureOffscreenSession`'s lazy creation happens under the content lock — otherwise, when two drivers
  run an offscreen frame for the **first** time concurrently, the loser would overwrite a half-built viewer (a segfault).
- **`ContentStore`'s rows and tables are serialised by the content lock, and recording never touches them**: recording reads only the values resolved at the frame head
  (`FrameHead`), so `C3` holds.
- **Cases**: `ACompilingFrameDoesNotWaitForAnotherDriversFrame` (with one frame stopped by a sink inside **compilation**, another driver's whole frame completes),
  `TwoDriversOnTwoThreadsEachKeepTheirOwnPicture` (two threads, 3 frames each, each drawing with its own default program, and each readback is its own colour),
  `AReadbackFromInsideAFrameBodyAnswersInsideFrame` (a read inside a sink answers `Failed`, one `vsg.readback.in_frame`).

**vsg's lock-free recording path**

- **The content phase is split into three**: `commitFrameThrough` is **resolution** (one content lock: `applyTargetPlans`, material arena growth, `resolveContent`),
  **recording** (no device lock: `FrameHead::record` turns values into vsg nodes), and **submission** (the content lock again: `executor.record`, the sweep, the session
  submission, the upload delta and the counter publication). Recording reads no device table, so `C3` holds for vsg.
- **`internal/FrameHead`: one frame's resolved values**. One `DrawPlan` per draw call: the recorder of the half it hit (`ContentDraw*`), the resolved `ContentDraw::Draw` /
  `ScreenDraw` (identity + pipeline + variant id + dynamic state + geometry span count), the block bindings, the push blocks, the vertex / index bindings; one slot per pass,
  carrying its own `core::StateRegistry` (the state memory that spans the resolution and recording phases). It is the vsg counterpart of vk's `FrameHead::DrawPlan`:
  **a value that owns the objects it names** (all `ref_ptr`s), with one extra lifetime pin: `CompiledPrograms::pin()` lends the halves' producers to the plan, so another
  driver's frame-head sweep cannot release a recorder this frame still needs (the retirement park knows nothing about "an unsubmitted frame has not handed it over yet").
- **Two APIs, resolution half and recording half**: `ContentDraw::plan(key)` (resolution, taking the layer and the variant pool, under the content lock) and
  `ContentDraw::record` / `recordScreen` (recording, binding only the resolved pipeline, touching neither the layer nor the pool); `core::StateRegistry` therefore gained a
  `resolve(variant, …)` overload — recording uses a value instead of looking the pool up again, which is also the precondition for two drivers' recordings to really run at
  the same time. `ContentPass::resolve` is the side-effect-free resolution half; `record` is just the "resolve + record" convenience entry (tests and
  `ContentAssembly::record` use it).
- **Blocks are written at the frame head too**: the offsets of the view / lights / shadow / draw / material blocks are written into this frame's slab during resolution
  (`BlockStorage`'s rotation is a frame-head matter anyway), and recording carries only offsets — so another driver's frame head rotating the ring, sweeping halves or growing
  the arena mid-frame does not disturb this frame's plan.
- **The seam and the case**: `BackendContentAccess::setRecordSeam` (a test seam at the start of recording, holding no content lock);
  `VsgBackendTest.ARecordingFrameDoesNotStopAnotherDriversContent` — with one frame stopped inside **recording**, another driver's whole frame (frame head + recording +
  submission) completes, and the stopped frame's readback pixels are still its own (the plan's values stay valid across another frame's frame head).

### 3.1.10 F10 Closing out the contract and the docs

- **Behaviour**: the threading sections of `RenderDevice.hpp` / `RenderFrameExecutor.hpp` / the plugin device headers are rewritten; `C21` / `N18` / `R18` are rewritten to say
  "two sessions may run concurrently"; the ADR records that the change is landed; the audit's `A6` / `A7` / `A11` rows are ticked off.
- **Constraints** (C10): the contract text must match what the **code actually guarantees** (`G4`); if it cannot, write "serial" first.
- **Requirements** (R10): `check_ai_docs.py --strict` and `check_design_docs.py` report 0 findings.
- **Acceptance** (T10): both documentation gates report 0 findings.
- **Caveat** (N4): `D13` (a global lock) and `D2` (per-entry locks) stay rejected in the documentation, so that no "temporary implementation" revives them.

## 4. Public Interface

### 4.1 `RenderDevice` (SDK)

| Interface | Contract | Notes |
|---|---|---|
| `RenderFrameExecutor* createFrameDriver()` | lends a new frame driver; the device owns it; by default `return &executor();` | the full signature is in `RenderDevice.hpp` |
| `void releaseFrameDriver(RenderFrameExecutor&)` | gives it back; no-op by default; `shutdown()` takes every session with it | the same "lend / give back" shape as `setDiagnosticsRoute` |
| ~~`setDefaultContentProgram`~~ | **moved out** of this class, see 4.2: it is a frame's setting, and two virtuals with the same signature would be ambiguous | `G1` / `G4` |

### 4.2 `RenderFrameExecutor` (SDK)

| Interface | Contract | Notes |
|---|---|---|
| `executeFrame` / `prepareFrame` / `frameReport` | **per session**: one session's thread at a time; **different sessions may run concurrently** | the contract text is rewritten (F10) |
| `void setDefaultContentProgram(intrusive_ptr<const ShaderProgram>)` | what this session's missing content is shaded with; no-op by default; `Device` overrides it and forwards to the device's own session | moved here from `RenderDevice` |

### 4.3 `gfx_backend_vk` (internal)

| Type | Responsibility | Notes |
|---|---|---|
| `FrameDriver` | frame machinery + frame scratch + frame gate + frame books + the claim set | its identity is a `graphics::FrameDriverId` (issued by `DeviceClock`) |
| `ContentCaches` | programs / geometries / pipelines / layouts / images / allocator + one exclusive lock | split out of `ContentDraw` |
| `FrameScratch` | ring / descriptor pools / set tables / `FrameSnapshot` | split out of `ContentDraw` |
| `ResidencyClaims` | the three states + publish / withdraw / retire | reuses `RetirementQueue`'s epoch semantics |
| `ContentCaches::FrameBuilds` | one frame's admissions: the `Use` hits, the `Build` claims, the `Copy` temporaries | a frame-scoped value; `publishAll` is called once the frame completes and the destructor collects the rest |
| `TargetTable` | target → session; try-claim / release | a conflict reports once, deduplicated by (target, episode) |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
|---|---|
| C1 | The **device** owns a session; an engine only borrows it (`createFrameDriver` / `releaseFrameDriver`). |
| C2 | The frame machinery (command pool / fence / ring / descriptor pools / frame books) exists **only** in a session. |
| C3 | The recording path **must not** take the device caches' lock; device-cache reads happen at the frame-head snapshot only. It holds in both backends today: both sides' recording reads only the values resolved at the frame head. |
| C4 | Only a `Resident` entry may be used by a non-owner; `Uploading` equals `Absent` + a temporary copy for everyone else. |
| C5 | One target has one session at a time; a claim is a lease, and the frame tail must give it back. |
| C6 | Destruction may only happen after the **device watermark**; device idleness must not be traded for correctness. |
| C7 | Readback borrows no session's frame machinery; it waits for the watermark and counts the wait. |
| C8 | The default content program belongs to the session; when two engines declare different programs each draws with its own. |
| C9 | vsg's window duties (swap chain / `Viewer` / `WindowTarget`) do not move because of sessions. |
| C10 | The contract text must match what the code actually guarantees; if it cannot, write "serial" first. |

### 5.2 Requirements (R#)

| # | Requirement |
|---|---|
| R1 | `createFrameDriver()` may be called before `initialize()`. |
| R2 | `executor()` stays, meaning "the device's own session". |
| R3 | A steady-state frame's device-cache lock acquisitions on the **recording path** are 0 (apart from the frame-head one; the whole frame's count does not grow with the draw count, and there is a counter and a case) |
| R4 | Residency entries are immutable; no `Uploading` may persist (publish or withdraw). |
| R5 | A target's rebuild is done by the claim holder only. |
| R6 | A release may be called while another session frames, and does not block until that frame ends (the wait's upper bound is one cache operation; T6) |
| R7 | Readback reads the "latest completed" frame. |
| R8 | A single-session backend behaves byte for byte as today. |
| R9 | `BackendParityTest`'s byte-for-byte comparison still holds with two sessions. |
| R10 | The documentation gates report 0 findings. |

### 5.3 Invariants (N#)

| # | Invariant |
|---|---|
| N1 | An identity has one publisher; publication is a compare-and-set. |
| N2 | Every `Uploading` entry is either `Resident` or back to `Absent` at the frame tail. |
| N3 | A target has one claim holder at a time. |
| N4 | There is no device-level lock on the recording path (`N1`–`N3` therefore need no check on the hot path); the frame-head one is the snapshot's mechanism, not the hot path |
| N5 | Device loss holds for **every** session at once (they share one `VkDevice`, which no failure classification can separate). |
| N6 | Capabilities (`capabilities()`) are a device-level read-only fact, askable from any session at any time. |

## 6. Caveats and Traps

### 6.1 P1 An entry stuck in `Uploading`

- How it fails: the owner's frame fails before publishing (allocation refused, `vkBeginCommandBuffer` refused, an early return) and the entry stays in `Uploading`. From
  then on **every** session treats it as "someone else is uploading" and copies — every frame copies: correct, but it never converges.
- How to detect it: the residency account's "temporaries per frame" does not return to zero; or an entry sits in `Uploading` for longer than one device watermark.
- How to prevent it: the frame tail's scope guard publishes or withdraws **unconditionally** (`N2`), and a Debug assertion says "when the frame ends, every entry this
  session built is decided".

### 6.2 P2 Waiting instead of copying on a race

- How it fails: A waits for B's `Uploading` while B's frame needs something A is drawing ⇒ they wait for each other and both sessions die.
- How to detect it: the two-session case hangs (it does not fail).
- How to prevent it: **copy only, never wait**; an `Uploading` entry has no wait path for a non-owner (the decidable form of `C4`).

### 6.3 P3 A leaked claim

- How it fails: after an early return in some pass (unknown target, skipped) the claim is not given back, and the next session skips that target for ever.
- How to detect it: T5's second session skips the same target two frames in a row.
- How to prevent it: the claim set is released by the session's frame guard (RAII), not by hand inside the pass loop.

### 6.4 P4 Two threads touching the watermark and the park queue

- How it fails: `DeviceClock::windowCompleted` and `RetirementQueue::retire/advance` are documented as "single-threaded" today; with two sessions two threads call them at
  once ⇒ a data race (visible to TSan, or worse: an early release).
- How to detect it: TSan; or `pending()` rising again after a release.
- How to prevent it: one small lock each, taken **only at the frame head / frame tail**; the ordering rule for `retire` and `advance` (park first, then advance) is written
  into the session's frame tail.

### 6.5 P5 `shutdown()` and a live session

- How it fails: an engine still holds a session reference while the creator calls `shutdown()` on the device; the next `frame()` dereferences a dangling pointer.
- How to detect it: ASan's UAF; or a Debug assertion "session's device is up".
- How to prevent it: the engine's `frame()` asks `device_->initialized()` first (as it does); `shutdown()` marks sessions dead and clears the reference; the contract states
  "the creator guarantees no frame is in flight at shutdown".

### 6.6 P6 A snapshot that misses an entry

- How it fails: the snapshot covers only "the identities obvious in the frame value" (a command's geometry / program) and misses pass-level or implicit ones (a shadow map's
  visibility, a screen draw's source target) ⇒ a miss and a lock every frame.
- How to detect it: R3's lock count is not 0 in the steady state.
- How to prevent it: the snapshot's input is the same frame value recording uses (one traversal, two results), not a second enumeration written by hand.

### 6.7 P7 The illusion that "readback is cheap"

- How it fails: readback is treated as a cheap verb and put on the per-frame path; it waits for the device watermark, which eats the concurrency gain.
- How to detect it: `deviceWaits()` grows linearly with the frame count.
- How to prevent it: readback stays off the frame path (the SDK documentation already says "readback is a host action"); a case asserts that a steady-state frame with two
  sessions does not raise `deviceWaits()`.

### 6.8 P8 A second copy of `d->driving`

- How it fails: after vsg's `driving` moves into the session, the device still keeps a "last driver", and the two disagree.
- How to detect it: a frame's pass uses another session's window target.
- How to prevent it: `driving` exists **only** in the session; the device's present path looks the session up from `PresentTarget` (the facade already forwards by index).

### 6.9 P9 A nested class cannot reach its base class' `protected` members

- How it fails: `Device::FrameDriver` is a **nested** class of the device and has access to `Device`'s private members — but `reportDiagnostic` is a **protected** member of
  `RenderDevice`: calling it **through an object** is allowed only to a derived class, and a nested class does not count (C++'s rule on the requestor's scope). The frame body
  has ~25 such calls, and moving it as it is produces a wall of compile errors.
- How to detect it: the compiler answers "call to non-static member function ... from nested type".
- How to prevent it: the frame body does not ask the device directly but goes through the three faces the device assembles on `Impl` (`report` / `ensure_target` /
  `note_device_lost`) — they happen to be called only "once per pass" and "once on failure", so they are not expensive even off the hot path (`G10`), and they turn "the
  decisions only the device can make" into an injectable face (which helps tests too).

## 7. Rejected Options

| # | Option | Reason | ADR |
|---|---|---|---|
| D1 | a global device lock around every verb | `G7`: the complexity is not absorbed but turned into "every call may block"; `G1`: the lock's granularity has nothing to do with the resource | ADR option E |
| D2 | one `shared_mutex` per entry | `G10`: what is read is an immutable record, and a per-entry lock only pays an atomic on the 2000-draw path; `G8`: there is no writer to coordinate | ADR option F |
| D3 | waiting instead of copying on a race | `G4`: it can deadlock (`P2`); a bounded copy is cheaper | ADR §3 |
| D4 | a session = a window (keeping vsg's shape) | `G1`: an offscreen session is not a window; `G5`: tying one concept to another makes both grow crooked | this page §2.1 |
| D5 | the engine allocating the session object itself | `G1`: the GPU objects' owner must be the device; `G10`: the engine should not be responsible for a command pool's lifetime | this page §2.8 |
| D6 | readback borrowing some session's frame machinery | `G4`: readback reads "what has completed", unrelated to "who just submitted"; borrowing either one makes them wait for each other | this page §2.7 |
| D7 | renaming `DeviceClock::WindowId` to `SessionId` now | `G8`: no consumer needs that spelling; a type alias first (`SessionId = WindowId`), the rename left for when a second API family exists (**superseded by `ADR-2026-11-16-redesign-seam-and-vocabulary`**: the three spellings are unified as `FrameDriverId`, and `WindowId` survives only as one `@deprecated` alias) | this page §2.6 |
| D8 | landing it in one go (no stages) | `G11`: a half-split tree is worse than an unsplit one — every step must be verifiable on its own | ADR §9 |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | a single-session backend changes no behaviour | `build/bin/test_graphics`, `test_vsg` (the default `createFrameDriver`) |
| T2 | `test_vk` is green on a tree with "one session only" | `build/bin/test_vk` |
| T3 | the recording path's cache locks are 0; the whole frame's count **does not grow with the draw count** (measured 4: 1 at the frame head + 2 for the books + 1 at the frame tail) | `test_vk` `VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls` (1 vs 6 commands; the mutation 6 → 10 goes red) |
| T4 | two sessions first-drawing the same geometry at once: no crash, both pictures right, uploads ≤ 2 | `test_vk` `VkResidenceWireTest.TwoDriversFirstDrawingOneGeometryShareOneEntryAndAtMostOneCopy` (really two sessions on two threads: claims 1 / published 1 / copies ≤ 1 / both pictures red / the next frame uploads nothing), `AKeyAnotherDriverIsFillingIsCopiedForThisFrameAndDiscarded` (a fake lease: a copy, exactly two streams, `used_bytes` back in place), `ARefusedBuildIsWithdrawnInsteadOfPublished`, `AReleasedGeometryIsForgottenSoTheNextFrameMayBuildItAgain`; mutations 3/3 red (not asking the table / publishing a failed fill / not forgetting on release) |
| T5 | two sessions drawing one target: one pass skipped + one report | a new `test_vk` case |
| T6 | a release overlapping an in-flight frame: the release does not wait for the frame to end, the frame still completes with the handles it resolved, the object parks to the watermark, and `deviceWaits()` does not rise |
  `VkMultiSessionTest.AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses` (mutation: the release takes the body gate again ⇒ red) |
| T7 | alternately drawing different targets + interleaved readbacks: each reads its own latest completed frame | a new `test_vk` case |
| T8 | two sessions with different default programs: each draws its own | a new `test_vk` case + a mutation (sharing one slot ⇒ red) |
| T9 | with two sessions `BackendParityTest`'s byte-for-byte comparison is unchanged | `test_vsg` |
| T10 | two sessions, one thread each, N frames each: both pictures are right and reentry is refused; **the frame bodies really overlap in time** (with one frame stopped by a sink another whole frame completes) | `VkMultiSessionTest.TwoDriversOnTwoThreadsEachKeepTheirOwnPicture` (each with its own texture),
  `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame` (mutation back to an exclusive gate ⇒ red); the "same-thread reentry is refused" assertion is kept |
| T11 | the data race has tool evidence (run, 2026-11-16: three suites, 0 reports) | `scripts/tsan_check.sh` (`test_vk` / `test_graphics` / `test_vsg` subsets) |
| T12 | the documentation matches the code | `python3 scripts/check_ai_docs.py --strict`, `python3 scripts/check_design_docs.py` |
| T13 | vsg: with one frame stopped by a sink inside **compilation** another driver's whole frame completes; with one frame stopped inside **recording** another driver's whole frame (frame head + recording + submission) completes and the stopped frame's pixels are still right; two threads each with their own target draw their own; a read inside a frame body answers `InsideFrame` | `test_vsg` `VsgBackendTest.ACompilingFrameDoesNotWaitForAnotherDriversFrame` (mutation: the device gate back to exclusive ⇒ a 5 s timeout, red), `VsgBackendTest.ARecordingFrameDoesNotStopAnotherDriversContent` (mutation: the three phases back to one lock ⇒ a 5 s timeout, red), `TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`, `AReadbackFromInsideAFrameBodyAnswersInsideFrame` |

## 9. Status and Evolution

### 9.1 Current status

The shape this page describes is entirely landed in both backends, which share one contract. How it landed, the cases and mutation evidence for each step, and that day's
TSan reading are history and live in `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`'s "implementation status" and in git; this page keeps only what
still binds today's code: §2's design points, §3's features, §4's interfaces and §5's constraints.

### 9.2 Known gaps / backlog

- `DeviceClock`'s verbs are still `addWindow` / `windowCompleted` (the identity type is already `FrameDriverId`); the rename is left for a later version.
- **How to read T3 correctly**: a whole frame takes a fixed 4 acquisitions (1 at the frame head + 2 for the books + 1 at the frame tail), and the case is `VkFrameHeadTest`;
  "a steady-state frame takes 0" is about the recording path, not the whole frame (see §2.3 and §8 T3).

### 9.3 Compatibility / migration

The migration is complete and no step is left. The division into steps, their order, and each step's acceptance and rollback conditions belong to the decision record — see
`../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` §9 ("landing in two steps" and "implementation status"). Two facts that still bind today's code stay
on the design side: release is two things (§2.6), and readback waits for the device watermark and counts it (§2.7). Trigger-gated follow-up work is in §9.2 and
`../backlog/graphics.md`.

## 10. References

### 10.1 Code

- the contract: `src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`, `RenderFrameExecutor.hpp`, `RenderBackend.hpp`
- the mechanisms: `src/viz/graphics/sdk/vine/graphics/FrameGate.hpp`, `DeviceClock.hpp`, `RetirementQueue.hpp`, `FrameTimeline.hpp`
- vk's present shape: `src/plugins/gfx_backend_vk/src/internal/Device.cpp` (`FrameDriver`'s frame machinery),
  `src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp` (the recorder and the cache wiring)
- vsg's present shape: `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp` (`Data`'s frame-scratch singleton),
  `src/plugins/gfx_backend_vsg/src/internal/WindowSession.cpp` (the window session), `src/plugins/gfx_backend_vsg/src/internal/OffscreenSession.cpp`
- the engine side: `src/viz/graphics/src/RenderEngine.cpp` (`setDevice` / `frame`), `src/viz/graphics/src/RenderPass.cpp` (the single-pass driver)

### 10.2 Documents and ADRs

- `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` (the decision and the options)
- `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` (the shared retirement mechanism)
- `ADR-2026-11-13-one-engine-one-surface.md` (one engine, one surface — this design's premise)
- `graphics-threading.md` `C21` / `R18` / `N18` / `D13` (the contract in use)
- `vk-backend.md` §2.11 (one `Device` object = one session pool), `vsg-backend.md` §2.9 (the backend architecture)
- `graphics-frame-pipeline.md` (frames are values, four layers, one-way edges)

### 10.3 External material

- the Vulkan specification: the external-synchronisation requirements per object (queues / command pools / descriptor pools / pipeline caches / fences) — the ADR's
  "hard constraints" table
- VSG 1.1.16: the device-level `MemoryBufferPools` (`mutable std::mutex`) plus per-task frame state — the ADR's cross-check
- O3DE RHI / wgpu: a thread-safe device and one command list per thread (external material, not verified in this repository)
