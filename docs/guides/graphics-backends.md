# 图形后端：契约与差异表

> **状态**：现行 ｜ **最后核对**：2026-11-23
> **范围**：宿主与引擎看到的**后端契约**（两个后端必须答一样的那部分）、两个后端的**有意差异**、
> **支持矩阵**、接缝与文件入口、构建与证据。
> **本文只写用户/维护者需要的事实**；每个后端的契约、设计决定与坑在
> [`.ai/design/vk-backend.md`](../../.ai/design/vk-backend.md) 与
> [`.ai/design/vsg-backend.md`](../../.ai/design/vsg-backend.md)（不在这里重复）。
> 引擎侧怎么用图形模块见 [`graphics.md`](graphics.md)；`RenderControl` 与视图见
> [`render-views.md`](render-views.md)。

## 1. 两个后端与当前定位

| 后端 | 注册名 | 插件 | 定位 | 平台 | 设备下限 |
| --- | --- | --- | --- | --- | --- |
| 直连 Vulkan | `vk` | `gfx_backend_vk` | **生产后端、最终替代 vsg**（[ADR-2026-11-14](../../.ai/decisions/ADR-2026-11-14-vk-becomes-the-default-target.md)） | Linux/X11、Win32 | Vulkan 1.2（多采样深度 resolve 用 1.2 的 `vkCreateRenderPass2`） |
| VulkanSceneGraph | `vsg` | `gfx_backend_vsg` | **参考实现 + 逐字节对拍的另一端**；能力齐全，但不作为切换目标 | Linux/X11、Win32 | Vulkan 1.4（`DeviceRequirements` 在会话建立时检查） |

* 两个后端都实现 `vn::graphics::RenderBackend`，宿主经 `RenderBackendRegistry::create(name, desc)` 按名创建，
  编译期不依赖任何后端。
* **哪个后端跑由应用点名，不由名字序**：宿主启动时点名（`RenderControl::setDefaultBackendName`，Vine 写的是
  `vk`），`VN_RENDER_BACKEND=vsg ./Vine --open demo` 用同一份构建跑参考后端。应用没点名时只接受**唯一注册**的那一个；
  多个已注册又没点名，attach 直接失败并在理由里列出全部注册名——**绝不按名字序取第一个**（那是名字的偶然，不是决定）。
  这是回退行为，不是切换门的结论（切不切见下一条）；两个后端都随包安装，按名创建可显式选 `vsg`。
* **切换门**（ADR-2026-11-14）：`releaseRenderTarget` / `readDepthBuffer` / 设备丢失报告 / `releasePass` 契约都已落地，
  `docs/guides` 的后端页也就是本页；Win32 分支也已实现且**窗口像素已断言**——`WindowPresentTest` 的两条用例在两个平台上
  走同一件仪器（`tests/test_vsg/TestHostWindow.hpp`：X11 走 `XGetImage`，Win32 走 `PrintWindow` 合成器读回），Windows 上
  `test_vk` 174 例全绿、无 SKIP。**仍缺** Wayland 回退实测与一份切换门接受的性能读数——在那之前 Windows 不上线。
  删 vsg 不是这一步：它是另一件要捆绑的事（backlog T60；条款见
  [ADR-2026-11-23](../../.ai/decisions/ADR-2026-11-23-retiring-the-reference-backend.md)）。
* 注册表**后注册者胜且不报告**：加载一个同名插件会把内建条目换成插件自己的工厂。必须验证"编进本二进制的实现"
  的代码（对拍）直接构造后端，不经注册表（契约写在 `RenderBackendRegistry::registerFactory` 的头注释里）。

## 2. 共同契约（宿主不必知道是哪个后端）

以下行为由 SDK 头定义，两个后端一致；本文只列入口，契约以头文件为准。

