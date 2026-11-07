# geometry 值类型：Aabb、Obb、Line、Plane

> **状态**：现行
> **最后核对**：2026-11-04
> **范围**：`src/base/geom/sdk/vine/geom/spatial` 的空间值类型（`Aabb` / `Obb` / `LineCurve` / `Plane`）：归属、命名与契约。
> 不含 `Shape` / `Mesh` / `Material` / `Model` 的存储与 revision（见 `graphics-geometry.md` §3.5），也不含 2D 区间 `Rect2`（仍在 `math`）。
> **权威来源**：`src/base/geom/sdk/vine/geom/spatial/Aabb.hpp`、`Obb.hpp`、`LineCurve.hpp`、`Plane.hpp`；`tests/test_geom_spatial`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

四个空间形状原先住在 `src/base/math`：`Rect3`（轴对齐盒）、`Line`（今 `LineCurve`）、`Plane`；同期引入 geometry 的 `Obb` 却是 geometry 的。同一概念族被模块边界切开。

`Rect3` 更乱：`math::Aabb` / `Aabbf` / `Aabbd` 是它的别名，`geometry/Aabb.hpp` 是转发 shim，`Mesh::Aabb` 是第三个拼写。一个轴对齐盒有四个名字、三个家。

消费者全部在 geometry 之上：`Mesh`、`graphics::Node`、`ChainBuilder`、`ChainPresets`、`Obb::toAabb()`。`math` 内部没有任何一处使用 `Rect3` / `Line` / `Plane`（`Plane` 用 `Line`，两者一起走；`Line` 现名 `LineCurve`，见 `geometry-kinds.md`）。

### 1.2 目标

- 一个轴对齐盒只有一个名字 `Aabb`，一个家 `geometry`。
- 盒、线、面与 `Obb` 同居：解析原语在同一模块，`Plane` 的返回值不必跨模块。
- `math` 只留数值与变换，不再持有形状。
- `Rect2` 的消费者（GUI 矩形）不受影响。

### 1.3 非目标

- 不搬 `Rect2`：其唯一消费者是 `appfw::Gui` 的布局矩形，不是形状（2.6）。
- 不提供 `math::Rect3` 兼容别名：`math` 不能 include `geometry`，那会形成回边（2.5、`D2`）。
- 不在本次引入相交/距离查询（ray–AABB、OBB–OBB、AABB–OBB）；类型先归位，查询等真实消费者（`G8`）。

### 1.4 一句话结论

**数值与变换归 `math`，空间实体归 `geometry`；轴对齐盒叫 `Aabb`，没有别名，也没有回边。**

## 2. 设计要点

### 2.1 边界判据：数值 vs 空间实体

- **是什么**：`math` = 标量、向量、点、矩阵、四元数、等距变换；`geometry` = 有空间含义的实体与形状算子（盒、线、面、网格、参数化形状）。
- **为什么**：`G1`、`G5` —— 判据一句话可判断新增类型放哪，不必逐个讨论；`Rect3` 的消费者本来就在 `geometry` 一侧。
- **边界**：判据只管**类型的归属**，不表示 `geometry` 可以反向依赖 `math` 之外的东西；`math` 依旧只链 `vn::Global`。
- **代价**：只要一个盒子的人现在要链 `vn::Geometry`（+ `vn::Core`）。今天的 `Aabb` 消费者（`geometry`、`graphics`、两者的测试）都已链它，实际没有新增依赖边（`G10`）。

### 2.2 一个盒子一个名字

- **是什么**：类型是 `vn::geom::spatial::Aabb<T>`；别名只有 `Aabbf` / `Aabbd` / `Aabbi`。`Rect3`、`math::Aabb*`、`geometry` 的空转发头全部清退。
- **为什么**：`G1`、`G4`、`G12` —— 名字说明形状；`Aabb` 与 `Obb` 成对，`Rect` 在 3D 里是误名。
- **边界**：`Mesh::Aabb` 作为 `Mesh` 的类作用域 float 拼写保留（`= Aabbf`），它是别名不是类型，不构成第二个概念。
- **模板参数有意不受 `vn::math::FP` 约束**：`Aabb<T>` / `Obb<T>` 是**空间值类型**，`Aabbi = Aabb<int32_t>` 是合法的整数索引盒（像素 / 网格索引），所以这里用 `typename T`；几何族（`Curve<T>` / `Surface<T>` 及其成员）则一律约束成浮点，因此 `Plane<T>` 在 2026-11-04 从 `typename T` 改成 `vn::math::FP T`，与族内其余类型一致（`G6`）。两处不一致是刻意的，不是遗漏。
- **只有 `Aabb` 有 `isValid()`**：`Aabb` 允许一个**空哨兵**状态（`min > max`，供 `expandBy` 从零开始生长），所以它需要一个判据（`isValid() == !isNeg()`，与 `isEmpty()` 同一事实的两个问法）；`Obb` 把非法状态在构造时就消除了（半轴取绝对值、零长四元数归一化回单位旋转），不可能构造出一个不可用的盒子，所以它没有也不需要 `isValid()`（`G4`：接口只写可能出错的问法）。

