# T30 评估：GPU 资源抽象（统一类型 / 多态总表）

> **状态**：调研评估（**非现行设计**；结论是维持 T30 的触发条件，并写清触发时的形状）
> **日期**：2026-11-13
> **范围**：`.ai/backlog/graphics.md` T30，及其边界 T15 / T32 / S4 / S5
> **方法**：只读现行代码与设计（不新建抽象）；每条判断给出可核对的事实
> **一句话结论**：今天统一的是**规则与账**（pin / 退役 / 驻留），这层已经完备；"统一类型 / 多态总表"
> 在第二个后端出现前没有消费者，且真出现时正确的形状是**不透明资源描述 + 每后端实现**，不是基类。
> **追加（2026-11-13）**：首个第二后端确定为**手撸 Vulkan**（学习/研究），与 vsg 同 API 家族——
> 上面的结论不变（它消费的是接缝，不是资源类型），见 §8。

## 1. 问题

### 1.1 T30 原文

> 现在统一的是**规则与账**（`ResidencyLedger` + pin），不是类型。基类或多态总表只在第二个后端出现时才值得：
> 届时形状是"SDK 资源描述 + 每后端实现"，不是包 vsg 的基类。

### 1.2 本文回答的三个问题

1. 今天的设施到底**统一了什么**、第二个后端会真正缺什么；
2. "统一类型 / 多态总表"是不是第二个后端的正确形状（哲学与 `G#` 判据）；
3. 如果不做，T30 的触发条件与第二个后端的**验收**应该怎么写。

## 2. 现状盘点（代码事实）

### 2.1 宿主面没有"创建 GPU 资源"的动词

`RenderDevice` 的 18 个虚动词里没有一个创建 buffer / texture / pipeline：

- 生命周期：`initialize` / `shutdown` / `initialized`
- 呈现：`createSurfaceTarget` / `setRenderTarget` / `executor`
- 释放：`releasePass` / `releaseRenderTarget` / `releaseContent(Object)` /
  `releaseContent(Buffer<float>)` / `releaseContent(Buffer<uint32_t>)`
- 读回：`readColorBuffer` / `readDepthBuffer`
- 描述与诊断：`capabilities` / `setDefaultContentProgram` / `setDiagnosticsRoute` / `removeDiagnosticsRoute` /
  `setDiagnosticSink`

资源是**后端从内容值物化**出来的：`GeometryDescription`（通道、切片、primitive）、`MaterialDescription`
（值 + 纹理句柄）、`ShaderProgramDescription`（文本）、`Texture`、`Buffer<T>`（+ revision）与 `RenderTarget`。
宿主命名的是**内容对象**与**目标/声明**，从不是 GPU 句柄。

**推断**：一个 `GpuResource` 基类今天没有宿主消费者，只有后端消费者——而后端是唯一知道资源形状的一方。

### 2.2 接缝的词汇：一半已中性，一半是 Vulkan 策略

- `sdk/vine/graphics/backend/`：24 个头、3,897 行；`BackendHalfGuard.hpp` 把"宿主包含它"变成**编译错误**，
  并单独装成 `VineGraphicsBackendSdk` 组件（`graphics-architecture.md` 2.9）。
- 已经是中性拼写：`LoadOp` / `StoreOp` / `ImageLayout`（枚举，不是 Vulkan 类型）、`StreamKey`
  （buffer + revision + 段）、`VertexLayoutKey`、`ResidencyKind` / `ResidencyRow` / `ResidencyLedger`、
  `ResourceTable`、`RetirementQueue`、`FrameTimeline`、`FrameArena`、`MaterialArena`、`VariantPool`。
- 仍是 Vulkan 策略：`RenderPassCompatibility::device_color_formats` / `device_depth_format`（以 `std::uint32_t`
  携带设备格式码，含义是 `VkFormat`）、`TargetShape::subpass`、`TargetPlan`、`ClearPlan`、`FrameCompiler`。
  `vsg-backend.md` C6 已冻结新增这类拼写。

### 2.3 驻留：一条规则，四个机制

