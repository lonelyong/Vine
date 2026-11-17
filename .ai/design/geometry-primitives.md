# geometry value types: Aabb, Obb, Line, Plane

> **Status**: current
> **Last verified**: 2026-11-04
> **Scope**: the spatial value types of `src/base/geom/sdk/vine/geom/spatial` (`Aabb` / `Obb` / `LineCurve` / `Plane`): membership, naming and contract.
> Not the storage and revision of `Shape` / `Mesh` / `Material` / `Model` (see `graphics-geometry.md` §3.5), and not the 2D rectangle `Rect2` (still in `math`).
> **Sources**: `src/base/geom/sdk/vine/geom/spatial/Aabb.hpp`, `Obb.hpp`, `LineCurve.hpp`, `Plane.hpp`; `tests/test_geom_spatial`.
> **Backlog**: `../backlog/graphics.md`

## 1. Overview

### 1.1 Problem and background

Four spatial shapes sat in `src/base/math`: `Rect3` (an axis-aligned box), `Line` (today `LineCurve`) and `Plane`; the `Obb` introduced at the same time, however, was geometry's. One concept family was cut in two by a module boundary.

`Rect3` was the worse case: `math::Aabb` / `Aabbf` / `Aabbd` were aliases of it, `geometry/Aabb.hpp` was a forwarding shim, and `Mesh::Aabb` was a third spelling. One axis-aligned box had four names and three homes.

Every consumer lives above geometry: `Mesh`, `graphics::Node`, `ChainBuilder`, `ChainPresets`, `Obb::toAabb()`. Nothing inside `math` used `Rect3` / `Line` / `Plane` (`Plane` uses `Line`, so the two move together; `Line` is now `LineCurve`, see `geometry-kinds.md`).

### 1.2 Goals

- An axis-aligned box has one name, `Aabb`, and one home, `geometry`.
- Box, line and plane live with `Obb`: the analytic primitives share one module, and a `Plane` return value need not cross a module boundary.
- `math` keeps numbers and transforms only and holds no shape.
- `Rect2`'s consumer (a GUI rectangle) is unaffected.

### 1.3 Non-goals

- Do not move `Rect2`: its only consumer is a layout rectangle in `appfw::Gui`, not a shape (2.6).
- Provide no `math::Rect3` compatibility alias: `math` may not include `geometry`, which would be a back edge (2.5, `D2`).
- Do not introduce intersection / distance queries here (ray–AABB, OBB–OBB, AABB–OBB); the types are put in place first and the queries wait for a real consumer (`G8`).

### 1.4 One-paragraph conclusion

**Numbers and transforms belong to `math`, spatial entities to `geometry`; the axis-aligned box is called `Aabb`, with no alias and no back edge.**

## 2. Design Points

### 2.1 The boundary test: numbers vs spatial entities

- **What**: `math` = scalars, vectors, points, matrices, quaternions, isometries; `geometry` = entities with spatial meaning and the shape operators (box, line, plane, mesh, parametric shape).
- **Why**: `G1`, `G5` — the test decides in one sentence where a new type goes, with no case-by-case discussion; `Rect3`'s consumers were already on the `geometry` side.
- **Boundary**: the test decides **type membership** only, and does not mean `geometry` may depend on anything besides `math`; `math` still links only `vn::Global`.
- **Cost**: someone who wants only a box now links `vn::Geometry` (+ `vn::Core`). Today's `Aabb` consumers (`geometry`, `graphics`, the tests of both) all link it already, so no dependency edge is actually added (`G10`).

### 2.2 One box, one name

