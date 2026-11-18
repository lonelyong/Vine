# ADR-2026-11-10: curve discretisation is an object, not three free functions

> **Status**: Accepted
> **Date**: 2026-11-10
> **Scope**: `src/base/geom/sdk/vine/geom/spatial/CurveSampling.hpp`, `src/base/geom/sdk/vine/geom/planar/CurveSampling.hpp`,
> `tests/test_geom_spatial/CurveSamplingTest.cpp`, `tests/test_geom_planar/CurveSamplingTest.cpp`

## Background

Curve discretisation lands for the first time. The repository's default shape for an **algorithm** is a **free function plus a `requires` constraint** (`closestPointOnCurve()`, `arcLength()`,
`parameterAtLength()`, `tessellateSolid()`), and its default shape for a **closed set** is **overloads plus compile-time dispatch** - `ADR-2026-11-09-solid-closed-set-without-variant.md`
even deleted a variant type on purpose and rejected "a virtual `kind()` / virtual `tessellate()`".

In design review the proposal changed from free functions to an object, for three concrete reasons:

1. Derived attributes need somewhere to live (the actual deviation, the per-segment arc length, the tangents). Returning a value structure means either precomputing every attribute
   or changing the signature each time;
2. A wireframe has dozens of segments, one curve per segment. The window and the parameters should be set once, and the loop should only swap the curve;
3. Progress and cancellation can only have one place, or every entry point needs an extra callback parameter while only one of them really uses it.

Those three run against "the repository default", which is why this is recorded.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Three free functions plus a `CurvePolyline<T>` return value (the repository default) | No state, no vptr, and a concrete curve's `evaluate` can inline; the smallest implementation | The window and parameters are passed again in the loop; derived attributes have nowhere to go (change the signature, or compute them again); progress / cancellation can only pollute every entry point's signature |
| B. A `CurveSampler<T>` base class plus three subclasses, setters configure, `compute()` takes no arguments (**adopted**) | Configure once and run many curves; attributes hang off the object, so a new attribute does not change existing signatures; the progress hook has one place | Two inheritance trees (`float` / `double`); one virtual `evaluate()` per sample point; `curve()` stores a reference only (a short-lived contract); the result-reset rule must be written down explicitly |
| C. A base class that computes on construction (OCCT's `Initialize` runs in the constructor) | The call site is the shortest | No virtual dispatch during construction (a pure virtual crashes, or the base version runs); the curve and the window are configuration and can only be pushed into constructor arguments, leaving the rest with nowhere to go |
| D. A base class plus one virtual `sample(curve)` taking the curve each time | The object has no state | A rule's parameters can only go into the constructor, the same disease as C; the "set first, then compute" reuse disappears |

## Decision

Adopt **B**, with three contracts that contain its cost:

- **C7**: `curve()` stores a reference only, so the sampler must not outlive the curve and does not extend its lifetime;
- **C8**: any setter and any failed `compute()` clears the result - `polyline()` either belongs to the last bound curve or is empty, never the points of the previous curve;
- setters return `void`, not `*this`: a chained return type would change with the base / derived class, and "which setter can follow which" would become an implicit convention
  (hit twice while writing tests). Configuration is written as statements, and any order works.

The rules themselves (equal parameters / equal arc length / chord-height deviation) are still a closed set: adding a rule is a new subclass that changes neither the base class nor the kernel. There is one algorithm, living in
`geom/detail/CurveSubdivision.hpp` (this ADR does not account for that; the measurement of "why it stays in the header" is in 2.7 of `geometry-module.md`).

## Consequences

- **Good**: `curve(c); compute();` is the main usage and a wireframe loop sets the configuration once; the result and the derived attributes live on the object; `C8` turns errors such as "read the previous curve's points"
  from "looks like it succeeded" into "reads empty"; switching rules at run time only needs a `std::unique_ptr<CurveSampler<T>>`.
- **Bad / cost**: `CurveSampler<float>` and `<double>` are two unrelated inheritance trees; `computeImpl()` can only take a `const Curve<T>&`, giving up "a concrete type's evaluation can inline"
  (`Curve.hpp`'s `@warning` keeps that for hot paths on purpose), and every sample point costs one more virtual call. Sampling is not a per-frame path, so this is accepted (`G10`).
- **Trigger conditions (when to reopen)**:
  - needing to take points **without materialising an array** (display lists, walking collision sweep edges one at a time) - that is an iterator / callback, a separate design, not turning the existing entry points into a class;
  - needing to **replace the whole rule set from outside** - that is an upper-layer (plugin) registry, not a virtual interface added in `geom`;
  - needing to keep one configuration across more than one object's lifetime - that is a value structure (the `TessellationQuality` precedent), not a setter class.

## Evidence

- Tests: `tests/test_geom_spatial/CurveSamplingTest.cpp` (10 cases, including setter reset, reuse across curves, budget refusal),
  `tests/test_geom_planar/CurveSamplingTest.cpp` (4 cases).
- Design: `../design/geometry-curve-discretization.md` 2.1 / 2.2 / 2.10 / 2.11 and §7 `D1`, `D6`, `D9`, `D10`.
- Cross-reference: `ADR-2026-11-09-solid-closed-set-without-variant.md` rejected "hanging virtual functions on the `Solid<T>` family interface", not
  "no algorithm object trees"; this ADR does not overturn it, it adopts the other half.
