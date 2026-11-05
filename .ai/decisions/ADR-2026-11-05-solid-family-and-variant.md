# ADR-2026-11-05：解析实体是"族接口 + 封闭值集"，模型按值持有

- 状态：已取代（"模型按值持有 `SolidShapef`"的部分由 `ADR-2026-11-05-primitive-is-a-modelling-base.md` 取代；`Solid<T>` 族接口 + 五个 kind 值仍然现行）
- 日期：2026-11-05
- 相关：`../design/geometry-solid.md`、`../design/geometry-parametric.md`、`../../src/base/geometry/sdk/vine/geometry/Solid.hpp`、`../../tests/test_geometry/SolidTest.cpp`

## 背景

几何算法最初挂在建模类型上：三角化、射线求交、点到实体距离各是一个以 `shapeType()` 为唯一入口的自由函数。后果是同一个形状的公式散在三个文件里、模型的消费者认识算法入口、每加一个能力都要再写一遍标签分派。

值类型一侧又缺少“实体”这个词汇：`Aabb` / `Obb` 是盒，五个曲面是无盖无内部的壳，`closestPointOnSurface()` 答不了实体语义的距离。

评估中出现两个真实约束，它们把设计推到最终形态：

1. **有些参数必须是具体类型**。一个导出签名或后端接口只能写 `const Solid<float>&`，不能是模板；所以需要一个“任意一种实体”的**族类型**。
2. **模型与几何不共享所有权**（`G13`），并且模型的每个形状都在堆上已经存在，几何应当躺在那块内存里，而不是再分配一次。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 五个模型子类各自带算法（原状） | 零改动 | `ShapeType` 与类可能漂移；同一形状的公式散在多处；模型消费者认识算法入口（`G1`、`G12`） |
| B `Solid<T>` 直接是 `std::variant` 的别名 | 最简、最像 C++ | 没有共同类型；只问一个问题的消费者也要自己 visit（`G7`） |
| C 族接口 + `Primitive` 用指针持有 | 共同类型、便宜拷贝 | 每次构造一次堆分配、每次 `bounds()` 一次解引用、共享所有权（`G10`、`G13`） |
| D 族接口 `Solid<T>` + 五个 kind 值 + 模型持有 `SolidShape<T>` 变体 | 共同类型；零分配；visit 穷尽；模型侧一次持有 | 每个 kind 多一个 vptr；两个视图（接口 / variant）各有一条使用规则 |

## 决定

采纳 D。

- `Solid<T>`：抽象接口，五个纯虚——`isValid()`、`hasVolume(T)`、`bounds()`、`closestPoint()`、`intersectWith(Ray)`。默认容差写在接口与每个 kind 的声明上（派生类会遮蔽基类的默认参数）。
- `Box<T>` … `Ellipsoid<T>`：`final` 值类型，持半量纲与自己的解析式。
- `SolidShape<T> = std::variant<五个 kind>`：模型按值存的那份几何。
- `solidOf(SolidShape<T>&)`：族视图，一个 `std::visit` 取引用，零拷贝。
- `Primitive`：`Shape`（身份、名字、revision）+ `SolidShapef`。`setSolid()` 是唯一 mutator，同时同步 `shapeType()`；`solid()` 给族视图，`shape()` 给 variant。
- 三角化、B-rep 桥、描述文件读写者 visit 这份 variant（它们必须按 kind 作答）。

## 后果

- 好的：一个形状一个家；模型不再认识算法；`ShapeType` 由几何派生，不会漂移；加一种 kind 时，每个 visit 点都是编译错误，编译器直接列出待办；`const Solidf&` 让“必须是具体类型”的签名成立。
- 好的：`Primitive` 仍然是零额外分配——几何躺在本来就要分配的那个对象里。
- 坏的 / 代价：每个 kind 有一个 vptr（`SolidShapef` 约 24–32 字节，`Primitive` 跟着变大，仍无分配）；两个视图各有一条使用规则（问一个问题用 `solid()`，按 kind 作答用 `shape()`）；默认参数要在两处重复。
- 触发条件（什么时候重开）：出现必须**异构持有实体**的容器（`vector<unique_ptr<Solid>>`），或第三方需要注册自定义 kind——那时才该把 variant 换成指针 + 纯接口（选项 C 的代价届时才有对应的收益）。

## 证据

- 测试：`tests/test_geometry/SolidTest.cpp`（接口抽象、两个视图一致、标签派生、体积判据）、`SolidDistanceTest.cpp`、`SolidIntersectionTest.cpp`、`SolidTessellationTest.cpp`；`test_geometry` 165 项全绿，`ctest` 28 项中除环境性的 `check_translations`（缺 `lupdate`）外全部通过。
- 门禁：`check_public_docs.py` 0 new、`check_include_hygiene.py` 0、`check_export_annotations.py` 0、`check_diagnostic_formats.py` 0、`check_ai_docs.py --strict` 0、`check_design_docs.py` 0。
- 外部对照：Box2D v3 用 tag + union（选项 B 的 C 风格等价物）；PhysX 的几何族有基类但**没有虚函数**，行为由 `PxGeometryQuery` 按 tag 分派；Bullet / Jolt 用虚函数，代价是形状必须指针持有——它们买的是第三方扩展，本仓库不卖。
