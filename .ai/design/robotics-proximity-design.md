# the Robotics Proximity module: the interface layer

> **Status**: current
> **Last verified**: 2026-10-23
> **Scope**: the interface layer of collision geometry / collision object / collision matrix / detector / geometry manager; the FCL implementation is out of scope.
> **Sources**: `src/robotics/core/sdk/vine/robotics/proximity/**`, `tests/test_proximity`.
> **Backlog**: `../backlog/`

## 1. Overview

### 1.1 Problem and background

`vn::robotics` needs the base interfaces of collision detection, but it should not push FCL's types and lifetime into the core module. VMR's proximity
interface is a mature reference; Vine needs the same names and the same shape, with the dependency on `workcell` removed.

### 1.2 Goals

- The interfaces of collision geometry / collision object / collision matrix / collision detector / geometry manager.
- The common logic that is not FCL's (matrix, pairing, result, bookkeeping) implemented directly.
- The FCL backend (geometry conversion, broadphase, narrowphase) left to a separate module.

### 1.3 Non-goals

- Not implementing FCL; not linking FCL in this module.
- Not depending on `workcell`; an owner is only `INamed`.
- Not managing the owner / frame lifetime (the caller guarantees it).

### 1.4 One-paragraph conclusion

`proximity` is a header-only interface layer: an owner is `INamed`, a frame is `kinematics::Frame`, geometry and objects are `intrusive_ptr`;
FCL joins only through pure-virtual hooks, and no FCL type appears in the core module.

## 2. Design Points

### 2.1 An owner model that does not depend on workcell

- The owner type is `const vn::INamed*` (reads `name()` only): `kinematics::Frame` and a future `workcell::SceneObject`
  both implement `INamed`, and the matrix and the detector group by owner pointer.
- The frame association is `raw_ptr<const kinematics::Frame>`; the default world pose:
  `world = frame ? Frame::frameInWorld(frame, state) * local : local;`.

### 2.2 FCL is interfaces only

| Class | Pure-virtual hooks | Notes |
| --- | --- | --- |
| `CollisionGeometry` | `buildFromShape` / `isValid` | the backend turns a `Shape` into an FCL geometry |
| `CollisionObject` | `isValid`; `computeWorldTransform` may be overridden | the backend keeps the FCL internal transform in step |
| `CollisionDetector` | `doAdd/doRemove/doRebuild/doEndUpdate`, `doCheckCollision ×3` | the backend implements broadphase / narrowphase |
| `CollisionGeometryManager` | `createCollisionGeometry` / `createCollisionObject` | the backend creates the FCL geometry / object |

`CollisionMatrix` is a **complete implementation** (not FCL); `CollisionRequest::collision_matrix` filters the object pairs.

### 2.3 Reference counting and export

- Geometry and objects use `vn::RefCounted<Derived>` + `vn::intrusive_ptr`.
- **A header-only interface cannot carry `VN_ROBOTICS_CORE_API`**: it would be a dllimport, and no .cpp in RoboticsCore includes these headers.
- `RefCounted<Derived>`'s destructor is protected and non-virtual → an interface class needs its own `virtual ~X() = default;` to be deleted polymorphically.

### 2.4 Thread-safety semantics (same as VMR)

- The detector is not thread-safe: registration / removal / pose updates exclude each other; `checkCollision` calls may run concurrently with each other, but not with registration or update.
- No reference count is added on the owner / frame; the caller guarantees the lifetime.

## 3. Features

### 3.1 Feature index

| # | Feature | Behaviour | Status |
| --- | --- | --- | --- |
| F1 | collision geometry | `CollisionGeometry` abstraction + `buildFromShape` / `isValid` | interface landed |
| F2 | collision object | `CollisionObject`: geometry + local pose + owner/frame | interface landed |
| F3 | matrix / pairing / result | `CollisionMatrix` / `CollisionPair` / `CollisionResult` | complete implementation |
| F4 | detector | `CollisionDetector` bookkeeping + `do*` hooks | interface landed |
| F5 | geometry manager | `CollisionGeometryManager` registry + factory hooks | interface landed |

### 3.1.1 F1 collision geometry

