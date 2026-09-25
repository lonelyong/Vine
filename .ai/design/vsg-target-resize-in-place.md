# 离屏目标：尺寸变化的"原地化"（resize in place）设计

状态：**已实现（2026-09-19，旧实现）**。**2026-09-25 注**：§1–§7 的设计理由与上游先例仍然成立
（新实现 = `TargetPlan` / `applyTargetPlans`，见 `docs/data-flow.md` §4、`.ai/design/vsg-reimplementation.md` §5.6）；
**§8 的实施记录针对已删除的旧实现，只作历史**（全文在 git 历史：`git show 248c3f3:.ai/design/vsg-target-resize-in-place.md`）。
门禁（现在）：`tests/test_vsg` 的目标 / 执行器用例（`MrtTargetTest`、`HostTargetsTest`、`OffscreenTargetTest`、
`ExecutorTest`）+ 门禁应用阶段的画面判据（`scripts/vsg_rewrite_gate.sh`）。
前置阅读：`.ai/design/vsg-reimplementation.md` §5.6（目标、寿命与设备空闲）、
`docs/backend.md` §5（生命周期与所有权）、`.ai/memory/graphics-perf-backlog.md` 2026-09-19 条目。

## 0. 一句话

今天"目标换了尺寸"被实现成"这个目标从来没存在过"（`resetTargetAttachments` 一次清掉挂在它上面的
一切：附件、pass 图、内容槽、程序槽、三套 per-size shader set、所有策略标志），代价是全屏程序槽整批
重建（**实测 ~26 ms/槽**，一次最大化 6 个槽 ≈ 133 ms）。但其中**只有描述符绑定的那几张图像视图真的换了**。
本设计把**尺寸变化**与**形状变化**分成两条路：形状变化照旧整体重建，尺寸变化走
`resizeOffscreenTarget()` —— 换图像 / 视图 / 帧缓冲 + 重指向描述符，**保留** pass 图、渲染图、视图、节点、
管线、内容槽与程序槽。

## 1. 实测：钱花在哪（2026-09-19，本机 Debug + RTX 4060 + `Vine.exe`）

| 阶段 | 启动首帧 752x480 | 最大化 2352x888 |
| --- | --- | --- |
| 全量 `viewer->compile()`（新 pass 图） | 5 次 / 123.0 ms | 2 次 / 10.4 ms |
| 全屏程序槽 | 5 个 / 179.0 ms | 6 个 / 183.0 ms |
| └ overlay glslang | 45.3 ms | 50.5 ms（**已缓存，现为 0**） |
| └ **view compile**（建管线 + 布局 + 描述符 + 命令缓冲） | 132.9 ms | 131.5 ms |
| └ 我们自己的节点组装 | ~0.8 ms | ~0.9 ms |
| targets / record / present | 8.7 / 6.5 / 0.2 ms | 21.9 / 9.3 / 0.3 ms |
| **合计** | **319.5 ms** | **225.5 ms**（glslang 缓存后 **149.6 ms**） |

两个补充测量（本轮新做，决定本设计可行性）：

- **空转的全量 `viewer->compile()` = 1.0–1.6 ms**（稳定帧与 resize 后各测 10 次）。
  ⇒ `vsg` 的编译遍历对"已编译对象"确实早退，"每次 resize 跑一次全量编译"是可以接受的代价。
- **上限探针**（临时让槽跨源重建存活，已还原）：最大化 **121.7 ms**，且 6 个槽里**只有 2 个**真的
  需要重建（直写离屏目标的那个 + 深度可采样性翻转的那个）。

⇒ 本设计的目标：把一次改尺寸从 **149.6 ms 压到 ~20–35 ms**（只剩新建图像/视图/帧缓冲 + 描述符集合重建 +
一次 1 ms 级的编译）。

## 2. 事实基础（已核实到代码/规范，逐条是本设计的承重点）

1. **render pass 的兼容性不含 load/store op 与 initialLayout**：只比较附件数量/格式/采样、子 pass 结构与
   depth/stencil 格式。仓库已有此结论与 VUID 引用（`VsgPassMaterialiser.cpp` 里
   "a render pass is COMPATIBLE with another when the attachments match (format / samples), and load-ops are
   not part of that (VUID-vkCmdDraw-renderPass-02684)"）。
2. **`VkPipeline` 挂在节点对象上，且按 viewID 索引**：`GraphicsPipeline::compile` 的全文是
   `if (!_implementation[viewID]) { 合并 pipeline states → 在**本节点自己的** _implementation 里找 states 相等的
   实现（找到就复用，不建管线）→ 否则 compile shader/layout/stages + Implementation::create（vkCreateGraphicsPipelines）}`。
   ⇒ 两条结论：
   * **保留节点对象 + 保留 View（同一个 viewID）** ⇒ 下一次编译直接早退，**0 次 `vkCreateGraphicsPipelines`**；
   * **换一个新的节点对象**（哪怕用 `SharedObjects` 把 `GraphicsPipeline` 对象共出来）仍要**指望 viewID 不变**才能复用
     —— 那是多一层假设。因此采样方的重指向选择"**保节点/保 View、只换描述符集**"（§3.3）。
   另：那个复用循环**只比 pipeline states，不比 render pass**（仓库早就记着这一点）—— 同一个 `GraphicsPipeline` 对象
   被两个 render pass 不兼容的槽共享就会用错管线，这也是后端坚持"每槽一份状态注册表"的原因；本设计不动这条。
