# geom-planar backlog

> **Status**: open items
> **Scope**: the follow-up types and queries of `src/base/geom/sdk/vine/geom/planar`. Current design:
> `../design/geometry-planar.md`.

## 1. Actionable Now (No Trigger)

Empty.

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| P3 | The tight box of a segment over a non-circular base curve | `TrimmedCurve` erases the base curve, so there is no support function to ask; today only `CircleCurve::bounds(first, last)` can answer a tight box | a consumer that trims or picks per segment and cannot accept a loose box | `../design/geometry-planar.md` 2.10, 9.2 |

## Criterion reminders

- Every item's trigger must be an **observable fact** (a consumer, a call site), not "it might be useful later" (`G8`).
- The planar types are value types, so any new capability first answers "why can it not be a free function" (`G7`,
  `G12`).
- A new type answers three questions at the same time - what the criterion of `isValid()` is, what a degenerate state
  answers, and what the dimension of `derivative` is - all three, none optional.
