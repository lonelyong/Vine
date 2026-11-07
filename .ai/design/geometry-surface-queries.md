# geometry 族级曲面查询：最近点与射线求交

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`src/base/geom/sdk/vine/geom/spatial` 里对**任意** `Surface<T>` 的两项算法——点到曲面的最近点 / 距离、射线 × 曲面。
> 不含具体类型的解析入口（`Plane::intersectWith(Ray)`、`signedValue()` 等，见 `geometry-ray.md` 与 `geometry-intersection.md`），不含参数化几何（见 `geometry-parametric.md`），不含点到曲线的距离与实体运算（`../backlog/geometry.md` G3）。
> **权威来源**：`src/base/geom/sdk/vine/geom/spatial/SurfaceParameterWindow.hpp`、`SurfaceProjection.hpp`、`RaySurfaceIntersection.hpp`；`tests/test_geom_spatial/SurfaceQueryTest.cpp`。
> **待办**：`../backlog/geometry.md` G3

## 1. 概要设计

### 1.1 问题与背景

`Surface<T>` 是求值接口（`geometry-kinds.md` 2.10）：调用方拿着 `const Surface<T>&` 能取点、取两个一阶导、问参数域，但**不能问任何几何问题**。具体类型各有精确入口——`Plane` 有射线相交，五个解析面有 `signedValue()` 供曲线求根——而“一个未知类型的曲面”只能被求值。

两个后果：

- **裁剪面与样条面没有射线入口**。射线 × `BezierSurface` / `RectangularTrimmedSurface` 今天没有答案，`intersectCurveWithSurface()` 也只接受有隐式方程的解析面。
- **最近点是空白**。点到平面的投影、点到球面的距离、点到样条面的最近点，各自都没有入口；而它们恰好是“未知曲面”最常见的两个问题。

逐类型补入口是 13 种曲面 × 2 个问题，还要为每个新曲面重复一遍（`G9` 的反例）。

### 1.2 目标

- 两个问题各有一个自由函数入口，参数是**接口**，不要求隐式方程、不要求具体类型。
- 参数域无界时由调用方给窗口，函数绝不自己编一个范围。
- 结果沿用既有词汇：距离 / 最近点 / 参数、`RayHit` 的 `t` / 点 / 朝向射线的法向。
- 不扩 `Surface<T>` 接口：不加 `inverse()`、`normalAt()`、`bounds()`。

### 1.3 非目标

- 不做点到**曲线**的距离、实体求交：今天没有消费者（`G8`），留在 backlog G3。
- 不保证全局最优或精确：这是数值算法，性质与失败形态写在 §5 / §6。
- 不做逐类型特例分派：已知类型继续用自己的解析入口，族级入口是兜底而不是替代。

### 1.4 一句话结论

**在参数窗口上采样播种、用牛顿迭代求解——最近点是二维最小化，射线交点是三元方程组；接口因此保持只回答“求值”，代价与局部性写在注意事项里。**

## 2. 设计要点

### 2.1 自由函数 + “派生自 `Surface<T>`”的约束

- **是什么**：`closestPointOnSurface(surface, query, ...)` 与 `intersectRayWithSurface(ray, surface, ...)` 都是头文件模板，`S` 由 `requires std::is_base_of_v<Surface<T>, S>` 约束。
- **为什么**：`G1`、`G9` —— 算法只依赖接口的五个成员（`evaluate`、两个一阶导、域、`isValid`），任何实现都覆盖；写成自由函数而不是虚函数，是因为“两个对象的关系”放不进一元接口，且热路径（`Plane` 的射线入口）不该为族级算法付 vptr。
- **为什么不是 `const Surface<T>&` 参数**：`Plane` 经 `ElementarySurface<T>` 间接派生，模板推导**看不穿**间接基类；概念写成 `SurfaceOf<T>` 时 MSVC 又拒绝概念体里的受约束模板 `Surface<T>`（C7605）。约束放在函数的 `requires` 子句里，两件事都成立。
- **边界**：已知具体类型时继续用解析入口（`Plane::intersectWith(Ray)`）；族级入口不转发、不改写它（`G8`）。

### 2.2 参数窗口是调用方的，且只解析一次

