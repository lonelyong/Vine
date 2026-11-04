# geometry 几何族：Curve 与 Surface

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`Curve` / `Surface` 两个分类标签，以及具体值类型的归属与新增类型的最小契约。
> 参数化几何（`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`）**不构成族**：它们只被建模使用，集合就是 `Primitive::Parameters`（见 `geometry-parametric.md`）。
> 不含具体类型的完整契约（见 `geometry-primitives.md`、`geometry-parametric.md`、`geometry-ray.md` 与各自头文件），不含求交与距离算法。
> **权威来源**：`src/base/geometry/sdk/vine/geometry/Curve.hpp`、`Surface.hpp`；`tests/test_geometry/GeometryKindsTest.cpp`。
> **待办**：`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

几何类型之间此前没有任何分类关系：`Line`、`Ray`、`Plane`、`Box` 都是互不相关的类，"这是一条曲线 / 一个曲面 / 一个实体"这句话只存在于讨论里。想按族写一个约束、想把一族类型放进同一个容器、想回答"这个类型是几维的"，都只能靠命名猜测；运行期分类只有 `ShapeType`，而它只覆盖 `Shape` 子类，值类型根本不在其中。

### 1.2 目标

- 两个族各有一个名字，归属写在类型定义处（`class Ray : public Curve`）。
- 分类可以在编译期查询（`std::is_base_of_v<Curve, Rayd>`）与枚举（`Curve::kDimension`）。
- **不付运行期代价**：不加 vptr、不改大小、不让按值拷贝变贵。

### 1.3 非目标

- 不做多态接口：标签里没有虚函数，也不提供 `pointAt` 之类的抽象签名。
- 不做异构容器：需要"一条未知曲线"时用 `std::variant` + `std::visit`。
- 不给参数化几何（`Box` / `Sphere` / …）设族标签：它们不提供自己的算法（射线–盒就是 `Aabb` 的 slab），"哪些几何能组成模型"由 `Primitive::Parameters` 的变体回答。
- 不做族级统一算法（射线–曲面、点到曲线的距离）：今天没有消费者（`G8`），见 `../backlog/geometry.md`。

### 1.4 一句话结论

**Curve / Surface 是空标签组成的族，不是虚基类；参数化几何不属于任何族，它们只是建模参数。**

## 2. 设计要点

### 2.1 分类而不是接口

- **是什么**：`struct Curve { static constexpr int kDimension = 1; };`，`Surface` 同形，维度取 2。
- **为什么**：`G10` —— `geometry` 里最热的路径是逐三角形的射线测试，`Ray` 按值传递、按值拷贝；给它加 vptr 会把刚测到的内联收益退回去（平行剔除 2.71 → 3.96 ns/三角形，见 `geometry-ray.md` 2.7）。分类需要的是"它属于哪一族"，不是"通过基类调用谁"。
- **边界**：标签不含状态、不含虚函数、不写成员函数。需要被各族共享的算法是**自由函数**，按具体类型重载或按 concept 约束。

### 2.2 空基类优化：分类零成本

- **是什么**：`sizeof(Rayd) == 2 * sizeof(Point3d)`、`sizeof(Circled) == 2 * sizeof(Point3d) + sizeof(double)`、`sizeof(SphericalSurfaced) == sizeof(Point3d) + sizeof(double)`，且这些类型仍是 trivially copyable。
- **为什么**：`G10`、`G4` —— "派生"不应该意味着"变大"；这条不变式由 `GeometryKindsTest.cpp` 的 `static_assert` 守着，改坏了是编译错误，不是性能回退。
- **边界**：空基类优化要求基类无状态且不是第一个成员的类型；新增族成员时不要给标签加字段，否则这条断言立刻失败——这是有意的护栏。

### 2.3 归属

| 族 | 维度 | 成员 | 参数的含义 |
|---|---|---|---|
| `Curve` | 1 | `Line`、`Ray`、`LineSegment`、`Circle` | 各类型自定：`Line` 是 `direction` 的倍数、`Ray` 是距离、`LineSegment` 是 [0,1]、`Circle` 是弧度 |
| `Surface` | 2 | 下分 `ElementarySurface` 与 `BoundedSurface`（见下表） | 三角形用重心 `(u,v)`，四边形用双线性 `(u,v)`，球面用方位角与极角 |
| `ElementarySurface` | 2 | `Plane`、`SphericalSurface`（已实现）；`CylindricalSurface`、`ConicalSurface`、`ToroidalSurface`（**占位**） | 无界或半无限的解析支撑面 |
| `BoundedSurface` | 2 | `BezierSurface`、`BSplineSurface`、`RectangularTrimmedSurface`（**全部占位**，无已实现成员） | 参数域有限，总能回答 `bounds()` |

- **为什么**：`G1`、`G12` —— 维度只决定"取一个点要几个参数"，具体含义留在类型上；把参数含义统一进基类会造出一个每种类型都要重新解释的约定。
- **占位类型不可构造**：未实现的曲面把默认构造删掉（`CylindricalSurface() = delete`），类型树先固定、行为等消费者，避免出现"没有 frame 和半径的圆柱面"这种静默无意义的对象。
- **参数化几何不在表里**：`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 是**建模类型**（`Primitive` 的子类，见 `geometry-parametric.md`），不是几何族成员。需要列"哪些几何能组成模型"时看 `Primitive` 的五个子类；需要算几何时用值类型或曲面。

