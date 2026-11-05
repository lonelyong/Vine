# geometry 要点

- **数组别名必须带元素类型**（`models/Array.hpp`）：`Vec2fArray` / `Vec3dArray` / `Vec3ui32Array`，没有默认 float 的
  `Vec2Array`；标量侧 `Int8Array` … `UInt64Array` 与 `FloatArray` / `DoubleArray`；命名照抄 math 的 `Vec2f` / `Vec2i8` / `Vec2ui32`。

- **纯内联的 SDK 类必须由本模块某个 TU include**：建模子类（`Box` / `Sphere` / …）与几何 kind（`BoxSolid` / `SphereSolid` / …）的成员定义都在头文件里，
  DLL 只有在带对应 `_LIB` 宏的 TU 里看到这个类时才会实例化并导出它们。现在每个建模类型都有自己的 `.cpp`（如 `models/src/Box.cpp`）。
- **建模类型是一个独立模块 `src/base/models`**（`sdk/vine/models/`，namespace `vn::models`，库 `vn::Models`）：`Shape` / `Primitive` / `Model` / `Mesh` 族 /
  `BrepShape` / `PointCloud` / `Material` 族都在这里，单向依赖 `vn::Geometry`；include 写 `<vine/models/Xxx.hpp>`。
  几何值类型（曲线 / 曲面 / `Solid` / `Aabb` / `Obb` / `Ray`）留在 `src/base/geom3d`。
- **参数化几何是建模类型，基类不持几何**：`Primitive : Shape` 只有受保护构造 `Primitive(ShapeType)` 与纯虚 `bounds()`，**不认识 `Solid`**；
  `Box` / `Sphere` / `Cylinder` / `Cone` / `Ellipsoid`（`final`，各自 `models/Box.hpp` … `models/Ellipsoid.hpp`）各存**全量纲**字段，并用 `solid()` 按当前尺寸
  构造自己的具体 kind（`BoxSolidf` …，返回**值**，不存不缓存）。`isValid()` / `hasVolume()` / `bounds()` 由子类转发给 `solid()`。
  两个入口在 `models/PrimitiveDispatch.hpp`：`visitPrimitive(primitive, visitor)`（closed set，不属五个时 `false`）与 `solidShapeOf(primitive)`（`optional<SolidShapef>`）。
  **没有** `Primitive::shape()` / `setSolid()`；`ShapeType` 仍由子类构造传给基类。理由见 `decisions/ADR-2026-11-05-primitive-is-a-modelling-base.md`。
- **值类型（`Aabb` / `Obb` / `LineCurve` / `Plane` / `Ray` 及全部曲线曲面）上有 `math::FP T`，但类上没有 `VN_GEOMETRY_API`**：
  短查询定义在头文件（内联发生在调用方，**不要**加 `extern template`，会打死拾取循环的实测收益）。
  导出只来自显式实例化，而**每个类型、`float` 与 `double` 两种精度都要实例化**：
  - 有 `src/` 实现的类型在自己的 `.cpp` 里（`LineCurve.cpp` / `Obb.cpp` / `Plane.cpp`）——显式实例化只能实例化该 TU 看得见的成员定义；
  - 纯头文件的类型集中在 `src/Instantiations.cpp`，每个类型两行（float、double）；新增类型别忘了加，否则库里没有它的符号。
  - **新增 .cpp 要重跑 `cmake -S . -B build`**（`vn_add_library` 用 glob，无 `CONFIGURE_DEPENDS`）。
  - 导出的 `std::vector` / `std::shared_ptr` 成员会报 C4251，已在 `Instantiations.cpp` 里局部屏蔽。
  理由与实测见 `design/geometry-primitives.md` 2.8、`decisions/ADR-2026-11-04-geometry-precision-and-instantiation.md`。
- 新增同类的纯内联形状时，同样要在本模块的某个 `.cpp` 里 include 它，或者把成员改成出实参（out-of-line）实现。
- **族级曲面查询**：`closestPointOnSurface()`（高斯-牛顿，返回点 / `(u,v)` / 距离）与 `intersectRayWithSurface()`（三元牛顿，结果内嵌 `RayHit` + `u/v`）都是头文件模板。
  **参数约束写成函数上的 `requires std::is_base_of_v<Surface<T>, S>`**：`Plane` 经 `ElementarySurface<T>` 间接派生，`const Surface<T>&` 形参推导不到 `T`；写成命名 concept 又会触发 MSVC C7605（概念体里出现受约束模板 `Surface<T>`）。
  窗口是 `SurfaceParameterWindow<T>`（参数矩形，不是 `Rect2`）；无限的一端取曲面自己的界，默认值 `naturalSurfaceWindow()` 就是自然域。`finiteParameterWindow()` 是唯一解析点。
  已知性质：最近点是局部最小；射线相切漏检（雅可比奇异）、网格分辨不了的折叠会漏命中；球极命中法向为零；射线在面上用三个跨射程探针判 `coincident`。