- **是什么**：`SurfaceParameterWindow<T>{ first_u, last_u, first_v, last_v }` 是**参数**矩形，不是空间盒；`finiteParameterWindow(surface, window)` 一处回答“这次搜索用哪个矩形”——**无限的一端取曲面自己的界**，其余按调用方写的，然后要求四个端点有限且区间有序。默认窗口 `naturalSurfaceWindow<T>()` 四个端点都是无限，所以默认就是自然域。
- **为什么**：`G4`、`G6` —— 与曲线 × 曲面的窗口规则是同一条（`geometry-intersection.md` C5）：无界方向由调用方或曲面自身收窄，函数不发明范围。用“四个零”当默认值不行——那是零宽窗口，含义恰好相反；无限端是显式、可读的“用自然域”。用 `Rect2` 会借来 `xmin/ymin` 的字段名，把参数说成坐标。
- **代价**：平面、圆柱、圆锥的自然域本身无界，调用方必须至少给那个方向的窗口；解析入口不受影响（`Plane` 的射线入口不要窗口）。

### 2.3 两项算法共用窗口与播种，不共用迭代

- **是什么**：最近点对 `|S(u,v) - P|²` 做**带阻尼的高斯-牛顿**（`JᵀJ`，只用两个一阶导）；射线交点在 `(u, v, t)` 上解 `S(u,v) = O + t·D`，雅可比是 `[dS/du dS/dv -D]`，用三阶线性方程直接解。
- **为什么**：`G10`、`G1` —— 两个问题的未知量维数不同（2 对 3），把射线化成“沿射线逐点投影再找根”会把一次迭代变成每步一次内层最小化；把投影塞进三元方程则要额外约束。共用的只有“窗口 + 网格播种 + 步长限制 + 线搜索”，共用的是骨架不是算法。
- **边界**：都只调 `evaluate()` 与两个一阶导；都不假设隐式方程、法向朝向或参数含义。

### 2.4 结果词汇：一个投影结果、一个 `RayHit` + 参数

- **是什么**：`SurfaceProjection<T>{ point, u, v, distance }` 同时回答“最近点在哪”和“距离多少”；`RaySurfaceHit<T>{ hit, u, v }` 内嵌 `RayHit<T>`，`RaySurfaceIntersection<T>{ coincident, hits }` 是射线侧的全部答案。
- **为什么**：`G4`、`G6` —— 距离与最近点是同一个极小值的两半，拆成两个入口等于让调用方算两遍；射线命中必须沿用 `RayHit` 的朝向规则（法向点积射线方向 ≤ 0），否则同一场景里出现两套法向约定。
- **边界**：`coincident` 是 `bool` 而不是三值枚举——射线的答案只有“无交 / 有限个交点 / 整条射线在面上”，前两者由 `hits` 是否为空区分，多一个 `None` 值只是重复。

### 2.5 数值性质：局部、可收敛、有代价

- **是什么**：种子是窗口上的 `(seed_count + 1)²` 网格；最近点只精修其中最便宜的 3 个，射线每个种子都试一次；每次迭代至多 64 步，步长限制到窗口的 1/4，线搜索最多 8 次折半。
- **为什么**：`G10`、`G11` —— 高斯-牛顿不需要二阶导；阻尼 + 线搜索保证代价单调不升，失败时停在“没有更好的种子”。复杂度是 `(seed_count+1)²` 次求值 + 每个种子的迭代次数。
- **代价**：最近点是**局部**最小；射线求交漏掉相切（雅可比奇异）与网格分辨不了的折叠；奇异参数点（球极）上的命中法向为零。全部写在 §6。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 点到曲面的最近点 / 距离 | `closestPointOnSurface(surface, query, ...)`：网格播种 + 高斯-牛顿，返回点、`(u, v)`、距离 | 已落地（T1–T4） |
| F2 | 射线 × 曲面 | `intersectRayWithSurface(ray, surface, ...)`：网格播种 + 三元牛顿，返回升序的 `RaySurfaceHit` | 已落地（T5–T9） |
| F3 | 窗口解析与拒绝 | `finiteParameterWindow()`：无界方向、反向区间、`isValid()` 为假一律拒绝 | 已落地（T1、T8） |
| F4 | 射线在面上 | 三个跨射程的探针都投影到面上 → `coincident`，不列点 | 已落地（T9） |
| F5 | 点到曲线距离、实体求交 | 族级算法的另一半 | 未做（G3） |

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `SurfaceParameterWindow.hpp` | `SurfaceParameterWindow<T>`、`naturalSurfaceWindow()` 与 `finiteParameterWindow()`；两个算法共用的唯一窗口解析点 |
| `SurfaceProjection.hpp` | `SurfaceProjection<T>` 与 `closestPointOnSurface()` |
| `RaySurfaceIntersection.hpp` | `RaySurfaceHit<T>`、`RaySurfaceIntersection<T>` 与 `intersectRayWithSurface()` |

