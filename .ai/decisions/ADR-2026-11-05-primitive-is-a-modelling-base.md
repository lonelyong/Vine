# ADR-2026-11-05：`Primitive` 是建模基类，不持有实体

- 状态：已采纳
- 日期：2026-11-05
- 取代：`ADR-2026-11-05-solid-family-and-variant.md`（其中"模型按值持有 `SolidShapef`"的部分；`Solid<T>` 族本身不变）
- 相关：`../design/geometry-parametric.md`、`../design/geometry-solid.md`、`../../src/base/geometry/sdk/vine/geometry/Primitive.hpp`、`../../tests/test_geometry/SolidTest.cpp`

## 背景

当天早些时候的做法是：`Primitive` 是**唯一**的建模类，按值持有一个 `SolidShapef`（五个解析体的变体），`shapeType()` 由变体派生，消费者 `std::visit` 它。

这个做法把一个假设写死进了基类：**每个图元都是一个解析实体**。实际不是：

- 描述文件里会出现没有解析体的图元（轮廓拉伸、扫掠面、以后可能的文本或曲线轮廓）；
- 一旦基类成员是 `SolidShapef`，这些图元要么无法表达，要么被迫塞进五个实体之一；
- 模型消费者（`brep` 桥、描述文件读写、三角化）也因此只认"变体"，而无法按子类作答。

同时，建模代码里需要的是 `Box` / `Sphere` 这样的**名字**：带身份（name / revision / tag）、带自己的尺寸、能 `obj_cast`。旧形态（`ADR-2026-11-04-primitive-is-a-shape.md`）正是这个形状，而当天被"变体持有"取代，代价是把"图元必然是实体"变成了基类的定义。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持变体持有（当天的形态） | 零改动；标签不可能漂移；消费者一次 `visit` | 基类假设了"图元是实体"；非实体图元无处安放；建模代码没有 `Box` 这个名字 |
| B 基类不持几何，五个子类各自持尺寸并给出自己的 `solid()` | 概念只要求"能回答 bounds 和自己的身份"；非实体图元天然可派生；建模侧有具体类名 | 每个子类一份尺寸字段；`bounds()` 变成纯虚；消费者要按 closed set 分派 |
| C 子类各存一份 `SolidShapef` + 一份尺寸字段 | 下标访问 `shape()` | 尺寸两个家，必然漂移（`G1`） |
| D 子类作为"视图"读写基类持有的变体 | 尺寸一个家 | 仍是 A 的假设；`setSolid()` 会打穿类型不变式（`G11`） |

## 决定

采纳 B。

- `Primitive : Shape`：受保护构造 `Primitive(ShapeType)`，**只**声明纯虚 `bounds()`；不持有任何几何、不认识 `Solid`。
- `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`：`final` 子类，各存自己的尺寸（`width_` / `height_` / `depth_`、`radius_`、`height_`、`radius_x_` …），量纲是描述文件写的**全量纲**。
- 每个子类另给 `solid()`：按当前字段**构造**自己的具体 kind（`BoxSolidf{ width_ / 2, … }`），返回**值**。尺寸只有一份，实体不存储、不缓存，因此没有失效点。
- `isValid()` / `hasVolume()` / `bounds()` 由子类实现，转发给 `solid()` 的那次构造：公式仍在几何侧（`G13`）。
- 几何侧的五个 kind 改名 `BoxSolid` / `SphereSolid` / `CylinderSolid` / `ConeSolid` / `EllipsoidSolid`（别名 `BoxSolidf` / `BoxSolidd` …），把 `Box` / `Sphere` 这些名字让给建模侧。
- 两个访问入口，都放在 `Primitive.hpp`：
  - `visitPrimitive(primitive, visitor)`：按 closed set 把具体子类交给 visitor；不属于五个时返回 `false`。
  - `solidShapeOf(primitive)`：返回 `std::optional<SolidShapef>`；不是实体时返回空。
- 移除 `Primitive::shape()` / `Primitive::solid()` / `Primitive::setSolid()` / `Primitive(const SolidShapef&)`。

## 后果

- 好的：基类不再声称"图元是实体"，非实体图元可以派生（`GeometryKindsTest` 之外的 `SolidTest.cpp` 用 `Profile` 类实测这一点）。
- 好的：建模代码写 `make_intrusive<Box>(2, 4, 6)`、`box.setWidth(...)`、`obj_cast<Box>(shape)`；描述文件的读写按子类作答。
- 好的：`Box` 与 `BoxSolid` 在调用点可区分（`G12`），且 `Box` 的名字不再和几何值冲突。
- 坏的 / 代价：每个子类要重复 `isValid()` / `hasVolume()` / `bounds()` 三个转发（15 个三行函数）；消费者每次从模型取几何都要构造一次值（`SolidShapef` 约 24–40 字节，无分配）；新增一种实体要同时动几何 kind 与建模子类。
- 触发条件（什么时候重开）：出现"图元必须异构持有"的容器（`vector<unique_ptr<Primitive>>` 已是这种做法，无需重开），或建模侧需要在不改子类的前提下替换实体 —— 那时再评估 B → A 之外的第三种形态。

## 证据

- 测试：`tests/test_geometry/SolidTest.cpp` 覆盖"基类不持几何"（`Profile`）、"子类按需构造实体"、"五个子类各报自己的标签"、`solidShapeOf` 只认五个、`visitPrimitive` 交出具体类型；`SolidDistanceTest` / `SolidIntersectionTest` / `SolidTessellationTest` 走子类到实体；`test_brep`、`test_robotics_io`、`test_proximity`、`test_graphics`、`test_meshio`、`test_brepio`、`test_gui` 全绿。 <!-- drift-ok -->
- 门禁：`check_ai_docs.py --strict`、`check_design_docs.py`、`check_public_docs.py`、`check_include_hygiene.py`、`check_export_annotations.py` 见提交说明。
