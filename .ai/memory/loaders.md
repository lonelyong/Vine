# loaders/ 层模块索引

> 各模块自己的详细笔记：`meshio` / `brepio` 见本文；`imageio` 见 `.ai/memory/imageio.md`。
> 定位依据（为什么加载器不做成 base 模块）见 `.ai/design/imaging-design.md` §3。

## 三层结构与不变量

```
base/*        ← 数据/算法（geometry 装 Mesh/BrepShape，imaging 装 Image）
loaders/*     ← 用三方库把数据从文件里读出来/写回去
```

**不变量**：数据模块（`geometry` / `imaging`）**不依赖任何三方库**；三方库只在 `loaders/` 里、
以 `PRIVATE` 链接、静态烘进各自 DLL。所以消费者看不到它们，也不会被迫构建它们。

## 2026-09-12：`modelio` 拆成 `meshio` + `brepio`

**为什么拆**：`modelio` 里是两件不相关的事 —— mesh I/O（**绑 assimp**）和 B-rep I/O（**零三方依赖**）。
只有一半需要 assimp，合在一起意味着"只想要 B-rep 加载器的人也得构建 assimp"。
模块名 `modelio` 对两半都不贴切。

**拆法**（纯拆分，零行为改动）：

| | `vn::MeshIO` | `vn::BrepIO` |
| --- | --- | --- |
| 目录 | `src/loaders/meshio/` | `src/loaders/brepio/` |
| 短名 / 宏 / 命名空间 | `MeshIO` / `VN_MESHIO_*` / `vn::meshio` | `BrepIO` / `VN_BREPIO_*` / `vn::brepio` |
| 头 | `MeshLoader.hpp` `MeshExporter.hpp` | `BrepLoader.hpp` `BrepExporter.hpp` |
| 依赖 | PUBLIC `Runtime Geometry Crypto` + **PRIVATE `assimp::assimp zlibstatic`** | PUBLIC `Runtime Geometry Crypto`（**零三方**） |
| 测试 | `tests/test_meshio`（**8**） | `tests/test_brepio`（**3**） |

- assimp 的 FetchContent 块**整体搬到 `meshio`**；`brepio` 的 CMakeLists 只有两行。
  注意 assimp 吃 `base/iobase` 产出的 `zlibstatic`，所以 `src/CMakeLists.txt` 里 `base` 必须先于
  `loaders` 配置（原有约束，未变）。
- 消费方：只有 `tools/urdf2vine`（用 `MeshLoader`）→ 改成 `vn::MeshIO` + `vn::meshio`。
- **体积证据**：`libviMeshIOd.so` **87.8 MB**（assimp 在内）vs `libviBrepIOd.so` **708 KB** ——
  拆开的收益是具体的。

## 2026-09-28：三个 mesh 内容源（`BinStlSource` / `AsciiStlSource` / `ObjSource`）—— 写出推迟到 save

**新增** `sdk/vine/meshio/MeshSource.hpp`（抽象基类，导出）+ `BinStlSource.hpp` / `AsciiStlSource.hpp` / `ObjSource.hpp`
（各一个 .cpp）：源把一棵 `Mesh` 现排成字节，但**构造时不转** —— 字节在 `read()` 被拉时才由 mesh 现算。
用法 `archive.addFile(u8"geoms/link.stl", std::make_shared<BinStlSource>(mesh))`，`ZipArchive::saveAs` 时才真正排版
（libzip 按需拉），全程 O(块)、没有模型的第二份拷贝。

**为什么是一个基类三个格式**：三者共享的全部东西（持有 mesh、盯 `Buffer::revision()`、拉取游标、一条记录的 scratch、
“末尾验长度”）与格式无关，“一条记录长什么样”才与格式有关 —— 所以基类做前者，格式只答三个问题：
**几条记录** / **一条最多多少字节** / **第 i 条怎么排**。名字也不是多余的：使用者拿的就是三种格式之一。
判据可复用：**共享的部分必须是“与格式无关”的那一半**，否则就变成"为了少写十行而多一层"。

为什么是 `DataSource` 而不是“一个 STL 的 istream”（2026-09-28 定稿，判据值得复用）：

- **所有源都是“拉时生成”**：`ifstream` 也是从文件系统现取字节，它只是**标准库已经写好了 streambuf**。所以“从 mesh 现转”不是特例；
  真正要选的是”包装成 streambuf 还是实现 `DataSource`“。
