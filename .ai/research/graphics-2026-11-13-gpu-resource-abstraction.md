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
- T30 的"统一类型 / 多态总表"仍无跨 API 消费者；vkraw 直接实现 `RenderDevice`（18 动词）+ `RenderFrameExecutor`
  （2 动词）+ `WindowPresentTarget`，寿命规则复用 `backend/` 的四个机制头。**T30 的触发条件收窄为"第二个 API 家族的后端"**。
- T32（`RenderBackend` 聚合拆分）**不阻塞**：直接实现聚合即可。

### 8.2 形态

- 插件 `gfx_backend_vkraw`，在 `load()` 里注册工厂 `"vkraw"`（`RenderApi::Vulkan`）；**不是默认后端**，
  vsg 仍是生产路径；测试独立（新建 `test_vkraw`，像 `tests/test_vsg` 一样直接编译插件源码）。
- **复用**（SDK `backend/`）：`ResourceTable`、`RetirementQueue`、`FrameTimeline`、`ResidencyLedger`、`FrameArena`、
  `MaterialArena`、`Keys` / `TargetPlan` / `Streams` / `ClearPlan` / `Readback` / `PixelProbe` / `Observe`；
  `ShaderAbi` 的四个块与 `vn_params.glsl` 是 L1 契约。
- **自己写**（学习内容）：instance / physical device / device / queue（volk）、简单分配器、命令池/缓冲、
  fence/semaphore、image/view/render pass/framebuffer、pipeline（`ShaderProgramDescription` → glslang → SPIR-V；
  `VertexLayoutKey` → `VkVertexInput`；`RenderState` → 固定状态）、descriptor 布局/池/集、流上传、读回、swapchain。

### 8.3 里程碑（每级都有可跑的验收）

| # | 内容 | 验收 |
|---|---|---|
| M0 | 插件 + factory + `initialize/shutdown` + `capabilities`（无窗口） | registry 测试 + 设备门控的 init/shutdown，validation 0 error |
| M1 | 离屏 target（RGBA8/D24）+ 无 draw 的 pass + `ClearPolicy` + `readColorBuffer` | 像素 = pass 的 clear 色 |
| M2 | 一个 content draw：内置程序 → pipeline；`GeometryDescription` 通道 → 上传；`VineViewBlock` / `VineDrawBlock`；`vkCmdDrawIndexed` | 三角形像素（`SdkPixelFixture` 式读回） |
| M3 | `TargetDecision`（ResizeInPlace/Rebuild）、viewport/scissor、depth range、MRT、per-attachment clear、`PassDraw::state` | resize 不重编管线；MRT 各附件值正确 |
| M4 | 材质块 + 纹理 + 灯块 + screen draw + 透明排序（引擎已排好） | 材质/采样/全屏拷贝像素 |
| M5 | `createSurfaceTarget` / `WindowPresentTarget` / acquire / present / resize | 窗口像素与 `PresentRecord` |
| M6 | `ResidencyLedger` 每类、`releaseContent`、`prepareFrame`、`FrameReport` | 稳态 0 上传/0 编译；驻留随内容离开而下降 |
| M7 | 与 vsg 对拍：`test_vsg` 的设备无关子集 + 像素子集 | 同一 `FrameDescription` 的记录/像素 diff |

### 8.4 成本与风险

- M0–M2 约 2–4k 行（含 CMake/测试），是学习核心，短周期能看到三角形。
- M3–M6 到"能跑现有像素套件"约 10–20k 行；vsg 是 28,354 行，vkraw 可以按需裁剪（先不做阴影/实例）。
- 风险：validation layer 的设备门控（WSLg）、glslang 运行时编译、插件 MODULE 不可链接（测试直接编译源码）、
  `VN_GRAPHICS_BACKEND_CONSUMER` 定义。不引入新第三方：volk / glslang 已在树内。

### 8.5 与默认路径的纪律

- vkraw **不进**默认后端选择（宿主按名字 `create("vsg")`）；`SdkPixelFixture` 与现有宿主仍用 vsg。
- 它的价值是**接缝的证据**：第二个实现能回答"`FrameDescription` 是不是够用、`backend/` 的规则是不是可复用"，
  这本身就是 T30 想要的东西——只是以"第二个实现"而不是"统一类型"的形式出现。

## 9. 参考

- 设计：`../design/graphics-architecture.md` 2.1 / 2.2 / 2.4 / 2.9、`../design/graphics-threading.md` §2、
  `../design/vsg-backend.md` C1–C6、`../design/graphics-content-lifecycle.md`
- 决策：`../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`（pin，选项 D 否决基类）、
  `../decisions/ADR-2026-11-13-content-seam-stays-in-the-plugin.md`
- 代码：`sdk/vine/graphics/RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`FrameDescription.hpp`、
  `Capabilities.hpp`、`Residency.hpp`、`backend/{Keys,TargetPlan,Streams,ResourceTable,MaterialArena,VariantPool}.hpp`、
  `plugins/gfx_backend_vsg/`（28,354 行）
- backlog：`../backlog/graphics.md` T30 / T15 / T32 / S4 / S5
