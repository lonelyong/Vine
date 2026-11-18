# ADR-2026-11-04: the frame of a rotational surface lives in a new `AxisSurface` layer, and the in-plane axis rule moves into `Frame.hpp`

> **Status**: Superseded (by `ADR-2026-11-04-surface-placement-value-type.md`: the frame became the value type `Placement` instead of an inheritance layer)
> **Date**: 2026-11-04
> **Related**: `../design/geometry-axis-surfaces.md` 2.1 / 2.2 / 2.3 / C1 / D1, `../design/geometry-kinds.md` 2.9; `src/base/geometry/sdk/vine/geometry/AxisSurface.hpp`, `Frame.hpp`, `CylindricalSurface.hpp`, `ConicalSurface.hpp`, `ToroidalSurface.hpp`, `ConicCurve.hpp`, `Plane.hpp`; `tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## Background

The cylindrical, conical and toroidal surfaces had been placeholders (present in the tree, not constructible, without parameters). Implementing them required answering two questions first:

1. **Where the placement is stored.** All three revolve about an arbitrary axis, while among the existing surfaces `Plane` stores no in-plane frame (its two axes are derived from `normal`) and `SphericalSurface` has only a centre and a radius (the world Z axis). Under `C9` / `C1`, `ElementarySurface` is a **classification tag** and cannot carry state or virtual functions.
2. **Where the rule "project a preferred direction into the plane orthogonal to the normal" lives.** It had two copies: `ConicCurve::inPlaneAxis()` (the curve family, a protected static) and `Plane::xAxis()` (the same rule without a preferred direction).

OCCT's answer is the first one: `Geom_ElementarySurface` holds a `gp_Ax3 pos`, all five elementary surfaces (plane, cylinder, cone, sphere, torus) carry a placement and expose `Axis()` / `Location()` / `Position()` / `SetPosition()` uniformly; it has no intermediate layer like `AxisSurface` (the other branch with a frame is `Geom_SweptSurface`'s `basisCurve` + `direction`). This ADR decides precisely whether to copy that.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Each surface stores `origin` / `axis` / `x_direction` and normalises them itself | The existing type tree is untouched | Three frame rules that can drift apart (`G1`); `continuity() = CN` is written three times as well |
| B. Put the frame into `ElementarySurface` (**the OCCT way**) | One home, and `Position()` can be asked of any elementary surface | Violates `C9`: the tag now carries state; `Plane` / `SphericalSurface` would be dragged into these fields too (`G8`), and `Plane` is copied by value on the picking hot path (`G10`) |
| C. Add the intermediate layer `AxisSurface<T> : ElementarySurface<T>` holding the frame, leaving only scalars in the three surfaces | One home for the frame and `continuity()`, isomorphic to `ConicCurve` in the curve family | One more level in the type tree; three more vectors per surface |
| D. Also migrate `SphericalSurface` to `AxisSurface` | The surface family's frame is fully unified | A sphere's axis only changes the parameterisation and not the point set, so the migration is a breaking change without a consumer (`G8`), and the current `u` measured from +X would change |

## Decision

Chose **C**, together with:

- extracting "the in-plane axis" into `Frame.hpp`'s `inPlaneAxis(preferred, normal)`, shared by `ConicCurve` (with a preferred direction), `Plane::xAxis()` (preferred direction zero) and `AxisSurface`;
- `AxisSurface` providing `yAxis() = axis x x_direction` and `continuity() = CN`, with the three surfaces implementing only evaluation, derivatives, the parameter domain and their own `isValid()`;
- `SphericalSurface` not migrating (option D), on the rule written as one sentence: **a placement that changes the point set is stored, one that only changes the parameterisation is derived**.

## Consequences

- Good: a new rotational surface needs one scalar plus an evaluation formula; frame normalisation and `CN` exist in one place (`G1`, `G6`); `Plane` and `ConicCurve` no longer each maintain their own projection rule.
- Bad / cost: `AxisSurface` is a state layer and not a tag, so a reader has to distinguish "a tag (`ElementarySurface` / `BoundedSurface`)" from "a frame layer (`AxisSurface`)"; each surface carries three more vectors; the protected static `ConicCurve::inPlaneAxis` is deleted, and derived conic curves call the free function from then on.
- Bad / cost (relative to OCCT): the uniform accessor of `Geom_ElementarySurface::Position()` is lost; asking today means `dynamic_cast<const AxisSurface<T>*>` (consistent with D6, "the vtable answers what I am"), and widening the interface is discussed when a consumer that holds an unknown elementary surface and wants its axis actually appears.
- Trigger (when to reopen): a need for "a sphere with a placement" (for example an arbitrarily oriented spherical face in B-rep) - then migrate `SphericalSurface` to `AxisSurface` and accept the change of the parameter origin.

## Evidence

- `AxisSurface.hpp`: the frame, `yAxis()`, `continuity()`, `hasUsableAxis()`. <!-- drift-ok -->
- `Frame.hpp`: the single implementation of `inPlaneAxis`; `ConicCurve.hpp` / `Plane.hpp` now call it.
- `tests/test_geometry/GeometryKindsTest.cpp`: `aCylindricalSurfaceWrapsItsAxis`, `aConicalSurfaceWalksItsGeneratrix`, `aToroidalSurfaceClosesInBothDirections`, plus the static assertions `is_base_of_v<AxisSurface<double>, ...>` and three size assertions including `sizeof(CylindricalSurfaced)`. <!-- drift-ok -->
- OCCT `Geom_CylindricalSurface` / `Geom_ConicalSurface` / `Geom_ToroidalSurface` and `ElSLib::CylinderValue` / `ConeValue` / `TorusValue`: the source of the parameterisation conventions (a cone's `v` is the generatrix arc length, a torus's `v` is measured from the equatorial plane).
