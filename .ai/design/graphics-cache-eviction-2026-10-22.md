# A6 缓存容量与释放设计（2026-10-22 修订）：不设上限，缓存随宿主持有量增长，释放显式管理

> **状态：已落地（2026-10-22）**。用户 2026-10-22 修正方向：**不设容量上限，有多少内存就用多少，释放要显式管理**。
> 前一版（LRU + 压力驱动增长 + 512 MiB 字节上限）**作废**；为什么作废见 §1.2。落地记录见 §9。

## 0. 结论

| # | 决定 | 理由 |
| --- | --- | --- |
| **D1** | 纹理缓存**删除容量上限与淘汰**：条目跟着"宿主仍持有的纹理"走 | 上限制造了"每帧重传"；活纹理的 GPU 驻留是宿主的显式选择 |
| **D2** | 材质 arena **删除 FIFO 淘汰**：在**记录之前**按 `ContentStore` 的活材质数一次长够（无硬上限）；拒绝只作兜底 | 淘汰活材质可能复用本帧已记录的槽（同 copy）⇒ 画错；记录前扩容既不丢 draw 也不移动已记录字节 |
| **D3** | 释放活内容只有一条路：A3 的 `releaseContent()`；`releaseAbandoned()` 只处理"宿主已经放手" | "显式管理"= 谁持有谁释放；缓存不替宿主决定 |
| **D4** | 材质槽必须随释放归还：显式 `releaseContent(material)` 与弃用扫尾都归还 arena 槽 | 不归还就不是"显式管理"，而是把内存漏给下一次扩容 |
| **D5** | 同一契约覆盖两处实现：`MaterialImages`（纹理）与 `core::MaterialArena`（材质槽） | 只改一处，另一处仍会退化（用户记忆里的"同一契约 N 处实现"教训） |
| **D6** | 报告：拒绝与扩容各一条（episode）；不再有"被上限挤掉"的 Warning | 没有上限，就没有"被上限挤掉"这件事 |
| **D7** | 不做：LRU、字节上限、增长阈值、收缩、全局 trim、宿主预算旋钮 | 在新方向下都没有存在理由 |

## 1. 背景

### 1.1 已量化的事实

| 事实 | 位置 | 对设计的影响 |
| --- | --- | --- |
| 纹理缓存按 `(texture, revision)` 键，条目持有纹理 | `MaterialImages::Entry` | 条目离开不影响画面（描述符集自持 view），只影响下一次 acquire 的重建成本 |
| 上限 256 条，淘汰键是 `stamp = ++clock`（插入序） | `MaterialImages::acquireImpl` | 300 张工作集 ⇒ 每轮 300/300 缺失、每张约 12.7 µs（lavapipe，§11.16cy） |
| 材质 arena 也是 256 槽 FIFO | `core::MaterialArena::note` | 300 个材质 ⇒ 每帧 0 命中、300 写、300 驱逐，**永远没有稳态帧**（§11.16cx） |
| 材质槽只在淘汰时复用，`release()` 没有生产调用者 | `MaterialArena` / `BlockStorage` | 显式释放材质不会归还槽；A3 已把它登记为 12/12 残量 |
| 纹理 acquire 只在**描述符集重建**时发生 | `ContentSets::ensure` | "每帧重传"只在集重建的帧出现；但 300 张的工作集每次重建都全量上传 |
| `releaseAbandoned()` 与显式 `releaseContent()` 已存在 | `ContentSweep` / `VsgBackend::releaseContent` | "显式管理"的入口已经有了，A6 只需要**不再自动丢活内容**，并把材质槽接上 |

### 1.2 上一版方案为什么作废

- 只改 LRU：300 张同序循环下 LRU 与 FIFO 一样 300/300 缺失（已实测的 A/B/X 判别）。
- 加上限/增长/字节预算：上限本身是任意数字（256 条对 4K 纹理可达 21 GiB，512 MiB 对 demo 又太紧），
  而且是在替宿主做"哪些内容值得留在 GPU 上"的决定。
