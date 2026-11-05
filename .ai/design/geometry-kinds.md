# geometry 几何族：Curve 与 Surface

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`Curve` / `Surface` 两个求值接口，以及具体值类型的归属与新增类型的最小契约。
> 参数化几何（`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`）**不构成族**：模型侧是一个建模类 `Primitive`，几何侧是族 `Solid<T>` 的五个 kind（见 `geometry-parametric.md`、`geometry-solid.md`）。
> 不含具体类型的完整契约（见 `geometry-primitives.md`、`geometry-parametric.md`、`geometry-ray.md` 与各自头文件），不含求交与距离算法。
> **权威来源**：`src/base/geometry/sdk/vine/geometry/Curve.hpp`、`Continuity.hpp`、`Surface.hpp`；`tests/test_geometry/GeometryKindsTest.cpp`。
> **待办**：`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

几何类型之间此前没有任何分类关系：`Line`（今 `LineCurve`）、`Ray`、`Plane`、`Box` 都是互不相关的类，"这是一条曲线 / 一个曲面 / 一个实体"这句话只存在于讨论里。想按族写一个约束、想把一族类型放进同一个容器、想回答"这个类型是几维的"，都只能靠命名猜测；运行期分类只有 `ShapeType`，而它只覆盖 `Shape` 子类，值类型根本不在其中。

### 1.2 目标

- 两个族各有一个名字，归属写在类型定义处（`class CircleCurve : public ConicCurve`）。
- 分类可以在编译期查询（`std::is_base_of_v<Curve<double>, CircleCurved>`）与枚举（`Surface::kDimension`）。
- 几何可被当作“未知类型的参数化对象”使用：曲线是求值、导数、参数域、光滑度、反向映射（2.7），曲面是求值、两个一阶导、两个方向的参数域与闭合周期（2.10）。

### 1.3 非目标

- 接口里只放“参数化对象必然能回答的事”：曲面的法向、包围盒、二阶导数与反向/变换都不进接口（2.10），曲线的同理（2.7）。
- 不做异构容器：需要“一列混合曲线”时用 `std::variant` + `std::visit`；接口只回答“一条”曲线 / “一张”曲面（2.4）。
- 不给参数化几何设族标签：它们不属于 `Curve` / `Surface` 树；几何侧是 `Solid<T>` 族（射线–盒仍是 `Aabb` 的 slab，实体语义归 `Solid<T>::intersectWith()`）；哪些形状能组成模型由 `ShapeType` 回答。
- 族级曲面算法（最近点、射线 × 曲面）已在 `geometry-surface-queries.md`，点到曲线的最近点 / 距离已在 `geometry-curve-queries.md`；实体求交仍没有消费者（`G8`），见 `../backlog/geometry.md` G3。

### 1.4 一句话结论

**`Curve` 与 `Surface` 都是求值接口，曲面多一层分类标签；参数化几何不属于任何族，它们只是建模参数。**

## 2. 设计要点

### 2.1 曲线与曲面都是求值接口

- **是什么**：`Curve<T>`（2.7 的十一项）与 `Surface<T>`（2.10 的十四项）都是**抽象接口**：虚析构 + 求值与一阶导数 + 参数域、闭合、周期 + 光滑度 + 有效性。`LineCurve` / 四个圆锥曲线 / `TrimmedCurve` / `Plane` / `SphericalSurface` 实现它们。
- **为什么**：`G4`、`G9` —— “未知类型也能求值并问参数域”只能靠虚函数。两个族问的是同一组事实，只是一个参数还是两个，所以接口也一对一（2.7 与 2.10）。
- **代价**：实现了接口的类型带 vptr，不再是 trivially copyable，`sizeof` 多一个指针。曲线都是冷类型；`Plane` 在射线求交里被复制，所以它**不**额外存面内框架（2.10）。
- **`Ray` 不在族里**：它在拾取里逐三角形按值拷贝，加 vptr 会把测到的内联收益退回去（`geometry-ray.md` 2.7）；它只保留 `pointAt`。
- **`TrimmedCurve<T>`**：持 `std::shared_ptr<const Curve<T>>` 基曲线 + 参数区间，`evaluate` 转发给基曲线；闭合/周期性由基曲线与区间共同决定（跨满全域才算闭合，跨满一个周期才算周期）。

### 2.2 分类标签本身仍是零成本

- **是什么**：`ElementarySurface<T>` / `BoundedSurface<T>` 只做分类，不加状态、不加虚函数；`Surface<T>` 的 vptr 是曲面族唯一的多态代价。`sizeof(Planed)` = 两个向量 + vptr，`sizeof(SphericalSurfaced)` = 中心 + 半径 + vptr，由 `static_assert` 盯着。
- **为什么**：`G10`、`G4` —— “派生”不应该再额外变大；标签层多一个成员就是每次拷贝多一份数据。
- **边界**：不再声称“曲面零成本”——2026-11-04 起曲面与曲线一样付 vptr（`../decisions/ADR-2026-11-04-surface-evaluation-interface.md`）；`Ray` 仍在族外，圆锥曲线的框架在 `ConicCurve` 里统一存三份向量（2.9）。

### 2.3 归属

| 族 | 维度 | 成员 | 参数的含义 |
|---|---|---|---|
| `Curve` | 1 | **抽象接口**：`evaluate` / 导数 / 参数域 / 闭合 / 周期 / 光滑度 / 反向映射 / 有效性（2.7）。实现者：`LineCurve`、`CircleCurve`、`EllipseCurve`、`HyperbolaCurve`、`ParabolaCurve`、`TrimmedCurve`、`BezierCurve`、`BSplineCurve`；抽象中间层 `ConicCurve`、`BoundedCurve`；仍不可构造：`OffsetCurve`（点与一阶导数已实现，二阶导数被缺三阶导数阻塞，见 2.8） | `LineCurve` 是 `direction` 的倍数，圆/椭圆是离心角，双曲线是双曲角，抛物线是垂直轴上的偏移，两种样条是各自的节点表示 |
| `Surface` | 2 | **抽象接口**：`evaluate` / 两个一阶导 / 参数域 / 两个方向的闭合与周期 / 光滑度（2.10）。实现者：`Plane`、`SphericalSurface`、`CylindricalSurface`、`ConicalSurface`、`ToroidalSurface`、`RectangularTrimmedSurface` | 球面用方位角与极角，平面用两个面内坐标 |
| `ElementarySurface` | 2 | `Plane`、`SphericalSurface`、`CylindricalSurface`、`ConicalSurface`、`ToroidalSurface`（全部已实现）；旋转面各持一个 `Placement` 值（见 `geometry-axis-surfaces.md` 2.1） | 无界或半无限的解析支撑面 |
| `BoundedSurface` | 2 | `RectangularTrimmedSurface`（视图）、`BezierSurface`、`BSplineSurface`（均已实现，见 `geometry-axis-surfaces.md` 2.4 / 2.5） | 参数域有限，总能回答 `bounds()` |

- **为什么**：`G1`、`G12` —— 维度只决定"取一个点要几个参数"，具体含义留在类型上；把参数含义统一进基类会造出一个每种类型都要重新解释的约定。
- **默认构造只在两种情况下存在**：① 默认值本身就是有意义的几何（`Placement` 是单位框架，`CircleCurve` 是半径 1 的 XY 圆，`SphericalSurface` 是半径 1 的球）；② 默认值是**能被 `isValid()` 认出的空状态**（`BezierCurve` / `BSplineCurve` / `BezierSurface` / `BSplineSurface` 没有数据，`isValid()` 恒假）。旋转面与平面两者都不是——“单位圆柱看起来完全合法”，所以它们根本不声明默认构造，`static_assert(!std::is_default_constructible_v<CylindricalSurfaced>)` 盯着这件事。
- **参数化几何不在表里**：`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 是**建模类型**（`Primitive` 的子类，见 `geometry-parametric.md`），不是几何族成员。需要列“哪些几何能组成模型”时看 `Primitive` 的五个子类；需要算几何时用值类型（实体的值类型是 `Solid<T>`，见 `geometry-solid.md`）或曲面。

