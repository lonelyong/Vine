# ADR-2026-11-27: the last interval verb answers a value

- Status: accepted
- Date: 2026-11-27
- Related: `../design/graphics-threading.md` 2.17 / `D14` / `T40`,
  `../research/graphics-2026-11-27-review-19.md` `Z3`,
  `src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp`,
  `src/viz/graphics/src/RenderEngine.cpp`

## Background

Five entries take the frame interval (`detail::FramePump`): `frame()`, `prepare()`, `applyPlan()`, `bindPassContent()` and
`setFrameThreading()`. Four of them answered what happened as a value - `FrameCallOutcome`, `PrepareReport`,
`PlanResult`, `ContentBindOutcome` - each naming the two interval refusals as `Reentrant` / `Concurrent` (or `Busy`)
rather than as a string.

`setFrameThreading()` answered `void`. Its two refusals existed only as the diagnostic codes
`engine.threading.refused_from_frame` / `refused_in_flight`, reported once per episode. A host that starts a frame
thread to decouple its UI therefore had no local answer to "did it start": it had to poll `frameThreading()` and
correlate a diagnostic string with the moment it asked - the exact shape the class refuses elsewhere ("a call that
cannot be resolved by waiting arrives as a VALUE instead of a race", `frame()`'s own rule).

The fact is observable (`frameThreading()` reads the mode back), so the defect is not a lost fact but a lost ANSWER:
"refused, and why" is a different question from "what is the mode now", and only the second one was askable.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. `bool` | smallest change | Folds the two refusals into one answer (`G4`): "this is the engine's own frame thread" is a programming error with a different repair from "another host thread is building a frame", and a caller cannot tell them apart |
| B. Reuse `EngineNotReadyReason` | the vocabulary `frame()` / `prepare()` already share | The enum means "this engine cannot drive a frame" and carries `NoDevice` / `NotInitialized`, which this verb cannot produce; a host switching on it would write dead branches, and the type would say the engine is unready when it is merely holding its interval |
| C. Return `detail::FramePump::Build::Refusal` | the exact answer, no new type | Puts an engine-internal type in a public signature (`G2` / `G12`), and `Build` is about the lease, not about "the mode is now X" |
| D. A three-state `FrameThreadingOutcome` (`Applied` / `RefusedFromFrame` / `RefusedInFlight`), no `operator bool` | The answer names the mistakes the episode codes name; `Applied` covers "changed" and "already there" because the caller's statement holds either way; the compiler makes a caller that reads it decide which retry it owes | One small public type for one verb (`G8`), bought back by deleting the poll-and-correlate rule |

## Decision

Option D.

`setFrameThreading(FrameThreading)` returns `FrameThreadingOutcome`:

- `Status::Applied` - the mode is NOW the one asked for (it changed, or it already was that). Whether it moved stays
  readable from `frameThreading()`, and the two are deliberately not folded into one value: a host that cares about
  the transition asks, a host that only needs the mode does not.
- `Status::RefusedFromFrame` - the call was made on this engine's own frame thread; the running mode cannot end
  itself. Ask between frames from the host thread.
- `Status::RefusedInFlight` - another host thread is building a frame; a switch is an entry between frames. Retry.

The episode still governs the REPORT (`engine.threading.refused_from_frame` / `refused_in_flight`, once per episode);
every call gets its value, because a caller that made the mistake twice still needs the answer once. The type has no
`operator bool` for the reason `ContentBindOutcome` has none: "refused" is an ordinary answer here, and a conversion
would let a caller read it as "nothing happened" without deciding whether to retry.

The appfw convenience verb (`RenderControl::setFrameThreading`) keeps forwarding and returning nothing: a control
without an engine has no mode to state, and a fourth state for that case would put a GUI concern into the SDK's enum.
Its documentation points at the engine answer for a host that needs it.

## Consequences

- Every interval verb now answers its refusals as values; the poll-and-correlate rule is gone from the host side.
- The change is source-compatible (callers that ignore the return compile unchanged) and an SDK change like the rest:
  a stale binary must be rebuilt with the header, the standing rule the plugin rebuild lesson states.
- Acceptance: `RenderEngineTest.ASecondThreadCannotChangeTheFrameThreadWhileAFrameIsInFlight` now asserts
  `RefusedInFlight` on both refused calls and `Applied` between frames; the reentrant case is the same mapping, and
  `FramePumpTest` keeps the pump's own classification (`T40`).
