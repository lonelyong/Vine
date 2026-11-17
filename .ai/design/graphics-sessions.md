# 设备是池、会话拥有帧：类型、所有权与并发规则

> **状态**：已落地（`T47` + `T48` 的 S1–S9 与 `F4` 接线）
> **最后核对**：2026-11-17
> **范围**：一个 `RenderDevice` 被多个引擎借用时，“共享什么、谁拥有帧、怎么并发、怎么退休”
> 的**实施形状**；契约的**为什么**与落地记录在 ADR，线程模型的**现在**在 `graphics-threading.md`。
> **权威来源**：`RenderDevice.hpp` / `RenderFrameExecutor.hpp` / `RenderBackend.hpp`（契约）、
> `graphics/FrameGate.hpp` / `graphics/DeviceClock.hpp` / `graphics/RetirementQueue.hpp`（机制）、
> `src/plugins/gfx_backend_vk/src/internal/Device.cpp` 与 `ContentDraw.cpp`（今天的设备级帧机器）、
> `src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`（已经是"设备 + 每窗口"的那一半）。
> **待办**：无未落地的会话迁移步骤；触发型工作见 §9.2 与 `../backlog/graphics.md`

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

- **是什么**：`FrameDriver` = 谁在驱动帧：命令池、命令缓冲、栅栏、常量环、描述符池、帧账、它 claim 的目标。
  类型名仍是 `RenderFrameExecutor`（公开契约，`ADR-2026-11-16` 的词汇表）；一个驱动者**至多**present 到一个窗口
  （窗口是它的目标，不是它的身份）。
- **为什么需要点名**：`gfx_backend_vsg` 的 `api::WindowSession` 是**一个窗口一个设备一个交换链**
  （`WindowSession.hpp`），而**收集/编译/执行**这条帧管线在 `VsgBackend::Data` 里是**设备级单例**
  （`recorder` / `compiler` / `arena` / `facts` / `packets` / `frame` / `driving`，见
  `VsgBackend.cpp:444`–`534`）。也就是说：**vsg 的"会话"是窗口会话，不是帧驱动者**——ADR 说"vsg 更接近"
  只对了一半。这条更正决定了迁移的分工：vsg 要搬的是**收集暂存**，不是交换链。
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
- **已落地（S6c-1 + S2 批 2b）的实测形状**：`ContentCaches` 的一把锁在**帧首**取一次，`planDraw`（两个重载） /
  `resolveTextures` 把结果**按值**交给驱动者（`FrameHead`：每调用一个 `FrameHead::DrawPlan`，帧级纹理/采样器事实挂 head），纹理上传也在帧首录进帧命令缓冲；
  `record` / `recordScreen` 只读这些值。一整帧的获取数固定为 **4**（帧首 1 + 帧首/帧尾数簿各 1 + `endFrame` 1），
  **不随 draw 数增长**（`VkFrameHeadTest`：1 条 vs 6 条命令同数；变异成逐 draw 取锁后 6 → 10，红）。
- **vsg 的形状（S8c-1 + S8c-2，2026-11-16）**：同一原则落在 vsg 的两段上。**帧首**（`openFrameHead`）在一次性内容锁下解析
  pass 编号 / 目标 / 采样目标 / 内容跟踪 / 目标事实表 / 存储增长并记录上传基线；**收集与计划编译**只读会话 scratch，
  不取设备锁；**内容相**再分两段（S8c-2）：**解析**（`resolveContent`，内容锁下一次）把 halves / declared sets / stream
  bind / 材质槽 / 纹理视图 / 块偏移 / pipeline 身份写成驱动者 `FrameHead` 的**值**，**录制**（`FrameHead::record`）只读这些
  值、不取设备锁，**提交**（`executor.record` + 清扫 + 会话提交）再取一次锁。与 vk 的差别只剩形状（vsg 的“值”是一组 vsg
  节点 + 偏移，vk 的是一组 `FrameHead::DrawPlan`），`C3` 对 vsg 现在成立（见 §3.1.9 的 S8c-2）。
- **记录也按驱动者归属（S6c-2a）**：纹理上传的 staging 与被替换的图像按 `ContentImages::BatchId`（= `clock_window + 1`）
  分组，帧尾只释放**自己**那一批；被替换的图像本身 park 到水位——另一个驱动者的在飞帧可能还拿着旧 view。
- **管线是例外，但它本来就在 miss 上**：管线键含状态（拓扑 / 混合 / 深度），状态是每命令解析的，
  帧首算不出来——但管线表只在**创建**时写，创建本身就是 ms 级 miss，取一次独占锁不影响结论。

### 2.4 驻留三态：发布或撤回，竞争即复制（`G3` / `G4`）