- **曲线 × 非解析曲面**：`intersectCurveWithSurfaceByProjection()` 与解析入口共用 `detail::scanCurveAgainstSurface`，只把取值换成 `detail::signedDistanceToSurface`（投影距离 + `dS/du × dS/dv` 的符号）。
  **这个带符号距离故意留在 `detail`**：极性属于参数化（球面的叉积朝内，符号与 `signedValue()` 相反），求根只用变号。奇异参数线（球极）返回 NaN = “无信息”。
  扫描收窄根后会再验一次 `|f| ≤ tolerance`，滤掉最近点分支跳变造成的伪变号（`geometry-intersection.md` P8）。
  设计见 `design/geometry-surface-queries.md`，测试在 `tests/test_geom3d/SurfaceQueryTest.cpp`。
- **点到曲线的最近点 / 距离**：`closestPointOnCurve()`（`CurveProjection.hpp`）把平方距离的驻点 `f(u) = (C(u) - P)·C'(u)` 当零点扫，`refineRoot()` 收窄，**窗口端点也是候选**（足点在窗口外时最小值在边界上）。只调 `evaluate` / `derivative`。
  窗口是 `optional<pair>`：有界曲线可省，无界曲线（直线 / 双曲线 / 抛物线）必须给。局部最小与“平坦极小值（f 不变号）”靠 `sample_count`，见 `design/geometry-curve-queries.md`。
- **解析实体 `Solid<T>` 是族接口，不是 variant**：`Solid.hpp` 里 `Solid<T>` 是抽象基类（`isValid` / `hasVolume` / `bounds` / `closestPoint` / `intersectWith` 五个纯虚），五种实体各一个头文件（`BoxSolid.hpp` … `EllipsoidSolid.hpp`），`SolidShape.hpp` 给 `SolidShape<T> = std::variant<五个 kind>` 与 `solidOf(shape)`（一个 `std::visit`，零拷贝）。建模子类用 `solid()` 构造这份值，`solidShapeOf(primitive)` 把模型转成它。**默认容差要同时写在基类与每个 kind 上**（派生声明会遮蔽基类默认参数）。设计见 `design/geometry-solid.md`，测试在 `tests/test_geom3d/SolidTest.cpp`。
- **解析实体的三角化**：`tessellateSolid(const SolidShapef&, quality)`（`src/base/models/sdk/vine/models/SolidTessellation.hpp` + `src/base/models/src/SolidTessellation.cpp`）用 `std::visit` 按 kind 解析铺网格，返回 `intrusive_ptr<TriangleMesh>`；三角形数：盒 12，球 / 椭球 `2·around·(along−1)`，圆柱 `4·around`（侧 2·around + 两盖各 around），圆锥 `2·around`（侧扇 + 底盖）。
  **只对 `Solid<float>` 成立**（`TriangleMesh` 是 float），所以它是自由函数而不是 `Solid<T>` 的成员。
  **顶点在法向不连续处复制**（盒棱、柱盖、锥顶按段），所以顶点数不是最小集；闭合性要按**位置**数边，不能按索引。结果只写位置 / 法向 / 索引，不写 UV。
  设计见 `design/geometry-tessellation.md`，测试在 `tests/test_geom3d/SolidTessellationTest.cpp`。
- **射线 × 解析实体**：`Solid<T>::intersectWith(ray, eps)` 走每个 kind 自己的解析式：盒 slab、球 / 椭球二次、圆柱侧面+两盖、圆锥侧面+底面。结果最多两个（相切一个），只报 `t ≥ -eps`，**内部起点只报离开**；法向按 `RayHit` 转向射线，圆锥顶点命中法向为零。`Solid<T>` 里的 `T` 是计算精度，实体量纲本身是 float。
- **点到解析实体的最近点 / 距离**：`Solid<T>::closestPoint(query, tolerance)` 是**实体**语义——内部（含边界）返回查询点本身、距离 0；外部返回边界最近点。盒钳制、球径向、椭球 Lagrange 一维二分、圆柱径向×轴向钳制、圆锥「底面盘 vs 母线轮廓」取较近。`tolerance` 只控制椭球求根，内 / 外判定是精确比较。
- **`Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` 是纯头文件类**（不导出）：成员和 `desc()` 都在头里，消费者各自编译；
  它们的描述符用内联 `VN_OBJECT_META`，靠 `Type::obtain()` 保证全进程只有一个，见 `core.md`。
