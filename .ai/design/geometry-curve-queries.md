# geometry 曲线查询与测度：最近点、弧长与曲率

> **状态**：现行
> **最后核对**：2026-11-05
> **范围**：`src/base/geom/sdk/vine/geom/spatial` 里对**任意** `Curve<T>` 的最近点 / 距离、两段曲线的最近距离、弧长及其反解、曲率，以及窗口规则与代价。
> 不含曲线 × 曲面求交（见 `geometry-intersection.md`）与点到曲面（见 `geometry-surface-queries.md`）。
> **权威来源**：`src/base/geom/sdk/vine/geom/spatial/CurveProjection.hpp`、`CurveLength.hpp`、`CurveCurvature.hpp`；`tests/test_geom_spatial/CurveDistanceTest.cpp`、`CurveLengthTest.cpp`、`CurveCurvatureTest.cpp`。
> **待办**：`../backlog/geometry.md` G3（实体求交）

## 1. 概要设计

### 1.1 问题与背景

`Curve<T>` 是求值接口（`geometry-kinds.md` 2.7）：拿 `const Curve<T>&` 的调用方能取点、取一阶 / 二阶导、问参数域，但**问不出“离这个点最近的是哪里”**。逐类型补入口要覆盖直线、四种圆锥曲线、裁剪视图与两种样条共八种类型，新曲线再写一遍（`G9` 的反例）；而曲面侧已经有 `closestPointOnSurface()`，曲线侧空着，两个族不对称。

### 1.2 目标

- 一个自由函数入口，参数是接口，不要求具体类型、不要求解析形式。
- 参数域无界时由调用方给窗口，函数不自选范围——与曲线 × 曲面、曲面投影同一条规则。
- 一个结果同时回答“最近点在哪”“参数是多少”“距离多少”。
- 同一套接口再回答两个标量问题：窗口上的弧长（及其反解）与一点的曲率。
- 两段曲线的最近点对与距离：同一套接口、同一套窗口规则。

### 1.3 非目标

- 不做等距重参数化（把整条曲线按弧长重新参数化）：今天只回答“给定长度求参数”，不产生新的曲线类型。
- 不扩 `Curve<T>` 接口：不加 `closestPoint()`、不加 `bounds()`。

### 1.4 一句话结论

**最近点是平方距离的驻点，也就是 `f(u) = (C(u) - P) · C'(u)` 的零点；采样找出变号区间、用 `rootInBracket()` 收窄，再与窗口端点比较，取距离最小的那个。弧长是速度的积分，反解是同一个积分的逆；曲率是速度与加速度叉积的长度除以速度三次方。**

## 2. 设计要点

### 2.1 自由函数 + “派生自 `Curve<T>`”的约束

- **是什么**：`closestPointOnCurve(curve, query, ...)` 是头文件模板，`C` 由 `requires std::is_base_of_v<Curve<T>, C>` 约束。
- **为什么**：`G1`、`G9` —— 算法只依赖 `evaluate` / `derivative` / 域 / `isValid`；写成自由函数而不是接口虚函数，“两个对象的关系”才装得下，热路径也不必为它付 vptr。
- **为什么不是 `const Curve<T>&` 参数**：`CircleCurve` 经 `ConicCurve<T>` 间接派生，模板推导看不穿间接基类；命名概念里出现受约束模板会被 MSVC 拒绝（C7605）。约束放在函数的 `requires` 子句里，两件事都成立（与 `geometry-surface-queries.md` 2.1 同一理由）。

### 2.2 窗口由调用方给，规则与求交一致

- **是什么**：`std::optional<std::pair<T, T>> window`——曲线参数区间。省略时有界曲线用自己的域；无界曲线（直线、双曲线、抛物线）**必须给**，且必须有限、有序。
- **为什么**：`G4`、`G6` —— 与 `intersectCurveWithSurface()` 的曲线窗口完全同一条规则（`geometry-intersection.md` C5），调用方不必为两个入口记两套约定。
- **代价**：无界曲线多一个参数；有界曲线完全不受影响。

### 2.3 驻点求根，而不是直接最小化

