# ADR-2026-11-04: the curve family is an evaluation interface, the surface family is still a classification tag

> **Status**: Accepted (item 6 is superseded by `ADR-2026-11-04-surface-evaluation-interface.md`)
> **Date**: 2026-11-04
> **Scope**: `src/base/geometry/sdk/vine/geometry/Curve.hpp`, `Continuity.hpp`, `BoundedCurve.hpp`, `TrimmedCurve.hpp`, `LineCurve.hpp`, `CircleCurve.hpp`, `Ray.hpp`, `tests/test_geometry` <!-- drift-ok -->

## Background

The same day's `ADR-2026-11-04-geometry-kinds.md` made `Curve` / `Surface` **empty tags**, because a vptr would break a value type's inlining and trivially copyable property. Half a day later a new shape of requirement appeared: algorithms need "an unknown curve can be evaluated, and its parameter domain and periodicity asked" (B-rep edges, traversal, sampling), which needs an **abstract interface**; and at that moment no consumer made the same demand of surfaces.

## Decision

1. `Curve<T>` becomes an **abstract interface**: virtual destructor plus evaluation and parameter-domain properties (`evaluate` / `firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period`), all pure virtual; derivatives, continuity and the reversed mapping are added in item 7.
2. `BoundedCurve<T>` offers non-virtual `startPoint()` / `endPoint()` (derived from `evaluate` plus the parameter domain), while the domain is still answered by the concrete type.
3. `TrimmedCurve<T>` implements the interface: it holds a `std::shared_ptr<const Curve<T>>` basis curve plus a parameter interval and forwards `evaluate`; closedness and periodicity are judged from the basis and the interval together.
4. `LineCurve<T>` / `CircleCurve<T>` implement the interface (a line: infinite domain, neither closed nor periodic; a circle: `[0, 2pi)`, closed with period `2pi`).
5. **`Ray` leaves the curve family**: it no longer derives from `Curve` and keeps only `pointAt`, which keeps it trivially copyable.
6. ~~The `Surface` family stays an empty tag (`ElementarySurface` / `BoundedSurface`) and pays no vptr.~~ Superseded by `ADR-2026-11-04-surface-evaluation-interface.md`: surfaces later gained the same consumer.
7. The interface's core properties are completed against OCCT's `Geom_Curve`: `derivative(u)` / `secondDerivative(u)` (D1/D2, returning velocity and acceleration vectors), `continuity()` (the ladder enum in the new `Continuity.hpp`) and `reversedParameter(u)` (the reversed parameter mapping), all pure virtual; the mutable `reverse()`, `bounds()`, `curveType()` and `D3`/`DN` are not taken, for the reasons in the rejected options.

## Rationale

- `G4`, `G9`: evaluation is the common contract of curve algorithms, and the interface writes "an unknown type can be asked" into the type system; a new curve is six functions.
- `G8`, `G10`: surfaces have no such consumer, so no vptr is paid in advance; `Ray` is copied by value on the picking hot path, measured at 7.09 -> 5.99 ns (hit) and 3.96 -> 2.71 ns (parallel rejection) per triangle from inlining, so it is excluded explicitly.
- `G1`: the interface has one name, `Curve<T>`, and there is no longer a "tag Curve plus interface Curve" pair of homes.
- `G4`, `G9`: the ten properties (point, first and second derivatives, parameter domain, closedness/periodicity/period, continuity, reversed mapping) are the minimal complete set for a parameterised curve; tangents and normals, sampling, curvature and seam decisions all depend only on them; the names follow OCCT's `Geom_Curve` vocabulary rather than inventing synonyms.
- `G4`: `derivative` returns a **velocity vector** and not a unit tangent, and `secondDerivative` returns acceleration and not a curvature vector; the difference is in the interface comments, not left for the caller to guess.

## Rejected options

| Option | Why not |
|---|---|
| Keep the whole `Curve` family as empty tags | Cannot express "an unknown curve can be evaluated"; callers would have to write their own `variant` + `visit`, and each consumer would copy the dispatch (`G7`) |
| Use `double` + `Point3d` in the interface (the CAD-kernel spelling) | `LineCurve<T>` / `CircleCurve<T>` are template value types, and a non-template interface could only be derived from by the `double` version, so a `float` version could never enter the family (`G6`) |
| Let `Ray` derive from `Curve<T>` as well | It is copied by value on the hot path, and a vptr would give back the measured inlining benefit and lose trivially copyable (`G10`) |
| `TrimmedCurve` holds an `intrusive_ptr` | It would need reference counting on `Curve<T>` (ADL `intrusive_ptr_release`), turning the interface into `RefCounted`; `shared_ptr` buys ownership only and does not change the interface (`G8`) |
| Offer a mutable `reverse()` as OCCT does | Curves are immutable and a basis curve can be shared by several trims, so an in-place reverse would interfere; reversal is expressed as "build a mirror plus `reversedParameter`", and a trim reflects only within its own interval (`G4`, `G10`) |
| Put `bounds()` into the curve interface | Only bounded curves can always answer; a line could only return an infinite box, disguising "unbounded" as a box (`G4`, `G8`) |
| A `curveType()` enum and `D3` / `DN` | `dynamic_cast` and the vtable already answer "what I am", so an enum is a second home; derivatives above the third have no consumer (`G1`, `G8`) |
| Put `transform` / `transformedParameter` into the interface | The coordinate system lives on the `Shape` node and a curve carries no placement; a transform in the interface would create a second coordinate semantics (`G1`, `G5`) |

## Consequences

- `LineCurve` / `CircleCurve` are no longer trivially copyable; each has one more vptr. Both are cold types.
- `Ray` is no longer a `Curve`: a caller that wants to treat a ray as a curve wraps it explicitly, and per `geometry-ray.md` 2.1 the two have different dimensions anyway.
- The placeholder types of the curve family (`EllipseCurve`, `BezierCurve`, `BSplineCurve`, `OffsetCurve`, ...) are **abstract**: not constructible, usable once the ten pure virtual functions are implemented.
- Ten pure virtual functions give "add one curve" a fixed cost; `LineCurve` (constant derivative, zero acceleration, `CN`, `-u`), `CircleCurve` (velocity `radius * (-X sin u + Y cos u)`, acceleration `centre - P(u)`, `CN`, `period - u`) and `TrimmedCurve` (four forwards, reversing reflected within its own interval) are implemented.
- The current design is recorded in `.ai/design/geometry-kinds.md` 2.1/2.2/2.3.