- **What**: the type is `vn::geom::spatial::Aabb<T>`; the only aliases are `Aabbf` / `Aabbd` / `Aabbi`. `Rect3`, `math::Aabb*` and geometry's empty forwarding header are all gone.
- **Why**: `G1`, `G4`, `G12` — the name states the shape; `Aabb` pairs with `Obb`, and `Rect` is a wrong name in 3D.
- **Boundary**: `Mesh::Aabb` stays as the class-scoped float spelling of `Mesh` (`= Aabbf`); it is an alias, not a type, and does not form a second concept.
- **The template parameter is deliberately not constrained to `vn::math::FP`**: `Aabb<T>` / `Obb<T>` are **spatial value types**, and `Aabbi = Aabb<int32_t>` is a legal integer index box (pixel / grid indices), so `typename T` is used here; the geometry families (`Curve<T>` / `Surface<T>` and their members) are always constrained to floating point, which is why `Plane<T>` uses `vn::math::FP T`, consistent with the rest of the family (`G6`). The two differ on purpose, not by omission.
- **Only `Aabb` has `isValid()`**: `Aabb` allows an **empty sentinel** state (`min > max`, so `expandBy` can grow from nothing), so it needs a test (`isValid() == !isNeg()`, two questions about one fact, the same as `isEmpty()`); `Obb` eliminates the invalid state at construction (absolute half extents, a zero-length quaternion normalised back to the identity rotation) and cannot be constructed unusable, so it has no `isValid()` and needs none (`G4`: an interface carries only the questions that can fail).

### 2.3 The analytic primitives share one module

- **What**: `LineCurve`, `Plane`, `Aabb` and `Obb` are all in `vine/geom/spatial`; the return value of `Plane::intersectWith(Plane, LineCurve&)` is in the same module.
- **Why**: `G1`, `G6` — shapes of one layer are not split across modules; `LineCurve`'s only user is `Plane`, so they move in one batch with no intermediate state.
- **Boundary**: they are **value types** and do not enter the `Shape` hierarchy (2.4); this does not turn `geometry` into "pure maths" — `Shape` / `Mesh` remain the main content. A ray is a spatial entity too: `Ray` and its intersections with primitives are already in `geometry`, with the contract in `geometry-ray.md`; the division of labour between parametric geometry (`Box` / `Sphere` / …) and the model holder `Primitive` is in `geometry-parametric.md`.

### 2.4 Value types stay out of the Shape hierarchy

- **What**: `Aabb` / `Obb` / `LineCurve` / `Plane` are template value types: no vptr, no `Object` meta type, no revision.
- **Why**: `G1`, `G10` — a bounding box is copied and returned by value; `Shape`'s run-time polymorphism and revision exist for editable content, not for geometric quantities.
- **Boundary**: a shape that needs polymorphic dispatch goes through `Shape`; a spatial quantity that needs cheap copies uses these four value types.

### 2.5 No back edge

- **What**: the CMake dependency of `math` is still `vn::Global` only; `math` includes no `vine/geom/spatial/*`.
- **Why**: `G2` — dependencies are one-way, and `geometry → math` may not be reversed.
- **Cost**: existing code using `math::Rect3` must move to `geom::spatial::Aabb` in one go, with no "compatible for one version" buffer (`D2`).

### 2.6 `Rect2` stays in `math`

- **What**: `vn::math::Rect2<T>` is untouched; `using Rect = math::Rect2i` in `appfw::Gui` is untouched.
- **Why**: `G1`, `G5`, `G8` — it expresses a layout / hit rectangle, not a spatial shape; `geometry` is all 3D entities today, and pulling it in for symmetry would add an unrelated responsibility.
- **Boundary**: a real 2D geometry consumer (projected bounds, 2D collision) is evaluated separately when it appears, not moved in advance.

### 2.7 Fitting lives in `Obb`, decomposition in `math`