### 2.4 运行期多态用 variant

- **是什么**：`Solid<T>::Shape`（`std::variant<Box, Sphere, Cylinder, Cone, Ellipsoid>`，五个解析体）是现行用例；`Solid<T>` 的成员与三角化都用 `std::visit` 分派，新增 kind 是编译错误。模型侧没有变体：`Primitive` 是带身份的基类，`brep` 与 `robotics::io` 按 `shapeType()` + `obj_cast` 分派。
- **为什么**：`G4`、`G9` —— 变体是封闭集合，标签与内容不可能不一致；新增成员是编译错误提示所有消费点，而不是运行期静默落到 `default`。
- **边界**：变体放的是**值**，所以它能和值语义共存；放指针加虚函数等于绕回被否决的方案。

### 2.5 新增类型只给能验证的契约

- **是什么**：`CircleCurve` 提供 `isValid()`、`pointAt(angle)`、`circumference()`、`bounds()`；椭圆/双曲线/抛物线提供 `isValid()`、`pointAt(u)`、`bounds()`（仅椭圆，闭合且有界）；`SphericalSurface` 提供 `pointAt(azimuth, polar)` / `normalAt` / `bounds()`；`LineCurve` 提供 `pointAt(t)` / `closestPoint` / `intersectWith`。
- **单个三角形、四边形不做类型**：`intersectTriangle(ray, a, b, c, hit)` 直接吃三个角点，不需要类型；名字 `TriangleSurface` / `QuadSurface` 作为保留名登记在 backlog（`../backlog/geometry.md` G7），等它们长出自己的查询（射线–面求交、点到面距离）时再引入，且届时带 `Surface` 后缀——仓库里已有 `TriangleMesh` / `QuadMesh`，裸名会被读成网格元素（`G4`）。
- **为什么**：`G8` —— 没有消费者的能力不写：射线求交、点到曲线的距离、曲面求交都在 backlog，等触发条件。今天已落地的那些（族级曲面查询、点到曲线、解析实体）都已按本条判据补上了消费者侧的证据。
- **边界**：`PointCloud` 仍是 `Shape`，不是曲线也不是曲面：它是离散点集，没有参数化取点；`Box` 等解析体是建模类型（`Primitive` 子类）或值类型（`Solid<T>`），也不在任何一族里。

### 2.6 算法放在值类型上，不另设算法类

- **是什么**：查询是**被交对象自己的成员**——`Aabb::intersectWith(const Ray&, RayHit&)`、`Obb::intersectWith(...)`、`Plane::intersectWith(const LineCurve&, ...)`、`Solid<T>::closestPoint()` / `intersectWith(Ray)`；没有值类型的三角形用自由函数 `intersectTriangle`。没有一个 `SphericalSurface` / `CylindricalSurface` 之类的算法类。
- **为什么**：`G1`、`G6`、`G12`、`G13` —— 名字与数据各只有一份（半径在 `Solid<T>::Sphere` 里，算法类不再存一遍）；调用方不必查“球面求交住在哪个类”；与 `geometry-ray.md` 2.2 的既有形状一致；算法只能住在几何一侧（`G13`）。
- **边界**：
  - 两个对象之间的运算（布尔、实体间最近距离、实体–实体求交）不属于任何单一对象 → **自由函数**，放在两者共同的模块。
  - 族级查询（点到曲面、射线 × 曲面、曲线 × 曲面）没有“一个能回答它的具体对象”，所以是自由函数 + 参数窗口（`geometry-surface-queries.md`、`geometry-intersection.md` 2.9）。
  - 需要后端的运算（STEP 解析、B-rep 三角化）→ `brep`；纯数学的解析实体三角化留在 `geometry`，且因为是 float 而写成自由函数（`geometry-tessellation.md` 2.1）。
  - 曲面族的解析面与裁剪面已全部在 `geometry` 里（轴对称曲面见 `geometry-axis-surfaces.md`），不依赖 OCCT；`brep` 一侧的 `Geom_*` 是另一套表示（引用计数 + 可变），不在这里重复实现（`G1`）。

### 2.7 曲线接口的核心属性

