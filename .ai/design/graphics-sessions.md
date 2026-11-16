# 设备是池、会话拥有帧：类型、所有权、并发规则与迁移

> **状态**：实施中（`T47` 已落地；`T48` 进行中——S1 / S2a / S2b / S3 / S4 / S5 / S6a / S6b /
> S6c-1 / S6c-2a / S6c-2b / S7 已落地，S8–S9 未做）
> **最后核对**：2026-11-16
> **范围**：一个 `RenderDevice` 被多个引擎借用时，"共享什么、谁拥有帧、怎么并发、怎么退休、怎么迁移"
> 的**实施形状**；契约的**为什么**在 ADR，线程模型的**现在**在 `graphics-threading.md`。
> **权威来源**：`RenderDevice.hpp` / `RenderFrameExecutor.hpp` / `RenderBackend.hpp`（契约）、
> `backend/FrameGate.hpp` / `backend/DeviceClock.hpp` / `backend/RetirementQueue.hpp`（机制）、
> `src/plugins/gfx_backend_vk/src/internal/Device.cpp` 与 `ContentDraw.cpp`（今天的设备级帧机器）、
> `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`（已经是"设备 + 每窗口"的那一半）。
> **待办**：`../backlog/graphics.md` 的 `T48`

## 1. 概要设计

### 1.1 问题与背景

需求（用户原话）：**geometry / shader / 纹理资源共享，帧不共享；共享之后要线程安全；两个 engine 可以同时 frame。**

`ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` 已经定下**契约与形状**（设备是池、会话拥有帧、
驻留三态、目标 claim、跨会话退休、三把锁），并拆成两步：第一步 `T47`（设备级帧闸，让"一次一帧"从
文档变成**检查**）已落地；第二步 `T48`（让并发真正成立）约 1350–2100 行、12–18 个文件。

**ADR 定的是"应该是什么"，本文定的是"怎么长成它"**：新增哪些类型、谁拥有谁、每帧在哪一步取锁、
失败时状态往哪里退、以及**怎样分阶段落地而每一阶段都绿**。本文不重复 ADR 的论证（选型、业界对比、
Vulkan 外部同步表），只引用它；也不重复 `graphics-threading.md` 的 `C21`（今天的契约）。

### 1.2 目标

1. **两个会话同时驱动一个设备**：内容一次上传、一次编译；两帧并行记录、各自提交。
2. **记录路径零锁**：稳态一帧不取任何设备级锁（`G10` 的账，见 §2.3）。
3. **每个失败有唯一的归属**：设备丢失、分配失败、目标冲突、上传竞争、释放与在飞重叠——五类各有
   一句契约、一个状态、一条报告，不需要宿主猜时序（`G4` / `G11`）。
4. **迁移可分阶段验收**：每一阶段结束时，现有 `test_graphics` / `test_vk` / `test_vsg` 全绿，且该阶段
   新增的不变量有**能红的用例**（§8）。

### 1.3 非目标

- **不做"一个目标被两个会话同时画"**：一张图像在一个时刻只有一个 layout，这不是锁能解决的（`G4`：
  说"不"比说"也许"好）。冲突时**只跳过那个 pass**，见 §2.5。
- **不做全局设备锁**（`D1`）、**不做 per-entry 读写锁**（`D2`）——ADR 已否决。
- **不做多帧在飞**（`ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`）：那是 `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` 的话题。本文只保证
  退休机制**与它共用**（§2.6），先做哪个都不返工。
- **不把"一个窗口一个目标"改成可共享**：平台约束（`createSurfaceTarget` 的 `WindowOccupied`）。
- **不改 appfw 的用法**：`SurfaceWindow` 今天每窗口造一个设备；共享设备在那里还没有消费者（`G8`）。

### 1.4 一句话结论

**会话是"帧驱动者"（不是窗口），它拥有帧机器与帧暂存；设备拥有 GPU、内容缓存与目标，并把缓存
在帧首一次性解析成会话本地的表**——于是并发不需要在记录路径上协调，只需要在"帧首一次"和"驻留
miss"两处协调。

## 2. 设计要点

### 2.1 会话是"帧驱动者"，不是"窗口"（`G1`）

- **是什么**：`Session` = 谁在驱动帧：命令池、命令缓冲、栅栏、常量环、描述符池、帧账、它 claim 的目标。
  一个会话**至多**present 到一个窗口（窗口是它的目标，不是它的身份）。
- **为什么需要点名**：`gfx_backend_vsg` 今天的 `api::Session` 是**一个窗口一个设备一个交换链**
  （`Session.hpp:13`），而**收集/编译/执行**这条帧管线在 `VsgBackend::Data` 里是**设备级单例**
  （`recorder` / `compiler` / `arena` / `facts` / `packets` / `frame` / `driving`，见
  `VsgBackend.cpp:444`–`534`）。也就是说：**vsg 的"会话"是窗口会话，不是帧驱动者**——ADR 说"vsg 更接近"
  只对了一半。这条更正决定了 §9.3 的迁移表：vsg 要搬的是**收集暂存**，不是交换链。
- **边界**：窗口仍然是"一个平台表面一个活交换链"；会话可以没有窗口（离屏），此时它 present 到无。

### 2.2 共享面 = 不可变；非共享面 = 有 layout 或有帧内写入（`G1` / `G4`）

| 归类 | 判据（一句话可判） | 例子（vk） |
|---|---|---|
| 可共享 | 键含 revision，且发布后**不再改写** | 程序模块、管线、几何流、纹理、采样器、布局 |
| 不可共享 | 在一个帧的生命周期内**被写** | 命令池、栅栏、常量环、描述符池、目标附件、交换链 |

不可共享的东西一律**按会话各一份**，所以它们不需要锁：所有权把并发变成了"两条线程碰不到同一块内存"。

### 2.3 帧首快照：缓存只在帧首读一次，记录路径零锁（`G10`）

- **是什么**：一帧开始时，会话持设备缓存的**独占锁一次**，把这一帧**将要用的身份**（帧值本来就列出了
  它们：每个 `RenderCommand` 的 geometry / program / material 描述、每个 pass 的目标）解析成
  **会话本地的表**（`FrameSnapshot`：身份 → 句柄 + 状态）。记录时只读会话自己的表，**不再碰设备缓存**。
- **为什么不能"每次查表加读锁"**：一帧 2000 draw，`std::shared_mutex` 一次读锁按 20–40 ns 算是
  40–80 µs，占本机宿主侧帧时间（~0.9 ms）的 5–9%，而这笔钱买到的只是"读一个没人写的表"——`G10` 的
  典型浪费，也是 `D2` 被否决的同一个理由。
- **为什么快照不贵**：快照的开销是 O(本帧身份数)，不是 O(缓存)；稳态下它只做哈希查找，没有分配
  （表按上一次的大小预留）。
- **为什么快照取的是独占锁而不是读锁**：快照**会写**（miss 时要发布 `Uploading`），而
  `std::shared_mutex` 不能升级；"先读锁扫一遍、放掉、再独占锁扫一遍"是两次遍历两把锁两个
  竞争窗口（`G3`：一趟能说清的流程不拆两趟）。每帧一次独占锁的代价是两个会话的快照串行 ~50 ns，
  占帧预算的十万分之一；这个数字比"读锁状态机"宾服得多。
- **边界：miss 仍要锁。** 快照里状态为 `Absent` 的条目要在帧内建：建**必须在独占锁下**把条目以
  `Uploading(owner, token)` 发布出去（§2.4），并把句柄记回会话本地的表。所以"零锁"的准确说法是：
  **记录路径零锁；帧首一次**。后者是本节开头写的那一次，前者是它的结果——两个数字要分开说，别把它们並成
  “稳态帧 0 次”（那是记录路径的说法，不是整帧的说法）。
- **已落地（S6c-1）的实测形状**：`ContentCaches` 的一把锁在**帧首**取一次，`planCommand` / `planScreen` /
  `resolveTextures` 把结果**按值**交给会话（`CommandPlan` / `ResolvedTextures`），纹理上传也在帧首录进帧命令缓冲；
  `record` / `recordScreen` 只读这些值。一整帧的获取数固定为 **4**（帧首 1 + 帧首/帧尾数簿各 1 + `endFrame` 1），
  **不随 draw 数增长**（`VkFrameHeadTest`：1 条 vs 6 条命令同数；变异成逐 draw 取锁后 6 → 10，红）。
- **记录也按驱动者归属（S6c-2a）**：纹理上传的 staging 与被替换的图像按 `ContentImages::BatchId`（= `clock_window + 1`）
  分组，帧尾只释放**自己**那一批；被替换的图像本身 park 到水位——另一个驱动者的在飞帧可能还拿着旧 view。
