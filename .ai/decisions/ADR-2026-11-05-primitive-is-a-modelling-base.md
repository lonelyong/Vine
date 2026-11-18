# ADR-2026-11-05: `Primitive` is a modelling base class and holds no solid

- Status: Accepted
- Date: 2026-11-05
- Supersedes: the part where "the model holds a `SolidShapef` by value" (git holds that record; the `Solid<T>` family itself is unchanged)
- Related: `../design/geometry-parametric.md`, `../design/geometry-solid.md`, `../../src/base/geometry/sdk/vine/geometry/Primitive.hpp`, `../../tests/test_geometry/SolidTest.cpp`

## Background

The approach earlier that day was: `Primitive` was the **only** modelling class, holding a `SolidShapef` (the variant of the five analytic solids) by value, with `shapeType()` derived from the variant and consumers using `std::visit`.

That approach wrote an assumption into the base class: **every primitive is an analytic solid**. In reality it is not:

- a description file will contain primitives without an analytic solid (an extruded profile, a swept face, possibly text or a curve profile later);
- once the base class member is a `SolidShapef`, those primitives either cannot be expressed or are forced into one of the five solids;
- the model consumers (the `brep` bridge, description file reading and writing, tessellation) therefore recognise only "the variant" and cannot answer by subclass.

At the same time the modelling code needs **names** such as `Box` / `Sphere`: identity (name / revision / tag), their own dimensions, and `obj_cast`. The earlier shape (`ADR-2026-11-04-primitive-is-a-shape.md`) was exactly that, and it was replaced that day by "the variant is held", at the price of making "a primitive is necessarily a solid" part of the base class's definition.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the variant (that day's shape) | No change; the tag cannot drift; the consumer visits once | The base class assumes "a primitive is a solid"; a non-solid primitive has nowhere to go; the modelling code has no `Box` name |
| B. The base class holds no geometry; each of the five subclasses holds its own dimensions and offers `solid()` | The concept only requires "answers bounds and its own identity"; a non-solid primitive can be derived naturally; the modelling side has concrete class names | One set of dimension fields per subclass; `bounds()` becomes pure virtual; consumers dispatch over the closed set |
| C. Each subclass stores a `SolidShapef` plus a set of dimension fields | `shape()` is an indexed access | Dimensions in two homes, which must drift (`G1`) |
| D. The subclasses act as "views" reading and writing the variant the base class holds | Dimensions in one home | Still A's assumption; `setSolid()` breaks the type invariant (`G11`) |

## Decision

Chose B.

- `Primitive : Shape`: a protected constructor `Primitive(ShapeType)` and **only** the pure virtual `bounds()`; it holds no geometry and does not know `Solid`.
- `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`: `final` subclasses, each holding its own dimensions (`width_` / `height_` / `depth_`, `radius_`, `height_`, `radius_x_`, ...), with the dimensions **full-size** as the description file writes them.
- Each subclass also offers `solid()`: it **constructs** its own concrete kind from the current fields (`BoxSolidf{ width_ / 2, ... }`) and returns a **value**. The dimensions exist once, and the solid is neither stored nor cached, so there is no invalidation point.
- `isValid()` / `hasVolume()` / `bounds()` are implemented by the subclass and forwarded to that construction in `solid()`: the formulas stay on the geometry side (`G13`).
- The five geometry kinds are renamed `BoxSolid` / `SphereSolid` / `CylinderSolid` / `ConeSolid` / `EllipsoidSolid` (aliases `BoxSolidf` / `BoxSolidd`, ...), giving the `Box` / `Sphere` names to the modelling side.
- Two access entries, both in `Primitive.hpp`:
  - `visitPrimitive(primitive, visitor)`: hands the concrete subclass to the visitor over the closed set, returning `false` when it is not one of the five.
  - `solidShapeOf(primitive)`: returns `std::optional<SolidShapef>`, empty when it is not a solid.
- `Primitive::shape()` / `Primitive::solid()` / `Primitive::setSolid()` / `Primitive(const SolidShapef&)` are removed.

## Consequences

- Good: the base class no longer claims "a primitive is a solid", and a non-solid primitive can be derived (`SolidTest.cpp`, beyond `GeometryKindsTest`, measures this with a `Profile` class).
- Good: modelling code writes `make_intrusive<Box>(2, 4, 6)`, `box.setWidth(...)`, `obj_cast<Box>(shape)`; reading and writing a description file answers by subclass.
- Good: `Box` and `BoxSolid` are distinguishable at the call site (`G12`), and the `Box` name no longer collides with a geometry value.
- Bad / cost: each subclass repeats three forwards, `isValid()` / `hasVolume()` / `bounds()` (15 three-line functions); a consumer constructs one value each time it takes geometry from a model (`SolidShapef` is about 24-40 bytes, no allocation); adding a solid means touching both the geometry kind and the modelling subclass.
- Trigger (when to reopen): a container that must hold primitives **heterogeneously** appears (`vector<unique_ptr<Primitive>>` is already that, so it does not count), or the modelling side needs to replace the solid without changing the subclass - then consider a third shape beyond B -> A.

## Evidence

- Tests: `tests/test_geometry/SolidTest.cpp` covers "the base class holds no geometry" (`Profile`), "a subclass constructs its solid on demand", "each of the five reports its own tag", "`solidShapeOf` recognises only the five" and "`visitPrimitive` hands out the concrete type"; `SolidDistanceTest` / `SolidIntersectionTest` / `SolidTessellationTest` go from subclass to solid; `test_brep`, `test_robotics_io`, `test_proximity`, `test_graphics`, `test_meshio`, `test_brepio` and `test_gui` are all green. <!-- drift-ok -->
- Gates: `check_ai_docs.py --strict`, `check_design_docs.py`, `check_public_docs.py`, `check_include_hygiene.py` and `check_export_annotations.py` as described in the commit message.