### 2.4 运行期多态用 variant

- **是什么**：`Primitive::Parameters`（`std::variant<Box, Sphere, Cylinder, Cone, Ellipsoid>`）是第一个用例；`brep` 与 `robotics::io` 用 `std::visit` 分派。
- **为什么**：`G4`、`G9` —— 变体是封闭集合，标签与内容不可能不一致；新增成员是编译错误提示所有消费点，而不是运行期静默落到 `default`。
- **边界**：变体放的是**值**，所以它能和值语义共存；放指针加虚函数等于绕回被否决的方案。

### 2.5 新增类型只给能验证的契约

- **是什么**：`LineSegment` / `Circle` 提供 `isValid()`、`pointAt(...)`、`bounds()`，以及各自的量（`length()`、`circumference()`、`direction()`）；`SphericalSurface` 提供 `pointAt(azimuth, polar)` / `normalAt` / `bounds()`。
- **单个三角形、四边形不做类型**：`intersectTriangle(ray, a, b, c, hit)` 直接吃三个角点，不需要类型；名字 `TriangleSurface` / `QuadSurface` 作为保留名登记在 backlog（`../backlog/geometry.md` G7），等它们长出自己的查询（射线–面求交、点到面距离）时再引入，且届时带 `Surface` 后缀——仓库里已有 `TriangleMesh` / `QuadMesh`，裸名会被读成网格元素（`G4`）。
- **为什么**：`G8` —— 没有消费者的能力不写：射线求交、点到曲线的距离、曲面求交都在 backlog，等触发条件。
- **边界**：`PointCloud` 仍是 `Shape`，不是曲线也不是曲面：它是离散点集，没有参数化取点。

### 2.6 算法放在值类型上，不另设算法类

- **是什么**：查询是**被交对象自己的成员**——`Sphere::intersectWith(const Rayf&, RayHitf&)`、`Cylinder::intersectWith(...)`、`Box::intersectWith(...)`、`Plane::intersectWith(...)`；没有值类型的三角形用自由函数 `intersectTriangle`。没有一个 `SphericalSurface` / `CylindricalSurface` 之类的算法类。
- **为什么**：`G1`、`G6`、`G12` —— 名字与数据各只有一份（半径在 `Sphere` 里，算法类不再存一遍）；调用方不必查“球面求交住在哪个类”；与 `geometry-ray.md` 2.2 的既有形状一致。
- **边界**：
  - 两个对象之间的运算（布尔、实体间最近距离、实体–实体求交）不属于任何单一对象 → **自由函数**，放在两者共同的模块。
  - 需要后端或 IO 的运算（STEP 解析、三角化）→ `brep`，不进 `geometry`（`geometry-parametric.md` 2.5）。
  - `SphericalSurface` / `CylindricalSurface` 这类**无界或裁剪曲面**只在消费者要求“不依赖 OCCT 的裁剪面”时才加（`../backlog/geometry.md` G6）；OCCT 的 `Geom_*` 已在 B-rep 一侧提供同一概念，提前再造一份就是第二套算法（`G1`）。
- **不因“类名更数学”而拆**：`Sphere` 既是实体，也是球面族的解析描述；拆成 `Sphere` + `SphericalSurface` 会让半径出现两个家。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 族分类 | 三个空标签，`kDimension` = 1/2/3 | 已落地 |
| F2 | 曲线参数化 | `Line` / `Ray` / `LineSegment` / `Circle` 的 `pointAt` | 已落地 |
| F3 | 球面参数化 | `SphericalSurface` 的方位角 / 极角与法向 | 已落地 |
| F4 | 解析包围盒 | 每个新类型的 `bounds()` 返回精确或紧致的 `Aabb<T>` | 已落地 |

### 3.1 F1 族分类

- **约束**（C1）：标签无状态、无虚函数；族的归属只在类型定义处声明一次。
- **验收**（T1）：`GeometryKindsTest.cpp` 的 `static_assert` 覆盖归属、非归属与大小。

### 3.2 F2 曲线参数化

