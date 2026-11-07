# geometry 参数化几何与模型数据

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`src/base/geom/sdk/vine/geom/spatial` 中参数化几何（`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`）作为**建模类型**的归属与契约，以及 `Primitive` 与几何算法的边界。
> 不含网格 / B-rep / 点云的存储与 revision（见 `graphics-geometry.md`），不含曲线与曲面族的分类（见 `geometry-kinds.md`，轴对称曲面与样条曲面见 `geometry-axis-surfaces.md`），不含射线–图元契约（见 `geometry-ray.md`）。
> **权威来源**：`src/base/models/sdk/vine/models/Primitive.hpp`、`src/base/models/sdk/vine/models/PrimitiveDispatch.hpp`、`src/base/models/src/Primitive.cpp`；`tests/test_geom_spatial`、`tests/test_brep`、`tests/test_robotics_io`。
> **待办**：`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

参数化几何是两件事的交汇点，而这两个需求方向相反：

- **建模**需要一种能放进场景、有身份、能被序列化的对象，回答"我是什么形状、多大、放在哪"。
- **算法**需要一种便宜的值，回答"这条射线打到哪里""这个盒的包围盒是什么"。

若把两者压成一个类型，建模对象会带进算法；若拆成两套类型，参数就有两个家。本仓库的取舍是：**参数化几何留在建模侧（`Primitive` 子类），算法留在几何值侧**，因为这类形状本身几乎没有算法：射线–盒就是 `Aabb::intersectWith(Ray)`，射线–球面属于曲面（`SphericalSurface`，见 `geometry-kinds.md` 2.10）。

### 1.2 目标

- `Primitive` 是参数化几何的唯一基类：五个子类各带自己的尺寸，`ShapeType` 由子类在构造时交给基类，标签不可能与类不符。
- 每个子类一个头文件：`models/Box.hpp` … `models/Ellipsoid.hpp`，且各有一个 `.cpp`；`PrimitiveDispatch.hpp` 只放 closed set 的分派。
- 几何算法不进 `Primitive`：实体只回答 `bounds()`、`isValid()`、`hasVolume()`。

### 1.3 非目标

- 不做值类型化的参数化几何：`Box` 等是 `Shape`，带身份与 revision；需要"只用尺寸算一算"的调用方应使用 `Aabb` / `Obb` / `Plane` / `SphericalSurface` 这些值类型。
- 不做参数化几何的距离中的**实体间**距离：点到实体的最近点 / 距离与射线求交已由族 `Solid<T>` 解释（`geometry-solid.md`）；两实体之间的距离仍没有消费者（`G8`）。
- 不让 `geometry` 依赖 OCCT：后端词汇与解析式构造留在 `brep`。

### 1.4 一句话结论

**参数化几何是建模类型（`Primitive` 的五个子类，各自一个头文件）；基类不持有几何，实体由子类按需构造；几何算法属于值类型与曲面；`models` 依赖 `geometry`，反向不依赖。**

## 2. 设计要点

### 2.0 为什么不是值类型（本日先做过，后撤回）

- **曾经的做法**：把五个尺寸做成无身份的值类型，`Primitive` 用 `std::variant` 持有，消费点用 `std::visit` / `as<T>()`。
- **撤回的原因**：`Box` / `Cone` 这类形状不提供自己的算法（射线–盒是 `Aabb` 的 slab，射线–球面属于曲面），把它们放进"算法值"的一侧会让 `Solid` 含义落空、让半径/边长在实体与曲面之间出现两个家；而建模侧需要的是身份、名字与 revision。
- **代价（已接受）**：`Box` 等带 vptr 与引用计数，复制不是 memcpy；加一种形状要动 `ShapeType`、`Shape.cpp` 的两个 switch、`brep` 的构造与 `robotics::io` 的读写。记录在此，避免重复论证。

### 2.1 `Primitive` 是抽象基类，每个子类一个头文件

- **是什么**：`class Primitive : public Shape` 只有受保护构造 `Primitive(ShapeType)` 与一个纯虚 `bounds()`，**不持有任何几何值**；`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 各在一个头文件（`models/Box.hpp` … `models/Ellipsoid.hpp`），各自存尺寸（全量纲）并各自给出 `solid()`。
- **目录边界**：带身份的类型（`Shape` / `Primitive` / `Model` / 网格 / `BrepShape` / `PointCloud` / `Material`）住在 `src/base/models`（`sdk/vine/models/`），几何值类型（曲线、曲面、`Solid`、`Aabb` / `Obb` / `Ray`）住在 `src/base/geom/sdk/vine/geom/spatial`。`models` 单向依赖 `geometry`。
- **为什么**：`G1`、`G12` —— 一个类型一个文件，消费者只 include 它写到的那个形状；`PrimitiveDispatch.hpp` 单独承担 closed set 的两个入口（`visitPrimitive()` / `solidShapeOf()`）。每个子类的 `.cpp` include 自己的头，DLL 导出由该 TU 生成，不再依赖“某个 TU 记得 include 全部类型”。
- **边界**：`Primitive` 自身不声明任何几何查询（没有 `pointAt`、没有 `intersectWith`、没有 `solid()`）。"是不是实体"是子类的属性：将来出现轮廓、扫掠面这类图元，直接派生而不用改基类（`SolidTest.cpp` 的 `Profile` 就是这条判据的验收）。