- 用户定的方向更简单，也更符合所有权：**缓存保留宿主仍持有的一切；宿主想省内存就显式释放**。
  这样"容量"不再是框架的旋钮，而是宿主持有量的函数。

### 1.3 顺带发现的一个正确性风险（A6-b 先证实再修）

`MaterialArena::note()` 在槽满时把 `order_.front()` 的槽直接复用给新材质，并写入**当前 copy**。
而同一帧里更早记录的 draw 可能已经绑定了同一个 `(slot, copy)` 的偏移——命令缓冲在提交时读字节，
于是那些 draw 会读到**新材质**的内容。也就是说：超过 256 个材质的帧可能不是"每帧重传"，而是**静默画错**。

这条要先写成设备像素判据证实（§4.1 的 `AWorkingSetLargerThanTheMaterialArenaNeverPaintsWithAnotherMaterialsBytes`）。
A6-b 删除淘汰之后，这条路径在构造上不存在；如果证实为真，它同时是一个独立缺陷的修复。

## 2. 新契约

### 2.1 纹理缓存：上限删除，条目跟宿主走

- **条目生命周期 = 宿主仍持有该纹理**。缓存仍持有一个 owning share（地址复用安全，见类注释），
  但它不再因为"装不下"而丢弃任何条目。
- **移除只有两条路**：
  1. 宿主丢掉纹理 ⇒ 下一帧扫尾 `releaseAbandoned()`（"没人持有"）；
  2. 宿主显式 `releaseContent(texture)` ⇒ `releaseTexture()` 立即删条目（描述符集自持 view，不画错）。
- 删除：`kMaxEntries`、`stamp`、`clock`、`evictions()`、`settleFrame()`、`Policy`。
- 保留并新增观测：`count()`（条目数）与 `bytes()`（活条目的暂存字节）。
  **驻留量是事实，不是预算**：框架不因它发警告，也不据此淘汰。
- `bytes` 在 `makeImage()` 里顺手得到（各层各 mip 的和），随插入/移除维护总数；不额外扫像素。
- 稳态帧：一次 acquire 也不发生（描述符集命中）⇒ 零工作、零分配。
- 类注释改写：不再说 "Past the bound the OLDEST entries are evicted"，改为
  "an entry lives as long as the host holds its texture; releasing is the host's decision"。

### 2.2 材质 arena：不淘汰，记录前按活材质数长够

- **删除 `order_` 与 `evictions_`**。`note()` 不再挤掉最老的材质。
- `BlockStorage::Layout::materials.capacity` 的默认 256 **只是第一次分配的起步量，不是上限**：
  它决定初始 buffer 大小，之后按需长。
- **扩容时机在记录之前**：`VsgBackend::commitFrameThrough()` 在 `recordContent()` 之前调用
  `growMaterialArenaIfNeeded(store->materialCount())`：
  - 需求 = `ContentStore` 当前持有的活材质行数（`ContentStore::materialCount()`，O(1)，不扫描本帧命令）；
  - 若 `need > capacity`，按 B5 的整块替换机制换新 storage：
    `materials.capacity = max(need, 2 × capacity)`、新 buffer、`repoint`、旧 storage 进退役队列；
  - **没有硬上限**：上限就是设备分配失败。`BlockStorage::create` 失败时保留旧 storage 并报告；
  - 新 arena 从空开始，本帧记录时把画到的材质全部分配进去。capacity ≥ store 的活材质数 ≥ 本帧画到的
    材质数，所以**正常路径不拒画、不丢帧**。
- **为什么用活材质数而不是本帧不同材质数**：前者 O(1)，且与"宿主仍持有的内容应当保留"同一口径；
  宿主想少占内存就 `releaseContent(material)`。更省内存的"只按本帧画到的材质"要一次扫描（§8）。
- **兜底**：`note()` 仍可能返回 `Refused`（一个没经过 store 的材质，或计数暂时落后）：
  `writeMaterial()` 返回 `valid == false`，`ContentPass` 报一次并跳过这条 draw，**绝不**用别人的材质字节顶上；
  同时记 `growth.materials = max(need, arena.capacityNeed())`，下一次替换兑现。
