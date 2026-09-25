# IOBase（vn::io）核心能力设计 —— VFS 与 Stream

> 状态：**设计稿，分阶段实现中**。现状：`Vfs` / `ZipVfs` / `DirectoryVfs` /
> `ZipArchive` 已落地（以 `sdk/vine/io/` 下的头文件为准），本设计只覆盖**核心功能缺口**，不照搬
> 外部需求文档的 API 形态。
>
> 阶段：**S1 / S2 已完成**（错误模型、路径校验、`stat`/`list`、`isReadOnly`、目录与删除操作）。
> **S2.5 已完成：API 重设计** —— 统一的 `Result`/`IoError` 返回、零 out-parameter、
> 每个操作只有一个名字、派生便利函数；旧的 `bool` 家族（`writeFile` / `readFile` / `remove` /
> `save(vector&)` / `mountFile` / 纯虚的 `exists` 等）已**删除**，`robotics::io` 与两套测试已同步迁移。
> **S3a 已完成：惰性只读 ZIP 后端** —— `ZipArchive::entries()` + `ZipVfs`（只读索引，条目按需解压），
> `robotics::io` 的 `loadPkg(path)` 与嵌套 `.vdevpkg` 已改用它。
> **S3a 收口：ZIP 家族职责收敛** —— 删除 `ZipMemoryVfs::openZip`（两个入口做同一件事）与
> `ZipArchive::entryNames`（被 `entries()` 取代）；`ZipArchive` 只做字节级编解码（§9.1）。
> **S3b 已落地（§9）**：`ZipMemoryVfs` **删除** —— 它的实现并入 `ZipArchive` 的 empty 状态 + 薄层 `ZipVfs`；
> `IMemoryVfs` → **`Vfs`**（无 `I` 前缀，用户明确要求）；唯一的 zip 后端 `ZipVfs` = 只读源句柄长持（读流可活过 VFS）
> + 改动 overlay + `commit`/`saveAs`/`toBytes`，三个开口 `open(…, ReadOnly)` / `open(…, ReadWrite)` / 默认构造；
> `ZipArchive` 成为存储层：opened 状态 + `openRead`（流式）+ 回调/片段来源 + 流式 `saveAs`/`commit` + CRC 校验。
> 过渡拼写已清（S3b 收尾）：`ZipVfs::openZip`、`Vfs::save` / `serialize`、`ZipArchive::save` 包装全部删除，
> 调用点已迁到 `open(…, ReadOnly)` / `saveAs` / `toBytes`；同一个动作只剩一个名字。
> 流词汇头文件也从 `VfsStream.hpp` 改名 **`Stream.hpp`**（同目录，内容不变）：它是 IOBase 级词汇，
> 存储层与 VFS 层共用，旧名错误地把它归给了 VFS（论证见 §8）。
> **条目记录统一（S3b 收口）**：`ZipEntryInfo` **删除** —— 条目信息只有一套词汇，即 `Vfs.hpp` 的 `VfsEntryInfo`
> （完整虚拟路径 + `is_directory` + 大小，目录不带尾部 `/`；原名 `FileInfo`，同日改名让词汇带上 `Vfs` 前缀）；
> `entries()` / `children()` / `index()` 一律报它。
> ZIP 私有的存储名拼写与 CRC 降为 `ZipArchive.cpp` 的 `StoredEntry` 内部记录，`adoptHandle` 的 CRC 校验链不变。
> **路径语义收紧（同日）**：前导 `/` 不再折叠成相对路径，改为 `InvalidPath` —— 虚路径无工作目录，
> 绝对拼写命不中任何条目；空段 / 重复分隔符 / `.` / 尾部 `/` 仍折叠。§4 的表与实现要点、§14 门禁表已同步。
> 本轮实测：`test_iobase` 45/45（`DirectoryVfsNeverEscapesItsRoot` 转绿），`test_robotics_io` 全绿。
> **命名收紧（同日）**：`importFile` → **`addFile(path, real_path)`**、`importDirectory` → **`addDirectory`** —— 与
> `addFile` 一族同族，名字直接说明"往树里加一个文件 / 一整棵目录"；§6 / §8 / §9 / §13 已同步。
> **词汇提升（同日）**：`ZipArchive::EntryKind` 删除，提为 `Vfs.hpp` 的 `VfsEntryKind` + 非虚 `Vfs::kindOf(path)`
> （由 `stat()` 派生；非法路径与不存在都报 `Missing`）；ZIP 内部条目表查询改名 `entryKindOf()`；`test_iobase` 46/46。
> **层次收敛（同日）**：一次性 ZIP 操作（`entries` / `readEntry` / `decompressFile`）从 `ZipArchive` 移入基础层
> `Zip.hpp` —— `ZipArchive` 只剩“打开态的 Vfs 后端”；两边共用的目录读取/存储名拼写下沉为 `src/ZipInternal.hpp`
> （与 `VfsInternal.hpp` 同款做法）。> **Zip 层口径统一（同日）**：`Zip` 全部走 `Result` / `IoError`（零 out-parameter，字节入参统一 `std::span<const unsigned char>`）：
> `compress` / `decompress` 返回字节，`readEntry` 返回 `Result<std::vector<unsigned char>>`，`compressDirectory` / `decompressFile` 返回 `IoError`。
> 错误映射：坏 zlib 流 / 非归档 → `InvalidData`，解压超上限 → `CapacityExceeded`，缺文件 → `NotFound`，
> 目录条目（尾 '/'）→ `IsADirectory`，空 name → `InvalidPath`，解包时越界条目名 → `InvalidPath`。
> **写入口统一（同日）**：加文件的一族名字全部收进 **`addFile`**（`write` / `addLocalFile` / `importFile` 一律并入，
> 目录侧 `addLocalDirectory` → **`addDirectory`**）—— `addFile(path, span<const unsigned char>)` /
> `addFile(path, const std::filesystem::path&)` / `addFile(path, std::span<const Fragment>)` /
> `addFile(path, std::shared_ptr<DataSource>)` **同名同通道**（都返回 `IoError`）；守卫补齐：只读先拒 → 路径先规范化 →
> 目录拒绝（`IsADirectory`）→ 空 source / 无数据片段报 `InvalidData`；空片段列表写空文件（与空 span 一致）。
> **写入口挂上基类（同日）**：四个 `addFile` 重载全部落在 **`Vfs`** 上 —— 片段与拉取来源两个此前只在 `ZipArchive`，
> 而调用方（`robotics::io` 全线持 `Vfs&`）够不着。它们是虚函数 + **默认实现**（只读先拒 → 片段拼成一个缓冲 / 来源按
> `size()` 拉满 → 委托 `addFile(path, span)`）；`ZipArchive` 改为 `override` 保住借用与惰性，`DirectoryVfs` 继承默认，
> 用 `using Vfs::addFile` 让基类重载在具体类型上也可达。新用例走 `Vfs&` 分别钉住两条路径；`test_iobase` 50/50。
> **打开模式显式化（同日）**：`openForRead(path | bytes)` 删除，改为 **`open(path | bytes, OpenMode)`** ——
> `OpenMode::ReadOnly` 全拒、`ReadWrite` 可改，**没有默认值**："能不能写"回到一个名字 + 一个模式（§9.1 的
> fstream 类比），`openRead` / `openForRead` 的词序撞脸随之消失；robotics 与两套测试调用点已同步，`test_iobase` 50/50。
> **内存字节两个入口（同日）**：`open(vector, …)` 拆成 **`vector&&`**（接管：右值引用挡掉"左值静默整包拷贝"）与
> **`span<const unsigned char>`**（借用：不持有、零拷贝，契约写死"活到 archive **及其所有读者**结束"，读者会带着 handle
> 活过 archive）；handle 里改成 `owned + span` 一种表示；新用例 `ZipArchiveTest.OpensBorrowedBytes` 钉住借用与"流活过 archive"。
> **条目记录带上校验和（同日）**：`VfsEntryInfo` 增加 `crc`（后端记录的内容校验和，未记录为 0）；`Zip::entries`
> 与 `ZipArchive::stat` / `list` / `index` 都填它（源归档条目有值，缓冲 / 文件 / 生成条目在写出前为 0），`DirectoryVfs` 恒为 0；
> `ZipArchive::Entry` 不再自带 `size` / `crc` / `is_directory`，改为内嵌 `VfsEntryInfo info`（path 仍由表键承担）。> `test_iobase` 39/39（S3b 时点记录；HEAD 重构后为 45 例），`test_robotics_io` / `test_core` / `test_crypto` / `test_runtime` / `test_system` 全绿。
> **虚拟路径统一到 `std::filesystem::path` + 归档名字“不拒收”解码（同日，用户决策，取代上一版 `String` 载体方案）**：
> 上一版把虚拟树里的名字当**字节载体**（`vn::String`），理由是归档名字可能根本不是文本、`fs::path` 在 Windows 上装不下
> 非法 UTF-8（构造即抛）。用户否掉了它：`path` 与 `String` 混用才是真正的问题（"统一，要么都用 path，要么都用 String"），
> 而"装不下"要靠**解码策略**解决，不是换类型 —— `String` 一样要面对非法字节，只是把问题推迟到边界。
> 现在分工固定：**路径一律 `std::filesystem::path`**（虚拟与真实同型），**内容一律字节**（`span<const unsigned char>`）。
> 代价（用户明确接受）：`addFile(path, real_path)` 两个参数同型，传反从编译错变成语义错。
> 解码策略是唯一一处"猜"，且**永不失败**（`detail::fromStoredName` 按序降级）：
> 1. **合法 UTF-8** → 直接用（现代写入方；EFS/UTF-8 位与 Info-ZIP Unicode Path extra field 0x7075 的升级在 libzip 读中央目录时已完成）；
> 2. **本机代码页** → 历史写入方（GBK 名字读回它的文本）；转换失败会抛 `std::system_error`，故这一步包在 `try` 里；
> 3. **都不是** → `detail::promoteBytesToText`：每个字节写成同值码点（经典保字节拼写）。它是**单射**（不同名字不会撞成一个）、
>    **不会失败**，于是"名字读不出"不再等于"条目不可达"。
> 配套三条：
> - **读归档取原样字节**：`readEntries` 传 `ZIP_FL_ENC_RAW` —— libzip 默认把非 UTF-8 当 CP437 翻译成 UTF-8，那是**编造**原文没有的文本；
> - **重打包写回原字节**：`StoredEntry::stored` / `ZipArchive::Entry::stored_name` 保存归档里的原始拼写，`emit()` 优先用它
>   （`zip_file_add(..., ZIP_FL_OVERWRITE)`，不强制 `ZIP_FL_ENC_UTF_8` —— 强制声明会让 libzip 直接拒收非 UTF-8 名字）；
>   所以"从外国归档读进树、未改名、再写出"是**字节保真**的，其余条目按 UTF-8 写出；
> - **按名字查条目也认解码结果**：`detail::locateEntry` 先按 UTF-8 拼写与存储字节比对，未命中再逐条解码比对文本，
>   于是"树里拿到的名字"必然能读回内容（`Zip::readEntry` 两个重载都走它）；目录标记取自归档（尾 `/`），不再要求调用方带尾斜杠。
> 建虚拟路径**不要用 `operator/`**（Windows 上拼原生 `\` → 被路径校验拒），用 `detail::joinVfs`；归档字节映射用 `detail::toUtf8Generic`。
> 回归用例 `tests/test_iobase/EncodingTest.cpp`（5 例）：非 ASCII 名字的三条边界（真实目录 ↔ 虚拟路径 ↔ ZIP 条目名）+
> legacy 本机编码名往返 + **既非 UTF-8 也非本机编码的名字仍可列出 / 读回 / 原字节写回**
> （夹具由归档字节就地改写名字、等长替换并清掉 UTF-8 位得到，因为本库已无法写出这样的名字）。
> 实测 `test_iobase` 全绿（EncodingTest 5/5）、`test_robotics_io` 全绿、`urdf2vine` 通过、include hygiene 0。
> 已知取舍：`DirectoryVfs` 落地到真实磁盘那一步仍要求名字能转成宿主文件名（真实名字由宿主决定）；`fs::path` 的值语义
> 本来就是宿主语义（`a\b` 在 Windows 是两个组件、`C:/x` 在 Windows 是绝对），故 `normalizeVfsPath` 仍显式拒 `\` / `:` / 前导 `/`。
> **流式面（§8）已落地（2026-09-25）**：写侧（片段 / `DataSource` 两类内容源）在 S3b 期间已在 `Vfs` 基类上（虚 + 默认实现，
> `ZipArchive` 覆写保惰性）；**读侧本次补齐**——`Vfs::openRead`（基类纯虚；`ZipArchive` 覆写复用既有 `EntryReadStream`、
> `DirectoryVfs` 以自有文件流覆写）+ `Vfs::read(path, DataSink&)`（基类虚 + 默认：经 `openRead` 按 64 KiB 分块推给 sink，
> sink 自己的错误原样返回、条目末尾的内容错误（损坏成员）按 `read()` 的口径报 `IoFailure`）。`§15.7`（流式 sink 写）
> 维持"本仓库无此需求"的结论。门禁见 §14。
> **同轮修掉一个 Linux 侧的真红（本次由用户 09-24 的 iobase 优化带出）**：`normalizeVfsPath` 一度要求整条路径是合法
> UTF-8。该闸门在 Windows 上从不触发（窄名到 path 已被代码页转成文本），而在**字节平台**上把上面这条"非文本条目名
> **不拒收**"的规则打反：`EncodingTest.HostEncodedNamesSurviveAZipRoundTrip` / `NamesThatAreTextInNoEncodingStayReachable`
> 在 Linux 上双双红（`addFile` / `read` 报 `InvalidPath`）。修法是按 §4 的规则删掉该闸门——名字是字节，解码规则归
> 归档层（`detail::fromStoredName`），`isValidUtf8` 仍为它服务。修后两棵树 `test_iobase` **58/58**。
> 下一步：`Vfs` 的流式**写**面与 `§15.7` 已如上收口；其余按 §15 的裁决记录（无待办）。
>
> 关联：外部《VFS 需求设计文档 v2.0》（下称"需求文档"）；第一个消费者
> `.ai/design/robotics-io-design.md`（`DeviceIO` / `WorkcellIO` 以 `vn::io::Vfs&` 为公共签名）。
> 约束：C++20；不引入新第三方依赖（libzip / zlib 已在 IOBase 内）；命名遵循
> `.github/copilot-instructions.md`。

## 1. 问题：现状的五个真实痛点

下表记录的是 **S1/S2.5 之前**的状态，也就是本设计要解决的东西（现已逐条落地，见 §13）。

| # | 痛点 | 证据 | 影响 | 本文章节 |
|---|---|---|---|---|
| P1 | **拿不到文件大小** | `IMemoryVfs` 只有 `exists/isFile/isDirectory/list/remove/writeFile/mountFile/readFile/save`，无 `stat`；`list()` 只回名字 | 想知道大小必须 `readFile` 全量读进内存；列目录要区分文件/目录得逐项 `isDirectory()`（N+1），大小完全拿不到 | §5 |
| P2 | **只能整文件读写** | 同上，无 `open` | 大文件无法部分/随机读；**没有追加**（写日志、增量构建只能 read-all + write-all）；不能边写边产出 | §8 |
| P3 | **错误不可区分** | 全部返回 `bool`；`DirectoryVfs::toReal` 对越界 `..` 返回空路径（`src/DirectoryVfs.cpp:34`），上层表现为 `false` | 调用方分不清"不存在 / 路径非法 / 只读 / 后端 I/O 错"，无法做正确反应（"不存在就用默认值" vs "I/O 错必须上报"） | §3、§4 |
| P4 | **写权限不可预知** | 无 `isReadOnly`；`DirectoryVfs::save(vector/ostream)` 直接 `false` | 写操作要试了才知道不行；需求文档 §6.5 要求"只读后端的写操作立即失败，不得延迟到实际写入" | §6 |
| P5 | **打开 zip 全量驻留内存** | `ZipMemoryVfs::openZip` 把所有条目读进内存 | 500 MB 资源包一打开就全量驻留；需求文档 §13.2 只要求"建索引 + O(1) 定位" | §9（**已修**：`ZipVfs`，`openZip` 本身已删） |

另有一处**安全缺口**（§4）：`DirectoryVfs::toReal` 只按子串拒绝 `..`，未拒绝形如 `C:/…` 的绝对段，
而 `std::filesystem::path::operator/` 在右操作数为绝对路径时会**替换**左操作数 —— 可能逃出 `root_`。

## 2. 边界：采纳 / 收敛 / 不做

| 需求文档条目 | 决定 | 理由 |
|---|---|---|
| §4 `VfsPath` 值类型（无默认构造、全 API 只收它） | **不做类型，但补校验** | 规范化已集中在 `detail::normalizeVfsPath` 一处；值类型买到的只是"编译期不可能传裸串"。真正的缺口是**校验**（§4），用返回 `IoError` 的规范化函数即可补齐，不动全部签名 |
| §5 核心 `IStream` 抽象 + §5.7 STL 适配层**分层** | **推迟** | 分层的价值是"错误可区分 + 非 STL 调用者"。阶段 1 用 `Result<VfsStream>` + `intrusive_ptr<Buffer>` 就能满足**功能**；等出现真正的文件流后端或非 STL 消费者再抽核心层 |
| §5.1 `close()` / §5.2 能力探测 | **收敛** | 能力探测用 `VfsStream` 的三个 bool + `size`；`close()` 阶段 1 由析构承担（**已知取舍**：析构期的写失败无处上报，见 §8.4） |
| §6.4 六种访问模式 | **收敛为 4 种** | `Read` / `Write`（截断/创建）/ `Append` / `ReadWrite` |
| §5.6 容量上限 + 预分配 | **采纳**（落在内存流） | 防不可信输入 OOM；预分配消除 log(n) 次重分配 | §11 |
| §9.3 TAR/GZIP 区分、§14 缓存、§16 TAR/HTTP/ACL/加密/去重 | **不做** | 现状也没做；不应过早进入核心 API |
| §11 错误可区分 | **采纳** | P3 的直接解；越早做越省，否则接口要改两遍 | §3 |
| §7 Mount 容器 | **采纳** | 唯一带来**新能力**的大件（多后端一棵树 + 优先级） | §10 |

## 3. 错误模型：`IoError` + `Result<T>`

```cpp
namespace vn::io {

/// @brief VFS / Stream 的可区分错误（需求文档 §11.1 一一对应）。
enum class IoError : std::uint8_t
{
    Ok,                // 成功（仅用于无返回值的表达）
    NotFound,          // 未找到
    AlreadyExists,     // 已存在
    PermissionDenied,  // 权限不足
    NotADirectory,     // 期望目录但实际是文件
    IsADirectory,      // 期望文件但实际是目录
    InvalidPath,       // 路径非法（前导 '/'、含 '\'、越界 '..'、嵌入 '\0'、盘符段）
    ReadOnly,          // 只读后端 / 只读挂载
    IoFailure,         // 底层 I/O 错误
    Unsupported,       // 该后端不支持此操作
    InvalidData,       // 数据格式错误（zip 损坏等）
    Closed,            // 操作已关闭的对象
    OutOfRange,        // 越界（seek 越界、偏移越界）
    CapacityExceeded,  // 超出容量上限
};

/// @brief 携带成功值或 `IoError` 的返回类型（C++20 没有 std::expected）。
template <typename T>
class Result
{
  public:
    Result(T value);            // 成功：隐式，`return info;` 即可
    Result(IoError error);      // 失败：隐式，`return IoError::NotFound;` 即可

