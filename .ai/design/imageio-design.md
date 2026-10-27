# imageio —— 图像容器的编解码公开面

> **状态**：现行
> **最后核对**：2026-10-27
> **范围**：`src/loaders/imageio` 的公开 API：容器词汇、能力表、解码、编码与失败分类。
> 不含 `imaging::Image` / `PixelFormat` 的数据形状（见 `imaging-design.md`），也不含 VFS / Stream（见 `iobase-vfs-design.md`）。
> **权威来源**：`src/loaders/imageio/sdk/vine/imageio/ImageCodec.hpp`、`tests/test_imageio`。
> **待办**：见 9.2。

## 1. 概要设计

### 1.1 问题与背景

编解码只有一条通道：stb 的 memory API（`STBI_NO_STDIO`），文件读写由本模块用 `std::filesystem` + `fstream` 做。
平移过来之后暴露了三个公开面上的问题：

1. **失败是异常**，而同层的 `meshio` / `brepio` 已经统一为 `Result` / `IoError`：同一层三个模块两套失败通道，
   调用方要同时记住两种约定。唯一消费者（`DemoScene.cpp` 里的立方体贴图）用 `try/catch` 只为打印原因并跳过。
2. **同一个事实四份副本**：`formatName()` / `formatFromPath()` / `canRead()` / `canWrite()` 各一个 `switch`，
   加一个容器要改四处，且可以互相漂移（"能读"的表列了它、扩展名表忘了它）。
3. **文件选择器没有单一来源**：`meshio` 有 `supportedExtensions()`，`imageio` 没有，任何对话框都得手写一份扩展名。

### 1.2 目标

- 容器是枚举，能力来自**一张表**：名字、扩展名、可读、可写都从它取，选择器的扩展名列表也由它生成。
- 失败是值：读回 `Result<T>`，写回 `IoError`，与 `meshio` / `brepio` 同一套词汇。
- 一个入口集合：读完公开头就知道能做什么；模块**无状态**。

### 1.3 非目标

- 不做 `ImageLoader` / `ImageEncoder` 类（没有 options，也没有缓存）。
- 不加 `std::istream` 重载、VFS 入口、`createSource()`：见 2.4 与 `D2`–`D4`。
- 不生成 mip 链，不写 JPEG（写出有损且需要质量参数，本 API 刻意没有那个位置）。

### 1.4 一句话结论

**容器是枚举、能力是一张表；读走 `Result`、写走 `IoError`；四个编解码入口是无状态自由函数，没有第二个概念。**

## 2. 设计要点

### 2.1 格式是类型，能力来自一张表

- **是什么**：`ImageFileFormat` 枚举；`ImageFormatInfo{ extensions, can_read, can_write }`；`formatInfo()` 取表；
  `formatName()` / `formatFromPath()` / `canRead()` / `canWrite()` / `supportedExtensions()` 都是表上的查询。
- **为什么**：`G1`、`G6` —— 同一事实只有一个家；与 `meshio` 的 `formatInfo()` / `supportedExtensions()` 同名同形。
- **边界**：表就是读写契约——`can_write == false` 的容器写不出（JPEG）；表外扩展名回答 `Unknown`。
- **与 meshio 的差异**：`meshio` 的表没有 `can_read` / `can_write`（"枚举即契约"：表内值都可读可写）；
  图像不是这样（JPEG 只读），所以表里保留两个布尔。这是**领域差异**，不是风格差异。

### 2.2 失败是值

- **是什么**：`decodeImage()` / `loadImage()` 返回 `vn::io::Result<intrusive_ptr<Image>>`；
  `encodeImage()` 返回 `Result<std::vector<std::byte>>`；`saveImage()` 返回 `IoError`（`Ok` 即成功）。
