# 图形后端：契约与当前实现

> **状态**：现行 ｜ **最后核对**：2026-11-23
> **范围**：宿主与引擎看到的**后端契约**、**当前实现（`vk`）的宿主可见事实**、**支持矩阵**、接缝与文件入口、构建与证据。
> **本文只写用户/维护者需要的事实**；`vk` 的契约、设计决定与坑在
> [`.ai/design/vk-backend.md`](../../.ai/design/vk-backend.md)（不在这里重复）。
> 参考后端（`gfx_backend_vsg`）已于 2026-11-23 退役（[ADR-2026-11-23](../../.ai/decisions/ADR-2026-11-23-retiring-the-reference-backend.md)）； <!-- drift-ok -->
> 它的历史记录留在 [`.ai/design/vsg-backend.md`](../../.ai/design/vsg-backend.md) / [`.ai/design/vsg-pitfalls.md`](../../.ai/design/vsg-pitfalls.md)，
> 最后一个能构建的树是 tag `retained/vsg-final`。
> 引擎侧怎么用图形模块见 [`graphics.md`](graphics.md)；`RenderControl` 与视图见
> [`render-views.md`](render-views.md)。

## 1. 当前后端

| 后端 | 注册名 | 插件 | 定位 | 平台 | 设备下限 |
| --- | --- | --- | --- | --- | --- |
| 直连 Vulkan | `vk` | `gfx_backend_vk` | **生产后端、默认目标**（[ADR-2026-11-14](../../.ai/decisions/ADR-2026-11-14-vk-becomes-the-default-target.md)） | Linux/X11、Win32 | Vulkan **1.3**，且启用了 `synchronization2` / `dynamicRendering` 特性；另需 `VK_EXT_extended_dynamic_state3`（多边形模式、颜色混合）。缺任一项即拒绝初始化（`vk.init.api_version_low` / `vk.init.device_features_missing` / `vk.init.dynamic_state_missing`） |

* 它实现 `vn::graphics::RenderBackend`，宿主经 `RenderBackendRegistry::create(name, desc)` 按名创建，
  编译期不依赖后端。
* **哪个后端跑由应用点名，不由名字序**：宿主启动时点名（`RenderControl::setDefaultBackendName`，Vine 写的是
  `vk`），`VN_RENDER_BACKEND=<name> ./Vine --open demo` 覆盖。应用没点名时只接受**唯一注册**的那一个；
  多个已注册又没点名，attach 直接失败并在理由里列出全部注册名——**绝不按名字序取第一个**（那是名字的偶然，不是决定）。
  这是宿主加载了别的后端插件时的回退行为，不是选择逻辑。
* 注册表**后注册者胜且不报告**：加载一个同名插件会把内建条目换成插件自己的工厂。这条规则最终要变成宿主声明的**配置项**，
  但策略本身还没定、也没实现（`T64` 目前只钉住不变量：给定配置下“谁占这个名字”由模块身份决定，**不由加载顺序决定**）。
  现状契约写在 `RenderBackendRegistry::registerFactory` 的头注释里。
