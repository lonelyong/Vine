# gfx_backend_vsg 复查问题清单

> ## ⚠️ 状态：本文件的问题已全部处理（2026-09-11 复核）
>
> 这是 2026-09-08 的一次复查记录。清单中的每一项**都已修复或已按文档收窄**，
> 正文保留作为复查来源，**不要按“建议修复”重复实现**：
>
> | # | 现状 |
> |---|---|
> | 1 Lines/Points 索引被按三角形拒绝/截断 | **已修**：索引保真（仅越界拒绝），topology 进入数据身份；Points/Lines 缺法线用常量 | 
> | 2 ScreenPass 未写当前 RenderTarget | **已修**：`drawScreenTexture/Program` 写 `setRenderTarget` 选定的当前 target（含离屏），自采样拒绝、跨 target 录制按依赖排序 |
> | 3 Window 不遵守 clearDepth=false | **已按能力收窄并文档化**：window 恒清深度（vsg 拥有交换链 pass），需保留深度的 pass 应渲染到离屏 target；三处文档同步 |
> | 4 custom 程序无法使用 loc2 | **已修**：custom 路径优先绑定 authored loc2（`pack_color4`），内建路径保留内部白 DYNAMIC opacity carrier |
> | 5 不同 layout 重复 GLSL 编译 | **已修**：L1a 按 (program, revision) 编译 stage + L1b 按 (program, revision, layout) 组装 ShaderSet；`programStageCompileCount()` 钉住 |
> | 6 readback 占位 | **已诚实标注**：`RenderTarget::readColorBuffer/readDepthBuffer` 标 NOT IMPLEMENTED，`RenderBackend::readColorBuffer/readDepthBuffer` 默认返回 false=unsupported（真实现点见注释） |
> | 7 文档漂移 | **已处理**：`vine-to-vsg-data-flow.md` 页首加权威声明；本文件与 `ISSUES.md` 顶部加状态表；当前权威见 `.ai/design/vsg-pass-lifecycle.md` |
>
> 2026-09-11 另外修复了正文未覆盖的一批生命周期缺陷（pass 身份/回收、深度策略归属、
> 深度共享重建风暴与释放、每帧必须提交）——见 `.ai/design/vsg-pass-lifecycle.md`。
> 验证：`test_graphics` 135 / `test_vsg` 53 / `scripts/gfx_lavapipe_check.sh` RESULT: PASS。

> 复查范围：`src/viz/graphics` 的公开接口与实现、`gfx_backend_vsg`、VSG/graphics
> 测试用例、设计文档。
>
> 复查日期：2026-09-08。
>
> 已验证：`ctest --test-dir build -R 'vsg|graphics' --output-on-failure` 通过；本文问题
> 主要是现有单元测试未覆盖的跨模块语义、GPU 路径和文档一致性问题。

## 1. Lines / Points 的索引流被按三角形规则拒绝或截断

**优先级：高**

### 位置

- `src/plugins/gfx_backend_vsg/src/SceneBridge.cpp`
  - `SceneBridge::buildGeometryData()`，约 976–1011 行。
- `src/viz/graphics/sdk/vine/graphics/StateNode.hpp`
  - `Topology`，约 48–62 行。

### 详情

graphics 模块把 topology 定义为独立于 Geometry 数据的渲染状态，并公开支持：

- `Topology::Triangles`
- `Topology::Points`
- `Topology::Lines`

但 VSG bridge 在构建任何 indexed geometry 时都执行以下三角形专用规则：

1. 计算 `src_indices.size() / 3`；
2. 少于 3 个索引时拒绝 geometry；
3. 不是 3 的倍数时丢弃尾部索引；
4. 仅将剩余索引传入 `DrawIndexed`。

`buildGeometryData()` 并未接收 `ResolvedRenderState`，无法获知该命令实际使用
Points、Lines 还是 Triangles。

### 影响

- 两个索引组成的一条线会被拒绝；
- 四个索引组成的两条线会被截为三个索引；
- 点云的尾部点在索引数非 3 倍数时被静默丢失；
- 即使 `RenderStateMapper` 正确映射了 Vulkan topology，提交给 GPU 的 index count
  仍已错误。

### 建议修复

- 让 geometry-data 构建路径获得 effective `ResolvedRenderState`，或将 index stream
  原样保留，把“用于自动法线计算的完整三角形前缀”与实际 DrawIndexed count 分离；