- **是什么**：`f(u) = (C(u) - P) · C'(u)` 是平方距离的导数；它的零点就是最近点、最远点与其它驻点。扫描窗口、对每个变号区间用 `geom::detail::rootInBracket()` 收窄，再与每个采样点、两个窗口端点比较距离，取最小者。
- **为什么**：`G1`、`G10` —— 求根器已经在 `math`（曲线 × 曲面在用），驻点方程只需一阶导；窗口端点必须参与比较，因为“足点落在窗口外”时 `f` 在窗口内没有零点，最小值在边界上。
- **为什么不用黄金分割 / 三分搜索**：那需要先给一个包含极小值的括号，仍然要靠采样；`rootInBracket()` 既已存在又是括号安全算法，解析曲线上还能收敛到精确的足点（线段、圆都是精确解），而不是单纯形搜索的近似值。见 §7 D2。
- **边界**：只调 `evaluate()` 与 `derivative()`；不要求解析形式，也不要求二阶导（驻点方程已经是一阶）。

### 2.4 结果与曲面侧同形

- **是什么**：`CurveProjection<T>{ point, parameter, distance }`，字段语义与 `SurfaceProjection<T>{ point, u, v, distance }` 一一对应，只把两个参数换成一个。
- **为什么**：`G6`、`G4` —— 同一类问题的答案在不同族里长成同一个形状，调用方不必记两套字段名；距离与最近点仍然是同一个极值的两半，不拆成两个入口（`G1`）。
- **边界**：参数是曲线自己的量纲（直线是距离、圆是角、样条是节点值），不做归一化。

### 2.5 代价与局部性

- **是什么**：`sample_count + 1` 次采样（每次一次 `evaluate` + 一次 `derivative`），每个变号区间至多 64 步收窄。
- **为什么**：`G10`、`G11` —— 与曲面投影同一个骨架：网格播种 + 括号安全收窄；不分配、不抛异常。
- **代价**：答案是**局部**最优；平坦极小值（`f` 不变号）只能靠采样恰好落在上面；分辨率不足时提升 `sample_count`。全部写在 §6。

### 2.6 弧长与反解：与平面侧同形的两个自由函数

- **是什么**：`arcLength(curve, window, tolerance, max_depth)` 对 `|C'(u)|` 做自适应 Simpson 积分；`parameterAtLength(curve, arc_length, window, tolerance, max_depth)` 用带二分保护的 Newton 反解同一个积分。
- **为什么**：`G1`、`G9`、`G6` —— 长度与“给定长度求参数”是曲线的一维测度，不属于任何具体类型；数值内核（`geom::detail::adaptiveSimpson()` 与 `geom::detail::parameterReaching()`）是模块内共享的一层，**2D 与 3D 用的是同一份实现**，两个命名空间只提供“速度”与“已走长度”这两个取值，所以两边语义不可能漂移（`../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`）。
- **边界**：`tolerance` 是**长度**误差，不是参数误差；`arc_length` 必须非负且不超过窗口总长（超出容差答空）；窗口规则与 `closestPointOnCurve()` 完全相同（C1）。
- **代价**：反解每轮重新积分一次，代价是迭代数乘一次积分。

### 2.7 曲率：空间曲线没有符号

- **是什么**：`curvature(curve, u) = |C'(u) x C''(u)| / |C'(u)|^3`，非负；曲率半径是它的倒数。
- **为什么**：`G4` —— 空间曲线没有自然的“左 / 右”，符号没有定义；平面侧有平面定向，所以那边叫 `signedCurvature()`。两边在名字上就把这件事说清楚，而不是靠调用方记约定。
- **边界**：速度为零处无定义，答 `std::nullopt`；只依赖前两阶导。把平面曲线嵌入 3D 后，3D 曲率等于平面带符号曲率的绝对值。
- **代价**：没有额外类型；要方向就用平面侧的带符号版本。

### 2.8 两段曲线的最近距离：采样 + 一边投影 + 二维 Gauss-Newton

