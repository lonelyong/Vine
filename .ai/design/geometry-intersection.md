# geometry 相交配对与结果形态

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`geometry` 里“两个对象求交”的**入口约定**（住在谁身上、叫什么、结果长什么样）与现有配对清单。
> 不含各配对内部的数值算法（在头文件与 `src/`）、不含“算法为什么挂在被交对象上”（`geometry-primitives.md` 2.8、`geometry-ray.md` 2.2）、不含拾取的场景语义（`graphics-scene.md`）。
> **权威来源**：`Plane.hpp`、`LineCurve.hpp`、`Aabb.hpp`、`Obb.hpp`、`Ray.hpp` 的 `intersectWith` / `intersects` / `intersectTriangle`；`tests/test_geometry/LinePlaneTest.cpp`、`RayTest.cpp`。
> **待办**：`../backlog/geometry.md` G11

## 1. 概要设计

### 1.1 问题与背景

相交入口有九个，分散在五个值类型和一个自由函数上。它们的**算法位置**是统一的（被交对象拥有算法），但读起来并不统一：

- 结果形态有三种——`bool` + `Point3` 出参、`bool` + `RayHit` 出参、直接返回 `Intersection` 结构；
- 动词有两种——`intersectWith`（产出几何）与 `intersects`（只判交）；
- “`false` 时出参能不能读”没有写下来的规则，而两种入口的答案不同：`Plane` 的射线入口在 `false` 时不写 `hit`，`Aabb × Aabb` 却在 `false` 时把 out 写成一只能读出间隙的空盒。

于是“两样东西怎么求交”没有一个可预测的答案，新增一个配对时也没有判据。

### 1.2 目标

- 一条能预测的规则：**入口住哪、叫什么、结果长什么样**，三条都写在本文，不在各头文件里各自解释。
- 现有的九个入口不改签名就能满足这条规则（只有一处形态例外，见 2.3）。
- 新配对出现时不必再讨论抽象，只按 §5 的约定加。

### 1.3 非目标

- 不做统一接口 / 虚 `Intersectable` 标签（D1）。
- 不做编译期配对矩阵（D2）。
- 不现在补齐配对表里缺的组合（§3 标“缺”的行）：今天没有消费者（`G8`），见 `../backlog/geometry.md` G11。

### 1.4 一句话结论

**抽象的是词汇与规则，不是分派：算法留在被交对象上，结果形态由“交集可能是什么维数”决定，缺的配对等消费者。**

## 2. 设计要点

### 2.1 动词：产出几何用 `intersectWith`，只判交用 `intersects`

- **是什么**：`intersectWith(...)` 返回 `bool`，交集走最后一个出参；`intersects(...)` 返回 `bool`，不产出任何几何（今天只有 `Obb::intersects(const Obb&)`）。
- **为什么**：`G4` —— 谓词与查询分开命名，调用方不必为了问“碰没碰到”构造一个用不上的结果；SAT 判定本来就不需要产出交集。
- **边界**：新配对先用 `intersectWith`；只有当消费者**明确**只要布尔（没有结果类型）时才加 `intersects`，两个都加等于两个家（`G1`）。

### 2.2 结果形态由“交集可能是什么维数”决定

| 交集 | 结果类型 | 例子 |
|---|---|---|
| 0 维、只需要点 | `Point3<T>` 出参 | `Plane × LineCurve` |
| 0 维 + 射线的到达信息 | `RayHit<T>`（`t` / `point` / 朝向射线的 `normal`） | `Aabb` / `Obb` / `Plane` × `Ray`、`intersectTriangle` |
| 1 维 | `LineCurve<T>` 出参 | `Plane × Plane` |
| 0 维 / 空 / 1 维，且需要分类 | 具名结果结构 | `LineCurve × LineCurve` 的 `Intersection` |

- **为什么**：`G4` —— 结果类型就是答案的形状；用 `RayHit` 表示“射线打到哪儿”比 `Point3` 多出两项本来就该有的信息（距离、朝向法向），而用 `Point3` 表示盒与盒的交集根本装不下。
- **边界**：`RayHit` 属于**射线族**词汇（拾取要的法向朝向规则写在 `geometry-ray.md` 2.3），不要拿它表示非射线配对的结果。新增配对不得引入第四种形态——不返回 `std::optional`、不返回 `std::variant`、不抛异常（D3）。

### 2.3 唯一一处形态例外：`LineCurve::Intersection`

