# MSAA（2026-10-22）：采样数是**目标**的属性，颜色解析、深度不解析

> R6 的定稿记录（含为什么不是管线开关、为什么深度这一轮不解析、几条必须显式拒绝的组合）。
> 一句话：**`RenderTarget::setSamples` 是唯一的入口**，它进目标形状 ⇒ 进管线键 ⇒ 进渲染通道与图像；
> 离屏目标自建多重采样图并把**颜色**解析成单采样图，那张图就是"画面"。

## 0. 规则

| 谁 | 说什么 | 谁跟 |
| --- | --- | --- |
| 宿主 `RenderTarget::setSamples(n)` | "这个目标用 n 个样本画" | `TargetShape::samples` ⇒ `RenderPassCompatibility::samples` ⇒ 管线编译时的 `VkSampleCountFlagBits` |
| 后端 `HostTargets::describe()` → `facts().wanted.shape.samples` | 宿主的值**原样**上行（不取已建目标的旧值） | 帧编译：形状变了 ⇒ 计划答 `Rebuild` |
| `VsgExecutor` 的 Rebuild 分支 | 带着新样本数重建图与通道 | 新图集 + 新帧缓冲 + 新 pass 图 |
| `Capabilities::max_samples` | "这台设备的限制"（颜色与深度两者的较小值） | 宿主据此选；超限 ⇒ `State::SamplesUnsupported` + 一条诊断 |
| 没设置 | **1** | 单采样是与今天逐像素一致的路径（网关像素零变化） |

## 1. 为什么不是"管线开关"而是目标属性

样本数决定三件互相绑定的事：**图像的样本数**、**渲染通道附件描述的样本数**、**管线
的 `rasterizationSamples`**。
Vulkan 要求三者相等（`VUID-VkGraphicsPipelineCreateInfo-renderPass-06055`），而图像的
数量与形状归**目标**所有——管线只是被"编译成能进这个通道"。
所以键早就有了 `samples` 字段（`RenderPassCompatibility::samples`），本轮只是让它**能被
宿主设置**并**真的用到**：

- `TargetShape` 的比较与 `compatibility()` 已含 `samples`（既有代码，原先恒为 1）；
- `ContentPipeline::statesFor(samples)` 按样本数缓存一组 `GraphicsPipelineStates`
  （只替换 `MultisampleState` 一项，其余状态是层的策略、共享不修改）；
- `makeOffscreenRenderPass(..., samples, ...)` 把它写进附件描述与 resolve 子通道；
- `fillClearValues` 的条目数 = **通道附件数**（多重采样时 resolve 附件排在最后，也要
  占位，否则驱动读到表尾）。

## 2. 颜色解析、深度不解析

一轮只做**颜色**：

| 附件 | 多重采样时 | 为什么 |
| --- | --- | --- |
| 颜色 | 建 MS 图（只作 attachment）+ 单采样 resolve 图（`COLOR_ATTACHMENT｜SAMPLED｜TRANSFER_SRC`）；子通道 `resolveAttachments` 把 MS 写进 resolve 图 | 采样/拷贝/跨 pass 读取都要求单样本（`VUID-vkCmdCopyImageToBuffer-srcImage-00186`） |
| 深度 | 建 MS 图，**只**给 `DEPTH_STENCIL_ATTACHMENT` 用途 | 深度解析（`depthStencilResolveAttachments`）这一轮不做 |

"画面"因此只有一个定义：`Attachments::Color::picture()` —— 单采样时是颜色图本身，多重
采样时是 resolve 图。`capture(i)` / `probe(i)` 都走它，所以读回路径**不因样本数分叉**。

## 3. 三条必须显式拒绝的组合（不兜底）

深度没有被解析，就意味着那张 MS 深度图永远不在"可采样/可拷贝/可借"的布局与用途里。
于是一次性、带理由地拒绝，而不是"看起来能跑"：

| 组合 | 答案 | 理由 |
| --- | --- | --- |
| `samples > max_samples` | `HostTargets::State::SamplesUnsupported` + 诊断（"above the device's limit"） | 设备没有那个计数 |
| `samples > 1` **且** `depth_promotion` | 同上（"a multisampled target cannot lend, promote or read back its depth"） | 提升 = 让 shader 采样深度 ⇒ 需要单采样或解析 |
| `samples > 1` **且** `shareDepth(...)` | 同上 | 借出方的帧缓冲按自己的样本数命名同一张图，一张图不能同时是两种 |
| 多重采样目标的**深度回读** | `core::ReadbackRefusal::Multisampled`（宿主侧 `HostReadbackRefusal::Multisampled` ⇒ SDK `ReadbackResult::Unsupported`） | 永久事实，不是"这一帧还没拷" |