* **模块要报出自己编译时的 SDK ABI**：每个后端模块用 `VN_DECLARE_RENDER_BACKEND_ABI()` 定义一次
  `vineRenderBackendAbi()`，注册时把 `{abi_version, sdk_version}` 一起交给 `registerFactory`。注册表在**调用工厂的任何
  虚函数之前**比对 `VN_RENDER_BACKEND_ABI_VERSION`，不一致就不收这个模块，并在注册表自己的 `diagnostics()` 上报
  `engine.backend.abi_mismatch` / `engine.backend.abi_missing`（加载期还没有引擎，所以这条路由归注册表）。旧模块得到的是
  一句“重建”，不是崩溃；规则、何时该升版本、为什么报告不能做成虚函数，见
  [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.16。

## 2. 共同契约（宿主不必知道是哪个后端）

以下行为由 SDK 头定义，`vk` 实现它；本文只列入口，契约以头文件为准。

| 入口 | 承诺 |
| --- | --- |
| `RenderBackend::initialize()` / `shutdown()` | `false` 之后没有会话；失败自己清理半成品；`shutdown()` 后再 `initialize()` 是合法重来 |
| `RenderFrameExecutor::executeFrame(value, present_to)` | 一帧的整个身体：开帧 → 按帧值走 pass → 编译 → 记录/提交 → 帧尾 present（`present_to` 为空走离屏会话、不 present） |
| `RenderFrameExecutor::prepareFrame(...)` | 预热：构建、不取图、不录制、不提交、不 present；之后的同值帧构建为 0 |
| `RenderDevice::capabilities()` | `render_targets`、`presentation`（创建意图）、`Features` / `Limits`；缺席不是零 |
| `RenderDevice::createFrameDriver()` / `releaseFrameDriver()` | 一个设备服务多个窗口/引擎；每个驱动者有自己的帧机器与暂存 |
| `createSurfaceTarget` / `setRenderTarget` / `releaseRenderTarget` / `releasePass` | 目标由设备持有；窗口目标一格一次（第二次答 `WindowOccupied`）；释放后下一帧重建 |
| `releaseContent(Object)` / `releaseContent(Buffer)` | 放掉 GPU 驻留，对象仍有效；释放本身不增加 `deviceWaits()` |
| `readColorImage` / `readDepthImage` | 先分类再工作：`Unsupported`（永远不行）与 `NotReady`（还没画）是不同答案；读回按每次一次计数等待设备 |
| `frameReport()` | 本帧成本（passes/draws、uploads、locking、retention、frames）+ 报告一次性的诊断 episode |
| `PresentTarget::present()` | **机制**：帧尾由执行器调用，宿主不直接调 |

三条横切规则（细节在 `.ai/design/`）：

* **一帧三相位**：帧首解析 → 录制 → 提交；录制路径不取设备锁，`FrameReport::locking` 是帧首/提交的 acquisition 数，
  稳态为常数、`waits` 为 0（[`graphics-threading.md`](../../.ai/design/graphics-threading.md) C3）。
* **拒绝一种形状**：引擎/设备/宿主三种来源的 `RenderDiagnostic` 都带 `code` / `subject` / `limit`；
  `diagnosticCount()` 只数引擎 + 设备，宿主经 `reportEngineProblem` 注入的另算（`DiagnosticOrigin::Host`）。
* **驻留由内容声明钉住**：几何行声明 stream 的 buffer、材质行与 pass 导入声明贴图；裁剪不丢上传，
  离树过一个 `slots + 1` 的窗口后才离场（[`graphics-content-lifecycle.md`](../../.ai/design/graphics-content-lifecycle.md)）。

## 3. 宿主可见的事实（`vk`）

这张表是**契约为 `vk` 落下的、宿主能观察到的具体答案**；每一行都满足 §2 的承诺，细节与拒绝代码在
[`vk-backend.md`](../../.ai/design/vk-backend.md) 对应的功能点。

| 事实 | `vk` 的答案 | 细节 |
| --- | --- | --- |
| 设备内存池 | 自建 suballocator，按设备 `bufferImageGranularity` 切片，处理 `maxMemoryAllocationCount` | `vk-backend.md` F10 |
| 管线缓存文件 | `DeviceDesc::cache_directory` → `PipelineCacheFile`，按设备键读写；拒绝即丢弃并报告 | F12 |
| GPU 计时 | 设备能写时间戳时 `Features::GpuTimings` 为真；每 pass 时间来自时间戳，`FrameReport::timing` 是本后端**测到的**数 | F13 |
| 内存数字 | `Features::MemoryUsage` 为真时，`FrameReport::memory`（`DeviceMemory`）报池的 used / reserved / peak / free / largest free run（字节）；**是本后端池的账**——不是整卡 VRAM，也不是本帧上传 | F10 |
| 验证层 | validation + debug utils **可用即开**（只影响诊断量，不影响契约） | F14 |
| 平台 | Linux/X11、Win32 | F3 |
| 渲染通道 | **动态渲染**（Vulkan 1.3）：附件、载入/清除策略与 MRT/MSAA resolve 都是每个附件的字段，没有 `VkRenderPass` / `VkFramebuffer` 对象 | F4 |
| 深度读回 | 纯深度格式（D16/D32/D32F；MSAA 读 resolve 图）；D24 与借用深度答 `Unsupported` | F11 |
| 顶点输入声明 | 只声明**顶点段真正读的** location（从编译出的 SPIR-V 读回；矩阵段按列算，实例流 12–15 全保留）——pipeline 说的是真话，验证层不会报 "attribute at location N not consumed" | F5 |
| 目标拒绝的精度 | 构建时给出 `vk.target.size_unknown` / `vk.target.borrow_extent_mismatch` | F4 |
| 窗口 | `createSurfaceTarget` 一格一次（第二次答 `WindowOccupied` 并**报一次**具名诊断），`nullptr` 请求设备自己的窗口 | F3 |

## 4. 支持矩阵（唯一一份）

| 能力 | `vk` | 证据（`test_vk`，真设备；无设备的用例在 `test_graphics`） |
| --- | --- | --- |
| 内容绘制（索引/非索引、点/线/三角） | ✅ | `VkContentDrawTest`、`VkVertexInputTest` |
| MRT、深度附件、逐附件清屏 | ✅ | `VkContentTargetTest` |
| MSAA（目标属性）与深度 resolve | ✅ | `VkMsaaTest` |
| 借用深度（`shareDepth`） | ✅ | `VkContentTargetTest` |
| 材质（贴图/立方体/白图 fallback）、灯（方向光 + 环境） | ✅ | `VkContentDrawTest` |
| 阴影（方向光） | ✅ | `VkShadowTest` |
| 延迟链、全屏调用、push 范围 | ✅ | `VkDeferredTest`、`VkUniformRingTest` |
| 自定义程序（声明式 ABI + 变体 + 实例流） | ✅ | `VkBuiltinProgramTest`、`VkVariantTest`、`VkShaderCompilerTest` |
| 读回（颜色 8 位族逐通道，1/2/4 字节；纯深度浮点） | ✅ | `VkContentTargetTest`（各格式 + 深度）+ 各像素用例的 `readColorImage` |
| 窗口（采纳宿主表面、跟随 resize、丢帧自愈） | ✅ | `VkWindowPresentTest`（Win32 上读的是合成器） |
| 多窗口 / 一个设备多个引擎 | ✅ | `VkMultiSessionTest`、`VkConcurrentFrameTest` |
| 驻留账与释放 | ✅ | `VkResidencyTest`、`VkResidenceWireTest` |
| 线框 / 点视图、线宽、各向异性 | 按设备 | `Feature::Wireframe` / `AnisotropicFiltering` + `Limits`；`VkContentTargetTest` 钉住线框是一条命令 |
| 设备内存池 / 管线缓存文件 | ✅ | `VkMemoryPoolTest`、`VkPipelineCacheFileTest`、`VkPipelineCacheSessionTest` |
| Win32 窗口 | ✅ | `VkWindowPresentTest`（像素断言）+ `VkProbeTest`（设备自建窗口的呈现/销毁） |
| 点光（cube）阴影 | — | backlog `S6`（本后端还没有，触发再做） |

## 5. 接缝：插件与 SDK 共享的东西

| 组 | 单元（`src/viz/graphics/sdk/vine/graphics/`） |
| --- | --- |
| 帧闸与时钟 | `FrameGate` / `DeviceClock` / `FrameTimeline` / `RetirementQueue` |
| 读回与目标 | `Readback` / `TargetPlan` |
| 键 | `DrawState` |
| 拒绝形状 | `DiagnosticDraft`（每个后端的拒绝都产它） |

* 接缝的准入规则是**它兑现的公共承诺**，不是读者的个数
  （[ADR-2026-11-23](../../.ai/decisions/ADR-2026-11-23-retiring-the-reference-backend.md)）：一个机制进 SDK，是因为某条宿主
  能读到的话只有“它只有一份实现”才成立；当它唯一的理由是“某个后端碰巧在用”时，它留在插件里。参考后端退役后，门禁
  改测“**在役后端能到达**”（`SEAM_HEADERS`）；每个机制对应的承诺见
  [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.9 的表。
* **一个后端“实现了接缝”的判据**：`tests/BackendSeamTest.cpp` 对它是全绿的——这份用例编进每个链接接缝的二进制，
  第二个开发者据此自测，不必猜。只有某一个后端读的类型住在该插件的 `internal/` 里，不进 SDK。
* **接缝没有自己的命名空间**：它是一条性质（"在役后端用到它"）加一份被门禁钉住的清单（`scripts/check_include_hygiene.py`
  的 `SEAM_HEADERS`），而不是 `vn::graphics::backend` 这类别名组——别名会让一个类有两个名字，而在役后端直接 include 的 SDK 头有
  37 个、机制只有 8 个，别名反而会把清单说小（[ADR-2026-11-18](../../.ai/decisions/ADR-2026-11-18-seam-identity-is-a-property-not-a-namespace.md)）。
  本节与 [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.9 也被同一道门禁检查：`SEAM_HEADERS`
  里的每个机制，两页都必须点名。
* 门禁钉住的是**边界的方向**，不是接缝的清单：图形模块不得 include 任何渲染 API 或具体后端（`check_vk_exposure.py` V2）、
  公开头的散文不得点名后端（V5，只允许 `vk.<category>.<cause>` 这样的诊断代码前缀）、插件 `shell/` 零 `Vk*`（V4）、
  帧路径不 include `support/`（E1）、公开头也不得点名插件 `internal/` 的单元（V6，T30 的落位问题）。
* **接缝的清单由 `scripts/check_include_hygiene.py` 按性质钉住**：`SEAM_HEADERS` 的八个机制头必须从在役后端
  能到达（`FrameTimeline` 经 `DeviceClock` 到达），而在役后端直接 include 的 SDK 头这一集合
  整体记在 `SEAM_SHARED_HEADERS` 里——一个头第一次被后端 include、或不再被 include，门禁都会变红，
  要求在同一提交里说明它是机制还是模型面。它不是旧 ADR 删掉的"目录文件集"：集合来自依赖事实，清单只
  记录那个事实；"是不是机制"仍由人判断（见 [ADR-2026-11-16-no-backend-half](../../.ai/decisions/ADR-2026-11-16-no-backend-half.md)）。
* **加一个后端要动哪几处**是一张表，不是形容词：见
  [`graphics-architecture.md`](../../.ai/design/graphics-architecture.md) §2.11。

## 6. 文件入口（改后端的人从这读）

| 层 | `gfx_backend_vk` |
| --- | --- |
| 外壳 | `include/vine/backend_vk/shell/`：`Plugin` / `BackendFactory` |
| 机器 | `src/internal/`：`Device`（= `RenderBackend`）、`WindowTarget`、`TargetTable` + `TargetObjects`、`ContentCaches` / `ContentDraw` / `ContentImages`、`ReadbackStaging`、`WindowSystem` |
| 支撑 | `src/support/`（窗口系统、入口点） |
| 单元全表 | [`vk-backend.md`](../../.ai/design/vk-backend.md) §4.3（`check_doc_symbols.py` 两个方向都检查：文档点名不存在的单元、目录里有未点名的单元都会红） |

**分层纪律**：`shell → internal → support` 单向；插件头不安装；`Vk*` 只出现在 `internal/`（与 `support/` 的显式出口）。

## 7. 构建、跑法与证据

```bash
cmake --build build --target gfx_backend_vk test_vk
./build/bin/test_vk        # 真设备：Windows 上整跑（186 例，无 SKIP）；Linux 上要 X11 + xcb 开发包 + 一个 Vulkan 设备
ctest --test-dir build -R vk_exposure                       # 暴露面门禁（V2 / V4 / V5 / E1 / E2）
cd build/bin && ./Vine --open demo                          # 自检：session up + Pending -> Presenting + 0 [error]
```

* 插件是 MODULE 库，测试**直接编实现源文件**（`tests/test_vk/CMakeLists.txt` 手工列源文件：新增
  `internal/*.cpp` 要同步它）。
* 改 SDK 虚表后**插件必须一起重建**，否则加载旧插件会段错误（2026-11-23 实测；ABI 规则见
  [`ADR-2026-11-23`](../../.ai/decisions/ADR-2026-11-23-retiring-the-reference-backend.md) D5 与 backlog `T65`）。
* WSLg 下 `XGetImage` 读不到窗口像素（纯 XCB 填充也读到黑），像素断言会 `SKIP` 并说出原因——那是仪器边界，
  不是后端缺陷。

## 8. 读数注意

* validation 层可用即开，`[error]` 计数是演示自检的判据；设备丢失后帧答 `Skipped`、读回答 `Failed`，
  重建走 `shutdown()` + `initialize()`；读回在帧体内（sink/hook）调用答 `Failed`，不会等自己。
* 一个窗口一份设备（`SurfaceWindow::useDefaultBackend()` 每次 attach 造一个后端）⇒ 两个窗口 = 两份显存、两遍编译；
  共享设备仍是以后的事（`Device : Engine = 1 : N`）。**没有设备数量上限**：旧实现里那条 4 设备的天花板随
  参考后端一起退役。

## 9. 文档分工（同一件事只写一处）

| 主题 | 唯一权威 |
| --- | --- |
| 引擎侧使用（场景、帧值、shader、颜色空间） | [`graphics.md`](graphics.md) |
| 视图 / `RenderControl` / 分屏与多窗口 | [`render-views.md`](render-views.md) |
| 两个后端的共同契约与差异 | 本文 |
| vk 的设计、契约、坑、实施记录 | [`.ai/design/vk-backend.md`](../../.ai/design/vk-backend.md) |
| vsg 的历史记录（已退休，不维护） | [`.ai/design/vsg-backend.md`](../../.ai/design/vsg-backend.md)、[`.ai/design/vsg-pitfalls.md`](../../.ai/design/vsg-pitfalls.md) |
| 帧管线 / 线程 / 内容生命周期 / 会话 | `graphics-frame-pipeline.md` / `graphics-threading.md` / `graphics-content-lifecycle.md` / `graphics-sessions.md`（均在此目录的 `../../.ai/design/` 下） |