- **What**: `Obb::computePca` / `Obb::computeFixedOrientation` only do "find the axes → project and take min/max → assemble the quaternion"; they depend on `math::symmetricEigen` (the Jacobi decomposition of a 3×3 symmetric matrix, see `EigenDecomposition.hpp`) and `math::toQuaternion` (rotation matrix → quaternion).
- **Why**: `G1`, `G2` — eigen decomposition is a numeric operation on a matrix, not a spatial entity; putting it in `geometry` would force `math` to depend on it in order to be reused. The fit itself is a shape operation and stays in `Obb`.
- **Boundary**: `symmetricEigen` promises only a symmetric matrix, an ascending spectrum, orthonormal columns and a positive largest component per column; handedness is not guaranteed, and a caller needing a rotation matrix completes the third axis with a cross product (which is what `Obb` does).
- **Why a separate header**: `Matrix3x3.hpp` keeps only the matrix itself and the algorithms are in `EigenDecomposition.hpp`; a translation unit that only uses the matrix does not pay for the algorithms and `<cmath>`, and the matrix type and the solver evolve separately (`G5`, `G10`).
- **Axis order**: PCA orders by **descending** variance (local X largest, Z smallest). The reason: the conventional reading of a principal axis is largest to smallest, so a consumer reading `halfExtents()` sees X as the longest edge (`G4`, `G12`); the old implementation took the first two columns in ascending order (X = smallest variance), which had no stated support and contradicted the intuition of "principal axis".
- **Not done**: the minimum-volume OBB (it depends on a third-party convex-hull rotating-calipers implementation and has no consumer today, `G8`); `merge` is not provided for the same reason.

### 2.8 The short queries of the value types are defined in the header

- **What**: the short queries and accessors of `Aabb` / `Obb` / `LineCurve` / `Plane` / `Ray` (`contains`, `expandBy`, `intersectWith`, `transformed`, `closestPoint`, `pointAt`, `distanceToPoint`, `corners`, `toAabb`, `intersects`, `intersectTriangle`) are defined in the `sdk` headers; only the long algorithms stay in `src/`, keeping the `template class VN_GEOMETRY_API` explicit instantiation.
- **Layout**: the class body of each type keeps only declarations and trivial accessors, and the definitions sit together after the class and before the aliases. The public shape of one type can therefore be read in one pass (`G4`, `G12`).
- **Why**: `G7`, `G10` — `geometry` is a DLL, so a cross-module call cannot inline; the per-triangle picking measured 7.09 → 5.99 ns on hit and 3.96 → 2.71 ns on the parallel rejection (an independent benchmark, about 15–30%). Keeping the call site in the caller's module is cheaper than making the caller cross a library boundary for one point test.
- **Boundary**: the test is "short and purely arithmetic" — it depends on no `EigenDecomposition.hpp`, no OCCT and no anonymous-namespace helper. It is not "inline everything that can inline": the fits (`Obb::computePca` / `computeFixedOrientation`), `LineCurve::intersectWith(LineCurve)` and `Plane::intersectWith(Plane, LineCurve&)` stay in `src/`, or a public header would drag in eigen decomposition and 60 lines of derivation comments and charge every consumer (`G11`, `G10`).
- **Cost**: the class carries no `VN_GEOMETRY_API`, and the export comes only from explicit instantiation; and **the library provides an explicit instantiation for every geometry type in both precisions**: a type with a `src/` implementation does it in its own `.cpp` (`LineCurve` / `Obb` / `Plane` — those `.cpp` files are the only place that can see its non-inline members), and header-only types are collected in `src/Instantiations.cpp` (one reviewable list that is also the compile-time check that "every type can be instantiated"). No `extern template` is written: inlining still happens at the call site, and the exported symbol is the fallback when inlining is impossible, not the only implementation. The policy for precision and instantiation is in `../decisions/ADR-2026-11-04-geometry-precision-and-instantiation.md`.
- **Not done**: no change of the parallel test's `eps * length(a) * length(b)` to a squared comparison — the measured 2.71 → 2.68 ns is inside the noise (`G8`); no BVH or batched entry point introduced for "more thorough header-only", that is the ray–mesh traversal item of `../backlog/graphics.md`, argued separately with evidence.

### 2.9 The box transform operator travels with the box