完整签名与每个参数的语义见头文件；`RaySurfaceHit::hit` 的 `t`、`point`、`normal` 契约由 `RayHit` 定义，本文不重复。

## 5. 约束、要求与不变量

- **C1**：搜索矩形必须有限。窗口里无限的一端由曲面自己的界补全；补完仍无限、或调用方给了反向 / 零宽区间，就返回 `std::nullopt` / 空结果，不自选范围。
- **C2**：只使用 `Surface<T>` 的 `evaluate` / `derivativeU` / `derivativeV` / 四个域端点 / `isValid`；不要求隐式方程、法向或任何具体类型。
- **C3**：容差是**几何**量（距离 / 点移动量），不是参数差；参数差只有在窗口尺度上才有意义。
- **C4**：射线命中只报告 `t ≥ -tolerance`；`t` 是射线距离，因为 `Ray` 的方向恒为单位。
- **C5**：`coincident` 为真时 `hits` 必须为空——无穷多个点不是一个点集。
- **R1**：不抛异常；输入不可用时返回空答案。
- **R2**：除射线结果的 `std::vector` 外不分配；播种网格与迭代都在栈上。
- **R3**：同一 `T` 的参数与结果，不做混精度。
- **N1**：`SurfaceProjection::point == surface.evaluate(u, v)`；`distance == |point - query|`。
- **N2**：射线命中按 `hit.t` 升序，同一交点只出现一次。
- **N3**：`RaySurfaceHit::hit.normal` 与射线方向点积 ≤ 0（`RayHit` 的规则）；奇异参数点上可以为零向量。

## 6. 注意事项与陷阱

- **P1 重合检测依赖探针**。会怎么错：射线**擦过**曲面但不在面上，三个探针恰好都落在面上（构造出来的情况）会被报成 `coincident`；反过来窗口只覆盖一小段时，探针可能落在窗口外的空间范围。怎么判：`coincident` 为真但相邻射线的命中是有限点。怎么防：探针取的是网格点在射线上的射程 `span_low / mid / span_high`，跨越整块曲面，不是任意三点。
- **P2 最近点是局部最小**。会怎么错：圆环、折叠样条面上有多个局部极小，网格没落在全局那一侧，`distance` 偏大。怎么判：把 `seed_count` 加倍后距离变小。怎么防：按曲面起伏尺度提高 `seed_count`；需要全局保证时用解析入口。
- **P3 球极的法向为零**。会怎么错：射线正好打在球的极点上，`hit.normal` 是零向量，`normal.dot(direction)` 失去意义。怎么判：`normal.length() == 0` 而 `t` / `point` 正常。怎么防：检查法向长度；需要极点法向时用具体类型自己的参数化算。
- **P4 相切漏检**。会怎么错：射线与球面 / 圆柱相切，雅可比奇异，返回空结果而目视相切。怎么判：最近距离 ≈ 0 但没有 hit。怎么防：与曲线 × 曲面的求根同一立场（`geometry-intersection.md` P4），需要切触时另做极值搜索。
- **P5 网格分辨不了的折叠**。会怎么错：样条面在一个网格单元内折回两次，两次穿越落在同一个种子的收敛域里，只回来一个。怎么判：`seed_count` 加倍后多出一个命中。怎么防：按控制网尺度调 `seed_count`。
- **P6 窗口当提示用**。会怎么错：调用方给一个只覆盖一半曲面的窗口，期待“窗外也算”，于是漏命中并以为算法有问题。怎么判：窗口放大后出现新命中。怎么防：窗口是硬约束（C1），不是 hint；要整张面就给整张面的域。

## 7. 已否决选项