- **为什么**：`G4`、`G6` —— 一个返回值说清成败与原因；与 `iobase`、`meshio`、`brepio` 同一条规则。
- **分类**（`R1`）：布局无法承载 → `Unsupported`；字节不是可读图像 / 空文件 → `InvalidData`；
  文件不存在 → `NotFound`；读不出、写不进、编码器没产出 → `IoFailure`；超过解码器可寻址长度 → `CapacityExceeded`。
- **代价**：`Result` 只带分类，丢掉 stb 的 `stbi_failure_reason()` 文本 —— 与 `meshio` 放弃 assimp 错误文本同一代价，
  且调用方本来就知道路径与格式（`D5`）。

### 2.3 无状态：四个自由函数，不是 Loader / Exporter

- **是什么**：解码 `decodeImage(bytes, layout)` / `loadImage(path, layout)`；编码 `encodeImage(image, format)` /
  `saveImage(path, image, format)`。
- **为什么**：`G8` —— 解码是纯函数，编码没有策略；`meshio` 之所以需要实例，是因为它有 options（缩放策略）与
  内容指纹缓存。照搬类形状只会让每个调用方先构造一个对象再调一个方法。
- **边界**：调用方要的像素布局是参数（`PixelFormat`），因为"解码出的 RGB8 没有 24-bit 纹理格式"是调用方的问题；
  容器由字节嗅探，扩展名只作提示。

### 2.4 只做编解码：不加流 / VFS / createSource

- **流**：stb 是 memory API，"从流解码"必然先整体进内存，`decodeImage(span)` 就是那条路的终点；`istream` 重载
  只替调用方 drain，却让库多承担一条"流中途坏掉"的路径（`G7` 的反向）。
- **VFS**：`Vfs::read()` / `Vfs::addFile()` 与 `decodeImage()` / `encodeImage()` 组合即可，两行；加 VFS 入口会把
  `Vfs` 类型拉进公开面只为一个组合（`G5`）。失败词汇（`IoError`）是另一回事：它是词汇，不是实现。
- **`createSource()`**：`encodeImage()` 已经产出完整字节，`Vfs::addFile(path, span)` 直接收；延迟编码只省一次
  几十毫秒的编码内存，却要引入一个必须持有 `intrusive_ptr<const Image>` 的源与一条生命周期契约（`G8`、`G10`）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 容器词汇与能力表 | 枚举 + `formatInfo()` + 四个查询 + `supportedExtensions()` | 已落地 |
| F2 | 内存解码 | `decodeImage(bytes, layout)` → `Result<Image>` | 已落地 |
| F3 | 文件解码 | `loadImage(path, layout)` → `Result<Image>` | 已落地 |
| F4 | 内存编码 | `encodeImage(image, format)` → `Result<vector<byte>>` | 已落地 |
| F5 | 文件编码 | `saveImage(path, image, format)` → `IoError` | 已落地 |

### 3.1.1 F1 容器词汇与能力表

- **行为**：`ImageFileFormat`（`Png` / `Jpeg` / `Bmp` / `Tga` / `Unknown`）；表回答扩展名、可读、可写。
- **约束/要求/验收**：`C1`、`R2`、`T1`、`T2`。
- **注意**：JPEG 两个扩展名（`.jpg` / `.jpeg`）；`Unknown` 的扩展名列表为空、两个能力都是假。

### 3.1.2 F2 / F3 解码

- **行为**：容器由字节嗅探；布局由调用方指定；结果恒为 1 个 mip；BGRA 布局在解码后交换 R/B。
- **约束/要求/验收**：`C2`、`C3`、`R1`、`T3`、`T4`。
- **注意**：一通道是 Rec.601 **亮度**，不是源的红通道；二通道（灰度 + alpha）拒绝（`C4`、`P1`）。

### 3.1.3 F4 / F5 编码

- **行为**：只写 base mip；不支持写出 base 之外的级别；BGRA 在临时的 R/B 交换副本上编码，不改调用方像素。
- **约束/要求/验收**：`C5`、`R1`、`T5`、`T6`。
- **注意**：BMP 只有 24-bit RGB / 32-bit RGBA 两条路，一通道会被展开成灰度三通道（`P2`）。