| 入口 | 承诺 |
| --- | --- |
| `RenderBackend::initialize()` / `shutdown()` | `false` 之后没有会话；失败自己清理半成品；`shutdown()` 后再 `initialize()` 是合法重来 |
| `RenderFrameExecutor::executeFrame(value, present_to)` | 一帧的整个身体：开帧 → 按帧值走 pass → 编译 → 记录/提交 → 帧尾 present（`present_to` 为空走离屏会话、不 present） |
| `RenderFrameExecutor::prepareFrame(...)` | 预热：构建、不取图、不录制、不提交、不 present；之后的同值帧构建为 0 |
| `RenderDevice::capabilities()` | `render_targets`、`presentation`（创建意图）、`Features` / `Limits`；缺席不是零 |
| `RenderDevice::createFrameDriver()` / `releaseFrameDriver()` | 一个设备服务多个窗口/引擎；每个驱动者有自己的帧机器与暂存 |
| `createSurfaceTarget` / `setRenderTarget` / `releaseRenderTarget` / `releasePass` | 目标由设备持有；窗口目标一格一次（第二次答 `WindowOccupied`）；释放后下一帧重建 |
| `releaseContent(Object)` / `releaseContent(Buffer)` | 放掉 GPU 驻留，对象仍有效；释放本身不增加 `deviceWaits()` |
| `readColorBuffer` / `readDepthBuffer` | 先分类再工作：`Unsupported`（永远不行）与 `NotReady`（还没画）是不同答案；读回按每次一次计数等待设备 |
| `frameReport()` | 本帧成本（passes/draws、uploads、locking、retention、frames）+ 报告一次性的诊断 episode |
| `PresentTarget::present()` | **机制**：帧尾由执行器调用，宿主不直接调 |

三条横切规则（细节在 `.ai/design/`）：

* **一帧三相位**：帧首解析 → 录制 → 提交；录制路径不取设备锁，`FrameReport::locking` 是帧首/提交的 acquisition 数，
  稳态为常数、`waits` 为 0（[`graphics-threading.md`](../../.ai/design/graphics-threading.md) C3）。
* **拒绝一种形状**：引擎/设备/宿主三种来源的 `RenderDiagnostic` 都带 `code` / `subject` / `limit`；
  `diagnosticCount()` 只数引擎 + 设备，宿主经 `reportEngineProblem` 注入的另算（`DiagnosticOrigin::Host`）。
* **驻留由内容声明钉住**：几何行声明 stream 的 buffer、材质行与 pass 导入声明贴图；裁剪不丢上传，
  离树过一个 `slots + 1` 的窗口后才离场（[`graphics-content-lifecycle.md`](../../.ai/design/graphics-content-lifecycle.md)）。

## 3. 能力差异表（有意不同）

这张表列的是**契约允许的差异**：每一行的两种答案都满足 §2 的承诺；不满足承诺的差异是缺陷，不是差异。

| 能力 | `vk` | `vsg` | 说明 |
| --- | --- | --- | --- |
| 设备内存池 | **有**：自建 suballocator，按设备 `bufferImageGranularity` 切片，处理 `maxMemoryAllocationCount` | 交给 vsg 的分配器 | 两套账各自成立；宿主看不到差异 |
| 管线缓存文件 | **有**：`DeviceDesc::cache_directory` → `PipelineCacheFile`，按设备键读写、拒绝即丢弃并报告 | **无**（vsg 1.1.16 没有 cache 管理，backlog T10） | 只有 vk 用这个字段；空目录 = 不用 |
| GPU 计时 | 按设备能力报 `Feature::GpuTimings`；每 pass 时间来自时间戳 | 同一条 `Feature`，来自设备时间戳 | "设备能写时间戳"不等于"本后端在报告"，读数看 `frameReport()` / 帧表 |
| 验证层 | validation + debug utils **可用即开** | vsg 自己的验证开关 | 只影响诊断量，不影响契约 |
| 平台 | Linux/X11、Win32 | Linux/X11 + Win32 | 切换门的一项 |
| 渲染通道 | 自建 `VkRenderPass` 变体（全 CLEAR / 深度 LOAD / 全 LOAD）+ MRT/MSAA resolve | vsg 的 render pass 机制 | 同一种 `RenderPassCompatibility` 进入两边管线键 |
| 窗口 | `createSurfaceTarget` 一格一次（第二次答 `WindowOccupied` 并**报一次**具名诊断），`nullptr` 请求设备自己的窗口 | 同一契约；窗口尺寸与格式的答案问平台 | 窗口目标由设备发放，设备/引擎不是任何一块窗口 |
| 深度读回 | 纯深度格式（D16/D32/D32F，MSAA 读 resolve 图）；D24 与借用深度答 `Unsupported` | 同分类 | 格式表在 SDK（`vn::graphics::depthReadbackOf`），两边共用 |
| 顶点输入声明 | 只声明**顶点段真正读的** location（从编译出的 SPIR-V 读回；矩阵段按列算，实例流 12–15 全保留） | 声明几何的全部通道：未读的通道不会被取数，驱动也不报错 | 只是声明面，不影响取数；vk 这样声明是为了 pipeline 说真话（否则验证层报 "attribute at location N not consumed"） |
| 目标拒绝的精度 | 构建时给出 `vk.target.size_unknown` / `vk.target.borrow_extent_mismatch` | 同一条件在构建时只答 `vsg.target.build_failed`；尺寸不匹配到 resize 阶段才拒绝 | 契约只要求"拒绝 + 一个 code"，粒度不是契约；要统一就给 vsg 补两条 code |