```
Absent ──帧首 miss，claim 赢得──▶ Uploading(owner, token)
                                   │  owner 的帧完成（今天：executeFrame 返回已 wait 过栅栏）
                                   ├──▶ Resident          （发布：FrameBuilds::publishAll）
                                   └──▶ Absent            （撤回：填充失败/帧失败，由守卫完成）
Resident ──releaseContent──▶ Retiring(epoch) ──设备水位 ≥ epoch──▶ 释放
```

三条规则（缺一条都不成立）：

1. **一个键只有一个发布者**：发布用 compare-and-set（`Absent → Uploading(me)`），输的一方**不发布**。
2. **`Resident` 只在填充可证明完成之后发布**：今天是"这一帧的栅栏已经 wait 过"；**预热（`prepareFrame`）的发布点就是
   它那次只含传输的提交返回**（`ADR-2026-11-14-prepare-may-submit-transfers.md`）；多帧在飞之后是"退休队列说
   那一帧完成"——同一个信号（§2.6）。
3. **发布或撤回，二者必居其一**：owner 在帧尾用作用域守卫把本帧建的条目**全部**置为 `Resident` 或
   全部置回 `Absent`。没有第三条路——挂在 `Uploading` 的条目会让**所有**会话永远 miss（`P1`）。

竞争（另一个会话在帧首看到 `Uploading`）：**建一份不发布的临时副本**，本次帧用完即弃。
`P2` 说明为什么不是"等"：等待是唯一能把两个会话**互相锁死**的形状；复制是有界的（每个身份至多
`N−1` 份，且只在"两个引擎第一次同时画它"的那一帧）。`prepare()` 在加载期就把它做掉了，真实宿主通常
一次都碰不到。

**已落地（F4，2026-11-16）**：三条规则由 `ContentCaches::ensureGeometry` + `ContentCaches::FrameBuilds` 强制，
而不是靠调用点自觉。帧首问表、按答案分流（`Use` 用缓存条目并补上释放打出的洞 / `Build` 填充并留下 claim /
`Copy` 建本帧专用副本）；`publishAll()` 只在帧栅栏 wait 过后发布本帧持有的 claim，失败的填充从不登记、由守卫退回 `Absent`；
析构把未发出的 claim 与副本一并收拾。两个驱动者的**帧首解析**仍在缓存的一把锁上（`C3` 管的是记录路径），
接线决定的是"谁能建、谁复制"。

### 2.5 目标 claim：一帧一 claim，冲突只跳过那个 pass（`G6`）

- **是什么**：设备持一张 `TargetTable`（`RenderTarget*` → `FrameDriverId`）。帧首对**本帧要画的目标集合**
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
- **谁是谁**：**一个帧驱动者就是一个 clock window**（`DeviceClock` 发的就是 `graphics::FrameDriverId`，u32；
  离屏会话也注册一个）。三个拼写已统一到它（批次 1，2026-11-16）：`DeviceClock::WindowId` 只留一版 `@deprecated` 别名。
- **释放的语义**：`releaseContent` / `releaseRenderTarget` 记下**当时每个会话的 `submitted`**，设备水位
  越过它才真正销毁；因此这两个动词**可以在别的会话正在 frame 时调用**。这条路 `RenderDevice.hpp:302`-
  `308` 的契约文本**已经写好了**（"a device that retires asynchronously parks the free until every session
  is past it"）——本文只是把 vk 变成那个设备。
- **线程**：`DeviceClock` 与 `RetirementQueue` 的契约是“调用方负责串行，多线程设备自己提供锁”（`P4`）。
  锁放在**设备**（`Impl::clock_mutex`），不放进两个 SDK 值类型：参考后端（vsg）是单线程，
  给它加锁是没有消费者的机制（`G8`）。所有拿 `clock.timeline()` 引用的地方都在锁内，引用不离开临界区。
- **多开帧**：`FrameTimeline` 是“每个驱动者一个”而不是“一个开着的帧槽”：`begin()` 每次铸造新号，
  `submitted`/`abandoned` 按 token 关闭。否则第二个驱动者会拿到第一个的 token，两帧在水位比较里塌成一帧。
