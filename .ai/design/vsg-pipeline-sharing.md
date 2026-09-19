# vsg 后端管线共享 / 变体缓存设计

> 模块：`src/plugins/gfx_backend_vsg`
> 日期：2026-09-08（本轮落地 SharedObjects 共享 + L1 program 缓存 + L2 变体模板缓存）
> 关联：`src/plugins/gfx_backend_vsg/gfx_backend_vsg.md`（模块全解）、
> `src/plugins/gfx_backend_vsg/docs/data-flow.md`（数据映射/缺陷表 D10/D16/D19/D22）、
> `.ai/design/vsg-custom-shader.md`（program ABI）、
> `.ai/design/vsg-user-mutation-strategy.md`（用户端可变/可配/可组合情形的处理策略）。
>
> 一句话：**管线的数量应跟随"状态变体数 × program 数 × 槽位"，而不是几何数；
> 材质是 descriptor（DS），不是管线维度。** 本文件记录让该不变量成立的机制、代码落点、
> 已验证行为、剩余边界。

## 1. 目标与背景

- 支撑"≥1k 个独立 drawable 的流畅渲染"以及后续 STEP 级大模型的加载/稳态帧成本。
- 背景（历史缺陷）：此前 `SceneBridge::shared_objects_` 虽被传入
  `GraphicsPipelineConfigurator::copyTo()`，但**从未被赋值（恒为 nullptr）**——
  VSG 只在 `SharedObjects` 非空时走内容级去重，因此每个几何各建一条 `VkPipeline`，
  连状态完全相同的几何也不合并。若干模块文档声称"已共享"，属文档-代码漂移。

## 2. 维度归位（什么是管线，什么不是）

`vkCreateGraphicsPipelines` 只由下列决定：

| 变化来源 | 影响层 | 归位 |
|---|---|---|
| RenderTarget（窗/离屏/MRT/depth-only、color_count） | render pass / subpass | 槽位分区（每个 content slot 一个 `SceneBridge`） |
| Shader（内置 Phong/Flat 或用户 program） | stages + descriptor layout + pipeline layout | **L1 program 缓存**（一"族"一 ShaderSet） |
| StateNode 折叠后的 `ResolvedRenderState` | depth/cull/polygon/blend/拓扑 | **L2 变体键**组件 |
| 材质**值** | UBO 内容 | **DS**（`VsgMaterialManager` 按 `Material*` 缓存），不进管线键 |
| Matrix / opacity / 顶点数据 | 逐几何 retained 数据 | 不变 |

管线键（内容级）≈ `(program, ResolvedRenderState, subpass/color_count)`；
**材质、矩阵、透明度、几何缓冲都不是管线维度。**

### 2.1 深度状态的归位：core 1.3 扩展动态状态（里程碑 A 已落地）

depth test / write / compare 属**管线创建状态**（core 1.1），所以"深度策略"曾是管线维度
：一个 slot 按 `DepthMode` 选三份 ShaderSet 之一（`VsgRenderer.cpp` 的
`depth_on/depth_testonly/depth_off_shader_set`），策略一变就要 `setContentDepthMode`
+ `invalidateState` 重建变体。core 1.3 把这三种状态变成**动态状态**，于是"同一管线服务
所有深度策略"成立，策略变化降级为每条 draw 一条命令。

- 落地物：`VsgDynamicDepth.hpp/.cpp`（`SetDepthState` 命令、`makeDynamicDepthState`
  声明、`declaresDynamicDepth` 判定、`kDynamicDepth` 常量）。三条建 set 的路径
  （`VsgRenderer` 窗口 set、`VsgTargetBookkeeping` 每 target、`VsgContentSlot` 懒建）
  都传 `kDynamicDepth`，避免三处漂移。
- **无需申请任何 device feature**：registry 里 `VK_EXT_extended_dynamic_state` 标
  `promotedto="VK_VERSION_1_3"`，其 feature 结构体标 `comment="Not promoted to 1.3"`
  ——`VkPhysicalDeviceVulkan13Features` **没有** `extendedDynamicState` 成员（写它编不过
  ），EXT 结构体则属 pre-1.3 的写法。真正兜底的是本后端的版本地板
  （`detail::kRequiredVulkanVersion = 1.3`，见 `VsgRenderer::initialize` 的拒绝路径）。
