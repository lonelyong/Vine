# geometry 平面几何：平面曲线契约与第一批值类型

> **状态**：现行
> **最后核对**：2026-11-05
> **范围**：`src/base/geom2d`——平面曲线求值契约 `Curve<T>`、圆锥曲线层 `ConicCurve<T>` 与 `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve` 六个值类型，以及模块的物理形态与命名规则。不含空间曲线与曲面（见 `geometry-kinds.md`），不含点到曲线的最近点、求交、包围盒与多边形（无消费者，见 `../backlog/geom2d.md`）。
> **权威来源**：`src/base/geom2d/sdk/vine/geom2d/Curve.hpp`、`ConicCurve.hpp`、`LineCurve.hpp`、`CircleCurve.hpp`、`EllipseCurve.hpp`、`HyperbolaCurve.hpp`、`ParabolaCurve.hpp`、`ArcCurve.hpp`、`BezierCurve.hpp`、`BSplineCurve.hpp`、`OffsetCurve.hpp`、`CurveProjection.hpp`、`CurveCurveIntersection.hpp`；`tests/test_geom2d/` 下的全部用例文件。
> **待办**：`../backlog/geom2d.md`

## 1. 概要设计

### 1.1 问题与背景

平面几何此前没有自己的类型：要么把点压进 `Point3` 让 `z = 0`，要么在模块外重写一份。前者给平面曲线加了一个它没有的法向和一条恒为零的维度，3D 侧的算法因此会“接受”一条平面曲线并按空间曲线解释它；后者让“曲线”这个概念出现第二套没有契约的定义。`geom2d` 在那之前只有一个命名空间，以及 `Continuity` 的一次重导出。

### 1.2 目标

- 一份**独立**的平面曲线契约：与 `geom3d::Curve<T>` 逐项同义（求值、一二阶导数、参数域、闭合与周期、光滑度、有效性、反向映射），只是点是 `Point2`、向量是 `Vector2`。
- 九个能被执行验证的值类型（直线、圆、椭圆、双曲线、抛物线、圆弧、Bezier、BSpline、偏置），让契约从“声明”变成“可测的行为”。
- 两个**族级算法**（点到曲线的最近点、两段曲线的求交），证明十一项契约足够回答曲线上的几何问题，而不需要知道具体类型。
- 一次说清模块的物理形态与命名规则：header-only / INTERFACE 目标、名字不带维度标记、精度别名仍是 `d` / `f`。

### 1.3 非目标

- 不把 3D 曲线族整族搬过来：通用裁剪视图仍等消费者（`G8`，见 `../backlog/geom2d.md`）。
- 算法只做“族级查询”这一类，今天有最近点与两段曲线的求交：弧长参数化、两曲线之间的最近距离、曲率仍不做（同 backlog）。
- 不借用 3D 的 `ConicCurve` 框架：平面里的圆锥曲线没有法向可存（2.4）。
- 不做屏幕 / 像素几何：`Point2`、`Vector2`、`Rect2` 属于 `math`，这个模块只有曲线。

### 1.4 一句话结论

**平面曲线是第二份契约、不是 3D 契约的特化：同样的十一项事实、`Point2` / `Vector2`、同一个 `math::Continuity`；今天有直线、圆、椭圆、双曲线、抛物线、圆弧、两种样条与偏置，其余等真实消费者。**

## 2. 设计要点

### 2.1 独立契约，不做 3D 的泛型特化

- **是什么**：`geom2d::Curve<T>` 自己声明十一项纯虚函数 + 虚析构，和 `geom3d::Curve<T>` 同名同义；两者之间没有继承、没有共享基类、没有以点类型为参数的公共模板。
- **为什么**：`G1`、`G5` —— “平面曲线”和“空间曲线”是两个概念，各自有一个定义、一个归属。做一份 `Curve<PointT>` 的泛型契约，等于把 3D 的每个消费点变成模板，并让 2D 类型被迫满足只有 3D 才需要的约束；让 2D 契约继承 3D 契约，则平面曲线必须带一个恒定法向和一条恒为零的维度，3D 算法会误收它。决策与被拒选项见 `../decisions/ADR-2026-11-05-planar-curve-contract.md`。
- **代价**：契约文字有两份，一侧新增事实时另一侧可能悄悄漂移（`G6`）。缓解只做两件事，不引入机制：共享词汇统一住在 `math`（`Continuity`、`FP`、将来的求根器），`geom2d_global.hpp` 把 `Continuity` 重导出成同一个类型，让两边写的是同一个名字；契约里只放**与维度无关**的事实（2.5），需要新事实时两份文档同时改。

### 2.2 名字不带维度标记

- **是什么**：`LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve` / `BezierCurve` / `BSplineCurve` / `Curve`，不是 `LineCurve2d` / `Curve2D`；精度别名仍是 `LineCurved` / `CircleCurvef` / `EllipseCurved` / `HyperbolaCurvef` / `ParabolaCurved` / `ArcCurvef` / `BezierCurved` / `BSplineCurved`，与 `geom3d` 的 `d` / `f` 规则一字不差。
- **为什么**：`G4`、`G6` —— 命名空间已经是限定词，“2d”写在类型名里就成了一句重复的说明；精度后缀表达的是“哪种标量”，与维度无关，所以它必须保留原样，否则同一个概念在两层里读法不同。
- **边界**：同名的 `geom2d::LineCurve` 与 `geom3d::LineCurve` 是两个类型。同时使用两侧的代码 `using` 其中一个、限定另一个；不得再造 `Line2d` 这类别名（D3）。

