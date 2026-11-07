# ADR-2026-11-06：`geom` 内部设 `detail` 子树，只收与维度无关的共享实现

- 状态：已采纳
- 日期：2026-11-06
- 相关：`../design/geometry-module.md` 2.2–2.3 / 3.1、`../design/geometry-planar.md` C10 / D3、`ADR-2026-11-06-merged-geom-module.md`、`ADR-2026-11-06-math-is-the-base-layer.md`、`ADR-2026-11-06-shared-numeric-kernels-in-math.md`

## 背景

合并 `geom/planar` / `geom/spatial` 解决了“库”，没解决“共享件的位置”。合并后实测：两棵树仍有逐行相同的实现，因为能同时看见两边的地方在库内并不存在——车库里只有两棵树和各自的 `detail`。

| 对 | 2D 行 | 3D 行 | 差异行 | 差异内容 |
|---|---|---|---|---|
| `CurveProjection.hpp` | 244 | 248 | 56 | `narrowCrossing` 逐字相同；`CurveScalar` / `CurveWindow` / `resolveCurveWindow` 只差一句文档 |
| `CurveCurveDistance.hpp` | 217 | 217 | 22 | `refineCurveCurveDistance` 逐行相同（纯泛型） |
| `CurveLength.hpp` | 138 | 139 | 21 | `arcLength` / `parameterAtLength` 的实现逐行相同 |

还有一处隐蔽耦合：`CurveScalar` 这个“曲线的标量”别名住在**投影**头里，于是 `CurveCurvature` / `CurveLength` / `CurveCurveDistance` / `CurveCurveIntersection` 全都为了一个别名而 include `CurveProjection.hpp`——一次不需要投影的曲率查询也会把投影器拉进编译。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持重复 | 零改动；每棵树自足 | 三份近 200 行的拷贝，改一处必须记得改另一处（文档已经在漂：一边写“与平面侧同一内核”，另一边什么都没写） |
| B 把 `math/numeric` 与 `math/spline` 搬进 `geom` 当 `detail` | 写起来最省事：几何侧不再跨层找内核 | `math` 是底座层，搬上去以后“怎么积一个函数”只有几何能回答；imaging / mesh / brep / robotics 为了一个积分要么依赖曲线库要么再复制一份。`Quadrature` / `RootFinding` 有独立单测与在效 ADR，降级成 `detail` 会让单测去测私有代码 |
| C 在 `geom` 内建 `detail` 子树，只收维度无关的几何共享件 | 重复消失，层向不变（`geom → math` 照旧），公开名字不动 | 多一层命名空间；`detail` 必须被门禁钉住“不得命名维度”，否则会变成第三个维度树 |
| D 给每个类型加 `2`/`3` 后缀，把两套命名空间并成一个 | 单命名空间 | 48 个类型名、82 个别名、62 个头、489 处限定引用、15 个目标；且名字会谎报族关系（两个契约零继承、不可互换），`BezierCurve3` 在 CAD 里已经等于“三次”。`D3` 早已否决同一件事 |

## 决定

采用 **C**：

- 新子树 `src/base/geom/sdk/vine/geom/detail/`，include 路径 `<vine/geom/detail/...>`，命名空间 `vn::geom::detail`（`geom_global.hpp` 新增 `VN_GEOM_NS_BEGIN` 与 `VN_GEOM_DETAIL_NS_BEGIN`）。
- 内容只收**与维度无关**的东西：`CurveTraits.hpp`（`CurveScalar` / `CurveWindow` / `resolveTolerance` / `resolveCurveWindow`）、`CrossingSearch.hpp`（`narrowCrossing`）、`CurveMeasure.hpp`（`arcLengthBetween` / `parameterCovering`）、`CurvePairSearch.hpp`（`refineCurvePair`）。
- **公开名字不动**：`geom::planar::arcLength` / `geom::spatial::arcLength` 仍是唯一文档化的入口，各自解析窗口与容差后转发（`G4`：契约留在各自命名空间，实现只留一份）。
- **门禁钉住方向**：`scripts/check_geom_layering.py` 从“两棵互不 include”扩成三条——`geom/planar` 不得 include `geom/spatial`、反之亦然、`geom/detail` 不得 include 任一方（命名了维度的 helper 就不再共享）。
- 判定下沉与否，看两份拷贝的差异：只有类型名与文档 → 下沉；差异在数学上 → 各留一份。因此 `signedCurvature`（2D 有符号）与 `curvature`（3D 模长）保持两份，`closestPointOnCurve` 也保持两份（结果类型带 `Point2` / `Point3`）。

判据：`G1`（共享件一份）、`G2`（共享件仍住在能向下看的那一层，几何不下沉到 `math`、`math` 不上浮到几何）、`G5`（几何级改动不再需要改两处）、`G7`（复杂度被吸收在一个 `detail` 里，而不是变成调用方的模板参数或回调）。`G8` 的张力：`geom/detail` 只有四个头、每个实现一份，不为“将来可能共享”预建任何东西。

## 后果

- 好的：`narrowCrossing`、窗口规则、容差规则、两曲线细化、弧长与反解各一份；`CurveCurvature` 不再为别名而拉进投影器；`CurveLength` 不再 include `CurveProjection.hpp`。
- 代价：`geom::detail::` 这个限定名比 `detail::` 长；`geom` 里现在同时存在“树局部 `detail`”（`geom::planar::detail` / `geom::spatial::detail`，装文件内 helper）与“模块共享 `detail`”（`vn::geom::detail`），两者靠限定名区分。
- 不做：不把 `math` 的 `numeric` / `spline` 搬进来（选项 B）；不给类型加 `2`/`3` 后缀（选项 D，`D3` 已有）。
- 边界：`geom/detail` 里的名字不是公开接口，不承诺 ABI 与稳定性；要公开一个共享能力，先在两个命名空间各起一个公开入口。
- 后记（同日）：上表 B 后来被完整采纳——`spline` 的节点模型与基函数、纯 Bernstein 求值、以及 `math/numeric` 的求根与积分全部搬进了本 ADR 建立的 `geom/detail`，`math` 只剩词汇。那两条针对 `numeric` 的反对意见（“底座层要能应答积分”）仍成立，只是被明确记为代价：出现第二个消费者时升独立模块，不在 `geom` 里将就。见 `ADR-2026-11-06-math-keeps-only-vocabulary.md`。

## 证据

- 新头：`geometry-planar.md` 2.5 列出的四个文件；`geom_global.hpp` 的三个命名空间宏与一处 `Continuity` 重导出。
- 删除：两棵树各删掉 `narrowCrossing`（含文档）、`CurveScalar` / `CurveWindow` / `resolveCurveWindow`、`refineCurveCurveDistance`、`arcLength` / `parameterAtLength` 的实现体——`CurveProjection.hpp` 244→141 / 248→145 行，`CurveCurveDistance.hpp` 217→150 行（两侧），`CurveLength.hpp` 138→120 / 139→121 行。剩余差异全部是文档。
- 门禁：`python scripts/check_geom_layering.py` → `0 finding(s) in 66 file(s)`。
- 验证：Debug 构建通过；`test_geom_planar` 138、`test_geom_spatial` 193、`test_math` 56 全绿（行为未变，纯搬迁）。