3. **`VkImage` 是延迟分配的**：`vsg::Image` 是描述节点，`vkCreateImage` + 内存分配发生在
   `Image::compile`（`vsg-src/src/vsg/state/Image.cpp`）。
   ⇒ 新建的附件**必须**再跑一次编译才会真正存在（§3.7）。
4. **帧缓冲必须与附件尺寸一致**（`VUID-VkFramebufferCreateInfo-pAttachments-00880`）
   ⇒ framebuffer 每次都换。
5. **描述符集只在首次编译时写**（`DescriptorSet::compile` 的 `if (!_implementation[deviceID])` 守卫），
   但 `DescriptorSet::setLayout` / `descriptors`、`DescriptorImage::imageInfoList`、`ImageInfo::imageView`、
   `PipelineLayout::setLayouts` **都是 public** ⇒ 可以照旧集合造一份等价新集合（§3.3）。
6. **视口是动态状态（有机制，不只是经验）**：
   `Context::create` 在 `resourceRequirements.viewportStateHint` 含 `DYNAMIC_VIEWPORTSTATE`（`ResourceRequirements` 的默认值）
   时往 `context->defaultPipelineStates` 里推 `DynamicState(VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR)`
   ⇒ 每条经该上下文的管线都是动态视口；`CompileTraversal` 会把 View 的 camera viewportState 当动态状态推，
   `RenderGraph::record` 则每帧从 `renderArea` 同步自己的 `viewportState` 再推。
   后端从不设 `viewportStateHint` ⇒ 全动态。窗口 target 的三套 shader set 只在 `initialize()` 建一次、
   从不随尺寸重建也一直正确，正是这条 ⇒ **per-size 的 shader set 与 `makeOverlayPipelineStates(extent)` 里的 baked extent
   都不必随 resize 重建**（它们只是初始值）。
7. **`vsg::createImageView(device, image, aspect)` 当场就把图落地**：它内部是
   `image->compile(device)` + `image->allocateAndBindMemory(device)` + `ImageView::create` + `imageView->compile(device)`
   （`vsg-src/src/vsg/state/ImageView.cpp` 尾部 + `Image::allocateAndBindMemory` 真的 reserve 一块 device-local
   `DeviceMemory` 并 `vkBindImageMemory`）。后端的 `createTargetAttachments()` 就是走这条 ⇒ **换完附件不需要再跑一次编译**
   （我前一版误以为需要，已改）；编译只为"把换过的描述符集写下去"服务。
8. **`RenderGraph::resized()` 会 rescale 子视口**（`WindowResizeHandler::scale_rect`，仓库踩过：HUD 矩形
   被放大 3 倍并跑出窗口）⇒ 改 renderArea 时**必须**同时把 `previous_extent` 设成新尺寸（窗口路径
   `VsgRenderer::resize()` 已有这个写法）。
9. **命令图的 children 每帧重建**（`applyRecordPlan`：`children.clear()` 后按依赖顺序装回）
   ⇒ 深度共享 barrier 换对象即可，不需要在图里做手术。
10. **`VsgRetireRing::park(::vsg::Object)`** 就是"换下来的对象晚 4 帧再释放"的原语，且它的文档明确
    点名"a pass' render pass / framebuffer swapped for another load-op variant"；`revokeDepthPromotion`
    已经在用这条纪律。

## 2b. 上游先例（`R:\opensrc\vsgExamples`，2026-09-19 核对）

上游自己有两个"运行时改离屏尺寸"的例子，结论对本设计是直接支持，值得抄与不抄的分开写：

- **`app/vsgoffscreenshot/vsgoffscreenshot.cpp`（交互式 resize，单命令图那条路，~728-765 行）**：
  尺寸变了以后它做的是
  `offscreenRenderGraph->renderArea = 新矩形` → `camera->viewportState->set(0,0,w,h)` →
  `transferImageView = createTransferImageView(...)`（新 image+view）→ `replaceChild(offscreenSwitch, 旧命令, 新命令)` →
  `offscreenRenderGraph->framebuffer = createOffscreenFramebuffer(...)` → 打印一行。
  **`RenderGraph` / `View` / `管线` 全部保留，不重编、不等设备**，下一帧直接 `recordAndSubmit`。
  ⇒ 本设计 §3.2 的"保留图/视图/管线"与 §3.6 的"改 renderArea + viewport"就是上游的做法。
  ⚠ 但它**每次都新建 render pass**（`createOffscreenFramebuffer` → `createTransferRenderPass(...)` + `Framebuffer::create(...)`），
  而管线是照旧那些 —— 这是"**新的但兼容的 render pass 对象可用**"的一条现成证据（§2.1）。
  ⇢ 它对"在飞对象可能仍指着旧图/旧 framebuffer"**完全不处理**（靠引用计数让它们自然活到没人指着为止）。
