# meshio / brepio —— 形状 I/O 的公开面

> **状态**：现行（公开面已按本文实现；OCCT 后端仍未接，见 9.2）
> **最后核对**：2026-10-27
> **范围**：`src/loaders/meshio`、`src/loaders/brepio` 的公开 API：格式词汇、读取器、写出器、失败与缓存。
> 不含几何数据本身（见 `graphics-geometry.md`）与 VFS / Stream 的定义（见 `iobase-vfs-design.md`）。
> **权威来源**：两个模块的 `sdk/` 头文件、`tests/test_meshio`、`tests/test_brepio`。
> **待办**：缺口见 9.2；实施清单见 9.3。

## 1. 概要设计

### 1.1 问题与背景

四个类是从 VMR 平移来的。平移之后暴露出五个结构性问题，都是公开面上的，不是实现里的：

1. **两套格式词汇。** 读按扩展名（assimp 注册表），写按写入器名（`objnomtl`、`collada`、`gltf2`）：`"dae"` 被拒、
   `collada` 可以；`isSupportedFormat("a.3ds")` 为真，它却不在任何可写声明里。
2. **失败三条通道。** `load` 是 `T* + IoError*` 出参，`exportAs` 抛异常，`createSource` 把失败推迟到
   `DataSource::error()`：调用方要同时记住三套约定。
3. **一个概念五份实现。** "mesh → 字节"有 `BinStlSource`、`AsciiStlSource`、`ObjSource`（meshio）与
   `StlMeshSource`、`MeshBinSource`（robotics/io）五份；meshio 那三份除测试外没有消费者。
4. **状态藏在单例里。** `defaultInstance()` 返回共享实例：options 可变、缓存不线程安全；两个真实消费者
   （`model_viewer`、`urdf2vine`）都已改用本地实例，并在注释里写明原因。
5. **brepio 是半成品。** 抽象 `BrepExporter` 没有实现者（只服务测试替身），`BrepLoader::Options` 是占位 `char`
   加三个访问器，`BrepShape` 不持有它加载出来的形状（裸指针 + 外部所有权，`G10`）。

### 1.2 目标

- 一套词汇：格式是枚举，能力来自一张表，读、写、文件选择器都从表里取。
- 一个入口集合：读完头文件就知道能做什么；没有只为对称而存在的重载。
- 失败是值：读返回 `vn::io::Result<T>`，写返回 `vn::io::IoError`。
- 删掉没有消费者的类与方法（`G8`），保住消费者真正需要的（读、写、缓存、包内写出）。

### 1.3 非目标

- 本次不实现 OCCT 的读写细节，只定"缺后端"的语义与构建开关；不引入第二个 B-rep 后端。
- 不为 PLY / glTF 写原生的逐记录写出器：assimp 能写的交给 assimp。
- 不改 `geometry::Mesh` / `geometry::Model` 的数据形状，只改 `BrepShape` 的所有权。
- 不合并 robotics/io 自己的 STL 源（见 `D5`）。

### 1.4 一句话结论

**格式是枚举，能力是一张表；读走 `Result`，写走 `IoError`；两个模块同一个形状：`Loader`（options + cache）
与 `Exporter`（options），四个类、五个自由函数。**

## 2. 设计要点

### 2.1 格式是类型，不是字符串

- **是什么**：`MeshFormat` / `BrepFormat` 枚举；`formatFromPath()` / `formatName()` / `formatInfo()` 三个自由函数；
  `supportedExtensions()` 由表生成。
- **为什么**：`G1`、`G4` —— 消掉 `dae`/`collada` 与 `obj`/`objnomtl` 两组陷阱；"能不能流式写"从运行期异常变成表里的
  布尔；与 imageio 的 `formatFromPath()` 同一套词汇（`G6`）。
- **边界**：表就是**读写契约**——表内每个值都可读可写；表外扩展名 `load()` 返回 `Unsupported`，不再透传 assimp 的
  四十多个格式（`.3ds`/`.x`/`.x3d` 因此不再默认支持）。
