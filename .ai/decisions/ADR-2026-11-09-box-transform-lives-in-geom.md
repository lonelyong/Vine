# ADR-2026-11-09: the box transform operator returns to geom; `Obb` is not offered yet

> **Status**: Accepted
> **Date**: 2026-11-09
> **Scope**: `src/base/geom/sdk/vine/geom/spatial/Aabb.hpp`, `src/viz/graphics/sdk/vine/graphics/Node.hpp`,
> `src/viz/graphics/src/{Node,Geometry,Scene,RayIntersection}.cpp`, `tests/test_geom_spatial/AabbTest.cpp`,
> `tests/test_graphics/GraphicsTest.cpp`

## Background

"Put a local-space axis-aligned box into the coordinate system of an accumulated matrix" had two implementations in `graphics`:
`graphics::transformBox()` (a public API in `Node.hpp`) and the private `transformBounds()` in `RayIntersection.cpp`;
there were six call sites (a `Geometry`'s instance box and world box, a `Group`'s subtree union, `Scene`'s bounds cache, the picking rejection box).
9.2 of `geometry-primitives.md` had long ago registered the gap: "the type has moved home, the transform operator is still on the consumer's side".

## Decision

1. `Aabb<T>::transformed(const math::Matrix4x4<T>&)` goes into `geom`: transform the eight corners and refit; it is defined only for an affine matrix,
   an empty box or a non-affine input answers an empty box, and `T` is restricted to floating point (an integer box times a matrix has no well-defined rounding).
2. `graphics::transformBox()` and `RayIntersection.cpp`'s `transformBounds()` are deleted; the six call sites become
   `box.transformed(matrix)`, and **no forwarding alias is left**.
3. `Obb::transformed` is not offered yet; the gap and its trigger condition are recorded in 9.2 of `geometry-primitives.md`.

## Rationale

- **`G1` / `G7` / `G13`**: this is a geometric fact of the value type itself, not render vocabulary. One operator with two homes drifts on the first change.
- **No forwarding**: repository practice is to change everything at once and leave no compatibility alias (`graphics::Ray` was handled the same way); keeping `transformBox()`
  would only give one operation two names during the transition (`G4`).
- **The `Obb` representation**: it stores a center plus half-extents plus a quaternion and is exact only for rigid transforms (and uniform scaling) - the image of a general affine
  map is a parallelepiped, which this representation cannot hold. `Obb` has zero production consumers today, so settle the semantics first (`G8`).

## Rejected options

| Option | Rejection reason |
|---|---|
| Keep `graphics::transformBox()` as a one-line forwarder | Two names for one operation (`G4`); this repository removes an old name all at once. |
| Return `Obb<T>` (the exact image) | A scene box and a picking rejection box want a bound; the exact image has zero consumers. |
| Return `std::optional<Aabb<T>>` | An empty box already is the representation of "no box", and wrapping it again carries no information. |
| Add `Obb::transformed` as well | Zero production consumers, and its semantics under a non-rigid transform are undecided (`G8`). |
| A free function `spatial::transformBox(aabb, m)` | Inconsistent with the value-type style already in place; `intersectTriangle` is a free function only because a triangle is not a value type. |

## Consequences

- `Aabb.hpp` gains a dependency on `math/Transform3.hpp` (the only definition of the matrix × point operator). The compile coupling rises from "point / vector" to
  "matrix", but `Aabb` already included `Ray.hpp`, so this is not a new property.
- The public API drops `VN_GRAPHICS_API transformBox`; the only call sites in the repository were `graphics` itself and one test, and all are changed.
- A non-affine matrix changes from "silently computes a wrong box" to "answers an empty box", watched by a case in `AabbTest.cpp`.
- The paragraph in `Node.hpp` that described `transformBox()` is deleted; `GraphicsTest` becomes
  `geom->localBounds().transformed(geom->worldMatrix())`, which still pins "the walk agrees with the self-answer".