- **是什么**：`Curve<T>` 的五组属性，全部纯虚——① 求值与导数：`evaluate(u)`、`derivative(u)`、`secondDerivative(u)`；② 参数域与周期：`firstParameter()` / `lastParameter()` / `isClosed()` / `isPeriodic()` / `period()`；③ 光滑度：`continuity()`（及由它导出的非虚 `isCN(n)`）；④ 反向参数映射：`reversedParameter(u)`；⑤ 有效性：`isValid()`（状态能不能求值，与参数无关）。
- **为什么**：`G4`、`G9` —— 这十一项是“参数化曲线”这个概念的最小完备集：一个点、它的变化率、它在哪里有效、它有多光滑、反着走时参数怎么变、这份状态到底能不能用。切线/法向、离散采样、曲率、B-rep 接缝判定都只依赖这些，不需要知道具体类型。
- **`derivative` 是速度向量，不是单位切向**：`circle.derivative(u)` 的长度就是半径，要方向就自己归一化。这条写进接口而不是留给约定（`G4`）；`secondDerivative` 同理，只有弧长参数化时它才等于曲率向量。
- **`continuity()` 用阶梯枚举**：`Continuity` 按光滑度排序，`>=` 即“至少这么光滑”；解析类型（直线、圆、平面）答 `CN`。`Polynomial` / `Rational` 不进去——它们描述表示法（nurbs 形式），不是光滑度。
- **`period()` 只说“这个对象是否重复”**：`isPeriodic()` 为假时 `period()` 恒为 0，即使它是一段从周期曲线上裁下来的区间。裁剪视图因此**不**转发基曲线的周期：四分之一圆是“有两个端点的一小段”，回答 2π 会让 `if (period() > 0)` 这类调用方把一小段当成整圈（`G4`）。要基曲线的周期就问基曲线——`TrimmedCurve::basis_curve` 是 public 的。
- **`reversedParameter` 而不是 `reverse()`**：反向是参数化的属性，不是对对象的编辑。曲线不可变、基曲线可被多条裁剪共享，可变反向会让共享同一基的裁剪互相干扰；想要反向曲线就构造它的镜像（直线取 `-direction`，圆取 `-normal`，裁剪交换区间），本函数只回答“同一个点在反向参数化里的参数”。裁剪在**自己区间内**反射（`first + last - u`），不在基曲线上反射。
- **`isValid()` 在接口上**：每个实现早就各自回答“这份状态能不能求值”（C12），但同一个问题经 `const Curve<T>&` 问不出来——调用方要先知道具体类型。它是**状态**的答案，不依赖参数，与 `Shape` / `Primitive` 同一个问法；放上接口后，族级算法与扫描测试可以在一处守卫（见 `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md`）。
- **边界（不进接口的东西）**：
  - `bounds()`：只有有限曲线总能回答，直线只能返回无穷盒，等于把“无界”伪装成盒子（`G4`）。
  - 弧长 / `length()`：要算法与参数化精度，属算法不属属性。
  - 变换族（OCCT 的 `Transform` / `TransformedParameter`）：坐标系住在 `Shape` 的节点上，曲线不持有放置。
  - `curveType()` 枚举：`dynamic_cast` 与 vtable 已经回答了“我是什么”，再维护一份枚举就是两个家（`G1`）。
  - `D3` / `DN`：没有消费者；等真需要更高阶导数时再加（`G8`）。

### 2.8 样条已可求值，偏置卡在缺少三阶导数

- **是什么**：`BezierCurve` / `BSplineCurve` 的十个纯虚函数全部落地，两者是**可构造的值类型**；`OffsetCurve` 实现了点与一阶导数，二阶导数仍为纯虚，因此仍不可构造：
  - `BezierCurve`：一个 de Casteljau 算法用三次——极点、一阶差分、二阶差分，`P' = n * D1(u)`、`P'' = n * (n - 1) * D2(u)`；有理形式在齐次坐标上跑同一套，最后除以权重。
  - `BSplineCurve`：只需求值参数所在跨的 `degree + 1` 个基函数（Cox–de Boor 与导数递推），对极点加权求和；有理形式用商法则 `P' = (A' - P w') / w`、`P'' = (A'' - 2 P' w' - P * w'') / w`；`isClosed()` 比较两个端点，是几何事实而非数据事实。
  - `OffsetCurve`：`evaluate` / `derivative` 用 `N = reference_direction x p'` 与 `N' = reference_direction x p''`，叉乘退化时退回基曲线本身（不产生 NaN）。
- **为什么 `OffsetCurve` 的 `secondDerivative` 是空的**：`P'' = p'' + d * (N / |N|)''` 里含 `p'''`，而 `Curve<T>` 故意只到二阶（2.7、D6）。补三阶导数要让四个圆锥曲线、两种样条、裁剪与直线各自新增一个纯虚实现，只为一条曲线的一种导数付账（`G8`），所以类型保持抽象，成本留到真正需要时。见 `../decisions/ADR-2026-11-04-curve-third-derivative.md`。
- **为什么现在补样条的数值回答**：数据模型与类型事实在 2.8 之前就已定义（`G1`），缺的只是数值函数；它们是仓库里唯一持有内存的曲线，构造与拷贝语义早已确定，补求值不改变任何契约。
- **数据模型的三个取舍（不变）**：
  - 节点存**相异值 + 重数**：重数是调用方唯一会问的东西，存展开向量等于每次查询都重新分组（`G4`）。两端重数 = 度 + 1（夹持），内部重数 ≤ 度——等于度加一时两侧完全独立，参数会跳，那是两段而不是一条曲线（C13、`../decisions/ADR-2026-11-04-bspline-without-knot-breaks.md`）。
  - 权重用**空数组表示多项式**，不用 `is_rational` 布尔：一个标志和数据可以互相矛盾，空数组不会（`G1`）。
  - `BezierCurve` 不存度数：度数是极点数 − 1，存两份就有一份会过期。
- **边界**：
  - 一次查询只碰一个跨的极点；`BSplineCurve` 用 `flatKnot()` 把扁平索引线性扫描回相异节点——一份事实、一次扫描，是 `G1` 让位给 `G10` 的显式取舍（头文件 `@note` 记录）。
  - 越界行为两边不同：`BezierCurve` 是多项式外推，`BSplineCurve` 把参数夹进节点域、域外取端点值（与 OCCT 的 `Geom_BSplineCurve` 一致），两条都写在头文件里（C10、P3）。
  - 三者仍是仓库里仅有的持有内存的曲线类型（`std::vector` 字段），所以它们是“深拷贝意义上的值”，不再 trivially copyable；`LineCurve` / 圆锥曲线 / `Ray` 依然零分配。

