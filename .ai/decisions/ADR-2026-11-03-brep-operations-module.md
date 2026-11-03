# ADR-2026-11-03：运行时 B-rep 运算独立成模块 `brep`

> **状态**：已采纳
> **日期**：2026-11-03
> **影响范围**：`src/base/brep`（新增）、`src/loaders/brepio`、`cmake/VineOpenCASCADE.cmake`（新增）

## 背景

B-rep 需要三件互不相同的东西：

1. **数据**——`geometry::BrepShape` 持有后端形状的句柄，SDK 只前向声明 `TopoDS_Shape`，不引 OpenCASCADE 头、不链接。
2. **文件**——`brepio` 读写 STEP/IGES，OpenCASCADE 可选用 PRIVATE 链接。
3. **运行时运算**——三角化（给渲染）、由参数化基本体造实体、将来的布尔。原先没有归属。

把第 3 件放进前两处都不成立：放进 `geometry` 会让 base 模块和公共头依赖后端；放进 `brepio` 会让"文件 I/O"模块承担与文件无关的运行时职责，并把 `Runtime`/`Crypto`/`IOBase` 的使用者一起拉进来。

## 决定

1. 新增 `src/base/brep`（目标 `vn::Brep`），只依赖 `vn::Geometry`。
2. 三角化以自由函数 `vn::brep::tessellate(BrepShape, TessellationOptions)` 提供，返回 `intrusive_ptr<TriangleMesh>`；失败是空句柄，构建能力由 `tessellationSupported()` 回答。
3. OpenCASCADE 的探测集中到 `cmake/VineOpenCASCADE.cmake`，`brep` 与 `brepio` 各自 PRIVATE 链接；`brepio` 改为使用该 helper。
4. 没有后端时接口仍存在：`tessellationSupported()` 回答 false，`tessellate()` 返回空网格。

## 理由

- 一个职责一条边界：数据、文件、运算各归其位（`G1`、`G5`）。
- 依赖方向单一：`brep → geometry`，`brepio → geometry`；两者互不依赖（`G2`）。
- 后端依赖 PRIVATE，消费者不继承，符合"mesh-only consumers keep not paying for it"的既有立场（`G10`）。
- 失败不借用 `Result` / `IoError`：那套词汇描述存储失败，与运算无关，还会让只做运算的模块为错误类型链接 `IOBase`；能力是构建属性（`tessellationSupported()`），失败是空值——与 `brepio::hasBackend()`、`graphics::geometryFromShape()` 同形（`G4`、`G8`）。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| `BrepShape::triangulate()` 成员函数 | 实现必须放进 `geometry`，base 模块与公共头都会依赖 OpenCASCADE（`G2`）。 |
| 放进 `brepio` | 模块名是"文件读写"，运行时转换与文件无关；还会为运算拉上导入导出才需要的依赖。 |
| 插件化后端（`BrepBackend` 抽象 + 注册） | 今天只有一个后端，机制成本大于收益（`G8`）；等第二个后端出现再升级。 |
| 用 `Bnd_Box` / `Bnd_OBB` 产出包围盒 | 已有精确的 `Aabb::compute` 与 `Obb::computePca`，不引入第二套算法（`G1`）。 |
| 由基本体直接三角化（绕开 OpenCASCADE） | 那是纯数学，归 `geometry`；本期没有消费者，不做。 |
| 返回 `vn::io::Result<T>`（`IoError`） | 那套词汇（`NotFound`、`PermissionDenied`、`IsADirectory`…）描述存储失败；借用它会让运算模块为错误类型链接 `IOBase`，还会让调用方去匹配用不到的分类。 |

## 后果

- 新增一个模块与一个 CMake helper；`brepio` 的探测代码从它的 CMakeLists 移到 helper，行为不变。
- 本期 `tessellate` 是无后端桩：接口、失败表示与测试先落地，OpenCASCADE 函数体在 `.ai/backlog/brep.md`。
- `vn::Brep` 不依赖 `IOBase`：模块的依赖只有 `vn::Geometry`。
- 渲染路径尚未接入：三角化实现后，再把"加载 B-rep → 三角化 → 显示"接起来。
- 现行设计记录在 `.ai/design/brep-operations.md`。
