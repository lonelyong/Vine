# ADR-2026-11-09：解析实体的封闭集合不再用 variant 表达

> **状态**：已采纳
> **日期**：2026-11-09
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial/SolidShape.hpp`（删除）、`BoxSolid.hpp` … `EllipsoidSolid.hpp`、 <!-- drift-ok -->
> `src/base/geom/sdk/vine/geom/spatial/SolidTessellation.hpp`、`src/base/models/sdk/vine/models/PrimitiveDispatch.hpp`、
> `src/viz/graphics/src/Geometry.cpp`、`tests/test_geom_spatial/SolidTest.cpp`、`GeometryKindsTest.cpp`

## 背景

`ADR-2026-11-05-solid-family-and-variant.md` 引入 `SolidShape<T> = std::variant<五个 kind>`。它当时的用途有两条：
`Primitive` 按值持有它（已由 `ADR-2026-11-05-primitive-is-a-modelling-base.md` 撤回），以及作为“按 kind 作答”的入参。

到本轮之前，剩下的读者只有三处：`tessellateSolid()` 的入参、`solidShapeOf()` 的返回值、`solidOf()` 的族视图
（生产代码只用它做一次 `isValid()`）。`ADR-2026-11-09-solid-tessellation-in-geom.md` 把三角化搬到 `geom` 之后，
入参也可以直接是具体 kind —— 于是 `SolidShape` 不再承担任何“存储”或“分发”的独有职责：

- `solidShapeOf()` 与 `visitPrimitive()` 是同一件事的两种写法（把 `Primitive` 变成按 kind 作答），同时存在只会让
  调用方二选一；
- `solidOf()` 只服务一次 `isValid()`，而每个 kind 自己就有 `isValid()`。

## 决定

1. 删除 `SolidShape` 那个变体类型（`SolidShape<T>`、`SolidShapef` / `SolidShaped` 与 `solidOf()` 一并移除）。 <!-- drift-ok -->
2. 需要按 kind 作答的几何入口写成模板重载：`tessellateSolid(const BoxSolid<T>&, …)` … `(const EllipsoidSolid<T>&, …)`。
3. 删除 `models::solidShapeOf()`；模型 → 几何只剩 `visitPrimitive()`：visitor 里拿具体子类，调它的 `solid()` 与对应重载。
4. `graphics::geometryFromShape()` 改为 `visitPrimitive` + `tessellateSolid` + `meshFromTriangulation()`。

## 理由

- **`G1`**：一个概念一个定义。“哪些解析实体、怎么按 kind 分派”只有一个答案：五个 kind + `visitPrimitive()`。
- **`G8`**：变体今天零存储消费者（模型不存、几何不返）；保留它只是为“将来可能要一个能存的值”预建体系。
- **`G4`**：`solidShapeOf()` 与 `visitPrimitive()` 并存时，调用方必须先知道该用哪个；删除后只有一条路。
- **`G6`**：几何值类型一贯是 `Xxx<T>` + 一 kind 一个重载，`std::variant` 在这里是唯一的例外。
- **穷尽性不丢**：新增 kind 时 `visitPrimitive()` 的 visitor 会调用一个不存在的 `tessellateSolid` 重载，编译错误仍在，
  只是从 `std::visit` 的实例化点移到调用点 —— `brep` 的 `solidOf()` 重载早就用同一个模式。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 保留 variant，只删 `solidShapeOf()` | 变体仍只剩三角化一个消费者，且 `tessellateSolid` 只能收它；等于为一次调用保留一个公开类型（`G8`）。 |
| `tessellateSolid(const Solid<T>&)` + 虚 `kind()` / 虚 `tessellate()` | 铺每种 kind 需要自己的量纲；虚接口会让新 kind 静默漏掉，且把网格知识推进族接口（`G4`、`G9`）。 |
| 让 `Primitive` 暴露一个变体（恢复 `shape()`） | 尺寸会出现在两个家（模型字段与变体），即 `ADR-2026-11-05-primitive-is-a-modelling-base.md` 已否决的形态。 |

## 后果

- 公共 API 删除：`SolidShape.hpp` 整个头文件、`solidShapeOf()`；三角化入口由一个变成五个模板重载。 <!-- drift-ok -->
- 测试：`SolidTest` 的变体断言与 `solidShapeOf` 用例删除，`visitPrimitive` 用例保留并加强；
  `GeometryKindsTest` 只保留“两种精度的 kind 是不同类型”这条断言。
- 若将来真需要一个“可存储的解析实体值”（例如文件格式里的一列），那时再加一个只做存储的类型，
  而不是把分派也塞进去。
