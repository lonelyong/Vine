# the device is a pool and sessions own frames: types, ownership and concurrency rules

> **Status**: current — the shape below is landed in the shipped backend (§9.1).
> **Last verified**: 2026-11-23
> **Scope**: the **implementation shape** of what is shared, who owns a frame, how frames run concurrently and how retirement works when one `RenderDevice` is borrowed by
> several engines; the contract's **why** and its landing record are in the ADR, and the threading model's **present** is in `graphics-threading.md`.
> **Sources**: `RenderDevice.hpp` / `RenderFrameExecutor.hpp` / `RenderBackend.hpp` (the contract),
> `graphics/FrameGate.hpp` / `graphics/DeviceClock.hpp` / `graphics/RetirementQueue.hpp` (the mechanisms),
> `src/plugins/gfx_backend_vk/src/internal/Device.cpp` and `ContentDraw.cpp` (the device-level frame machinery)
> carry every session rule this page states; the retired reference backend's half is a record, not a comparison
> (`vsg-backend.md`, tag `retained/vsg-final`).
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
4. **Every rule has a case that can go red**: `test_graphics` / `test_vk` are green, and each invariant this page states names its case or its counter (§8).

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
- **Why it needs a name**: the shape is **one window, one device, one swap chain** - a window is a target, and a device may also drive frames that present to nothing - while a
  frame's **resolution / recording / submission** is state one frame owns: the scratch (arena, recorder, packets) and the values the frame head resolved. So a session is a
  **frame driver**, not a window session: what belongs to it is the **frame scratch**, and the swap chain stays the window's (`D4`).
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
  (`FrameHead`: one `FrameHead::DrawPlan` per draw packet, with the frame-level texture / sampler facts hanging off the head); texture uploads are recorded into the frame's
  command buffer at the frame head as well; `record` / `recordFullscreen` read only those values. A whole frame's acquisition count is a constant **4** (1 at the frame head +
  1 each for the frame head's and the frame tail's bookkeeping + 1 in `endFrame`) and **does not grow with the draw count** (`VkFrameHeadTest`).
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
- **What is reused**: `backend/DeviceClock.hpp` is "a device numbers its submissions and proves them in order, and that sequence is the watermark", and
  `backend/RetirementQueue.hpp` is "park + release when due". The device adopts both, so a session's completion has one evidence type (`G6`) instead of a second counter grown
  beside them (`G7`). The evidence is the DEVICE'S (`ADR-2026-11-24-completion-belongs-to-the-submission`): a driver is not a witness, because a driver that stops framing
  knows nothing about another driver's submission - and a minimum over drivers freezes every release as soon as one engine idles (`G1`).
- **What release means**: `releaseContent` / `releaseRenderTarget` record **the device's current submission number** at the time of the call, and the object is destroyed
  only once the completed watermark passes it *and* no value the device lent still names it (`ADR-2026-11-24-residency-borrowing`); so those two verbs **may be called while another session is framing**. The contract text at `RenderDevice.hpp:302`–`308` already says exactly
  this ("a device that retires asynchronously parks the free until every session is past it") — this page only makes vk such a device.
- **Threads**: `DeviceClock` and `RetirementQueue` are contracted as "the caller serialises; a multi-threaded device supplies its own lock" (`P4`). The lock lives on the
  **device** (`Impl::clock_mutex`), not inside the two SDK value types: a single-session device supplies none, and a lock inside the values would be a mechanism with no
  consumer (`G8`). Every place that takes a `clock.timeline()` reference is inside the lock, and the reference does not leave the critical section.
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
  `submitted` completed) before copying, and the wait is counted in `deviceWaits()`. One wait, one counter, for every session (`G6`).
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

- **The default implementation is "one session"**: `createFrameDriver()` returns `&executor()` by default, so **a backend and every case change not one line**; only a backend
  that hands out several sessions overrides it. That makes "concurrent sessions" a **seam** rather than a repository-wide rewrite (`G9`), and does not force a shape a backend
  does not need today (`G8`).
- **Ownership**: **the device owns the session**; `createFrameDriver()` lends a reference, contracted as "until `releaseFrameDriver` or the device's `shutdown()`". An engine creates one
  in `setDevice` and releases it when the device changes or it is destroyed. An engine that frames after `shutdown()` asks `initialized()` first (as it already does) and
  reports "the device is not up", **without dereferencing** the reference — `RenderDevice`'s existing "the creator guarantees no frame in flight" is the same ordering
  guarantee (`P5`).
