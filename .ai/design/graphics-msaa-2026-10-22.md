# MSAA（2026-10-22）：采样数是**目标**的属性，颜色与深度各解析一张图

> R6（第一片：颜色解析）与 R6b（同日：深度解析）的定稿记录，含为什么不是管线开关、几条必须显式拒绝的组合。
> 一句话：**`RenderTarget::setSamples` 是唯一的入口**，它进目标形状 ⇒ 进管线键 ⇒ 进渲染通道与图像；
> 离屏目标自建多重采样图并把**颜色与深度**各解析成一张单采样图，那两张图就是"画面"。

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

## 2. 两个附件都解析，读的都是"画面"

| 附件 | 多重采样时 | 为什么 |
| --- | --- | --- |
| 颜色 | 建 MS 图（只作 attachment）+ 单采样 resolve 图（`COLOR_ATTACHMENT｜SAMPLED｜TRANSFER_SRC`）；子通道 `resolveAttachments` 把 MS 写进 resolve 图 | 采样/拷贝/跨 pass 读取都要求单样本（`VUID-vkCmdCopyImageToBuffer-srcImage-00186`） |
| 深度 | 建 MS 图（**只**给 `DEPTH_STENCIL_ATTACHMENT`）+ 单采样 resolve 图（`DEPTH_STENCIL_ATTACHMENT｜TRANSFER_SRC[｜SAMPLED]`）；子通道 `depthStencilResolveAttachments` + `depthResolveMode = sample0` | 同上；`sample0` 让"打开多重采样"**不改变深度值**（不平均） |

"画面"因此只有一个定义：`Attachments::Color::picture()` / `Attachments::depthPicture()` —— 单采样时是
绘制图本身，多重采样时是 resolve 图。`capture(i)` / `probe(i)` / `captureDepth()` / `depthProbe()` /
借出 / 提升都走它，所以**读回与共享路径不因样本数分叉**。

## 3. 必须显式拒绝的组合（不兜底）

| 组合 | 答案 | 理由 |
| --- | --- | --- |
| `samples > max_samples` | `HostTargets::State::SamplesUnsupported` + 诊断（"above the device's limit"） | 设备没有那个计数 |
| `samples > 1` **且** `shareDepth(...)`（多重采样**借入**） | 同上（"borrows its depth from another target…"） | 借到的是出借方的单采样解析图，而一个帧缓冲的附件共用一个样本数 |

**出借方可以是多重采样**（借出的是它的解析图），**提升也可以是**（采样的是解析图）—— 这两条在 R6 第一片
里也是拒绝项，R6b 之后合法，见 §7。

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
- `MsaaTargetTest.AFourSampleTargetsDepthIsTheResolvedPicture`：4 样本的深度可读，**没有中间值**
  （`sample0` 不平均），内部/外部与 1 样本逐点一致；
- `MsaaTargetTest.ADescriptionTheDeviceCannotBuildIsRefusedWithTheReasonItCannot`：
  两种拒绝（超限、多重采样借入），**外加两个对照组**（多重采样出借、多重采样提升都能建）；
- `MsaaTargetTest.RebuildingAtANewCountReplacesTheImages`：活目标换计数 = 重建（图集、
  帧缓冲、pass 图都换），深度策略仍归目标。

回归：`ctest` 24/24；`vsg_rewrite_gate.sh` 全阶段干净（**1 样本像素零变化**，VUID 0）；
6 个静态网关 0 finding。

## 5. 这一轮没做的（有意）

- **resolve mode 的宿主词汇**：深度固定 `sample0`（不平均、不改值）。min/max 是深度缓冲的语义而不是
  画质开关，等真有反例再开口子。
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

## 7. R6b（同日）：深度也解析

- **图像**：MS 深度图只给 `DEPTH_STENCIL_ATTACHMENT`；另一张单采样 resolve 图给
  `DEPTH_STENCIL_ATTACHMENT｜TRANSFER_SRC[｜SAMPLED]` —— 回读、借出、提升都读它。**用途必须在建视图
  之前定**：vsg 在第一次 `createImageView` 时真的 `vkCreateImage`，之后再改 `usage` 不生效（实测：阴影图
  那条路径直接段错误、无诊断）。
- **通道**：`depthStencilResolveAttachments` + `depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT`；
  `stencilResolveMode` 只有格式**真带模板**（`D24` ⇒ `D24_UNORM_S8`）才 `SAMPLE_ZERO`，否则 `NONE`
  （`VUID-VkSubpassDescriptionDepthStencilResolve-pDepthStencilResolveAttachment-03178`）。
- **一条"画面"规则管两个附件**：`depthSteadyLayout()` 描述的就是解析图的布局；回读的拷贝与屏障、借出方
  的 attachment、提升的 SHADER_READ_ONLY 全部指向 `depthPicture()`。
- **解除三条**：多重采样目标现在可以提升深度、可以出借深度、可以 `readDepthBuffer()`。`ReadbackState` 的
  `multisampled` 字段与 `ReadbackRefusal::Multisampled`（及宿主侧对应项）**一起删掉** —— 词汇里不留一个
  已经不会被报告的理由。
- **保留一条**：多重采样目标**借入**深度仍被拒（借到的是单采样解析图，而一个帧缓冲的附件共用样本数），
  `HostTargets`（带诊断）与 `OffscreenTarget::create`（直接调用者也绕不过）两处都拒。
- 判据：`MsaaTargetTest.AFourSampleTargetsDepthIsTheResolvedPicture`（4 样本深度可读、无中间值、与 1 样本
  逐点一致）、拒绝用例改成"两种拒绝 + 两个对照组"；全仓 `test_vsg` 480 例 0 失败。
