# ADR-2026-11-06：`geom/planar` 与 `geom/spatial` 合并为一个 `vn::Geom` 模块

- 状态：已采纳
- 日期：2026-11-06
- 相关：`../design/geometry-planar.md` 2.5、`ADR-2026-11-05-planar-curve-contract.md`、`ADR-2026-11-06-math-is-the-base-layer.md`、`ADR-2026-11-06-shared-numeric-kernels-in-math.md`

## 背景

两个几何模块一直是两个 CMake 单元：`vn::Geom3D` 是 SHARED（有显式实例化与三个非模板实现），`vn::Geom2D` 是 INTERFACE（17 个头、无 `src/`、零导出符号）。消费者也极不对称：`vn::Geom3D` 有 15 个 CMake 目标在用，`vn::Geom2D` 只有它自己的测试。

这种“兄弟目录”的代价在最近几轮暴露出来：`detail::narrowCrossing` 与三对族级算法（弧长、投影、两曲线距离）在两个模块里各有一份，因为**能同时看见两边的位置不存在**——`math` 不认识曲线，两个兄弟模块之间也不该互相依赖。要么复制，要么合并。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 保持两个模块，重复继续存在 | 零改动；2D 保持 INTERFACE、无 ABI | 共享件只能复制；每加一条几何级共享逻辑就多一份漂移风险 |
| B 合并为 `src/base/geom` + 单库 `vn::Geom`，SDK 树仍分 `geom/planar` / `geom/spatial`，两树共用一个 `geom/geom_global.hpp` | 共享件有家；一个 target；一个 API 宏；**include 路径与命名空间零改动** | 失去编译器强制的 2D/3D 隔离，改由脚本门禁；2D 消费者要链 `libGeom`；文档路径与 baseline 要一次性更新 |
| C 新建一个位于两者之下的 `vn::GeomBase` 模块放共享件 | 保住 target 边界 | 为两个头开一个新模块，且共享件仍不能使用曲线类型本身 |

## 决定

采用 **B**：

- 目录 `src/base/geom/{sdk/vine/geom/planar, sdk/vine/geom/spatial, sdk/vine/geom, src}`；目标 `vn::Geom`，编译定义 `VN_GEOM_LIB`。
- **一个 global**：`sdk/vine/geom/geom_global.hpp` 提供 `VN_GEOM_API`、`VN_GEOM_PLANAR_NS_BEGIN/END`、`VN_GEOM_SPATIAL_NS_BEGIN/END`，并在两个命名空间各重导出一次 `Continuity`。原来的两个模块各有一个 global 头（`geom/planar_global.hpp` / `geom/spatial_global.hpp`），现已删除；64 个头改成 `#include <vine/geom/geom_global.hpp>`（参考 `appfw_global.hpp` 的形态：一个模块一个 global，多个命名空间写在同一文件里）。 <!-- drift-ok -->
- **隔离改由门禁保证**：新增 `scripts/check_geom_layering.py`，`geom/planar` 的头不得 include `geom/spatial`，反之亦然。原先是“两个 target 互相链接不了”，现在是“脚本报错”，可执行、可进 CI。

判据：`G1`（共享件一份）、`G5`（几何级改动落在一个模块内）、`G6`（模块形态统一：一个库、一个 global）、`G10`（消费者链接面收窄；2D 不导出符号，无 ABI 损失）。`G8` 的张力如实记录：合并当下的直接收益只有 `narrowCrossing` 与三对算法的去重，属于结构性收益，不是功能驱动——这一步是用户明确要求的方向，因此执行，但代价与替代方案都在此存照。

## 后果

- 好的：2D/3D 可以共享实现；消费者统一链 `vn::Geom`；一个 API 宏；`check_public_docs` baseline 与几何无关（几何头全部有文档），因此移动没有产生新的欠账。
- 坏的 / 代价：**隔离从编译器降级为约定 + 脚本**；`vn::Geom2D` 这个名字消失（消费者 `tests/test_geom_planar` 改为链 `vn::Geom`）；`geometry-planar.md` 2.5 原先写的“header-only / INTERFACE 目标”不再成立，已改写为“一个模块、一个库、一个 global，两套命名空间”。
- 迁移：`vn::Geom3D` / `vn::Geom2D` → `vn::Geom`（16 个 CMakeLists）；`#include "geom2d_global.hpp"` / `"geom3d_global.hpp"` → `#include <vine/geom/geom_global.hpp>`（64 个头）；`VN_GEOM2D_API` / `VN_GEOM3D_API` → `VN_GEOM_API`。
- 不做：重命名命名空间（`vn::geom::planar` / `vn::geom::spatial` 保持不变——它们是概念名，不是模块名）。

## 证据

- `src/base/geom/CMakeLists.txt`：单 target、`VN_GEOM_LIB`、以及“隔离由脚本保证”的注释。
- `src/base/geom/sdk/vine/geom/geom_global.hpp`：一个 API 宏、两个命名空间宏、两处 `Continuity` 重导出。
- `scripts/check_geom_layering.py`：62 个几何头，0 处跨树 include。
- 验证：全量 Debug 构建通过；`test_math` 56、`test_geom_planar` 138、`test_geom_spatial` 193 全绿；`check_ai_docs` / `check_design_docs` / `check_public_docs` / `check_include_hygiene` / `check_doc_symbols` / `check_export_annotations` / `check_diagnostic_formats` 均 0 发现。