- **The default content program belongs to the session, and its declaration sits on** `RenderFrameExecutor`. The program is the engine's
  (`RenderEngine::forwardProgram()`), it shades a **frame**, and resolution happens at record time — so it is a session setting. **The second reason for that placement is
  the implementation**: `RenderBackend` inherits both `RenderDevice` and `RenderFrameExecutor`, and two virtuals with the same name and signature would make
  `backend->setDefaultContentProgram(...)` ambiguous (`G4`: an interface must not require the caller to write a qualified name). `Device` overrides it and forwards to **its
  own session**, so a caller driving the device directly (`test_vk`) behaves unchanged; `RenderEngine` declares it on **its own session** inside `setDevice`.
- **Windows**: `createSurfaceTarget` stays on the device (a platform surface is not a session's property); the **first** session to claim it becomes its owner, and a second
  session gets the existing "foreign target" refusal. No new parameter is needed.

### 2.9 A backend's whole multi-session cost is one session type (`G5` / `G9`)

- **What a backend supplies**: one `RenderFrameExecutor` implementation, owned by the device, holding the frame machinery (command pool / completion fence / timestamp pool /
  frame books), the frame scratch (ring / descriptor pools / set tables / `FrameSnapshot`) and its own default content program; it overrides `createFrameDriver()` /
  `releaseFrameDriver()` and nothing else.
- **What it inherits unchanged**: the device owns the content caches, the targets and the body gate, the frame head resolves the caches into the driver's values, completion is
  `DeviceClock` + `RetirementQueue`, and a release parks to the watermark. None of that is per-backend code, so a second implementation cannot re-derive a different answer to a
  promise a host reads (`G5` / `G6`).
- **Boundary**: a backend that drives one session overrides neither method and keeps `&executor()` (§2.8); window duties (swap chain, presentation, `createSurfaceTarget`) are
  not part of a session's cost (`C9`). This is the session half of `graphics-architecture.md` 2.11's "a backend" row.

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
| F9 | A backend's session type and the device's lock table | the session owns the frame machinery, the scratch and its default program; the device owns the caches, the targets and the body gate; the frame head resolves, recording takes no device lock, submission takes the queue lock | landed: `Device::FrameDriver` (vk) is the shipped instance; §3.1.9 states every lock, when it is taken and by whom |
| F10 | Closing out the contract and the docs | `C21` / `N18` rewritten, three headers' text, the ADR's status, the audit rows | landed: `C21` / `R18` / `N18` say "frames are concurrent per session"; the threading sections of `RenderDevice` / `RenderFrameExecutor` / vk's `Device` agree |

### 3.1.1 F1 Session type and creation

- **Behaviour**: `RenderDevice::createFrameDriver()` returns `&executor()` by default; a backend that hands out several sessions overrides it to "build one". `releaseFrameDriver` is
  a no-op by default. `RenderEngine` holds a `RenderFrameExecutor*`, creates it in `setDevice` and releases it on a device change or destruction.
- **Constraints** (C1): the **device** owns a session; an engine only borrows it.
- **Requirements** (R1): `createFrameDriver()` may be called before `initialize()` (the creator initializes the device and the engine attaches first).
- **Acceptance** (T1): a single-session backend (the default implementation) runs `test_graphics` with no behaviour change.
- **Caveat** (N1): `shutdown()` leaves no dangling session behind — see `P5`.

### 3.1.2 F2 Per-session frame machinery

- **Behaviour**: `FrameDriver` holds `command_pool` / the `completion` fence / the timestamp pool / `Books` / `FrameGate` / its own set of `ReportOnce`s; `Device::executeFrame`
  becomes "forward to the target session".
- **Constraints** (C2): the frame machinery exists **only** in a session; the device has no second `command_pool`.
- **Requirements** (R2): `executor()` stays, with its meaning narrowed to "the device's own session" (`G6`, the same shape as `setDiagnosticsRoute`).
- **Acceptance** (T2): `test_vk` (186 cases / 45 suites) is green; the device's own driver answers the same frames it did before the session split.
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
  completion evidence), but it is not some session's machinery; **each readback counts one** `deviceWaits()`, the reading a device-wide wait gives (`G6`).
