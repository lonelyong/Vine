# geometry 射线与图元相交

> **状态**：现行（值类型与四个图元相交已落地；`graphics` 已改用它）
> **最后核对**：2026-11-04
> **范围**：`src/base/geometry` 的射线值类型与射线–AABB / OBB / 平面 / 三角形查询的归属与契约。
> 不含场景走查、拾取身份与选择态（见 `graphics-scene.md`），不含射线–网格遍历与加速结构（见 §9.2）。
> **权威来源**：`src/base/geometry/sdk/vine/geometry/Ray.hpp`、`RayHit.hpp`、`Aabb.hpp` / `Obb.hpp` / `Plane.hpp` 的 `intersectWith(Ray, RayHit&)`、`tests/test_geometry`。
> **待办**：`../backlog/graphics.md`（射线–网格遍历等开放项）

## 1. 概要设计

### 1.1 问题与背景

射线今天住在 `viz`：`vn::graphics::Ray`（`Vec3d` 原点 + 单位方向）与 `vn::graphics::RayIntersection` 同模块，而后者把两件事混在一起：

- **图元相交**——slab（盒）、Möller–Trumbore（三角形）：只依赖几何数据；
- **场景走查**——节点树、世界矩阵、实例流、拾取身份：只属于渲染侧。

后果有三条：

- 非渲染的射线消费者（`brep` 的选中面、机器人 LiDAR 仿真、碰撞查询）要么依赖 `viz`，要么自己复制一份 `Ray`；`brep` 在 `base`，依赖 `viz` 是一条明确的回边。
- 同一份 slab / 三角形相交只能在 graphics 里实现一次，别的层拿不到。
- `geometry` 的 `Aabb` / `Obb` / `Plane` 没有射线入口，射线查询与这些类型的契约分裂在两个模块。

### 1.2 目标

- `Ray` 只有一个定义，放在 `base` 一侧，与 `Aabb` / `Obb` / `LineCurve` / `Plane` 同层。
- 四个图元相交与被交类型同模块，命名沿用 `Plane::intersectWith(LineCurve, Point3&)` 的既有约定。
- `graphics` 只留场景语义：屏幕→世界、世界→局部、最近命中与拾取身份。

### 1.3 非目标

- 不做射线–网格遍历与加速结构（BVH / 八叉树）：今天没有性能证据，且它会引入第二种数据组织（`G8`）。
- 不做场景查询与选中态：拾取是机制、选择是业务（`graphics-scene.md`）。
- 不给 `SphericalSurface` / 胶囊 / 圆锥面这类**值类型**补射线入口：没有消费者；等出现再按同一约定加。解析实体（`Solid<T>`）的射线求交已经存在，见 `geometry-solid.md`。

### 1.4 一句话结论

**射线是空间实体，归 `geometry`；相交挂在被交对象上；场景走查与拾取身份留在 `graphics`；`math` 不碰。**

## 2. 设计要点

### 2.1 `Ray` 与 `LineCurve` 是两种语义，不合并

- **是什么**：`Ray<T>{ origin, direction }`，方向**单位化**，`t` 是模型单位的距离；`LineCurve<T>` 是无限直线，方向不必单位，`t` 与 `|direction|` 绑定。
- **为什么**：`G4`、`G12` —— 用一个 `bool is_half_line` 或“约定方向长度”把两者合成一个类型，会让每个读代码的人重新推一遍 `t` 的量纲。
- **边界**：`Ray` 不进入 `Shape` 层级（与 2.4 的四值类型同一立场）；`LineCurve` 原有契约不动。

### 2.2 相交挂在被交对象上

- **是什么**：`Aabb<T>::intersectWith(const Ray<T>&, RayHit<T>&)`、`Obb<T>` 同签名、`Plane<T>` 同签名；三角形因为没有值类型，做成自由函数 `intersectTriangle(ray, a, b, c, hit)`。
- **为什么**：`G1`、`G6` —— 与 `Plane::intersectWith(LineCurve, Point3&)` 同一形状：被交的东西拥有算法，调用方不必知道 slab 或 Möller–Trumbore。
- **边界**：`Obb` 的实现把射线逆变换到局部空间再复用 `Aabb`，不写第二套 slab（`G1`、`G10`）。
- **边界**：`Aabb` / `Obb` 只报**最近命中**（边界语义）；`Solid<T>::intersectWith()` 报穿过实体的全部交点（进入 / 离开，体积语义）。两者共用 `Aabb.hpp` 的 `detail::rayBoxInterval()`，不各写一套 slab。