- **行为**：`LineSegment::pointAt(t)` 在 [0,1] 上线性；`Circle::pointAt(angle)` 由 `normal` 导出的正交基行走；`Line::pointAt(t)` 与 `Ray::pointAt(t)` 沿用各自既有语义。
- **约束**（C2）：退化输入不产生 NaN——零长度段返回零方向，退化圆返回圆心，非法状态由 `isValid()` 报告。
- **验收**（T2）：端点、方向、周长与平面内行走各一例。

### 3.3 F3 球面参数化

- **行为**：`SphericalSurface::pointAt(azimuth, polar)` 极角自 +Z 量起、方位角绕 +Z（与 OCCT 的球面一致）；`normalAt` 返回背离圆心的单位向量。
- **约束**（C3）：退化半径（≤ eps）时 `isValid()` 为假，`normalAt` 返回零向量而不是 NaN。
- **验收**（T3）：角点对应、中心点、法向与共线退化。

### 3.4 F4 解析包围盒

- **行为**：线段/三角形/四边形取角点盒；`Circle::bounds()` 按 `radius * sqrt(1 - n_i²)` 逐轴取半宽，是圆盘的精确盒。
- **约束**（C4）：`bounds()` 是**局部**空间的盒，世界盒要乘节点变换。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `Curve.hpp` / `Surface.hpp` | `Curve`；`Surface` + `ElementarySurface` / `BoundedSurface` + 六个占位曲面 |
| `LineSegment.hpp` / `Circle.hpp` / `SphericalSurface.hpp` | 新增的曲线与球面值类型及 `*f` / `*d` 别名 |

## 5. 约束、要求与不变量

- **C1**：标签不携带状态、不声明虚函数。
- **C2**：退化输入不产生 NaN；`isValid()` 是唯一的判定入口。
- **C3**：族的归属在类型定义处声明一次，不在别处重复。
- **N1**：`Line::pointAt(t)` 的 `t` 乘的是 `direction` 的长度，只有单位方向时它才是距离；`Ray` 的方向恒为单位，所以两者同名不同量纲。
- **N2**：占位曲面不可构造（删除默认构造），类型树先行、行为等消费者；`Plane` 是唯一不带 `Surface` 后缀的曲面（平面没有第二个含义，OCCT 同样如此）。
- **R1**：`pointAt` / `bounds` / `isValid` 不分配、不抛异常。
- **R2**：`geometry` 不因为族分类新增任何依赖；标签只 include `geometry_global.hpp`。

## 6. 注意事项与陷阱

- **P1**：往标签里加虚函数 = 给每个值类型加 vptr（`Ray` 在热循环里按值拷贝）。要共享算法就写自由函数。
- **P2**：所有 `bounds()` 都是**局部**空间的盒，世界盒要乘节点变换；当成世界包围盒缓存会错在场景层级上。

## 7. 已否决选项

- **D1** `Curve` / `Surface` 做成虚基类：给所有值类型加 vptr，打掉内联与 trivially copyable（`G10`、`G8`）。见 `../decisions/ADR-2026-11-04-geometry-kinds.md`。
- **D2** 只提供 concept、不提供标签：`is_base_of` 这类编译期问法与"族"的文档位置都会消失，而且运行期仍需要 variant 覆盖同一集合（`G1`）。
- **D3** 把参数含义提升进基类（统一 `pointAt(t)`）：每种曲线的 `t` 语义不同，统一签名只会把差异藏进文档（`G4`）。

## 8. 测试与验收

| 验收 | 用例 |
|---|---|
| T1 归属、非归属与零成本 | `tests/test_geometry/GeometryKindsTest.cpp` 顶部 `static_assert` |
| T2 曲线参数化与退化输入 | 同文件 `aLineSegmentSpansItsTwoEndPoints`、`aLineAnswersThePointAtItsParameter`、`aCircleWalksItsOwnPlane`、`aCircleWithoutANormalIsInvalid` |
| T3 球面参数化与退化输入 | 同文件 `aSphericalSheetPutsPolarZeroAtThePole`、`aCircleWithoutANormalIsInvalid` |

## 9. 状态与演进

- 两个族、曲面分 `ElementarySurface` / `BoundedSurface`、四种新值类型、归属与零成本断言均已落地。
- 六个曲面是**占位**（类型在树里、不可构造、无参数无算法），等不依赖 OCCT 的裁剪面消费者（`../backlog/geometry.md` G6）。
- 已知缺口：族级算法（射线–曲面、点到曲线距离、实体求交）尚不存在；`Curve` / `Surface` 也还没有 concept 形式的约束，等出现第一个泛型算法时再加。
- 兼容性：新增派生不改变任何既有类型的布局与语义；改动是纯增量。

## 10. 参考

- `geometry-primitives.md`（`Aabb` / `Obb` / `Line` / `Plane` 的归属与头文件规则）
- `geometry-parametric.md`（参数化几何与 `Primitive`）
- `geometry-ray.md`（`Ray` 的契约与内联理由）
- `../decisions/ADR-2026-11-04-geometry-kinds.md`
