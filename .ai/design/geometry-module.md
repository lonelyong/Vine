# geometry 模块：一个模块、两个契约、一份共享实现

> **状态**：现行
> **最后核对**：2026-11-06
> **范围**：`src/base/geom` 的物理形态与命名规则——目录与命名空间、`vn::Geom` 目标、`geom/planar` 与 `geom/spatial` 两个契约子树、共享实现子树 `geom/detail`、模块级词汇、隔离门禁、以及"什么放哪里"的判据。不含任何具体曲线/曲面/实体的契约与算法（见 `geometry-planar.md`、`geometry-kinds.md`、`geometry-primitives.md` 等）。
> **权威来源**：`src/base/geom/CMakeLists.txt`、`src/base/geom/sdk/vine/geom/geom_global.hpp`、`src/base/geom/sdk/vine/geom/Continuity.hpp`、`src/base/geom/sdk/vine/geom/{planar,spatial,detail}/`、`scripts/check_geom_layering.py`、`tests/test_geom_planar/ModuleContractTest.cpp`。
> **待办**：无

## 1. 概要设计

### 1.1 问题与背景

几何层有过三种形态：两个独立目标（`vn::Geom3D` 是 SHARED、`vn::Geom2D` 是 INTERFACE）；合并成一个模块后两棵树仍各存一份 `narrowCrossing`、窗口规则与弧长实现；共享件下沉到 `geom/detail` 之后，两个契约目录还是模块目录的**兄弟**，于是 `Continuity` 要靠两句 `using` 重导出、共享件要靠 `geom::detail::` 全限定。2026-11-06 收敛成现在这一形态：一个库、模块目录拥有全部子树、一个 global。

### 1.2 目标

1. 平面与空间是同一领域的两个契约，消费者只链一个库（`G6`）。
2. 与维度无关的实现只有一份，住在能同时看见两个契约的地方（`G1`）。
3. 契约之间的隔离**可执行**，不是口头约定（`G11`）。
4. 目录、命名空间、类型名三者一致：`geom/planar` ↔ `vn::geom::planar`（`G4`、`G12`）。

### 1.3 非目标

- 不做"2D 与 3D 的统一泛型"：两个契约并列、零继承、不可互换（`R1`）。
- 不给类型加维度标记：维度已经在命名空间里（`G4`）。
- 不为目录整齐再重排：重排必须能消除税，否则不做（`G8`）。

### 1.4 结论

**`src/base/geom` 是一个模块、一个库 `vn::Geom`、一个 global；`vn::geom` 之下并列三棵子树——`planar`（平面曲线契约）、`spatial`（空间几何）、`detail`（与维度无关的共享实现）——外加模块根上的词汇 `Continuity`。契约可以 include `detail`，不得互相 include，`detail` 不得命名维度；五条规则由脚本门禁保证。**

## 2. 设计要点

### 2.1 一个模块、一个库、一个 global

- **是什么**：`src/base/geom` 是一个 CMake 单元，产出 `vn::Geom`（编译定义 `VN_GEOM_LIB`，实体标 `VN_GEOM_API`）；SDK 根是 `sdk/`，include 路径是 `<vine/geom/planar/...>`、`<vine/geom/spatial/...>`、`<vine/geom/detail/...>`。目标为 SHARED，因为空间侧有显式实例化与三个非模板实现（`src/Instantiations.cpp`、`LineCurve.cpp`、`Obb.cpp`、`Plane.cpp`）；平面侧是 header-only 模板，随同一个库发布、不导出符号。
- **为什么**：`G6`、`G10` —— 两个契约共用一个编译定义、一个 API 宏、一份符号；分成两个目标时任何一次共享都要靠"第三个模块"或复制。
- **边界**：`geom` 只依赖 `vn::Math` 与 `vn::Core`；`math` 不认识几何（`math-base-layer.md`）。

### 2.2 目录与命名空间一一对应

| 目录 | 命名空间 | 内容 | 是不是 API |
|---|---|---|---|
| `sdk/vine/geom/` | `vn::geom` | `geom_global.hpp`（宏）、`Continuity.hpp`（光滑度阶梯） | 是 |
| `sdk/vine/geom/planar/` | `vn::geom::planar` | 平面曲线契约 `Curve<T>`、九个类型、六个族级算法（17 个头） | 是 |
| `sdk/vine/geom/spatial/` | `vn::geom::spatial` | 曲线、曲面、实体、射线、`Obb` / `Aabb`（47 个头） | 是 |
| `sdk/vine/geom/detail/` | `vn::geom::detail` | 与维度无关的共享实现（8 个头） | **不是** |

