# ADR-2026-11-09: the closed set of analytic solids is no longer expressed as a variant

> **Status**: Accepted
> **Date**: 2026-11-09
> **Scope**: `src/base/geom/sdk/vine/geom/spatial/SolidShape.hpp` (deleted), `BoxSolid.hpp` … `EllipsoidSolid.hpp`, <!-- drift-ok -->
> `src/base/geom/sdk/vine/geom/spatial/SolidTessellation.hpp`, `src/base/models/sdk/vine/models/PrimitiveDispatch.hpp`,
> `src/viz/graphics/src/Geometry.cpp`, `tests/test_geom_spatial/SolidTest.cpp`, `GeometryKindsTest.cpp`

## Background

`ADR-2026-11-05-solid-family-and-variant.md` introduced `SolidShape<T> = std::variant<the five kinds>`. It had two uses at the time:
`Primitive` held it by value (withdrawn by `ADR-2026-11-05-primitive-is-a-modelling-base.md`), and it was the argument for "answer by kind".

Before this round, only three readers were left: the argument of `tessellateSolid()`, the return value of `solidShapeOf()`, and `solidOf()`'s family view
(production code used it only for one `isValid()`). After `ADR-2026-11-09-solid-tessellation-in-geom.md` moved tessellation into `geom`,
the argument could be a concrete kind directly - so `SolidShape` no longer carried any duty of its own, neither "storage" nor "dispatch":

- `solidShapeOf()` and `visitPrimitive()` are two spellings of the same thing (turn a `Primitive` into an answer by kind), and having both only forces
  a caller to choose one;
- `solidOf()` serves one `isValid()`, and every kind already has `isValid()` itself.

## Decision

1. Delete the variant type `SolidShape` (`SolidShape<T>`, `SolidShapef` / `SolidShaped` and `solidOf()` go with it). <!-- drift-ok -->
2. A geometric entry point that has to answer by kind becomes a template overload: `tessellateSolid(const BoxSolid<T>&, …)` … `(const EllipsoidSolid<T>&, …)`.
3. Delete `models::solidShapeOf()`; from a model to geometry there is only `visitPrimitive()`: the visitor takes the concrete subclass and calls its `solid()` and the
   matching overload.
4. `graphics::geometryFromShape()` becomes `visitPrimitive` + `tessellateSolid` + `meshFromTriangulation()`.

## Rationale

- **`G1`**: one concept, one definition. "Which analytic solids exist, and how is dispatch done by kind" has one answer: five kinds plus `visitPrimitive()`.
- **`G8`**: the variant has zero storage consumers today (a model does not store it, geometry does not return it); keeping it would only pre-build machinery for
  "one day we may want a value we can store".
- **`G4`**: while `solidShapeOf()` and `visitPrimitive()` coexist, a caller first has to know which one to use; after the deletion there is one path.
- **`G6`**: geometry value types are consistently `Xxx<T>` plus one overload per kind, and `std::variant` was the single exception here.
- **Exhaustiveness is not lost**: when a kind is added, the visitor passed to `visitPrimitive()` calls a `tessellateSolid` overload that does not exist, so the compile error
  is still there, only moved from the `std::visit` instantiation point to the call site - `brep`'s `solidOf()` overloads have used the same pattern for a long time.

## Rejected options

| Option | Rejection reason |
|---|---|
| Keep the variant and delete only `solidShapeOf()` | The variant still has tessellation as its only consumer, and `tessellateSolid` could only take it; that keeps a public type alive for one call (`G8`). |
| `tessellateSolid(const Solid<T>&)` plus a virtual `kind()` / virtual `tessellate()` | Each kind needs its own dimension to tessellate; a virtual interface would let a new kind be missed silently, and pushes mesh knowledge into the family interface (`G4`, `G9`). |
| Have `Primitive` expose a variant (restore `shape()`) | The dimensions would live in two homes (a model field and the variant), the shape `ADR-2026-11-05-primitive-is-a-modelling-base.md` already rejected. |

## Consequences

- Public API removed: the whole `SolidShape.hpp` header and `solidShapeOf()`; the tessellation entry point goes from one to five template overloads. <!-- drift-ok -->
- Tests: the variant assertions and the `solidShapeOf` case in `SolidTest` are deleted, the `visitPrimitive` cases stay and are strengthened;
  `GeometryKindsTest` keeps only the assertion "the kinds of the two precisions are different types".
- If a "storable analytic-solid value" is really needed one day (a column in a file format, say), add a storage-only type then,
  rather than pushing dispatch into it as well.