### 2.3 解析原语同住一个模块

- **是什么**：`LineCurve`、`Plane`、`Aabb`、`Obb` 都在 `vine/geom/spatial`；`Plane::intersectWith(Plane, LineCurve&)` 的返回值同模块。
- **为什么**：`G1`、`G6` —— 同一层的形状不跨模块分裂；`LineCurve` 的唯一使用者就是 `Plane`，同批移动不留中间态。
- **边界**：它们是**值类型**，不进入 `Shape` 层级（2.4）；这不表示 `geometry` 的语义变成“纯数学”，`Shape` / `Mesh` 仍是主内容。射线同样是空间实体：`Ray` 与图元相交已在 `geometry`，契约见 `geometry-ray.md`；参数化几何（`Box` / `Sphere` / …）与模型持有者 `Primitive` 的分工见 `geometry-parametric.md`。

### 2.4 值类型不进 Shape 层级

- **是什么**：`Aabb` / `Obb` / `LineCurve` / `Plane` 是模板值类型：无 vptr、无 `Object` 元类型、无 revision。
- **为什么**：`G1`、`G10` —— 包围盒按值拷贝、按值返回；`Shape` 的运行时多态与 revision 是给可编辑内容用的，不是给几何量用的。
- **边界**：需要多态分发的形状走 `Shape`；需要便宜拷贝的空间量走这四个值类型。

### 2.5 没有回边

- **是什么**：`math` 的 CMake 依赖仍只有 `vn::Global`；`math` 不 include 任何 `vine/geom/spatial/*`。
- **为什么**：`G2` —— 依赖单向，`geometry → math` 不允许反向。
- **代价**：已有代码里的 `math::Rect3` 必须一次性改到 `geom::spatial::Aabb`，没有“兼容一个版本”的缓冲（`D2`）。

### 2.6 `Rect2` 留在 `math`

- **是什么**：`vn::math::Rect2<T>` 不动；`appfw::Gui` 的 `using Rect = math::Rect2i` 不动。
- **为什么**：`G1`、`G5`、`G8` —— 它表达的是布局/命中矩形，不是空间形状；`geometry` 今天全是 3D 实体，把它拉进来只为对称会引入一个无关职责。
- **边界**：将来出现真正的 2D 几何消费者（投影包围、2D 碰撞）再单独评估，不预先搬。

### 2.7 拟合在 `Obb`，分解在 `math`

- **是什么**：`Obb::computePca` / `Obb::computeFixedOrientation` 只做“求轴向 → 投影取 min/max → 组装四元数”；它们依赖 `math::symmetricEigen`（3×3 对称矩阵 Jacobi 分解，见 `EigenDecomposition.hpp`）与 `math::toQuaternion`（旋转矩阵 → 四元数）。
- **为什么**：`G1`、`G2` —— 特征分解是对矩阵的数值运算，不是空间实体；放进 `geometry` 会让 `math` 反向依赖它才能复用。拟合本身是形状操作，留在 `Obb`。
- **边界**：`symmetricEigen` 只承诺对称矩阵、升序谱、列正交、每列最大分量取正；handedness 不保证，需要旋转矩阵的调用方自己用叉积补第三轴（`Obb` 就是这么做的）。
- **为什么单独一个头文件**：`Matrix3x3.hpp` 只留矩阵本身，算法在 `EigenDecomposition.hpp`；只用到矩阵的翻译单元不付算法与 `<cmath>` 的编译成本，矩阵类型和求解器也各自演进（`G5`、`G10`）。
- **轴序**：PCA 按方差**降序**排列（局部 X 最大、Z 最小）。理由：主轴的惯例读法是从大到小，消费方读 `halfExtents()` 时 X 即最长边（`G4`、`G12`）；旧实现取升序的前两列（X = 最小方差）既没有声明支撑，也与“主轴”直觉相反。
- **不做**：最小体积 OBB（依赖第三方凸包旋转卡壳实现，今天没有消费者，`G8`）；`merge` 因同原因暂不提供。

