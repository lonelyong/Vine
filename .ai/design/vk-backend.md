# vk 后端：直连 Vulkan 的渲染后端

> **状态**：现行
> **最后核对**：2026-11-17
> **范围**：`gfx_backend_vk` 插件（注册名 `"vk"`）——`RenderBackend` 的**直连 Vulkan** 实现：设备与队列、
> 目标与窗口、内容绘制、设备内存池、管线缓存、报告与诊断。**不含** SDK 接缝本身（见 `graphics-*`），
> **不含** `gfx_backend_vsg`（见 `vsg-backend.md`）。
> **权威来源**：`src/plugins/gfx_backend_vk/include/vine/backend_vk/**`（契约）、`tests/test_vk/**`（行为）、
> `scripts/check_vk_exposure.py`（暴露边界）
> **待办**：`../backlog/graphics.md`
>
> **编号约定**：本文档的 `F#` / `C#` / `R#` / `N#` / `P#` / `D#` / `T#` 是**本模块**的稳定 ID；审查报告的发现写作
> “审查 `F#`”，backlog 条目写作 “backlog `T#`”——三套编号互不冒充。

## 1. 概要设计

### 1.1 问题与背景

- SDK 的共享机制（`vine/graphics/`）需要一个**第二实现**来被评判：只有一个 后端时，“这是接缝的要求”
  与“这是 vsg 的习惯”分不开。本插件是那个第二实现，路径是**直连 Vulkan**（自建 `VkInstance` / `VkDevice` /
  pipeline），不经 VulkanSceneGraph。
- 它消费的是接缝，不是引擎：绘制什么由 `FrameDescription` 说，怎么画由本后端说。这使它可以被 `test_vk` 直接
  驱动（插件是 MODULE，测试编译其源码），**不需要窗口**。
- 现状：里程碑 M0–M13 全部落地；工业标准审查（`../research/graphics-2026-11-14-vk-industry-review.md`，F1–F13）
  中资源与成本模型项（审查 F1 / F2 / F4 / F7 / F8 / F12）已落地。行为由 `test_vk` **169 例 / 41 套**钉住，与 vsg 的对拍
  由 `BackendParityTest`（13 例，含在 `test_vsg` 内）保证。

### 1.2 目标

1. **规范符合**：validation 层静默（0 VUID）；每个 Vulkan 调用的失败形态被分类，不是“画不出来”。
2. **可对拍**：同一帧在 vk 与 vsg 上的渲染区域**逐字节相同**——这是“实现的是接缝”而不是“实现的是巧合”的证据。
3. **可诊断**：能力只报实际启用的开关与限制；每个拒绝“一次情节一条”（`ReportOnce`）；报告字段**缺席 ≠ 0**。
4. **工业级资源与成本模型**：设备内存池（一个会话一块一个池）、描述符集缓存、常量环（dynamic offsets）、
   会话内与跨会话管线缓存、设备时钟。
5. **边界可查**：插件 shell 不出现任何 `Vk*`；Vulkan 形状只存在于 `internal/`；唯一的逃生门是
   `support/Handles.hpp`（`vkHandles()`），且帧路径不使用它。

### 1.3 非目标

- **多帧在飞**（审查 F5）：每帧 `submit` + `fence wait` 是**当前的契约选择**（§2.3），改它要动 SDK 的“画完即读”，
  必须走 ADR；记录在 backlog，不在代码里。
- **`vkCmdPipelineBarrier2` / dynamic rendering**（审查 F9 / F10）：需要 API 1.3；当前目标是 1.2，且现有屏障表是单一
  定义、被同步验证覆盖过。属于现代化，不是缺陷。
- **专用传输队列 / 队列族所有权转移**（审查 F11）：同步提交模型下收益很小（G8）。
- **`VMA`**：不引第三方依赖；自建池已经覆盖今天的消费者（§2.4、`MemoryAllocator`）。
- **Win32 / Wayland 的实机证据**（backlog T12 / T13）：`WindowSystem` 预留了那一层，今天只交付 Linux/X11。
- **成为默认后端**：门禁在 `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`。**本后端今天就是宿主默认跑的那一个**——
  宿主的回退是注册表名字序的第一个，不是切换门的结论；切换门仍未全绿（Win32 实机、Wayland 回退），所以 Windows 不上线
  （backlog T35 跟踪）。

### 1.4 一句话结论

**一个会话 = 一个设备 + 一个内存池 + 一个管线缓存；一帧 = 一份命令缓冲 + 一次提交 + 一次栅栏等待；每个能力、
每个拒绝、每个缓存都有一个可被测试读取的证人。**

## 2. 设计要点

### 2.1 半边：shell 只露接缝，Vulkan 只住 internal

- **是什么**：`shell/`（`Plugin`、`BackendFactory`）只命名 `RenderBackend` / `RenderBackendFactory`；`internal/`
  才是 Vulkan；`support/Handles.hpp` 是唯一把 `Vk*` 交给外部（测试 / 工具）的门。
- **为什么**：G1（概念各有其位）+ G2（依赖单向：`internal` → SDK → `base`，`shell` 不反向依赖 Vulkan 类型）。
  这也让“把 vk 换成别的实现”是一次插件替换，而不是 SDK 变更。
- **边界**：`scripts/check_vk_exposure.py` 是门禁（V2 SDK backend 面、V4 插件 shell、E1 帧路径、E2 低层单元的
  免接缝），跟着 ctest 跑。
- **低层那一半**：`MemoryAllocator`、`PipelineCacheFile`、`WindowSystem`、`ReadbackStaging`、`Handles` **完全不
  include `vine/graphics`**（约 2.0k 行）——它们是“工具可以不经接缝直接用”的候选，E2 就是钉这条性质的。今天没有被
  **外部**消费者使用（`G8`），所以**不抽出独立库**：抽库只是搬家，边界已经在这儿；等真有一个不经过 `graphics` 的
  消费者，切出去是一次移动而不是考古。`BarrierPlan` / `ShaderCompiler` 各有一个接缝 include（`ImageLayout` /
  `ShaderStage`），因此不在这一组里。

### 2.2 设备是会话语义，丢失是第一种失败

- **是什么**：`initialize()` / `shutdown()` 幂等；`initialized()` 在 `VK_ERROR_DEVICE_LOST` 之后转 `false`，帧被
  跳过而不是继续触碰设备；重建路径固定为 `shutdown()` + `initialize()`。
- **为什么**：G4（失败模式写在类型/契约里）+ G11（一种失败可被定位）。设备丢失不是“某一帧失败”，而是**会话
  结束**——把它当帧错误处理，会让宿主在死设备上重试到进程结束。
- **边界**：`noteDeviceLost` 是唯一分类点（队列提交、acquire、present 三处都经过它），一次情节一条报告。

### 2.3 一帧一次提交：同步换确定性

- **是什么**：`executeFrame` 录一份命令缓冲 → 提交 → 等自己的栅栏 → 读回/释放可以在同一次调用后立即发生；
  `prepareFrame` 例外地允许**一批只含传输**的提交（ADR-2026-11-14-prepare-may-submit-transfers）。
- **为什么**：G3（流程一趟读完）+ G11（“画完即读”让测试与宿主都不需要猜时序）。代价写在这里：**宿主与设备串行**。
  实测（lavapipe，60 帧基准）：一次 `renderFrame()` 的墙钟中位 **8.63–8.94 ms**，同一帧的设备时间 **7.95 ms**，
  差额 **≈0.7–0.9 ms 是宿主侧的活** ⇒ **设备是瓶颈（约 90%）**，不是宿主。所以多帧在飞的收益上限是
  `max(宿主, 设备) ≈ 7.95 ms`（约 10%），而代价是改掉“画完即读”的契约——决定与算式见
  `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`。**更正**：本页早先写“CPU 中位 8.2–9.0 / GPU
  7.15–7.77，即 CPU 是瓶颈”，那是把**包含栅栏等待的墙钟**当成了宿主工作量，已按上表更正。
- **边界**：同步一帧是**释放变得廉价的原因之一**，但不再是唯一的那个：释放如今交给
  集合队列（§2.15），所以“不需要 idle”是队列的保证（`device_waits` 为证）而非时序的偶然。
  破坏“一帧一次提交”仍然要同时改释放与读回，因此需要 ADR，而不是一处 `waitForFence` 的删除。

### 2.4 成本模型：每帧四件套，各自有证人

- **是什么**：一帧的常量进**一个**持久映射的 ring（`MemorySlice` + `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`）；
  描述符集按**它绑定的资源**缓存；几何/实例流、程序、管线按**身份**缓存；设备内存来自**一个会话级池**。
- **为什么**：G5（变化留在模块内）+ G7（复杂度被吸收，而不是推给调用方）+ G10（次数、寿命、布局被计量）。
  旧形态是“每 draw 每帧建/毁一个 buffer 与它的内存、每 draw 每帧分配一个 set”，实测把稳态中位从 9.03–9.23 ms
  压到 8.16–8.30 ms（−10%）。
- **边界**：四件套各自有可读证人——`Device::contentCacheCounters()`（`ring_builds` / `set_builds` / `sets_live`）、
  `Device::contentPipelines()`、`Device::memoryStats()`、`Device::pipelineCacheReport()`。**没有证人的优化不算落地。**

