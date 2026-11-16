# ADR-2026-11-15：设备是资源池，会话拥有帧 —— 两个引擎共享内容、各自出图

- 状态：已采纳（本文定**契约与形状**；落地分两步，见 §决定 第 9 条。第一步 `T47` 已落地 2026-11-15；第二步
  `T48` 的 vk 侧 S1–S7 / S9 已落地 2026-11-16——见文末「实施状态」）
- 日期：2026-11-15
- 影响范围：`../design/graphics-threading.md`（新增 §2.16 / `C21` / `R18` / `N18` / `D13`）、
  `../design/vk-backend.md`（新增 §2.11 / `§9.2` 缺口）、`../backlog/graphics.md`（`T47` / `T48`）、
  `RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`RenderEngine.hpp` 的契约文本与实现
- 相关：`ADR-2026-11-13-one-engine-one-surface.md`（一引擎一表面——本文是它的下一步，不是它的反面）、
  `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`（一帧在飞；本文与它**共用**退休机制）

## 背景

需求（2026-11-15，用户原话）：**geometry / shader / 纹理资源共享，帧不共享；共享之后要线程安全；
两个 engine 可以同时 frame。**

已经成立的一半，是"共享"：

- 一个 `RenderDevice` 被多个 `RenderEngine` 借用**已经支持**：设备由创建者 `initialize()`，引擎只借用
  （见 `RenderEngine::setDevice` 的契约），`tests/test_graphics/HeadlessBackendTest.cpp` 的
  `OneDeviceServesSeveralAreasWithoutCrossingThem` 就是两个引擎、一个设备、两个区、两个目标。
- 但那条用例的两帧是**先后**跑的（`engine_a->frame(); engine_b->frame();`）——用例名里的
  "without crossing them" 正是这个意思：**共享已证，并发未证。**
- 内容驻留本来就按**身份**缓存（几何按 `GeometryDescription` 的地址 + revision，程序按描述 + variant 位，
  纹理按 `Texture` 的地址 + revision），所以同一份几何在两帧里是**同一个键**：共享在键这一层已经是同一个概念。

不成立的一半，是**帧**：一个设备只有一个 `RenderFrameExecutor`（`executor()` 的契约明写"是属性，不是状态"），
而它身下是**一个命令池、一个栅栏、一个常量环、一个描述符池**。两个引擎同时 frame，就是两条线程往同一份帧状态里写。

### 挡路的具体事实（都可在代码里核对）

| # | 事实 | 位置 |
|---|---|---|
| 1 | 一个设备只答一个执行器，且契约说它"不随会话变化" | `RenderDevice.hpp` 的 `executor()` |
| 2 | 一帧的机器是**设备级**的：一个 `command_pool` + 一个 `completion` 栅栏，帧路径**和**两条读回路径共用 | `Device.cpp`（`Impl` 的 `command_pool` / `completion`；提交与等待在 `executeFrame` 内，读回在 `:2434` / `:2566` 传同一个 `impl->completion`） |
| 3 | `ContentDraw` 一个对象里同时装**缓存**（模块 / 流 / 管线 / 布局）与**帧状态**（常量环、描述符池、逐 draw UBO） | `ContentDraw.hpp` 的 `Impl` |
| 4 | 帧边界重置的是**设备级**状态：`beginFrame()` 重置描述符池，`endFrame()` 释放逐 draw UBO | `ContentDraw.hpp` 的 "FRAME BOUNDARY" 段 |
| 5 | 内容缓存**没有"正在上传"这个状态**：miss 时把拷贝录进当前帧的命令缓冲，条目随即出现在表里 | `ContentDraw.cpp` 的 `resolve*` / `prepare*` |
| 6 | 默认内容程序是**设备级单槽**：第二个引擎 `setDevice()` 就把第一个的换掉 | `RenderDevice.hpp` 的 `setDefaultContentProgram`、`RenderEngine.cpp` 的 `setDevice` |
| 7 | `releaseContent` / `releaseRenderTarget` 的说明是"在帧之间调用"——今天成立**只因为**一帧在飞 | `Device.hpp` |
| 8 | 三处契约文本写着"一次一帧、一条线程"：设备、执行器、插件设备 | `RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`Device.hpp` |