- **`app/vsgheadless/vsgheadless.cpp`（`--resize N`，~704-780 行）**：同一件事的"粗暴但安全"版——
  先 `viewer->deviceWaitIdle()`，然后重建 color/depth view **+ 新的 render pass**（`vsg::createRenderPass` / `createMultisampledRenderPass`）
  + 新 framebuffer，`replace_child` 换进图，**同样不重编**。
  ⇢ 它给出了另半个答案：**要么等设备，要么不管**。
- **窗口侧的同类证据**：`vsgwindows` / `vsgviewer` 循环里对窗口 resize **什么都不做**（swapchain 由 vsg 重建），
  同一个 View / 同一批管线照旧在新尺寸下记录 ⇒ "尺寸变化本身不使管线失效"在窗口侧早就是上游的默认假设。
- **`SharedObjects` 的粒度**（影响本设计选的机制）：`GraphicsPipelineConfigurator::copyTo(stateGroup, sharedObjects)` 会把
  `layout->setLayouts` / `layout` / `graphicsPipeline` / `bindGraphicsPipeline` **以及** `descriptor` / `ds->setLayout` / `ds` /
  `image_info->imageView->image` 全丢进注册表共享。我们的 overlay 路径传的是**空注册表**（`copyTo(stateGroup, {})`）
  ⇒ 每次建节点都是新对象；内容路径则每槽一份注册表（管线对象因此可跨重建复用）。
- **`threading/vsgdynamicviews` / `vsgdynamicwindows`**：运行时加 view 的标准写法
  `compileManager->add(*window, view)` + `compile(graph, [view](Context& c){ return c.view == view.get(); })`
  —— 这就是本设计"只为某一个视图做一次受限编译"时可用的现成形状（后端内容槽的增量编译路径已在用它）。

**本设计在这两者之间取第三条路**：`park`（不等待、也不放任）。仓库已有这个原语与纪律（§2.10），
而它比 offscreenshot 更严（不依赖"引用计数刚好不会提前销毁"）、比 headless 更快（不做设备等待）。

## 3. 设计

### 3.1 触发与分叉

今天是单条谓词（`VsgRenderer.cpp` 的 `render()`）：

```cpp
if (!target.attachments_built || target.width != target_key->width() ||
    target.height != target_key->height() || borrow_needs_rebuild ||
    !target.build_key.matches(*target_key)) {
    detail::buildOffscreenTarget(state, diagnostics, target_key);
}
```

改成三条互斥的判断（**实现时收敛成一处**：`detail::syncOffscreenTarget()`，因为同一个决定有两个调用方——pass 路径
`VsgRenderer::render()` 和全屏程序的目的地路径 `resolveProgramSlotDestination()`——两边各抄一份就是两份会分叉的规则）：

```cpp
// detail::syncOffscreenTarget(state, diagnostics, target_key)
if (!t.attachments_built || t.attachments_invalidated || !t.build_key.matches(*target_key) ||
    detail::borrowNeedsRebuild(state, t, target_key)) {
    detail::buildOffscreenTarget(state, diagnostics, target_key);       // 今天那条路（整体重建）
    return t.attachments_built;
}
if (t.width != target_key->width() || t.height != target_key->height() ||
    detail::borrowPointsAtAnotherImage(state, t, target_key)) {
    detail::resizeOffscreenTarget(state, diagnostics, target_key);      // 新路（原地）
}
```

- **形状变化**（附件数 / 彩色格式 / 深度格式 / `depthPromotion`）⇒ 照旧整体重建：兼容性和管线布局都变了。
- **尺寸变化** ⇒ 原地。
- **借用深度指向了另一个镜像**（原本 `borrowNeedsRebuild` 的第 2 条）：分成两种情形，见 §3.5 —— 借用"决定"没变、
  只是源换了图像 ⇒ 原地（`borrowPointsAtAnotherImage`）；决定本身变了（源被释放 / 不再可借）⇒ 重建。

### 3.2 `resizeOffscreenTarget()` 的对象表