### 2.3 契约只放“曲线必然能回答”的事实

- **是什么**：十一项全纯虚——① 求值与导数：`evaluate` / `derivative` / `secondDerivative`；② 参数域与周期：`firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period`；③ 光滑度：`continuity`（及由它导出的非虚 `isCN(n)`）；④ 反向参数映射：`reversedParameter`；⑤ 有效性：`isValid`。
- **为什么**：`G4`、`G9` —— 这是“参数化曲线”的最小完备集，和 3D 侧同一套判据（`geometry-kinds.md` 2.7）：一个点、它的变化率、它在哪里有效、它有多光滑、反着走参数怎么变、这份状态能不能用。平面侧不是“少几项”，而是同一组事实少一个维度。
- **不进接口的东西与 3D 侧逐条相同**：`bounds()`（无界曲线只能返回无穷盒）、`length()` / 弧长参数化（是算法）、变换族（坐标系住在 `Shape` 节点上）、`curveType()` 枚举（vtable 已经回答了“我是什么”）、高于二阶的导数。理由只在 `geometry-kinds.md` 2.7 与 D6 写一次，这里不复制。

### 2.4 非圆圆锥曲线共享一层起始轴，圆不进这层

- **是什么**：`CircleCurve` 存 `center` 与 `radius`，`P(u) = center + radius * (cos u, sin u)`；参数是弧度、域 `[0, 2π]`、逆时针、反向映射是 `2π - u`，它没有框架。`ConicCurve<T>` 持有 `center`（抛物线的顶点就是它）+ `x_direction`（构造时归一化）+ 派生的 `yAxis()`（+90° 旋转）+ `continuity() == CN`；`EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 派生自它，只声明自己的标量。
- **为什么**：`G1`、`G13`、`G8` —— 3D 的圆锥框架需要法向，是因为曲线在空间里必须回答“在哪个平面、从哪条轴量角”；平面里没有平面可选，只剩一根起始轴。圆连起始轴也没有——逆时针与 +X 是唯一约定——为它存一根永远 `(1,0)` 的轴就是无法被检验的字段。三条非圆圆锥曲线确实共享同一组事实，所以这一层现在有真实消费者，而不是为一个字段预建的抽象。
- **边界**：层里不放求值与参数域——参数的含义各类型不同（偏心角、双曲角、垂直轴上的偏移）；退化方向存零向量，所有点查询因此答 `center`，`isValid()` 是唯一判据。OCCT 的 `Geom2d_Circle` 反而派生自 `Geom2d_Conic`、带 `gp_Ax2d`（起点可旋转）；这条我们不跟，见 D5。
- **直线同理**：`LineCurve` 存 `origin` 与 `direction`，方向不需要单位长度，参数是方向的倍数——与 3D 的 `LineCurve` 同一契约（`geometry-primitives.md` 2.3）。

### 2.5 header-only，`vn::Geom2D` 是 INTERFACE 目标

- **是什么**：十三个头文件都是 `template <vn::math::FP T>` 的完整定义，模块没有 `src/` 目录；`vn_add_library` 因此生成 INTERFACE 目标，只声明 `target_link_libraries(... INTERFACE vn::Math vn::Core)`。
- **为什么**：`G10`、`G6` —— 精度模板决定了实现必须在调用方编译；有 `src/` 的模块要维护“每个类型 × 两种精度”的显式实例化表（`geometry-primitives.md` 2.8），而这里一个符号都不导出，那张表也就不存在。目标名、include 目录与安装规则仍走同一个 helper，所以它和 `vn::Geom3D` 的用法没有区别。
- **代价**：没有 ABI 边界，曲线代码在每个消费方各编译一次；也没有 `VN_GEOM2D_API` 可加。等到出现第一个非模板实现（需要缓冲的曲线、或某个查询的 `.cpp`），这条要重新评估，届时 `src/` 一出现目标就自动变成 SHARED 库。

### 2.6 退化状态给答案，不失败

- **是什么**：`isValid()` 是唯一判据，且只看**状态**、不看参数。无效状态下查询**必须有限**，具体答什么由类型文档写明：零方向直线答 `origin`；半径为零的圆答 `center`，负半径仍描出同一个圆；零主轴的圆锥曲线答 `center`，半轴或焦距为零时塌成线段 / 直线 / 顶点；焦距为零的抛物线答顶点（它本来会除零）。反向区间的圆弧答无效，查询仍转发给圆。
- **为什么**：`G4`、`G11` —— 与 3D 侧的规则一字不差（`geometry-kinds.md` C12）：错误变成**可检查的错误答案**，而不是未定义行为或异常；调用方在入口检查一次 `isValid()`，内部循环就不必逐个参数判空。

### 2.7 值类型保留比接口更便宜的路径

- **是什么**：`LineCurve::pointAt(t)` 是 `constexpr`，`CircleCurve::pointAt(angle)` / `EllipseCurve::pointAt(angle)` / `HyperbolaCurve::pointAt(u)` / `ParabolaCurve::pointAt(u)` 只是类型上的普通成员（三角函数不是常量表达式）；`evaluate()` 都转发给它们。具体类型的调用因此可以内联，而 `Curve<T>&` 上的调用必然要过一次虚表。
- **为什么**：`G7`、`G10` —— 多态只在“不知道具体类型”时才有价值，热路径必须能绕开它；这条路径是**加法**（多一个成员），不是把复杂度推给调用方。
- **边界**：不为对称而对称。`ArcCurve` 没有 `pointAt()`——它只会转发给基圆，不省任何东西；其余五个类型的 `pointAt()` 与各自 `evaluate()` 同值，圆的那个与 3D 同名类型一致。

### 2.8 圆弧是值类型上的裁剪视图

- **是什么**：`ArcCurve` 存一个 `CircleCurve`（按值）加 `first_angle` / `last_angle`；求值与导数转发给圆，闭合与周期由“基曲线 + 区间”共同决定，`period()` 只在自身 `isPeriodic()` 为真时回答 `2π`，反向映射在区间内反射。
- **为什么**：`G6`、`G10` —— 与 3D 的 `TrimmedCurve` 同一条规则（闭合、周期、反向、越界行为逐条照抄），但基曲线按值存：一个圆只有两个字段，复制它比一次堆分配便宜，也让 R2（值类型不持有动态内存）继续成立。
- **代价**：没有通用的平面裁剪。“任意曲线 + 区间”的 `TrimmedCurve` 等第二个非圆基曲线；当它出现时，通用视图**取代** `ArcCurve`，不与之并存（D6、`../decisions/ADR-2026-11-05-planar-arc-is-a-value-type.md`）。

### 2.9 族级查询是自由函数，窗口由调用方给

- **是什么**：`closestPointOnCurve(curve, query, window, tolerance, sample_count)` 是头文件模板，`C` 由 `requires std::is_base_of_v<Curve<T>, C>` 约束；结果 `CurveProjection<T>{ point, parameter, distance }`，一个极值同时回答“最近点在哪、参数多少、距离多少”。
- **为什么**：`G1`、`G9` —— 查询只依赖 `evaluate` / `derivative` / 域端点 / `isValid`，写成自由函数而不是接口虚函数，“两个对象的关系”才装得下，六个实现也不必各写一遍。形态与规则照搬 3D 的 `closestPointOnCurve()`（`geometry-curve-queries.md` 2.1–2.4），调用方不必记两套约定。
- **窗口规则**：无界曲线（直线、双曲线、抛物线）**必须**给窗口；有界曲线（圆、椭圆、圆弧）省略时用自己的域；没有有限窗口就返回 `std::nullopt`，函数从不自选范围。
- **边界**：这两个查询是模块里仅有的算法，`G8` 的张力写在这里——它们今天没有生产消费者，价值是**验证契约**：如果十一项不够，这两个函数就写不出来。第三个算法仍按 backlog 的触发条件进入。

### 2.10 包围盒是有界类型自己的查询，不在接口上

- **是什么**：`CircleCurve` / `EllipseCurve` / `ArcCurve` 各有一个 `bounds()`，返回 `math::Rect2<T>`；`LineCurve` / `HyperbolaCurve` / `ParabolaCurve` 没有。
- **为什么**：`G4`、`G13` —— 无界曲线只能返回无穷盒或一个假盒，把“无界”伪装成矩形，所以接口不声明它（与 `geometry-kinds.md` 2.7 同一条判据）。平面盒直接复用 `math::Rect2`，模块不为一个盒子再建一个类型。
- **边界**：盒是 `evaluate()` 描出的**点集**的盒：圆是 `center ± |r|`；椭圆用支撑函数 `sqrt((a·X_i)² + (b·Y_i)²)`，不是 `±a` 的松盒；圆弧取两个端点加区间内的四个轴向极值。区间的方向不改变点集，所以反向区间答同一个盒。

### 2.11 两段曲线的求交：自由函数 + 三值结果

- **是什么**：`intersectCurveWithCurve(first, second, first_window, second_window, tolerance, sample_count)` 返回 `CurveCurveIntersection<T>{ contact, hits }`：`contact` 是 `None` / `Isolated` / `Coincident`，每个 hit 带点与两边各自的参数，按第一条曲线的参数升序。
- **为什么**：`G12`、`G4` —— 两个对象的关系不属于其中任何一个，所以是自由函数；结果必须是三值的，因为“交于一点”“整段重合”“没有交点”是三种不同的答案，把重合段拆成一串点既错又不收敛（与 3D 的 `CurveSurfaceContact` 同一个理由）。
- **怎么算**：沿第一条曲线采样，每点用 `closestPointOnCurve()` 取到第二条曲线的垂距——一个量同时给出候选（垂距在一步长度内）与重合判定（连续至少三个样本垂距在容差内）；候选再用二维 Newton 收敛 `C1(u) = C2(v)`，括号取自两个窗口。
- **边界**：局部方法——相切默认漏检（P9）；两交点间距小于三个采样步长会被读成重合；窗口规则同 C5。

### 2.12 样条是持有内存的曲线

- **是什么**：`BezierCurve` 存极点与可空权重，`degree()` 由极点数减一得出；`BSplineCurve` 存相异节点 + 重数、极点与可选权重，`continuity()` 由内部重数得出（`knotContinuity`）。两者的求值、一阶与二阶导数都是 `math::spline` 的同一类东西：Bezier 用极点/一阶差商/二阶差商的 de Casteljau 并乘 `1 / n / n(n-1)`，BSpline 用所在跨的 `p+1` 个基函数及其导数加权求和。
- **为什么**：`G1`、`G10` —— 数据模型就是接口：不另存度数（两份会矛盾），用空数组而不是 `isRational` 布尔（布尔与数据可以互相矛盾）；blending 核在 `math`，与维度无关，所以这一层只负责数据与缩放，不复写求值。
- **代价**：深拷贝、不再 trivially copyable，`evaluate` 每次分配一份临时数组（`deCasteljau` 按值取数组）——R2 因此改写为“默认不持有内存，例外要写明”；这是有意的取舍，与 3D 同名类型一致，而字面量采样时它仍然比虚调用便宜。
- **边界**：两者的 `bounds()` 都是**极点盒**（曲线在极点凸包内），不是紧盒；紧盒要解每个坐标导数的根，今天没有消费者（P10）。有理形式的分母在域内不会归零（权重必须为正）。Bezier 的域外参数是多项式外推；BSpline 的域是首末相异节点，域外参数被夹到端点，不向外推（与 OCCT 的同名类型一致）。

### 2.13 偏置是具体基曲线类型的模板，三阶导留在具体类型上

- **是什么**：`OffsetCurve<C>` 按值持有基曲线 `C` 与一个有符号距离，`O = C + d·N`，`N` 是左法向（`C'` 逆时针转 90°）；三个数值回答都是精确的，二阶导用基曲线的三阶导。
- **为什么**：`G4`、`G10`、`G6` —— 偏置的二阶导不可避地需要 `C'''`；把它做成 `Curve<T>` 的第 12 项纯虚，等于让所有曲线类型与所有依赖“契约只到二阶”的算法一起付账（见 `../decisions/ADR-2026-11-05-planar-offset-curve.md`）。三阶导作为具体类型的非虚成员只加在能闭式回答的人身上，契约仍是十一项。
- **代价**：八份三阶导实现；偏置的基曲线在编译期确定，无法持有 `Curve<T>&` 句柄（运行期分派留给调用方）；不给 `bounds()`。
- **边界**：正距离在行进方向左侧（逆时针圆的偏移圆半径因此是 `r - d`）；基曲线速度为零的尖点上法向未定义，查询退回基曲线自身的值，不产生 NaN；同一几何曲线反向走是“反向基曲线 + 取反距离”。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 平面曲线契约 | `Curve<T>` 十一项纯虚 + `isCN()`，`Continuity` 来自 `math` | 已落地（T1–T5） |
| F2 | 平面直线 | `LineCurve`：无界、非周期、`CN`，退化方向给原点 | 已落地（T1–T4） |
| F3 | 平面圆 | `CircleCurve`：弧度参数、闭合周期 `2π`、逆时针、退化半径给圆心 | 已落地（T1–T5） |
| F4 | 平面椭圆 | `EllipseCurve`：偏心角参数、闭合周期 `2π`、自带主轴方向、退化轴给中心 | 已落地（T1–T6） |
| F5 | 平面圆弧 | `ArcCurve`：圆按值 + 角度区间，闭合 / 周期只看区间是否跨满圆的域 | 已落地（T1–T6） |
| F6 | 平面双曲线与抛物线 | `HyperbolaCurve` / `ParabolaCurve`：双曲角与垂直轴偏移、无界非周期、共享 `ConicCurve` 的起始轴 | 已落地（T1–T6） |
| F7 | 点到曲线的最近点 | `closestPointOnCurve()`：平方距离驻点求根 + 窗口端点比较，返回点 / 参数 / 距离 | 已落地（T7） |
| F8 | 平面包围盒 | 圆 / 椭圆 / 圆弧的 `bounds()`，返回 `math::Rect2`；无界类型没有这个入口 | 已落地（T8） |
| F9 | 平面曲线求交 | `intersectCurveWithCurve()`：采样 + 垂距候选 + 二维 Newton，结果三值（无交点 / 有限交点 / 重合段） | 已落地（T9） |
| F10 | 平面样条 | `BezierCurve`（极点 + 可选权重，de Casteljau）与 `BSplineCurve`（相异节点 + 重数，基函数求和，连续度由重数得出） | 已落地（T10、T11） |
| F11 | 通用裁剪 | 任意基曲线的 `TrimmedCurve`（落地时取代 `ArcCurve`） | 未做（`../backlog/geom2d.md` P1） |
| F12 | 平面偏置 | `OffsetCurve<C>`：左法向、有符号距离，三个导数都精确 | 已落地（T12） |