- 五个基本体的局部空间：`BoxSolid` / `SphereSolid` / `EllipsoidSolid` 原点居中；`CylinderSolid` / `ConeSolid` 沿 Z 跨 `[-h/2, +h/2]`，锥底在 `-h/2`。
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
  （`vn::geom3d::inPlaneAxis`），`ConicCurve` 不再有同名 protected static。
- **`geom2d` 是第二份契约，不是 3D 的泛型特化**：`geom2d::Curve<T>` 与 `geom3d::Curve<T>` 同名同义、互不继承
  （`decisions/ADR-2026-11-05-planar-curve-contract.md`）；今天只有 `LineCurve` / `CircleCurve`，平面圆是 `center` + `radius`、
  没有 `ConicCurve` 框架。模块 header-only，`vn::Geom2D` 是 **INTERFACE 目标**（没有 `src/`，不导出符号，也就没有实例化表）；
  名字不带维度标记，精度后缀仍是 `d` / `f`。设计见 `design/geometry-planar.md`。
- **平面几何（`geom2d`）现有四型**：`LineCurve` / `CircleCurve` / `EllipseCurve` / `ArcCurve`。椭圆自带 `x_direction`（主轴，`yAxis()` 由 +90° 旋转导出），圆没有——圆的起点由平面定死，所以**没有**平面 `ConicCurve` 层。
  `ArcCurve` 是“圆按值 + 角度区间”：闭合要区间盖满圆的域、周期要至少一个周期，否则 `period() == 0`；裁剪不重参数化，导数就是圆在同一个角度上的速度。
  **`reversedParameter` 不返回同一个点**：它给的是反向曲线（原曲线关于域中点轴 / 起始轴的镜像）上的同一点，只保证 involution 与两端互换；
  `evaluate(reversedParameter(u)) != evaluate(u)`（圆是 y 反号、弧是 x↔y 交换）。契约由 `tests/test_geom2d/CurveContractTest.cpp` 的 float / double 扫查盯着。
- **平面圆锥曲线共享 `ConicCurve<T>`**（`geom2d`）：层里只有 `center` + `x_direction`（构造时归一化）+ `yAxis()`（+90° 旋转）+ `continuity() == CN`，不放求值与参数域。
  `EllipseCurve` / `HyperbolaCurve` / `ParabolaCurve` 派生自它；**圆不派生**（它的起点由平面定死，存一根永远 (1,0) 的轴是死字段）。
  参数含义各不相同：椭圆是偏心角、双曲线是双曲角（只走 `+x_direction` 一支，另一支是取反方向）、抛物线是垂直轴上的偏移（`P = vertex + u²/(4f)·X + u·Y`）。
  零方向存零向量 ⇒ 点查询答 `center`，`isValid()` 是唯一判据；三条曲线都无界非周期，`reversedParameter` 都是 `-u`。
- **平面最近点 `closestPointOnCurve()`**（`geom2d/CurveProjection.hpp`）：与 3D 同名函数同一骨架——平方距离驻点 `f(u) = (C(u)-Q)·C'(u)` 求根 + 采样点/window 端点比较；
  无界曲线（直线/双曲线/抛物线）**必须**给窗口，不给就 `nullopt`。注意两点：等距多解（圆心）报的是“其中一个”，不要断言参数是 0；
  双曲线对轴上点的最近点通常**不在顶点**（如 (3,0) 到 `x=2cosh, y=sinh` 的最小点在 `5cosh u = 6`，距离 √0.8），写测试先解一遍驻点方程。
- **平面包围盒**：`bounds()` 只在有界类型上（`CircleCurve` / `EllipseCurve` / `ArcCurve`，返回 `math::Rect2<T>`），接口不声明它——无界曲线只能返回假盒。
  椭圆取世界轴上的支撑函数 `sqrt((a·X_i)² + (b·Y_i)²)`，**不要**用 `±major_radius`（旋转后会松一圈）；圆弧取两端点加区间内四个轴向极值，
  反向区间与正向同盒（区间方向不改点集）；圆用 `|radius|`。
- **无效状态必须答有限值**：抛物线焦距为零原先会除零（`u²/(4·0)` → NaN / inf），现在 `pointAt` 答顶点、两条导数答零向量（`hasFocalLength()`）；
  其余退化形态：零半径圆→圆心、零半轴椭圆→线段、零半轴双曲线→过中心的直线、零方向圆锥曲线→center。