| 对象 | 处理 |
| --- | --- |
| `color_images` / `color_views` | **换**（新建；旧的 park） |
| `depth_image` / `depth_view`（自有） | **换**（park 旧） |
| `depth_source_view`（借用） | **改指**源的新视图（不新建图） |
| `depth_share_barrier` | **换**（`makeDepthShareBarrier`，park 旧） |
| 每个 pass 的 `render_pass` / `render_pass_transient` | **保留**（兼容性不变）；仅当变体重算时（§3.4）才换，换下的 park |
| 每个 pass 的 `framebuffer` | **换**（`::vsg::Framebuffer::create(render_pass, 新附件, w, h, 1)`；park 旧） |
| 每个 pass 的 `graph` | **保留**，只改 `renderArea` + `previous_extent`（+ `viewportState` 由 record 每帧同步） |
| `passes` / `order` / clear 值 / load 标志 | **保留** |
| `content_slots`（含 `bridge` 缓存、编译注册、`viewport_state`） | **保留**（每帧 `updateSlotViewport` 按新 surf 尺寸重设；内容槽的描述符不绑本目标的图） |
| `program_slots`（画进本目标的） | **保留**（视图/节点/管线都在，render pass 兼容） |
| 采样本目标的 `program_slots`（长在别的目标里） | **重指向**（§3.3），**不重建** |
| `t.width` / `t.height` / `build_key` | 更新 |
| `color_seeded` / `depth_seeded` | 置 **false**（新图是 UNDEFINED，见 §3.4） |
| `any_load_pass` | 保留（它描述 pass，不描述图） |
| `depth_sampleable` | 保留（变体重算 / `publishPass` 会再决定） |
| 设备等待 | **不做**（全 park）——这是本设计与今天那条路最重要的一条区别 |

### 3.3 采样方：只换描述符集（program slot）

前提：保留槽的 `view` 与 `node` ⇒ `GraphicsPipeline::_implementation[viewID]` 命中 ⇒ **0 次
`vkCreateGraphicsPipelines`**。要换的只有"采样哪几张图像视图"。

做法（**新集合 + 停旧集合**，与仓库既有纪律一致）：

1. 建节点时把 set 0 的 `DescriptorSet` 交出来存进槽：`makeFullscreenProgramNode()` 已经在内部持有
   `config->descriptorConfigurator->descriptorSets[0]`（`GraphicsPipelineConfigurator` 的公开字段），
   加一个出参即可（和现有的 `ProgramNodeFailure*` 同一个形状）。
2. 尺寸变化后，该槽下一次绘制时（`drawScreenProgram`）判断："形状键"全同（源目标 / program revision /
   shadow map / `source_depth_sampleable` / 绑定集合）× "源目标的附件代号变了" ⇒ 走重指向，否则仍是今天的重建。
3. 重指向 = 用**旧集合自己的** `setLayout` + `descriptors` 造一份新集合：
   - `DescriptorImage`：复制 `imageInfoList`（**sampler 复用**），把 `imageView` 换成新视图；
   - 其余描述符（shadow block 这类 buffer）**原样复用**（与尺寸无关）。
4. 把 `StateGroup::stateCommands` 里的旧 `DescriptorSet` 换成新集合，并把引用它的 `BindDescriptorSet`
   改指新集合（一次遍历，两者都在 `stateCommands` 里）。
5. **park 旧集合**：在飞的命令行缓冲还指着它的 VkDescriptorSet，而且它持有旧视图 ⇒ 旧图像不会提前销毁。
6. 这一帧结束前跑**一次** `viewer->compile()`（实测空转 1.0–1.6 ms）：新集合的
   `_implementation[deviceID]` 为空 ⇒ 只多做一次分配 + 一次 `vkUpdateDescriptorSets`。

被否掉的三种写法，理由写下来免得以后重试：

- **重建节点 + 每槽一份 `SharedObjects` 让 `GraphicsPipeline` 对象共出来**：看上去更"像上游"（内容路径就是这么做的），
  但 `GraphicsPipeline::compile` 的复用是"**本节点对象的 `_implementation[viewID]`**"——换了节点对象还得指望 viewID 不变。
  保节点/保 View 才是**无条件**早退的那条（§2.2）。
- **就地改写 + `release(deviceID)` + compile**：会改写一个可能仍在飞行的 VkDescriptorSet（VSG 的池还会
  复用同一个句柄）。代码更短（~10 行），可作为校验层证明"新集合"方案不可行时的退路，不作默认。
- **复用同一批 `vsg::Image`/`ImageView` 对象，`release(deviceID)` 后重建**：`release` 会**立刻**
  `vkDestroyImageView`/`vkDestroyImage`，而旧帧与旧 framebuffer 还指着它们 —— 只能靠设备等待兜底，
  与本设计的核心收益（不做设备等待）冲突。**新建对象 + park 旧对象**才是安全的那条。

### 3.3b 落地修正：`BindDescriptorSet` 必须**换对象**，不能只改字段（2026-09-19 实测）

上一条第 4 步写成"把引用它的 `BindDescriptorSet` 改指新集合"——**那样不生效**，这是本次回归
（用户可见症状："默认 demo 最大化还原就黑了"）的唯一根因：