- 写 streambuf 要实现 `underflow`/`xsgetn`/`seekoff`/`seekpos` + get-area 约定（容易错）；`DataSource` 只要 `size`/`rewind`/`read`/`error`。
- `DataSource` 路线**自持 mesh 句柄**，不用调用方保住一个栈对象活到 `saveAs()`。
- **固定布局的能报长度**：binary STL 是定长（80 字节头 + 4 字节三角形数 + 50 字节/三角形）⇒ `size() = 84 + 50n` 不必先排版就知道；
  **文本布局的报 `kUnknownSize`**（`AsciiStlSource` / `ObjSource`）—— 从前这是硬门槛（“ZIP 得先知道长度”），现在不是：
  libzip 对没声明长度的源写 **zip64 头 + 写完回填**（`.ai/design/iobase-vfs-design.md` §8.5 末条），代价只是那条目 zip64。
- **现在自己写 writer**（三个源都是），所以 byte 与 `MeshExporter` 的路径/流重载**不一致**：assimp 会去重顶点、按材质分组，
  我们一行一顶点/一面。两者只承诺“同一几何、合法文件”，**不承诺字节相同** —— 故用例只钉“包内往返”（`MeshLoader` 读回）与文本形态，
  不钉与 `exportAs*` 的输出相等。**assimp 帮不上忙**：它的写出面只有“落文件”（`Exporter::Export`）或“整份 blob”。

实现要点：**每条记录都是“序号（或三角形号）的纯函数”** ⇒ 调用方的缓冲把一条记录切开时，重新生成一次即可（没有半成品状态要携带）；
`rewind()` 只归零游标/记录号，两次 save 内容逐字节一致。**改过的 mesh 会被拒**：基类存了交接时三个 `Buffer` 的 `revision()`，
每次 `read()` 比对，动过就报 `InvalidData`（把“一半旧一半新”从静默错误变成失败的 save）；
而**替换**缓冲（`setPositions`）不算改 —— 源继续读它当初拿到的那份存储，正是它承诺的内容。

**依赖变化**：`MeshIO` 现在 **PUBLIC 链 `IOBase`**（`MeshSource` 把 `vn::io::DataSource` 放进公开签名）。分层允许（两者都在 `base`，
loaders 在其上）；且 meshio 早就吃 iobase 产的 `zlibstatic`，构建顺序本来就在它之后。
若将来不想让 loaders 链 iobase：把 `Stream.hpp` 的词汇下沉 core（iobase 侧已无 Vfs 名字，可整头平移）。

## 2026-09-28：进出 `std::istream`/`std::ostream` 的两个入口

给 `MeshLoader` / `MeshExporter` 各加一个流入口，配合 iobase 的 `DataSourceStream`（`DataSource` → `std::istream`）：

- `MeshLoader::load(std::istream& in, const char* format_hint)`：先把流 `drain` 成一段内存，再 `Importer::ReadFileFromMemory`
  （assimp 只吃路径或内存；**hint 必需**，内存里没有扩展名可猜）。指纹按**读到的字节**算 ⇒ 与 `load(path)` **命中同一条缓存**。
- `MeshExporter::exportAsStl(const Mesh&, std::ostream& out)`：assimp 没有流式写出面 ⇒ `ExportToBlob` 拿整份 blob 再推给流
  （**省临时文件，不省内存**）。文件目标仍优先 path 重载。
- `MeshExporter::exportAsObj(const Mesh&, std::ostream& out)`：同理，但走 assimp 的 **`"objnomtl"`**（无材质变体）——
  普通 `"obj"` 写出器**总会另开一个 `.mtl` 文件**（`ExportToBlob` 返回的是 blob **链**，一个流装不下），
  若只推第一个 blob，文本里的 `mtllib`/`usemtl` 就会指向一个从没写出来的库。两个写出入口共用
  `writeSceneToStream()`（blob → 流，并拒绝"写出多于一个文件"的情况）。
- 结果：包内模型**不用落盘**就能读回 —— 条目 → `vn::io::DataSourceStream` → `load(stream, "stl"/"obj")`；
  两个格式各有"写出到流 → 从流读回"的往返用例（`MeshStreamIoTest`，配合 iobase 的 `DataSourceStreamTest`）。

## 2026-09-28：brepio 跟上同一对入口（对称性）

`BrepLoader` / `BrepExporter` 与 meshio 对齐 **文件 + 流** 两个目标：

