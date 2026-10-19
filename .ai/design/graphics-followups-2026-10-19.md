# Graphics 后续任务列表（2026-10-19）

> **用途**：下一 session 的开工顺序。E2 已落地（`114e0465`），K1 已落地（2026-10-19）；本文件只登记
> 未做项、触发条件与判据；设计正文仍在 `graphics-image-source.md` 与
> `graphics-abstraction-review-2026-10-19.md`。
> **F2b 按本轮决定不做**，保留在“明确不做”一节，不进入排期。

## 1. P1（F2b 除外）

K1 已落地；H1 / L1 / I1 / J1 仍是触发条件型，触发未命中时硬做会违背清单原有的“不为将来可能加”裁决；
B2 是“保持现状”的登记项。

### 1.1 K1：`Pipeline` → `RenderPipeline`（已落地 2026-10-19）

- **结论**：公开类 `Pipeline` → `RenderPipeline`，`PipelineOptions` → `RenderPipelineOptions`，头/源文件
  改为 `RenderPipeline.hpp/.cpp`；`PipelineStage` 与 `pipelineStageOrder` 保留（它们是“阶段”，不是 PSO）。
  公共头、builder、engine、appfw/model_viewer/demo、测试和用户指南同步。
- **判据**：全量构建 0/0；`ctest` 24/24；`test_vsg` 463；`check_public_docs.py` 0 new；
  `check_doc_symbols.py` 通过；8 条 hygiene 门禁 0 finding。
- **风险已发生**：这是一次公开源码/ABI 破坏，下一次公开面变更窗口已用掉。

### 1.2 H1：能力预检（触发条件型，不做）

- **触发**：第二个图形家族（D3D12/Metal/WebGPU）或 MSAA/compute 立项。
- **动作**：`Capabilities` 增加 `Feature` 枚举与查询，只放“画之前就能回答”的事实；其余仍走调用结果与诊断。
- **现在不做**：没有第二个后端，也没有新的预检问题；加空枚举只会制造第二套真相。

### 1.3 L1：owned 帧（触发条件型，不做）

- **触发**：离线 / 远程渲染、帧录制、跨线程提交。
- **动作**：`OwnedFrame` 或 `FrameDescription` 的 owned 兄弟；借用规则与释放点单独设计。
- **现在不做**：当前帧是 ephemeral borrowed snapshot（见 `FrameDescription.hpp`），owned 形式会引入第二套
  生命周期规则，收益要等真实消费者。

### 1.4 I1：`shareDepth` 的资源图边（触发条件型，不做）

- **触发**：第三个共享 depth 的消费者，或出现排序缺陷。
- **动作**：独立 `DepthView` / `ImageView` 关系，让资源图边是值而不是描述字段。
- **现在不做**：`RenderTarget::shareDepth` 当前由 `FrameCompiler` 的借入边覆盖，已有两个消费者且测试稳定。

### 1.5 J1：`SceneView` 门面拆分（触发条件型，不做）

- **触发**：多视图 / 输入路由重新进入 SDK。
- **动作**：拆 `InputRouter` / `LayoutParticipant`，`SceneView` 保留“视图门面”。
- **现在不做**：一区一图的裁决仍在，拆开只会增加调用方要理解的角色。

### 1.6 B2：`RenderPass::execute` 虚函数（保持现状）

- 值路径（`describe()`）是主路，`execute()` 是逃生口；两者并存是已登记的扩展路径，不改。

## 2. P0：E2 直接收尾（新 session 在 P1 之后做）

按价值排序；每项都要结构测试 + 真实设备像素判据。

1. **全屏 ABI 支持 imported texture**
   - 现状：`ScreenPass` 声明 imported 图会被引擎报 `ContentSkipped` 并跳过；全屏 ABI 只绑 produced source
     + shadow map（`ContentPipeline::sampledSetLayout` / `ContentPass::recordScreenDraw`）。
   - 动作：定义 imported 输入的 binding（source 颜色 → depth → imported → shadow），扩展
     `sampledSetLayout` 与 `recordScreenDraw`；`ScreenPass` 不再拒绝 imported 输入。
   - 判据：`VsgBackendTest` 新增“全屏程序采样 imported 图”的像素用例；无 imported 的既有全屏程序不变。
   - 风险：shadow map 的固定 binding（4 色 G-buffer 的 5/6）不能被 import 槽位挤掉；先写 binding 规则再改代码。

2. **produced 输入按 attachment 绑定**
   - 现状：`ImageSource::attachment` / `depth` 只是选择符，vsg 仍绑定整个 target（binding N = attachment N）。
   - 动作：`CompiledInput` 带 `attachment` / `depth`，`recordContent` 只 push 对应 view；
     `color_attachments` 跟随单图而不是 target 的附件数。
   - 判据：MRT target 只声明 attachment 1 的内容 pass，绑定集合只有一张图；现有 G-buffer 预览与
     `screenCopyProgram(n)` 不回归。
   - 风险：全屏预览依赖“binding N = attachment N”，需要保留全屏路径的整 target 绑定或显式例外。

3. **imported input revision 失效用例**
   - 现状：`(Texture*, revision)` 已进 `CompiledInput`，`MaterialImages` 也按它缓存。
   - 动作：补“`setImage` 后下一帧重上传”的真实设备用例（或结构断言缓存键变化）。
   - 判据：改内容后像素变化；`MaterialImages::count()` 不增长（命中同一条目）。

4. **imported input 的 sampler state**
   - 现状：用输入 set 的 `inputSampler()`；`Texture` 没有 wrap/filter/mip 描述。
   - 动作：出现“同一贴图在不同 pass 要不同采样”的真实需求时，加 `Sampler` 值类型并进入输入声明。
   - 判据：wrap/filter 的像素用例；不是现在做。

5. **Cube / array 输入**
   - 现状：第一切片只支持 `Texture::Kind::D2`。
   - 动作：程序声明 `samplerCube` 时复用 `MaterialImages::whiteCube` fallback，输入 set 按声明 kind 建。
   - 判据：cube 输入被采样到目标；无效/不完整 cube 绑白 cube 并报告。

## 3. 明确不做

- **F2b**：色彩空间/数值域强类型（`LinearColor` / `SrgbColor` / `UnitScalar`）——本轮按决定不做；
  等第二次同类色彩/数值域缺陷出现再启动。
- 不给“将来可能”的能力加空枚举；不把 `Texture` 纳入 producer 规则；不改 `RenderTarget` 名字。

## 4. 下一 session 的第一条命令

K1 已完成，直接从 §2 的 1 → 2 → 3 顺序做 E2 收尾；每项先补失败用例，再改后端。