### 2.2 标签由子类交给基类

- **是什么**：每个子类的构造函数委托给 `Primitive(ShapeType::Box)` 这类调用，`shape_type_` 只在基类构造函数里写一次。
- **为什么**：`G11` —— 旧实现里五个构造函数各自写 `shape_type_ = ...`，写错要到很远的地方才暴露；交给基类后，"一个类只能报自己的类型"由构造链保证。
- **边界**：仍可再派生子类并报一个不存在的类型（`ConstructionTest` 的 `StrayPrimitive` 就测这一点）；消费方按 `switch (shapeType())` + `obj_cast` 双重校验，`brep` 与 `robotics::io` 都这么做。

### 2.3 canonical 局部空间

- **是什么**：盒与椭球以原点为中心；圆柱与圆锥的轴在 +Z 上、跨 `[-height/2, +height/2]`，圆锥底面在 `-height/2`。
- **为什么**：`G4`、`G12` —— 这是 `brep` 构造、机器人 IO 与既有测试共同假设的空间；摆放属于节点 transform，形状本身不带坐标系。

### 2.4 算法不在 `Primitive` 上

- **是什么**：模型只回答 `bounds()`、`isValid()`、`hasVolume(eps)`，而且三个都由子类**转发**给一次 `solid()` 构造；需要几何时用两种入口之一：每个子类的 `solid()`（具体 kind 的值）或自由函数 `solidShapeOf(primitive)`（`std::optional<SolidShapef>`，不是实体时为空）。三角化是 `tessellateSolid(solidShapeOf(primitive))`（`geometry-tessellation.md`）。
- **为什么**：`G13`、`G1`、`G8`、`G12` —— 建模类型的职责是身份 / 名字 / revision，几何类型的职责是公式；几何查询挂在模型上会让同一形状的公式散在多处，也让模型消费者认识算法入口。`G13` 不看有没有新消费者：把已有能力放到它该在的一侧，本身就是一条约束。
- **边界**：模型侧没有按 `ShapeType` 分派几何的代码；`Solid<T>` 不认识 `Shape` / `Primitive`，两者只在 `Primitive` 一处相接（`geometry-solid.md` 2.1）。

### 2.5 OCCT 只做 STEP 解析与三角化

- **是什么**：`brep` 的长期职责是"读 STEP / IGES 得到 `BrepShape`，把它三角化成 `TriangleMesh`"；`brep::makeBrep(Primitive)` 是用 `BRepPrimAPI` 把参数化形状变成解析实体的**过渡桥**。
- **为什么**：`G2`、`G5` —— 后端词汇不进 `geometry`；没有后端时 `hasBackend()` 为假、`makeBrep` 返回空句柄，契约照旧。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 建模类型 | `Primitive : Shape` + 五个子类（同一头文件），`shapeType()` 由构造链确定 | 已落地 |
| F2 | 形状契约 | `isValid()` / `hasVolume(eps)` / `bounds()`（canonical 局部空间） | 已落地 |
| F3 | B-rep 桥 | `brep::makeBrep(Primitive)` 按类型分派，无后端返回空 | 已落地（本机构建含 OCCT 后端） |
| F4 | 序列化 | `robotics::io` 按 `ShapeType` 读写五种几何元素 | 已落地 |
| F5 | 三角化 | `tessellateSolid(Solid<float>, quality)`：五个解析体变成闭合、外向、带精确法向的 `TriangleMesh`（见 `geometry-tessellation.md`） | 已落地 |
| F6 | 模型 → 几何 | 每个子类 `solid()`（按当前尺寸构造的具体 kind）、`solidShapeOf()`（可选值）、`visitPrimitive()`（交给 closed set 的具体类型） | 已落地 |
| F7 | 实体查询 | `Solid<T>::closestPoint()` / `intersectWith(Ray)`：实体语义的最近点 / 距离与射线交点，模型不参与（见 `geometry-solid.md`） | 已落地 |

