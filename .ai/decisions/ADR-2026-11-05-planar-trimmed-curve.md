# ADR-2026-11-05: a planar trimmed view replaces the circle-only `ArcCurve`

- Status: Accepted
- Date: 2026-11-05
- Supersedes: the arc-as-value-type decision (git holds that record)
- Related: `../design/geometry-planar.md` 2.8 / 2.10 / D6, `ADR-2026-11-05-curve-derivative-capability.md`

## Background

`ArcCurve` was "a circle by value plus an angle interval": zero allocation and a tight box, but only a circle could be trimmed. The earlier ADR stated its trigger explicitly - as soon as a non-circular basis needs trimming (elliptical arcs, trimmed splines, truncated imported profiles), introduce the general `TrimmedCurve` and **delete** `ArcCurve` rather than keeping both. That trigger is now met by the design itself: once the capability protocol (`maxDerivativeOrder()` / `derivativeAt()`) landed, a type-erased trimmed view can forward any order faithfully, so the premise "the basis must be a concrete type" is gone.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep `ArcCurve` and add a general view beside it | A piece of a circle keeps zero allocation and a tight box | Two types answer one concept ("a piece of a curve"), and the four rules of closure / periodicity / reversal / out-of-domain are written twice and drift |
| B. A general `TrimmedCurve<T>` holding a `shared_ptr<const Curve<T>>`, with `ArcCurve` deleted | The same name and shape as 3D; any basis curve; one set of rules | One heap allocation per construction (R2's second class of exception); the piece's tight box is no longer answered by the trimmed view |
| C. A general `TrimmedCurve<C>` holding the basis by value | Zero allocation, and it can forward when the basis has `bounds()` | Still a template parameter, so a runtime handle is unavailable; the same concept has different shapes in 2D (by value) and 3D (a handle) |

## Decision

Chose **B**: `TrimmedCurve<T>` stores a `std::shared_ptr<const Curve<T>>` plus two parameter bounds, the `trimCurve(basis, first, last)` factory builds the handle from a concrete type, and `ArcCurve` is **deleted together with its test file**.

The capability that is lost goes back where it belongs, per `G13`: the tight box of a piece of a circle is the circle's support function over the interval, so it is `CircleCurve::bounds(first_parameter, last_parameter)` and not a member of the trimmed view - an erased view cannot ask for a support function, and keeping it there could only return a loose box.

Criteria: `G1` (one concept, one expression), `G6` (2D and 3D trimming share a name and a shape), `G9` (a new basis needs no new type), `G10` (one allocation replaces a whole set of duplicated rules; an angle interval on a circle is still answered exactly by `bounds(first, last)`).

## Consequences

- Good: elliptical arcs, trimmed splines and truncated profiles all use one type; the four rules of closure / periodicity / reversal / out-of-domain have one implementation and one set of tests; `OffsetCurve` can offset a piece directly.
- Bad / cost: `TrimmedCurve` becomes R2's second class of exception (it holds dynamic memory and has deep-copy semantics) and allocates once per construction; the piece's tight box is available only for circles; `ArcCurve`'s public surface is deleted (its only consumer in the module was the test, so there is no ABI burden).
- Migration: `ArcCurve(circle, a, b)` -> `trimCurve(circle, a, b)`; `arc.bounds()` -> `circle.bounds(a, b)`; `arc.circle` -> the basis handle or the original `CircleCurve` value.

## Evidence

- `TrimmedCurve.hpp` and `bounds(first, last)` in `CircleCurve.hpp`, both under `src/base/geom/sdk/vine/geom/planar/`.
- `tests/test_geom_planar/TrimmedCurveTest.cpp`: walking a circle, a whole turn being closed, out-of-domain not closed, reversal inside the interval, an invalid reversed interval, a degenerate basis, domain forwarding, dispatch through `Curve<float>&`, trimming a Bezier, capability forwarding (3 for a circle, 2 for an offset), an empty handle, and a trim of a trim.
- `tests/test_geom_planar/BoundsTest.cpp`: the tight box of a piece of a circle, and a reversed interval giving the same box.