- ⚠️ **`StateCommand::slot` 是"状态栈身份"而不是优先级**：vsg 把同组状态命令按 slot 压栈，
  `StateStack::record` **只录制栈顶**，同 slot 的后一条会把前一条**彻底遮蔽**（前一条从不
  录制）。vsg 自身分配：`0` 管线绑定、`1 + firstSet` 描述符绑定、`2` view-dependent
  state / push constants。`SetDepthState` 因此取 `kDepthStateSlot = 15`（`STATESTACK_SIZE`
  上限），而不是"最小空闲槽"——描述符 set 索引由 program 决定，`1 + firstSet` 会随之上移。
  **第一版用了默认 slot 0**：结果整组丢掉了管线绑定（validation layer 报
  `VUID-vkCmdDrawIndexed-None-08606`，lavapipe 随即崩溃）。
- `slot` 还必须被 `CollectResourceRequirements` 收集（它按每条 `StateCommand::slot` 抬
  `maxSlots.state`，`State::stateStacks` 依此 sizing；超出者既不录制也会越界索引）。实测
  probe：`stateStacks.size() == 16`、`maxSlots.state == 15`，即收集确实看到变体组里的这
  条命令。
- 声明判定用**并集**（`declaresDynamicDepth` 跨 `DynamicState` 对象累计三个名字）：vsg 的
  `mergeGraphicsPipelineStates` 对同类型管线状态做"同类型替换 / DynamicState 取并集"，而
  `Context` 默认注入 `DynamicState(VIEWPORT, SCISSOR)`（见 §7），故本 set 的声明在
  compile 期必然与它合并。若只查单个对象就会误判集合（保守拒绝一个驱动本来认账的 set）。
- 里程碑 A 是**行为中立**的管线：命令携带的值正是原本烘焙进 create-info 的值，因此证据
  与改动前逐字节一致（除两个已知漂移计数）。有效性用**变异**证明：把发出的命令强制
  `depthTestEnable = VK_FALSE`（烘焙值不变）⇒ 深度相 21 项断言 FAIL（"nothing wrote depth
  in stage 1" 等），说明生效值确实来自该命令；随后还原。
- 仍未做（**里程碑 B**）：三份按 depth 策略区分的 ShaderSet 仍在、`depth` 仍在变体键/
  `shaderSetFor` 里——B 要把 depth 维度从变体身份中移除，使策略变化 = 每条 draw 一条命令，
  而不再走 `setContentDepthMode` + `invalidateState` 重建。

## 3. 机制

### 3.1 SharedObjects 内容级共享（基础层）
- `SceneBridge` 构造创建 `shared_objects_ = ::vsg::SharedObjects::create();`
  （此前缺失 → 该 1 行修复让 `copyTo()` 的 `share()` 去重生效）。
- `SharedObjects::share` 用 `std::set` + 内容比较（`Object::compare`）去重：
  布局 / GraphicsPipeline / BindGraphicsPipeline / DescriptorSet 均被替换为已注册的等价对象。
- `GraphicsPipeline::compare` 比较 `pipelineStates` 内容 → 状态相同即合并为一条
  `VkPipeline`；编译时逐对象 `vkCreateGraphicsPipelines`，因此合并发生在 build 期。
- 计数：`pipelineVariantCount()` —— `copyTo()` 后 `bindGraphicsPipeline` 仍是本地对象
  = 新变体（++）；被共享替换 = 去重命中（不 ++）。

### 3.2 L1 · program ShaderSet 缓存
- `SceneBridge::getProgramShaderSet(program)`：按 `(slot, program)` 缓存运行期
  glslang 编译产物（`program_shader_sets_`）。同一 program 被 N 个几何引用只编译一次；
  编译失败也缓存 `null`（不再每帧重试，行为同 D9 的静默回退）。

### 3.3 L2 · 变体模板缓存（显式 PipelineKey 快速路径）
- 键 = `(program*, material*, ResolvedRenderState)` 的内容哈希
  （`variant_cache_`：`hash → unique_ptr<VariantEntry>`；`VariantEntry` 存完整键做
  等值比较，哈希碰撞 → 覆盖旧模板 = 仅失去缓存，不产生错误渲染）。
- 首个几何：完整 `GraphicsPipelineConfigurator` → `init()` → `copyTo()`，随后捕获
  `state_commands`（共享 BindPipeline + 该材质的 BindDescriptorSet）、
  `base_binding`、`prototypeArrayState` 存入 `VariantEntry`。
- 后续同变体几何：**跳过 configurator**，把捕获的共享 state 命令装入新 `StateGroup`，
  只装配自己的 `BindVertexBuffers / BindIndexBuffer / DrawIndexed`。
- 计数：`variantReuseCount()`（命中 ++）。

## 4. 缓存与所有权（代码落点）