    [[nodiscard]] bool    ok() const noexcept;          // 是否带值
    explicit              operator bool() const noexcept;
    [[nodiscard]] IoError error() const noexcept;       // ok() 时为 IoError::Ok
    [[nodiscard]] T&      value() noexcept;             // 前置条件：ok()
    [[nodiscard]] T take();                             // 前置条件：ok()，移出值

  private:
    std::optional<T> value_;
    IoError          error_{ IoError::Ok };
};
} // namespace vn::io
```

要点：

1. **14 个失败类别 + `Ok`**：需求文档 §11.1 的 13 个逐条对应，外加 §7.4 说明的 `NotEmpty`
   （`rmdir` 的"非空"是预期结果而非故障，不该借用 `IoFailure`）。`ioErrorName()` 把类别变成可读名字
   （`"NotFound"`），诊断与测试输出都用它。
2. **用构造函数而不是静态工厂**：`static Result ok(T)` 会和查询用的 `bool ok() const` 同名 ——
   静态成员与普通成员同名同签名的重载是不允许的，为避开这个坑直接用两个隐式构造函数。
   `Result<T>` **不做** `and_then` / `map` 等组合子：阶段 1 的调用点是"取错误码、判成功"。
3. `T` 需要可移动；`T = void` 的场景直接用 `IoError`（需求文档 §11.2 的"无返回值成功/失败"）。
4. 放在 `vn::io`：错误枚举是 IO 语义，`Result` 与它同域；**不**上提到 `vn::core`。
5. **值而非异常**：业务失败一律走返回值（需求文档 §11.3）。异常仍用于编程错误（`bad_alloc`、误用的 `invalid_argument`）。此约束**只约束 `vn::io`**，不回溯改造既有 `vn::crypto`（`HashCalculator` 现在抛 `logic_error` / `runtime_error`）。
6. **同名不同返回类型无法重载**（S1 实测结论，也是 S2.5 重设计的直接原因）：`writeFile` / `readFile` /
   `remove` / `save(vector&)` / `mountFile` 要改成返回 `IoError`，就不能与 `bool` 版共存；
   `list` 因为参数个数不同才能合法重载。结论：**不要让两套 API 长期并存** —— 见 §13。

## 4. 路径校验（不新增类型，但必须补校验）

现状（S1 前的实现）：

```cpp
// src/VfsInternal.hpp —— 只去首尾 '/'，"" == root；不校验任何非法输入
String detail::normalizeVfsPath(StringView path);