### 3.1.1 F1 平面曲线契约

- **行为**：`const Curve<T>&` 上可求任意参数的点与一二阶导数、问参数域 / 闭合 / 周期 / 光滑度 / 反向映射 / 有效性；`isCN(order)` 由 `continuity()` 推出。
- **约束**（C1、C2、C3）：
- **要求**（R1、R2）：
- **验收**（T1–T6）：
- **注意**（N2、N3、P4）：

### 3.1.2 F2–F6 解析曲线族（直线、圆、椭圆、双曲线、抛物线）

- **行为**：直线 `origin + direction·t`（全实轴、永不闭合）；圆 `center + r·(cos u, sin u)` 与椭圆 `center + a·cos u·X + b·sin u·Y`（域 `[0, 2π]`、闭合、周期 `2π`）；双曲线 `center + a·cosh u·X + b·sinh u·Y` 只走 `+X` 一支，抛物线 `vertex + u²/(4f)·X + u·Y`（两者无界非周期，`reversedParameter` 为 `-u`）。全部 `CN`。
- **约束**（C1–C3）：每种类型只接受自己可用的尺寸（方向、半径、半轴、焦距），否则 `isValid()` 为假，点查询塌到该类型文档写明的有限退化值。
- **要求**（R1、R2）：闭式求值、不分配；圆的 `derivative` 长度等于半径，椭圆二阶导是 `center - P(u)`，双曲线加速度是 `P(u) - center`，抛物线加速度是常量 `X/(2f)`。
- **验收**（T1–T6）：
- **注意**（N1–N4、P1–P3）：