- **Behaviour**: `CollisionGeometry` holds an `intrusive_ptr<const models::Shape>`; `buildFromShape` lets the backend turn the shape into
  an FCL geometry, `isValid` reports whether it is usable.
- **Constraints** (C1): no FCL type appears in the core module; the `Shape` lifetime is the caller's guarantee.
- **Requirements** (R1): geometry uses `RefCounted` + `intrusive_ptr` and can be deleted polymorphically.
- **Acceptance** (T1): `MockGeometry` in `tests/test_proximity` compiles / derives.
- **Caveats** (N1): a header-only interface does not take an export macro.

### 3.1.2 F2 collision object

- **Behaviour**: `CollisionObject` = geometry + local pose + owner/frame; the default `computeWorldTransform` uses
  `Frame::frameInWorld(frame, state) * local`.
- **Constraints** (C2): the owner is `const INamed*`; the frame is `raw_ptr<const kinematics::Frame>` and adds no reference count.
- **Requirements** (R2): the backend may override `computeWorldTransform` to keep the FCL internal transform in step.
- **Acceptance** (T2): the world-pose cases.
- **Caveats** (N2): the caller must keep the owner / frame alive longer than the collision object.

### 3.1.3 F3 matrix / pairing / result

- **Behaviour**: `CollisionMatrix` is a complete N×N implementation; `CollisionPair` is an unordered owner pair + hasher;
  `CollisionResult` is a snapshot of the results grouped by owner pair.
- **Constraints** (C3): the `minDistance` sentinels: `a==b → -1.0`, unregistered `→ -2.0`;
  `shouldCheckCollision(a,b) = a != b && !isIgnored(a,b)`.
- **Requirements** (R3): an unregistered owner does not count as ignored → returns true.
- **Acceptance** (T3): the matrix / pairing / result cases.
- **Caveats** (N3): `CollisionResult`'s `pairs` are grouped by unordered owner pair.

### 3.1.4 F4 detector

- **Behaviour**: `CollisionDetector`'s bookkeeping (`beginUpdate/endUpdate`, `addObject/removeObject`) is implemented;
  broadphase / narrowphase come from the `do*` hooks.
- **Constraints** (C4): `beginUpdate/endUpdate` count in pairs and the count reaching zero triggers `doEndUpdate()`;
  `addObject` is a no-op for an already registered owner.
- **Requirements** (R4): only a collision object whose `doAddCollisionObject` returns true is accepted.
- **Acceptance** (T4): the detector bookkeeping cases.
- **Caveats** (N4): the detector is not thread-safe; registration / update cannot run concurrently with a query.

### 3.1.5 F5 geometry manager

- **Behaviour**: `CollisionGeometryManager`'s `add/get/remove/update/clear` is implemented;
  `createCollisionGeometry` / `createCollisionObject` are the factory hooks.
- **Constraints** (C5): the core module does not create FCL objects; the backend owns the factory implementation.
- **Requirements** (R5): VMR's weak_ptr lazy cleanup is dropped (Vine has no weak_ptr); the shape lifetime is the caller's.
- **Acceptance** (T5): the manager registry cases.
- **Caveats** (N5): `createCollisionGeometry` is protected, `createCollisionObject` is public.

## 4. Public Interface

| Class | Contract | Notes |
| --- | --- | --- |
| `ProximityCore.hpp` | `ProximityScalar = double` | — |
| `CollisionGeometry` | `buildFromShape` / `isValid` | implemented by the backend |
| `CollisionObject` | geometry + pose + owner/frame; `computeWorldTransform` | may be overridden |
| `CollisionContact` | contact point | — |
| `CollisionPair` | unordered owner pair + hasher | — |
| `CollisionRequest` | query options + `collision_matrix` | — |
| `CollisionResult` | results grouped by owner pair | — |
| `CollisionMatrix` | N×N matrix | complete implementation |
| `CollisionDetector` | bookkeeping + `do*` hooks | implemented by the backend |
| `CollisionGeometryManager` | registry + factory hooks | implemented by the backend |

## 5. Constraints, Requirements and Invariants

### 5.1 Constraints (C#)