- **是什么**：`curveToCurveDistance(first, second, first_window, second_window, tolerance, sample_count)` 返回 `CurveCurveDistance<T>{ first_point, second_point, first_parameter, second_parameter, distance }`。第一条曲线被采样，每个样本用 `closestPointOnCurve()` 投影到第二条曲线（对样本精确），再用 `(J^T J) [du, dv] = -J^T r`（`J = [C1', -C2']`）细化，取最小的距离。
- **为什么**：`G12`、`G4`、`G9` —— “两段曲线的关系”不属于其中任何一段，所以是自由函数；结果同时给两点、两参数与距离，避免调用方二次求值并让 `distance` 与点不可能互相矛盾。复用最近点与窗口规则，八个类型不需要各写一遍。
- **边界**：局部方法——多处靠近时答案取决于采样是否落进每个低谷；局部平行（`J^T J` 奇异）时保留采样对，采样投影仍然精确；窗口规则同 C1。相切或穿越时距离可以是 0（扫描或细化落到接触处）。
- **代价**：每条样本做一次最近点查询，代价是 `sample_count ×` 最近点；没有引入新接口项。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 点到曲线的最近点 / 距离 | `closestPointOnCurve(curve, query, window, tolerance, sample_count)`：驻点求根 + 端点比较，返回点、参数、距离 | 已落地（T1–T4） |
| F2 | 窗口当硬约束 | 足点落在窗口外时答案为更近的那个端点，不外推 | 已落地（T2） |
| F3 | 曲线 × 曲线最近距离 | `curveToCurveDistance()`：采样 + 一边投影 + 二维 Gauss-Newton，返回两点 / 两参数 / 距离 | 已落地（T7） |
| F4 | 弧长与反解 | `arcLength()` / `parameterAtLength()`：对 |C'| 做自适应 Simpson（`geom::detail::adaptiveSimpson`）+ 带二分保护的 Newton（`geom::detail::parameterReaching`） | 已落地（T5） |
| F5 | 曲率 | `curvature()`：`|C' x C''| / |C'|^3`，非负，速度为零答空 | 已落地（T6） |

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `CurveProjection.hpp` | `CurveProjection<T>` 与 `closestPointOnCurve()` |
| `CurveLength.hpp` | `arcLength()` 与 `parameterAtLength()` |
| `CurveCurvature.hpp` | `curvature()` |
| `CurveCurveDistance.hpp` | `CurveCurveDistance<T>` 与 `curveToCurveDistance()` |

完整签名与每个参数的语义见头文件。

## 5. 约束、要求与不变量

- **C1**：搜索区间必须有限。无界曲线必须由调用方给窗口；没给、窗口反向或零宽都返回 `std::nullopt`，不自选范围。
- **C2**：容差是**几何**量；换算成参数容差时用 `tolerance / |dP/du|`，在曲线停住处回退到采样步长的一个分数。
- **R1**：不分配、不抛异常；输入不可用时返回空答案。
- **R2**：只使用 `Curve<T>` 的 `evaluate` / `derivative` / 两个域端点 / `isValid`。
- **N1**：`CurveProjection::point == curve.evaluate(parameter)`；`distance == |point - query|`。
- **N2**：同距离的多个候选按参数从小到大先到先得（例如圆心到圆上每一点等距，报第一个采样点）。
- **N3**：`arcLength()` 是**路径长度**（含折返）；`parameterAtLength()` 从窗口起点量起，且 `arcLength(curve, {a, u})` 在容差内等于请求的长度。
- **N4**：`curvature()` 非负；半径相同的圆在两点上答同一个值，空间曲线没有“左 / 右”之分。
- **N5**：`CurveCurveDistance` 的三个等式同时成立：`first_point == first.evaluate(first_parameter)`、`second_point == second.evaluate(second_parameter)`、`distance == |first_point - second_point|`。

## 6. 注意事项与陷阱

- **P1 窗口太窄**。会怎么错：足点在窗口外，答案落在端点上而被当成“最近点”。怎么判：参数等于窗口端点且 `f` 在端点外仍同号。怎么防：先用包围或几何直觉估窗口（C1）。
- **P2 局部最小**。会怎么错：曲线来回靠近查询点，采样只找到其中一个极小值，距离偏大。怎么判：`sample_count` 加倍后距离变小。怎么防：按曲线起伏尺度提高 `sample_count`。
- **P3 平坦极小值没有括号**。会怎么错：`f` 在极小值处相切于零而不变号，`rootInBracket()` 没有可收窄的区间，只有恰好采到才命中。怎么判：采样点附近距离几乎不变而参数不精确。怎么防：密采样；需要精确参数时对该段单独做极小值搜索。
- **P4 采样别名**。会怎么错：曲线振荡比扫描间隔快，两个驻点落进同一格，变号相互抵消。怎么判：`sample_count` 加倍后出现更小的距离。怎么防：按起伏尺度调扫描密度。
- **P5 把弧长当参数**。会怎么错：把 `arcLength()` 的结果直接交给 `evaluate()`。怎么判：点的位置对不上，且换一个参数化后结果又变了。怎么防：要参数用 `parameterAtLength()`。
- **P6 以为 3D 曲率有符号**。会怎么错：拿 `curvature()` 的正负判断弯曲方向。怎么判：任何输入都返回非负，同一段点集反向遍历值不变。怎么防：要方向用平面侧的 `signedCurvature()`（需要平面定向），或自己比较加速度与法向。
- **P7 把两曲线最近距离当全局**。会怎么错：两条曲线多处靠近，采样与细化只落进一个低谷，距离偏大。怎么判：提高 `sample_count` 后距离变小。怎么防：按起伏提高采样；需要严格全局时先分段再取最小。