- **代价**：`G9` —— 加格式改表并重编（一行）；换来读、写、选择器只有一个答案。

### 2.2 失败是值

- **是什么**：`load`/`loadMerged` 返回 `vn::io::Result<vn::intrusive_ptr<T>>`；`exportAs` 返回 `IoError`（`Ok` 即成功）。
- **为什么**：`G4`、`G6` —— 一个返回值说清成败与原因；`Vfs`、`ZipArchive` 已在用 `Result`，出参与异常并存等于两套规则。
- **边界与代价**：`Result` 只带失败**分类**，不带 assimp / OCCT 的错误文本；值语义下 `intrusive_ptr` 多一次原子计数，
  相对解析成本可忽略（`G10`）。

### 2.3 状态归实例

- **是什么**：`Options` 构造时给、只读；缓存属于 loader，`clearCache()` 显式释放
  （`../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md`）。
- **为什么**：`G4` —— 共享可变单例把"谁的选项生效"变成隐式约定；两个消费者已改用本地实例。
- **边界**：不保留 `defaultInstance()`；缓存不线程安全写在类上（`N1`），不用 `mutable` + `const` 假装安全。

### 2.4 一个写出器，三种目标、两种形状

- **是什么**：每种格式只有一份排版实现；`exportAs(path)`、`exportAs(stream)`、`createSource()` 都从它出发，
  `Mesh` 与 `Model` 各一套；非流式格式（`.gltf` 要 `.bin`、`.3mf` 要路径）由 `streamable == false` 拒绝。
- **为什么**：`G1`、`G5` —— 现在 assimp 路径与原生 STL/OBJ 路径的输出**不承诺一致**；收拢后"一份内容，三个目标"可测。
- **烘平归写出器，不归我们**：`stl`/`ply` 的导出器注册就强制 `aiProcess_PreTransformVertices`（先复制场景再处理），
  OBJ 自己按节点累积矩阵写顶点；我们只如实建 aiScene（节点带局部变换）。合并不成单个 mesh / 布尔融合都不是契约 ——
  STL/PLY 本来就把多个网格写进同一个文件。
- **`createSource()` 是框架能力，不是便利重载**：`DataSource` 是 iobase 的包内写出通道，包内写出不落临时文件，也不逼
  调用方自己绕临时文件（`G7`）。明知 `G8` 的消费者答案是没有，仍保留 —— 消费者就是这条能力。

### 2.5 brepio 与 meshio 同构，且后端可选

- **是什么**：`BrepLoader` / `BrepExporter` 与 meshio 同形（`G6`）：路径 / 流 / 数据源各一个入口；两者都没有 options。
- **为什么**：`G8` —— 占位 `Options`、`OptionsHash`、抽象导出器基类、`canExport` 都没有消费者；形状只有一种，
  "能不能导出"就等于 `isValid()`。
- **可选后端**：`find_package(OpenCASCADE QUIET)`；找到就 PRIVATE 链接并定义 `VN_BREPIO_HAS_OCCT`，找不到就只留契约：
  `supportedExtensions()` 为空，`load` / `exportAs` 回 `Unsupported`，source 的 `error()` 也是它 —— **不抛异常**，
  否则等于给读写加第三条失败通道。不 FetchContent（`G10`：几百 MB、构建以十分钟计，且只有 brep 需要）。

### 2.6 `BrepShape` 持有它加载出来的形状

- **是什么**：`setShape(std::shared_ptr<const TopoDS_Shape>)` 接管所有权，`shape()` 返回借用视图（`G10`）。
- **为什么**：现在的 `setShape(TopoDS_Shape*)` 没有人负责释放，加载器一返回就悬空或泄漏。
- **边界**：geometry 只前置声明 `TopoDS_Shape`，不 include OCCT、不链接；`make_shared` 在 brepio 侧完成，删除器随
  `shared_ptr` 类型擦除，geometry 侧析构安全。

### 2.7 与 VMR 的对照

