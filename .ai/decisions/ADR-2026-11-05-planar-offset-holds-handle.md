# ADR-2026-11-05: the planar offset holds a basis handle, and its order follows the capability

- Status: Accepted
- Date: 2026-11-05
- Supersedes: `ADR-2026-11-05-planar-offset-curve.md`
- Related: `../design/geometry-planar.md` 2.13, `ADR-2026-11-05-curve-derivative-capability.md`, `ADR-2026-11-05-planar-trimmed-curve.md`

## Background

The previous version made the planar offset `OffsetCurve<C>`, templated on the **concrete** basis type and holding it by value. That shape was forced by the contract at the time - the offset's second derivative needs the basis's third derivative, the contract declared only up to the second order, and the third derivative existed only as a non-virtual member of concrete types, so a runtime basis curve simply could not be asked for a third derivative.

Once the capability protocol landed, that constraint disappeared: a `Curve<T>&` can answer `maxDerivativeOrder()` and `derivativeAt(u, 3)`. Keeping a by-value template would preserve a workaround that is no longer necessary and would make legitimate combinations such as "offset a `TrimmedCurve`" or "offset another offset" something that has to be piled up out of template instantiations.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep `OffsetCurve<C>` as a by-value template | Zero allocation, and calls on a concrete type can inline | A runtime basis is unavailable; `OffsetCurve<TrimmedCurve<T>>` cannot express "any basis curve"; the shape differs from the same-named 3D type |
| B. `OffsetCurve<T>` holds a `shared_ptr<const Curve<T>>` and takes its order from the basis capability | The same shape as 3D; any basis curve; the capability is reported honestly | One heap allocation; the second derivative costs one more virtual call |
| C. Keep both (a value template plus a handle view) | Either cost is available | One concept with two names and two sets of rules (`G1`) |

## Decision

Chose **B**: `OffsetCurve<T>` stores a `shared_ptr<const Curve<T>>` and a signed distance; the `offsetCurve(basis, distance)` factory builds the handle from a concrete type.

The order rule is `min(2, basis.maxDerivativeOrder() - 1)`: the offset's nth derivative needs the basis's (n+1)th. When the basis answers 3 (every planar concrete type answers 3) the offset answers 2; when the basis answers 2 (another offset) the offset answers 1, `derivativeAt(u, 2)` answers `std::nullopt` and `isValid()` is false - when the basis cannot supply a third derivative, `secondDerivative()` answers a zero vector and honestly marks itself unusable instead of making a number up.

Criteria: `G1` (an offset is "another object plus a scalar", not a specialisation of some basis type), `G4` (the capability decides the available order, and beyond it the answer is empty), `G6` (a basis handle with the same shape as `geom::spatial::OffsetCurve`), `G9` (a new basis does not add an offset type), `G10` (one allocation instead of one offset instantiation per basis type).

## Consequences

- Good: a `TrimmedCurve` can be offset, an offset can be offset, any runtime curve can be offset; the 2D and 3D `OffsetCurve` now have the same shape (the spatial side was aligned with the planar one on 2026-11-06, see `ADR-2026-11-06-space-third-derivative-and-offset.md`); and "the offset's nth order needs the basis's (n+1)th" is a fact that can be queried for the first time instead of read in a comment.
- Bad / cost: one heap allocation; `bounds()` is still not offered; offsetting an offset yields a "position plus velocity" view (`isValid()` false), and a second derivative requires going back to the original basis rather than stacking offsets.
- Migration: `OffsetCurve<C> v(c, d)` -> `auto v = offsetCurve(c, d)`; a caller that already holds a `shared_ptr` uses `OffsetCurve<T>(handle, d)` directly.

## Evidence

- `src/base/geom/sdk/vine/geom/planar/OffsetCurve.hpp`: the handle basis, the `min(2, basis - 1)` of `maxDerivativeOrder()`, `derivativeAt` answering empty out of range, and `isValid()`'s capability check.
- `tests/test_geom_planar/OffsetCurveTest.cpp`: line / circle / ellipse / spline bases, the second derivative agreeing with the difference, a zero distance giving the basis, a cusp staying finite, a trimmed piece as the basis, and family algorithms accepting an offset.
- `TheDerivativeCapabilityReportsWhatEachTypeCanAnswer` in `tests/test_geom_planar/CurveContractTest.cpp`: the offset reports 2 and `derivativeAt(u, 3)` answers empty.