- **释放是两件事**：缓存表**立刻**忘掉条目（驻留账的 `progress().streams_live` 立即下降，下一帧重传），
  GPU 对象则在退役队列里等设备水位——一个 slot 时是 `submitted + 2`，即最多两帧后才销毁。观察点是
  `Device::retirementNumbers()`（`pending` / `released` / `device_waits`），不是驻留账；这两帧不是缺陷，
  它是“slot 已被回收”这份证据的代价，也是多会话下不可少的保守性。
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
| 帧机器今天在哪 | 全在 `Device::Impl`：`command_pool` / `completion` / 时间戳池 / `Books`（`Device.cpp:445`–`655`） | 命令图/提交**已经在每个窗口**（`api::WindowSession` 持 `vsg::Viewer`）；**收集暂存已入会话（S8a，2026-11-16）**：`FrameScratch`（arena / recorder / compiler / facts / 输入表 / packets / frame）由窗口与离屏会话各持一份 |
| 帧暂存在哪 | **已分家**（S3）：`ContentCaches`（设备）持 programs / geometries / pipelines / 布局 / 图像；`FrameScratch`（会话）持环 / 描述符池 / set 表 / `frame_index`；`ContentDraw` 只剩“把一条 draw 变成命令”的记录器 | `BlockStorage`（每帧 slab 轮转）+ `ContentAssembly` 的 `beginFrame` 清扫（设备缓存，S8b 才加锁/快照） |
| 要搬什么 | 拆 `ContentDraw` 为 `ContentCaches`（设备）+ `FrameScratch`（会话） | **已搬（S8a）**；**执行器帧状态已归驱动（S8b）**；**帧首快照与内容锁已落地（S8c-1，2026-11-16）**：设备闸两侧化、`FrameDrive`、帧首解析、内容相一把锁；**记录路径零锁已落地（S8c-2，2026-11-16）**：`FrameHead`（每驱动一份的值）+ 三相位（解析持锁 / 录制无锁 / 提交持锁） |
| 完成证据 | **没有**——需要采用 `DeviceClock` + `RetirementQueue` | **已有**（`d->clock` / `d->retirement`，由会话借用） |
| 量级 | 大（约 900–1400 行） | 小（约 300–500 行） |

**顺序**：先 vk（它是"设备 = 池"缺得最多的那个，也是 `test_vk` 的 102 例覆盖最厚的），再 vsg
（它拿到的是一条已在树上验证过的 SDK 形状 + 它自己的完成证据）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 会话类型与创建 | 设备拥有会话；`createSession()` 借出，默认答 `&executor()` | **已落地（S6a+S6b）**：SDK 默认接缝 + 引擎持有；vk 真的多发驱动者（各自命令池/栅栏/暂存/默认程序），**帧体真并行**（S6c-2a），窗口帧独占体闸 |
| F2 | 每会话帧机器 | 命令池 / 栅栏 / 时间戳池 / 帧账 / 帧闸按会话各一份 | **已落地（S2a+S2b）**：`Device::FrameDriver` 拥有帧机器与帧体，`Device` 只转发 |
| F3 | 设备缓存 + 帧首快照 | 缓存只在帧首读一次，记录路径零锁 | **已落地（S3 + S6c-1）**：`ContentCaches` / `FrameScratch` 分家；计划按值；观察点 `contentCacheCounters().cache_lock_acquisitions`（整帧固定 4 次） |
| F4 | 驻留三态 | `Absent → Uploading → Resident`，发布或撤回，竞争即复制 | **已落地（S4 + F4 接线，2026-11-16）**：状态机 + 8 条单元用例；`ensureGeometry` 问表（`Use`/`Build`/`Copy`），`FrameBuilds` 在帧完成时发布、竞争的帧建临时副本、失败与释放各有归属（见 §3.1.4） |
| F5 | 目标 claim | 一帧一 claim，冲突只跳过那个 pass | **已落地（S5 + S6b）**：`TargetTable` 租约 + 帧/预热接线；公开 `createSession()` 之后两会话冲突真的会发生（只跳该 pass，报 `vk.target.claim_conflict`） |
| F6 | 跨会话退休 | vk 采用 `DeviceClock` + `RetirementQueue`；释放可在别人 frame 时调用 | **已落地（S1 + S6c-2b）**：时钟 / park 队列 / 水位 / 开帧规则都在树上；释放不等帧（R6） |
| F7 | 设备级读回 | 自己的池与栅栏，拷贝前等设备水位，计入 `deviceWaits()` | **已落地（S7）**：`ReadbackStaging` 自带命令池与栅栏；等待 = 体闸的"帧之间"，每次读回计一次 `deviceWaits()`；帧体内调用答 `Failed`（见 §3.1.7） |
| F8 | 默认内容程序归会话 | 会话上可设；设备级只服务直接调用方；单会话后端行为不变 | **已落地（S6a+S6b）**：声明在 `RenderFrameExecutor`；vk 的每个 `FrameDriver` 各持一份，两驱动者各画各的（T8 用例） |
| F9 | vsg 的对应落地 | 收集暂存搬进会话；缓存加锁/快照；完成证据已有 | **已落地（S8a+S8b+S8c-1+S8c-2）**：收集暂存入会话（`FrameScratch`）；执行器帧状态归驱动（`VsgDriver` + 每驱动默认程序）；帧首快照 + 内容锁 + 设备闸两侧化（S8c-1）；记录路径零锁（`FrameHead`，S8c-2） |
| F10 | 契约与文档收口 | `C21` / `N18` 改写、三处头文件文本、ADR 状态、审计行 | **已落地（S9）**：`C21` / `R18` / `N18` 改成"帧按会话并发"；`RenderDevice` / `RenderFrameExecutor` / vk `Device` 的线程段同步；ADR 追加实施状态；审计 A6/A7/A9/A11 逐条标状态 |