**默认窗口/默认帧缓冲仍是单采样**：交换链的样本数归平台。窗口抗锯齿的画法是"画进多重
采样离屏目标、把 resolve 图当屏幕 pass 的输入"，这正是引擎既有预设的形状，不需要新的
API。

## 4. 判据

设备无关：

- `CoreReadbackTest.AMultisampledTargetIsReadableInColourAndRefusedInDepth`：颜色照服
  务、深度答 `Multisampled`（且"没有深度"先答 `UnknownAttachment`）；
- `HostTargetsTest.TheSampleCountLimitStartsAtOneAndZeroMeansOne`：默认 1、0 视作 1；
- `FrameScheduleTest.ATargetsSampleCountTravelsWithItAndDefaultsToOne`：往返 + 默认 + 一
  帧不吞掉这个值。

设备级（lavapipe）：

- `MsaaTargetTest.FourSamplesBlendThePixelsATriangleEdgePassesThrough`：同一个三角形、
  同一个 shader，**1 样本时没有任何"混合"像素（0 个），4 样本时一定 > 0**；内部仍是
  纯绿、角落仍是清除色（解析只平均样本，不洗淡整幅）；
- `MsaaTargetTest.AFourSampleTargetRefusesItsDepthReadbackAndKeepsItsColourReadback`；
- `MsaaTargetTest.ADescriptionTheDeviceCannotBuildIsRefusedWithTheReasonItCannot`：
  超限/借深度/提升深度三种拒绝，**外加对照组**（普通 MS 深度能建 ⇒ 拒绝的是组合，不是
  多重采样）；
- `MsaaTargetTest.RebuildingAtANewCountReplacesTheImages`：活目标换计数 = 重建（图集、
  帧缓冲、pass 图都换），深度策略仍归目标。

回归：`ctest` 24/24；`vsg_rewrite_gate.sh` 全阶段干净（**1 样本像素零变化**，VUID 0）；
6 个静态网关 0 finding。

## 5. 这一轮没做的（有意）

- **深度解析**：要 `depthStencilResolveAttachments` + 一张单采样深度图，且"哪个样本
  赢"是策略（min/max/sample0）。做了它才能解除 §3 的后两条拒绝。
- **样本级着色（sample shading）/alpha-to-coverage**：`MultisampleState` 的字段都在，
  但没有宿主词汇；等真有需求再给它们写进 `PassDesc`。
- **交换链多重采样**：归窗口后端（`WindowTarget::refresh`）。

## 6. 实现落点（改哪儿）

| 文件 | 改了什么 |
| --- | --- |
| `sdk/vine/graphics/RenderTarget.hpp/.cpp` | `samples()` / `setSamples()`（0 当 1）；文档说明它是目标属性、离屏会被解析、窗口是单采样 |
| `sdk/vine/graphics/Capabilities.hpp` | `max_samples{1}`：**限制**而非特性位，取颜色/深度两者的较小值 |
| `sdk/vine/graphics/backend/Readback.hpp/.cpp` | `ReadbackState::multisampled` + `ReadbackRefusal::Multisampled`（深度先于格式/拷贝判定） |
| `plugin/internal/HostTargets.*` | `Description::samples`、`setMaxSamples/maxSamples`、`State::SamplesUnsupported`；`facts()` 上行宿主的值（并让"设备半"只在样本数也一致时才声明） |
| `plugin/internal/OffscreenTarget.*` | `TargetLayout::samples`、MS 图 + resolve 图、`Attachments::Color::picture()`、MS 深度的用途/回读限制、`fillClearValues` 按附件数、帧缓冲视图顺序 |
| `plugin/internal/ContentPipeline.*` | `statesFor(samples)`：按计数缓存状态组，`acquire()` 用 `key.compatibility.samples` |
| `plugin/internal/StateCommands.*` | `samplesToVk()`：**唯一**一处计数映射（通道与管线共用） |
| `plugin/internal/VsgBackend.cpp` | 初始化时把设备上限写进 `HostTargets`；`capabilities()` 报同一个数；拒绝诊断按原因分支 |
| `plugin/internal/VsgExecutor.cpp` | Rebuild 分支带上 `layout.samples` |