- **D1** 给 `Surface<T>` 加 `inverse(point, u, v)` / `normalAt()` / `bounds()`：每个实现都要写一套迭代，接口把“数值算法”变成每个曲面作者的义务，且 `GeometryKindsTest` 的族级扫查要多验三项（`G7`、`G8`）；算法留在自由函数里，实现者只回答求值。
- **D2** 逐类型写最近点与射线特例：13 种曲面 × 2 个问题，新曲面还要再写一遍（`G9`）；已有解析入口的四个类型保持现状，族级入口是兜底。
- **D3** 把最近点做成 `optional<T>` 距离入口、另加一个 `project()`：同一个极小值算两遍，调用方还要学两个名字（`G1`、`G4`）。
- **D4** 射线结果复用 `CurveSurfaceContact` 三值枚举：`Ray` 故意不在曲线族里（`geometry-ray.md` 2.1），借用会把这个事实写错；`bool coincident` + 空 `hits` 已能表达三种答案。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 平面投影与窗口规则 | `tests/test_geom_spatial/SurfaceQueryTest.cpp` 的 `aPointProjectsOntoAPlaneAtItsFoot`、`anUnboundedDirectionNeedsAWindow`、`anInfiniteEndTakesTheSurfacesOwnBound` |
| T2 | 球面 / 圆柱 / 圆环的最近点 | 同文件 `aPointProjectsOntoASphereAlongItsRadius`、`aPointProjectsOntoACylinderThroughItsAxis`、`theNearestPointOnATorusIsTheBestLocalMinimum` |
| T3 | 双精度一致 | 同文件 `bothPrecisionsFindTheSameNearestPoint` |
| T4 | 未使用输入 | 同文件 `anUnboundedDirectionNeedsAWindow` 的反向 / 零宽窗口分支 |
| T5 | 射线 × 平面与解析入口一致 | 同文件 `aRayCrossesAPlaneLikeTheAnalyticEntry` |
| T6 | 射线 × 球面（两次命中、法向朝向、参数自洽） | 同文件 `aRayThroughASphereIsHitTwice`、`aRayFromInsideASphereLeavesOnceWithAnInwardNormal` |
| T7 | 射线 × 圆环四次命中 | 同文件 `aRayThroughATorusIsHitFourTimes`（与 `CurveSurfaceTest` 的同一几何对照） |
| T8 | 窗口是约束、背后命中不算、不可用输入 | 同文件 `theWindowIsAConstraintNotAHint`、`aHitBehindTheOriginIsNotACrossing`、`unusableInputsAnswerNothing`、`anUnboundedDirectionNeedsAWindow` |
| T9 | 样条面命中与重合分类 | 同文件 `aSplinePatchIsHitThroughItsOwnParameters`、`aRayLyingInAPlaneIsCoincidentNotAListOfPoints`、`aCoincidentRayIsAlsoSeenInFloat`（与 `bothPrecisionsAgreeOnASphere` 覆盖 float / double） |

## 9. 状态与演进

- `closestPointOnSurface()` 与 `intersectRayWithSurface()` 已落地，覆盖五个解析面、两个样条面与裁剪视图（都只经接口）。
- 最近点还被曲线 × 曲面用作隐式化：`intersectCurveWithSurfaceByProjection()` 把投影出的带符号距离沿曲线求根（`geometry-intersection.md` 2.9 / F9）；那个带符号距离是 `detail`，因为它的极性属于参数化。
- 曲线侧的同一问题（点到曲线的最近点 / 距离）见 `geometry-curve-queries.md`：同一套“有限窗口 + 采样播种 + 括号安全收窄”的约束。
- 已知缺口：点到曲线的距离、实体求交（`../backlog/geometry.md` G3）；最近点的全局性、相切命中、奇异点法向按 §6 的规则不提供（P2–P4）。
- 兼容性：纯增量——三个新头文件、新类型、新自由函数；`Surface<T>` 与所有值类型的签名不变。
- 代价没有变好也没有变坏：解析入口仍是热路径，族级入口是未知类型的兜底。

## 10. 参考

- `geometry-kinds.md` 2.7 / 2.10（`Curve<T>` / `Surface<T>` 的接口与 `isValid()`）
- `geometry-intersection.md` 2.7 / C5（曲线 × 曲面的窗口规则与求根器）
- `geometry-ray.md` 2.3（`RayHit` 的朝向规则）
- `../decisions/ADR-2026-11-04-surface-family-queries.md`
- OCCT `GeomAPI_ProjectPointOnSurf` / `GeomAPI_IntCS`（参考实现的形态：有状态的算法类；本仓库用无状态自由函数）
