# 几何数据的 revision 契约（2026-10-22）：谁公告，按什么粒度

> B6 的"先定契约，再谈省带宽"那一半。本文只回答契约与落点，不改代码；实现形状见 §6 的切片。
> 登记出处：`.ai/design/graphics-followups-2026-10-19.md` §3.6 的 B6 行。

## 0. 一句话

仓库里**已经有两级公告**（`Geometry::revision()` 粗、"这个几何变了"；`Buffer::revision()` 细、"这块字节变了"），
而且几何自己的派生缓存（局部包围盒）**两级都认**；但后端缓存字节的地方只读了粗的那一个。结果是：按细的那份
契约公告的宿主**今天会静默拿到旧字节**，顺便每次任一通道变动都把全部通道重传一遍（已量化 16.98 MiB / +30 ms）。

## 1. 今天的两份契约（以代码注释为准）

| 契约 | 原文要点 | 证据 |
| --- | --- | --- |
| 几何级 | `Geometry::setRevision()` 是"数据变了"的**唯一**公告；setter 不公告；`addBuffer` / `removeBuffer` / `setPositions` 等都不公告，由调用方报 revision | `Geometry.hpp` §revision（`setRevision` / `markModified`），`addBuffer` 的 `@details`、`removeBuffer` 的 `@details` |
| 缓冲级 | `Buffer::revision()` 是**内容版本**，"缓存了字节的人在用之前拿它比一比"；没有任何写路径自己移动它，只有 `setRevision()` | `Buffer.hpp` 文件注释 + `revision()` / `setRevision()` 的 `@details` |

几何自己的缓存就是这么写的：局部包围盒的键是 **`(buffer, 该 buffer 的 revision, 段) + 几何自己的 revision`**，
注释明说"通过裸指针写 buffer 由**这两个里的任一个**公告（本仓库每个缓存都按这条契约做键）"；用例也把这条钉住了
（`GraphicsTest` 里 `positions->setRevision(...)` 之后盒重算，注释写着"a buffer edited in place is announced"；
`ChannelSliceTest` 同样在改完 buffer 后 bump 它的 revision）。

## 2. 矛盾在哪里

后端上传缓存的键是 `core::StreamKey`，它的字段注释写的是：

> `revision` 来自**上游资源**（`Geometry::revision()` / `Buffer::revision()`）；后端从不自己比较字节造一个。

而实现是（`src/plugins/gfx_backend_vsg/src/internal/GeometryFacts.cpp`）：

- 每个属性通道：`facts.key.revision = geometry.revision();`（L107）
- 索引流：`index_facts.key.revision = geometry.revision();`（L178）

即"声明了两级、实际只认一级"。造成两个后果：

1. **正确性**：宿主按 `Buffer` 的契约改一块顶点再 bump 它的 revision，后端既不会重建事实行
   （`ContentFacts` 的几何行按 `(identity, geometry.revision())` 建键，`GeometryKey{row.geometry, row.revision}`），
   也不会换 `StreamKey` ⇒ 画出来的还是旧字节，而且**没有诊断**。这正是"两份契约只能留一套"里更坏的那种：
   留着两份、却只有一份生效。
2. **带宽**：宿主按几何级公告（今天唯一生效的那条），改一个通道 ⇒ 每通道的 `StreamKey` 全变 ⇒ 共享上传表全部
   未命中 ⇒ 全部重传。量化形状：16.98 MiB / +30 ms 一帧（B6 行）。

## 3. 为什么"直接把键换成 `Buffer::revision()`"是错的

那会把上面两半对调：细的那份生效，粗的那份失效。按几何级公告的宿主（今天所有写入方：`FpsOverlay`、
model 加载器、`SceneView` 的宿主）改完字节只 bump 几何 revision，通道自己的 buffer revision 没动 ⇒ 键不变 ⇒
**静默旧字节**。所以不能选边站，必须定"两级各自是什么意思"。

## 4. 目标形状：两级公告，粒度不同，都生效

| 谁 bump | 语义 | 后端的反应 |
| --- | --- | --- |
| `Buffer::setRevision(n)` | "**这块字节**变了" | 重建事实行；**只有读这个 buffer 的通道**换 `StreamKey` ⇒ 只重传它 |
| `Geometry::setRevision(n)` | "这个几何变了，我**没说是哪条**"（今天的写法，保持不变） | 重建事实行；**所有通道**换键 ⇒ 全部重传（与今天一致） |
| 派生通道（后端自己造的白色载体 / 零 UV / 计算法线） | 没有 buffer，字节归这个几何 | 与今天一致：跟着几何公告走 |