- **为什么帧内不扩容**：块偏移是 `capacity × copies × 64 B` 的一段连续 buffer，扩容就是换 buffer；
  原地长大会移动已提交命令缓冲仍命名的字节（`BlockStorage` 文件头写明）。记录前替换没有"已记录的本帧字节"，
  所以安全；B5 已经用"新 buffer + 停放"把这套机制实现过一次。

### 2.3 显式释放（"谁持有谁释放"）

| 内容 | 宿主丢掉引用 | 宿主显式释放 |
| --- | --- | --- |
| 纹理 | `releaseAbandoned()` 删条目（下一帧扫尾） | `releaseContent(texture)` ⇒ `releaseTexture()` 立即删条目 |
| 材质 | `releaseAbandoned()` 删行 **并归还 arena 槽** | `releaseContent(material)` 删行 **并归还 arena 槽** |
| 几何 / 程序 | 现有 A3 扫尾 | 现有 A3 入口 |

- 材质槽的归还：`BlockStorage` 新增 `releaseMaterial(const void*)` → `arena.release()`。
  - `VsgBackend::releaseContent(material)` 在 `store->releaseMaterial()` 成功后调用它；
  - `ContentSweep` 的弃用扫尾需要**被释放材质的身份**（现在只返回计数）：`ContentStore::releaseAbandoned`
    增加一个复用 vector 的出参，`VsgBackend` 逐个 `storage->releaseMaterial(identity)`。
    身份是裸指针，只当键用、不解引用（对象可能已经析构），与 `MaterialArena` 的契约一致。
- **槽立即复用安全吗**：安全。每个槽有 `copies` 份（比在飞帧数多一），新材质写的是**当前 copy**，
  在飞帧读的是更旧的 copy；等轮回到旧 copy 时那些帧已经完成。这正是 arena 存在的理由。
- 释放必须发生在**帧与帧之间**（A3 的既有规则），所以不会和"本帧正在记录"相遇。
- **已知代价**：一个曾经画过、但宿主仍持有且不再画的材质，会一直占着一个槽，直到宿主调用
  `releaseContent(material)`。这正是"谁持有谁释放"的定义，不是漏；如果宿主希望按"最近是否使用"自动回收，
  那是它自己的策略（它知道自己的材质何时不再需要），不是框架的淘汰策略。

### 2.4 报告与观测

- **材质槽满（兜底路径）**：`ResourcePressure / Warning`，每 episode 一条：
  "the material arena is full (N slots): the draws of M material(s) were skipped this frame; the block storage
  grows before the next frame"。计数与句子都走现有的 `ReportOnce`。正常路径（记录前按活材质数扩容）
  不会走到这里。
- **扩容成功**：沿用同一套替换与报告（`adoptBlockStorage` + 现有存储替换句子，`ContentSkipped / Info`），
  `append("materials", from, to)`；一次替换一条。
- **纹理缓存**：不再有上限警告。`count()` / `bytes()` 供诊断与测试；宿主若要"看 GPU 驻留量"，
  是一个独立的公开面问题（§8），不是 A6 自动要做的事。
- **不把"内存用得多"当异常**：那是宿主的策略；框架只负责"用多少、什么时候放"都说得清。

## 3. 形状（实现级）

### 3.1 `MaterialImages`

```cpp
// 删除：kMaxEntries、Entry::stamp、Data::clock、evictions()、settleFrame()、Policy。
// 保留：owner（地址复用安全）、revision（重填后重建）、view、default_sampler。
// 新增：
[[nodiscard]] std::uint64_t bytes() const noexcept;  ///< 活条目的暂存字节（mip 链总和）。
[[nodiscard]] std::size_t   count() const noexcept;  ///< 条目数（已有）。
```