### 2.3 结果是 `RayHit`，不是 `optional`

- **是什么**：`RayHit<T>{ t, point, normal }`；查询返回 `bool`，`false` 时 `hit` 不被写。
- **为什么**：`G4` —— 拾取要的正是这三样；“命中/未命中”是一个值，不是异常也不是 `optional` 包装（仓库既有相交入口都是 `bool` + 出参）。
- **边界**：`point == ray.pointAt(t)` 是契约不变量；`normal` 一律**朝向射线来向**（与 `direction` 点积 ≤ 0），盒、OBB、平面、三角形同一规则——包括从盒内出发的射线（它看到的是出射面的反面）。

### 2.4 精度跟随类型，相机路径用 `Rayd`

- **是什么**：`Ray<T>` 模板 + `Rayf` / `Rayd` 别名，与 `Aabb` / `Obb` / `LineCurve` / `Plane` 一致；`Camera::screenToWorldRay()` 返回 `Rayd`。
- **为什么**：`G10` —— 屏幕像素反解的世界坐标用 float 会丢精度；网格顶点仍是 float，相交时按 `Rayd` 逐三角形取值计算，只有一条代码路径。
- **边界**：不提供 `Rayd` 到 `Rayf` 的隐式转换；需要 float 版本时显式构造。

### 2.5 场景语义留在 `graphics`

- **是什么**：`RayIntersection` 保留 `intersect()` / `intersectScene()` / `intersectSceneAll()` 与 `RayIntersectionResult{ hit, point, normal, distance, geometry, primitiveIndex, instanceIndex }`，内部换成 `geometry::RayHit<double>` 加自己的身份字段。
- **为什么**：`G2` —— 节点树、世界矩阵、实例流、可见性是 graphics 的词汇；几何侧一旦认识它们，`brep`/机器人就得反向依赖 `viz`。
- **边界**：`graphics::Ray` **删除**，不留别名（与 `D2`「不留 `math::Rect3` 兼容别名」同一立场）；调用点一次性改完。

### 2.6 不开新模块，也不进 `math`

- **是什么**：射线与四个相交入口都放进 `geometry`。
- **为什么**：`G8` —— 一个值类型加四个查询不成一层；`math` 只放数值与变换，把空间实体放进去正是 `geometry-primitives.md` 2.1 那条判据要消灭的事。
- **边界**：`math` 只提供它已有的点/向量/变换；射线不引入任何 `math` 新类型。

### 2.7 射线与三角形求交定义在头文件

- **是什么**：`Ray<T>` 的成员与 `intersectTriangle` 全部在 `Ray.hpp` 中定义，该类不再有独立实现文件，`VN_GEOMETRY_API` 显式实例化一并去掉。
- **为什么**：`G7`、`G10` —— `graphics` 的 `RayIntersection::traverseMesh` 逐三角形调用，跨 DLL 调用不可能内联；实测命中断 7.09 → 5.99 ns、平行剔除 3.96 → 2.71 ns。
- **整类搬而不是单函数**：只把 `intersectTriangle` 放进头文件、而把 `pointAt()` 留在 `.cpp`，收益从 1.10 降到 0.39 ns/tri —— 函数体里的 `hit.point = ray.pointAt(t)` 仍是一次外部调用。
- **边界**：与其它值类型同一规则（`geometry-primitives.md` 2.8）；调用点仍只有 `graphics` 一处，`C1`–`C3` 与 §4 的签名不变。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 射线值类型 | `Ray<T>`：原点 + 单位方向；`pointAt()` / `distanceToPoint()` | 已落地 |
| F2 | 射线–AABB | slab 法，返回首个正向命中与出射面法线 | 已落地 |
| F3 | 射线–OBB | 逆变换到局部后复用 AABB，`t` 仍是世界距离 | 已落地 |
| F4 | 射线–平面 / 三角形 | 平面：`n·d` 平行判定；三角形：Möller–Trumbore，双面 | 已落地 |
| F5 | graphics 迁移 | `Camera` 返回 `geometry::Rayd`；`RayIntersection` 只剩场景走查与身份 | 已落地 |

### 3.1 F1 射线值类型

- **行为**：构造时归一化方向；`direction` 为零向量时构造合法但**所有查询返回 false**（不产生 NaN）。
- **约束**（C1）：`t` 是模型单位距离；`pointAt(t) == origin + direction * t`。
- **验收**（T1）：`RayTest` 覆盖归一化、`pointAt`、零方向、`distanceToPoint`。