### 3.1.1 F1 会话类型与创建

- **行为**：`RenderDevice::createSession()` 默认 `return &executor();`；vk / vsg 覆写为"造一个会话"。
  `releaseSession(RenderFrameExecutor&)` 默认 no-op。`RenderEngine` 持 `RenderFrameExecutor*`，
  `setDevice` 创建、换设备/析构释放。
- **约束**（C1）：会话由**设备**拥有；引擎只借。
- **要求**（R1）：`createSession()` 可以在 `initialize()` 之前调用（设备由创建者 initialize，引擎先 attach）。
- **验收**（T1）：单会话后端（默认实现）跑完 `test_graphics`，行为零变化。
- **注意**（N1）：`shutdown()` 不给活着的会话留悬垂——见 `P5`。

### 3.1.2 F2 每会话帧机器

- **行为**：`FrameDriver` 持 `command_pool` / `completion` 栅栏 / 时间戳池 / `Books` / `FrameGate` / 自己那一组
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
**F4 接线落地（2026-11-16）**：`ContentCaches::ensureGeometry` 先问表再上传——`Use` 用缓存条目（顺带补上
`releaseStreams` 打出的洞），`Build` 填充缓存条目、把 `Claim` 交给本帧的 `ContentCaches::FrameBuilds`，`Copy` 在表外
建一份**脱钩**几何只服务本帧。`FrameBuilds` 是帧作用域的值：`publishAll()` 在**帧栅栏 wait 过之后**发布可用的填充
（失败的填充**从不登记 claim**：失败处就让守卫把它退回 `Absent`，`Resident` 不能被一次失败的填充取得），析构把未发布的 claim 撤回、把临时副本
**立即销毁**（副本从不进表，只有本帧能命名它，而本帧已结束；它的上传计入 `stream_builds`，销毁不是释放，不计
`stream_releases`）。预热的发布点是它那次只含传输的提交返回；`releaseGeometry` 同时 `forget` 键，`destroy` 清表。
**证据**：`VkResidenceWireTest` 4 例——假租约下的一帧复制且像素正确（恰好两条流、`used_bytes` 回位）、真两会话
两线程首画同一几何（claims 1 / published 1 / copies ≤ 1 / 两图红 / 随后帧零上传）、失败的填充被撤回（`uploading 0`、
`stateOf == Absent`、不报 Resident）、释放后键可再建；变异 3/3 红（不问表 ⇒ 4 例红；发布失败的填充 ⇒ 撤回用例红；
释放不 forget ⇒ 释放用例红）。
**仍是的边界**：两个驱动者的帧首解析仍在缓存的一把锁上（`C3` 要的是记录路径零锁）；今天 vk 的填充是主机可见
memcpy，所以填充在 `fillStreams` 返回时就已可证明完成——将来把上传搬到设备本地（M6）时，发布点仍是同一个
`publishAll`，只是证据换成栅栏。

### 3.1.5 F5 目标 claim

- **行为**：帧首 claim 本帧目标；冲突 ⇒ 跳过那个 pass 并报一条；帧尾全部释放。
- **约束**（C5）：一个目标同一时刻只有一个会话；claim 是**租约**，不是锁。
- **要求**（R5）：一个目标的重建只由 claim 持有者做。
- **验收**（T5）：两会话画同一目标 ⇒ 一个会话那个 pass 被跳过并报一条（且只一条）。
- **注意**（P3）：任何提前返回都要经过同一个释放点（作用域守卫）。

**S5 落地（2026-11-15）**：`internal/TargetTable.hpp`——设备持一张 `RenderTarget* → FrameDriverId` 表；
`lease(driver)` 发一个帧作用域的 `Lease`，`Lease::claim(target)` 冲突时答 false，析构把本帧所有 claim 还回去
（`P3` 由类型强制）。`FrameDriver::executeFrame` 与 `prepareFrame` 都在 **`ensure` 之前** claim（重建是持有者的
动词，`R5`）；冲突只跳过那个 pass，报 `vk.target.claim_conflict` **每个（目标, 情节）一条**：情节在本帧/本次预热
无冲突地跑完时结束。窗口目标不 claim（`createSurfaceTarget` 已经保证一窗一会话）。
**验证**：`TargetTableTest` 8 例 + `TargetClaimFrameTest` 2 例（设备级：用一条冒充“另一个驱动者”的租约让 pass 被
跳过、两次只报一条、干净帧后新冲突再报）；变异 2/2 红（租约不释放 / 情节不结束）。
**B5a（2026-11-16）**：租约不再是第三张表——map、形状/构建与 claim 书籍同属 `internal/TargetTable`；
`release(target)` 一次做完“条目离开 map + 记录交给 park”，`drain()` 一次清记录与租约（+1 例钉住）。

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
  噪声，`G10`），读回路径不再碰 `FrameDriver` 的命令池与栅栏（C7 结构性成立）。
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
- **约束**（C9）：`api::WindowSession` 的**窗口**职责不动（交换链、`Viewer`、`WindowTarget` 已经是每窗口）。
- **要求**（R9）：`BackendParityTest` 的逐字节对拍在两会话下仍成立。
- **验收**（T9）：`test_vsg` 全绿 + 两会话用例；对拍不变。
- **注意**（P8）：`d->driving` 是"哪张图属于哪个会话"的**唯一**记录，搬走后不能再有第二份。