- **管线是例外，但它本来就在 miss 上**：管线键含状态（拓扑 / 混合 / 深度），状态是每命令解析的，
  帧首算不出来——但管线表只在**创建**时写，创建本身就是 ms 级 miss，取一次独占锁不影响结论。

### 2.4 驻留三态：发布或撤回，竞争即复制（`G3` / `G4`）

```
Absent ──快照 miss，持锁发布──▶ Uploading(owner, token)
                                   │  owner 的帧完成（今天：executeFrame 返回已 wait 过栅栏）
                                   ├──▶ Resident          （发布）
                                   └──▶ Absent            （撤回：帧失败/取消，见下）
Resident ──releaseContent──▶ Retiring(epoch) ──设备水位 ≥ epoch──▶ 释放
```

三条规则（缺一条都不成立）：

1. **一个键只有一个发布者**：发布用 compare-and-set（`Absent → Uploading(me)`），输的一方**不发布**。
2. **`Resident` 只在填充可证明完成之后发布**：今天是"这一帧的栅栏已经 wait 过"；**预热（`prepareFrame`）的发布点就是
   它那次只含传输的提交返回**（`ADR-2026-11-14-prepare-may-submit-transfers.md`）；多帧在飞之后是"退休队列说
   那一帧完成"——同一个信号（§2.6）。
3. **发布或撤回，二者必居其一**：owner 在帧尾用作用域守卫把本帧建的条目**全部**置为 `Resident` 或
   全部置回 `Absent`。没有第三条路——挂在 `Uploading` 的条目会让**所有**会话永远 miss（`P1`）。

竞争（另一个会话在快照里看到 `Uploading`）：**建一份不发布的临时副本**，本次帧用完即弃。
`P2` 说明为什么不是"等"：等待是唯一能把两个会话**互相锁死**的形状；复制是有界的（每个身份至多
`N−1` 份，且只在"两个引擎第一次同时画它"的那一帧）。`prepare()` 在加载期就把它做掉了，真实宿主通常
一次都碰不到。

### 2.5 目标 claim：一帧一 claim，冲突只跳过那个 pass（`G6`）

- **是什么**：设备持一张 `TargetClaims`（`RenderTarget*` → `SessionId`）。帧首对**本帧要画的目标集合**
  逐个 try-claim；冲突的目标**不 claim**，用它的那个 pass **跳过**并报一条（按 `(目标, episode)` 去重）。
- **为什么按 pass 而不是按帧**：一帧里其余的 pass 与冲突目标无关，整帧失败会放大一次局部冲突
  （`G7`：把复杂性吸收在局部）。
- **为什么不是新失败形态**：既有语义里已经有"这个设备建不起这个目标 ⇒ 报一次并跳过它的 pass"；claim
  冲突走同一条报告路径、同一个 category（`G6`）。
- **边界**：窗口目标不需要 claim——一个窗口只属于一个会话（`WindowOccupied` / foreign target 已在）。
  claim 在**帧尾全部释放**；注意力放在 `P3`：任何提前返回都必须经过同一个释放点。
- **重建与 claim 的关系**：目标的重建（resize / repair）**只能由持有 claim 的会话做**；另一个会话看到
  的是"这一帧跳过"，而不是"重建到一半的图像"。

### 2.6 跨会话退休：复用 `DeviceClock` + `RetirementQueue`，不新造（`G6` / `G9`）

- **是什么**：vk 目前"`executeFrame` 内部 wait ⇒ 下一帧之前一切都可以立即销毁"，所以 `releaseContent`
  只能在帧之间调用。两个会话之后"帧之间"不再是一个时刻，需要**设备水位**。
- **复用哪个**：`backend/DeviceClock.hpp` 已经是"每个窗口报自己的完成证据，设备水位取**最小值**"，
  `backend/RetirementQueue.hpp` 已经是"park + 到点释放"，且两者 **vsg 已经在用**（`d->clock` /
  `d->retirement`，由每个会话借用）。vk 采用它们 = 让两个后端共用同一套完成证据（`G6`），而不是在 vk
  里再造一个计数器（`G7`）。
- **谁是谁**：**一个会话就是一个 clock window**（`DeviceClock::WindowId` 记的就是一个帧驱动者；离屏会话
  也注册一个）。`SessionId` 就是它的类型别名；今天叫 `WindowId` 是历史，重命名推迟到 §9.2（`D7`）。
- **释放的语义**：`releaseContent` / `releaseRenderTarget` 记下**当时每个会话的 `submitted`**，设备水位
  越过它才真正销毁；因此这两个动词**可以在别的会话正在 frame 时调用**。这条路 `RenderDevice.hpp:302`-
  `308` 的契约文本**已经写好了**（"a device that retires asynchronously parks the free until every session
  is past it"）——本文只是把 vk 变成那个设备。
- **线程**：`DeviceClock` 与 `RetirementQueue` 今天都写着"NOT thread-safe：设备由一条线程驱动"。两个会话
  之后它们会被两条线程碰，所以它们各自要**一把小锁**（只在帧首/帧尾各取一次，`P4`）。- **已落地（S6c-2a）的取舍**：小锁放在**设备**（`Impl::clock_mutex`），不放进两个 SDK 值类型：参考后端（vsg）是单线程，
  给它加锁是没有消费者的机制（`G8`）；两个类型的契约改成“调用方负责串行，多线程设备自己提供锁”。所有拿
  `clock.timeline()` 引用的地方都在锁内，引用不离开临界区。
- **多开帧（S6c-2a）**：`FrameTimeline` 从“一个开着的帧槽”改成“每个驱动者一个”：`begin()` 每次铸造新号，
  `submitted`/`abandoned` 按 token 关闭。否则第二个驱动者会拿到第一个的 token，两帧在水位比较里塌成一帧。
### 2.7 读回是设备的动词，不是会话的（`G4` / `G6`）

- **问题**：今天读回借的是帧的 `command_pool` + `completion` 栅栏（ADR "挡路的事实"第 2 条）。会话化之后
  它不能借任何一个会话的——那会让读回和那个会话的帧互相等。
- **做法**：读回有**自己的**池与栅栏（设备级、一把锁护住"同一时刻一次读回"），并在拷贝前**等设备水位**
  （每个会话 `submitted` 都完成）。这正是 vsg 今天做的（`readColorBuffer` 里对**每个窗口**
  `waitDeviceIdle`，并把它计入 `deviceWaits()`），vk 走同一条路（`G6`）。
- **为什么诚实**：读回读的是"已经完成的东西"，不是"某个会话刚提交的东西"；等水位是把这句话变成机制，
  而不是把"一帧在飞"当成隐含前提。

### 2.8 会话的接口形状：借出一个执行器，设备拥有它（`G4` / `G8`）

```cpp
// RenderDevice
virtual RenderFrameExecutor* createSession();                 // 默认：return &executor();（一个设备一个会话）
virtual void                 releaseSession(RenderFrameExecutor& session);  // 默认：no-op
// RenderFrameExecutor（新增一个可选项，不是要求）
virtual void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program);  // 默认：no-op
```

- **默认实现就是"今天"**：`createSession()` 默认返回 `&executor()`，所以**已有后端与所有用例一字不改**；
  只有 vk 与 vsg 覆写它。这让"并发会话"成为一个**接缝**而不是一次全仓改造（`G9`），也不给第三个后端
  强加它今天不需要的形状（`G8`）。
- **所有权**：**设备拥有会话**；`createSession()` 借出一个引用，契约是"直到 `releaseSession` 或设备
  `shutdown()`"。引擎在 `setDevice` 时创建、在换设备/析构时释放。`shutdown()` 之后引擎再 frame，先问
  `initialized()`（已经如此）并报"设备没起来"，**不解引用**那个引用——`RenderDevice` 已有的
  "the creator guarantees no frame in flight" 是同一条时序保证（`P5`）。
- **默认内容程序归会话，且它的声明要**从 `RenderDevice` 移到 `RenderFrameExecutor`**。程序是引擎给的
  （`RenderEngine` 的 `forwardProgram()`），它着色的是**帧**，解析发生在记录时——所以它是会话的设置。
  vsg 早就这么建模了（`FrameRecorder::setDefaultContentProgram` 是**记录器**的，设备级字段只是它喂给
  记录器的来源）。**移声明的第二个理由是实现层的**：`RenderBackend` 同时继承 `RenderDevice` 与
  `RenderFrameExecutor`，两处同名同签名的 virtual 会让 `backend->setDefaultContentProgram(...)` 变成
  二义调用（`G4`：接口不能要求调用方写限定名）。
  `Device` 覆写它并转发给**设备自己的会话**，所以直接驱动设备的调用方（`test_vk`）行为不变；
  `RenderEngine` 在 `setDevice` 里对**自己的会话**宣布。
