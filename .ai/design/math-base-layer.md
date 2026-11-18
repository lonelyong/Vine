# the math module: the base layer and its mathematical vocabulary

> **Status**: current
> **Last verified**: 2026-11-06
> **Scope**: `src/base/math` — the value types (vector, point, matrix, quaternion, isometry, rectangle), the mathematical constants and predicates, the concepts and tags, and the 3×3 symmetric eigen-decomposition. **No procedure at all** (root finding, integration and spline evaluation live in `geom/detail`), and no geometric vocabulary or representation convention.
> **Sources**: the 18 headers under `src/base/math/sdk/vine/math/`; the 14 explicit-instantiation units in `src/base/math/src/`; `tests/test_math/` (7 case files, 36 cases). The decision is `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.
> **Backlog**: none

## 1. Overview

### 1.1 Problem and background

`math` is the only layer in the repository that **every module depends on**: graphics needs `Vec3d` / `Mat4d` / `Point3d`, robotics needs `Vec3d` / `Quatd`, geometry needs `Point2` / `Rect2`, the model and loaders need `Vec3f`, and core needs `Vec3f` as the vertex element type.

Root finding, integration, the B-spline knot model and the continuity ladder live in the geometry module (`geom/detail/` and `geom/Continuity.hpp`): **`math` holds mathematical vocabulary only and has no subdirectory at all.** The two reasons differ — the knot model and `Continuity` because “the signature carries our own data model”, root finding / integration / `deCasteljau` because “today's only consumer is geometry”, and the consequences plus the reversal condition are written in `ADR-2026-11-06-math-keeps-only-vocabulary.md`.

### 1.2 Goals

1. Any module can use the same value types, and **only depends downward** (`G2`).
2. “Mathematical vocabulary” and “numeric procedure” are two layers, the criterion is executable, and a new name put in the wrong place is visible at a glance (`G1`).
3. Precision (`float` / `double`) and storage order (column-major / row-major) are type parameters rather than runtime flags, at zero cost (`G10`).

### 1.3 Non-goals

- Not a general numeric library: no linear solver, sparse matrix, complex field, optimiser, integrator or root finder. The last two live in `geom/detail`.
- Not geometry: curves, surfaces, bounding boxes and representation conventions are not here.
- No unit system, no interval arithmetic, no arbitrary precision.

### 1.4 Conclusion

**`math` is the base layer and its content is “mathematical vocabulary” only: value types, constants and predicates, concepts and tags, plus one eigen-decomposition with an internally fixed budget. The criterion is “does it take a caller-supplied precision target” — if it does, it is not vocabulary and belongs in `geom/detail`.**

## 2. Design Points

### 2.1 The base layer: it depends on `global` and `core` only, and any module may depend on it

- **What it is**: `vn::Math` is a SHARED library (`src/` holds the instantiation units → defines `VN_MATH_LIB`, entities marked `VN_MATH_API`) that links `vn::Global` `PUBLIC` only. Release turns LTO on; under Clang the inline-hint optimisations are switched on as well.
- **Why**: `G2` — every upper module (graphics, geometry, robotics, models, loaders) needs it, so it cannot depend back on any of them; one back edge would turn “anyone can use it” into “everyone drags it in”.
- **Boundary**: `math` knows nothing about curves, rendering, IO or a CAD representation convention. A conversion that needs curve speed (geometric tolerance → parameter tolerance) therefore stays on the geometry side.

### 2.2 Vocabulary only, no procedure

| Category | Criterion | Content |
|---|---|---|
| mathematical **vocabulary** | a number in, a number out; no notion of “converge to some precision” | `Vector2/3/4` , `VectorN`, `Point2/3`, `Matrix3x3/4x4`, `Quaternion`, `Isometry2/3`, `Rect2`, the constants and predicates of `Math.hpp`, the concepts and tags of `Types.hpp`, `EigenDecomposition.hpp` |

There is no second category in `math`. The numeric procedures (`adaptiveSimpson` / `rootInBracket` / `parameterReaching`) and spline evaluation (`deCasteljau` / `differences`) live in `geom/detail`, and their public entries are geometric names such as `geom::planar::arcLength` / `geom::spatial::arcLength`.

- **The criterion has to be able to say no**: a `tolerance` in a signature does not make a procedure — `Vector3::isEqual(b, eps)`'s `eps` is a **comparison tolerance** and the answer is immediate; a procedure means “I try to approach the target you gave and may fail”.
- **Why not keep a “general numeric” layer**: three functions are not a module (`G8`), and their only consumer today is geometry. The cost is explicit: a non-geometric module needing integration or root finding must depend on `vn::Geom`, copy it, or promote the kernel into its own module — the last is the reversal path in `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.