### 3.1 F1 建模类型

- **约束**（C1）：子类只能通过受保护构造报告自己的 `ShapeType`；`Primitive` 不可直接实例化。
- **验收**（T1）：`tests/test_brep/ConstructionTest.cpp` 的 `aPrimitiveOutsideTheFiveProducesNoSolid`。

### 3.2 F2 形状契约

- **行为**：默认构造给出单位尺寸；`bounds()` 返回局部空间 `Aabbf`（盒由 `Solid<T>` 派生，不在子类里重算，见 `geometry-solid.md` 2.1）；非法尺寸由 `isValid()` 报告而非构造拒绝。
- **约束**（C2）：`Cone::bounds()` 取底面的外接圆柱盒。
- **验收**（T2）：`everyPrimitiveFillsItsAnalyticBounds` 用三角化后的包围盒核对五种形状的 canonical 空间。

### 3.3 F3 B-rep 桥

- **行为**：`brep::makeBrep(Primitive)` 按 `shapeType()` 分派到 `BRepPrimAPI` 构造解析实体；无后端时 `hasBackend()` 为假、返回空句柄；非法尺寸的实体不构造实体。
- **约束**（C4）：`geometry` 不认识 OCCT——桥住在 `brep`，`R2` 的方向不变。
- **验收**（T3）：`tests/test_brep/ConstructionTest.cpp` 的 `aBuildWithoutABackendProducesNoSolid`、`anInvalidPrimitiveProducesNoSolid`、`theSolidCarriesThePrimitiveName`（后两个在无后端构建里提前返回）。

### 3.4 F4 序列化

- **行为**：`robotics::io` 读 URDF / 自定义 XML 时按标签构造子类，写回时按 `ShapeType` 分派，并校验 `obj_cast` 成功。
- **验收**（T4）：`tests/test_robotics_io/DeviceIOTest.cpp`、`PkgIOTest.cpp`。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `models/Primitive.hpp` | `Primitive`（受保护构造，接受 `ShapeType`） |
| `models/Box.hpp` … `models/Ellipsoid.hpp` | 五个子类，每个类型一个头文件与一个源文件 |
| `models/PrimitiveDispatch.hpp` | `visitPrimitive()` 与 `solidShapeOf()` |
| `models/SolidTessellation.hpp` | `TessellationQuality` 与 `tessellateSolid()`（三角化见 `geometry-tessellation.md`） |
| `geometry/Solid.hpp` | `Solid<T>` 接口与 `Solidf` / `Solidd`（见 `geometry-solid.md`） |
| `geometry/SolidShape.hpp` | `SolidShape<T>`、`solidOf()` 与 `SolidShapef` / `SolidShaped` |
| `models/Shape.hpp` | `ShapeType`、`isValid()`、`hasVolume()`、`revision()` |

## 5. 约束、要求与不变量

