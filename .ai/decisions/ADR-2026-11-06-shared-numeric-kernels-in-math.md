# ADR-2026-11-06: dimension-independent numeric kernels live in `math`

- Status: Superseded (the kernels have moved to `geom/detail`, see `ADR-2026-11-06-math-keeps-only-vocabulary.md`; the kernels' signature contracts and criteria still stand as described here)
- Date: 2026-11-06
- Related: `../design/geometry-curve-queries.md` 2.6, `../design/geometry-planar.md` 2.14, `../design/geometry-intersection.md` 2.7

## Background

`geom/planar` and `geom/spatial` each had their own arc length and inverse solve: an adaptive Simpson recursion and a Newton iteration protected by bisection, line for line identical, differing only in "how speed is computed" and "which point of a dimension is used for length". This duplication was no accident - it was ported line by line from the 3D version, so one piece of numeric logic had two implementations free to drift apart (a `G1` violation), and fixing a numeric defect meant remembering two places.

`rootInBracket()` had already given the answer: 2.7 of `geometry-intersection.md` says explicitly "why the root finder lives in `math`" - it knows only "a continuous univariate function plus a bracket", it is a pure numeric algorithm, and the geometry side only supplies values and assembles the result. The same criterion applies to integration and the monotone inverse solve.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep two implementations | No change, neither module references the other | One algorithm in two homes, and a fix in one must be remembered in the other; the tolerance semantics on the two sides can silently diverge (`G1`, `G5`) |
| B. The 3D side includes the 2D side's headers | A small change | `geom/spatial` would depend back on `geom/planar`, which is the wrong layer (`G2`) |
| C. Put the kernels in `math`: they know only scalar functions and intervals, geometry keeps the sampling and the result types | One implementation; the dependency still points geometry -> math; the two sides' semantics cannot drift | `math` gains two template functions; the geometry side still writes a dozen sampling lines of glue per algorithm |
| D. Template the whole 2D/3D curve algorithm into one (a concept-constrained curve algorithm layer) | Even the glue exists once | Would move the concept "curve" into `math`, or need a new shared layer; a larger interface surface for a gain of those dozen lines of glue |

## Decision

Chose **C**, landing two kernels first:

- `vn::math::numeric::adaptiveSimpson(function, lower, upper, tolerance, max_depth)` in `math/numeric/Quadrature.hpp`: adaptive Simpson with a Richardson error estimate; non-finite samples and a reversed interval answer `std::nullopt`.
- `vn::math::numeric::parameterReaching(value, slope, lower, upper, target, tolerance, max_iterations)` in `math/numeric/RootFinding.hpp`: a safeguarded Newton for a given monotone quantity and a slope hint (falling back to bisection outside the bounds or when the slope is too small), with the stopping criterion on the **value**; the return value is that **parameter**, which is why the name starts with `parameter`. `rootInBracket(function, lower, upper, tolerance, max_iterations)` in the same header is the sign-change version - both names state their premise: the bracket comes from the caller and the function only narrows it.
- The two kernels share the sub-namespace of the existing `vn::math::numeric::rootInBracket()`: the `vn::math` root layer holds numeric vocabulary only (constants, types, concepts, elementary functions) and algorithm families go into the sub-layer - the same rule as `vn::math::spline` (`G6`, and no extra level for two names).

`geom::planar::arcLength` / `parameterAtLength` and their `geom/spatial` namesakes now supply only `speed(u)` and `reached(u)`, leaving the rest to the kernel. The curve's own semantics (window rules, the `isValid()` guard, the length dimension, the `std::nullopt` conditions) stay on the geometry side, not one line fewer.

**Consumers (both sides use them, which is the premise for sharing)**:

| Kernel | `geom/planar` | `geom/spatial` |
|---|---|---|
| `adaptiveSimpson` | `arcLength()` (`CurveLength.hpp`) | `arcLength()` (`CurveLength.hpp`) |
| `parameterReaching` | `parameterAtLength()` | `parameterAtLength()` |
| `rootInBracket` | `closestPointOnCurve()` (`CurveProjection.hpp`); `intersectCurveWithCurve()` uses it indirectly | `closestPointOnCurve()`; `intersectCurveWithSurface()` (`CurveSurfaceIntersection.hpp`) |

All three kernels have consumers in both modules; none was put into `math` for a possible future.

Criteria: `G1` (one implementation), `G2` (the dependency still points at `math`), `G9` (a new dimension or curve type reuses the same kernel), `G10` (no allocation, the same number of samples as before), `G11` (the kernels are testable alone: `tests/test_math/QuadratureTest.cpp` and `RootFindingTest.cpp`). <!-- drift-ok -->

**Why not a separate `numeric` module**: the axis here is not "algorithm versus mathematics" but "operations on values" against "numeric processes taking a function as input" - the latter is still mathematics, and OCCT puts `math_FunctionRoot` / `math_Integration` in its Math package; this repository had already put "eigendecomposition of a 3x3 symmetric matrix" into `math` by the same criterion (`geometry-primitives.md` 2.7: `symmetricEigen` is a numeric operation and not a spatial entity, and putting it into `geometry` would make `math` depend back on it). Isolation does not come from a new module but from three things: a separate header (the same practice as `EigenDecomposition`), the `numeric` sub-namespace, and a separate test file. A new module would instead leave `EigenDecomposition` and `spline` in "another home", making two homes rather than one.

**The criterion (look at the signature, not at "does it look like an algorithm")**: does the caller **supply a precision target** (`tolerance` / `max_iterations`), and can the function answer "did not reach it" (`std::nullopt`)? Yes -> `numeric` (a process); no -> the `math` root layer (an evaluated object). Why `math` also carries CAD representation conventions (the spline knot multiplicity, clamped form and continuity ladder) is in `ADR-2026-11-06-math-is-the-base-layer.md`.

| Facility | Caller tolerance in the signature? | Home |
|---|---|---|
| `spline::deCasteljau` / `differences` / `basisValue` / `knotSpan` / `knotContinuity` | No - exact evaluation, computed once | The `math` root layer (the `spline` sub-namespace). The 2D and 3D `BezierCurve` / `BSplineCurve` / `BezierSurface` / `BSplineSurface` all use it, the same sharing reason that predates `numeric` |
| `symmetricEigen` | No - internally fixed at `EPS<T>()*4`, returning a spectrum plus a converged flag | The `math` root layer |
| `rootInBracket` / `parameterReaching` / `adaptiveSimpson` | Yes - the tolerance and iteration cap come from the caller and a failure answers `std::nullopt` | `numeric` |

So `spline` in `math` is not a broken precedent but another outcome of the same criterion: it is "a family of functions that compute", not "a process approximating a target".

D was not chosen because the gain does not outweigh the risk at this point: the glue is a dozen lines, while moving the concept of a curve into `math` would widen that layer's semantics; it is re-evaluated when a third pair of algorithms (projection, distance) also proves to differ only in sampling. **That is a trigger for later evaluation, not a permanent rejection.**

## Consequences

- Good: the arc length and the inverse solve exist once, and both modules' tests watch the same implementation; the kernels have their own unit tests (known integrals, a kink, the depth cap, single precision, refusal conditions); the boundary between `math` and geometry is clearer - `math` knows only scalar functions.
- Good (where the tolerance conversion lives): converting "geometry tolerance" to "parameter tolerance" (`value_tolerance / |dP/du|`) needs the curve's speed, so it stays on the geometry side, with one `detail::narrowCrossing(curve, value, previous, current, value_tolerance, fallback)` in each module; the duplicated prologue at three call sites (2D projection, 3D projection, 3D curve against surface) became one call, while the kernel still does not know curves.
- Bad / cost: the geometry side gains two includes; the kernels' parameters (`std::optional` return, the meaning of the depth cap) become a convention shared by two modules, and changing it means reading both design points. The meaning of `max_depth = 0` (still one bisection) is in the kernel documentation, so the two sides do not interpret it separately.
- Not done: the sampling and Gauss-Newton of `CurveProjection` / `CurveCurveDistance` are still two copies, because their glue proportion is higher (candidate filtering, coincidence decisions, two-dimensional step); re-evaluate per the trigger above.
- Not done: a more general root finder (Brent's inverse quadratic interpolation, an `|f|` tolerance, tangential roots). `rootInBracket()` only does "a single root inside a sign-change bracket plus a parameter-width tolerance", and the header lists item by item what it is not; all three call sites need parameter precision only today, and an upgrade would be over-design in the `G8` sense. Trigger: a consumer that requires `|f|` to meet a bound, or a bisection convergence rate on a skewed bracket that becomes the bottleneck.
- Escalation trigger (when these processes should become a `vn::Numeric` module): a non-template implementation needing `src/` appears (an integrator with a work buffer, for example), or the processes in `numeric` outgrow the sub-namespace, or a third consumer module requires its own link edge. When escalating, move `EigenDecomposition` in as well and do not do half of it - a "numeric algorithm" can have only one home.

## Evidence

- `src/base/math/sdk/vine/math/numeric/Quadrature.hpp`, `numeric/RootFinding.hpp` (`rootInBracket` / `parameterReaching`). <!-- drift-ok -->
- `src/base/geom/sdk/vine/geom/planar/CurveLength.hpp`, `src/base/geom/sdk/vine/geom/spatial/CurveLength.hpp`: the bodies of both public functions are down to about a dozen lines each.
- `tests/test_math/QuadratureTest.cpp`, `tests/test_math/RootFindingTest.cpp`: the kernels' unit tests. <!-- drift-ok -->
- `tests/test_geom_planar/CurveLengthTest.cpp`, `tests/test_geom_spatial/CurveLengthTest.cpp`: the end-to-end acceptance on both sides (closed-form length, a cusp, a rejected window, an interior inverse, both precisions), green after the extraction without a single line changed.
