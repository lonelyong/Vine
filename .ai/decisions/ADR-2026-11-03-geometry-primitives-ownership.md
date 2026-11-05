# ADR-2026-11-03：空间值类型归 geometry，轴对齐盒改名为 Aabb

> **状态**：已采纳
> **日期**：2026-11-03
> **影响范围**：`src/base/math`、`src/base/geometry`、`src/viz/graphics`、`tests/test_math`、`tests/test_geometry` <!-- drift-ok -->

## 背景

`Line`、`Plane`、`Rect3`（轴对齐盒）住在 `src/base/math`，而同一概念族的 `Obb` 在新一轮几何工作中落到了 `src/base/geometry`。`Rect3` 同时拥有 `math::Aabb` / `Aabbf` / `Aabbd` 别名、`geometry/Aabb.hpp` 转发 shim 和 `Mesh::Aabb` 第三个拼写：一个轴对齐盒三个家。 <!-- drift-ok -->

`Rect3` 的消费者全部在 `geometry` 之上（`Mesh`、`graphics::Node`、`ChainBuilder`、`ChainPresets`、`Obb::toAabb`），`math` 内部没有任何使用者。`Line` 的唯一使用者是 `Plane`；`Plane` 没有模块外消费者。

## 决定

1. `Line`、`Plane` 与轴对齐盒的类型定义移入 `vine::geometry`。
2. 轴对齐盒的类型名改为 `Aabb`；`Rect3` 与 `math::Aabb*` 不再存在。
3. `Rect2` 留在 `vine::math`，不迁移。
4. 不设 `math` 侧的兼容别名或转发头。

## 理由

- 判据可以一句话判断：`math` 只有数值与变换，`geometry` 有空间实体。`G1`、`G5`。
- 类型与消费者同侧，`Aabb` 与 `Obb` 同模块、同名族，`Plane` 的 `Line` 返回值不再跨模块。`G1`、`G6`、`G12`。
- `Rect2` 的唯一消费者是 `appfw::Gui` 的布局矩形；搬进去会让 UI 词汇进入几何模块，并给 `appfw` 公共头加一条 `geometry` 依赖。`G1`、`G5`、`G8`。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 保留 `Rect3` 作为类型名 | 3D “Rect” 是误名，且与 `Obb` 不成对；别名已证明会漂移成三份。 |
| 在 `math/Rect3.hpp` 留 `using Rect3 = geometry::Aabb` | `math` 会反向依赖 `geometry`，形成回边（`G2`）；一个版本的过渡期不值得一条回边。 |
| 把 `Rect2` 一起搬进 `geometry` | 消费者是 GUI 布局矩形，不是形状；为对称引入无关职责。 |
| 只改名字不搬模块 | `Obb` 与 `Aabb` 永久分居两个模块，`toAabb()` 继续跨模块取类型。 |

## 后果

- 所有 `vn::math::Rect3*` / `vn::math::Aabb*` 引用一次性改到 `vn::geometry::Aabb*`；这是破坏性变更，仓库内已同步修改，未保留过渡别名。
- 只想要一个包围盒的调用方需要链 `vn::Geometry`（+ `vn::Core`）。今天的消费者都已链它，实际没有新增依赖边。
- `Aabb` 的模板实例化随源文件搬进 `geometry`，导出宏改为 `VN_GEOMETRY_API`。
- 现行设计记录在 `.ai/design/geometry-primitives.md`。