### 3.1.7 F7–F9 族级查询与包围盒

- **行为**：最近点取平方距离驻点的零点（`math::refineRoot()` 收窄）+ 采样点与窗口端点比较；包围盒是 `evaluate()` 在域内描出的点集的轴对齐盒（圆 `center ± |r|`、椭圆用支撑函数、圆弧取两端点加区间内轴向极值）；求交返回三值结果（`None` / `Isolated` 带两边参数 / `Coincident` 不给点）。
- **约束**（C5–C7）：查询窗口必须有限，无界曲线由调用方给；盒只对有界类型存在且必须包住点集；`Coincident` 时不列点。
- **要求**（R1、R2）：只用 `evaluate` / `derivative` / 域端点 / `isValid` 与 `closestPointOnCurve()`；不抛异常。
- **验收**（T7–T9）：
- **注意**（N5–N7、P7–P9）：

### 3.1.10 F10 平面样条（Bezier 与 BSpline）

- **行为**：`BezierCurve` 用极点的 de Casteljau 混合求值，差商给出两阶导，域 `[0, 1]`，`isClosed()` 只看首末极点，反向是 `1 - u`；`BSplineCurve` 用所在跨的基函数求和，域是首末相异节点，`continuity()` 由内部重数得出，`isClosed()` 比较两端点。两者都可选权重而成为有理曲线（四分之一圆是标准例子）。
- **约束**（C1、C3）：极点与权重必须可用；BSpline 还要求夹持节点向量——首末重数 `= 度 + 1`、内部重数 `<= 度`。不可用时答首极点（无极点则答原点）。
- **要求**（R1、R2）：复用 `math::spline` 的核；Bezier 的域外参数是外推，BSpline 是夹到端点。
- **验收**（T10、T11）：
- **注意**（N8、P10）：