- **窗口**：`createSurfaceTarget` 留在设备上（一个平台表面不是会话的财产）；**第一个** claim 它的会话成为
  它的主人，第二个会话拿到现有的"foreign target"拒绝。不需要新参数。
- **S6a 已落地（2026-11-15）**：上面三个声明都已进树。"暂时仍是一个驱动者"是**有意的**：vk 的 `createSession()`
  在接缝落地时仍答 `&executor()`，因为缓存/分配/提交的锁（S6b）到位之前，第二个驱动者会让两个引擎竞争未保护的状态。
  接缝先行的价值是：引擎的所有权、声明的位置、以及"谁来释放"的形状都已在树上被用例钉住，S6b 只换引擎内部的实现。

### 2.9 两个后端的落地差异（`G5` / `G10`）

| | vk（`gfx_backend_vk`） | vsg（`gfx_backend_vsg`） |
|---|---|---|
| 帧机器今天在哪 | 全在 `Device::Impl`：`command_pool` / `completion` / 时间戳池 / `Books`（`Device.cpp:445`–`655`） | 命令图/提交**已经在每个窗口**（`api::Session` 持 `vsg::Viewer`）；**收集暂存**在设备（`recorder` / `compiler` / `arena` / `facts` / `packets` / `frame` / `driving`） |
| 帧暂存在哪 | **已分家**（S3）：`ContentCaches`（设备）持 programs / geometries / pipelines / 布局 / 图像；`FrameScratch`（会话）持环 / 描述符池 / set 表 / `frame_index`；`ContentDraw` 只剩“把一条 draw 变成命令”的记录器 | `BlockStorage`（每帧 slab 轮转）+ `ContentAssembly` 的 `beginFrame` 清扫 |
| 要搬什么 | 拆 `ContentDraw` 为 `ContentCaches`（设备）+ `FrameScratch`（会话） | 把收集暂存搬进会话；缓存加锁/快照 |
| 完成证据 | **没有**——需要采用 `DeviceClock` + `RetirementQueue` | **已有**（`d->clock` / `d->retirement`，由会话借用） |
| 量级 | 大（约 900–1400 行） | 小（约 300–500 行） |

**顺序**：先 vk（它是"设备 = 池"缺得最多的那个，也是 `test_vk` 的 102 例覆盖最厚的），再 vsg
（它拿到的是一条已在树上验证过的 SDK 形状 + 它自己的完成证据）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 会话类型与创建 | 设备拥有会话；`createSession()` 借出，默认答 `&executor()` | **已落地（S6a+S6b）**：SDK 默认接缝 + 引擎持有；vk 真的多发驱动者（各自命令池/栅栏/暂存/默认程序），**帧体真并行**（S6c-2a），窗口帧独占体闸 |
| F2 | 每会话帧机器 | 命令池 / 栅栏 / 时间戳池 / 帧账 / 帧闸按会话各一份 | **已落地（S2a+S2b）**：`Device::Session` 拥有帧机器与帧体，`Device` 只转发 |
| F3 | 设备缓存 + 帧首快照 | 缓存只在帧首读一次，记录路径零锁 | **已落地（S3 + S6c-1）**：`ContentCaches` / `FrameScratch` 分家；计划按值；观察点 `contentCacheCounters().cache_lock_acquisitions`（整帧固定 4 次） |
| F4 | 驻留三态 | `Absent → Uploading → Resident`，发布或撤回，竞争即复制 | **部分（S4）**：状态机 + 8 条单元用例已落地；**设备接线仍未做**（见 §2.17、§9.2）——同一内容的并发首次构建仍在缓存锁上串行 |
| F5 | 目标 claim | 一帧一 claim，冲突只跳过那个 pass | **已落地（S5 + S6b）**：`TargetClaims` 租约 + 帧/预热接线；公开 `createSession()` 之后两会话冲突真的会发生（只跳该 pass，报 `vk.target.claim_conflict`） |
| F6 | 跨会话退休 | vk 采用 `DeviceClock` + `RetirementQueue`；释放可在别人 frame 时调用 | **已落地（S1 + S6c-2b）**：时钟 / park 队列 / 水位 / 开帧规则都在树上；释放不等帧（R6） |
| F7 | 设备级读回 | 自己的池与栅栏，拷贝前等设备水位，计入 `deviceWaits()` | **已落地（S7）**：`ReadbackStaging` 自带命令池与栅栏；等待 = 体闸的"帧之间"，每次读回计一次 `deviceWaits()`；帧体内调用答 `Failed`（见 §3.1.7） |
| F8 | 默认内容程序归会话 | 会话上可设；设备级只服务直接调用方；单会话后端行为不变 | **已落地（S6a+S6b）**：声明在 `RenderFrameExecutor`；vk 的每个 `Session` 各持一份，两驱动者各画各的（T8 用例） |
| F9 | vsg 的对应落地 | 收集暂存搬进会话；缓存加锁/快照；完成证据已有 | 部分（窗口会话已有） |
| F10 | 契约与文档收口 | `C21` / `N18` 改写、三处头文件文本、ADR 状态、审计行 | 部分（T47 改了三处文本） |

### 3.1.1 F1 会话类型与创建

- **行为**：`RenderDevice::createSession()` 默认 `return &executor();`；vk / vsg 覆写为"造一个会话"。
  `releaseSession(RenderFrameExecutor&)` 默认 no-op。`RenderEngine` 持 `RenderFrameExecutor*`，
  `setDevice` 创建、换设备/析构释放。
- **约束**（C1）：会话由**设备**拥有；引擎只借。
- **要求**（R1）：`createSession()` 可以在 `initialize()` 之前调用（设备由创建者 initialize，引擎先 attach）。
- **验收**（T1）：单会话后端（默认实现）跑完 `test_graphics`，行为零变化。
- **注意**（N1）：`shutdown()` 不给活着的会话留悬垂——见 `P5`。

### 3.1.2 F2 每会话帧机器

- **行为**：`Session` 持 `command_pool` / `completion` 栅栏 / 时间戳池 / `Books` / `FrameGate` / 自己那一组
  `ReportOnce`；`Device::executeFrame` 变成"转发给目标会话"。
- **约束**（C2）：帧机器**只**在会话里；设备不再有第二个 `command_pool`。
- **要求**（R2）：`executor()` 保留，含义收窄为"设备自己的那个会话"（`G6`，与 `setDiagnosticsRoute` 同形）。
- **验收**（T2）：`test_vk` 102 例在"只有一个会话"的树上全绿（纯搬家，行为零变化）。
- **注意**（N2）：`PrepareReport` / `FrameReport` 都是会话的答案，设备级的那个答自己的会话。

### 3.1.3 F3 设备缓存 + 帧首快照

- **行为**：`ContentCaches`（设备）持 programs / geometries / pipelines / 布局 / 图像缓存 / 分配器；
  `FrameScratch`（会话）持环、描述符池、set 表、逐帧计数与 `FrameSnapshot`。
- **约束**（C3）：记录路径**不得**取设备缓存的锁；一切设备缓存读发生在帧首。
- **要求**（R3）：稳态帧的设备锁获取次数为 **0**（可观测：一个计数器 + 一条用例）。
- **验收**（T3）：2000 draw 的一帧，锁计数为 0；有 miss 的帧锁计数 ≤ miss 数 + 1。
- **注意**（P6）：快照漏掉一个身份 = 该 draw 帧内 miss，**正确性不受影响**，只是多取一次锁。

### 3.1.4 F4 驻留三态

- **行为**：条目状态 `Absent` / `Uploading(owner, token)` / `Resident` / `Retiring(epoch)`；发布用
  compare-and-set；竞争会话建临时副本；owner 帧尾**发布或撤回**。
- **约束**（C4）：只有 `Resident` 可被非 owner 使用；`Uploading` 的条目对别人等于 `Absent` + 复制。
- **要求**（R4）：条目不可变（键含 revision）；任何 `Uploading` 不得长期存在。
- **验收**（T4）：两会话同时首画同一几何 ⇒ 不崩、两张图都对、上传次数 ≤ 2；变异（去掉撤回）⇒ 用例红。
- **注意**（P1）：卡在 `Uploading` 是**全设备**的疤，不是一条 draw 的失败。