- **What**: `Aabb<T>::transformed(const math::Matrix4x4<T>&)` returns the axis-aligned box that **contains** the box after placing it in another coordinate system (transform the eight corners and refit); it is defined for affine matrices only, and an empty box or a non-affine input answers an empty box. `Obb` does not offer an operator of the same name yet.
- **Why**: `G1`, `G7`, `G13` — this is the value type's own geometric fact, not rendering vocabulary. Leaving it on the consumer side means every consumer (world box, subtree union, bounds cache, picking rejection box) writes the same eight-corner refit; that is exactly how it was before the move (see `../decisions/ADR-2026-11-09-box-transform-lives-in-geom.md`).
- **Boundary**: the image of a box is generally not axis-aligned any more, so this operator gives a **bound** and not the image; a caller needing the exact image must use `Obb` (no consumer today, see 9.2). `T` must be a floating-point type: an integer box multiplied by a matrix has no well-defined rounding.
- **Cost**: `Aabb.hpp` now includes `math/Transform3.hpp` (the one definition of the matrix × point operator). `Aabb` already includes `Ray.hpp`, so this "value type depends on a math value type" coupling is not new.

## 3. Features

| # | Feature | Description |
|---|---|---|
| F1 | Axis-aligned box | `Aabb<T>`: min/max corners, the `empty()` sentinel, `expandBy` / `intersectWith` / `compute(span)` / `transformed(affine)`. |
| F2 | Oriented box | `Obb<T>`: centre + half extents + quaternion orientation; `toAabb()` returns an `Aabb<T>`. |
| F3 | Line | `LineCurve<T>`: point–direction parametric form; `intersectWith` gives the nearest pair and the parallel / collinear flags; `closestPoint` projects. |
| F4 | Plane | `Plane<T>`: point + normal; meets a `LineCurve` at a point and a `Plane` in a `LineCurve`. |
| F5 | One name | `Rect3` and `math::Aabb*` do not exist; `geom::spatial::Aabbf/Aabbd/Aabbi` are available. |
| F6 | 2D stays put | `math::Rect2` and `Rect2i` keep their place. |
| F7 | PCA fit | `Obb::computePca(span)`: the covariance eigen-axes, with the largest-variance direction as local X. |
| F8 | Fixed-orientation fit | `Obb::computeFixedOrientation(span, x_dir, y_dir)`: the caller supplies X/Y, and Y is first orthogonalised against X. |

### 3.1 F1 the axis-aligned box

- **Behaviour**: default construction is a zero-size box at the origin (`isValid()` true); `empty()` is the reversed sentinel, valid only after an `expandBy`.
- **Constraint** (C1): the fields change monotonically in their direction only, and `min()` / `max()` return the stored value as is; the output of `intersectWith` is always normalised.
- **Requirement** (R1): `compute()` accepts a vertex span in any axis order, and an empty input returns the default zero box.
- **Acceptance** (T1): `AabbTest.cpp` covers expandBy, intersection (including the gap box of a non-intersection), `compute`, merging with the empty sentinel, and `transformed` under identity / translation / non-uniform scale / rotation with refit / mirror / empty box and non-affine input.
- **Caveat** (N1): `isNeg()` / `isEmpty()` / `isValid()` are three questions about one decision and must not be given different meanings.

### 3.2 F4 the plane

- **Behaviour**: `intersectWith(LineCurve, Point3&)` returns false when `|n·d| <= eps` (the line is parallel to the plane); `intersectWith(Plane, LineCurve&)` returns false when the normals are collinear.
- **Constraint** (C2): the output line is supplied by the caller — `LineCurve` has no default constructor (`P1`).
- **Requirement** (R2): the intermediate solution of the intersection pivots on the largest component axis to avoid an ill-conditioned submatrix.
- **Acceptance** (T3): `LinePlaneTest.cpp` covers the point intersection, the parallel no-intersection, and the direction and position of the plane–plane line.

### 3.3 F7/F8 the fits

