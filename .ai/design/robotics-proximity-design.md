# Robotics Proximity 模块设计（接口层）

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：碰撞几何/碰撞体/碰撞矩阵/检测器/几何管理器的接口层；不覆盖 FCL 实现。
> **权威来源**：`src/robotics/core/sdk/vine/robotics/proximity/**`、`tests/test_proximity`。
> **待办**：`../backlog/`

## 1. 概要设计

### 1.1 问题与背景

`vn::robotics` 需要碰撞检测的基础接口，但不应把 FCL 的类型和生命周期塞进核心模块。VMR 的 proximity
接口是一份成熟参照；Vine 需要同名同形的接口，同时去掉对 `workcell` 的依赖。

### 1.2 目标

- 碰撞几何 / 碰撞体 / 碰撞矩阵 / 碰撞检测器 / 几何管理器的接口。
- 非 FCL 的公共逻辑（矩阵、配对、结果、bookkeeping）直接实现。
- FCL 后端（几何转换、broadphase、narrowphase）留给独立模块。

### 1.3 非目标

- 不实现 FCL；不在本模块里链接 FCL。
- 不依赖 `workcell`；owner 只用 `INamed`。
- 不管理 owner/frame 的生命周期（调用方保证）。

### 1.4 一句话结论

`proximity` 是纯头文件接口层：owner 用 `INamed`、帧用 `kinematics::Frame`、几何/对象用 `intrusive_ptr`；
FCL 只通过纯虚钩子接入，核心模块不出现 FCL 类型。

## 2. 设计要点

### 2.1 不依赖 workcell 的 owner 模型

- owner 类型用 `const vn::INamed*`（只读 `name()`）：`kinematics::Frame` 与未来的 `workcell::SceneObject`
  都实现 `INamed`，矩阵/检测器按 owner 指针分组。
- 帧关联用 `raw_ptr<const kinematics::Frame>`；世界位姿默认实现：
  `world = frame ? Frame::frameInWorld(frame, state) * local : local;`。

### 2.2 FCL 只留接口

| 类 | 纯虚钩子 | 说明 |
| --- | --- | --- |
| `CollisionGeometry` | `buildFromShape` / `isValid` | 后端把 `Shape` 转成 FCL 几何 |
| `CollisionObject` | `isValid`；`computeWorldTransform` 可覆写 | 后端同步 FCL 内部变换 |
| `CollisionDetector` | `doAdd/doRemove/doRebuild/doEndUpdate`、`doCheckCollision ×3` | 后端实现 broadphase/narrowphase |
| `CollisionGeometryManager` | `createCollisionGeometry` / `createCollisionObject` | 后端创建 FCL 几何/对象 |

`CollisionMatrix` 是**完整实现**（非 FCL）；`CollisionRequest::collision_matrix` 用于过滤对象对。

### 2.3 引用计数与导出

- 几何/对象用 `vn::RefCounted<Derived>` + `vn::intrusive_ptr`。
- **纯头文件接口不能加 `VN_ROBOTICS_CORE_API`**：会 dllimport，而 RoboticsCore 里没有 .cpp 包含这些头。
- `RefCounted<Derived>` 析构是 protected 非虚 → 接口类须自带 `virtual ~X() = default;` 才能多态删除。

### 2.4 线程安全语义（同 VMR）

- 检测器非线程安全：注册/移除/位姿更新互斥；`checkCollision` 之间可并发，但不能与注册/更新并发。
- 不增加对 owner/frame 的引用计数；调用方保证生命周期。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 碰撞几何 | `CollisionGeometry` 抽象 + `buildFromShape` / `isValid` | 接口已落地 |
| F2 | 碰撞体 | `CollisionObject`：几何 + 局部位姿 + owner/frame | 接口已落地 |
| F3 | 矩阵/配对/结果 | `CollisionMatrix` / `CollisionPair` / `CollisionResult` | 完整实现 |
| F4 | 检测器 | `CollisionDetector` bookkeeping + `do*` 钩子 | 接口已落地 |
| F5 | 几何管理器 | `CollisionGeometryManager` 注册表 + 工厂钩子 | 接口已落地 |

