# vsg 后端与上游（vsg 1.1.16 / vsgExamples）对齐审查（2026-09-19 轮次）

本轮的目标不是加功能，而是**逐条拿上游的机制当尺子量本后端**：哪里我们用了上游已经提供的机制（应改回上游写法）、
哪里我们绕开了它（要说清为什么）、哪里是"猜的/不确定的"（要有实测或判据）。结论分四类，每条都带 `file:line`。

判据：整包构建 0 error；`test_graphics` 273、`test_vsg`、`test_gui` 全绿；自检 `VINE_SELFTEST_FRAMES=6`
（`VINE_VSG_DEBUG_LAYER=1`）**0 条断言失败**；本轮代码改动**行为中性**（证明见 §3）。

---

## 1. 已核实：与上游一致（别再往这些方向找收益）

| 检查项 | 上游怎么写 | 我们怎么写 | 判据 |
| --- | --- | --- | --- |
| 每 pass / 每 view 的矩形 | `vsg::RenderGraph::viewportStateHint` **默认就是 `DYNAMIC_VIEWPORTSTATE`**，录制时把 `viewportState` 同步成 `renderArea` 并 `pushView`（⇒ `vkCmdSetViewport/Scissor`）；`vsg::State::pushView` 还会把**该 view 的相机 `viewportState`** 压进去；`vsg::Context` 在 hint 为 dynamic 时给管线加 `DynamicState(VIEWPORT, SCISSOR)` | 从不改这个 hint（⇒ 继承默认），槽的矩形写在**该槽相机的 `viewportState`** 上（`VsgProgramSlot.cpp:129` 全屏程序、`VsgContentSlot.cpp:53-96` 内容槽，且矩形不变就早退、变了就**原位**改），渲染图上的 `ViewportState` 只是目标尺寸（`VsgPassMaterialiser.cpp:158`） | `vsg/app/RenderGraph.h:71`、`vsg/app/RenderGraph.cpp:147-162`、`vsg/vk/State.cpp:68,74`、`vsg/vk/Context.cpp:134`。⇒ 矩形是动态状态，窗口/PiP/子矩形都靠它，**管线不需要据矩形重建**（这正是"目的地尺寸不进重建身份"那条注释成立的原因） |
| 管线状态里烤一个静态 viewport | 上游**自己也烤**：`CompileTraversal::add(window/framebuffer)` 把 `ViewportState::create(extent)` 放进 `defaultPipelineStates`；`GraphicsPipelineConfigurator` 也建 `ViewportState::create(vs)` | `makeScenePipelineStates(extent)` / `makeOverlayPipelineStates(extent)` 做同样的事（`VsgPipelineFactory.cpp:143,779`），且用的是**目标表面尺寸**（`VsgProgramSlot.cpp:530` 的 `surface`），不是槽的矩形 | `vsg/app/CompileTraversal.cpp:95,129,171`、`vsg/utils/GraphicsPipelineConfigurator.cpp:83`。⇒ 与上游同构；它确实让 extent 进管线身份，但上游也如此 |
| 编译模型 | 上游每个 app 一次 `viewer->compile()`（53+ 个样例核对过），运行时用 `CompileManager` 池 + 每 (view, render pass) 一个 context | `VsgViewCompiler.cpp`：会话一个 `CompileManager` 池（池内 traversal 起手无 context），**增量**编译队列 `pending_compile_views`（生产者是真有的：`VsgContentSlot.cpp` 的 sync 尾部、`VsgTargetBookkeeping.cpp:316`），`submitFrame` 里排空；`VINE_VSG_DISABLE_INCREMENTAL_COMPILE` 是 A/B 逃生口 | 上游的 `CompileManager` 只增不减（1.1.16 无 `remove`），我们的 `VsgCompileRegistration`/`forget` 是补上这一半；池里 context 数 = 活槽数（自检断言 `compile_contexts <= content_slots`） |
| 着色器阶段的重复编译 | 上游把 `createPhongShaderSet()` 之类的 set 建**一次**复用 | 会话级 `CompiledStageTable`（键 = program + revision + ABI 变体，`VsgPipelineFactory.cpp:158-269`）+ `OverlayStageTable`（键 = 片元文本 + entry，`:796-936`）⇒ 同一段 GLSL 只过一遍 glslang | 启动行 `overlay glslang 8`、`overlay pipelines 5 of 5 distinct key(s)` |
| 跨槽共享管线 | 上游整个 Viewer 一张 `SharedObjects` | 每槽私有（P4 已否决，有实测：vsg 的 `GraphicsPipeline::compile` 复用实现时**只比 `_pipelineStates`、不比 render pass** ⇒ 跨 render pass 复用 = 用错管线；`GraphisPipeline.cpp:167-179`） | `.ai/memory/graphics-perf-backlog.md` P4 行 + §5 的"仍可做的版本" |
| Qt 集成 | 上游有 `vsgQt::QtWindow` | 自写 `detail::VsgHostWindow`（派生平台的 vsg 窗口，只改"不销毁采纳的窗口" + "能跟着换表面"） | 仓库规则"不引入第三方依赖" + 本引擎的渲染面是 **Qt 持有的 widget**（vsgQt 是反过来的所有权）。⇒ 自写是有理由的，不改 |
| `releaseRenderTarget` 的协议 | — | 引擎可以在**帧中间**释放（SDK 只要求"调用方随后可销毁 target"），后端相应放弃该目标的公告 | `RenderBackend.hpp:363-384` ⇒ 见 §4 的缺陷 |

