# ADR-2026-11-04：曲面的框架是值类型 `Placement`，不是继承层

> **状态**：已采纳（取代 `ADR-2026-11-04-axis-surface-frame.md` 的 C 选项）
> **日期**：2026-11-04
> **相关**：`../design/geometry-axis-surfaces.md` 2.1 / 2.5 / C1 / D1；`src/base/geometry/sdk/vine/geometry/Placement.hpp`、`Frame.hpp`、`CylindricalSurface.hpp`、`ConicalSurface.hpp`、`ToroidalSurface.hpp`；`tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## 背景

`ADR-2026-11-04-axis-surface-frame.md` 把旋转面的框架（原点 / 主方向 / X 方向 + `yAxis()` + `continuity() = CN`）放进新的中间层 `AxisSurface`，理由是不想让三个曲面各存一份会漂移的规则。

落地后复查发现：真正被共享的只有三样小东西——三个字段的初始化（归一化 + `inPlaneAxis`）、`yAxis()` 的一行叉乘、以及 `return Continuity::CN`。为此在类型树里加一层“状态层”，读者要区分“标签（`ElementarySurface`）”与“状态层（`AxisSurface`）”，且类型树与 OCCT 的形状不一致（OCCT 是 `Geom_ElementarySurface` → 五个具体类，放置 `gp_Ax3` 在标签里）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 保持 `AxisSurface` 继承层 | 三样东西共享一处 | 多一层类型；为一组字段与一个 `return CN` 付继承的账（`G8`）；“标签 vs 状态层”要解释 |
| B. 每型各存三个字段 + 自由函数 | 类型树与 OCCT 同形 | 同一段初始化抄三遍（规则仍在 `Frame.hpp`，抄的是调用） |
| C. `Placement<T>` 值类型 + 成员组合 | 规则与字段一个家；树形与 OCCT 同形；`Placement` 可复用于未来的 `SurfaceOfRevolution`、B-rep 的面放置，甚至 `ConicCurve` | 多一个公开值类型；`continuity()` 写三份 `return CN`；字段访问变成 `surface.placement.*` |

## 决定

采用 C。

`Placement<T>`（`origin` + `axis` + `x_direction` + `yAxis()` + `hasUsableAxis()`，恒右手、不带手性开关）住在 `Placement.hpp`；三个曲面各持一个 `placement` 字段，`continuity()` 各自 `return Continuity::CN`。

## 后果

- 好的：框架有了名字，成为一个可复用的值类型（`gp_Ax3` 的恒右手版）；类型树与 OCCT 一致；`dynamic_cast<AxisSurface*>` 不再需要——拿到具体曲面直接读 `placement`；`ElementarySurface` 保持空标签（`C9`）。
- 坏的 / 代价：`continuity()` 三份（刻意的取舍：为共享一行 `return` 而加基类属于过度设计）；多一个公开类型；`origin` / `axis` / `x_direction` 的访问路径变为 `surface.placement.*`；拿着 `ElementarySurface<T>&` 的调用方问不到放置（真出现这种消费者时先考虑 `dynamic_cast`，再谈扩接口）。
- 触发条件（什么时候重开）：`Placement` 需要表达镜像（届时加手性位，或退回“构造镜像”的不可变规则）；或第四个旋转类曲面出现、三份 `continuity()` 变得刺眼（届时优先考虑 mixin，而不是重新引入状态层）。

## 证据

- `src/base/geometry/sdk/vine/geometry/Placement.hpp`；三个曲面各持 `Placement<T> placement;`。 <!-- drift-ok -->
- `src/base/geometry/src/Instantiations.cpp`：`Placement<float>` / `Placement<double>` 两行实例化；`AxisSurface` 两行已删除。 <!-- drift-ok -->
- `tests/test_geometry/GeometryKindsTest.cpp`：`aPlacementCompletesAndNormalisesItsFrame`（默认框架、非单位首选方向的投影、退化轴回退）、`static_assert(sizeof(Placement<double>) == 3 * sizeof(Point3d))`、`is_trivially_copyable_v<Placement<double>>`。 <!-- drift-ok -->