### 2.9 圆锥曲线共有的框架与光滑度住在 `ConicCurve`

- **是什么**：`ConicCurve` 持有框架 `center` / `normal` / `x_direction`（构造时归一化），并提供派生的 `yAxis()` / `planeNormal()`，同时把 `continuity()` 定为 `CN`；四个圆锥曲线只声明自己的标量（半径 / 两个半轴 / 焦距）与数值回答。
- **为什么**：`G1`、`G6` —— 框架此前在 `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 各有一份（`yAxis()` / `planeNormal()` / `continuity()` 各四份拷贝），改一处要改四处，四处 的归一化规则也可以悄悄分叉。OCCT 的 `Geom_Conic` 同样用 `gp_Ax2` 持有框架、把 `Continuity()` 定为 Cn，这里只是把同一件事写进我们的类型。
- **代价**：`CircleCurve` 不再每次由 `normal` 推导起始轴，而是构造时存一次 `x_direction`，因此多 24 字节（2.2 的大小断言跟着变）；同时框架字段构造后不可再改写——这从“写了会悄悄改变参数化起点”变成“写了会让三个向量互相矛盾”，后者更该被禁止，已写进每个圆锥曲线的 `@warning`。
- **抛物线的原点就是顶点**：`ParabolaCurve` 不再单独存 `vertex`，用继承的 `center`（OCCT 的 `Geom_Conic::Location()` 对抛物线同样返回顶点）。
- **退化框架的答案写在 `ConicCurve`**：`normal` 为零时 `yAxis()` 为零，参数化塌成沿 `x_direction` 的线段（圆、椭圆）或射线（双曲线、抛物线）；四个 `pointAt()` 的 `@return` 都指向这条规则，`isValid()` 是唯一判据（C12）。`CircleCurve::bounds()` 与 `EllipseCurve::bounds()` 用同一个支撑函数 `sqrt((a·X_i)² + (b·Y_i)²)` 造盒，所以退化时给出线段盒，而不是半径立方（`G1`）。
- **边界**：`isClosed()` / `isPeriodic()` / `period()` 不往 `ConicCurve` 提升——圆与椭圆闭合周期、双曲线与抛物线无界非周期，两边各两行，硬造一层“闭合圆锥曲线”只会多一个类型（`G8`）。

### 2.10 曲面接口的核心属性

- **是什么**：`Surface<T>` 的十四项全纯虚——① 求值与一阶导：`evaluate(u, v)`、`derivativeU(u, v)`、`derivativeV(u, v)`；② 参数域：`firstUParameter()` / `lastUParameter()` / `firstVParameter()` / `lastVParameter()`；③ 两个方向的闭合与周期：`isUClosed()` / `isVClosed()` / `isUPeriodic()` / `isVPeriodic()` / `uPeriod()` / `vPeriod()`；④ 光滑度：`continuity()`（及非虚 `isCN(n)`）；⑤ 有效性：`isValid()`。
- **为什么**：`G4`、`G9` —— 这是“参数化曲面”的最小完备集，与 `Curve<T>` 一一对应（一个参数变两个），所以记一套就够：面遍历、采样、求交的前置都只依赖这些。
- **去掉的东西与理由**：
  - `normalAt(u, v)`：它就是 `derivativeU × derivativeV` 归一化，一行就能写；放进接口就必须选一个朝向约定（球面向外、平面哪一侧？），等于把约定当契约（`G4`）。具体类型仍保留自己的 `normalAt`（球面的向外法向）。
  - `bounds()`（包围盒）：对无界曲面只能返回无穷盒，与无界曲线同一个理由（2.7）。
  - 二阶导数 / 曲率：真正要它的消费者（曲率、G2 拼接）还没有；曲面要三组（UU / UV / VV），是接口里最贵的一组（`G8`）。
  - 反向与变换族（`uReversedParameter` / `transform`）：与曲线同理——反向用构造镜像表达，变换住在 `Shape` 节点上。
- **`Plane` 不存面内框架**：两个面内轴由 `normal` 每次推导（世界轴里与法向最不平行的那个的投影），所以平面的大小不因为接口多一份数据，代价是每次求值多一次投影与叉乘——两个新函数今天没有调用方。零法向是平面唯一的退化输入，由 `isValid()` 报告（`normal.length() > EPS`），与圆锥曲线对退化框架的规则一致（P6）。
- **`SphericalSurface` 的 v 方向不闭合**：`v` 从 0 到 π，两端是两个极点而不是一条缝；缝合与极点的参数化奇异是两件事，`continuity()` 仍答 `CN`。
- **`SphericalSurface` 的 v 从 +Z 量起，与 OCCT 差一个平移（有意）**：OCCT 的 `Geom_SphericalSurface` 从赤道量起、域 `[-π/2, π/2]`；这里用标准球坐标 `[0, π]`（0 = +Z 极点），换算 `v_occt = π/2 - v`。理由：非负域、两端就是极点，读 `v` 不需要靠符号判断（`G4`）；代价是从 OCCT 转参数时要转一次。
- **旋转面与裁剪见 `geometry-axis-surfaces.md`**：旋转面的坐标系值（`Placement`）、共用的“平面内轴”规则（`Frame.hpp`）、矩形裁剪曲面与样条曲面的现状，以及与 OCCT 的对齐 / 分歧都在那份文档里。
- **裁剪视图的周期与曲线同一条规则**：`RectangularTrimmedSurface` 的 `uPeriod()` / `vPeriod()` 只在 `isUPeriodic()` / `isVPeriodic()` 为真时回答基曲面的周期，否则 0（`G6`：两种裁剪视图不许有两种答案）。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 族分类 | 两个求值接口 + 曲面的两个分类标签，`Surface::kDimension` = 2 | 已落地 |
| F2 | 曲线参数化 | `LineCurve` / `CircleCurve` 的 `pointAt`（`Ray::pointAt` 同形，但 `Ray` 不在族内，见 2.1） | 已落地 |
| F3 | 球面参数化 | `SphericalSurface` 的方位角 / 极角与法向 | 已落地 |
| F4 | 解析包围盒 | 每个新类型的 `bounds()` 返回精确或紧致的 `Aabb<T>` | 已落地 |
| F5 | 曲线核心属性 | 求值与一二阶导数、参数域与周期、光滑度、反向参数映射 | 已落地 |
| F6 | 圆锥曲线族 | `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 四种解析圆锥曲线的求值、导数与参数域 | 已落地 |
| F7 | 样条与偏置 | `BezierCurve` / `BSplineCurve` 的求值、一二阶导数与样条闭合；`OffsetCurve` 的点与一阶导数 | 部分：偏置二阶导数阻塞 |
| F8 | 曲面接口 | `evaluate` / 两个一阶导、参数域与两个方向的闭合周期、光滑度；`Plane`、`SphericalSurface` 实现 | 已落地 |

