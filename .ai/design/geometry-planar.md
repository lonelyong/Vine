# geometry 平面几何：平面曲线契约与第一批值类型

> **状态**：现行
> **最后核对**：2026-11-05
> **范围**：`src/base/geom2d`——平面曲线求值契约 `Curve<T>`、圆锥曲线层 `ConicCurve<T>` 与 `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve` 六个值类型，以及模块的物理形态与命名规则。不含空间曲线与曲面（见 `geometry-kinds.md`），不含点到曲线的最近点、求交、包围盒与多边形（无消费者，见 `../backlog/geom2d.md`）。
> **权威来源**：`src/base/geom2d/sdk/vine/geom2d/Curve.hpp`、`ConicCurve.hpp`、`LineCurve.hpp`、`CircleCurve.hpp`、`EllipseCurve.hpp`、`HyperbolaCurve.hpp`、`ParabolaCurve.hpp`、`ArcCurve.hpp`；`tests/test_geom2d/` 下的全部用例文件。
> **待办**：`../backlog/geom2d.md`

## 1. 概要设计

### 1.1 问题与背景

平面几何此前没有自己的类型：要么把点压进 `Point3` 让 `z = 0`，要么在模块外重写一份。前者给平面曲线加了一个它没有的法向和一条恒为零的维度，3D 侧的算法因此会“接受”一条平面曲线并按空间曲线解释它；后者让“曲线”这个概念出现第二套没有契约的定义。`geom2d` 在那之前只有一个命名空间，以及 `Continuity` 的一次重导出。

### 1.2 目标

- 一份**独立**的平面曲线契约：与 `geom3d::Curve<T>` 逐项同义（求值、一二阶导数、参数域、闭合与周期、光滑度、有效性、反向映射），只是点是 `Point2`、向量是 `Vector2`。
- 六个能被执行验证的值类型（直线、圆、椭圆、双曲线、抛物线、圆弧），让契约从“声明”变成“可测的行为”。
- 一次说清模块的物理形态与命名规则：header-only / INTERFACE 目标、名字不带维度标记、精度别名仍是 `d` / `f`。

### 1.3 非目标

- 不把 3D 曲线族整族搬过来：样条与偏置曲线都要等消费者（`G8`，见 `../backlog/geom2d.md`）。
- 不做**算法**：最近点、曲线 × 曲线求交、弧长、包围盒都不进这个模块（同 backlog）。
- 不借用 3D 的 `ConicCurve` 框架：平面里的圆锥曲线没有法向可存（2.4）。
- 不做屏幕 / 像素几何：`Point2`、`Vector2`、`Rect2` 属于 `math`，这个模块只有曲线。

### 1.4 一句话结论

**平面曲线是第二份契约、不是 3D 契约的特化：同样的十一项事实、`Point2` / `Vector2`、同一个 `math::Continuity`；今天有直线、圆、椭圆、双曲线、抛物线与圆弧，其余等真实消费者。**

## 2. 设计要点

### 2.1 独立契约，不做 3D 的泛型特化

- **是什么**：`geom2d::Curve<T>` 自己声明十一项纯虚函数 + 虚析构，和 `geom3d::Curve<T>` 同名同义；两者之间没有继承、没有共享基类、没有以点类型为参数的公共模板。
- **为什么**：`G1`、`G5` —— “平面曲线”和“空间曲线”是两个概念，各自有一个定义、一个归属。做一份 `Curve<PointT>` 的泛型契约，等于把 3D 的每个消费点变成模板，并让 2D 类型被迫满足只有 3D 才需要的约束；让 2D 契约继承 3D 契约，则平面曲线必须带一个恒定法向和一条恒为零的维度，3D 算法会误收它。决策与被拒选项见 `../decisions/ADR-2026-11-05-planar-curve-contract.md`。
- **代价**：契约文字有两份，一侧新增事实时另一侧可能悄悄漂移（`G6`）。缓解只做两件事，不引入机制：共享词汇统一住在 `math`（`Continuity`、`FP`、将来的求根器），`geom2d_global.hpp` 把 `Continuity` 重导出成同一个类型，让两边写的是同一个名字；契约里只放**与维度无关**的事实（2.5），需要新事实时两份文档同时改。

### 2.2 名字不带维度标记