## 2. 本轮的代码改动（一处：删掉临时逃生口）

`VINE_VSG_OWN_WINDOW` 是"临时测试逃生口"（后端自建 vsg 窗口，绕过 Qt 子窗口合成），在**生产路径**里以三处形式存在：
`VsgRenderer::initialize()` 里 `if (forceOwnWindow()) host_handle = nullptr;`、`renderContentSlot` 里"自建窗口时不同步 presenting 槽"、
以及 `VsgBackendUtility` 的 env 读取。它没有任何测试/脚本使用（只有文档在提）。

改动：**删掉逃生口**，把它代表的**事实**换成一句直白的判据 `detail::onHostWindow(state.window)`
（`VsgBackendUtility.hpp/.cpp`），并**删掉"自建窗口不同步 presenting 槽"这条跳过**——因为那条跳过只在 env 被设时才生效，
而"没有宿主句柄"的会话（自检/无头运行）一直都**是同步**的（自检的像素与诊断相位正是走那条路径）。

行为中性的证明：env 未设时 `forceOwnWindow() == false` ⇒ 旧代码 `host_handle` 不变、`if (!(... && false && ...))` 恒真（同步）；
新代码没有 env 可读、同步无分支。两条路径逐字节等价，唯一的差别是"设了 env 强制自建窗口"这件事**不再可能**（无人使用）。

**为什么值得做**：它让"窗口模式"只剩两种由会话自己决定的状态（有宿主句柄 ⇒ 采纳；没有 ⇒ vsg 自建窗口并自销毁），
而不是"会话状态 + 一个谁能翻的开关"，同时让自检的 presenting 槽走上和其他槽一样的路。

## 3. 本轮发现的缺陷：**在用的对象被销毁**（自检里 5–13 条 VUID，且随帧数增长）

**现象**（`VINE_VSG_DEBUG_LAYER=1`，断言 0 失败）：6 帧 → **5** 条，30 帧 → **13** 条，
类型 `00873`（destroy renderPass in use）、`00892`（framebuffer）、`00765`（pipeline）、`03047`（descriptor set 在用时被更新）。
序列：`released GPU resources for removed render target`（`VsgTargetBookkeeping.cpp:821`）→ 下一相位首次录制 → VUID。

**定位实验**（把 `kDeferredReleaseFrames` 从 4 改到 8，其余不动）：

| 配置 | 断言失败 | VUID | 读数 |
| --- | --- | --- | --- |
| 深度 4（原值），6 帧 | 0 | 5 | 基线 |
| 深度 8，6 帧 | 0（"策略滚动需要 ≥10 帧"那条由帧数引起） | **0** | ⇒ 直接指向"释放早了一帧"，而不是"某处根本没停放" |

