# ADR-2026-11-06：几何两棵树的目录位置与维度命名维持现状

- 状态：已取代（两个契约已下沉为 `geom/{planar,spatial}` 子目录与 `vn::geom::{planar,spatial}` 子命名空间，见 `ADR-2026-11-06-geom-contracts-nested-under-module.md`；本文对 `2` 后缀与“两棵契约都加标记”的否决仍然有效）
- 日期：2026-11-06
- 相关：`../design/geometry-module.md` 2.2–2.5 / 7、`ADR-2026-11-06-merged-geom-module.md`、`ADR-2026-11-06-geom-detail-subtree.md`

## 背景

同一个会话里出现过三个重排提议，都指向同一件事——"让模块的物理形状更像它自称的样子"：

1. 给类型加 `2` / `3` 后缀，把两套命名空间并成一个；
2. 把两棵树下移到 `geom/planar` 与 `geom/spatial`，命名空间跟着下沉；
3. 平铺到 `geom/` 下，只给 2D 加标记（`Curve2`、`CircleCurve2`），3D 保持不带标记。

它们要解决的实际问题（与维度无关的代码需要一个模块内的共享位置）**已经解决了**：`geom/planar` 与 `geom/spatial` 今天就是通过 include `vine/geom/detail/...` 共享实现的（64 个头里有 16 个这么做），`scripts/check_geom_layering.py` 明确允许这个方向、禁止两棵树互相 include，当前 0 处违规。**重排剩下的只是"名字和目录看起来更整齐"，不是能力。**

## 选项

| 选项 | 好处 | 否决理由 |
|---|---|---|
| A 嵌套 `geom/planar` + `geom/spatial`（命名空间 `vn::geom::planar` / `spatial`） | `Continuity` 与 `detail` 由外层命名空间自动可见，`geom_global.hpp` 的两段 `using`（及"兄弟命名空间查不到"的说明）可以删掉 | 689 处代码引用 + 265 处文档引用全部改写，换来 20 行减少；两棵树仍各有一个 `CircleCurve.hpp`，文件识别问题一点没解决 |
| B 平铺 + 只标 2D（`Curve2` / `CircleCurve2`），3D 原名 | 3D 的名字一个不动；文件一眼可分 | ① 4 对**公开函数**同名，其中 `closestPointOnCurve` 与 `curveToCurveDistance` 参数表相同、返回类型不同——C++ 不能按返回类型重载，必须连函数一起改名（`closestPointOnCurve2`），这是用户可见 API；② `LineCurved`（3D）对 `LineCurve2d`（2D）：`d` / `f` 已被精度别名占用（`Vec2d`、`Rect2d`），两个后缀挤在一个位置；③ `CircleCurve` 变成"无标记即 3D"的不成文约定（`G4`）；④ `BezierCurve2` / `BSplineCurve2` 与 `degree()` 所在的"二次/三次"语义冲突 |
| C 平铺 + 两棵树都标（`Curve2` / `Curve3`） | 完全对称，单命名空间 | 15 个 2D 类型 + 47 个 3D 类型 + 4 个函数同时改名；且名字谎称"同一个族的两个维度"——2D 没有曲面与实体，两个契约零继承、不可互换（`G4`、`G1`） |

行业对照只支持其中一种写法，而且不是尾缀：OCCT 是"2D 加标记、3D 不加"，但标记写成 **`2d` 中缀**（`Geom_Curve` 对 `Geom2d_Curve`、`gp_Circ` 对 `gp_Circ2d`），正是为了避开与度数、阶数抢同一个位置。本仓库连 `2d` 都用不了——`d` 已经是标量后缀。

## 决定

维持现状：

- 目录：`sdk/vine/geom/planar` 与 `sdk/vine/geom/spatial` 是 `sdk/vine/` 下与 `geom/` 平级的两棵树；`geom/` 放模块的 `geom_global.hpp`、`Continuity.hpp` 与 `detail/`。
- 命名：维度由**命名空间**承载（`vn::geom::planar` / `vn::geom::spatial`），类型名不带维度标记，精度后缀仍是 `d` / `f`。
- 共享：两棵树都可以 include `vine/geom/detail/`，且不得互相 include——由门禁保证，不由目录位置保证。
- 已知代价（接受）：`geom/planar` / `geom/spatial` 是 `geom` 的兄弟而非孩子，所以 `geom_global.hpp` 必须把 `Continuity` 重导出进两个命名空间；这段 import 及其说明保留，`@note` 已写明"非限定名靠 import 才解析"。

判据：`G8`（没有功能收益的重排不做）、`G4`（维度已经在命名空间里说清，再进名字就是重复，且会与度数/精度抢位置）、`G11`（用户可见的 `closestPointOnCurve` / `arcLength` 不改名）。

## 后果

- 好的：本轮不需要改动任何代码；`geom/detail` 的共享关系保持原样；两棵树的公开 API 与别名一个不动。
- 代价 / 遗留：`math` 的 18 个头全在 `vine/math/` 下，而几何的三棵树不全在 `vine/geom/` 下——这条不对称留着。它是**纯目录观感**问题，不值得用 689 处改写作交换。
- 何时重新评估：出现**第三棵树**（例如多边形或网格几何），或 `geom` 的模块级词汇多到需要自己的命名空间层次时。判断依据是"有没有功能收益"，不是"名字是否更好看"。

## 证据

- 现状：`geom/planar` 17 个头、`geom/spatial` 47 个头、`geom/` 2 个头 + `detail/` 8 个；其中 16 个树内头 include 了 `vine/geom/detail/...`。
- 门禁：`python scripts/check_geom_layering.py` → `0 finding(s) in 70 file(s)`。
- 改名面：`src` + `tests` 里 `geom::planar::|geom::spatial::|vine/geom/planar|vine/geom/spatial` 共 689 处；`.ai` 265 处。
- 同名函数实测：`arcLength`、`parameterAtLength`、`closestPointOnCurve`、`curveToCurveDistance` 两棵树各一份；2D 另有 `offsetCurve` / `trimCurve` / `intersectCurveWithCurve`，3D 另有 `curvature` / `closestPointOnSurface` 等 17 个。