- 两条直线的交集可能是**一个点、空集（异面）或整条直线（共线）**，还要回答平行与最近距离，四态用 `bool + 出参` 表达不了——所以要一个具名结果（`is_intersected` / `is_parallel` / `is_collinear` / 两个参数 / 距离）。
- 这不是遗留形态，而是 2.2 规则的另一半：**当交集的维数不固定、且分类本身是答案的一部分时，返回结构**。将来出现同类配对（例如 `Plane × Plane` 若也要区分共面 / 平行 / 相交）按同一办法处理。
- **代价**：调用方要读结构而不是一个出参（`G7` 的反向代价：把分类塞进 `bool` + 两个额外出参更难看）。

### 2.4 `false` 时出参的语义按入口写清，不再靠猜

- **规则**：几何结果**只在返回 `true` 时被读取**（`RayHit` 类入口一律如此：`false` 时 `hit` 保持调用方原值）。
- **唯一例外**：`Aabb::intersectWith(const Aabb&, Aabb<T>& out)` 在 `false` 时把 `out` 写成一只能读出**间隙**的空盒——那是它的消费者（距离/剔除）需要的信息，写在头文件里。
- **为什么**：`G4`、`G11` —— “出参是不是定义了”必须在入口处可读，否则调用方要么多写一个 `if`，要么读到上一条查询的残留值。

### 2.5 入口住在谁身上

- **射线族归被交体**：`Aabb` / `Obb` / `Plane` / 三角形（自由函数，因为它没有类型）。理由是拾取沿被交对象走（`geometry-ray.md` 2.2）。
- **对称且没有主导方**：自由函数，放在两者共同的模块——将来 `Aabb × Obb`、`Obb × LineCurve` 都属于这一条。
- **需要四态结果的对称配对**：成员函数（`LineCurve × LineCurve`），因为挂在任一侧都成立，选成员只是为了让 `line.intersectWith(other)` 可被发现（`G12`）。

### 2.7 曲线 × 曲面：走隐式求根，不逐对写特例

- **是什么**：任何曲线 × 任何**解析**曲面都由一个模板入口解决：`intersectCurveWithSurface(curve, surface, window, tolerance, interval_count)`。做法是把曲面的**带符号距离**沿曲线采样成 `f(u) = signedValue(surface, curve.evaluate(u))`，在变号的区间上用 `vn::math::refineRoot()` 收窄，零点就是交点。
- **为什么**：`G9`、`G1` —— 逐对写特例是 8 种曲线 × 5 种曲面的矩阵；而“点是否在面上”已经由各曲面自己的隐式方程回答，求根器只需一个一元的 `f(u)`。新加一种曲线不需要改这里，新加一种曲面只需补一个 `signedValue()`。
- **代价**：它只对**有隐式方程**的曲面成立（五个解析面都有），样条曲面/裁剪面没有——那需要参数反演 + 参数格细分，即 `Surface::inverse()` 与包围盒，而这两项正是接口刻意不提供的（2.10 / D8）。所以这一半写成“参数层”，等触发条件（`../backlog/geometry.md` G12）。
- **求根器为什么在 `math`**：`G2` —— `refineRoot()` 只认识“一元连续函数 + 括号”，是纯数值算法；几何侧只负责取值与组装结果。
- **为什么是安全保护区间的假位法而不是纯 Newton**：Newton 无法判断会落到哪个根、在平处发散；假位法 + 二分回退（估计值落进括号两端 10% 内就二分）保证收敛，同时保留快速的前几步。相切仍然漏检（P4），这一点写在头文件里而不是留给调用方猜。

### 2.8 精度：同一 `T`，不做混精度