// src/DirectoryVfs.cpp:34 —— 子串拒 '..'（已防逃逸），但：
//   1) 拒绝与"未找到"不可区分（都表现为 false）
//   2) 未拒绝 'C:/…' 这类绝对段：fs::path::operator/ 会用右操作数替换左操作数 → 逃出 root_
std::filesystem::path DirectoryVfs::toReal(const String& vfs_path) const;
```

设计（S1 已实现）：校验收敛成**一个返回错误码**的函数，不做 `VfsPath` 值类型：

```cpp
namespace vn::io::detail {

/// @brief 规范化虚拟路径并校验。
/// @param path 调用方给的原始路径（"" 表示根）。
/// @param out 接收规范化结果（无首尾 '/'、无空段；根为 ""）；失败时不改动。
/// @return Ok，或 InvalidPath（下表任一规则不满足）。
IoError normalizeVfsPath(const std::filesystem::path& path, std::filesystem::path& out);
} // namespace vn::io::detail
```

| 规则 | S1 实现 |
|---|---|
| 空路径 | "" 合法，表示根 |
| 空段 / 重复分隔符 / 尾部 `/` | **折叠**（`a//b` == `a/b` == `a/b/`），不报错 |
| `.` 段 | **折叠**为当前目录（`./a` == `a`） |
| 前导 `/` | **拒绝** → `InvalidPath`：内部路径**无工作目录**，绝对拼写命不中任何条目（2026-09-23 前是折叠成相对路径） |
| `\` | 拒绝 → `InvalidPath`（防后端混淆） |
| 嵌入 `\0` | 拒绝 |
| `..` | **按段判断**：抵消上一段；抵消到根之外 → `InvalidPath`。（旧的子串判断会误杀 `a..b` 这类合法名，S1 已修正） |
| `:` 段 | 拒绝（Windows 盘符 / UNC / 类 URI 段）→ `InvalidPath`。这是 S1 真正堵掉的漏洞 |
| 非文本条目名 | **不拒收**：归档里的字节不是任何编码的文本时，按“合法 UTF-8 → 本机代码页 → 每个字节提为一个同值码点”解码（`detail::fromStoredName`），条目仍然列得出、读得回；归档里的原字节在重打包时原样写回（§9） |
| 后端落地 | 后端把规范化后的相对路径拼到自己的根上，并**再验证** `is_absolute()` / `has_root_name()`（双保险） |

实现要点：

1. `DirectoryVfs` 侧只有一个校验入口 `resolve(path, normalized, real)`，旧的 `toReal()` 变成它的薄壳
   （非法 → 空路径 → 既有的 `bool` 方法自然返回 `false`）。旧路径里那条**子串** `..` 判断已删除。
2. **`NotFound` 不能靠 `error_code` 判断**（S1 实测的跨平台坑）：MSVC 在路径不存在时**也会置位** `ec`，
   而标准只要求 `status()` 返回 `file_type::not_found`。因此 `stat` / `list` 一律**先看 `file_type`**
   （`not_found` → `NotFound`），再看 `ec`（→ `IoFailure`）。只看 `ec` 会把"不存在"误报成 I/O 错误。
3. **前导 `/` 拒绝（2026-09-23 收紧）**：原先 `"/a/b"` 折叠成 `"a/b"`，等于把调用方眼中的另一个位置
   静默解释成本后端根下的同名条目；虚路径没有工作目录，绝对拼写没有任何条目可指，因此直接 `InvalidPath`。
   重复分隔符 / `.` / 尾部 `/` 的折叠不变，后端落地的 `is_absolute()` 复验仍作双保险（`DirectoryVfs::resolve`）。

## 5. 文件信息：`VfsEntryInfo` + `stat` + `list`

```cpp
/// @brief 虚拟文件/目录的最小信息（需求文档 §6.6）。
struct VN_IOBASE_API VfsEntryInfo
{
    std::filesystem::path path;          // 完整规范化路径（空路径是根）
    bool          is_directory{ false };
    std::uint64_t size{ 0 };             // 目录恒为 0
    std::uint32_t crc{ 0 };              // 后端记录的内容校验和（ZIP = CRC-32）；没有记录或尚未写出为 0

    /// @brief 最后一个路径段，即条目自己的名字；根为空串。
    [[nodiscard]] std::filesystem::path name() const;
};

/// @brief 路径指向什么（`kindOf` 的返回值）。
enum class VfsEntryKind : std::uint8_t
{
    Missing,   // 不存在，或不是合法虚拟路径
    File,      // 文件
    Directory, // 目录（显式，或由更长的路径隐含）
};

