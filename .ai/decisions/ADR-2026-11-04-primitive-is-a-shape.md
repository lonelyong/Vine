# ADR-2026-11-04：参数化几何是建模类型（`Primitive` 子类），算法属于曲面

> **状态**：已采纳
> **日期**：2026-11-04
> **影响范围**：`src/base/geometry`、`src/base/brep`、`src/robotics/io`、`tests/test_brep`、`tests/test_robotics_io`、`tests/test_proximity`、`tests/test_graphics`

## 背景

五种参数化几何（`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`）原本是 `Primitive` 的子类，尺寸是子类私有字段。当日先试过另一条路：把它们变成无身份的值类型，`Primitive` 用 `std::variant` 持有，消费点用 `std::visit` / `as<T>()`。

评估后两条路各有代价，而判断依据是"这些形状到底提不提供算法"：

- `Box` 的射线求交**就是** `Aabb::intersectWith(Ray)`，它没有自己的算法；
- `Cone` / `Cylinder` 的求交本质是**曲面**算法（射线–圆柱面、射线–圆锥面），属于将来的 `CylindricalSurface` / `ConicalSurface`；
- 建模侧真正需要的是身份、名字与 revision，这些只有 `Shape` 提供。

同时 OCCT 的长期职责被收窄为 **STEP 解析与三角化**，解析式构造（`BRepPrimAPI`）只是过渡桥。

## 决定

1. 参数化几何**留在建模侧**：`Primitive : Shape` 是抽象基类（受保护构造、接受 `ShapeType`），`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 是它的子类。
2. 五个子类与基类**同住 `Primitive.hpp`**：一个头文件，避免五个文件带来的 include 负担，也让 DLL 的导出随 `Primitive.cpp` 一起隐式完成。
3. `shape_type_` 由子类通过构造链交给基类，**不再由每个构造函数手写**。
4. 几何算法**不进** `Primitive`：实体只回答 `bounds()` / `isValid()` / `hasVolume()`；求交与距离属于几何值类型（`Aabb::intersectWith(Ray)`）与曲面（`SphericalSurface` 等，见 `geometry-kinds.md`）。
5. 取消 `std::variant` 持有与 `as<T>()`：消费点回到"按 `ShapeType` 分派 + 校验 `obj_cast`"。

## 理由

- `G1`、`G12`：`Box` 这五个是建模参数，不是算法载体；放进"值"一侧会让 `Solid` 的含义落空，并让半径/边长在实体与曲面之间出现两个家。
- `G5`、`G7`：五个参数记录住在一个头里，与基类同文件；新增形状的消费点在编译期被 `switch` 暴露。
- `G11`：标签由构造链确定，比"每个构造函数各写一次"少一类错。
- `G8`：不为"值化"的便利提前引入算法，曲面算法只在有消费者时落地。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 无身份值类型 + `Primitive` 用 `std::variant` 持有 | `Box` / `Cone` 不提供自己的算法，"值"换不到东西；半径/边长在实体与曲面之间出现两个家；建模需要的身份、名字与 revision 仍要另一套类型（`G1`） |
| 五个子类各占一个头文件 | 同一个基类上的五个参数记录，拆开后每个消费者都要多 include 四份用不到的定义，且 DLL 导出需要"记得在某个 TU include"的约定（`G7`） |
| 在 `Primitive` 上开放虚查询（`intersectWith` / `distance`） | 算法面挂在建模类型上，消费者要记住该查谁；也与"算法属于曲面"的分层冲突（`G8`） |
| 让 `geometry` 依赖 OCCT 直接构造解析实体 | 后端词汇进核心模块（`G2`），无后端构建将失去契约 |

## 后果

- `Box` 等重新带 vptr 与引用计数，复制不是逐字节拷贝；`GeometryKindsTest` 不再断言它们的 `sizeof`。
- `tests/test_brep` 的 `aPrimitiveOutsideTheFiveProducesNoSolid` 恢复（子类可以谎报 `ShapeType`，消费方必须校验 cast）。
- `tests/test_graphics/MeshShapeTest.cpp`、`tests/test_robotics_io/*`、`tests/test_proximity` 的 include 改为 `Primitive.hpp`（五个分开的头已删除）。
- 现行设计记录在 `.ai/design/geometry-parametric.md`。