`ADR-2026-11-10-gpu-residency-pinned-by-content.md` 把五种寿命判据收成一条：

- **pin =（内容身份, revision）**；回收只问"现在谁活着"，不问"它死了吗"；
- `ResourceTable<K,V>` 承担"按 key 构建、被 pin 留住、无人 pin 就停车"；
- `RetirementQueue` + `FrameTimeline` 承担"在飞帧还能读"；
- `ResidencyLedger`（Vulkan-free，宿主半段）承担**每类**的账；`MaterialArena` / `VariantPool` 是"策略 + 账"，
  不拥有 GPU 对象。

流、纹理、材质块、半片、描述符集五类驻留全部走同一张 `ResourceTable::retireUnpinned`（ADR 迁移 2–5 步）。

### 2.4 体量：成本在物化，不在"资源类型"

`src/plugins/gfx_backend_vsg` 共 **28,354 行 / 85 个 internal 文件**（45 个头 + 40 个源，另有 `support/`）。
其中 `ContentStore` / `BlockStorage` / `MaterialImages` / `ContentSets` / `ContentHalves` / `ContentPipeline` /
`StreamUploads` / `Session` / `VsgExecutor` / `WindowTarget` / `HostTargets` 都是**物化层**：把值变成 vsg/Vulkan
对象。第二个后端的成本主要在这里。

### 2.5 装载路径已经存在

`RenderBackendRegistry` / `RenderBackendFactory` / `RenderBackendInfo` / `RenderApi` 与 `vn_add_plugin` 已经让
"按名字创建后端、不编译期依赖后端模块"成立；`gfx_backend_vsg` 是一个在 `load()` 里注册 "vsg" 的插件。
**第二个后端缺的不是装载，而是接缝里的设备无关性与物化实现。**

## 3. 哲学：为什么"统一类型 / 多态总表"不是答案

| 判据 | 结论 |
|---|---|
| **G8 保持克制** | 没有"宿主直接命名 GPU 资源"的消费者，第二个后端也未出现；今天建基类就是为假想需求预建体系 |
| **G2 职责分明** | 基类的释放/创建要带设备概念（`RenderDevice&`），形成"内容 → 设备"的边；ADR 的选项 D 已按此否决 |
| **G7 不转移复杂性** | vsg 自己创建对象；基类只会变成一层转发，把"谁真正拥有对象"藏起来 |
| **G1 / G6 概念与统一** | 已统一的是**规则**（pin / 退役 / 账）。把"相同规则"误读成"相同对象"，会得到两个都写不干净的抽象 |
| **G10 真实成本** | 第二个后端的成本是物化层（同量级万行），不是基类；基类不减少其中任何一行 |
| **G12 意图在代码里** | `releaseContent(Object)` 已经表达"寿命跟着内容走"；`ResidencyKind` 已经表达"账按类记" |
| **G13 的同类问题** | 让 `Geometry` / `Material` 继承 `GpuResource`，等于把 GPU 寿命概念压给内容模型——内容不拥有 GPU 对象 |

**统一发生在"描述"，不发生在"GPU 对象"**：`GeometryDescription` / `MaterialDescription` /
`ShaderProgramDescription` / `Texture` / `Buffer` / `RenderTarget` 就是跨后端的资源描述值；第二个后端从它们
物化自己的对象。这正是 T30 原文的"SDK 资源描述 + 每后端实现"。

## 4. 完备性：如果明天加第二个后端

### 4.1 已经够用的（可复用或已中立）

- **帧的一条线**：`FrameDescription`（pass / draw / 输入 / 灯 / 目标）是一份值，`RenderFrameExecutor` 只有
  `executeFrame` + `prepareFrame` 两个动词；后端不得重排、不得自建依赖图（R1/C3）。
- **设备的形状**：18 个动词、`Capabilities` 值、`TargetShape` / `TargetDecision` 的纯函数、`RenderState` 解析。
- **驻留与寿命**：`ResourceTable` / `RetirementQueue` / `FrameTimeline` / `MaterialArena` / `VariantPool` /
  `ResidencyLedger` 都不含 Vulkan 类型。