**通道键要同时带两个事实**，`StreamKey` 加一个字段：

| 字段 | 值 | 为什么 |
| --- | --- | --- |
| `revision`（已有） | `geometry.revision()` | 粗粒度公告必须让**每个**通道换键——否则"我没说是哪条"就失效了 |
| `buffer_revision`（新增） | 该通道自己的 buffer revision（派生通道为 0，buffer 也为 null） | 细粒度公告只让**读这个 buffer 的**通道换键 |

相等与哈希都看两个字段。这样两份契约同时成立：buffer bump ⇒ 只有那一条换键 ⇒ 只重传它；几何 bump ⇒
全部换键 ⇒ 全部重传（与今天同义）。

**一处必须说清的代价**：两个几何读同一 buffer 同一段时，上传能不能共用仍然取决于**几何 revision 是否相等**
（今天就是这样，因为 `StreamKey.revision` 是它）。"按字节身份共用、与几何 revision 无关"和"几何级公告让全部
通道失效"是**互斥**的——前者要求键里没有几何 rev，后者要求有。选后者：粗粒度是仓库里**唯一在用的**公告，
而字节级共用是在"两个几何恰好同 revision"时才可能发生的既有行为，没人为它登记过需求。

**`Geometry::contentRevision()`**（派生、不可写、不是新的公告口）只回答"这份事实表要不要重算"：
`fold(自身 revision, 每个通道的 (location, buffer 指针, buffer revision, components, offset, scalarCount),
索引流的 (buffer 指针, revision, offset, count))`。事实行键用它（`(identity, contentRevision())`），
**不**用作单通道上传键。它是 FNV-1a 折叠值，不单调、只当身份比较——这一点写进注释，免得有人拿它排序。

单调性沿用今天的规矩（`Buffer::revision()` 的注释已经写明"只升不降"）。

## 5. 成本与收益

| 项 | 今天 | 改后 |
| --- | --- | --- |
| 每帧每几何 | `revision()` 一次读 | `contentRevision()` 一次折叠：通道数那么多次整数读（无分配） |
| 任一通道变动时的事实行重建 | 复制**所有**通道的标量到 `vsg::floatArray` | 同样复制所有通道（形状不变；`geometryFacts()` 是按几何整份建的） |
| 任一通道变动时的**上传** | **所有**通道 | **只有**变的那一条（其余 `StreamKey` 不变，共享上传表直接命中） |
| 共享上传的别名 | 两个几何只有在**几何 revision 相等**时才可能共用一段 arena 的上传 | 按字节身份共用：同一 buffer 同一 revision 的同一段就是同一次上传，与几何 revision 无关 |
| 按 `Buffer` 契约公告的宿主 | **静默旧字节**（缺陷） | 生效 |

代价一句话：每次"内容变了"的判断从 O(1) 变成 O(通道数)，换回上传量与正确性。通道数是个位数，这个交换没有争议。

## 6. 切片与判据

**S1（本文）**：契约、落点、判据。无代码。

**S2（已落地 2026-10-22，见 §9）**：

1. `Geometry::contentRevision()`（公开、派生）+ 相应 `@details`；`Geometry::revision()` 的注释补一句"粗粒度，
   不知道是哪条通道"。
2. `GeometryFacts.cpp`：通道 / 索引键取 buffer 自己的 revision；派生通道取 `contentRevision()`。
3. `ContentFacts` 的几何行：建键与查找都用 `contentRevision()`（`GeometryKey` 的字段不变）。
4. `StreamKey` 加 `buffer_revision`（相等 + 哈希都看），并把两个字段的注释写清：
   `revision` = 几何级公告，`buffer_revision` = 该 buffer 自己的公告（派生通道为 0）。

判据（每条都要能红）：

- **细粒度生效**：两个通道的几何，改 positions 缓冲的字节 + bump 它的 revision ⇒ 重算事实后**只有 positions
  的键变了**，另一个通道的键逐字段相同（等价于"只重传一条"）。变异：`buffer_revision` 恒 0 ⇒ 一条都不变，红。