**一处更正**：早先的评估说"诊断路由也是设备级单槽"，那是错的。`setDiagnosticsRoute` 明确"每个报告到达
**每一条**已安装的路由，第二个引擎不会拿走第一个的计数"，访问器从设备自己的路由答——**路由已经是多路的**。
真正的单槽只剩第 6 条（默认内容程序）。

## 问题必须拆成三个，因为三个答案不一样

| 问题 | 答案 | 依据 |
|---|---|---|
| **什么是可共享的？** | **上传之后不再改变**的 GPU 资源：顶点/索引流、着色模块与管线、纹理与采样器，加上设备本身（instance / physical / logical / 队列 / 内存池 / 管线缓存文件） | 缓存键含 revision ⇒ 条目**不可变**；不可变的东西没有"谁的"问题 |
| **什么不能共享？** | 任何**有 layout** 或**有可变宿主可见区间**的东西：目标与它的附件、交换链、命令池与命令缓冲、栅栏、描述符池、常量环 | 一张图像在任一时刻只有一个 layout；常量环是"帧内写、帧内读"的暂存 |
| **谁能同时碰共享部分？** | **按资源限制，不按线程限制**：不可变资源任意会话并发读；可变共享状态（管线缓存、队列、分配器、缓存表）各自一把锁；每会话的东西线程限定 | 见 §Vulkan 的硬约束 与 §锁表 |

这个拆法就是 `G1`：三个问题各有一个概念、一个 owner、一条边界。把"线程安全"当成一个整体来加锁，
既是 `G8` 的反面（为不存在的写者协调），也是 `G10` 的反面（把成本加到热路径上）。

## Vulkan 的硬约束（不是我们的口味，是规范的）

Vulkan 的设备**本身**不是"一次一个线程"：多数入口可以并发调用，规范只对**具体对象**要求外部同步。
这条决定了形状——共享是合法的，需要加锁的是少数几个对象：

| 对象 | 规范要求 | 我们的做法 |
|---|---|---|
| `VkDevice`（一般入口） | 并发调用合法 | 不加全局锁（**否决 D13**） |
| `VkQueue` | 外部同步（同一队列的提交必须串行） | 一把**提交锁**；提交是 O(1) 的驱动调用，设备执行在锁外 |
| `VkPipelineCache` | 外部同步（`vkCreateGraphicsPipelines` 传入时） | 一把**管线锁**；只在 miss 时取，稳态零次 |
| `VkCommandPool` / 命令缓冲录制 | 外部同步（按池） | **每会话一个**池，无锁 |
| `VkDescriptorPool` | 外部同步（按池） | **每会话一个**池，无锁 |
| `VkFence` / 信号量 | 外部同步（按对象） | **每会话一个**，无锁 |
| `VkPipeline` / `VkPipelineLayout` / `VkDescriptorSetLayout` / `VkSampler` / `VkImageView` | 创建后只读，可并发使用 | 共享，不加锁 |
| `VkBuffer` / `VkImage` / `VkDeviceMemory` | 区间不重叠即可并发；**layout 是图像的状态** | 上传后只读；目标由一帧 claim（见 §决定 第 4 条） |

`VkDeviceMemory` 那一行是本设计的支点：**内存可以共享，只要两个使用者不在同一段字节上写**。
驻留条目上传完就不再写，所以"共享显存"与"并发记录"不冲突。

## 业界怎么分（可参考的部分，标注是否在本仓核对）

| 系统 | 设备 | 帧 / 命令 | 共享的边界 | 核对 |
|---|---|---|---|---|
| **VSG 1.1.16**（本仓参考后端） | `vsg::Device`；`Context` 在它身上建**两个设备级** `MemoryBufferPools`（device / staging），池内 `mutable std::mutex _mutex` | 每个 `RecordAndSubmitTask` 自己一份（`numBuffers = 3`，`_fences` 每槽一个） | **设备级池 + 每任务帧状态**——与本文同形 | 已核对：`MemoryBufferPools.h` 第 69 行 `mutable std::mutex _mutex;`；`Context.cpp` 把两个池挂到 `device->...BufferPools`；`RecordAndSubmitTask.h` 持 `std::vector<ref_ptr<Fence>> _fences` |
| **O3DE RHI** | `Device` 线程安全；`PipelineLibrary` 共享并加锁 | `CommandList` 每线程一个 | 同上 | 外部资料，未在本仓核对 |
| **wgpu** | `Device` / `Queue` 是 `Send + Sync` | `CommandEncoder` 每线程 | 同上 | 外部资料，未在本仓核对 |
| **Filament** | `Engine` **明确 not thread-safe**，`Driver` 单线程 | 全部在渲染线程 | 不共享 | 外部资料；**反例**：代价是"一引擎一设备" |
| **bgfx** | `bgfx::frame()` 单线程 API，内部渲染线程 | 内部 | 不共享 | 外部资料；同为反例 |