### 2.5 内容身份，不做内容比较

- **是什么**：几何按 `GeometryDescription` 的地址 + revision，纹理按 `Texture` 的地址 + revision，程序按描述 +
  variant 位；`releaseContent` 只把对应的流置空（留洞），下一帧只补洞。
- **为什么**：G5（缓存键 = 身份，释放 = 留洞，不需要遍历比较）+ G10（一次释放一个上传，不是一个重建）。
- **边界**：键里持有 `intrusive_ptr`，所以地址不会被后来者复用；这是“身份”能当键的前提。

### 2.6 形状 vs extent：resize 不重编管线

- **是什么**：render pass 由**形状**（颜色/深度格式、采样数）决定；图像、视图、framebuffer 由 **extent** 决定。
  `planTarget` 据此给出 Rebuild / ResizeInPlace / Repair 三答案（SDK 的 `TargetPlan`）。
- **为什么**：G1（形状与尺寸是两个概念）+ G9（新格式/新采样数只扩展形状表）+ G10（resize 不付重编代价）。
- **边界**：`ResizeInPlace` 保留 render pass（管线兼容），`Rebuild` 才换形状；`TargetObjects` 的三个 load-op 变体
  共享一个 framebuffer（load op 不进兼容性）。

### 2.7 屏障是一张表，不是散落的调用

- **是什么**：`BarrierPlan.hpp`（`stateOf` / `barrierBetween` / `loadOpOf` / `storeOpOf`）把 SDK 的
  `ImageLayout`（当作“用途”）映射成 Vulkan 的 layout + stage + access；所有过渡过 `barrierBetween`。
- **为什么**：G1（一个定义、一个 owner）+ G11（同步错误只有一处可能写错，测试与 validation 都指向它）。
- **边界**：表覆盖本后端会产生的用途（Undefined / ColorAttachment / DepthAttachment / ShaderReadOnly /
  TransferSrc / TransferDst / PresentSrc）；新用途是加一行，不是加一处内联屏障。

### 2.8 报告诚实：缺席不是零，一次情节一条

- **是什么**：`FrameReport` 的 `timing` 缺失表示“这张卡不报时钟”，不是 0；`PrepareReport`、`FrameCounters`、
  `memoryStats`、`pipelineCacheReport` 同理；每个“做不到”有独立的 `ReportOnce`。
- **为什么**：G4（字段说它是什么）+ G11（宿主能区分“没发生”与“发生了且是 0”）。
- **边界**：`ReportOnce` 的 `rearm` 由“条件结束”的那一处调用，不在关机时统一清空（否则下一次会话的第一次拒绝
  会被上一次的静默吞掉）。

### 2.9 位置是宿主的，格式是后端的

- **是什么**：`DeviceDesc::cache_directory` 是**创建输入**（宿主说文件可以放哪）；`PipelineCacheFile` 说文件长
  什么样（`[magic][format][key][size][blob]`，键 = vendor / device / driver / API / `pipelineCacheUUID`）。
  宿主不写时，插件在 `Plugin::load` 用自己的 application data 目录当默认值；两者都没有 = 不持久化。
- **为什么**：G1（归属）+ G2（SDK 不知道缓存格式，后端不知道用户路径）。把“用户文件放哪”塞进后端，是让渲染层
  替应用做产品决定；把缓存格式塞进 SDK，是让接缝知道 Vulkan。
- **边界**：缓存是优化，不是正确性：读不到、被驱动拒收、目录不存在，都只意味着冷启动。

### 2.10 测试是契约的一半

- **是什么**：`test_vk` 直接编译插件源码，无窗口驱动设备；`BackendParityTest` 在 `test_vsg` 里把同一帧交给两个
  后端并**逐字节**比较渲染区域；关键守卫用**变异电池**证明承重（关掉它，对应用例必须红）。
- **为什么**：G11（模块可被单独测试、失败可被定位）。没有窗口的测试让 CI 与桌面走同一条路径。
- **边界**：`test_vk` 的设备用例在无 Vulkan 设备的机器上 **skip** 而不是失败（环境不是契约）。

### 2.11 一个 `Device` 对象 = 一个会话池（`G1` / `G8`）

- **是什么**：本插件的 `Device` 同时是**设备池**与**它自己的那个会话**：一个 `VkInstance` / `VkPhysicalDevice` / `VkDevice` / 图形队列，
  一份设备级内容缓存与目标表、一份 `DeviceClock` + `RetirementQueue`；`createSession()` 另外借出 `FrameDriver`。
  `RenderDevice::executor()` 答的是设备自带的那个驱动者。
- **帧机器**：在驱动者里——命令池 / 栅栏 / 时间戳池 / 帧账 / `FrameGate` / clock window；设备拥有它，并在 `shutdown()` 里销毁；
  帧体（`executeFrame` / `prepareFrame` / `frameReport`）也在驱动者里，`Device` 只剩三个一行转发。
- **会话接缝**：`createSession()` 造一个 `FrameDriver`（可早于 `initialize()`，由它统一拉起；已起的设备上当场建），
  `releaseSession()` 销毁并遗忘（设备自己的驱动者不可归还）。默认内容程序归 `RenderFrameExecutor`：本设备的每个
  `FrameDriver` 各持一份，`Device` 覆写转发给自带的驱动者。
- **并发**：设备的大锁是 `graphics::FrameGate body_gate`：帧体（非窗口）
  取 SHARED，两个驱动者在两条线程上同时记录/提交；出窗口的帧与所有宿主动词（读回 / 释放 / 默认程序 / 释放目标 /
  建窗口 / 诊断读取）取 EXCLUSIVE（可等帧，不竞争）；`shutdown` 也取 EXCLUSIVE。**从帧体线程调用的宿主动词得到空租约**：
  它回答自己的拒绝值（读回答 `Failed`、建目标答 `UnsupportedSurface`、计数器答 0）并报**一条** `vk.device.reentry`
  （`G4`：拒绝要能被听见，而不是静默继续）。报告 sink 的锁是**递归**的，因为 sink 本身
  可能回调（同一线程再报就是那条 `vk.device.reentry`）。支撑它的是一组小锁：`clock_mutex`
  （`DeviceClock` + `RetirementQueue`，两个 SDK 值类型仍标非线程安全，多线程设备自己提供锁，`P4`）、分配器一把、
  目标 map 一把、报告 sink 一把、每会话默认程序一把、表面列表一把。`FrameTimeline` 支持多开帧（开帧时铸造号）。
  纹理 staging 与被替换图像按 `ContentImages::BatchId` 归驱动者，替换图像 park 到水位。采样目标（screen 源 / pass
  输入）也取目标 lease。**用例**：`VkMultiSessionTest.AFrameBodyDoesNotWaitForAnotherDriversFrame`、
  `VkConcurrentFrameTest.AHostVerbCalledFromInsideAFrameIsRefusedInsteadOfHanging`。
- **释放**：`releaseContent`（三种重载）与 `releaseRenderTarget` 不取体闸：它们只等一次缓存操作（帧首解析持有的锁），
  安全由缓存锁 + park + 水位的开帧规则共同给出。`targets` 的条目是 `shared_ptr<TargetObjects>`（释放把引用交给 park，
  已经解析它的帧仍持有有效指针），`released_nodes` 是原子，队列的读者（`retirementNumbers` / `frameReport`）也取
  `clock_mutex`。
- **帧暂存三件**：`ContentDraw` 不是一个“什么都有”的对象，而是三件：
  `ContentCaches`（**设备**：programs / geometries / instance streams / pipelines / 布局 / 图像 / 管线缓存）、
  `FrameScratch`（**驱动者**：常量环 + 描述符池 + set 表 + `frame_index`）、以及 `ContentDraw` 本身
  （**记录器**：借用前两者，只有 `prepare` / `prepareScreen` / `record` / `recordScreen`）。`Device::FrameDriver` 拥有自己的
  `scratch` 与 `recorder`；`Device::Impl` 只留 `caches`；`createScratch()` 在布局建好之后由 `initialize()` 调，拆除时
  scratch 先于 caches（set 的 layout 是借的）。
- **为什么是这个形状**：每个驱动者一帧在飞（§2.3）让"帧之间"成为一个**时刻**，于是释放、读回、缓存填充都能共用一份机器；
  `ContentDraw` 的 `beginFrame` / `endFrame` 重置的是设备级状态，正是这条的直接后果。在那个前提下把它拆成两个对象
  只会多一层转发（`G8`）。
- **边界**：两个驱动者的帧体可以同时跑；同一个驱动者的两帧仍由它的 `FrameGate` 串行（另一条线程**等待**，
  同一线程重入**拒绝**，`vk.session.frame_reentry` / `PresentOutcome::Refused`）。窗口帧独占体闸（交换链是一份设备资源）。
  每帧自己报告它在缓存锁上花了什么（`FrameReport::locking`：帧首一次 hold + 帧尾一次采样，稳态是常数、`waits` 为 0，
  见 `VkFrameHeadTest.AFrameReportsWhatItSpentOnTheCacheLock`）。读回仍取体闸 EXCLUSIVE，因此**会等**在飞帧；释放不等帧（R6）。
  形状与论证在 `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`。