| # | Constraint |
| --- | --- |
| C1 | no FCL type appears in the core module; the `Shape` lifetime is the caller's guarantee. |
| C2 | the owner is `const INamed*`; the frame is borrowed and adds no reference count. |
| C3 | the matrix sentinels are `-1.0` / `-2.0`; the `shouldCheckCollision` rule is fixed. |
| C4 | the detector bookkeeping counts in pairs and the count reaching zero triggers `doEndUpdate()`. |
| C5 | the core module does not create FCL objects; the factory hooks belong to the backend. |

### 5.2 Requirements (R#)

| # | Requirement |
| --- | --- |
| R1 | geometry and objects use `RefCounted` + `intrusive_ptr` and can be deleted polymorphically. |
| R2 | the backend may override `computeWorldTransform` to keep the FCL transform in step. |
| R3 | an unregistered owner does not count as ignored. |
| R4 | the detector accepts only an object whose `doAddCollisionObject` returns true. |
| R5 | VMR's weak_ptr lazy cleanup is dropped; the shape lifetime is the caller's. |

### 5.3 Invariants (N#)

| # | Invariant |
| --- | --- |
| N1 | a header-only interface does not take `VN_ROBOTICS_CORE_API`. |
| N2 | an interface class carries `virtual ~X() = default;`. |
| N3 | the detector is not thread-safe; a query cannot run concurrently with registration / update. |
| N4 | no dependency on `workcell`. |

## 6. Caveats and Traps

### 6.1 Signatures a mock has to cover

- How it fails: `kinematics::State` is not fully qualified, the pure virtual does not match, and the mock stays abstract.
- How to detect it: the compiler reports `cannot instantiate abstract class`.
- How to prevent it: write `vn::robotics::kinematics::State`; an override of a public virtual keeps the same access level.

### 6.2 Polymorphic deletion

- How it fails: `RefCounted<Derived>`'s destructor is protected and non-virtual, so deleting through a base pointer skips it.
- How to detect it: ASan, or the derived destructor never runs.
- How to prevent it: the interface class carries `virtual ~X() = default;`.

## 7. Rejected Options

| # | Option | Why not | ADR |
| --- | --- | --- | --- |
| D1 | depend on `workcell` | the core module should be reusable by kinematics and by a scene without workcell. | — |
| D2 | implement FCL in this module | it would bring FCL types and build dependencies into the core. | — |
| D3 | give the header-only interface an export macro | it would be a dllimport, and RoboticsCore has no matching .cpp. | — |
| D4 | keep VMR's weak_ptr lazy cleanup | Vine has no weak_ptr; the shape lifetime is the caller's. | — |

## 8. Tests and Acceptance

| # | Criterion | Test case / command |
| --- | --- | --- |
| T1 | the interface compiles and can be derived | the mock subclass in `tests/test_proximity/ProximityTest.cpp` |
| T2 | matrix / pairing / result / world pose | same |
| T3 | detector bookkeeping | same |
| T4 | manager registry | same |

## 9. Status and Evolution

### 9.1 Current Status

The interface layer is implemented (headers only, no FCL); 9 tests cover the mock derivation and the non-FCL logic.

### 9.2 Known Gaps and Backlog

The FCL backend lives in a separate module (for example `robotics/proximity_fcl`, depending on `RoboticsIO`/`RoboticsCore` + FCL):

- a `CollisionGeometry` subclass (Box/Sphere/Cylinder/Cone/TriangleMesh → `fcl::BVHModel` and so on).
- a `CollisionObject` subclass (holds an `fcl::CollisionObjectd`, overrides `computeWorldTransform`).
- a `CollisionDetector` subclass (`fcl::BroadPhaseCollisionManagerd` + `fcl::collide/distance`).
- a `CollisionGeometryManager` subclass (the factory hooks).

### 9.3 Compatibility and Migration

New code depends only on this module's interfaces; no FCL type appears in a core header.

## 10. References

### 10.1 Code

- `src/robotics/core/sdk/vine/robotics/proximity/**`
- `tests/test_proximity/ProximityTest.cpp`

### 10.2 Documents and ADRs

- `.ai/memory/robotics-proximity.md`
- `.ai/design/robotics-io-design.md`
- `../backlog/`

### 10.3 External Sources

- VMR's `scene_core/include/vmr/proximity` (external repository; `V` prefix dropped, Qt-style accessors, C++20).