- Points 保留全部索引；Lines 按两个元素成组；Triangles 才按三个元素成组；
- 自动法线仅适用于三角形，线和点应使用安全默认法线或要求自定义 shader；
- 添加 indexed/non-indexed Points、Lines、Triangles 的 draw-count 测试。

## 2. ScreenPass 未写入当前 RenderTarget

**优先级：高**

### 位置

- `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`
  - `VsgRenderer::drawScreenTexture()`，约 1567 行以后；
  - `VsgRenderer::drawScreenProgram()`，约 1818 行以后。
- `src/viz/graphics/sdk/vine/graphics/RenderBackend.hpp`
  - `drawScreenTexture()` 契约，约 79–101 行；
  - `drawScreenProgram()` 契约，约 116–144 行。

### 详情

graphics 接口规定全屏纹理/程序 pass 应绘制到最近一次 `setRenderTarget()` 选定的
**当前 target**。但 VSG 实现固定从 `impl->targets[nullptr]` 取得 destination graph，
即始终把 PiP 和 deferred fullscreen view 附加到 window graph。

### 影响

- ScreenPass 无法将采样结果写入另一个 off-screen RenderTarget；
- 后处理链（A → B → C → window）无法实现；
- API 语义与实现不一致，调用者即使先调用 `setRenderTarget(B)` 也不会得到 B；
- `active_target` 没有在 screen draw 路径统一消费，状态模型也与 `render()` 不一致。

### 建议修复

- 将 screen/program slots 归属到当前 active target，而非固定 window target；
- 统一 `render()`、`drawScreenTexture()`、`drawScreenProgram()` 对 active target 的消费规则；
- 为 window、off-screen target、以及多级 off-screen 后处理链增加 GPU self-test；
- 为源 target 与目标 target 相同的反馈回路显式拒绝或实现 ping-pong，避免 Vulkan
  同时读写同一 attachment。

## 3. Window 路径仍不遵守 clearDepth=false

**优先级：高**

### 位置

- `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`
  - `VsgRenderer::clear()`，约 2424–2485 行，尤其 window 分支约 2456–2467 行；
- `src/viz/graphics/sdk/vine/graphics/RenderBackend.hpp`
  - `clear(const Color&, bool clearDepth)`，约 229–234 行。

### 详情

离屏 target 已通过不同 render-pass load op 支持 depth CLEAR/LOAD。但 window 分支仍
总是更新 swapchain graph 的 depth clear value；实现注释也明确说明不能抑制 window
render pass 的深度清除。

### 影响

- `clear(color, false)` 在 window 上仍会清深度；
- 同一帧中需要保留主 pass 深度的 window 后续 pass 不能按接口工作；
- 同一 RenderBackend API 在 window 与 off-screen target 上语义不同。

### 建议修复

- 明确选择一种方案：支持 window render-pass 的 depth LOAD 变体，或将接口能力收窄并
  在 graphics API 中显式表达限制；
- 如果支持，按 target/clear policy 管理 window render graph 和兼容 pipeline 生命周期；
- 添加两个 window pass 的端到端测试：第一个写深度，第二个 `clearDepth=false` 后依赖该深度。

## 4. 自定义 ShaderProgram 无法使用 Geometry location 2 数据

**优先级：中**

### 位置

- `src/plugins/gfx_backend_vsg/src/SceneBridge.cpp`
  - 自定义 attribute 遍历，约 1050–1053 行；
- `.ai/design/vsg-custom-attributes.md`
  - location 2 规则，§4、§8；
- `tests/test_vsg/CustomAttributeTest.cpp`
  - 当前锁定 location 2 被忽略的测试，约 296 行。

### 详情

设计文档规定 custom-program 路径中：Geometry loc2 存在时应作为 `vsg_Color`
透传；不存在时才使用内部白色数组。当前实现直接跳过所有 `location <= 2` 的用户
buffer，始终绑定后端生成的白色/opacity carrier。

### 影响

- 自定义 shader 读取 `layout(location = 2) in vec4 color` 时不能得到 Geometry 输入；
- 用户提供的数据被静默替换；
- 代码、测试与自定义 attribute 设计文档相互矛盾。

### 建议修复

- 内建路径保留内部动态 opacity carrier；
- custom-program 路径优先采用有效 Geometry loc2 数据，否则回退白色数组；
- 确认自定义 program 的 opacity 语义：若需要 Node opacity，应定义独立 attribute、
  uniform 或在 loc2 alpha 上明确合成规则；
- 将现有“loc2 被忽略”的测试改为透传测试。

## 5. 不同 custom layout 会重复执行 GLSL 编译