- **拆分时哪些东西会动**（清单是起点，不是拆分本身）：`ContentDraw` 的 `Impl` 一半是**缓存**（模块 / 流 / 管线 /
  布局，归设备）、一半是**帧状态**（常量环 / 描述符池 / 逐 draw UBO，归会话）；`command_pool` 与 `completion` 归会话；
  `allocator`、`targets`、`surfaces`、管线缓存、`default_program` 归设备。

### 2.12 分配失败的分类只在设备一处（`G1` / `G4`）

- **是什么**：`MemoryAllocator` 只回答**事实**——`MemoryRefusal`（`NoType` / `DeviceLost` / `OutOfMemory` /
  `DriverRefused` / `InvalidRequest` / `NotOpen` / `Internal`）+ 驱动的原始 `VkResult` + 请求字节 / 类型 / 池的
  当下数字，装在一个 `MemoryRefusalReport` 里，**每一次分配（含成功）**交给一个 sink。sink 由 `Device` 在
  `initialize()` 里安装，是唯一的消费者；`Device::noteMemoryRefusal` 是唯一的分类点：`DeviceLost` 走
  `noteDeviceLost`（会话结束，一次一条），`OutOfMemory` 是宿主可释放内容的警告，`DriverRefused` 是要上报的错误，
  两个接线缺陷各自一条。
- **为什么**：`allocate` 有六个调用点，其中五个只需要"成不成"——把理由交回给它们，等于要求每个调用点记得用它，
  而它们不会。这正是修掉的缺陷：`vkAllocateMemory` 的 `VkResult` 被丢弃，**设备丢失一路被读成"显存不足"**，
  宿主在死设备上继续重试（审计 `A1`）。
- **边界**：池不 include 接缝（`C10`），所以它不知道 `DiagnosticCategory`，也不该知道；句子（含数字）由设备写。
  成功也上报，是因为 `ReportOnce` 的情节要在**条件结束处** re-arm。
- **成本**：每次分配一次 `std::function` 调用与一个栈上报告值；分配是 miss 路径事件（`R24`：稳态帧零分配），
  所以热路径不受影响。
- **可行动的拒绝**（审计 `A2`）：`MemoryStats` 带 `free_bytes` / `largest_free_run`，且**只在拒绝时**走一遍块表把它们填上
  （成功路径只拿已有的计数器）。于是“堆拒绝了新块、池里却还有 12 MiB 空闲（最大连续 1 MiB）”能写成一句话——
  宿主据此知道“释放一个大资源有用、释放小的未必”。**没有回收阶梯**：池内没有可驱逐的东西（释放已会销毁全空块，
  至多留一个当缓存），真正能腾出显存的是宿主的 `releaseContent`，所以诚实的做法是把“为什么不够”说清楚，而不是
  在池里造一个假装能腾地方的机制（`G8`）。

### 2.13 诊断带 code、subject 与 limit：句子是给人的，值才是给宿主的（`G4` / `G11`）

- **是什么**：每条报告带一个**稳定 code**（`vk.<class>.<cause>`，登记在 `scripts/diagnostic_codes.txt`）、一个
  **subject**（这个报告说的是哪个目标）和一个可选的 **limit**（`requested` / `supported` 两个数）。`message`
  仍带全部数字，但**它可以改措辞**，code 不可以。
- **为什么**：宿主与用例要能"不解析句子就行动"（`R28`：按 code 断言）。同一个 category 下的两种原因（"超过上限"
  与"不是 2 的幂"）在值上是两个 code，在句子上只是两句话——只有句子时，用例只能 `find(u8" MiB")`。
- **边界**：code 的词汇表属于**报告它的模块**（SDK 不认识 `Vk*`，`C3` / `C10`），所以它是字符串而不是中央枚举；
  `check_diagnostic_formats.py` 双向检查（用到的必须登记、登记的必须被用到），并用
  `scripts/diagnostic_code_baseline.txt` 给尚未转换的调用点设**上限**——新报告必须带 code。
- **已转换**：本后端**全部报告**——内存、会话、目标、窗口、初始化、帧与预热、内容七族共 65 个 code；
  `scripts/diagnostic_code_baseline.txt` 里已没有本插件的文件（审计 `A3`），且该文件自批次 4 起**全空**。
- **成本**：报告路径上一个 `String` 字段（情节已去重，所以不在热路径）；`limit` 是 `std::optional`，不报 limit
  时零成本。

### 2.14 原因在知道它的地方命名：内容拒绝是一个值，不是两个出参（`G1` / `G4` / `G7`）

- **是什么**：内容路径的拒绝（这条 draw 为什么没画出来）由**记录器**给出——只有它知道是哪一级拒绝的（程序、流、实例
  流、管线、环形缓冲、描述符集）。它填一个 `ContentRefusal`——**它就是接缝的 `graphics::DiagnosticDraft` 的别名**
  （`category` + **code** + subject + limit + 句子，`refuse()` 唯一写者、先到者胜）；`executeFrame` / `prepareFrame`
  只把它**转发**成 `RenderDiagnostic`。vsg 的读回拒绝（`HostReadbackRefusal` 枚举）也产出同一个类型：拒绝只有一种形状
  （ADR-2026-11-16 §4）。
- **为什么**：这条路径原本是 `(vn::String* why, DiagnosticCategory* category)` 两个出参——能填一个而忘了另一个，
  而且"跳过一条 draw"的七种原因到宿主那里只剩一个 `ContentSkipped`。把 code 放在**知道原因的那一侧**，宿主与用例
  才第一次能说"这条 draw 没有程序"，而不是"有条 draw 被跳过了"（`R28` 的落点）。
- **边界**：`refuse()` 是唯一的写入者，且**先到者胜**——第二个原因（"没有几何，所以也没有管线"）不会覆盖第一个，
  这条规则原来在调用点用 `if (why.empty())` 重复了三遍。空句子 + 有 code 仍是拒绝：句子的有无从来不是判据。
  `ContentRefusal` 是**内部头**（`internal/ContentRefusal.hpp`），不进 SDK；形状本身住在 SDK 的后端半。
- **原因是一个枚举，code 是一张无 `default` 的 switch**（`ContentRefusalCause` / `contentRefusalCode`）：`refuse`
  的调用点曾直接写字符串字面量，所以新增一种原因可以不带 code 编译通过（D-22）。现在加一个枚举值时，缺那个
  `case` 就编不过（学 vsg 的 `HostReadbackRefusal` 映射）；`VkContentRefusalTest.EveryCauseNamesAStableCode…`
  还验一遍"每个值都有 code、没有两个值共用一个"。
- **门禁的两半**：报告调用若**构造**诊断（≥3 个实参）就必须写 code；若**转发**一个 `RenderDiagnostic`
  （`reportDiagnostic(diagnostic)`）则不算——值里带着 code，sink 丢不掉它。`check_diagnostic_formats.py`
  同时双向检查登记表：用到的必须登记，登记的必须被某个 builder / code 表 / 报告用到。

### 2.15 释放是交给队列，不是立刻销毁（`G4` / `G10`）

- **是什么**：`releaseContent` / `releaseRenderTarget` 把缓存条目**当场忘掉**（驻留数立即下降、下一帧重传），
  而它背后的 Vulkan 对象交给 `graphics/RetirementQueue.hpp`，**等设备的完成水位越过**那一帧才销毁。
  完成证据来自 `graphics/DeviceClock.hpp`：本后端注册**一个** clock window（一帧 在飞，`slots = 1`）。
- **为什么**：释放要说的是两件事——“这份内容还在不在”与“这个对象现在能不能毁”——合在一处就是
"
  "“释放只能在帧之间调”这条宿主必须守的时序。拆开之后 vsg 已经在用的同一套机制装上了两个实现（`G6`），
  而且它是下一步多会话的前提（`.ai/design/graphics-sessions.md` §2.6）。
- **代价（写在这里而不是隐藏）**：一个已释放对象活到 `submitted + slots + 1`，即**最多两帧后**
  才销毁（它是“slot 已被回收”这个证据的保守代价）。两帧的内存延迟与一次设备空转相比是廉
  价的，而等待变成了按帧计时的后台事件。
- **边界**：**拆除不 park**。`shutdown()` 先 `vkDeviceWaitIdle`，再把接缝置空并 `releaseAll()`（它就是“仲裁已经
  出现”的那个动词）；否则 park 的对象会随会话泄漏。**替换路径**（重传时的旧 buffer、ring 重建）仍是
  立即销毁，因为它发生在记录本帧时、提交之前；多会话之后它们必须一同 park（见设计文档 S6）。
- **观察点**：`Device::retirementNumbers()`（`pending` / `released` / `device_waits`）。`device_waits` 记"宿主把设备停了一下"：
  无法 park 的破坏性路径，以及**读回的等待**（S7：读回等它要读的帧完成，等一次记一次）。帧的组装路径（记录 / 提交 /
  栅栏等待）永远不抬高它——那才是这条数字要守的不变量。

### 2.16 设备是池、驱动者是帧：缓存与帧暂存分家（`G1` / `G4` / `G10`）