## 7. 已否决选项

- **D1** 给 `Curve<T>` 加 `closestPoint()` / `bounds()`：每个实现各写一套迭代，接口从“求值”扩成“求值 + 求解”（`G7`、`G8`）；算法留在自由函数里，实现者只回答求值。
- **D2** 黄金分割 / 三分搜索直接最小化距离：需要先给一个含极小值的括号（仍然靠采样），只有线性收敛，解析曲线也只能得到近似；驻点求根复用既有 `rootInBracket()` 并能对线段、圆给出精确足点（`G1`、`G10`）。
- **D3** 距离与最近点拆成两个入口（`distanceToCurve()` + `projectOnCurve()`）：同一个极值算两遍，调用方还要学两个名字（`G1`、`G4`）。
- **D4** 用二阶导做 Newton：`f'(u) = |C'|² + (C - P)·C''` 是现成的，但 Newton 不保证不越出区间，也没有现成的括号保护；`rootInBracket()` 已经满足需求，二阶导留给真正需要曲率的消费者。
- **D5** 把弧长或曲率做成 `Curve<T>` 的虚函数：与平面侧 D8 同一理由（`G1`、`G8`）——它们是曲线的测度，不属于某一类的身份，放上接口会让每个实现各写一遍。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 直线足点与不可用输入 | `tests/test_geom_spatial/CurveDistanceTest.cpp` 的 `aPointProjectsOntoALineCurveAtItsFoot`、`unusableInputsAnswerNothing` |
| T2 | 无界曲线必须给窗口、足点在窗口外落到端点 | 同文件 `anUnboundedCurveNeedsAWindow`、`aFootOutsideTheWindowFallsOnTheNearerEnd` |
| T3 | 圆与样条的最近点 | 同文件 `aPointProjectsOntoACircle`、`aPointProjectsOntoASplineCurve` |
| T4 | 等距多解与两种精度 | 同文件 `theCentreOfACircleReportsOneOfItsEquidistantPoints`、`bothPrecisionsFindTheSameFoot` |
| T5 | 弧长与反解：直线按速度、圆按半径、样条与密折线一致、无界缺窗口拒绝、内部反解与两种精度 | `tests/test_geom_spatial/CurveLengthTest.cpp` |
| T6 | 曲率：直线为 0、圆为 1/r（含 float）、椭圆两轴端点、尖点与无效曲线答空、非负性 | `tests/test_geom_spatial/CurveCurvatureTest.cpp` |
| T7 | 两曲线距离：平行线、直线 × 圆、两圆、相交为 0、两参数与点的一致性、无界缺窗口、无效曲线、两种精度 | `tests/test_geom_spatial/CurveCurveDistanceTest.cpp` |

## 9. 状态与演进

- `closestPointOnCurve()`、`curveToCurveDistance()`、`arcLength()`、`parameterAtLength()` 与 `curvature()` 已落地，覆盖直线、圆锥曲线、样条与裁剪视图（都只经接口）。
- 与平面侧同名同形：弧长与反解相同，曲率在 3D 无符号（平面侧是 `signedCurvature()`），两曲线距离的结果只把点换成 `Point3`。
- 已知缺口：最近点与最近距离都是**局部**搜索，多谷曲线需要足够密的采样（P2、P7）；平坦极小值按 §6（P3）。
- 兼容性：纯增量——四个新头文件、两个新类型与四个新自由函数；`Curve<T>` 与所有值类型的签名不变。

## 10. 参考

- `geometry-kinds.md` 2.7（`Curve<T>` 的接口与 `isValid()`）
- `geometry-intersection.md` 2.7 / C5（窗口规则与 `rootInBracket()` 的用法）
- `geometry-surface-queries.md`（曲面侧的同一问题与同一约束写法）
- `../decisions/ADR-2026-11-04-surface-family-queries.md`（自由函数 + 约束 + 数值解法的决定）