- **A refusal does not stop the device**: refusals that can be answered before waiting (unknown / unbuilt target, out-of-range attachment, unsupported format, borrowed depth)
  are answered in phase one; phase two **re-resolves** inside the gate, because a frame may have rebuilt or replaced the target while waiting.
- **A call from inside a frame body**: when a diagnostic sink / hook calls back into this device from inside a frame body, the read answers `Failed` instead of waiting for
  itself — `FrameBodyDepth` (the in-thread frame-body depth) turns that class of deadlock into an answerable refusal; `Device.hpp` states this contract.
- **External synchronisation**: `vkQueueSubmit` / `vkDeviceWaitIdle` go through the device's `queue_submit` lock (the ADR's hard-constraint table requires it; two drivers
  submitting at once is a Vulkan external-synchronisation violation).

**Cases**: `VkMultiSessionTest` — two drivers alternately draw, and interleaved readbacks each get their own last frame (T7); a readback while a frame is stopped by a sink
must **wait**, then read that frame's pixels and count +1; a read from inside a frame body answers `Failed` and touches no host vector.

### 3.1.8 F8 The default content program belongs to the session

- **Behaviour**: the `setDefaultContentProgram` declaration sits on `RenderFrameExecutor` (default no-op; `Device` overrides it and forwards to the
  device's own session), and the engine declares it on **its own session** in `setDevice`; the device-level one serves only a caller that drives the device directly.
- **Constraints** (C8): when two engines declare different default programs, **each draws with its own** (no more "the last declaration wins").
- **Requirements** (R8): a single-session backend behaves byte for byte as today.
- **Acceptance** (T8): two sessions declaring different default programs ⇒ each frame's pixels come from its own program; a mutation (sharing one device-level slot) ⇒ red.
- **Caveat** (N3): this replaces `graphics-threading.md`'s `N18` (one device-level slot), see F10.

### 3.1.9 F9 A backend's session type and the device's lock table

- **Behaviour**: the session is a type the device owns. In the shipped backend it is `Device::FrameDriver` (nested in `Device`, so it reaches the device's private state
  without a friend declaration, `P9`): it owns the frame machinery and the scratch, it implements the three executor verbs, and the device's own driver is the `executor()` a
  direct caller is handed.
- **Constraints** (C9): window duties (swap chain, presentation, `createSurfaceTarget`) do not move because of sessions.
- **Requirements** (R9): the SDK carries exactly two session methods (`createFrameDriver` / `releaseFrameDriver`) and no session type; a backend adds one type and no SDK change.
- **Acceptance** (T9): a backend that overrides neither method builds and its frames are unchanged (`test_graphics`), and the shipped one hands out a real second driver
  (`VkSessionShapeTest.TheDeviceHandsOutANewDriverAndKeepsItsOwn`).
- **Caveat** (P8): the driving session must exist in exactly one place — the device may not keep a second copy of "which picture belongs to which session".

**The lock table** — every device-level lock, the verb that takes it, and what it protects:

| Lock | Taken by | When | What it protects |
|---|---|---|---|
| the caches' `ContentCaches::Lock` | the frame head (`planDraw` / `resolveTextures`) | once per frame, once more per miss | the cache tables and the residency table; recording reads only the values in the plans (`C3`) |
| `graphics::FrameGate body_gate` | every frame body — **shared** for an offscreen frame, **exclusive** for a window frame; every host verb — **exclusive** | the whole frame body / the whole host verb | that a window frame is alone on the device while offscreen frames of different drivers overlap; a host verb called from a frame-body thread gets an empty lease and is refused (`vk.device.reentry`, readback `InsideFrame`) instead of waiting for itself |
| `queue_submit` | `endFrame` and every `vkDeviceWaitIdle` | the driver call only | Vulkan's external synchronisation on the queue: two drivers may submit from two threads |
| `clock_mutex` | the `DeviceClock` / `RetirementQueue` calls at the frame head and tail | one call | the watermark and the park queue (`P4`) |
| `program_mutex` | `setDefaultContentProgram` and the frame head's read of the default program | a pointer swap | a program announced while another driver frames (`VkMultiSessionTest.TheDefaultProgramMayChangeWhileAnotherDriverFrames`) |

**The phase order a frame body walks** (C3's mechanism): `scratch.beginFrame()` and `frame_head.beginFrame(...)` **resolve** (one cache lock) → recording turns the plans into
commands (**no device lock**) → `endFrame` **submits** (`queue_submit`, then the clock). A head is a value and owns what it names: an object a release or a replacement removes
while the frame records is parked to the device watermark (§2.6), so another driver's frame head cannot invalidate this frame's plan.

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
| C9 | Window duties (swap chain / presentation / `createSurfaceTarget`) do not move because of sessions; a window belongs to the session that claimed it first. |
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
| R9 | The SDK carries exactly two session methods (`createFrameDriver` / `releaseFrameDriver`) and no session type; a backend adds one type and no SDK change. |
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

- How it fails: `DeviceClock::noteSubmission` / `noteCompletion` and `RetirementQueue::retire/advance` are documented as "single-threaded" (the device supplies the lock);
  with two sessions two threads call them at
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
  visibility, a full-screen draw's source target) ⇒ a miss and a lock every frame.
- How to detect it: R3's lock count is not 0 in the steady state.
- How to prevent it: the snapshot's input is the same frame value recording uses (one traversal, two results), not a second enumeration written by hand.

### 6.7 P7 The illusion that "readback is cheap"

- How it fails: readback is treated as a cheap verb and put on the per-frame path; it waits for the device watermark, which eats the concurrency gain.
- How to detect it: `deviceWaits()` grows linearly with the frame count.
- How to prevent it: readback stays off the frame path (the SDK documentation already says "readback is a host action"); a case asserts that a steady-state frame with two
  sessions does not raise `deviceWaits()`.

### 6.8 P8 A second copy of the driving session

- How it fails: the device also keeps a "last driver" beside the driver's own frame state (this frame's scratch, its window, its upload baseline), and the two disagree — a
  frame's pass then uses another session's window target, or presents another session's picture.
- How to detect it: a frame presents a picture another driver recorded, or a pass draws into a target that driver never claimed.
- How to prevent it: the frame's state exists **only** in the driver; the device keeps the body gate and, for `frameReport()`, a copy of the frame that ended last. A frame
  presents from the driver that ran it (the frame's own tail), never from a device-level "current session".

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
| D4 | a session = a window | `G1`: an offscreen session is not a window; `G5`: tying one concept to another makes both grow crooked | this page §2.1 |
| D5 | the engine allocating the session object itself | `G1`: the GPU objects' owner must be the device; `G10`: the engine should not be responsible for a command pool's lifetime | this page §2.8 |
| D6 | readback borrowing some session's frame machinery | `G4`: readback reads "what has completed", unrelated to "who just submitted"; borrowing either one makes them wait for each other | this page §2.7 |
| D7 | renaming `DeviceClock::WindowId` to `SessionId` now | `G8`: no consumer needs that spelling; a type alias first (`SessionId = WindowId`), the rename left for when a second API family exists (**superseded by `ADR-2026-11-16-redesign-seam-and-vocabulary`**: the three spellings are unified as `FrameDriverId`, and `WindowId` was deleted by `BC-32`) | this page §2.6 |
| D8 | landing it in one go (no stages) | `G11`: a half-split tree is worse than an unsplit one — every step must be verifiable on its own | ADR §9 |

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | a single-session backend changes no behaviour | `build/bin/test_graphics` (the default `createFrameDriver`) |
| T2 | the shipped backend's own driver answers every frame the device runs | `build/bin/test_vk` (186 cases / 45 suites) |
| T3 | the recording path's cache locks are 0; the whole frame's count **does not grow with the draw count** (measured 4: 1 at the frame head + 2 for the books + 1 at the frame tail) | `test_vk` `VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls` (1 vs 6 commands; the mutation 6 → 10 goes red) |
| T4 | two sessions first-drawing the same geometry at once: no crash, both pictures right, uploads ≤ 2 | `test_vk` `VkResidenceWireTest.TwoDriversFirstDrawingOneGeometryShareOneEntryAndAtMostOneCopy` (really two sessions on two threads: claims 1 / published 1 / copies ≤ 1 / both pictures red / the next frame uploads nothing), `AKeyAnotherDriverIsFillingIsCopiedForThisFrameAndDiscarded` (a fake lease: a copy, exactly two streams, `used_bytes` back in place), `ARefusedBuildIsWithdrawnInsteadOfPublished`, `AReleasedGeometryIsForgottenSoTheNextFrameMayBuildItAgain`; mutations 3/3 red (not asking the table / publishing a failed fill / not forgetting on release) |
| T5 | two sessions drawing one target: one pass skipped + one report | `test_vk` `TargetClaimFrameTest.AFrameThatLostItsTargetSkipsThatPassAndSaysSoOncePerEpisode`, `TargetTableTest.ASecondDriverIsRefusedAndSkipsOnlyItsOwnPass` |
| T6 | a release overlapping an in-flight frame: the release does not wait for the frame to end, the frame still completes with the handles it resolved, the object parks to the watermark, and `deviceWaits()` does not rise |
  `VkMultiSessionTest.AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses` (mutation: the release takes the body gate again ⇒ red) |
| T7 | alternately drawing different targets + interleaved readbacks: each reads its own latest completed frame | `test_vk` `VkMultiSessionTest.EachDriversTargetReadsBackItsOwnLastCompletedFrame`, `AReadbackWaitsForTheFrameInFlightAndCountsTheWait` |
| T8 | two sessions with different default programs: each draws its own | `test_vk` `VkMultiSessionTest.TwoDriversShadeWithTheirOwnDefaultProgram`; the mutation (sharing one slot) goes red |
| T9 | the seam is additive: two SDK methods, no session type, and a backend that overrides neither keeps its behaviour | `test_graphics` (the default `createFrameDriver`), `test_vk` `VkSessionShapeTest.TheDeviceHandsOutANewDriverAndKeepsItsOwn` |
| T10 | two sessions, one thread each, N frames each: both pictures are right and reentry is refused; **the frame bodies really overlap in time** (with one frame stopped by a sink another whole frame completes) | `VkMultiSessionTest.TwoDriversOnTwoThreadsEachKeepTheirOwnPicture` (each with its own texture),
  `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame` (mutation back to an exclusive gate ⇒ red); the "same-thread reentry is refused" assertion is kept |
| T11 | the data race has tool evidence (run, 2026-11-16: three suites, 0 reports) | `scripts/tsan_check.sh` (`test_vk` / `test_graphics` subsets) |
| T12 | the documentation matches the code | `python3 scripts/check_ai_docs.py --strict`, `python3 scripts/check_design_docs.py` |
| T13 | the lock table's two sides: two drivers' frame bodies overlap, a host verb waits instead of racing, a host verb called from a frame body is refused, and a program may change while another driver frames | `test_vk` `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame` (mutation: the gate back to exclusive ⇒ red), `AHostVerbWaitsForTheOtherDriversFrameInsteadOfRacingIt`, `AReadbackFromInsideAFrameIsRefusedInsteadOfWaitingForIt`, `TheDefaultProgramMayChangeWhileAnotherDriverFrames` |

## 9. Status and Evolution

### 9.1 Current status

The shape this page describes is landed in the shipped backend. How it landed, the cases and mutation evidence for each step, and that day's TSan reading are history and live
in `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`'s "implementation status" and in git; this page keeps only what still binds today's code: §2's design
points, §3's features, §4's interfaces and §5's constraints.

### 9.2 Known gaps / backlog

- `DeviceClock`'s verbs are `addDriver` / `removeDriver` (the identity type is `FrameDriverId`), and the completion
  watermark is the device's own SUBMISSION sequence (`ADR-2026-11-24-completion-belongs-to-the-submission`); a driver's
  own evidence no longer constrains it, so an idle driver stalls nothing.
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
- the engine side: `src/viz/graphics/src/RenderEngine.cpp` (`setDevice` / `frame`), `src/viz/graphics/src/RenderPass.cpp` (the single-pass driver)

### 10.2 Documents and ADRs

- `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` (the decision and the options)
- `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` (the shared retirement mechanism)
- `ADR-2026-11-13-one-engine-one-surface.md` (one engine, one surface — this design's premise)
- `graphics-threading.md` `C21` / `R18` / `N18` / `D13` (the contract in use)
- `vk-backend.md` §2.11 (one `Device` object = one session pool) and §4.3 (the units this page names)
- `graphics-frame-pipeline.md` (frames are values, four layers, one-way edges)

### 10.3 External material

- the Vulkan specification: the external-synchronisation requirements per object (queues / command pools / descriptor pools / pipeline caches / fences) — the ADR's
  "hard constraints" table
- O3DE RHI / wgpu: a thread-safe device and one command list per thread (external material, not verified in this repository)