| VMR | 本设计 |
|---|---|
| `load`/`loadMerged` 四入口；`format` 是写入器名字符串 | 入口保留；格式改枚举（`G4`：名字说清交回什么） |
| 缓存按 `Options` 分组 + `Default()` 单例 | 删：选项与缓存都属于实例（`G4`、`G11`） |
| `IsSupportedFormat`、抽象 `BrepExporter`、`canExport`、占位 `Options` | 删（`G8`）；格式问题统一问 `formatFromPath()` |
| `createSource(mesh \| model)` | 保留（框架能力，`G7`） |

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 格式词汇与能力表 | 枚举 + 三个自由函数 + `supportedExtensions()` | 已落地 |
| F2 | `MeshLoader` | 路径/流 → `Model` 或合并 `Mesh`，按内容缓存 | 已落地 |
| F3 | `MeshExporter` | `Mesh`/`Model` → 路径 / 流 / 数据源；平面格式由写出器烘平 | 已落地 |
| F4 | `BrepLoader` / `BrepExporter` | STEP / IGES / BREP，路径与流 | 已落地（OCCT 可选） |
| F5 | `BrepShape` 所有权 | `shared_ptr` 接管 OCC 形状 | 已落地 |
| F6 | 文件选择器接口 | `supportedExtensions()` 是唯一过滤来源 | 已由 `model_viewer` 使用 |

### 3.1.1 F1 格式词汇与能力表

- **行为**：`MeshFormat` / `BrepFormat` 枚举 + `formatFromPath()` / `formatName()` / `formatInfo()` +
  `supportedExtensions()`；表回答 `extension` / `has_encoding` / `streamable`。
- **约束/要求/验收**：`C1`、`R1`、`R3`、`T1`、`T2`。
- **注意**：`N1`；`Gltf`/`ThreeMf` 要第二个文件或路径 ⇒ `streamable == false`；只有 `Stl`/`Ply`/`Fbx` 响应 `Options::encoding`。

### 3.1.2 F2 `MeshLoader`

- **行为**：`load()` 保留结构（节点树 / 多网格 / 材质），`loadMerged()` 烘平为一个 `IndexedTriangleMesh`；
  路径与流各一个入口；`clearCache()`。
- **约束/要求/验收**：`C2`、`C3`、`R2`、`R4`、`T3`、`T4`。**注意**：`N2`、`N3`、`N4`、`N6`。

### 3.1.3 F3 `MeshExporter`

- **行为**：六个入口：`exportAs(mesh|model, format, path|stream)` 与 `createSource(mesh|model, format)`。
- **约束/要求/验收**：`C4`、`C5`、`R1`、`R2`、`T1`、`T5`、`T7`。
- **注意**：assimp 格式的 source 在首次拉取时把整份文件放进内存（`size()` 已知）；原生 STL/OBJ 才是逐记录。

### 3.1.4 F4 `BrepLoader` / `BrepExporter`

- **行为**：`load(path)`、`load(stream, format)`、`exportAs(shape, format, path | stream)`、`createSource(shape, format)`。
- **约束/要求/验收**：`C6`、`R2`、`R4`、`T6`。
- **注意**：流的格式必填 —— OCCT 的 reader 按格式选，不像 assimp 会嗅探内容。

### 3.1.5 F5 `BrepShape` 所有权

- **行为**：`setShape(std::shared_ptr<const TopoDS_Shape>)` 接管、`shape()` 借用、`isValid()` = 非空。
- **约束/验收**：geometry 不出现 OCCT 头文件与链接；`T8`。

### 3.1.6 F6 文件选择器接口

- **行为**：`supportedExtensions()` 由表生成（小写、带前导点）；**约束/验收**：`C1`、`T1`。

## 4. 公开接口

格式词汇（两个模块同名同形；`BrepFormat` 无能力表，三种都是单文件）：

