# ADR-2026-11-06：与维度无关的数值内核住在 `math`

- 状态：已取代（内核已移入 `geom/detail`，见 `ADR-2026-11-06-math-keeps-only-vocabulary.md`；内核的签名契约与判据仍然照本条所述）
- 日期：2026-11-06
- 相关：`../design/geometry-curve-queries.md` 2.6、`../design/geometry-planar.md` 2.14、`../design/geometry-intersection.md` 2.7

## 背景

`geom/planar` 与 `geom/spatial` 各自有一份弧长与反解：自适应 Simpson 的递归、带二分保护的 Newton 迭代，逐行相同，只有“速度怎么算”和“长度用哪个点是不同维度”的差别。这类复制不是巧合——它们是照着 3D 版逐行移植出来的，于是同一段数值逻辑有两份可独立漂移的实现（`G1` 违反），而修一个数值缺陷要记得改两处。

`rootInBracket()` 已经给出过答案：`geometry-intersection.md` 2.7 明确写了“求根器为什么在 `math`”——它只认识“一元连续函数 + 括号”，是纯数值算法，几何侧只负责取值与组装结果。同一条判据适用于积分与单调反解。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 维持两份实现 | 零改动，两个模块互不引用 | 同一算法两个家，修一处要记得改另一处；两边的容差语义可以悄悄分叉（`G1`、`G5`） |
| B 3D 侧包含 2D 侧的头 | 改动小 | `geom/spatial` 反向依赖 `geom/planar`，层次出错（`G2`） |
| C 把内核放进 `math`：只认标量函数与区间，几何侧保留取值与结果类型 | 一份实现；依赖方向仍是 geometry → math；两侧语义不可能漂移 | `math` 多了两个模板函数；几何侧仍要为每个算法写十几个取值的胶水 |
| D 只把 2D/3D 曲线算法整体模板化为一份（concept 约束的曲线算法层） | 连胶水也只剩一份 | 需要把“曲线”这个概念搬进 `math`，或者新建共享层；接口面更大，收益只在胶水那十几行 |

## 决定

采用 **C**，先落地两个内核：

- `math/numeric/Quadrature.hpp` 的 `vn::math::numeric::adaptiveSimpson(function, lower, upper, tolerance, max_depth)`：Richardson 误差估计的自适应 Simpson，非有限采样与倒序区间答 `std::nullopt`。
- `math/numeric/RootFinding.hpp` 的 `vn::math::numeric::parameterReaching(value, slope, lower, upper, target, tolerance, max_iterations)`：给定单调量与斜率提示的安全 Newton（越界或斜率过小退回二分），停判据在**值**上；返回值是那个**参数**，名字因此以 `parameter` 开头。同名头里的 `rootInBracket(function, lower, upper, tolerance, max_iterations)` 是变号版本——两个名字都把前提写在里面：括号由调用方给，函数只收窄。
- 两个内核与既有的 `vn::math::numeric::rootInBracket()` 同一个子命名空间：`vn::math` 根层只放数值词汇（常量、类型、概念、初等函数），算法族进子层——这与 `vn::math::spline` 是同一条规矩（`G6`，且不为两个名字多长一层）。

`geom::planar::arcLength` / `parameterAtLength` 与 `geom/spatial` 的同名函数改为只提供 `speed(u)` 与 `reached(u)`，其余交给内核。曲线自己的语义（窗口规则、`isValid()` 守卫、长度量纲、`std::nullopt` 条件）仍留在几何侧，一行不少。

**消费者（两边都用，这是共享的前提）**：

| 内核 | `geom/planar` | `geom/spatial` |
|---|---|---|
| `adaptiveSimpson` | `arcLength()`（`CurveLength.hpp`） | `arcLength()`（`CurveLength.hpp`） |
| `parameterReaching` | `parameterAtLength()` | `parameterAtLength()` |
| `rootInBracket` | `closestPointOnCurve()`（`CurveProjection.hpp`）；`intersectCurveWithCurve()` 经它间接使用 | `closestPointOnCurve()`；`intersectCurveWithSurface()`（`CurveSurfaceIntersection.hpp`） |

三个内核都有两个模块的消费者，没有一个是为“将来可能”而放进 `math` 的。

判据：`G1`（一份实现）、`G2`（依赖仍朝 `math`）、`G9`（新维度 / 新曲线类型复用同一内核）、`G10`（无分配，与原来同样的采样次数）、`G11`（内核可单独测：`tests/test_math/QuadratureTest.cpp` 与 `RootFindingTest.cpp`）。 <!-- drift-ok -->