旋转面、矩形裁剪与样条曲面的功能点在 `geometry-axis-surfaces.md` §3（F1–F3）。

### 3.1 F1 族分类

- **约束**（C1）：标签无状态、无虚函数；族的归属只在类型定义处声明一次。
- **验收**（T1）：`GeometryKindsTest.cpp` 的 `static_assert` 覆盖归属、非归属与大小。

### 3.2 F2 曲线参数化

- **行为**：`CircleCurve::pointAt(angle)` 由 `normal` 导出的正交基行走；`LineCurve::pointAt(t)` 是 `direction` 的倍数，`Ray::pointAt(t)` 是距离。
- **约束**（C2、C12）：退化输入不产生 NaN——零方向的直线由 `isValid()` 报告，`closestPoint()` 返回原点、`intersectWith()` 返回无标志（不除零）；退化框架的圆锥曲线塌成沿 `x_direction` 的线段/射线，同样不返回 NaN。
- **验收**（T2）：端点、方向、周长与平面内行走各一例。

### 3.3 F3 球面参数化

- **行为**：`SphericalSurface::pointAt(azimuth, polar)` 极角自 +Z 量起、方位角绕 +Z（与 OCCT 的球面一致）；`normalAt` 返回背离圆心的单位向量。
- **约束**（C3）：退化半径（≤ eps）时 `isValid()` 为假，`normalAt` 返回零向量而不是 NaN。
- **验收**（T3）：角点对应、中心点、法向与共线退化。

### 3.4 F4 解析包围盒

- **行为**：`LineCurve` 无 `bounds()`（无界，见 D5）；`CircleCurve::bounds()` 按支撑函数 `radius * sqrt(X_i^2 + Y_i^2)` 逐轴取半宽——圆锥曲线共用这套推导，圆盘的盒因此是精确的，退化框架给出线段盒；三角形的盒由 `Aabb::compute(span)` 取三个角点。
- **约束**（C4）：`bounds()` 是**局部**空间的盒，世界盒要乘节点变换。

### 3.5 F5 曲线核心属性

- **行为**：`LineCurve` 答常速度 `direction`、零加速度、`CN`、`-u`；`CircleCurve` 答速度 `radius * (-X sin u + Y cos u)`、加速度 `centre - P(u)`、`CN`、`period - u`；`TrimmedCurve` 四项全部转发给基曲线，只有反向映射在自身区间内反射。
- **约束**（C5）：导数返回 `Vector`（与 `evaluate()` 同一个点空间），长度是速度而不是 1；`reversedParameter` 是自反的。
- **验收**（T5）：直线的常量导数、圆的速度/加速度与半径的关系、裁剪区间内的反射与转发。

### 3.6 F6 圆锥曲线族

- **行为**：四种圆锥曲线共用一套“平面 + 原点 + 面内轴”的解析形式，参数含义各自不同——圆与椭圆是离心角（闭合、周期 `2π`），双曲线是双曲角（无界，只参数化 `+X` 那一支），抛物线是垂直轴上的偏移（无界，`u = 0` 在顶点）。
- **约束**（C6）：面内轴由 `Frame.hpp` 的 `inPlaneAxis()` 投影加归一化（`ConicCurve`、`Plane`、`Placement` 共用同一份），退化时按“与法向最不平行的世界轴”回退，因此退化输入只产生无意义曲线，不产生 NaN；`minor` 轴一律由 `normal × x_direction` 导出，不存第三份向量。
- **验收**（T6）：椭圆两轴的端点、速度与半轴的关系、逐轴精确包围盒；双曲线顶点、顶点速度与加速度朝向；抛物线“到焦点距离 = 到准线距离”。

### 3.7 F7 样条与偏置

- **行为**：`BezierCurve::degree()` 等于极点数 − 1，求值是一串 de Casteljau 混合；`BSplineCurve::continuity()` 读内部重数，求值只累加参数所在跨的基函数；`OffsetCurve` 的所有参数域答案来自基曲线，位移方向是 `reference_direction x tangent`。
- **约束**（C7、C10、C11）：`OffsetCurve` 仍不可构造（`is_abstract` 由 `static_assert` 守着）；`BSplineCurve::isValid()` 要求夹持形式（首末重数 = `degree + 1`）、相异节点递增、重数之和 = 极点数 + 度数 + 1；两种样条的 `isValid()` 都要求权重为空或与极点数相等且严格为正；`OffsetCurve` 的基曲线不得为空。
- **验收**（T7）：`aBezierCurveBlendsItsPoles` 验端点、中点、两端导数与常二阶差分；`aRationalBezierCurveIsPulledTowardsItsHeavierPole` 验商法则（`P(u) = 3u / (1 + 2u)` 的取值与两侧导数）；`aBSplineCurveBlendsItsClampedSpan` 验夹持端点、单跨与 Bezier 等价、域外取端点；`aRationalBSplineCurveTracesAQuarterCircle` 验有理基函数（四分之一圆）；`anOffsetCurveInheritsTheBasisDomain` 验域转发与位移几何。

### 3.8 F8 曲面接口

