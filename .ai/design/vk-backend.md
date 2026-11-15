# vk 后端：直连 Vulkan 的渲染后端

> **状态**：现行
> **最后核对**：2026-11-15
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

- SDK 的 backend half（`vine/graphics/backend/`）需要一个**第二实现**来被评判：只有一个后端时，“这是接缝的要求”
  与“这是 vsg 的习惯”分不开。本插件是那个第二实现，路径是**直连 Vulkan**（自建 `VkInstance` / `VkDevice` /
  pipeline），不经 VulkanSceneGraph。
- 它消费的是接缝，不是引擎：绘制什么由 `FrameDescription` 说，怎么画由本后端说。这使它可以被 `test_vk` 直接
  驱动（插件是 MODULE，测试编译其源码），**不需要窗口**。
- 现状：里程碑 M0–M13 全部落地；工业标准审查（`../research/graphics-2026-11-14-vk-industry-review.md`，F1–F13）
  中资源与成本模型项（审查 F1 / F2 / F4 / F7 / F8 / F12）已落地。行为由 `test_vk` **89 例 / 23 套**钉住，与 vsg 的对拍
  由 `BackendParityTest`（12 例，含在 `test_vsg` 内）保证。

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
- **成为默认后端**：门禁在 `../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`，未完成前 `"vsg"` 是
  生产后端（backlog T35）。

### 1.4 一句话结论

**一个会话 = 一个设备 + 一个内存池 + 一个管线缓存；一帧 = 一份命令缓冲 + 一次提交 + 一次栅栏等待；每个能力、
每个拒绝、每个缓存都有一个可被测试读取的证人。**

## 2. 设计要点

### 2.1 半边：shell 只露接缝，Vulkan 只住 internal

- **是什么**：`shell/`（`Plugin`、`BackendFactory`）只命名 `RenderBackend` / `RenderBackendFactory`；`internal/`
  才是 Vulkan；`support/Handles.hpp` 是唯一把 `Vk*` 交给外部（测试 / 工具）的门。
- **为什么**：G1（概念各有其位）+ G2（依赖单向：`internal` → SDK → `base`，`shell` 不反向依赖 Vulkan 类型）。
  这也让“把 vk 换成别的实现”是一次插件替换，而不是 SDK 变更。
- **边界**：`scripts/check_vk_exposure.py` 是门禁（V2 SDK backend 面、V4 插件 shell、E1 帧路径），跟着 ctest 跑。

### 2.2 设备是会话语义，丢失是第一种失败

- **是什么**：`initialize()` / `shutdown()` 幂等；`initialized()` 在 `VK_ERROR_DEVICE_LOST` 之后转 `false`，帧被
  跳过而不是继续触碰设备；重建路径固定为 `shutdown()` + `initialize()`。
- **为什么**：G4（失败模式写在类型/契约里）+ G11（一种失败可被定位）。设备丢失不是“某一帧失败”，而是**会话
  结束**——把它当帧错误处理，会让宿主在死设备上重试到进程结束。
- **边界**：`noteDeviceLost` 是唯一分类点（队列提交、acquire、present 三处都经过它），一次情节一条报告。

### 2.3 一帧一次提交：同步换确定性

- **是什么**：`executeFrame` 录一份命令缓冲 → 提交 → 等自己的栅栏 → 读回/释放可以在同一次调用后立即发生；
  `prepareFrame` 例外地允许**一批只含传输**的提交（ADR-2026-11-14-prepare-may-submit-transfers）。
- **为什么**：G3（流程一趟读完）+ G11（“画完即读”让测试与宿主都不需要猜时序）。代价写在这里：**CPU 与 GPU
  不重叠**，实测 CPU 中位 8.2–9.0 ms / GPU 7.15–7.74 ms，即 CPU 是瓶颈；多帧在飞是审查 F5 的议题（记录在 backlog，动它要 ADR）。