## 4. 公开接口

```cpp
// vn::imageio —— 格式词汇（src/loaders/imageio/sdk/vine/imageio/ImageCodec.hpp）
enum class ImageFileFormat : std::uint8_t { Unknown = 0, Png, Jpeg, Bmp, Tga };
struct ImageFormatInfo {
    std::span<const char* const> extensions;  // 小写、带前导点；Unknown → 空
    bool                         can_read;
    bool                         can_write;
};
const ImageFormatInfo& formatInfo(ImageFileFormat) noexcept;
const char*            formatName(ImageFileFormat) noexcept;          // "PNG"
ImageFileFormat        formatFromPath(const std::filesystem::path&);  // 按扩展名，仅作提示
bool                   canRead(ImageFileFormat) noexcept;
bool                   canWrite(ImageFileFormat) noexcept;
const std::set<vn::String>& supportedExtensions();                    // 由表生成
```

| 接口 | 契约 | 失败 |
|---|---|---|
| `Result<intrusive_ptr<Image>> decodeImage(span<const byte>, PixelFormat)` | 容器嗅探；布局由调用方给；1 个 mip | `Unsupported`（布局无对应形态）/ `InvalidData` / `CapacityExceeded` |
| `Result<intrusive_ptr<Image>> loadImage(path, PixelFormat)` | 读整份文件后解码 | `NotFound` / `IoFailure` / 解码的失败 |
| `Result<vector<byte>> encodeImage(const Image&, ImageFileFormat)` | 只写 base mip | `Unsupported` / `InvalidData` / `IoFailure` |
| `IoError saveImage(path, const Image&, ImageFileFormat)` | 格式取自参数，不由扩展名猜 | 编码的失败 / `IoFailure` |

## 5. 约束、要求与不变量

### 5.1 约束

- `C1` 枚举与表是唯一的格式词汇：`supportedExtensions()`、`formatFromPath()`、读写两端都从它取，不得手写第二份。
- `C2` 解码的容器由字节判定，扩展名不参与；文件名撒谎不影响结果。
- `C3` 解码结果恒为 1 个 mip：容器存一张图，链是另一件事。
- `C4` 两通道布局拒绝：文件里的两通道是灰度 + alpha，`Rg8Unorm` 是红 + 绿。
- `C5` 编码只写 base mip；JPEG 不可写（`can_write == false`）。

### 5.2 要求

- `R1` 失败必须分类（2.2 的五类），不得只回空值或抛异常。
- `R2` 公开头只使用 base 模块的类型（`Imaging`、`IOBase`、`Core`），stb 不外泄（PRIVATE 链接）。
- `R3` 公开入口无状态：同一组参数调两次得到同一份字节（`T6`）。

### 5.3 不变量

- `N1` `Result` 的 `*` / `->` 在失败时是未定义行为，先判 `ok()`。
- `N2` 每次解码分配一张新 `Image`，调用方独占；没有缓存，也没有共享实例。
- `N3` 8-bit 颜色布局 = 1 / 3 / 4 通道；浮点与深度布局不在可编码范围内。

## 6. 注意事项与陷阱