- **是什么**：`LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve` / `Curve`，不是 `LineCurve2d` / `Curve2D`；精度别名仍是 `LineCurved` / `CircleCurvef` / `EllipseCurved` / `HyperbolaCurvef` / `ParabolaCurved` / `ArcCurvef`，与 `geom3d` 的 `d` / `f` 规则一字不差。
- **为什么**：`G4`、`G6` —— 命名空间已经是限定词，“2d”写在类型名里就成了一句重复的说明；精度后缀表达的是“哪种标量”，与维度无关，所以它必须保留原样，否则同一个概念在两层里读法不同。
- **边界**：同名的 `geom2d::LineCurve` 与 `geom3d::LineCurve` 是两个类型。同时使用两侧的代码 `using` 其中一个、限定另一个；不得再造 `Line2d` 这类别名（D3）。

### 2.3 契约只放“曲线必然能回答”的事实

- **是什么**：十一项全纯虚——① 求值与导数：`evaluate` / `derivative` / `secondDerivative`；② 参数域与周期：`firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period`；③ 光滑度：`continuity`（及由它导出的非虚 `isCN(n)`）；④ 反向参数映射：`reversedParameter`；⑤ 有效性：`isValid`。
- **为什么**：`G4`、`G9` —— 这是“参数化曲线”的最小完备集，和 3D 侧同一套判据（`geometry-kinds.md` 2.7）：一个点、它的变化率、它在哪里有效、它有多光滑、反着走参数怎么变、这份状态能不能用。平面侧不是“少几项”，而是同一组事实少一个维度。
- **不进接口的东西与 3D 侧逐条相同**：`bounds()`（无界曲线只能返回无穷盒）、`length()` / 弧长参数化（是算法）、变换族（坐标系住在 `Shape` 节点上）、`curveType()` 枚举（vtable 已经回答了“我是什么”）、高于二阶的导数。理由只在 `geometry-kinds.md` 2.7 与 D6 写一次，这里不复制。

### 2.4 非圆圆锥曲线共享一层起始轴，圆不进这层