`Entry` 增加 `std::uint64_t bytes`；`Data` 增加 `bytes_total`。`acquireImpl` 删除插入后的
`while (size > kMaxEntries)` 整段。`releaseAbandoned` / `releaseTexture` / `clear` 同步 `bytes_total`。
`makeImage()` 返回 `total`（或新增一个 `textureBytes()` helper，与 `makeMipmapLayout` 共用同一算术）。

### 3.2 `core::MaterialArena`

```cpp
enum class WriteKind : std::uint8_t { Allocated, Rewritten, Unchanged, Refused };

struct Write
{
    WriteKind     kind{WriteKind::Unchanged};
    std::uint32_t slot{0};   ///< Refused 时无意义。
    std::uint32_t copy{0};
    std::uint64_t bytes{0};
};

[[nodiscard]] std::uint32_t capacityNeed() const noexcept;  ///< live + refused_this_frame（扩容请求）。
[[nodiscard]] std::uint64_t refusals() const noexcept;      ///< 因槽满被拒的 note 次数（累计）。
```

删除 `order_` 与 `evictions_`；`index_` 仍保存 identity → 槽。`release()` 只把槽放回 `free_slots_`。
`beginFrame()` 重置 `refused_this_frame_`。
`clear()` 保留计数器（现有语义），但 arena 会被新 storage 整体替换，实际不长驻。

### 3.3 `BlockStorage`

```cpp
struct MaterialWrite
{
    core::MaterialArena::WriteKind kind{...};
    bool          valid{false};  ///< false = 本帧装不下（扩容请求已记下）。
    std::uint64_t offset{0};
    std::uint64_t bytes{0};
};

struct Growth
{
    std::uint32_t views{0};
    std::uint32_t draws{0};
    std::uint32_t lights{0};
    std::uint32_t shadows{0};
    std::uint32_t materials{0};  ///< 需要装下的活材质数（记录前由 store 给出；兜底 = live + 本帧被拒）。
    [[nodiscard]] bool needed() const noexcept { ... || materials != 0; }
};

bool releaseMaterial(const void* material) noexcept;  ///< arena.release() 的转发。
```

`writeMaterial()`：`note()` 返回 `Refused` ⇒ `valid = false`，并把
`growth.materials = max(growth.materials, arena.capacityNeed())`。
`grownLayout()`：`materials` 非零时 `max(need, 2 × capacity)`，其余 region 不动。

### 3.4 释放接线与容量查询

- `ContentStore::materialCount()`：活材质行数（`d->materials.size()`，O(1)，**不含默认材质那条 null 行**），
  记录前扩容的需求来源。
- `ContentStore::releaseAbandoned(timeline, retirement, std::vector<const void*>& released_materials)`：
  每删一个材质行把 identity 推进出参（复用 vector，不每帧分配）。
- `ContentSweep::releaseAbandonedContent` 把出参传给 `VsgBackend`；后者对每个 identity 调
  `storage->releaseMaterial()`。
- `VsgBackend::releaseContent(material)`：`store->releaseMaterial()` 成功后
  `storage->releaseMaterial(material)`。
- 纹理路径不变（`releaseTexture` 已经立即删条目）。

### 3.5 `ContentPass`

`writeMaterial` 返回 `invalid` ⇒ `reportRefused("the command's material", "the material arena is full")`
并 `return false`（与 draw block 预算满时的形状一致）。**不**回退到别的材质槽。

### 3.6 文档

- `RenderDiagnostic.hpp` 的 `ResourcePressure` 注释：现在写的是"纹理被缓存上限挤掉后重建重传"，A6 之后
  不再有这样的路径；改写为"后端本帧撞到自身容量（目前是材质 arena 的兜底拒绝），下一帧扩容"。
- `MaterialImages.hpp` 类注释：上限段落改为"条目跟宿主持有量走；释放是宿主的决定"。
- `MaterialArena.hpp`：删掉 FIFO 段落，写明 `Refused` 与 `capacityNeed()`。
- `BlockStorage.hpp`：`Growth` 增加 materials；`writeMaterial` 的拒绝语义。
- `docs/guides/vsg-backend.md`：资源一节写"材质槽按需增长、纹理驻留由宿主显式释放"。

