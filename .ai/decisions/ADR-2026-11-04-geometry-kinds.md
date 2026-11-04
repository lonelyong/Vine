# ADR-2026-11-04：几何族用空标签分类，不用虚基类

> **状态**：已采纳
> **日期**：2026-11-04
> **影响范围**：`src/base/geometry/sdk/vine/geometry`（新增 `Curve.hpp`、`Surface.hpp`、`SphericalSurface.hpp`、`LineSegment.hpp`、`Circle.hpp`；既有类型声明归属）、`tests/test_geometry`

## 背景

几何类型之间没有分类关系："一条曲线 / 一个曲面 / 一个实体"只在讨论里存在，代码里既不能按族约束，也不能在编译期回答"这是几维的"。运行期分类只有 `ShapeType`，而它只覆盖 `Shape` 子类。

同一天刚落地的两项改动把这个决定的代价变得具体：

- `Aabb` / `Obb` / `Line` / `Plane` / `Ray` 的短查询从 `.cpp` 搬进头文件，跨模块内联。
- 逐三角形射线测试实测 7.09 → 5.99 ns（命中）、3.96 → 2.71 ns（平行剔除，`geometry-ray.md` 2.7）。

## 决定

1. `Curve` / `Surface` 是**空标签**：无状态、无虚函数，只带 `static constexpr int kDimension`（1/2）。
2. 具体类型公开派生：`Line` / `Ray` / `LineSegment` / `Circle` → `Curve`；`Surface` 下再分 `ElementarySurface`（`Plane`、`SphericalSurface`，另三个占位）与 `BoundedSurface`（`Triangle`、`Quad`，另三个占位）。
3. 族归属只在类型定义处声明一次；参数的含义留在具体类型（`Ray` 是距离、`LineSegment` 是 [0,1]、`Circle` 是弧度）。
4. 新增 `LineSegment` / `Circle` / `Triangle` / `Quad` 四个值类型，只提供 `isValid()` / `pointAt(...)` / `bounds()` 与各自的量（`length`、`circumference`、`normal`、`direction`）。
5. 运行期多态仍用 `std::variant` + `std::visit`（`Primitive::Parameters` 是既有先例）。
6. 零成本由测试守：`static_assert` 断言归属、非归属、`sizeof` 不变与 trivially copyable。

## 理由

- `G10`：虚基类会给 `Ray`、`Line`、`Box` 各加一个 vptr，破坏按值拷贝与跨模块内联，把刚测到的收益退回去。
- `G1`、`G12`：分类需要一个名字和一处声明；空标签把这件事写进类型系统，而不新增运行期机制。
- `G8`：只加分类与四个能被测试验证的值类型；族级算法（求交、距离）没有消费者，进 backlog。
- `G9`：新成员接入是"派生 + 一个 variant 分支"，消费点由编译错误指出，不靠约定。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 三个族做成虚基类，具体类型公开继承 | 每个值类型多一个 vptr，`Ray` 在热循环里按值拷贝；内联收益与 trivially copyable 一起丢失（`G10`） |
| 只提供 concept 约束、不提供标签 | `is_base_of` 这类编译期问法与族的文档位置消失；运行期仍需要 variant 覆盖同一集合（`G1`） |
| 把 `pointAt(t)` 提升进基类统一定义 | 四种曲线的 `t` 语义不同，统一签名把差异藏进文档（`G4`） |
| 先不做分类，等有消费者再说 | 分类是命名与文档问题，成本是一次性且可验证为零；缺了它，新几何无处声明归属（`G12`） |

## 后果

- 既有类型的布局、语义、ABI 均不变（空基类优化），改动是纯增量。
- 新增四个值类型；坐标/面族的名称与 `QuadMesh`、`TriangleMesh` 不冲突（前者是单个面，后者是网格存储）。
- 族级算法仍缺失，已记入 `../backlog/geometry.md`。
- 现行设计记录在 `.ai/design/geometry-kinds.md`。
