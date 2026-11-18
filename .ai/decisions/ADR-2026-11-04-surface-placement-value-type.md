# ADR-2026-11-04: a surface's frame is the value type `Placement`, not an inheritance layer

> **Status**: Accepted (supersedes option C of `ADR-2026-11-04-axis-surface-frame.md`)
> **Date**: 2026-11-04
> **Related**: `../design/geometry-axis-surfaces.md` 2.1 / 2.5 / C1 / D1; `src/base/geometry/sdk/vine/geometry/Placement.hpp`, `Frame.hpp`, `CylindricalSurface.hpp`, `ConicalSurface.hpp`, `ToroidalSurface.hpp`; `tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## Background

`ADR-2026-11-04-axis-surface-frame.md` put the frame of the rotational surfaces (origin, main axis, X direction, plus `yAxis()` and `continuity() = CN`) into a new intermediate layer, `AxisSurface`, so that the three surfaces would not each store a rule that could drift.

Reviewing it after it landed showed that only three small things are actually shared: the initialisation of three fields (normalisation plus `inPlaneAxis`), the one-line cross product in `yAxis()`, and `return Continuity::CN`. To share them, a "state layer" was added to the type tree, readers had to tell "a tag (`ElementarySurface`)" from "a state layer (`AxisSurface`)", and the type tree no longer matched OCCT's shape (OCCT has `Geom_ElementarySurface` -> five concrete classes, with `gp_Ax3` in the tag).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the `AxisSurface` layer | The three small things share one place | One more level in the type tree; paying inheritance for a few fields and one `return CN` (`G8`); "tag versus state layer" has to be explained |
| B. Each type stores the three fields plus free functions | The type tree matches OCCT | The same initialisation copied three times (the rule stays in `Frame.hpp`, the calls are copied) |
| C. A `Placement<T>` value type held as a member | The rule and the fields have one home; the tree matches OCCT; `Placement` is reusable for a future `SurfaceOfRevolution`, for B-rep face placement, even for `ConicCurve` | One more public value type; `continuity()` written three times; field access becomes `surface.placement.*` |

## Decision

Chose C.

`Placement<T>` (`origin` + `axis` + `x_direction` + `yAxis()` + `hasUsableAxis()`, always right-handed, with no handedness switch) lives in `Placement.hpp`; each of the three surfaces holds one `placement` field and each `continuity()` returns `Continuity::CN`.

## Consequences

- Good: the frame has a name and becomes a reusable value type (the always-right-handed version of `gp_Ax3`); the type tree matches OCCT; `dynamic_cast<AxisSurface*>` is no longer needed and a caller reads `placement` from the concrete surface; `ElementarySurface` stays an empty tag (`C9`).
- Bad / cost: three copies of `continuity()` (a deliberate trade-off: adding a base class to share one `return` would be over-design); one more public type; the access path of `origin` / `axis` / `x_direction` becomes `surface.placement.*`; a caller holding `ElementarySurface<T>&` cannot ask for the placement (if such a consumer appears, `dynamic_cast` is the first thing to consider, and only then widening the interface).
- Trigger (when to reopen): `Placement` has to express a mirror (then add a handedness bit, or go back to the immutable "build a mirror" rule); or a fourth rotational surface appears and the three copies of `continuity()` become glaring (then prefer a mixin over reintroducing a state layer).

## Evidence

- `src/base/geometry/sdk/vine/geometry/Placement.hpp`; each of the three surfaces holds `Placement<T> placement;`. <!-- drift-ok -->
- `src/base/geometry/src/Instantiations.cpp`: two instantiation lines, `Placement<float>` / `Placement<double>`; the two `AxisSurface` lines are deleted. <!-- drift-ok -->
- `tests/test_geometry/GeometryKindsTest.cpp`: `aPlacementCompletesAndNormalisesItsFrame` (the default frame, the projection of a non-unit preferred direction, the degenerate-axis fallback), `static_assert(sizeof(Placement<double>) == 3 * sizeof(Point3d))` and `is_trivially_copyable_v<Placement<double>>`. <!-- drift-ok -->