## 4. 判据

### 4.1 必须红的用例

| 用例 | 钉住什么 | 变异（应当红） |
| --- | --- | --- |
| `MaterialImagesTest.EveryLiveTextureKeepsItsImages` | 300 张全持有 ⇒ `count() == 300`；第二轮 0 缺失 | 恢复任何容量淘汰 |
| `MaterialImagesTest.DroppingTheTextureIsWhatRemovesItsImages` | 丢掉宿主引用 ⇒ 扫尾释放；`count()` 下降、`bytes()` 同步下降 | 扫尾不删条目 / 不更新字节 |
| `MaterialImagesTest.ExplicitReleaseRemovesTheEntryImmediately` | `releaseTexture()` ⇒ 立刻 `has() == false`；再 acquire 重建 | 释放只标记不删 |
| `MaterialImagesTest.TheCacheReportsItsResidency` | `count()` / `bytes()` 等于实际条目与 mip 总和 | 字节数少算一层/一层多算 |
| `CoreMaterialArenaTest.AMaterialThatDoesNotFitIsRefusedNotEvicted` | 容量满时 `note()` 返回 `Refused`，已有材质仍命中（`hits` 增长、`evictions` 不存在） | 仍挤掉最老 |
| `CoreMaterialArenaTest.AReleasedSlotIsReusedByTheNextMaterial` | `release(A)` 后 note D ⇒ D 复用 A 的槽 | 释放不归还 |
| `CoreMaterialArenaTest.TheCapacityNeedCoversEveryRefusedMaterial` | 容量 2、note 5 个 ⇒ `capacityNeed() == 5` | 只记 live+1 |
| `BlockStorageTest.AMaterialRefusalAsksForTheWholeFramesNeed` | `writeMaterial` 拒绝 ⇒ `growthNeeded().materials == 本帧不同材质数` | 不记请求 / 记少 |
| `BlockStorageTest.TheMaterialRegionGrowsWhileTheOthersStay` | `grownLayout` 只动 materials | 顺带放大其它 region |
| `BlockStorageTest.ReleasingAMaterialReturnsItsSlot` | `releaseMaterial()` ⇒ `liveMaterials()` 下降、下一次 note 复用 | 不转发 |
| `VsgBackendTest.ThreeHundredLiveTexturesStayResident` | 设备级：300 张跨帧 ⇒ `count()==300`、第二轮 0 上传、`deviceWaits()` 不变 | 恢复上限 |
| `VsgBackendTest.ThreeHundredLiveMaterialsGrowTheArenaBeforeRecording` | 300 个不同颜色活材质、起步 256 槽：记录前换到 ≥300 槽，**第 1 帧**就全部画成自己的颜色，0 条拒绝诊断 | 不做记录前扩容（会拒画或画错） |
| `VsgBackendTest.ReleasingAMaterialFreesItsArenaSlot` | `releaseContent(material)` ⇒ 槽归还；再 note 新材质不再触发扩容 | 释放不接 arena |

### 4.2 既有用例的改写

- 删除/替换：`MaterialImagesTest.TheOldestEntryIsTrimmedAtCapacity`、
  `MaterialImagesTest.TheBoundDropsTheOldestEntryAndCountsOnlyThat`、
  `VsgBackendTest.AWorkingSetLargerThanTheTextureCacheIsReportedOncePerEpisode`、
  `VsgBackendTest` 里钉 FIFO 的 A/B/X 段与 300 张"每轮全重建"断言。
- `CoreMaterialArenaTest.TheCapacityBoundEvictsTheOldestMaterial` 改为 `...IsRefusedNotEvicted`。
- `BlockStorageTest` 里打印 `materialEvictions()` 的测量配方改为打印 `materialRefusals()` 与 `liveMaterials()`
  （性能账本里“300 个材质 ⇒ 每帧 300 驱逐”那一行随之改为“拒绝/扩容”）。
- `ContentSweepTest`：`releaseAbandoned` 的新出参要覆盖"材质身份被交回"。

