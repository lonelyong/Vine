# ADR-2026-11-10：GPU 驻留按内容对象钉住（pin），按"谁活着"回收

- 状态：已采纳（第一、二步落地，其余按 §迁移 推进）
- 日期：2026-11-10
- 相关：`../design/graphics-content-lifecycle.md`、`../design/vsg-backend.md` §2.10、`../backlog/graphics.md`
  `src/viz/graphics/src/Scene.cpp`、`src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp`、
  `src/viz/graphics/sdk/vine/graphics/backend/Streams.hpp`、`src/viz/graphics/sdk/vine/graphics/Residency.hpp`

## 背景

同一个问题——"这份 GPU 驻留什么时候可以放掉"——今天有五种答案：

| 驻留 | 判据 | 机制 |
|---|---|---|
| 顶点/索引/实例流 | 最近被命名 + 窗口（`grace = slots + 1`）| `SharedStreams::releaseUnseen` |
| 纹理图像 | 引用计数 `owner->useCount() <= 1` | `MaterialImages::releaseAbandoned` |
| 材质块槽 | 引用计数 + 行扫尾 | `ContentStore::releaseAbandoned` → `BlockStorage::releaseMaterial` |
| 编译半片（管线）| 反事实查询（表还答 program@rev 吗 ∧ 还有几何问这个 layout 吗）| `ContentHalves::answerable` |
| 描述符集 | 同上 + 每帧 `ensure` | `ContentSets::sweep` |