- **是什么**：内容路径拆成 `ContentCaches`（设备）与 `FrameScratch`（驱动者），`ContentDraw` 退化为两者之上的记录器。
  `ContentCaches` 持 programs / geometries / instance streams / pipelines / 三个 set layout 与两个 pipeline layout /
  管线缓存 / `ContentImages`，以及释放（`release*`）+ 驻留账（`Progress`）；`FrameScratch` 持常量环（一个 host-visible
  buffer + 切片游标）、描述符池、set 缓存、`draw_set`、`frame_index`、`ring_builds` / `set_builds`。
- **为什么**：缓存按**宿主身份**（程序 revision / 几何 revision / buffer 地址）键控且重建很贵——第二个驱动者要的是
  **找到**它们，而不是复制一份；而环与 set 缓存按**帧**写入（`beginFrame` 从零发切片、帧内会增长），两个帧交错写同一
  个环就等于互相覆盖对方已提交帧还在读的常量。把“贵的、按身份共享的”与“每帧被写坏的”按这个界线切开，S6 的
  `createSession()` 才是“多发一个驱动者”，而不是“多一个能毁掉第一个的东西”（`G5`）。
- **边界**：布局归 caches（管线状态要它们），池与环归 scratch（分配发生在帧内）；scratch **借用**布局与设备入口，所以
  拆除顺序是 scratch → caches → allocator → device（`P8` 的同一族规则）。set 缓存的淘汰用 `frame_index` 而不是锁：
  它只属于自己的驱动者。
- **代价（如实）**：`ContentDraw` 的 `record` 每帧多一次指针间接（两个借用引用），换来的是“谁拥有帧内状态”可以一行
  指向一个对象。
- **锁与帧首快照（S6c-1 + S2 批 2b，已落地）**：`ContentCaches` 有一把**自己的锁**（`ContentCaches::Lock`，不可重入），帧只在**帧首**取它
  一次：`planDraw`（两个重载）/ `resolveTextures` 把每一调用要画的东西（pipeline、顶点/索引/实例 buffer 句柄、
  材质、纹理 view/sampler、阴影 sampler）**按值**写进 `FrameHead::DrawPlan`（帧级纹理/采样器事实挂 `FrameHead`），纹理上传也在帧首录进帧自己的
  命令缓冲（render pass 之外，唯一合法的地方）。**记录路径此后不取锁、不碰缓存条目**；条目被替换/释放时旧对象**park 到
  水位**，所以“帧首解析过的值”在整帧内有效（`G10`）。`Lock&` 参数把“我持锁”变成编译期证据（`G4`）。
- **观察点（T3 的准确形式）**：`Device::contentCacheCounters().cache_lock_acquisitions` 是累计获取次数。一整帧固定 **4 次**
  （帧首 1 + 帧首/帧尾两次内容数簿各 1（`ContentCaches::numbers()` 一次读两半）+ `endFrame` 1），**不随 draw 数增长**；
  设计里“稳态帧 0 次”指的是**记录路径**，帧首的 1 次是 §2.3 自己写的机制（见 `graphics-sessions.md` §8 T3 的订正）。
  `VkFrameHeadTest.AFramesCacheWorkDoesNotGrowWithItsDrawingCalls` 用 1 条 vs 6 条命令比较同一个数；把记录路径改回
  逐 draw 取锁会让它变红（实测 6 → 10）。
- **观察点（另一半）**：`Device::contentCacheCounters()` 由两半拼成——ring / set 数字来自设备**自己那个驱动者**的
  `FrameScratch`，其余来自 `caches.numbers()`。单会话语义与拆分前逐值相同（`test_vk` 全绿，`BackendParityTest` 12/12）。
- **帧首存储的复用与证人（B11 / I-02）**：`FrameHead` 的 pass / call 槽位是 `clear()` 而不是重建（`storageGrows()`
  只在容量不够时增加）；`Device::frameHeadStorageGrows()` 把它暴露给测试，`VkFrameHeadTest.ASteadyFrameDoesNotGrowTheHeadsStorage`
  用“3 帧预热后 5 帧不变”钉住。**它证明的只是头部的存储复用**，不是整帧零分配（管线键与内容缓存有它们自己的分配，
  本批不声称也不测量）。同批修掉一个真实缺陷：`planDraw` 原先用 `plan = FrameHead::DrawPlan{}` 清空，等于每命令释放并重建
  复用向量——现在是 `plan.clear()`。

### 2.17 驻留三态：一个键只有一个发布者（`G1` / `G4` / `G10`）

- **是什么**：`internal/ResidenceTable.hpp` 是一条键一条条目的状态机：`Absent → Uploading(owner, token)` →
  `Resident`。`admit(key, session)` 答三件事之一——`Use`（已经发布，直接用）、`Build`（我赢了 compare-and-set，
  拿到 move-only 的 `Claim`）、`Copy(owner)`（别人正在填，自己建一份**不发布的临时副本**）。`Resident` 只能由
  持有 token 的 owner 通过 `Claim::publish()` 声明（“填充已完成、证据在我手里”）；未决议的 `Claim` 析构时把键退回
  `Absent`。
- **为什么不是“等”**：等待是唯一能把两个驱动者互相锁死的形状（设计 §2.4 `P2` / `D3`），而复制有界——每个键每个
  竞争者在“两个引擎第一次同时画它”的那一帧至多一份。`Uploading` 的条目对别人等于 `Absent`，所以一条半成品
  永远不会被当成 `Resident` 用。
- **为什么是 token 而不是“谁建的谁发布”**：token 单调且不复用，所以三种真实时序都能诚实回答：`forget`
  （`releaseContent`）发生在填充中途、claim 已撤回、键被释放后又被新 claim 拿走——老 token 的 `publish` 一律失败
  并计入 `Numbers::stale`（“调用方以为自己拥有它”）。
- **边界**：表**不销毁任何东西**，也**不排退休**：`forget` 只忘键，Vulkan 对象仍交给 `RetirementQueue`（§2.15）。
  它也不是 SDK 的 `ResourceTable`——那张表回答“这份驻留值现在能不能放”（单线程、按 pin 退休），这张回答“谁可以
  建这个键”（两驱动者、只到发布为止）。
- **接线**：`ContentCaches::ensureGeometry` 先问表（`Use` / `Build` / `Copy`），
  帧作用域的 `ContentCaches::FrameBuilds` 在帧栅栏 wait 过之后 `publishAll()`（跳过失败的填充），`releaseGeometry`
  同时 `forget` 键；竞争者拿 `Copy` 建**脱钩**临时条目、帧结束即销毁。

### 2.18 目标表：对象、形状与一帧一个持有者（`G1` / `G6` / `G7`）

- **是什么**：设备持一张 `TargetTable`——从宿主的 `RenderTarget*` 到它成的 `TargetObjects`（map + 形状缓存 +
  构建/重建/修复）、六条目标拒绝情节，以及每帧的租约（`RenderTarget* → FrameDriverId`）**是同一张表的三个问题**；
  `lease(driver)` 发一个帧作用域的
  `Lease`；`executeFrame` 与 `prepareFrame` 在每个 pass 的 **`ensure` 之前** `claim` 它的目标。冲突时那个
  pass **被跳过**（帧里其它 pass 照跑），并报 `vk.target.claim_conflict`——**每个（目标, 情节）一条**，情节在无冲突
  的一帧/一次预热后结束。
- **为什么是每 pass 而不是整帧**：一次局部冲突不该放大成“整帧失败”；冲突走的是已有“这个设备建不起这个目标”
  同一条报告路径（`G6`），只是多了一个 code。
- **为什么 claim 在 `ensure` 之前**：建/重建附件是**持有者的动词**（`R5`），所以“谁可以重建”与“谁可以画”
  是同一个答案；claim 晚一步，两个会话就可能在同一个目标上交错重建。
- **为什么一张表**：map 与租约回答的是同一个问题——“这个目标现在是什么、谁可以动它”。拆成两张表时，释放要
  在两处各做一半，测试也不得不同时驱动两个对象（`G1`）；合并后 `release(target)` 是“条目离开 map + 记录交给
  park”一个调用，`drain()` 是“记录与租约一起清”。租约仍持有自己的锁（另一个驱动者的 per-pass claim 不能排在
  一次构建后面）。
- **边界**：租约不是锁（`C5`）——等待会让两个引擎的帧率互相绑死；冲突不销毁任何东西，目标下一帧再试。
  `Lease` 的析构是**唯一释放点**（`P3`），所以帧的任何一个提前返回都不会把目标钉住。窗口目标不需要 claim：
  `createSurfaceTarget` 已经保证一个窗口只属于一个会话（`WindowOccupied`）。
- **冲突何时发生**：两个驱动者由 `RenderDevice::createSession()` 借出之后，冲突由一条冒充“另一个驱动者”的租约与
  `VkMultiSessionTest` 的并发用例跑实；单驱动者不会冲突。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 设备生命周期与丢失 | instance / device / queue / 命令池 / 栅栏；丢失即会话结束 | 已落地 |