- **行为**：`Plane` 答 `evaluate(u, v) = origin + u * X + v * Y`（两轴由 `normal` 推导）、两个方向均无界且不闭合、`CN`；`SphericalSurface` 答 `u` 从 0 到 `2π`闭合且周期、`v` 从 0 到 `π`不闭合不周期、`CN`。
- **约束**（C9）：两个已实现的曲面必须能通过 `const Surface<T>&` 回答全部十四项；`continuity()` 不被极点或缝合打断（极点不是不连续）。
- **验收**（T8）：`aPlaneAnswersTheSurfaceInterface`、`aSphericalSheetClosesOnlyAroundItsAxis`，两者都经基类引用调用以证明虚分派。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `Curve.hpp` / `Surface.hpp` | `Curve` 与 `Surface` 两个接口；`Surface::kDimension` |
| `Continuity.hpp` | 光滑度阶梯枚举，曲线与曲面共用 |
| `Curve.hpp` / `ConicCurve.hpp` / `BoundedCurve.hpp` | 曲线族与两个中间层；具体曲线各自一个头 |
| `LineCurve.hpp` / `CircleCurve.hpp` / `Ray.hpp` / `SphericalSurface.hpp` | 已实现的值类型与 `*f` / `*d` 别名；`Ray` 不在族内（2.1） |
| `EllipseCurve.hpp` / `HyperbolaCurve.hpp` / `ParabolaCurve.hpp` | 另外三种解析圆锥曲线及 `*f` / `*d` 别名 |
| `BezierCurve.hpp` / `BSplineCurve.hpp` | 两种可构造的样条值类型：极点 + 权重（+ 度数、节点、重数）与十个曲线属性 |
| `OffsetCurve.hpp` | 偏置曲线：点与一阶导数已实现，二阶导数仍为纯虚（2.8、D9） |
| `Surface.hpp` / `ElementarySurface.hpp` / `BoundedSurface.hpp` | 曲面接口与两个分类标签 |
| `Frame.hpp` | 共用的“平面内轴”规则（圆锥曲线、平面、旋转面共用） |
| `Placement.hpp` / `CylindricalSurface.hpp` / `ConicalSurface.hpp` / `ToroidalSurface.hpp` / `RectangularTrimmedSurface.hpp` | 旋转面的坐标系值（`Placement`）与裁剪曲面，见 `geometry-axis-surfaces.md` |
| `src/base/geometry/src/Instantiations.cpp` | 纯头文件几何类型的显式实例化清单（`float` + `double`） |

## 5. 约束、要求与不变量

- **C1**：曲面标签不携带状态、不声明虚函数；曲线接口的虚函数是全族唯一的多态点（2.1、2.7）。
- **C2**：退化输入不产生 NaN；`isValid()` 是唯一的判定入口。
- **C3**：族的归属在类型定义处声明一次，不在别处重复。
- **C4**：所有 `bounds()` 都是局部空间盒，世界盒要乘节点变换。
- **C5**：`derivative` / `secondDerivative` 返回速度与加速度向量，不是单位切向与曲率向量；`reversedParameter` 自反（`reversedParameter(reversedParameter(u)) == u`）。
- **C6**：圆锥曲线的面内轴经 `Frame.hpp` 的 `inPlaneAxis()` 投影归一化（平面与旋转面共用这一份），第二轴由 `normal × x_direction` 导出；退化输入得到 `isValid() == false` 的曲线，不得产生 NaN。
- **C7**：`isCN(n)` 是 `continuity()` 的查询，不单独描述曲线；`G1` / `G2` 不回答 `isCN(1)` / `isCN(2)`（几何连续不是导数连续）。
- **C8**：圆锥曲线的框架（`center` / `normal` / `x_direction`）只在 `ConicCurve` 定义一次并在构造时归一化；四个圆锥曲线不得再声明同名字段，也不得在构造后改写框架。
- **C9**：曲面接口的十四项必须能经 `const Surface<T>&` 回答；极点、缝合不属于“不连续”，`continuity()` 仍答 `CN`；分类标签不得再加状态或虚函数。
- **C10**：越界参数的语义按类型定义，不靠约定——`BezierCurve` 是多项式外推，`BSplineCurve` 把参数夹进节点域后取端点值；两者都写在头文件里。
- **C11**：有理曲线的权重必须严格为正，否则域内分母会穿过零；`BezierCurve` / `BSplineCurve` 的 `isValid()` 是唯一检查点。
- **C12**：`isValid()` 覆盖四类退化输入：直线（零方向）、组合类型（裁剪的空基曲线或反向区间、偏置的空基曲线或零参考方向）、有理样条（非正权重）、圆锥曲线、平面与球面（退化框架 / 退化半径）；组合类型报告自己的输入是否可用，不把基曲线的有效性折叠进来。这个回答在 `Curve<T>` / `Surface<T>` 上是**纯虚**的（调用方拿 `const Curve<T>&` 就能问），退化复合曲线被查询时答退化值（原点、零向量、`C0`、假）而不是解引用空句柄——那是防崩环境的行为，不是几何事实。
- **C13**：样条的内部节点重数不超过度数（两端仍是度 + 1 的夹持）。内部重数 = 度 + 1 是缺口而不是连接，`isValid()` 拒绝它；因此 `continuity()` 的 `C0` 始终意味着“各段交于一点”，`degree - 重数` 不需要被夹住。需要保留断点就拆成多条曲线，不把洞交给每个算法处理。
- **N1**：`LineCurve::pointAt(t)` 的 `t` 乘的是 `direction` 的长度，只有单位方向时它才是距离；`Ray` 的方向恒为单位，所以两者同名不同量纲。
- **N2**：默认构造的准入规则与 2.3 一致：要么默认值是有意义的几何，要么是可被 `isValid()` 认出的空状态；旋转面与平面两者都不是，所以不声明默认构造。`Plane` 是唯一不带 `Surface` 后缀的曲面（平面没有第二个含义，OCCT 同样如此）。
- **R1**：零分配、不抛异常的范围是 `LineCurve` / 圆锥曲线 / `Aabb` 的 `pointAt` / `bounds` / `derivative`；`BSplineCurve` 的求值也做到零分配（只用局部标量），而 `BezierCurve` 每次求值拷贝一份极点数组的临时缓冲（de Casteljau 需要 `n` 个暂存值，不为此引入可变成员）。
- **R2**：族分类不新增外部依赖；标签只 include `geometry_global.hpp`，曲线接口额外只 include `vine/math` 的 `Point3` / `Vector3` 与本目录的 `Continuity.hpp`。

## 6. 注意事项与陷阱