- vsg 的 `BindDescriptorSet::compile()` 把 `VkDescriptorSet` **缓存进命令对象**
  （`vkd._vkDescriptorSet`），并且**已编译就早退**；`record()` 绑的是那份缓存句柄
  （`build/_deps/vsg-src/src/vsg/state/BindDescriptorSet.cpp:195-215`）。所以 `bind->descriptorSet = new`
  之后：录制的每一帧仍然绑**旧集合**，`state.compile_needed` 触发的那次 `viewer->compile()` 也刷不掉它。
- 后果链（实测，`VINE_VSG_DEBUG_LAYER=1` 跑 `Vine.exe` + 最大化/还原）：旧集合里的视图被原地换掉、
  4 帧后被泊车环释放 ⇒ `VUID-vkDestroyImageView-imageView-01026`（"仍在使用中就被销毁"）+ 每帧每个绑定一条
  `VUID-vkCmdDraw-None-08114`（`albedo_tex`/`normal_tex`/`spec_tex`/`pos_tex`/`screen_tex` 都指向已销毁视图）
  ⇒ **画面不画了（黑）**；同一帧还连带出交换链失步四族
  （`vkQueuePresentKHR-03268`、`MissingAcquireWait`、`-01430`、`vkAcquireNextImageKHR-01286/01779`）。
- 判别实验（都记下来，免得以后重试）：**把重指向整体关掉、让每个采样槽走重建** ⇒ **四族全部消失**
  （证明根因在重指向，不在原地换附件本身）；**泊车深度 4→16** 无区别；**换附件前加一次计数设备等待**
  无区别。
- **修法**：`stateCommands` 里那个命令**换成新对象**
  （`BindDescriptorSet::create(bindPoint, layout, firstSet, replacement)`，并复刻 `dynamicOffsets`），
  旧命令对象随迭代器一起释放（它持有的旧集合由环继续 park）。新命令没有缓存 ⇒ 本帧的
  `viewer->compile()`（`state.compile_needed`）把它编译成新句柄。
- 验收：同一台机器、同一个 demo、同样的最大化/还原序列，开校验层 ⇒ **0 条 `Validation Error`**
  （只剩两条既有的 `ShaderOutputNotConsumed` 警告）；`test_vsg` / `test_gui` 全绿。
- 教训（已进仓库记忆）：**改一个已经编译过的 vsg 对象成员，不等于改录制**。凡是句柄被 `compile()` 缓存进
  对象的（`StateCommand` 一族），换身份就必须换/重编**对象**。

### 3.3c 落地修正二：overlay 管线的混合状态要跟 **DESTINATION** 的附件数（2026-09-19 实测）

`makeOverlayPipelineStates` 原来用 vsg 默认的 `ColorBlendState::create()`——**恰好一个** attachment。
目标的附件数一旦不是 1（例如"形状变化"后 2 个颜色附件），`vkCreateGraphicsPipelines` 就报
`VUID-VkGraphicsPipelineCreateInfo-renderPass-07609`（`attachmentCount 1` vs render pass `2`），而那条管线
是废的——不是"建了但没画对"，是管线根本不成立。

修法是把 DESTINATION 的颜色附件数一路传下去：`ProgramSlotDestination::color_count`（在
`resolveProgramSlotDestination` 里读目标入口的 `color_views.size()`，窗口恒为 1）→
`makeFullscreenProgramNode` → `makeOverlayShaderSet` → `makeOverlayPipelineStates` → 与 content 路径共用的
`makeColorBlendState(n)`（0/1/N 一条代码路径）；`overlayPipelineKey` 把它算进键，否则"附件数不同的两条管线"
会被当成同一条重复管线的证据。

**验收**：自检 resize 相位（"形状变化仍然重建一次"那一段）开校验层 ⇒ 0 条 `Validation Error`；同一轮里
`00873/00892/00765` 与 `07609` 都已消失。

### 3.4 新图是 UNDEFINED：seeding 必须重算

附件是新建的（`initialLayout = UNDEFINED`）⇒ 目标的第一遍必须 CLEAR（colour bootstrap / depth seed），
否则会 LOAD 到 garbage（Vulkan 不报错，画面却错）。

今天靠"整体重建把 `t.passes` 清空 + `color_seeded/depth_seeded = false`"自然做到。原地化后 `t.passes`
还在 ⇒ `passGraph()` 会走 `reuseSteadyPass()` 早退（它只比较"请求的 clear 策略有没有变"），
bootstrap/seed 永远不会被记录。

修法（小）：给 `PassObjects` 加一个"这份变体是在哪个附件代号上定的"字段（`attachments_generation` 或
`bool seeded{true}`），`reuseSteadyPass()` 在该值不匹配时返回空 ⇒ 该 pass 这一帧按 `planPass()` 重算变体
（render pass 重建但**兼容**，framebuffer 本来就要换），`publishPass()` 把代号更新。
`settleSubmittedFrame()` 的 transient → steady 交换照旧，transient（CLEAR seed / promotion）自动跟着来。

### 3.5 借用深度与共享 barrier

- 借用方的 framebuffer 里装的是**源的 depth view** ⇒ 源换尺寸时借用方必须一起换：原地换自己的视图 +
  把 `depth_source_view` 改指源的新视图 + 换 framebuffer + 换 barrier。
