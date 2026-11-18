# ADR-2026-11-03: runtime B-rep operations become their own module, `brep`

> **Status**: Accepted
> **Date**: 2026-11-03
> **Scope**: `src/base/brep` (new), `src/loaders/brepio`, `cmake/VineOpenCASCADE.cmake` (new)

## Background

B-rep needs three different things:

1. **data** - `geometry::BrepShape` holds a handle of the backend shape; the SDK only forward-declares `TopoDS_Shape` and neither includes OpenCASCADE headers nor links them.
2. **files** - `brepio` reads and writes STEP/IGES, with OpenCASCADE optionally linked PRIVATE.
3. **runtime operations** - tessellation (for rendering), building a solid from a parametric primitive, and booleans later. This had no home.

Putting the third into either of the first two does not hold: putting it into `geometry` would make the base module and the public headers depend on the backend; putting it into `brepio` would make a "file I/O" module carry runtime duties that have nothing to do with files, and would drag in the users of `Runtime`/`Crypto`/`IOBase`.

## Decision

1. Add `src/base/brep` (target `vn::Brep`), depending only on `vn::Geometry`.
2. Tessellation is offered as the free function `vn::brep::tessellate(BrepShape, TessellationOptions)`, returning an `intrusive_ptr<TriangleMesh>`; a failure is an empty handle, and the capability is answered by `tessellationSupported()`.
3. OpenCASCADE detection is gathered into `cmake/VineOpenCASCADE.cmake`, and `brep` and `brepio` each link it PRIVATE; `brepio` now uses that helper.
4. Without a backend the interface still exists: `tessellationSupported()` answers false and `tessellate()` returns an empty mesh.

## Rationale

- One responsibility, one boundary: data, files and operations each get their own place (`G1`, `G5`).
- One dependency direction: `brep -> geometry` and `brepio -> geometry`; neither depends on the other (`G2`).
- The backend dependency is PRIVATE and consumers do not inherit it, consistent with the existing position that mesh-only consumers keep not paying for it (`G10`).
- A failure does not borrow `Result` / `IoError`: that vocabulary describes storage failures and has nothing to do with operations, and borrowing it would make an operations-only module link `IOBase` for an error type; capability is a build property (`tessellationSupported()`) and a failure is an empty value - the same shape as `brepio::hasBackend()` and `graphics::geometryFromShape()` (`G4`, `G8`).

## Rejected options

| Option | Why not |
|---|---|
| A `BrepShape::triangulate()` member function | The implementation would have to live in `geometry`, so the base module and the public headers would depend on OpenCASCADE (`G2`). |
| Put it in `brepio` | The module is named for "file reading and writing", and a runtime conversion has nothing to do with files; it would also pull in dependencies that only import and export need. |
| A pluggable backend (a `BrepBackend` abstraction plus registration) | There is one backend today and the mechanism costs more than it returns (`G8`); upgrade when a second backend appears. |
| Produce bounding boxes with `Bnd_Box` / `Bnd_OBB` | The precise `Aabb::compute` and `Obb::computePca` already exist, so a second algorithm is not introduced (`G1`). |
| Tessellate the primitives directly (bypassing OpenCASCADE) | That is pure mathematics and belongs to `geometry`; there is no consumer this period, so it is not done. |
| Return `vn::io::Result<T>` (`IoError`) | That vocabulary (`NotFound`, `PermissionDenied`, `IsADirectory`, ...) describes storage failures; borrowing it would make the operations module link `IOBase` for an error type and would make callers match categories they do not need. |

## Consequences

- One module and one CMake helper are added; `brepio`'s detection code moves from its CMakeLists into the helper with unchanged behaviour.
- The OpenCASCADE branch of `tessellate` lands with the module (`VN_BREP_HAS_OCCT`); with no backend on this machine the empty implementation is used, and that branch waits to be verified in a build that has one.
- `vn::Brep` does not depend on `IOBase`: the module's only dependency is `vn::Geometry`.
- The render path is not wired yet: once tessellation exists, "load a B-rep -> tessellate -> display" is connected.
- The current design is recorded in `.ai/design/brep-operations.md`.