### 2.8 值类型的短查询定义在头文件

- **是什么**：`Aabb` / `Obb` / `LineCurve` / `Plane` / `Ray` 的短查询与访问器（`contains`、`expandBy`、`intersectWith`、`closestPoint`、`pointAt`、`distanceToPoint`、`corners`、`toAabb`、`intersects`、`intersectTriangle`）定义在 `sdk` 头文件里；只有长算法留在 `src/`，并保留 `template class VN_GEOMETRY_API` 显式实例化。
- **布局**：每个类型的类体内只留声明与平凡访问器，定义集中在该类之后、别名之前。一个类型的公开形状因此可以一次读完（`G4`、`G12`）。
- **为什么**：`G7`、`G10` —— `geometry` 是 DLL，跨模块调用不可能内联；`graphics` 的逐三角形拾取实测命中断 7.09 → 5.99 ns、平行剔除 3.96 → 2.71 ns（独立基准，约 15–30%）。把调用点留在调用方模块里，比让调用方为一次点测试过一道库边界便宜。
- **边界**：判据是“短且纯算术”——不依赖 `EigenDecomposition.hpp`、OCCT 或匿名命名空间辅助。不是“凡是能内联的都内联”：拟合（`Obb::computePca` / `computeFixedOrientation`）、`LineCurve::intersectWith(LineCurve)`、`Plane::intersectWith(Plane, LineCurve&)` 留在 `src/`，否则公共头要拖进特征分解与 60 行推导注释，成本付给每一个 consumer（`G11`、`G10`）。
- **代价**：类上没有 `VN_GEOMETRY_API`，导出只来自显式实例化；而**库为每一个几何类型、两种精度都提供显式实例化**：有 `src/` 实现的类型在自己的 `.cpp` 里（`LineCurve` / `Obb` / `Plane`——那些 `.cpp` 是唯一能看见其非内联成员的地方），纯头文件的类型集中在 `src/Instantiations.cpp`（一份可审查的清单，同时是“每个类型都能实例化”的编译期检查）。不写 `extern template`：内联仍然在调用方发生，导出的符号是“不能内联时的兵底”，不是唯一实现。精度与实例化的策略见 `../decisions/ADR-2026-11-04-geometry-precision-and-instantiation.md`。
- **不做**：不把平行判定的 `eps * length(a) * length(b)` 改成平方比较——实测 2.71 → 2.68 ns 在噪声内（`G8`）；不为“更彻底的 header-only”引入 BVH 或批处理入口，那是 `../backlog/graphics.md` 的射线–网格遍历项，按证据另议。

## 3. 功能点

| # | 功能 | 说明 |
|---|---|---|
| F1 | 轴对齐盒 | `Aabb<T>`：min/max 角点、`empty()` 哨兵、`expandBy` / `intersectWith` / `compute(span)`。 |
| F2 | 有向盒 | `Obb<T>`：center + half-extents + 四元数朝向；`toAabb()` 返回 `Aabb<T>`。 |
| F3 | 直线 | `LineCurve<T>`：点–方向参数式；`intersectWith` 给最近点对与平行/共线标志；`closestPoint` 投影。 |
| F4 | 平面 | `Plane<T>`：点 + 法向；与 `LineCurve` 交于一点、与 `Plane` 交于 `LineCurve`。 |
| F5 | 单一名字 | `Rect3` 与 `math::Aabb*` 不存在；`geom::spatial::Aabbf/Aabbd/Aabbi` 可用。 |
| F6 | 2D 不迁 | `math::Rect2` 与 `Rect2i` 保持原位。 |
| F7 | PCA 拟合 | `Obb::computePca(span)`：协方差特征轴，最大方差方向为局部 X。 |
| F8 | 固定轴向拟合 | `Obb::computeFixedOrientation(span, x_dir, y_dir)`：调用方给 X/Y，Y 先对 X 正交化。 |

### 3.1 F1 轴对齐盒

- **行为**：默认构造是原点处的零尺寸盒（`isValid()` 为真）；`empty()` 是反向哨兵，只有 `expandBy` 之后才有效。
- **约束**（C1）：字段只按方向单调变化，`min()/max()` 原样返回存储值；`intersectWith` 的输出始终归一化。
- **要求**（R1）：`compute()` 接受任意轴序的顶点 span，空输入返回默认零盒。
- **验收**（T1）：`AabbTest.cpp` 覆盖 expandBy、相交（含不相交的缝隙盒）、`compute`、空哨兵合并。
- **注意**（N1）：`isNeg()` / `isEmpty()` / `isValid()` 是同一判定的三种问法，不要给它们不同的语义。