### 3.2 F2 射线–AABB

- **行为**：slab 法；起点在盒外时取进入面法线；起点在盒内时 `t = 0`，法线取它看到的那个面（出射面的反面，保持与 `direction` 点积 ≤ 0）。
- **约束**（C2）：`t >= 0`；`normal` 朝向射线来向。
- **注意**（N1）：盒子退化（零尺寸）仍按同一公式处理，命中面可为任意一对平行面。
- **验收**（T2）：命中、未命中、起点在内部、平行于面、擦边五类。

### 3.3 F3 射线–OBB

- **行为**：用 `Obb` 的朝向与半尺寸把射线变换到局部空间，调用 AABB 查询，再变换法线回世界。
- **约束**（C3）：`Obb` 的变换是正交的，所以 `t` 在变换下不变，直接就是世界距离。
- **验收**（T3）：旋转盒的命中与 `Aabb` 版本一致（同一形状、同一射线）。

### 3.4 F4 射线–平面 / 三角形

- **行为**：平面在 `|n·d| <= eps` 时返回 false（平行）；三角形双面命中，法线按绕序与射线朝向调整。
- **约束**（C4）：顶点共线/退化三角形的命中结果由实现决定，但不得返回 NaN。
- **验收**（T4）：背面命中、平行、退化三角形、命中点在三角形外。

### 3.5 F5 graphics 迁移

- **行为**：`graphics::Ray` 删除；`Camera::screenToWorldRay()` 返回 `geometry::Rayd`；`RayIntersection` 用 `RayHit` 组装原有结果。
- **要求**（R4）：`model_viewer`、`OrbitCameraManipulator`、`RayIntersectionTest` 同步改完，仓库内不再出现 `graphics::Ray`。
- **验收**（T5）：`RayIntersectionTest` 全绿且不依赖 `graphics::Ray`。

## 4. 公开接口

| 类型 / 文件 | 内容 |
|---|---|
| `Ray.hpp` | `Ray<T>`、`Rayf` / `Rayd`；`pointAt()` / `distanceToPoint()` |
| `RayHit.hpp` | `RayHit<T>{ t, point, normal }` |
| `Aabb.hpp` / `Obb.hpp` / `Plane.hpp` | 各自的 `intersectWith(const Ray<T>&, RayHit<T>&)` |
| 自由函数 | `intersectTriangle(const Ray<T>&, a, b, c, RayHit<T>&)` |

完整签名与注释在头文件；本文件只记录归属与理由。

## 5. 约束、要求与不变量

- **C1**：`t` 是模型单位距离；方向恒为单位向量（零方向除外，见 C3）。
- **C2**：只接受 `t >= 0` 的命中；`normal` 朝向射线来向。
- **C3**：零方向射线不产生命中，也不产生 NaN。
- **C4**：退化输入（共线三角形、平行平面）返回 false，不抛异常。
- **R1**：相交是纯函数：不修改被交对象，不分配。
- **R2**：`Obb` 复用 `Aabb` 的实现，不写第二套 slab。
- **R3**：查询对被交类型的精度如实：`Rayd` × `Aabbd` 用 double，`Rayf` × `Aabbf` 用 float，不隐式混用。
- **R4**：`graphics` 不再定义射线类型；只消费 `geometry` 的。
- **N1**：`point == ray.pointAt(t)` 恒成立；调用方不必自己再算一遍。
- **N2**：起点恰在表面上时算命中（`t = 0`），拾取方要区分“从内部开始”时自己看 `t`。
- **N3**：`eps` 只是“方向视为零”的容差；slab 的平行判定用**精确零**，否则近轴射线会被当成平行而误判为未命中；三角形的行列式容差是**相对**的（按两条边长约去），否则小三角形会因面积小而永远打不中。

## 6. 注意事项与陷阱

