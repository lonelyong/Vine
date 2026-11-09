# ADR-2026-11-09：解析实体的三角化回到 geom，结果是顶点数组而不是网格

> **状态**：已采纳
> **日期**：2026-11-09
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial/SolidTessellation.hpp`（新增）、`src/base/geom/src/SolidTessellation.cpp`（新增）、
> `src/base/models/sdk/vine/models/SolidTessellation.hpp`、`src/viz/graphics/src/Geometry.cpp`、
> `tests/test_geom_spatial/SolidTessellationTest.cpp`

## 背景

`tessellateSolid()` 出生在 `src/base/geometry/src/`（提交 `79058532`）。`9be3b6f4`（“modelling leaves geometry”）把它 <!-- drift-ok -->
连同 `TriangleMesh` 一起搬进 `src/base/models`，于是 `geometry-tessellation.md` 的 2.1 至今写着“在 geometry 的 `src/`
里实现”，与实际路径矛盾。

搬迁之后，这个函数**唯一**的建模依赖是私有 `MeshBuilder::finish()` 里的四行：构造 `TriangleMesh`、把三个数组设进去、
`computeAabb()`。前面的 250 行只碰 `SolidShapef`、五个 kind、`Vec3f` 与 `std::vector`。也就是说，它住在 `models` 是因为
**返回类型**是建模类型，而不是因为算法需要建模。

同时，唯一的生产消费者 `graphics::geometryFromShape()` 拿到网格后立刻把顶点重新打包成 `Geometry` 的 `Buffer<float>`，
中间那个 `TriangleMesh` 对这条路径只是中转。

## 决定

1. 三角化回到 `geom`：`vn::geom::spatial::tessellateSolid()`，与五个 kind 同目录
   （`geom/spatial/SolidTessellation.hpp`），实现在 `src/base/geom/src/SolidTessellation.cpp`，一 kind 一个重载。
2. 结果是几何值 `vn::geom::spatial::Triangulation<T>`：`positions` / `normals` / `indices` 三个 `std::vector` 公开字段，
   加 `vertexCount()` / `triangleCount()` / `empty()`，别名 `Triangulationf` / `Triangulationd`。精度是模板参数 `T`，
   跟着实体走；两种精度在同一个 `.cpp` 里显式实例化并导出，与其它几何值类型同一条规矩。不可用实体返回空值。
3. `TessellationQuality` 随之上移。模型侧只保留装配：
   `vn::models::meshFromTriangulation(Triangulationf)` 把三个数组 move 进 `TriangleMesh` 并算包围盒，空值返回空网格。
4. 调用链变成 `meshFromTriangulation(tessellateSolid(shape.solid()))`（`shape` 来自 `visitPrimitive()`），`graphics` 侧语义不变。

## 理由

- **`G2`**：几何算法只依赖几何与 core（`Geom` 链接 `Math + Core`，`Buffer` / `intrusive_ptr` 在 core）。把
  `TriangleMesh` 写进返回类型会在 `geom` 与 `models` 之间造一条回边；而 `TriangleMesh` 是建模类型，不能下移。
- **`G8` / `G10`**：为什么不返回 `Buffer`。`Mesh::setPositions()` 只收 `std::vector`，返回
  `intrusive_ptr<Buffer<T>>` 会让模型侧多一次拷贝；今天也没有第二个消费者需要 Buffer 形式（唯一的 `graphics`
  路径本来就要重新打包）。值类型让模型侧 `std::move` 零拷贝。
- **`G1` / `G13`**：结果值不持有身份、revision 或面拓扑，只描述“这次铺出了哪些三角形”，与实体的建模身份分开。
- **`G6`**：`brep::tessellate()` 仍返回 `TriangleMesh` —— 它位于 `models` 之上，可以命名模型网格；两条三角化的结果
  不同是层次不同，而不是同一件事两种写法。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 保持现状（`models::tessellateSolid` 返回 `TriangleMesh`） | 文档与代码已经矛盾；且它让一个纯数学算法为返回类型背上整个建模模块。 |
| 移到 geom，仍返回 `TriangleMesh` | `geom → models` 回边（`G2`）。 |
| 移到 geom，返回三个 `intrusive_ptr<Buffer<T>>` | 模型侧 `Mesh::setPositions()` 只收 `std::vector`，反而多一次拷贝；今天没有 Buffer 形式的消费者（`G8`、`G10`）。 |
| 移到 geom，用三个 `std::vector&` 出参 | 名称与所有权都靠约定，`G4` 不合格；空结果的表示也不清楚。 |

## 后果

- 公共签名变化：`TessellationQuality` / `Triangulation<T>` 从 `vn::models` 移到 `vn::geom::spatial`；
  `models::tessellateSolid()` 被 `geom::spatial::tessellateSolid()` + `models::meshFromTriangulation()` 取代。
  仓库内调用点只有 `graphics::geometryFromShape()` 与一个测试文件，均已更新。
- 入口按 kind 重载、按精度模板化：variant 入参的删除记在 `ADR-2026-11-09-solid-closed-set-without-variant.md`。
- `geom` 新增一个 `.cpp`（`vn_add_library` 用 glob，需要重跑 `cmake -S . -B build`）。
- 测试改为断言 `Triangulation` 的三个数组，另用一条用例覆盖模型侧装配。
- 若将来渲染路径要省掉 `packAttribute` 的一次拷贝，应让 `geometryFromShape()` 直接吃 `Triangulation`，
  而不是把 `Buffer` 形式塞回几何入口。