- **是什么**：`CircleCurve` 存 `center` 与 `radius`，`P(u) = center + radius * (cos u, sin u)`；参数是弧度、域 `[0, 2π]`、逆时针、反向映射是 `2π - u`，它没有框架。`ConicCurve<T>` 持有 `center`（抛物线的顶点就是它）+ `x_direction`（构造时归一化）+ 派生的 `yAxis()`（+90° 旋转）+ `continuity() == CN`；`EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 派生自它，只声明自己的标量。
- **为什么**：`G1`、`G13`、`G8` —— 3D 的圆锥框架需要法向，是因为曲线在空间里必须回答“在哪个平面、从哪条轴量角”；平面里没有平面可选，只剩一根起始轴。圆连起始轴也没有——逆时针与 +X 是唯一约定——为它存一根永远 `(1,0)` 的轴就是无法被检验的字段。三条非圆圆锥曲线确实共享同一组事实，所以这一层现在有真实消费者，而不是为一个字段预建的抽象。
- **边界**：层里不放求值与参数域——参数的含义各类型不同（偏心角、双曲角、垂直轴上的偏移）；退化方向存零向量，所有点查询因此答 `center`，`isValid()` 是唯一判据。
- **直线同理**：`LineCurve` 存 `origin` 与 `direction`，方向不需要单位长度，参数是方向的倍数——与 3D 的 `LineCurve` 同一契约（`geometry-primitives.md` 2.3）。

### 2.5 header-only，`vn::Geom2D` 是 INTERFACE 目标

- **是什么**：八个头文件都是 `template <vn::math::FP T>` 的完整定义，模块没有 `src/` 目录；`vn_add_library` 因此生成 INTERFACE 目标，只声明 `target_link_libraries(... INTERFACE vn::Math vn::Core)`。
- **为什么**：`G10`、`G6` —— 精度模板决定了实现必须在调用方编译；有 `src/` 的模块要维护“每个类型 × 两种精度”的显式实例化表（`geometry-primitives.md` 2.8），而这里一个符号都不导出，那张表也就不存在。目标名、include 目录与安装规则仍走同一个 helper，所以它和 `vn::Geom3D` 的用法没有区别。
- **代价**：没有 ABI 边界，曲线代码在每个消费方各编译一次；也没有 `VN_GEOM2D_API` 可加。等到出现第一个非模板实现（需要缓冲的曲线、或某个查询的 `.cpp`），这条要重新评估，届时 `src/` 一出现目标就自动变成 SHARED 库。

### 2.6 退化状态给答案，不失败

- **是什么**：`isValid()` 是唯一判据，且只看**状态**、不看参数。零方向直线答 `evaluate(u) == origin`，非正半径圆答 `evaluate(u) == center`，零主轴的圆锥曲线（椭圆 / 双曲线 / 抛物线）答 `center`；三条都写在类型文档里，都不产生 NaN / 无穷；反向区间的圆弧答无效，查询仍转发给圆的退化答案。
- **为什么**：`G4`、`G11` —— 与 3D 侧的规则一字不差（`geometry-kinds.md` C12）：错误变成**可检查的错误答案**，而不是未定义行为或异常；调用方在入口检查一次 `isValid()`，内部循环就不必逐个参数判空。

### 2.7 值类型保留比接口更便宜的路径

- **是什么**：`LineCurve::pointAt(t)` 是 `constexpr`，`CircleCurve::pointAt(angle)` / `EllipseCurve::pointAt(angle)` / `HyperbolaCurve::pointAt(u)` / `ParabolaCurve::pointAt(u)` 只是类型上的普通成员（三角函数不是常量表达式）；`evaluate()` 都转发给它们。具体类型的调用因此可以内联，而 `Curve<T>&` 上的调用必然要过一次虚表。
- **为什么**：`G7`、`G10` —— 多态只在“不知道具体类型”时才有价值，热路径必须能绕开它；这条路径是**加法**（多一个成员），不是把复杂度推给调用方。
- **边界**：不为对称而对称。`ArcCurve` 没有 `pointAt()`——它只会转发给基圆，不省任何东西；其余五个类型的 `pointAt()` 与各自 `evaluate()` 同值，圆的那个与 3D 同名类型一致。

### 2.8 圆弧是值类型上的裁剪视图

- **是什么**：`ArcCurve` 存一个 `CircleCurve`（按值）加 `first_angle` / `last_angle`；求值与导数转发给圆，闭合与周期由“基曲线 + 区间”共同决定，`period()` 只在自身 `isPeriodic()` 为真时回答 `2π`，反向映射在区间内反射。
- **为什么**：`G6`、`G10` —— 与 3D 的 `TrimmedCurve` 同一条规则（闭合、周期、反向、越界行为逐条照抄），但基曲线按值存：一个圆只有两个字段，复制它比一次堆分配便宜，也让 R2（值类型不持有动态内存）继续成立。
- **代价**：没有通用的平面裁剪。“任意曲线 + 区间”的 `TrimmedCurve` 等第二个非圆基曲线（`../backlog/geom2d.md` P4）；今天圆弧与非圆曲线的组合不存在。

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
| F7 | 平面查询 | 点到曲线的最近点、曲线 × 曲线求交、包围盒 | 未做（`../backlog/geom2d.md` P1、P2） |
| F8 | 样条、偏置与通用裁剪 | `BezierCurve` / `BSplineCurve` / `OffsetCurve`、任意基曲线的 `TrimmedCurve` | 未做（`../backlog/geom2d.md` P3、P4） |

### 3.1.1 F1 平面曲线契约

- **行为**：`const Curve<T>&` 上可求任意参数的点与一二阶导数、问参数域 / 闭合 / 周期 / 光滑度 / 反向映射 / 有效性；`isCN(order)` 由 `continuity()` 推出。
- **约束**（C1、C2、C3）：
- **要求**（R1、R2）：
- **验收**（T1–T6）：
- **注意**（N2、N3、P4）：

### 3.1.2 F2 平面直线

- **行为**：`P(t) = origin + direction * t`；域是整个实轴（`±infinity`），永不闭合、永无周期，`continuity()` 为 `CN`，二阶及以上导数为零向量。
- **约束**（C1、C3）：零方向（`|direction| <= EPS`）不描述直线，`isValid()` 为假，`evaluate` 答 `origin`。
- **要求**（R1、R2）：字段只有 `Point2` / `Vector2`，无分配。
- **验收**（T1、T2、T3、T4、T6）：
- **注意**（N1、N3、P1、P2）：

### 3.1.3 F3 平面圆

- **行为**：`P(u) = center + radius * (cos u, sin u)`；域 `[0, 2π]`，闭合、周期 `2π`，`derivative` 长度等于半径，`secondDerivative` 指向圆心；`circumference()` 是接口外的便利查询。
- **约束**（C2、C3、C4）：`radius <= EPS` 不描述圆，`isValid()` 为假，`evaluate` 答 `center`，`period()` 仍答 `2π`（它是参数化的事实，不是几何的事实）。
- **要求**（R1、R2）：一次 `sin` / `cos`，无分配、无异常。
- **验收**（T1、T2、T3、T4、T5、T6）：
- **注意**（N2、N3、N4、P3、P5）：

### 3.1.4 F4 平面椭圆

- **行为**：`EllipseCurve` 存 `center` / `x_direction` / `major_radius` / `minor_radius`，`P(u) = center + a·cos(u)·X + b·sin(u)·Y`，`Y` 由 `X` 旋转 +90° 导出；参数是偏心角，域 `[0, 2π]`、闭合、周期 `2π`、`CN`。
- **约束**（C1–C3）：主轴方向与半轴必须可用——方向按长度归一化，零方向退化为零向量，`pointAt()` 因此答 `center`；`major_radius` / `minor_radius` 都得大于 `EPS`。
- **要求**（R1、R2）：只存 `Point2` / `Vector2` / 标量，无分配；二阶导等于 `center - P(u)`，与圆的形状一致。
- **验收**（T1、T2、T3、T4、T5、T6）：
- **注意**（N2、N3、P2、P3、P6）：

### 3.1.5 F5 平面圆弧

- **行为**：`ArcCurve` 在 `[first_angle, last_angle]` 上转发一个 `CircleCurve`；`isClosed()` 要求区间覆盖圆的整个域，`isPeriodic()` 要求区间至少一个周期，两者都成立时 `period()` 为 `2π`，否则为 0；区间外仍按圆求值。
- **约束**（C2、C3）：`isValid()` 同时要求基圆可用且 `first_angle <= last_angle`；反向区间不改写、不重排，只报告无效。
- **要求**（R1、R2）：基圆按值持有，不分配；裁剪不重参数化，导数就是圆在同一个角度上的速度。
- **验收**（T3、T4、T5、T6）：
- **注意**（N2、N4、P1、P6）：

### 3.1.6 F6 平面双曲线与抛物线

- **行为**：`HyperbolaCurve` 走 `major_radius > 0` 的一支，`P(u) = center + a·cosh(u)·X + b·sinh(u)·Y`；另一支是 `-x_direction`，不是负参数。`ParabolaCurve` 走 `P(u) = vertex + (u² / (4f))·X + u·Y`，`X` 指向焦点。两者都无界、非周期、`CN`，`reversedParameter` 都是 `-u`。
- **约束**（C1–C3）：双曲线要求两个半轴可用，抛物线要求焦距可用；两者都要求起始轴可用，否则点查询答 `center`。
- **要求**（R1、R2）：双曲线的加速度是 `P(u) - center`（双曲角不经符号变化）；抛物线的加速度常量 `X / (2f)`。
- **验收**（T1–T6）：
- **注意**（N2、N3、P2、P3）：

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

完整签名与每个参数的语义见头文件；本表只说明归属。

## 5. 约束、要求与不变量

- **C1**：参数用类型自己的量纲（直线是方向的倍数，圆、椭圆与圆弧是弧度，双曲线是双曲角，抛物线是垂直轴上的偏移）。参数域是曲线的属性，不是 `evaluate` 的前置条件；域外参数的意义由类型文档写明。
- **C2**：`period()` 只在 `isPeriodic()` 为真时有意义，否则为 0；有界的片段不转发整圈的周期（与 3D 的 `TrimmedCurve` 同一条规则）。
- **C3**：`isValid()` 只看状态，与参数无关；无效状态下的每条查询返回类型文档写明的退化值，不得产生 NaN 或无穷。
- **C4**：`evaluate` / `derivative` / `secondDerivative` 是常数时间的闭式求值，不分配、不抛异常。
- **R1**：平面曲线只依赖 `Point2`、`Vector2`、标量与 `math` 的现有设施；不得引入 3D 概念（法向、框架、z 分量）。
- **R2**：值类型不持有动态内存。需要缓冲的曲线（样条类）引入时必须在这条上写明例外并重新评估。
- **N1**：提供 `pointAt()` 的类型（直线、圆、椭圆、双曲线、抛物线）上它与 `evaluate(u)` 同值。
- **N2**：`reversedParameter` 是 involution，并把参数域两端互换（`reversedParameter(firstParameter()) == lastParameter()`）。它给出的是**反向曲线**上同一个点的参数——反向曲线是原曲线关于域中点轴（周期曲线是起始轴）的镜像，`evaluate(reversedParameter(u))` 不等于 `evaluate(u)`。
- **N3**：`derivative` 是速度向量而非单位切向（圆的长度 = 半径，双曲线在顶点最小、向两端增长）；`secondDerivative` 只在弧长参数化时等于曲率向量（抛物线则是常量）。
- **N4**：圆与椭圆的 `period() == lastParameter() - firstParameter() == 2π`，域外参数按周期性回绕；圆弧只在区间跨满圆的域时回答 `2π`，更短的一段回答 0（C2）。

## 6. 注意事项与陷阱

- **P1 参数量纲混用**。会怎么错：把弧长当角度传给圆，得到“看起来在圆上但不是想要的位置”的点，没有报错。怎么判：用已知点反算参数，或换一个半径后结果不按比例变化。怎么防：读类型的 `@param`（圆一律写“弧度”）；跨类型调用时在参数名上带量纲，如 `arc_length` / `angle`。
- **P2 把 `derivative` 当单位切向**。会怎么错：直线的方向不是单位长度、圆的切向长度是半径，归一化前拿它当方向，比例全错。怎么判：同一曲线在两点上的切向长度不同。怎么防：用前先 `normalized()`（N3）。
- **P3 退化状态给出“合理”的答案**。会怎么错：半径 0 的圆对每个参数都答圆心，零主轴的椭圆 / 双曲线 / 抛物线答中心，零方向直线对每个参数都答原点，后续算法把它当成真的曲线。怎么判：结果与参数无关。怎么防：入口判 `isValid()`（C3）。
- **P4 `float` 上的 `2π` 比较**。会怎么错：`CircleCurvef::period() == math::PI_TWO` 为假，`isPeriodic()` 分支被误判。怎么判：同一用例用 double 通过、float 不通过。怎么防：参数比较一律带容差（`EPS<T>()` 或按参数尺度定的容差），只对布尔事实用精确比较。
- **P5 经 `Curve<T>&` 的虚调用**。会怎么错：批量采样挂在基类引用上，每次都过虚表，且编译器无法内联。怎么判：反汇编或计时代码在具体类型上明显更快。怎么防：热循环持有具体类型（2.7）。
- **P6 把反向映射当成“同一个点”**。会怎么错：以为 `evaluate(reversedParameter(u)) == evaluate(u)`，实际拿到的是关于域中点轴（圆与椭圆是起始轴）的镜像点，两条曲线被当成一条用。怎么判：反向后的点与原点在 `u = 域中点` 两侧对称，端点互换而中间点不重合。怎么防：只把 `reversedParameter` 用于“反向曲线上的同一点”（N2）；需要同一点时直接 `evaluate(u)`。

## 7. 已否决选项

- **D1** 一份 `Curve<PointT>` 泛型契约，或让 `geom2d::Curve` 继承 3D 契约：`G1`、`G5`——两个维度本是两个概念，合并后 2D 类型被迫携带法向与 z 分量，3D 算法也会误收平面曲线。见 `../decisions/ADR-2026-11-05-planar-curve-contract.md`。
- **D2** 平面圆借用 3D `ConicCurve` 的框架（`normal` + `x_direction`）：`G1`、`G8`——平面里没有可存的方向，两个字段既无法取值也无法校验，只让每个圆多 24 字节。
- **D3** 名字加维度标记（`Curve2d` / `Circle2d`）或再补 `*2d` 别名：`G4`、`G6`——命名空间已是限定词，别名会让同一个概念有两个名字。
- **D4** 现在就把 3D 曲线族整套复制过来：`G8`——没有消费者的类型只会在两侧同时漂移；类型按 `../backlog/geom2d.md` 的触发条件逐个引入。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 参数化：直线沿线、圆按角 | `tests/test_geom2d/CurveBasicsTest.cpp` 的 `WalksAlongItsDirection`、`WalksItsAngle` |
| T2 | 导数语义：直线常量速度、圆的速度与向心加速度 | 同文件 `HasAConstantVelocity`、`TurnsTowardsItsCentre` |
| T3 | 域 / 闭合 / 周期 / 光滑度 | 同文件 `IsUnboundedAndNeverClosed`、`IsClosedPeriodicAndSmooth` |
| T4 | 退化状态答退化值而非 NaN | `CurveBasicsTest.cpp` 的 `AZeroDirectionIsNotALine`、`ANonPositiveRadiusIsNotACircle`；`EllipseCurveTest.cpp`、`ConicCurvesTest.cpp` 的退化用例 |
| T5 | 反向映射是 involution、交换域两端，且经 `Curve<T>&` 与 `unique_ptr<const Curve<T>>` 可用 | `CurveBasicsTest.cpp` 的 `ReversesByNegatingItsParameter`、`ReversesByReflectingInsideThePeriod`、`AnswersThroughTheBaseInterface`；`EllipseCurveTest.cpp`、`ArcCurveTest.cpp` 各自的反向与多态用例 |
| T6 | 契约扫查：域有序、反向 involution、闭合曲线两端同点、周期曲线按 `period()` 重复、非周期曲线 `period() == 0`，六个类型 × float / double | `tests/test_geom2d/CurveContractTest.cpp` 的 `EveryPlanarCurveKeepsItsPromisesInDouble` / `…InFloat` |

## 9. 状态与演进

### 9.1 当前状态

八个头文件（`Curve` / `ConicCurve` / `LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `ArcCurve`）+ `test_geom2d`（39 个用例，含六个类型 × float / double 的契约扫查）；`vn::Geom2D` 是 INTERFACE 目标，不导出任何符号；模块内没有生产代码消费者——今天唯一的使用者是测试。