### 3.2 F4 平面

- **行为**：`intersectWith(LineCurve, Point3&)` 在 `|n·d| <= eps` 时返回 false（线平行于面）；`intersectWith(Plane, LineCurve&)` 在法向共线时返回 false。
- **约束**（C2）：输出线由调用方提供——`LineCurve` 没有默认构造函数（`P1`）。
- **要求**（R2）：求交的中间解在最大分量轴上做 2×2 主元选择，避免病态子矩阵。
- **验收**（T3）：`LinePlaneTest.cpp` 覆盖点交、平行无交、面面交线的方向与位置。

### 3.3 F7/F8 拟合

- **行为**：两者都返回“包围全部输入点”的盒子；轴向只决定紧致度，不决定是否包围（`C4`）。
- **PCA**：质心 → 协方差 → `symmetricEigen` → 按方差**降序**取轴：局部 X 是最大方差方向、局部 Z 最小，第三轴取叉积。半尺寸按这个次序量取，**不排序**（`N4`）。
- **固定轴向**：`x_dir` 归一化；`y_dir` 减去其在 X 上的投影后再归一化；Z 取叉积。
- **退化**（`N3`）：`x_dir` 为零 → 世界 X；`y` 与 `x` 平行 → 取与 X 最不平行的世界轴（保留调用方的 X）；所有点共享同一坐标 → 该点处零尺寸盒 + 单位朝向；空输入 → 默认盒（`R4`）。
- **验收**（T6/T7）：`ObbFitTest.cpp` 覆盖旋转盒的谱/轴向恢复、共线点集、单点与重合点、空输入、非法方向对与“全点被包围”不变量。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `src/base/geom/sdk/vine/geom/spatial/Aabb.hpp` | `Aabb<T>`、`Aabbf` / `Aabbd` / `Aabbi` |
| `src/base/geom/sdk/vine/geom/spatial/Obb.hpp` | `Obb<T>`、`Obbf` / `Obbd` |
| `src/base/geom/sdk/vine/geom/spatial/LineCurve.hpp` | `LineCurve<T>`、`LineCurvef` / `LineCurved` |
| `src/base/geom/sdk/vine/geom/spatial/Plane.hpp` | `Plane<T>`、`Planef` / `Planed` |
| `src/base/geom/src/Instantiations.cpp` | 纯头文件几何类型的显式实例化清单（`float` + `double`） |

短查询定义在头文件（2.8）；只有长算法留在 `src/` 并保留 `VN_GEOMETRY_API` 显式实例化：`LineCurve::intersectWith(const LineCurve&)`、`Plane::intersectWith(const Plane&, LineCurve&)`、`Obb::computeFixedOrientation` / `computePca`。

## 5. 约束、要求与不变量

- **C1**：`Aabb` 的字段方向不变式——min 只减、max 只增；`intersectWith` 的输出归一化。
- **C2**：输出参数由调用方构造，函数不分配、不返回引用。
- **C3**：这四个类型不参与 `Shape` 的类型判定（`obj_cast` / `ShapeType`）。
- **C4**：拟合结果永远是包围盒：轴向只影响紧致度，任何输入下 `contains()` 对每个输入点都为真（在容差内）。
- **R1**：`Aabb::compute()` 对空 span 返回默认零盒，不抛异常。
- **R2**：`Plane` 求交在退化输入上返回 `false`，不产生 NaN。
- **R3**：`math` 不 include `geometry`；`geometry` 可以 include `math`。
- **R4**：两个拟合入口对空 span 返回默认 `Obb`（原点、零尺寸、单位朝向）。
- **N1**：`empty()` 不是“空集合”，是一个反向 min/max 哨兵，只能作为累加起点。
- **N2**：`LineCurve` 的方向不必单位化，参数 `t` 的尺度和 `|direction|` 绑定。
- **N3**：PCA 在共线点集上次轴任意但确定；重复特征值只保证正交，不保证唯一；所有点重合时返回单位朝向的零尺寸盒。
- **N4**：PCA 的轴序是**方差序**而非**尺寸序**：`halfExtents()` 不保证 X ≥ Y ≥ Z（盒状点云两者一致，非均匀点云可能不一致）；要按长度选择，请调用方自行排序。旧 VObb 取 col(0)/col(1)（X = 最小方差），本仓库反过来，理由写在 2.7。