**S8a 落地（2026-11-16）**：

- **`FrameScratch`**（`internal/FrameScratch.hpp`）：arena / recorder / compiler / `facts` / `light_scratch` /
  `input_*` / `packets` / `frame` 收成一个类型；它借用设备的 `Diagnostics` 与 `Observe`（两者都比会话长寿）。
- **归属**：`api::WindowSession`（窗口）与 `api::OffscreenSession` 各持一份，`initialize()` 建、随会话销毁；
  `WindowSessionOptions` / `OffscreenSessionOptions` 新增 `observe`（null = 会话自备一份，独立建立会话的测试仍可用）。
- **设备只借一个句柄**：`Data::scratch` 在开帧时指向驱动会话的那一份，`ScratchCloser` 在**每个出口**清空；
  `commitFrameThrough` 从会话参数取回同一份——设备上不再有任何一份收集暂存。
- **设备级设置覆盖所有会话**：`setDefaultContentProgram` 与 `releaseRenderTarget` 对每个会话的 scratch 生效；
  开帧时再补一次默认程序（给设定之后才起来的会话），一个指针拷贝。
- **行为零变化**：`test_vsg` 557/557、`BackendParityTest` 13/13、`test_graphics` 587、`test_vk` 133；
  TSan（test_vsg 子集）0 报告。
- **后续已落地**：S8c 的两半（设备缓存的锁与帧首快照、帧闸的解禁）见下面的「S8c-1」与「S8c-2」小节。

**S8b 落地（2026-11-16）**：

- **`TargetTable`**：目标表与身份解析合为一（B5b）——设备唯一的一张表（一个身份一条：描述、对象、
  拥有者的标签，以及执行器的“没应用”情节）；每个驱动者的执行器借用它，所以两个驱动者解析同一张身份表（`P8`：
  没有第二份）。独立构造的执行器（测试）自备一份，`adopt` 登记别人建好的对象（非拥有）。
- **`VsgDriver`**（`internal/VsgDriver.hpp`）：`createSession()` 交出的帧驱动者，`RenderFrameExecutor` 的实现；
  自己拥有一个 `VsgExecutor`（`window_` / `window_recorded_` / `recorded_` / `profile_entries_` / `skipped_`）与
  它自己的默认内容程序；设备拥有它，`releaseSession` 销毁，`shutdown` 带走。
- **设备自己也只用一条路**：`VsgBackend::executeFrame` / `prepareFrame` 是两行转发，真相在
  `executeFrameFor(executor, ...)` / `prepareFrameFor(executor, ...)`；`Data::driving_executor` 像 `Data::scratch`
  一样在开帧时设、每个出口清，`presentWindow` 从它取回本次帧的驱动者。
- **默认程序归驱动（C8）**：`VsgExecutor` 持有对象与文本；设备级 `setDefaultContentProgram` 只设设备自己的驱动者，
  每帧开帧时把**驱动者自己的**文本交给驱动会话的 recorder；程序仍在设备 store 里登记（内容表是设备的）。
- **证据**：`VsgBackendTest.TwoDriversKeepTheirOwnDefaultProgram`——两个驱动者各自的默认程序互不覆盖，设备自己的
  驱动者没被污染；同一张离屏目标上，A 画红、B 画绿；释放 A 后 B 仍按自己的绿色出图。`test_vsg` **558/558**、
  对拍 13/13。变异：把 `VsgDriver::setDefaultContentProgram` 转发给设备自己的驱动者 ⇒ 两个像素断言红。

**S8c-1 落地（2026-11-16）：帧体共享设备，设备缓存进一把内容锁**

