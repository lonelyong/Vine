# ADR-2026-11-09：盒的变换算子回到 geom，`Obb` 暂不提供

> **状态**：已采纳
> **日期**：2026-11-09
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial/Aabb.hpp`、`src/viz/graphics/sdk/vine/graphics/Node.hpp`、
> `src/viz/graphics/src/{Node,Geometry,Scene,RayIntersection}.cpp`、`tests/test_geom_spatial/AabbTest.cpp`、
> `tests/test_graphics/GraphicsTest.cpp`

## 背景

“把一个局部空间的轴对齐盒放进累积矩阵的坐标系”在 `graphics` 里有两份实现：
`graphics::transformBox()`（`Node.hpp` 里的公开 API）与 `RayIntersection.cpp` 的私有 `transformBounds()`；
调用点六个（`Geometry` 的实例盒与世界盒、`Group` 的子树并集、`Scene` 的边界缓存、拾取的拒绝盒）。
`geometry-primitives.md` 9.2 早已登记这条缺口：“类型已经归位，变换算子还在消费者一侧”。

## 决定

1. `Aabb<T>::transformed(const math::Matrix4x4<T>&)` 进 `geom`：八个角点变换后重拟合；只对仿射矩阵有定义，
   空盒或非仿射输入答空盒；`T` 限浮点（整数盒乘矩阵没有定义好的舍入）。
2. 删除 `graphics::transformBox()` 与 `RayIntersection.cpp` 的 `transformBounds()`；六个调用点改为
   `box.transformed(matrix)`；**不留转发别名**。
3. `Obb::transformed` 暂不提供；缺口与触发条件记在 `geometry-primitives.md` 9.2。

## 理由

- **`G1` / `G7` / `G13`**：这是值类型自己的几何事实，不是渲染词汇。一个算子两个家，改动必然漂移。
- **不留转发**：仓库惯例是一次改完、不留兼容别名（`graphics::Ray` 同样处理过）；留下 `transformBox()`
  只会在过渡期里给同一个操作两个名字（`G4`）。
- **`Obb` 的表示**：它存 center + 半长 + 四元数，只对刚体（及均匀缩放）精确——一般仿射的像是平行六面体，
  这个表示装不下。今天 `Obb` 零生产消费者，先定语义再动手（`G8`）。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 留 `graphics::transformBox()` 作为一行转发 | 同一操作两个名字（`G4`）；仓库对旧名字的处理是一次删完。 |
| 返回 `Obb<T>`（精确像） | 场景盒与拾取拒绝盒要的是界；精确像零消费者。 |
| 返回 `std::optional<Aabb<T>>` | 空盒已经是“没有盒”的表示，外面再套一层不带信息。 |
| 同时加 `Obb::transformed` | 零生产消费者，且非刚体变换下的语义还没定（`G8`）。 |
| 自由函数 `spatial::transformBox(aabb, m)` | 与值类型的既有风格不一致；`intersectTriangle` 是自由函数只因为三角形不是值类型。 |

## 后果

- `Aabb.hpp` 新增对 `math/Transform3.hpp` 的依赖（矩阵 × 点运算符的唯一定义）。编译耦合从“点 / 向量”升到
  “矩阵”，但 `Aabb` 早已 include `Ray.hpp`，这不是新性质。
- 公共 API 删除 `VN_GRAPHICS_API transformBox`；仓库内调用点只有 `graphics` 自己与一个测试，已全部改完。
- 非仿射矩阵从“静默算出一个错盒”变成“答空盒”，并由 `AabbTest.cpp` 的用例盯住。
- `Node.hpp` 里原本描述 `transformBox()` 的段落删除；`GraphicsTest` 改成
  `geom->localBounds().transformed(geom->worldMatrix())`，仍然钉住“走查与自答一致”。
