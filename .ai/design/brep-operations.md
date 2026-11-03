# brep —— 运行时 B-rep 运算

> **状态**：现行（接口已落地；OpenCASCADE 函数体在 backlog）
> **最后核对**：2026-11-03
> **范围**：`src/base/brep`：B-rep 的运行时运算（三角化，及将来的由基本体造实体）的归属与契约。
> 不含 `BrepShape` 的数据定义（见 `graphics-geometry.md` §3.5）、不含 STEP/IGES 读写（见 `meshio-brepio-design.md`）、不含包围盒与 OBB（见 `geometry-primitives.md`）。
> **权威来源**：`src/base/brep/sdk/vine/brep/Tessellation.hpp`、`tests/test_brep`。
> **待办**：`../backlog/brep.md`

## 1. 概要设计

### 1.1 问题与背景

B-rep 在仓库里原本只有两半：

- **数据**：`geometry` 的 `BrepShape`——持有后端形状的句柄，SDK 只前向声明 `TopoDS_Shape`，不引 OpenCASCADE 头、不链接。
- **文件**：`brepio` 的 STEP/IGES 读写——需要 OpenCASCADE，PRIVATE 链接，没找到后端就回答 `IoError::Unsupported`。

缺的是第三半：**运行时对实体本身做运算**（三角化给渲染、由基本体造实体、将来的布尔）。把它塞进任何一边都不成立：

- 放进 `geometry`：base 模块从此链接 OpenCASCADE，所有 geometry 消费者跟着付钱，且公共头出现后端词汇。
- 放进 `brepio`：那个模块是"读取保存"，而三角化与文件无关；它还会为了一个运算把 `Runtime`/`Crypto`/`IOBase` 的使用者拉进来。

### 1.2 目标

- 运行时 B-rep 运算有一个只依赖 `geometry` + `iobase` 的家。
- OpenCASCADE 只有**一个探测点**，两个需要它的模块各自 PRIVATE 链接。
- 没有后端时接口仍然存在且行为确定：契约不变，失败是 `IoError::Unsupported`。

### 1.3 非目标

- 不做多后端与插件注册：今天只有 OpenCASCADE 一个后端（`G8`）。
- 不做 `Primitive` → 网格：那是纯数学、零依赖，将来有消费者时归 `geometry`，不归这里。
- 本期不实现 OpenCASCADE 函数体，也不做布尔/缝合（`../backlog/brep.md`）。

### 1.4 一句话结论

**数据归 `geometry`，文件归 `brepio`，运算归 `brep`；后端只探测一次，且永远不进 `geometry` 的公共头。**

## 2. 设计要点

### 2.1 三个模块各管一段

| 模块 | 职责 | 依赖 | OpenCASCADE |
|---|---|---|---|
| `vn::Geometry` | 形状数据：`Shape` / `Mesh` / `Primitive` / `BrepShape`（纯句柄） | Math、Core | 不碰 |
| `vn::Brep` | 运行时运算：三角化，将来的 `makeBrep` / 布尔 | Geometry、IOBase | 可选，PRIVATE |
| `vn::BrepIO` | 文件：STEP/IGES/BREP 的读写 | Runtime、Geometry、Crypto、IOBase | 可选，PRIVATE |

`Brep` 与 `BrepIO` 是兄弟，不互相依赖：一个管运算、一个管文件。满足 `G1`（职责一条边界）与 `G5`（预期变更留在模块内）。

### 2.2 一个探测点、私有链接

`cmake/VineOpenCASCADE.cmake` 做唯一一次 `find_package(OpenCASCADE CONFIG QUIET)`，并给出 `vn_link_opencascade(target macro [BOOLEANS])`：定义该模块自己的 `VN_*_HAS_OCCT`、PRIVATE 链接需要的 OCCT 模块。两个模块都调它，`find_package` 结果被缓存、导入目标相同。

PRIVATE 是关键：`vn::Brep` 的消费者不会继承 OpenCASCADE（`G10` 关注真实成本）。

### 2.3 失败是值

失败走 `vn::io::Result<T>` + `IoError`，与 `brepio` 同一套词汇（`G6`）。本模块只用两个分类：`InvalidData`（`BrepShape` 没有形状）与 `Unsupported`（没有后端）。`IoFailure` 是存储失败的词，这里不用（`N1`）。

### 2.4 没有后端时契约仍然成立

`Tessellation.cpp` 是一个桩：先判 `shape.isValid()`，再回答 `Unsupported`。这不是权宜之计——它让调用方只写一条失败分支，也让 `test_brep` 在没有 OpenCASCADE 的构建里可编译、可运行。后端函数体落地时只在 `VN_BREP_HAS_OCCT` 分支内生长，接口与失败分类都不变（`G9`）。

### 2.5 输出是 `TriangleMesh`，不带包围盒

三角化只回答"这个实体由哪些三角形组成"：顶点与索引在实体自身空间，`compute_normals` 时带法线。包围盒与 OBB 由调用方用已有的 `Aabb::compute()` / `Obb::computePca()` 从顶点量——不引入第二套包围盒算法（`G1`、`D4`）。

### 2.6 三角化不是 `BrepShape` 的成员

做成成员就必须把实现放进 `geometry`，等于让 base 模块链接 OpenCASCADE，并让 `BrepShape.hpp` 从"前向声明后端类型"退化成"依赖后端"。自由函数把后端留在 `brep`，`geometry` 继续零第三方（`G2`、`D1`）。

