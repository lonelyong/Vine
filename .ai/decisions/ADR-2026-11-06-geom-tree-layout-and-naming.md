# ADR-2026-11-06: the geometry trees keep their directory position and their dimension naming

- Status: Superseded (both contracts have sunk into the `geom/{planar,spatial}` subdirectories and the `vn::geom::{planar,spatial}` sub-namespaces, see `ADR-2026-11-06-geom-contracts-nested-under-module.md`; this document's rejection of a `2` suffix and of "tag both contracts" still holds)
- Date: 2026-11-06
- Related: `../design/geometry-module.md` 2.2-2.5 / 7, `ADR-2026-11-06-merged-geom-module.md`, `ADR-2026-11-06-geom-detail-subtree.md`

## Background

One session produced three reorder proposals, all aimed at the same thing - "make the module's physical shape look more like what it says it is":

1. add `2` / `3` suffixes to the types and merge the two namespaces into one;
2. move the two trees down to `geom/planar` and `geom/spatial` and sink the namespaces with them;
3. flatten everything into `geom/` and tag only 2D (`Curve2`, `CircleCurve2`), leaving 3D untagged.

The real problem they were meant to solve (dimension-independent code needs a shared place inside the module) **was already solved**: `geom/planar` and `geom/spatial` share implementations today by including `vine/geom/detail/...` (16 of their 64 headers do so), and `scripts/check_geom_layering.py` explicitly allows that direction and forbids the two trees from including each other, with 0 violations at the time. **All the reorder could add was "names and directories that look tidier", not capability.**

## Options

| Option | Benefit | Reason for rejection |
|---|---|---|
| A. Nest `geom/planar` + `geom/spatial` (namespaces `vn::geom::planar` / `spatial`) | `Continuity` and `detail` become visible through the enclosing namespace, and the two `using` blocks in `geom_global.hpp` (plus the "sibling namespaces cannot be found" note) can be deleted | All 689 code references and 265 documentation references rewritten for 20 lines fewer; the two trees still each have a `CircleCurve.hpp`, so the file-identification problem is untouched |
| B. Flatten and tag only 2D (`Curve2` / `CircleCurve2`), 3D unchanged | Not one 3D name moves; files are distinguishable at a glance | (1) four pairs of **public functions** share a name, and `closestPointOnCurve` and `curveToCurveDistance` have the same parameter list and different return types - C++ cannot overload on the return type, so the functions have to be renamed too (`closestPointOnCurve2`), and that is user-visible API; (2) `LineCurved` (3D) against `LineCurve2d` (2D): `d` / `f` are already taken by the precision aliases (`Vec2d`, `Rect2d`), so two suffixes crowd into one position; (3) `CircleCurve` becomes the unwritten convention "untagged means 3D" (`G4`); (4) `BezierCurve2` / `BSplineCurve2` collide with the "quadratic/cubic" meaning that `degree()` carries |
| C. Flatten and tag both trees (`Curve2` / `Curve3`) | Fully symmetric, one namespace | 15 2D types, 47 3D types and 4 functions renamed at once; and the names would claim "two dimensions of one family" - 2D has no surfaces or solids, and the two contracts share zero inheritance and are not interchangeable (`G4`, `G1`) |

The industry comparison supports only one of these spellings, and it is not a suffix: OCCT tags 2D and leaves 3D untagged, but writes the tag as an **infix** (`Geom_Curve` against `Geom2d_Curve`, `gp_Circ` against `gp_Circ2d`), precisely to avoid competing with degree and order. This repository cannot even use `2d` - `d` is already the scalar suffix.

## Decision

Keep the status quo:

- Directories: `sdk/vine/geom/planar` and `sdk/vine/geom/spatial` are two trees under `sdk/vine/` at the same level as `geom/`; `geom/` holds the module's `geom_global.hpp`, `Continuity.hpp` and `detail/`.
- Naming: the dimension is carried by the **namespace** (`vn::geom::planar` / `vn::geom::spatial`), type names carry no dimension tag, and the precision suffix stays `d` / `f`.
- Sharing: both trees may include `vine/geom/detail/` and may not include each other - guaranteed by the gate, not by the directory position.
- Known cost (accepted): `geom/planar` / `geom/spatial` are siblings of `geom` rather than children, so `geom_global.hpp` has to re-export `Continuity` into both namespaces; that import and its explanation stay, and the `@note` already says "unqualified names resolve only through the import".

Criteria: `G8` (a reorder without a functional gain is not done), `G4` (the dimension is already stated in the namespace, so putting it into the name repeats it and competes with degree and precision), `G11` (the user-visible `closestPointOnCurve` / `arcLength` are not renamed).

## Consequences

- Good: this round needed no code change; the sharing inside `geom/detail` is as it was; the public API and aliases of both trees did not move.
- Cost / leftover: all 18 of `math`'s headers are under `vine/math/`, while the three geometry trees are not all under `vine/geom/` - that asymmetry stays. It is a **purely cosmetic directory** question and not worth 689 rewrites.
- When to re-evaluate: a **third tree** appears (polygonal or mesh geometry, for example), or `geom`'s module-level vocabulary grows enough to need its own namespace levels. The criterion is "is there a functional gain", not "do the names look better".

## Evidence

- Today: `geom/planar` 17 headers, `geom/spatial` 47 headers, `geom/` 2 headers plus 8 under `detail/`; 16 of the in-tree headers include `vine/geom/detail/...`.
- Gate: `python scripts/check_geom_layering.py` -> `0 finding(s) in 70 file(s)`.
- Rename surface: 689 occurrences of `geom::planar::|geom::spatial::|vine/geom/planar|vine/geom/spatial` in `src` + `tests`; 265 in `.ai`.
- Same-named functions measured: `arcLength`, `parameterAtLength`, `closestPointOnCurve` and `curveToCurveDistance` exist once in each tree; 2D also has `offsetCurve` / `trimCurve` / `intersectCurveWithCurve`, and 3D has `curvature` / `closestPointOnSurface` and 17 others.