**根因（一处顺序）**：`VsgRenderer::submitFrame()` 里 `settleSubmittedFrame(*commit)`（推进延迟释放环）写在
`releaseAbandonedContent()` **之前**，而后者正是会**停放**东西的扫尾（丢弃被放手的槽与保留节点）⇒
在推进之后停放的节点进的是"刚进入的那个桶"，只等到 `深度 - 1` 帧，比承诺少一帧 ⇒
某个已提交但未完成的命令缓冲仍能命名它的 render pass / framebuffer / pipeline 时，它就被销毁了。

**修法**：**推进是帧的最后一步**——移到 `releaseAbandonedContent()` 之后（`VsgRenderer.cpp` 的 `submitFrame` 尾部），
深度仍为 4（不再用"加深"掩盖）。没有东西依赖它更早：它释放的是更早帧停放的对象。

**实测（修后，`VINE_VSG_DEBUG_LAYER=1`）**：

| 负载 | 断言失败 | `Validation Error` |
| --- | --- | --- |
| 自检 6 帧 | 0 | **0**（修前 5） |
| 自检 30 帧 | 0 | **0**（修前 13） |
| app（`build/maximize_restore_probe.ps1`，含最大化/还原） | — | **0** |

回归：`test_graphics` 273 / `test_vsg`（含 `DeferredReleaseTest`、`FrameCommitTest`、`PassObjectReleaseTest`、`PassProtocolTest`）/ `test_gui` 全绿。

**为什么这是架构问题而不是补丁**：延迟释放的全部正确性都挂在"停放早于推进 + 深度 = 能命名它的帧数 + 1"这一个不变量上，
而它此前只活在注释里、没有任何东西保证调用顺序（`FrameCommit` 只挡住"没提交就推进"）。修完后不变量写进了
`VsgDeferredRelease` / `VsgRetireRing::advance` / `submitFrame` 三处文档，并被"VUID 归零"钉住。

## 4. 与上游不同、但**有意为之**（记录理由，避免下一轮又"顺手改回去"）

| 差异 | 理由 | 现状 |
| --- | --- | --- |
| 每槽私有的管线/描述符注册表（`shared_objects_`） | vsg 的实现复用只比 `_pipelineStates`、不比 render pass（`GraphicsPipeline.cpp:167-179`），而后端**故意按 pass 变体建不同的 `VkRenderPass`** ⇒ 会话级共享实测把 policy-churn 相位打红（P4） | 保留。**可做的正确版本**：按 **render pass 对象**（不是会话、也不是槽）分表 ⇒ 同一 render pass 下的两个 view 共享管线对象是**安全**的（复用时的 render pass 相同）。收益有上限：启动 `pass graphs 5 (118.9 ms)` + `program slots 5 (view compiles 137.5 ms)` 的绝大部分是**建管线**（走查对照 `rebind compiles 1 (1.0–1.6 ms)`），而样例里 4 个预览槽各自一个离屏目标 ⇒ 各自一个 render pass ⇒ 真正能共享的只有"同一目标下的多个槽"，估算省 1–2 个管线（每个 ~20 ms）。**结论：等有 ≥3 个槽落在同一 render pass 的工作负载再做**，否则风险（共享表生命周期、`clearCache()` 不能 `clear` 只能 `prune`、退役环）大于收益 |
| 退役环（park）而不是设备等待 | 上游没有这层：它靠"帧循环里 `waitForFences`"+对象随帧释放 | 保留（它是原地 resize 能便宜到 2–36 ms 的原因）。§3 的缝已经补上：**推进必须是帧的最后一步** |
| 自绘 HUD（七段盒） | 引擎的 HUD 是自包含 pass，不依赖字体/纹理上传路径 | 保留（2026-09-19 已压成"一个几何体、一次 draw、一次原地流刷新"） |

## 5. 文献登记已漂移（`docs/data-flow.md` §13 是**历史表**，但读者会当成现状）

下列条目在代码里**已修好**，而历史表仍标 🔴/🟡；§13.7 的"优先处置建议"因此是过期的（会把人引到已修项上）：

