# ADR-2026-11-13: one engine drives one surface - "one engine, many surfaces" rejected (graphics-next F8)

> **Status**: Accepted
> **Date**: 2026-11-13
> **Scope**: `.ai/design/graphics-frame-pipeline.md` (F8 / §3.8 / R21 / R27 / R37 / T29 / K10 / §7 / §9.3),
> `.ai/design/graphics-architecture.md` (F19 / D2 / §2.8 noun discipline), `.ai/backlog/graphics.md`. **No code changes**.

## Background

- F8 / `R21` / `R27` / `R37` / `T29` / `K10` of graphics-next require "one Session drives several Surfaces":
  `Target = Surface{SurfaceId}`, a frame request carries a `SurfaceId`, the criterion is "one Session presents correctly to two Surfaces",
  and implementing it with a second engine thread is explicitly forbidden (`K10`).
- In the same repository, §7 **D2 of `graphics-architecture.md` had long rejected** "one frame across several windows"; yet §3.5 F19 says "one engine may serve
  several windows" - the two sentences contradict each other.
- The code stands with D2: `RenderEngine::createSurface()` refuses a second area (`area_refused_`, one report per episode),
  `announceAreaSize()` only accepts its own area, a frame has one `frame.target`, `RenderSurface`'s rule is "one area, one picture",
  and `RenderControl` / `SurfaceWindow` build one engine per window.
- The user document `docs/guides/render-views.md` §7 already says "one engine per window, one device may serve several engines", and marks
  "one frame across several windows" as "registered, not doing".
- A per-file costing was done on 2026-11-13: implementing F8 costs about **900-1400 lines across 14-20 files** (SDK shape ~300-450, engine internals
  ~150-250, vsg backend ~250-400, appfw ~150-300, tests ~150-250), and it would rewrite the single-value semantics of `FrameResult` / `PresentRecord` -
  the latter having just landed in F12 (the same day).

## Decision

1. **One `RenderEngine` drives one `RenderSurface`** (the status quo, unchanged).
2. **Several windows = one `RenderDevice` serving several engines**: one engine, one area and one `Viewer` per window (the status quo).
3. Delete F8, `R37`, `T29`, `K10` and "step 4" of §9.3 from graphics-next; rewrite `R21` / `R27` as "one engine, one surface";
   remove `SurfaceId` from the §2.5 id table.
4. Correct F19 of `graphics-architecture.md`, and record its D2 as this ADR's predecessor.
5. `FrameResult.present` / `PresentRecord` keep their **single value**: one frame, one surface.

## Rationale

- `G8`: the repository has no real host that needs "the same plan presented in two windows with different viewports". Two windows are two engines today and work fine;
  pre-building a surface set, `SurfaceId` and per-surface results without a consumer is manufacturing the complexity first.
- `G5`: a surface set is a new mechanism and needs a home (`detail::`), or `RenderEngine` (1484 lines) keeps growing, directly against
  §2.8 of `graphics-architecture.md`, which declares that composition root.
- `G4` / `G10`: several surfaces per frame would turn `FrameResult.present`, `PresentRecord`, the failure semantics (one surface `Lost` is not the whole frame
  `Lost`) and the transient default size (which surface does it follow) all into per-surface values; F12 landed the single value the same day, so doing it again is rework.
- `G2`: one frame across surfaces = N acquires / presents inside one `executeFrame`, while the backend executor has one window (`d->driving`);
  that is new semantics in the execution layer, not a move.

## Rejected options

| Option | Rejection reason |
|---|---|
| Implement "one engine, many Surfaces" as F8 says | No consumer (`G8`); the public result shape was just changed and would change again (`G10`); appfw's ownership (the window owns the engine) would be rewritten; the failure semantics would be brand new. |
| Do only the engine / SDK half (F8a), leave appfw alone | `T29` requires "one Session presents correctly to two Surfaces", and without the appfw shape there is no real call site, which is pre-building a capability (`G8`). |
| Keep F19's wording and change no code | Documentation contradicting the code, and D2 as well - a reader would write a host against a shape that does not exist. |
| Implement several windows with two Sessions sharing a device | `I6`: only one engine thread touches the device; two Sessions each with an engine thread means two (audit X4 already pointed this out). |

## Consequences

- **Given up**: the benefit of several windows sharing the pass table / plan / compilation; each window has its own engine state (plan, slots, publish table).
  **Still shared**: device-level resources - two engines share one `RenderDevice`, and the pipeline cache and video memory are counted per device.
- The documentation is consistent again: F19, D2, §7 of `docs/guides/render-views.md` and the contract of `createSurface()` now say the same thing.
- The single-value semantics of `FrameResult` / `PresentRecord` have a written basis (one frame, one surface).
- The cost is stated plainly: **one compilation and residency account per window** (§7 of `docs/guides/render-views.md` already records the same fact).

## Trigger conditions (when to reopen)

Hitting any one of them reopens the question in a **new ADR**, without changing this one:

1. A real host needs "the same plan (the same set of passes / the same chain) presented in two windows with different viewports", and the measured duplication of
   compilation / video memory across several engines has become a bottleneck;
2. A second backend lands and makes "device = Session" necessary (the same trigger as backlog `T15` / `T22`).

## References

- `.ai/design/graphics-frame-pipeline.md` F8 / §3.8 / `R21` / `R27` / `R37` / `T29` / `K10` (deleted or rewritten by this ADR)
- `.ai/design/graphics-architecture.md` §3.5 F19, §7 D2
- `docs/guides/render-views.md` §7 (the standing multi-window position)
- `src/viz/graphics/src/RenderEngine.cpp`: `createSurface()`, `announceAreaSize()`, and `frame.target` in `buildContentHalf()`
- `tests/test_graphics/RenderSurfaceTest.cpp` (the second area is refused), <!-- drift-ok -->
  `tests/test_graphics/HeadlessBackendTest.cpp` (`OneDeviceServesSeveralAreasWithoutCrossingThem`)
