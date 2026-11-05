# ADR-2026-11-04：B 样条的内部重数不超过度数，缺口不是一条曲线

> **状态**：已采纳
> **日期**：2026-11-04
> **相关**：`../design/geometry-kinds.md` 2.8 / C13 / P10；`src/base/geometry/sdk/vine/geometry/BSplineBasis.hpp`（`isClampedKnotVector`、`knotContinuity`）、`BSplineCurve.hpp`、`BSplineSurface.hpp`；`tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## 背景

`isClampedKnotVector()` 此前只要求每个相异节点的重数在 `[1, degree + 1]`，两端必须恰好是 `degree + 1`。
于是**内部**节点的重数也可以取到 `degree + 1`。数学上这是把曲线（或曲面的一个方向）切成两段互不相关的部分：
参数跨过该节点时点会**跳**，两侧各自是独立的 B 样条段。

问题是框架的其余部分不会知道这件事：

- `knotContinuity()` 算 `degree - m`，`m = degree + 1` 时得到 `-1`，被夹到 `C0`。于是 `continuity()` 回答 `C0`，
  而 `Continuity::C0` 的定义是「位置连续：各段交于一点」——对一个根本不相交的对象说了假话；
- `isCN(0)` 随之为真，`isValid()` 也随之报可用，于是「唯一的判定入口」（`geometry-kinds.md` C12）在这条输入上失效。

这不是理论构造：`{3, 3, 3}` 这样的重数在从 CAD 数据重建样条时很常见，只要调用方把一段被切开的数据当成一条曲线喂进来。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. `isValid()` 拒绝内部重数 `degree + 1` | 不改公共枚举；`continuity()` 的 `C0` 重新是真的（「交于一点」）；把「一段数据 ≠ 一条曲线」这个判断交给调用方显式拆分 | 收窄可接受的数据：与 OCCT（`Geom_BSplineCurve` 允许内部重数到 `degree + 1`）分道扬镳；重建时必须拆成多条 |
| B. 给 `Continuity` 加一个「不连续」底阶（如 `Discontinuous`），如实回答 | 保持 OCCT 兼容，接受任意重数 | 公共枚举与 `isCN()` 的语义都要重新解释；`isClosed()` / `reversedParameter()` / 任何遍历算法都要开始处理「曲线有洞」；而仓库里今天没有任何消费者需要它（`G8`） |
| C. 维持现状，在文档里警告 | 零改动 | 接口继续在合法输入上讲假话（`G4`），且 `isCN(0)` 与 `isValid()` 同时给出错误答案，比缺能力更糟 |

## 决定

采用 A。

`isClampedKnotVector()` 对两端要求 `degree + 1`，对内部要求不超过 `degree`。因为 `degree - m >= 0` 恒成立，
`Continuity` 阶梯不需要新增底阶，`C0` 的含义（各段交于一点）重新精确。

「一条曲线」因此有了可检查的定义：**参数域上的点集是连通的**。需要保留断点的调用方把它拆成两条曲线
（`TrimmedCurve` 与 `BSplineCurve` 都够用），这比让每个算法都学会处理洞更便宜（`G1`、`G10`）。

## 后果

- 好的：`isValid()` / `continuity()` / `isCN()` 三者重新互相一致；`knotContinuity()` 的 `degree - m >= 0`
  成了一个可以引用的事实，而不是一个被夹住的巧合。
- 好的：与既有取舍同向——`BSplineCurve` / `BSplineSurface` 本来只描述夹持、非周期的形式（`geometry-kinds.md` P5），
  「一段连通的多项式片」是同一条收窄的自然续写。
- 坏的 / 代价：与 OCCT 的接受范围不同。从 OCCT 或 STL 读进来的数据若带 `degree + 1` 内部重数，
  `isValid()` 会返回假，调用方必须拆分。这条差异写在头文件的 `@details` 与 P10 里。
- 触发条件（什么时候重开）：出现真正需要**一条曲线里有洞**的消费者（例如把整份 STEP 边表存成一个对象），
  届时按 B 方案加 `Continuity::Discontinuous` 并让遍历算法显式处理，而不是回到「夹成 C0」。

## 证据

- `src/base/geometry/sdk/vine/geometry/BSplineBasis.hpp`：`isClampedKnotVector()` 的新逐项上限与 `knotContinuity()` 的说明。 <!-- drift-ok -->
- `tests/test_geometry/GeometryKindsTest.cpp`：`aKnotVectorThatBreaksTheCurveIsNotACurve`——`{3, 3, 3}`（度 2）的曲线与
  曲面 `isValid()` 均为假；重数恰好等于度数的 `{3, 2, 3}` 仍为真且 `continuity() == C0`、`isCN(0)` 为真、`isCN(1)` 为假。