| 条目 | 历史表说法 | 代码现状（file:line） |
| --- | --- | --- |
| D3 | 用户 loc6 顶点色被白色载体覆盖 🔴 | `SceneBridgeGeometry.cpp:425-455`：用户 loc2 颜色**原样**绑定为 `vine_Color`，没有才用静态白兜底；不透明度走 `vine_draw` 块 |
| D10 | `ShaderProgram` 无 revision 🔴 | `ShaderProgram.hpp:99-108` 有 `revision()`，每次改 stage 都 bump；`SceneBridge.cpp:632,674` 用它当重建键 |
| D2 / D4 / D7 | loc1 分量数、顶点数不一致、非三角拓扑"静默" | `SceneBridgeGeometry.cpp:327-344`（loc1 报告后按缺失处理）、`:500-516`（计数不符报告并跳过）、`:188-215`（拓扑分支） |
| D22 | 运行期新增几何触发全图 `compile()` | 增量队列 + `VsgViewCompiler.cpp`（见 §1）；`pass graphs` 只在新 pass 图落地时全量编译一次 |

⇒ 建议：把这一节作为"现状核查"记在本文件（已做），并在历史表顶部加一行指针（避免再次误读）。

## 6. 待办与优先级

1. ~~修 §3 的"销毁在用的对象"~~ **已完成（2026-09-19）**：根因是"推进早于停放"的一帧差，见 §3；自检 6/30 帧与 app 的 VUID 全为 0。
2. **给证据门禁加一条 `Validation Error == 0`**（`scripts/vsg_selftest_evidence.sh`）：本类缺陷能被像素/断言全绿地漏过去，
   正是因为门禁只数失败断言。**已加（2026-09-19）**：在"退出码检查"与"基线比对"之间数 `VUID-vk`，有则硬失败；脚本**不**强制
   `VINE_VSG_DEBUG_LAYER=1`（没装层的环境会连 instance 都建不起来），只判"开了层的运行该被判的东西"。**本机未能实跑这条门禁**
   （脚本按 Linux/CI 提交为 LF，本机检出是 CRLF；自检二进制在 WSL 里没有可执行位），所以它的运行路径留给 CI；本机验证到的是它守护的事实
   （自检 6/30 帧与 app 的 VUID 都是 0）。
3. D20/D21（真机 GPU 冒烟 / 离屏 multipass 在最新 showcase 下复验）：仍是**验证**缺口，不是设计缺口。
4. ~~D6/D8（固定 CCW + cull None、program 路径不吃 per-drawable opacity）~~ **两条都已在 §7 落地（2026-09-19）**：
   D6 是后端自己搞反了 front face（不是产品选择），D8 是程序**声明即可得**（声明填不上就拒绝）——都不需要产品侧拍板了。

---

## 7. 本轮一并修掉的两条契约缺陷 + 真机验证（2026-09-19 续）

### 7.1 D6：`frontFace` 与 vsg 的 Y 翻转反了（🔴 用户可见）

**合约**：SDK 说“三角形在**世界空间**里顺时针/逆时针看＝前/后（`StateNode.hpp`）” 并明确“后端负责把自己的 front face 映到这个规则上”；
Vulkan 则在**帧缓冲坐标**里判定。
**证据**：vsg 的投影**反 Y**——`vsg/maths/transform.h:140` 的 `perspective()` 把 Y 项写成 `-f`，自述
“Y NDC coordinates are inverted in Vulkan”；`orthographic()` 同理。
**缺陷**：`RenderStateMapper.hpp:181` 写死 `VK_FRONT_FACE_COUNTER_CLOCKWISE` ⇒ SDK 的**前**面在帧缓冲里是 CW ⇒ 被当成**后**面 ⇒
`CullMode::Back` 把 SDK 的正面剔掉。demo 的 `culled_box`（`AppShellDemo.cpp:672`，几何是**按 CCW 外向绕序构造**的，见 `addBox` 的注释）
因此画的是盒子的**内壁**，而且零报告（正是 SDK 合约警告的静默失效）。
**修法**：`frontFace = VK_FRONT_FACE_CLOCKWISE`（与投影约定成对，两处文档都写明“改一个必须改另一个”）；
门禁：`RenderStateMapperTest.DefaultStateReproducesBackendDefaults` 与 `…CullModeAndPolygonMap`（后者的注释钉住“掩码与 front face 是一个决定”）。

### 7.2 D8：用户程序现在能拿到每 drawable 的值（🟡 → 可用；采用远端并行实现的版本）