**S4 落地（2026-11-15）**：状态机是 `internal/ResidenceTable.hpp`（无 Vulkan 类型），一条键一条条目：
`admit(key, session)` 答 `Use` / `Build`（带 move-only 的 `Claim`）/ `Copy(owner)`；`Claim::publish()` 是 owner
“填充已完成”的声明，`Claim` 析构时未决议的条目退回 `Absent`（规则 3 由类型强制，不靠调用点记得）；
token 单调递增且不复用，所以 `forget`（`releaseContent`）之后的老 claim 不可能把条目复活。
**未接线（截至 2026-11-16）**：`ContentCaches::ensureGeometry` 仍走原来的路径——S6 引入会话 id 之后仍未把 `admit`
接进去（半接的状态机比没接更糟，`D8`；状态机自身的验收不依赖设备）。今天的后果是：两个驱动者首次同时构建同一份
内容时在**缓存锁上串行**（不是等待对方的帧）。触发接线：真实宿主"一边编辑一边烘焙"让 miss 进入帧预算。

### 3.1.5 F5 目标 claim

- **行为**：帧首 claim 本帧目标；冲突 ⇒ 跳过那个 pass 并报一条；帧尾全部释放。
- **约束**（C5）：一个目标同一时刻只有一个会话；claim 是**租约**，不是锁。
- **要求**（R5）：一个目标的重建只由 claim 持有者做。
- **验收**（T5）：两会话画同一目标 ⇒ 一个会话那个 pass 被跳过并报一条（且只一条）。
- **注意**（P3）：任何提前返回都要经过同一个释放点（作用域守卫）。

**S5 落地（2026-11-15）**：`internal/TargetClaims.hpp`——设备持一张 `RenderTarget* → SessionId` 表；
`begin(session)` 发一个帧作用域的 `Lease`，`Lease::claim(target)` 冲突时答 false，析构把本帧所有 claim 还回去
（`P3` 由类型强制）。`Session::executeFrame` 与 `prepareFrame` 都在 **`ensure_target` 之前** claim（重建是持有者的
动词，`R5`）；冲突只跳过那个 pass，报 `vk.target.claim_conflict` **每个（目标, 情节）一条**：情节在本帧/本次预热
无冲突地跑完时结束。窗口目标不 claim（`createSurfaceTarget` 已经保证一窗一会话）。
**验证**：`TargetClaimsTest` 7 例 + `TargetClaimFrameTest` 2 例（设备级：用一条冒充“另一个驱动者”的租约让 pass 被
跳过、两次只报一条、干净帧后新冲突再报）；变异 2/2 红（租约不释放 / 情节不结束）。

### 3.1.6 F6 跨会话退休

- **行为**：vk 采用 `DeviceClock`（会话 = clock window）+ `RetirementQueue`（设备级）；释放记 epoch，
  水位越过才销毁。
- **约束**（C6）：销毁只能发生在设备水位之后；不得用 `vkDeviceWaitIdle` 换正确性（`G10`：那是把代价
  从设计挪到每帧）。
- **要求**（R6）：`releaseContent` 在另一个会话 frame 时调用**安全且不阻塞到那一帧结束**（只 park）。
- **验收**（T6）：释放与在飞帧重叠 ⇒ 拷贝完成前不销毁（计数器 + 一条用例）；`deviceWaits()` 不涨。
- **注意**（P4）：`DeviceClock` / `RetirementQueue` 要各加一把小锁，取用点只在帧首/帧尾。

### 3.1.7 F7 设备级读回

- **行为**：读回用自己的池与栅栏；拷贝前等设备水位；等一次记一次 `deviceWaits()`。
- **约束**（C7）：读回不得借任何会话的帧机器。
- **要求**（R7）：读回在两会话各跑若干帧之后，读到的是**最新已完成**的那一帧（与今天单会话语义一致）。
- **验收**（T7）：两会话交替画不同目标、穿插读回 ⇒ 每张图都是它自己最后一次完成的帧。
- **注意**（P7）：读回是"最慢的动词"，别把它放进帧预算；它的等待要可观测。

**S7 落地（2026-11-16）**：

- **自己的机器**：`ReadbackStaging` 现在拥有**自己的命令池与栅栏**（一次拷贝一套；创建/销毁对着一次 GPU 往返是
  噪声，`G10`），读回路径不再碰 `Session` 的命令池与栅栏（C7 结构性成立）。
- **等待是设备的"帧之间"**：以体闸 EXCLUSIVE 表达——它在墙上仍等**在飞的帧**（这就是完成证据），但那不是某个
  会话的机器；**每次读回计一次** `deviceWaits()`，与参考后端的 `waitDeviceIdle` 同一个读数（`G6`）。
- **拒绝不停止设备**：能在等待前回答的拒绝（未知/未建目标、越界附件、不支持的格式、借用深度）在相位一回答；
  相位二在闸内**重新解析**，因为等闸期间帧可能重建或替换了目标。
- **帧体内的调用**：诊断 sink / hook 在帧体里回调本设备时，读回答 `Failed` 而不是等自己——`FrameBodyDepth`
  （线程内帧体深度）把这类死锁变成可回答的拒绝；`Device.hpp` 写明了这条契约。
- **顺带修掉的外部同步缺口**：`vkQueueSubmit` / `vkDeviceWaitIdle` 现在过设备的 `queue_submit` 锁（ADR 的硬
  约束表早就写了"提交锁"，S6c-2a 的代码里没有——两驱动者同时提交是 Vulkan 的外部同步违规）。
- **证据**：`VkMultiSessionTest` 新增 3 例——两个驱动者交替画、穿插读回各自都是自己的最后一帧（T7）；读回在
  帧被 sink 停住时必须**等待**，结束读到该帧像素，计数 +1；帧体内读回答 `Failed` 且不碰宿主 vector。变异：
  体闸改回 SHARED ⇒ 等待用例红；去掉计数 ⇒ 计数断言红。

### 3.1.8 F8 默认内容程序归会话

- **行为**：`setDefaultContentProgram` 的声明从 `RenderDevice` 移到 `RenderFrameExecutor`（默认 no-op；`Device` 覆写它并转发给设备自己的会话），
  引擎在 `setDevice` 里对**自己的会话**宣布；设备级的那个只服务"直接驱动设备"的调用方。
- **约束**（C8）：两个引擎声明不同默认程序时，**各画各的**（不再"后声明者生效"）。
- **要求**（R8）：单会话后端行为与今天逐字节相同。
- **验收**（T8）：两会话声明不同默认程序 ⇒ 两帧的像素各是各自的程序；变异（共用设备级单槽）⇒ 红。
- **注意**（N3）：这条把 `graphics-threading.md` 的 `N18`（设备级单槽）替换掉，见 F10。

### 3.1.9 F9 vsg 的对应落地

- **行为**：`recorder` / `compiler` / `arena` / `facts` / `light_scratch` / `input_*` / `packets` / `frame` /
  `driving` 从 `VsgBackend::Data` 搬进会话；设备缓存（`store` / `halves` / `images` / `uploads` / `targets` /
  `passes` / `pool` / `storage`）按 F3 加锁 + 快照。
- **约束**（C9）：`api::Session` 的**窗口**职责不动（交换链、`Viewer`、`WindowTarget` 已经是每窗口）。
- **要求**（R9）：`BackendParityTest` 的逐字节对拍在两会话下仍成立。
- **验收**（T9）：`test_vsg` 全绿 + 两会话用例；对拍不变。
- **注意**（P8）：`d->driving` 是"哪张图属于哪个会话"的**唯一**记录，搬走后不能再有第二份。

### 3.1.10 F10 契约与文档收口

- **行为**：`RenderDevice.hpp` / `RenderFrameExecutor.hpp` / 插件设备头的线程段改写；`C21` / `N18` /
  `R18` 改写成"两会话可并发"；ADR 标明第二步已落地；审计 `A6` / `A7` / `A11` 逐条勾掉。
- **约束**（C10）：契约文本必须与**代码的实际保证**一致（`G4`）；做不到就先写"串行"。
- **要求**（R10）：`check_ai_docs.py --strict` 与 `check_design_docs.py` 0 finding。
- **验收**（T10）：两道文档门禁 0 finding。
- **注意**（N4）：`D13`（全局锁）与 `D2`（per-entry 锁）在文档里保持否决，别让"临时实现"复活它们。

## 4. 公开接口

### 4.1 `RenderDevice`（SDK）

| 接口 | 契约 | 备注 |
|---|---|---|
| `RenderFrameExecutor* createSession()` | 借出一个新的帧驱动者；设备拥有它；默认 `return &executor();` | 完整签名见 `RenderDevice.hpp` |
| `void releaseSession(RenderFrameExecutor&)` | 归还；默认 no-op；`shutdown()` 会带走全部会话 | 与 `setDiagnosticsRoute` 同形的"借用/归还" |
| ~~`setDefaultContentProgram`~~ | **移出**本类，见 4.2：它是帧的设置，且两处同签名 virtual 会二义 | `G1` / `G4` |

