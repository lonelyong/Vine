# ADR-2026-11-06: `geom/planar` and `geom/spatial` merge into one `vn::Geom` module

- Status: Accepted
- Date: 2026-11-06
- Related: `../design/geometry-planar.md` 2.5, `ADR-2026-11-05-planar-curve-contract.md`, `ADR-2026-11-06-math-is-the-base-layer.md`, `ADR-2026-11-06-shared-numeric-kernels-in-math.md`

## Background

The two geometry modules had always been two CMake units: `vn::Geom3D` was SHARED (explicit instantiations plus three non-template implementations) and `vn::Geom2D` was INTERFACE (17 headers, no `src/`, zero exported symbols). The consumers were extremely asymmetric as well: 15 CMake targets used `vn::Geom3D` and only its own tests used `vn::Geom2D`.

The cost of these "sibling directories" showed up over the last few rounds: `detail::narrowCrossing` and three pairs of family algorithms (arc length, projection, two-curve distance) existed once in each module, because **no place can see both sides** - `math` does not know curves, and two sibling modules should not depend on each other. Either copy, or merge.

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep two modules and the duplication | No change; 2D stays INTERFACE with no ABI | Shared code can only be copied; every new geometry-level shared piece adds a drift risk |
| B. Merge into `src/base/geom` and one library `vn::Geom`, with the SDK trees still split into `geom/planar` / `geom/spatial` and both sharing one `geom/geom_global.hpp` | The shared code has a home; one target; one API macro; **include paths and namespaces do not move at all** | The compiler-enforced 2D/3D isolation is lost and moves to a script gate; 2D consumers link `libGeom`; documentation paths and the baseline need a one-off update |
| C. Create a `vn::GeomBase` module below both for the shared code | The target boundary is kept | A new module for two headers, and the shared code still cannot use the curve types themselves |

## Decision

Chose **B**:

- Directories `src/base/geom/{sdk/vine/geom/planar, sdk/vine/geom/spatial, sdk/vine/geom, src}`; target `vn::Geom`, compile definition `VN_GEOM_LIB`.
- **One global**: `sdk/vine/geom/geom_global.hpp` provides `VN_GEOM_API`, `VN_GEOM_PLANAR_NS_BEGIN/END` and `VN_GEOM_SPATIAL_NS_BEGIN/END`, and re-exports `Continuity` once into each namespace. The two modules each had their own global header before (`geom/planar_global.hpp` / `geom/spatial_global.hpp`) and both are deleted; 64 headers now include `<vine/geom/geom_global.hpp>` (following the shape of `appfw_global.hpp`: one global per module, several namespaces written into one file). <!-- drift-ok -->
- **Isolation moves to the gate**: a new `scripts/check_geom_layering.py` forbids a `geom/planar` header from including `geom/spatial` and the reverse. What used to be "the two targets cannot link to each other" is now "the script reports it", which is executable and can go into CI.

Criteria: `G1` (one copy of the shared code), `G5` (a geometry-level change lands in one module), `G6` (a uniform module shape: one library, one global), `G10` (the consumer's link surface narrows; 2D exports no symbols, so there is no ABI loss). The tension with `G8` is recorded honestly: the immediate benefit of merging was only de-duplicating `narrowCrossing` and three pairs of algorithms, a structural gain rather than a functional driver - this step is a direction the user explicitly asked for, so it was taken, but its cost and alternatives are on the record here.

## Consequences

- Good: 2D and 3D can share implementations; consumers link `vn::Geom` uniformly; one API macro; the `check_public_docs` baseline is unrelated to geometry (every geometry header has documentation), so the move created no new debt.
- Bad / cost: **the isolation is downgraded from the compiler to a convention plus a script**; the name `vn::Geom2D` disappears (its consumer `tests/test_geom_planar` links `vn::Geom` now); what 2.5 of `geometry-planar.md` used to say ("header-only / INTERFACE target") no longer holds and was rewritten as "one module, one library, one global, two namespaces".
- Migration: `vn::Geom3D` / `vn::Geom2D` -> `vn::Geom` (16 CMakeLists); `#include "geom2d_global.hpp"` / `"geom3d_global.hpp"` -> `#include <vine/geom/geom_global.hpp>` (64 headers); `VN_GEOM2D_API` / `VN_GEOM3D_API` -> `VN_GEOM_API`.
- Not done: renaming the namespaces (`vn::geom::planar` / `vn::geom::spatial` stay - they are concept names, not module names).

## Evidence

- `src/base/geom/CMakeLists.txt`: one target, `VN_GEOM_LIB`, and the comment saying the isolation is guaranteed by the script.
- `src/base/geom/sdk/vine/geom/geom_global.hpp`: one API macro, two namespace macros, two `Continuity` re-exports.
- `scripts/check_geom_layering.py`: 62 geometry headers, 0 cross-tree includes.
- Verified: the full Debug build passes; `test_math` 56, `test_geom_planar` 138 and `test_geom_spatial` 193 green; `check_ai_docs` / `check_design_docs` / `check_public_docs` / `check_include_hygiene` / `check_doc_symbols` / `check_export_annotations` / `check_diagnostic_formats` all report 0 findings.