// Vfs（纯虚）
virtual Result<VfsEntryInfo>              stat(const std::filesystem::path& path) const = 0;
virtual Result<std::vector<VfsEntryInfo>> list(const std::filesystem::path& dir) const = 0;
```

- **没有 out-parameter**：查询返回 `Result<T>`，调用点写成 `if (const auto info = vfs.stat(p)) { ... }`。
- **只有一个 `list`**：不再有"名字版"重载。`VfsEntryInfo::path` 是完整路径，需要名字时用 `name()`；
  这也去掉了"同名文件与目录"的歧义（之前两个 `list` 重载会触发派生类的名字隐藏）。
- **目录的 `size` 恒为 0**：zip / 内存后端没有"目录大小"概念，模拟 OS 值只会误导。
- **`crc` 是唯一的"后端记账"字段**：后端记录的内容校验和（ZIP = 中央目录里的 CRC-32）；没有记录、或条目还没写出去时为 0
  （它自己不可当强保证 —— `0` 也是合法 CRC，拿它做完整性判断要配合 `stat` 的结果看）；时间/权限等仍**不放进**
  `VfsEntryInfo`，将来加字段是加法（有默认值），不会破坏调用方。
- `stat` 失败：路径不存在 → `NotFound`；非法 → `InvalidPath`。`IsADirectory` / `NotADirectory`
  只在调用方用错了具体操作时出现，`stat` 本身两类都成功返回。
- `DirectoryVfs::stat` 对"存在但不是文件也不是目录"（设备、管道）返回 `NotFound`，与 `exists()` 口径一致。
- `list` 给出的是**完整路径**，可直接回喂 `stat` / `read`。
- **一次问出 kind**：非虚 `Vfs::kindOf(path)` 返回 `VfsEntryKind`，由 `stat()` 派生 —— 根恒为 `Directory`，
  非法路径与不存在都报 `Missing`（要区分就用 `stat()`）；后端不为此新增虚函数。
  `ZipArchive` 内部的条目表查询因此改名 `entryKindOf()`，不遮蔽基类这条便利。

## 6. 只读声明：`isReadOnly`

```cpp
/// @brief 后端是否只读。只读后端的写/改操作必须在动手之前返回 ReadOnly。
[[nodiscard]] virtual bool isReadOnly() const noexcept = 0;
```

| 后端 | `isReadOnly()` | 说明 |
|---|---|---|
| `DirectoryVfs` | `false` | 直接写真实目录 |
| `MemoryVfs`（新） | `false` | 内存树可写 |
| `ZipMemoryVfs` | `false` | 内存副本可写，`save` 时落盘（robotics 的 `savePkg(obj, pkg_path)` 依赖这一点，**不能改成只读**） |
| `ZipVfs`（新，惰性） | `true` | 只能读 zip 内容；要改就重写整个 zip，属于另一件事 |
| `MountVfs`（新） | 全部挂载只读时为 `true` | 单个挂载点的只读由挂载项声明 |

"动手之前"的落地：`addFile` 一族 / `writeText` / `createDirectory` / `createDirectories` /
`rename` / `remove` / `removeAll` / `open(Write|Append|ReadWrite)` 的第一行都先查 `isReadOnly()`
或挂载项的只读位，命中即返回 `ReadOnly`，**不触碰后端**（`ReadOnlyBackendRefusesEveryChange` 用例覆盖）。

## 7. 目录与文件操作（S2 + S2.5 已实现）

```cpp
// Vfs（纯虚），三个操作家族都是"单个"+"递归/整体"一对
virtual IoError createDirectory(const std::filesystem::path& path) = 0;    // mkdir：父目录必须已在
virtual IoError createDirectories(const std::filesystem::path& path) = 0;  // mkdir -p：已存在不算错
virtual IoError rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
virtual IoError remove(const std::filesystem::path& path) = 0;             // 文件或空目录
virtual IoError removeAll(const std::filesystem::path& path) = 0;          // 文件或整棵子树
```

**为什么把 `recursive` 布尔参数换成两个函数**：一是 `createDirectory(p, true)` 在调用点看不出意图，
二是布尔默认值要在基类与每个派生类重复声明（否则在具体类型上调用会找不到默认值），
三是这是 Java / `std::filesystem` 都用的分法（`createDirectory` / `createDirectories`，
`remove` / `remove_all`）。

**为什么 `remove` 改成 `std::filesystem::remove` 语义**：旧的 `remove` 是"删文件或整个子树"，
与 `fs::remove_all` 同义而与 `fs::remove` 不同名，容易让人误判。改成
"`remove` = 文件或空目录、`removeAll` = 子树"后，两条名字与 std 一一对应，
原来的 `removeEmpty` 也就不需要了（需确认：旧 `remove` 删子树的行为已消失，但仓库内无调用方）。

### 7.1 `createDirectory` / `createDirectories`

| 情况 | `createDirectory` | `createDirectories` |
|---|---|---|
| 路径非法 | `InvalidPath` | `InvalidPath` |
| 根 `""` | `AlreadyExists` | `Ok` |
| 已是目录（显式或隐式） | `AlreadyExists` | `Ok`（幂等） |
| 已被**文件**占名 | `AlreadyExists` | `AlreadyExists` |
| 父目录缺失 | `NotFound` | 逐级创建 |
| 祖先中有**文件**挡路（任意深度） | `NotADirectory` | `NotADirectory` |

`createDirectory` 的 `AlreadyExists` 正是需求文档 §17.5"创建已存在 → 已存在"那条；
`createDirectories` 则是幂等的 `mkdir -p`。

### 7.2 `rename`

| 情况 | 结果 |
|---|---|
| 任一路径非法 | `InvalidPath` |
| `from` 或 `to` 是根 | `InvalidPath`（根既不能移动，也不能作为目标） |
| `from == to`（规范化后） | `Ok`（无事可做，不是错误） |
| `to` 在 `from` 子树内 | `InvalidPath`（否则会把树搬进自己） |
| `from` 不存在 | `NotFound` |
| `to` 已被占（文件或目录，含隐式目录） | `AlreadyExists`（**任何平台都不隐式覆盖**） |
| `to` 的父目录缺失 | `NotFound` |
| `to` 的父是文件 | `NotADirectory` |

`DirectoryVfs` 那份靠 `to` 的存在性预检实现"不覆盖"（`fs::rename` 在 POSIX 上会静默替换，在 Windows 上会失败，
行为不一致，所以一律先检）。

### 7.3 `remove` / `removeAll`

| 情况 | `remove` | `removeAll` |
|---|---|---|
| 路径非法 / 根 | `InvalidPath` | `InvalidPath` |
| 不存在 | `NotFound` | `NotFound` |
| 文件 | 删除 | 删除 |
| 空目录 | 删除 | 删除 |
| 目录非空 | `NotEmpty` | 删整棵子树 |

### 7.4 新增错误类 `NotEmpty`（14 个失败类）

需求文档 §11.1 的 13 个里**没有**"目录非空"，但 `rmdir` 语义下这是**预期结果而非故障**。
若借用 `IoFailure` 表达，又把"正常拒绝"和"磁盘坏了"混在一起 —— 正是 P3 要治的病。
因此 S2 在 `IoError` 里加了 `NotEmpty`（`IoError.hpp` 已同步 `ioErrorName`）。

### 7.5 内存后端怎么表示目录

`ZipMemoryVfs` 原先只认隐式目录（由条目路径前缀推出），因此空目录**无法存在**，
`createDirectory` 也就无从观察。S2 给 `Entry` 加了 `is_directory` 标记：

- 目录 = 显式标记（`createDirectory` 产生，或从 ZIP 的 `name/` 条目读入）**或**隐式父目录；
  私有判定收敛在一个 `isDirectoryPath(normalized)` 里，`stat` / `isDirectory` / 写操作守卫都调它。
- `addFile` 一族碰到已有目录（含隐式）一律**拒绝**（`IsADirectory`），
  否则树里会同时冒出"同名文件"和"同名目录"；反之，`createDirectory` / `createDirectories`
  碰到已有文件返回 `AlreadyExists`，祖先里有文件返回 `NotADirectory`。
- `saveAs` 时显式目录写入 ZIP 目录条目 → 空目录能过 `saveAs` / `open(…, ReadOnly)` 往返；
  `open(…, ReadOnly)` 侧也不再丢弃 `name/` 条目，而是还原成目录标记。
- 依赖也是懒的：`addFile(path, real_path)` 只记下真实路径并先确认它可读，真正读盘发生在 `saveAs` / `toBytes`。
- 为此给把两种目录写开：单个目录条目（空目录落盘）由私有的 `insertDirectory(name)` 插入，
  对外的 `addDirectory(prefix, dir)` 是"**递归导入真实目录**"，两者不是同一件事。

## 8. 打开与流：读流 / 写回调

不引入 `IStream` 继承体系（§2 已述），按“大块数据从哪里来、到哪里去”只给两种形态。

**住哪里**：四个类型（`Fragment` / `DataSource` / `DataSink` / `VfsReadStream`）全在 `sdk/vine/io/Stream.hpp`，
它们是 IOBase 级的**中性词汇**（同 `IoError.hpp`），不是“VFS 专属”：存储层 `ZipArchive` 直接用它拉源头、推送 sink，
VFS 层的 `openRead` / `read(path, sink)` 落地后也用它。因此**不拆两个头**——`ZipArchive::openRead()` 本身就返回
`VfsReadStream`，拆完存储层依旧要包含它，拆分只增噪音。（旧名 `VfsStream.hpp` 已改：名字暗示“VFS 的流”，
而当时唯一的包含者就是存储层。）

### 8.1 读：两种形态（pull 流 / push 回调）

```cpp
/// @brief 一次一个条目的顺序读；能力探测见 seekable()。
class VN_IOBASE_API VfsReadStream
{
  public:
    virtual ~VfsReadStream() = default;
    /// @brief Reads up to out.size() bytes; 0 means the end.
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> out) = 0;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual bool seekable() const noexcept = 0;
    [[nodiscard]] virtual IoError seek(std::uint64_t offset) = 0;
};

[[nodiscard]] virtual Result<std::unique_ptr<VfsReadStream>> openRead(const std::filesystem::path& path) const = 0;