| F2 | 物理设备选择 | 按 `VkPhysicalDeviceType` 排序，同分保留先出现者 | 已落地 |
| F3 | 窗口与交换链 | surface、FIFO、每图 present 信号量、1 s acquire 超时、重建 | 已落地（Linux/X11） |
| F4 | 离屏目标 | render pass 变体、MRT、MSAA + depth resolve、借用深度、resize 保留 pass | 已落地 |
| F5 | 内容绘制 | 程序 → SPIR-V → 管线；状态映射；顶点/索引/实例流 | 已落地 |
| F6 | 全屏绘制 | screen ABI；深度强制 Disabled；绑定 pass 解析到的阴影图 | 已落地 |
| F7 | 纹理与采样器 | revision 缓存、1×1 stand-in、上传在 render pass 之前 | 已落地 |
| F8 | 常量 ring | 每帧一个持久映射 buffer，切片 + dynamic offsets | 已落地 |
| F9 | 描述符集缓存 | 按（视图集合、阴影图）缓存；逐条释放，定期清扫 | 已落地 |
| F10 | 设备内存池 | 块 + 切片、usage/kind、granularity、整块映射 | 已落地 |
| F11 | 读回 | 颜色（RGBA8）与深度（D32）；自有池与栅栏；一次读回一次计数等待 | 已落地 |
| F12 | 管线缓存 | 会话内 `VkPipelineCache` + 跨会话设备键控文件 | 已落地 |
| F13 | 设备时钟 | `FrameTiming`（每 30 帧采样一次） | 已落地 |
| F14 | 报告与诊断 | `FrameReport`、`PrepareReport`、驱动消息、`ReportOnce` | 已落地 |
| F15 | 暴露边界 | shell 零 `Vk*`；`Handles` 逃生门；门禁脚本 | 已落地 |

### 3.1.1 F1 设备生命周期与丢失

- **行为**：`initialize()` 建 instance、选物理设备、建设备/队列/命令池/栅栏，再建窗口面与内容路径；`shutdown()`
  按“内容 → 目标 → 窗口 → 池 → 设备 → instance”释放，幂等。
- **约束**：`C1`、`C2`、`C6`。
- **验收**：`T1`（全套）、`BackendTest` 的初始化/关机往返。
- **注意**：`P14`（关机顺序：内容 → 目标 → 窗口 → 池 → 设备 → instance）。

### 3.1.2 F2 物理设备选择

- **行为**：枚举全部物理设备，按类型排（discrete 4 / integrated 3 / virtual 2 / cpu 1 / other 0），同分保留先出现
  者；有窗口时还要求该族的 present 支持。
- **约束**：`C7`。
- **验收**：`T1`；选择结果由 `handles()` 可读。
- **注意**：`P10`（设备身份是缓存键的一半）。

### 3.1.3 F3 窗口与交换链

- **行为**：`WindowSystem` 建 surface（X11 / xcb）；交换链 FIFO、`minImageCount + 1`、`currentTransform`、复合
  alpha 回退、优先 BGRA8/RGBA8 sRGB；**每张交换图一个 present 等待信号量**；`vkAcquireNextImageKHR` 1 s 超时，
  超时按“这一帧没拿到图”跳过并按情节报一次；`OUT_OF_DATE` / `SUBOPTIMAL` 重建。
- **约束**：`C1`、`C2`、`R3`。
- **验收**：`WindowPresentTest`、`ProbeTest`；`T6`（真实宿主）。
- **注意**：`P13`（窗口测试的红先怀疑环境；交换链信号量按图一份，不能共用一个）。

### 3.1.4 F4 离屏目标

- **行为**：`TargetObjects` 持有颜色/采样缓冲/深度/深度 resolve 图像与视图、三个 render pass 变体（清深度、载
  深度、全载）、一个 framebuffer。MSAA 目标把颜色 resolve 进单采样图；深度用 `vkCreateRenderPass2` 的
  `pDepthStencilResolveAttachment`（`sample0`）resolve；深度-only 目标即阴影图；`shareDepth` 的目标借用另一个
  目标的深度视图。
- **约束**：`C5`、`C8`、`N2`。
- **验收**：`MsaaTest`、`ContentTargetTest`、`OffscreenClearTest`、`DeferredTest`。
- **注意**：`P4`、`P15`（变体之间连**依赖**都要一致）。

### 3.1.5 F5 内容绘制

- **行为**：`ShaderProgramDescription` → glslang → SPIR-V → `VkShaderModule`；管线键 = 渲染通道兼容性 + 状态
  （polygon / cull / depth test/write/compare / blend）+ variant 位；`RenderCommand` 一条 = 一次 `vkCmdDrawIndexed`。
  实例流按 buffer 地址 + revision 缓存。
- **约束**：`C4`、`C7`、`N3`。
- **验收**：`ContentDrawTest`、`VariantTest`、`BuiltinProgramTest`、`BarrierPlanTest`。
- **注意**：`P6`（ring 只在帧间增长）。

### 3.1.6 F6 全屏绘制

- **行为**：`fullscreenVertexProgram()` 提供的顶点阶段 + 调用方片元阶段；source 的颜色附件绑到 set 0 的 0..N-1，
  阴影图与灯块绑 7 / 8；`planScreen()` **强制** `DepthMode::Disabled`。
- **约束**：`C7`、`N3`。
- **验收**：`ContentDrawTest` 的 `AScreenCopyIsDrawnWhateverThePassDepthPolicySays`、`ShadowTest` 的
  `AScreenPassShadesWithTheMapItResolved`、`DeferredTest`。
- **注意**：`P1`、`P2`。

### 3.1.7 F7 纹理与采样器

- **行为**：`ContentImages` 按 `Texture` 地址 + revision 缓存 image/view；sampler 按 `Sampler` 值缓存；缺槽绑 1×1
  白色 stand-in 并清掉材质的槽位；上传（barrier + `vkCmdCopyBufferToImage`）只能录在 render pass **之前**。
- **约束**：`C5`、`N1`。
- **验收**：`ResidencyTest`、`ContentDrawTest` 的纹理采样用例、`PrepareUploadTest`。
- **注意**：`P5`。

### 3.1.8 F8 常量 ring

- **行为**：每帧 `beginFrame()` 把游标归零；每个 draw 取一段 `sliceSize()` 切片，四个块（material / lights /
  draw / shadow）写在固定偏移上；所有 uniform 绑定都是 DYNAMIC，偏移随 `vkCmdBindDescriptorSets` 传入。
- **约束**：`C5`、`N4`。
- **验收**：`VkUniformRingTest.ASteadyFrameCreatesNoRingAndBuildsNoSet`、
  `VkUniformRingTest.TwoDrawablesWithDifferentMaterialsShareTheRing`。
- **注意**：`P6`。

### 3.1.9 F9 描述符集缓存

- **行为**：内容集按（五个纹理视图 + 阴影图）缓存，screen 集按（source 视图 + 阴影图）缓存，per-draw 集每代
  ring 一个；池带 `FREE_DESCRIPTOR_SET`，条目逐条释放，600 帧未用、每 120 帧清扫一次。
- **约束**：`C5`、`N1`。
- **验收**：`VkUniformRingTest.*`；关掉缓存的变异让两个成本模型用例同时红。
- **注意**：`N1`（只有 ring 换代才清空）。

### 3.1.10 F10 设备内存池

- **行为**：`MemoryAllocator` 按 memory type 建 `VkDeviceMemory` 块，资源持 `MemorySlice{memory, offset, size,
  block}` 并按 offset 绑定；`MemoryUsage` 决定类型（host-visible 强制 coherent），`MemoryKind` 承载
  `bufferImageGranularity`；释放与相邻空闲区间合并；整块空闲且同类还有别的块则销毁；host-visible 块整块映射一次。
- **约束**：`C6`、`N4`。
- **验收**：`VkMemoryPoolTest.*`（6 目标 = 12 片 ≤ 2 块；稳态帧零分配；读回归还；关机归零）、
  `VkMemoryAllocatorTest.*`（相邻合并；linear / optimal 不共页）。
- **注意**：`P7`（能否映射看 memory type；池先于第一个消费内存者打开）。

### 3.1.11 F11 读回

- **行为**：`ReadbackStaging` 一次一份，并且**自带命令池与栅栏**（S7：读回不借任何会话的帧机器，C7）；颜色读回要求
  目标附件是 `R8G8B8A8_UNORM`，深度读回把 D32 转成 float 数组；结果是 `ReadbackResult` 四态（`NotReady` /
  `Unsupported` / `Invalid` / `Failed`，由调用方分类）。读回取体闸 EXCLUSIVE（设备的"帧之间"）并**计一次**
  `deviceWaits()`；能在等待前回答的拒绝（未知/未建目标、越界附件、不支持的格式、借用深度）不停止设备；帧体内
  回调读回答 `Failed`（`FrameBodyDepth`，不等待自己）。
- **约束**：`C1`、`R3`、`R28`。
- **相位一读的是值，不是记录**（A3-01）：能在等待前回答的拒绝由 `TargetTable::peek` 在 map 锁内取一份快照
  （`built` / `fresh` / 引擎形状）；记录本身会被 `ensure` 原地重建、被 `release` 交 park 在表外销毁，借
  `lookup` 再读它的字段没有可指的同步（TSan 曾在此报真实竞争）。答案允许**旧一个 pass**：
  `TargetObjects::fresh` 由帧体在记录路径上写（`C3`/`C5` 不许取锁），所以它是原子位；相位一拿它当提示，
  相位二在闸内重查。
