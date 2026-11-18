# ADR-2026-11-05: the planar offset is a template on the concrete type, and `Curve<T>` gains no third derivative

- Status: Superseded (by `ADR-2026-11-05-planar-offset-holds-handle.md`: once the capability protocol landed, the offset holds a basis handle)
- Date: 2026-11-05
- Related: `../design/geometry-planar.md`, `ADR-2026-11-04-curve-third-derivative.md`

## Background

The second derivative of the offset curve `O = C + d * N` contains the basis curve's third derivative: `N` is the unit normal, and its second derivative expands to include `C'''`. The spatial conclusion was "do not add a third derivative to `Curve<T>`", so `geom::spatial::OffsetCurve` implements only the point and the first derivative, `secondDerivative` is still pure, and the type stays unconstructible.

The planar side reached the same decision point: copy that conclusion (the offset reaches only the first derivative, or is not done at all) or write it differently so the offset is complete.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Add a twelfth pure virtual `thirdDerivative()` to `Curve<T>` | The offset can hold a `Curve<T>` handle directly; the same name as 3D | The contract grows from eleven to twelve items and the two modules' contracts are no longer item-for-item synonymous; every curve type is forced to implement a derivative only the offset uses |
| B. The offset is a template on the **concrete basis type**, with the third derivative a non-virtual member of that type | The contract stays at eleven items; the third derivative is added only to types that can answer it in closed form; the offset holds the basis by value, with zero allocation and no type erasure | The third derivative becomes eight duplicated implementations (one member per type); the offset cannot hold a `Curve<T>&` handle |
| C. The basis is a `shared_ptr<const Curve<T>>` and the second derivative is a central difference of the first | The offset can hold any curve | `secondDerivative()` is approximate from then on; R2 gains one more exception; the choice of step enters the public semantics |
| D. Do no offset | Zero cost | The most common planar need - a 2D profile or cutter compensation - cannot be written |

## Decision

Chose B.

Criteria: `G4`, `G10`, `G6`. The contract is the module's core asset (`geometry-planar.md` 2.1: eleven items synonymous with 3D) - changing it so that one derived type can compute derivatives above the first makes every curve type, and every algorithm that relies on "the contract stops at the second order", pay with it; and option C quietly turns "the second derivative is exact" into an approximation, which is far worse than one type too few.

Putting the third derivative on the concrete types also satisfies another existing criterion: value types already have cheap non-virtual entries (`pointAt()`, `bounds()`), and the third derivative is one of them; `Curve<T>`'s eleven items are unchanged, and a new curve type **need not** implement it - it simply cannot be used by an offset. The cost is eight small implementations, one line each (the zero vector for a line, `r(sin, -cos)` for a circle, `a*sin*X - b*cos*Y` for an ellipse, `P'` for a hyperbola, the zero vector for a parabola, a forward for an arc, the third difference for a Bezier, the third derivative of the basis functions for a BSpline).

## Consequences

- Good: the three numbers an `OffsetCurve<C>` answers are exact; the basis is held by value with no heap allocation; the planar contract stays item-for-item synonymous with 3D; `test_geom_planar` can verify it point by point against a central difference of the first derivative.
- Bad / cost: the offset's basis is fixed at compile time, family algorithms such as `ClosestPoint` accept it, but "decide at runtime which curve to offset" forces the caller to dispatch; the third derivative is written once per type across eight types, and a new type does not gain it automatically.
- Trigger (when to reopen): a consumer that needs "offset a `Curve<T>&`" appears (a curve chosen at runtime, an offset inside a heterogeneous list). Then compare A against a type-erased offset wrapper; there is no such consumer today.

## Evidence

- `src/base/geom/sdk/vine/geom/planar/OffsetCurve.hpp`: the template shape, the by-value basis and the place the third derivative is used.
- `thirdDerivative()` on the eight types: `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve` / `BezierCurve` / `BSplineCurve`.
- `tests/test_geom_planar/OffsetCurveTest.cpp`: a parallel offset of a line, a counter-clockwise circle shrinking inwards, the second derivative of an ellipse and a Bezier agreeing with the difference, a reversed basis requiring a negated distance, a zero distance giving the basis itself, and a cusp producing no NaN.
- The existing spatial decision: `ADR-2026-11-04-curve-third-derivative.md` (it governs 3D, and this ADR does not overturn it).
