# ADR-2026-11-06：`math` 是底座层，不只是数学库

- 状态：已取代（`spline/` 的节点模型与 `Continuity` 已移入 `geom`，见 `ADR-2026-11-06-spline-model-moves-to-geom.md`；`numeric` 的判断不受影响）
- 日期：2026-11-06
- 相关：`ADR-2026-11-06-shared-numeric-kernels-in-math.md`、`../design/geometry-axis-surfaces.md` 2.5、`../design/geometry-primitives.md` 2.7

## 背景

`vn::math` 里现在有三类东西，它们的共同点不是“数学”：

1. **数学对象与初等运算**：`Point2/3`、`Vector*`、`Matrix*`、`Quaternion`、`Transform*`、`Rect2`、`EPS`/`PI`、`Math.hpp` 的初等函数。
2. **数学求值（一次算得出，没有容差）**：`spline::deCasteljau` / `differences`（Bernstein 混合与有限差分）、`spline::basisValue`（Cox–de Boor 基函数及其导数）、`symmetricEigen`。这些是真数学：给定数值就算出数值，没有“收敛到某个精度”的说法。
3. **CAD 表示约定（不是数学）**：`spline::flatKnotCount` / `knotAt` / `knotSpan`（相异节点 + 重数的扁平索引）、`isClampedKnotVector`（夹持形式校验）、`knotContinuity`（重数 → `Continuity` 阶梯），以及 `Continuity` 本身。这些是 NURBS 数据模型的词汇——换一种表示法（比如 OCCT 的展开节点向量）它们就得改写，所以它们不是数学事实。

第 3 类放进 `math` 会让人觉得“怪”，因为它把一个表示约定塞进了数学库。但代码里没有更好的位置：`geom/planar` 与 `geom/spatial` 是**兄弟模块**，两者都要用它，而仓库里没有位于它们之下的公共几何层（`src/base` 下只有 `math` 在两者之前）。所以位置是被依赖图逼出来的，问题在**名字与职责的表述**，不在分层。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持现状，但把 `math` 的职责写明为“底座层” | 零风险；与“两个头不开模块”的 `numeric` 判断一致 | 名字仍然叫 `math`，新读者仍会疑惑一次 |
| B 把 `spline/` 升成模块 `vn::Nurbs`，插在 `math` 与两个几何模块之间 | 语义最准：一层“数学”、一层“表示”、一层“几何类型” | 为两个头开一个模块，与 `numeric` 的判断自相矛盾；且 `math/Continuity.hpp` 也得跟着走，牵连 `Curve`/`Surface` 接口 |
| C 把第 3 类拆成独立子命名空间（`spline` 留基函数、`knots` 放约定） | 同一模块内可见分界 | 两类函数总是一起用（求值前先定位跨），拆开只会让调用方写两个前缀 |

## 决定

采用 **A**：位置不动，把两件事写进文档。

- `vn::math` 的职责是**底座层 + 数学**：任何模块都可以依赖它，它不依赖任何模块（除 `global`/`core`）。数学词汇进根层，数学**过程**进 `numeric`（判据见 `ADR-2026-11-06-shared-numeric-kernels-in-math.md`），查表式的**表示约定**进 `spline`。
- `math/spline` 内部本来就分成两半，头文件里逐条标明：`deCasteljau` / `differences` / `basisValue` 是数学求值；`flatKnotCount` / `knotAt` / `knotSpan` / `isClampedKnotVector` / `knotContinuity` 是 NURBS 表示约定（重数、夹持形式、连续度阶梯）。`Continuity` 同理：它是光滑度阶梯，几何接口与样条约定共用，所以住在 `math` 而不是 `geom/spatial`。

**升级触发条件（什么时候该建 `vn::Nurbs` 模块）**：出现第二个“表示约定”家族需要同一个位置——例如 NURBS 参数化（节点插入、升阶、有理权重的规范化）、裁剪边界约定、容差与逼近策略的共享词汇。那时把 `spline/` 与这些新家族一起升为独立模块，`Continuity` 一并评估（它是三处共用：接口光滑度、样条重数、网格连续性）。只有一个家族时不动：那只是把 `math` 的一条边抽成一层空壳。

## 后果

- 好的：位置有了写下来的理由，下一次“为什么在 math”不必重推；三类的判据一致（求值 → 根层 / 过程 → `numeric` / 表示约定 → `spline`），不因“像不像算法”而摇摆。
- 坏的 / 代价：`math` 这个名字仍然低报了它的职责；新成员必须读这份 ADR 才知道“`math` 也是底座层”。
- 不做：重命名 `vn::math`（跨全部模块的破坏性变更，收益只是名字）。

## 证据

- `src/base/math/sdk/vine/math/spline/BSplineBasis.hpp`：`basisValue`（数学求值）与 `knotSpan` / `isClampedKnotVector` / `knotContinuity`（表示约定）同头，两类函数各有 `@brief` 说明自己属于哪一半。 <!-- drift-ok -->
- `src/base/math/sdk/vine/math/spline/BezierBlend.hpp`：`deCasteljau` 与 `differences`，纯求值，无容差。 <!-- drift-ok -->
- 消费者：`geom/planar` 与 `geom/spatial` 的 `BezierCurve` / `BSplineCurve` / `BezierSurface` / `BSplineSurface`（共 6 个头）——两个兄弟模块都用，是共享的唯一理由。
- `src/base` 目录清单：`geom/planar` 与 `geom/spatial` 之间没有公共几何模块，`math` 是唯一的下层。