/// @brief Push 变体：后端把数据一块块推给 sink，调用方不拿整条。
[[nodiscard]] virtual IoError read(const std::filesystem::path& path, DataSink& sink) const = 0;
```

- **pull 流**：调用方控节奏（解析器、拷进 GPU 缓冲、分块处理）；
- **push 回调**：只消费一次（算哈希、直接喂下游 API）；
- 两者都**不把整条读进内存**：libzip 侧就是 `zip_fopen_index` + 循环 `zip_fread`（`zip_file_t` 是独立对象，可同时开多个）；
- `seekable()` 有真实含义：对应 `zip_file_is_seekable()` —— **STORED 条目可 seek**，压缩条目上 seek 等于“解压到该位置”（慢）。这正好兑现需求文档 §5.2 的能力探测。
**（已落地 2026-09-25）**：`Vfs::openRead` 是基类的读侧原语（纯虚）；`Vfs::read(path, DataSink&)` 是它的 push 变体（虚 + 默认实现，
基类只写一遍：按 64 KiB 分块把 `openRead` 的字节推给 sink，sink 拒绝的那一块原样返回错误、条目末尾的 `stream->error()`
当作本次调用的结果）。`ZipArchive` 覆写 `openRead`（复用它的 `EntryReadStream`，本就是流式的）；`DirectoryVfs` 覆写为自有
文件流（`FileReadStream`：ifstream + 开档时捕获的尺寸；`seekable() == true`、越界 seek 答 `OutOfRange`）。读流可以活过 VFS
（两个实现都自持句柄）。两个派生类各有一条 `using Vfs::read;`——覆写单参 `read` 会遮住基类的 push 重载。门禁见 §14。
### 8.2 写：只有“回调/来源”这一种零拷贝形态

```cpp
/// @brief 数据来源（pull）：保存时由 VFS/libzip 来拉取。用于“边算边写”，零拷贝。
class VN_IOBASE_API DataSource
{
  public:
    virtual ~DataSource() = default;
    [[nodiscard]] virtual std::uint64_t size() const = 0;                 ///< 必须准确；与实际读出的字节数不一致 → InvalidData
    [[nodiscard]] virtual std::size_t read(std::span<std::byte> out) = 0; ///< 0 表示 EOF
};
```

| API | 数据在哪 | 拷贝 | 适用 |
|---|---|---|---|
| `addFile(path, std::span<const unsigned char>)` | 调用方缓冲 → 条目表 | **1 次** | 小数据（XML、几百 KB 的 bin） |
| `addFile(path, std::span<const Fragment>)` | 多个不连续的块，**无需拼接** | 0 | 数据本来就分散（positions / normals / indices 三段） |
| `addFile(path, std::shared_ptr<DataSource>)` | 调用方对象，**save 时才被拉取** | 0 | 生成式大块（边算边写） |
| `addFile(path, real_path)` | 磁盘文件，**save 时才读** | 0（内存） | 已有文件资源（大 mesh） |
| （备选）`addFile(path, std::vector<unsigned char>&&)` | 移动进条目表 | 0 | 已有就绪缓冲且要零拷贝 |

四个重载都挂在 `Vfs` 上：前两个纯虚，片段 / 来源两个虚 + 默认实现（拼装 / 拉取后委托给 `addFile(path, span)`），
`ZipArchive` 覆写它们保住借用与惰性 —— 调用方只持 `Vfs&` 也能用全家族。

- **“1 GB 数据必须先拼成一个连续缓冲”这个前提不成立**：libzip 原生支持**分散来源** ——
  `zip_source_buffer_fragment_create(fragments, n, freep, &err)`（`zip_buffer_fragment_t{ data, size }`，纯借用，
  同样必须活到 `zip_close`）。所以：数据本来就分散（**scatter**）→ 直接交片段列表；
  数据是现算的（**gather**）→ 用 `DataSource` 回调。两条路都不需要连续化、也不需要中间副本。

- **不做 `openWrite()` + 反复 `write(chunk)` 的 push 形态**：libzip 的 source 模型是 **pull**
  （`ZIP_SOURCE_STAT` / `READ` / `CLOSE`，由 libzip 在 `zip_close` 时按需调用），push 式写出只有两条下场 ——
  把数据缓冲起来（= 回到整条驻留），或 spool 到临时文件（多一次磁盘往返）。所以“生成式大块”用 `DataSource`。
- `DataSource` 由 VFS **持有**（`shared_ptr`），生命周期覆盖 `saveAs` / `commit`；回调在**执行保存的那个线程**被调用（§12 要写明）。
- `size()` 是否允许“未知长度”需实测（libzip 写侧是否要求预先知道长度）；不行就只能 spool 到临时文件。


### 8.3 零拷贝与峰值内存（为什么必须有流/回调）

| 场景 | 整条 API | 流式 / 回调 API |
|---|---|---|
| 读一条 1 GB mesh，只为算哈希 | `read()` ⇒ 1 GB 驻留 | `read(path, sink)` ⇒ 只一个块缓冲 |
| 生成一条 1 GB mesh 写进包 | `addFile(span)` ⇒ VFS 副本 + 调用方那份 = **2×** | `addFile(path, source)` ⇒ 0 驻留 |
| 把整包交给别人 | `toBytes()` ⇒ **2× 整包**（§9.7） | `saveAs(path)` ⇒ 单条目 |

结论：**大块数据一律走 `openRead` / `read(sink)` / `addFile(path, source)` / `addFile(path, real_path)`**；
`read()` / `addFile(path, span)` / `toBytes()` 只用于中小数据（它们的存在价值是“简单”，不是“省内存”）。

### 8.4 生命周期与已知取舍

- **读流可以活过 VFS**：`ZipVfs` 把 libzip 句柄放进 `shared_ptr`，`VfsReadStream` 持一份（满足 §14 的
  “VFS 析构后 Stream 仍可读”门禁）；流不暴露任何后端句柄（需求文档 §5.3）。代价：源文件被删/被替换时读会失败，文档要写明。
- **push 写不做**（理由见 §8.2）：zip 成员必须是“大小已知的连续压缩数据”，生产者只能被拉取。
- **`Append` 在 zip 上没有意义**：zip 条目不能追加（改一个条目 = 重写序列）；`open(path, Append)`
  只对 `DirectoryVfs`（真实文件）有意义。
- 写失败上报：`commit` / `saveAs` 是唯一的“落介质”点，错误在那一层返回；`write` 的错误是内存错误（立即、真实）。
- 跨进程 / 跨线程可见性：见 §12。

### 8.5 领域数据怎么接（接口不派生，用适配器）

`DataSource` / `DataSink` 只表达“字节流 + 精确长度”，**不做领域派生**：mesh / image / BRep 的“特殊结构”
由调用方或领域模块**实现**这两个接口（例如 `RoboticsIO` 里的 `MeshDataSource` 把 positions / normals / indices 边拉边拼；
交错顶点、量化解码、GPU 映射内存都同理）。理由：模块图是 `vn::IOBase ← vn::RoboticsIO`，IOBase 不得依赖领域类型；
反过来给 IOBase 加 `MeshSource` 会让它退化成“已知数据形态枚举表”，每来一种数据都要改 IOBase。

通用场景由**工厂函数**覆盖（不新增类型；实现类藏在 .cpp 里）：

| 工厂 | 语义 | 拷贝 |
|---|---|---|
| `bufferSource(span<const unsigned char>)` | 连续借用 | 0 |
| `fragmentSource(span<const Fragment>)` | 分散借用（scatter） | 0 |
| `fileSource(path, offset, size)` | 磁盘文件，save 时才读 | 0（内存） |
| `vectorSource(vector&&)` | 移动接管 | 0 |
| `vectorSink(vector&)` / `ostreamSink(ostream&)` | 整条读的实现 / 流到外部目标 | — |

于是 `addFile(path, span)` / `addFile(path, fragments)` / `addFile(path, real)` 都只是“包一个 source 交给同一条 emit 路径”，
不出现第二套写出逻辑。

**实现者必须遵守的契约**：

1. `size()` 必须精确 —— 与实际产出不一致 → `InvalidData`；
2. `read()` 允许少于请求，`0` 表示 EOF（**不得**用 0 表示“暂时无数据”）；
3. 借用式字节（`Fragment` / `bufferSource`）必须活到 `saveAs` / `commit` 结束（§9.6）；
4. **一个 source 实例只挂一个条目**：libzip 的 `zip_source_t` 是每条目一个，同一份数据被两个条目引用时各自造实例；
   回调只在**执行保存的那个线程**被调用（§12）；
5. `DataSink::write` 返回 `IoError`（`Ok` = 继续，其它 = 停止并由调用方原样上报），
   而 `DataSource::read` 返回字节数（要报告进度）—— 这处不对称是有意的。

## 9. 后端家族

三条硬约束把可选空间压到只剩一个答案：

| # | 约束 | 后果 |
|---|---|---|
| **C1** | 打开 500 MB 包不得全量驻留内存（痛点 P5） | 条目内容必须**惰性**取；而“惰性”意味着 VFS 得知道“内容在哪个归档的哪个条目里” → **VFS 必须有介质意识**，纯内存树做不到 |
| **C2** | 改完要能反向读 / 列表 / 移除，而且落地不留半成品 | 改动与原始介质必须**分离**（overlay），落地走“写临时文件 + 原子替换” |
| **C3** | 概念要少（“不要三个类型”） | “介质”只留一条轴，这条轴上各一个成员 |

C1 与“把持久化完全外置”（`MemoryVfs` 只管内存、zip 交给外部序列化）是**互斥**的：
外置意味着“打开 zip”= 把条目全部解压进内存，正好回到 P5。所以 **`MemoryVfs` 这个类型不建** ——
它能做的事已经被 `ZipVfs()`（无源归档 = 只有改动区的空树）+ `toBytes()` 覆盖，而它做不到惰性。

```
Vfs（抽象接口，原 IMemoryVfs）      // 路径/树/权限/错误模型；不假设介质
 ├── DirectoryVfs   介质 = 真实目录；写即落盘，commit() 无事可做 → Ok
 ├── ZipVfs         介质 = ZIP：惰性索引 + 改动 overlay + commit/saveAs 回 zip
 └── MountVfs       待做：多后端一棵树（§10）

