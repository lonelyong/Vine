# vk 目标记录的 `fresh` 与「不等待的拒绝检查」数据竞争

- **状态**：已修复（2026-11-17，`gfx_backend_vk`；`test_vk` 169 全绿、TSan 0 报告）
- **发现日期**：2026-11-17
- **模块**：`src/plugins/gfx_backend_vk`（插件内部；没有公开 API 变化）
- **涉及**：`include/vine/backend_vk/internal/TargetObjects.hpp`（`fresh`）、
  `include/vine/backend_vk/internal/TargetTable.hpp` 与 `src/internal/TargetTable.cpp`（`peek`）、
  `src/internal/Device.cpp`（读回相位一/相位二、帧体的读写）、`src/internal/TargetObjects.cpp`（构建/重建/销毁）

> **EN**: Fixed (2026-11-17): `readColorBuffer`'s first phase borrowed a target record across the map lock and read `fresh` and shape fields while the lock-free frame body wrote `fresh = false`, so TSan reported a real race on a plain `bool` (the suite was green, the answer stayed legal, which is why only TSan saw it). `fresh` is now `std::atomic<bool>` with relaxed ordering and a new `TargetTable::peek()` returns built, fresh and shape as one value read under the map lock, so phase one never borrows a record.

## 现象与影响

`VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` 报一条真实竞争：写 `Device.cpp:1733`（帧体
`images->fresh = false`）对读 `Device.cpp:3484`（`readColorBuffer` 相位一的 `peek->fresh`），两个不同线程、无同步。
套件本身 167/167 全绿——普通断言的测试看不见它。

后果是标准意义上的 UB（普通 `bool` 被两线程无锁共享）。实际可见后果被答答案的语义吸收：读到旧值最终仍落在 SDK
允许的 `NotReady` / Ok 里，所以「结果是对的」不能当证据用；证据只能是 TSan（或某天一次编译器优化）。

同一段相位一还借 `lookup` 的 `shared_ptr` 读 `built()` / `colorCount()` / `colorFormat()`。`ensure` 是**原地**
重建（resize 直接改写 `desc`、rebuild 先 `destroyTarget` 再建），`release`/`drain` 则把记录交给 park 在**表外**
销毁——所以这不是某一个字段的疏漏，而是「借记录读字段」这一形状在相位一里整体不成立。

## 根因

三类写入者，两种同步方式：

1. `ensure`（持 map 锁）：build / resize / destroy 原地改写记录；
2. `release` / `drain`（持 map 锁 erase，表外由 park 决定销毁时刻）；
3. **帧体**（不取任何锁——记录路径不许取锁，`C3` / `C5`）：每个 pass 结束时写 `fresh = false`。

相位一在锁外借 `lookup` 的指针读字段：对 1 / 2 没有那把锁，对 3 也没有原子。相位二（体闸 EXCLUSIVE 之内）本来
就是安全的——闸把帧体排空——所以「等一会儿再查一遍」保住了**结果**，却没有保住**读取**本身。这正是缺陷容易被漏掉
的原因：它只活在两处检查之间的那一段。

## 修复

1. `TargetObjects::fresh` 改成 `std::atomic<bool>`（relaxed）。理由：它是唯一由帧体在记录路径上写的字段，那一侧
   不能取锁；relaxed 够用，因为相位一的结论在闸内重查，而两处真正的 happens-before 由既有的交接给出——claim 锁
   （本帧写 → 下一持有者读）与体闸（帧体写 → 宿主动词读）。
2. 新增 `TargetTable::peek()`：在 map 锁内把 `built` / `fresh` / **形状**读成一份值（`TargetTable::Peek`），
   相位一只读这份值。锁是每个写入者都取的那把，所以快照永远是一个完整状态（要么重建前、要么重建后），记录不再
   跨表借用。
3. 相位一的深度格式改用**已建成形状**的格式，而不是宿主对象当下的声明——提示必须描述真会被拷贝的那张图；否则
   「宿主改了声明、帧还没来」时相位一可能给出永久的 `Unsupported`。
4. 相位二与帧体的每个读取点都显式 `load(std::memory_order_relaxed)`：同一个对象不能一半原子、一半非原子。

## 验证

- `VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` → **169/169、0 ThreadSanitizer warning**（修复前同一命令
  1 条真实竞争）。
- `test_vk` **169**（167 + 2 新例）、`test_graphics` **617**、`test_vsg` **572**、`BackendParityTest` **13**。
- 新用例 `VkMultiSessionTest.AReadbackRacingARebuiltTargetAnswersAValueItCanStandBehind`：读回线程与「每帧
  resize 一次」的驱动线程并发，答案只能是 `Ok` 或 `NotReady`，`Ok` 时必须是一帧画完的整幅图（尺寸属于两个被画过的
  尺寸之一、像素是 clear 色）。新用例 `TargetTableTest.APeekAnswersAnUnbuiltTargetAndInsertsNothing`：`peek`
  对未知目标答「未建」且**不插条目**。
- 九道门禁全绿（`check_ai_docs --strict` / `check_design_docs` / `check_doc_symbols` /
  `check_include_hygiene` / `check_api_style` / `check_diagnostic_formats` / `check_public_docs` /
  `check_export_annotations` / `check_vk_exposure`）。

## 设计约束

- **G10**：共享可变状态要算清所有权与同步；一个 `bool` 的代价就是一次 UB。
- **G11**：失败要可定位——相位一现在只有一个入口、一把锁，读的是哪份状态可以一眼说出。
- **G4**：`lookup`（借记录，有寿命）与 `peek`（取值，无寿命）用两个拼写区分两件事，调用方不必猜。

## 相邻问题（本次未修，已记入 backlog）

`executeFrame` / `prepareFrame` 在 `ensure` 之后立刻 `*target_table.lookup(target)`（`Device.cpp:1343`、
`Device.cpp:2056`）。`releaseRenderTarget` 按 R6 **不等在飞的帧**，可以在这两步之间 erase 条目，第二次 `lookup`
返回 null ⇒ 解引用空指针。修法：让 `ensure` 交出记录本身，或给第二次 `lookup` 判空并按 claim 冲突跳过该 pass。