- **内容的描述值**：几何/材质/程序的 description 与 `Texture` / `Buffer`（revision 已 atomic）。
- **装载**：registry + factory + plugin（§2.5）。
- **测试形态**：`test_vsg` 直接编译插件源码、像素探针 / `BlockStorageTest` / `MeasureWhat*` 是可搬的验证形态。

### 4.2 缺口（按依赖排序）

1. **T15：设备格式码的语义**。`RenderPassCompatibility` 里的 `std::uint32_t` 是 `VkFormat` 值；第二个后端要么
   复用同一套码（不成立），要么先把它改成不透明 `DeviceFormat` + 每后端格式表。这是 T30 的**前置**，
   且必须在"新增第二个后端的拼写"之前做（C6）。
2. **T32：`RenderBackend` 聚合拆分**。今天一个对象同时是 `RenderDevice` 与 `RenderFrameExecutor`
   （`RenderBackend.hpp` 多重继承）。第二个后端若只想实现设备、复用别人的执行器（或反之），需要两个工厂出口。
3. **S4 / S5：参数 sink 与灯块布局**。`ParameterSink` 的第二后端实现、`VineLightsBlock` 上移 SDK，
   否则第二个后端要复制一份 GLSL 布局。
4. **验证与安装**：`VineGraphicsBackendSdk` 组件已存在；仍需一个"第二个后端的一致性套件"
   （同一 `FrameDescription` 的记录/像素对拍、`ResidencyLedger` 每类非空、`backend/` 不含后端 include、TSan）。
5. **物化层**：`ContentStore` 等的**规则**（pin、代次、声明索引）可复用；**实现**要按 API 重写，不继承。

### 4.3 判定

- **规则层：完备。** 帧值、能力、驻留、退役、上传、目标决策、状态解析、内容描述值都在。
- **类型层：不完备，但不应现在补。** "统一类型"没有宿主消费者；第二个后端会自己回答它需要什么形状。
- **装载层：完备**（registry / factory / plugin）。
- **验证层：部分**（有形态、无可执行的第二后端 conformance 套件）。
- **第二个后端的最小入口**：实现 `RenderDevice`（18 动词）与 `RenderFrameExecutor`（2 动词）+ 复用 4 个机制头
  （`ResourceTable` / `RetirementQueue` / `FrameTimeline` / `ResidencyLedger`）+ 重写物化层（同量级万行），
  而不是"实现一个 GPU 资源基类"。

## 5. 如果现在就要动：最小切口（不建议，但写清）

1. **只做 T15 的前半**：把设备格式码换成不透明 `DeviceFormat` + 每后端表，保持"未知 ≠ 缺席"的现有行为
   （`RenderPassCompatibility` 的注释：unknown 不能合并两个家族）。——与第二个后端无关的宿主也受益（`G1`），
   且不引入新概念。
2. **给 `ResidencyLedger` 加"每后端自证"的验收**：每个 kind 的 `live` / `bytes` 由后端回答，不加类型。
3. **不建 `GpuResource` 基类、不做多态总表**：ADR-2026-11-10 选项 D 的理由仍然成立。

## 6. 触发与验收

- **T30 保持触发条件型**：触发 = 第二个后端（与 T15 / S4 / S5 同一触发）。
- **第二个后端落地时的验收**（也是 T30 的判据）：
  - 同一 `FrameDescription` 在两个后端上记录/像素对拍（允许后端自有的差异项显式列出）；
  - `ResidencyLedger` 的每一类都有 `live`，`bytes` 在两端都可空可答；
  - SDK `graphics/backend/` 不 include 任何后端的头（`BackendHalfGuard` 之外再加脚本门）；
  - `scripts/tsan_check.sh` 场景 0 warning；`test_vsg` 的 device-free 子集可跑；
  - 一个 resize 不重编管线、一个 host 状态变更不重编管线（`Keys.hpp` 的 audit 表照搬）。

## 7. 什么会推翻本文