- **粗粒度仍是"全部"**：只 bump 几何 revision ⇒ **每个**通道的键都变（`revision` 字段）。变异：通道键的
  `revision` 改成 `buffer_revision`（丢掉粗粒度）⇒ 只 bump 几何时一条都不变，红。
- **事实行跟着内容走**：只 bump 一个 buffer 的 revision ⇒ `contentRevision()` 变、`geometry.revision()` 不变，
  所以事实行会重建（读到的是新字节）。变异：行键改回 `geometry.revision()` ⇒ 用例看到旧字节，红。
- **别名照旧**：两个几何同 buffer 同段同 revision ⇒ 键相等（一次上传）；几何 revision 不同 ⇒ 键不等（各一次）。
  这一条钉的是"别名范围没有悄悄扩大也没有缩小"。

**S3（已落地 2026-10-22）**：`Geometry::revision()` / `contentRevision()`、`StreamKey`、`FrameRecorder` / `FrameCompiler`
的 `geometry_revision` 注释统一成两级口径；B6 行关闭并指向本文。

## 7. 不做的话

- 缺陷留着：按 `Buffer` 文档写代码的宿主拿到旧字节，且**没有任何诊断**能提示它 —— 这类"两份契约一份生效"的
  形状，正是本仓库一直在收的那种。
- 带宽形状留着：单通道编辑 = 全通道重传（16.98 MiB / +30 ms 已量化）。

## 8. 待拍板

1. **范围**：S2 一起做（推荐：它同时是正确性修复），还是只落 S1 的契约、等宿主报带宽。
2. **`contentRevision()` 的名字**：备选 `streamRevision()`（更像"通道们合起来"）、`dataRevision()`（与
   `Geometry::revision()` 太近，不推荐）。倾向 `contentRevision()`：它答的就是"这份内容现在是什么版本"。

## 9. 落地记录（2026-10-22 晚）

| 片 | 落了什么 | 证据 |
| --- | --- | --- |
| **SDK** | `Geometry::contentRevision()`：折叠 `revision_` + 索引流 `(buffer, revision, first, count)` + 每个通道 `(location, buffer, buffer revision, components, offset, scalarCount)`，FNV-1a，不分配、O(通道数)；`revision()` 的注释写明它是**粗**公告 | `Geometry.hpp` / `Geometry.cpp` |
| **上传键** | `StreamKey` 新增 `buffer_revision`（相等 + 哈希都看）。`revision` 仍是几何级公告（粗粒度让每个通道换键），`buffer_revision` 是该 buffer 自己的（细粒度只让读它的通道换键） | `Streams.hpp` / `Streams.cpp` |
| **事实** | `buildGeometryFacts` 的行 revision = `contentRevision()`；通道 / 索引键写两个 revision（派生通道 `buffer_revision` 为 0） | `GeometryFacts.cpp` |
| **链路** | `FrameRecorder` 把 `contentRevision()` 写进 `CollectedCommand.geometry_revision`；`ContentStore::ensureGeometry` / 它的 `findGeometry` 都用同一个数 | `FrameRecorder.cpp`、`ContentStore.cpp` |
| **判据** | 新套件 `GeometryRevisionTest`（3 条）：buffer 公告只换它那条键；几何公告换全部键；同 buffer 同段的两几何仍只在**几何 revision 相等**时共用上传（别名范围没变）。变异实测：`buffer_revision` 恒 0 ⇒ 第 1 条红；通道 `revision` 改取 buffer revision ⇒ 3 条全红；行 revision 退回 `geometry.revision()` ⇒ 第 1 条红 | `tests/test_vsg/GeometryRevisionTest.cpp` |
| **数字** | `test_vsg` 473（+3）、`test_graphics` 413、`test_gui` 261、`test_core` 165；完整 ctest 24/24；`vsg_rewrite_gate.sh` 全阶段 clean（0 VUID / 0 SYNC-HAZARD、相位闭合、画面三项与基线一致）；6 个静态门禁 0 finding | 见提交信息 |
| **顺手** | 迁移测试里手写的命令/查找参数（`commandOf(... geometry->revision() ...)`）到 `contentRevision()`：那是 `FrameRecorder` 的同一件事；一处陈旧插件 `.so` 让 `VsgBackendPluginTest` 假红（`StreamKey` 布局变了），全量重建后绿 —— **改 SDK 布局后必须整树重建** | `ContentHalvesTest` / `ContentStoreTest` / `ContentSweepTest` / `ContentPassTest` / `GeometryFactsTest` |