- **P1**：往标签里加虚函数 = 给每个值类型加 vptr（`Ray` 在热循环里按值拷贝）。要共享算法就写自由函数。
- **P2**：所有 `bounds()` 都是**局部**空间的盒，世界盒要乘节点变换；当成世界包围盒缓存会错在场景层级上。
- **P3**：两种样条的域外行为不同（C10）。会怎么错：按同一种“外推”去采样域外参数，B 样条得到一条平线，而贝塞尔得到发散的曲线；怎么判：域外点是否突变成常量或切线是否突然为零；怎么防：外推前查 `firstParameter()` / `lastParameter()`，需要外推就在调用方做，曲线本身不改。
- **P4**：有理曲线的分母是 `Σ N_i(u) w_i`。会怎么错：零或负权重让它在域内穿过零，`evaluate()` 返回无穷大或符号翻转的远点；怎么判：`isValid()` 为假，或中点附近坐标突变；怎么防：构造前保证权重全为正（C11）。
- **P5**：`BSplineCurve` 的求值只覆盖夹持形式。会怎么错：拿未夹持的节点（首末重数 < `degree + 1`）去求值，域端点处基函数之和不为 1；怎么判：`isValid()` 为假；怎么防：按夹持形式构造，`continuity()` 的重数语义也依赖它。
- **P6**：退化框架的圆锥曲线不是“半径为 0 的圆”。会怎么错：`normal` 为零时 `yAxis()` 为零，`evaluate()` 沿世界 X 轴走一条线段，`bounds()` 也只是那条线段；把结果当圆去取点或求周长只会得到一串共线点。怎么判：`isValid()` 为假。怎么防：构造前确认 `normal` 非零，让 `ConicCurve` 统一归一化框架。
- **P7**：直线的“退化”只有一条阈值——`isValid()` 的 `|direction| > EPS`。会怎么错：另写一个 `|direction|² < EPS` 的判据（它等价于 `|direction| < 1e-4`），会把合法短方向当成零方向、把投影悄悄换成原点；怎么判：方向长度在 `EPS` 与 `sqrt(EPS)` 之间时两种答案不同；怎么防：判据只从 `isValid()` 来。
- **P8**：裁剪视图不是基曲线 / 基曲面。会怎么错：拿 `TrimmedCurve::period()` 当基圆的 2π 去归一化参数，或以为 `isClosed()` 跟基曲线一致，于是四分之一圆被当成整圈做接缝判断；怎么判：裁剪的 `isPeriodic()` / `isClosed()` / `period()` 为假或 0，而基曲线的答案为真；怎么防：闭合、周期、周期值一律问裁剪自己（2.7 的周期规则、C12），要基曲线的数字就问 `basis_curve` / `basis`。
- **P9**：`isValid()` 为假的组合类型仍能被查询。会怎么错：空基的裁剪 / 偏置答原点、零向量、`C0`，看起来像“退化成一条点的曲线”，被当成合法结果参与后续计算；怎么判：`isValid()` 为假；怎么防：拿到 `TrimmedCurve` / `OffsetCurve` / `RectangularTrimmedSurface` 先查 `isValid()`——退化答案只为避免解引用空指针而存在（C12）。
- **P10**：把一段被切开的数据当成一条样条。会怎么错：内部重数写成了 `度 + 1`，曲线在节点处跳；求值仍能跑、参数区间也连续，于是采样出一串跨越缝的点。怎么判：`isValid()` 为假（2026-11-04 起），或直接算 `continuity()`——重数等于度数时它确实答 `C0`（各段交于一点），两者不再混淆。怎么防：重建时按断点拆成多条曲线（C13）。

## 7. 已否决选项

- **D1** `Curve` / `Surface` 做成虚基类：给所有值类型加 vptr，打掉内联与 trivially copyable（`G10`、`G8`）。见 `../decisions/ADR-2026-11-04-geometry-kinds.md`。
- **D2** 只提供 concept、不提供标签：`is_base_of` 这类编译期问法与"族"的文档位置都会消失，而且运行期仍需要 variant 覆盖同一集合（`G1`）。
- **D3** 把参数含义提升进基类（统一 `pointAt(t)`）：每种曲线的 `t` 语义不同，统一签名只会把差异藏进文档（`G4`）。
- **D4** 照 OCCT 提供可变的 `reverse()`：曲线不可变、基曲线可被多条裁剪共享，可变反向会互相干扰；反向表达为构造镜像 + `reversedParameter`（2.7）。
- **D5** `bounds()` 进曲线接口：只有有限曲线总能回答；直线只能返回无穷盒，等于把“无界”伪装成盒子（`G4`）。
- **D6** `curveType()` 枚举与 `D3` / `DN`：`dynamic_cast` 与 vtable 已能回答“我是什么”，再维护枚举就是两个家；三阶以上导数没有消费者（`G1`、`G8`）。
- **D7** 曲面继续做空标签：标签答不了“未知曲面也能求值 + 问两个方向的参数域与周期”，而这正是面遍历、采样、求交前置需要的（`G4`、`G9`）。见 `../decisions/ADR-2026-11-04-surface-evaluation-interface.md`。
- **D8** 曲面接口里放 `normalAt()` / `bounds()` / 二阶导数：前两个是派生值或伪值（无界盒），第三个没有消费者（2.10）。
- **D9** 给 `Curve<T>` 加 `thirdDerivative()`，好让 `OffsetCurve::secondDerivative()` 落地：只为一条曲线的一种导数，给八种具体曲线各加一个纯虚函数，其余七种今天没有消费者（`G8`）；偏置保持抽象就能如实表达“这一项还没有实现”。见 `../decisions/ADR-2026-11-04-curve-third-derivative.md`。

## 8. 测试与验收