### 4.3 收尾判据

- 全量构建 0 error / 0 warning（先 `touch` 目标模块源，避免增量构建藏 warning）。
- `test_vsg` 计数（前 → 后）、`test_graphics` 不变、`test_gui` 不变。
- 设备电池 `skipped == 0`；`vsg_rewrite_gate.sh` 两棵树 `vuid=0 hazard=0`，应用阶段画面行逐字不变。
- 六道静态脚本 0 finding（`check_include_hygiene` / `check_public_docs` / `check_doc_symbols` /
  `check_diagnostic_formats` / `check_ai_docs` / `check_export_annotations`）。
- 变异电池按 §4.1 逐条跑，恢复后复验绿。

## 5. 明确不做

- **LRU / 任何容量淘汰**：没有上限就没有淘汰对象。
- **字节上限 / 增长阈值 / 收缩**：容量是宿主持有量的函数，不是框架预算。
- **全局 `trimContent()`**：A3 的 `releaseContent` 就是显式入口；自动 trim 会把"谁持有"变成框架推断。
- **宿主预算旋钮**：宿主已经能用 `releaseContent` 管理；再加预算就是第二个真相。
- **`SharedStreams` / `VariantPool`**：前者已按"帧命名 + grace"回收（A2 收口），后者是编译变体池，
  与本契约不同；没有测到退化。
- **帧内/帧前预扩容**：见 §8；不改公开协议，但每帧多一次材质集合扫描，先不做。

## 6. 实施切片

| 片 | 内容 | 判据 |
| --- | --- | --- |
| **A6-a** | `MaterialImages`：删上限/淘汰/`kMaxEntries`，加 `bytes()`，改写类注释与既有用例 | §4.1 前四条 + `MaterialImagesTest` 全绿 |
| **A6-b** | `MaterialArena`：删 FIFO，`Refused` + `capacityNeed()`；`BlockStorage`：`MaterialWrite.valid`、`Growth::materials`、`releaseMaterial`；记录前按 `store->materialCount()` 扩容；`ContentPass` 兜底拒绝路径 | §4.1 材质六条；先跑那条"不画错"的像素判据确认 §1.3 |
| **A6-c** | 释放接线（`ContentStore` 出参、`ContentSweep`、`VsgBackend::releaseContent`）+ 文档 + 登记 | §4.1 最后两条 + §4.2 改写 |
| **A6-d** | 文档与登记收口（§7） | §4.3 |

A6-a 与 A6-b 可以独立提交；A6-c 依赖 A6-b 的 `releaseMaterial`。

## 7. 落地后要改的登记

- `graphics-followups-2026-10-19.md` §3.6 的 **A6** 行 → 关闭（写清"不设上限 + 显式释放 + 两处实现"与判据）。
- `vsg-reimplementation.md` §6 的 **A6** 行 → 同上；§11.16cy 的"FIFO 语义"标注"已被 A6 取代"。
- `vsg-reimplementation.md` §11.16bw 的 A6 形状（"淘汰键换成最近 acquire 帧号"）标注作废。
- `.ai/memory/graphics.md` 新增一条（结论在前、判据与坑）。
- `.ai/memory/graphics-perf-backlog.md` 的 A6 引用同步。

## 8. 待拍板

1. **材质容量的口径**：默认按 `ContentStore` 的**活材质数**（O(1)，会为宿主仍持有但本帧不画的材质留槽，
   符合"用多少拿多少 + 显式释放"）。更省内存的口径是"只按本帧画到的不同材质"，需要每帧扫一遍命令 + 一个
   复用哈希表；与"稳态零新增工作"冲突。若你更在意显存，再换成后者。
2. **宿主可见的驻留量**：今天 `count()` / `bytes()` 只在后端内部（测试与诊断可读）。若你想让宿主按
   "用了多少显存"做释放决策，需要把纹理字节/条数放进 `RetentionStats`（公开面）。本设计暂不扩，等真实消费者。