```cpp
// vn::meshio
enum class MeshFormat : std::uint8_t { Unknown = 0, Stl, Obj, Ply, Gltf, Glb, Fbx, Collada, ThreeMf };
struct MeshFormatInfo { const char* extension; bool has_encoding; bool streamable; };  // Unknown → 全空
const char*           formatName(MeshFormat) noexcept;                                 // "stl"
MeshFormat            formatFromPath(const std::filesystem::path&) noexcept;           // 未知 → Unknown
const MeshFormatInfo& formatInfo(MeshFormat) noexcept;

// vn::brepio —— 同名同形；没有能力表（三种都是单文件）
enum class BrepFormat : std::uint8_t { Unknown = 0, Step, Iges, Brep };
const char* formatName(BrepFormat) noexcept;                                           // "step"
BrepFormat  formatFromPath(const std::filesystem::path&) noexcept;
```

### 4.1 `vn::meshio` —— `MeshLoader` / `MeshExporter`

| 接口 | 契约 | 备注 |
|---|---|---|
| `static supportedExtensions()` | 由表生成，小写带点 | 文件选择器的唯一过滤来源（`C1`） |
| `Options{ scale_mode, auto_scale_output_unit, custom_scale_factor }` | 构造时给，`options()` 只读 | 两套策略 = 两个 loader |
| `Result<intrusive_ptr<Model>> load(path)` / `load(istream&, MeshFormat hint = Unknown)` | 保留节点树/网格/材质 | 路径与字节命中同一条缓存（`C3`） |
| `Result<intrusive_ptr<Mesh>> loadMerged(同上)` | 烘平为一个 `IndexedTriangleMesh` | 唯一带 `PreTransformVertices` 的入口（`N6`） |
| `void clearCache()` | 释放缓存里的几何 | 显式释放，不设上限 |
| `Options{ scale_factor, encoding }` | 构造时给，`options()` 只读 | `encoding` 只对 `has_encoding` 的格式生效 |
| `IoError exportAs(mesh\|model, MeshFormat, path)` | 写文件、建父目录；平面格式由写出器烘平 | `Model` 的附属部件（`.mtl`）写到同目录 |
| `IoError exportAs(mesh\|model, MeshFormat, ostream&)` | 一份自包含内容进一个流 | `streamable == false` ⇒ `Unsupported`（`C4`）；只差附属部件的丢弃并写明 |
| `shared_ptr<DataSource> createSource(mesh\|model, MeshFormat)` | `exportAs(stream)` 的懒版本：首次拉取生成，`rewind()` 重放 | 框架能力：包内写出不落临时文件（`G7`） |

### 4.2 `vn::brepio` —— `BrepLoader` / `BrepExporter`

| 接口 | 契约 | 备注 |
|---|---|---|
| `static supportedExtensions()` | `.stp` `.step` `.igs` `.iges` `.brep` | 与 meshio 同一形状（`G6`） |
| `Result<intrusive_ptr<BrepShape>> BrepLoader::load(path)` | 按扩展名选 reader | 内容指纹缓存，`clearCache()` |
| `Result<...> BrepLoader::load(istream&, BrepFormat)` | 流的格式**必填** | OCCT 不嗅探 |
| `IoError BrepExporter::exportAs(shape, BrepFormat, path \| ostream&)` | 两个目标都是一份实现 | 无 options |
| `shared_ptr<DataSource> BrepExporter::createSource(shape, BrepFormat)` | 与 meshio 同一懒契约；无后端时 `error()` 为 `Unsupported` | 框架能力（`G7`） |

### 4.3 `vn::geometry::BrepShape`（只改所有权）

| 接口 | 契约 |
|---|---|
| `const TopoDS_Shape* shape() const` | 借用；未设置时为空 |
| `void setShape(std::shared_ptr<const TopoDS_Shape>)` | 接管所有权（替换今天的裸指针） |
| `bool isValid() const` | `shape() != nullptr` |

## 5. 约束、要求与不变量

### 5.1 约束