- 宿主需要**直接命名/移交 GPU 资源**（远程渲染、录制回放把资源句柄给宿主，或与外部渲染器共享纹理）；
- 第二个后端需要与 vsg **共享同一个 GPU 对象**（不同 API 下不成立，但若出现软件/硬件双后端共享内存，另当别论）；
- 宿主需要**逐对象**读驻留（今天只有逐类）。那时扩的是 `ResidencyLedger` 的粒度，仍不是基类。

## 8. 追加（2026-11-13）：首个第二后端确定为手撸 Vulkan（学习/研究）

结论先说：**同 API 家族**，所以上面"不做类型抽象"的结论不变；它消费的是**接缝**，不是资源类型。

### 8.1 为什么它不触发 T15 / T30 的类型问题

- `backend/` 的 Vulkan 策略（`device_color_formats` 的 `VkFormat` 语义、`subpass`、`ImageLayout`）对它是**可用**的，
  不需要先改成不透明格式码——那是跨 API 家族（D3D12 / Metal / WebGPU）的问题。**T15 不触发**。
- T30 的"统一类型 / 多态总表"仍无跨 API 消费者；vk 直接实现 `RenderDevice`（18 动词）+ `RenderFrameExecutor`
  （2 动词）+ `WindowPresentTarget`，寿命规则复用 `backend/` 的四个机制头。**T30 的触发条件收窄为"第二个 API 家族的后端"**。
- T32（`RenderBackend` 聚合拆分）**不阻塞**：直接实现聚合即可。

### 8.2 形态

- 插件 `gfx_backend_vk`，在 `load()` 里注册工厂 `"vk"`（`RenderApi::Vulkan`）；**不是默认后端**，
  vsg 仍是生产路径；测试独立（新建 `test_vk`，像 `tests/test_vsg` 一样直接编译插件源码）。
- **复用**（SDK `backend/`）：`ResourceTable`、`RetirementQueue`、`FrameTimeline`、`ResidencyLedger`、`FrameArena`、
  `MaterialArena`、`Keys` / `TargetPlan` / `Streams` / `ClearPlan` / `Readback` / `PixelProbe` / `Observe`；
  `ShaderAbi` 的四个块与 `vn_params.glsl` 是 L1 契约。
- **自己写**（学习内容）：instance / physical device / device / queue（volk）、简单分配器、命令池/缓冲、
  fence/semaphore、image/view/render pass/framebuffer、pipeline（`ShaderProgramDescription` → glslang → SPIR-V；
  `VertexLayoutKey` → `VkVertexInput`；`RenderState` → 固定状态）、descriptor 布局/池/集、流上传、读回、swapchain。

### 8.3 里程碑（每级都有可跑的验收）

