# geometry 几何族：Curve 与 Surface

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`Curve` / `Surface` 两个求值接口，以及具体值类型的归属与新增类型的最小契约。
> 参数化几何（`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`）**不构成族**：它们只被建模使用，集合就是 `Primitive::Parameters`（见 `geometry-parametric.md`）。
> 不含具体类型的完整契约（见 `geometry-primitives.md`、`geometry-parametric.md`、`geometry-ray.md` 与各自头文件），不含求交与距离算法。
> **权威来源**：`src/base/geometry/sdk/vine/geometry/Curve.hpp`、`Continuity.hpp`、`Surface.hpp`；`tests/test_geometry/GeometryKindsTest.cpp`。
> **待办**：`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

几何类型之间此前没有任何分类关系：`Line`、`Ray`、`Plane`、`Box` 都是互不相关的类，"这是一条曲线 / 一个曲面 / 一个实体"这句话只存在于讨论里。想按族写一个约束、想把一族类型放进同一个容器、想回答"这个类型是几维的"，都只能靠命名猜测；运行期分类只有 `ShapeType`，而它只覆盖 `Shape` 子类，值类型根本不在其中。

### 1.2 目标

- 两个族各有一个名字，归属写在类型定义处（`class CircleCurve : public ConicCurve`）。
- 分类可以在编译期查询（`std::is_base_of_v<Curve<double>, CircleCurved>`）与枚举（`Surface::kDimension`）。
- 几何可被当作“未知类型的参数化对象”使用：曲线是求值、导数、参数域、光滑度、反向映射（2.7），曲面是求值、两个一阶导、两个方向的参数域与闭合周期（2.10）。

### 1.3 非目标

- 接口里只放“参数化对象必然能回答的事”：曲面的法向、包围盒、二阶导数与反向/变换都不进接口（2.10），曲线的同理（2.7）。
- 不做异构容器：需要“一列混合曲线”时用 `std::variant` + `std::visit`；接口只回答“一条”曲线 / “一张”曲面（2.4）。
- 不给参数化几何（`Box` / `Sphere` / …）设族标签：它们不提供自己的算法（射线–盒就是 `Aabb` 的 slab），"哪些几何能组成模型"由 `Primitive::Parameters` 的变体回答。
- 不做族级统一算法（射线–曲面、点到曲线的距离）：今天没有消费者（`G8`），见 `../backlog/geometry.md`。

### 1.4 一句话结论

**`Curve` 与 `Surface` 都是求值接口，曲面多一层分类标签；参数化几何不属于任何族，它们只是建模参数。**

## 2. 设计要点

### 2.1 曲线与曲面都是求值接口

- **是什么**：`Curve<T>`（2.7 的十项）与 `Surface<T>`（2.10 的十三项）都是**抽象接口**：虚析构 + 求值与一阶导数 + 参数域、闭合、周期 + 光滑度。`LineCurve` / 四个圆锥曲线 / `TrimmedCurve` / `Plane` / `SphericalSurface` 实现它们。
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
| `Curve` | 1 | **抽象接口**：`evaluate` / 导数 / 参数域 / 闭合 / 周期 / 光滑度 / 反向映射（2.7）。实现者：`LineCurve`、`CircleCurve`、`EllipseCurve`、`HyperbolaCurve`、`ParabolaCurve`、`TrimmedCurve`；抽象中间层 `ConicCurve`、`BoundedCurve`；仍为占位：`BezierCurve`、`BSplineCurve`、`OffsetCurve` | `LineCurve` 是 `direction` 的倍数，圆/椭圆是离心角，双曲线是双曲角，抛物线是垂直轴上的偏移 |
| `Surface` | 2 | **抽象接口**：`evaluate` / 两个一阶导 / 参数域 / 两个方向的闭合与周期 / 光滑度（2.10）。实现者：`Plane`、`SphericalSurface` | 球面用方位角与极角，平面用两个面内坐标 |
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

- **是什么**：`CircleCurve` 提供 `isValid()`、`pointAt(angle)`、`circumference()`、`bounds()`；椭圆/双曲线/抛物线提供 `isValid()`、`pointAt(u)`、`bounds()`（仅椭圆，闭合且有界）；`SphericalSurface` 提供 `pointAt(azimuth, polar)` / `normalAt` / `bounds()`；`LineCurve` 提供 `pointAt(t)` / `closestPoint` / `intersectWith`。
- **单个三角形、四边形不做类型**：`intersectTriangle(ray, a, b, c, hit)` 直接吃三个角点，不需要类型；名字 `TriangleSurface` / `QuadSurface` 作为保留名登记在 backlog（`../backlog/geometry.md` G7），等它们长出自己的查询（射线–面求交、点到面距离）时再引入，且届时带 `Surface` 后缀——仓库里已有 `TriangleMesh` / `QuadMesh`，裸名会被读成网格元素（`G4`）。
- **为什么**：`G8` —— 没有消费者的能力不写：射线求交、点到曲线的距离、曲面求交都在 backlog，等触发条件。
- **边界**：`PointCloud` 仍是 `Shape`，不是曲线也不是曲面：它是离散点集，没有参数化取点；`Box` 等参数化几何是建模类型（`Primitive` 子类），也不在任何一族里。

### 2.6 算法放在值类型上，不另设算法类

- **是什么**：查询是**被交对象自己的成员**——`Sphere::intersectWith(const Rayf&, RayHitf&)`、`Cylinder::intersectWith(...)`、`Box::intersectWith(...)`、`Plane::intersectWith(...)`；没有值类型的三角形用自由函数 `intersectTriangle`。没有一个 `SphericalSurface` / `CylindricalSurface` 之类的算法类。
- **为什么**：`G1`、`G6`、`G12` —— 名字与数据各只有一份（半径在 `Sphere` 里，算法类不再存一遍）；调用方不必查“球面求交住在哪个类”；与 `geometry-ray.md` 2.2 的既有形状一致。
- **边界**：
  - 两个对象之间的运算（布尔、实体间最近距离、实体–实体求交）不属于任何单一对象 → **自由函数**，放在两者共同的模块。
  - 需要后端或 IO 的运算（STEP 解析、三角化）→ `brep`，不进 `geometry`（`geometry-parametric.md` 2.5）。
  - `SphericalSurface` / `CylindricalSurface` 这类**无界或裁剪曲面**只在消费者要求“不依赖 OCCT 的裁剪面”时才加（`../backlog/geometry.md` G6）；OCCT 的 `Geom_*` 已在 B-rep 一侧提供同一概念，提前再造一份就是第二套算法（`G1`）。
- **不因“类名更数学”而拆**：`Sphere` 既是实体，也是球面族的解析描述；拆成 `Sphere` + `SphericalSurface` 会让半径出现两个家。

### 2.7 曲线接口的核心属性

- **是什么**：`Curve<T>` 的四组属性，全部纯虚——① 求值与导数：`evaluate(u)`、`derivative(u)`、`secondDerivative(u)`；② 参数域与周期：`firstParameter()` / `lastParameter()` / `isClosed()` / `isPeriodic()` / `period()`；③ 光滑度：`continuity()`（及由它导出的非虚 `isCN(n)`）；④ 反向参数映射：`reversedParameter(u)`。
- **为什么**：`G4`、`G9` —— 这十项是“参数化曲线”这个概念的最小完备集：一个点、它的变化率、它在哪里有效、它有多光滑、反着走时参数怎么变。切线/法向、离散采样、曲率、B-rep 接缝判定都只依赖这些，不需要知道具体类型。
- **`derivative` 是速度向量，不是单位切向**：`circle.derivative(u)` 的长度就是半径，要方向就自己归一化。这条写进接口而不是留给约定（`G4`）；`secondDerivative` 同理，只有弧长参数化时它才等于曲率向量。
- **`continuity()` 用阶梯枚举**：`Continuity` 按光滑度排序，`>=` 即“至少这么光滑”；解析类型（直线、圆、平面）答 `CN`。`Polynomial` / `Rational` 不进去——它们描述表示法（nurbs 形式），不是光滑度。
- **`reversedParameter` 而不是 `reverse()`**：反向是参数化的属性，不是对对象的编辑。曲线不可变、基曲线可被多条裁剪共享，可变反向会让共享同一基的裁剪互相干扰；想要反向曲线就构造它的镜像（直线取 `-direction`，圆取 `-normal`，裁剪交换区间），本函数只回答“同一个点在反向参数化里的参数”。裁剪在**自己区间内**反射（`first + last - u`），不在基曲线上反射。
- **边界（不进接口的东西）**：
  - `bounds()`：只有有限曲线总能回答，直线只能返回无穷盒，等于把“无界”伪装成盒子（`G4`）。
  - 弧长 / `length()`：要算法与参数化精度，属算法不属属性。
  - 变换族（OCCT 的 `Transform` / `TransformedParameter`）：坐标系住在 `Shape` 的节点上，曲线不持有放置。
  - `curveType()` 枚举：`dynamic_cast` 与 vtable 已经回答了“我是什么”，再维护一份枚举就是两个家（`G1`）。
  - `D3` / `DN`：没有消费者；等真需要更高阶导数时再加（`G8`）。

### 2.8 尚未求值的三种曲线：先把接口定下来

- **是什么**：`BezierCurve` / `BSplineCurve` / `OffsetCurve` 三个类型已把**数据模型**与**类型事实**定义完，只把数值回答（`evaluate` / `derivative` / `secondDerivative`，以及样条的 `isClosed`）留成纯虚：
  - `BezierCurve`：极点数组 + 可选权重；度数由极点数导出（不存第二份）；域 `[0, 1]`、永不周期、内部 `CN`、首尾极点重合即闭合、反向 `1 - u`。
  - `BSplineCurve`：`degree` + 极点 + **相异节点 + 重数**（不是展开的节点向量）+ 可选权重；`continuity()` 由内部重数推出 `C(degree - 最大内部重数)`，无内部节点时为 `CN`；域 `[knots.front(), knots.back()]`（夹持形式），反向 `first + last - u`。
  - `OffsetCurve`：基曲线 + 有符号距离 + 参考方向；参数域、闭合、周期、连续性、反向参数全部转发基曲线。
- **为什么区分“定义接口”与“实现”**：`G4`、`G8` —— 调用方依赖的是契约（参数域、周期性、光滑度、反向映射），这些是**关于类型的事实**；de Casteljau / de Boor / 叉乘保护才是算法。先把事实写进类型，消费者出现时只剩一个数值函数要填。
- **为什么三个仍然抽象**：没有求值就不能构造，`is_abstract` 被测试的 `static_assert` 盯着；契约本身用 `GeometryKindsTest.cpp` 里的 `NumericStub` 补上三个数值回答后验证（T7）。
- **数据模型的三个取舍**：
  - 节点存**相异值 + 重数**：重数是调用方唯一会问的东西，存展开向量等于每次查询都重新分组（`G4`）。
  - 权重用**空数组表示多项式**，不用 `is_rational` 布尔：一个标志和数据可以互相矛盾，空数组不会（`G1`）。
  - `BezierCurve` 不存度数：度数是极点数 − 1，存两份就有一份会过期。
- **边界**：这三种是仓库里唯一**持有内存**的曲线类型（`std::vector` 字段），所以它们是“深拷贝意义上的值”，不再是 trivially copyable；`LineCurve` / 圆锥曲线 / `Ray` 依然零分配。

### 2.9 圆锥曲线共有的框架与光滑度住在 `ConicCurve`

- **是什么**：`ConicCurve` 持有框架 `center` / `normal` / `x_direction`（构造时归一化），并提供派生的 `yAxis()` / `planeNormal()`，同时把 `continuity()` 定为 `CN`；四个圆锥曲线只声明自己的标量（半径 / 两个半轴 / 焦距）与数值回答。
- **为什么**：`G1`、`G6` —— 框架此前在 `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 各有一份（`yAxis()` / `planeNormal()` / `continuity()` 各四份拷贝），改一处要改四处，四处 的归一化规则也可以悄悄分叉。OCCT 的 `Geom_Conic` 同样用 `gp_Ax2` 持有框架、把 `Continuity()` 定为 Cn，这里只是把同一件事写进我们的类型。
- **代价**：`CircleCurve` 不再每次由 `normal` 推导起始轴，而是构造时存一次 `x_direction`，因此多 24 字节（2.2 的大小断言跟着变）；同时框架字段构造后不可再改写——这从“写了会悄悄改变参数化起点”变成“写了会让三个向量互相矛盾”，后者更该被禁止，已写进每个圆锥曲线的 `@warning`。
- **抛物线的原点就是顶点**：`ParabolaCurve` 不再单独存 `vertex`，用继承的 `center`（OCCT 的 `Geom_Conic::Location()` 对抛物线同样返回顶点）。
- **边界**：`isClosed()` / `isPeriodic()` / `period()` 不往 `ConicCurve` 提升——圆与椭圆闭合周期、双曲线与抛物线无界非周期，两边各两行，硬造一层“闭合圆锥曲线”只会多一个类型（`G8`）。