### 2.3 Why `math` holds no geometric vocabulary and no `detail`

- **Geometric vocabulary belongs to geometry**: the B-spline knot model (`knotSpan` / `isClampedKnotVector` / `knotContinuity` / `basisValue`) carries our own data model in its signatures, and `Continuity` appears in the pure-virtual signatures of `Curve<T>` / `Surface<T>`, whose only consumer is geometry. They live in `geom/detail/BSplineBasis.hpp` and `geom/Continuity.hpp` (`G1`, `G2`). The criterion is **“does the signature carry our own data model”**.
- **`math` has no `detail`**: `detail` answers “is this a public interface”, and every header in `math` is public vocabulary — there is neither a “non-public shared implementation” nor a second content category to tell apart. The trigger for creating `math/detail` is “two or more `math` headers need the same implementation that must not be exposed”, and there is none (`G8`).
- **No sub-namespace is left either**: `numeric` and `spline` left with their content, and `math_global.hpp` provides the root-level namespace macro only.

### 2.4 The precision strategy: a template + explicit instantiation + aliases

- **What it is**: every value type is `template <typename T>` (a floating-point one is further constrained by `vn::math::FP`); `src/*.cpp` instantiates explicitly as `template class VN_MATH_API X<float>;`, and `Vector2/3/4`, `Point2/3` are instantiated for integer types too (an integer vector is a real need for indices / pixel coordinates — `Aabbi` is one).
- **Why**: `G10` — defining in the header and instantiating explicitly where it leaves the DLL lets the arithmetic inline while keeping one copy of each symbol; precision as a type parameter means no runtime branch and no “the float version was not kept in step”.
- **Boundary**: `T` is not arbitrary. Integer instances exist for vectors and points only, not for matrices and quaternions (their division and square root are meaningless on integers); `VectorN` accepts floating-point and integer types (including `bool`).
- **Alias rule**: `<Noun><dim><scalar>` — `Vec2f` / `Vec3d` / `Vec3ui32` / `Point3d` / `Point2ui16` / `Mat3f` / `Mat4d` / `Quatf` / `Quatd` / `Isometry3d` / `Rect2i` / `Rect2d`. Note `Vector2`'s alias is `Vec2*`, and there is **no** spelling such as `Vector2d`.

### 2.5 Storage order is a type parameter, not a runtime switch

- **What it is**: `Matrix3x3<T, Order>` / `Matrix4x4<T, Order>` default `Order` to `ColMajor`, the other choice being `RowMajor`; the two empty structs in `Types.hpp` are those tags. A matrix acts on a vector under the **column-vector** convention (`y = M * x`) with the translation in column 4; the geometric transforms (translation, rotation, scale) are **free functions** in `Transform3.hpp`, not members.
- **Why**: `G4`, `G10` — the layout decides how `rawData()` is read, and making it a runtime flag would add a branch to every multiplication; as a type parameter both layouts inline, and they cannot be silently interchanged (a `ColMajor` and a `RowMajor` matrix of the same `T` are different types).
- **Boundary**: `Order` affects storage and how a raw-data construction is read, never the mathematical meaning; when exchanging data with a third-party library the caller must check that library's order at the call site (`loaders` maps assimp's column-major matrix directly, without transposing).

### 2.6 `symmetricEigen` is the only iteration in the vocabulary, and it still counts as vocabulary

