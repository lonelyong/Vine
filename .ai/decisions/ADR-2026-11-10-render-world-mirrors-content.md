# ADR-2026-11-10: the render world mirrors content, and the content world stays on the UI thread

- Status: Accepted (at the contract level; implementation proceeds by the stages of §9.3 in `../design/graphics-threading.md` and the trigger conditions of T31 in `../backlog/graphics.md`)
- Date: 2026-11-10
- Related: `../design/graphics-threading.md`, `../design/graphics-scene.md`, `../design/vsg-backend.md`,
  `../design/appfw-render-surface.md` §9.2, `../backlog/graphics.md` T24/T31

## Background

Today every part of a frame runs inside one call stack on the Qt application thread (`SurfaceWindow::renderFrame` → `RenderEngine::frame()`
→ `compileFrame` → `executor().executeFrame`), and the application thread is also the UI thread. The costs are measured: 2000 drawables with a moving camera cost
1597 µs/frame (71% of it per-command construction / resolution), the backend records 33 allocations per recorded triangle, session initialisation is ~184 ms, and vsg sleeps 16 ms on an empty recorded frame.
appfw already treats application-thread occupancy as a budget (the 150 ms startup gate).

The hard part is **who owns the content**: the backend's fact rows read from live content every frame (`GeometryFacts.cpp:181`, `ContentStore.cpp:377`),
and collection has to walk the scene graph. So "move rendering to another thread" necessarily settles a relationship between the two sides.

## Options

| Option | Benefit | Cost |
|---|---|---|
| **A. The content world stays on the content thread (UI), the render world mirrors it, updated incrementally by deltas** | The content-editing API is unchanged (it reads and writes its own document synchronously); the render side has no locks; a still scene costs nothing (only the changed part is paid for while editing); only three value channels | A mirror and a delta protocol are needed; announcements must be **exhaustive** (a missed one leaves the mirror silently stale); a shape edit has to build a description value once |
| B. Content belongs to the engine thread; the host only sends intent and reads snapshots | No mirror needed | An editor that reads and writes its own document synchronously (property panel, gizmo, picking) cannot be done; every host and test would be rewritten; Unreal / Qt Quick / Godot / Bevy all keep an authoring scene graph |
| C. Extract / `sync()` every frame (the Qt Quick / Bevy shape) | The simplest protocol (copy everything every frame) | O(drawables) copying per frame lands on the content thread - exactly the cost that is being taken away |
| D. Double-buffered worlds (clone the scene while editing, swap atomically at the frame boundary) | The content side can read and write synchronously | O(content) per edit; two scenes means two resident copies |
| E. Keep the status quo (everything on the UI thread) and only parallelise select | No migration; the 71% of collection is won first | The UI thread still carries upload / record / present; the 16 ms empty-frame sleep and the 184 ms session init stay on the UI thread |
| F. Lock the content model | Editing and rendering can run concurrently | Two owners for one concept; a lock on every read path; against zero allocation in steady state (`G1`/`G10`) |

## Decision

Chose **A**, and drew the boundary at values:

1. **Two worlds, one writer each**: the content world (`Scene`/`Node`/`Geometry`/`Buffer`/`Program`/`Material`/`Texture`/`Camera`)
   is written only by the content thread; the render world (mirror nodes, geometry proxies, material blocks, residency table, vsg command graph, swap chain) is written only by the engine thread.
2. **The mirror is the bake that already exists**: world matrices, world boxes, folded state and order (what `SceneBake` computes today) move to the engine side,
   with the input changing from "read live nodes" to "the values after applying deltas". The mirror holds `intrusive_ptr<Node>` only to keep the address alive and **reads no fields**.
3. **Three channels**: delta (values, the payload is required ⇒ a missed announcement does not compile), handoff (ownership, through a queue with an edge),
   snapshot (an already-published immutable value plus a frame number). The original design's "request" channel is **deleted** (picking, bounding boxes and the camera can all answer themselves on
   the content side).