### 3.1.11 F12 平面偏置

- **行为**：`OffsetCurve<C>` 给出 `C + d·N`、`C' + d·N'`、`C'' + d·N''`；参数域与反向映射转发给基曲线，闭合要求两端点重合（基曲线闭合还不够），周期跟随基曲线。
- **约束**（C8）：正距离是行进方向左侧；反向基曲线需取反距离才追同一条几何曲线；基曲线速度为零处退回基曲线自身值。
- **要求**（R1、R2）：基曲线按值持有，不分配额外内存；三阶导只从具体类型取（2.13）。
- **验收**（T12）：
- **注意**（N9、P11）：

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `geom2d_global.hpp` | `VN_GEOM2D_NS_BEGIN` / `_END`，`VN_GEOM2D_API`，`Continuity` 的重导出 |
| `Curve.hpp` | `Curve<T>` 与 `isCN()` |
| `ConicCurve.hpp` | `ConicCurve<T>`：`center` + `x_direction` + `yAxis()` + `CN` |
| `LineCurve.hpp` | `LineCurve<T>`、`LineCurvef` / `LineCurved` |
| `CircleCurve.hpp` | `CircleCurve<T>`、`CircleCurvef` / `CircleCurved` |
| `EllipseCurve.hpp` | `EllipseCurve<T>`、`EllipseCurvef` / `EllipseCurved` |
| `HyperbolaCurve.hpp` | `HyperbolaCurve<T>`、`HyperbolaCurvef` / `HyperbolaCurved` |
| `ParabolaCurve.hpp` | `ParabolaCurve<T>`、`ParabolaCurvef` / `ParabolaCurved` |
| `ArcCurve.hpp` | `ArcCurve<T>`、`ArcCurvef` / `ArcCurved` |
| `BezierCurve.hpp` | `BezierCurve<T>`、`BezierCurvef` / `BezierCurved` |
| `CurveProjection.hpp` | `CurveProjection<T>` 与 `closestPointOnCurve()` |
| `CurveCurveIntersection.hpp` | `CurveCurveContact`、`CurveCurveHit<T>`、`CurveCurveIntersection<T>` 与 `intersectCurveWithCurve()` |

完整签名与每个参数的语义见头文件；本表只说明归属。

## 5. 约束、要求与不变量