**问题**：引擎的 ABI 说“不透明度是每 drawable 的值，在 `VineDrawBlock::params.x`”（B1 收敛后的唯一通道），
但 `assembleProgramShaderSet`（程序路径）只声明 set 0 的 material/diffuseMap ⇒ 自定义着色器**根本读不到**它（`setOpacity` 无效且无人说）。

**修法（2026-09-19 合并后口径）**：程序路径先扫**程序自己的文本**得到它声明的全部 `(set, binding)` 对（`detail::declaredBindings`，逐阶段），
再**按声明逐项兑现**——声明了才加到 ShaderSet，不声明就保持原样（也就不会默默占掉宿主留给自己的槽）：

| 程序声明 | 兑现的东西 |
| --- | --- |
| `set=1, binding=0` | `vine_draw`（`UNIFORM_BUFFER_DYNAMIC`，形状与引擎自带 set 一致，见 `buildVineShaderSet`）+ 拥有 set 1 布局的 `DrawBlockSetBinding`（两个都要：声明才是把 set 1 放进 pipeline layout，而 layout 缺 set 是非法管线）⇒ **每 drawable 的 `params.x` 由此可达** |
| `set=0, binding=2` | `vine_lights`（该槽的灯光块） |
| `set=0, binding=3` | `shadow_map`（该 pass 声明为输入的贴图） |
| `set=0, binding=4` | `vine_shadow`（把片元放进那张图的块） |

material/diffuseMap（set 0 的 0/1）**始终**声明：程序 set 是挂在引擎材质路径上装配的，它们是 ABI 的一部分。
引擎的每帧块用 `kProgramAbiStages = VK_SHADER_STAGE_ALL_GRAPHICS`（宿主程序可能任一阶段读它；比 SPIR-V 窄的掩码会直接校验失败）。

**声明了却填不上 = 拒绝，而不是静默空绑**：`ProgramBindingRefusal{set, binding, refused}` 记下第一对填不上的声明并让调用方报出来
（错的是程序的文本，不是“没能建起来的管线”）；理由是 pipeline layout 由这个 set 生成，SPIR-V 用了而 layout 没有的绑定不是“画错”，
是**每帧都建不出管线**。全屏程序路径（`makeFullscreenProgramNode`）同样拒绝。

**合并注记**：我的 `programReadsDrawBlock`（一个“程序是否读 draw block”的布尔）与 `ProgramDrawBlockTest` 被远端 `baf0e5c` 的
per-binding 方案取代而删除——后者更严（逐对校验、填不上就拒绝、报出 (set,binding)），门禁也更全。

**门禁**（`tests/test_vsg/ForwardShaderSetTest.cpp`）：`ThePerDrawBlockIsSetOneWithItsOwnBinding`、
`AProgramThatDeclaresTheEnginesBlocksIsDrawnNotRefused`、`AProgramBindingNothingCanFillIsRefusedNotDroppedSilently`、
`AForeignSetIsReportedInsteadOfQuietlyUnbound`。

### 7.3 D20/D21：真机 + 离屏 multipass 验证（本轮有实测）

`build/maximize_restore_probe.ps1` + `VINE_VSG_OFFSCREEN=1 VINE_VSG_OFFSCREEN_MULTISLOT=1 VINE_VSG_SLOT_DEMO=1`，
`VINE_VSG_DEBUG_LAYER=1`，Windows 11 + RTX 4060（Vulkan 1.4.351）—— 一轮包含**最大化 + 还原**：

- 4 个离屏目标（`gbuffer`/`composite`/`SceneColor`/`shadow_map`）、7 个全屏程序槽、PiP 子矩形、两处共享深度借用、
  原地 resize（752×480 → 2352×888 **10.2 ms**，还原 **0.4 ms**）、启动与关停时的目标释放；
- **`Validation Error` = 0**（含关停路径），退出干净（插件逐个 unload）。
⇒ “离屏 multipass 未在最新 showcase 下复验” 与 “真机 GPU 冒烟” 这两条**验证缺口已被这一轮覆盖**。

**顺带一个新数据点**：同一行的 `overlay pipelines 7 of 6 distinct key(s)` —— 在本轮 demo 开关下第一次出现**可去重的重复**
（7 条建成、6 个不同键）⇒ §4 里“按 render pass 分表共享管线”的触发条件比原以为的更近（但收益仍是每重复 1 条 ≈ 20 ms）。
