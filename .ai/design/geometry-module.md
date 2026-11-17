# geometry module: one module, two contracts, one shared implementation

> **Status**: current
> **Last verified**: 2026-11-10
> **Scope**: the physical shape and naming rules of `src/base/geom` — directories and namespaces, the `vn::Geom` target, the two contract subtrees `geom/planar` and `geom/spatial`, the shared implementation subtree `geom/detail`, the module-level vocabulary, the isolation gates, and the test for "what goes where". It contains no contract or algorithm of a specific curve / surface / solid (see `geometry-planar.md`, `geometry-kinds.md`, `geometry-primitives.md`).
> **Sources**: `src/base/geom/CMakeLists.txt`, `src/base/geom/sdk/vine/geom/geom_global.hpp`, `src/base/geom/sdk/vine/geom/Continuity.hpp`, `src/base/geom/sdk/vine/geom/{planar,spatial,detail}/`, `scripts/check_geom_layering.py`, `tests/test_geom_planar/ModuleContractTest.cpp`.
> **Backlog**: none

## 1. Overview

### 1.1 Problem and background

The geometry layer serves two contracts, planar and spatial, and both need the implementation that has no dimension. Three shapes fail: two independent targets (`vn::Geom3D` SHARED, `vn::Geom2D` INTERFACE) turn every sharing into a copy; merged into one module, the two trees still each hold a copy of `narrowCrossing`, the window rules and arc length; with the shared pieces moved down into `geom/detail`, two contract directories that stay **siblings** of the module directory force `Continuity` to be re-exported by two `using` lines and the shared pieces to be spelled `geom::detail::` in full. The current shape is one library, a module directory that owns every subtree, and one global.

### 1.2 Goals

1. Planar and spatial are two contracts of one domain; a consumer links one library (`G6`).
2. The implementation that has no dimension exists once, where both contracts are visible (`G1`).
3. The isolation between the contracts is **executable**, not a spoken agreement (`G11`).
4. Directory, namespace and type name agree: `geom/planar` ↔ `vn::geom::planar` (`G4`, `G12`).

### 1.3 Non-goals

- No "unified generic for 2D and 3D": the two contracts sit side by side, with zero inheritance and no interchangeability (`R1`).
- No dimension marker on a type: the namespace already carries the dimension (`G4`).
- No re-shuffle for tidiness: a re-shuffle must remove a tax, or it is not done (`G8`).

### 1.4 Conclusion

**`src/base/geom` is one module, one library `vn::Geom` and one global; under `vn::geom` three subtrees sit side by side — `planar` (the planar curve contract), `spatial` (spatial geometry) and `detail` (the dimension-free shared implementation) — plus the vocabulary `Continuity` at the module root. A contract may include `detail`, the two contracts may not include each other, and `detail` may not name a dimension; five rules are held by script gates.**

## 2. Design Points

### 2.1 One module, one library, one global

- **What**: `src/base/geom` is one CMake unit producing `vn::Geom` (compile definition `VN_GEOM_LIB`, entities marked `VN_GEOM_API`); the SDK root is `sdk/`, and the include paths are `<vine/geom/planar/...>`, `<vine/geom/spatial/...>`, `<vine/geom/detail/...>`. The target is SHARED because the spatial side has explicit instantiations and three non-template implementations (`src/Instantiations.cpp`, `LineCurve.cpp`, `Obb.cpp`, `Plane.cpp`); the planar side is header-only templates, shipped with the same library and exporting no symbol.
- **Why**: `G6`, `G10` — both contracts share one compile definition, one API macro and one set of symbols; split into two targets, every sharing needs a "third module" or a copy.
- **Boundary**: `geom` depends only on `vn::Math` and `vn::Core`; `math` does not know geometry (`math-base-layer.md`).

### 2.2 Directory and namespace correspond one to one