- **C1**：参数用类型自己的量纲（直线是方向的倍数，圆、椭圆与圆弧是弧度，双曲线是双曲角，抛物线是垂直轴上的偏移）。参数域是曲线的属性，不是 `evaluate` 的前置条件；域外参数的意义由类型文档写明。
- **C2**：`period()` 只在 `isPeriodic()` 为真时有意义，否则为 0；有界的片段不转发整圈的周期（与 3D 的 `TrimmedCurve` 同一条规则）。
- **C3**：`isValid()` 只看状态，与参数无关；无效状态下的每条查询返回类型文档写明的退化值，不得产生 NaN 或无穷。
- **C4**：`evaluate` / `derivative` / `secondDerivative` 是常数时间的闭式求值，不分配、不抛异常。
- **C5**：族级查询的搜索区间必须有限：无界曲线必须由调用方给窗口，没给、反向或零宽都返回 `std::nullopt`，函数不自选范围（与 `geometry-curve-queries.md` C1 同一条规则）。
- **C6**：包围盒只在有界类型上提供，且只由 `bounds()` 回答；接口不声明 `bounds()`，无界类型没有这个入口。
- **C7**：求交结果是三值的：`Coincident` 表示“一段重合”，此时不列点；`Isolated` 才给 `hits`，每个 hit 的点同时在两条曲线上。
- **C8**：偏置的符号跟行进方向走：正距离在左侧；反向基曲线要追同一条几何曲线必须用**取反**的距离；基曲线速度为零的尖点上法向未定义，查询退回基曲线自身的值。
- **R1**：平面曲线只依赖 `Point2`、`Vector2`、标量与 `math` 的现有设施；不得引入 3D 概念（法向、框架、z 分量）。
- **R2**：值类型**默认**不持有动态内存。`BezierCurve` 是已经写明的例外：`std::vector` 极点与可选权重意味着深拷贝、不再 trivially copyable（与 3D 同名类型同一取舍）；任何新引入的持有内存的类型必须在这条上写下同样的例外与代价。
- **N1**：提供 `pointAt()` 的类型（直线、圆、椭圆、双曲线、抛物线）上它与 `evaluate(u)` 同值。
- **N2**：`reversedParameter` 是 involution，并把参数域两端互换（`reversedParameter(firstParameter()) == lastParameter()`）。它给出的是**反向曲线**上同一个点的参数——反向曲线是原曲线关于域中点轴（周期曲线是起始轴）的镜像，`evaluate(reversedParameter(u))` 不等于 `evaluate(u)`。
- **N3**：`derivative` 是速度向量而非单位切向（圆的长度 = 半径，双曲线在顶点最小、向两端增长）；`secondDerivative` 只在弧长参数化时等于曲率向量（抛物线则是常量）。
- **N4**：圆与椭圆的 `period() == lastParameter() - firstParameter() == 2π`，域外参数按周期性回绕；圆弧只在区间跨满圆的域时回答 `2π`，更短的一段回答 0（C2）。
- **N5**：`CurveProjection::point == curve.evaluate(parameter)`，`distance == |point - query|`；等距多解时报告其中一个（圆心到圆上每一点等距，答案取决于扫描与求根落在哪里）。
- **N6**：`bounds()` 包含 `evaluate(u)` 在曲线域内的每一个点（含端点与区间内的轴向极值）；椭圆的盒是支撑函数给出的紧盒，不是半轴造的松盒。
- **N7**：`CurveCurveHit::point` 同时等于 `first.evaluate(first_parameter)` 与 `second.evaluate(second_parameter)`；`hits` 按 `first_parameter` 升序，同一交点只报一次。
- **N8**：`BezierCurve::degree() == poles.size() - 1`；`isRational() == !weights.empty()`；有理曲线的每个 `evaluate` 都在极点凸包内。
- **N9**：`OffsetCurve::evaluate(u) == basis.evaluate(u) + distance * normalAt(u)`，`normalAt(u)` 是基曲线切向的 +90° 单位旋转；它的两个导数都是闭式精确值，不是差分近似。

## 6. 注意事项与陷阱

- **P1 参数量纲混用**。会怎么错：把弧长当角度传给圆，得到“看起来在圆上但不是想要的位置”的点，没有报错。怎么判：用已知点反算参数，或换一个半径后结果不按比例变化。怎么防：读类型的 `@param`（圆一律写“弧度”）；跨类型调用时在参数名上带量纲，如 `arc_length` / `angle`。
- **P2 把 `derivative` 当单位切向**。会怎么错：直线的方向不是单位长度、圆的切向长度是半径，归一化前拿它当方向，比例全错。怎么判：同一曲线在两点上的切向长度不同。怎么防：用前先 `normalized()`（N3）。
- **P3 退化状态给出“合理”的答案**。会怎么错：半径 0 的圆对每个参数都答圆心，零主轴的椭圆 / 双曲线 / 抛物线答中心，零方向直线对每个参数都答原点，后续算法把它当成真的曲线。怎么判：结果与参数无关。怎么防：入口判 `isValid()`（C3）。
- **P4 `float` 上的 `2π` 比较**。会怎么错：`CircleCurvef::period() == math::PI_TWO` 为假，`isPeriodic()` 分支被误判。怎么判：同一用例用 double 通过、float 不通过。怎么防：参数比较一律带容差（`EPS<T>()` 或按参数尺度定的容差），只对布尔事实用精确比较。
- **P5 经 `Curve<T>&` 的虚调用**。会怎么错：批量采样挂在基类引用上，每次都过虚表，且编译器无法内联。怎么判：反汇编或计时代码在具体类型上明显更快。怎么防：热循环持有具体类型（2.7）。
- **P6 把反向映射当成“同一个点”**。会怎么错：以为 `evaluate(reversedParameter(u)) == evaluate(u)`，实际拿到的是关于域中点轴（圆与椭圆是起始轴）的镜像点，两条曲线被当成一条用。怎么判：反向后的点与原点在 `u = 域中点` 两侧对称，端点互换而中间点不重合。怎么防：只把 `reversedParameter` 用于“反向曲线上的同一点”（N2）；需要同一点时直接 `evaluate(u)`。
- **P7 最近点是局部最小**。会怎么错：曲线来回靠近查询点，采样只找到其中一个极小值，距离偏大；`f` 相切于零的平坦极小值没有可收窄的区间。怎么判：`sample_count` 加倍后距离变小。怎么防：按曲线起伏尺度提高 `sample_count`；同一分析的完整版本（开区间、相切、采样别名）在 `geometry-curve-queries.md` §6。
- **P8 用半轴当包围盒**。会怎么错：旋转过的椭圆按 `±major_radius` 造盒，盒大一圈，裁剪与拾取多测无用区域。怎么判：盒角点不在曲线上，且盒宽等于半轴的一半。怎么防：用 `bounds()`（支撑函数），不自己按半轴造盒（2.10）。
- **P9 相切与重合只差一个采样步长**。会怎么错：两条曲线相切时没有横向根，默认漏检；两个交点挨得比三个采样步长还近时被读成 `Coincident`。怎么判：加密 `sample_count` 后相交关系变了，或把容差调小后重合消失。怎么防：知道这是“交集形状”的判定而不是逐点判定；需要区分时加密采样并把容差调到远小于曲率半径。
- **P10 把极点盒当紧盒**。会怎么错：用 `BezierCurve::bounds()` 裁剪或估算尺寸，得到的是极点凸包的盒，比曲线大。怎么判：盒角点远离曲线，且盒尺寸只由极点数决定。怎么防：需要紧盒时采样后再收紧；极点盒只用作拒绝测试的粗包。
- **P11 把偏置当“处处可导”**。会怎么错：对含尖点的基曲线（重合的首极点、零长度方向）求偏置二阶导，`continuity()` 仍答基曲线的值，而法向在那里不存在。怎么判：该参数处 `derivative().length()` 为零。怎么防：先看速度不为零再用二阶导；需要尖点两侧各算一次。

