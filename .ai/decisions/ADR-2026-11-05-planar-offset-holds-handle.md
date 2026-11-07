# ADR-2026-11-05：平面偏置持有基曲线句柄，阶数由能力决定

- 状态：已采纳
- 日期：2026-11-05
- 取代：`ADR-2026-11-05-planar-offset-curve.md`
- 相关：`../design/geometry-planar.md` 2.13、`ADR-2026-11-05-curve-derivative-capability.md`、`ADR-2026-11-05-planar-trimmed-curve.md`

## 背景

上一版把平面偏置做成 `OffsetCurve<C>`：模板参数是**具体**基曲线类型、按值持有。那个形状是被当时的契约逼出来的——偏置的二阶导需要基曲线的三阶导，而契约只声明到二阶，三阶导只作为具体类型的非虚成员存在，于是“运行期基曲线”根本问不出三阶导。

能力协议落地后，这个约束消失了：`Curve<T>&` 能回答 `maxDerivativeOrder()` 与 `derivativeAt(u, 3)`。继续用模板按值持有，等于保留一个不再必要的变通，并且让“偏置一个 `TrimmedCurve` / 偏置另一个偏置”这类合法组合只能靠模板实例化堆出来。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持 `OffsetCurve<C>` 按值模板 | 零分配、具体类型调用可内联 | 运行期基曲线不可用；`OffsetCurve<TrimmedCurve<T>>` 无法表达“任意基曲线”；与 3D 同名类型的形状不一致 |
| B `OffsetCurve<T>` 持 `shared_ptr<const Curve<T>>`，阶数按基曲线能力决定 | 与 3D 同形；任意基曲线；能力如实报告 | 一次堆分配；二阶导多一次虚调用 |
| C 两者并存（值模板 + 句柄视图） | 两种代价都能选 | 一个概念两个名字，规则两份（`G1`） |

## 决定

采用 **B**：`OffsetCurve<T>` 存 `shared_ptr<const Curve<T>>` 与有符号距离；`offsetCurve(basis, distance)` 工厂从具体类型造句柄。

阶数的规则是 `min(2, basis.maxDerivativeOrder() - 1)`：偏置的第 n 阶导需要基曲线的第 n+1 阶。基曲线答 3（每个平面具体类型都答 3）时偏置答 2；基曲线答 2（另一个偏置）时偏置答 1，`derivativeAt(u, 2)` 答 `std::nullopt`，且 `isValid()` 为假——基曲线给不出三阶导时，`secondDerivative()` 答零向量并如实标记为不可用，而不是编一个数。

判据：`G1`（偏置是“另一个对象加一个标量”，不是某种基曲线的特化）、`G4`（能力决定可用阶数，超出答空）、`G6`（与 `geom::spatial::OffsetCurve` 同形的基曲线句柄）、`G9`（新基曲线不新增偏置类型）、`G10`（一次分配；不再需要为每种基曲线各实例化一份偏置）。

## 后果

- 好的：可以偏置 `TrimmedCurve`、偏置偏置、偏置任意运行期曲线；2D 与 3D 的 `OffsetCurve` 现在是同一个形状（空间侧后来在 2026-11-06 与平面侧完全对齐，见 `ADR-2026-11-06-space-third-derivative-and-offset.md`）；“偏置的第 n 阶要基曲线第 n+1 阶”这条事实第一次能被查询，而不是靠注释。
- 坏的 / 代价：一次堆分配；`bounds()` 仍不提供；偏置一个偏置得到的是“位置 + 速度”视图（`isValid()` 为假），要二阶导就必须回到原始基曲线而不是叠加偏置。
- 迁移：`OffsetCurve<C> v(c, d)` → `auto v = offsetCurve(c, d)`；持有 `shared_ptr` 的调用方直接用 `OffsetCurve<T>(handle, d)`。

## 证据

- `src/base/geom/sdk/vine/geom/planar/OffsetCurve.hpp`：句柄基曲线、`maxDerivativeOrder()` 的 `min(2, basis - 1)`、`derivativeAt` 越界答空、`isValid()` 的能力判据。
- `tests/test_geom_planar/OffsetCurveTest.cpp`：直线 / 圆 / 椭圆 / 样条基曲线、二阶导与差分一致、零距离等于基曲线、尖点保持有限、裁剪片段作基曲线、族级算法接受偏置。
- `tests/test_geom_planar/CurveContractTest.cpp` 的 `TheDerivativeCapabilityReportsWhatEachTypeCanAnswer`：偏置报 2、`derivativeAt(u, 3)` 答空。
