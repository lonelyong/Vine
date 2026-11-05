# geometry 点到曲线的最近点与距离

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`src/base/geom3d` 里对**任意** `Curve<T>` 的最近点 / 距离，以及窗口规则与代价。
> 不含曲线 × 曲面求交（见 `geometry-intersection.md`）、点到曲面（见 `geometry-surface-queries.md`）、曲线与曲线之间的最近距离、弧长参数化。
> **权威来源**：`src/base/geom3d/sdk/vine/geom3d/CurveProjection.hpp`；`tests/test_geom3d/CurveDistanceTest.cpp`。
> **待办**：`../backlog/geometry.md` G3（只剩实体求交）

## 1. 概要设计

### 1.1 问题与背景

`Curve<T>` 是求值接口（`geometry-kinds.md` 2.7）：拿 `const Curve<T>&` 的调用方能取点、取一阶 / 二阶导、问参数域，但**问不出“离这个点最近的是哪里”**。逐类型补入口要覆盖直线、四种圆锥曲线、裁剪视图与两种样条共八种类型，新曲线再写一遍（`G9` 的反例）；而曲面侧已经有 `closestPointOnSurface()`，曲线侧空着，两个族不对称。

### 1.2 目标

- 一个自由函数入口，参数是接口，不要求具体类型、不要求解析形式。
- 参数域无界时由调用方给窗口，函数不自选范围——与曲线 × 曲面、曲面投影同一条规则。
- 一个结果同时回答“最近点在哪”“参数是多少”“距离多少”。

### 1.3 非目标

- 不做曲线与曲线之间的距离 / 最近点对：没有消费者（`G8`），留在 backlog。
- 不做弧长参数化与等距重参数化：那是另一件事，与最近点无关。
- 不扩 `Curve<T>` 接口：不加 `closestPoint()`、不加 `bounds()`。

### 1.4 一句话结论

**最近点是平方距离的驻点，也就是 `f(u) = (C(u) - P) · C'(u)` 的零点；采样找出变号区间、用 `refineRoot()` 收窄，再与窗口端点比较，取距离最小的那个。**

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

- **是什么**：`f(u) = (C(u) - P) · C'(u)` 是平方距离的导数；它的零点就是最近点、最远点与其它驻点。扫描窗口、对每个变号区间用 `vn::math::refineRoot()` 收窄，再与每个采样点、两个窗口端点比较距离，取最小者。
- **为什么**：`G1`、`G10` —— 求根器已经在 `math`（曲线 × 曲面在用），驻点方程只需一阶导；窗口端点必须参与比较，因为“足点落在窗口外”时 `f` 在窗口内没有零点，最小值在边界上。
- **为什么不用黄金分割 / 三分搜索**：那需要先给一个包含极小值的括号，仍然要靠采样；`refineRoot()` 既已存在又是括号安全算法，解析曲线上还能收敛到精确的足点（线段、圆都是精确解），而不是单纯形搜索的近似值。见 §7 D2。
- **边界**：只调 `evaluate()` 与 `derivative()`；不要求解析形式，也不要求二阶导（驻点方程已经是一阶）。

### 2.4 结果与曲面侧同形

- **是什么**：`CurveProjection<T>{ point, parameter, distance }`，字段语义与 `SurfaceProjection<T>{ point, u, v, distance }` 一一对应，只把两个参数换成一个。
- **为什么**：`G6`、`G4` —— 同一类问题的答案在不同族里长成同一个形状，调用方不必记两套字段名；距离与最近点仍然是同一个极值的两半，不拆成两个入口（`G1`）。
- **边界**：参数是曲线自己的量纲（直线是距离、圆是角、样条是节点值），不做归一化。

### 2.5 代价与局部性

- **是什么**：`sample_count + 1` 次采样（每次一次 `evaluate` + 一次 `derivative`），每个变号区间至多 64 步收窄。
- **为什么**：`G10`、`G11` —— 与曲面投影同一个骨架：网格播种 + 括号安全收窄；不分配、不抛异常。
- **代价**：答案是**局部**最优；平坦极小值（`f` 不变号）只能靠采样恰好落在上面；分辨率不足时提升 `sample_count`。全部写在 §6。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 点到曲线的最近点 / 距离 | `closestPointOnCurve(curve, query, window, tolerance, sample_count)`：驻点求根 + 端点比较，返回点、参数、距离 | 已落地（T1–T4） |
| F2 | 窗口当硬约束 | 足点落在窗口外时答案为更近的那个端点，不外推 | 已落地（T2） |
| F3 | 曲线 × 曲线最近距离 | 两族之间的最小距离（需要组合求根或细分） | 未做（`G8`，backlog G3） |

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `CurveProjection.hpp` | `CurveProjection<T>` 与 `closestPointOnCurve()` |

