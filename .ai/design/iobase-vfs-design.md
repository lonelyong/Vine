# IOBase（vn::io）核心能力设计 —— VFS 与 Stream

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`Vfs` / `Stream` / 错误模型 / 路径 / 后端 / MountVfs；迁移与门禁。
> **权威来源**：`src/base/iobase/sdk/vine/io/**`、`tests/test_iobase`。
> **待办**：`../backlog/`

## 1. 概要设计

### 1.1 问题与背景

旧 VFS 只有 `bool` 返回、没有 `stat`、不能部分读写、写权限不可预知；打开 zip 全量驻留内存；
`DirectoryVfs` 的路径校验只按子串拒 `..`，绝对段可能逃出根。需求是：可区分错误、惰性 zip、流式读写、多后端一棵树。

### 1.2 目标

- `IoError` + `Result<T>`：业务失败走返回值，异常只用于编程错误。
- 路径校验收敛到 `normalizeVfsPath`：无工作目录、绝对拼写拒绝、`..` 按段抵消。
- `Vfs` 家族：`DirectoryVfs` / `ZipArchive` / `MountVfs`；zip 惰性索引 + overlay + 原子落盘。
- 读流 / push sink / 写 source：大块数据不整条驻留。
- 方法级不线程安全、无内部锁；共享对象由调用方同步。

### 1.3 非目标

- 不做 `VfsPath` 值类型；不做核心 `IStream` 继承体系。
- 不建 `MemoryVfs`（`ZipArchive()` 无源树已覆盖）。
- 不做 TAR/GZIP 区分、缓存、HTTP/ACL/加密/去重。
- 不做 push 写（libzip source 是 pull）。

### 1.4 一句话结论

`vn::io` 用 `Result<IoError>` 区分失败、用规范化的虚拟路径做唯一寻址、用 `Vfs` 三后端覆盖目录/zip/挂载；
大块数据走 `openRead`/`read(sink)`/`addFile(source)`，落介质只在 `commit`/`saveAs`。

## 2. 设计要点

### 2.1 错误模型

- `IoError`：`NotFound` / `AlreadyExists` / `PermissionDenied` / `NotADirectory` / `IsADirectory` /
  `InvalidPath` / `ReadOnly` / `IoFailure` / `Unsupported` / `InvalidData` / `Closed` / `OutOfRange` /
  `CapacityExceeded` / `NotEmpty`。
- `Result<T>`：隐式构造成功值或 `IoError`；`ok()`、`error()`、`value()`、`take()`。
- 不做 `and_then`/`map` 组合子；`T=void` 直接用 `IoError`。
- 值而非异常；异常仍用于 `bad_alloc`/误用 `invalid_argument`。

### 2.2 路径校验

`detail::normalizeVfsPath(path, out)` 返回 `IoError`：

- 空路径 = 根；空段/重复分隔符/尾部 `/`/`.` 折叠。
- 前导 `/` 拒绝（虚路径无工作目录）。
- `\`、嵌入 `\0`、`:` 段拒绝（盘符/UNC/URI）。
- `..` 按段抵消；抵消到根之外 → `InvalidPath`。
- 非文本条目名按“合法 UTF-8 → 本机代码页 → 逐字节同值码点”解码，归档原字节重打包原样写回。

### 2.3 后端家族

```
Vfs
 ├── DirectoryVfs   真实目录；写即落盘，commit() = Ok
 ├── ZipArchive     ZIP：惰性索引 + 改动 overlay + commit/saveAs/toBytes
 └── MountVfs       多后端一棵树（最长前缀 + 优先级 + 只读）
