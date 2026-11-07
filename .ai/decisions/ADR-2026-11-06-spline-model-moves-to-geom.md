# ADR-2026-11-06：B 样条的节点模型与 `Continuity` 从 `math` 移入 `geom`

- 状态：部分取代（“Bernstein 求值留在 `math/spline`”与“`math/numeric` 不动”两条已被 `ADR-2026-11-06-math-keeps-only-vocabulary.md` 取代；节点模型与 `Continuity` 的移动结论不变）
- 日期：2026-11-06
- 相关：`../design/math-base-layer.md`、`../design/geometry-module.md` 2.3 / 7、`ADR-2026-11-06-geom-detail-subtree.md`、`ADR-2026-11-04-bspline-without-knot-breaks.md`

## 背景

`math/spline/` 自建立起就装着两件事：`BezierBlend.hpp`（`deCasteljau` / `differences`，Bernstein 求值）与 `BSplineBasis.hpp`（相异节点 + 重数这一套节点模型，加上 Cox–de Boor 基函数）。它们进 `math` 的理由只是“当时没有别的位置”，`ADR-2026-11-06-math-is-the-base-layer.md` 为此把 `math` 重新定义成“底座层，不只是数学库”——**一篇文档要为名字辩护，本身就是错位的信号**（`G4`）。

判据后来变了：`geom/planar` / `geom/spatial` 合并成一个模块并有了 `geom/detail` 子树，于是“共享的几何东西放哪”有了答案，而“共享的数学东西”仍然只有 `math`。分开之后问题变得清楚：

| 头 | 内容 | 签名里有我们自己的数据模型吗 | 消费者 |
|---|---|---|---|
| `BezierBlend.hpp` | 一组值 + 一个标量，Bernstein 混合与有限差分 | 没有 | 两棵树的 `BezierCurve` / `BezierSurface` |
| `BSplineBasis.hpp` | `flatKnotCount` / `knotAt` / `knotSpan` / `isClampedKnotVector` / `knotContinuity` / `basisValue` | **有**：相异节点 + 重数就是我们选的 NURBS 表示，`knotContinuity` 甚至直接答几何接口的 `Continuity` | 两棵树的 `BSplineCurve` / `BSplineSurface` |

同样的检查落到 `math/Continuity.hpp`：全仓库直接 include 它的只有 `geom/planar/Curve.hpp`、`geom/spatial/Curve.hpp`、`geom_global.hpp`——它的唯一消费者也是几何。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持现状 | 零改动 | 继续用一篇文档为 `math` 的名字辩护；`basisValue` 的模型住在“数学”层，而 `math` 里没有第二个消费者 |
| B 整个 `spline/` 目录搬进 `geom/detail` | 一条规则讲完 | 连带把纯数学的 Bernstein 求值也搬走：将来做动画/重采样要用它，就得依赖曲线库（`G2` 反向） |
| C 按“签名里有没有我们的数据模型”切：`BSplineBasis` → `geom/detail`，`BezierBlend` 留 `math/spline`；`Continuity` → `geom/Continuity.hpp` | 每一层只装自己认识的东西；两个头都不必改写签名 | `math/spline` 只剩一个头；`Continuity` 换了 include 路径 |
| D 让 `math` 保持纯数学：`basisValue` 改吃**扁平**节点向量，由几何侧负责扁平化 | 模型与求值彻底分开 | 每次求值都要扁平化一次（分配），正是当初选“相异 + 重数”要避免的代价（`G1` 让位给 `G10` 的那条取舍） |

## 决定

采用 **C**：

- `BSplineBasis.hpp` → `src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp`，命名空间 `vn::geom::detail`。它维度无关（不命名 `Point2` / `Point3`），符合 `geom/detail` 的既有规则；它只有模块内消费者，所以 `detail` 是诚实的标签。
- `Continuity.hpp` → `src/base/geom/sdk/vine/geom/Continuity.hpp`，命名空间 `vn::geom`（模块根，**不是** `detail`）：它出现在 `Curve<T>` / `Surface<T>` 的纯虚签名里，是公共词汇；`geom_global.hpp` 把它重导出进两个维度命名空间（`geom/planar` / `geom/spatial` 是 `geom` 的**兄弟**而不是子命名空间，所以这个 import 是必需的，不是便利）。
- `BezierBlend.hpp` 留在 `src/base/math/sdk/vine/math/spline/`：纯求值，无模型；将来若出现非几何的 Bernstein 消费者，这条决定就被印证。 <!-- drift-ok -->
- `math/numeric` 不动：求根与积分是“调用方给精度目标”的过程，任何模块都可能需要，`basisValue` 不是那种东西。
- 一处实现约束：`Continuity.hpp` 直接写 `VN_ROOT_NS_BEGIN namespace geom {`，不通过 `VN_GEOM_NS_BEGIN`——`geom_global.hpp` 要 include 它来做重导出，用它自己的宏就会形成“两边都还没声明就互相 include”的环。

判据：`G1`（一个概念一个所有者：节点表示归几何）、`G2`（纯数学仍留在底座层，几何概念不再占底座层）、`G4`（名字不再需要文档辩护）、`G10`（不引入每次求值的扁平化）、`G13`（表示的约定跟着存它的几何类型走）。`G8` 的反面检查：`math/spline` 只剩一个头，但它不是为将来预建的——它是 Bernstein 求值，今天就有消费者。

## 后果

- 好的：`math` 里不再有任何几何概念；`geom` 自带全部几何词汇（`Continuity`）与表示约定（节点模型），几何模块对样条表示的自足性不再依赖“底座层的巧合”；`knotContinuity` 与使用它的 `continuity()` 在同一个模块内。
- 代价 / 迁移：`<vine/math/spline/BSplineBasis.hpp>` → `<vine/geom/detail/BSplineBasis.hpp>`，`math::spline::` → `geom::detail::`（3 个头、26 处调用）；`<vine/math/Continuity.hpp>` → `<vine/geom/Continuity.hpp>`（3 个头直接 include，其余经 `geom_global.hpp`）；`vn::math::Continuity` → `vn::geom::Continuity`（6 个测试、13 处）；`tests/test_math/BSplineBasisTest.cpp` → `tests/test_geom_spatial/`（`test_math` 只链 `vn::Math`，不再认识这个头）。 <!-- drift-ok -->
- 反转条件：出现**非几何**的节点模型消费者（例如机器人轨迹插值要直接构造 B 样条）时重新评估——那时它要么再上一个共享位置，要么连 `vn::Nurbs` 一起升模块；出现第二个表示约定家族时，`ADR-2026-11-06-math-is-the-base-layer.md` 里写的 `vn::Nurbs` 触发条件依然有效。

## 证据

- `src/base/geom/sdk/vine/geom/detail/BSplineBasis.hpp`、`src/base/geom/sdk/vine/geom/Continuity.hpp`；`src/base/math/sdk/vine/math/spline/` 只剩 `BezierBlend.hpp`。 <!-- drift-ok -->
- `geom_global.hpp`：`using geom::Continuity;` 两处，`VN_GEOM_NS_BEGIN` / `VN_GEOM_DETAIL_NS_BEGIN` 三个模块级宏。
- 测试：`test_math` 56 → 49（7 个 B 样条基函数用例迁走），`test_geom_spatial` 193 → 200，`test_geom_planar` 138 不变；合计 357 全绿。
- 门禁：`check_geom_layering.py` 67 个几何头 0 处违规（`geom/detail` 不 include 任一棵维度树）。