| # | 内容 | 验收 |
|---|---|---|
| M0 | 插件 + factory + `initialize/shutdown` + `capabilities`（无窗口） | registry 测试 + 设备门控的 init/shutdown，validation 0 error · **已落地 2026-11-13**：`test_vk` 3 例（`BackendTest.cpp`），本机 `validation=on` 且 `InitFailed` 诊断计数 0；`plugins/vine/gfx_backend_vkd.so` 可加载 |
| M1 | 离屏 target（RGBA8/D24）+ 无 draw 的 pass + `ClearPolicy` + `readColorBuffer` | 像素 = pass 的 clear 色 · **已落地 2026-11-13**：`BarrierPlan`（`ImageLayout` + 载/存操作 → Vk 布局/阶段/access，5 例无设备表驱动测试）+ `TargetImages` + 同步提交 + `readColorBuffer`；`VkOffscreenTest` 2 例（64×64 RGBA8+D24 全像素等于 clear 色；未画的 target 答 `NotReady` 而非黑像素），validation 静默 |
| M2 | 一个 content draw：内置程序 → pipeline；`GeometryDescription` 通道 → 上传；`VineViewBlock` / `VineDrawBlock`；`vkCmdDrawIndexed` | 三角形像素（`SdkPixelFixture` 式读回） · **已落地 2026-11-13**：`ContentDraw`（程序 revision → glslang → `VkShaderModule` → `VkPipeline`；通道 → 顶点/索引 buffer；每 draw 一个 `VineViewBlock`+`VineDrawBlock` UBO；`vkCmdDrawIndexed`）+ `ShaderCompiler`（直接调 glslang，2 例无设备测试）；内容 ABI 定为本后端的 set 0 / binding 0（view）、1（draw）；`test_vk` 14 例（三角形中心 = 片元色、角落 = clear、默认程序路径，validation 静默）。glslang 来源已决定：provisioning 上移 `cmake/VineGlslang.cmake` + 顶层调用（IMPORTED 目标按目录可见），vsg 与 vk 共用一处 |
| M3 | `TargetDecision`（ResizeInPlace/Rebuild）、viewport/scissor、depth range、MRT、per-attachment clear、`PassDraw::state` | resize 不重编管线；MRT 各附件值正确 · **已落地 2026-11-13**：`planTarget` 驱动 Rebuild / ResizeInPlace / Repair（resize 只换 image/view/framebuffer，保留 render pass）；pipeline 键改为 `RenderPassCompatibility`（resize 不重编，`Device::contentPipelines()` 可观测）；MRT 最多 4 个颜色附件 + 每附件 clear / readback；`ClearPolicy::depth == false` 走 depth-LOAD 变体（fresh 目标首帧仍 CLEAR）；`StateMapping` 表（reverse-Z 比较、CW 前置面、blend / polygon / topology / depth band）；`PassDraw::state` 逐命令 `resolveRenderState`；`test_vk` 22 例 |
| M4 | 材质块 + 纹理 + 灯块 + screen draw + 透明排序（引擎已排好） | 材质/采样/全屏拷贝像素 · **已落地 2026-11-13**：内容 ABI 改为引擎 ABI（set 0 = material@0 / 五槽 1–5 / lights@6，set 1 = draw@0，相机 = 128B push）；`ContentImages`（纹理上传 + sampler 缓存 + 1x1 白色 stand-in，上传在 render pass 之前由 `preparePass` 记录）；`LightBlock`（view-space 打包，`kMaxLights = 8`）；`recordScreen`（引擎 fullscreen vertex + draw 自己的 fragment，source 颜色附件 = set 0 binding 0..N-1）；无 clear 的 pass 由 colour+depth LOAD 变体承载（fresh 时回退 CLEAR）；blend 走逐命令 `resolveRenderState`；target 颜色/深度图像新增 `SAMPLED` usage；`test_vk` 26 例（材质因子、纹理采样、ambient 灯、全屏拷贝像素、半透明叠加） |
| M5 | `createSurfaceTarget` / `WindowPresentTarget` / acquire / present / resize | 窗口像素与 `PresentRecord` · **已落地 2026-11-13（Handle + Probe）**：`WindowTarget`（XCB surface 由 `XcbSurface` 用 dlopen 的 `xcb_connect` 创建；swapchain + D24 + 两个 render pass（CLEAR/LOAD）+ 每 image framebuffer；acquire/present 信号量 + 提交后 `COLOR_ATTACHMENT → PRESENT_SRC` 转移；resize/out-of-date 重建）；`Device::createSurfaceTarget` 一窗口一 target（重复 = `WindowOccupied`，不再交出第二个）；`DevicePresentation::Handle` 是创建输入（pre-init 的 target 在 initialize 时 attach）；`Probe` 自建窗口：`ProbeWindow`（X11 用 dlopen 的 libX11 建窗口，Win32 用注册窗口类；macOS 不支持，答 `UnsupportedSurface`），`initialize()` 在建 instance **之前**建窗（surface 要在它上面创建），`createSurfaceTarget(nullptr)` 在建窗后取 target；`executeFrame(frame, present_to)` 把 `target == nullptr` 的 pass 画进 acquired image 并在帧尾 present；`capabilities().presentation` 从创建意图开始就如实回答；`test_vk` 37 例。**已知缺口**：WSLg 下 XGetImage 读不到窗口像素（连纯 XCB 填充都读回黑色），所以窗口像素断言在仪器不可读时跳过，由 `Presented` + validation 判定 |
| M6 | `ResidencyLedger` 每类、`releaseContent`、`prepareFrame`、`FrameReport` | 稳态 0 上传/0 编译；驻留随内容离开而下降 · **已落地 2026-11-13**：`ContentDraw::Progress` / `ContentImages::Progress`（cumulative builds/bytes/releases + live 合计）；`releaseContent(Object)` 按 Geometry / ShaderProgram / Material / Texture 派发，`releaseContent(Buffer)` 按 buffer 身份只放它供出的 stream；`prepareFrame` 与 `executeFrame` 共用 `planCommand` / `planScreen`；`FrameReport` 的 residency 只记 Streams / Images / Halves（material block / set 逐帧存在）；`test_vk` 35 例 |
| M7 | 与 vsg 对拍：`test_vsg` 的设备无关子集 + 像素子集 | 同一 `FrameDescription` 的记录/像素 diff · **已落地 2026-11-13**：`BackendParityTest`（`test_vsg`，8 例）把同一个 frame 值交给两个后端（都是 `DevicePresentation::None`）：clear / 不透明绘制 / 深度序 / 半透明混合 / 全屏拷贝 / 纹理采样六类像素**逐字节相同**，`FrameReport::counters` 与 `PresentOutcome` 相同；无效果的 pass、不命名 pass 的 entry 两例各答 0/0 并同样拒绝回读。对拍修掉三处：vk 投影只折 Y 不折深度（深度保留更远片元）；vsg `FrameCounters` 从不复位（报告逐帧累加）；vsg 无窗口设备回读不等离屏会话（读到全 0）。`test_vk` 35 例 / `test_vsg` 556 例 |