- **设备闸换了形状**：vsg 的设备闸是 **`graphics::FrameGate`**（S2 批 2 起与 vk 同一个类型、设备各有实例）——帧体取**共享**侧（两个驱动者的帧体可以同时进入），宿主动词取**独占**侧（initialize / shutdown / 读回 / `frameReport` / 窗口集 / resize）。从帧体线程调用的宿主动词得到**空租约**并回答拒绝（读回回答 `InsideFrame`，内容动词回答 false / 不动并报一条 `vsg.device.reentry`），从不等待自己。诊断路由与 episode 锁是**递归**的，因为 sink 本身可能回调。
- **帧状态归驱动**：`VsgExecutor::FrameDrive`（scratch / 本次窗口索引 / 上传基线 / 上一帧的 delta）取代 `Data::{driving, scratch, driving_executor, uploads_before, last_uploads}`（最后一项保留为设备的“最后一帧”副本）。`presentWindow` 通过**线程本地的在飞帧**（`FrameInFlight`）取回本次帧的驱动者——窗口目标的 `present()` 没有帧参数，设备级单份会把一帧的计划交给另一驱动的 present。
- **帧首快照（vsg 的形状）**：`openFrameHead` 在**一次**内容锁下解析这一帧会读的所有**设备事实**——每个 pass 的编号（`PassRegistry::adopt` 写入 `FrameScratch::pass_ids`）、它画的每个宿主目标（`adoptHostTarget`）、它采样的每个目标（`TargetTable::observe`）、它命令里点名的内容（`ContentStore::track`）、目标事实表的副本（`FrameScratch::facts`）、staging 清扫与块存储的增长。**收集**（`collectFrame`）与**计划编译**（`closeFrameBooks`）随后只读会话自己的 scratch，因此它们里的报告（计划编译器的“这个输入/目标解析不了”）到达 sink 时**不持任何设备锁**。
- **内容相是设备唯一的串行段**：`commitFrameThrough` 在**一次**内容锁下做 `applyTargetPlans` / 材质 arena 增长 / `recordContent`（assembly 开帧 + 录制）/ `executor.record` / 清扫 / 会话提交，并在每个出口发布本帧的上传 delta 与计数器（`Data::last_uploads` / `last_counters`）。
- **每会话一个帧主张**：窗口槽与离屏会话各一把 `frame` 互斥锁，帧体持到结束——同一窗口/离屏会话的两帧**等待**（与旧设备闸同语义：两个引擎都离屏出图是受支持的形态，不能拒绝），**不同窗口互不等待**。
- **计数器归会话**（`WindowSessionOptions::observe` 不再借设备的 `Observe`）：recorder 开帧会**重置**计数器（`FrameRecorder::beginFrame`），共享一份会让两个驱动者互相覆盖；`frameReport()` 引用**最后结束**那一帧的副本（`Data::last_counters`），retention 仍是设备的。
- **一处真实竞态（TSan 之前的用例先抓到）**：两个驱动者并发地**首次**运行离屏帧时，`ensureOffscreenSession` 的无锁惰性创建让输家覆盖半建的 viewer（段错误）。修 = 创建在内容锁下；这同时说明惰性设备件必须与帧首解析使用同一把锁。
- **`ContentStore` 的行/表由内容锁串行，而记录不再碰它们**：S8c-1 时 vsg 的**记录路径仍读设备表**（halves / sets / uploads / images / 材质槽都在 `recordContent` 里查），所以 `C3` 在那一步没有满足；**S8c-2 把它收口**（见下一小节）。
- **证据**：`test_vsg` **561/561**、`BackendParityTest` 13/13、TSan（vsg 子集）**0 报告**、ASan 新路径 PASS。新增三例：`ACompilingFrameDoesNotWaitForAnotherDriversFrame`（一帧被 sink 停在**编译**里时，另一驱动者的整帧完成；变异：设备闸改回独占 ⇒ 5 s 超时红）、`TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`（两线程各 3 帧、各画各的默认程序，读回各是自己的颜色）、`AReadbackFromInsideAFrameBodyAnswersInsideFrame`（sink 里读回答 `Failed`，一次一条 `vsg.readback.in_frame`）。

**S8c-2 落地（2026-11-16）：记录路径零锁（`FrameHead`）**

- **内容相拆成三段**：`commitFrameThrough` 现在是 **解析**（一次内容锁：`applyTargetPlans`、材质 arena 增长、`resolveContent`）、
  **录制**（无设备锁：`FrameHead::record` 把值变成 vsg 节点）、**提交**（再一次内容锁：`executor.record`、清扫、会话提交、
  上传 delta 与计数器发布）。“记录也要读表”不再是前提，`C3` 对 vsg 自此成立。
- **`internal/FrameHead`（S2 批 2b 前叫 `ContentPlan`）：一帧解析出的值**。每个绘制调用一个 `DrawPlan`：它命中的 half 的录制器（`ContentDraw*`）、已解析的
  `ContentDraw::Draw` / `ScreenDraw`（身份 + pipeline + 变体 id + 动态状态 + 几何 span 计数）、块绑定、推送块、顶点/索引绑定；
  每个 pass 一个槽，自带它的 `core::StateRegistry`（跨“解析/录制”两相的状态内存）。它是 vk `FrameHead::DrawPlan` 的 vsg 对形：
  **值，且拥有它点名的对象**（全部 `ref_ptr`），生命周期上还有一个钉：`ProgramHalves::pin()` 把 halves 的生产者租给计划，
  另一个驱动者的帧首清扫就不会释放这一帧还要用到的录制器（退役 park 不管“尚未提交的帧还没交上去”这件事）。