`geom_global.hpp` 提供 `VN_GEOM_API` 与三个命名空间宏（`VN_GEOM_NS_BEGIN`、`VN_GEOM_PLANAR_NS_BEGIN`、`VN_GEOM_SPATIAL_NS_BEGIN`、`VN_GEOM_DETAIL_NS_BEGIN` 及对应 `_END`）。

**嵌套带来的两个直接结果**：

- **模块根的词汇不需要重导出**。`Continuity` 在 `vn::geom`，两个契约是它的子命名空间，非限定名靠封闭命名空间查找找到——`geom_global.hpp` 里没有一句 `using`。
- **共享件必须写全 `geom::detail::`**。C++ 的近邻规则让不带限定的 `detail::` 在两个契约里解析到**各自的** `planar::detail` / `spatial::detail`（六个头文件声明了它们），所以共享件的限定不是风格，是正确性（见 P1）。

### 2.3 什么放哪里：三步判定

1. **是不是公开契约的一部分？** 是 → 对应契约子树，进 `vn::geom::planar` / `vn::geom::spatial`。
2. **两个契约是不是都需要它？** 否 → 留在本契约（必要时放进本契约的 `detail`）。
3. **需要，而且与维度无关？** 是 → `geom/detail/`（`vn::geom::detail`）。判据：只依赖 `C::value_type` 与 `evaluate` / `derivative`，签名里不出现 `Point2` / `Point3`。

**判定要能拒绝**：并排看两份拷贝，差异只有类型名与一句文档 → 下沉；差异在数学上 → 各留一份（`signedCurvature` 有符号 vs `curvature` 模长；`closestPointOnCurve` 的结果类型带 `Point2` / `Point3`）。

### 2.4 命名规则

- **类型名不带维度标记**：`LineCurve` / `Curve`，不是 `LineCurve2d` / `Curve2`；维度由命名空间承载。精度后缀仍是 `f` / `d`（`LineCurved`、`CircleCurvef`）。
- **`detail` 不是 API**：不承诺稳定与 ABI；要公开一个共享能力，先在两个契约命名空间各起一个公开入口。`planar::arcLength` / `spatial::arcLength` 就是这样一层薄壳（解析窗口与容差后转发）。
- **同名的公开入口按契约分属两个命名空间**：`arcLength`、`parameterAtLength`、`closestPointOnCurve`、`curveToCurveDistance` 两个契约各一份（见 P2）。

### 2.5 依赖方向与门禁

- **方向**：`planar` / `spatial` → `detail` → `math`；两个契约之间没有边。
- **门禁五条**（`scripts/check_geom_layering.py`，当前 70 个文件 0 处违规）：
  1. `planar` 不得 include `spatial`；
  2. `spatial` 不得 include `planar`；
  3. `detail` 不得 include 任一个契约；
  4. `detail` 不得**命名** `Point2` / `Point3` / `Vector2` / `Vector3`——按 include 查不住的维度泄漏，按名字查住；
  5. 几何头的 include 域：带路径的角括号只能是 `vine/geom/` 或 `vine/math/`，引号只能是同目录——即"唯一的边是几何 → 几何、几何 → 数学"。
- **为什么用脚本而不是链接**：三个子树上在同一个 target 内，编译器拦不住；两个 target 的老办法会让"一个模块"这个事实消失（`ADR-2026-11-06-merged-geom-module.md`）。
- **门禁必须能失败**：新增规则后要用一条临时违规头文件反向验证，再删掉它确认回到 0。

### 2.6 与 `math` 的关系

- `math` 只提供数学词汇（值类型、常量、谓词、概念），不认识几何；几何侧的数值链（求根 → 容差换算 → 积分/反解、样条求值与节点模型）全在 `geom/detail`。
- 代价明确接受：任何非几何模块要用积分或求根，只能依赖 `vn::Geom`、复制一份、或把内核升成独立模块；后者是记账的反转路径（`ADR-2026-11-06-math-keeps-only-vocabulary.md`）。

## 3. 功能点

| 编号 | 功能点 | 内容 | 状态 |
|---|---|---|---|
| F1 | 单库布局 | `vn::Geom` 承载两个契约与共享实现，include 路径三段分工 | 已落地 |
| F2 | 一个 global | `vn::Geom` 的 API 宏、四个命名空间宏（根 + 两个契约 + `detail`） | 已落地 |
| F3 | 共享实现子树 | 8 个 `geom/detail/*.hpp`，两个契约都可 include | 已落地 |
| F4 | 隔离门禁 | `check_geom_layering.py` 五条规则 | 已落地 |
| F5 | 命名规则 | 类型不带维度、精度后缀 `f`/`d`、`detail` 非 API | 已落地 |

### 3.1 F3 共享实现子树（当前清单）

