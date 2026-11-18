# ADR-2026-11-04: the offset curve's second derivative is not implemented, and `Curve<T>` gains no third derivative

> **Status**: Superseded (by `ADR-2026-11-06-space-third-derivative-and-offset.md`: once the capability protocol landed, the spatial curve family answers the third derivative and `OffsetCurve` became a constructible value type)
> **Date**: 2026-11-04
> **Related**: `../design/geometry-kinds.md` 2.7 / 2.8 / D9; `src/base/geom/sdk/vine/geom/spatial/Curve.hpp`, `OffsetCurve.hpp`; `tests/test_geom_spatial/GeometryKindsTest.cpp` <!-- drift-ok -->

## Background

Once the evaluation of `BezierCurve` / `BSplineCurve` had landed, `OffsetCurve` had one pure virtual function left: `secondDerivative()`. By definition `P(u) = p(u) + d * N(u) / |N(u)|`, where `N(u) = reference_direction x p'(u)`, so

- `P'(u)` needs `p''` (already in the interface);
- the expansion of `P''(u)` contains `N''(u) = reference_direction x p'''(u)`, which is the **basis curve's third derivative**.

The very first version of `Curve<T>` excluded `D3` / `DN` from the interface explicitly (`ADR-2026-11-04-curve-evaluation-interface.md` item 7, `geometry-kinds.md` D6), because there was no consumer at the time.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Add a pure virtual `thirdDerivative()` to `Curve<T>` | The offset's second derivative is exact and aligns with OCCT's `Geom_Curve::D3` | Every concrete curve (lines, the four conics, the two splines, trims, the offset itself) has to implement it; the interface grows from ten items to eleven, adding a cost per new curve, while only one curve needs it today (`G8`) |
| B. Approximate with a central difference of `derivative()` | The interface is unchanged and `OffsetCurve` is constructible at once | The accuracy varies with the step and depends on the parameterisation scale; an implicit numerical error is hidden inside a contract that is meant to be exact (`G4`, `G11`) |
| C. Keep `secondDerivative()` pure and implement `evaluate()` / `derivative()` as usual | The interface does not widen and no numerical error is introduced; what is not implemented is stated honestly by the type system (an abstract type is not constructible) | `OffsetCurve` stays unconstructible and can only be tested through a derived class (a small stub that supplies only this item) |

## Decision

Chose C.

`OffsetCurve` re-declares `secondDerivative(T) const override = 0;` in the derived class and states the blocking reason at the declaration (the third derivative is missing, finite differences are refused). The type therefore stays abstract, exactly consistent with "this item is not implemented yet".

## Consequences

- Good: the interface size does not change, and the eight concrete curves need not pay for a capability without a consumer (`G8`); the offset's point and first derivative are already usable, and adding the third derivative later means one item on `Curve<T>` and one formula in `OffsetCurve`.
- Bad / cost: `OffsetCurve` cannot be constructed, and `evaluate()` / `derivative()` can only be called through a derived class; a user has to look once at why it is abstract.
- Trigger (when to reopen): a consumer that genuinely needs **an offset curve's second derivative** appears (for example the curvature of an offset curve, or a G2 continuity check), or a second algorithm needs the third derivative (for example jerk-continuous motion planning). Then extend the interface along A and update `geometry-kinds.md` 2.7 and `D6` with it.

## Evidence

- `src/base/geometry/sdk/vine/geometry/OffsetCurve.hpp`: the implementations of `evaluate()` / `derivative()` and the `= 0` <!-- drift-ok -->
  and its note on `secondDerivative()`.
- `tests/test_geometry/GeometryKindsTest.cpp`: `static_assert(std::is_abstract_v<OffsetCurved>)`, the <!-- drift-ok -->
  `OffsetStub` supplying only `secondDerivative()`, and `anOffsetCurveInheritsTheBasisDomain` covering domain forwarding and the displaced geometry.
- `CalculateD2` in OCCT's `Geom_OffsetCurveUtils.pxx` uses the basis curve's `D3`, i.e. the same blockage exists in the reference implementation.