- **解析半与录制半分开的两个 API**：`ContentDraw::plan(key)`（解析，取 layer 与 variant pool，在内容锁下）与
  `ContentDraw::record` / `recordScreen`（录制，只绑已解析的 pipeline，不碰 layer、不碰 pool）；`core::StateRegistry`
  因此多了 `resolve(variant, …)` 重载——录制用品值而不是回查 pool，这也是两个驱动者的录制可以真正同时跑的前提。
  `ContentPass::resolve` 取代了带副作用的 `record`（旧签名保留为“解析 + 录制”的便利入口，测试与 `ContentAssembly::record` 用它）。
- **块也是在帧首写的**：view / lights / shadow / draw / material 块的偏移在解析时写进本帧 slab（`BlockStorage` 的轮转本来
  就是帧首的事），录制只带偏移——所以另一个驱动者的帧首在中间轮转环、清扫 halves、增长 arena，都不影响这一帧的计划。
- **接口与证据**：`BackendContentAccess::setRecordSeam`（测试接缝，录相开始处、不持内容锁）；
  `VsgBackendTest.ARecordingFrameDoesNotStopAnotherDriversContent`——一帧被停在**录制**里时，另一驱动者的整帧（帧首 + 录制 +
  提交）完成，且被停帧读回的像素仍是自己的（计划的值跨过另一帧的帧首仍有效）；变异：三相位改回 S8c-1 的单锁 ⇒ 5 s 超时红。
  `test_vsg` **562/562**、对拍 13/13、TSan vsg 子集 0 报告、ASan 新路径 PASS。

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
| `FrameDriver` | 帧机器 + 帧暂存 + 帧闸 + 帧账 + claim 集合 | 身份是 `graphics::FrameDriverId`（`DeviceClock` 发） |
| `ContentCaches` | programs / geometries / pipelines / 布局 / 图像 / 分配器 + 一把独占锁 | 从 `ContentDraw` 拆出 |
| `FrameScratch` | 环 / 描述符池 / set 表 / `FrameSnapshot` | 从 `ContentDraw` 拆出 |
| `ResidenceTable` | 三态 + 发布/撤回/退休 | 复用 `RetirementQueue` 的 epoch 语义 |
| `ContentCaches::FrameBuilds` | 一帧的容纳结果：`Use` 命中、`Build` 的 claim、`Copy` 的临时副本 | 帧作用域值；`publishAll` 在帧完成后调用，析构收拾其余 |
| `TargetTable` | 目标 → 会话；try-claim / release | 冲突报一条，按 (目标, episode) 去重 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 会话由**设备**拥有；引擎只借（`createSession` / `releaseSession`）。 |
| C2 | 帧机器（命令池 / 栅栏 / 环 / 描述符池 / 帧账）**只**在会话里。 |
| C3 | 记录路径**不得**取设备缓存的锁；设备缓存的读只发生在帧首快照。vk 自 S6c-1、vsg 自 S8c-2（`FrameHead`）起成立：两边的记录都只读帧首解析出的值。 |
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

- **会怎么错**：`Device::FrameDriver` 是设备的**内嵌**类，拥有对 `Device` 私有成员的访问权——但 `reportDiagnostic` 是
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
| D7 | 现在就把 `DeviceClock::WindowId` 改名 `SessionId` | `G8`：没有消费者需要这个拼写；先用类型别名（`SessionId = WindowId`），重命名留到有第二个 API 家族时一起做（**已由 `ADR-2026-11-16-redesign-seam-and-vocabulary` 取代**：批次 1 把三个拼写统一成 `FrameDriverId`，`WindowId` 只留一版 `@deprecated` 别名） | 本文 §2.6 |
| D8 | 一次落地（不做分阶段） | `G11`：半拆的树比没拆更糟——每一步都要能单独验证 | ADR §9 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 单会话后端行为零变化 | `build/bin/test_graphics`、`test_vsg`（默认 `createSession`） |
| T2 | `test_vk` 在"只有一个会话"的树上全绿 | `build/bin/test_vk` |
| T3 | 记录路径的缓存锁为 0；整帧获取次数**不随 draw 数增长**（实测 4：帧首 1 + 数簿 2 + 帧尾 1） | `test_vk` `VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls`（1 条 vs 6 条命令；变异 6 → 10 红） |
| T4 | 两会话同时首画同一几何：不崩、两图都对、上传 ≤ 2 | `test_vk` `VkResidenceWireTest.TwoDriversFirstDrawingOneGeometryShareOneEntryAndAtMostOneCopy`（真两会话两线程：claims 1 / published 1 / copies ≤ 1 / 两图红 / 随后帧零上传）、`AKeyAnotherDriverIsFillingIsCopiedForThisFrameAndDiscarded`（假租约：复制、恰好两条流、`used_bytes` 回位）、`ARefusedBuildIsWithdrawnInsteadOfPublished`、`AReleasedGeometryIsForgottenSoTheNextFrameMayBuildItAgain`；变异 3/3 红（不问表 / 发布失败的填充 / 释放不 forget） |
| T5 | 两会话画同一目标：一个 pass 被跳过 + 一条报告 | `test_vk` 新用例 |
| T6 | 释放与在飞帧重叠：释放不等到帧结束，帧仍用已解析的句柄完成，对象 park 到水位，`deviceWaits()` 不涨 |
  `VkMultiSessionTest.AReleaseDoesNotWaitForTheFrameInFlightAndDoesNotDestroyWhatItUses`（变异：释放重新取体闸 ⇒ 红） |
