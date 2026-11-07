# geometry 轴对称解析曲面与裁剪

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：旋转面（圆柱 / 圆锥 / 圆环）的框架与参数化、矩形裁剪曲面，以及双参数样条曲面（贝塞尔 / B 样条）的数据模型与求值。
> 不含 `Surface<T>` 接口本身（见 `geometry-kinds.md` 2.10），不含族的归属（见同文档 2.3），不含求交 / 距离 / 三角化算法。
> **权威来源**：`src/base/geom/sdk/vine/geom/spatial/Placement.hpp`、`Frame.hpp`、`CylindricalSurface.hpp`、`ConicalSurface.hpp`、`ToroidalSurface.hpp`、`RectangularTrimmedSurface.hpp`、`BezierSurface.hpp`、`BSplineSurface.hpp`；`src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp`、`BezierBlend.hpp`；`tests/test_geom_spatial/GeometryKindsTest.cpp`
> **待办**：`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

圆柱、圆锥、圆环三个类型此前只是占位。它们的共同点不是“无界”，而是**都绕一根任意轴旋转**：落地前必须先回答两件事——放置存在哪里，“把一个首选方向投到与法向正交的平面里”这条规则放在哪里。仓库里已经有两份这条规则的拷贝（`ConicCurve::inPlaneAxis` 与 `Plane::xAxis`）。

### 1.2 目标

- 三个旋转面可构造、可求值，能经 `const Surface<T>&` 回答十三项。
- 两个样条曲面可构造、可求值，与 `BSplineCurve` 共用同一份节点 / 基函数实现。
- 框架与 `continuity()` 只有一个家；“平面内轴”的规则与曲线族、平面共用一份。
- 裁剪曲面是视图，不复制基曲面。

### 1.3 非目标

- 不把 `SphericalSurface` 迁进旋转面框架：它的轴只改变参数化（2.2）。
- 不给任何曲面提供 `bounds()`：矩形上的支撑需要求解，极点凸包盒是保守的伪值，而今天没有消费者（`G8`）。
- 不做样条曲面的求交、三角化、拼接与周期（wrapped）节点向量；只描述夹持形式。

### 1.4 一句话结论

**旋转面把框架交给值类型 `Placement`、把“平面内轴”交给 `Frame.hpp`；裁剪曲面是基曲面的视图；两个样条曲面是行优先扁平控制网的值类型，求值靠与曲线共用的 `BezierBlend` / `BSplineBasis`。**

## 2. 设计要点

### 2.1 框架是值类型 `Placement`，不是继承层

- **是什么**：`Placement<T>`（`Placement.hpp`）是“一个右手坐标系”：`origin`（原点）、`axis`（单位主方向）、`x_direction`（`u` 的起点），`yAxis() = axis × x_direction` 派生，`hasUsableAxis()` 给出可用性。三个曲面各持一个 `placement` 字段，只剩自己的标量（半径 / 参考半径 + 半角 / 大半径 + 小半径）。
- **为什么**：`G1`、`G6`、`G12` —— 框架的字段与派生规则只有一个家；而**为了一组字段和一个 `return CN` 加一层继承是 `G8` 反对的过度设计**，所以框架做成值类型、`continuity()` 各自一行。类型树因此与 OCCT 同形（`ElementarySurface` → 具体类，见 2.6）。
- **边界**：`ElementarySurface` 仍是空标签（`C9`）；`Placement` 不随类型树分层，也不带虚拟函数；`dynamic_cast` 不再需要——拿到具体曲面就能直接读 `placement`。

### 2.2 放置存不存由“是否改变点集”决定

- 圆柱 / 圆锥 / 圆环绕另一个轴就是另一个曲面，所以放量必须存；球面的轴只改变参数化、不改变点集，所以 `SphericalSurface` 保持世界 Z 轴、不持 `Placement`（`G1`、`G8`）。
- **代价**：球面与 OCCT 球面的参数起点不同（已写在头文件）；三个旋转面各多三个向量，`geometry-kinds.md` 2.2 的大小断言跟着变。

### 2.3 “平面内轴”只有一份实现

- `Frame.hpp` 的 `inPlaneAxis(preferred, normal)`：把首选方向投影到与法向正交的平面并归一化；投影为零时取与法向最不平行的世界轴，规则确定、不需要存状态。
- 此前有两份拷贝（`ConicCurve::inPlaneAxis`、`Plane::xAxis`），现在圆锥曲线、平面、旋转面三处共用一份（`G1`）；`ConicCurve` 的 protected static 已删除，是源码级破坏（仓库内无调用者）。

### 2.4 裁剪曲面是视图

- `RectangularTrimmedSurface<T>` 持 `std::shared_ptr<const Surface<T>>` 与四个参数界，十三项全部转发；闭合 / 周期由“基曲面在该方向闭合 / 周期”与“矩形覆盖整段 / 至少一个周期”共同决定（`G7`、`G10`）。闭合还要求**两端真正落在同一条边界上**：周期基面上是“跨度必须是整数个周期”，矩形覆盖整段但多走半个周期时必须答 false；非周期闭合基面（样条面片）上没有可重复的周期，必须正好用基面自己的两个自然端，外推出去的矩形答 false（与 `TrimmedCurve` 的两端点重合规则同源；两者都要求“覆盖 + 两端落在同一点 / 同一条边界上”）。

### 2.5 样条曲面复用曲线侧的混合与基函数，不写第二份

- **是什么**：`BezierSurface` / `BSplineSurface` 都是 `BoundedSurface<T>` 的值类型：行优先扁平的极点数组 + `u_pole_count` / `v_pole_count`（索引 `iu + u_pole_count * iv`，`u` 连续）、可选的等长权重数组（空 = 多项式）。求值是两次一维过程的外积：先沿 `u` 把每一行混合成一点，再沿 `v` 混合这些点；有理形式在四维齐次坐标上做同样的双重混合，最后除权重。
- **共享的两件基础设施**（都在 `geom::detail`，两个几何模块共用一份）：`BSplineBasis.hpp`（相异节点 + 重数 → 扁平索引、`knotSpan`、`basisValue`、`isClampedKnotVector`，以及“重数 → `Continuity` 阶梯”的 `knotContinuity`）与 `BezierBlend.hpp`（de Casteljau 与有限差分，只吃“一组值 + 一个标量”）。曲面侧没有第二份节点扫描、基函数递推或连续性阶梯（`G1`，C3）。
- **度数与计数只有一个家**：`BezierSurface` 的度数是计数减一，因此不存；`BSplineSurface` 存两个度数，因为节点向量不蕴含它。
- **闭合是数据事实**：两个方向首末行 / 列的**齐次**极点逐点相等即闭合。`BSplineSurface` 也用数据判定——夹持形式下 `v = v_min` 的边界曲线就是首行极点的曲线，而夹持基线性无关，所以曲线相等当且仅当极点相等，不必求值比较（`G10`）。
- **`continuity()`**：贝塞尔恒为 `CN`（段内多项式）；B 样条取两个方向 `C(度 − 最大内部重数)` 的**更差者**，无内部节点的一侧视为 `CN`——这正是把 B 样条曲面做成一个类型的理由（`G1`）。
- **代价**：求值每次调用按控制点数分配一小段临时数组（`deCasteljau` 按值取数组、`differences` 返回新数组），与 `BSplineCurve` 的无分配递归不同；点数大到成为热路径时再优化，见 `../backlog/geometry.md` G10。

### 2.6 与 OCCT 的对齐与分歧

- **OCCT 没有 `Placement` 这个类，也没有 `AxisSurface`**：它把放置 `gp_Ax3 pos` 放进 `Geom_ElementarySurface`，五个初等曲面（平面、圆柱、圆锥、球、圆环）都带它，并统一暴露 `Axis()` / `Location()` / `Position()` / `SetPosition()`；另一个带框架的分支是 `Geom_SweptSurface`（`basisCurve` + `gp_Dir direction`）。
- **对齐的部分**：`placement.origin` = OCCT 的 `Location()`（轴上一点），`placement.axis` = `Axis()` 的主方向，`placement.x_direction` = `gp_Ax3.XDirection`；三个曲面的参数化、参数域、两个方向的闭合 / 周期、`CN` 与“法向由 `D1U × D1V` 决定”的约定逐条对齐（见 `ElSLib::CylinderValue` / `ConeValue` / `TorusValue` 与各自的 `Bounds`）。
- **分歧的部分与理由**：
  - 框架不在标签里：`C9` 要求 `ElementarySurface` 无状态，所以它是值类型成员（2.1）；OCCT 能放进基类是因为它的曲面是引用计数的堆对象，多三个向量不影响拷贝代价，而我们是值类型（`G10`）。
  - 代价是失去 OCCT 那种“对任意初等曲面问 `Position()`”的统一访问器：拿到具体类型就能读 `placement`，拿到 `ElementarySurface<T>&` 时不能。等真出现后者这种消费者时，先考虑 `dynamic_cast`（与 D6 “vtable 回答我是什么”一致），再谈扩接口。
  - 样条曲面只描述**夹持形式**（两个方向首末重数 = 度 + 1）：OCCT 的 `Geom_BSplineSurface` 还接受非夹持与周期节点，这是有意的收窄（`G8`：今天没有消费者），`isValid()` 拒绝非夹持形式。
  - `Plane` 不存放置：它在拾取热路径上按值拷贝，两轴每次由 `normal` 推导（`geometry-kinds.md` 2.10）；OCCT 的 `Geom_Plane` 存 `gp_Ax3`，这是有意的分歧（`G10`）。
  - `SphericalSurface` 仍是世界 Z 轴、`v` 从 +Z 量起（`[0, π]`），OCCT 是 `gp_Ax3` + `v` 从赤道量起（`[-π/2, π/2]`）；理由见 2.2 与 `geometry-kinds.md` 2.10。
  - 不提供可变 API（`SetPosition` / `UReverse` / `VReverse`）：值类型不可变，反向用构造镜像表达（`geometry-kinds.md` 2.7 / D4）。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 轴对称解析曲面 | `CylindricalSurface` / `ConicalSurface` / `ToroidalSurface` 的求值、两个一阶导、参数域与两向闭合周期、向外法向 | 已落地 |
| F2 | 矩形裁剪曲面 | 转发基曲面；闭合 / 周期由基曲面与矩形共同判定；`isValid()` 拒绝空基与反向区间 | 已落地 |
| F3 | 双参数样条曲面 | `BezierSurface` / `BSplineSurface` 的求值、两个一阶导、参数域、两向闭合与 `continuity()` | 已落地 |

### 3.1 F1 轴对称解析曲面

- **行为**：三者共用“径向量 × 标量 + 轴向 × 标量”的形状，差异只在半径如何随 `v` 变：圆柱 `r = R`；圆锥 `r = R + v·sinα`、轴向 `v·cosα`；圆环 `r = R + r₀·cos v`、轴向 `r₀·sin v`。参数约定照 OCCT：圆柱 / 圆锥的 `u` 是方位角，`v` 分别是轴向距离与**母线弧长**，圆环两向都是角。
- **约束**（C1）：`u` 三个类型都在 `[0, 2π]` 闭合周期；圆柱 / 圆锥的 `v` 无界不闭合，圆环的 `v` 也闭合周期；退化轴让参数化塌成沿 `x_direction` 的线段且 `isValid()` 为假（与 `ConicCurve` 同一规则）。
- **验收**（T1）：`aCylindricalSurfaceWrapsItsAxis`、`aConicalSurfaceWalksItsGeneratrix`、`aToroidalSurfaceClosesInBothDirections`。

### 3.2 F2 矩形裁剪曲面

- **行为**：四个参数界直接回答；`isUClosed()` / `isVClosed()` 要求基曲面在该方向闭合、矩形覆盖它的整段，且两端落在同一条边界上（周期基面上 = 跨度是整数个周期；非周期基面上 = 正好用它的两个自然端）；`isUPeriodic()` / `isVPeriodic()` 要求矩形至少盖满一个周期。两者是独立事实：跨 1.5 个周期的矩形周期为真、闭合为假。
- **约束**（C2）：空基曲面与反向矩形都由 `isValid()` 报告；`evaluate()` 在矩形外转发给基曲面，不夹紧。
- **验收**（T1）：`aRectangularTrimmedSurfaceForwardsToItsBasis`。

### 3.3 F3 双参数样条曲面

- **行为**：`BezierSurface` 在 `[0, 1]²` 上做张量积贝塞尔求值，`BSplineSurface` 在两个方向的夹持节点向量上求和；两者都先沿 `u` 把每一行混合成一点、再沿 `v` 混合（2.5）。`derivativeU` / `derivativeV` 用同一套行 / 列差分，有理形式在四维齐次坐标上求值后按商法则求导。`continuity()`：贝塞尔恒 `CN`，B 样条取两个方向的更差者。
- **约束**（C3、C4）：混合与基函数来自 `BezierBlend.hpp` / `BSplineBasis.hpp`，曲面侧不重写；控制网行优先扁平；参数在域外时贝塞尔外推、B 样条夹紧到端点（与各自的曲线同一规则）；`isValid()` 为假时 `evaluate()` 返回首个极点而不报错。
- **验收**（T3）：`aBezierPatchBlendsItsNet`、`aRationalBezierPatchPullsTowardsItsHeavierCorner`、`aBSplinePatchSumsItsTwoBasisFamilies`。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `Placement.hpp` | 右手坐标系值类型：`origin` / `axis` / `x_direction`、`yAxis()`、`hasUsableAxis()` |
| `CylindricalSurface.hpp` / `ConicalSurface.hpp` / `ToroidalSurface.hpp` | 三个解析曲面（各持一个 `Placement`）、各自的 `pointAt` / `normalAt` / `isValid` 与 `*f` / `*d` 别名 |
| `RectangularTrimmedSurface.hpp` | 基曲面句柄 + 四个参数界与 `*f` / `*d` 别名 |
| `BezierSurface.hpp` / `BSplineSurface.hpp` | 两个样条曲面：行优先扁平控制网 + 可选等长权重、`degreeU()` / `degreeV()`、`poleAt(iu, iv)`、`isValid()` 与 `*f` / `*d` 别名 |
| `BezierBlend.hpp` / `BSplineBasis.hpp` | 与曲线共用的两件基础设施：de Casteljau 混合 + 有限差分；相异节点 + 重数 → `knotSpan` / `basisValue` / `isClampedKnotVector` / `knotContinuity` |
| `Frame.hpp` | 共用的 `inPlaneAxis(preferred, normal)`（曲线族、平面、旋转面共用） |

## 5. 约束、要求与不变量

- **C1**：框架只在 `Placement` 定义一次并在构造时归一化；三个曲面各持一个 `placement` 字段，不得再声明同名的 `origin` / `axis` / `x_direction` 字段，也不得在构造后改写它。
- **C2**：裁剪曲面的闭合 / 周期是“基曲面 + 矩形”共同的事实，不得只看一边；闭合还要求“两端落在同一条边界上”，周期基面上即跨度是整数个周期，非周期基面上即正好落在它的两个自然端。空基与反向矩形由 `isValid()` 报告。
- **C3**：样条曲面的求值必须复用曲线侧的共享实现（`BSplineBasis.hpp` 的节点扫描 / 基函数、`BezierBlend.hpp` 的 de Casteljau 与差分），不得在曲面侧重写。
- **C4**：控制网是行优先扁平数组，索引 `iu + u_pole_count * iv`；两个计数、网长度与权重长度必须互相一致，由 `isValid()` 检查，不得只靠调用方自觉。
- **R1**：三个解析曲面的 `evaluate` / `derivativeU` / `derivativeV` / `isValid` 不分配、不抛异常，只有标量运算。
- **R2**：样条曲面的 `evaluate` / 两个一阶导不抛异常，但每次调用按控制点数分配临时数组（2.5 的代价）；`isValid()` 不分配。
- **N1**：`axis` 与 `x_direction` 在构造后恒为单位且互相正交，因此 `yAxis()` 也是单位向量；构造后改写这三个字段会让它们互相矛盾（C1 的读法）。
- **N2**：`pointAt(u, v)` 与 `evaluate(u, v)` 恒为同一个值；`normalAt` 恒指向背离轴（圆环是背离管心）的一侧，即 `derivativeU × derivativeV` 的方向，不随参数翻转。
- **N3**：样条曲面 `isValid()` 为真时，两个计数 ≥ 2、网长度 = 计数之积、权重为空或等长且全为正、`BSplineSurface` 两个方向首末重数 = 度 + 1 且相异节点严格递增；为假时几何查询沿用退化规则（`evaluate()` 返回首极点、`isUClosed()` / `isVClosed()` 为假），不抛异常。

## 6. 注意事项与陷阱

- **P1**：圆柱 / 圆锥的 `v` 与圆环的 `v` 不是同一种量。会怎么错：把 `v` 一律当“高度”，于是把圆锥的 `v` 当轴向长度、圆环的 `v` 当半径。怎么判：`derivativeV().length()` 在圆柱是 1（轴向）、圆锥也是 1（母线弧长）、圆环是小半径。怎么防：按头文件写的量纲传参，不靠字段名猜。
- **P2**：裁剪曲面的闭合不是基曲面的闭合，也不是“覆盖整段”。会怎么错：基曲面是圆柱就认为裁剪块也闭合，把四分之一柱面当一整圈做接缝判断；或者矩形覆盖了整段却多走半个周期，就以为两条边界边重合。怎么判：`isUClosed()` 返回假而基曲面返回真，或两条 u 边上的对应点距离不为零。怎么防：闭合 / 周期只问裁剪本身（C2）；不要自己按“基面闭合 + 覆盖整段”推结论。
- **P3**：行优先的索引方向弄反。会怎么错：把控制网当列优先（`iv + v_pole_count * iu`），得到的是转置的曲面；方阵时外形完全正确、`2×3` 时悄悄换成 `3×2`、单行 / 单列时看不出。怎么判：`poleAt(0, 1)` 是否等于 `poles[u_pole_count]`。怎么防：读网只用 `poleAt(iu, iv)`，自己按下标访问前先看私有 `offset()` 的写法（C4）。
- **P4**：矩形裁剪的 `uPeriod()` / `vPeriod()` 不是基曲面的周期。会怎么错：基曲面是圆柱就以为裁剪块也是 2π 周期，拿它去归一化参数；四分之一柱面会因此被当成一圈。怎么判：`isUPeriodic()` / `isVPeriodic()` 为假而 `uPeriod()` / `vPeriod()` 为 0（基曲面的对应答案为真 / 2π）。怎么防：周期值只问裁剪自己，要基曲面的数字就直接读 public 的 `basis`（与曲线侧 `TrimmedCurve` 同一规则，`geometry-kinds.md` 2.7 / P8）。

## 7. 已否决选项

- **D1** 把框架做成继承层（`AxisSurface`，本仓库曾经如此）或塞进 `ElementarySurface` 标签：前者为一组字段和一个 `return CN` 加一层继承，后者违反 `C9`；也不让 `SphericalSurface` 迁移——球的轴只改参数化（2.2）。见 `../decisions/ADR-2026-11-04-surface-placement-value-type.md`（已取代 `ADR-2026-11-04-axis-surface-frame.md`）。
- **D2** 用 `vector<vector<Point>>` 存样条曲面的控制网：一层指针换不来表达能力，行列索引要么多一次间接、要么隐含长度约定（`G10`、`G12`）；定为行优先的扁平网（C4）。
- **D3** 用“求值比较两条边界曲线”判 `BSplineSurface` 的闭合：夹持形式下 `v = v_min` 的边界曲线就是首行极点的曲线，而夹持基线性无关，所以齐次极点相等与曲线相等等价（2.5）；求值比较只会更慢、还会引入相等的容差问题（`G10`）。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 三个旋转面的参数化、导数、域与闭合、退化输入；裁剪的转发、四种无效输入与整数周期闭合 | `tests/test_geom_spatial/GeometryKindsTest.cpp` 的 `aCylindricalSurfaceWrapsItsAxis`、`aConicalSurfaceWalksItsGeneratrix`、`aToroidalSurfaceClosesInBothDirections`、`aRectangularTrimmedSurfaceForwardsToItsBasis`、`aTrimmedSurfaceClosesOnlyWhenItsEdgesMeet` |
| T2 | 归属与大小 | 同文件顶部 `static_assert`（`sizeof(CylindricalSurfaced)` 等三项、`sizeof(Placement<double>) == 3 * sizeof(Point3d)`、两个样条曲面是 `BoundedSurface` 且非抽象）与 `aPlacementCompletesAndNormalisesItsFrame` |
| T3 | 样条曲面的求值、有理形式、`continuity()` 与无效网 | `aBezierPatchBlendsItsNet`（双线性网中点为 `(0.5, 0.5, 0.5)`、角点插值、`derivativeU` / `derivativeV`）、`aRationalBezierPatchPullsTowardsItsHeavierCorner`（`2×2` 权重 `{1,1,1,3}` 中点 = `(2/3, 2/3)`）、`aBSplinePatchSumsItsTwoBasisFamilies`（双线性 B 样条等于同网的贝塞尔、二次 `apron` 的 `derivativeU(0,0) = (2,2,0)`、内部重数 2 使 `continuity()` 降为 `C0`） |
| T4 | 族级曲面契约、裁剪的周期与空基行为 | `everySurfaceKeepsTheInterfacePromises`（八个曲面逐条问域有序、周期与周期值一致且按周期重复、`isCN` 与 `continuity()` 一致、有限矩形内两个一阶导有限）、`aRectangularTrimmedSurfaceForwardsToItsBasis` 内的 `uPeriod()` 与空基断言 |

## 9. 状态与演进

- 三个旋转面、矩形裁剪曲面与两个样条曲面已落地；框架是值类型 `Placement`，三个曲面各持一个。
- 共享基础设施：`BSplineBasis.hpp` 现在还提供 `knotContinuity()`，重数 → `Continuity` 的阶梯只有一份，曲线与曲面都调它（C3 的延伸）。
- 已知缺口：样条曲面求值会分配临时数组（R2，触发条件见 `../backlog/geometry.md` G10）、只支持夹持节点向量、无 `bounds()`；样条曲面自身没有待办项（G6 已关闭）。
- 兼容性：`AxisSurface` 层已删除（曾短暂存在）；`ConicCurve::inPlaneAxis` 被删除（源码级破坏，仓库内无调用者）；`BSplineCurve` 的私有 `basisValue` / `findSpan` / 扁平索引扫描已移到 `BSplineBasis.hpp`（私有接口，不破坏）；`GeometryKindsTest.cpp` 的占位断言改成“已实现 + 不可默认构造”，并新增 `aPlacementCompletesAndNormalisesItsFrame` 与三个样条曲面用例。

## 10. 参考

- `geometry-kinds.md` 2.3（族的归属）、2.10（`Surface<T>` 接口）、2.9（`ConicCurve` 的同构做法）
- `../decisions/ADR-2026-11-04-surface-placement-value-type.md`（框架是值类型）
- `../decisions/ADR-2026-11-04-axis-surface-frame.md`（已取代：曾把框架做成继承层）
- OCCT `Geom_CylindricalSurface` / `Geom_ConicalSurface` / `Geom_ToroidalSurface` / `Geom_RectangularTrimmedSurface` / `Geom_BezierSurface` / `Geom_BSplineSurface` 与 `ElSLib::CylinderValue` / `ConeValue` / `TorusValue`（参数化约定来源）
