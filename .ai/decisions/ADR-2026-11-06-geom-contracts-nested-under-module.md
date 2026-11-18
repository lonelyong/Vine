# ADR-2026-11-06: the two contracts sink into sub-namespaces of the module

- Status: Accepted (supersedes the "keep sibling directories" decision of `ADR-2026-11-06-geom-tree-layout-and-naming.md`)
- Date: 2026-11-06
- Related: `../design/geometry-module.md` 2.2 / 2.5 / 6, `ADR-2026-11-06-merged-geom-module.md`, `ADR-2026-11-06-geom-detail-subtree.md`

## Background

Earlier the same day, after `geom/detail` sank, I evaluated a reorder once and concluded "keep `geom2d` / `geom3d` as sibling directories of `geom`", on the ground that "the reorder only makes the names look better while costing 689 references". That evaluation described the benefit as "deleting two `using` lines" and **got it wrong**:

- `geom2d` was a sub-namespace of `vn` rather than of `vn::geom`, so `Continuity` had to be re-exported with two `using` lines and `detail` had to be written in full as `geom::detail::`. That is a **tax collected every day**, not a one-off tidiness question.
- The directory said `vine/geom2d`, the namespace said `vn::geom2d`, and the module directory `vine/geom` held only two headers - three places disagreeing (`G4`, `G12`).

## Options

| Option | Benefit | Cost |
|---|---|---|
| A. Keep the sibling directories | No change | The re-export, the sibling trap and the directory/namespace mismatch stay forever |
| B. Nest only the namespace, leave the directories | The visibility of `Continuity` / `detail` improves | The directory and the namespace say two different things, which is worse than the status quo |
| C. Sink both directory and namespace: `geom/{planar,spatial,detail}` plus `vn::geom::{planar,spatial,detail}` | The three places agree; the re-exports go to zero; the module directory owns the module's content | 850 code references and 265 documentation references rewritten mechanically |

## Decision

Chose **C**:

- `sdk/vine/geom2d/` -> `sdk/vine/geom/planar/`, `vn::geom2d` -> `vn::geom::planar`; `sdk/vine/geom3d/` -> `sdk/vine/geom/spatial/`, `vn::geom3d` -> `vn::geom::spatial`.
- The macros are renamed: `VN_GEOM2D_NS_BEGIN/END` -> `VN_GEOM_PLANAR_NS_BEGIN/END`, `VN_GEOM3D_NS_*` -> `VN_GEOM_SPATIAL_NS_*`; `planar` / `spatial` are expanded from `VN_GEOM_NS_BEGIN`, so the three really nest.
- **`geom_global.hpp` re-exports nothing any more**: `Continuity` lives in `vn::geom`, the two contracts are its sub-namespaces, and unqualified names are found by enclosing-namespace lookup. `Continuity.hpp` now includes `geom_global.hpp` and uses `VN_GEOM_NS_BEGIN` - `geom_global.hpp` no longer includes it, so there is no include cycle.
- The test suites are renamed with it: `tests/test_geom_planar` / `tests/test_geom_spatial`.
- The gate's path constants become `geom/{planar,spatial}`, the five rules are unchanged, and the gate was reverse-verified once (a temporary offending header -> 2 findings -> 0 after deleting it).

Criteria: `G11` (one trap and two re-exports removed, two qualifiers fewer written every day), `G4` (directory, namespace and type names agree), `G12` (the module directory owns the module's content).

## Consequences

- Good: `Continuity` is an ordinary name in both contracts and re-exporting does not exist; directories and namespaces correspond one to one; include paths start with the module name (`<vine/geom/planar/...>`); `geom_global.hpp` shrinks to "macros plus an explanation".
- **Cost (measured)**: 850 code references and 265 documentation references rewritten mechanically; the two suite directories and their `TEST_NAME` renamed; the follow-up and supersession notes of five ADRs.
- **One expected benefit that did not materialise (recorded)**: the estimate was that "177 of the 192 `geom::detail::` occurrences can be shortened to `detail::`". The compiler disproved it at once - the six contract headers declare same-named `planar::detail` / `spatial::detail`, and as soon as any of them enters the same TU, a bare `detail::` resolves to that layer (`error C2039`). The shared code **still has to be written as `geom::detail::`**, and this reorder did not shorten it. That is the price of one word, `detail`, carrying two roles inside a module (module-shared and contract-private).
- Reversal condition: none. The directory and the namespace agree now, and the only change left below is "a third subtree" or "promote the kernel to its own module".

## Evidence

- Directories: `src/base/geom/sdk/vine/geom/{planar,spatial,detail}` (17 / 47 / 8 headers) plus 2 headers at the module root.
- Gate: `python scripts/check_geom_layering.py` -> `0 finding(s) in 70 file(s)`; with a temporary probe header -> `2 finding(s) in 71 file(s)`, exit code 1.
- Tests: `test_math` 36, `test_geom_planar` 140, `test_geom_spatial` 213, 389 cases green together.
- Volume rewritten: `_nest_geom.py` reported 133 source files; the namespace, macro and path replacements counted 195 include paths, 132 macros and 493 qualified names; 37 documentation files.