### 8.4 成本与风险

- M0–M2 约 2–4k 行（含 CMake/测试），是学习核心，短周期能看到三角形。
- M3–M6 到"能跑现有像素套件"约 10–20k 行；vsg 是 28,354 行，vk 可以按需裁剪（先不做阴影/实例）。
- 风险：validation layer 的设备门控（WSLg）、glslang 运行时编译、插件 MODULE 不可链接（测试直接编译源码）、
  `VN_GRAPHICS_BACKEND_CONSUMER` 定义。不引入新第三方：volk / glslang 已在树内。

### 8.5 与默认路径的纪律

- vk **不进**默认后端选择（宿主按名字 `create("vsg")`）；`SdkPixelFixture` 与现有宿主仍用 vsg。
- 它的价值是**接缝的证据**：第二个实现能回答"`FrameDescription` 是不是够用、`backend/` 的规则是不是可复用"，
  这本身就是 T30 想要的东西——只是以"第二个实现"而不是"统一类型"的形式出现。

## 9. 追加（2026-11-13）：vk 的通用化边界（设计论证）

**范围决定**：只做 Vulkan（`vk`），但**新写的东西、以及好改的东西先按通用形状落**；shader 不做实现层通用化，
只在概念上预留。本节是这条原则的论证与可执行清单，不是实现计划（里程碑见 §8.3）。

### 9.1 原则：通用化发生在「值 / 规则 / 映射表」，不发生在「API 对象」

上面三层契约的结论是：**值与规则已经通用，API 调用各写各的**。所以"考虑拓展性"在这里不是"多加一层抽象"
（`G7`），而是三件更便宜的事：

1. **只消费既有中性输入**，不另造同义类型（`G1` / `G6`）；
2. **把"中性输入 → API 输出"的翻译收敛到一张表**，让第二个 API 家族只换表、不换输入（`G5`）；
3. **明确写下不通用化的东西**，让下一个人知道那里是选择而不是遗漏（`G12`）。

判据（用来回答"这个新东西要不要现在通用化"，三条按顺序问）：

| # | 判据 | 结论 |
|---|---|---|
| J1 | 它是**输入**（被消费的值）还是**输出**（API 对象/调用）？ | 输入可以通用化；输出留在插件 |
| J2 | 它**今天就有第二个消费者**吗（vsg 与 vk 都读）？ | 有 ⇒ 共享；没有 ⇒ 插件私有（`G8`） |
| J3 | 通用化它要不要动**公开面**（SDK 头/公开枚举）？ | 要 ⇒ 推迟到触发；不要 ⇒ 现在做 |