- **验收**：`VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack`、`OffscreenClearTest`、`ContentTargetTest`，
  `VkMultiSessionTest` 的三例（交错读回各自最后一帧 / 等待且计数 / 帧体内拒绝）。
- **注意**：`P9`、`P7`（读回不在帧预算里）。

### 3.1.12 F12 管线缓存

- **行为**：会话内一个 `VkPipelineCache` 传给两处 `vkCreateGraphicsPipelines`；`shutdown()` 把
  `vkGetPipelineCacheData` 的 blob 写成设备键控文件（严格校验、原子写入）；下次 `initialize()` 用
  `pInitialData` 装回；驱动拒收时**不带数据重试一次**。
- **约束**：`C9`、`N5`。
- **验收**：`VkPipelineCacheFileTest.*`（5 例，不需要设备）、`VkPipelineCacheSessionTest.*`（4 例）。
- **注意**：`P9`、`P10`、`P11`。

### 3.1.13 F13 设备时钟

- **行为**：一对 `VK_QUERY_TYPE_TIMESTAMP` 包住一帧的命令缓冲，栅栏之后读回，写进 `FrameReport::timing`
  （`FrameTiming::gpu_micros`）；**每个 pass 另有一对边界**（写在它的上传之前，所以它要求的转换与拷贝算在它头上），
  读回后写进 `FrameTiming::passes`（执行序，名字取自 `RenderPass::name`，所以宿主可以拿它对自己读到的计划）；
  每 30 帧采样一次（实测这对时间戳在 lavapipe 上值 ~0.8 ms/帧），未采样帧**缺席**而不是重复上一次的值。
- **约束**：`C7`、`C13`。
- **验收**：`VkDeviceClockTest.AFrameReportsItsDeviceTime`、
  `VkPassTimingTest.TheReportAttributesTheFrameToThePassesThatSpentIt`（两个 pass、两个条目、名字对得上、
  各段之和不超过整帧）。
- **注意**：`P12`。

### 3.1.14 F14 报告与诊断

- **行为**：`frameReport()` 汇总一帧的计数器、进度、上传、计时；`prepareFrame` 回 `PrepareReport`；
  驱动消息与后端拒绝走同一条诊断路由；每个条件一个 `ReportOnce`。
- **约束**：`R3`、`R4`。
- **验收**：`BackendTest`、`ContentDrawTest` 的诊断计数断言。
- **注意**：`P8`（报告字段不能活在会被销毁的对象里）。

### 3.1.15 F15 暴露边界

- **行为**：`support/Handles.hpp` 的 `vkHandles()` 交出 `Handles`（instance / physical / device / queue / family /
  validation）；`Device::handles()` 在插件内部回答同一个值；帧路径（`src/internal/`）不 include `support/`。
- **约束**：`C4`。
- **验收**：`test_vk` 的 `vk_exposure`（ctest 内）。
- **注意**：—（门禁本身就是防错位的那道闸）。

## 4. 公开接口

### 4.1 `Device`（插件内部面）

| 接口 | 契约 | 备注 |
|---|---|---|
| `initialize()` / `shutdown()` / `initialized()` | 幂等；丢失后 `initialized()` 为 false | §2.2 |
| `executeFrame(frame, present_to)` | 一次提交 + 一次等待；`Presented` / `Skipped` / `Lost` | §2.3 |
| `prepareFrame(frame, present_to)` | 只录内容与传输，不画不呈现；可提交一批只含传输的命令 | ADR-2026-11-14-prepare-may-submit-transfers |
| `readColorBuffer` / `readDepthBuffer` | 画完即可读（由 `C1` 保证） | F11 |
| `setRenderTarget` / `releaseRenderTarget` | 目标由设备持有；释放即归还切片 | F4 / F10 |
| `releaseContent(...)` | 按身份留洞，下一帧补洞 | §2.5 |
| `createSurfaceTarget(handle, why)` | 一个窗口一个活动目标 | F3 |
| `capabilities()` | 只报实际启用的开关与限制 | §2.8 |
| `handles()` | 逃生门的值（插件内） | F15 |
| `contentPipelines()` / `contentCacheCounters()` | 成本模型证人（管线数、ring / set） | §2.4 |
| `memoryStats()` / `pipelineCacheReport()` | 内存池与缓存证人 | F10 / F12 |

完整签名与逐参数契约见 `include/vine/backend_vk/internal/Device.hpp`。

### 4.2 `BackendFactory` / `Plugin`

| 接口 | 契约 | 备注 |
|---|---|---|
| `BackendFactory::create(desc)` | 造设备（惰性）；`desc.cache_directory` 为空时套用默认目录 | §2.9 |
| `BackendFactory::setDefaultCacheDirectory` | 由 `Plugin::load` 用 `PluginLoadContext::ensureDataDirectory` 设置 | §2.9 |
| `Plugin::load` / `unload` | 注册/注销工厂；加载时取插件数据目录 | `shell/Plugin.cpp` |

### 4.3 内部单元

| 单元 | 责任 | 关键契约 |
|---|---|---|
| `Device` | 会话、帧、窗口表、报告（目标表见 `TargetTable`） | 本节 4.1 |
| `ContentDraw` | 内容/全屏录制、程序与管线、ring、set 缓存、管线缓存对象 | F5 / F6 / F8 / F9 / F12 |
| `ContentImages` | 纹理 revision、sampler、stand-in | F7 |
| `TargetTable` | 宿主目标 → 对象（map + 形状 + 构建/重建/修复）+ 每帧租约 + 释放/park + 无等待读的值快照（`peek`） | F4 / §2.18 |
| `TargetObjects` | 离屏目标的对象与 pass 变体 | F4 |
| `WindowTarget` / `WindowSystem` | swapchain 与 surface | F3 |
| `MemoryAllocator` | 设备内存池（块/切片/映射） | F10 |
| `PipelineCacheFile` | 缓存文件格式、校验、原子写 | F12 |
| `ReadbackStaging` | 一次读回的 staging 序列 | F11 |
| `BarrierPlan.hpp` / `StateMapping` | `stateOf` / `barrierBetween` / `loadOpOf` / `storeOpOf`；`StateMapping` 把 SDK 状态翻成 Vulkan 枚举 | §2.7 |
| `ShaderCompiler.hpp` / `ProgramVariant` | `compileStage` + `SpirvModule`（glslang）；variant 位 | F5 |
| `Handles`（`internal/` + `support/`） | 逃生门 | F15 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- **C1 一帧在飞**：`executeFrame` 返回时队列空闲；读回、释放、ring 增长都建立在这一点上（§2.3）。
- **C2 提交即等待**：每次提交都有与之配对的栅栏等待；`prepareFrame` 的传输批是唯一例外，且它自己等。
- **C3 录制期间不改计划**：`applyPlan` 在录帧时拒绝（SDK 的 `PlanResult::Busy`）；本后端不缓存“下一帧会变”的状态。
- **C4 边界**：`shell/` 不出现 `Vk*`；`internal/` 不被 `support/` 反向依赖；帧路径不使用 `vkHandles()`。
- **C5 帧间才生长**：ring、池块、管线缓存对象都只在没有命令缓冲引用它们时替换或增长。
- **C6 绑定偏移**：所有 `vkBind*Memory` 的 offset 满足资源 alignment，且不同 `MemoryKind` 的切片不共享一个
  `bufferImageGranularity` 页。
- **C7 能力=实际**：`capabilities()` 的每一位来自实际启用的 feature/limit，不来自“设备也许支持”。
- **C8 两棵树**：Debug 与 Release 都要构建、都要跑 `test_vk`；Release 的设备用例与 Debug 是同一批。
- **C9 缓存只信任同设备**：缓存文件的键必须与当前设备一致；不一致即拒绝，不“尽量用”。
- **C10 低层单元不依赖接缝**：`MemoryAllocator`、`PipelineCacheFile`、`WindowSystem`、`ReadbackStaging`、
  `Handles` 不得 `#include <vine/graphics/...>`；由 `check_vk_exposure.py` 的 E2 检查（新增单元要显式加入清单，
  这正是“决定它属于哪一半”的那一步）。
- **C11 分配失败的分类单一**：`MemoryAllocator` 只报事实（`MemoryRefusal` + `VkResult` + 数字），
  `Device::noteMemoryRefusal` 是唯一分类点；任何调用点都不得自行把失败归成“显存不足”（§2.12）。
- **C12 每条报告带稳定 code**：新写的 `reportDiagnostic` 必须带 `code`（`check_diagnostic_formats.py` 用
  baseline 设上限，超了即红）；`code` 一旦发布不改写、不复用（`scripts/diagnostic_codes.txt`）。
- **C13 归因是分段，不是整帧重复**：每个 pass 的时间是它与下一个边界之间的那一段（含它要求的上传与转换），
  所以各段之和 ≤ 整帧；把整帧记到每个 pass 上是错的（`VkPassTimingTest` 钉住）。

### 5.2 要求（R#）