- `C1` 枚举与表是唯一的格式词汇：`supportedExtensions()`、`formatFromPath()`、读写两端都从它取，不得手写第二份。
- `C2` 缓存键 = 文件内容指纹；选项属于实例，因此键里没有选项。
- `C3` 流入口先把流落成一串字节再解析，与路径入口命中同一条缓存。
- `C4` `streamable == false` 的格式在 stream / source 目标上返回 `IoError::Unsupported`，不发生成再失败。
- `C5` `Model` 写到 path 时可写附属文件；`stream` / `source` 只容纳一份自包含内容：必须有第二个文件的（`.gltf`、`.3mf`）返回 `Unsupported`，只是附属部件（OBJ 的 `.mtl`）的丢弃并写明。
- `C6` 没有 OCCT 时 `supportedExtensions()` 为空，`load` / `exportAs` / source 的 `error()` 都是 `Unsupported`；`formatFromPath` 照常回答（格式是格式，能力是能力）。空的答案必须是**失败值**，不能是 `Ok` + 空指针。

### 5.2 要求

- `R1` 每个枚举值都能写、能读回（往返用例，`T1`）；`streamable` 的值还必须能由 `createSource()` 产出同一份内容（`T5`）。
- `R2` 公开头只使用 base 模块的类型（iobase / geometry / runtime / crypto），三方库不外泄。
- `R3` 读、写、选择器三条路对同一格式给出一致答案（`T1` 同时钉两侧）。
- `R4` 失败必须分类：`NotFound` / `InvalidData` / `Unsupported` / `IoFailure`，不得只回空。

### 5.3 不变量

- `N1` loader 的缓存不是线程安全的；一个 loader 实例同一时刻只在一个线程上用（类注释 `@warning`）。
- `N2` 路径入口读两遍文件：一遍算指纹、一遍解析。这是"不返回陈旧几何"的代价。
- `N3` `ScaleMode::Auto` 默认按 AABB 对角线 10 个源单位判 mm/m，再换算到 `auto_scale_output_unit`。
- `N4` 流入口没有真实文件名：OBJ 的 `mtllib` 不会被解析（要附属文件就走路径入口）。
- `N5` `Result` 的 `*` / `->` 在失败时是未定义行为，先判 `ok()`。
- `N6` `aiProcess_PreTransformVertices` 只出现在 `loadMerged()` 的后处理里；`load()` 带上它就会毁掉节点树。

## 6. 注意事项与陷阱

