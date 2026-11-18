# ADR-2026-11-04: parametric geometry is a modelling type (a `Primitive` subclass), and algorithms belong to the surfaces

> **Status**: Accepted
> **Date**: 2026-11-04
> **Scope**: `src/base/geometry`, `src/base/brep`, `src/robotics/io`, `tests/test_brep`, `tests/test_robotics_io`, `tests/test_proximity`, `tests/test_graphics` <!-- drift-ok -->

## Background

The five parametric solids (`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`) were subclasses of `Primitive` with their dimensions in private fields. Another road was tried earlier that day: turn them into value types without identity, hold them in `Primitive` with `std::variant`, and have consumers use `std::visit` / `as<T>()`.

On review both roads have costs, and the criterion is "do these shapes provide algorithms of their own":

- a `Box`'s ray intersection **is** `Aabb::intersectWith(Ray)`; it has no algorithm of its own;
- `Cone` / `Cylinder` intersection is essentially a **surface** algorithm (ray-cylinder, ray-cone) and belongs to the future `CylindricalSurface` / `ConicalSurface`;
- what the modelling side really needs is identity, name and revision, which only `Shape` provides.

At the same time OCCT's long-term duty was narrowed to **STEP parsing and tessellation**, with analytic construction (`BRepPrimAPI`) as a transitional bridge.

## Decision

1. Parametric geometry **stays on the modelling side**: `Primitive : Shape` is an abstract base class (protected constructor, takes a `ShapeType`), and `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` are its subclasses.
2. The five subclasses live **together with the base class in `Primitive.hpp`**: one header, which avoids the include burden of five files and lets the DLL exports happen implicitly with `Primitive.cpp`.
3. `shape_type_` is handed to the base class through the constructor chain, **no longer written by hand in every constructor**.
4. Geometry algorithms **do not enter** `Primitive`: a solid answers only `bounds()` / `isValid()` / `hasVolume()`; intersection and distance belong to the geometry value types (`Aabb::intersectWith(Ray)`) and the surfaces (`SphericalSurface` and so on, see `geometry-kinds.md`).
5. The `std::variant` holding and `as<T>()` are removed: consumers return to "dispatch on `ShapeType` and validate the `obj_cast`".

## Rationale

- `G1`, `G12`: the five are modelling parameters, not algorithm carriers; putting them on the "value" side would leave `Solid` without a meaning and give a radius or an edge length two homes, one on the solid and one on the surface.
- `G5`, `G7`: the five parameter records live in one header with the base class, and a consumer of a new shape is exposed at compile time by the `switch`.
- `G11`: the tag comes from the constructor chain, one class of error fewer than "written once per constructor".
- `G8`: no algorithm is introduced early for the convenience of "value-isation"; a surface algorithm lands only when it has a consumer.

## Rejected options

| Option | Why not |
|---|---|
| Value types without identity, held in `Primitive` by `std::variant` | `Box` / `Cone` provide no algorithm of their own, so "value" buys nothing; a radius or edge length would live both on the solid and on the surface; the identity, name and revision that modelling needs still require another type (`G1`) |
| One header per subclass | Five parameter records on one base class; splitting them makes every consumer include four definitions it does not use, and the DLL exports would need a "remember to include it in some TU" convention (`G7`) |
| Open virtual queries (`intersectWith` / `distance`) on `Primitive` | The algorithm surface would hang off the modelling type and consumers would have to remember whom to ask; it also conflicts with the layering "algorithms belong to surfaces" (`G8`) |
| Let `geometry` depend on OCCT and build analytic solids directly | Backend vocabulary enters the core module (`G2`), and a backend-less build would lose the contract |

## Consequences

- `Box` and the others carry a vptr and a reference count again, so copying is not byte-wise; `GeometryKindsTest` no longer asserts their `sizeof`.
- `aPrimitiveOutsideTheFiveProducesNoSolid` in `tests/test_brep` is restored (a subclass can lie about its `ShapeType`, so the consumer must validate the cast).
- The includes in `tests/test_graphics/MeshShapeTest.cpp`, `tests/test_robotics_io/*` and `tests/test_proximity` became `Primitive.hpp` (the five separate headers are deleted).
- The current design is recorded in `.ai/design/geometry-parametric.md`.