| 缓存 | 键 | 值 | 谁持有 / 何时释放 |
|---|---|---|---|
| `SceneBridge::cache_` | `Geometry*` | `unique_ptr<Item>`（矩阵变换+顶点数据） | bridge；外侧无人持有（`abandoned(shares)`）即逐出（2026-09-14 起无时间窗） |
| `SceneBridge::program_shader_sets_` | `ShaderProgram*` | `ref_ptr<ShaderSet>`（L1） | bridge；`clearCache()` |
| `SceneBridge::variant_cache_` | 变体内容哈希 | `unique_ptr<VariantEntry>`（L2） | bridge；`clearCache()` |
| `shared_objects_` | —（内容去重） | pipeline/layout/DS | bridge；`clearCache()` + 析构 |
| `VsgMaterialManager::cache` | `Material*` | `ref_ptr<vsg::ubyteArray>`（`VineMaterialBlock` 的字节） | 引擎注入（跨槽共享） |

- **per-view 约束**：vsg pipeline 按 viewID 编译；跨 view 共享已编译管线会崩
  （`setupContentSlot` 注释）。因此所有共享都在**单个 content slot 的 bridge** 内，
  槽间不共享。
- 顶点数据 / 材质 UBO 上传与管线共享正交：管线少 ≠ 顶点缓冲少；每几何仍独立上传
  数据（后续 instancing / buffer 身份共享再优化）。

## 5. 已验证行为（tests/test_vsg/SceneBridgePipelineSharingTest.cpp）

- 250 个同状态同材质几何 → **1 pipeline 变体 + 249 次 reuse**（configurator 只跑 1 次；
  同用例 11ms → 3ms）。
- 200 种材质 → **仍 1 pipeline、0 reuse**（材质是 DS 维度；不同材质各付一次首建）。
- 2 种解析状态（默认 + Points）→ 2 变体、98 reuse。
- 同一用户 program × 3 几何 → **1 变体 + 2 reuse**（L1 只编译一次）。
- 几何重建（revision 变）→ 复用既有模板，不新增管线。
- **透明度实时（P0，2026-09-08）**：默认路径的颜色数组标 `DYNAMIC_DATA`，改写 alpha 后
  `dirty()` —— vsg `TransferTask` 只在 dirty 时回传（`syncModifiedCounts`），未变化帧零拷贝；
  由 `OpacityEditRebuildsNothing` 回归守护（不透明度改写不重建几何；以前那条“颜色数组必须是
  DYNAMIC 载体”的断言随载体一起删了）。program 路径
  颜色数组保持静态（D8：program 拥有 opacity）。
- **材质刷新（P2）**：每帧每个**去重材质**先比较后覆写（O(distinct materials)，D19 缓解）。
- **材质属性热改（2026-09-08）**：material 块的数据标 `DYNAMIC_DATA`（VsgMaterialManager），
  确有写入时 `value->dirty()`（SceneBridge 尾部）——材质属性编辑在次帧 TransferTask 回传，未变零拷贝；
  回归 `MaterialManagerTest` 的 DYNAMIC 断言（2026-09-13 起 payload 是 `VineMaterialBlock` 的字节，不再
  是 `vsg::PhongMaterialValue`）。
- **每帧至多一次全图编译**：`VsgRenderer` 把 created 触发的编译延迟到 `submitFrame()`
  （多槽一帧只编一次；setupContentSlot 的新 View 仍即时编译）。
- **数据/状态解耦（2026-09-08）**：Item 子树改为 `MatrixTransform → state_node → data_node`。
  - `buildGeometryData()` 只做顶点数据（物化 + Bind/BindIndex/Draw）；`buildStateGroup()`
    只做 (program,material,state) 的管线+DS 包装（L1/L2 在其内）。
  - 重建语义：**revision 变 → 只重建 data_node**（state 原样复用）；
    **material/state/program 变 → 只重建 state_node**（复用 data_node，不再重物化/重传网格）。
  - 这是后续“子集/别名 drawable”（STEP 元素级高亮）的结构基础。
  - 测试：`StateOnlyRebuildReusesGeometryData`（材质变→transform 同对象+顶点数组同身份）、
    `DataOnlyRebuildLeavesStateUntouched`（数据变→不新增变体/不跑 configurator）。

## 6. 与既有缺陷表对照

- D8（program 路径 opacity）：仍待（program 路径颜色数组静态，opacity 由 program 拥有）。
- D10（program 无 revision）：**已修（2026-09-08）** —— `ShaderProgram` 新增内容 `revision()`
  与 `clearStages/replaceStages/setStage`（SDK）；后端 L1（ProgramEntry 存 revision）、Item
  （`program_revision`）与 L2 变体哈希都纳入 program revision → 同一对象改 GLSL 次帧重建新管线。
  回归测试 `EditingProgramSourceRebuildsVariant`（改源→created=1、数据节点复用、变体 1→2）。