4. **Byte content does not go through deltas**: `Buffer::revision()` / `Geometry::revision()` become atomic counters and the render side keeps polling
   (the current pin criterion is already O(#resident entries) and independent of content size). This is the only cross-side read allowed.
5. **The surface belongs to the content thread, the swap chain to the engine thread**: an extent change is applied only at the frame boundary, and frames during a rebuild are dropped
   without counting as an error.
6. **Parallelism only for pure computation over the mirror**: the select stage of collection runs in parallel blocks, merged in block order, bit-identical to serial;
   small scenes do not parallelise.
7. **One implementation, two modes**: `inline` (the calling thread is the engine thread - today's behaviour, the default for tests and headless) and `threaded`, sharing the frame loop and the
   mirror.

## Consequences

- **Good**: zero semantic change on the host side (the editing API does not move, and under `inline` behaviour is bit-identical); the render side leaves the UI thread; a still scene copies nothing per frame;
  the mirror's correctness has an executable criterion (the mirror's output is bit-identical to the content side's `collectRenderCommands` on the same view),
  and parallelisation rests on that criterion too; `RenderDevice`'s "one thread drives it" contract does not change.
- **Bad / cost**: one more mirror (memory O(nodes), the same order as the content itself); announcements must be exhaustive, or the mirror goes stale with no error at all;
  a shape edit builds a description value on the content side once (O(#channels + #primitives)); the engine side recomputes world matrices and boxes per subtree delta (new code);
  the first handshake is O(nodes), paid once while a large scene loads.
- **How misuse fails**: a missed delta / reading a content field - neither crashes, both only draw wrong, and they often appear only under a particular edit sequence (§6.1, §6.2 of the design page).
  That is why "the payload is required" is a compile-time constraint and the "mirror vs rebuilt from scratch" equivalence case is the master switch.
- **Trigger conditions (when to reopen this decision)**:
  - a real need appears for "content and rendering to see each other within one frame" (a per-frame animation whose result must be read back in the same frame to react); A allows one frame of
    delay, so either accept the delay or open a dedicated synchronous channel for that datum;
  - the content thread's editing cost itself becomes the bottleneck (the O(#primitives) description construction of a shape edit shows up in a profile) - then consider moving the description
    construction to a worker (handoff) without changing this decision;
  - a real consumer for `OwnedFrame` (remote / recording) lands, and the cross-thread frame's shape follows it.

## Evidence

- The status quo is entirely on the application thread, with no render thread: `src/fw/appfw/src/gui/SurfaceWindow.cpp:832-861`, `src/viz/graphics/src/RenderEngine.cpp:240-313`;
  nothing calls vsg's `setupThreading()` anywhere in the tree.
- Fact rows read from live content: `GeometryFacts.cpp:181`, `ContentStore.cpp:377`; the residency live criterion reads `Buffer::revision()` (the `beginFrame` predicate in `StreamUploads.cpp`).
- The mirror's raw material already exists: `Scene.cpp:937-1000` (bake / per-view memo / sort keys).
- The announcement hooks and their 52 call sites: `Node.cpp:14-36`; `Geometry.cpp:398/412`; `StateNode.cpp`.
- Both revisions are only compared and are monotonic: `Buffer.hpp:169-181`, `Geometry.hpp:766-770`; the storage is a non-atomic `std::uint64_t` (`Buffer.hpp:368`).
- The threading contract only promises "one thread", not which one: `RenderDevice.hpp:135-138`, `RenderFrameExecutor.hpp:52-55`.
- The cost numbers: `../backlog/graphics.md` T24 (1597 µs @2000 / 141 µs @200 / 71% per command), `vsg-backend.md` (33 allocations per recorded
  triangle), `appfw-render-surface.md` §9.2 (~184 ms session init), vsg `RecordAndSubmitTask::finish()`'s 16 ms empty-record sleep.
- Cross-thread reference counting has no happens-before: `RefCounted.hpp:62-83` (relaxed add / acq_rel release) ⇒ handoff must go through a queue with an edge.
- Comparison with other libraries (from their published architecture documents): Unreal's `FScene` + `FPrimitiveSceneProxy` + `ENQUEUE_RENDER_COMMAND` (= this decision),
  Qt Quick's per-frame `sync()`, Godot's `CommandQueueMT` + RID, Bevy's `Extract`, Filament's single-threaded API plus job system.
  They all keep an authoring scene graph, which is the direct basis for rejecting option B.
