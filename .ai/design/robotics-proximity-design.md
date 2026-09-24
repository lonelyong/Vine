# Robotics Proximity 模块设计（接口层）

> 状态：**接口层已实现**（纯头文件，无 FCL）。参考 VMR
> `D:\PROJS\VMR\src\scene_core\include\vmr\proximity`（去 `V` 前缀、Qt 式访问器、C++20）。
> 约束：可依赖 `kinematics`，**禁止依赖 `workcell`**；涉及 FCL 的实现跳过，仅定义接口。

## 1. 背景与目标

为 `vn::robotics` 提供碰撞检测的基础接口：碰撞几何 / 碰撞体 / 碰撞矩阵 / 碰撞检测器 / 几何管理器。
当前只落接口与非 FCL 的公共逻辑；FCL 后端（几何转换、broadphase、narrowphase）以后在独立模块实现。

## 2. 模块结构（新建）

```
src/robotics/core/sdk/vine/robotics/proximity/
  ProximityCore.hpp            # ProximityScalar = double
  CollisionGeometry.hpp        # 引擎侧碰撞几何（抽象，FCL 实现）
  CollisionObject.hpp          # 碰撞体：几何 + 局部位姿 + owner/frame（含默认 computeWorldTransform）
  CollisionContact.hpp         # 触点
  CollisionPair.hpp            # 无序 owner 对 + hasher
  CollisionRequest.hpp         # 查询选项
  CollisionResult.hpp          # 结果快照（按 owner 对分组）
  CollisionMatrix.hpp          # N×N 碰撞矩阵（完整实现，非 FCL）
  CollisionDetector.hpp        # 检测器基类（bookkeeping 已实现，do* 钩子纯虚）
  CollisionGeometryManager.hpp # 几何工厂/注册表（add/get/remove/update/clear 已实现，create* 纯虚）
```

命名空间宏 `VN_ROBOTICS_PROXIMITY_NS_BEGIN/END` 加在 `robot_core_global.hpp`。
测试：`tests/test_proximity/`（9 用例，mock 子类编译验证 + 非 FCL 逻辑）。

## 3. 关键设计决策

### 3.1 不依赖 workcell 的 owner 模型
- owner 类型用 **`const vn::INamed*`**（只读 `name()`，无需 `setName`）：`kinematics::Frame` 与未来的
  `workcell::SceneObject` 都实现 `INamed`，天然通用；矩阵/检测器按 owner 指针分组。
- 帧关联用 `raw_ptr<const kinematics::Frame>`；世界位姿默认实现：
  `world = frame ? Frame::frameInWorld(frame, state) * local : local;`（`CollisionObject` 内联默认）。
- 与 VMR 的 `const VNamedObject*` 对应；`VState` 对应 `kinematics::State`。

### 3.2 FCL 相关的只留接口
| 类 | 纯虚钩子 | 说明 |
|---|---|---|
| `CollisionGeometry` | `buildFromShape` / `isValid` | FCL 后端把 `Shape` 转成 `fcl::CollisionGeometry` 子类 |
| `CollisionObject` | `isValid`；`computeWorldTransform` 可覆写 | 后端覆写以同步 fcl 内部变换 |
| `CollisionDetector` | `doAdd/doRemove/doRebuild/doEndUpdate`、`doCheckCollision ×3` | 后端实现 broadphase/narrowphase |
| `CollisionGeometryManager` | `createCollisionGeometry`（protected）/ `createCollisionObject`（public） | 后端创建 fcl 几何/对象 |

`CollisionRequest` 中 `collision_matrix` 指针用于过滤对象对；`CollisionMatrix` 为**完整实现**（非 FCL）。

### 3.3 引用计数与内存
- 几何/对象用 `vn::RefCounted<Derived>` + `vn::intrusive_ptr`（与 `Shape` 一致）。
- **纯头文件接口不能加 `VN_ROBOTICS_CORE_API`**：会 dllimport，而 RoboticsCore 里没有 .cpp 包含
  这些头 → 链接 LNK2019。header-only 一律不加导出宏。