三种失败形状，实测都在：**裁剪丢上传**（第一条的时间性：离开视野 `slots + 1` 帧就让出上传，回来重传）；
**`useCount` 阈值随持有者变化而静默失效**（第二、三条：任何一个新持有者都让 1 变成 2）；
**每 pass 的 O(#halves × #geometries) 反事实扫描**（第四、五条，随资源种类数增长）。

## 其它库怎么做的（借什么、不借什么）

- **wgpu-core**：资源分 **user refs** 与 **submission refs** 两套引用，销毁走 triage（`ResourceTracker`）——
  只在没有在飞提交引用时真正销毁。**借**：两套引用是两件事（对应本仓库的"引用计数"与"退役队列"）。
  **不借**：它把两套引用都挂在资源上；本仓库的退役队列已经是独立对象，不需要资源知道在飞状态。
- **bgfx**：句柄 + 延迟销毁队列，按帧延迟（`BGFX_CONFIG_MAX_FRAME_LATENCY`）。**借**：延迟销毁的窗口就是
  `RetirementQueue`；**不借**：延迟窗口不能替代"谁还在用"的判断。
- **Diligent Engine**：显式 `ReleaseStaleResources()` + fence。**借**：释放是显式的、由宿主驱动（与
  `ADR-2026-10-22-cache-unbounded-explicit-release` 同向）。
- **Godot RenderingDevice**：RID + `free_rid(rid, dependencies)`，依赖决定真正释放的时机。**借**：
  "释放时点名依赖"就是本 ADR 的 pin。
- **QRhi（Qt）**：`QRhiResource` 基类 + `create()` / `destroy()`。**不借**：它成立的前提是 QRhi **自己**
  创建 Vulkan 对象；本仓库把创建交给 vsg，基类会变成一层转发（`G7`）。
- **Dawn（WebGPU）**：对象内部引用 + 命令缓冲的"内部用途"引用。**借**：内部引用与用户引用分开；
  **不借**：需要改 `RefCounted` 的布局（本仓库的 `intrusive_ptr` 刻意零分配）。
- **VMA**：suballocation（材质块/每帧块就是它的形状）。**借**：块不是独立资源，账与释放分开记。
- **VulkanSceneGraph**：侵入式引用 + 每帧 record/compile 出新的命令节点，`CompileTraversal` 缓存
  实现。**事实**：对象释放只能靠"丢引用"，所以"什么时候丢"必须由本仓库自己回答。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 保持现状 | 零改动 | 五种判据；裁剪丢上传；反事实扫描（`G1` / `G6` / `G10`）|
| B 窗口调大（`grace = N`）| 抖动变少 | 仍是时间猜测；"多久算久"是策略而不是事实（`G8`）|
| C 两级淘汰（释放驻留但留 key）| 早还内存 | 回来仍要重传；多一个状态（`G1`）|
| D `GpuResource` 基类 + 虚释放 | 统一动词 | 覆盖不到流/描述符集/管线（无内容对象可继承）；需 `RenderDevice&` 参数（`G2` 反向边）；把 GPU 概念压给内容类型（`G13` 同类问题）|
| E 生命周期状态机挂在对象上 | 直观 | 状态本是三时钟的函数；在飞事实会进对象，`RetirementQueue` 已精确表达（`G1`）|
| F **pin =（内容身份, revision），按"谁活着"回收** | 一条规则、一个动词、零时间窗、反事实扫描消失 | 显存不再自动让出（见"代价"）|

## 决定

选 **F**：

1. 每个驻留条目记 **pin =（内容对象的身份, revision）**；`revision` 是内容对象自己的"这一次的内容"公告
   （`Geometry::revision` / `Material::revision` / `ProgramFacts::generation`），所以"换了一版"与"换了一个对象"
   用同一个键表达。
2. 回收不问"它死了吗"，只问"**现在谁活着**"：每帧（或内容变更时）由事实表枚举活的 pin 集合，不在集合里的
   驻留统一 retire（`RetirementQueue` 停车，不 free）。
3. 删除全部时间窗与容量上限：`Streams.hpp` 的 `last_frame` / `grace` / `releaseUnseen` / `evictions` 退役；
   `ContentHalves::answerable` 与 `ContentSets::sweep` 由 pin 存活替代（半片的 pin = program@rev，见"开放项"）。
4. 显式释放仍是宿主动词（`releaseContent(Object)`，补 `Buffer`），它等价于"立刻把这些 pin 判死"。
5. 重建是天然的：`acquire` miss 调 builder；同一 key 允许**多代共存**（`generation`），旧的靠停车窗口消失。

**为什么这比"基类"好**：pin 是**值**，不动对象模型，所以内容类型不承担 GPU 概念（`G13`），
不需要 `RenderDevice&` 参数（`G2`），而且覆盖到**没有内容对象可继承**的那三类（流、描述符集、管线）。

## 代价

- **显存**：驻留 = 活内容所需要的全部。移除场景但宿主仍持有的 600 MB 模型会常驻，直到 `releaseContent()`。
  这是显式管理（`ADR-2026-10-22`）的必然结果，也是本 ADR 唯一的真实代价，必须写进
  `graphics-content-lifecycle.md` 的现役要点。
- **半片累积**：pin 只看 program@rev 时，layout/变体更替会让半片累积（每个是几十 KB 的管线）。
  这一条要按 `G10` 量一次再定（见开放项 1）。

## 迁移

每一步独立可验证、可回退：

1. **词汇 + 账本**（已落地）：`vn::graphics::ResidencyLedger` / `ResidencyRow` / `ResidencyKind`（`graphics/` 半，
   Vulkan-free），`RetentionStats::residency` 暴露；`gfx_backend_vsg` 把流 / 图像 / 材质块三张表接进去。
   无行为变化。
2. **流**迁入 pin 模型：`StreamKey` 表泛化到 `backend/ResourceTable<K, V>`，pin = 活几何行声明的
   （buffer 地址, 该 buffer 此刻的 revision），删窗口。
   验收：裁剪 100 帧零重传；离树后 `slots + 1` 帧内离场。
3. **纹理**迁入：删 `MaterialImages::releaseAbandoned`，pin = texture@rev。
4. **材质块**迁入：builder 写 arena、destroyer 放槽；删 `ContentStore` 三个释放循环 → 一个 `retireOwner`。
5. **半片 + 描述符集**迁入：删 `answerable` 与 `ContentSets::sweep`，pin = program@rev（+ 描述符集另加
   5 个贴图 pin）。删掉的是两份 O(n × m) 扫描。

### 迁移 2 的设计：流 → pin（`backend/ResourceTable<K, V>`）

#### 为什么先有表，而不是就地改 `StreamUploads`

流、纹理、材质块、半片、描述符集要的是**同一条规则**（值按键构建、被活内容钉住、没人钉住就停车），
差别只在键与值（`G1`、`G6`）。把它写成一张表，`StreamUploads` 就只剩"什么算一条流、一次上传要建哪些对象"；
后面的四步不再各自重写一遍"表 + 停车 + 代次"，也不会各自长出一套寿命判据（`G5`：变化留在模块里）。

#### 接口（`src/viz/graphics/sdk/vine/graphics/backend/ResourceTable.hpp`，Vulkan-free）

```cpp
template <class Key, class Value, class Hash = std::hash<Key>>
class ResourceTable {
  public:
    struct Acquired {
        Value*        value{ nullptr }; // 命中或刚构建的值；留在表里的期间可读（unordered_map 引用稳定）
        std::uint64_t generation{ 0 };  // 这一个"值"的代次（见下）
        bool          built{ false };   // true = 本次调用建的值
    };

    template <class Build>              // Build() -> std::optional<Value>；空 = 拒绝，表不变
    [[nodiscard]] Acquired acquire(const Key& key, Build&& build);

    template <class Live>               // Live(const Key&) -> bool
    std::size_t retireUnpinned(Live&& is_live, FrameTimeline&, RetirementQueue&);

    [[nodiscard]] std::size_t   live() const noexcept;
    [[nodiscard]] std::uint64_t builds() const noexcept;
    [[nodiscard]] std::size_t   retired() const noexcept;  // 累计离场（本代已停车）
    [[nodiscard]] std::size_t   retained() const noexcept; // 停不上车而留下的次数
    void clear();
};
```

- **没有容量、没有时间戳、没有 `last_frame`**：寿命唯一地由 `is_live` 决定。`acquire` 命中不碰 builder
  （稳态一帧零构建）；未命中的 builder 在表外构造，成功才移动进表，拒绝（派生通道、非规范 location 等）
  不改变表。
- **`retireUnpinned`**：对每个 `is_live(key) == false` 的条目，把值移进停车 lambda、从表里删掉、`retired++`；
  `RetirementQueue::retire` 拒绝（还没学到在飞槽数）⇒ **留下**并计 `retained`，下一帧再试。绝不提前删
  （与 `ContentStore` 的 `retained` 同一条纪律）。
- **`Value` 必须可拷贝**：停车用的 `ReleaseFn` 是 `std::function`，它要求可调用体可拷贝；`vsg::ref_ptr`
  与 `intrusive_ptr` 满足这一条，而"值"正是它们的组合。

#### `generation`：同键多代

`generation` 是**一个已构建值**的表内编号（单调、不回收），`acquire` 的调用方和表自己的日志都能拿它认人。
同一个键可以在旧值**还在停车窗口里**时重新构建：旧代次随它捕获的值停在 `RetirementQueue` 里，新代次从下一个
编号开始服务查找。所以"同键多代共存"不是表里有两份条目，而是**表里一份 + 队列里 N 份**；编号让任何迟缓的
记账（测试、日志、下一层可能持有的旧句柄）能认出自己手里的是哪一代，而不会把新值当成旧的——与
`ProgramFacts::generation` 是同一个想法低一层。

步骤 2 里流的键已含 buffer revision，正常路径不会重建同键；这一条是给步骤 4（材质块按键身份、revision 另计）
和步骤 5（半片按 program@rev）留的同一个抽屉（`G9`），今天由 `ResourceTableTest` 的计数型 `Value` 钉住。

#### live-pin 的枚举点：内容层发布，条目自答

**先把成本定死**：驻留账每帧只能走 O(#驻留条目)，不能走 O(活内容)。pin 模型让裁剪不释放，于是"被裁剪的
几何行"会一直留在表里——若每帧去扫这些行，一个什么都没画的帧反而最贵。这正是第一版的缺陷（按代码结构推算，
10 万几何 × 4 条声明 ≈ 每帧几十万次哈希/解引用，且与画面无关）。所以 v2 把 pin 拆成两半，各用一个 O(1) 事实：

- **地址那一半由内容层发布**：`ContentStore` 的几何表包装（`GeometryTable`）在行 `append` / `eraseIf` /
  `clear` 的**同一个方法里**维护一个 `地址 → 声明计数` 索引（共享 arena 被多个通道 / 多行声明时按引用计数，
  最后一个声明走掉地址才离开），`ContentFacts::declarations`（`ContentDeclarations`）只借用它。
  裁剪不删行 ⇒ 计数不变 ⇒ 一个 `declaresBuffer()` 就答完。
- **revision 那一半由条目自答**：条目持有它上传时用的 buffer 句柄，`key.revision == owner->revision()`
  是一次比较。buffer `edit()` 换代不碰任何行，旧条目自己就知道该走。
- 于是 `StreamUploads::beginFrame(const ContentFacts&, timeline, retirement)` 每帧只遍历自己的两张表；
  内容变化帧的额外成本是行变更本身（本来就是 O(delta)）加一次索引借用，不是每帧重扫。
- `PrimitiveFacts::instances` 保留：实例 buffer 是图元声明的字节，索引在行建好时把它计入——所以实例化
  draw 被裁剪也不会丢实例上传。
- **owner 必填**：没有句柄的"借用身份"没有任何东西能保留地址（旧实现用快照兜底，等于把一条静默错图留给
  落在同址的新 buffer）。现在 `acquire*` 直接 `Refused`；生产录制路径本来就总是带句柄，测试夹具改成构造
  真实 `vn::Buffer`（`tests/test_vsg/TestStreamUploads.hpp`），与生产同形。

#### `StreamUploads` 的 builder / destroyer 形状

- **builder**：`acquireVertex` / `acquireIndex` 的未命中分支原样搬进 `ResourceTable::acquire` 的 builder lambda
  （零拷贝别名或快照 + 一个 `Bind*Buffers`）；返回值从 "Action + ref_ptr" 变成 "Action + 表给出的 Value*"，
  `ref_ptr` 由调用方拷一份。条目仍持有 buffer 句柄（地址在键可查期间不被回收，见
  `graphics-content-lifecycle.md` 2.6）。
- **destroyer**：没有独立函数。`retireUnpinned` 把整个 `Value`（bind + owner 句柄）移进停车 lambda，
  停车到期时 lambda 析构 = 释放 bind 与句柄。因此"条目离场"（现在）与"对象销毁"（窗口到期）仍然分开，
  与 `ContentStore` 的停车同一把尺子。
- **删除**：`beginFrame(frame, grace)`、`releaseUnseen()`、`evictions()`、`unused()`、容量构造参数；
  `core::SharedStreams` 整个类（`backend/Streams.hpp` 只留 `StreamKey` / `StreamKeyHash`）。
  腾出的名字按新口径：
  - `beginFrame(facts, timeline, retirement)` 返回本帧离场数（读的是 `facts.declarations`）；
  - `retired()` 取代 `unused() + evictions()` 喂 `ResidencyLedger::Streams.retired`；
  - `uploads()` / `aliases()` / `refusals()` 不变（`aliases` 由适配层数，因为表只数构建）。

#### 验收怎么落到用例

| 判据 | 用例 |
|---|---|
| 裁剪 100 帧 `uploads()` 不涨 | `ContentStoreTest.ACulledGeometryKeepsItsUploadsForAHundredFrames`（同一份事实跑 100 帧，零构建；条目数不变） |
| 离树后 `slots + 1` 帧内 `live()` 下降 | `ContentStoreTest.AStreamLeavesWithinTheParkingWindowAfterItsGeometryIsDropped`（`releaseAbandoned` 停车 → 行删 → 声明消失 → 条目离场） |
| 同 buffer 两几何共享一条上传，走掉一个不影响 | `StreamUploadsTest.TwoGeometriesOfOneBufferShareOneUploadUntilBothLeave`（声明计数合并） |
| revision 前进旧条目离场 | `ContentStoreTest.AStreamWhoseBufferMovedLeavesWithoutTheGeometryChanging` + `StreamUploadsTest.ABufferRevisionThatMovedRetiresTheEntryTheContentStillDeclares`（条目读自己的句柄） |
| 声明索引随行变更、共享 buffer 的计数、以及停车窗口 | `ContentStoreTest.TheDeclarationsFollowTheRowsAndOutliveThemForTheParkingWindow` |
| 表自己的契约（同位命中 / 拒绝不改表 / 停车 / 保留 / 代次） | `ResourceTableTest`（计数型 `Value`：旧值在停车窗口里仍活着，新值另起一代） |

每步之后跑：`check_ai_docs.py --strict`、`check_design_docs.py`、`check_doc_symbols.py`、
`check_include_hygiene.py`、`check_export_annotations.py`、`check_public_docs.py`、
`check_vsg_content_seam.cmake`、`test_vsg`（含 `StreamUploadsTest` / `BlockStorageTest` / `ContentPassTest` /
`VsgBackendTest` 的编辑成本配方）。

## 开放项

1. 半片的 pin 是否只看 program@rev（会累积）还是另加"layout 仍被活几何需要"（回到一份扫描）。**倾向后者只
   在实测显示累积不可忽略时才加**。
2. 是否给 `RefCounted` 加 weak 支持：**不做**。pin 集合由事实表枚举，不需要"对象已死"的通知。
3. `Buffer` 仍然不在事实表里，但它不需要“成为身份”：流的 pin 从**几何行的 buffer 句柄**上读
   （地址 + 此刻的 `revision()`），所以几何声明字节、buffer 宣告换代，两件事各自生效；共享 buffer 的条目
   自然由多个几何行共同钉住。

## 后果

- `G1` / `G4` / `G6`：驻留寿命一个定义、一个动词、一套判据。
- `G7`：宿主的接口不变（`releaseContent` + 现有报告），复杂度收进后端。
- `G10`：删掉每 pass 的反事实扫描；代价是显存，写清楚并计量。
- `G13`：内容类型不新增 GPU 概念；pin 由后端从身份派生。
- 被取代：`Streams.hpp` 的读者计数与时间窗都退役（`SharedStreams` 整个类删除）；`StreamKey` 作为身份不变。
