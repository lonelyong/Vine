# loaders/ 层模块索引

> meshio / brepio 见本文；`imageio` 见 `.ai/memory/imageio.md`。
> 现行设计：`../design/meshio-brepio-design.md`。

## 三层结构与不变量

```
base/*        ← 数据/算法（geometry 装 Mesh/BrepShape，imaging 装 Image）
loaders/*     ← 用三方库把数据从文件里读出来/写回去
```

**不变量**：数据模块（`geometry` / `imaging`）**不依赖任何三方库**；三方库只在 `loaders/` 里、
以 `PRIVATE` 链接、静态烘进各自 DLL。所以消费者看不到它们，也不会被迫构建它们。

## 公开面（2026-10-27 重新设计）

- 两个模块同形：`MeshFormat` / `BrepFormat` 枚举 + `formatFromPath()` / `formatName()`
  （meshio 另有 `formatInfo()`）；读返回 `vn::io::Result<T>`，写返回 `vn::io::IoError`；类只有
  `Loader`（options + cache）与 `Exporter`（options）。
- **格式表是唯一词汇**：`supportedExtensions()` 由枚举生成；表外扩展名 `load()` 直接回 `Unsupported`
  （不再透传 assimp 的注册表，`.3ds`/`.x`/`.x3d` 需在表里加一行）。`exportAs` 不再吃写入器名
  （`objnomtl`/`collada`/`gltf2` 都收进枚举里）。
- **无单例**：`defaultInstance()` 已删；options 构造时给、只读，缓存属于实例且**不线程安全**。
- meshio 六个导出入口：`exportAs(mesh|model, format, path|stream)` + `createSource(mesh|model, format)`。
  - Model → STL/PLY 的烘平由 **assimp 的导出器注册**完成（`stl`/`ply` 强制 `aiProcess_PreTransformVertices`，
    并先复制场景）；OBJ 导出器自己按节点累积矩阵写顶点。我们只建忠实的 aiScene，**不要再自己烘一遍**。
  - `streamable == false` 的格式（`.gltf` 要 `.bin`、`.3mf` 走路径）在 stream/source 目标上回 `Unsupported`；
    Model → stream 的 OBJ 丢 `.mtl`，几何与名字保留。
- `MeshSource` 与 `BinStlSource` / `AsciiStlSource` / `ObjSource` 已删：`createSource()` 是唯一懒入口，
  内容由 `exportAs(stream)` 产出（**整份进内存**，首次拉取时生成，`rewind()` 重放）。
- brepio：`find_package(OpenCASCADE CONFIG QUIET)` 可选链接（PRIVATE：`TKDESTEP` `TKDEIGES` `TKXSBase` `TKBRep`
  `TKTopAlgo` `TKernel` `TKMath`），找到才定义 `VN_BREPIO_HAS_OCCT`；找不到的构建里 `supportedExtensions()` 为空、
  读写回 `Unsupported`，`formatFromPath()` 照常回答。OCCT 8 的流读取是 `ReadStream(fileName, stream)`（没有单参数版），
  path 入口走 `ReadFile`；`BRepTools` 的 Read/Write 返回 `void`。
- `BrepShape` 持有 `std::shared_ptr<const TopoDS_Shape>`：`setShape()` 接管所有权，`shape()` 借用。

## 现状与坑

- **新增 `src/**/*.cpp` 要 `cmake -S . -B build`**（`vn_add_library` 的 glob 无 `CONFIGURE_DEPENDS`）；
  新增测试 `.cpp` 用的是 `CONFIGURE_DEPENDS`，会自动生效。
- 路径入口读两遍文件（一遍算指纹、一遍解析）；流入口先把流 `drain` 成字节，所以与 path 入口命中同一条缓存。
- 失败分类就是那四类：`Unsupported`（表外扩展名 / 无后端 / 非流式目标）、`NotFound`、`IoFailure`、`InvalidData`；
  `Result` 的 `*` / `->` 在失败时是 UB，先判 `ok()`。