| T7 | 交替画不同目标 + 穿插读回：各读各的最新完成帧 | `test_vk` 新用例 |
| T8 | 两会话不同默认程序：各画各的 | `test_vk` 新用例 + 变异（共用单槽 ⇒ 红） |
| T9 | 两会话下 `BackendParityTest` 逐字节对拍不变 | `test_vsg` |
| T10 | 两会话各一条线程各 N 帧：两张图各自正确，重入被拒绝；**帧体时间上真的重叠**（一帧被 sink 停住时另一帧整帧完成） | `VkMultiSessionTest.TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`（各持一张自己的贴图）、
  `VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame`（变异回独占闸 ⇒ 红）；T47 的“同线程重入被拒”断言保留 |
| T11 | 数据竞争有工具证据（已跑，2026-11-16：三套 0 报告） | `scripts/tsan_check.sh`（`test_vk` / `test_graphics` / `test_vsg` 子集） |
| T12 | 文档与代码一致 | `python3 scripts/check_ai_docs.py --strict`、`python3 scripts/check_design_docs.py` |
| T13 | vsg：一帧被 sink 停在**编译**里时，另一驱动者的帧整帧完成；一帧被停在**录制**里时，另一驱动者的整帧（帧首 + 录制 + 提交）完成且被停帧像素仍正确；两线程各持自己的目标各画各的；帧体内读回答 `InsideFrame` | `test_vsg` `VsgBackendTest.ACompilingFrameDoesNotWaitForAnotherDriversFrame`（变异：设备闸改回独占 ⇒ 5 s 超时红）、`VsgBackendTest.ARecordingFrameDoesNotStopAnotherDriversContent`（变异：三相位改回单锁 ⇒ 5 s 超时红）、`TwoDriversOnTwoThreadsEachKeepTheirOwnPicture`、`AReadbackFromInsideAFrameBodyAnswersInsideFrame` |

## 9. 状态与演进

### 9.1 当前状态

本页描述的形状已全部落地：`T47` 的设备级帧闸，`T48` 的 S1–S9 与 `F4` 的驻留接线，两个后端共用同一套契约。
落地顺序、每一步的用例与变异证据、当天的 TSan 读数都是历史，记在
`../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` 的「实施状态」与 git 历史里；
本页只保留仍然约束今天代码的部分：§2 的设计要点、§3 的功能点、§4 的接口与 §5 的约束。

### 9.2 已知缺口 / 待办

- `DeviceClock` 的动词仍叫 `addWindow` / `windowCompleted`（身份类型已是 `FrameDriverId`）；动词改名留一版。
- **T3 的准确读法**：整帧固定 4 次获取（帧首 1 + 数簿 2 + 帧尾 1），用例是 `VkFrameHeadTest`；
  「稳态帧 0 次」指的是记录路径，不是整帧（见 §2.3 与 §8 T3）。

### 9.3 兼容性 / 迁移

迁移已完成，没有待做的步骤。步骤的划分、顺序、每一步的验收与回滚条件属于决策记录，
见 `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` §9「分两步落地」与「实施状态」；
今天仍然约束代码的两条事实留在设计侧：释放是两件事（§2.6）、读回等设备水位并计数（§2.7）。
触发型的后续工作见 §9.2 与 `../backlog/graphics.md`。

## 10. 参考

### 10.1 代码

- 契约：`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`RenderBackend.hpp`
- 机制：`src/viz/graphics/sdk/vine/graphics/FrameGate.hpp`、`DeviceClock.hpp`、`RetirementQueue.hpp`、`FrameTimeline.hpp`
- vk 的现状：`src/plugins/gfx_backend_vk/src/internal/Device.cpp`（`FrameDriver` 的帧机器）、
  `src/plugins/gfx_backend_vk/src/internal/ContentDraw.cpp`（记录器与缓存接线）
- vsg 的现状：`src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`（`Data` 的帧暂存单例）、
  `src/plugins/gfx_backend_vsg/src/internal/WindowSession.cpp`（窗口会话）、`src/plugins/gfx_backend_vsg/src/internal/OffscreenSession.cpp`
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