Zip                 一次性 compress/decompress/entries/readEntry
```

- `ZipArchive` 三个开口：`open(..., ReadOnly)` / `open(..., ReadWrite)` / 默认构造（无源内存树）。
- 源 `zip_t` 全程只读；目标 `zip_t` 只在 `commit`/`saveAs` 时打开。
- `commit()`：临时文件 + 原子替换 + 重建索引；`saveAs(path|ostream)` / `toBytes()` 到别处。
- 未改动条目由 `zip_source_zip_file` 搬运；峰值内存 = 单条目。
- 读路径自己算 CRC 对照目录记录；不符 → `IoFailure`。

### 2.4 流与零拷贝

- `Stream.hpp`：`Fragment` / `DataSource` / `DataSink` / `kUnknownSize`，不含 Vfs 名字。
- `Vfs.hpp`：`VfsEntrySource`（`DataSource` 子类）。
- `Adapters.hpp`：`IstreamSource` / `OstreamSink` / `Conversion<In,Out>`。
- 读：`openRead(path)` 返回 `VfsEntrySource`；`read(path, DataSink&)` 是 push 变体（基类按 64 KiB 分块）。
- 写：`addFile(path, span)`（拷贝 1 次）/ `span<Fragment>`（0）/ `shared_ptr<DataSource>`（0，save 时拉）/
  `real_path`（0，save 时读）。
- `kUnknownSize` 允许：libzip 走 zip64 + 写完回填 local header；目标必须可 seek。
- 一个 source 实例只挂一个条目；回调在保存线程调用。

### 2.5 MountVfs

- 最长前缀优先；同前缀按 priority 降序；写路由到第一个可写后端。
- 枚举合并去重，并补出中间目录（挂了 `/data/a`、`/data/b`，列 `/data` 要看到 a/b）。
- 跨后端 `rename` = 读源 → 写目标 → 删源；失败保留源。
- 单后端命中直接转发；一个后端挂多处时 `commit()` 只调一次；只读挂载跳过。

### 2.6 并发

- 方法级不线程安全、无内部锁；共享对象/两对象共享存储由调用方同步。
- 互不共享（缓冲/句柄/存储）的对象可并发。
- `DirectoryVfs` 无文件锁，最后写者胜；`ZipArchive::commit()` 固定临时名，同一文件并发 commit 不支持。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 错误模型 | `IoError` + `Result<T>` | 已落地 |
| F2 | 路径校验 | `normalizeVfsPath` | 已落地 |
| F3 | 文件信息 | `stat` / `list` / `VfsEntryInfo` | 已落地 |
| F4 | 只读声明 | `isReadOnly()`；写操作立即 `ReadOnly` | 已落地 |
| F5 | 目录/文件操作 | create/rename/remove/removeAll | 已落地 |
| F6 | 读流/push | `openRead` / `read(sink)` / `copy` | 已落地 |
| F7 | 写来源 | `addFile(span/fragments/source/real)` | 已落地 |
| F8 | 后端家族 | Directory/Zip/Mount | 已落地 |
| F9 | 内存流 | `reserve` + 容量上限 | 已落地 |

### 3.1.1 F1 错误模型

- **行为**：业务失败返回 `IoError`；`Result<T>` 携带值或错误。
- **引用**：C1、R1、T1、N1。

### 3.1.2 F2 路径校验

- **行为**：`normalizeVfsPath` 折叠合法段、拒绝非法段。
- **引用**：C2、R2、T2、P2、P4。

### 3.1.3 F3 文件信息

- **行为**：`stat(path)` 返回 `VfsEntryInfo`（目录 `size=0`、`kind` 只有 File/Directory、`crc` 无记录为 0）；`list(dir)` 返回可直接回喂的完整路径。
- **引用**：C3、R3、T3、P5。

### 3.1.4 F4 只读声明

- **行为**：`isReadOnly()`；写/改操作第一行检查，命中立即 `ReadOnly`。
- **引用**：C4、R4、T4。

### 3.1.5 F5 目录/文件操作

- **行为**：`createDirectory(s)` / `rename` / `remove` / `removeAll`。
- **引用**：C5、R5、T5、P6。

### 3.1.6 F6 读流/push

- **行为**：`openRead` 返回 `VfsEntrySource`；`read(path, sink)` 分块推送；`copy` 跨树搬运。
- **引用**：C6、R6、T6、P7。

### 3.1.7 F7 写来源

- **行为**：四个 `addFile` 重载；`Fragment` 借用、`DataSource` 拉取、真实文件延迟读。
- **引用**：C7、R7、T7、P8。

### 3.1.8 F8 后端家族

- **行为**：`DirectoryVfs` / `ZipArchive` / `MountVfs`。
- **引用**：C8、R8、T8、P3。

### 3.1.9 F9 内存流

- **行为**：`MemoryStreamBuf` / `ChunkedMemoryStreamBuf` 的 `reserve` + `setCapacityLimit`。
- **引用**：C9、R9、T9、P9。

## 4. 公开接口

### 4.1 `Vfs`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `stat` / `list` / `kindOf` | 文件信息 | `Result` |
| `isReadOnly` | 只读声明 | 写操作先查 |
| `createDirectory(s)` / `rename` / `remove` / `removeAll` | 目录/文件操作 | `IoError` |
| `openRead` / `read(sink)` / `copy` | 读 | 流可活过 VFS |
| `addFile(span/fragments/source/real)` | 写来源 | 借用/拉取 |
| `commit` / `saveAs` / `toBytes` | 落介质 | 目标决定 |

### 4.2 错误/信息/流

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `IoError` / `Result<T>` | 值而非异常 | `ioErrorName()` |
| `VfsEntryInfo` | path/kind/size/crc/name | 目录 size=0 |
| `DataSource` / `DataSink` / `Fragment` | 字节流词汇 | 不含 Vfs 名字 |
| `MountVfs` | 多后端一棵树 | 最长前缀/优先级 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 业务失败走返回值；异常只用于编程错误。 |
| C2 | 虚路径无工作目录；前导 `/`/`\`/`:`/根外 `..` 拒绝。 |
| C3 | `stat`/`list` 只描述存在的条目；存在但非文件/目录返回 `NotFound`，缺失由 `Result` 错误码承担。 |
| C4 | 只读后端写操作立即 `ReadOnly`，不触碰后端。 |
| C5 | 目录非空删除返回 `NotEmpty`；跨后端 rename 失败保留源。 |
| C6 | 读流可活过 VFS；push sink 拒绝原样返回。 |
| C7 | 借用数据活到落介质；`DataSource::size()` 必须准确。 |
| C8 | zip 惰性索引 + overlay；落介质只在 `commit`/`saveAs`。 |
| C9 | 内存流上限只约束增长；`reserve` 失败不改缓冲。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | `ioErrorName()` 可读；诊断/测试用它。 |
| R2 | 后端落地前复验绝对路径。 |
| R3 | `list` 返回完整路径，可直接回喂。 |
| R4 | 只读挂载点/后端都参与只读判定。 |
| R5 | 路径校验先行；失败保留源。 |
| R6 | 读流允许短读（0 = EOF）；`error()` 说明 0 字节原因。 |
| R7 | 一个 source 只挂一条目；回调在保存线程。 |
| R8 | `commit()` 临时文件 + 原子替换 + 重建索引。 |
| R9 | 单字符写也受容量上限约束。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | `NotFound` 先看 `file_type`，再看 `error_code`。 |
| N2 | 非文本条目名可按 UTF-8 → 本机代码页 → 字节码点解码；重打包原字节写回。 |
| N3 | zip 读路径自己算 CRC；不符 → `IoFailure`。 |
| N4 | 方法级不线程安全；共享对象由调用方同步。 |

## 6. 注意事项与陷阱

| # | 坑 | 会怎么错 | 怎么判 / 怎么防 |
| --- | --- | --- | --- |
| P1 | `error_code` 判 `NotFound` | MSVC 在路径不存在时也置位 `ec`，把“不存在”误报成 `IoFailure`。 | `stat`/`list` 先看 `file_type::not_found`；按 N1 写，跨平台用例钉住。 |
| P2 | 绝对段逃逸 | `C:/…` 作为右操作数拼到根上会替换左操作数。 | `normalizeVfsPath` 拒绝 `:` 段/前导 `/`；路径校验只留一个入口。 |
| P3 | zip CRC | libzip 读路径不校验 CRC，损坏条目静默给错字节。 | 自己算 `crc32()` 对照目录记录；不符 `IoFailure`；`readEntries` 取 `ZIP_STAT_CRC`，流末尾对照。 |
| P4 | 用子串判 `..` | 按子串拒 `..` 会误杀 `a..b`，真逃逸却可能漏判。 | 只按段抵消；`normalizeVfsPath` 是唯一入口（C2）。 |
| P5 | 把 `VfsEntryInfo::path` 当名字 | 拿完整路径当名字会拼出双路径。 | 名字走 `name()`（R3）。 |
| P6 | `rename` 同名覆盖 | 覆盖语义按后端，`DirectoryVfs` 与 zip overlay 结果不同。 | 需要“不覆盖”时先 `stat`；跨后端 rename 失败保留源（C5）。 |
| P7 | 压缩条目 seek | 每次 seek 都从头解压该条目。 | 顺序读写；反复 seek 先解到内存流。 |
| P8 | `kUnknownSize` | 目标不可 seek 时 zip64 回填失败。 | 未知长度只写可 seek 目标，否则先缓冲（C7）。 |
| P9 | `release()` / `clearData()` | 以为清空内容会连上限一起重置。 | 上限独立于内容；移动构造/赋值带上限（C9）。 |

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | `VfsPath` 值类型 | 编译期防裸串的收益不抵全 API 改造；校验才是缺口。 | — |
| D2 | 核心 `IStream` 继承体系 | 阶段 1 用 `Result<VfsStream>` + `intrusive_ptr<Buffer>` 够用。 | — |
| D3 | `MemoryVfs` 类型 | 已被 `ZipArchive()` 无源树 + `toBytes()` 覆盖；它做不到惰性。 | — |
| D4 | `save(path)` 一个名字 | 写回自己 vs 另存别处目标不同；拆 `commit`/`saveAs`。 | — |
| D5 | push 写 `openWrite()` | libzip source 是 pull；push 只能缓冲或 spool。 | — |
| D6 | 大包 `toBytes()`/`saveAs(ostream&)` | 峰值 ≈ 整包 ×2；大包走 `saveAs(path)`/`commit`。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 错误模型/`Result` | `test_iobase` |
| T2 | 路径校验/绝对段/`..` | `test_iobase` |
| T3 | `stat`/`list`/`kindOf` | `test_iobase` |
| T4 | 只读拒绝 | `ReadOnlyBackendRefusesEveryChange` |
| T5 | 目录/文件操作 | `test_iobase` |
| T6 | 惰性 zip/CRC/流 | `ZipArchiveOpensWithoutReadingContent` |
| T7 | `addFile`/`copy`/`kUnknownSize` | `test_iobase` |
| T8 | MountVfs 合并/跨后端 rename | `MountVfsTest` |
| T9 | 内存流上限/reserve | `MemoryStreamTest` |

## 9. 状态与演进

### 9.1 当前状态

S1/S2/S2.5 已落地：错误模型、路径校验、stat/list、只读、目录操作、流/来源、ZipArchive、MountVfs、内存流上限。

### 9.2 已知缺口 / 待办

- TAR/GZIP 区分、缓存、HTTP/ACL/加密/去重不做。
- `saveAs(ostream&)`/`toBytes()` 大包不可用；`toBytes()` 的 2× 峰值可消除，等真实消费者。
- 同一文件并发 `commit()` 不支持（固定临时名）。
- 核心 `IStream` 分层推迟；`Stream.hpp` 下沉 core 的触发条件是第一个不依赖 iobase 的消费者。

### 9.3 兼容性 / 迁移

S2.5 一次断代：`IMemoryVfs` → `Vfs`；`ZipMemoryVfs` 删除；`save(path)` → `saveAs`/`toBytes`；
`bool` 家族删除。新代码只按 §4 的接口写。

## 10. 参考

### 10.1 代码

- `src/base/iobase/sdk/vine/io/Vfs.hpp`
- `src/base/iobase/sdk/vine/io/Stream.hpp`
- `src/base/iobase/sdk/vine/io/Adapters.hpp`
- `src/base/iobase/sdk/vine/io/MountVfs.hpp`
- `src/base/core/sdk/vine/MemoryStream.hpp`
- `tests/test_iobase`

### 10.2 文档与 ADR

- `robotics-io-design.md`
- `appfw-document-model.md`
- `../backlog/`

### 10.3 外部资料

- libzip `zip_source_*` / `zip_file_*` 语义。
- `std::filesystem` 的 `file_type` / `error_code` 跨平台差异。
