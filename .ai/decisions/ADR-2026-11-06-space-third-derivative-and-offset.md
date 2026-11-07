# ADR-2026-11-06：空间曲线族答三阶导，`OffsetCurve` 成为可构造的值类型

- 状态：已采纳
- 日期：2026-11-06
- 取代：`ADR-2026-11-04-curve-third-derivative.md`
- 相关：`../design/geometry-kinds.md` 2.7 / 2.8 / D9、`ADR-2026-11-05-curve-derivative-capability.md`、`../design/geometry-planar.md` 2.13

## 背景

2026-11-04 的决定是“不给 `Curve<T>` 加三阶导数，`OffsetCurve` 因此保持抽象”。半年后的能力协议（`maxDerivativeOrder()` / `derivativeAt()`）改变了这个前提：三阶导可以只作为**具体类型的能力**存在，契约不必多一项纯虚。于是“偏置必须抽象”这个结论失效了——它原来成立的理由是“运行期基曲线问不出三阶导”。

同时平面侧已经在 2026-11-05 落地了“偏置持句柄、阶数 = 基曲线能力 − 1”的形态，空间侧若继续抽象，同一个概念在两个模块里形状不同（`G6`），且空间偏置不可构造、不能进族级扫查。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 维持现状（偏置抽象、曲线只答二阶） | 零改动 | 空间偏置不可构造；两个模块的同一个概念形状不同；裁剪片段也无法偏置 |
| B 给 `Curve<T>` 加纯虚 `thirdDerivative()` | 偏置直接能算 | 阶梯不闭合（偏置的 D3 要基曲线 D4）；所有实现者付账；与能力协议重复（`G8`） |
| C 具体类型加非虚 `thirdDerivative()` + 覆写 `maxDerivativeOrder()==3` / `derivativeAt()`；`OffsetCurve` 用能力判定并变具体 | 契约不变；阶梯由能力表达；两模块同形 | 每个能答三阶的类型多两个覆写；多一份三阶导实现 |
| D 只把 `OffsetCurve` 变具体，曲线族不答三阶 | 改动更小 | 没有基曲线能答三阶 ⇒ 偏置永远 `isValid() == false`，等于没实现 |

## 决定

采用 **C**。

- 具体类型新增非虚 `thirdDerivative(u)`，并覆写 `maxDerivativeOrder() == 3` 与 `derivativeAt(u, 3)`：`LineCurve`（零）、`CircleCurve`（`-P'`）、`EllipseCurve`（`-P'`）、`HyperbolaCurve`（`P'`）、`ParabolaCurve`（零）、`BezierCurve`（三阶差分 × `n(n-1)(n-2)`，有理形式加一层商法则）、`BSplineCurve`（基函数第三层 + 商法则）。`TrimmedCurve` 不变，已经向基曲线转发。
- `geom::spatial::OffsetCurve` 去掉纯虚的 `secondDerivative`：用 `N = e × p'`、`R = |N|`、`k = (N·N')/R²` 写成 `U'' = N''/R − 2 N' k/R + U (k² − k')`，`N'' = e × p'''` 来自 `basis_curve->derivativeAt(u, 3)`。
- `maxDerivativeOrder()` 报 `min(2, 基曲线能力 − 1)`（与平面侧同一条规则），`isValid()` 增加“基曲线能答三阶”这一条，`derivativeAt()` 在越界时答 `std::nullopt`。

判据：`G1`（阶数是能力，不是契约项）、`G4`（越界答空、能力决定 `isValid()`，不伪造零）、`G6`（2D / 3D 的 `OffsetCurve` 同形）、`G9`（新曲线只需报告能力）、`G10`（偏置二阶导只多一次虚调用，没有数值差分）。

## 后果

- 好的：`OffsetCurve` 可构造、可进族级扫查、可偏置裁剪片段；两个模块的偏置形状一致；“偏置第 n 阶要基曲线第 n+1 阶”成为可查询的事实；`secondDerivative` 是精确值而不是差分近似。
- 坏的 / 代价：每个能答三阶的类型多两个覆写与一份闭式实现；偏置套偏置仍是“位置 + 速度”视图（`isValid()` 为假、`maxDerivativeOrder() == 1`）；偏置仍不提供 `bounds()`。
- 触发条件（何时重开）：需要偏置的第三阶导（曲率变化率、G3 拼接）时，规则会自动降到“基曲线必须答 4”，届时再决定是给具体类型加第四阶还是让偏置停在那一步。

## 证据

- `src/base/geom/sdk/vine/geom/spatial/`：`LineCurve` / `CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` / `BezierCurve` / `BSplineCurve` 的 `thirdDerivative` 与能力覆写；`OffsetCurve` 的 `secondDerivative` / `maxDerivativeOrder` / `derivativeAt` / `isValid`。
- `tests/test_geom_spatial/GeometryKindsTest.cpp`：`static_assert(!std::is_abstract_v<OffsetCurve<double>>)`；`everySpaceCurveAnswersItsThirdDerivative`（八种类型的三阶导与二阶导的中心差分一致，含两种有理形式，并对照四条闭式关系）；`theSpaceOffsetAnswersASecondDerivativeExactly`（二阶导与一阶导差分一致、零距离等于基曲线）；`anOffsetOfAnOffsetStopsAtTheVelocity`（报 1、`isValid()` 为假、二阶导答空、位置仍精确）；`sweepCurves` 两次精度都包含偏置。
