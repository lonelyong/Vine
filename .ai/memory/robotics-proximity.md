# Robotics proximity notes

Location: `src/robotics/core/sdk/vine/robotics/proximity/` (10 pure-header interfaces, namespace
`vn::robotics::proximity`). The `VN_ROBOTICS_PROXIMITY_NS_BEGIN/END` macros live in
`robot_core_global.hpp`. Tests: `tests/test_proximity/` (9 cases). Design:
`.ai/design/robotics-proximity-design.md`.

## Constraints and design

- `kinematics` may be depended on (Frame/State/QState); **workcell may not**. The owner type is
  `const vn::INamed*` (read-only `name()`, no `setName`: both `Frame` and `workcell::SceneObject`
  implement `INamed`).
- Reference: VMR's `D:\PROJS\VMR\src\scene_core\include\vmr\proximity` (the `V*` prefix dropped,
  Qt-style accessors).
- **FCL is not implemented**: `CollisionGeometry` / `CollisionObject` / `CollisionDetector` /
  `CollisionGeometryManager` are abstract bases (the detector's seven `do*` pure virtuals, the
  manager's `createCollisionGeometry/Object`). A backend becomes its own module later.
- Implemented inline (the non-FCL part): `CollisionMatrix`, `CollisionPair` (+hasher),
  `CollisionRequest`, `CollisionResult`, `CollisionContact`, and `CollisionObject`'s default
  `computeWorldTransform`.
- The default world pose is `frame ? Frame::frameInWorld(frame,state)*local : local`.

## Traps

- **A pure-header interface must not carry `VN_ROBOTICS_CORE_API`** (dllimport → 48 unresolved
  symbols, LNK2019); header-only code takes no export macro.
- `RefCounted<Derived>` has a protected non-virtual destructor → an interface class declares its own
  `virtual ~X() = default;`.
- A mock overriding `doCheckCollision` must fully qualify `vn::robotics::kinematics::State` (under a
  using-directive the name resolves differently, the pure virtual does not match, and the error reads
  "abstract member").
- A subclass turning a public base virtual into a protected one hides the static call; an override keeps
  the base's access.
- `minDistance` sentinels: a==b → -1.0, unregistered → -2.0. `shouldCheckCollision(a,b)` = a!=b &&
  !isIgnored(a,b) (the same rule as VMR).