- **What it is**: `symmetricEigen(matrix, eigenvalues, eigenvectors?)` computes the eigen-decomposition of a 3×3 real symmetric matrix with cyclic Jacobi: eigenvalues ascending, eigenvectors in the matching columns, each eigenvector's largest component made positive to remove the sign ambiguity, only the upper triangle read; the sweep budget is fixed and a `bool` reports whether it converged.
- **Why it stays here**: the criterion is **who supplies the precision target**. Here the caller supplies none: the budget is fixed internally and the relative tolerance is `4 * EPS<T>()`. So it is not “I try to approach the target you gave” but “I compute to a fixed cost and tell you how it went” (`G4`). It **can** answer “it did not reach it” (`false`), but that only shows that the first half of the criterion is the decisive one.
- **Boundary**: it does not fix the orientation of the eigenbasis (a caller building a rotation matrix crosses them for the third axis); with a repeated eigenvalue the basis is not unique.

### 2.7 Degenerate input gives a checkable value, never an exception

- **What it is**: `isZero` / `isEqual` handle `NaN` and `inf` explicitly (with `EPS` = `inf` every finite value counts as “zero / equal”, with `NaN` they degrade to an exact comparison); `EPS<T>()` is specialised for `float` / `double` and answers `0` for any other `T`.
- **Why**: `G11`, `G4` — `math` sits at the very bottom, and throwing would turn “a predicate” into control flow; returning `NaN` would let an error spread silently until the caller's far end notices it. `isZero(NaN, eps) == false` is deliberate: a `NaN` is not zero.
- **Boundary**: `EPS<T>()` answering 0 for an integer means integer “zero” is an exact comparison — which is right, because integers do not round.

## 3. Features

| # | Feature | Content | Status |
|---|---|---|---|
| F1 | value types | vector, point, matrix, quaternion, isometry, rectangle, N-dimensional vector | landed |
| F2 | constants and predicates | π and the natural-logarithm constants, `EPS<T>()`, angle normalisation, tolerance comparison, integer-safe multiply-add and square root | landed |
| F3 | concepts and tags | `FP` / `Real` / `Arithmetic` / `Integer` / `Integral` / `TypeF`, `ColMajor` / `RowMajor` | landed |
| F4 | eigen-decomposition | 3×3 real symmetric cyclic Jacobi | landed |

### 3.1 F1 value types

- **Behaviour**: `Vector2/3/4`, `Point2/3`, `Matrix3x3/4x4`, `Quaternion`, `Isometry2/3`, `Rect2`, `VectorN` are `constexpr`-friendly value types; `Isometry3` is a rigid transform of a translation plus a quaternion rotation, and `Transform3.hpp` provides `translate` / rotation / scale free functions that return a matrix.
- **Constraints** (C2, C3): a floating-point type is constrained by `FP`; a matrix's storage order is a type parameter with the column-vector convention; a geometric transform is a free function rather than a member (`G1`: a matrix does not know the purpose “transform”).
- **Acceptance** (T1): `VectorTest`, `Matrix3x3Test`, `Matrix4x4Test`, `QuaternionTest`, `Rect2Test`, `Transform3Test`.

### 3.2 F2 constants and predicates

- **Behaviour**: `PI` / `PI_TWO` / `PI_HALF` / `PI_QUARTER`, `E` / `LOG2E` / `LOG10E` / `LN2` / `LN10`, `RAD_TO_DEG` / `DEG_TO_RAD`, `EPS<T>()`, `isZero` / `isEqual`, `normalizeAngle` (`[-π, π]`) / `normalizeAnglePositive` (`[0, 2π)`), `safeMultiply` / `safeLength` / `safeLengthSquared` (integers promoted to `double` first), and `arithmeticAdd/Sub/Multiply/Division/Nagate` (`bool` interpreted as a logical operation).
- **Constraints** (C4): `isZero` / `isEqual` must handle `NaN` / `inf` explicitly and must never let them pass silently.
- **Acceptance** (T2): covered together by the predicate cases in `VectorTest` / `Rect2Test` and the `isValid()` cases on the geometry side.