## 4. 支持矩阵（唯一一份）

| 能力 | `vk` | `vsg` | 证据（同一条用例在两边都跑就是"对拍"） |
| --- | --- | --- | --- |
| 内容绘制（索引/非索引、点/线/三角） | ✅ | ✅ | `test_vk` / `test_vsg` + `BackendParityTest` |
| MRT、深度附件、逐附件清屏 | ✅ | ✅ | `VkContentTargetTest`、`VsgBackendTest` |
| MSAA（目标属性）与深度 resolve | ✅ | ✅（vsg 侧由目标变体承担） | `VkMsaaTest`、`MsaaTargetTest` |
| 借用深度（`shareDepth`） | ✅ | ✅ | `VkContentTargetTest`、`SharedDepthTest` |
| 材质（贴图/立方体/白图 fallback）、灯（方向光 + 环境） | ✅ | ✅ | `VkContentDrawTest`、`ContentDrawTest` |
| 阴影（方向光） | ✅ | ✅ | `VkShadowTest`、对拍第 12 例 |
| 延迟链、全屏调用、push 范围 | ✅ | ✅ | `VkDeferredTest`、对拍 |
| 自定义程序（声明式 ABI + 变体 + 实例流） | ✅ | ✅ | `VkBuiltinProgramTest` / `VkVariantTest`、`ProgramAbiTest` |
| 读回（颜色 RGBA8、纯深度浮点） | ✅ | ✅ | 各自的读回用例 |
| 窗口（采纳宿主表面、跟随 resize、丢帧自愈） | ✅ | ✅ | `VkWindowPresentTest`（Win32 上读的是合成器）、`VsgBackendTest` |
| 线框 / 点视图、线宽、各向异性 | 按设备 | 按设备 | `Feature::Wireframe` / `AnisotropicFiltering` + `Limits` |
| 设备内存池 / 管线缓存文件 | ✅ | — | 见 §3（有意不同） |
| Win32 窗口 | ✅ | ✅ | `VkWindowPresentTest`（像素断言）+ `VkProbeTest`（设备自建窗口的呈现/销毁） |
| 点光（cube）阴影 | — | — | backlog `S6`（两个后端都没有，触发再做） |

## 5. 接缝：两个后端共享的东西

| 组 | 单元（`src/viz/graphics/sdk/vine/graphics/`） |
| --- | --- |
| 帧闸与时钟 | `FrameGate` / `DeviceClock` / `FrameTimeline` / `RetirementQueue` |
| 读回与目标 | `Readback` / `TargetPlan` |
| 键 | `DrawState` |
| 拒绝形状 | `DiagnosticDraft`（两个后端的拒绝都产它） |