- **落地时这条分成了两种情形，判定收敛到一条规则**（`detail::borrowVerdict()`）：
  借用能不能成立，本来在两处各写一遍——build 里的 `resolveDepthBorrow()`，和这里的重建谓词——两处
  一旦分叉就是"借用方一直贴着源已经不再写的图像"或"每帧重建一次"。现在两处都问同一个函数：
  * `None`（没要求借用）/ `Honoured` / `SourceGone`（已释放或没渲染过）/ `SourceNotReady`（本帧还没建深度）/
    `SourceSizeMismatch`（尺寸不同，附件必须与帧缓冲同尺寸）/ `SourceSampled`（源把深度提升成被采样的纹理，
    采样图像不能当附件）。
  * 谓词（`borrowNeedsRebuild`）只问"**重建后的决定会不会和现在不一样**"：在借用中 + 判定仍是
    `Honoured` ⇒ 不重建（图像换了就走原地，`borrowPointsAtAnotherImage`）；判定变了 ⇒ 重建。没借用 +
    判定变成 `Honoured` ⇒ 重建（这就是"本帧还没建深度"的重试）。
  * 判定用**当前尺寸**问（不是记录尺寸）：两边同一帧长大时，借用仍然是那个借用 —— 这是"最大化一帧内完成"
    能走原地的前提。
  * "源被释放"today 是靠把记录尺寸清零来"逼"重建的（旧谓词把任何尺寸变化都当重建）；原地化之后那个
    信号会被读成"尺寸变化 ⇒ 原地"，所以释放路径改成显式的 `attachments_invalidated` 一次性标志（build 清掉它）。
- `resolveDepthBorrow()` 的"尺寸不同就不借"仍然成立（两边最终是同一尺寸），`unusable_depth_source`
  的持久/瞬态判定不受影响。
- `applyRecordPlan` 每帧重建 children ⇒ barrier 换对象即可，不需要在图里做手术。

### 3.6 图与视口

- 每个 pass 的 graph：`renderArea = VkRect2D{{0,0},{w,h}}`、`previous_extent = {w,h}`
  （**必须**，否则 vsg `resized()` 会 rescale 子视口，见 §2.7）；`viewportState` 由 record 每帧从
  `renderArea` 同步，不必手改。
- 内容槽 / 程序槽的矩形：每帧由 `updateSlotViewport` / 该 pass 的 viewport 从目标尺寸重算 ⇒ 不动。

### 3.7 编译与安全

- **新附件不需要额外的编译**：`createTargetAttachments()` 走的 `vsg::createImageView()` 当场就把
  VkImage 建好、把 device-local 内存 reserve 并 bind 好、把 VkImageView 建好（§2.7）。
  （上面 §1 里"每次 resize 一次全量编译"的说法随之只剩描述符那一步。）
- **唯一需要的编译是描述符那一步**：新构造的 `DescriptorSet` 的 `_implementation[deviceID]` 为空，
  必须被一次编译遍历访问才会分配并写入。两个选择：
  (a) `viewer->compile()`（**实测空转 1.0–1.6 ms**）；(b) 仿 `vsgdynamicviews` 的受限编译
  （`compileManager->add(...)` + `compile(object, predicate)`），代价更小但要自己管注册。
  默认 **(a)**：简单、已验证、1 ms 级。
- 一切换下来的对象 `state.retireRing.park(...)`：图像与视图、framebuffer、barrier、旧描述符集、
  （若变体重算）旧 render pass(+transient)。
- **本路径不调 `waitForIdle`**：它不拆任何 bridge / 缓存（那正是 `unhookTargetPasses` 需要等待的原因），
  换下的对象只被保留节点与命令行缓冲引用 ⇒ park 足够（与 `revokeDepthPromotion`、变体交换同一纪律）。
  门禁：`device_wait_count()` 不得因 resize 上升（policy-churn 相位已断言 0）。

## 4. 计数器 / 日志 / build profile

- `VsgRendererCounters` 加 `offscreen_resizes`（"目标原地改尺寸的次数"）。`offscreen_builds` 保持
  "整体重建"的语义（它的文档写着"稳定尺寸下必须持平"，原地化不该破坏这句话）。
- 日志一行：`[VsgRenderer] off-screen target 'X' resized 752x480 -> 2352x888 in place (N pass framebuffer(s))`
  —— 测试与脚本按它断言（`EXPERIMENTAL ... attached` 那类行的先例）。
- `VsgBuildProfile` 加 `target_resizes` / `target_resizes_ns`：否则这块钱从 `targets` 桶里"消失"，
  profile 反而看不出 resize 花了多少。

## 5. 验证计划

### `tests/test_vsg`（**落地版**：判定写进 `TargetBookkeepingTest`，像素写进自检）

计划里的 1–5 条分成了两半，因为仓库的规矩是"只有 GPU 能区分的，归自检"：

