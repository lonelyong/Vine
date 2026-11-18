# ADR-2026-11-04: surfaces are an evaluation interface too

> **Status**: Accepted
> **Date**: 2026-11-04
> **Scope**: `src/base/geometry/sdk/vine/geometry/Surface.hpp`, `ElementarySurface.hpp`, `BoundedSurface.hpp`, `Plane.hpp`, `SphericalSurface.hpp`, the six placeholder surface headers, `tests/test_geometry` <!-- drift-ok -->

## Background

Item 6 of `ADR-2026-11-04-curve-evaluation-interface.md` left the surface family an empty tag, on the ground that "no consumer requires an unknown surface to be evaluable". Half a day later the same requirement appeared on the surface side: face traversal, sampling and the prerequisites of intersection all need "evaluation plus the parameter domain and periodicity in two directions", and the curve interface had already proved the shape of that contract.

## Decision

1. `Surface` becomes the **template interface** `Surface<T>`: virtual destructor plus thirteen pure virtuals - `evaluate(u, v)`, the two first derivatives `derivativeU/V`, the four parameter bounds `first/lastUParameter` / `first/lastVParameter`, closure and periodicity per direction (`isUClosed/isVClosed/isUPeriodic/isVPeriodic/uPeriod/vPeriod`), `continuity()`; plus the non-virtual `isCN(n)` and `kDimension`.
2. `ElementarySurface<T>` / `BoundedSurface<T>` are demoted to classification tags: no state and no virtual functions.
3. `Plane<T>` implements the interface: the two in-plane axes are derived from `normal` every time (the projection of the world axis least parallel to the normal), with no third vector stored - a plane is copied during ray intersection.
4. `SphericalSurface<T>` implements the interface: `u` is the azimuth (`[0, 2pi]`, closed and periodic) and `v` the polar angle (`[0, pi]`, with poles at both ends, neither closed nor periodic).
5. The six placeholder surfaces are templated as `XxxSurface<T>` and inherit the tags; they remain unconstructible.
6. The interface does **not** carry `normalAt()` / `bounds()` / second derivatives / the reversal and transform families.

## Rationale

- `G4`, `G9`: the thirteen items are the minimal complete set for a parameterised surface and correspond one to one with `Curve<T>` (one parameter becomes two); a new surface is thirteen functions.
- `G6`: the naming and grouping of `Curve<T>` are copied verbatim (`first/last` -> `first/lastU`), so one lesson covers both.
- `G1`: the interface carries only facts that must always have an answer and only one answer; the normal's orientation, the unboundedness of a bounding box and the derivative order all need a convention, so they stay with the concrete type.
- `G10`: `Plane` / `SphericalSurface` pay one vptr for the interface and not one member more.

## Rejected options

| Option | Why not |
|---|---|
| Keep surfaces as empty tags | A tag cannot answer "evaluate an unknown surface plus the parameter domain and periodicity in two directions", so face traversal and sampling would each write their own dispatch (`G7`) |
| Pack the four parameter bounds into one call (OCCT's `Bounds(U1,U2,V1,V2)`) | Packing is the C++98 out-parameter style; four virtual functions line up exactly with `Curve<T>`'s `first/last` (`G6`) |
| Put `normalAt(u, v)` into the interface | It is one normalised `dU x dV` line and it has to choose an orientation convention; putting it on the interface turns a convention into a contract (`G4`) |
| Have `Plane` store in-plane frame vectors (like `ConicCurve`) | A plane is copied during ray intersection and its size is asserted; deriving is cheaper than storing (`G10`) |
| Keep a non-template `Surface` tag and start a separate surface interface | Two `Surface`s are two homes, and `is_base_of` would split into two trees (`G1`) |

## Consequences

- **Breaking**: `Surface` / `ElementarySurface` / `BoundedSurface` are templates now and need `<T>`; the only consumer in the repository is `tests/test_geometry`. <!-- drift-ok -->
- `Plane` / `SphericalSurface` each gain a vptr and are no longer trivially copyable; the `sizeof` assertions gain `sizeof(void*)`.
- Surfaces and curves now have the same shape, and the current design is recorded in `.ai/design/geometry-kinds.md` 2.1 / 2.2 / 2.10.