- `RefCounted<Derived>` 析构是 protected 非虚 → 接口类须自带 `virtual ~X() = default;` 才能多态删除。

### 3.4 线程安全语义（同 VMR）
- 检测器非线程安全：注册/移除/位姿更新互斥；`checkCollision` 之间可并发，但不能与注册/更新并发。
- 不增加对 owner/frame 的引用计数，调用方需保证生命周期。

## 4. 与 VMR 接口对照

| Vine | VMR | 差异 |
|---|---|---|
| `ProximityCore.hpp` | `VProximityCore.h` | `ProximityScalar` 同名 |
| `CollisionGeometry` | `VCollisionGeometry` | `Shape` → `intrusive_ptr<const geometry::Shape>` |
| `CollisionObject` | `VCollisionObject` | `VFrame` → `kinematics::Frame`；`VNamedObject` → `INameable` |
| `CollisionContact` | `VCollisionContact` | `co1/co2/b1/b2/pos` → `object1/object2/primitive1/primitive2/position` |
| `CollisionPair` | `VCollisionPair` | `VNamedObject*` → `const INameable*` |
| `CollisionRequest` | `VCollisionRequest` | 同字段 |
| `CollisionResult` | `VCollisionResult` | `pairs` 按 owner 无序对分组；`print/hasSameCollisionPairsAs` 内联实现 |
| `CollisionMatrix` | `VCollisionMatrix` | 完整实现；`DefaultCollisionOptions` → `defaultCollisionOptions()`（header-only） |
| `CollisionDetector` | `VCollisionDetector` | 同结构，`do*` 钩子纯虚 |
| `CollisionGeometryManager` | `VCollisionGeometryManager` | 去掉 VMR 的 weak_ptr 惰性清理（Vine 无 weak_ptr），shape 生命周期由调用方保证 |

## 5. 行为约定（与 VMR 一致）
- `CollisionMatrix::minDistance` 哨兵：`a==b → -1.0`，未注册 `→ -2.0`。
- `shouldCheckCollision(a,b) = a != b && !isIgnored(a,b)`；未注册 owner 不算 ignored → 返回 true。
- 检测器 `beginUpdate/endUpdate` 成对计数，计数归零触发 `doEndUpdate()`。
- `addObject` 对已注册 owner 是 no-op；仅接受 `doAddCollisionObject` 返回 true 的碰撞体。

## 6. 后续 FCL 接入点
新增独立模块（如 `robotics/proximity_fcl`，依赖 `RoboticsIO`/`RoboticsCore` + FCL）：
- 实现 `CollisionGeometry` 子类（Box/Sphere/Cylinder/Cone/TriangleMesh → `fcl::BVHModel` 等）。
- 实现 `CollisionObject` 子类（持 `fcl::CollisionObjectd`，覆写 `computeWorldTransform` 同步变换）。
- 实现 `CollisionDetector` 子类（`fcl::BroadPhaseCollisionManagerd` + `fcl::collide/distance`）。
- 实现 `CollisionGeometryManager` 子类（`createCollisionGeometry/createCollisionObject`）。
- 对照 `CollisionRequest` 字段映射到 `fcl::CollisionRequest`。

## 7. 测试要点
- `tests/test_proximity/ProximityTest.cpp` 用 mock 子类（`MockGeometry/MockCollisionObject/MockDetector/
  MockGeometryManager`）证明接口可编译、可派生，并覆盖矩阵/配对/结果/世界位姿/检测器 bookkeeping。
- 坑：mock 覆盖 `doCheckCollision` 时 `kinematics::State` 必须全限定 `vn::robotics::kinematics::State`
  （using 指令下解析不一致 → 纯虚不匹配）；子类覆盖 public 虚函数保持与基类一致的访问级别。
