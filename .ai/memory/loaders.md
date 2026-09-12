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

| | `vi::MeshIO` | `vi::BrepIO` |
| --- | --- | --- |
| 目录 | `src/loaders/meshio/` | `src/loaders/brepio/` |
| 短名 / 宏 / 命名空间 | `MeshIO` / `V_MESHIO_*` / `vine::meshio` | `BrepIO` / `V_BREPIO_*` / `vine::brepio` |
| 头 | `MeshLoader.hpp` `MeshExporter.hpp` | `BrepLoader.hpp` `BrepExporter.hpp` |
| 依赖 | PUBLIC `Runtime Geometry Crypto` + **PRIVATE `assimp::assimp zlibstatic`** | PUBLIC `Runtime Geometry Crypto`（**零三方**） |
| 测试 | `tests/test_meshio`（**8**） | `tests/test_brepio`（**3**） |

- assimp 的 FetchContent 块**整体搬到 `meshio`**；`brepio` 的 CMakeLists 只有两行。
  注意 assimp 吃 `base/iobase` 产出的 `zlibstatic`，所以 `src/CMakeLists.txt` 里 `base` 必须先于
  `loaders` 配置（原有约束，未变）。
- 消费方：只有 `tools/urdf2vine`（用 `MeshLoader`）→ 改成 `vi::MeshIO` + `vine::meshio`。
- **体积证据**：`libviMeshIOd.so` **87.8 MB**（assimp 在内）vs `libviBrepIOd.so` **708 KB** ——
  拆开的收益是具体的。

## 现状与坑

- **`BrepLoader::load()` 仍是桩**，返回 null（`BrepLoader.cpp` 的 TODO：需要 OpenCASCADE）。
  能用的只有 `isSupportedFormat` / `options` / `defaultInstance`。**测试只覆盖这些** ——
  别把"有 BrepLoader" 误读成"能读 STEP"。
- `brepio` 的加载后端（OCCT）将来以 `PRIVATE` 链进 `brepio` 即可，**mesh-only 消费者不受影响**。
  这正是拆模块要换来的东西。
- `MeshLoader::load()` 有**按内容指纹的内存缓存**（`Crypto` + `Runtime::InMemoryCache`），
  所以这两条依赖是 **PUBLIC**（缓存类型出现在公开头里）。
- **新增/移动文件后必须 `cmake -S . -B build`**（`v_add_library` 的 glob 无 `CONFIGURE_DEPENDS`）。
- **搬完后记得清陈旧产物**：`rm -rf build/src/loaders/<old> build/lib/libvi<Old>*`，
  否则旧 `.so` 会留在 `build/lib` 里造成困惑（本仓有过"构建产物不一致导致假结果"的教训，见
  `.ai/memory/graphics.md` 的验证口径章节）。

## 判据（2026-09-12）

- `test_meshio` **8** + `test_brepio` **3** = 11（与拆分前 `test_modelio` 的 11 个一致，**零丢失**）；
- 全量 `ninja` **零 error / 零 warning**；`urdf2vine` 正常链接构建；
- 全仓 `modelio` 引用清零（只剩两个 CMakeLists 里刻意的 "Split out of the former `modelio`"）。