完整签名与每个参数的语义见头文件。

## 5. 约束、要求与不变量

- **C1**：搜索区间必须有限。无界曲线必须由调用方给窗口；没给、窗口反向或零宽都返回 `std::nullopt`，不自选范围。
- **C2**：容差是**几何**量；换算成参数容差时用 `tolerance / |dP/du|`，在曲线停住处回退到采样步长的一个分数。
- **R1**：不分配、不抛异常；输入不可用时返回空答案。
- **R2**：只使用 `Curve<T>` 的 `evaluate` / `derivative` / 两个域端点 / `isValid`。
- **N1**：`CurveProjection::point == curve.evaluate(parameter)`；`distance == |point - query|`。
- **N2**：同距离的多个候选按参数从小到大先到先得（例如圆心到圆上每一点等距，报第一个采样点）。

## 6. 注意事项与陷阱

- **P1 窗口太窄**。会怎么错：足点在窗口外，答案落在端点上而被当成“最近点”。怎么判：参数等于窗口端点且 `f` 在端点外仍同号。怎么防：先用包围或几何直觉估窗口（C1）。
- **P2 局部最小**。会怎么错：曲线来回靠近查询点，采样只找到其中一个极小值，距离偏大。怎么判：`sample_count` 加倍后距离变小。怎么防：按曲线起伏尺度提高 `sample_count`。
- **P3 平坦极小值没有括号**。会怎么错：`f` 在极小值处相切于零而不变号，`refineRoot()` 没有可收窄的区间，只有恰好采到才命中。怎么判：采样点附近距离几乎不变而参数不精确。怎么防：密采样；需要精确参数时对该段单独做极小值搜索。
- **P4 采样别名**。会怎么错：曲线振荡比扫描间隔快，两个驻点落进同一格，变号相互抵消。怎么判：`sample_count` 加倍后出现更小的距离。怎么防：按起伏尺度调扫描密度。

## 7. 已否决选项

- **D1** 给 `Curve<T>` 加 `closestPoint()` / `bounds()`：每个实现各写一套迭代，接口从“求值”扩成“求值 + 求解”（`G7`、`G8`）；算法留在自由函数里，实现者只回答求值。
- **D2** 黄金分割 / 三分搜索直接最小化距离：需要先给一个含极小值的括号（仍然靠采样），只有线性收敛，解析曲线也只能得到近似；驻点求根复用既有 `refineRoot()` 并能对线段、圆给出精确足点（`G1`、`G10`）。
- **D3** 距离与最近点拆成两个入口（`distanceToCurve()` + `projectOnCurve()`）：同一个极值算两遍，调用方还要学两个名字（`G1`、`G4`）。
- **D4** 用二阶导做 Newton：`f'(u) = |C'|² + (C - P)·C''` 是现成的，但 Newton 不保证不越出区间，也没有现成的括号保护；`refineRoot()` 已经满足需求，二阶导留给将来真正需要曲率的消费者（`G8`）。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 直线足点与不可用输入 | `tests/test_geom3d/CurveDistanceTest.cpp` 的 `aPointProjectsOntoALineCurveAtItsFoot`、`unusableInputsAnswerNothing` |
| T2 | 无界曲线必须给窗口、足点在窗口外落到端点 | 同文件 `anUnboundedCurveNeedsAWindow`、`aFootOutsideTheWindowFallsOnTheNearerEnd` |
| T3 | 圆与样条的最近点 | 同文件 `aPointProjectsOntoACircle`、`aPointProjectsOntoASplineCurve` |
| T4 | 等距多解与两种精度 | 同文件 `theCentreOfACircleReportsOneOfItsEquidistantPoints`、`bothPrecisionsFindTheSameFoot` |

## 9. 状态与演进

- `closestPointOnCurve()` 已落地，覆盖直线、圆锥曲线、样条与裁剪视图（都只经接口）。
- 已知缺口：曲线 × 曲线的最近距离（backlog G3）；局部最优与平坦极小值的限制按 §6 不提供（P2、P3）。
- 兼容性：纯增量——一个新头文件、一个新类型、一个新自由函数；`Curve<T>` 与所有值类型的签名不变。

## 10. 参考

- `geometry-kinds.md` 2.7（`Curve<T>` 的接口与 `isValid()`）
- `geometry-intersection.md` 2.7 / C5（窗口规则与 `refineRoot()` 的用法）
- `geometry-surface-queries.md`（曲面侧的同一问题与同一约束写法）
- `../decisions/ADR-2026-11-04-surface-family-queries.md`（自由函数 + 约束 + 数值解法的决定）