### 3.1.1 F1 碰撞几何

- **行为**：`CollisionGeometry` 持 `intrusive_ptr<const geometry::Shape>`；`buildFromShape` 由后端把形状转成
  FCL 几何，`isValid` 报是否可用。
- **约束**（C1）：核心模块不出现 FCL 类型；`Shape` 生命周期由调用方保证。
- **要求**（R1）：几何用 `RefCounted` + `intrusive_ptr`，可多态删除。
- **验收**（T1）：`tests/test_proximity` 的 `MockGeometry` 编译/派生。
- **注意**（N1）：纯头文件接口不加导出宏。

### 3.1.2 F2 碰撞体

- **行为**：`CollisionObject` = 几何 + 局部位姿 + owner/frame；默认 `computeWorldTransform` 用
  `Frame::frameInWorld(frame, state) * local`。
- **约束**（C2）：owner 是 `const INamed*`；frame 是 `raw_ptr<const kinematics::Frame>`，不增加引用计数。
- **要求**（R2）：后端可覆写 `computeWorldTransform` 同步 FCL 内部变换。
- **验收**（T2）：世界位姿用例。
- **注意**（N2）：调用方必须保证 owner/frame 活得比碰撞体久。

### 3.1.3 F3 矩阵/配对/结果

- **行为**：`CollisionMatrix` 是 N×N 完整实现；`CollisionPair` 是无序 owner 对 + hasher；
  `CollisionResult` 是按 owner 对分组的结果快照。
- **约束**（C3）：`minDistance` 哨兵：`a==b → -1.0`，未注册 `→ -2.0`；
  `shouldCheckCollision(a,b) = a != b && !isIgnored(a,b)`。
- **要求**（R3）：未注册 owner 不算 ignored → 返回 true。
- **验收**（T3）：矩阵/配对/结果用例。
- **注意**（N3）：`CollisionResult` 的 `pairs` 按 owner 无序对分组。

### 3.1.4 F4 检测器

- **行为**：`CollisionDetector` 的 bookkeeping（`beginUpdate/endUpdate`、`addObject/removeObject`）已实现；
  broadphase/narrowphase 由 `do*` 钩子实现。
- **约束**（C4）：`beginUpdate/endUpdate` 成对计数，归零触发 `doEndUpdate()`；
  `addObject` 对已注册 owner 是 no-op。
- **要求**（R4）：仅接受 `doAddCollisionObject` 返回 true 的碰撞体。
- **验收**（T4）：检测器 bookkeeping 用例。
- **注意**（N4）：检测器非线程安全；注册/更新与查询不能并发。

### 3.1.5 F5 几何管理器

- **行为**：`CollisionGeometryManager` 的 `add/get/remove/update/clear` 已实现；
  `createCollisionGeometry` / `createCollisionObject` 是工厂钩子。
- **约束**（C5）：核心模块不创建 FCL 对象；后端负责工厂实现。
- **要求**（R5）：去掉 VMR 的 weak_ptr 惰性清理（Vine 无 weak_ptr），shape 生命周期归调用方。
- **验收**（T5）：管理器注册表用例。
- **注意**（N5）：`createCollisionGeometry` 是 protected，`createCollisionObject` 是 public。

## 4. 公开接口