- **Behaviour**: both return a box that encloses every input point; the axis directions affect tightness only, never whether the points are enclosed (`C4`).
- **PCA**: centroid → covariance → `symmetricEigen` → take the axes by **descending** variance: local X is the largest-variance direction, local Z the smallest, and the third axis is the cross product. The half extents are measured in this order and **not sorted** (`N4`).
- **Fixed orientation**: `x_dir` is normalised; `y_dir` loses its projection on X and is then normalised; Z is the cross product.
- **Degenerate cases** (`N3`): a zero `x_dir` → world X; `y` parallel to `x` → the world axis least parallel to X (the caller's X is kept); all points sharing one coordinate → a zero-size box at that point with the identity orientation; an empty input → the default box (`R4`).
- **Acceptance** (T6/T7): `ObbFitTest.cpp` covers spectrum / axis recovery of a rotated box, a collinear point set, a single point and coincident points, an empty input, an illegal direction pair and the invariant "every point is enclosed".

## 4. Public Interface

| Header | Content |
|---|---|
| `src/base/geom/sdk/vine/geom/spatial/Aabb.hpp` | `Aabb<T>`, `Aabbf` / `Aabbd` / `Aabbi` |
| `src/base/geom/sdk/vine/geom/spatial/Obb.hpp` | `Obb<T>`, `Obbf` / `Obbd` |
| `src/base/geom/sdk/vine/geom/spatial/LineCurve.hpp` | `LineCurve<T>`, `LineCurvef` / `LineCurved` |
| `src/base/geom/sdk/vine/geom/spatial/Plane.hpp` | `Plane<T>`, `Planef` / `Planed` |
| `src/base/geom/src/Instantiations.cpp` | the explicit-instantiation list of the header-only geometry types (`float` + `double`) |

The short queries are defined in the headers (2.8); only the long algorithms stay in `src/` with the `VN_GEOMETRY_API` explicit instantiation: `LineCurve::intersectWith(const LineCurve&)`, `Plane::intersectWith(const Plane&, LineCurve&)`, `Obb::computeFixedOrientation` / `computePca`.

## 5. Constraints, Requirements and Invariants

- **C1**: the field-direction invariant of `Aabb` — min only decreases and max only increases; the output of `intersectWith` is normalised.
- **C2**: an output parameter is constructed by the caller; a function allocates nothing and returns no reference.
- **C3**: these four types take no part in `Shape`'s type decisions (`obj_cast` / `ShapeType`).
- **C4**: a fit result is always a bounding box: the axes affect tightness only, and under any input `contains()` is true for every input point (within tolerance).
- **C5**: `Aabb::transformed()` accepts affine matrices only (`Matrix4x4::isAffine()`); an empty box or a non-affine input answers an empty box, and the result is always normalised (min ≤ max).
- **R1**: `Aabb::compute()` returns the default zero box for an empty span and throws nothing.
- **R2**: `Plane` intersection returns `false` on degenerate input and produces no NaN.
- **R3**: `math` does not include `geometry`; `geometry` may include `math`.
- **R4**: both fit entry points return the default `Obb` (origin, zero size, identity orientation) for an empty span.
- **N1**: `empty()` is not "the empty set" but a reversed min/max sentinel, usable only as the start of an accumulation.
- **N2**: a `LineCurve` direction need not be unit length, and the scale of the parameter `t` is tied to `|direction|`.
- **N3**: on a collinear point set PCA's secondary axis is arbitrary but deterministic; repeated eigenvalues guarantee orthogonality only, not uniqueness; when all points coincide it returns a zero-size box with the identity orientation.
- **N4**: PCA's axis order is **variance order**, not **size order**: `halfExtents()` does not guarantee X ≥ Y ≥ Z (a box-shaped cloud agrees, a non-uniform cloud may not); a caller selecting by length sorts itself. The old VObb took col(0)/col(1) (X = smallest variance); this repository does the reverse, for the reason in 2.7.

## 6. Caveats and Traps

- **P1**: `LineCurve` has no default constructor — the output parameter of `Plane::intersectWith(Plane, LineCurve&)` must be an already constructed line (any legal value will do; the function overwrites it entirely). The failure mode is a compile error, not a run-time error.
- **P2**: a default-constructed `Aabb` is the **zero box**, not an empty one; using it as the start of an accumulation pulls the origin into the bounding box. Use `Aabb::empty()` for accumulation (`N1`).
- **P3**: `Aabb::compute()` and `expandBy()` do not normalise; the output of `intersectWith()` always does. Mixing the two states makes `isNeg()` read the opposite of what you expect.
- **P4**: `math::toQuaternion()` requires a pure rotation (orthonormal, det = +1); it neither checks nor repairs, so a scaling or degenerate matrix yields an arbitrary quaternion. `rotate3x3()` is its inverse and the two share the same branch convention.

## 7. Rejected Options

- **D1**: keeping `Rect3` as the type name — a 3D "Rect" is a wrong name and does not pair with `Obb` (`G1`/`G4`). See `../decisions/ADR-2026-11-03-geometry-primitives-ownership.md`.
- **D2**: keeping `using Rect3 = geom::spatial::Aabb` as a compatibility alias in `math/Rect3.hpp` — it would make `math` depend on `geometry` in reverse (`G2`), and a transition period removed one version later is not worth a back edge.
- **D3**: moving `Rect2` into `geometry` as well — its only consumer is a GUI layout rectangle, and it would force the public headers of `appfw` to depend on `geometry` (`G1`/`G5`).

## 8. Tests and Acceptance

| # | Criterion | Case |
|---|---|---|
| T1 | box expansion, intersection, `compute`, the empty sentinel | `tests/test_geom_spatial/AabbTest.cpp` (`Aabb.*`) |
| T2 | normalisation, containment and `toAabb` enclosure of the oriented box | `tests/test_geom_spatial/ObbTest.cpp` |
| T3 | line–line nearest points and the parallel / collinear flags | `LinePlaneTest.cpp` (`Line.*`) |
| T4 | plane–line and plane–plane intersection and the degenerate branches | `LinePlaneTest.cpp` (`Plane.*`) |
| T5 | the 2D rectangle is unaffected | `tests/test_math/Rect2Test.cpp` |
| T6 | spectrum, orthonormal basis, reconstruction and ordering | `tests/test_math/EigenDecompositionTest.cpp` (`EigenDecomposition.*`) |
| T7 | axis-angle → matrix → quaternion round trip | `tests/test_math/Transform3Test.cpp` (`Transform3.*`) |

## 9. Status and Evolution

### 9.1 Current state

The four types are in `vn::geometry`, and `math` holds no matching header or source; `Mesh`, `graphics::Node`, `ChainPresets`, `ChainBuilder` and the tests all use the new names.
`Obb` carries the PCA and fixed-orientation fits itself; eigen decomposition and matrix → quaternion are in `math` (`Matrix3x3.hpp` / `Transform3.cpp`), and the two headers declare the contract only.

### 9.2 Known gaps

- No transform operator on `Obb`: it stores centre + half extents + quaternion, which is exact only for rigid bodies (and uniform scaling) — the image under a general affine map is a parallelepiped this representation cannot hold. Trigger: a consumer needing the exact image; settle the semantics of a non-rigid transform first (reject / refit / store three half-axis vectors).
- `LineCurve` / `Plane` have cases for the basic branches only; degenerate tolerances and near-collinear cases are not covered.
- The minimum-volume OBB and `merge` are not provided (2.7).

## 10. References

- Code: `src/base/geom/sdk/vine/geom/spatial/{Aabb,Obb,Line,Plane}.hpp`, `src/base/geom/src/{Aabb,Obb,Line,Plane}.cpp`.
- Mathematical facilities: `src/base/math/sdk/vine/math/EigenDecomposition.hpp` (`symmetricEigen`), `Transform3.hpp` (`toQuaternion`), `src/base/math/src/Transform3.cpp`.
- Design: `graphics-geometry.md` (the current design of `Shape` / `Mesh` / revision), `../decisions/ADR-2026-11-03-geometry-primitives-ownership.md`.
- Tests: `tests/test_geom_spatial/AabbTest.cpp`, `ObbTest.cpp`, `ObbFitTest.cpp`, `LinePlaneTest.cpp`; `tests/test_math/Rect2Test.cpp`, `EigenDecompositionTest.cpp`, `Transform3Test.cpp`.