| Directory | Namespace | Content | API? |
|---|---|---|---|
| `sdk/vine/geom/` | `vn::geom` | `geom_global.hpp` (macros), `Continuity.hpp` (the smoothness ladder) | yes |
| `sdk/vine/geom/planar/` | `vn::geom::planar` | the planar curve contract `Curve<T>`, nine types, seven family algorithms (17 headers) | yes |
| `sdk/vine/geom/spatial/` | `vn::geom::spatial` | curves, surfaces, solids, rays, `Obb` / `Aabb` (50 headers, plus 7 under this contract's `spatial/detail/`) | yes |
| `sdk/vine/geom/detail/` | `vn::geom::detail` | the dimension-free shared implementation (9 headers) | **no** |

`geom_global.hpp` provides `VN_GEOM_API` and three namespace macros (`VN_GEOM_NS_BEGIN`, `VN_GEOM_PLANAR_NS_BEGIN`, `VN_GEOM_SPATIAL_NS_BEGIN`, `VN_GEOM_DETAIL_NS_BEGIN` and the matching `_END`).

**Nesting has two direct consequences**:

- **The vocabulary at the module root needs no re-export**. `Continuity` lives in `vn::geom`, the two contracts are its child namespaces, and an unqualified name is found by enclosing-namespace lookup — `geom_global.hpp` contains no `using`.
- **A shared piece must be spelled `geom::detail::` in full**. C++'s nearest-name rule makes an unqualified `detail::` inside either contract resolve to its **own** `planar::detail` / `spatial::detail` (six headers declare them), so qualifying the shared piece is not style but correctness (see P1).

### 2.3 What goes where: three-step test

1. **Is it part of a public contract?** Yes → the matching contract subtree, `vn::geom::planar` / `vn::geom::spatial`.
2. **Do both contracts need it?** No → keep it in its own contract (in that contract's `detail` when necessary).
3. **Both need it, and it is dimension-free?** Yes → `geom/detail/` (`vn::geom::detail`). The test: it depends only on `C::value_type` and `evaluate` / `derivative`, and `Point2` / `Point3` never appear in a signature.

**The test has to be able to refuse**: put the two copies side by side; if the only difference is a type name and one line of documentation → move it down; if the difference is mathematical → keep one each (`signedCurvature` signed vs `curvature` magnitude; `closestPointOnCurve`'s result type carries `Point2` / `Point3`).

### 2.4 Naming rules

- **A type name carries no dimension marker**: `LineCurve` / `Curve`, not `LineCurve2d` / `Curve2`; the namespace carries the dimension. The precision suffix stays `f` / `d` (`LineCurved`, `CircleCurvef`).
- **`detail` is not API**: no stability and no ABI promise; to publish a shared capability, first add one public entry in each contract namespace. `planar::arcLength` / `spatial::arcLength` are exactly such a thin shell (resolve the window and tolerance, then forward). This is no longer a spoken agreement: gate 7 lets only this module and its own two test directories include `detail`.
- **A same-named public entry belongs to two namespaces by contract**: `arcLength`, `parameterAtLength`, `closestPointOnCurve` and `curveToCurveDistance` exist once per contract (see P2).

### 2.5 Dependency direction and gates

- **Direction**: `planar` / `spatial` → `detail` → `math`; there is no edge between the two contracts.
- **Seven gates** (`scripts/check_geom_layering.py`, currently 83 files with 0 violations):
  1. `planar` may not include `spatial`;
  2. `spatial` may not include `planar`;
  3. `detail` may not include either contract;
  4. `detail` may not **name** `Point2` / `Point3` / `Vector2` / `Vector3` — a dimension leak that an include check cannot see is caught by name;
  5. the include domain of a geometry header: a bracketed include with a path may only be `vine/geom/` or `vine/math/`, a quoted include only the same directory — that is, "the only edges are geometry → geometry and geometry → math";
  6. `spatial/Bvh.hpp` may not include `spatial/BvhBuilder.hpp` — the tree holds primitives and does not know, and must not know, how it was built;
  7. only `src/base/geom` and `tests/test_geom_planar` / `tests/test_geom_spatial` may include `detail`; any other module that includes it is pinned to a surface with no stability promise.
- **Why a script and not the linker**: the three subtrees sit inside one target, so the compiler cannot stop a violation; the old two-target arrangement would make the "one module" fact disappear (`ADR-2026-11-06-merged-geom-module.md`).
- **A gate must be able to fail**: after adding a rule, verify it in reverse with a temporary violating header, then delete that header and confirm the count is back to 0.

### 2.6 Relation to `math`

- `math` provides mathematical vocabulary only (value types, constants, predicates, concepts) and does not know geometry; the numeric chain on the geometry side (root finding → tolerance conversion → integration / inverse, spline evaluation and the knot model) all lives in `geom/detail`.
- The cost is accepted explicitly: any non-geometry module that needs integration or root finding must depend on `vn::Geom`, copy a piece, or promote the kernel to a module of its own; the last is the booked reversal path (`ADR-2026-11-06-math-keeps-only-vocabulary.md`).

### 2.7 Why the shared implementation lives in headers (measured, not re-argued)

- **The test**: look at the **size of the instantiation set**, not at the length of the code. A set that grows without bound with the caller's types (a `Quadrature`, `RootFinding`, `CrossingSearch`, `CurvePairSearch`, `BSplineBasis` or `BezierBlend` generalised over a lambda or a container) ⇒ header only; a set that is finite and known ⇒ may move down into `src/*.cpp` with explicit instantiation (`SolidTessellation.cpp` is 5 kinds × 2 precisions, `Instantiations.cpp` is the value types).
- **Measured**: after splitting the three kernels of `CurveSubdivision.hpp` into `src/CurveSubdivision.cpp` (2 contracts × 2 precisions × 3 kernels = 12 exported instantiations), the compile time of a TU including `CurveSampling.hpp` was **2.16 s → 2.16 s** (MSBuild `ClCompile` forced single-file rebuild, minimum of three; `CurveLengthTest.cpp` in the same directory as a control, 1.96 s). At this size there is no measurable gain, so the kernels stay header-only and the split is reverted. <!-- drift-ok -->
- **Boundary**: the conclusion is not permanent. Reopen when a TU that only wants the declarations has `detail` as its compile-time bottleneck, or when the number of consumers reaches double digits. Then split in the shape of `SolidTessellation.cpp` (declarations in the header, definitions plus explicit instantiation in a `.cpp`) rather than inventing a third spelling.

## 3. Features

| # | Feature | Content | Status |
|---|---|---|---|
| F1 | Single-library layout | `vn::Geom` carries both contracts and the shared implementation; the include path is split in three | landed |
| F2 | One global | the API macro of `vn::Geom`, four namespace macros (root + two contracts + `detail`) | landed |
| F3 | Shared implementation subtree | 9 `geom/detail/*.hpp`, includable from both contracts | landed |
| F4 | Isolation gates | the five rules of `check_geom_layering.py` | landed |
| F5 | Naming rules | no dimension in a type name, precision suffix `f`/`d`, `detail` is not API | landed |

### 3.1 F3 the shared implementation subtree (current list)

`CurveTraits.hpp` (`CurveScalar` / `CurveWindow` / `resolveTolerance` / `resolveCurveWindow`), `CrossingSearch.hpp` (`narrowCrossing`), `CurveMeasure.hpp` (`arcLengthBetween` / `parameterCovering`), `CurvePairSearch.hpp` (`refineCurvePair`), `CurveSubdivision.hpp` (`sampleByParameter` / `sampleByLength` / `subdivideByDeflection`, touching only `C::Point`), `BSplineBasis.hpp` (the distinct-knot + multiplicity knot model and the Cox–de Boor basis), `BezierBlend.hpp` (de Casteljau and divided differences), `Quadrature.hpp` (adaptive Simpson), `RootFinding.hpp` (root finding in a bracket, inverting a monotone quantity).

**Acceptance** (T1, T3): both geometry test targets are green; the gate reports 0 violations; no reference inside one contract points at the other contract.

## 4. Public Interface

| Header | Public content |
|---|---|
| `geom_global.hpp` | `VN_GEOM_API`, `VN_GEOM_NS_BEGIN` / `VN_GEOM_PLANAR_NS_BEGIN` / `VN_GEOM_SPATIAL_NS_BEGIN` / `VN_GEOM_DETAIL_NS_BEGIN` (and the matching `_END`) |
| `Continuity.hpp` | `vn::geom::Continuity` (an ordered enum); both contracts use it directly through enclosing-namespace lookup |
| `planar/*.hpp` | `vn::geom::planar`: `Curve<T>`, nine types, six family algorithms |
| `spatial/*.hpp` | `vn::geom::spatial`: curves / surfaces / solids / rays / `Obb` / `Aabb` and their family algorithms |
| `detail/*.hpp` | `vn::geom::detail`: **not** API |

## 5. Constraints, Requirements and Invariants

- **C1**: one module, one CMake unit, one target `vn::Geom`, one compile definition `VN_GEOM_LIB`, one global header.
- **C2**: directory and namespace correspond one to one: `planar/`↔`vn::geom::planar`, `spatial/`↔`vn::geom::spatial`, `detail/`↔`vn::geom::detail`, module root↔`vn::geom`.
- **C3**: a dimension-free shared implementation lives only in `geom/detail/`; a helper meaningful to one contract only stays in that contract's `detail`.
- **C4**: a type name carries no dimension marker; the precision suffix is `f` / `d`; a name in `detail` is not API.
- **C5**: the dependency direction is `contract → detail → math`, with no back edge and no edge between the two contracts.
- **C6**: the five gates must all be green; `Point2` / `Point3` / `Vector2` / `Vector3` may not appear in `geom/detail` (by name or by include).
- **C7**: a name at the module root **may not** be re-exported inside either contract — the nested namespace already provides visibility, and one extra `using` is a second source of truth.
- **C8**: a new subtree (a third kind of geometry) is first tested by 2.3, then decided to be a directory, a namespace or a new module.
- **R1**: the two contracts sit side by side, with zero inheritance and no interchangeability; `planar::Curve<T>` is not a specialisation of `spatial::Curve<T>`.
- **R2**: a geometry header includes only `vine/geom*`, `vine/math*` and the standard library.
- **N1**: `continuity()` in both contracts returns the one type `vn::geom::Continuity` (`ModuleContractTest` pins it with `static_assert`).
- **N2**: a same-named public entry (`arcLength` and friends) belongs to two namespaces by contract; the signatures may be identical.

## 6. Caveats and Traps

- **P1 An unqualified `detail::` resolves to the contract's own one**: `planar/CurveCurveIntersection.hpp` and `spatial/{Aabb,CurveSurfaceIntersection,RaySurfaceIntersection,SolidDetail,SurfaceProjection}.hpp` declare a tree-private `detail` of the same name; as soon as any of them enters the same TU, `detail::X` inside the contract resolves to that layer and not to `geom::detail`. **Always spell a shared piece `geom::detail::`** (measured: a bare `detail::` fails immediately with C2039).
- **P2 Same-named public functions cannot share one namespace**: `closestPointOnCurve` and `curveToCurveDistance` have **identical parameter lists and different return types** in the two contracts, and C++ cannot overload on the return type; `arcLength` / `parameterAtLength` do have the same return type, but merging them would distinguish calls only through mutually exclusive `requires` clauses — an implicit mechanism (`G11`). That is why the dimension cannot go into the type name and the two contracts cannot merge into one namespace.
- **P3 The precision suffix and a dimension suffix compete for the same place**: we already use `d` / `f` as the scalar suffix (`Vec2d`, `Rect2d`, `LineCurved`), and both spellings the industry uses for a dimension (CGAL's `_2`, OCCT's `2d`) collide with precision here; a trailing `2` would also blur into the "quadratic" meaning of `BezierCurve::degree()`. So the dimension goes into the namespace only.
- **P4 Look at the numbers before re-laying-out the library**: one directory and naming re-shuffle touched 850 code references + 265 documentation references (measured). Either it removes a long-standing tax (as this one removed two re-exports and one sibling-namespace trap) or it is not done.

## 7. Rejected Options

- **D1** Two targets (`vn::Geom2D` INTERFACE + `vn::Geom3D` SHARED): `G1`, `G6` — every sharing needs a third module or a copy. See `../decisions/ADR-2026-11-06-merged-geom-module.md`.
- **D2** Each contract copies the shared pieces: `G1`, `G5` — when the only difference is a type name, two copies are two maintenance sites. See `../decisions/ADR-2026-11-06-geom-detail-subtree.md`.
- **D3** Keep the old shape where `geom2d` / `geom3d` are siblings of `geom` (re-export plus full qualification): `G11`, `G4` — the module directory does not contain the module's content, `Continuity` needs two `using` lines, `detail` needs a prefix to tell apart, and there are two vocabularies for directory and namespace. See `../decisions/ADR-2026-11-06-geom-contracts-nested-under-module.md`.
- **D4** Flatten into `geom/` and mark only the planar contract (`Curve2` / `CircleCurve2`): `G4`, `G11` — four pairs of same-named public functions (two pairs of which cannot be overloaded), a `d` collision between `LineCurved` and `LineCurve2d`, and unmarked types silently meaning "spatial by default".
- **D5** Flatten and mark both contracts (`Curve2` / `Curve3`): `G4`, `G1` — 15 + 47 types and 4 functions renamed at once, and the names would claim "two dimensions of one family".
- **D6** Let `math` keep carrying root finding / integration and the spline representation conventions: `G1`, `G4` — `math` would keep two subdirectories used only by geometry. See `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.
- The full comparison of D4–D5 with the industry evidence (OCCT / CGAL / Eigen / GLM) is in `../decisions/ADR-2026-11-06-geom-contracts-nested-under-module.md`.

## 8. Tests and Acceptance

| # | Criterion | Case / command |
|---|---|---|
| T1 | Both contracts work inside one library | `test_geom_planar` (140 cases), `test_geom_spatial` (213 cases) green |
| T2 | Isolation: no cross-contract include, `detail` names no dimension, include domain | `python scripts/check_geom_layering.py` → `0 finding(s) in 70 file(s)`, and a temporary violating header makes it fail |
| T2b | Module-level invariant: `continuity()` in both contracts returns the same `vn::geom::Continuity`, and the ladder is ordered | the three `static_assert`s and `TheLadderIsOrderedBySmoothness` in `tests/test_geom_planar/ModuleContractTest.cpp` |
| T3 | The shared pieces really exist once | the same-named implementations on the two contracts differ only in documentation (line-by-line diff: the bodies of `CurveLength` / `CurveCurveDistance` / `CurveProjection` have no difference left) |
| T4 | Header hygiene and public-surface documentation | `check_include_hygiene.py`, `check_public_docs.py` both 0 findings |
| T5 | Document structure | `check_ai_docs.py --strict`, `check_design_docs.py` both 0 findings |

## 9. Status and Evolution

- **Current state**: F1–F5 are all in place; `vn::Geom` is a single target; `geom/{planar,spatial}` hold 65 headers and `geom/detail` 8; the five gates report 0 violations; the two geometry test targets run 353 cases, all green.
- **Reversal conditions**: (1) a **third subtree** appears (polygons, mesh geometry, …) — test it by 2.3 to decide whether it is a directory, a namespace or a new module; (2) a non-geometry consumer of root finding / integration / Bernstein appears — promote the kernel to a module of its own and let the geometry side change only its includes (see `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`).
- **Not done**: a dimension marker in a type name (D4–D5); unless a second tax shows up that proves it removes one.

## 10. References

- Code: `src/base/geom/CMakeLists.txt`, `sdk/vine/geom/geom_global.hpp`, `sdk/vine/geom/Continuity.hpp`, `sdk/vine/geom/detail/` (8 headers), `sdk/vine/geom/planar/` (16), `sdk/vine/geom/spatial/` (49, plus 7 under `detail/`).
- Gates: `scripts/check_geom_layering.py`.
- Decisions: `ADR-2026-11-06-merged-geom-module.md`, `ADR-2026-11-06-geom-detail-subtree.md`, `ADR-2026-11-06-geom-contracts-nested-under-module.md`, `ADR-2026-11-05-planar-curve-contract.md`, `ADR-2026-11-06-spline-model-moves-to-geom.md`, `ADR-2026-11-06-math-keeps-only-vocabulary.md`.
- Related design: `geometry-planar.md` (the planar curve contract and its nine types), `geometry-kinds.md` (the spatial curve / surface families), `geometry-primitives.md` (spatial value types), `math-base-layer.md` (the layer below holds vocabulary only).