- **C1**：`Primitive` 不可直接实例化；子类的 `shape_type_` 只能由自己的构造链给出。
- **C2**：所有 `bounds()` 是 canonical 局部空间的盒，世界盒要乘节点变换。
- **C3**：尺寸非正即非法；非法形状不构造实体，但 `bounds()` 仍是良构的盒。
- **N1**：`Shape::hasVolume()` 的默认容差是 `double`，子类按 float 尺寸给默认值；显式传 `eps` 时以调用方为准。
- **N2**：五个子类都是 `final`，不再允许"报一个不存在的 `ShapeType`"的派生；`brep` / `robotics::io` 用 `visitPrimitive` 按 closed set 分派，不属于五个的图元返回 `false`。
- **N3**：子类不存实体、不缓存 `solid()`：每次调用都按当前尺寸构造一个值，所以改完尺寸下一次查询就生效，没有失效点；反过来，调用方不能保存 `solid()` 返回值的引用。
- **R1**：`isValid()` / `hasVolume()` / `bounds()` 不分配、不抛异常。
- **R2**：`brep` 可以 include `geometry`，`geometry` 不 include `brep`（`G2`）。

## 6. 注意事项与陷阱

- **P1**：`bounds()` 是**局部**空间的盒；世界盒要乘节点变换（`graphics-geometry.md` F15/F21）。
- **P2**：`setWidth()` 之类不校验：把边长设成 0 后，形状仍在，只由 `isValid()` 报出。

## 7. 已否决选项

- **D1** 五个尺寸做成无身份值类型 + `Primitive` 用 `std::variant` 持有：`Box` / `Cone` 没有自己的算法，"值"这一侧换不到东西，反而让半径/边长在实体与曲面之间有两个家（`G1`）。见 `../decisions/ADR-2026-11-04-primitive-is-a-shape.md`。
- **D2** 让 `geometry` 依赖 OCCT 直接构造解析实体：后端词汇进核心模块（`G2`），无后端构建会失去全部契约。
- **D3** 在 `Primitive` 上开放几何查询（虚 `intersectWith`）：算法面挂在建模类型上，消费者要记住查谁；几何属于族接口 `Solid<T>` 与它的五个 kind（`G1`、`G8`）。
- **D4** 以模型为参数写几何自由函数（`f(primitive)` 形态）：每个能力各写一份标签分派，同一形状的公式散在多处；几何值由子类的 `solid()` 一次给出后，算法在几何一侧只有一套（`G1`、`G12`）。
- **D5** 让 `Primitive` 按值持有一个 `SolidShapef`（2026-11-05 的中间形态，已撤回）：基类因此假设"图元必然是实体"，轮廓 / 扫掠面这类图元无处安放（`G1`）。见 `../decisions/ADR-2026-11-05-primitive-is-a-modelling-base.md`。

## 8. 测试与验收

| 验收 | 用例 |
|---|---|
| T1 非五种形状不产生实体 | `tests/test_brep/ConstructionTest.cpp` `aPrimitiveOutsideTheFiveProducesNoSolid` |
| T2 canonical 包围盒 | 同文件 `everyPrimitiveFillsItsAnalyticBounds` |
| T3 B-rep 桥 | 同文件 `aBuildWithoutABackendProducesNoSolid`、`anInvalidPrimitiveProducesNoSolid`、`theSolidCarriesThePrimitiveName` |
| T4 序列化往返 | `tests/test_robotics_io/DeviceIOTest.cpp`、`PkgIOTest.cpp` |

## 9. 状态与演进

- 五个子类、受保护构造与单头文件组织已落地；`brep` 与 `robotics::io` 按任务书分派。
- 本机构建含 OCCT 后端（`VN_HAS_OCCT=TRUE`，vcpkg 的 `opencascade`），`test_brep` 可构建并通过，F3 的实体构造分支因此是真跑过的；无后端的构建仍走 `hasBackend()` 为假的路径。
- 下一步：曲线 / 曲面族已无占位类型，见 `geometry-kinds.md` 2.10 与 `geometry-axis-surfaces.md`；三角化、射线求交与最近点 / 距离已全部落在族 `Solid<T>` 上（`geometry-solid.md`、`geometry-tessellation.md`，F5–F7），本主题没有开放项。

## 10. 参考

- `geometry-kinds.md`（`Curve` / `Surface` 族与曲面树）、`geometry-primitives.md`（值类型的归属与头文件规则）、`geometry-solid.md`（解析实体的值类型：最近点与射线求交）、`geometry-tessellation.md`（三角化）
- `graphics-geometry.md`（网格存储与渲染边界）
- `../decisions/ADR-2026-11-04-primitive-is-a-shape.md`、`../decisions/ADR-2026-11-05-primitive-is-a-modelling-base.md`