`CurveTraits.hpp`（`CurveScalar` / `CurveWindow` / `resolveTolerance` / `resolveCurveWindow`）、`CrossingSearch.hpp`（`narrowCrossing`）、`CurveMeasure.hpp`（`arcLengthBetween` / `parameterCovering`）、`CurvePairSearch.hpp`（`refineCurvePair`）、`BSplineBasis.hpp`（相异节点 + 重数的节点模型与 Cox–de Boor 基函数）、`BezierBlend.hpp`（de Casteljau 与差商）、`Quadrature.hpp`（自适应 Simpson）、`RootFinding.hpp`（括号内求根、单调量反解）。

**验收**（T1、T3）：两套几何测试全绿；门禁 0 违规；契约内的引用点无一指向另一个契约。

## 4. 公开接口

| 头 | 公开内容 |
|---|---|
| `geom_global.hpp` | `VN_GEOM_API`、`VN_GEOM_NS_BEGIN` / `VN_GEOM_PLANAR_NS_BEGIN` / `VN_GEOM_SPATIAL_NS_BEGIN` / `VN_GEOM_DETAIL_NS_BEGIN`（及对应 `_END`） |
| `Continuity.hpp` | `vn::geom::Continuity`（有序枚举）；两个契约靠封闭命名空间查找直接使用它 |
| `planar/*.hpp` | `vn::geom::planar`：`Curve<T>` 与九个类型、六个族级算法 |
| `spatial/*.hpp` | `vn::geom::spatial`：曲线/曲面/实体/射线/`Obb`/`Aabb` 与其族级算法 |
| `detail/*.hpp` | `vn::geom::detail`：**不是** API |

## 5. 约束、要求与不变量

- **C1**：一个模块、一个 CMake 单元、一个目标 `vn::Geom`、一个编译定义 `VN_GEOM_LIB`、一个 global 头。
- **C2**：目录与命名空间一一对应：`planar/`↔`vn::geom::planar`、`spatial/`↔`vn::geom::spatial`、`detail/`↔`vn::geom::detail`、模块根↔`vn::geom`。
- **C3**：与维度无关的共享实现只在 `geom/detail/`；只对本契约有意义的 helper 留在本契约的 `detail`。
- **C4**：类型名不带维度标记；精度后缀 `f` / `d`；`detail` 里的名字不是 API。
- **C5**：依赖方向 `契约 → detail → math`，没有回边，两个契约之间没有边。
- **C6**：五条门禁必须全绿；`geom/detail` 里不得出现 `Point2` / `Point3` / `Vector2` / `Vector3`（名字与 include 都算）。
- **C7**：模块根上的名字**不得**在两个契约里重导出——嵌套命名空间已经提供可见性，多一句 `using` 就是第二个事实来源。
- **C8**：新增一棵子树（第三类几何）时，先按 2.3 判定，再决定它是目录、命名空间还是新模块。
- **R1**：两个契约并列、零继承、不可互换；`planar::Curve<T>` 不是 `spatial::Curve<T>` 的特化。
- **R2**：几何头只 include `vine/geom*`、`vine/math*` 与标准库。
- **N1**：两个契约的 `continuity()` 都返回 `vn::geom::Continuity` 这一个类型（`ModuleContractTest` 用 `static_assert` 盯着）。
- **N2**：同名的公开入口（`arcLength` 等）按契约分属两个命名空间，签名可以相同。

## 6. 注意事项与陷阱

- **P1 不带限定的 `detail::` 会解析到契约自己的那个**：`planar/CurveCurveIntersection.hpp`、`spatial/{Aabb,CurveSurfaceIntersection,RaySurfaceIntersection,SolidDetail,SurfaceProjection}.hpp` 声明了同名的树私有 `detail`；只要其中任何一个进了同一个 TU，契约里的 `detail::X` 就解析到那一层而不是 `geom::detail`。**共享件一律写全 `geom::detail::`**（实测：写成裸 `detail::` 立刻 C2039）。
- **P2 同名公开函数不能并进一个命名空间**：`closestPointOnCurve` 与 `curveToCurveDistance` 两个契约**参数表相同、返回类型不同**，C++ 不能按返回类型重载；`arcLength` / `parameterAtLength` 虽然返回类型相同，但并起来只能靠互斥的 requires 子句区分调用——那是隐式机制（`G11`）。这就是维度不能塞进类型名、把两个契约并成一个命名空间的原因。
- **P3 精度后缀与维度后缀抢位置**：我们已有 `d` / `f` 作为标量后缀（`Vec2d`、`Rect2d`、`LineCurved`），行业里给维度用的两种拼写（CGAL 的 `_2`、OCCT 的 `2d`）在这里都会与精度撞车；尾缀 `2` 又会与 `BezierCurve::degree()` 的"二次"语义混。因此维度只进命名空间。
- **P4 改库内布局前先看数字**：一次目录与命名重排要动 850 处代码引用 + 265 处文档引用（本次实测）。要么它消除一项长期的税（例如本次消掉两句重导出与一处兄弟命名空间陷阱），要么别做。

