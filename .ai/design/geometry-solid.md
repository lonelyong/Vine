# geometry 解析实体 Solid

> **状态**：现行
> **最后核对**：2026-11-05
> **范围**：`Solid<T>` 族（五个解析体：`BoxSolid` / `SphereSolid` / `CylinderSolid` / `ConeSolid` / `EllipsoidSolid`）、它的两个查询（最近点、射线求交），以及模型侧如何给出这份实体（每个建模子类的 `solid()`、`solidShapeOf()`、`visitPrimitive()`）。
> 不含三角化（见 `geometry-tessellation.md`），不含实体间运算（布尔、实体间距离，见 `../backlog/geometry.md` G3），不含曲面族查询（见 `geometry-surface-queries.md`）。
> **权威来源**：`src/base/geometry/sdk/vine/geometry/Solid.hpp`、`SolidShape.hpp`、`BoxSolid.hpp` … `EllipsoidSolid.hpp`；`tests/test_geometry/SolidTest.cpp`、`SolidDistanceTest.cpp`、`SolidIntersectionTest.cpp`。
> **待办**：`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

`Primitive` 是**建模**类型：它带身份、名字与 revision，回答的是“这个连杆的几何是什么”，不是“算一算这个几何”。几何算法最初直接挂在它身上——三角化、射线求交、点到实体距离各是一个以 `shapeType()` 为唯一入口的自由函数。后果有三：同一个形状的公式分散在三个文件里；模型的消费者（序列化、场景树）被迫认识几何入口；每加一个能力就要再写一遍标签分派。

值类型一侧又缺少“实体”这个词汇：`Aabb` / `Obb` 是盒，五个曲面是无盖、无内部的壳，`closestPointOnSurface()` 答不了实体语义的“点到实体的距离”（内部为 0）。

### 1.2 目标

- 一个**族**：`const Solid<T>&` 能接受任意一种实体，并对它提问——这是“参数必须是具体类型”的消费者要的东西（一个导出签名、一个后端接口）。
- 一个**封闭值集**：五个 kind 按值存放，模型一次持有，没有指针、没有堆、没有共享所有权。
- 模型侧只剩一次转换：每个建模子类用自己的尺寸构造自己的 kind（`Box::solid()`），`solidShapeOf()` 给出可选值；几何不认识 `ShapeType`。
- 加一种 kind 时，编译器把“哪些消费者必须处理它”列出来。

### 1.3 非目标

- 不做实体间的布尔与实体间最近距离：结果类型在 `geometry` 里装不下，没有消费者（G3，走 `brep` + OCCT）。
- 不做有表示的实体：NURBS 体、网格体、布尔结果属于 `BrepShape`，永远不是 `Solid`（`G13`）。
- 不做 `Solid<T>` 的三角化成员：`TriangleMesh` 是 float，成员化会让 `Solid<double>` 隐式降精度（`geometry-tessellation.md` 2.1）。
- 不做双精度与单精度之间的隐式转换：要 `double` 的调用方显式构造 `BoxSolidd` / `SphereSolidd` / …（2.6）。

### 1.4 一句话结论

**`Solid<T>` 是族接口（五个虚查询），五个 kind 是它的值；不认识 kind 的消费者问族接口，必须按 kind 作答的消费者用 `solidShapeOf()` 拿到 `SolidShape<T>` 再 visit（或 `visitPrimitive()` 直接拿具体建模类）。**

## 2. 设计要点

### 2.1 两个层次，两个问题

- **是什么**：`Solid<T>`（抽象，五个纯虚：`isValid` / `hasVolume` / `bounds` / `closestPoint` / `intersectWith`）回答“这个实体怎么样”；`BoxSolid<T>` … `EllipsoidSolid<T>` 是它的值；`SolidShape<T> = std::variant<五个 kind>` 是模型存的那份值。
- **为什么**：`G13`、`G1` —— 有些参数必须是具体类型，而不是模板：一个导出签名、一个后端接口只能写 `const Solid<float>&`。接口层给出的正是这个类型；variant 层给出的是“按 kind 作答”的穷尽分派。两层各回答一个真问题，不是同一件事做两遍。
- **边界**：`solidOf(shape)` 是两层之间唯一的转换（`std::visit` 取引用，零成本、无拷贝）；模型的入口是子类的 `solid()`（返回具体 kind 的值）与自由函数 `solidShapeOf()`（可选值，不是实体时为空）。

### 2.2 封闭集合仍然按值持有

- **是什么**：`SolidShape<T>` 按值持有五个 kind；模型不再存它：建模子类存自己的尺寸，`solid()` 每次构造一个值。
- **为什么**：`G10`、`G13` —— 模型对象本来就在堆上（`Shape`），那 24–32 字节躺在一个已经存在的块里，**零额外分配**；换成 `unique_ptr<Solid>` 就是每个连杆一次分配加一次解引用，换成 `shared_ptr` 还违反“模型与几何不共享所有权”。
- **边界**：`SolidShape<T>` 是 `std::variant`，所以 kind 的集合是封闭的；`BrepShape` 才是“任意实体”的出口。

### 2.3 visit 与虚调用各管一段

- **是什么**：三角化、B-rep 桥、描述文件写入者 visit `SolidShape`；`Primitive::isValid()`/`hasVolume()`/`bounds()` 与“只问一个问题”的消费者走虚函数。
- **为什么**：`G9`、`G11` —— 需要“每种 kind 一个答案”的地方（网格、`BRepPrimAPI` 调用、XML 元素）必须穷尽，`std::visit` 漏一个 kind 是编译错误；只需要“一个答案”的地方（包围盒、有效性、射线交点）用接口，调用点不关心 kind。
- **边界**：`std::visit` 的 visitor 只用一次的地方写 lambda，重复使用的写具名结构（例如三角化的 `TessellateVisitor`）。

### 2.4 canonical 局部空间与半量纲

- **是什么**：盒 / 球 / 椭球居中于原点；圆柱 / 圆锥的轴在 +Z、跨 `[-half_height, +half_height]`，圆锥底面在 `-half_height`。`BoxSolid` 存**半**边长，圆柱与圆锥存**半**高。
- **为什么**：`G4`、`G12` —— 该空间的公式一律用半量纲书写（盒是三次 clamp，圆柱是径向 + 轴向 clamp），字段名 `half_x` / `half_height` 自己说明是哪一半；全量纲只出现在模型描述文件那一侧。
- **边界**：摆放属于持有形状的节点变换，`Solid` 没有坐标系、没有放置、也没有“旋转过的实体”。

### 2.5 两个查询的结果形态与默认容差

- **是什么**：`closestPoint(query, tolerance = EPS)` 返回 `std::optional<SolidProjection<T>>`；`intersectWith(ray, eps = EPS)` 返回升序去重的 `std::vector<RayHit<T>>`。
- **为什么**：`G4`、`G6` —— 单值答案用可选值、变长答案用集合，与 `closestPointOnSurface()` / `intersectCurveWithSurface()` 一致；默认容差写在**接口与每个 kind 两处**，因为派生类的声明会遮蔽基类的默认参数（P5）。
- **边界**：`Aabb::intersectWith(Ray)` 仍是 `bool` + 出参——那是“一个命中”的问题，形态由问题决定。

### 2.6 尺寸是 float，更高精度是显式选择

- **是什么**：模型存 `float`；`BoxSolidd` / `SphereSolidd` / … 是 `double` 的 kind，需要它们的调用方自己构造。
- **为什么**：`G4` —— 描述文件读进来的就是 float，网格也是 float（`TriangleMesh`）；没有“隐式把 float 实体当 double 算”的转换，就不会有“以为拿到了 double 尺寸”的误解。
- **代价**：跨精度要显式写（构造 `BoxSolidd{...}`）；换精度不是一行 cast，而是一次显式的类型选择。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 族接口与五个 kind | `Solid<T>` 五个纯虚；`BoxSolid` / `SphereSolid` / `CylinderSolid` / `ConeSolid` / `EllipsoidSolid` 各持量与公式 | 已落地（T1、T2） |
| F2 | 点到实体的最近点 / 距离 | `closestPoint(query, tolerance)`：实体语义，内部为 0 | 已落地（T3–T6） |
| F3 | 射线 × 实体 | `intersectWith(ray, eps)`：升序、最多两个、法向朝射线 | 已落地（T4、T7） |
| F4 | 模型给出实体 | 每个建模子类的 `solid()`（值）、`solidShapeOf(Primitive)`（可选值）、`visitPrimitive()`；`solidOf(SolidShape)` 取族引用 | 已落地（T2、T8） |
| F5 | 三角化 | `tessellateSolid(SolidShapef, quality)` | 见 `geometry-tessellation.md` |

### 3.2 F1 接口与 kind

- **行为**：五个 kind 各自 `final`，持有半量纲与自己的公式；每个都可以默认构造（全 0 量纲），由 `isValid()` 拒绝。
- **约束**（C1、C2）：空间与量纲见 2.4；量纲非正即不可用。
- **验收**（T1）：`SolidTest.cpp` 的 `theFamilyIsAnInterfaceAndTheKindsAreValues`、`theBoundsAreTheAnalyticExtentOfEachKind`。

### 3.3 F2 最近点与距离

- **行为**：盒三次 clamp；球径向缩放；椭球缩放后的 Lagrange 一维求根；圆柱径向 × 轴向 clamp；圆锥在“底面盘”与“母线轮廓线段”两个候选中取较近者。查询在实体内部时返回查询点本身、距离 0。
- **约束**（C3）：不可用的实体答 `std::nullopt`。
- **要求**（R1）：除返回值外不分配；椭球是唯一迭代的 kind，上限 256 次二分。
- **验收**（T3–T6）：四个 kind 各一个用例，外加“不可用输入”与“两种精度一致”。

### 3.4 F3 射线 × 实体

- **行为**：盒用 slab；球 / 椭球用二次方程（椭球先缩放到单位空间）；圆柱是侧面二次方程加 `|z| <= half_height` 过滤，再加两个端面圆盘；圆锥是侧面二次方程加轴向过滤，再加底面圆盘。结果按 `t` 升序、在 `eps` 内去重。
- **约束**（C4、C5）：只报 `t ≥ -eps` 的边界交点；法向是外向法向再转向射线。
- **注意**（N4）：圆锥顶点命中时法向为零向量。
- **验收**（T7）：`SolidIntersectionTest.cpp` 的九个用例。

### 3.5 F4 模型持有与族视图

- **行为**：建模子类按当前尺寸构造 `SolidShapef`（`Box::solid()`），`solidShapeOf()` 把任意 `Primitive` 转成 `std::optional<SolidShapef>`，`solidOf()` 用一个 `std::visit` 把 variant 转成 `const Solidf&`。
- **约束**（C6）：`Solid<T>` 不认识 `Shape` / `Primitive`；模型侧没有按 `ShapeType` 分派几何的代码。
- **验收**（T2、T8）：`theTwoViewsOfOneSolidAgree`、`theModelHoldsTheGeometryAndDerivesItsTag`。

## 4. 公开接口

### 4.1 族与 kind

| 接口 | 契约 | 备注 |
|---|---|---|
| `Solid<T>` | 五个纯虚：`isValid()`、`hasVolume(T)`、`bounds()`、`closestPoint(query, tolerance)`、`intersectWith(ray, eps)` | 完整签名见 `Solid.hpp` |
| `BoxSolid<T>` / `SphereSolid<T>` / `CylinderSolid<T>` / `ConeSolid<T>` / `EllipsoidSolid<T>` | `final` 实现者，持半量纲 | 公开字段 `half_x`、`radius`、`half_height`、`base_radius`、`radius_x/y/z` |
| `SolidShape<T>` | `std::variant` 五个 kind | 模型存的那份值 |
| `solidOf(SolidShape<T> const&)` | 取族引用 | 零拷贝；引用跟随 variant 的内容 |
| `Solidf` / `Solidd`、`BoxSolidf` / `BoxSolidd`、… | 两种精度的别名 | 与 `Aabbf` / `Aabbd` 同一命名法 |

### 4.2 模型侧

| 接口 | 契约 | 备注 |
|---|---|---|
| `Box::solid()` … `Ellipsoid::solid()` | 具体 kind 的值 | 子类用自己的尺寸构造 |
| `solidShapeOf(const Primitive&)` | `std::optional<SolidShapef>` | 不是五个实体之一时为空 |
| `visitPrimitive(primitive, visitor)` | `bool` | 把具体建模类交给 visitor |
| `Primitive::isValid()` / `hasVolume(double)` / `bounds()` | 子类转发给 `solid()` | 不重算任何公式 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- **C1**：所有量纲在 canonical 局部空间；摆放属于节点变换，族没有坐标系。
- **C2**：`BoxSolid` 存半边长，`CylinderSolid` / `ConeSolid` 存半高。
- **C3**：不可用的实体（任一半量纲 ≤ 0）答 `std::nullopt` / 空结果，不抛异常、不除零。
- **C4**：射线结果只含 `t ≥ -eps` 的交点，升序，在 `eps` 内去重。
- **C5**：法向是外向法向再转向射线（`RayHit` 的规则）；奇异点（圆锥顶点）给零向量。
- **C6**：`Solid<T>` 不认识 `Shape` / `Primitive`；`Primitive` 是唯一拥有这两侧的类型。

### 5.2 要求（R#）

- **R1**：`isValid()` / `hasVolume()` / `bounds()` / `closestPoint()` 不分配；`intersectWith()` 只分配结果 `vector`。
- **R2**：不依赖 OCCT，也不依赖 `brep`（`G2`）。
- **R3**：header-only 模板；两种精度都在 `test_geometry` 里被实例化。

### 5.3 不变量（N#）

- **N1**：`SolidShape<T>` 恒持有一个 kind；`std::visit` 的分派没有“空”分支。
- **N2**：`bounds()` 对退化实体仍返回良构盒（min ≤ max）。
- **N3**：内部起点时射线最多报一个交点；相切（含双根）报一个。
- **N4**：`Primitive::shapeType()` 恒等于 `shape()` 里那个 kind 的类型，由 `setSolid()` 一处维护。

## 6. 注意事项与陷阱

### 6.1 实体语义与面语义

- **会怎么错**：拿 `closestPoint()` 求“点到球面”的距离，得到 0 而不是到球壳的距离。
- **怎么判**：查询点在实体内部时距离为 0。
- **怎么防**：要面语义就用 `SphericalSurface` / `closestPointOnSurface()`，两者不是同一个问题（2.2 的判据表）。

### 6.2 圆锥顶点的零法向

- **会怎么错**：射线穿过顶点时 `hit.normal` 是零向量，用它算光照得到全黑。
- **怎么判**：`t` / `point` 正常而 `normal.length() == 0`。
- **怎么防**：C5；需要顶点法向时由调用方按侧面极限自己算。

### 6.3 射线躺在面内

- **会怎么错**：以为“躺在面上”应报无穷多点或一个重合标记。
- **怎么判**：结果只有两个端点。
- **怎么防**：实体的交是一个区间，报告的是它的两个端点（C4）。

### 6.4 尺寸精度

- **会怎么错**：以为 `Primitive::solid()` 给的是 `double`。
- **怎么判**：类型是 `const Solid<float>&`，编译期就能看出来。
- **怎么防**：2.6；要高精度就显式构造 `BoxSolidd` / `SphereSolidd` / …

### 6.5 派生类会遮蔽基类的默认参数

- **会怎么错**：只在 `Solid<T>` 上写 `= EPS<T>()`，然后通过 `BoxSolidf` 调用 `closestPoint(query)`，编译报“参数太少”。
- **怎么判**：通过具体 kind 调用的地方编译不过，通过 `const Solidf&` 调用的地方正常。
- **怎么防**：默认值在接口与每个 kind 的声明上各写一次（2.5）；调用点不要自己重复传 `EPS`。

## 7. 已否决选项

- **D1** `Solid<T>` 直接就是 `std::variant` 的别名（没有接口层）：失去“参数是具体类型”的能力，每个只问一个问题的消费者都得自己 visit（`G7`）。见 `../decisions/ADR-2026-11-05-solid-family-and-variant.md`。
- **D2** 族接口 + `Primitive` 用指针持有：每次模型构造一次堆分配、每次 `bounds()` 一次解引用，并引入“模型与几何共享所有权”的路径（`G10`、`G13`）。同一 ADR。
- **D3** 每个形状一个模型子类（`BoxSolid(Primitive)` …）：`ShapeType` 与类可能漂移、同一个形状的公式散在多处、模型消费者认识算法入口（`G1`、`G12`）。同一 ADR。
- **D4** 把三角化做成 `Solid<T>` 的成员：`TriangleMesh` 是 float，`Solid<double>::tessellate()` 只能降精度输出（`G4`）。见 `geometry-tessellation.md` D5。
- **D5** float / double 实体之间隐式转换：会把“尺寸只有 float 分辨率”这件事藏进调用链（`G4`）。见 2.6。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 接口抽象、五个 kind 是值、variant 的类型就是那五个 | `SolidTest.cpp` 的 `theFamilyIsAnInterfaceAndTheKindsAreValues` |
| T2 | 两种视图指向同一个对象；模型持有并派生标签 | 同文件 `theTwoViewsOfOneSolidAgree`、`theModelHoldsTheGeometryAndDerivesItsTag` |
| T3 | 盒的面 / 角 / 内部 | `SolidDistanceTest.cpp` 的 `aBoxClampsToItsFacesAndCorners` |
| T4 | 球的内外与两交 / 相切 | `aSphereProjectsAlongItsRadius`、`SolidIntersectionTest.cpp` 的 `aTangentialRayTouchesASphereOnce` |
| T5 | 椭球三轴 + Lagrange 条件 | `SolidDistanceTest.cpp` 的 `anEllipsoidProjectsOntoItsSurfaceAlongTheNormal` |
| T6 | 圆柱径向 / 轴向 / 棱，圆锥底面与母线选择 | `aCylinderClampsRadiallyAndAxially`、`aConeChoosesBetweenItsBaseAndItsSide` |
| T7 | 内部起点只报离开、端面命中、去重 | `SolidIntersectionTest.cpp` 的 `aRayFromInsideASolidReportsOnlyTheExit`、`aCylinderIsHitOnItsSideAndOnItsCaps` |
| T8 | 不可用实体、退化量纲、每 kind 的体积判据 | `SolidTest.cpp` 的 `anUnusableModelStillCarriesItsGeometry`、`aDegenerateKindIsUnusableButStillWellFormed`、`everyKindAnswersItsOwnVolume` |

命令：`cmake --build build --config Debug --target test_geometry`，再运行 `build/bin/Debug/test_geometry.exe`。

## 9. 状态与演进

### 9.1 当前状态

- 族接口与五个 kind 已落地；模型持有 `SolidShapef`；三角化是 `tessellateSolid()`（见 `geometry-tessellation.md`）；`brep` 与描述文件写入者都 visit 这份 variant。

### 9.2 已知缺口 / 待办

- 实体间运算（布尔、实体间最近距离）没有消费者，见 `../backlog/geometry.md` G3。
- 新 kind（torus / capsule / 棱柱）：新增一个 `XxxSolid` 头文件，在 `SolidShape.hpp` 的 variant 里加一项，并逐个处理 `visit` 点（编译器会列出来）+ 一个 `ShapeType`；有表示的实体不进这里，走 `BrepShape`。
- 三角化只有 `SolidShapef` 入口：要画 `BoxSolidd` 的调用方自己构造对应 `BoxSolidf`（`geometry-tessellation.md` 9.2）。

### 9.3 兼容性 / 迁移

- 模型侧：`Primitive` 是唯一类，构造用 `Primitive(BoxSolidf{...})`，读取用 `solid()`（族）或 `shape()`（variant）。
- 以模型为参数的三个算法入口（三角化、射线、距离）已删除；替代分别是 `tessellateSolid(p.shape())` 与 `p.solid().closestPoint(...)` / `p.solid().intersectWith(...)`。

## 10. 参考

### 10.1 代码

- `src/base/geometry/sdk/vine/geometry/Solid.hpp`、`SolidShape.hpp`、`BoxSolid.hpp` … `EllipsoidSolid.hpp`
- `src/base/models/sdk/vine/models/Primitive.hpp`、`Box.hpp` … `Ellipsoid.hpp`、`PrimitiveDispatch.hpp`
- `tests/test_geometry/SolidTest.cpp`、`SolidDistanceTest.cpp`、`SolidIntersectionTest.cpp`

### 10.2 文档与 ADR

- `geometry-parametric.md`（建模类型的契约）
- `geometry-kinds.md` 2.6（算法放在几何一侧）
- `geometry-tessellation.md`（同一批实体的网格形式）
- `../decisions/ADR-2026-11-05-solid-family-and-variant.md`