- 两条 assimp 后处理表：`loadMerged()` 带 `aiProcess_PreTransformVertices`，`load()` **绝不能**带（会删节点图）。
- 法线：两份列表都带 `aiProcess_GenNormals`，代价是逐面法线导致顶点拆分（`JoinIdenticalVertices` 因此少合并）；
  换来"文件没给法线"时不会交出一份无法线的网格。
- 矩阵：assimp 的 `aiMatrix4x4` 与 `vn::math::Matrix4x4` 都是列向量约定、平移在第 4 列，16 个元素直接对应，**不转置**。
- 材质类型按 assimp 通用键选择：`$mat.metallicFactor|roughnessFactor` → `PbrMaterial`；`$mat.opacity < 1` 也 → Pbr
  （只有它装得下透明度）；`$clr.specular`/`$mat.shininess` → `PhongMaterial`；只有 `$clr.diffuse` → `ColorMaterial`。
- robotics/io 有自己的 `StlMeshSource`（不链 assimp，见 `MeshBin.hpp`），与 meshio 的写出刻意重复：合并会让
  robotics 为一份 STL 写出器背上 assimp。
- iobase 的坑（真缺陷，已修）：libzip 不支持 seek **压缩**条目，且一次被拒的 seek 会毒化 reader —— `zip_file_is_seekable()`
  分流，不可 seek 就重新 `zip_fopen_index`（见 `../design/iobase-vfs-design.md`）。
- **OCCT 运行时不要进 `build/bin`**：不拷 DLL，让 `F:/vcpkg/installed/x64-windows/bin` 在 `PATH` 上即可；
  vcpkg 工具链构建由 applocal 拷，手工 glob 拷贝是错的做法（曾把 50 个 `TK*.dll` 倒进 bin）。
- **清单模式与全局安装不能共用**：清单已改名 `_vcpkg.json`（CMake / vcpkg 只认 `vcpkg.json`，改名即关掉清单模式），
  依赖声明仍留在里面给 CI 用；本机的 `CMAKE_TOOLCHAIN_FILE=F:/vcpkg/scripts/buildsystems/vcpkg.cmake` 写在
  `.vscode/settings.json` 的 `cmake.configureSettings` 里 → 经典模式，直接用 `VCPKG_INSTALLED_DIR`（默认
  `F:/vcpkg/installed`）里的全局安装，并由 applocal 把 DLL 拷到输出目录，不需要手工拷。
  为什么不能再叫 `vcpkg.json`：它会按 `builtin-baseline`（opencascade 解析成 8.0.1#0）重解析，全局装的是 8.0.0#0；
  `overrides` 能钉回 8.0.0，但依赖链（`freetype` `libpng` `zlib` `brotli` `bzip2` `opengl*` `egl-registry`
  `vcpkg-cmake*`）在当前 baseline 里都更新 → 仍要重建，重建还要 vcpkg 自己下 CMake 4.4.3（本机 SSL 不通）；
  且清单模式会把 `VCPKG_INSTALLED_DIR` 里**未声明的包全删**（实测列出 boost/opencv/pcl 数百条 "will be removed"）。
- **待办**：`.3ds`/`.x`/`.x3d` 不在表内；PLY 没有原生逐记录写出（都走 assimp 的整份映像）。

## 历史登记 —— 2026-09-12 modelio 拆分 / 2026-09-28 三个内容源

- `modelio` 拆成 `meshio`（绑 assimp）与 `brepio`（当时零三方依赖）。体积证据：`libviMeshIOd.so` 87.8 MB vs
  `libviBrepIOd.so` 708 KB。
- 2026-09-28 的 `MeshSource` / `BinStlSource` / `AsciiStlSource` / `ObjSource` 家族（`sdk/vine/meshio/MeshSource.hpp`）
  让包内写出不落临时文件；2026-10-27 收进 `MeshExporter::createSource()`，那几个类退出 SDK。
- 它们当时的选择仍然成立：所有源都是"拉时生成"，用 `DataSource` 而不是自定义 `streambuf`；固定布局能报长度，
  文本布局报 `kUnknownSize`；改过的 mesh 会被拒（revision 比对）。