### 3.3 F4 eigen-decomposition

- **Behaviour**: see 2.6.
- **Acceptance** (T4): `EigenDecompositionTest`.

## 4. Public Interface

| Header | Public content |
|---|---|
| `Vector2.hpp` / `Vector3.hpp` / `Vector4.hpp` / `VectorN.hpp` | `Vector2/3/4<T>`, `VectorN<T, N>`, the `Vec2f` / `Vec3d` / `Vec3ui32` aliases |
| `Point2.hpp` / `Point3.hpp` | `Point2/3<T>`, the `Point2d` / `Point3f` / `Point2ui16` aliases |
| `Matrix3x3.hpp` / `Matrix4x4.hpp` | `Matrix3x3<T, Order>` / `Matrix4x4<T, Order>`、`Mat3f` / `Mat4d` |
| `Quaternion.hpp` / `Isometry2.hpp` / `Isometry3.hpp` | `Quaternion<T>`、`Isometry2/3<T>`、`Quatf` / `Quatd` / `Isometry3d` |
| `Rect2.hpp` / `Transform2.hpp` / `Transform3.hpp` | `Rect2<T>` (`Rect2i/f/d`) and the transform constructors (returning a matrix) |
| `Math.hpp` | constants, `EPS<T>()`, predicates, integer-safe arithmetic, angle normalisation |
| `Types.hpp` | concepts (`FP` / `Real` / `Arithmetic` / …), `TypeF<T>`, `ColMajor` / `RowMajor` |
| `EigenDecomposition.hpp` | `symmetricEigen()` |
| `math_global.hpp` | `VN_MATH_API`、`VN_MATH_NS_BEGIN` / `VN_MATH_NS_END` |

## 5. Constraints, Requirements and Invariants

- **C1**: the dependency direction is “upper layer → `math`” only. A public `math` header includes this module's headers and the standard library, and no other `vine` module.
- **C2**: precision is a template parameter; a floating-point type is constrained by `FP`; integer instances cover vectors and points only.
- **C3**: a matrix's storage order is a type parameter defaulting to `ColMajor`, with the column-vector convention `y = M * x`; a geometric transform is a free function.
- **C4**: a degenerate or non-finite input gets a checkable value (`NaN`-aware predicates, `bool`) — no exception and no silent `NaN`.
- **C5**: `math` holds vocabulary only. A function with a caller-supplied precision target, or any function carrying our own data model, is not here.
- **C6**: `math_global.hpp` provides the root-level namespace macro only; the module has no sub-namespace.
- **C7**: a public alias is `<Noun><dim><scalar>`; `Vector2`'s alias is `Vec2*`.
- **R1**: a value type allocates no memory by default (`VectorN` is a fixed-size array).
- **N1**: `EPS<T>()` is specialised for `float` / `double` only and answers `0` for any other `T`.
- **N2**: `normalizeAngle` lands in `[-π, π]` and `normalizeAnglePositive` in `[0, 2π)`.
- **N3**: `symmetricEigen` orders eigenvalues ascending, reads the upper triangle only and normalises eigenvector signs; `false` means “did not converge within the fixed budget” and the returned basis is an approximation.

## 6. Caveats and Traps

- **P1 a predicate's answer for `NaN` is not the intuitive one**: `isZero(NaN, eps)` is `false`; with `eps` = `inf` every finite value counts as zero / equal. A caller that wants “`NaN` counts as invalid” uses an explicit criterion such as `isValid()` and does not expect it from a predicate.
- **P2 do not guess an alias**: `Vector2`'s floating-point aliases are `Vec2f` / `Vec2d`, and there is **no** `Vector2d`; the same holds for `Vector3`. A wrong spelling is a compile error, but easy to miss in a cross-module review.
- **P3 `Point3::length()` does not exist**: a distance between two points is `(a - b).length()`, and a single point has no such thing as a “length”.
- **P4 `Isometry2` / `Isometry3` take `float` / `double` only** for `T`: an integer rigid transform is meaningless, and the template constraint blocks it in the class definition; vectors and points do accept integers.
- **P5 do not look in `math` for integration, root finding or spline evaluation**: they are in `geom/detail` (they become their own module only when a third non-geometric consumer appears — see `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`).