## 7. 已否决选项

- **D1** 一份 `Curve<PointT>` 泛型契约，或让 `geom2d::Curve` 继承 3D 契约：`G1`、`G5`——两个维度本是两个概念，合并后 2D 类型被迫携带法向与 z 分量，3D 算法也会误收平面曲线。见 `../decisions/ADR-2026-11-05-planar-curve-contract.md`。
- **D2** 平面圆借用 3D `ConicCurve` 的框架（`normal` + `x_direction`）：`G1`、`G8`——平面里没有可存的方向，两个字段既无法取值也无法校验，只让每个圆多 24 字节。
- **D3** 名字加维度标记（`Curve2d` / `Circle2d`）或再补 `*2d` 别名：`G4`、`G6`——命名空间已是限定词，别名会让同一个概念有两个名字。
- **D4** 现在就把 3D 曲线族整套复制过来：`G8`——没有消费者的类型只会在两侧同时漂移；类型按 `../backlog/geom2d.md` 的触发条件逐个引入。
- **D5** 学 OCCT 让平面圆也带一根起始轴（`Geom2d_Circle` 派生自 `Geom2d_Conic`、持 `gp_Ax2d`）：`G8`、`G4`——半径相同、起点可旋转的圆是同一个几何，多一份可写状态只会让“参数 0 在哪”多一条隐含约定；没有消费者要求过旋转圆的参数原点。
- **D6** 在 `ArcCurve` 旁边再加一个通用 `TrimmedCurve<T>`：`G1`、`G8`——“圆的一段”只该有一种表达；真需要裁剪非圆基曲线时，通用视图取代 `ArcCurve`。见 `../decisions/ADR-2026-11-05-planar-arc-is-a-value-type.md`。
- **D7** 给 `Curve<T>` 加第 12 项纯虚 `thirdDerivative()`（或让偏置的二阶导用差分近似）：`G4`、`G10`——一个派生类型的需要不能抬高整个契约，近似又换掉了“导数是精确值”的语义。见 `../decisions/ADR-2026-11-05-planar-offset-curve.md`。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 参数化：直线沿线、圆按角 | `tests/test_geom2d/CurveBasicsTest.cpp` 的 `WalksAlongItsDirection`、`WalksItsAngle` |
| T2 | 导数语义：直线常量速度、圆的速度与向心加速度 | 同文件 `HasAConstantVelocity`、`TurnsTowardsItsCentre` |
| T3 | 域 / 闭合 / 周期 / 光滑度 | 同文件 `IsUnboundedAndNeverClosed`、`IsClosedPeriodicAndSmooth` |
| T4 | 退化状态答退化值而非 NaN | `CurveBasicsTest.cpp` 的 `AZeroDirectionIsNotALine`、`ANonPositiveRadiusIsNotACircle`；`EllipseCurveTest.cpp`、`ConicCurvesTest.cpp` 的退化用例 |
| T5 | 反向映射是 involution、交换域两端，且经 `Curve<T>&` 与 `unique_ptr<const Curve<T>>` 可用 | `CurveBasicsTest.cpp` 的 `ReversesByNegatingItsParameter`、`ReversesByReflectingInsideThePeriod`、`AnswersThroughTheBaseInterface`；`EllipseCurveTest.cpp`、`ArcCurveTest.cpp` 各自的反向与多态用例 |
| T6 | 契约扫查：域有序、反向 involution、闭合曲线两端同点、周期曲线按 `period()` 重复、非周期曲线 `period() == 0`，九个类型 × float / double；另有一份导数链检查（每阶导与前一阶导的中心差分一致，含两种有理形式） | `tests/test_geom2d/CurveContractTest.cpp` 的 `EveryPlanarCurveKeepsItsPromisesInDouble` / `…InFloat` / `EveryCurveDifferentiatesConsistently` |
| T7 | 最近点：直线精确足点、无界曲线必须给窗口、足点落在窗口外落到端点、圆 / 椭圆 / 圆弧的足点、圆心的等距多解、无效曲线、两种精度 | `tests/test_geom2d/CurveProjectionTest.cpp` |
| T8 | 包围盒：圆的负半径 / 零半径、旋转与 45° 椭圆的支撑函数、圆弧区间内极值、反向区间、盒包含全部采样点、两种精度 | `tests/test_geom2d/BoundsTest.cpp` |
| T9 | 求交：直线 × 直线、平行线、直线 × 圆（两点且按第一条曲线参数升序）、两圆、圆弧被窗口裁剪、重合弧与重合圆、无界曲线缺窗口、无效曲线、两种精度 | `tests/test_geom2d/CurveIntersectionTest.cpp` |
| T10 | Bezier：极点行进、差商导数、有理四分之一圆、无效极点 / 权重、闭合与反向、极点盒、族级算法（最近点与求交）无需改动即可用、两种精度 | `tests/test_geom2d/BezierCurveTest.cpp` |
| T11 | BSpline：夹持二次即 Bezier、折线节点、重数决定 `C0` / `C1`、有理四分之一圆、域外夹到端点、不一致节点向量、闭合与反向、极点盒、族级算法、两种精度 | `tests/test_geom2d/BSplineCurveTest.cpp` |
| T12 | 偏置：直线左移、逆时针圆向内、椭圆与 Bezier 的二阶导与差分一致、样条三阶导、反向基曲线取反距离、零距离即基曲线、尖点不产生 NaN、族级算法可用 | `tests/test_geom2d/OffsetCurveTest.cpp` |

