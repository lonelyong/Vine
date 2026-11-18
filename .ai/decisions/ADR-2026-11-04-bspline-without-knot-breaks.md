# ADR-2026-11-04: a B-spline's internal multiplicity does not exceed its degree; a gap is not one curve

> **Status**: Accepted
> **Date**: 2026-11-04
> **Related**: `../design/geometry-kinds.md` 2.8 / C13 / P10; `src/base/geometry/sdk/vine/geometry/BSplineBasis.hpp` (`isClampedKnotVector`, `knotContinuity`), `BSplineCurve.hpp`, `BSplineSurface.hpp`; `tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## Background

`isClampedKnotVector()` used to require only that the multiplicity of each distinct knot lie in `[1, degree + 1]`, with the two ends exactly `degree + 1`.
So an **internal** knot's multiplicity could also reach `degree + 1`. Mathematically that cuts the curve (or one direction of a surface) into two unrelated parts: the point **jumps** when the parameter crosses that knot, and each side is an independent B-spline segment.

The rest of the framework would not know about this:

- `knotContinuity()` computes `degree - m`, which gives `-1` when `m = degree + 1` and is clamped to `C0`. So `continuity()` answers `C0`, while the definition of `Continuity::C0` is "position continuous: the segments meet at one point" - a lie about an object whose segments do not meet at all;
- `isCN(0)` is true with it, and `isValid()` reports usable with it, so "the single decision entry" (`geometry-kinds.md` C12) fails on this input.

This is not a theoretical construction: a multiplicity like `{3, 3, 3}` is common when a spline is rebuilt from CAD data, as soon as a caller feeds a cut piece of data in as one curve.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. `isValid()` rejects an internal multiplicity of `degree + 1` | The public enum is unchanged; `continuity()`'s `C0` is true again ("meet at one point"); the judgement "a piece of data is not one curve" goes to the caller as an explicit split | Narrows the accepted data: parts ways with OCCT (`Geom_BSplineCurve` allows an internal multiplicity up to `degree + 1`); a rebuild has to split into several curves |
| B. Add a "discontinuous" bottom step to `Continuity` (for example `Discontinuous`) and answer truthfully | Stays OCCT-compatible and accepts any multiplicity | The public enum and the semantics of `isCN()` both have to be reinterpreted; `isClosed()`, `reversedParameter()` and every traversal algorithm would have to handle "a curve with a hole"; and nothing in the repository needs it today (`G8`) |
| C. Keep the status quo and warn in the documentation | No change | The interface keeps lying about legal input (`G4`), and `isCN(0)` and `isValid()` both give a wrong answer, which is worse than a missing capability |

## Decision

Chose A.

`isClampedKnotVector()` requires `degree + 1` at the two ends and at most `degree` inside. Because `degree - m >= 0` always holds, the `Continuity` ladder needs no new bottom step, and the meaning of `C0` (the segments meet at one point) is exact again.

"One curve" therefore has a checkable definition: **the point set over the parameter domain is connected**. A caller that must keep a break splits it into two curves (`TrimmedCurve` and `BSplineCurve` both suffice), which is cheaper than teaching every algorithm to handle a hole (`G1`, `G10`).

## Consequences

- Good: `isValid()`, `continuity()` and `isCN()` agree with each other again; `degree - m >= 0` in `knotContinuity()` becomes a fact that can be cited rather than a clamped coincidence.
- Good: it points the same way as the existing trade-off - `BSplineCurve` / `BSplineSurface` already describe only clamped, non-periodic forms (`geometry-kinds.md` P5), and "one connected polynomial piece" is the natural continuation of the same narrowing.
- Bad / cost: the accepted range differs from OCCT. Data read from OCCT or STL with an internal multiplicity of `degree + 1` makes `isValid()` false, and the caller must split. The difference is written into the header's `@details` and into P10.
- Trigger (when to reopen): a consumer that genuinely needs **a hole inside one curve** appears (for example storing a whole STEP edge table as one object); then add `Continuity::Discontinuous` along option B and make the traversal algorithms handle it explicitly, instead of going back to "clamped to C0".

## Evidence

- `src/base/geometry/sdk/vine/geometry/BSplineBasis.hpp`: the new per-item upper bounds in `isClampedKnotVector()` and the note on `knotContinuity()`. <!-- drift-ok -->
- `tests/test_geometry/GeometryKindsTest.cpp`: `aKnotVectorThatBreaksTheCurveIsNotACurve` - both the curve and <!-- drift-ok -->
  the surface of `{3, 3, 3}` (degree 2) report `isValid()` false; `{3, 2, 3}`, whose multiplicity equals the degree exactly, is still valid with `continuity() == C0`, `isCN(0)` true and `isCN(1)` false.
