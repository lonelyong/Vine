# ADR-2026-11-05: the planar curve contract is independent of the spatial curve contract

- Status: Accepted
- Date: 2026-11-05
- Related: `../design/geometry-planar.md`, `../design/geometry-kinds.md`

## Background

`geom::spatial::Curve<T>` is already an eleven-item evaluation interface (`geometry-kinds.md` 2.7): `evaluate` / `derivative` / `secondDerivative`, the two ends of the parameter domain, `isClosed` / `isPeriodic` / `period`, `continuity`, `isValid` and `reversedParameter`. When `geom/planar` landed, a structural question had to be answered: are these eleven facts **declared again** for the plane, or do the 2D and 3D types share one interface?

There are two common ways to share: make the point type a template parameter (`Curve<PointT>`), or let `geom::planar::Curve<T>` inherit `geom::spatial::Curve<T>` and treat the plane as the subspace `z = 0`. The first is "one contract, two dimensions"; the second is "one kind of object, two dimensions".

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Declare `geom::planar::Curve<T>` independently (the same eleven items with the same meanings, over `Point2` / `Vector2`) | Each concept has its own definition and owner; the 2D type carries no z and no normal; 3D code cannot accidentally accept a planar curve | The contract text exists twice, and a fact added on one side can drift from the other |
| B. One generic `Curve<PointT>` contract shared by both modules | The contract exists once | Every 3D consumer becomes a template; the 2D type is forced to satisfy constraints only 3D needs (a normal, a frame, later); the interface gains members that are "unavailable in 2D" |
| C. `geom::planar::Curve<T>` inherits `geom::spatial::Curve<T>` with the plane embedded at `z = 0` | Implementation and algorithm signatures are reused | A planar curve is given an identity normal and a constant-zero dimension; 3D algorithms consume a planar curve as a spatial one; every evaluation carries one more coordinate |
| D. Wrap planar curves in a variant or adapter on the 3D side | No new contract | Every evaluation costs one more call and branch; the planar curve still stores its own point, so the wrapper removes no code |

## Decision

Chose A: `geom::planar::Curve<T>` is a **second contract**, item-for-item synonymous with the 3D one and inheriting nothing from it.

The criteria are `G1` (concepts clear, abstractions natural) and `G5` (cohesive and loosely coupled): the boundary between "planar curve" and "spatial curve" is not two specialisations of one type family but two concepts, each with its own consumers and its own geometry facts. Sharing the contract can only be done by "adding a field the 2D type does not have" (C) or by "making 3D code pay a template cost for the dimension" (B), and both move the complexity to the side that does not need it (`G7`).

What is genuinely shared is collected in the vocabulary layer rather than the type layer: `math::Continuity`, `math::FP` and the future root-finding machinery have one definition in `math`, and `geom_global.hpp` only re-exports names. Dimension-independent facts are therefore consistent by construction, and dimension-dependent facts stay on their own types.

## Consequences

- Good: `geom/spatial` does not know `geom/planar` exists, so the two modules evolve on their own; a planar type's size matches `Point2` plus a scalar, with no polymorphism cost beyond the vptr; 3D family algorithms (closest point, intersection, tessellation) reject planar curves at compile time.
- Bad / cost: the contract text exists twice; a fact added on one side is not automatically gained on the other (an explicit concession to `G6`). The eleven items are synonymous on both sides today with no known divergence, and the comment in `Curve.hpp` states this as "the same contract with one dimension less", so that a change is read on both sides together.
- Trigger (when to reopen): an algorithm appears that must be written for both the 2D and the 3D curve and depends only on `evaluate` / `derivative` / the parameter domain / `isValid`. The right move then is to extract that algorithm's dependencies into a free function template independent of both interfaces (the shape of `closestPointOnCurve()`), not to merge the interfaces.

## Evidence

- `src/base/geom/sdk/vine/geom/planar/Curve.hpp` and `src/base/geom/sdk/vine/geom/spatial/Curve.hpp`: the eleven signatures correspond item by item, replacing only `Point3` / `Vector3` with `Point2` / `Vector2`.
- `src/base/geom/sdk/vine/geom/geom_global.hpp`: `Continuity` is re-exported from `math` and is not a second enum; the two namespaces share one module global.
- `src/base/geom/sdk/vine/geom/planar/CircleCurve.hpp`: a planar circle is `center` plus `radius`, without 3D's `ConicCurve` frame.
- `tests/test_geom_planar/CurveBasicsTest.cpp`: `AnswersThroughTheBaseInterface` verifies through `Curve<double>&` and `unique_ptr<const Curve<double>>` that the polymorphic path works without 3D.