Zip                 一次性操作层：compress / decompress / compressDirectory / decompressFile / entries / readEntry
ZipArchive          打开态的 ZIP 树（Vfs 后端）：open（ReadOnly | ReadWrite）+ read / openRead / addFile×4 / saveAs / commit / toBytes
```

### 9.1 一个 `ZipVfs` 的三个开口

| 入口 | 绑定 | `isReadOnly()` | 语义 |
|---|---|---|---|
| `open(path \| bytes, OpenMode::ReadOnly)` | 只读源归档 | `true` | 只读视图；写操作立即 `ReadOnly`（需求文档 §6.5） |
| `open(path \| bytes, OpenMode::ReadWrite)` | 只读源 + 可写 overlay；文件目标 = 同一路径（供 `commit()`） | `false` | 读 + 改；`commit()` 原子替换回原路径（`bytes` 版无文件目标 → `Unsupported`） |
| `ZipVfs()` | 无源、无目标 | `false` | 纯内存树；`commit()` → `Unsupported`，落地走 `saveAs` / `toBytes` |

三个开口是**同一个类型**的三种状态，不是三个类型：VFS 的操作集合是同一套，“能不能写”是**状态**而不是**接口**。
正确的类比是 `std::fstream` + `ios::in|out`（一个类 + 模式），不是 `istream`/`ostream`/`iostream`
（后者出于运算符方向 + 虚继承的历史包袱）。`isReadOnly()` 本来就是为这件事准备的（§6）。
模式是显式实参（`OpenMode`，**无默认值**）：加载方要只读就在调用点写出来，不会因为少写一个实参而悄悄拿到可写视图。
内存字节同样两个入口：`open(vector&&, …)` 接管（move 进来）、`open(span, …)` 借用（不持有；span 必须活到 archive 及其所有读者结束）。

### 9.2 一个 `ZipVfs` 的两个句柄（生命周期不同）

| 句柄 | 生命周期 | 干什么 |
|---|---|---|
| 源 `zip_t`（`ZIP_RDONLY`） | **VFS 全程** | 打开时一次读全目录（名字/大小/类型）；`read()` 按需 `zip_fopen_index` / `zip_fread`；`saveAs` / `commit` 时被 `zip_source_zip` 透传引用 |
| 目标 `zip_t` | **仅一次 `commit` / `saveAs`** | 逐条目 emit，`zip_close` 收尾 |

为什么不是“一个可写句柄当目录树”（即 `zip_open(path, 0)` → 改 → `zip_close` 原地写回）：

- 目标被构造时绑定 → `saveAs(别的目标)` 不可能，与“目标由 `saveAs` 参数决定”冲突；
- 要能改就得以可写方式打开 → **只读介质 / 只读包打不开**；
- libzip 的“立即写入”并非立即落盘（zip 不能只追加条目，改动先进句柄的内存改动表，
  磁盘写入集中在 `zip_close`），且 `zip_close` 对文件源是**原地重写** —— 中途失败就是半成品包；
- 源句柄只读，保证“本次操作永不改写源”。

### 9.3 持久化接口：`commit` / `saveAs` / `toBytes`

基类**不提供** `save(path)`：它把“写回自己的位置”与“另存到别处”混在一个名字里，而这两件事的目标来源不同。

| 方法 | 语义 | `DirectoryVfs` | `ZipVfs`（有源） | `ZipVfs`（无源） |
|---|---|---|---|---|
| `commit()` | 写回构造时绑定的目标 | `Ok`（本已落盘） | 临时文件 + 原子替换 + 重建索引 | `Unsupported` |
| `saveAs(path)` / `saveAs(ostream&)` | 写到指定目标 | `Unsupported` | 支持 | 支持 |
| `toBytes()` | 字节（原 `serialize`） | `Unsupported` | 支持 | 支持 |

- `commit()` 的顺序必须是：emit 到**临时文件** → 关源句柄 → `fs::rename` 原子替换 → 重新打开并重建索引
  （Windows 上 rename 覆盖一个自己开着的文件会失败）。
- **libzip 依据（`libzip-src/lib/zip.h`）**：可写目标必须可 seek ——
  `ZIP_SOURCE_SUPPORTS_WRITABLE = SUPPORTS_SEEKABLE | BEGIN_WRITE | COMMIT_WRITE | ROLLBACK_WRITE | WRITE | SEEK_WRITE | TELL_WRITE`，
  而 `SUPPORTS_SEEKABLE` 含 `SEEK` / `TELL`。所以不可 seek 的 ostream（管道 / socket）当不了 target，
  `saveAs(ostream&)` 只能“先拼字节再吐出”；真流式的目标只有**文件**。
- **“添加即落盘”不存在**：`zip_file_add` / `zip_dir_add` / `zip_delete` 只改句柄的内存改动表，
  写入集中在 `zip_close`（中央目录在末尾、要回填 local header）。所以接口上写操作**只改内存**，
  “落介质”永远只有 `commit` / `saveAs` 这一步。
- **输入侧是流式的**：条目以 `zip_source_t` 提供（`zip_source_file_create` 惰性读盘、
  `zip_source_zip_file(target, source, idx, ZIP_FL_UNCHANGED, 0, -1, nullptr)` 透传压缩数据、
  `zip_source_buffer` 内存字节），`zip_close` 逐条目写出 ⇒ 峰值内存 = 单条目。
  libzip 内部重压缩路径用的正是同一个 `zip_source_zip_file_create(za, i, ZIP_FL_UNCHANGED, 0, -1, NULL, &error)`（`zip_close.c`）。
- **I/O 与内存是两件事**：写**新**目标时未改动条目也要拷一遍（压缩数据直拷、不解压不重压缩）⇒ I/O ≈ 整包；
  libzip 的 `BEGIN_WRITE_CLONING` 优化（保留原文件前缀、跳过“已隐式拷贝”的条目）只在**写回同一文件源**时生效 —— 见 §15.6（已评估，暂不采纳）。
- 失败路径用 `zip_discard`（`zip_close` 失败后句柄仍存活）。透传是否保留 mtime / external attributes **需实测**，
  必要时用 `zip_file_set_mtime` / `zip_file_set_external_attributes` 补。
- `open(…, ReadOnly)` 下的 `commit()` 与所有写操作一样返回 `ReadOnly`。

### 9.4 惰性与透传

- 打开只读目录，不解压任何条目。证明方式（替换旧的“删源文件后读失败”用例）：
  破坏条目数据区、保留文件末尾的中央目录 → `open(…, ReadOnly)` / `list` / `stat` 成功，`read` 失败。
- 保存时：未改动条目由 `zip_source_zip_file(...)` 逐条目搬运 **内存 O(单条目)**；`addFile(path, real_path)` 的条目 `zip_source_file_create`（save 时才读盘）；
  overlay 字节走 `zip_source_buffer`。峰值内存 = 单条目。
- **实测修正（S3b）**：“搬运 = 不解压不重压缩”**不成立**：libzip 在把条目写进新归档时会读它并校验内容 ——
  把源包某条目的数据区破坏后，`saveAs` 会在 `zip_close` 报 `CRC error`（而不是静默产出坏包）。
  所以代价是 **CPU 要过一遍**（不驻留内存），真正“零解压零重压缩”的只有**就地更新（cloning）**那条路（§15.6）。
  副产品：损坏条目会**挡住**保存 —— 语义上比产出坏包好。
- **CRC 校验（两条读路径都已修）**：libzip 的**读路径不校验 CRC**（实测：损坏条目的 `read()` 会成功并给出错误字节）。
  现在 `readEntries` 一并取 `ZIP_STAT_CRC`（S3b 收口后它是 `.cpp` 内部的 `StoredEntry`）；`ZipArchive::read()` 读完自己算 `crc32()` 对照目录记录的值，不符 → `IoFailure`；
  `VfsReadStream` 同样在流内累计 crc，**读到末尾时**对照并把结果放进 `error()`（`seek(0)` 重置校验，其它 seek 关闭校验，
  因为增量校验只对顺序读成立）。两条路径都由 `IoBaseTest.ZipArchiveOpensWithoutReadingContent` 钉住。
- **收益已兑现在 `robotics::io`**：`loadPkg(path)` 与嵌套 `.vdevpkg` 用 `open(…, OpenMode::ReadOnly)`，只解压真正要用到的条目。

### 9.5 命名与一次断代

- `IMemoryVfs` → **`IVfs`**：`DirectoryVfs` / `ZipVfs` 都不是“memory”，旧名误导；`I` 前缀与 `INamed` / `IHierarchyNode` 一致。
- `save(path)` / `save(ostream&)` / `serialize()` → `saveAs(path)` / `saveAs(ostream&)` / `toBytes()`；新增 `commit()`。
- `ZipMemoryVfs`（头文件与实现）删除；`ZipVfs` 由“只读惰性”变为“惰性源 + overlay”的唯一 zip 后端。
- 迁移面：`robotics::io`（`loadPkg` → `open(…, ReadOnly)`，`savePkg(obj, path)` → 无源树 + `saveAs`）、
  `tests/test_iobase`（`ZipMemoryVfs` → `ZipVfs`）、`tests/test_robotics_io`。

### 9.6 数据所有权与生命周期

libzip 的 `zip_source_buffer(za, ptr, len, 0)` 是**借用**（`freep=0` 不拷贝），而写入发生在 `zip_close` ——
所以任何交给 libzip 的 buffer **必须活到 close**，否则读到已释放内存（`ZipArchive::buildZip` 里那个
活到循环外的 `file_backed` 就是为这个坑加的）。新设计的对策是**让每一类条目都由一个天然活到 close 的持有者提供**：

| 条目来源 | 谁持有 | 活到什么时候 | 拷贝 |
|---|---|---|---|
| `addFile(path, span)` | VFS 条目表的 `data` | 到 `saveAs` / `commit` 结束 | **一次**（接口语义：当场取走一份，调用方可立刻释放自己的缓冲） |
| `addFile(path, fragments)` | 不持有，只借用（`ZipArchive`；基类默认实现当场拷一份） | 到 `saveAs` / `commit` 结束 | 0 或 1 次 |
| `addFile(path, source)` | VFS 条目表持 `shared_ptr` | 到 `saveAs` / `commit` 结束 | 0（save 时才拉取） |
| 源归档条目 | `ZipArchive` 的源句柄 | VFS 全程 | 不拷（`zip_source_zip_file` 透传压缩数据） |
| `addFile(path, real)` | 不持有，只记路径 | —（`saveAs`/`commit` 时才读盘） | 不拷；**save 那一刻文件的内容**才是结果，文档要写明 |
| `emit()` 期间 | 不需要任何临时缓冲 | — | 三种来源分别是成员数据 / `zip_source_file_create`（libzip 自己读）/ 源句柄透传 |

- 因此 `buildZip` 那种“把文件读进内存再喂 buffer、并保证它活到 close”的 fragile 模式在新设计里消失。
- 想省掉 `addFile(path, span)` 的那次拷贝，只有两条路：①加虚函数重载 `addFile(path, std::vector<unsigned char>&&)`
  （零拷贝，但虚函数再多一条，每个后端两条写路径）；②让 `ZipArchive` 用 `zip_source_function_create`
  + 回调持有 `shared_ptr`（正式的所有权移交，但**不减少驻留**，只适合内部自用）。
  ①**等出现真实的大块内存写入场景再加**（现状大 mesh 走 `addFile(path, real_path)`，本来就不驻留）。

### 9.7 规模评估：2 GB 包

设包 2 GB（压后）、10k 条目、本次只改一个 XML：

| 操作 | 峰值内存 | I/O | 结论 |
|---|---|---|---|
| `open(path, ReadOnly)` | 目录索引 ~1 MB | 只读中央目录 | ✅ 与包大小无关（惰性的目的） |
| `read(一条)` | 该条目解压后大小 | 读该条目 | ✅ 峰值 = 单条目；⚠️ 单条 >100 MB 的 mesh 仍整条进内存，要等 §8 的流式读 |
| `addFile` / `rename` / `remove` | O(改动) | 0 | ✅ 只改内存 |
| `commit()` / `saveAs(path)` | O(单条目) | **≈ 2 GB 读 + 2 GB 写** | 未改动条目压缩数据直拷 ⇒ CPU 几乎为 0、纯 I/O；本地 SSD 约 1.5–8 s |
| `saveAs(ostream&)` | **≈ 4 GB** | 2 GB 写 | ❌ 该规模不可用 |
| `toBytes()` | **≈ 4 GB** | — | ❌ 该规模不可用 |
| 就地更新（§15.6） | O(单条目) | ≈ 改动 + 目录 | 仅“频繁小改动”时值得（非原子） |

- **大包禁用 `toBytes()` / `saveAs(ostream&)`**（它们的语义是“把包交给别人”，代价是整包在内存里）；2 GB 走 `saveAs(path)` / `commit()`。
  `toBytes()` 的 2× 峰值可以消除（自写 `zip_source_function` 直接写进返回 buffer），但那要新增一个回调 source，**等有真实消费者再做**。
- **`updateInPlace()`（若 2 GB + 频繁保存成为真实工作流）**：新增一个**语义独立**的方法而不是给 `commit()` 加模式开关 ——
  名字即契约（非原子、就地、近增量），可配 hardlink 备份兜底；原子性不该藏在同一个名字后面。
- 工作流层最省的替代：**大包用 `DirectoryVfs`**（解包成目录 → 直接改文件 → 需要时再打包），改动只写磁盘上那几个文件；
  代价是交付形态从单文件包变成目录树。

## 10. `MountVfs`：多后端一棵树

```cpp
class VN_IOBASE_API MountVfs : public Vfs
{
  public:
    /// @brief 挂载一个后端（前缀 + 优先级 + 只读）。
    IoError mount(const std::filesystem::path& prefix, std::shared_ptr<Vfs> backend, int priority = 0, bool read_only = false);