### 4.2 `RenderFrameExecutor`（SDK）

| 接口 | 契约 | 备注 |
|---|---|---|
| `executeFrame` / `prepareFrame` / `frameReport` | **按会话**：同一会话线程限定；**不同会话可并发** | 契约文本改写（F10） |
| `void setDefaultContentProgram(intrusive_ptr<const ShaderProgram>)` | 本会话的程序缺失内容用什么；默认 no-op；`Device` 覆写并转发给设备自己的会话 | 从 `RenderDevice` 迁来 |

### 4.3 `gfx_backend_vk`（内部）

| 类型 | 职责 | 备注 |
|---|---|---|
| `Session` | 帧机器 + 帧暂存 + 帧闸 + 帧账 + claim 集合 | `SessionId` 绑定 `DeviceClock::WindowId` |
| `ContentCaches` | programs / geometries / pipelines / 布局 / 图像 / 分配器 + 一把独占锁 | 从 `ContentDraw` 拆出 |
| `FrameScratch` | 环 / 描述符池 / set 表 / `FrameSnapshot` | 从 `ContentDraw` 拆出 |
| `ResidenceTable` | 三态 + 发布/撤回/退休 | 复用 `RetirementQueue` 的 epoch 语义 |
| `TargetClaims` | 目标 → 会话；try-claim / release | 冲突报一条，按 (目标, episode) 去重 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 会话由**设备**拥有；引擎只借（`createSession` / `releaseSession`）。 |
| C2 | 帧机器（命令池 / 栅栏 / 环 / 描述符池 / 帧账）**只**在会话里。 |
| C3 | 记录路径**不得**取设备缓存的锁；设备缓存的读只发生在帧首快照。 |
| C4 | 只有 `Resident` 的条目可被非 owner 使用；`Uploading` 对别人等于 `Absent` + 临时副本。 |
| C5 | 一个目标同一时刻只有一个会话；claim 是租约，帧尾必须归还。 |
| C6 | 销毁只能发生在**设备水位**之后；不得用设备空转换正确性。 |
| C7 | 读回不借任何会话的帧机器；它等水位，并把等待计数。 |
| C8 | 默认内容程序归会话；两个引擎声明不同程序时各画各的。 |
| C9 | vsg 的窗口职责（交换链 / `Viewer` / `WindowTarget`）不因会话化而移动。 |
| C10 | 契约文本必须与代码的实际保证一致；做不到就先写"串行"。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | `createSession()` 可在 `initialize()` 之前调用。 |
| R2 | `executor()` 保留，含义是"设备自己的会话"。 |
| R3 | 稳态帧的**记录路径**设备缓存锁获取次数为 0（帧首那次除外；整帧次数不随 draw 数增长，有计数器与用例） |
| R4 | 驻留条目不可变；`Uploading` 不得长期存在（发布或撤回）。 |
| R5 | 目标重建只由 claim 持有者做。 |
| R6 | 释放可在别的会话 frame 时调用，且不阻塞到那一帧结束（等待上界是一次缓存操作；2026-11-16 落地，T6） |
| R7 | 读回读到"最新已完成"的帧。 |
| R8 | 单会话后端行为与今天逐字节相同。 |
| R9 | `BackendParityTest` 的逐字节对拍在两会话下仍成立。 |
| R10 | 文档门禁 0 finding。 |

### 5.3 不变量（N#）

| # | 不变量 |
|---|---|
| N1 | 一个身份只有一个发布者；发布用 compare-and-set。 |
| N2 | 每个 `Uploading` 条目在帧尾要么 `Resident`，要么回到 `Absent`。 |
| N3 | 一个目标同一时刻只有一个 claim 持有者。 |
| N4 | 记录路径上没有设备级锁（`N1`–`N3` 因此不必在热路径上检查）；帧首那一次是快照的机制，不是热路径 |
| N5 | 设备丢失对**所有**会话同时成立（共用 `VkDevice`，不是失败分类能分开的）。 |
| N6 | 能力（`capabilities()`）是设备级只读事实，任意会话任意时刻可问。 |

## 6. 注意事项与陷阱

### 6.1 `Uploading` 卡死（P1）

- **会怎么错**：owner 的帧在发布前失败（分配拒绝、`vkBeginCommandBuffer` 拒绝、提前 return），条目停在
  `Uploading`。之后**所有**会话都把它当"别人在上传"而复制——每帧都复制，正确但永不收敛。
- **怎么判**：驻留账里"每帧临时副本数"不归零；或一个条目在 `Uploading` 超过一次设备水位。
- **怎么防**：帧尾作用域守卫**无条件**发布或撤回（`N2`），并在 Debug 断言"帧结束时本会话建的条目全部已定态"。

### 6.2 竞争时"等"而不是"复制"（P2）

- **会怎么错**：A 等 B 的 `Uploading`，B 的帧里又需要 A 正在画的某样东西 ⇒ 互相等，两只会话一起死。
- **怎么判**：两会话用例挂死（不是失败）。
- **怎么防**：**只复制，不等**；`Uploading` 对非 owner 没有等待路径（`C4` 的可判定形式）。

### 6.3 claim 泄漏（P3）

- **会怎么错**：某个 pass 的提前 return（未知目标、跳过）之后没有归还 claim，下一个会话永远跳过那个目标。
- **怎么判**：T5 的第二个会话连续两帧都跳过同一目标。
- **怎么防**：claim 集合由会话的帧守卫统一释放（RAII），不在 pass 循环里手工 release。

### 6.4 水位与 park 队列被两条线程碰（P4）

- **会怎么错**：`DeviceClock::windowCompleted` 与 `RetirementQueue::retire/advance` 今天写着"单线程"，
  两会话之后两条线程同时调 ⇒ 数据竞争（TSan 可见，或更糟：提前释放）。
- **怎么判**：TSan；或 `pending()` 在释放后再涨。
- **怎么防**：两者各一把小锁，**只在帧首/帧尾取用**；`retire` 与 `advance` 的顺序规则（先 park 再 advance）
  写进会话的帧尾。

### 6.5 `shutdown()` 与活着的会话（P5）

- **会怎么错**：引擎还持有会话引用，设备被创建者 `shutdown()`；下一次 `frame()` 解引用悬垂。
- **怎么判**：ASan 的 UAF；或 Debug 断言"session's device is up"。
- **怎么防**：引擎 `frame()` 先问 `device_->initialized()`（已如此）；`shutdown()` 把会话标为 dead 并把
  引用置空；契约明写"创建者保证 shutdown 时没有帧在飞"。

### 6.6 快照漏项（P6）

- **会怎么错**：快照只覆盖"帧值里显而易见的身份"（命令的 geometry / program），漏掉 pass 级或隐式身份
  （shadow map 的可视、screen draw 的源目标）⇒ 每帧 miss、每帧取锁。
- **怎么判**：R3 的锁计数在稳态不为 0。
- **怎么防**：快照的输入是**同一次遍历**记录用的那份帧值（一次遍历出两种结果），而不是另写一份枚举。

### 6.7 "读回很快"的错觉（P7）

- **会怎么错**：把读回当成便宜的动词，放进每帧路径；它等设备水位，等于把并发收益吃掉。
- **怎么判**：`deviceWaits()` 随帧数线性涨。
- **怎么防**：读回不进帧路径（SDK 文档已写"读回是宿主动作"）；用例断言两会话稳态帧 `deviceWaits()` 不涨。

### 6.8 `d->driving` 的第二份（P8）

- **会怎么错**：vsg 的 `driving` 搬进会话后，设备仍留一份"最后驱动者"，两处不一致。
- **怎么判**：一个帧的 pass 用到另一个会话的窗口目标。
- **怎么防**：`driving` **只有**会话里一份；设备的 present 路径从 `PresentTarget` 反查会话（facade 已按索引转发）。

### 6.9 内嵌类拿不到基类的 `protected` 成员（P9）

- **会怎么错**：`Device::Session` 是设备的**内嵌**类，拥有对 `Device` 私有成员的访问权——但 `reportDiagnostic` 是
  `RenderDevice` 的 **protected** 成员：**通过对象**调用它只允许派生类自己，内嵌类不算（C++ 的请求者视野规则）。
  帧体里有 ~25 处这样的调用，直接搬过去就是一片编译错误。
