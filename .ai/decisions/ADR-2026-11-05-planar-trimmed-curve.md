# ADR-2026-11-05：平面裁剪视图取代圆专用的 `ArcCurve`

- 状态：已采纳
- 日期：2026-11-05
- 取代：`ADR-2026-11-05-planar-arc-is-a-value-type.md`
- 相关：`../design/geometry-planar.md` 2.8 / 2.10 / D6、`ADR-2026-11-05-curve-derivative-capability.md`

## 背景

`ArcCurve` 是“圆按值 + 角度区间”：零分配、盒是紧盒，但只能裁剪圆。原 ADR 明确写了触发条件——一旦要裁剪非圆基曲线（椭圆弧、样条修剪、导入轮廓截断），就引入通用 `TrimmedCurve` 并**删除** `ArcCurve`，不与之并存。这条触发条件现在由设计本身满足：能力协议（`maxDerivativeOrder()` / `derivativeAt()`）落地后，类型擦除的裁剪视图可以如实转发任意阶导，不再需要“基曲线必须是具体类型”这个前提。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保留 `ArcCurve`，另加通用视图 | 圆的片段保持零分配与紧盒 | 两个类型回答同一个概念（“一条曲线的一段”），闭合 / 周期 / 反向 / 越界四条规则各写一遍并会漂移 |
| B 通用 `TrimmedCurve<T>` 持 `shared_ptr<const Curve<T>>`，删除 `ArcCurve` | 与 3D 同名同形；任何基曲线；一份规则 | 每次构造一次堆分配（R2 的第二类例外）；片段紧盒不再由裁剪视图回答 |
| C 通用 `TrimmedCurve<C>` 按值持基曲线 | 零分配，且基曲线有 `bounds()` 时可转发 | 仍是模板参数，运行期句柄不可用；同一概念在 2D（按值）与 3D（句柄）形状不同 |

## 决定

采用 **B**：`TrimmedCurve<T>` 存 `std::shared_ptr<const Curve<T>>` 加两个参数界，`trimCurve(basis, first, last)` 工厂从具体类型造句柄；`ArcCurve` 与其测试文件**删除**。

丢失的能力按 `G13` 放回它该在的地方：一段圆的紧盒是圆的支撑函数在区间上的结果，所以它是 `CircleCurve::bounds(first_parameter, last_parameter)`，不是裁剪视图的成员——擦除后的视图问不出支撑函数，硬留在视图上只能返回松盒。

判据：`G1`（一个概念一个表达）、`G6`（2D 与 3D 的裁剪同名同形）、`G9`（新基曲线不必新类型）、`G10`（一次分配换掉一整套重复规则；角度区间在圆上仍可用 `bounds(first, last)` 精确回答）。

## 后果

- 好的：椭圆弧、样条修剪、轮廓截断都能用同一个类型；闭合 / 周期 / 反向 / 越界四条规则只有一份实现与一套测试；`OffsetCurve` 可以直接偏置一个片段。
- 坏的 / 代价：`TrimmedCurve` 成为 R2 的第二类例外（持有动态内存、深拷贝语义），构造一次分配；片段紧盒只对圆可用；`ArcCurve` 的公开面被删除（模块内唯一消费者是测试，无 ABI 负担）。
- 迁移：`ArcCurve(circle, a, b)` → `trimCurve(circle, a, b)`；`arc.bounds()` → `circle.bounds(a, b)`；`arc.circle` → 基曲线句柄或原 `CircleCurve` 值。

## 证据

- `src/base/geom/sdk/vine/geom/planar/TrimmedCurve.hpp`、`CircleCurve.hpp` 的 `bounds(first, last)`。
- `tests/test_geom_planar/TrimmedCurveTest.cpp`：走圆、整圈闭合、越界不闭合、区间内反向、反向区间无效、退化基曲线、域外转发、经 `Curve<float>&` 分派、裁剪 Bezier、能力转发（圆 3 / 偏置 2）、空句柄、裁剪套裁剪。
- `tests/test_geom_planar/BoundsTest.cpp`：一段圆的紧盒与反向区间同盒。