### 2.10 曲面接口的核心属性

- **是什么**：`Surface<T>` 的十三项全纯虚——① 求值与一阶导：`evaluate(u, v)`、`derivativeU(u, v)`、`derivativeV(u, v)`；② 参数域：`firstUParameter()` / `lastUParameter()` / `firstVParameter()` / `lastVParameter()`；③ 两个方向的闭合与周期：`isUClosed()` / `isVClosed()` / `isUPeriodic()` / `isVPeriodic()` / `uPeriod()` / `vPeriod()`；④ 光滑度：`continuity()`（及非虚 `isCN(n)`）。
- **为什么**：`G4`、`G9` —— 这是“参数化曲面”的最小完备集，与 `Curve<T>` 一一对应（一个参数变两个），所以记一套就够：面遍历、采样、求交的前置都只依赖这些。
- **去掉的东西与理由**：
  - `normalAt(u, v)`：它就是 `derivativeU × derivativeV` 归一化，一行就能写；放进接口就必须选一个朝向约定（球面向外、平面哪一侧？），等于把约定当契约（`G4`）。具体类型仍保留自己的 `normalAt`（球面的向外法向）。
  - `bounds()`（包围盒）：对无界曲面只能返回无穷盒，与无界曲线同一个理由（2.7）。
  - 二阶导数 / 曲率：真正要它的消费者（曲率、G2 拼接）还没有；曲面要三组（UU / UV / VV），是接口里最贵的一组（`G8`）。
  - 反向与变换族（`uReversedParameter` / `transform`）：与曲线同理——反向用构造镜像表达，变换住在 `Shape` 节点上。