- **怎么判**：编译器回 "call to non-static member function ... from nested type"。
- **怎么防**：帧体不直接向设备提问，而是走 `Impl` 上由设备装配的三个面（`report` / `ensure_target` / `note_device_lost`）——
  它们恰好只有“每 pass 一次”和“失败时一次”的调用频率，即使不在热路径上也不贵（`G10`），而且把“只有设备能做的决定”
  变成了一个可注入的面（对用例也好）。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 全局设备锁包住每个动词 | `G7`：复杂度没被吸收，而是转移成"每次调用都可能阻塞"；`G1`：锁粒度与资源无关 | ADR 选项 E |
| D2 | 每条目一把 `shared_mutex` | `G10`：读取的是一条不可变记录，per-entry 锁只在 2000 draw 路径上付原子操作；`G8`：没有写者要协调 | ADR 选项 F |
| D3 | 竞争时"等"而不是"复制" | `G4`：可能互相锁死（`P2`）；有界复制更便宜 | ADR §3 |
| D4 | 会话 = 窗口（沿用 vsg 今天的形状） | `G1`：离屏会话不是窗口；`G5`：把一个概念捆在另一个概念上，两个都会长歪 | 本文 §2.1 |
| D5 | 引擎自己 new 一个会话对象 | `G1`：GPU 对象的所有者必须是设备；`G10`：引擎不该为命令池的寿命负责 | 本文 §2.8 |
| D6 | 读回借某个会话的帧机器 | `G4`：读回读的是"已完成"，与"谁刚提交"无关；借谁的都会与那个会话互相等 | 本文 §2.7 |
| D7 | 现在就把 `DeviceClock::WindowId` 改名 `SessionId` | `G8`：没有消费者需要这个拼写；先用类型别名（`SessionId = WindowId`），重命名留到有第二个 API 家族时一起做 | 本文 §2.6 |
| D8 | 一次落地（不做分阶段） | `G11`：半拆的树比没拆更糟——每一步都要能单独验证 | 本文 §9.3 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 单会话后端行为零变化 | `build/bin/test_graphics`、`test_vsg`（默认 `createSession`） |
| T2 | `test_vk` 在"只有一个会话"的树上全绿 | `build/bin/test_vk` |
| T3 | 记录路径的缓存锁为 0；整帧获取次数**不随 draw 数增长**（实测 4：帧首 1 + 数簿 2 + 帧尾 1） | `test_vk` `VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls`（1 条 vs 6 条命令；变异 6 → 10 红） |
| T4 | 两会话同时首画同一几何：不崩、两图都对、上传 ≤ 2 | `test_vk` 新用例 + 变异（去掉撤回 ⇒ 红） |
| T5 | 两会话画同一目标：一个 pass 被跳过 + 一条报告 | `test_vk` 新用例 |
| T6 | 释放与在飞帧重叠：释放不等到帧结束，帧仍用已解析的句柄完成，对象 park 到水位，`deviceWaits()` 不涨 |
  `VkMultiSessionTest.AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses`（变异：释放重新取体闸 ⇒ 红） |
| T7 | 交替画不同目标 + 穿插读回：各读各的最新完成帧 | `test_vk` 新用例 |
| T8 | 两会话不同默认程序：各画各的 | `test_vk` 新用例 + 变异（共用单槽 ⇒ 红） |
| T9 | 两会话下 `BackendParityTest` 逐字节对拍不变 | `test_vsg` |
| T10 | 两会话各一条线程各 N 帧：两张图各自正确，重入被拒绝；**帧体时间上真的重叠**（一帧被 sink 停住时另一帧整帧完成） | `VkMultiSessionTest.TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`（各持一张自己的贴图）、
  `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame`（变异回独占闸 ⇒ 红）；T47 的“同线程重入被拒”断言保留 |
| T11 | 数据竞争有工具证据 | `scripts/tsan_check.sh`（需要 `build-tsan`，见 §9.2） |
| T12 | 文档与代码一致 | `python3 scripts/check_ai_docs.py --strict`、`python3 scripts/check_design_docs.py` |

## 9. 状态与演进

### 9.1 当前状态

- **已落地**：`T47`——设备级帧闸（`backend/FrameGate.hpp`）、两个后端同一实现、三处契约文本、并发用例；
  vk 的 `vk.*` 诊断 code 全族；`graphics-threading.md` 的 `C21` / `R18` / `N18` / `D13`。
- **已落地**：`T48` 的 **S1**——vk 采用 `backend/DeviceClock.hpp` + `RetirementQueue`（会话 = clock window），
  `releaseContent` / `releaseRenderTarget` 把对象**交给队列**，`shutdown()` 在 idle 之后 `releaseAll()`（SDK 新增的拆除动词）。
- **已落地**：`T48` 的 **S2a + S2b**——帧机器与帧体都在 `Device::Session`；`executor()` 返回它，
  `Device::executeFrame` / `prepareFrame` / `frameReport` 是三个一行转发。帧体不再向设备提问：报告 / 建目标 / 认定丢失
  走 `Impl` 上装配好的三个面（见 `P9`）。
- **已落地**：`T48` 的 **S3**——`ContentDraw` 拆成三件：`ContentCaches`（设备：programs / geometries / pipelines /
  布局 / 图像 / 管线缓存）、`FrameScratch`（驱动者：常量环 + 描述符池 + set 表 + `frame_index`）、`ContentDraw`
  （记录器：持两者的借用引用，`prepare` / `record` 两个动词）。`Device::Session` 拥有自己的 `scratch` 与 `recorder`，
  `Device::Impl` 只留 `caches`；`createScratch()` 在布局建好之后由 `initialize()` 调，拆除时 scratch 先于 caches。
  行为零变化（`test_vk` 103/103，ASan 103/103，变异 1/1 红）。
- **已落地**：`T48` 的 **S4**——`internal/ResidenceTable.hpp`：一条键一条条目，`admit` 答 `Use` / `Build`
  （move-only `Claim`）/ `Copy(owner)`；`Claim::publish()` 是 owner 的"填充已完成"声明，析构时未决议的条目退回
  `Absent`；token 单调不复用，`forget` 之后的老 claim 无法复活。`ResidenceTableTest` **8 例**（含 8 线程抢一键
  恰一个 `Build`），变异 2/2 红（去掉析构撤回 / 去掉 token 比较）。`test_vk` 111/111。**设备接线仍未做**（截至
  2026-11-16）：先造机器再接线，而不是半接（`D8`）；见 §3.1.4 与 §9.2。
- **已落地**：`T48` 的 **S5**——`internal/TargetClaims.hpp`：设备持 `RenderTarget* → SessionId`；`begin(session)`
  发帧作用域 `Lease`，`claim` 冲突答 false、析构释放（`P3`）；`executeFrame` / `prepareFrame` 在 `ensure_target`
  之前 claim，冲突只跳该 pass 并报 `vk.target.claim_conflict`（每（目标, 情节）一条）。`TargetClaimsTest` 7 例 +
  `TargetClaimFrameTest` 2 例，变异 2/2 红；`test_vk` 120/120（Debug + ASan 门禁）。
- **已落地**：`T48` 的 **S6b（串行版）**——vk 真的多发驱动者：`createSession()` 造 `Session`（早于 `initialize()` 的由
  initialize 统一拉起；已起的设备上当场建），`releaseSession` 销毁并遗忘，设备自己的驱动者不可归还。设备持一把
  `frame_mutex` 串行化帧体（缓存/分配/队列尚未在两条运行帧之间成立，`C10`：先写串行）。**每一个碰共享状态的宿主动词
  （读回 / 释放 / 默认程序 / 释放目标 / 建设备窗口 / 诊断读取）也取同一把锁**：它们可能**等待**在飞的帧体，但不再与它竞争——
  多驱动者之后"帧之间"已不是宿主能自己命名的一个时刻，设备替它命名（`C10` 的第二半）。`VkMultiSessionTest` 6 例
  （T8 两驱动者不同默认程序各画各的、T10 两线程各 6 帧两图各自正确、R1 早借晚用、释放与再借），变异 2/2 红；
  `test_vk` **125/125**、`test_graphics` 587、`test_vsg` 556，ASan 门禁 PASS。
- **已落地**：`T48` 的 **S6a**——`RenderDevice::createSession()` / `releaseSession()` 接缝（默认 `&executor()` /
  no-op）；`setDefaultContentProgram` 移到 `RenderFrameExecutor`；`RenderEngine` 持 `session_`（借/还，帧与默认程序
  走它）；vk 的默认程序归 `Session`。三套 **556 / 121 / 587** 全绿，变异 1/1 红。**S6b** 才是第二个真实驱动者：
  缓存/分配/提交三把锁（或帧首快照）、`ensureGeometry` 接 `ResidenceTable`、替换路径改 park、T8 并发证据。
