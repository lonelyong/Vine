# ADR-2026-11-06: the spatial curve family answers the third derivative, and `OffsetCurve` becomes a constructible value type

- Status: Accepted
- Date: 2026-11-06
- Supersedes: the earlier decision against a third derivative (git holds that record)
- Related: `../design/geometry-kinds.md` 2.7 / 2.8 / D9, `ADR-2026-11-05-curve-derivative-capability.md`, `../design/geometry-planar.md` 2.13

## Background

The decision of 2026-11-04 was "do not add a third derivative to `Curve<T>`, so `OffsetCurve` stays abstract". The capability protocol (`maxDerivativeOrder()` / `derivativeAt()`) changed that premise: the third derivative can exist only as **a capability of the concrete types**, and the contract need not gain a pure virtual. The conclusion "the offset must be abstract" therefore expired - the reason it held was "a runtime basis curve cannot be asked for a third derivative".

The planar side had also landed, on 2026-11-05, the shape "the offset holds a handle, and the order is the basis capability minus one"; if the spatial side stayed abstract the same concept would have different shapes in the two modules (`G6`), and the spatial offset would be unconstructible and unable to join family sweeps.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the status quo (the offset abstract, curves answering two orders) | No change | The spatial offset cannot be constructed; the same concept has different shapes in the two modules; a trimmed piece cannot be offset either |
| B. Add a pure virtual `thirdDerivative()` to `Curve<T>` | The offset can compute directly | The ladder does not close (the offset's D3 needs the basis D4); every implementer pays; it duplicates the capability protocol (`G8`) |
| C. A non-virtual `thirdDerivative()` on the concrete types plus overrides of `maxDerivativeOrder() == 3` / `derivativeAt()`, with `OffsetCurve` deciding by capability and becoming concrete | The contract is unchanged; the ladder is expressed by capability; the two modules share a shape | Two more overrides per type that can answer the third order; one more third-derivative implementation |
| D. Make only `OffsetCurve` concrete and let the curve family keep answering two orders | A smaller change | No basis answers the third order => the offset is forever `isValid() == false`, i.e. not implemented at all |

## Decision

Chose **C**.

- The concrete types gain a non-virtual `thirdDerivative(u)` and override `maxDerivativeOrder() == 3` and `derivativeAt(u, 3)`: `LineCurve` (zero), `CircleCurve` (`-P'`), `EllipseCurve` (`-P'`), `HyperbolaCurve` (`P'`), `ParabolaCurve` (zero), `BezierCurve` (the third difference times `n(n-1)(n-2)`, with one quotient-rule layer for the rational form), `BSplineCurve` (the third layer of basis functions plus the quotient rule). `TrimmedCurve` is unchanged and already forwards to the basis.
- `geom::spatial::OffsetCurve` drops the pure virtual `secondDerivative`: with `N = e x p'`, `R = |N|` and `k = (N.N')/R^2`, it writes `U'' = N''/R - 2 N' k/R + U (k^2 - k')`, where `N'' = e x p'''` comes from `basis_curve->derivativeAt(u, 3)`.
- `maxDerivativeOrder()` reports `min(2, basis capability - 1)` (the same rule as the planar side), `isValid()` gains "the basis can answer the third order", and `derivativeAt()` answers `std::nullopt` out of range.

Criteria: `G1` (the order is a capability, not a contract item), `G4` (out of range answers empty, the capability decides `isValid()`, and no zero is fabricated), `G6` (the 2D and 3D `OffsetCurve` share a shape), `G9` (a new curve only reports its capability), `G10` (the offset's second derivative costs one more virtual call and no numerical difference).

## Consequences

- Good: `OffsetCurve` is constructible, can join family sweeps and can offset trimmed pieces; the two modules' offsets have the same shape; "the offset's nth order needs the basis's (n+1)th" is a queryable fact; `secondDerivative` is an exact value rather than a difference approximation.
- Bad / cost: each type that can answer the third order gains two overrides and one closed-form implementation; an offset of an offset is still a "position plus velocity" view (`isValid()` false, `maxDerivativeOrder() == 1`); the offset still offers no `bounds()`.
- Trigger (when to reopen): when the offset's third derivative is needed (rate of curvature change, G3 joining), the rule drops automatically to "the basis must answer 4", and then it is decided whether to add a fourth derivative to the concrete types or stop the offset at that step.

## Evidence

- `src/base/geom/sdk/vine/geom/spatial/`: `thirdDerivative` and the capability overrides of `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `BezierCurve` / `BSplineCurve`; `secondDerivative` / `maxDerivativeOrder` / `derivativeAt` / `isValid` of `OffsetCurve`.
- `tests/test_geom_spatial/GeometryKindsTest.cpp`: `static_assert(!std::is_abstract_v<OffsetCurve<double>>)`; `everySpaceCurveAnswersItsThirdDerivative` (the third derivative of eight types agreeing with a central difference of the second, both rational forms included, checked against four closed-form relations); `theSpaceOffsetAnswersASecondDerivativeExactly` (the second derivative agreeing with a difference of the first, a zero distance giving the basis); `anOffsetOfAnOffsetStopsAtTheVelocity` (reports 1, `isValid()` false, a second derivative answering empty, the position still exact); `sweepCurves` covers the offset in both precisions.