## 3. 功能点

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 三角化 | `tessellate(shape, options) → Result<TriangleMesh>`：按面三角化，索引按面朝向绕序 | 接口已落地，后端为桩 |
| F2 | 由基本体造实体 | `makeBrep(primitive) → Result<BrepShape>`：供导出与布尔使用 | 未开始（backlog） |

### 3.1 F1 三角化

- **行为**：逐面输出三角形；`deflection` 是面到三角形的最大距离，`angle` 是同一曲面上相邻三角形法线的最大夹角；`compute_normals` 为真时每个顶点带一个法线。
- **约束**（C1）：结果在实体自身空间，不做任何放置、缩放、变换。
- **约束**（C2）：面缝处顶点不焊接——同一棱上两面各有一份顶点，因为法线不同；索引绕序按各面自己的朝向。
- **要求**（R1）：没有后端回答 `IoError::Unsupported`；`BrepShape` 无形状回答 `IoError::InvalidData`。
- **验收**（T1）：`TessellationTest` 覆盖无形状 → `InvalidData`，且不随后端存在与否改变。
- **注意**（N2）：`deflection` 与 `angle` 是**后端无关**的契约，不暴露 OpenCASCADE 的其余参数。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `src/base/brep/sdk/vine/brep/Tessellation.hpp` | `TessellationOptions{ deflection, angle, compute_normals }`、`tessellate()` |

`tessellate()` 的完整契约在头文件注释里；本文件只记录归属与理由。

## 5. 约束、要求与不变量

- **C1**：结果在实体自身空间，无变换、无缩放。
- **C2**：面缝顶点不焊接；`compute_normals == false` 时只有位置与索引。
- **R1**：没有后端 → `Unsupported`；形状为空 → `InvalidData`。
- **R2**：接口在无后端构建下仍可编译、可链接、可测试。
- **R3**：后端依赖 PRIVATE 链接，消费者不继承。
- **N1**：`IoFailure` 属于存储失败，本模块不使用。
- **N2**：`deflection` 用模型单位，与模型尺度绑定。

## 6. 注意事项与陷阱

- **P1**：`deflection` 是**绝对长度**，把毫米模型和米模型混用时同一个 0.1 意义完全不同；缩放模型后要按比例改。
- **P2**：面缝不焊接会让顶点数约为焊接后的 2–3 倍；需要焊接的消费者自己合并在同一位置、同一法线的顶点，不要指望后端替它做。
- **P3**：旧实现里的 `length_scale` 只缩放了位置变换的平移分量，几何根本没有被缩放；本模块不提供这个参数，缩放请对实体本身做。
- **P4**：`Unsupported` 与 `InvalidData` 必须分开：前者是构建能力问题（换构建），后者是输入问题（换数据）。

## 7. 已否决选项

- **D1**：`BrepShape::triangulate()` 成员函数 —— 会把 OpenCASCADE 拖进 `geometry`，base 模块与公共头都失守（`G2`、`G5`）。见 `../decisions/ADR-2026-11-03-brep-operations-module.md`。
- **D2**：放进 `brepio` —— 模块名说"文件"，而运算是运行时需求，还会为无关能力拉上 `Runtime`/`Crypto`。
- **D3**：插件化后端（`BrepBackend` 抽象 + 注册）—— 为单一后端建机制，`G8` 反对；等第二个后端出现再升级。
- **D4**：用 `Bnd_Box` / `Bnd_OBB` 产出包围盒 —— 我们已有精确的 `Aabb::compute` 与 `Obb::computePca`，不引入第二套算法。

## 8. 测试与验收

| # | 判据 | 用例 |
|---|---|---|
| T1 | 无形状的实体报 `InvalidData`，且与后端无关 | `tests/test_brep/TessellationTest.cpp` |
| T2 | 后端落地后：盒体三角化为 12 个三角形、法线单位长度、顶点落在盒面上 | 待新增（见 backlog） |
| T3 | 后端落地后：没有后端的构建仍回答 `Unsupported` | 待新增 |

## 9. 状态与演进

### 9.1 现状

模块、头文件契约、CMake 探测与无后端桩已落地；`test_brep` 覆盖 `InvalidData` 分支。仓库当前构建没有 OpenCASCADE，因此后端函数体尚未编写。

### 9.2 已知缺口

- OpenCASCADE 三角化函数体、`makeBrep(primitive)`、布尔/缝合：见 `../backlog/brep.md`，均有触发条件。
- 渲染路径尚未接入：`graphics::geometryFromShape()` 仍只接受网格；三角化落地后再把"加载 B-rep → 三角化 → 显示"接起来。

## 10. 参考

- 代码：`cmake/VineOpenCASCADE.cmake`、`src/base/brep/`、`src/loaders/brepio/CMakeLists.txt`、`src/base/geometry/sdk/vine/geometry/BrepShape.hpp`。
- 设计：`graphics-geometry.md`（形状与网格的存储）、`meshio-brepio-design.md`（B-rep 文件读写）、`geometry-primitives.md`（包围盒与 OBB）。
- 决策：`../decisions/ADR-2026-11-03-brep-operations-module.md`。
- 测试：`tests/test_brep/TessellationTest.cpp`。
