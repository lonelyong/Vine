# ADR-2026-11-05: curve derivatives are exposed as a capability, not as one order written into the contract

- Status: Accepted
- Date: 2026-11-05
- Related: `../design/geometry-kinds.md` 2.7, `../design/geometry-planar.md` 2.3 / 2.13, `ADR-2026-11-04-curve-third-derivative.md`, `ADR-2026-11-05-planar-offset-curve.md`

## Background

The second derivative of the offset curve `O = C + d*N` needs the basis curve's third derivative. The contract declared only up to the second order, so a caller holding a `Curve<T>&` at runtime had **no** way to obtain a third derivative; the two existing ADRs chose "the offset is a template on the concrete type" and "the spatial offset stays abstract" respectively.

P1/P2 want a runtime basis curve. Neither boundary route holds:

- a contract that stops at the second order: a runtime basis cannot supply a third derivative, so the offset degrades to a template or to type erasure;
- adding a **pure virtual** `thirdDerivative()` as the twelfth item: `OffsetCurve` is itself a `Curve<T>`, and its third derivative needs the basis fourth, so the obligation is always one order short; only exceptions or truncation remain (OCCT's `DN` plus `NotImplemented`).

The real question was never "do we need this order" but **the derivative order is a parameter of the algorithm and a capability of the object**, not part of the type's identity.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the status quo (up to the second order) | No change | A runtime basis cannot be offset exactly; P2 can only use a template or erasure |
| B. Add a pure virtual `thirdDerivative()` | The offset holds a handle directly; aligns with OCCT's `D3` | The ladder does not close (the offset's D3 needs the basis D4); every type pays; out of range means an exception or truncation |
| C. A capability protocol: `maxDerivativeOrder()` plus `derivativeAt(u, order)` (first and second by default, overridden by types that have a closed form for the third) | The order is a parameter; a runtime basis can be asked for any reachable order; out of range answers `std::nullopt`; the two modules have the same shape | Two more non-virtual members; the default implementation costs one more indirection within the second order |
| D. Arbitrary order `DN(N)` (the OCCT shape) | The most general | An inflated interface, an implementation limit and exception semantics, all conflicting with this repository's "finite, correct, no exceptions" convention |

## Decision

Chose **C**.

Criteria: `G1` (the order is an algorithm parameter, not the type's identity), `G4` (capability is queried explicitly, and beyond it the answer is empty rather than a fabricated zero), `G6` (2D and 3D share names and meanings), `G9` (a new type only reports its capability and does not change the contract), `G10` (no allocation; the default implementation only adds one virtual call).

It also gives P2's target shape its premise: an offset view queries the basis `maxDerivativeOrder() >= 3` when it is constructed and raises a construction-time error otherwise - no exception, no truncation, and no "one order short" hole.

## Consequences

- Good: `Curve<T>` still has eleven core pure virtuals plus **a pair of non-virtual capability members**; `derivativeAt` covers the first and second order, and types with a closed third-order form override it to 3; the 2D and 3D additions are item-for-item synonymous; a runtime basis can, for the first time, answer exactly "which order can I reach".
- Bad / cost: the default `derivativeAt` goes through the two virtual functions `derivative` / `secondDerivative`, so within the second order it costs one more indirection; the target shape makes `derivativeAt` the primitive with the other two as non-virtual convenience wrappers, and that step is recorded as a follow-up migration rather than done here.
- Trigger (when to reopen): turning trims and offsets into immutable shared-handle views, and construction-time `expected` validation, are still tracked by the target architecture in `../research/` and the backlog; this ADR only settles "how to ask for an arbitrary order".

## Evidence

- `src/base/geom/sdk/vine/geom/planar/Curve.hpp`, `src/base/geom/sdk/vine/geom/spatial/Curve.hpp`: the capability protocol's declarations and default implementations.
- `tests/test_geom_planar/CurveContractTest.cpp`: the first- and second-order agreement of nine types, and the agreement between "is `derivativeAt(3)` reachable" and `maxDerivativeOrder()`.
- `tests/test_geom_spatial/GeometryKindsTest.cpp`: the family sweep's check of the capability.
- For comparison: OCCT's `Geom2d_Curve::EvalD0..EvalDN` (the order is on the interface, capped at 3); this decision keeps "ask on demand, answer empty beyond" and introduces neither exceptions nor truncation.