- **边界**：这条契约决定了所有“释放前无需 idle”的说明（租赁、纹理换代、ring 增长）都成立；破坏它要同时改
  释放策略与读回语义，因此需要 ADR，而不是一处 `waitForFence` 的删除。

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
- **边界**：`ResizeInPlace` 保留 render pass（管线兼容），`Rebuild` 才换形状；`TargetImages` 的三个 load-op 变体
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
| F11 | 读回 | 颜色（RGBA8）与深度（D32）；staging 一次一份 | 已落地 |
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

- **行为**：`TargetImages` 持有颜色/采样缓冲/深度/深度 resolve 图像与视图、三个 render pass 变体（清深度、载
  深度、全载）、一个 framebuffer。MSAA 目标把颜色 resolve 进单采样图；深度用 `vkCreateRenderPass2` 的
  `pDepthStencilResolveAttachment`（`sample0`）resolve；深度-only 目标即阴影图；`shareDepth` 的目标借用另一个
  目标的深度视图。
- **约束**：`C5`、`C8`、`N2`。
- **验收**：`MsaaTest`、`ContentTargetTest`、`OffscreenClearTest`、`DeferredTest`。
- **注意**：`P4`。

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

- **行为**：`ReadbackStaging` 一次一份（buffer + 一次性命令缓冲 + 提交/等待/映射）；颜色读回要求目标附件是
  `R8G8B8A8_UNORM`，深度读回把 D32 转成 float 数组；结果是 `ReadbackResult` 四态（`NotReady` / `Unsupported` / `Invalid` / `Failed`，由调用方分类）。
- **约束**：`C1`、`R3`。
- **验收**：`VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack`、`OffscreenClearTest`、`ContentTargetTest`。
- **注意**：`P9`。

### 3.1.12 F12 管线缓存

- **行为**：会话内一个 `VkPipelineCache` 传给两处 `vkCreateGraphicsPipelines`；`shutdown()` 把
  `vkGetPipelineCacheData` 的 blob 写成设备键控文件（严格校验、原子写入）；下次 `initialize()` 用
  `pInitialData` 装回；驱动拒收时**不带数据重试一次**。
- **约束**：`C9`、`N5`。
- **验收**：`VkPipelineCacheFileTest.*`（5 例，不需要设备）、`VkPipelineCacheSessionTest.*`（4 例）。
- **注意**：`P9`、`P10`、`P11`。

### 3.1.13 F13 设备时钟

- **行为**：一对 `VK_QUERY_TYPE_TIMESTAMP` 包住一帧的命令缓冲，栅栏之后读回，写进 `FrameReport::timing`
  （`FrameTiming::gpu_micros`）；每 30 帧采样一次（实测这对时间戳在 lavapipe 上值 ~0.8 ms/帧），未采样帧**缺席**
  而不是重复上一次的值。