## 7. 已否决选项

- **D1** 两个目标（`vn::Geom2D` INTERFACE + `vn::Geom3D` SHARED）：`G1`、`G6`——任何共享都要靠第三个模块或复制。见 `../decisions/ADR-2026-11-06-merged-geom-module.md`。
- **D2** 两个契约各自复制共享件：`G1`、`G5`——差异只在类型名时，两份拷贝就是两次维护。见 `../decisions/ADR-2026-11-06-geom-detail-subtree.md`。
- **D3** 保持"`geom2d` / `geom3d` 是 `geom` 的兄弟目录"的旧形态（重导出 + 全限定）：`G11`、`G4`——模块目录不包含模块内容、`Continuity` 要靠两句 `using`、`detail` 要靠前缀区分，且目录与命名空间两套话。见 `../decisions/ADR-2026-11-06-geom-contracts-nested-under-module.md`。
- **D4** 平铺到 `geom/` 并只给平面契约加标记（`Curve2` / `CircleCurve2`）：`G4`、`G11`——4 对同名公开函数（其中两对不能重载）、`LineCurved` 与 `LineCurve2d` 的 `d` 冲突、无标记的类型变成"默认是空间"的不成文约定。
- **D5** 平铺并把两个契约都标上（`Curve2` / `Curve3`）：`G4`、`G1`——15 + 47 个类型与 4 个函数同时改名，且名字谎称"同一个族的两个维度"。
- **D6** 让 `math` 继续承载求根/积分与样条表示约定：`G1`、`G4`——`math` 会一直有两个只被几何使用的子目录。见 `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`。
- D4–D5 的完整对照与行业证据（OCCT / CGAL / Eigen / GLM）见 `../decisions/ADR-2026-11-06-geom-contracts-nested-under-module.md`。

## 8. 测试与验收

| 编号 | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 两个契约在同一库内正常工作 | `test_geom_planar`（140 例）、`test_geom_spatial`（213 例）全绿 |
| T2 | 隔离：无跨契约 include、`detail` 不命名维度、include 域 | `python scripts/check_geom_layering.py` → `0 finding(s) in 70 file(s)`，且临时违规头文件能让它报错 |
| T2b | 模块级不变量：两个契约的 `continuity()` 返回同一个 `vn::geom::Continuity`，且阶梯有序 | `tests/test_geom_planar/ModuleContractTest.cpp` 的三条 `static_assert` 与 `TheLadderIsOrderedBySmoothness` |
| T3 | 共享件真的只有一份 | 两个契约上同名实现的差异只剩文档（逐行 diff 实测：`CurveLength` / `CurveCurveDistance` / `CurveProjection` 的实现体已无差异） |
| T4 | 头文件卫生与公开面文档 | `check_include_hygiene.py`、`check_public_docs.py` 均 0 发现 |
| T5 | 文档结构 | `check_ai_docs.py --strict`、`check_design_docs.py` 均 0 发现 |

## 9. 状态与演进

- **现状**：F1–F5 全部落地；`vn::Geom` 单一目标；`geom/{planar,spatial}` 共 64 个头、`geom/detail` 8 个；五条门禁 0 违规；两套几何测试共 353 例全绿。
- **反转条件**：① 出现**第三棵子树**（多边形、网格几何等）——按 2.3 判定它属于目录、命名空间还是新模块；② 出现非几何的求根/积分/Bernstein 消费者——把内核升成独立模块，几何侧只换 include（见 `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`）。
- **不做**：类型名带维度标记（D4–D5）；除非出现第二条能证明它能消除的税。

## 10. 参考

- 代码：`src/base/geom/CMakeLists.txt`、`sdk/vine/geom/geom_global.hpp`、`sdk/vine/geom/Continuity.hpp`、`sdk/vine/geom/detail/`（8 个头）、`sdk/vine/geom/planar/`（17）、`sdk/vine/geom/spatial/`（47）。
- 门禁：`scripts/check_geom_layering.py`。
- 决策：`ADR-2026-11-06-merged-geom-module.md`、`ADR-2026-11-06-geom-detail-subtree.md`、`ADR-2026-11-06-geom-contracts-nested-under-module.md`、`ADR-2026-11-05-planar-curve-contract.md`、`ADR-2026-11-06-spline-model-moves-to-geom.md`、`ADR-2026-11-06-math-keeps-only-vocabulary.md`。
- 相关设计：`geometry-planar.md`（平面曲线的契约与九个类型）、`geometry-kinds.md`（空间曲线/曲面族）、`geometry-primitives.md`（空间值类型）、`math-base-layer.md`（下层只有词汇）。