- 所有入口的两侧类型参数相同；`Aabb::intersectWith(const Ray<U>&, ...)` 显式写了 `requires std::is_same_v<T, U>` 来固定这一点。
- **为什么**：`G4` —— 混精度交叉会在容差里悄悄选一个尺度；`graphics` 的相机射线与顶点本来就是同一精度（`geometry-ray.md` 2.4）。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 直线 × 平面 | `Plane::intersectWith(const LineCurve<T>&, Point3<T>&, eps)`：0 维交点，平行 → `false` | 已落地（T1） |
| F2 | 平面 × 平面 | `Plane::intersectWith(const Plane<T>&, LineCurve<T>&, eps)`：1 维交线；平行与**共面**都 → `false`（N1） | 已落地（T2） |
| F3 | 射线 × 平面 | `Plane::intersectWith(const Ray<T>&, RayHit<T>&, eps)`：命中在射线后方 → `false` | 已落地（T3） |
| F4 | 直线 × 直线 | `LineCurve::intersectWith(const LineCurve&, eps)` → `Intersection` 四态（相交 / 平行 / 共线 / 异面）+ 最近距离 | 已落地（T1） |
| F5 | 盒 × 盒 | `Aabb::intersectWith(const Aabb<T>&, Aabb<T>&)`：`false` 时 `out` 是**间隙盒**（2.4 的唯一例外） | 已落地（T4） |
| F6 | 有向盒 × 有向盒 | `Obb::intersects(const Obb<T>&)`：SAT 判交，不产出交集 | 已落地（T5） |
| F7 | 射线 × 盒 / 有向盒 / 三角形 | `RayHit`：起点在盒内 → `t = 0`、法向朝射线；`Obb` 逆变换到局部复用 `Aabb` | 已落地（T3） |
| F8 | 配对表里缺的组合 | 射线–直线、直线–盒、平面–盒、有向盒–盒 | 未做（`G11`） |
| F9 | 曲线 / 曲面**族**互求交（运行期 `const Surface<T>&` 与样条面 / 裁剪面） | 需要参数反演 + 参数格细分 | 未做（`G12`） |
| F10 | 统一相交接口 | 虚 `Intersectable` / 模板配对矩阵 | 不做（D1、D2） |
| F11 | 曲线 × 解析曲面 | `intersectCurveWithSurface(curve, surface, window, tolerance)`：沿曲线对曲面的带符号距离 `signedValue()` 求根，得到全部交点 | 已落地（T6） |
| F12 | 曲线落在面上 | 同上；连续三个采样点都贴在面上 → `CurveSurfaceContact::Coincident`，不再列点 | 已落地（T7） |

### 3.1 入口清单与结果形态

- **F1 / F2 / F3 的共同点**：交点、交线、射线命中三种结果分别对应 2.2 表的前三行，三者都能从同一对平面方程组里得到，但**结果类型不同**，所以不能共用一个入口。
- **F4 为什么返回结构**：结果维数不固定且分类本身是答案的一部分（2.3）。
- **F5 / F6 为什么一个产出盒子、一个只给布尔**：盒 × 盒的交集仍是一个盒（能装进 `Aabb`），而有向盒 × 有向盒的交集一般是个多面体，`geometry` 里没有能装它的类型——与其返回一个“差不多”的盒，不如只回答“碰没碰到”。

| 被交体 | 对象 | 入口 | 结果 | 说明 |
|---|---|---|---|---|
| `Plane` | `LineCurve` | `Plane::intersectWith(const LineCurve<T>&, Point3<T>&, eps)` | 0 维点 | F1 |
| `Plane` | `Plane` | `Plane::intersectWith(const Plane<T>&, LineCurve<T>&, eps)` | 1 维直线 | F2 |
| `Plane` | `Ray` | `Plane::intersectWith(const Ray<T>&, RayHit<T>&, eps)` | `RayHit` | F3 |
| `LineCurve` | `LineCurve` | `LineCurve::intersectWith(const LineCurve&, eps)` | `Intersection` | F4 |
| `Aabb` | `Aabb` | `Aabb::intersectWith(const Aabb<T>&, Aabb<T>&)` | 盒 | F5 |
| `Aabb` | `Ray` | `Aabb::intersectWith(const Ray<U>&, RayHit<U>&, eps)`（`U == T`） | `RayHit` | F7 |
| `Obb` | `Obb` | `Obb::intersects(const Obb<T>&)` | `bool` | F6 |
| `Obb` | `Ray` | `Obb::intersectWith(const Ray<T>&, RayHit<T>&, eps)` | `RayHit` | F7 |
| 三角形 | `Ray` | `intersectTriangle(ray, a, b, c, hit)` | `RayHit` | F7 |
| 任意曲线 | 五个解析曲面 | `intersectCurveWithSurface(curve, surface, window, tolerance)` | `CurveSurfaceIntersection`（分类 + 交点集合） | F11 / F12 |
| `Ray` | `LineCurve` | — | — | F8 |
| `LineCurve` | `Aabb` / `Obb` | — | — | F8 |
| `Plane` | `Aabb` / `Obb` | — | — | F8 |
| `Aabb` | `Obb` | — | — | F8 |
| 曲线 / 曲面族 | 任意 | — | — | F9 |

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `Plane.hpp` | 三个入口：× 直线（点）、× 平面（直线）、× 射线（`RayHit`） |
| `LineCurve.hpp` | × 直线 → `Intersection`（四态 + 两个参数 + 最近距离） |
| `Aabb.hpp` / `Obb.hpp` | × 盒 / × 射线；`Obb::intersects` 是唯一的纯判交入口 |
| `Ray.hpp` | `intersectTriangle`（自由函数，头文件内联，见 `geometry-primitives.md` 2.8） |
| `Plane.hpp` / `SphericalSurface.hpp` / `CylindricalSurface.hpp` / `ConicalSurface.hpp` / `ToroidalSurface.hpp` | 各一个 `signedValue(surface, point)` 重载：带符号距离，零点即面 |
| `CurveSurfaceIntersection.hpp` | `CurveSurfaceContact`、`CurveSurfaceHit<T>`、`CurveSurfaceIntersection<T>` 与头文件模板 `intersectCurveWithSurface` |
| `math/RootFinding.hpp` | `refineRoot(function, lower, upper, tolerance)`：安全区间的假位法 + 二分回退 |

