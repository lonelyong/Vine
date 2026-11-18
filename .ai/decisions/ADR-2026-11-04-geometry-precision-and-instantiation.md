# ADR-2026-11-04: a geometry type's precision stays a two-option parameter, and both precisions are explicitly instantiated

> **Status**: Accepted
> **Date**: 2026-11-04
> **Scope**: `src/base/geometry/sdk/vine/geometry` (every value type and the `T` of the curve and surface families), `src/base/geometry/src/Instantiations.cpp` (new), the explicit instantiations in `LineCurve.cpp` / `Obb.cpp` / `Plane.cpp`, `.ai/design/geometry-primitives.md` 2.8 <!-- drift-ok -->

## Background

Every geometry value type is parameterised by `math::FP T` (`Aabb<T>`, `Ray<T>`, `Plane<T>`, `Curve<T>`, `Surface<T>`, ...). The real consumers (`graphics`, `meshio`, `robotics`, `brep`) use only `double`, which raised two questions:

1. the template parameter looked like "a degree of freedom with zero consumers", and it was nearly removed, with `float` treated as a promise to be narrowed;
2. only `LineCurve` / `Obb` / `Plane` were explicitly instantiated, because some of their members are defined in a `.cpp`; every other header-only type had **no symbol in the library at all** - each consumer instantiated its own copy, and nobody could say what the library "provides".

## Decision

1. **Keep `T`.** Geometry value types stay parameterised by precision.
2. **Both precisions ship with the library**: `float` and `double` are each explicitly instantiated once and exported, and the `*f` / `*d` aliases are kept.
3. **The instantiations are gathered and complete**: the list for header-only types lives in `src/base/geometry/src/Instantiations.cpp` (two lines per type); `LineCurve` / `Obb` / `Plane`, which have a `src/` implementation, keep instantiating in their own `.cpp`, because an explicit instantiation can only instantiate the member definitions visible in that translation unit. <!-- drift-ok -->
4. **No `extern template`**: inlining still happens in the caller, and the exported symbol is only the fallback for when it cannot.

## Rationale

- **Precision is an industry variable, not an implementation detail** (`G9`, `G8`). `float` geometry is the right answer in real-time and games (Unity and PhysX are float-only, Bullet uses `btScalar` with `BT_USE_DOUBLE_PRECISION` as a switch), in mobile and AR/VR (some GPUs have no double hardware), in GPU and hardware ray tracing (BVHs and compute are all float32) and in point clouds and LiDAR (PCD and PLY are float32, and a hundred million points is the difference between fitting in memory and not); `double` is mandatory in CAD/CAM, metrology and motion planning. The framework cannot pick a side for future customers, and a parameterised scalar is the cheapest way not to - which is also what Eigen, GLM, CGAL and PCL do (this repository already depends on Eigen).
- **What is not instantiated generates no code**: the intrinsic cost of `T` is zero; the real cost is the API surface and the documentation, which "`*d` / `*f` aliases plus one policy page" answers (`G4`).
- **Shipping both precisions** is what makes "double precision" a fact rather than a claim (`G4`): a consumer need not know where the member definitions live and need not instantiate anything; a missing symbol becomes a link error instead of each side silently generating its own copy.
- **A gathered list** turns "which geometry types does the library provide" into a reviewable list and turns that translation unit into a compile-time check that every type can be instantiated (`G11`).
- **No `extern template`** (`G10`): the measured gain on the picking loop (`geometry-ray.md` 2.7: 7.09 -> 5.99 ns for a hit, 3.96 -> 2.71 ns for parallel rejection) comes from inlining the short queries into the caller, and `dllimport` would erase it. Exporting is the fallback, not the replacement.

## Rejected options

| Option | Why not |
|---|---|
| Pin the family to `double` and drop the template | Declares that "this framework serves precise geometry only" and shuts out real-time, mobile, GPU and point-cloud work, while an uninstantiated template costs nothing (`G9`) |
| Ship only `double` and delete the `*f` aliases and the float instantiation | Downgrades "float is supported" from a fact to "instantiate it yourself": each consumer generates its own implementation, and neither the ABI nor the behaviour is defined (`G4`) |
| Instantiate only the three types with a `.cpp` (today's shape) | A header-only type has no symbol in the library, so "what the library provides" depends on who compiled it (`G11`) |
| Add `extern template` and make consumers depend on the DLL alone | It kills the inlining of the short queries and gives back the measured gain (`G10`) |
| Switch the global precision with one `#define` (the Bullet shape) | One binary can hold only one precision, and switching changes the layout of every compiled target; two instantiations coexisting is cheaper |

## Consequences

- The symbol table of `vnGeometry` contains the `float` and `double` instances of every geometry type (verifiable with `dumpbin /exports`, e.g. `?$EllipseCurve@M@...` and `?$EllipseCurve@N@...`).
- The four types with `std::vector` / `std::shared_ptr` members (`TrimmedCurve`, `OffsetCurve`, `BezierCurve`, `BSplineCurve`) produce `C4251` at their exports (an STL member without a dll-interface). That is the existing limitation of exporting STL members; it is disabled locally in `Instantiations.cpp` with a note, and it is not a defect.
- The fixed cost of a new geometry type: two lines in `Instantiations.cpp` (or two lines in its own `.cpp` when it has an implementation).
- The precision policy is explicit: the official API of analytic geometry is `double`, and `float` is an equally supported instance; precisions are never mixed (`requires is_same_v<T, U>` in `Aabb::intersectWith` is the mechanical form of that rule).