### 9.2 已知缺口

- 族里没有样条与偏置曲线（`../backlog/geom2d.md` P3）。
- 没有任何平面算法：最近点、求交、包围盒（P1、P2）。
- 没有通用平面裁剪：`ArcCurve` 只认圆，任意基曲线的 `TrimmedCurve` 等第二个基曲线（P4）。

- 纯增量的平面类型同样没有 `bounds()`：`Rect2` 形态的平面包围盒还没有消费者（P2）。

### 9.3 兼容性 / 迁移

纯增量：`geom3d` 的接口与类型未改动，两个模块互不引用。新增平面类型只影响本模块与 `tests/test_geom2d`；因为目标是 INTERFACE，也没有 ABI 兼容性需要维持。

## 10. 参考

### 10.1 代码

- `src/base/geom2d/sdk/vine/geom2d/Curve.hpp`、`ConicCurve.hpp`、`LineCurve.hpp`、`CircleCurve.hpp`、`EllipseCurve.hpp`、`HyperbolaCurve.hpp`、`ParabolaCurve.hpp`、`ArcCurve.hpp`、`geom2d_global.hpp`
- `tests/test_geom2d/CurveBasicsTest.cpp`、`EllipseCurveTest.cpp`、`ConicCurvesTest.cpp`、`ArcCurveTest.cpp`、`CurveContractTest.cpp`
- 3D 对应物：`src/base/geom3d/sdk/vine/geom3d/Curve.hpp`、`ConicCurve.hpp`、`EllipseCurve.hpp`、`HyperbolaCurve.hpp`、`ParabolaCurve.hpp`、`TrimmedCurve.hpp`

### 10.2 文档与 ADR

- `geometry-kinds.md` 2.7 —— 曲线接口十一项与不进接口的事实（本模块逐条沿用）。
- `geometry-primitives.md` 2.3、2.8 —— 值类型的命名与精度实例化规则。
- `../decisions/ADR-2026-11-05-planar-curve-contract.md` —— 为什么是第二份契约。
- `../decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md` —— `isValid()` 上接口。

### 10.3 外部资料

无。平面曲线的契约全部来自本仓库 3D 侧的既有定义，不引入新的几何约定。