| 类 | 契约 | 备注 |
| --- | --- | --- |
| `ProximityCore.hpp` | `ProximityScalar = double` | — |
| `CollisionGeometry` | `buildFromShape` / `isValid` | 后端实现 |
| `CollisionObject` | 几何 + 位姿 + owner/frame；`computeWorldTransform` | 可覆写 |
| `CollisionContact` | 触点 | — |
| `CollisionPair` | 无序 owner 对 + hasher | — |
| `CollisionRequest` | 查询选项 + `collision_matrix` | — |
| `CollisionResult` | 按 owner 对分组的结果 | — |
| `CollisionMatrix` | N×N 矩阵 | 完整实现 |
| `CollisionDetector` | bookkeeping + `do*` 钩子 | 后端实现 |
| `CollisionGeometryManager` | 注册表 + 工厂钩子 | 后端实现 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 核心模块不出现 FCL 类型；`Shape` 生命周期由调用方保证。 |
| C2 | owner 是 `const INamed*`；frame 是借用，不增加引用计数。 |
| C3 | 矩阵哨兵 `-1.0` / `-2.0`；`shouldCheckCollision` 规则固定。 |
| C4 | 检测器 bookkeeping 成对计数，归零触发 `doEndUpdate()`。 |
| C5 | 核心模块不创建 FCL 对象；工厂钩子归后端。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 几何/对象用 `RefCounted` + `intrusive_ptr`，可多态删除。 |
| R2 | 后端可覆写 `computeWorldTransform` 同步 FCL 变换。 |
| R3 | 未注册 owner 不算 ignored。 |
| R4 | 检测器只接受 `doAddCollisionObject` 返回 true 的对象。 |
| R5 | 去掉 VMR 的 weak_ptr 惰性清理；shape 生命周期归调用方。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 纯头文件接口不加 `VN_ROBOTICS_CORE_API`。 |
| N2 | 接口类自带 `virtual ~X() = default;`。 |
| N3 | 检测器非线程安全；查询与注册/更新不能并发。 |
| N4 | 不依赖 `workcell`。 |

## 6. 注意事项与陷阱

### 6.1 mock 覆盖的签名

- 会怎么错：`kinematics::State` 没全限定，纯虚函数不匹配，mock 仍是抽象类。
- 怎么判：编译报 `cannot instantiate abstract class`。
- 怎么防：写 `vn::robotics::kinematics::State`；覆盖 public 虚函数保持访问级别一致。

### 6.2 多态删除

- 会怎么错：`RefCounted<Derived>` 析构 protected 非虚，删除基类指针时漏析构。
- 怎么判：ASan/派生类析构未跑。
- 怎么防：接口类自带 `virtual ~X() = default;`。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 依赖 `workcell` | 核心模块应能被 kinematics/无 workcell 场景复用。 | — |
| D2 | 在本模块实现 FCL | 会把 FCL 类型/构建依赖带进核心。 | — |
| D3 | 给 header-only 接口加导出宏 | 会 dllimport，RoboticsCore 没有对应 .cpp。 | — |
| D4 | 保留 VMR 的 weak_ptr 惰性清理 | Vine 无 weak_ptr；shape 生命周期归调用方。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 接口可编译、可派生 | `tests/test_proximity/ProximityTest.cpp` mock 子类 |
| T2 | 矩阵/配对/结果/世界位姿 | 同上 |
| T3 | 检测器 bookkeeping | 同上 |
| T4 | 管理器注册表 | 同上 |

## 9. 状态与演进

### 9.1 当前状态

接口层已实现（纯头文件，无 FCL）；9 个测试覆盖 mock 派生与非 FCL 逻辑。

### 9.2 已知缺口 / 待办

FCL 后端在独立模块（如 `robotics/proximity_fcl`，依赖 `RoboticsIO`/`RoboticsCore` + FCL）：

- `CollisionGeometry` 子类（Box/Sphere/Cylinder/Cone/TriangleMesh → `fcl::BVHModel` 等）。
- `CollisionObject` 子类（持 `fcl::CollisionObjectd`，覆写 `computeWorldTransform`）。
- `CollisionDetector` 子类（`fcl::BroadPhaseCollisionManagerd` + `fcl::collide/distance`）。
- `CollisionGeometryManager` 子类（工厂钩子）。

### 9.3 兼容性 / 迁移

新代码只依赖本模块的接口；FCL 类型不出现在核心头文件里。

## 10. 参考

### 10.1 代码

- `src/robotics/core/sdk/vine/robotics/proximity/**`
- `tests/test_proximity/ProximityTest.cpp`

### 10.2 文档与 ADR

- `.ai/memory/robotics-proximity.md`
- `.ai/design/robotics-io-design.md`
- `../backlog/`

### 10.3 外部资料

- VMR 的 scene_core/include/vmr/proximity（外部仓库；去 `V` 前缀、Qt 式访问器、C++20）。