- D16（shared/变体只增不减）：**已修（2026-09-11，vsg-pass-lifecycle §20）** —— 三个 program
  缓存迁到缓存骨架：容量用同一套 FIFO `trimToCapacity`（`program_stages_` 64 /
  `program_shader_sets_` 64 / `variant_cache_` 256），插入时修剪，条目自持键对象，每帧
  `releaseAbandonedCaches()` 回收链尾；“超限即整表清空”已删（它会把当前场景正在绘制的程序
  一并丢掉）。`clearCache()`（teardown/resize/release）仍清空。<br>注意：`shared_objects_`
  （vsg 内容去重表）仍是“只增 + 槽释放时清空”，没有容量上界。
- D13（`updateMaterial` 换对象使 DS 失效）：**已修** —— 改就地刷新同一 Phong 对象 + `dirty()`。
- D19（每帧 O(materials) 就地改写）：2026-09-08 起**按去重材质 + 比较后写**（O(distinct
  materials)），与共享 DS 兼容（值写同一 UBO）。
- D22（新几何触发全图 compile）：`renderContentSlot` 把新增的槽 view 收进 `impl->pending_compile_views`，
  `submitFrame()` 只编译这些 view（**最终结论 2026-09-08，修正三轮排查的错误归因**）：
  - **真实根因不是 “compileTask 用临时 traversal、不填池”**（v1.1.16 的 `Viewer::compile()` 走
    `compileManager->compileTask(task, …)`，全图编译本身没毛病），而是 **compileManager 的池 traversal
    只在首次 `Viewer::compile()` 时建一次**；而 Vine `initialize()` 的首次全图编译发生在**空的窗口图**
    （content-slot View 是之后懒加的）→ 池 traversal 的 contexts **为空** → `compileManager->compile(view)`
    用 0 个 context 遍历 = “成功但什么都没编” → record 时管线缺 `_implementation[viewID]` → SIGSEGV。
  - **实现（不改 vsg、纯公共 API）**：新增 `VsgRenderer::incrementalCompileViews()`。每个待编译 view
    首次见到时用公共 `CompileManager::add(window/framebuffer, view, requirements)` 把
    “(该 target 的 renderPass：窗口 swapchain / 离屏 framebuffer) + view” 注册进池（每个槽一次，
    `ContentSlot::compile_context_registered` 去重；requirements 由 `CollectResourceRequirements` 从该
    view 收集）；随后 `compileManager->compile(view, selector)` **只选 context.view == 该 view** 的
    context 编译（pipeline 创建需要 renderPass；`apply(View)` 会把 viewID 设对），再 `updateViewer`
    同步 dynamic data/bin。等于把全图 compile 缩放到单个变更 view。
  - 状态：**默认开启按 view 增量编译**（2026-09-08 真机 demo 验证通过后去除 opt-in env 门控）——
    `submitFrame()` 每帧只要有槽新增/重建就走 `incrementalCompileViews()`；失败自动回退全图。
    保留逃生开关：设 `VINE_VSG_DISABLE_INCREMENTAL_COMPILE`（任意值）即强制走稳定的全图编译（A/B）。

## 7. 边界与后续

- material 进变体键 ⇒ 每个不同材质首几何仍跑一次 configurator（成本受**材质数**而非
  几何数约束）。若要彻底分离：两段式 `(program,state) → pipeline` +
  `(program,material) → DS`。
- viewport **无需改造**：vsg `ResourceRequirements.viewportStateHint` 默认 `DYNAMIC_VIEWPORTSTATE`，
  `Context` 默认向 `defaultPipelineStates` 注入 `DynamicState(VIEWPORT, SCISSOR)`（compile 时并入），
  且 `State::pushView` 每帧把 `camera->viewportState` 压栈 → record 期 `vkCmdSetViewport` 生效。
  烘焙的静态 `ViewportState(extent)` 只是编译期默认，resize 不要求重建几何管线。
- 增量 compile（D22）**已做**（见 §6；需真机复验，见 §7 风险）。
- 大网格 + 元素级选择/高亮（STEP 上万个边/面）的设计已定稿：
  `.ai/design/vsg-selection-highlight.md`（部件级大网格 + 元素表 + 紧凑动态子集 drawable + BVH 拾取）。
- ⚠️ **D22 按 view 增量 compile 已默认开启**（2026-09-08 真机 demo 验证 + 默认路径 gdb 冒烟无崩）。
  仍建议在真实 demo 复验“运行期新增几何/离屏多槽/材质·透明度·shader 热编辑”；异常时设
  `VINE_VSG_DISABLE_INCREMENTAL_COMPILE` 即回退到稳定的全图编译（逃生开关，非默认路径）。