- **已落地**：`T48` 的 **S6c-1（帧首快照）**——`ContentCaches` 有一把不可重入的锁（`ContentCaches::Lock`，`Lock&` 参数把
  “我持锁”变成编译期证据）；帧只在**帧首**取它一次，`planCommand` / `planScreen` / `resolveTextures` 把 pipeline、
  顶点/索引/实例 buffer 句柄、材质、纹理 view/sampler **按值**写进 `CommandPlan` / `ResolvedTextures`，纹理上传也
  在帧首录进帧命令缓冲（render pass 之外，唯一合法处）。`ContentDraw` 只剩记录职责（`prepare`/`prepareScreen` 删除，
  预热走与帧首相同的 `ContentCaches::planCommand`）。**记录路径不取锁、不碰缓存条目**；instance stream 换代与失败的
  geometry 子树改 **park** 到水位，所以帧首解析的值整帧有效。观察点：`contentCacheCounters().cache_lock_acquisitions`
  （整帧固定 4，不随 draw 数增长）。`test_vk` **128/128**（ASan 门禁 PASS），变异（记录路径改回逐 draw 取锁）⇒ 6 → 10 红。
- **已落地**：`T48` 的 **S6c-2a（帧体真并行）**——体闸改成 `std::shared_mutex`：帧体取 SHARED（两驱动者同时记录/提交），
  出窗口的帧与所有宿主动词取 EXCLUSIVE（可能等帧，但不再竞争）。为此：`FrameTimeline` 支持多开帧（开帧时铸造号，
  `submitted`/`abandoned` 按 token 关闭；两驱动者不再拿到同一个号）；`DeviceClock`/`RetirementQueue` 仍标“非线程安全”，
  由设备的 `clock_mutex` 串行（P4，参考后端单线程不需要它）；`MemoryAllocator`、目标 map、报告 sink、每会话默认程序、
  表面列表各一把小锁；采样目标（screen 源与 pass 输入）也取目标 lease；纹理上传/替换图像按驱动者 batch 归属（替换图像
  park 到水位）。确定性别据：`VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame` 用诊断 sink 停住一帧，
  另一驱动者的帧必须整帧完成；变异回独占闸 ⇒ 红。`test_vk` **129/129**（ASan PASS）、`test_vsg` 557、`test_graphics` 587。
- **已落地**：`T48` 的 **S6c-2b（R6 / T6）**——释放不再等帧（`releaseContent` ×3 与 `releaseRenderTarget` 只等一次缓存操作）；
  目标条目共享所有权使“已经解析它的帧”在释放后仍有一个有效指针；park 与水位的开帧规则保证对象不被提前销毁。见 §9.3 的 S6c-2b 行。
- **已落地**：`T48` 的 **S7（读回独立化）**——`ReadbackStaging` 自带命令池与栅栏（C7），等待 = 体闸的“帧之间”且
  **每次读回计一次** `deviceWaits()`（与参考后端的读数一致，G6）；等待前可回答的拒绝不停止设备；帧体内读答
  `Failed`（`FrameBodyDepth`）。**顺带修掉 S6c-2a 的外部同步缺口**：`vkQueueSubmit` / `vkDeviceWaitIdle` 过设备的
  `queue_submit` 锁。`test_vk` **133/133**，变异 2/2 红（体闸改 SHARED ⇒ 等待用例红；去掉计数 ⇒ 计数断言红）。
- **未做**：`T48` 的 S8–S9（vsg 的 F9、F10 收口 + TSan）。
- **一处更正**：ADR 说"vsg 更接近"，准确说法是"vsg 的**窗口**会话已就位，**帧驱动者**仍是设备级单例"
  （§2.1）。这不改变结论，只改变迁移表的分工。

### 9.2 已知缺口 / 待办

- **F4 的设备接线（`ResidenceTable::admit` 进 `ensureGeometry`）仍未做**：状态机与它的用例在树上，缓存仍走原路径。
  今天两个驱动者首次同时构建同一内容时**在缓存锁上串行**（不是等待对方的帧）；触发条件是一个真实宿主的
  "一边编辑一边烘焙"让 miss 进入帧预算。接线形状见 §3.1.4（`Build` 留下的 `Claim` 在条目建成后 `publish`，竞争
  者拿 `Copy` 建临时副本）。
- `DeviceClock::WindowId` 的命名（`D7`）；`SessionId` 先做别名。
- **T3 已落地（按准确形式）**：S3 只做结构性拆分；S6c-1 让记录路径真的不取锁，计数器是 `
  contentCacheCounters().cache_lock_acquisitions`，用例是 `VkFrameHeadTest`。整帧 4 次（帧首 1 + 数簿 2 + 帧尾 1），
  旧的“稳态帧 0 次”指的是记录路径（见 §2.3 与 §8 T3 的订正）。
### 9.3 兼容性 / 迁移

**原则：每一步都让树保持绿，每一步都能单独回滚。** 顺序按"先造机制、后公开形状"排。

| 步 | 做什么 | 结束时可验收 | 风险 |
|---|---|---|---|
| S1 | vk 采用 `DeviceClock` + `RetirementQueue`（单会话）；`releaseContent` 变成 park | **已落地 2026-11-15**：T6 单会话版 + `test_vk` 103 全绿，3 个变异各自被杀 | 低（契约文本已允许） |
| S2a | 帧机器搬进 `Device::Session`（命令池 / 栅欄 / 时间戳池 / 帧账 / 帧闸 / clock window） | **已落地 2026-11-15** | 低 |
| S2b | 帧体（`executeFrame` / `prepareFrame` / `frameReport`）搬进 `Session`，`executor()` 返回它，`Device` 只剩三个转发 | **已落地 2026-11-15**：`test_vk` 103 全绿（含 ASan） | 中 |
| S3 | 拆 `ContentDraw` → `ContentCaches` + `FrameScratch`；记录一次"锁计数" | T3（单会话版） | 中（500–700 行搬家） |
| S3 | **已落地 2026-11-15**：`ContentCaches` / `FrameScratch` / `ContentDraw`（记录器）三件；`Session` 持 scratch +
  recorder，`Device::Impl` 只留 caches；`test_vk` 103/103（Debug + ASan），变异（去掉 scratch 的环）⇒ 画图用例红。
  **锁计数随 S4**：单会话没有第二个读者，现在加锁是死机制（`G8`）；S4 的 `ResidenceTable` 引入 owner/competitor 时
  才有“稳态帧 0 次获取”可测 | T3 推迟到 S4 | 中 |
| S4 | `ResidenceTable` 三态 + 发布/撤回；用**两个内部会话**做单元用例 | T4 | 中（状态机） |
| S4 | **已落地 2026-11-15**：`internal/ResidenceTable.hpp/.cpp`——`admit` 答 `Use`/`Build`/`Copy`，move-only
  `Claim` 发布或撤回，token 单调且不复用（`forget` 后的老 claim 不能复活）；`Numbers` 见证 `claims` /
  `published` / `withdrawn` / `copies` / `stale` / `uploading`。`ResidenceTableTest` **8 例**（含 8 线程抢一键：
  恰一个 `Build`）；变异 2/2 红（去掉析构撤回 / 去掉 token 比较）。**未接线**：`ContentCaches::ensureGeometry`
  仍走原路径，S6 接入会话 id 时切换 | T4（机器半） | 低 |
| S5 | `TargetClaims`；仍用两个内部会话验证 | T5 | 低 |
| S5 | **已落地 2026-11-15**：`internal/TargetClaims.hpp/.cpp`（`begin` → 帧作用域 `Lease`，冲突 false，析构释放）；
  `Session::executeFrame` / `prepareFrame` 在 `ensure_target` 之前 claim，冲突只跳该 pass 并报
  `vk.target.claim_conflict`（每（目标, 情节）一条）；`Device::targetClaims()` 是内部可观察量。`test_vk` **120/120**
  （Debug + ASan 门禁），变异 2/2 红 | T5（单会话 + 冒充租约） | 低 |
| S6 | `createSession()` / `releaseSession()` 公开；引擎持有会话；**帧闸变成每会话** | T8、T10 | 中（公开形状） |
| S6a | **已落地 2026-11-15**：`RenderDevice::createSession()` / `releaseSession()` 默认接缝（默认答 `&executor()` /
  no-op）；`setDefaultContentProgram` 从 `RenderDevice` 移到 `RenderFrameExecutor`（默认 no-op，`RenderBackend` 不再
  二义）；`RenderEngine` 在 `setDevice` 借、换设备/析构还，帧与默认程序走自己的 `session_`；vk 的默认程序从 `Impl`
  移到 `Session`（`Device` 覆写转发）。`test_graphics` 587（+1 借/还用例）、`test_vk` 121（+1 默认形状）、
  `test_vsg` 556；变异 1/1 红（析构不还）。**坑**：改了 SDK 的虚表后**动态插件必须重建**（旧 .so 让插件用例段错误）。