## 5. 约束、要求与不变量

- **C1**：动词只有两个：产出几何 → `intersectWith`（`bool` + 出参），只判交 → `intersects`。
- **C2**：结果类型只由交集维数与分类需要决定（2.2）；不引入第四种形态，不 `optional`、不 `variant`、不抛异常。
- **C3**：几何结果只在 `true` 时被读取，`RayHit` 类入口在 `false` 时不得写出参；`Aabb × Aabb` 的间隙盒是写明在头文件的例外。
- **C4**：同一对入口两侧精度一致；跨精度不提供隐式入口。
- **R1**：所有入口不分配、不抛异常；结果只由标量运算得到（长算法在 `src/`，短查询在头文件，见 `geometry-primitives.md` 2.8）。
- **N1**：`Plane × Plane` 在共面时返回 `false` 而不是“返回整个平面”——2 维交集没有结果类型（2.2），需要时由调用方按法向与距离判定。
- **C5**：曲线 × 曲面的窗口由调用方给：有界曲线可省略（用自己的域），**无界曲线必须给**，且窗口必须有限；缺窗口返回 `None`，不自选范围。
- **C6**：相切（不变号）不报；`None` 的语义是“没找到交点”，包含掠过的情形（P4）。
- **C7**：一段曲线落在面上时返回 `Coincident` 且清空 `hits`——无穷多点不是一个点集，不能列。
- **C8**：几何容差与参数容差相差一个 `|dP/du|`；`|dP/du| ≈ 0`（雉顶、球极）时改用采样步长的一个分数，不得直接除。

## 6. 注意事项与陷阱

- **P1**：把 `RayHit` 当通用“交点”。会怎么错：拿它表示 `Plane × LineCurve` 的结果，于是 `normal` 的朝向规则（`geometry-ray.md` 2.3）被套到没有射线的场景上。怎么判：结果类型里出现了 `t` 却没有射线。怎么防：2.2 的表格，非射线配对用 `Point3`。
- **P2**：`false` 时读 `RayHit`。会怎么错：拿到上一条查询的残留命中去做法向判断；怎么判：`hit.t` 与被忽略的返回值不一致。怎么防：C3——先看返回值。
- **P3**：以为 `Aabb × Aabb` 的 `out` 只在相交时有意义。会怎么错：`false` 时把它当未初始化丢弃，于是拿不到间隙盒；怎么判：头文件写的是“disjoint 时表示两盒之间的间隙”。怎么防：这一条的例外写在 2.4。
- **P4**：相切漏检（曲线 × 曲面）。会怎么错：曲线擦过曲面（球面相切、柱面相切），`signedValue()` 不变号，于是报 `None` 而目视相交。怎么判：最近距离 ≈ 0 却没有 hit。怎么防：需要切触时先算 `f` 的极值（`f'(u) = 0`）再判 `|f| ≤ tol`，那是另一个算法；默认不做写在头文件 `@warning` 里。
- **P5**：采样别名。会怎么错：曲线的振荡比扫描间隔还快，某些交点落在同一个间隔里、符号变化相互抵消，于是漏根。怎么判：把 `interval_count` 加倍后多出交点。怎么防：按曲线的起伏尺度调间隔，不要用默认值处理高度振荡的曲线。
- **P6**：窗口选错。会怎么错：无界曲线的窗口太小 → 窗外的根不存在于结果里，且表现为“没有交点”。怎么判：把窗口放大一倍后出现新根。怎么防：先用射线/包围盒估一个窗口（3.4 / C5）。
- **P7**：参数容差在奇异点失效。会怎么错：锥顶、球极附近 `|dP/du| → 0`，`tol / |dP/du|` 爆大或除零，二分“收敛”到错参数。怎么判：迭代数触顶或交点跑偏。怎么防：C8 的回退分支——用采样步长的一个分数作参数容差。

