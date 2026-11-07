# ADR-2026-11-06：`math` 只留数学词汇，求根/积分/Bernstein 求值移入 `geom/detail`

- 状态：已采纳（取代 `ADR-2026-11-06-shared-numeric-kernels-in-math.md`；`ADR-2026-11-06-spline-model-moves-to-geom.md` 的“Bernstein 留在 `math`”那一半也被本 ADR 取代）
- 日期：2026-11-06
- 相关：`../design/math-base-layer.md`、`../design/geometry-module.md` 2.6 / 7、`ADR-2026-11-06-geom-detail-subtree.md`、`ADR-2026-11-06-spline-model-moves-to-geom.md`

## 背景

同一天早些时候，B 样条的节点模型与 `Continuity` 从 `math` 移入几何模块，判据是“签名里有没有我们自己的数据模型”。那次移动留下了两个残余：

- `math/numeric/`（`adaptiveSimpson`、`rootInBracket`、`parameterReaching`）：签名里**没有**几何模型，按上一条判据应当留在 `math`。
- `math/spline/BezierBlend.hpp`（`deCasteljau`、`differences`）：同样没有模型。

于是 `math` 变成了“18 个词汇头 + 两个只被几何使用的子目录”：它们没有几何模型，却也没有第二个消费者——`brep`、`robotics`、`meshio`、`graphics`、`models`、`loaders` 里没有任何一处求根、积分或 Bernstein 求值的代码。

**这里的判断已经换了一条轴**：不是“签名里有没有我们的模型”（`numeric` 与 `BezierBlend` 都没有），而是**“这个能力今天由谁消费、以及它服务于谁的概念”**。求根与积分确实是通用数学，但在本仓库里它们唯一的用途是“几何容差 → 参数容差”与“沿曲线积分速度”；把整条几何数值链留在同一个模块内，比把三个函数留在底座层更值得，代价是底座层少了一个能力。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持 `math/numeric` 与 `math/spline` | 通用能力留在底座层，未来任何模块可直接用 | `math` 里始终有两个“只有几何在用”的子目录；读者必须先读 ADR 才知道为什么它们在那里 |
| B 全部移入 `geom/detail` | `math` 只剩词汇，名字与内容一致；几何的数值链（求根 → 容差换算 → 弧长/最近点/求交）全在一个模块内，改动不必跨模块 | 任何将来的非几何消费者（新的求解器、轨迹插值、重采样）要为一次积分依赖 `vn::Geom`；`Quadrature` / `RootFinding` 从公开 API 降为 `detail` |
| C 建独立的 `vn::Numeric` 模块（`math` 与 `geom` 之间） | 通用过程有自己的一层，两个消费者都能到达 | 为三个头开一个模块；且 `math` 与 `geom` 之间多一条层级，`geom` 要链两个库 |

## 决定

采用 **B**：

- `Quadrature.hpp`、`RootFinding.hpp`、`BezierBlend.hpp` → `src/base/geom/sdk/vine/geom/detail/`，命名空间 `vn::geom::detail`。
- `math` 只剩 18 个根层头（值类型、常量与谓词、概念、`EigenDecomposition`、`math_global`），**没有任何子目录**。
- 三件都按 `detail` 处理：消费者是几何的曲线/曲面与 `geom/detail` 自己的 helper（`narrowCrossing` 用 `rootInBracket`，`arcLengthBetween` / `parameterCovering` 用 `adaptiveSimpson` / `parameterReaching`，Bezier 曲线与曲面用 `deCasteljau` / `differences`）。
- 迁移：`tests/test_math/{QuadratureTest,RootFindingTest}.cpp` → `tests/test_geom_spatial/`（`test_math` 只链 `vn::Math`，不再认识这些头）。

判据：`G1`（每个能力一个家）、`G5`（几何的数值链改动落在几何模块内）、`G6`（`math` 与 `geom` 各只有一种内容：词汇 / 几何），`G4`（名字不再需要文档辩护）。`G2` 的张力**如实记录并在下面写成代价**：这次是把一个通用能力从底座层上移，反转了同日早些时候 `ADR-2026-11-06-shared-numeric-kernels-in-math.md` 的判断。选 B 的理由是“今天没有第二个消费者，而两个家比一个家更贵”，不是“通用性不重要”。

## 后果

- 好的：`math` 的名字与内容终于一致；几何的数值链不再跨模块（求根、容差换算、积分、反解都在 `geom/detail`）；`narrowCrossing` 与它的内核现在同命名空间，调用点可以不加限定名。
- **代价（明确接受）**：任何非几何模块要用积分或求根，只有三条路——依赖 `vn::Geom`、复制一份、或者那时再把内核上移（选项 C）。第三条是本次决定的自然反转路径：**出现第二个消费者时，把 `Quadrature` / `RootFinding` 升成独立模块，而不是在 `geom` 里将就**。
- 代价（标签）：三个头现在是 `detail`，不承诺稳定性。它们仍有独立的头文件与测试，但"公开可依赖"这句话不再成立。
- 迁移清单：`<vine/math/numeric/*.hpp>`、`<vine/math/spline/BezierBlend.hpp>` → `<vine/geom/detail/*.hpp>`；`vn::math::numeric::` / `math::spline::` → `geom::detail::`（几何侧 9 个头、83 处）；两个测试文件换套件。
- 反转条件：出现任何非几何的求根/积分/Bernstein 消费者。那时按选项 C 处理，`geom` 侧只需换 include，不必改签名。

## 证据

- `src/base/geom/sdk/vine/geom/detail/`：`Quadrature.hpp`、`RootFinding.hpp`、`BezierBlend.hpp`（新增）与先前的 5 个头共 8 个；`src/base/math/sdk/vine/math/` 只剩 18 个头、无子目录。
- `math` 的消费者统计：`adaptiveSimpson` / `rootInBracket` / `parameterReaching` / `deCasteljau` / `differences` 的非几何引用数为 **0**。
- 测试：`test_math` 49 → 36（7 个文件），`test_geom_spatial` 200 → 213，`test_geom_planar` 138 不变；合计 387 全绿。
- 门禁：`check_geom_layering.py` 70 个几何头 0 处违规（`geom/detail` 不 include 任一棵维度树）。
