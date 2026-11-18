# ADR-2026-11-04: `isValid()` joins the `Curve<T>` / `Surface<T>` interfaces

> **Status**: Accepted
> **Date**: 2026-11-04
> **Related**: `../design/geometry-kinds.md` 2.7 / 2.10 / C12 / section 9; `src/base/geometry/sdk/vine/geometry/Curve.hpp`, `Surface.hpp`, `Plane.hpp`; `tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## Background

Every concrete type in the geometry families **already** had an `isValid()`: a line looks at its direction, a conic at its frame, a spline at its weights and knots, both kinds of trim at the basis handle and the interval, the three rotational surfaces at the axis and the radius, the two spline surfaces at the net and the multiplicities. It is also the single criterion the design page states (`geometry-kinds.md` C12: "`isValid()` is the only decision entry").

The problem was that the question **could not be asked**: `isValid()` was not a member of `Curve<T>` / `Surface<T>`, so a caller holding a `const Surface<double>&` had to know the concrete type first, or `dynamic_cast`, or assume the input is usable and evaluate directly. `Shape` / `BrepShape` / `Primitive` in the same repository had long taken the other road - `Shape::isValid()` is virtual. The geometry families were the exception to that family convention (`G6`).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Add `virtual bool isValid() const = 0` to `Curve<T>` / `Surface<T>` | One answer per question, askable through `const Curve<T>&`; the same question as `Shape` / `Primitive`; family algorithms and contract sweeps can guard in one place | The interface widens (curves 10 -> 11, surfaces 13 -> 14 items); one more vtable slot per implementation; an outside implementer has to provide it (there is none in the repository) |
| B. A free function `bool isValid(const Curve<T>&)` with `dynamic_cast` | The interface is untouched | Only a free function that knows every concrete type can be written, and a new curve means editing that function - the closed set is written into an algorithm (`G9`) |
| C. A concept constraint (`requires { t.isValid(); }`) | Compile-time checking, zero runtime cost | Only open to templates, so the runtime polymorphic path of `const Curve<T>&` still cannot ask; and runtime polymorphism is the main body of the geometry families (`Primitive::Parameters` uses a variant and evaluation uses a vtable) |
| D. Keep the status quo: each type has its own `isValid()`, the interface says nothing | The interface is untouched | "The single criterion" is unavailable at half the call sites and in practice degrades to "assume the input is usable", contradicting the promise of C12 (`G4`) |

## Decision

Chose A.

- `Curve<T>` / `Surface<T>` each gain one pure virtual `[[nodiscard]] virtual bool isValid() const = 0;`, next to `isCN()`, with a comment stating two things: it is the answer about **state** (independent of the parameters), and a false answer makes queries return degenerate values in order to "keep the error an error instead of undefined behaviour".
- The fifteen implementations already had the method and only gained `override`; `Plane` was the only one without it, because a plane had been considered to have no invalid state. It has one now: a zero normal has no perpendicular direction and no in-plane axis, so the parameterisation would quietly fall back to the world axes - `Plane::isValid()` is therefore `normal.length() > EPS`, consistent with the conic curves' rule about a degenerate frame (P6).
- `OffsetCurve` already has the implementation, and `secondDerivative` is still pure, so the type stays abstract (the earlier decision against a third derivative is unaffected; git holds it).

## Consequences

- Good: `expectCurveInvariants` / `expectSurfaceInvariants` now assert `isValid()` first, and family algorithms can guard in one place; "why is `isValid()` on the interface" no longer needs an explanation, because it is the same question `Shape` asks (`G6`, `G12`).
- Good: `Plane`'s zero normal goes from "quietly fall back to the world axes" to "`isValid()` is false", unified with the degenerate rule of the other types in the family.
- Bad / cost: the interface widens and the surface vtable goes from 13 to 14 slots; an outside implementer has to add this one method. The only implementers in the repository are this module, and the 84 cases of `test_geometry` and the whole-repository build pass after the change.
- Trigger (when to reopen): a geometry-family implementer that **does not need** a validity criterion appears (for example a purely functional adapter that is legal by construction) - then consider demoting it to a non-virtual default `bool isValid() const { return true; }`.

## Evidence

- `src/base/geometry/sdk/vine/geometry/Curve.hpp`, `Surface.hpp`: the new pure virtual function and its `@details`. <!-- drift-ok -->
- `src/base/geometry/sdk/vine/geometry/Plane.hpp`: the only newly written implementation, `normal.length() > EPS`. <!-- drift-ok -->
- `tests/test_geometry/GeometryKindsTest.cpp`: two `static_assert` lines (askable through the interface, returns `bool`), <!-- drift-ok -->
  `validityIsAskableThroughTheInterface` (five questions asked through the interface), and `EXPECT_TRUE(curve.isValid())` in the family sweep.
- `src/base/models/sdk/vine/models/Shape.hpp`: `virtual bool isValid() const;` - the precedent for the same question.