| # | 会怎么错 | 怎么判 | 怎么防 |
|---|---|---|---|
| P1 | 把一通道当作源的红通道 | 灰度图与红通道采样不同 | `C4` 的注释；`ASingleChannelIsLumaAndNotTheSourcesRed` |
| P2 | 以为 BMP 能存两通道 | 读回后第二通道恒为 `0xFF` | `canWrite` 表项 + BMP 只走 RGB/RGBA 两条路 |
| P3 | 在 JPEG 上测亮度语义 | 亮度差一两步 | 亮度用例只跑无损容器（PNG） |
| P4 | 用 `> INT_MAX` 的缓冲解码 | 长度被截断成 `int` | `CapacityExceeded` 在进解码器之前返回 |
| P5 | 文件对话框自己写扩展名 | 新增格式后对话框不认 | 用 `supportedExtensions()`（`C1`） |

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 照搬 `Loader` / `Exporter` 类形状 | `G8`：没有 options、没有缓存，类只增加生命周期 | — |
| D2 | `load(std::istream&)` / `save(std::ostream&)` | `G7`：stb 是 memory API，重载只替调用方 drain | — |
| D3 | `loadImage(Vfs&, path)` / 加 `DataSource` 入口 | `G5`/`G7`：`Vfs::read()` 与 `decodeImage()` 两行即可，加了要把 `Vfs` 拉进公开面 | — |
| D4 | `createSource()`（延迟编码进包） | `G8`/`G10`：只省一次编码内存，换来一个持有 `Image` 的源与生命周期契约 | — |
| D5 | 保留异常作失败通道 | `G4`/`G6`：同层模块两套失败约定，调用方无法按分类处理 | — |
| D6 | 保留四个手写 `switch` | `G1`：同一事实四份副本，加格式要改四处 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 表逐值自洽：`formatInfo()` 的每个扩展名都能被 `formatFromPath()` 认回，且出现在 `supportedExtensions()` 里 | `ImageFileFormatTest.TheTableStatesEachFormatsExtensionsAndCapabilities` |
| T2 | 扩展名列表由表生成（5 项：png / jpg / jpeg / bmp / tga） | `ImageFileFormatTest.TheExtensionListIsDerivedFromTheTable` |
| T3 | 每个可写容器 × 每个 8-bit 布局往返逐字节相同 | `ImageEncodeTest.EveryWritableContainerRoundTripsEveryLayoutExactly` |
| T4 | 失败分类：缺失文件 `NotFound`、空文件 / 垃圾字节 `InvalidData`、不可写容器 `Unsupported`、写不进的路径 `IoFailure` | `ImageDecodeTest.ReportsAMissingFileAsNotFound` 等 |
| T5 | 二通道与浮点布局被拒（`Unsupported`），不是静默转换 | `ImageDecodeTest.RefusesTwoChannelLayouts...`、`RefusesALayoutAContainerCannotHold` |
| T6 | 同一输入两次编码字节相同；只写 base mip | `ImageEncodeTest.OnlyTheBaseLevelIsWritten` |

## 9. 状态与演进

### 9.1 当前状态

公开面已按本文实现；`tests/test_imageio` 28 个用例全过。消费者只有 `demo_plugin` 的立方体贴图（六个 JPEG 面）。

### 9.2 已知缺口

- **mip 生成没有**：解码只产 base 级，链由上传侧另行生成（见 `imaging-design.md` 9.2）。
- **JPEG 写出**：需要质量参数，属于"新增参数"而不是"打开表项"；有消费者时按 `WriteOptions` 的形式加。
- **VFS / 包的出入口**：按 `D3` 由调用方组合；出现"不进内存地读包内大图"或"包内成批导出"时再评估（`../../.ai/backlog/` 无对应条目时不预置）。

### 9.3 兼容性 / 迁移

公开面破坏性变化：四个编解码入口从"抛异常 / 返回裸值"改为 `Result` / `IoError`，并新增 `formatInfo()` 与
`supportedExtensions()`。仓内调用点（`demo_plugin` 与 `tests/test_imageio`）已全部迁移。

## 10. 参考

- 代码：`src/loaders/imageio/sdk/vine/imageio/ImageCodec.hpp`、`src/loaders/imageio/src/ImageFormat.cpp`、
  `src/loaders/imageio/src/ImageCodec.cpp`。
- 词汇来源：`src/base/iobase/sdk/vine/io/IoError.hpp`（`Result` / `IoError`）。
- 同层对照：`meshio-brepio-design.md`（同一套失败语义与能力表；形状差别的理由见 2.3 / 2.4）。
- 数据形状：`imaging-design.md`；VFS：`iobase-vfs-design.md`。