- `BrepLoader::load(std::istream& in, const char* format_hint)`：hint 是扩展名形式（`".step"`/`"step"`）。
  **OCCT 的 reader 本身就吃流**（`STEPControl_Reader::ReadStream` / `IGESControl_Reader::ReadStream`）⇒ 不像 meshio 需要先 drain，
  所以这里**刻意没有**内存镜像那一步。**当前仍是桩**（返回 null，与 `load(path)` 一致，无 OCCT）；注释里写明"这就是后端将来落地的地方"。
- `BrepExporter::save(std::ostream& out, const BrepShape& shape)`：**纯虚**，与 `save(path, shape)` 并列 ——
  写出器必须对两个目标都表态（造不出来就 `return false`）。OCCT 的 writer 也吃流（`STEPControl_Writer::WriteStream` /
  `IGESControl_Writer::Write`），所以"能写文件就能写流"在这个后端上成立。
- 没有 OCCT ⇒ 无法测真解析/真排版；测试用**替身写出器**（测试文件内的 `RecordingExporter`，带 `VN_OBJECT_META_IMPL`）
  钉住两个入口都被调用、且参数正确；loader 侧钉住两个入口今天都返回 null（用例名就写着 "NotWiredYet" 的语义）。

**2026-09-28 实跑（两模块同批）**：`test_meshio` 21/21、`test_brepio` 5/5、`test_iobase` 85/85、`test_robotics_io`（另一个消费者）也过；构建 0 warning。
首跑抓到一处**用例自身的错**：`StlSourceTest.AppliesTheScaleFactor` 写死 `134` 字节，而 `makeIndexedQuad()` 是**两个**三角形（`84 + 50×2`）——
尺寸类的断言要用表达式（`84u + 50u * 2u`）而不是手算常量，否则几何夹具一改就错得没道理。

**同日第二批（三个源）实跑**：`test_meshio` 31/31、`test_iobase` 86/86、`test_brepio` 5/5、`test_robotics_io` 过；构建 0 warning。
首跑 3 红：两处是用例自己的错（缩进的 `vertex` 行没被行首匹配算上；尺寸断言手算常量），
**一处是真缺陷（在 iobase）**：从**保存过的包**里读一个**压缩**条目再 `rewind()` 会得到空流且不报错 ——
`EntryReadStream::seek` 直接用 `zip_fseek`，而 libzip **不支持** seek 压缩条目（`zip_fseek` 手册：“only works on uncompressed (stored), unencrypted data”），
且**一次被拒的 seek 会毒化它自己的 reader**（`zip_fread` 之后永远返回 -1）；旧用例只跑了内存态/未压缩条目，所以一直绿。
修法：用 `zip_file_is_seekable()` 分流 —— 可 seek 就真 seek，不可 seek 就 `zip_fopen_index` **重新打开**（向前偏移用读并丢弃），失败落进 `error_`。

## 现状与坑

- **`BrepLoader::load()` 仍是桩**，返回 null（`BrepLoader.cpp` 的 TODO：需要 OpenCASCADE）。
  能用的只有 `isSupportedFormat` / `options` / `defaultInstance`。**测试只覆盖这些** ——
  别把"有 BrepLoader" 误读成"能读 STEP"。
- `brepio` 的加载后端（OCCT）将来以 `PRIVATE` 链进 `brepio` 即可，**mesh-only 消费者不受影响**。
  这正是拆模块要换来的东西。
- `MeshLoader::load()` 有**按内容指纹的内存缓存**（`Crypto` + `Runtime::InMemoryCache`），
  所以这两条依赖是 **PUBLIC**（缓存类型出现在公开头里）。
- **新增/移动文件后必须 `cmake -S . -B build`**（`vn_add_library` 的 glob 无 `CONFIGURE_DEPENDS`）。
- **搬完后记得清陈旧产物**：`rm -rf build/src/loaders/<old> build/lib/libvi<Old>*`，
  否则旧 `.so` 会留在 `build/lib` 里造成困惑（本仓有过"构建产物不一致导致假结果"的教训，见
  `.ai/memory/graphics.md` 的验证口径章节）。

## 判据（2026-09-12）

- `test_meshio` **8** + `test_brepio` **3** = 11（与拆分前 `test_modelio` 的 11 个一致，**零丢失**）；
- 全量 `ninja` **零 error / 零 warning**；`urdf2vine` 正常链接构建；
- 全仓 `modelio` 引用清零（只剩两个 CMakeLists 里刻意的 "Split out of the former `modelio`"）。