### 9.2 现在就通用化（写出即通用，成本≈0）

这些不是"新抽象"，是**在 vk 里直接使用已有类型**——通用化的成本就是"别另写一份"。

| 项 | 消费的既有类型 | 为什么便宜 |
|---|---|---|
| 后端身份 | `RenderBackendInfo` + `RenderApi::Vulkan` | 已有 factory/registry，零改动 |
| 能力 | `Capabilities`（features 位 + 极限值） | 已是值，`supports()` 已定义语义 |
| 目标决策 | `planTarget(TargetInstance, TargetDesc)` | 已是纯函数、已测；vk 不自己判"尺寸变了" |
| 管线身份 | `PipelineKey` / `PipelineKeyHash` / `VariantPool` | `Keys.hpp` 的 audit 表（什么可以进键）自动适用于第二后端 |
| 寿命 | `ResourceTable` / `RetirementQueue` / `FrameTimeline` | pin 规则与 API 无关 |
| 驻留账 | `ResidencyLedger`（`bytes` 可 absent，`live` 必答） | absent ≠ 0 的规则已经定好 |
| 读回 | `ReadbackResult`（机器可读原因） | 不要发明 bool + printf |
| 诊断 | `DiagnosticsRoute` / `DiagnosticSink` | validation 消息走同一条路 |
| 帧 | `FrameDescription` + `executeFrame` / `prepareFrame` | 一条线两个动词 |

**纪律**：这些类型一个都不许在 vk 里出现"影子版本"（`vn::backend_vk::PipelineKey`、`vn::backend_vk::Residency`…）。影子版本是重复，
不是拓展性（`G1`）。

### 9.3 唯一值得新做的通用物：一张映射表

`BarrierPlan`（新，落在 vk 插件的 `src/internal/`）：

- **输入**（全部中性、全部已存在）：`ImageLayout`（当作"这一 pass 要什么用途"，不是 Vulkan 布局）、
  `LoadOp` / `StoreOp`、以及一个极小的 `AccessKind{Sampled, ColorWrite, DepthWrite, TransferRead, Present}`；
- **输出**：`VkImageLayout` + src/dst stage/access（synch2 或 legacy 由实现选）+ `VkImageMemoryBarrier` 参数；
- **为什么它是通用点**：这是全后端唯一"中性输入 → API 输出"的**收敛处**。DX12 写
  `D3D12_RESOURCE_STATES` + enhanced barriers，Metal 写 stage/access 与 barrier scope——**输入不变，只换这张表**。
- **成本**：150–250 行 + 一张无设备的表驱动测试（每个 `(ImageLayout, prev, next)` 组合断言输出），
  不需要 GPU。
- **反例（不要做）**：一个"通用 barrier 类型"。它会把 Vulkan 的 stage/access 三元组变成 SDK 词汇，
  而 DX12/Metal 的表达不是一个三元组（`G8` / `G10`）。

### 9.4 只留概念、不写代码：shader

按决定，shader **不做实现层通用化**。概念预留的边界只有两件事，别多：

1. **源码语言**：`ShaderProgramDescription` 今天只装文本，没有"这是什么语言"的字段。未来形状是
   `ShaderSourceLanguage{Glsl, Hlsl, Msl}` + 每后端一个编译入口；**现在不加字段**——一个只有 `Glsl` 一个值的枚举是
   纯投机（`G8`）。
2. **绑定解码**：`ShaderAbi` 已经是 `(set, binding)` 的规范形，Vulkan 直接映射、DX12 映射到 root signature、
   Metal 映射到 argument buffer。**这部分已经通用**，不需要预留。

一个容易走偏的判断：**块布局不是缺口**。`ShaderAbi.hpp:107` 已写明"每个成员是 mat4/vec4、16 字节对齐，
所以 std140、D3D cbuffer packing 和 root constants 都成立"。真正缺的是**源码语言**，不是内存布局。

