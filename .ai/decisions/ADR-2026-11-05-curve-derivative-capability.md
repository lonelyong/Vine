# ADR-2026-11-05：曲线导数按“能力”暴露，而不是把某一阶写进契约

- 状态：已采纳
- 日期：2026-11-05
- 相关：`../design/geometry-kinds.md` 2.7、`../design/geometry-planar.md` 2.3 / 2.13、`ADR-2026-11-04-curve-third-derivative.md`、`ADR-2026-11-05-planar-offset-curve.md`

## 背景

偏置曲线 `O = C + d·N` 的二阶导需要基曲线的三阶导。契约此前只声明到二阶，于是运行期拿到 `Curve<T>&` 时**没有**办法取得三阶导；两条既有 ADR 分别选择了“偏置是具体类型的模板”和“空间侧偏置保持抽象”。

P1/P2 要的是运行期基曲线。两条边界路线都不成立：

- 契约只到二阶：运行期基拿不到三阶导，偏置只能退化为模板或擦除；
- 给契约加第 12 项**纯虚** `thirdDerivative()`：`OffsetCurve` 自己也是 `Curve<T>`，它的三阶导需要基曲线四阶导，于是义务永远差一阶；只能靠异常或截断（OCCT 的 `DN` + `NotImplemented`）。

真正的问题不是“要不要某一阶”，而是**导数阶数应该是算法的参数、对象的能力**，而不是类型身份的一部分。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 维持现状（只到二阶） | 零改动 | 运行期基无法精确偏置；P2 只能模板或擦除 |
| B 加纯虚 `thirdDerivative()` | 偏置直接持句柄；与 OCCT `D3` 对齐 | 阶梯不闭合（偏置的 D3 要基 D4）；每个类型都付账；超出范围只能异常/截断 |
| C 能力协议：`maxDerivativeOrder()` + `derivativeAt(u, order)`（默认实现一、二阶，有三阶导的类型覆写） | 阶数是参数；运行期基可问任意可达阶；超出范围答 `std::nullopt`；两个模块同形 | 多两个非纯虚成员；默认实现二阶以内多一次间接 |
| D 任意阶 `DN(N)`（OCCT 形态） | 最通用 | 接口膨胀；实现上限与异常语义；与本仓库“有限、正确、无异常”的约定冲突 |

## 决定

采用 **C**。

判据：`G1`（阶数是算法参数，不是类型身份）、`G4`（能力显式查询，超出能力答空而不是伪造零）、`G6`（2D / 3D 同名同义）、`G9`（新类型只需报告能力，不必改契约）、`G10`（无分配；默认实现只多一次虚调用）。

它同时给 P2 的目标形态提供了前提：偏置视图在构造时查询基曲线 `maxDerivativeOrder() >= 3`，不满足就给构造期错误——没有异常、没有截断、也没有“差一阶”的漏洞。

## 后果

- 好的：`Curve<T>` 仍是十一项核心纯虚 + **一对非纯虚能力成员**；`derivativeAt` 覆盖一、二阶，带三阶闭式解的类型覆写为 3；2D 与 3D 的增补逐项同义；运行期基第一次可以精确回答“我能到几阶”。
- 坏的 / 代价：`derivativeAt` 的默认实现经由 `derivative` / `secondDerivative` 两个虚函数，二阶以内多一次间接；目标形态是以 `derivativeAt` 为原语、另两个作为非虚便利包装，这一步登记为后续迁移，不与本 ADR 一起做。
- 触发条件（何时重开）：把裁剪 / 偏置改成不可变共享句柄视图、以及构造期 `expected` 验证，仍由 `../research/` 的目标架构与 backlog 跟踪；本 ADR 只解决“如何问任意阶导”。

## 证据

- `src/base/geom/sdk/vine/geom/planar/Curve.hpp`、`src/base/geom/sdk/vine/geom/spatial/Curve.hpp`：能力协议的声明与默认实现。
- `tests/test_geom_planar/CurveContractTest.cpp`：九个类型的一、二阶一致性，以及“`derivativeAt(3)` 是否可达”与 `maxDerivativeOrder()` 的一致。
- `tests/test_geom_spatial/GeometryKindsTest.cpp`：族级扫查对能力的检查。
- 对照：OCCT `Geom2d_Curve::EvalD0..EvalDN`（阶数在接口上，上限 3）；本决定保留“按需询问 + 超出答空”，不引入异常与截断。