**为什么不是另起一个 `numeric` 模块**：这里的分层轴不是“算法 vs 数学”，而是“值上的运算”与“以函数为输入的数值过程”——后者仍是数学，OCCT 把 `math_FunctionRoot` / `math_Integration` 就放在它的 Math 包里；本仓库也已经按同一条判据把“3×3 对称矩阵特征分解”放进了 `math`（`geometry-primitives.md` 2.7：`symmetricEigen` 是数值运算不是空间实体，放进 `geometry` 会让 `math` 反向依赖）。隔离不靠新模块，靠三件事：独立头文件（与 `EigenDecomposition` 同一做法）、子命名空间 `numeric`、独立测试文件。新建模块反而会让 `EigenDecomposition` 与 `spline` 留在“另一个家”，变成两个家而不是一个。

**判据（看签名，不看“像不像算法”）**：调用方是否**给出精度目标**（`tolerance` / `max_iterations`），且函数能答“没达到”（`std::nullopt`）？是 → `numeric`（过程）；否 → `math` 根层（求值对象）。`math` 为什么同时承载数学词汇与 CAD 表示约定（`spline` 的节点重数、夹持形式、连续度阶梯），见 `ADR-2026-11-06-math-is-the-base-layer.md`。

| 设施 | 签名里有调用方容差吗 | 归属 |
|---|---|---|
| `spline::deCasteljau` / `differences` / `basisValue` / `knotSpan` / `knotContinuity` | 没有——精确求值，一次算出 | `math` 根层（`spline` 子命名空间）。2D 与 3D 的 `BezierCurve` / `BSplineCurve` / `BezierSurface` / `BSplineSurface` 都用它，是提前于 `numeric` 就存在的同一条分享理由 |
| `symmetricEigen` | 没有——内部固定 `EPS<T>()*4`，返回谱 + 收敛 bool | `math` 根层 |
| `rootInBracket` / `parameterReaching` / `adaptiveSimpson` | 有——容差与迭代上限由调用方给，失败答 `std::nullopt` | `numeric` |

所以 `spline` 在 `math` 里不是先例被破坏，而是同一个判据的另一个结果：它是“算得出来的函数族”，不是“逼近一个目标的过程”。

不选 D 的理由是此刻收益不抵风险：胶水只有十几行，而把曲线概念搬进 `math` 会扩大那层的语义；等到第三对算法（投影 / 距离）也证明“只有取值不同”时再评估。**这条是后续评估的触发条件，不是永久否决。**

## 后果

- 好的：弧长与反解只有一份实现，两个模块的测试同时盯着它；内核有自己的单元测试（已知积分、折点、深度上限、单精度、拒绝条件）；`math` 与几何侧的边界更清楚——`math` 只认标量函数。
- 好的（容差换算的位置）：从“几何容差”到“参数容差”的换算（`value_tolerance / |dP/du|`）需要曲线速度，所以它留在几何侧，两个模块各有一个 `detail::narrowCrossing(curve, value, previous, current, value_tolerance, fallback)`；三处调用点（2D 投影、3D 投影、3D 曲线×曲面）原来逐行重复的那段前置换成一句调用，而内核仍不认识曲线。
- 坏的 / 代价：几何侧多两个 include；内核的参数（`std::optional` 返回值、深度上限语义）成了两个模块共享的约定，改它要同时看两边的设计点。`max_depth = 0` 的含义（仍做一次二分）写在内核文档里，避免两边各自解释。
- 未做：`CurveProjection` / `CurveCurveDistance` 的采样与 Gauss-Newton 仍是两份，因为它们的胶水比例更高（候选筛选、重合判定、二维步长）；按上文的触发条件再评估。
- 未做：更一般的求根器（Brent 的逆二次插值、`|f|` 容差、相切根）。`rootInBracket()` 只做“变号括号内的单根 + 参数宽度容差”，头文件里逐条写了它不是什么；今天三个调用点都只需要参数精度，升级是 `G8` 意义上的过度设计。触发条件：出现要求 `|f|` 达标的消费者，或偏斜括号上的二分收敛率成为瓶颈。
- 升级触发条件（什么时候该把这些过程升成 `vn::Numeric` 模块）：出现需要 `src/` 的非模板实现（比如带工作缓冲的积分器），或 `numeric` 里的过程多到子命名空间装不下，或出现第三个消费者模块要求一条单独链接边。升级时把 `EigenDecomposition` 一并迁入，不做一半——一个“数值算法”只能有一个家。

## 证据

- `src/base/math/sdk/vine/math/numeric/Quadrature.hpp`、`numeric/RootFinding.hpp`（`rootInBracket` / `parameterReaching`）。 <!-- drift-ok -->
- `src/base/geom/sdk/vine/geom/planar/CurveLength.hpp`、`src/base/geom/sdk/vine/geom/spatial/CurveLength.hpp`：两个公开函数的函数体各剩十余行。
- `tests/test_math/QuadratureTest.cpp`、`tests/test_math/RootFindingTest.cpp`：内核的单元测试。 <!-- drift-ok -->
- `tests/test_geom_planar/CurveLengthTest.cpp`、`tests/test_geom_spatial/CurveLengthTest.cpp`：两侧的端到端验收（闭式长度、尖点、窗口拒绝、内部反解、两种精度），抽取后未改一行仍全绿。