    // Vfs 全部实现；自身可被挂载（需求文档 §7.7 的嵌套）
};
```

规则：

1. **最长前缀优先**：挂载点集按前缀长度降序排列；挂载点数量在 O(10) 量级，线性扫描即可，路由结果可缓存。
2. **读**：同一前缀下按优先级降序，命中即返回。
3. **写**：见 §15 待裁决（需求文档 §7.3 的"第一个可写后端"会让读到的与写入的不是同一个）。
4. **枚举合并**：合并各后端（去重、同名高优先级胜），并且**必须补出中间目录** —— 例如挂了
   `/data/a` 与 `/data/b`，列 `/data` 要看到 `a`、`b` 两个目录。这是最容易漏的一条。
5. **跨后端 `rename`**：同后端走 native `rename`；跨后端 = 读源 → 写目标 → 删源，失败保留源并返回错误。
6. 所有 `IMemoryVfs` 方法在 `prefix` 命中为单后端时**直接转发**，不做多余复制。

## 11. 内存流：`reserve` + 容量上限

```cpp
// MemoryStreamBuf / ChunkedMemoryStreamBuf（新）
bool reserve(std::size_t bytes) noexcept;              // 只改容量，不改 size()/内容；超上限返回 false
void setCapacityLimit(std::size_t max_bytes) noexcept; // 0 = 无限（默认）
[[nodiscard]] std::size_t capacityLimit() const noexcept;
```

- `reserve` 可能重分配 → 必须沿现有 `setPointers()` 重建 get/put 区并**保留读写偏移**（`MemoryStreamBuf::grow`
  已经是这个模式）。
- **达到上限时写入沿现有失败路径**：`overflow()` 返回 `eof`、`xsputn()` 短写 → `badbit`。STL 适配层
  "只能表达失败"（需求文档 §5.7 原文），而**可区分的 `CapacityExceeded` 落在高层**
  （`open` / `write` / 后端写入前预检），这正是 §2 里"推迟核心 `IStream`"的代价与分工。
- 上限对 `seekp` 生效（不可 seek 到超过上限的位置）、对 `reserve` 生效（超出即 `false`）。

## 12. 并发级别（需求文档 §12）

- 各 VFS / Stream 类文档**必须声明级别**。阶段 1 统一声明：**方法级不线程安全**，调用方外部同步；
  不同流之间可并发（各自持有自己的 `Buffer` / 文件句柄）。
- 这几天落地的内存流类型（`MemoryStreamBuf` / `ChunkedMemoryStreamBuf` / `SpanStreamBuf` 及其包装）
  同样声明"非线程安全"，保持全 IOBase 一致。

## 13. 迁移策略：一次断代（S2.5 已执行）

**决策**：不做"新旧两套 API 并存"，而是**一次性换成一套**。理由：

1. **重载做不到**：`writeFile` / `readFile` / `remove` / `save(vector&)` / `mountFile` 改成返回 `IoError`
   就是"同名同参数、只有返回类型不同"，C++ 不允许重载 —— 想并存就得给一半操作起临时名，
   而那半套临时名会活到最后一个调用方迁完，中间没有任何收益。
2. **并存期才是真正的危险期**：两套语义（`bool` 吞错 vs `IoError` 可区分）同时存在，
   新代码很容易顺手用旧的，错误又静默了。
3. **代价已经被实测过**：全仓库只需改 3 个 robotics 源文件（`readFile` / `writeFile` ×2 / `save` 共十几处）
   与两个测试文件，没有任何意料之外的使用者。

**重设计的四条规则**（新代码必须遵守，也是审查要点）：

1. **一个操作一个名字**，没有 `xxx` 与 `xxxFile` 这种孪生。
2. **零 out-parameter**：需要返回值就返回 `Result<T>`（`stat` / `list` / `read` / `readText` / `serialize`），
   不需要就返回 `IoError`。
3. **派生便利函数写在基类**：`exists` / `isFile` / `isDirectory` / `readText` / `writeText` 都是
   基于虚函数的**非虚**实现（`IMemoryVfs.cpp`），后端只需实现 15 个原始操作，不必重复写便利逻辑。
4. **命名对照**（旧 → 新）：

| 旧 (`bool`) | 新 |
|---|---|
| `readFile(path, out)` | `Result<std::vector<unsigned char>> read(path) const` |
| `writeFile(path, data/size)` `(path, vector)` `(path, text)` | `IoError addFile(path, span<const unsigned char>)` / `writeText(path, String)` |
| `mountFile(path, real)` | `IoError addFile(path, real_path)`（"mount" 一词留给 `MountVfs`，避免语义撞车） |
| `remove(path)`（删子树） | `remove(path)`（文件/空目录） + `removeAll(path)`（子树） |
| `save(vector<unsigned char>&)` | `Result<std::vector<unsigned char>> serialize() const` |
| `save(path)` / `save(ostream&)` | 同名，改为返回 `IoError` 且 **`const`**（序列化不改树） |
| `exists` / `isFile` / `isDirectory`（纯虚） | 同名，改为**非虚**派生实现 |
| `list(dir)`（名字） + `list(dir, out)` | 单个 `Result<std::vector<VfsEntryInfo>> list(dir) const` |
| `createDirectory(path, recursive)` | `createDirectory` / `createDirectories` |
| `removeEmpty(path)` | 并入 `remove`（std 语义） |
| `stat(path, out)` | `Result<VfsEntryInfo> stat(path) const` |
| — | `writeText` / `readText`（文本便利，UTF-8） |

虚函数从 20 个（含 7 个纯虚 `bool`）变成 **15 个纯虚** + `addFile` 的片段 / 拉取来源两个带默认实现的虚函数，全部走同一个错误通道。

**阶段划分**（每阶段结束都必须全绿）：

| 阶段 | 内容 | 状态 |
|---|---|---|
| S1 | §3 `IoError`/`Result` + §4 路径校验 + §5 `stat`/`list` + §6 `isReadOnly` | **已完成** |
| S2 | §7 目录/删除操作 + 内存后端的显式目录标记 | **已完成** |
| S2.5 | **API 重设计**：零 out-parameter、一操作一名、派生便利函数，删旧 `bool` 家族并迁移调用方 | **已完成**（`test_iobase` 33/33） |
| S3a | §9 `ZipArchive::entries` + 惰性只读 `ZipVfs`（+ robotics 改用它） | **已完成**（`test_iobase` 39/39） |
| S3b | §9 单一 zip 后端（惰性源 + overlay + `commit`/`saveAs`/`toBytes`）、`ZipMemoryVfs` 删除、`IMemoryVfs` → `Vfs` | **已完成**（对账 2026-09-25：类名最终为 `ZipArchive`——空状态下它就是可写内存树；见顶部 banner 与 §9） |
| S4 | §10 `MountVfs` | 待做 |
| S5 | §11 `reserve`/上限（内存流侧） | 与 VFS 解耦，可并行 |
| S6 | §12 并发级别文档 + 跨进程/跨模块复查 | 待做 |

## 14. 测试门禁（对照需求文档 §17.5）

| 需求文档验收项 | 落点 |
|---|---|
| 空路径 / 相对路径 / 越界 `..` → 路径非法 | **已过**：`VfsCoreTest.InvalidPathsAreRejected` |
| 一次问出 kind（文件 / 目录 / 缺失） | **已过**：`VfsCoreTest.KindOfAnswersFileDirectoryAndMissing`（两个后端；非法路径 → `Missing`） |
| 前导 `/`（绝对拼写）→ 路径非法 | **已过**：`VfsCoreTest.InvalidPathsAreRejected` / `DirectoryVfsNeverEscapesItsRoot` |
| 重复分隔符 / 折叠 → 规范化正确 | **已过**：同上（`a/../a.txt`、`./a.txt`、`a..b`） |
| 根路径操作 → 正确 | **已过**：`stat("")` 为目录（两个后端） |
| 不存在的文件 / 目录 → 未找到 | **已过**：`stat`/`list` 的 `NotFound` |
| 创建已存在 → 已存在 | **已过**：`ZipCreateDirectoryIsStrict` / `DirectoryCreateRenameRemove` |
| `mkdir -p` 幂等 | **已过**：`ZipCreateDirectoriesMakesTheWholeChain` |
| 读取目录作为文件 / 枚举文件作为目录 | **部分已过**：`read(目录)` → `IsADirectory`；`list(文件)` → `NotADirectory`；`open` 待 S3 |
| 重命名到已存在 / 到自己的子树 / 到缺失父目录 | **已过**：`ZipRenameRejectsBadTargets` / `DirectoryCreateRenameRemove` |
| 删文件 / 删空目录 / 删非空目录 / 删子树 | **已过**：`ZipRemoveAndRemoveAll` / `DirectoryCreateRenameRemove` |
| 空目录能过 `save`/`ZipVfs` 往返 | **已过**：`ZipDirectoryEntriesSurviveSaveAndOpen` |
| 只读后端写操作立即失败 | **已过**：`ReadOnlyBackendRefusesEveryChange`（探针子类覆写 `isReadOnly()`） |
| 不支持的能力报 `Unsupported` 而非静默 `false` | **已过**：`DirectoryBackendRefusesArchives`（`serialize` / `save(ostream)`） |
| **绝对路径不得逃出后端根**（需求文档未列，S1 新增） | **已过**：`DirectoryVfsNeverEscapesItsRoot` |
| ZIP 越界条目 → 未找到 | **已过**：`ZipVfsTest.ReportsErrorsAndRefusesEveryChange`（`NotFound` / `IsADirectory` / `NotADirectory`） |
| 惰性 zip 只读索引、条目按需解压 | **已过**：`ZipVfsTest.ReadsEntriesOnDemand`（删掉归档文件后再读 → `IoFailure`） |
| 惰性后端与内存后端行为一致 | **已过**：`ZipVfsTest.MatchesTheMemoryBackendForTheSameContent`（拿产生该包的 `ZipMemoryVfs` 树做基准） |
| 归档索引可供 `stat` / `list` 直接使用 | **已过**：`ZipVfsTest.ArchiveIndexReportsNamesSizesAndKinds` |
| Mount 优先级覆盖 → 高优先级胜出 | S4 |
| Mount 同前缀多后端 `list` → 合并去重（含中间目录） | S4 |
| VFS 析构后 Stream 仍可读 → 生命周期正确 | S3（弱化版：`intrusive_ptr<Buffer>` 存活） |
| 超出容量上限 → 超出容量 | S5 |
| seek 越界 → 越界 | S5 |
| 内容来源（片段 / 拉取）经 `Vfs&` 可达（默认实现 / override 两条路径） | **已过**：`IoBaseTest.DirectoryVfsAssemblesSourcesThroughTheBaseInterface` / `ZipArchiveKeepsSourcesLazyThroughTheBaseInterface` |
| 读侧流式经 `Vfs&` 可达（`openRead` / `read(sink)`；override / 默认两条路径） | **已过**：`IoBaseTest.ZipArchiveStreamsReadsThroughTheBaseInterface` / `DirectoryVfsStreamsReadsThroughTheBaseInterface`（三种拼法同内容：整读 / 流读 / 推 sink；sink 拒绝即停并原样返回错误；损坏成员在 push 末尾报 `IoFailure`，同文件 `ZipArchiveOpensWithoutReadingContent` 的损坏夹具）；越界 seek → `OutOfRange` |

## 15. 待裁决

1. ~~冲突操作的命名~~ **已决**：采用"一次断代"，直接删旧 `bool` 家族，新名字见 §13 对照表。
2. **写路由语义**（§10.3）：需求文档说"写走高优先级第一个**可写**后端"，这会出现"读到的与写入的不是同一个文件"
   （高优先级是只读 zip 时，写落到低优先级目录）。我建议**读和写都走同一个"第一个命中的挂载"，命中项只读就返回 `ReadOnly`** —— 可预测、不会写偏。要哪种？
3. **`MountVfs` 的持有方式**：建议用 `std::shared_ptr<IMemoryVfs>`（不动 `IMemoryVfs` 的 ref-count 状态；
   `unique_ptr` 可隐式转 `shared_ptr`）；是否接受？
4. ~~两个 zip 后端并存~~ **已决（§9.1）**：职责分层 —— `ZipVfs` 是打开已有包的唯一入口且只读，
   `ZipMemoryVfs` 只写（建树 → zip），`ZipArchive` 只做编解码；三方名字保留，重叠的 API 删掉不保留。
5. **容量上限的 API 形态**：`setCapacityLimit()` + `bool reserve()`（本文建议）还是只做 `reserve()`、上限推到 S3 的 `open` 层？
6. **就地更新（libzip cloning）备选 —— 已评估，暂不采纳**：`zip_close` 对支持 cloning 的文件源会保留原文件前缀、
   只重写后面的条目 + 中央目录（`zip_close.c` 的 `ZIP_SOURCE_BEGIN_WRITE_CLONING` 分支，
   “already implicitly copied by cloning”），所以**就地更新**大包的 I/O ≈ 改动 + 目录，
   而 `commit()` 写新目标的 I/O ≈ 整包（内存仍是 O(单条目)，两者不是一回事）。
   要它就得付出：可写方式打开源（只读介质 / 只读包打不开）、放弃“源永不被本次操作改写”、**非原子**（崩溃即坏包）。
   暂不采纳的理由：包是资产文件，损坏代价远大于一次压缩数据直拷的 I/O。
   **翻案条件**：出现“几十 MB 级以上包上频繁单文件编辑”的真实场景时，再把 `commit()` 切到就地 clone 更新。
7. **流式 sink（管道 / socket）写入 —— 本仓库无此需求，记录实现选项**：libzip 做不到（可写 source 强制 seek，
   `zip.h` 的 `ZIP_SOURCE_SUPPORTS_WRITABLE ⊇ SUPPORTS_SEEKABLE`），但 **zip 格式允许**：
   local header 尺寸填 0 + 通用标志位 bit 3（data descriptor）+ 尾部中央目录。能做到的库：
   Java `ZipOutputStream`、Go `archive/zip`、Python `zipfile`、.NET `ZipArchive`(Create)、minizip / minizip-ng、libarchive；
   做不到：libzip、Rust `zip`、miniz、minizip 需 seek 的 API 路径。
   若将来真需要：**优先自己写最小顺序 writer**（STORED/DEFLATE + data descriptor + 中央目录，约 300 行，
   零新依赖、保持 `.zip` 兼容，可与 libzip 路径共存：目标可 seek → libzip，不可 seek → 自写）；
   其次才是换 codec 库（代价：重写 zip 层 + 丢惰性 / 透传 / cloning + 新第三方依赖）。

---

**文档结束**