结论不是"业界都这么做"，而是**分界一致**：**设备是池（带锁），帧是每线程状态。**
分歧只在"API 是否把第二个帧驱动者暴露出来"——Filament 与 bgfx 选择不暴露（一个引擎一个设备），
我们选择暴露，因为需求就是"两个引擎、一个设备、两份内容只传一次"。

**一个必须说清的差别**：VSG 的 `MemoryBufferPools` 是**设备级**的，但 `Viewer` 建自己的池是常见用法——
也就是说 VSG 共享**设备**，未必共享**驻留**。本仓要的更强：同一份几何在两个引擎里**只上传一次**。
这正是本文是框架决策、不是"照抄 VSG"的原因。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| **A 只写规范，代码不动**（"两个引擎可以共用一个设备，但帧必须串行"） | 零改动；契约与代码一致（`G6`） | 需求没实现；而且**今天连串行都不保证**——两条线程同时 `executeFrame` 是数据竞争，不是"慢一点" |
| **B 设备级帧锁**（一把 `std::mutex` 包住 `executeFrame`） | 小改动（一把锁 + 契约 + 一条用例）；把"静默竞争"变成"正确串行"；不改任何公开契约（`G4` / `G11`） | 两个引擎互相等一整帧（本机约 8.6 ms）；`G8` 说这是**过渡**不是终点 |
| **C 设备 = 池，会话拥有帧**（本文） | 帧路径**零锁**；共享只在 miss 时付费；两个引擎真正并行；形状与 VSG / O3DE / wgpu 一致（`G1` / `G5` / `G9`） | 要拆 `ContentDraw`、加驻留状态机、加三把锁、加目标 claim；约 1350–2100 行、12–18 个文件 |
| **D 每引擎一个设备**（不共享） | 零风险；今天就能做（`SurfaceWindow` 正是这样：每窗口一个设备） | 需求的反面：同一份几何上传两遍、管线编译两遍、显存两份；`G5`（该共享的被切开） |
| **E 全局锁包住每个设备动词** | 写起来最省事 | 把 `G7` 反过来：复杂度没被吸收，而是**转移**成"每次调用都可能阻塞"；且锁的粒度与资源无关，`G1` 失败（"线程安全"被当成一个概念）。**否决 D13** |
| **F 按资源的读写锁**（每条目一把 `shared_mutex`） | 听起来最精细 | 今天的共享资源**创建后不可变**，没有"写者"要协调；per-entry 锁只会在 2000 draw 的路径上付 2000 次原子操作（`G10`），解决一个不存在的问题（`G8`） |

## 决定

**采纳 C，并以 B 作为第一步的落地形态**（先让现状正确，再让并发成立）。

### 1. 名词：设备是池，会话拥有帧

| 名词 | 是什么 | 谁拥有 | 并发性 |
|---|---|---|---|
| `RenderDevice` | **池**：GPU、内存分配器、内容驻留缓存、目标、管线缓存、诊断路由 | 创建者（宿主） | 共享；内部**按资源**加锁 |
| `Session`（今天叫 `RenderFrameExecutor`） | **一帧的执行者**：命令池 + 命令缓冲 + 栅栏 + 常量环 + 描述符池 + 帧账 | 一个引擎 | **线程限定**：同一时刻只有一条线程 |
| 驻留条目 | 一份内容在 GPU 上的副本，键 = 身份 + revision | 设备 | 上传后**不可变**；`Resident` 可被任意会话并发读 |
| 目标 | 一组有 layout 的图像 + 它的 render pass / framebuffer | 设备建，会话借 | 一帧内被**一个**会话 claim |

