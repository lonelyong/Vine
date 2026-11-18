# ADR-2026-11-04: geometry families are classified by empty tags, not by virtual base classes

> **Status**: Accepted (the curve half is superseded by `ADR-2026-11-04-curve-evaluation-interface.md`: `Curve<T>` is now a virtual interface)
> **Date**: 2026-11-04
> **Scope**: `src/base/geometry/sdk/vine/geometry` (the curve and surface families completed and renamed one type per header: `Curve` / `LineCurve` / `CircleCurve` / `ConicCurve` / `BoundedCurve` / `OffsetCurve` / `Surface` / `ElementarySurface` / `BoundedSurface` and the placeholder types; the existing type declarations' placement), `tests/test_geometry` <!-- drift-ok -->

## Background

The geometry types had no classification relationship: "a curve / a surface / a solid" existed only in discussion, and the code could neither constrain by family nor answer "how many dimensions is this" at compile time. The only runtime classification was `ShapeType`, and it covers only `Shape` subclasses.

Two changes that landed the same day made the cost of this decision concrete:

- the short queries of `Aabb` / `Obb` / `LineCurve` / `Plane` / `Ray` moved from `.cpp` into headers and inline across modules;
- the per-triangle ray test was measured at 7.09 -> 5.99 ns (hit) and 3.96 -> 2.71 ns (parallel rejection, `geometry-ray.md` 2.7).

## Decision

1. `Curve` / `Surface` are **empty tags**: no state, no virtual functions, only `static constexpr int kDimension` (1/2).
2. The curve tree follows CAD-kernel vocabulary: `LineCurve` under `Curve`; `ConicCurve` for the four conics (`CircleCurve`, `EllipseCurve`, `HyperbolaCurve`, `ParabolaCurve`, all implemented); `BoundedCurve` for bounded curves (`BezierCurve` / `BSplineCurve` still placeholders); `OffsetCurve` a placeholder.
3. The surface tree likewise: `ElementarySurface` (`Plane` and `SphericalSurface` implemented, three placeholder) and `BoundedSurface` (three placeholders).
4. One type per header; an unimplemented type is declared only, with its default constructor deleted, so it cannot be constructed.
5. Family membership is declared once, at the type definition; the meaning of the parameters stays with the concrete type (`Ray` is a distance, `LineCurve` a direction multiplier, `CircleCurve` radians).
6. Runtime polymorphism still uses `std::variant` + `std::visit`.
7. The zero cost is guarded by tests: `static_assert` asserts membership, non-membership, unchanged `sizeof` and trivially copyable.

## Rationale

- `G10`: a virtual base class would add a vptr to `Ray`, `LineCurve` and `Box`, breaking copy by value and cross-module inlining and giving back the benefit just measured.
- `G1`, `G12`: classification needs a name and one declaration site; an empty tag writes that into the type system without adding a runtime mechanism.
- `G8`: only classification and the implementations verified by tests are added; family-level algorithms (intersection, distance) have no consumer and go to the backlog.
- `G9`: adding a member is one header plus one derivation declaration, and the consumers are pointed at by compile errors rather than by convention.

## Rejected options

| Option | Why not |
|---|---|
| Make the three families virtual base classes that concrete types inherit publicly | One more vptr per value type, and `Ray` is copied by value in hot loops; the inlining benefit and trivially copyable are lost together (`G10`) |
| Offer concept constraints only, no tags | Compile-time questions such as `is_base_of` lose their answer, and the family loses its documentation home; runtime still needs a variant over the same set (`G1`) |
| Hoist `pointAt(t)` into a base class with one definition | The four curves mean different things by `t`, and one signature would hide the difference in documentation (`G4`) |
| Do no classification and wait for a consumer | Classification is a naming and documentation question whose cost is one-off and verifiably zero; without it a new geometry type has nowhere to declare its family (`G12`) |

## Consequences

- The layout, semantics and ABI of existing types are unchanged (empty base optimisation), so the change is purely additive.
- Four value types are added; the names of the point and surface families do not clash with `QuadMesh` or `TriangleMesh` (the former is a single face, the latter mesh storage).
- Family-level algorithms are still missing and are recorded in `../backlog/geometry.md`.
- The current design is recorded in `.ai/design/geometry-kinds.md`.