- **约束**：`C7`。
- **验收**：`VkDeviceClockTest.AFrameReportsItsDeviceTime`。
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
| `Device` | 会话、帧、目标表、窗口表、报告 | 本节 4.1 |
| `ContentDraw` | 内容/全屏录制、程序与管线、ring、set 缓存、管线缓存对象 | F5 / F6 / F8 / F9 / F12 |
| `ContentImages` | 纹理 revision、sampler、stand-in | F7 |
| `TargetImages` | 离屏目标的对象与 pass 变体 | F4 |
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
- **N6** 设备丢失后 `initialized()` 为 false、帧被跳过、只报一次。
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

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 引 `VMA` 做内存 | G8：今天的消费者（目标、纹理、流、ring、staging）由自建池足够覆盖；新依赖要有人付费 | — |
| D2 | 每资源一次 `vkAllocateMemory` | G10：分配次数随场景增长，`maxMemoryAllocationCount` 的失败形态是“看起来显存不足” | —（已改，见 F10） |
| D3 | 用 vsg 的 `ShaderCompiler` 编 SPIR-V | G2：那是把依赖指向另一个后端插件；glslang 由上层的 `cmake/VineGlslang.cmake` 统一供给 | — |
| D4 | 现在就上 synchronization2 / dynamic rendering | G8：当前目标是 API 1.2；现有屏障表是单一定义且被验证覆盖，升级到 1.3 时整层替换（审查 F9/F10） | — |
| D5 | 把“缓存文件格式”放进 SDK | G1：SDK 不知道 Vulkan；正确的接缝是“宿主给位置、后端给格式”（§2.9） | — |
| D6 | 多帧在飞直接实现 | G4：动“画完即读”的契约必须先有决策文档与迁移计划 | 待写（审查 F5） |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 全套行为 | `build/bin/test_vk`（89 例 / 23 套）；`ctest -R test_vk` |
| T2 | 与 vsg 逐字节一致 | `test_vsg` 的 `BackendParityTest`（12 例） |
| T3 | 暴露边界 | `ctest -R vk_exposure`（`scripts/check_vk_exposure.py`） |
| T4 | 成本模型不回归 | `VkUniformRingTest.*`、`VkMemoryPoolTest.ASteadyFrameAllocatesNothing`（关缓存的变异必须红） |
| T5 | validation 静默 | 上述运行中 `diagnosticCount(InitFailed)` 与 layer 计数为 0 |
| T6 | 真实宿主 | `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=48 ./Vine --open demo`：median ≈ 8.2 ms、11 pass / 10 draw、无 `vk:` 诊断 |
| T7 | 文档与门禁 | `python3 scripts/check_ai_docs.py --strict` + §5.2 R5 的脚本 |
| T8 | 变异承重 | 记录在 `../memory/graphics.md`：ring/set、池、缓存文件的变异各自只让对应用例红 |
| T9 | 读回 | `OffscreenClearTest`、`ContentTargetTest`、`VkMemoryPoolTest.AReadbackGivesItsStagingBytesBack` |

## 9. 状态与演进

### 9.1 当前状态

- M0–M13 全部落地；审查项 F1/F2/F4/F7/F8/F12 已落地，审查 F5/F9/F10/F11/F13 按 §1.3 保留。
- `test_vk` **89 例 / 23 套**（Debug 与 Release 各一次全绿；Release 连跑 5 次无波动）；Debug 全量 ctest **32/32**；
  `BackendParityTest` **12/12**。
- 实测（lavapipe，真实宿主）：11 pass / 10 draw，稳态中位 **8.24 ms**，GPU **7.15–7.74 ms**；ring + set 缓存改动
  −10%（9.03–9.23 → 8.16–8.30 ms）。
- 已知仓库级现象：`test_vsg` 在 Release 偶发堆破坏/段错误（`FrameUploadsTest` / `PrepareUploadTest` 附近），
  A/B 证明**旧提交同样发生**，与本文档的后端无关（backlog T8 / T36 同族）。

### 9.2 已知缺口 / 待办

- **审查 F5（多帧在飞）**：需要 ADR（会改“画完即读”）。
- **审查 F9 / F10**：`vkCmdPipelineBarrier2` 与 dynamic rendering，等 API 1.3 目标。
- **backlog `T35`（成为默认后端）**的门禁（`../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`）：
  `releaseRenderTarget` 已真正释放附件、`readDepthBuffer` 与设备丢失报告已落地；**仍缺** Win32 实机证据（未过之前
  不支持 Windows 上线）、Wayland 回退实测、`releasePass` 的显式 no-op + 理由、`docs/guides/` 的 vk 用户页。
- **backlog `T41`**：`PrepareReport.passes` 与录制后的 `FrameCounters.passes` 口径不一致（SDK 字段契约缺陷，跨后端）。
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
  `TargetImages`、`ContentImages`、`WindowTarget`、`MemoryAllocator`、`PipelineCacheFile`、`ReadbackStaging`、
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