## 9. 状态与演进

### 9.1 当前状态

十三个头文件（`Curve` / `ConicCurve` / `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve` / `BezierCurve` / `BSplineCurve` / `OffsetCurve` / `CurveProjection` / `CurveCurveIntersection`）+ `test_geom2d`（99 个用例，含九个类型 × float / double 的契约扫查、导数链检查与两个族级算法的双精度对照）；`vn::Geom2D` 是 INTERFACE 目标，不导出任何符号；模块内没有生产代码消费者——今天唯一的使用者是测试。

### 9.2 已知缺口

- 族里没有通用裁剪视图：圆的一段由 `ArcCurve` 表达，非圆基曲线的裁剪未做（`../backlog/geom2d.md` P1）。
- 族级算法只有两个：两曲线之间的最近距离、弧长参数化、曲率都未做。

### 9.3 兼容性 / 迁移

纯增量：`geom3d` 的接口与类型未改动，两个模块互不引用。新增平面类型只影响本模块与 `tests/test_geom2d`；因为目标是 INTERFACE，也没有 ABI 兼容性需要维持。

## 10. 参考

### 10.1 代码

- `src/base/geom2d/sdk/vine/geom2d/Curve.hpp`、`ConicCurve.hpp`、`LineCurve.hpp`、`CircleCurve.hpp`、`EllipseCurve.hpp`、`HyperbolaCurve.hpp`、`ParabolaCurve.hpp`、`ArcCurve.hpp`、`BezierCurve.hpp`、`BSplineCurve.hpp`、`OffsetCurve.hpp`、`CurveProjection.hpp`、`CurveCurveIntersection.hpp`、`geom2d_global.hpp`
- 平面盒类型：`src/base/math/sdk/vine/math/Rect2.hpp`（本模块不自建盒子）
- 样条核：`src/base/math/sdk/vine/math/spline/BezierBlend.hpp`、`src/base/math/sdk/vine/math/spline/BSplineBasis.hpp`（与维度无关；`knotSpan` 的搜索不变量见 `../bugs/spline-knotspan-hang.md`）
- `tests/test_geom2d/CurveBasicsTest.cpp`、`EllipseCurveTest.cpp`、`ConicCurvesTest.cpp`、`ArcCurveTest.cpp`、`BezierCurveTest.cpp`、`BSplineCurveTest.cpp`、`OffsetCurveTest.cpp`、`CurveContractTest.cpp`、`CurveProjectionTest.cpp`、`BoundsTest.cpp`、`CurveIntersectionTest.cpp`
- 3D 对应物：`src/base/geom3d/sdk/vine/geom3d/Curve.hpp`、`ConicCurve.hpp`、`EllipseCurve.hpp`、`HyperbolaCurve.hpp`、`ParabolaCurve.hpp`、`BezierCurve.hpp`、`BSplineCurve.hpp`、`OffsetCurve.hpp`、`TrimmedCurve.hpp`、`CurveProjection.hpp`、`CurveSurfaceIntersection.hpp`

### 10.2 文档与 ADR

- `geometry-kinds.md` 2.7 —— 曲线接口十一项与不进接口的事实（本模块逐条沿用）。
- `geometry-curve-queries.md` —— 3D 侧点到曲线的最近点：同一套驻点求根、窗口规则与代价分析，本模块 2.9 逐条沿用。
- `geometry-primitives.md` 2.3、2.8 —— 值类型的命名与精度实例化规则。
- `../decisions/ADR-2026-11-05-planar-curve-contract.md` —— 为什么是第二份契约。
- `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md` —— `isValid()` 上接口。

### 10.3 外部资料

- OpenCASCADE `TKG2d/Geom2d`（`Geom2d_Curve`、`Geom2d_Conic`、五个解析曲线、`Bezier` / `BSpline` / `Trimmed` / `Offset`）只作**能力清单与参数约定**的对照：圆锥曲线的 `gp_Ax2d`（原点 + 起始轴）与本模块的 `ConicCurve` 同构，偏心角 / 双曲角 / 焦距的含义与 OCCT 一致。API 形态不照抄：OCCT 是可变的引用计数对象，带变换族与原地 `Reverse()`；本模块是值类型 + `reversedParameter`（D5、`geometry-kinds.md` 2.7）。