| # | 会怎么错 | 怎么判 | 怎么防 |
|---|---|---|---|
| P1 | 给 `load()` 加 `PreTransformVertices`：节点图被删、变换被烘进顶点 | `ModelLoadTest` 的"变换留在节点上" | `N6`；两份后处理表分开写 |
| P2 | 在空的 `Result` 上解引用（`m->name()`） | `N5`；`[[nodiscard]]` 让漏判报警 | 先 `if (!r)` 再 `r.value()` |
| P3 | 以为 `createSource(mesh, Gltf)` 逐记录写出，实际首次拉取整份进内存 | `T5` 只钉 STL 的精确长度与逐记录 | 在 `createSource()` 注释里按格式说明 |
| P4 | 为省一个局部对象又加单例 | 评审按 `G4` 拒绝 | 2.3 是设计决定 |
| P5 | 自己烘平一遍又把带变换的场景交给 assimp ⇒ STL/PLY 被变换两次 | 导出带平移的模型再读回，包围盒偏移一倍 | 场景只存局部变换，烘平只由写出器做（`stl`/`ply` 已强制） |

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 保留两套格式词汇（读扩展名、写写入器名） | `G1`：同一个格式两个名字，必然漂移 | — |
| D2 | `load` 保留 `IoError*` 出参 | `G4`：成败与结果分在两个参数里 | — |
| D3 | 保留抽象 `BrepExporter` 基类 | `G8`：唯一实现者是测试替身 | — |
| D4 | 保留四个公开 source 类 | `G1`/`G8`：四个类表达一个 `createSource()` | — |
| D5 | 把 robotics 的 STL 源合并进 meshio | `G10`：会让 robotics 为一份 STL 写出器背上 assimp | — |
| D6 | 对 `Model` 关闭 stream / source（只写 path） | `G4`：能写成一个文件的模型不该被迫先落盘；代价只是一次烘平 | — |
| D7 | `BrepShape` 用 `shared_ptr<const void>` 类型擦除 | `G4`：每次取用都要 `static_cast`，类型安全交给约定 | — |
| D8 | 继续用 assimp 注册表当读契约 | `G1`：读表与写表两份，回到 1.1 的第 1 条 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 表逐值：每个枚举值都能 `exportAs(path)` 并 `loadMerged` 读回；`supportedExtensions()` 与表逐项相等 | `MeshIoTest.FormatVocabularyIsTheReadContract`、`TheExtensionTableNamesWhatTheLoaderActuallyReads` |
| T2 | 扩展名与枚举同一答案：`formatFromPath("a.DAE") == Collada`，`exportAs(..., Collada, "a.dae")` 成功 | `MeshIoTest.DaeAndColladaAreOneFormat` |
| T3 | 失败分类：缺失文件 `NotFound`、坏字节 `InvalidData`、表外扩展名 `Unsupported` | `MeshIoTest.LoadSaysWhyItFailed` |
| T4 | 缓存：同字节两次负载命中同一对象；`clearCache()` 后重建 | `MeshIoTest.LoadCachesSameFile` |
| T5 | source：每个 `streamable` 格式的 `createSource()` 与 `exportAs(stream)` 字节一致 | `MeshSourceTest.MatchesTheStreamEntry` |
| T6 | brepio：无 OCCT 时 `load` 返回 `Unsupported`；`formatFromPath` / `supportedExtensions` 正确 | `BrepIoTest.LoadRefusesWithAValueNotAnException` |
| T7 | `Model` → STL：带平移/缩放的节点导出后读回，包围盒等于世界空间结果（变换恰好一次），材质与层级丢弃 | `MeshIoTest.ModelToStlAppliesTransformsOnce` |
| T8 | `BrepShape` 未设形状时 `isValid()` 为假；`setShape({})` 可清空 | `BrepIoTest.AnEmptyShapeIsNotValid` |
| T9 | OCCT 往返：box → STEP / IGES / BREP → 读回 `isValid`；同一字节的流入口命中同一条缓存 | `BrepIoTest.ExportsABoxAndReadsItBack` |

## 9. 状态与演进

### 9.1 当前状态

公开面已按本文实现：meshio 完整（格式表、`Result`/`IoError`、六个导出入口、`createSource()`），
brepio 的 API 与失败语义已落地，OCCT 读写仍是缺口。

### 9.2 已知缺口

- OCCT 只在配置能找到时才接：`find_package(OpenCASCADE CONFIG QUIET)` 不强制，找不到的构建里扩展名列表为空、
  读写回 `Unsupported`（`C6`）。
- `.3ds` / `.x` / `.x3d` 不在表内；需要时加一行枚举与表项。
- PLY 没有原生逐记录写出；`createSource()` 对 assimp 格式整份进内存。
- robotics/io 的 `StlMeshSource` 与 meshio 的 STL 写出器是刻意的重复（`D5`）。

### 9.3 兼容性 / 迁移

公开面是破坏性变化：`format` 改为枚举、读入口改为 `Result`、写入口改为 `IoError`，`defaultInstance()` /
`isSupportedFormat()` 与四个 source 类删除。仓内调用点（`model_viewer`、`urdf2vine`、两套测试）已全部迁移；
仓外调用方按 §4 改签名即可。

## 10. 参考

- VMR 参考实现：`D:\PROJS\VMR\src\scene_core\include\vmr\io` 的 `VMeshLoader.h`、`VMeshExporter.h`、
  `VBrepLoader.h`、`VBrepExporter.h`。
- 现有实现：`src/loaders/meshio`、`src/loaders/brepio`；词汇来源：`src/base/iobase/sdk/vine/io/IoError.hpp`
  （`Result` / `IoError`）与 `src/base/iobase/sdk/vine/io/Stream.hpp`（`DataSource`）。
- 决策与要点：`../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md`、`../memory/loaders.md`。