## 6. 注意事项与陷阱

- **P1**：`LineCurve` 没有默认构造函数——`Plane::intersectWith(Plane, LineCurve&)` 的输出参数必须是一个已构造的线（用任意合法值占位，函数内部整体覆写）。失败方式是编译错误，不是运行期错误。
- **P2**：`Aabb` 默认构造是**零盒**而非空盒；把它当累加器起点会把原点算进包围盒。要累加请用 `Aabb::empty()`（`N1`）。
- **P3**：`Aabb::compute()` 与 `expandBy()` 不 normalize；`intersectWith()` 的输出则总是归一化。混用两种状态会让 `isNeg()` 读数与预期相反。
- **P4**：`math::toQuaternion()` 要求输入是纯旋转（正交、det = +1）；它不检查、不修复，缩放或退化矩阵会得到任意四元数。`rotate3x3()` 是它的逆，两者用同一分支约定。

## 7. 已否决选项

- **D1**：保留 `Rect3` 作为类型名 —— 3D “Rect” 是误名，且与 `Obb` 不成对（`G1`/`G4`）。见 `../decisions/ADR-2026-11-03-geometry-primitives-ownership.md`。
- **D2**：在 `math/Rect3.hpp` 里留 `using Rect3 = geom::spatial::Aabb` 兼容别名 —— 会让 `math` 反向依赖 `geometry`（`G2`），一个版本后再删的过渡期不值得一条回边。
- **D3**：把 `Rect2` 一起搬进 `geometry` —— 唯一消费者是 GUI 布局矩形，会迫使 `appfw` 的公共头依赖 `geometry`（`G1`/`G5`）。

## 8. 测试与验收

| # | 判据 | 用例 |
|---|---|---|
| T1 | 盒的扩展、相交、`compute`、空哨兵 | `tests/test_geom_spatial/AabbTest.cpp`（`Aabb.*`） |
| T2 | 有向盒的规范化、包含、`toAabb` 包围 | `tests/test_geom_spatial/ObbTest.cpp` |
| T3 | 线–线最近点与平行/共线标志 | `LinePlaneTest.cpp`（`Line.*`） |
| T4 | 面–线、面–面求交与退化分支 | `LinePlaneTest.cpp`（`Plane.*`） |
| T5 | 2D 矩形不受影响 | `tests/test_math/Rect2Test.cpp` |
| T6 | 谱、正交基、重构与升序 | `tests/test_math/EigenDecompositionTest.cpp`（`EigenDecomposition.*`） |
| T7 | 轴角 → 矩阵 → 四元数往返 | `tests/test_math/Transform3Test.cpp`（`Transform3.*`） |

## 9. 状态与演进

### 9.1 现状

四个类型已在 `vn::geometry` 落地，`math` 下不再有对应头文件与源文件；`Mesh`、`graphics::Node`、`ChainPresets`、`ChainBuilder` 与测试已改到新名字。
`Obb` 的 PCA 与固定轴向拟合已落地；特征分解与矩阵→四元数在 `math`（`Matrix3x3.hpp` / `Transform3.cpp`），两个头文件只声明契约。

### 9.2 已知缺口

- `transformBox()` 仍是 `graphics/Node` 的自由函数：类型已经归位，变换算子还在消费者一侧，后续可评估搬入 `geometry`。
- `LineCurve` / `Plane` 只有基础分支的用例；退化容差、近共线情形未覆盖。
- 最小体积 OBB 与 `merge` 未提供（2.7）。

## 10. 参考

- 代码：`src/base/geom/sdk/vine/geom/spatial/{Aabb,Obb,Line,Plane}.hpp`、`src/base/geom/src/{Aabb,Obb,Line,Plane}.cpp`。
- 数学设施：`src/base/math/sdk/vine/math/EigenDecomposition.hpp`（`symmetricEigen`）、`Transform3.hpp`（`toQuaternion`）、`src/base/math/src/Transform3.cpp`。
- 设计：`graphics-geometry.md`（`Shape` / `Mesh` / revision 的现行设计）、`../decisions/ADR-2026-11-03-geometry-primitives-ownership.md`。
- 测试：`tests/test_geom_spatial/AabbTest.cpp`、`ObbTest.cpp`、`ObbFitTest.cpp`、`LinePlaneTest.cpp`；`tests/test_math/Rect2Test.cpp`、`EigenDecompositionTest.cpp`、`Transform3Test.cpp`。
