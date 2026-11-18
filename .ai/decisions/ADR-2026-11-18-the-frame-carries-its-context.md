# ADR-2026-11-18: the frame carries its context, and one clock measures it

- Status: **Accepted and implemented** (2026-11-18), on an explicit go **ahead of its trigger**. Answers `T40` of
  [graphics-2026-11-18-review-5](../research/graphics-2026-11-18-review-5.md) §2.1, which registered it as
  trigger-gated; the trigger ("the first feature needing two of the fields") has not fired, and the decision records
  that the batch was ordered anyway.
- Date: 2026-11-18
- Scope: `FrameContext.hpp` (new), `FrameUpdate.hpp`, `RenderEngine` (`frameContext()`, the frame clock),
  `PassChain::update` / `SceneView::update` (one argument), `FpsOverlay`, `AxisGizmo`, appfw's `SurfaceWindow`, and
  the two test doubles.
- Related: [ADR-2026-11-16-device-is-a-pool-sessions-own-frames](ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md)
  (why `PassChain` reaches the engine through narrow leases), `graphics-frame-pipeline.md` 2.13.

## Background

`RenderEngine::frame()` took no arguments and `FrameUpdate::update()` took none, so an object doing its own per-frame
work had to find out for itself which frame it was in and how big the surface was. `AxisGizmo` reached for the source
camera it was told about, and `FpsOverlay` **read `steady_clock` itself**: one clock per time-dependent object, in a
system whose whole point is that one frame is one value.

The fifth review recorded the shape it should take and registered it as trigger-gated with a stated criterion: "the
first feature that needs two of the fields (frame number, elapsed time, area size, pixel ratio)". The user ordered the
batch before that trigger, which is the user's call - the design below is the shape the review argued for, and the
trigger is recorded as not hit rather than quietly declared met.

Three constraints shaped it.

1. **`PassChain` must not reach the engine.** It holds two narrow leases (`PassRegistry`, `DiagnosticsHandle`) on
   purpose: the engine may die first, and the chain must answer "gone" rather than keep it alive. A "the chain asks the
   engine for a context" design would either widen that dependency or need a third lease type.
2. **The frame's own clock is one reading.** A per-frame value that mutates when read is not a value: a host that asked
   twice would get two answers for one frame.
3. **A size is the host's announcement, and device pixels have one rounding rule** -
   `EngineCompile.cpp` lays a frame out with `std::lround(logical * ratio)`, and a second spelling of that arithmetic
   is how two parts of a system start disagreeing by a pixel.

## Decision

**One `FrameContext` per frame, composed by the engine, handed down unchanged; one clock, read once per frame.**

- `FrameContext` is a value: `frame_number`, `elapsed_seconds`, `area_width` / `area_height` (logical pixels, as the
  host announced them), `pixel_ratio`, plus `renderWidth()` / `renderHeight()` / `hasArea()`. It describes the frame
  the caller is about to build, so `frame_number` is `frameNumber() + 1`.
- `RenderEngine::frameContext()` **composes it on every read** - the id, size and ratio are live facts, so a host that
  asks twice gets the same answer - and returns the interval the clock recorded.
- The clock is read **once per frame, at the head of `frame()`, after the readiness checks**: a `NotReady` frame is not
  a frame's interval, and counting it would report a stalled engine as one slow frame. `elapsed_seconds` is therefore
  the interval between the two most recent frames - one frame behind the one being prepared, which is what "how fast is
  this loop running" means.
- `FrameUpdate::update(const FrameContext& context)` receives it; `PassChain::update(context)` hands **the same value**
  to every overlay of that call, and `SceneView::update(context)` forwards.
- `FpsOverlay` stops reading the clock and feeds `context.elapsed_seconds` to its sampler: the number of clocks in the
  system drops to one.

## Options rejected

- **`update(double dt)`**: names one field of the context and leaves the other three to be fetched separately, which
  is the arrangement this replaces. It also invites `dt` to be re-derived by each caller.
- **A host-supplied clock** (`frame(const FrameContext&)`, or `setFrameTime`): it makes the frame a function of what
  the host says, which is the right shape for replay - but nobody has asked for replay, and it puts a field every host
  must fill in front of every host that does not care (`G8`). The review's third argument (determinism) therefore
  stays open, and the docs say so: a host that needs a fixed step keeps its own clock and reads the other fields.
- **A `FrameClock` lease so `PassChain::update()` keeps its zero-argument signature**: it would leave every host call
  site untouched, at the cost of a third narrow lease on the engine and a value the chain composes without the host
  knowing which frame it is looking at. One argument at the call site is cheaper than one more long-lived boundary
  (`G1`).
- **Advancing the clock inside `frameContext()`**: it would remove the one-frame lag, at the price of making an
  accessor mutate - a host that asked twice would see two frames. The lag is the correct trade.
- **A host-side helper that fills a `FrameContext` from the engine**: the same value, wrapped in a second name.

## Consequences

- **`BC-25`**: every caller of `SceneView::update` / `PassChain::update` passes one argument (`engine->frameContext()`),
  and every `FrameUpdate` implementation takes one. appfw does it for the standard path; a host with its own loop
  writes the same one call.
- **The FPS readout's test became deterministic**: it used to `sleep_for(80ms)` and hope the wall clock produced an
  interval. It now states `elapsed_seconds` and asserts the published figure - a real gain for the suite.
- **`AxisGizmo` ignores the argument** and says so: the mirror reads the source camera, not the frame. A hook with one
  shape and two consumers that use different parts of it is the point of a context value.
- **The low-pass warning stands**: this batch landed before its trigger. If a second consumer of the fields never
  appears, the value still earned its place by deleting a per-object clock; if a replay requirement appears, the
  host-supplied clock remains the additive step and this ADR's rejected option is the entry to revisit.