- **R1** validation 层静默：`test_vk` 与真实宿主都不产生 VUID。
- **R2** 与 vsg 对拍：同一帧的渲染区域逐字节相同（`BackendParityTest`）。
- **R3** 一次情节一条：每个“做不到”只有一个 `ReportOnce`，条件结束时 `rearm` 在结束处。
- **R4** 证人数值可读：成本模型与缓存的每条结论都有 `Device` 上的数字，测试读它而不是数驱动调用。
- **R5** 门禁全绿：`check_ai_docs --strict`、`check_design_docs`、`check_doc_symbols`、`check_include_hygiene`、
  `check_api_style`、`check_public_docs`、`check_export_annotations`、`check_vk_exposure`、`check_diagnostic_formats`。
- **R6** 变异承重：每个守卫有一条“去掉它，对应用例必须红”的记录。

### 5.3 不变量（N#）

- **N1** 描述符集只在 ring 换代时整体清空；平帧不建不毁。
- **N2** 形状不变时 resize 不重建 render pass，因此不重编管线。
- **N3** 一帧的命令缓冲在 `fence` 信号之后才被重置/释放；没有对象在仍被引用时销毁。
- **N4** 池切片：同块内区间有序且 live/free 交替；释放只与两个邻居合并。
- **N5** 缓存文件要么被完整读回（magic/版本/键/长度都对），要么被拒；没有“半解析”。
- **N6** 设备丢失后 `initialized()` 为 false、帧被跳过、只报一次——**分配路径上的 `VK_ERROR_DEVICE_LOST` 与提交路径走同一条路**（`noteDeviceLost`，§2.12）。
- **N7** 一帧的 `FrameReport.timing` 要么属于这一帧，要么缺席；不复用上一帧的值。

## 6. 注意事项与陷阱

### 6.1 P1 全屏调用必须深度 Disabled

- **会怎么错**：全屏三角形在 z = 0（reverse-Z 远平面）；继承 pass 的 `TestAndWrite` 会整块被丢弃，**零 VUID**。
- **怎么判**：记录层每条 `recorded=1`，但画面是 clear 色；`readColorBuffer` 全等于背景。
- **怎么防**：`planScreen()` 强制 `DepthMode::Disabled`；用例 `AScreenCopyIsDrawnWhateverThePassDepthPolicySays`
  用“默认 pass 状态 + 带深度目标”的夹具钉住（旧夹具显式设了 Disabled，正是它没抓到的原因）。

### 6.2 P2 screen set 必须绑 pass 解析到的阴影图

- **会怎么错**：binding 7/8 存在 ≠ 绑对了图；白 stand-in + 零块让延迟路径“前向有阴影、延迟没有”，且无诊断。
- **怎么判**：前向/延迟画面差异；`ShadowTest` 的采样用例读到的深度是 255（stand-in）而不是地图值。
- **怎么防**：`recordScreen` 收 `ShadowSource`，有图绑真实深度 + 按调用打包的 `VineShadowBlock`，无图才 stand-in
  + `params.x = 0`。

### 6.3 P3 计划行按 `(owner, name)` 唯一

- **会怎么错**：同名声明互相覆盖，只剩最后一条（“只有第 4 块预览显示”）。
- **怎么判**：帧表里名字重复；`VN_FRAME_PLAN=1` 可直接看到。
- **怎么防**：预览/声明用唯一名字；改完对象必须**重新声明**（计划不读对象，读声明）。

### 6.4 P4 深度只在这三种形态下可采样

- **会怎么错**：MSAA 深度、借用深度、fresh 目标的深度，三者都可被当“可采样图”误用；borrow 的目标自己不拥有图。
- **怎么判**：validation 报 layout / usage；或者画面全黑。
- **怎么防**：`depthPicture()` / `depthPictureView()` 是唯一读图入口；借用目标回答 `VK_NULL_HANDLE`，读我们要去
  借出方。

### 6.5 P5 上传只能录在 render pass 之前

- **会怎么错**：`vkCmdCopyBufferToImage` 与部分 barrier 在 render pass 实例内非法。
- **怎么判**：validation 直接报；或纹理永远停在 TRANSFER_DST。
- **怎么防**：`preparePass` 先于 `vkCmdBeginRenderPass`；`ContentImages` 的“未准备”会给出明确 `why`。

### 6.6 P6 ring 只在帧间增长

- **会怎么错**：帧内销毁 ring 会踩到已录制命令缓冲命名的 buffer。
- **怎么判**：validation 的“destroyed while in use”；或崩溃。
- **怎么防**：帧内不足只记 `demand` 并跳过该 draw（报一次），`beginFrame` 再增长。

### 6.7 P7 池的“能否映射”是 memory type 的属性

- **会怎么错**：按“块最初为何创建”判断可映射性；在 device-local 与 host-visible 同 type 的设备（lavapipe、ReBAR）
  上把合法的 host-visible 切片拒掉，设备起不来（只报“no Vulkan device”）。
- **怎么判**：`ensureRing` 失败；`map()` 返回 null。
- **怎么防**：`map()` 查 `memoryTypes[type].propertyFlags & HOST_VISIBLE`；池必须在**第一个消费内存者**之前打开
  （窗口交换链的深度图在 `attach` 时就建）。

### 6.8 P8 缓存报告要快照，别读会被销毁的对象

- **会怎么错**：报告字段存在 `ContentDraw` 里，`shutdown()` 先 `content.destroy()` 再读——全变 0。
- **怎么判**：关机后读报告是 0，而文件确实写了。
- **怎么防**：`Device::Impl::CacheBooks` 在 `create()` 之后快照；只有累计量（`cache_hits`）在会话活着时读实时值。

### 6.9 P9 驱动可以什么都不校验

- **会怎么错**：lavapipe 广播 `VK_EXT_pipeline_creation_feedback` 却**从不报 hit**，也**不校验** `pInitialData`
  （改掉 blob 的 vendor 照收）。按驱动沉默判“缓存没生效”会误判。
- **怎么判**：`hits_measured == true` 但 `cache_hits` 恒为 0。
- **怎么防**：后端只声明自己能证明的一半——`preload_handed`（blob 确实交给了 `vkCreatePipelineCache`）；命中数
  上报而不强求；用例钉 hand-over，不钉驱动的沉默。

### 6.10 P10 缓存文件键必须属于设备

- **会怎么错**：把别的驱动/设备的缓存交给 `vkCreatePipelineCache` 是**未定义行为**，不是慢一点。
- **怎么判**：症状随机（驱动内部崩溃、错误管线）。
- **怎么防**：`PipelineCacheFile` 的键（vendor/device/driver/api/`pipelineCacheUUID`）+ 严格读；键不符直接拒绝。

### 6.11 P11 关机路径要能写盘

- **会怎么错**：进程被 `SIGKILL`/`SIGTERM` 杀掉时不会走 `shutdown()`，缓存文件不会出现。
- **怎么判**：`timeout` 跑的宿主永远没有缓存文件，而测试里有。
- **怎么防**：把“写缓存”放在 `shutdown()` 里且先于销毁缓存对象；测试用显式 `shutdown()` 的会话语义。

### 6.12 P12 时钟不是免费的

- **会怎么错**：每帧一对时间戳在软件光栅器上值 ~0.8 ms/帧（关 8.30 / 开 9.01–9.20，各三次）。
- **怎么判**：开时钟后稳态中位明显上移。
- **怎么防**：每 30 帧采样一次；未采样帧回答缺席。

### 6.13 P13 窗口测试的红先怀疑环境

- **会怎么错**：遗留窗口遮住测试窗口 ⇒ X11 读像素读到别人的画面；并行跑多个窗口测试互相抢 X。
- **怎么判**：单个用例单独跑是绿的；`read-error 8`（`BadMatch`）。
- **怎么防**：先 `pgrep` 清遗留进程；GUI 测试串行跑；“最后一行 `[ RUN ]`”不是崩溃点（stdout 重定向时 gtest 全
  缓冲，用 `stdbuf -oL`）。

### 6.14 P14 关机顺序：资源先于它借的东西

- **会怎么错**：先 `vkDestroyDevice` 再让资源的析构跑——析构里用陈旧的 `VkDevice` 与设备表，崩溃发生在看起来
  无关的地方；或先 `allocator.close()` 再释放目标，切片归还成了对已释放块的写。
- **怎么判**：关机时崩溃 / validation 的“destroyed while in use”；报告字段在关机后变 0（P8 的另一种形态）。
- **怎么防**：`shutdown()` 固定顺序——`vkDeviceWaitIdle` → 写缓存 → 内容 → 目标 → 窗口 → **池** → 命令池/栅栏/查询池
  → 设备 → instance；每步幂等，任一步失败都不跳过后续。

### 6.15 P15 一个 framebuffer 服务多个 render-pass 变体时，**依赖**也必须一致

- **会怎么错**：load op 不进兼容性，**但 subpass dependency 进**。给 LOAD 变体加宽 `srcStage/srcAccess`（“读到上一
  个 pass 写的深度”）而 CLEAR 变体不加，两个变体的依赖就不同，同一个 framebuffer 只与 CLEAR 变体兼容——第二个写同一
  个深度目标的 pass 开始录制时报 `VUID-VkRenderPassBeginInfo-renderPass-00904`（实测；只在“一帧两个 pass 写同一个
  带深度的目标”时出现，所以既有的跨目标用例看不见）。
