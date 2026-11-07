# ADR-2026-11-06：两个契约下沉为模块的子命名空间

- 状态：已采纳（取代 `ADR-2026-11-06-geom-tree-layout-and-naming.md` 的"维持兄弟目录"决定）
- 日期：2026-11-06
- 相关：`../design/geometry-module.md` 2.2 / 2.5 / 6、`ADR-2026-11-06-merged-geom-module.md`、`ADR-2026-11-06-geom-detail-subtree.md`

## 背景

同一天早些时候，`geom/detail` 下沉之后我评估过一次重排，结论是"维持 `geom2d` / `geom3d` 作为 `geom` 的兄弟目录"，理由是"重排只剩名字好看，而代价是 689 处引用"。那次评估把收益写成"删掉两句 `using`"，**估错了**：

- `geom2d` 是 `vn` 的子命名空间而不是 `vn::geom` 的，所以 `Continuity` 必须用两句 `using` 重导出，`detail` 必须写全 `geom::detail::`。这是一条**每天都在收的税**，不是一次性的整齐问题。
- 目录说 `vine/geom2d`、命名空间说 `vn::geom2d`、模块目录 `vine/geom` 里只有两个头——三处不一致（`G4`、`G12`）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 维持兄弟目录 | 零改动 | 重导出 + 兄弟陷阱 + 目录/命名空间不一致永久保留 |
| B 只把命名空间嵌套、目录不动 | `Continuity`/`detail` 的可见性改善 | 目录与命名空间说两套话，比现状更差 |
| C 目录与命名空间一起下沉：`geom/{planar,spatial,detail}` + `vn::geom::{planar,spatial,detail}` | 三处一致；重导出归零；模块目录拥有模块内容 | 850 处代码引用 + 265 处文档引用机械改写 |

## 决定

采用 **C**：

- `sdk/vine/geom2d/` → `sdk/vine/geom/planar/`，`vn::geom2d` → `vn::geom::planar`；`sdk/vine/geom3d/` → `sdk/vine/geom/spatial/`，`vn::geom3d` → `vn::geom::spatial`。
- 宏改名：`VN_GEOM2D_NS_BEGIN/END` → `VN_GEOM_PLANAR_NS_BEGIN/END`，`VN_GEOM3D_NS_*` → `VN_GEOM_SPATIAL_NS_*`；`planar` / `spatial` 由 `VN_GEOM_NS_BEGIN` 展开而来，所以三者是真正的嵌套。
- **`geom_global.hpp` 不再重导出任何东西**：`Continuity` 在 `vn::geom`，两个契约是它的子命名空间，非限定名由封闭命名空间查找找到。`Continuity.hpp` 改为 include `geom_global.hpp` 并使用 `VN_GEOM_NS_BEGIN`——`geom_global.hpp` 不再 include 它，所以没有包含环。
- 测试套件同步改名：`tests/test_geom_planar` / `tests/test_geom_spatial`。
- 门禁的路径常量改为 `geom/{planar,spatial}`，五条规则内容不变，反向验证一次（临时违规头文件 → 2 条 finding → 删除后回到 0）。

判据：`G11`（去掉一处陷阱与两句重导出，每天少写两次限定）、`G4`（目录、命名空间、类型名一致）、`G12`（模块目录拥有模块内容）。

## 后果

- 好的：`Continuity` 在两个契约里是普通名字，没有重导出这回事；目录与命名空间一一对应；include 路径以模块名开头（`<vine/geom/planar/...>`）；`geom_global.hpp` 缩成"宏 + 说明"。
- **代价（实测）**：850 处代码引用 + 265 处文档引用机械改写；`tests/` 的两个套件目录与 `TEST_NAME` 改名；五份 ADR 的后记/取代标注。
- **一项预期收益落空（记录在案）**：原估"192 处 `geom::detail::` 里 177 处可以简写成 `detail::`"。编译立刻证伪——六个契约头文件声明了同名的 `planar::detail` / `spatial::detail`，只要其中任何一个进了同一个 TU，裸 `detail::` 就解析到那一层（`error C2039`）。所以共享件**仍然必须写全 `geom::detail::`**，这次重排没有缩短它。这是 `detail` 一词在同模块内承担两种角色（模块共享 / 契约私有）的代价。
- 反转条件：没有。目录与命名空间已经一致，再往下的变化只可能是"第三棵子树"或"内核升独立模块"。

## 证据

- 目录：`src/base/geom/sdk/vine/geom/{planar,spatial,detail}`（17 / 47 / 8 个头）+ 模块根 2 个头。
- 门禁：`python scripts/check_geom_layering.py` → `0 finding(s) in 70 file(s)`；临时探针头文件 → `2 finding(s) in 71 file(s)`，退出码 1。
- 测试：`test_math` 36、`test_geom_planar` 140、`test_geom_spatial` 213，共 389 例全绿。
- 改写量：`_nest_geom.py` 报 133 个源文件；命名空间/宏/路径替换统计为 include 路径 195、宏 132、限定名 493；文档侧 37 个文件。
