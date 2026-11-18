# ADR-2026-11-03: spatial value types belong to geometry, and the axis-aligned box is renamed `Aabb`

> **Status**: Accepted
> **Date**: 2026-11-03
> **Scope**: `src/base/math`, `src/base/geometry`, `src/viz/graphics`, `tests/test_math`, `tests/test_geometry` <!-- drift-ok -->

## Background

`Line`, `Plane` and `Rect3` (the axis-aligned box) lived in `src/base/math`, while `Obb` from the same concept family landed in `src/base/geometry` during a new round of geometry work. `Rect3` also owned the `math::Aabb` / `Aabbf` / `Aabbd` aliases, a forwarding shim at `geometry/Aabb.hpp`, and a third spelling in `Mesh::Aabb`: one axis-aligned box with three homes. <!-- drift-ok -->

Every consumer of `Rect3` sits above `geometry` (`Mesh`, `graphics::Node`, `ChainBuilder`, `ChainPresets`, `Obb::toAabb`), and `math` itself has no user. The only user of `Line` is `Plane`, and `Plane` has no consumer outside the module.

## Decision

1. The type definitions of `Line`, `Plane` and the axis-aligned box move into `vine::geometry`.
2. The axis-aligned box is named `Aabb`; `Rect3` and `math::Aabb*` no longer exist.
3. `Rect2` stays in `vine::math` and does not move.
4. No compatibility alias and no forwarding header are kept on the `math` side.

## Rationale

- One sentence decides it: `math` has only numbers and transforms, `geometry` has spatial entities. `G1`, `G5`.
- A type sits on the same side as its consumers, `Aabb` and `Obb` share a module and a name family, and `Plane`'s `Line` return value no longer crosses a module. `G1`, `G6`, `G12`.
- The only consumer of `Rect2` is a layout rectangle in `appfw::Gui`; moving it would bring UI vocabulary into the geometry module and add a `geometry` dependency to appfw's public headers. `G1`, `G5`, `G8`.

## Rejected options

| Option | Why not |
|---|---|
| Keep `Rect3` as the type name | A 3D "Rect" is a misnomer and does not pair with `Obb`; the aliases have already drifted into three copies. |
| Leave `using Rect3 = geometry::Aabb` in `math/Rect3.hpp` | `math` would then depend back on `geometry`, a back edge (`G2`); a one-version transition is not worth a back edge. |
| Move `Rect2` into `geometry` as well | Its consumer is a GUI layout rectangle, not a shape; symmetry would introduce an unrelated responsibility. |
| Rename without moving the module | `Obb` and `Aabb` would live in two modules forever, and `toAabb()` would keep taking a type across modules. |

## Consequences

- Every `vn::math::Rect3*` / `vn::math::Aabb*` reference was changed to `vn::geometry::Aabb*` in one pass; this is a breaking change, changed in the repository in the same batch, with no transitional alias kept.
- A caller that only wants a bounding box has to link `vn::Geometry` (plus `vn::Core`). Every consumer does so today, so no dependency edge is actually added.
- The template instantiations of `Aabb` move into `geometry` with the source files, and the export macro becomes `VN_GEOMETRY_API`.
- The current design is recorded in `.ai/design/geometry-primitives.md`.