1. **保留的槽 / 计数**（1、4、5 条）⇒ `tests/test_vsg/TargetBookkeepingTest.cpp`，**不需要设备**：
   `borrowVerdict` 报出的每一种拒绝、"只换了图像的源 ⇒ 原地不重建"、"决定变了 ⇒ 重建"、
   "两边同一帧长大 ⇒ 借用照旧"、"没借成的借用只在还有可能时重试"。快（<1 ms）且能红。
2. **像素 / 不等待 / 归还**（2、3、6 条）⇒ 自检新相位（见下），因为它要真设备和真图像。

### 自检（`vsg_backend_selftest`）

新相位 `selftest_resize.cpp`：一对 deferred 目标（生产者带色 + 深度，消费者用**拷贝** program 采样它）→
两边一起改尺寸 → 断言：计数（`offscreenResizeCount` +2、`offscreenBuildCount` / `programSlotBuildCount` 不涨）、
像素（消费者读回是**产者当前的**图像：中心红、边角产者的 clear；并且拷贝铺满了**新**尺寸的全部像素）、
不等待、parked 数会回落；再用一个形状变化收尾（加彩色附件 ⇒ 仍然重建一次）。
证据行（`[selftest]` 前缀）纳入基线（`scripts/vsg_selftest_evidence.sh --update`）。

### 真机（本设计的动机来源，必须做）

`build/bin/Debug/Vine.exe`：启动 → 最大化 → 还原，读 `build profile` 行。实测结果见 §8。

### 变异（每条都要能红，否则门禁没有咬住）

| 变异 | 期望红在哪 |
| --- | --- |
| 删掉 `graph->previous_extent` 那行 | 子视口被缩放 ⇒ 矩形/像素断言 |
| 不 park 旧 framebuffer / 旧视图 | 在飞命令行缓冲引用已销毁对象 ⇒ 校验层 / `parkedCount` 断言 |
| 不置 `color_seeded/depth_seeded = false` | 第一帧 LOAD 到 UNDEFINED ⇒ 像素断言 |
| 描述符走"就地改写"而不是新集合 | in-flight 改写 ⇒ 校验层 / 新加的断言 |
| `resizeOffscreenTarget` 里偷偷加一次 `waitForIdle` | `device_waits == 0` 断言 |

## 6. 风险与边界

- **承重假设：尺寸变化不改变任何与 render pass 兼容性相关的属性。** 它由 `BuildKey`（附件数/格式/深度格式/
  depthPromotion）保证；若将来 `BuildKey` 漏掉一个兼容性相关属性（例如 samples），原地化会静默错。
  缓解：这条假设写进 `BuildKey` 的文档与本文；校验层门禁常跑。
- **不覆盖**：形状变化（整体重建）、窗口 target（本来就是原地：swapchain 由 vsg 重建、render pass 不变）、
  target 释放、readback 的语义（新图在下一帧画之前是 UNDEFINED，与今天一致）。
- **复杂度收益比**：本设计换掉 **133 ms/次 resize**，并顺带消掉最大化时 ~250 ms 的黑带（新区域在新尺寸
  帧落地前没有内容）。代价是 ~150–250 行 + 两个新用例 + 一个自检相位。

## 7. 分步实施（每步可独立验证、可独立回退）

1. **目标侧先做**（§3.2 + §3.4 + §3.6；采样方暂时仍按今天的 `dropConsumersSampling` 重建）：
   把"换图 / 换 framebuffer / 变体重算 / 不改子视口"这条链的正确性钉住（像素 + 校验层）。
   预期：**总耗时几乎不动**（槽仍是 133 ms 的大头），但它隔离掉了最难出错的一半。
2. **加采样方重指向**（§3.3）：预期 resize 从 149.6 → **~20–35 ms**。
3. 计数器 / 日志 / profile / 文档（§4）。
4. 自检相位 + 真机数字 + 证据基线（§5）。

## 8. 实施结果（2026-09-19，本机 Debug + RTX 4060 + `Vine.exe`）

实测（`build profile` 行，未改任何阈值）：

| 场景 | 之前 | 之后 | 说明 |
| --- | --- | --- | --- |
| 最大化 752x480 → 2352x888 | 225.5 ms（glslang 缓存后 149.6） | **36.4–51.4 ms** | `targets 0`、`target resizes 2 (5–6 ms)`、`rebind compiles 1 (1–2 ms)`、`program slots 1` |
| 还原 2352x888 → 752x480 | ~200 ms | **2.2 ms** | `target resizes 2 (0.3 ms)`、`program slots 0` |

那剩下的 1 个槽是**真正的新建**：日志里它的原因是 `no slot yet` —— 新尺寸下多露出一个面板，它的 pass
第一次画（28–36 ms：glslang 2 次 + 节点 + 视口编译）。
它不是 resize 的代价，而是"新内容第一次可见"的代价，与本次改动无关。

与设计预期的差异（都是落地时发现的）：