## 7. Rejected Options

- **D1** keep a general numeric layer (`math/numeric` or a standalone `vn::Numeric` module): `G8`, `G5` — the only consumer today is geometry, so a layer for three functions is an empty shell; the trigger for a standalone module is written in the ADR (a second consumer), and splitting then is cheaper than pre-building now.
- **D2** promote `spline/` into a standalone `vn::Nurbs` module: `G8` — with one “representation convention” family it would be an empty shell, and that family has gone to the geometry side with `geom/detail`.
- **D3** let `math` go on hosting the geometric vocabulary and the representation conventions: `G1`, `G2` - `math` would have to defend its name with a document (one page did exactly that; git holds it), while its consumers are geometry only. Overturned; see `../decisions/ADR-2026-11-06-spline-model-moves-to-geom.md` and `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.
- **D4** add a `detail` sub-tree to `math`: `G8` — there is no “non-public shared implementation” to put in it, see 2.3.
- **D5** split further by type family (`vector/`, `matrix/`, `transform/`): `G8`, `G4` — the 18 root-level headers can already be found by name, and one more level would scatter types that must be used together (`Point3` / `Vector3` / `Matrix3x3`) across three paths.

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
|---|---|---|
| T1 | the arithmetic, comparison and transform semantics of the value types | `test_math`: `VectorTest`, `Matrix3x3Test`, `Matrix4x4Test`, `QuaternionTest`, `Rect2Test`, `Transform3Test` |
| T2 | the edges of the constants and predicates (`NaN` / `inf` / a direction) | same, plus the `isValid()` cases on the geometry side |
| T4 | eigenvalues ascending, eigenvectors orthogonal with normalised signs, a repeated eigenvalue | `test_math`: `EigenDecompositionTest` |
| T5 | the full suite | `build\bin\Debug\test_math.exe --gtest_brief=1` (7 suites, 36 cases) |

The integration, root-finding and spline-evaluation cases live in `test_geom_spatial` (`QuadratureTest`, `RootFindingTest`, `BSplineBasisTest`) together with their implementation.

## 9. Status and Evolution

- **Current state**: F1–F4 are all in place; all 36 cases in `test_math` are green. Modules that depend on it: graphics, geometry, robotics, models, loaders, core (`Vec3f` as the vertex element type).
- **Boundary**: `Continuity` and the B-spline knot model are in `geom`; root finding / integration / Bernstein evaluation are in `geom/detail`; `math` holds vocabulary only and no BOM-only placeholder header. <!-- drift-ok -->
- **Known deviation**: the root-level mathematical constants keep all-capital names such as `PI` / `PI_TWO` / `EPSF`, which does not match the repository's “a constant uses the `k` prefix” rule (a legacy, not renamed: they appear at a great many call sites and the rename buys less than it risks). New constants follow the `k` rule.
- **Backlog**: none. If a non-geometric consumer of root finding / integration / Bernstein evaluation appears, promote it into its own module along the reversal path of `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`.

## 10. References

- Code: `src/base/math/sdk/vine/math/` (18 headers, no subdirectory), `src/base/math/src/` (14 instantiation units), `tests/test_math/` (7 case files, 36 cases).
- Decisions: `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`, `../decisions/ADR-2026-11-06-spline-model-moves-to-geom.md`, `../decisions/ADR-2026-11-06-math-is-the-base-layer.md` (superseded), `../decisions/ADR-2026-11-06-shared-numeric-kernels-in-math.md` (superseded).
- Related design: `geometry-module.md` (the geometry side's directories, naming and shared sub-tree), `geometry-intersection.md` 2.7 (why the root finder sits on the geometry side), `geometry-primitives.md` 2.7 (why `symmetricEigen` does not go into geometry).
- Postmortem: `../bugs/spline-knotspan-hang.md` (`knotSpan` looping forever; the header lives in geometry now).