零成本推迟之所以成立：`ShaderProgramDescription` 是 SDK 的**值**，加一个字段便宜且不碰 GPU 布局；而工具链
（DXC 出 DXIL、SPIRV-Cross 出 MSL）是**新第三方依赖**，本来就该走 ADR，不能顺手决定。

vk 的 pipeline builder 因此只接受 GLSL，并在源码不是 GLSL 时报告一条明确诊断（而不是编译失败后报一串 SPIR-V 错误）。

### 9.5 明确不做（附触发）

| 不做 | 理由 | 触发 |
|---|---|---|
| 内存/分配器抽象（VMA 式接口） | DX12 heap、Metal MTLHeap、Vulkan memory type 的差别大于共性；接口会变成并集（`G7`） | 第二个 API 家族的分配器真的出现，且两份实现有相同的分配策略 |
| 命令记录抽象（第二套 recorder） | `FrameDescription` / `FrameRecorder` / `FrameCompiler` 已经是**值级**接缝；再抽象一层是二层抽象（`G7`） | 不预期 |
| descriptor 模型抽象 | root signature / descriptor set / argument buffer 三者形状不同 | T15 触发 |
| 资源类型基类 / 多态总表 | T30 §3 的结论不变 | T30 触发 |
| 交换链抽象 | `WindowPresentTarget` 已是中性契约 | 不需要 |
| 通用 barrier 类型 | 见 9.3 反例 | 不需要 |

### 9.6 怎么知道没跑偏（可执行的纪律）

| # | 门 | 判据 |
|---|---|---|
| D1 | vk 不改 `backend/` 的公开面 | `git diff --stat src/viz/graphics/sdk/vine/graphics/backend/` 为空（新增中性**输入**需单独说明，见 9.7） |
| D2 | `Vk*` 不出现在 vk 的**工厂/入口头**（`shell/`） | 只允许 `internal/` 与 `support/`（与 vsg 同形）；**不是门，是形状**——插件头未安装、`shell/` 无第二消费者（`G8`）。边界论证与升级条件见 `graphics-2026-11-13-vk-exposure-boundary.md` |
| D3 | 驻留账完整 | `ResidencyLedger` 每类有行或显式 absent（`live` 必答） |
| D4 | 同值同画 | 同一 `FrameDescription` 与 vsg 的记录/像素对拍（M7 已落地 2026-11-13：`tests/test_vsg/BackendParityTest.cpp`，8 例逐字节相同） |
| D5 | 不重编 | resize 不重编管线、host 状态变更不重编管线（`Keys.hpp` audit 表照搬） |

### 9.7 新加中性枚举时的判据

若 vk 确实需要一个新的中性拼写（例如 9.3 的 `AccessKind`），先问：**它是输入还是输出？**

- **输入**（被两个后端消费的值，如用途/访问类型）⇒ 可以进 `backend/`，但必须在同一提交里说明"第二个消费者是谁"，
  否则先放在 vk 的 `internal/`（`G8`）；
- **输出**（Vulkan 布局、stage 位、格式码）⇒ 永远留在插件（C6）。

这条判据替代了"看到 Vulkan 拼写就去抽象"的反射：`backend/` 里已有的 Vulkan 策略（`device_color_formats`、
`subpass`、`initial/final layout`）在 vk 上是**可用**的，不需要先改（§8.1）。

## 10. 参考

- 设计：`../design/graphics-architecture.md` 2.1 / 2.2 / 2.4 / 2.9、`../design/graphics-threading.md` §2、
  `../design/vsg-backend.md` C1–C6、`../design/graphics-content-lifecycle.md`
- 决策：`../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`（pin，选项 D 否决基类）、
  `../decisions/ADR-2026-11-13-content-seam-stays-in-the-plugin.md`
- 代码：`sdk/vine/graphics/RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`FrameDescription.hpp`、
  `Capabilities.hpp`、`Residency.hpp`、`backend/{Keys,TargetPlan,Streams,ResourceTable,MaterialArena,VariantPool}.hpp`、
  `plugins/gfx_backend_vsg/`（28,354 行）
- backlog：`../backlog/graphics.md` T30 / T15 / T32 / S4 / S5
