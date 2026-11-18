# ADR-2026-10-22: pass scheduling lives only in the engine

- Status: Accepted
- Date: 2026-10-22
- Related: `../design/graphics-architecture.md`

## Background

One round had two orderings: the engine ordered by stage and registration, and the backend `FrameGraph` ordered again by input edges.
Two frame vocabularies, two "compilations", and two orders that could contradict each other.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the backend `FrameGraph` | The backend is self-sufficient | Two schedulers; engine and backend can disagree |
| B. Move scheduling up into the engine | One implementation; the backend only materialises | The backend can no longer order on its own |
| C. Allow only an explicit order plus dependency edges | Simplest | The host has to write the order; a stable order is lost |

## Decision

Chose **B**: `detail::FrameScheduler` (iterative Tarjan plus a heap ready queue, zero allocation in steady state) builds the edges, sorts and publishes in order inside `EngineCompile.cpp::scheduleRows()`; `PassStage` is only a tie-break within one priority; a cyclic component is skipped as a whole and reported once by the engine.

## Consequences

- Good: one order and one frame vocabulary; the backend materialises `FrameDescription.passes` without reordering and without reporting cycles.
- Cost: the backend cannot schedule on its own; any change in scheduling semantics edits the engine.
- Reopening condition: a second backend needs different scheduling semantics (none today).

## Evidence

- `backend::FrameGraph`, `FrameCompiler::graph()` and `FrameGraphTest` are deleted.
- `FrameScheduleTest` covers scheduling; `vsg_rewrite_gate.sh` reports 0 VUID / 0 SYNC-HAZARD over two runs.