- **`Plane` 不存面内框架**：两个面内轴由 `normal` 每次推导（世界轴里与法向最不平行的那个的投影），所以平面的大小不因为接口多一份数据，代价是每次求值多一次投影与叉乘——两个新函数今天没有调用方。
- **`SphericalSurface` 的 v 方向不闭合**：`v` 从 0 到 π，两端是两个极点而不是一条缝；缝合与极点的参数化奇异是两件事，`continuity()` 仍答 `CN`。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 族分类 | 两个求值接口 + 曲面的两个分类标签，`Surface::kDimension` = 2 | 已落地 |
| F2 | 曲线参数化 | `LineCurve` / `Ray` / `CircleCurve` 的 `pointAt` | 已落地 |
| F3 | 球面参数化 | `SphericalSurface` 的方位角 / 极角与法向 | 已落地 |
| F4 | 解析包围盒 | 每个新类型的 `bounds()` 返回精确或紧致的 `Aabb<T>` | 已落地 |
| F5 | 曲线核心属性 | 求值与一二阶导数、参数域与周期、光滑度、反向参数映射 | 已落地 |
| F6 | 圆锥曲线族 | `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 四种解析圆锥曲线的求值、导数与参数域 | 已落地 |
| F7 | 样条与偏置的接口 | `BezierCurve` / `BSplineCurve` / `OffsetCurve` 的数据模型与类型事实；数值求值留空（类型仍抽象） | 接口已落地，求值未实现 |
| F8 | 曲面接口 | `evaluate` / 两个一阶导、参数域与两个方向的闭合周期、光滑度；`Plane`、`SphericalSurface` 实现 | 已落地 |

### 3.1 F1 族分类

- **约束**（C1）：标签无状态、无虚函数；族的归属只在类型定义处声明一次。
- **验收**（T1）：`GeometryKindsTest.cpp` 的 `static_assert` 覆盖归属、非归属与大小。

### 3.2 F2 曲线参数化

- **行为**：`CircleCurve::pointAt(angle)` 由 `normal` 导出的正交基行走；`LineCurve::pointAt(t)` 是 `direction` 的倍数，`Ray::pointAt(t)` 是距离。
- **约束**（C2）：退化输入不产生 NaN——零长度段返回零方向，退化圆返回圆心，非法状态由 `isValid()` 报告。
- **验收**（T2）：端点、方向、周长与平面内行走各一例。

### 3.3 F3 球面参数化

- **行为**：`SphericalSurface::pointAt(azimuth, polar)` 极角自 +Z 量起、方位角绕 +Z（与 OCCT 的球面一致）；`normalAt` 返回背离圆心的单位向量。
- **约束**（C3）：退化半径（≤ eps）时 `isValid()` 为假，`normalAt` 返回零向量而不是 NaN。
- **验收**（T3）：角点对应、中心点、法向与共线退化。

### 3.4 F4 解析包围盒

- **行为**：线段/三角形/四边形取角点盒；`Circle::bounds()` 按 `radius * sqrt(1 - n_i²)` 逐轴取半宽，是圆盘的精确盒。
- **约束**（C4）：`bounds()` 是**局部**空间的盒，世界盒要乘节点变换。

### 3.5 F5 曲线核心属性

- **行为**：`LineCurve` 答常速度 `direction`、零加速度、`CN`、`-u`；`CircleCurve` 答速度 `radius * (-X sin u + Y cos u)`、加速度 `centre - P(u)`、`CN`、`period - u`；`TrimmedCurve` 四项全部转发给基曲线，只有反向映射在自身区间内反射。
- **约束**（C5）：导数返回 `Vector`（与 `evaluate()` 同一个点空间），长度是速度而不是 1；`reversedParameter` 是自反的。
- **验收**（T5）：直线的常量导数、圆的速度/加速度与半径的关系、裁剪区间内的反射与转发。

### 3.6 F6 圆锥曲线族

- **行为**：四种圆锥曲线共用一套“平面 + 原点 + 面内轴”的解析形式，参数含义各自不同——圆与椭圆是离心角（闭合、周期 `2π`），双曲线是双曲角（无界，只参数化 `+X` 那一支），抛物线是垂直轴上的偏移（无界，`u = 0` 在顶点）。
- **约束**（C6）：面内轴由 `ConicCurve::inPlaneAxis()` 投影加归一化，退化时按“与法向最不平行的世界轴”回退，因此退化输入只产生无意义曲线，不产生 NaN；`minor` 轴一律由 `normal × x_direction` 导出，不存第三份向量。
- **验收**（T6）：椭圆两轴的端点、速度与半轴的关系、逐轴精确包围盒；双曲线顶点、顶点速度与加速度朝向；抛物线“到焦点距离 = 到准线距离”。

### 3.7 F7 样条与偏置的接口

- **行为**：`BezierCurve::degree()` 等于极点数 − 1；`BSplineCurve::continuity()` 读内部重数；`OffsetCurve` 的所有参数域答案来自基曲线。
- **约束**（C7）：三个类型均不可构造（`is_abstract` 由 `static_assert` 守着）；`BSplineCurve::isValid()` 要求重数之和等于极点数 + 度数 + 1（夹持形式）；`OffsetCurve` 的基曲线不得为空；`BezierCurve::isValid()` 要求权重为空或与极点数相等。
- **验收**（T7）：在测试里用 `NumericStub` 补齐三个数值回答，验证贝塞尔度数/闭合/反向、样条重数到连续的映射与无效计数、偏置对基曲线参数域的转发。

### 3.8 F8 曲面接口

- **行为**：`Plane` 答 `evaluate(u, v) = origin + u * X + v * Y`（两轴由 `normal` 推导）、两个方向均无界且不闭合、`CN`；`SphericalSurface` 答 `u` 从 0 到 `2π`闭合且周期、`v` 从 0 到 `π`不闭合不周期、`CN`。
- **约束**（C9）：两个已实现的曲面必须能通过 `const Surface<T>&` 回答全部十三项；`continuity()` 不被极点或缝合打断（极点不是不连续）。
- **验收**（T8）：`aPlaneAnswersTheSurfaceInterface`、`aSphericalSheetClosesOnlyAroundItsAxis`，两者都经基类引用调用以证明虚分派。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `Curve.hpp` / `Surface.hpp` | `Curve` 与 `Surface` 两个接口；`Surface::kDimension` |
| `Continuity.hpp` | 光滑度阶梯枚举，曲线与曲面共用 |
| `Curve.hpp` / `ConicCurve.hpp` / `BoundedCurve.hpp` / `OffsetCurve.hpp` | 曲线族与两个中间层；七个占位曲线各自一个头 |
| `LineCurve.hpp` / `CircleCurve.hpp` / `Ray.hpp` / `SphericalSurface.hpp` | 已实现的曲线与球面值类型及 `*f` / `*d` 别名 |
| `EllipseCurve.hpp` / `HyperbolaCurve.hpp` / `ParabolaCurve.hpp` | 另外三种解析圆锥曲线及 `*f` / `*d` 别名 |
| `BezierCurve.hpp` / `BSplineCurve.hpp` / `OffsetCurve.hpp` | 三种只定义接口的曲线（数据模型 + 类型事实），不可构造 |
| `Surface.hpp` / `ElementarySurface.hpp` / `BoundedSurface.hpp` | 曲面接口与两个分类标签；六个占位曲面各自一个头 |
| `src/base/geometry/src/Instantiations.cpp` | 纯头文件几何类型的显式实例化清单（`float` + `double`） |

## 5. 约束、要求与不变量

- **C1**：曲面标签不携带状态、不声明虚函数；曲线接口的虚函数是全族唯一的多态点（2.1、2.7）。
- **C2**：退化输入不产生 NaN；`isValid()` 是唯一的判定入口。
- **C3**：族的归属在类型定义处声明一次，不在别处重复。
- **C4**：所有 `bounds()` 都是局部空间盒，世界盒要乘节点变换。
- **C5**：`derivative` / `secondDerivative` 返回速度与加速度向量，不是单位切向与曲率向量；`reversedParameter` 自反（`reversedParameter(reversedParameter(u)) == u`）。
- **C6**：圆锥曲线的面内轴经 `inPlaneAxis()` 投影归一化，第二轴由 `normal × x_direction` 导出；退化输入得到 `isValid() == false` 的曲线，不得产生 NaN。
- **C7**：`isCN(n)` 是 `continuity()` 的查询，不单独描述曲线；`G1` / `G2` 不回答 `isCN(1)` / `isCN(2)`（几何连续不是导数连续）。
- **C8**：圆锥曲线的框架（`center` / `normal` / `x_direction`）只在 `ConicCurve` 定义一次并在构造时归一化；四个圆锥曲线不得再声明同名字段，也不得在构造后改写框架。
- **C9**：曲面接口的十三项必须能经 `const Surface<T>&` 回答；极点、缝合不属于“不连续”，`continuity()` 仍答 `CN`；分类标签不得再加状态或虚函数。
- **N1**：`Line::pointAt(t)` 的 `t` 乘的是 `direction` 的长度，只有单位方向时它才是距离；`Ray` 的方向恒为单位，所以两者同名不同量纲。
- **N2**：占位曲面不可构造（删除默认构造），类型树先行、行为等消费者；`Plane` 是唯一不带 `Surface` 后缀的曲面（平面没有第二个含义，OCCT 同样如此）。
- **R1**：`pointAt` / `bounds` / `isValid` / `derivative` 不分配、不抛异常。
- **R2**：族分类不新增外部依赖；标签只 include `geometry_global.hpp`，曲线接口额外只 include `vine/math` 的 `Point3` / `Vector3` 与本目录的 `Continuity.hpp`。

## 6. 注意事项与陷阱

- **P1**：往标签里加虚函数 = 给每个值类型加 vptr（`Ray` 在热循环里按值拷贝）。要共享算法就写自由函数。
- **P2**：所有 `bounds()` 都是**局部**空间的盒，世界盒要乘节点变换；当成世界包围盒缓存会错在场景层级上。

## 7. 已否决选项

- **D1** `Curve` / `Surface` 做成虚基类：给所有值类型加 vptr，打掉内联与 trivially copyable（`G10`、`G8`）。见 `../decisions/ADR-2026-11-04-geometry-kinds.md`。
- **D2** 只提供 concept、不提供标签：`is_base_of` 这类编译期问法与"族"的文档位置都会消失，而且运行期仍需要 variant 覆盖同一集合（`G1`）。
- **D3** 把参数含义提升进基类（统一 `pointAt(t)`）：每种曲线的 `t` 语义不同，统一签名只会把差异藏进文档（`G4`）。
- **D4** 照 OCCT 提供可变的 `reverse()`：曲线不可变、基曲线可被多条裁剪共享，可变反向会互相干扰；反向表达为构造镜像 + `reversedParameter`（2.7）。
- **D5** `bounds()` 进曲线接口：只有有限曲线总能回答；直线只能返回无穷盒，等于把“无界”伪装成盒子（`G4`）。
- **D6** `curveType()` 枚举与 `D3` / `DN`：`dynamic_cast` 与 vtable 已能回答“我是什么”，再维护枚举就是两个家；三阶以上导数没有消费者（`G1`、`G8`）。
- **D7** 曲面继续做空标签：标签答不了“未知曲面也能求值 + 问两个方向的参数域与周期”，而这正是面遍历、采样、求交前置需要的（`G4`、`G9`）。见 `../decisions/ADR-2026-11-04-surface-evaluation-interface.md`。
- **D8** 曲面接口里放 `normalAt()` / `bounds()` / 二阶导数：前两个是派生值或伪值（无界盒），第三个没有消费者（2.10）。

## 8. 测试与验收

| 验收 | 用例 |
|---|---|
| T1 归属、非归属与零成本 | `tests/test_geometry/GeometryKindsTest.cpp` 顶部 `static_assert` |
| T2 曲线参数化与退化输入 | 同文件 `aLineAnswersThePointAtItsParameter`、`aCircleWalksItsOwnPlane`、`aCircleWithoutANormalIsInvalid` |
| T3 球面参数化与退化输入 | 同文件 `aSphericalSheetPutsPolarZeroAtThePole`、`aCircleWithoutANormalIsInvalid` |
| T4 曲线接口的参数域与转发 | 同文件 `aCurveAnswersItsDomainAndPeriod`、`aTrimmedCurveForwardsToItsBasis` |
| T5 曲线核心属性 | 同文件 `aLineCurveHasAConstantVelocity`、`aCircleCurveTurnsTowardsItsCentre`、`aTrimmedCurveMirrorsInsideItsOwnRange` |
| T6 圆锥曲线族 | 同文件 `anEllipseCurveWalksItsTwoAxes`、`aHyperbolaBranchWalksAwayFromItsCentre`、`aParabolaIsEquidistantFromFocusAndDirectrix` |
| T7 样条与偏置的接口 | 同文件 `aBezierCurveDescribesItselfWithoutEvaluating`、`aBSplineCurveDerivesItsContinuityFromItsMultiplicities`、`anOffsetCurveInheritsTheBasisDomain`、`continuityAnswersTheDerivativeOrderQuestion` |
| T8 曲面接口 | 同文件 `aPlaneAnswersTheSurfaceInterface`、`aSphericalSheetClosesOnlyAroundItsAxis` |

## 9. 状态与演进

- 两个族、曲面分 `ElementarySurface` / `BoundedSurface`、四种新值类型、归属与大小断言均已落地。
- 曲线接口的十项属性已落地（2.7），`trimmed` 是唯一的组合曲线；其余占位曲线变实只需要实现这十项。
- 曲面接口的十三项已落地（F8，2.10），`Plane` 与 `SphericalSurface` 实现；两个分类标签仍无状态、无虚函数，但**曲面现在与曲线一样付 vptr**。
- 四种圆锥曲线已全部实现（F6）；`BezierCurve` / `BSplineCurve` / `OffsetCurve` 的数据模型与类型事实已定义（F7，2.8），剩 de Casteljau / de Boor / 叉乘保护三步数值工作，触发条件见 `../backlog/geometry.md` G8。
- 六个曲面是**占位**（类型在树里、不可构造、无参数无算法），它们继承曲面接口后仍是抽象类型；实现等不依赖 OCCT 的裁剪面消费者（`../backlog/geometry.md` G6）。
- 已知缺口：族级算法（射线–曲面、点到曲线距离、实体求交）尚不存在；`Curve` / `Surface` 也还没有 concept 形式的约束，等出现第一个泛型算法时再加。
- 兼容性：曲面接口是从非模板的 `Surface` 标签改为模板接口，是**破坏性变更**（`Surface` / `ElementarySurface` / `BoundedSurface` 现在都要带 `<T>`）；仓库内唯一消费点是 `tests`。

## 10. 参考

- `geometry-primitives.md`（`Aabb` / `Obb` / `LineCurve` / `Plane` 的归属与头文件规则）
- `geometry-parametric.md`（参数化几何与 `Primitive`）
- `geometry-ray.md`（`Ray` 的契约与内联理由）
- `../decisions/ADR-2026-11-04-geometry-kinds.md`
- `../decisions/ADR-2026-11-04-curve-evaluation-interface.md`（曲线接口）
- `../decisions/ADR-2026-11-04-surface-evaluation-interface.md`（曲面接口）
- OCCT `Geom_Curve.hxx` / `Geom_Surface.hxx` / `Geom_Conic.hxx`（接口词汇的来源，本地源码在 `E:\opensrc\OCCT\src\ModelingData\TKG3d\Geom`）