| 验收 | 用例 |
|---|---|
| T1 归属、非归属与零成本 | `tests/test_geometry/GeometryKindsTest.cpp` 顶部 `static_assert` |
| T2 曲线参数化与退化输入 | 同文件 `aLineCurveAnswersThePointAtItsParameter`、`aCircleCurveWalksItsOwnPlane`、`aCircleCurveWithoutANormalIsInvalid` |
| T3 球面参数化与退化输入 | 同文件 `aSphericalSheetPutsPolarZeroAtThePole`、`aCircleCurveWithoutANormalIsInvalid` |
| T4 曲线接口的参数域与转发 | 同文件 `aCurveAnswersItsDomainAndPeriod`、`aTrimmedCurveForwardsToItsBasis` |
| T5 曲线核心属性 | 同文件 `aLineCurveHasAConstantVelocity`、`aCircleCurveTurnsTowardsItsCentre`、`aTrimmedCurveMirrorsInsideItsOwnRange` |
| T6 圆锥曲线族 | 同文件 `anEllipseCurveWalksItsTwoAxes`、`aHyperbolaBranchWalksAwayFromItsCentre`、`aParabolaIsEquidistantFromFocusAndDirectrix` |
| T7 样条求值与偏置 | 同文件 `aBezierCurveDescribesItself`、`aBezierCurveBlendsItsPoles`、`aRationalBezierCurveIsPulledTowardsItsHeavierPole`、`aBSplineCurveDerivesItsContinuityFromItsMultiplicities`、`aBSplineCurveBlendsItsClampedSpan`、`aRationalBSplineCurveTracesAQuarterCircle`、`anOffsetCurveInheritsTheBasisDomain`、`continuityAnswersTheDerivativeOrderQuestion` |
| T8 曲面接口 | 同文件 `aPlaneAnswersTheSurfaceInterface`、`aSphericalSheetClosesOnlyAroundItsAxis`；轴对称曲面与矩形裁剪的验收见 `geometry-axis-surfaces.md` T1 |
| T9 族级契约扫查 | 同文件 `everyCurveKeepsTheInterfacePromises` / `everySurfaceKeepsTheInterfacePromises` 与它们的 `…InFloat` 版本（**两种精度各跑一遍**：库对 float / double 都显式实例化导出）：对全部实现逐条问：域有序、`isValid()` 为真、闭合 / 周期与周期值一致、`isCN` 与 `continuity()` 一致、`reversedParameter` 自反、有限域内导数有限；裁剪的两条契约另有 `aTrimOfAPeriodicBasisReportsAPeriodOnlyWhenItRepeats`、`aTrimWithoutABasisAnswersInsteadOfDereferencingIt`、`anOffsetCurveWithoutABasisAnswersInsteadOfDereferencingIt`；默认构造的准入规则由顶部 `static_assert` 与 `anEmptyPatchIsRecognisablyEmpty` 盯住，接口上的有效性由 `validityIsAskableThroughTheInterface` 与两行 `static_assert` 盯住 |
## 9. 状态与演进

- 两个族、曲面分 `ElementarySurface` / `BoundedSurface`、四种新值类型、归属与大小断言均已落地。
- 曲线接口的十一项属性已落地（2.7）；`TrimmedCurve` 是组合曲线，`BezierCurve` / `BSplineCurve` 是两种可构造的样条（F7，2.8）。
- 曲面接口的十四项已落地（F8，2.10），`Plane` 与 `SphericalSurface` 实现；两个分类标签仍无状态、无虚函数，但**曲面现在与曲线一样付 vptr**。
- 四种圆锥曲线已全部实现（F6）；`BezierCurve` / `BSplineCurve` 的求值与导数已实现（F7，2.8）。`OffsetCurve` 只差 `secondDerivative`，被 `Curve<T>` 缺少三阶导数阻塞（D9，backlog）。
- 三个轴对称解析曲面、矩形裁剪曲面与两个样条曲面已实现，见 `geometry-axis-surfaces.md`；曲面族至此没有占位类型。
- 族级算法已存在两侧：曲面（最近点 / 距离、射线 × 曲面，见 `geometry-surface-queries.md`）与曲线（最近点 / 距离，见 `geometry-curve-queries.md`）；都用自由函数 + `requires std::is_base_of_v<…>` 约束，不给接口加虚函数。已知缺口：实体求交（backlog G3）、曲线 × 曲线的最近距离。
- 兼容性：曲面接口是从非模板的 `Surface` 标签改为模板接口，是**破坏性变更**（`Surface` / `ElementarySurface` / `BoundedSurface` 现在都要带 `<T>`）；仓库内唯一消费点是 `tests`。
- 兼容性（本轮的语义修正）：`TrimmedCurve::period()` 与 `RectangularTrimmedSurface::uPeriod()` / `vPeriod()` 以前转发基曲线 / 基曲面的周期，现在按接口契约在 `isPeriodic()` 为假时返回 0——行为变化，签名不变（仓库内无调用者）；两种裁剪视图的空基查询也不再解引用空句柄（C12、P9）。两类事实由族级契约扫查 `everyCurveKeepsTheInterfacePromises` / `everySurfaceKeepsTheInterfacePromises` 盯住（T9）。
- 兼容性（接口扩充）：`isValid()` 现在是 `Curve<T>` / `Surface<T>` 的纯虚函数（十项→十一项、十三项→十四项），所有实现原本就有这个方法，只是加上了 `override`；`Plane` 因此第一次有了真正的有效性判据（零法向为假，以前一律可用）。自定义实现者必须实现它——仓库内没有其他实现者。见 `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md`。

## 10. 参考

- `geometry-primitives.md`（`Aabb` / `Obb` / `LineCurve` / `Plane` 的归属与头文件规则）
- `geometry-parametric.md`（参数化几何与 `Primitive`）
- `geometry-ray.md`（`Ray` 的契约与内联理由）
- `../decisions/ADR-2026-11-04-geometry-kinds.md`
- `../decisions/ADR-2026-11-04-curve-evaluation-interface.md`（曲线接口）
- `../decisions/ADR-2026-11-04-surface-evaluation-interface.md`（曲面接口）
- `../decisions/ADR-2026-11-04-curve-third-derivative.md`（偏置二阶导数不实现）
- `../decisions/ADR-2026-11-04-axis-surface-frame.md`（旋转面框架层与共用的平面内轴规则）
- OCCT `Geom_Curve.hxx` / `Geom_Surface.hxx` / `Geom_Conic.hxx` / `Geom_BezierCurve.hxx` / `Geom_BSplineCurve.hxx` / `Geom_OffsetCurve.hxx`（接口词汇与数值算法的来源，本地源码在 `E:\opensrc\OCCT\src\ModelingData\TKG3d\Geom`）
