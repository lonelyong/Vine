# Bug: the knot-span search loops forever over the last two spans

- **Module**: `src/base/geom` (`geom/detail/BSplineBasis.hpp`, then in `src/base/math/spline/`) <!-- drift-ok -->
- **Status**: Fixed
- **Files**: `BSplineBasis.hpp`, `tests/test_geom_spatial/BSplineBasisTest.cpp` (then in `tests/test_math/`) <!-- drift-ok -->

## Symptom

For a B-spline clamped at both ends with a single interior knot (degree 1, distinct knots `0 / 1 / 2`, multiplicities
`2 / 1 / 2`), evaluating in the second span (for example `u = 1.5`) loops forever and the process hangs. It first showed
up while writing the tests for `geom::planar::BSplineCurve`.

The geometry side had never exposed the defect: the existing 3D spline cases were either single-span (two values in
`knots`, so the search returned early) or failed `isValid()` after construction, so evaluation returned a degenerate value
straight away. In other words, **a spline with an interior knot had never really been evaluated**.

## Root cause

The binary search in `knotSpan()` initialised `high` to `last_span`, while the invariant the search depends on is
`U[low] <= u < U[high + 1]`. When the bracket is one wide (`high == low + 1`), `mid == low`, and the "`u` is in the right
half" branch performs `low = mid`, so the midpoint stops advancing and the loop cannot exit.

## Fix

`high` is `last_span + 1` again, which restores the invariant of the classic NURBS `FindSpan`, `U[low] <= u < U[high]`;
the reason is in a comment, so the next "optimisation" does not drop the `+ 1` again.

## Verification

- `BSplineBasisTest.TheSpanSearchTerminatesOnAOneApartBracket` (degree 1, `u = 1.75`) and two cross-search cases: they
  hung before the fix and return the correct span number after it.
- The property tests in the same file pin the neighbouring logic as well: the basis functions sum to 1 and stay
  non-negative (partition of unity), the span contains the parameter, the analytic derivative agrees with a central
  difference, and the continuity follows from the interior multiplicities. Before the fix these cases hung on the first
  sweep.
- The 11 cases of `geom::planar::BSplineCurve` cover the same path as normal behaviour (`FollowsItsKnots`,
  `ContinuityComesFromTheMultiplicities`).
- Full ctest 29/29.