`RenderFrameExecutor` **不新增类型**：它就是会话这个概念的既有名字，变的是"谁拥有它、有几个"。
`RenderDevice::executor()` 保留，含义收窄为"**没有引擎驱动时**设备自己的那个会话"——与
`setDiagnosticsRoute` 已有的同一条约定同形（"没有引擎驱动的设备从它自带的私有路由答"），`G6`。

### 2. 共享面与非共享面（一句话各自可判）

- **可共享** = 上传后不再改变的 GPU 资源 + 设备对象。判据：**它的键含 revision，且它的 layout 不再变。**
- **不可共享** = 有 layout 的目标、交换链、命令池/缓冲、栅栏、描述符池、常量环。判据：**它在一个帧的生命周期内被写。**
- **窗口仍然一窗一目标**（`createSurfaceTarget` 的 `WindowOccupied` 规则不变）：两个引擎不能画同一个窗口，
  这条是平台约束，不是本 ADR 的取舍。

### 3. 驻留状态机：`Absent → Uploading(owner, frame) → Resident`，只有 `Resident` 可画

今天的缓存只有"在表里 / 不在表里"两态，因为填充发生在**唯一**一帧的命令缓冲里。两个会话之后必须有第三态：

```
Absent ──第一个需要的会话 claim──▶ Uploading(owner, frame)
                                        │ owner 的帧完成（今天的同步模型：executeFrame 返回时已 wait 过栅栏）
                                        ▼
                                     Resident ──releaseContent──▶ Retiring(epoch) ──所有会话 completed ≥ epoch──▶ 释放
                                        ▲
                     竞争者看到 Uploading：在自己的帧里建一份**临时副本**（不发布、帧末丢弃），照常画
```

三条规则，缺一条都不成立：

1. **一个键只有一个发布者。** claim 是原子的；输的一方**不发布**，只在本次帧内用自己的副本。
2. **`Resident` 只在填充可证明完成之后发布**——今天就是 `executeFrame` 返回（它内部 `vkWaitForFences`），
   预热路径就是那次只含传输的提交返回。多帧在飞之后（F5），发布点变成"那一帧完成"，与退休同一个机制。
3. **条目不可变**：键含 revision，所以 `Resident` 的条目永远不会被改写；新 revision 是**新条目**。
   这条是既有事实（`ContentDraw` 的键），本 ADR 只是把它写成前提。

**为什么竞争者是"复制"而不是"等"**：等待是会话之间的阻塞，是唯一能把两个会话**锁死**的东西
（A 等 B 的帧，B 等 A 的帧），而复制是有界的——每个身份至多 `N−1` 份临时副本，只在"两个引擎第一次同时画它"
的那一帧发生，之后都是命中。`prepare()` 在加载期就把这件事做掉了，所以真实宿主通常一次都碰不到。

### 4. 目标：一帧 claim，冲突只跳过那个 pass

`executeFrame` 拿到的 `FrameDescription` 已经列出每个 pass 的目标，所以 claim 不需要宿主配合：
帧首对**它将要画的目标集合**取 claim，帧尾释放。冲突（另一个会话在飞且名字里有同一个目标）时
**只跳过那个 pass**，按 `(目标, episode)` 报一条——与既有的"这个设备建不起这个目标，报一次并跳过"
**同一形态**（`G6`：不新增失败形态）。整帧不因此失败：一帧里其余 pass 与它无关。

目标由**设备**建、按形状缓存（`planTarget` 的 Rebuild / ResizeInPlace / Repair 不变），
所以两个引擎画**不同**目标时，附件、render pass、framebuffer 仍然只建一次。

### 5. 释放：park + 所有会话的 `completed ≥ epoch`

`releaseContent` / `releaseRenderTarget` 今天写"在帧之间调用"，是因为一帧在飞。两个会话之后，"帧之间"
不再是一个时刻。做法沿用 `RetirementQueue` 的既有形状（它已经是"什么时候可以毁"的决策队列）：

