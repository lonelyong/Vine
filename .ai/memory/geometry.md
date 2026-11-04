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
- **三个旋转面已实现**（`CylindricalSurface` / `ConicalSurface` / `ToroidalSurface`）：每个持一个 `Placement<T>` 值（`origin` / `axis` / `x_direction` + `yAxis()`，恒右手），
  `continuity()` 各自 `return CN`；平面内轴的规则只有一份，在 `Frame.hpp` 的 `inPlaneAxis()`（`ConicCurve` / `Plane::xAxis` / `Placement` 都用它）。
  放置判据：**改变点集就存**（圆柱/圆锥/圆环），**只改参数化就推导**（球面仍是世界 Z 轴）。
  参数约定照 OCCT：圆柱 `v` 是轴向距离、圆锥 `v` 是**母线弧长**（`dP/dv` 为单位向量）、圆环 `v` 是**赤道面角**。
  `RectangularTrimmedSurface` 是视图（基曲面 `shared_ptr` + 四个参数界），闭合/周期要“基曲面 + 矩形”同时成立。
  `BezierSurface` / `BSplineSurface` **已可求值**：行优先扁平控制网（索引 `iu + u_pole_count * iv`，读网只用 `poleAt(iu, iv)`）+ 可选等长权重（空 = 多项式）；
  张量积求值是“先沿 u 混合每行、再沿 v 混合”，有理形式在四维齐次坐标上做后除权重。两者共用曲线侧的基础设施：
  `BezierBlend.hpp`（`deCasteljau` / `differences`）与 `BSplineBasis.hpp`（`knotSpan<T>` / `basisValue<T>` / `isClampedKnotVector` / `knotContinuity`，
  **注意调用点必须显式写 `<T>`**，`std::span` 推不出）。`BSplineSurface` 只接受夹持形式；闭合用齐次极点相等判定（夹持基线性无关，等效于求值比较）。
  代价：求值每次分配临时数组（`backlog/geometry.md` G10）。曲面侧的完整设计在 `design/geometry-axis-surfaces.md`。
- **裁剪视图（`TrimmedCurve` / `RectangularTrimmedSurface`）的两条契约**：① `period()` / `uPeriod()` / `vPeriod()` 只在自身 `isPeriodic()` 为真时
  回答基曲线 / 基曲面的周期，否则 0（以前转发基的周期，四分之一圆会答 2π）；要基的数字直接读 public 的 `basis_curve` / `basis`。
  ② 空基不再解引用：查询退化为 `Point()`、零向量、`C0`、`false`（`isValid()` 是唯一判据）。`OffsetCurve` 同样处理（它的
  `isValid()` 还要求参考方向非零）。这类跨类型契约由 `everyCurveKeepsTheInterfacePromises` / `everySurfaceKeepsTheInterfacePromises` 扫查，改接口时先跑它们。
- **样条重数规则**：两端 `度 + 1`（夹持），**内部 ≤ 度**；内部重数 = 度 + 1 是缺口，`isValid()` 直接拒绝——
  所以 `continuity()` 的 `C0` 永远真的是“交于一点”。内部重数 = 度 → `C0`（有拐点但仍连通）。见
  `decisions/ADR-2026-11-04-bspline-without-knot-breaks.md`。
- **`isValid()` 是 `Curve<T>` / `Surface<T>` 的纯虚函数**（曲线 11 项、曲面 14 项）：新增一种曲线/曲面**必须**实现它，
  否则编不过。`Plane::isValid()` 是 `normal.length() > EPS`（零法向已不再"永远可用"）。见
  `decisions/ADR-2026-11-04-isvalid-on-family-interfaces.md`。
- **模板参数约束**：几何族一律 `template <vn::math::FP T>`（含 `Plane`，它曾例外）；`Aabb` / `Obb` 有意保留 `typename T`
  以允许 `Aabbi = Aabb<int32_t>`。新增族内类型时用 `FP`，别抄值类型那条。
- **测试精度**：`GeometryKindsTest` 的族级扫查 `expect*Invariants` / `sweepCurves` / `sweepSurfaces` 是 **模板化的**，
  double 与 float 各跑一遍（`…InFloat`）；新增曲线/曲面把它加进 `sweep*()` 就能同时拿到两种精度。旧测试（Ray/Plane/曲线系列）
  多数仍是 double-only，`Aabb` / `Obb` 单测用的是 float。
- **`Curve<T>` 是纯虚接口（十一项）**：新增一种曲线要一次实现 `evaluate` / `derivative` / `secondDerivative` /
  `firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period` / `continuity` / `reversedParameter`。
  陷阱：`derivative` 是**速度向量**（圆的长度 = 半径），不是单位切向；`TrimmedCurve` 的 `reversedParameter` 在自己区间内
  反射（`first + last - u`），不要转发给基曲线。`Ray` 故意不派生 `Curve`（热路径按值拷贝）。见 `design/geometry-kinds.md` 2.7。
- **曲线 × 解析曲面求交已可用**：`intersectCurveWithSurface(curve, surface, window, tolerance, interval_count)`（`CurveSurfaceIntersection.hpp`）
  把曲面的带符号距离 `signedValue(surface, point)` 沿曲线求根（`math::refineRoot`，假位法 + 二分回退）。五个解析面各有一个 `signedValue` 重载；
  **无界曲线必须给窗口**（`LineCurve` / 双曲线 / 抛物线），相切默认漏检（P4），一段落在面上报 `Coincident` 不列点。
  样条面/裁剪面需要参数反演，未做（backlog G12）。
- **圆锥曲线四个都已实现**（`CircleCurve` / `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve`）；**框架与 `continuity()`
  = `CN` 住在 `ConicCurve`**（`center` / `normal` / `x_direction` + `yAxis()`），四个子类型只留自己的标量（抛物线用继承的
  `center` 当顶点，没有单独的 `vertex` 字段）。改 `ConicCurve` 的框架 = 改四个类型的大小（`CircleCurved` 现在是 3 向量 + double）。
  `BezierCurve` / `BSplineCurve` **已可求值**（de Casteljau 三次复用 / Cox–de Boor 基函数 + 商法则），两者可构造，也能经 `Curve<T>&` 分派；
  `OffsetCurve` 只有 `evaluate` / `derivative`，`secondDerivative` 仍是纯虚（缺基曲线三阶导数，见
  `decisions/ADR-2026-11-04-curve-third-derivative.md`）⇒ 测试里用 `OffsetStub` 只补这一项。
  `BSplineCurve::isValid()` 要求**夹持形式**（首末重数 = `degree + 1`、相异节点递增）；两种样条的权重必须全为正。
  退化输入只有一个判据——`isValid()`：直线（零方向，`closestPoint()` 回原点、`intersectWith()` 回无标志、不除零）、
  裁剪（空基曲线 / 反向区间）、圆锥曲线（`normal` 为零 ⇒ 塌成沿 `x_direction` 的线段或射线，不是“半径 0 的圆”）。
  `CircleCurve::bounds()` 与 `EllipseCurve::bounds()` 用同一支撑函数 `sqrt((a·X_i)² + (b·Y_i)²)`，退化时给线段盒。
  写派生类时注意：**依赖基类的成员一律要限定** —— 基类的 static 函数写 `ConicCurve<T>::inPlaneAxis(...)`，继承的数据成员/函数
  加 `using ConicCurve<T>::center;` 之类的声明，否则 MSVC 报 C3861/C2065。`inPlaneAxis` 现在是 `Frame.hpp` 的自由函数
  （`vn::geometry::inPlaneAxis`），`ConicCurve` 不再有同名 protected static。
