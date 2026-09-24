# Robotics proximity 模块要点

位置: `src/robotics/core/sdk/vine/robotics/proximity/`（10 个纯头文件接口，ns `vn::robotics::proximity`）。
宏 `VN_ROBOTICS_PROXIMITY_NS_BEGIN/END` 在 `robot_core_global.hpp`。测试: `tests/test_proximity/`（9 用例）。
设计文档: `docs/robotics-proximity-design.md`。

## 约束与设计
- 可依赖 `kinematics`（Frame/State/QState）；**禁止依赖 workcell**。owner 类型 `const vn::INamed*`
  （只读 name()，无需 setName；Frame 与 workcell::SceneObject 都实现 INamed）。
- 参考 VMR `D:\PROJS\VMR\src\scene_core\include\vmr\proximity`（V* 去前缀、Qt 式访问器）。
- **FCL 不实现**：`CollisionGeometry`/`CollisionObject`/`CollisionDetector`/`CollisionGeometryManager`
  是抽象基类（detector `do*` 纯虚 ×7；manager `createCollisionGeometry/Object` 纯虚）。后端以后另建模块。
- 非 FCL 已实现（内联）：`CollisionMatrix`、`CollisionPair`(+hasher)、`CollisionRequest`、
  `CollisionResult`、`CollisionContact`、`CollisionObject` 默认 `computeWorldTransform`。
- 世界位姿默认 = `frame ? Frame::frameInWorld(frame,state)*local : local`。

## 关键坑
- **纯头文件接口不能加 `VN_ROBOTICS_CORE_API`**（dllimport → LNK2019 48 个未解析）；header-only 不加导出宏。
- `RefCounted<Derived>` 析构 protected 非虚 → 接口类自加 `virtual ~X() = default;`。
- mock 覆盖 `doCheckCollision` 时 `kinematics::State` 必须全限定 `vn::robotics::kinematics::State`
  （using 指令下解析不同 → 纯虚不匹配，报"抽象成员"）。
- 子类把基类 public 虚函数改成 protected 会遮蔽静态调用；覆盖保持与基类访问一致。
- `minDistance` 哨兵: a==b → -1.0，未注册 → -2.0。`shouldCheckCollision(a,b)` = a!=b && !isIgnored(a,b)（同 VMR）。