**优先级：中（性能与设计一致性）**

### 位置

- `src/plugins/gfx_backend_vsg/src/SceneBridge.cpp`
  - `SceneBridge::getProgramShaderSet()`，约 544–609 行；
  - `buildProgramShaderSet()`，约 398–453 行；
- `.ai/design/vsg-custom-attributes.md`
  - L1a/L1b 缓存设计，§6。

### 详情

当前缓存键包含 `(program pointer, custom layout)`。当同一 ShaderProgram 第一次遇到
另一套 custom attribute layout 时，会再次调用 `buildProgramShaderSet()`，并对 program
中的每个 stage 再次执行 glslang 编译。

设计文档要求的模型是：shader stages 按 program/revision 仅编译一次；不同 layout
只创建不同 ShaderSet/vertex-input 声明。

### 影响

- 多种点云/网格 layout 共用一个 shader 时，加载和热编辑开销线性增长；
- GLSlang 失败缓存也按 layout 重复；
- 与已写入仓库的设计承诺不一致。

### 建议修复

- 拆分 stage/SPIR-V 缓存与 ShaderSet layout 缓存；
- stage 缓存 key 使用 `(program pointer, revision)`；
- ShaderSet 缓存 key 使用 `(program pointer, revision, layout signature)`；
- 添加测试，验证同一 program 的不同 layout 不增加 stage 编译计数。

## 6. RenderTarget readback 是占位实现

**优先级：中**

### 位置

- `src/viz/graphics/sdk/vine/graphics/RenderTarget.hpp`
  - `readColorBuffer()` / `readDepthBuffer()`，约 113–123 行；
- `src/viz/graphics/src/RenderTarget.cpp`
  - 两个函数实现，约 74–84 行；
- `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`
  - 离屏 image 已声明 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`，约 1462 行。

### 详情

graphics API 文档称 RenderTarget 提供颜色/深度读回。但当前实现只分配同尺寸的全零
vector；VSG 后端没有 staging buffer、image-to-buffer copy、queue 同步或结果交付机制。

### 影响

- 导出、截图、离线分析、CPU 后处理无法读取真实渲染结果；
- API 表现看似正常但返回错误数据，错误不易被察觉；
- 现有 `TRANSFER_SRC` usage 没有被实际利用。

### 建议修复

- 先在 graphics 层明确 readback 的异步/同步、格式和所有权模型；
- 为 RenderBackend 增加受控的 readback hook，避免 logical RenderTarget 自己伪造数据；
- VSG 实现 staging buffer、layout transition、copy、fence/wait 和格式转换；
- 增加小尺寸 render target 的像素/深度 readback GPU 测试。

## 7. vine-to-vsg-data-flow.md 与当前实现漂移

**优先级：低（维护风险）**

### 位置

- `src/plugins/gfx_backend_vsg/vine-to-vsg-data-flow.md`

### 详情

文档仍描述多个已改变的实现细节，例如：

- position/normal 固定按 3-float stride 解包；
- 自定义 attribute 尚未接线；
- 缓存和编译流程仍是旧版本描述。

这与当前的 components-safe 解包、自定义 location >= 3 支持、增量 compile 路径及
`.ai/design/vsg-custom-attributes.md` 冲突。

### 影响

- 后续开发可能按过期行为实现功能或编写错误测试；
- 设计文档无法作为代码审查和故障排查的可信依据。

### 建议修复

- 以现有 `SceneBridge.cpp` 和 `VsgRenderer.cpp` 为准更新数据流文档；
- 明确 location 2 的最终决策；
- 把历史“待接线”描述移入 change log，不与当前契约混写；
- 在文档页首标注权威来源与最后核对的代码版本。

## 建议测试矩阵

| 场景 | 应验证的结果 |
|---|---|
| Indexed Points | 所有 index 保留，DrawIndexed count 与源一致 |
| Indexed Lines | 以两个 index 为一条线，不被三角形逻辑拒绝/截断 |
| ScreenPass 到 off-screen target | 输出存在于 destination target，不是 window |
| A → B → C 后处理链 | 每一步采样前一 target，最终结果正确 |
| Window clearDepth=false | 后续 pass 可观察并使用前一 pass 深度 |
| Custom program + loc2 | shader 收到 Geometry 提供的 vec4 数据 |
| 同 program、多 layout | stage 编译一次，ShaderSet 按 layout 区分 |
| Color/depth readback | 返回真实 GPU 像素与深度，而不是全零 |