1. **决定必须只有一处**：`render()` 和 `resolveProgramSlotDestination()` 是两个调用方，第二个用同一份
   "尺寸变了就整体重建"的代码把目的地目标的槽也重建了一遍（实测：`composite` 仍然 `attached`，4 个槽
   重建）。收敛成 `syncOffscreenTarget()` 后消失。
2. **借用必须只有一条规则**：把"源的图像换了"一律当重建时，源**提升深度**（`depthPromotion`）那一类
   也会被当成"换个图像"而走原地 —— 借用方会把一个"被采样的深度"继续当附件用（`selftest` 的
   `shared depth` / `target description` 两条相位立刻报红）。修法见 §3.5。
3. **靠尺寸清零传递"你得重建"是个隐性契约**：释放路径用 `width = 0` 逼旧谓词重建；原地化把这个信号
   读成了尺寸变化。改成显式的 `attachments_invalidated` 一次性标志。
4. **槽没有被重建 ⇒ 连管线都没重建**（先前写成"会重建"，**2026-09-19 修**）：`rebind compiles 1 (1–2 ms)` 买的是
   "遍历 + 新描述符集的分配与写入"，**不是** `vkCreateGraphicsPipelines`。依据：`GraphicsPipeline::compile`
   （`build/_deps/vsg-src/src/vsg/state/GraphicsPipeline.cpp:160-219`）先查 `_implementation[context.viewID]`，
   未命中时再在**全部** `_implementation` 里找 pipeline states 相等的实现对象复用，**比较里不含 render pass**；
   保节点/保 View ⇒ 早退。旁证：一次真正的"新 View"槽要 ~26 ms（启动 `view compiles 5 = 137 ms`），
   而 `rebind compiles` 与"空转 compile"（1.0–1.6 ms）同量级。因此"保留槽 ≠ 不建管线"那句是错的，
   已从 `data-flow.md` D41 与 `docs/backend.md` 改掉。

剩余工作（不在本次范围）：

- 首次可见的新面板仍要建一个槽：**实测 22.2 ms**（`nodes 8.6` 含 glslang 8.4 + `view compiles 13.5`）。
  压掉它**不只是后端的选择**：app 的那次 `no slot yet` 是"引擎在此之前**根本没有公告那个 pass**"（面板的
  render view 那时还不存在），所以后端没有可预建的对象——要做得让宿主能**提前公告**一个 pass（或提前给出
  它的尺寸），属于宿主 API 的设计；"渲染线程本来就有空闲帧"只解决了"什么时候做"，没解决"为谁做"。
  另：这 8.4 ms glslang 是**本会话第一次**看到该 fragment 文本（进程级表按文本去重），跨会话要省得做
  **磁盘 SPIR-V 缓存**——那是另一个子系统（失效策略、缓存目录），本轮不做。
- 真机截图确认子视口位置：本轮用桌面截图尝试不可靠（Vulkan 窗口不是 GDI 表面），子视口的正确性改由
  自检相位的像素断言覆盖（拷贝程序铺满新尺寸、中心/边角都对）。
- **顺带把优先级摆正**（本轮实测，免得以后去优化错的地方）：后端启动首帧 **~313 ms**
  （`pass graphs 5 = 118.5` + `program slots 5 = 184.0` + targets 3.1 + record/present ~6），而 app 自己的路径是
  `[RenderControl] surface shown after 2292 ms` → `Attached -> Presenting after 3294 ms` ⇒ **后端只占 app 启动路径的 ~10%**，
  其余是插件加载、demo 内容（两组 cube map 六面贴图）与 splash 切换；而这段已被 splash 盖住。
  **后端侧现在唯一还可见的是那 22 ms**（以及增长期 3–7 ms 的镜像分配，2 个目标 × 新尺寸图像，属于"该付的"）。
- **证据基线（`scripts/vsg_selftest_evidence.txt`）必须在录制环境里 rebase**：新相位多一行
  `[selftest] resize: ...`，字节级 diff 必然不同 ⇒ 在 lavapipe/Linux 侧跑
  `VINE_EVIDENCE_FRAMES=30 scripts/vsg_selftest_evidence.sh --update`。本机（Windows + RTX 4060）核过两件事：
  **只多这一行**（其余 54 行逐字相同），以及基线里另一处差异——那条**带光照的**证据行
  `variant 'custom loc3-attribute program + normals + loc3 channel': centre=(230→229,38,13)`——**与本次改动无关**：
  把 `src/plugins/gfx_backend_vsg` 与 `tests/test_vsg` 暂存回 HEAD 重编再跑，同一台机器、同一个驱动同样输出
  `229`（其余几条光照值 `(34,6,2)` / `(22,13,16)` 与基线逐字相同，因为它们离舍入边界远）。
  ⇒ 在本机 rebase 会把这块 GPU 的 1 LSB 烙进基线，反而让 CI 与基线不符，所以 Windows 侧不 rebase。