* 接缝的准入规则是**它兑现的公共承诺**，不是读者的个数
  （[ADR-2026-11-23](../../.ai/decisions/ADR-2026-11-23-retiring-the-reference-backend.md)）：一个机制进 SDK，是因为某条宿主
  能读到的话只有“它只有一份实现”才成立；当它唯一的理由是“两个后端碰巧都在用”时，它留在插件里。现阶段的证据仍是
  “两个后端都用”（`SEAM_HEADERS` 按此测量），删除 vsg 后门禁改测“在役后端能到达”；每个机制对应的承诺见
  [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.9 的表。
* **一个后端“实现了接缝”的判据**：`tests/BackendSeamTest.cpp` 对它是全绿的——这份用例编进每个链接接缝的二进制，
  第二个开发者据此自测，不必猜。只有某一个后端读的类型住在该插件的
  `include/vine/backend_vsg/internal/`（`vn::backend_vsg::core`）与 vk 的 `internal/` 里。
* **接缝没有自己的命名空间**：它是一条性质（"两个后端都用到"）加一份被门禁钉住的清单（`scripts/check_include_hygiene.py`
  的 `SEAM_HEADERS`），而不是 `vn::graphics::backend` 这类别名组——别名会让一个类有两个名字，而两边都直接 include 的 SDK 头有
  25 个、机制只有 8 个，别名反而会把清单说小（[ADR-2026-11-18](../../.ai/decisions/ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md)）。
  本节与 [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.9 也被同一道门禁检查：`SEAM_HEADERS`
  里的每个机制，两页都必须点名。
* 门禁钉住的是**边界的方向**，不是接缝的清单：图形模块不得 include 任何具体后端（`check_vk_exposure.py` V2）、
  插件 `shell/` 零 `Vk*`（V4）、帧路径不 include `support/`（E1）；vsg 的内容记账半边（`ContentFacts` /
  `CompiledPrograms` / `PushRange` / `ContentSources` / `ContentStore`）不出现 `::vsg::`，由
  `scripts/check_vsg_content_seam.cmake` 在 ctest `content_seam` 里断言。
* **接缝的清单由 `scripts/check_include_hygiene.py` 按性质钉住**：`SEAM_HEADERS` 的八个机制头必须两边都
  能到达（`FrameTimeline` 经 vk 的 `DeviceClock` 到达），而"两个后端都直接 include 的 SDK 头"这一集合
  整体记在 `SEAM_SHARED_HEADERS` 里——一个头第一次被两边同时 include、或不再被两边 include，门禁都会变红，
  要求在同一提交里说明它是机制还是模型面。它不是旧 ADR 删掉的"目录文件集"：集合来自依赖事实，清单只
  记录那个事实；"是不是机制"仍由人判断（见 [ADR-2026-11-16-no-backend-half](../../.ai/decisions/ADR-2026-11-16-no-backend-half.md)）。
* **加一个后端要动哪几处**是一张表，不是形容词：见
  [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.11。
* 同角色类型在两个后端的名字对照见同一页 §2.12（`Content*` 命名法）。

## 6. 文件入口（改后端的人从这读）

| 层 | `gfx_backend_vk` | `gfx_backend_vsg` |
| --- | --- | --- |
| 外壳 | `include/vine/backend_vk/shell/`：`Plugin` / `BackendFactory` | `include/vine/backend_vsg/shell/`：`GfxBackendVsgPlugin` / `VsgRenderBackendFactory` |
| 机器 | `src/internal/`：`Device`（= `RenderBackend`）、`WindowTarget`、`TargetTable` + `TargetObjects`、`ContentCaches` / `ContentDraw` / `ContentImages`、`ReadbackStaging`、`WindowSystem` | `include/vine/backend_vsg/internal/` + `src/internal/`（单元表见下） |
| 支撑 | `src/support/`（窗口系统、入口点） | `src/support/`（vsg 适配：`VsgHostWindow` / `VsgSceneRules` / `VsgDynamicState`） |
| 单元全表 | [`vk-backend.md`](../../.ai/design/vk-backend.md) §4.3 | 下表（两个方向都被 `check_doc_symbols.py` 检查：文档点名不存在的单元、目录里有未点名的单元都会红） |

**vsg 的单元表**（`include/vine/backend_vsg/` + `src/`）：

| 组 | 单元 |
| --- | --- |
| 外壳 | `GfxBackendVsgPlugin` `VsgRenderBackendFactory` |
| 门面与会话 | `VsgBackend` `VsgDriver` `WindowSession` `OffscreenSession` `GraphLifetime` `SessionDrive` `SessionContent` `SessionMove` `TransferStaging` `WindowTarget` `Device` `DeviceFeatures` `DeviceRequirements` `DeviceProbe` `VsgExecutor` `FrameHead` |
| 内容层 | `ContentAssembly` `ContentPass` `CompiledPrograms` `BindingSets` `ContentDraw` `ContentFacts` `ContentSources` `ContentStore` `ContentImages` `PushRange` `ContentPipeline` `DrawNeeds` `MaterialImages` `MaterialSlotCheck` `GeometryFacts` `ShadowBlock` `ViewBlock` `ProgramAbi` `ProgramVariant` |
| 流、块、表 | `StreamUploads` `Streams` `BlockStorage` `BlockDescriptors` `VariantPool` `PassRegistry` `StateCommands` `FactResult` |
| 可移植半边（ADR-2026-11-16 §2 从 SDK 搬入，`vn::backend_vsg::core`） | `FrameRecorder` `FrameCompiler` `FrameArena` `FrameRing` `FrameScratch` `ClearPlan` `LoadOpVariantKey` `StateRegistry` `MaterialArena` `SlotProbe` `Observe` `PhaseTable` `PixelProbe` `DepthProbe` `AllocationGate` `PinnedResources` |
| 目标与读回 | `TargetObjects` `TargetTable` `HostReadback` `WhiteImage` `OneShot` |
| 支撑 | `VsgHostWindow` `VsgSceneRules` `VsgDynamicState` `VsgVulkanEntryPoints` `VsgBackendUtility` `VsgUtils` `VsgFwd` `VsgBufferView` `RenderStateMapper` |
| 全局与测试视图 | `backend_vsg_global` `BackendContent` |

**分层纪律**：`shell → internal → support` 单向；插件头不安装；`Vk*` 与 `::vsg::` 只出现在 `internal/`
（Vulkan 类型只在 vk 的 `internal/`）。VSG 后端的 `internal/` 里还有一条更窄的缝：内容记账半边
（`ContentFacts` / `ContentStore` / `CompiledPrograms` / `ContentSources` / `PushRange`）不许出现 `::vsg::`，
由 `content_seam` 用例在 ctest 里执行。

## 7. 构建、跑法与证据

```bash
cmake --build build --target gfx_backend_vk gfx_backend_vsg test_vk test_vsg
./build/bin/test_vk        # 直连 Vulkan：Windows 上整跑（174 例，无 SKIP）；Linux 上要 X11 + xcb 开发包 + 一个 Vulkan 设备
./build/bin/test_vsg       # VSG 套件（含 BackendParityTest）
./build/bin/test_vsg --gtest_filter='BackendParityTest.*'   # 两个后端逐字节对拍
ctest --test-dir build -R 'content_seam|vk_exposure'        # 两条边界门禁
bash scripts/vsg_rewrite_gate.sh build                      # vsg 的完整门禁（Debug；Release 同理）
cd build/bin && ./Vine --open demo                          # 自检：session up + Pending -> Presenting + 0 [error]
```

* 插件是 MODULE 库，测试**直接编实现源文件**（`tests/test_vk/CMakeLists.txt` 与 `tests/test_vsg/CMakeLists.txt`
  各自手工列源文件：新增 `internal/*.cpp` 要同步两处）。
* 改 SDK 虚表后**插件 .so 必须一起重建**，否则 `VsgBackendPluginTest` 会段错误。
* 对拍 fixture 里 `vk` 后端是**静态编译进 `test_vsg`** 的：改 vk 会重建 `test_vsg`，也可能影响对拍。
* `test_vsg` 的窗口用例环境敏感（WSLg）：并行 ctest 偶发 `initialize()` false，单跑复验。

## 8. 读数注意（两个后端各自特有）

* **`vk`**：validation 层可用即开，`[error]` 计数是演示自检的判据；设备丢失后帧答 `Skipped`、读回答 `Failed`，
  重建走 `shutdown()` + `initialize()`；读回在帧体内（sink/hook）调用答 `Failed`，不会等自己。
* **`vsg`**：一份文档视图 = 一块自己的 `vsg::Device`，而 vsg 的 `VSG_MAX_DEVICES` = 4（三档实现里只有 `<=4` 的固定数组
  是正确的；第 5 块面今天没有守卫，已记档）。读窗口像素要读后端日志里的渲染窗口 id（不是 Qt 容器），
  并在窗口 viewable 之后多跑一帧；窗口/像素类测试用 `QT_QPA_PLATFORM=offscreen`（xcb 会让测试窗口被 Qt 窗口遮挡）。
  应用的窗口尺寸不确定，门禁在采样前先把它拖到固定尺寸。

## 9. 文档分工（同一件事只写一处）

| 主题 | 唯一权威 |
| --- | --- |
| 引擎侧使用（场景、帧值、shader、颜色空间） | [`graphics.md`](graphics.md) |
| 视图 / `RenderControl` / 分屏与多窗口 | [`render-views.md`](render-views.md) |
| 两个后端的共同契约与差异 | 本文 |
| vk 的设计、契约、坑、实施记录 | [`.ai/design/vk-backend.md`](../../.ai/design/vk-backend.md) |
| vsg 的设计、契约、坑、实施记录 | [`.ai/design/vsg-backend.md`](../../.ai/design/vsg-backend.md)、[`.ai/design/vsg-pitfalls.md`](../../.ai/design/vsg-pitfalls.md) |
| 帧管线 / 线程 / 内容生命周期 / 会话 | `graphics-frame-pipeline.md` / `graphics-threading.md` / `graphics-content-lifecycle.md` / `graphics-sessions.md`（均在此目录的 `../../.ai/design/` 下） |