- **P1**：把 `Ray` 当成 `LineCurve` 用会得到错误量纲——`LineCurve` 的 `t` 依赖 `|direction|`，`Ray` 的 `t` 是距离。会怎么错：命中距离与显示的距离差一个方向长度；怎么判：`pointAt(t)` 与预期点不符；怎么防：只通过 `intersectWith` 取 `t`。
- **P2**：起点在盒内的拾取会得到 `t = 0` 与出射面的反面法线，看起来像“贴着面命中”。怎么判：`t` 为 0；怎么防：需要内部最近面时用 `intersectSceneAll()` 或先做包含判定。
- **P3**：擦边（射线穿过棱或顶点）在浮点下可能命中相邻两个面。怎么判：两个距离几乎相等；怎么防：不要依赖“唯一命中”，选择逻辑按距离排序后取首个。
- **P4**：把 float 网格的顶点直接与 `Rayd` 相乘会丢一次精度。怎么判：远处物体的命中点抖动；怎么防：三角形顶点先升到 double 再算（F5 的实现约定）。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 把 `Ray` 放进 `math` | `math` 只放数值与变换，空间实体归 `geometry`（`geometry-primitives.md` 2.1、`G1`）。 |
| D2 | 留在 `graphics`，其它层各复制一份 | 一个概念两三个实现，必然漂移（`G1`）；`brep` 依赖 `viz` 是回边（`G2`）。 |
| D3 | `Ray` 复用 `LineCurve`（加布尔或约定方向长度） | 两种量纲塞进一个类型，`t` 的含义要靠读文档推（`G4`）。 |
| D4 | 相交写成自由函数 `intersect(ray, target)` 的单一重载集 | 与被交类型分离，`Obb` 复用 `Aabb`、平面特例都要在外部重写；成员形式与既有 `Plane::intersectWith` 一致（`G6`）。 |
| D5 | 本期就做射线–网格与 BVH | 没有性能证据，加速结构会引入新的数据组织与失效规则（`G8`、`G10`）。 |
| D6 | 保留 `graphics::Ray` 兼容别名 | 与 `geometry-primitives.md` D2 同一立场；一个类型两个名字必然漂移（`G4`）。 |

## 8. 测试与验收

| # | 判据 | 用例 |
|---|---|---|
| T1 | 归一化、`pointAt`、零方向不产生 NaN、`distanceToPoint` | `tests/test_geometry/RayTest.cpp` |
| T2 | 盒：命中、未命中、起点在内、平行、擦边 | `RayTest.cpp` |
| T3 | OBB：旋转盒与等价 AABB 结果一致 | `RayTest.cpp` |
| T4 | 平面与三角形：背面、平行、退化、命中点在三角形外 | `RayTest.cpp` |
| T5 | 迁移后 `RayIntersectionTest` 全绿且不依赖 `graphics::Ray` | `tests/test_graphics`（`HitsTriangle` / `MissesTriangle` 目前被一个既有崩溃挡住，见 backlog） |

## 9. 状态与演进

### 9.1 当前状态

已落地：`geometry::Ray` / `RayHit`、`Aabb` / `Obb` / `Plane` 的射线重载与 `intersectTriangle`；`graphics` 的 `Ray` 已删除，`Camera::screenToWorldRay()` 返回 `geometry::Rayd`，`RayIntersection` 只剩场景走查、世界→局部与拾取身份。

### 9.2 已知缺口

- 射线–网格遍历与加速结构（BVH / 八叉树）：等出现“每帧大量射线”的消费者（LiDAR 仿真）再设计（`D5`）。
- `tests/test_graphics` 里几个走 `geometryFromShape()` 的用例（`GeometryTest.MeshCounts`、`RayIntersectionTest.HitsTriangle` / `MissesTriangle`、`SceneTest.BoundingBoxAggregates`）在本机上崩溃，且在本轮改动之前就已如此；已记入 `../backlog/graphics.md`。

### 9.3 兼容性与迁移

`graphics::Ray` 直接删除，不留别名；迁移点：`Camera.cpp`、`RayIntersection.cpp`、`OrbitCameraManipulator.cpp`、`model_viewer`、`RayIntersectionTest`。

## 10. 参考

- 代码：`src/base/geometry/sdk/vine/geometry/Ray.hpp`、`RayHit.hpp`、`Aabb.hpp`、`Obb.hpp`、`Plane.hpp`；`src/viz/graphics/sdk/vine/graphics/Camera.hpp`、`RayIntersection.hpp`。
- 设计：`geometry-primitives.md`（空间值类型的边界判据）、`graphics-scene.md`（拾取是机制）、`geometry-intersection.md`（所有配对与结果形态的总表）。
- 测试：`tests/test_geometry/RayTest.cpp`、`tests/test_graphics`（相机与拾取）。
- 外部资料：Möller–Trumbore 射线–三角形求交；Kay–Kajiya slab 法。