- 释放请求记下**当时每个会话的 `submitted`**（`FrameTimeline` 的既有计数器）；
- 当**每个**会话都报 `completed ≥` 它记下的值，才真正销毁；
- 因此 `releaseContent` 可以在另一个引擎正在 frame 时被调用——这正是本 ADR 想要的场景（宿主在编辑，
  后台在烘焙），而不是一个需要宿主管住时序的动词。

### 6. 默认内容程序归会话，设备只留"没有引擎时"的 fallback

第 6 条单槽的直接后果是：两个引擎的 program-less 内容会被**另一个引擎的**程序着色，且没有任何报告。
修法与诊断路由同形：程序是**会话的**（每个引擎一个），设备保留一个自己的，只服务于**直接驱动设备**的
调用方（`test_vk` 就是这样用它的）。`RenderEngine::setDevice()` 不再向设备宣布它，而是在 `initialize()`
把它交给自己的会话；运行中 `setDefaultContentProgram` 只影响自己那个会话。

### 7. 锁表（哪一把锁、护什么、为什么便宜）

| 共享物 | 保护 | 何时取 | 成本 |
|---|---|---|---|
| `VkQueue` 提交 | 提交锁 | 每帧一次 | 驱动调用级；设备执行在锁外 |
| `VkPipelineCache` | 管线锁 | 只在管线 miss | 稳态零次 |
| 内存分配器 | 分配锁 | 只在上传 miss | 稳态零次 |
| 驻留缓存表（程序 / 流 / 管线 / 图像） | 缓存锁（读写锁） | **帧首一次**：把本帧要用的身份解析成会话本地的 `Resolved` 表 | 每帧一次，不是每 draw 一次 |
| 目标表 / 每目标的附件 | 目标锁 + 每目标 `building` 标志 | 建与查时 | 帧首一次 |
| 上传暂存块 | 暂存锁 | 只在 miss | 稳态零次 |
| 每会话：命令池 / 栅栏 / 描述符池 / 常量环 / 帧账 | **无锁**（线程限定） | — | 帧路径零原子操作 |
| 每会话的 `submitted` / `completed` | 原子 | 写：会话线程；读：退休判定 | 只在释放时读 |

**记录路径（每 draw）不加锁**，这是整个设计成立的条件，也是 `G10` 的账：一帧 2000 draw，
若每次取锁按 20–40 ns 算是 40–80 µs，占本机宿主侧 ~0.9 ms 的 5–9%——不可接受，所以帧首快照一次。

### 8. 契约文本要改的三处

`RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`Device.hpp` 的"一次一帧、一条线程"改成：

- **设备**：可被多个引擎借用；内容驻留共享（一次上传、一次编译）；**改变共享状态的宿主动词分两类**：
  - **按会话的**（`setRenderTarget` / `releaseRenderTarget` / `setDefaultContentProgram`）不得与**它自己那个
    会话**的帧并发，但不受别的会话影响；
  - **设备级的**（`releaseContent`）可以在任意会话在飞时调用——它只 park，不立即销毁（§决定 第 5 条）。
- **会话**：线程限定；两个线程同时进**同一个**会话是契约违反（有守卫，报一条并拒绝）。
- **两个会话可以同时 frame**：它们共享的只有只读内容；不得共享的是一个目标与一个窗口。

**第一步与第二步的区别要写进同一段**：第一步的实现在设备里把两个会话**串行**（帧闸），第二步才让它们
并行——所以契约现在就可以写"可以同时调用"，而"同时"的代价随第二步消失（`G4`：说清楚代价属于哪个版本）。

### 9. 分两步落地

**第一步（`T47`，低风险，先落）**——让现状**正确**、契约**诚实**：

1. 设备级帧闸（`std::mutex` + 记录持有线程）：跨线程**串行**；同线程重入（一个 `before_draw` 钩子
   去驱动另一个引擎）报一条并跳过，Debug 断言。今天这种重入会直接写坏同一个命令池。
   **闸同时护 `prepareFrame`**（它也会提交一个只含传输的批，见 §决定 第 10 条）。
2. 第 6 条单槽**写进文档**（"一个设备一个默认程序；两个引擎声明不同的默认程序时不组合，后声明的生效"），
   不在第一步半修——结构性的修法在第二步，半修会造出一个第二步要拆掉的概念（`G8`）。
3. 三处契约文本（§决定 第 8 条）。
4. 用例：两个引擎、一个设备、两条线程、各 N 帧 ⇒ 不崩、各自画面正确、诊断 ≤ 1 条；
   变异：拿掉帧闸 ⇒ 用例必须红（ASan / TSan 至少一种）。

**第二步（`T48`）**——让并发**成立**：

| 改动 | 文件 | 量级 |
|---|---|---|
| `createSession()` 动词 + 引擎持有自己的会话 | `RenderDevice.hpp`、`RenderEngine.cpp/hpp`、`RenderBackend.hpp` | 200–350 行 |
| `ContentDraw` 拆成 `ContentCaches`（设备）+ `FrameScratch`（会话） | `ContentDraw.hpp/.cpp`、`Device.hpp/.cpp` | 500–700 行（多为搬家） |
| 驻留状态机 + 帧首快照 | 同上 + `Keys.hpp` | 200–300 行 |
| 三把锁（提交 / 管线 / 分配）+ 目标 claim | `Device.cpp`、`MemoryAllocator.cpp` | 150–250 行 |
| 多会话退休（§决定 第 5 条） | `RetirementQueue.hpp`、`FrameTimeline.hpp` | 100–200 行 |
| 用例（`test_vk` 两会话 / `test_graphics` 两引擎 / TSan） | `tests/test_vk`、`tests/test_graphics` | 200–300 行 |

合计 **约 1350–2100 行、12–18 个文件**。vsg 后端**更接近**：它已经是"设备 + 每会话命令图"
（`VsgBackend` 的 `Session` / `OffscreenSession`），缺的是设备级驻留共享与退休跨会话。

### 10. 三个设备级事实不因拆分而变

- **设备丢失是设备级的**：一个会话看到 `VK_ERROR_DEVICE_LOST`，`initialized()` 对**所有**会话转 false，
  重建路径仍是 `shutdown()` + `initialize()`。两个引擎不会"一个死一个活"——它们共用同一个 `VkDevice`，
  那不是失败分类能分开的事。
- **能力是设备级的**：`capabilities()` 只读，任意会话任意时刻可问（它决定一帧怎么被驱动，见 `RenderEngine::frame`）。
- **预热与帧走同一扇门**：`prepareFrame` 也会提交一个只含传输的批（`ADR-2026-11-14-prepare-may-submit-transfers.md`），
  所以第一步的帧闸必须**同时护它**，否则一条线程的预热会和另一条线程的帧撞在同一个命令池上；第二步里
  它自然变成会话动词。

## 后果

- **好的**：
  - 帧路径**零锁**，且这不是优化而是结构：共享资源上传后不可变，所以"并发"不需要在热路径上协调（`G1` / `G10`）。
  - 名词与业界分界一致（设备 = 池，帧 = 每线程状态），换后端时这条不变（`G9`）。
  - 第一步就能把今天的**静默数据竞争**变成正确串行 + 一条诊断（`G4` / `G11`）。
  - 与 F5（多帧在飞）**共用**同一套退休机制：先做哪个都不返工。
- **坏的 / 代价**：
  - 第二步是跨 5 个模块的改动（SDK 形状、引擎、vk 插件、退休机制、用例），`G5` 的账要按模块算。
  - 常量环从"一个设备一份"变成"一个会话一份"：显存乘上会话数（每会话按该会话一帧的 draw 数增长）。
    这是"帧不共享"的直接价格，`G10` 记账而不是隐藏。
  - 第一次同时画同一份内容的那一帧，竞争会话会多上传一次（有界，见 §决定 第 3 条）。
  - 本机（lavapipe）上**几乎测不出收益**：软件光栅的"设备时间"就是 CPU 时间，两个会话抢同一批 CPU。
    收益的真实位置是"设备快、宿主慢"的真实 GPU——与 `ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`
    的数字是同一枚硬币的两面（那里 8.63–8.94 ms 墙钟 / 7.95 ms 设备 ⇒ 本机设备占 ~90%）。
    **所以本文不能用本机基准证明收益**，只能用结构证明成本（热路径零锁、miss 路径才付费）。
- **触发条件（什么时候重开）**：
  1. 出现真实宿主"一边编辑一边烘焙"（两个引擎同时要图），且第一步的串行等待进入帧预算；
  2. F5 的触发命中（设备时间 < 墙钟一半）——那时多帧在飞与多会话共用退休机制，一起做；
  3. 第二个 API 家族的后端（D3D12 / Metal）落地，使"设备 = 池"必须成为接缝的正式名词。
- **明确不做**：
  - 不做"一个目标被两个会话同时画"——一张图像只有一个 layout，这不是锁能解决的（`G4`：说"不"比说"也许"好）。
  - 不做全局设备锁（`D13`）。
  - 不做 per-entry 读写锁（选项 F）。
  - 不改"一引擎一表面"（`ADR-2026-11-13`）——两个引擎各画各的表面，正是本 ADR 的前提。

## 证据

- **代码事实**：本文"挡路的具体事实"表（8 条，逐条给了文件与成员）；`Device.cpp` 的 `:2434` / `:2566`
  传 `impl->completion` 证明读回与帧共用同一栅栏；`RenderDevice.hpp` 的 `setDiagnosticsRoute` 证明
  多路由**已经**落地（因此单槽只剩默认程序）。
- **既有用例**：`tests/test_graphics/HeadlessBackendTest.cpp` 的
  `OneDeviceServesSeveralAreasWithoutCrossingThem`（共享已证、并发未证）；`tests/test_vk` 89 例
  （单会话语义被整套用例隐含依赖）；`tests/test_vsg/BackendParityTest.cpp`（两个后端在同一帧语义下逐字节对拍）。
- **先例**：VSG 的设备级池 + 每任务帧状态（`MemoryBufferPools.h` 第 69 行的 `mutable std::mutex`、
  `Context.cpp` 把池挂到 `device`、`RecordAndSubmitTask.h` 的 `_fences`）。
- **规范**：Vulkan 的外部同步要求按对象给出（队列 / 命令池 / 描述符池 / 管线缓存 / 栅栏），
  设备本身允许并发入口——见 §Vulkan 的硬约束。
- **数字**：`ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` 的 8.63–8.94 ms / 7.95 ms；
  本机无法测出并发收益，理由见 §后果。
- **门禁**：`scripts/tsan_check.sh`（第二步的竞争证据）；`scripts/check_vk_exposure.py`（形状变了但边界不变）。

## 实施状态（追加，2026-11-16）

- **第一步（`T47`）：已落地 2026-11-15**——设备级帧闸（`backend/FrameGate.hpp`，两个后端同一实现）、三处契约文本、
  并发用例。
- **第二步（`T48`）：vk 侧已落地**——S1（释放走 `RetirementQueue`）、S2a/S2b（帧机器与帧体进 `Device::Session`）、
  S3（`ContentCaches` + `FrameScratch`）、S4（`ResidenceTable` 三态状态机）、S5（`TargetClaims` 租约）、S6a/S6b
  （`createSession()` + 引擎持有驱动者 + 默认程序归会话）、S6c-1（帧首快照）、S6c-2a（帧体真并行）、S6c-2b（释放
  不等帧）、S7（读回独立：自带池/栅栏 + 计数等待）、S9（契约/文档收口 + TSan）。
  **仍未做**：S8（vsg 的收集暂存搬进会话）与 `ResidenceTable` 的设备接线——状态机在树上、缓存仍走原路径，两个
  驱动者首次同时构建同一内容时在缓存锁上**串行**（`graphics-sessions.md` §9.2）。
- **契约文本已按实现改写**：`graphics-threading.md` 的 `C21` / `R18` / `N18`（帧按会话并发；默认程序归会话）、
  `RenderDevice.hpp` / `RenderFrameExecutor.hpp` 的线程段、vk `Device.hpp` 的 `@warning`。
- **竞争证据（TSan，2026-11-16）**：`test_vk` 133/133、`test_graphics` 587/587、`test_vsg`（除去窗口 / probe / 插件
  套件）各 **0 race report**。工具发现并修掉一处**真缺陷**：`Geometry::description()` 的惰性缓存无保护（两个引擎
  的 content 线程同时描述同一几何即竞争）——现在读者持共享锁、单个构建者持独占锁。
- **平台**：Linux/X11；Win32 / Wayland 的实机证据仍按切换门（`T35`）未过。
