# ADR-2026-11-04：旋转面的框架放在新的 `AxisSurface` 层，平面内轴的规则抽成 `Frame.hpp`

> **状态**：已取代（被 `ADR-2026-11-04-surface-placement-value-type.md` 取代：框架改为值类型 `Placement`，不再是一层继承）
> **日期**：2026-11-04
> **相关**：`../design/geometry-axis-surfaces.md` 2.1 / 2.2 / 2.3 / C1 / D1、`../design/geometry-kinds.md` 2.9；`src/base/geometry/sdk/vine/geometry/AxisSurface.hpp`、`Frame.hpp`、`CylindricalSurface.hpp`、`ConicalSurface.hpp`、`ToroidalSurface.hpp`、`ConicCurve.hpp`、`Plane.hpp`；`tests/test_geometry/GeometryKindsTest.cpp` <!-- drift-ok -->

## 背景

圆柱、圆锥、圆环三个曲面此前是占位（类型在树里、不可构造、无参数）。要把它们实现，先要回答两个问题：

1. **放置存在哪里**。这三个曲面都绕一根任意轴旋转，而现有曲面里 `Plane` 不存面内框架（两轴由 `normal` 推导），`SphericalSurface` 只有圆心和半径（世界 Z 轴）。按 `C9` / `C1`，`ElementarySurface` 是**分类标签**，不能承载状态或虚函数。
2. **“把一个首选方向投到与法向正交的平面里”这条规则放在哪里**。它此前有两份拷贝：`ConicCurve::inPlaneAxis()`（曲线族，protected static）与 `Plane::xAxis()`（同一规则、无首选方向的版本）。

OCCT 的答案是前者：`Geom_ElementarySurface` 持有一个 `gp_Ax3 pos`，五个初等曲面（平面、圆柱、圆锥、球、圆环）都带放置，并统一暴露 `Axis()` / `Location()` / `Position()` / `SetPosition()`；它没有 `AxisSurface` 这样的中间层（另一个带框架的分支是 `Geom_SweptSurface` 的 `basisCurve` + `direction`）。本 ADR 要决定的正是要不要照它来。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 每个曲面各存 `origin` / `axis` / `x_direction` 并各自归一化 | 不动现有类型树 | 三份可各自漂移的框架规则（`G1`）；`continuity() = CN` 也要写三份 |
| B. 把框架放进 `ElementarySurface`（**OCCT 的做法**） | 一个家，并且对任意初等曲面都能问 `Position()` | 违反 `C9`：标签从此带状态；`Plane` / `SphericalSurface` 也会被拖进这组字段（`G8`），而 `Plane` 在拾取热路径上按值拷贝（`G10`） |
| C. 新增中间层 `AxisSurface<T> : ElementarySurface<T>` 持框架，三个曲面只剩标量 | 框架与 `continuity()` 一个家，与曲线族的 `ConicCurve` 同构 | 类型树多一层；三个曲面各多三个向量 |
| D. 同时把 `SphericalSurface` 迁到 `AxisSurface` | 曲面族框架完全统一 | 球的轴只改变参数化、不改变点集，迁移是破坏性变更而没有消费者（`G8`），且现有 `u` 从 +X 量起的行为会变 |

## 决定

采用 **C**，并同时：

- 把“平面内轴”抽成 `Frame.hpp` 的 `inPlaneAxis(preferred, normal)`，`ConicCurve`（首选方向）、`Plane::xAxis()`（首选方向为零）、`AxisSurface` 三处共用；
- `AxisSurface` 提供 `yAxis() = axis × x_direction` 与 `continuity() = CN`，三个曲面只实现求值、导数、参数域与自己的 `isValid()`；
- `SphericalSurface` 不迁移（选项 D），判据写成一句：**放置改变点集就存，只改变参数化就推导**。

## 后果

- 好的：新增一个旋转面只需一个标量 + 求值公式；框架归一化与 `CN` 只有一处（`G1`、`G6`）；`Plane` 与 `ConicCurve` 的投影规则不再各自维护。
- 坏的 / 代价：`AxisSurface` 是状态层而不是标签，读者要区分“标签（`ElementarySurface` / `BoundedSurface`）”与“框架层（`AxisSurface`）”；三个曲面各多 3 个向量；`ConicCurve::inPlaneAxis` 这个 protected static 被删除，派生圆锥曲线从此调用自由函数。
- 坏的 / 代价（相对 OCCT）：失去了 `Geom_ElementarySurface::Position()` 那种统一访问器；今天要问就 `dynamic_cast<const AxisSurface<T>*>`（与 D6 “vtable 回答我是什么”一致），等真出现“拿着不认识的初等曲面要它的轴”的消费者时再谈扩接口。
- 触发条件（什么时候重开）：出现需要“带放置的球面”（例如 B-rep 里任意朝向的球面面片）时，把 `SphericalSurface` 迁到 `AxisSurface` 并接受参数起点的变化。

## 证据

- `AxisSurface.hpp`：框架、`yAxis()`、`continuity()`、`hasUsableAxis()`。 <!-- drift-ok -->
- `Frame.hpp`：`inPlaneAxis` 的唯一实现；`ConicCurve.hpp` / `Plane.hpp` 改为调用它。
- `tests/test_geometry/GeometryKindsTest.cpp`：`aCylindricalSurfaceWrapsItsAxis`、`aConicalSurfaceWalksItsGeneratrix`、`aToroidalSurfaceClosesInBothDirections`，以及静态断言 `is_base_of_v<AxisSurface<double>, …>`、`sizeof(CylindricalSurfaced)` 等三项大小断言。 <!-- drift-ok -->
- OCCT `Geom_CylindricalSurface` / `Geom_ConicalSurface` / `Geom_ToroidalSurface` 与 `ElSLib::CylinderValue` / `ConeValue` / `TorusValue`：参数化约定的来源（圆锥的 `v` 是母线弧长，圆环的 `v` 从赤道面量起）。