- **怎么判**：validation 的 00904（renderPass 与 framebuffer 不兼容）；肉眼是“多 pass 同目标”的帧。
- **怎么防**：三个变体**用同一份依赖**，需要加宽就一律加宽（CLEAR 变体覆盖同样的源作用域不花钱）；用例
  `BackendParityTest.APrepareReportsTheShapeBothBackendsThenRecord` 在跑帧期间断言 vk 后端零 warning/error。

### 6.16 P16 分配失败被跨类

- **会怎么错**：把 `vkAllocateMemory` 的任何失败都当成“显存不足”——设备丢失的会话继续跑，宿主拿到的是“目标建不起来”，
  而它会去释放内容（没用），并在死设备上重试到进程结束。
- **怎么判**：`test_vk` 的 `VkMemoryFailureTest.ALostDeviceDuringAnAllocationEndsTheSession`（注入
  `VK_ERROR_DEVICE_LOST`：`initialized()` 必须转 false）与 `VkMemoryAllocatorTest.ARefusedBlockSaysWhichKindOfFailureItIs`
  （四类各自成值，驱动原始结果随报告走）。
- **怎么防**：分类只在一处（§2.12），池只报事实；`MemoryBlockCreator` 是失败路径的注入点——健康机器上
  `vkAllocateMemory` 不会失败，没有它这几条用例写不出来。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 引 `VMA` 做内存 | G8：今天的消费者（目标、纹理、流、ring、staging）由自建池足够覆盖；新依赖要有人付费 | — |
| D2 | 每资源一次 `vkAllocateMemory` | G10：分配次数随场景增长，`maxMemoryAllocationCount` 的失败形态是“看起来显存不足” | —（已改，见 F10） |
| D3 | 用 vsg 的 `ShaderCompiler` 编 SPIR-V | G2：那是把依赖指向另一个后端插件；glslang 由上层的 `cmake/VineGlslang.cmake` 统一供给 | — |
| D4 | 现在就上 synchronization2 / dynamic rendering | G8：当前目标是 API 1.2；现有屏障表是单一定义且被验证覆盖，升级到 1.3 时整层替换（审查 F9/F10） | — |
| D5 | 把“缓存文件格式”放进 SDK | G1：SDK 不知道 Vulkan；正确的接缝是“宿主给位置、后端给格式”（§2.9） | — |
| D6 | 多帧在飞直接实现 | G4：动“画完即读”的契约必须先有决策文档；实测收益上限约 10%（设备占 90%） | `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` |
| D7 | 现在就拆“设备 / 会话”两个对象 | G8：今天的契约是一帧在飞、一次一帧，拆分只会多一层转发；并发帧的需求已明确，所以按**两步**落地（先帧闸 + 契约，再拆分），而不是顺手一起做 | `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 全套行为 | `build/bin/test_vk`（169 例 / 41 套）；`ctest -R test_vk` |
| T2 | 与 vsg 逐字节一致 | `test_vsg` 的 `BackendParityTest`（13 例） |
| T3 | 暴露边界与低层免接缝 | `ctest -R vk_exposure`（V2 / V4 / E1 / E2，`scripts/check_vk_exposure.py`） |
| T4 | 成本模型不回归 | `VkUniformRingTest.*`、`VkMemoryPoolTest.ASteadyFrameAllocatesNothing`（关缓存的变异必须红） |
| T5 | validation 静默 | 上述运行中 `diagnosticCount(InitFailed)` 与 layer 计数为 0 |
| T6 | 真实宿主 | `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=48 ./Vine --open demo`：median ≈ 8.2 ms、11 pass / 10 draw、无 `vk:` 诊断 |
| T7 | 文档与门禁 | `python3 scripts/check_ai_docs.py --strict` + §5.2 R5 的脚本 |
| T8 | 变异承重 | 记录在 `../memory/graphics.md`：ring/set、池、缓存文件的变异各自只让对应用例红 |
| T9 | 读回 | `OffscreenClearTest`、`ContentTargetTest`、`VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack` |

## 9. 状态与演进

### 9.1 当前状态

- M0–M13 全部落地；审查项 F1/F2/F4/F7/F8/F12 已落地，审查 F5/F9/F10/F11/F13 按 §1.3 保留。
  多驱动者（backlog `T47` / `T48`）已全部落地，形状见 §2.11 与 `graphics-sessions.md`；
  **今天的三套 TSan 读数见 `graphics-threading.md` R18。**
- `test_vk` **169 例 / 41 套**（Debug 全绿，ASan 门禁 vk 范围 0 报告）；`test_vsg` **572**、`test_graphics` **617**；
  `BackendParityTest` **13/13**。目标 map、形状与租约是同一张 `TargetTable`（`TargetTableTest`）；帧首有一条存储证人，
  灯向量不每 pass 一份。
- **一条构建教训**：SDK 的虚表改动后，**动态插件必须一起重建**（旧 `.so` 让 `VsgBackendPluginTest` 段错误）。
- 实测（lavapipe，真实宿主）：11 pass / 10 draw，稳态中位 **8.24 ms**，GPU **7.15–7.74 ms**；ring + set 缓存改动
  −10%（9.03–9.23 → 8.16–8.30 ms）。
- 曾经记录为"已知仓库级现象"的 `test_vsg`（Release）偶发堆破坏/段错误已定位并修掉：vsg 有三处释放早于提交完成，
  证据与修复见 `../bugs/vsg-teardown-frees-in-flight-resources.md`；`T36` 的 `EachAttachmentClearsToItsOwnColour`
  偶发是另一件事（WSLg 下的窗口/设备创建，见 backlog）。

### 9.2 已知缺口 / 待办

- **审查 F5（多帧在飞）**：**已裁决保持一帧在飞**（`../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`），
  触发条件写在 ADR 里（设备时间 < 墙钟一半、或需要与 GPU 重叠的产物形态、或第二个后端要同一语义）。
- **审查 F9 / F10**：`vkCmdPipelineBarrier2` 与 dynamic rendering，等 API 1.3 目标。
- **backlog `T35`（成为默认后端）**的门禁（`../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`）：
  `releaseRenderTarget` 已真正释放附件、`readDepthBuffer` 与设备丢失报告已落地；**仍缺** Win32 实机证据（未过之前
  不支持 Windows 上线）、Wayland 回退实测、`releasePass` 的显式 no-op + 理由、`docs/guides/` 的 vk 用户页。
- **共享设备的并发帧**（backlog `T47` / `T48`）：已全部落地（§2.11、`graphics-sessions.md`）。形状与论证见
  `../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`。
- **backlog `T12` / `T13`**：Windows 窗口读像素、Wayland 回退实测。

### 9.3 兼容性 / 迁移

- **SDK**：`DeviceDesc` 追加 `cache_directory`（有默认值，旧的二参数聚合初始化不变）。
- **插件 ABI**：对宿主只有 `RenderBackend` / `RenderBackendFactory`；内部头（`internal/`）不承诺稳定。
- **缓存文件**：格式版本 `1`；版本不符即拒绝（不迁移、不猜测）。
- **设备要求**：Vulkan 1.2（多重采样深度 resolve 依赖 1.2 的 `vkCreateRenderPass2`）；1.3 设备多用核心
  creation feedback，不需要扩展。
- **平台**：Linux/X11 是当前唯一交付的窗口路径。

## 10. 参考

### 10.1 代码

- `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/**`：全部契约（`Device`、`ContentDraw`、
  `TargetObjects`、`ContentImages`、`WindowTarget`、`MemoryAllocator`、`PipelineCacheFile`、`ReadbackStaging`、
  `BarrierPlan`、`StateMapping`、`Handles`）。
- `src/plugins/gfx_backend_vk/src/shell/**`：`Plugin`、`BackendFactory`。
- `tests/test_vk/**`：行为；`tests/test_vsg/BackendParityTest.cpp`：对拍。
- `scripts/check_vk_exposure.py`：暴露边界门禁。

### 10.2 文档与 ADR

- `graphics-architecture.md`、`graphics-rendering.md`、`graphics-frame-pipeline.md`、`graphics-content-lifecycle.md`：
  SDK 侧的概念与一帧。
- `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`：成为默认后端的门禁。
- `../decisions/ADR-2026-11-14-prepare-may-submit-transfers.md`：`prepareFrame` 为什么可以提交一批传输。
- `../decisions/ADR-2026-11-14-sample-count-refused-not-lowered.md`：采样数“拒绝而不降级”。
- `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`：驻留由内容钉住。
- `../research/graphics-2026-11-14-vk-industry-review.md`：F1–F13 的审查与判词。
- `../research/graphics-2026-11-13-vk-exposure-boundary.md`：V1–V4 边界决定。

### 10.3 外部资料

- Vulkan 规范：*Memory Allocation*（`bufferImageGranularity`、`maxMemoryAllocationCount`）、*Synchronization and
  Cache Control*（render pass 内外的 barrier 规则）、*Pipeline Cache*（`pInitialData` 必须来自同一实现）、
  *Pipelines*（`VkPipelineCreationFeedback`）。
- Vulkan Validation Layers 的 VUID 文本：本后端的“零 VUID”目标按它核对。
- Vulkan Memory Allocator 的设计笔记（只作为“块 + 子分配 + 对齐 + 空闲表”的参考，不引为依赖）。