| S6b | **已落地 2026-11-15（串行版）**：`Device::createSession()` 真的造一个 `Session`（可早于 `initialize()`，由它统一拉起；
  已起的设备上则当场建）；`releaseSession` 销毁并遗忘（设备自己的驱动者不可归还）。**设备持一把 `frame_mutex`**：会话各持机器，
  但缓存/分配器/队列还没在两条运行中的帧之间成立，所以帧体串行——这正是 `C10` 要求的"做不到就先写串行"。T8（两驱动者不同默认程序、
  各画各的）与 T10（两线程各 N 帧、两图各自正确）已绿；变异 2/2 红。`test_vk` **125/125**（ASan 门禁 PASS）。
| S6c-1 | **帧首快照**：`ContentCaches` 一把锁（`Lock` 参数化）/ 帧首一次 / `CommandPlan`·`ResolvedTextures` **按值** /
  纹理上传搬到帧首 / 替换路径（instance stream、失败的 geometry）改 park / 锁计数接到 `contentCacheCounters` | T3 | 大 |
| S6c-1 | **已落地 2026-11-15**：记录路径零锁（`record`/`recordScreen` 不调 `planCommand`，不碰 `images()`）；`ContentDraw`
  只剩记录职责，计划由帧首/预热统一解析；`test_vk` **128/128**（含 ASan），变异（记录路径变回逐 draw 取锁）⇒ 整帧获取数
  6 → 10，`VkFrameHeadTest` 红 | T3 | 大 |
| S6c-2a | **真并行（帧体）**：`shared_mutex` 体闸（帧 SHARED、宿主动词 EXCLUSIVE）、窗口帧独占、多开帧 token、
  clock/queue/allocator/target map/sink 各自的小锁、每驱动者纹理 batch、替换图像 park、采样目标也 claim | T10 的并行证据 | 大 |
| S6c-2a | **已落地 2026-11-16**：两帧体**时间上重叠**（`VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame`
  用诊断 sink 把一个帧体停住，另一驱动者的帧必须整帧跑完；变异回独占闸 ⇒ 5 s 超时后红）。`FrameTimeline` 支持多开帧
  （开帧时铸造号，两驱动者不再共用 token）；`DeviceClock`/`RetirementQueue` 由设备的 `clock_mutex` 串行（P4）；
  分配器、目标 map、报告 sink 各一把小锁；每驱动者一个纹理 batch；被替换的纹理图像 park。`test_vk` **129/129**
  （ASan 门禁 PASS）、`test_vsg` **557**、`test_graphics` **587** | T10 | 大 |
| S6c-2b | **释放不再等帧（R6）**：`releaseContent` ×3 / `releaseRenderTarget` 脱离体闸；目标条目改共享所有权 + 水位 park | T6 | 中 |
| S6c-2b | **已落地 2026-11-16**：三种 `releaseContent` 与 `releaseRenderTarget` 不再取体闸——它们只等**一次缓存操作**（帧首解析持有的那一把），
  不等一整帧。安全来自已有的两件事：缓存锁（帧首解析与释放互斥）与 park（帧已解析的句柄活到水位，而开着的帧把水位按住）。
  `targets` 的条目改成 `shared_ptr<TargetImages>`（`releaseRenderTarget` 不能再把值从 map 节点里搬走——帧持有的是指向节点的指针）；
  `released_nodes` 改原子；`retirementNumbers()` / `frameReport()` 读队列时也取 `clock_mutex`（释放现在可与它们并发）。
  **证据**：`VkMultiSessionTest.AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses`——帧被诊断 sink 停住时，宿主线程
  释放该帧已解析的程序/几何与一个已建目标，释放必须在 2 s 内返回；帧随后仍用解析出的句柄画完，`device_waits` 不涨、park 尚未放行。
  变异（把体闸加回 `releaseContent`）⇒ 2 s 超时后红。`test_vk` **130/130**（ASan PASS）。 | T6 | 中 |
| S7 | F7 读回独立化 | T7 | 低 |
| S7 | **已落地 2026-11-16**：`ReadbackStaging` 拥有自己的命令池与栅栏（C7 结构性成立）；等待是设备的\"帧之间\"\n  （体闸 EXCLUSIVE）且**每次读回计一次** `deviceWaits()`；等待前可回答的拒绝不停止设备；帧体内读答 `Failed`\n  （`FrameBodyDepth`）；`vkQueueSubmit` / `vkDeviceWaitIdle` 补上 ADR 早就写明的提交锁。`VkMultiSessionTest`\n  新增 3 例（交错读回各自最后一帧、等待并计数、帧体内拒绝），变异 2/2 红；`test_vk` **133/133** | T7（两会话） | 低 |
| S8 | vsg 的 F9（搬收集暂存 + 缓存加锁/快照） | T9 | 中 |
| S9 | F10 收口 + TSan + 文档 | T11、T12 | 低 |

**S6 是唯一改变并发语义的一步**：在它之前，两个会话只存在于单元用例里；S6a/S6b 之后，两个引擎各有自己的驱动者、
可以同时**调用**，但设备把**帧体**串行化（缓存/分配/队列的锁与帧首快照是 S6c）。所以 S1–S5 的验收标准都是"单会话行为不变"；
S6b 由 T8/T10 承担"两个驱动者各自正确"的证据，S6c 才承担"时间上重叠"的证据。
**S6c 又分三步（D8）**：S6c-1（已落地）交付帧首快照与锁——记录路径不再碰缓存，替换对象一律 park；S6c-2a（已落地）
把体闸改成 `shared_mutex`（帧 SHARED、宿主动词 EXCLUSIVE）并补齐两帧同时跑所需的每一把小锁与每个驱动者的纹
理 batch，于是“时间上重叠”有了确定性证据；S6c-2b（已落地）把 `releaseContent` / `releaseRenderTarget` 从体闸上拿下来
（R6：可在别人 frame 时调用且不阻塞到它结束），安全由缓存锁、park 与“开着的帧按住水位”共同给出。
**S1 已落地（与历史估计的一处差异）**：释放是**两件事**，而不是一件。缓存表**立刻**忘掉条目（所以`ResidencyLedger`、`progress().streams_live` 的数字照旧立即下降，下一帧重传），而 GPU 对象在
集合队列里等水位——一个 slot 时是 `submitted + 2`，即**最多两帧后**才销毁。新的观察点是
`Device::retirementNumbers()`（`pending` / `released` / `device_waits`），而不是驻留账。两帧不是 bug：它是
"slot 已经被回收"这个证据的代价，且它是 S6 多会话下不可少的保守性。

## 10. 参考

### 10.1 代码

- 契约：`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`RenderBackend.hpp`
- 机制：`src/viz/graphics/sdk/vine/graphics/backend/FrameGate.hpp`、`DeviceClock.hpp`、`RetirementQueue.hpp`、`FrameTimeline.hpp`
- vk 的现状：`src/plugins/gfx_backend_vk/src/internal/Device.cpp`（`Impl` 的帧机器）、
  `src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp`（缓存与帧状态同居）
- vsg 的现状：`src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`（`Data` 的帧暂存单例）、
  `src/plugins/gfx_backend_vsg/src/internal/Session.cpp`（窗口会话）、`src/plugins/gfx_backend_vsg/src/internal/OffscreenSession.cpp`
- 引擎侧：`src/viz/graphics/src/RenderEngine.cpp`（`setDevice` / `frame`）、`src/viz/graphics/src/RenderPass.cpp`（单 pass 驱动）

### 10.2 文档与 ADR

- `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`（决定与选项）
- `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`（共用退休机制）
- `ADR-2026-11-13-one-engine-one-surface.md`（一引擎一表面，本设计的前提）
- `graphics-threading.md` `C21` / `R18` / `N18` / `D13`（今天的契约）
- `vk-backend.md` §2.11（一个 `Device` 对象 = 一个会话）、`vsg-backend.md` §2.9（后端架构）
- `graphics-frame-pipeline.md`（帧是值、四层单向）

### 10.3 外部资料

- Vulkan 规范：按对象的外部同步要求（队列 / 命令池 / 描述符池 / 管线缓存 / 栅栏）——ADR 的"硬约束"表
- VSG 1.1.16：设备级 `MemoryBufferPools`（`mutable std::mutex`）+ 每任务帧状态——ADR 的核对
- O3DE RHI / wgpu：设备线程安全、命令列表每线程（外部资料，未在本仓核对）