## 7. 已否决选项

- **D1** 虚 `Intersectable` / 标签 + 单一 `intersect(const Intersectable&)`：配对是**二元关系**，一元接口表达不了；给 `Aabb` / `Obb` / `Ray` 加热路径 vptr（`G10`），还要求调用方学第二种分派方式（`G7`）。
- **D2** 模板 `intersects<A, B>` concept 矩阵：`Plane::intersectWith(Plane, LineCurve&)` 这类长算法住在 DLL 的 `src/`（`geometry-primitives.md` 2.8），模板过不了 DLL 边界，只会剩下一层薄转发 + 一个 N×M 的编译期矩阵（`G10`、`G7`）。
- **D3** 统一返回 `std::optional<Result>` / 抛异常：仓库既有入口都是 `bool` + 出参（`geometry-ray.md` 2.3），多一种形态等于让调用方同时学两套（`G6`、`G4`）。
- **D4** 现在把 §3 标“缺”的配对全部补齐：没有消费者（`G8`），先写下来等触发条件（G11）。

## 8. 测试与验收

| 验收 | 用例 |
|---|---|
| T1 直线 × 直线 / 直线 × 平面 | `tests/test_geometry/LinePlaneTest.cpp` 的 `skewLineCurvesReportTheClosestPair`、`parallelAndCollinearFlags`、`lineCrossesThePlaneAtOnePoint`、`parallelLineCurveHasNoIntersection` |
| T2 平面 × 平面（含平行、共面） | 同文件 `twoPlanesMeetAlongALineCurve`、`coincidentPlanesReportNoIntersectionLineCurve` |
| T3 射线 × 平面 / 盒 / 三角形（含起点在内、平行、背面、退化） | `tests/test_geometry/RayTest.cpp` |
| T4 盒 × 盒（含不相交的间隙盒） | `tests/test_geometry/AabbTest.cpp` 的 `intersectWith` |
| T5 有向盒 × 有向盒（SAT，含相切与分离） | `tests/test_geometry/ObbTest.cpp` 的 `IntersectsIncludingTouchingAndRejectsSeparatedBoxes` |
| T6 曲线 × 解析曲面 | `tests/test_geometry/CurveSurfaceTest.cpp` 的 `aLineCrossesAPlaneWhereTheAnalyticAnswerSays`（与解析入口逐点对齐）、`aCircleCrossesAnOffCentreSphereTwice`、`aLineThroughATorusMeetsItFourTimes`（两种精度）、`anUnboundedCurveNeedsAWindow`、`unusableInputsAnswerNothingInsteadOfNaN` |
| T7 重合与相切 | 同文件 `aCurveLyingOnTheSurfaceIsCoincidentNotAListOfPoints`、`aTouchWithoutCrossingIsReportedAsNoCrossing`（锁定 P4 的默认行为） |
| T8 求根器本身 | `tests/test_math/RootFindingTest.cpp`（括号、无变号、NaN、斜括号收敛、迭代上限、单精度） |

## 9. 状态与演进

- 九个点/盒/射线入口与“曲线 × 解析曲面”的求根入口均已落地；本轮的曲线 × 曲面是把 `../research/geometry-curve-surface-intersection-2026-11-04.md` 并入本文的结果。
- 已知缺口：§3 标“缺”的盒/射线配对（G11）；曲线 × **非解析**曲面（样条面、裁剪面）需要参数反演 + 参数格细分，未做（G12）；切触交点未做（P4）。
- 兼容性：只新增自由函数与头文件模板，不改任何现有签名；`signedValue()` 与 `CurveSurfaceIntersection.hpp` 是纯增量。

## 10. 参考

- `geometry-ray.md`（射线值类型、`RayHit` 的朝向规则、为什么入口在头文件）
- `geometry-primitives.md` 2.8（短查询在头文件、长算法在 `src/` 的判据）
- `geometry-kinds.md` 2.7 / 2.10（`Curve<T>` / `Surface<T>` 的接口与 `isValid()`）
- `../backlog/geometry.md` G3 / G11 / G12
- OCCT `IntCurvesFace_ShapeIntersector` / `GeomAPI_IntCS` / `BRepAlgoAPI_*`（参考实现的形态：有状态的相交算法类；`geometry-kinds.md` 2.6 说明了本仓库为什么不走这条路）