3. **A6-a 与 A6-b 是否同轮**：不同轮也能各自正确；同轮的好处是"两个有界缓存同一份契约"一次收口。

## 9. 落地记录（2026-10-22）

### 9.1 实际形状

- **A6-a（纹理）**：`MaterialImages` 删除 `kMaxEntries`、`stamp`、`clock`、`evictions()`；`Entry` 记录
  `bytes`，`Data` 维护 `bytes_total`，`bytes()` 可读；插入不再驱逐，`releaseAbandoned()` / `releaseTexture()` /
  `clear()` 同步字节。`VsgBackend::sweepAbandonedContent` 不再发 `ResourcePressure` 警告。
- **A6-b（材质）**：`core::MaterialArena` 删除 `order_`/`evictions_`；槽满返回 `WriteKind::Refused`，
  `capacityNeed() = live + 本帧不同被拒材质`；`BlockStorage::MaterialWrite` 加 `valid`，`Growth::materials` 与
  `grownLayout` 的材质扩容；`ContentPass` 拒绝该 draw 并报一次。
- **A6-c（显式释放与记录前扩容）**：`ContentStore::materialCount()`（不含默认 null 行）、
  `releaseAbandoned(..., std::vector<const void*>&)` 交回被弃材质身份、`BlockStorage::releaseMaterial()`；
  `VsgBackend::releaseContent(material)` 与扫尾都归还槽；`growMaterialArenaIfNeeded()` 在 `recordContent()` 前
  按活材质数换 storage。

### 9.2 判据

- 设备无关：`MaterialImagesTest` 17 条、`CoreMaterialArenaTest`（含 `AMaterialThatDoesNotFitIsRefusedNotEvicted` /
  `TheCapacityNeedCountsDistinctRefusedMaterialsPerFrame` / `AReleasedSlotIsReusedByTheNextMaterial`）、
  `BlockStorageTest.AMaterialRefusalAsksForTheWholeFramesNeed` / `TheMaterialRegionGrowsWhileTheOthersStay`、
  `ContentSweepTest` 的材质身份出参。
- 设备级（lavapipe）：`EveryLiveTextureStaysResidentPastTheOldBound`（300 张超过旧上限，第二轮 **0** 缺失）、
  `AWorkingSetLargerThanTheOldTextureBoundStaysResident`（300 张驻留、无压力句子、丢掉后清空）、
  `MeasureWhatCommandsCostWhenTheirDrawingLandsNothing`（200/300/2000 个材质：扩容后稳态 `write/f=0`、
  `refuse/f=0`、`live=200/300/2000`）。
- **两棵树**（Debug `build/` 与 Release `build-release/`）：`test_vsg` **498/498**、`test_graphics` **441/441**；
  六道静态脚本 0 finding；**完整门禁** `scripts/vsg_rewrite_gate.sh build --no-build` 全阶段干净
  （0 VUID / 0 SYNC-HAZARD、应用画面 `87.04%/244`）。

### 9.3 诚实记录

- **第一次门禁的应用阶段红了一次**：`background 5.07% > 5%`（窗口布局/截图的既有敏感判据），重跑同一门禁
  即 `0.02%`、`content 87.04%/preview 244` 与历史逐字相同 ⇒ 按既有规矩判为**环境/截图偶发**，不是本片引入。
- **"材质画错"的设备像素专项已补**：`VsgBackendTest.ThreeHundredLiveMaterialsDrawTheirOwnColours`
  （300 个材质 > 起始 256 槽，20×15 个非重叠小四边形，逐个采样）：第 0 格必须是蓝（旧 FIFO 会把它画成后 44 个
  材质的颜色）、第 299 格必须是红、中间格按 `index/299` 匹配；扩容前后都在同一帧完成，`deviceWaits()` 只算读回。
  单元侧的 `AMaterialThatDoesNotFitIsRefusedNotEvicted`（拒绝后旧材质仍命中）继续钉住底层不变量。
- **容量口径**按 §8 的默认：`ContentStore` 的活材质数（O(1)）。
