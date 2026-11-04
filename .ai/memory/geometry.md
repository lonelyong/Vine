# geometry 要点

- **纯内联的 SDK 类必须由本模块某个 TU include**：`Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` 的成员全部定义在头文件里，
  DLL 只有在带 `VN_GEOMETRY_LIB` 的 TU 里看到这个类时才会实例化并导出它们。现在由 `Primitive.cpp` 显式列出这五个头。
- **参数化几何是建模类型**：`Primitive : Shape` + `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid` **全部在 `Primitive.hpp` 一个文件**里
  （原来五个分开的头已删）。子类把 `ShapeType` 传给受保护构造，不再手写 `shape_type_`。
  消费点：`obj_cast<vn::geometry::Box>`、`switch (shapeType())`；它们**不提供**几何算法（射线–盒用 `Aabb::intersectWith`，
  射线–球面属于 `SphericalSurface`）。理由见 `design/geometry-parametric.md`、`decisions/ADR-2026-11-04-primitive-is-a-shape.md`。
- **值类型（`Aabb` / `Obb` / `LineCurve` / `Plane` / `Ray` 及全部曲线曲面）上有 `math::FP T`，但类上没有 `VN_GEOMETRY_API`**：
  短查询定义在头文件（内联发生在调用方，**不要**加 `extern template`，会打死拾取循环的实测收益）。
  导出只来自显式实例化，而**每个类型、`float` 与 `double` 两种精度都要实例化**：
  - 有 `src/` 实现的类型在自己的 `.cpp` 里（`LineCurve.cpp` / `Obb.cpp` / `Plane.cpp`）——显式实例化只能实例化该 TU 看得见的成员定义；
  - 纯头文件的类型集中在 `src/Instantiations.cpp`，每个类型两行（float、double）；新增类型别忘了加，否则库里没有它的符号。
  - **新增 .cpp 要重跑 `cmake -S . -B build`**（`vn_add_library` 用 glob，无 `CONFIGURE_DEPENDS`）。
  - 导出的 `std::vector` / `std::shared_ptr` 成员会报 C4251，已在 `Instantiations.cpp` 里局部屏蔽。
  理由与实测见 `design/geometry-primitives.md` 2.8、`decisions/ADR-2026-11-04-geometry-precision-and-instantiation.md`。
- 新增同类的纯内联形状时，同样要在本模块的某个 `.cpp` 里 include 它，或者把成员改成出实参（out-of-line）实现。
- **`Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` 是纯头文件类**（不导出）：成员和 `desc()` 都在头里，消费者各自编译；
  它们的描述符用内联 `VN_OBJECT_META`，靠 `Type::obtain()` 保证全进程只有一个，见 `core.md`。
- 五个基本体的局部空间：`Box` / `Sphere` / `Ellipsoid` 原点居中；`Cylinder` / `Cone` 沿 Z 跨 `[-h/2, +h/2]`，锥底在 `-h/2`。
- **曲面同理**：`Surface<T>` 是**模板接口**（十三项：`evaluate(u,v)`、`derivativeU/V`、四个参数界、两个方向的闭合/周期、`continuity()`），`ElementarySurface<T>` /
  `BoundedSurface<T>` 只是分类标签（无状态、无虚函数）。`Surface` / `ElementarySurface` / `BoundedSurface` **现在都要带 `<T>`**（旧的非模板写成 `Surface<double>`）。
  `Plane` 不存面内框架（两轴由 `normal` 每次推导，因为它被复制）；`SphericalSurface` 的 `u`（方位角）闭合周期、`v`（极角）不闭合。接口里**没有** `normalAt` / `bounds` / 二阶导数。
- **`Curve<T>` 是纯虚接口（十个纯虚函数）**：新增一种曲线要一次实现 `evaluate` / `derivative` / `secondDerivative` /
  `firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period` / `continuity` / `reversedParameter`。
  陷阱：`derivative` 是**速度向量**（圆的长度 = 半径），不是单位切向；`TrimmedCurve` 的 `reversedParameter` 在自己区间内
  反射（`first + last - u`），不要转发给基曲线。`Ray` 故意不派生 `Curve`（热路径按值拷贝）。见 `design/geometry-kinds.md` 2.7。
- **圆锥曲线四个都已实现**（`CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve`）；**框架与 `continuity()`
  = `CN` 住在 `ConicCurve`**（`center` / `normal` / `x_direction` + `yAxis()`），四个子类型只留自己的标量（抛物线用继承的
  `center` 当顶点，没有单独的 `vertex` 字段）。改 `ConicCurve` 的框架 = 改四个类型的大小（`CircleCurved` 现在是 3 向量 + double）。
  `BezierCurve` / `BSplineCurve` / `OffsetCurve` 的数据模型与类型事实已定义，但 `evaluate` / `derivative` / `secondDerivative`
  （样条还有 `isClosed`）仍是纯虚 ⇒ **三者不可构造**，测它们的契约要在测试里写 `NumericStub` 补上数值回答（backlog G8）。
  写派生类时注意：**依赖基类的成员一律要限定** —— 基类的 static 函数写 `ConicCurve<T>::inPlaneAxis(...)`，继承的数据成员/函数
  加 `using ConicCurve<T>::center;` 之类的声明，否则 MSVC 报 C3861/C2065。
