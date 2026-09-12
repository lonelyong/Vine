# Graphics 模块核心

> 2026-09-12 **`render()` 只取一处（设计 §43）**：先判断 —— 115 行里值得抽的只有**一处**（两段深度借用判定），
> 其余（`ContentSlotRequest` 填充、`retargetPass` 前后）是机械搬运。①`Impl::borrowNeedsRebuild(t, target_key)
> const` 把两条互异的"借用失效"合成一个纯函数：**PENDING**（请求的借用还没兑现，源当时没有深度图像；永久不可用
> 的源记成 `unusable_depth_source` ⇒ 只在"仅仅在等"时重试，禁用/未构建的产出方每帧只花一次查表）+ **STALE**
> （已兑现的借用绑源的深度 *view*，源被重建会换图像 ⇒ 借用方继续测没人写的旧图像、**借来的深度静默冻结**；
> 用"源当前 view vs 烘入时记下的 view"检测）。②**删掉重复注释**：`render()` 里 20 行 scope 属性说明在
> `setPassOrder`/`setDepthMode`/`setRenderTarget` 各自的注释里**已有** ⇒ 压到 8 行（README 式一句 + 指向 setter，
> 保留 render() 特有的"每目标共用一条槽路径、附件在此确保"）。`render()` 115→**81**，TU 960→926。**踩坑（第二次
> 同族）**：`targets` 表的键是非 const 指针 ⇒ `wanted_source` 不能声明成 `const RenderTarget*`（`map::find` lose
> const qualifier），用普通指针（`depthSource() const` 本就返回普通指针）。验收同前（证据逐字节相同 / VUID 0 /
> FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **"单调用点 helper"审计 + 提交后一步合并（设计 §42b，承 §42）**：审计实测 —— `submitFrame`
> 拆出的五个方法**各 1 个调用点**（既有的 `retireInactivePassSlots` 也一样），本批无死代码；但分三类：
> ①`compilePendingViews()` **有真实复用潜力**（它碰的队列被 `renderContentSlot` push、detach/teardown erase，
> 将来"提前编译"入口或第二条提交路径就该共享它）；②"帧已提交"事件类 2 个（任何未来提交者都必须调）；
> ③契约上就该单调用点 2 个（`reportSessionDevice` 已有标志守卫 ⇒ 多调 no-op；`releaseAbandonedTargets` 挂在
> 帧边界上，而边界只有一处）。**处理 = 不改结构、改可发现性**：五个方法各写一条前置/幂等契约（顺序类 `@pre`：
> 编译早于本帧记录、结算晚于 `recordAndSubmit()+present()`；幂等类写明多调是 no-op）—— 比写"只被调用一次"
> 更稳（事实型断言会腐烂）。**并合并提交后那一对** `settleTransientPassVariants()` + `releaseParkedObjects()`
> → **`settleSubmittedFrame()`**（同一事件触发，合并后无法只做一半：早结算毁掉正在记录的帧、一帧推两次环会
> 早一帧释放）；名字 5→4，`submitFrame` 33 行。**先例警示**：`Impl::parkTargetObjects`（单调用点 helper 方案被
> 否决、函数体删了，声明+相反结论的注释又活好几批，§38 才清）⇒ 单调用点模式的真实代价是**静默腐烂**。
> **反向观察**：长得像但**保证不同**的规则不该合并（`releaseRenderTarget` 丢消费者槽**每槽一次计数等待** vs
> 重建路径**不能等**）—— 这种"看似可复用"比单调用点 helper 更值得警惕。验收同前（证据逐字节相同 / VUID 0 /
> FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **`submitFrame` 拆成帧协议（设计 §42）**：130 → **35** 行。五个具名步骤（`VsgRenderer` 私有、无
> 参数）：`releaseAbandonedTargets()`（宿主没打招呼丢掉的目标，表持有所有权因此再也查不到它）、
> `reportSessionDevice()`（首次提交记录驱动）、`compilePendingViews()`（D22 增量 + `VINE_VSG_DISABLE_INCREMENTAL_COMPILE`
> 逃逸 + 失败回落全图）、`settleTransientPassVariants()`（一次性变体**提交之后**才换回稳态）、
> `releaseParkedObjects()`（`kRetireRingDepth` 帧前停放对象：各内容槽桥环 + 渲染器自己的环）。`submitFrame`
> 现在就是协议本身，顺序一眼可见。**行数账**：函数 130→35，但 TU 981→960、头文件 +55（五段理由搬进声明处
> Doxygen，与该私有区既有风格一致）；收益是"不被理由淹没"而非总行数。**踩坑**：第一次编译 clang **崩溃**
> （frontend exit 135，栈顶指向一个注释里的 "annotation token"）—— 重跑同一条命令即通过 ⇒ 编译器瞬时崩溃
> （并行编译内存压力），不是代码问题；但**崩溃重跑干净后仍要跑完整判据**才认通过。验收同前（证据逐字节相同 /
> VUID 0 / FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **`buildOffscreenTarget` 拆分（设计 §41）**：范式不是"计划/施工"（其决定 `resolveDepthBorrow()` 早
> 已抽出且会报告），而是**按"一个 builder 拥有什么"命名**：①`Impl::resetTargetAttachments(t)` —— **列清一次
> build 拥有的全部东西**（三张槽表 / 颜色+深度图像与视图 / `passes` / `attachments_built`、`depth_seeded`、
> `any_load_pass`、`color_seeded`、`depth_sampleable`、`depth_borrow_pending_reported` / 借用源+视图+barrier /
> `graph` / 三套 depth 策略 shader set / 宽高 / `build_key`）；这份清单就是**所有权边界**，`Target` 加字段忘了
> 重置会变成残留图像/残留 built 标志而无人报错。②`Impl::dropConsumersSampling(target)` —— "重建方换了新视图，
> 而消费者的过期检查只看源**尺寸**" ⇒ 同尺寸重建后消费者仍采样旧视图；消费者靠槽属性 `source_target` 找，摘除
> 走 `detachSlotView()`。③`Impl::createTargetAttachments(...)` —— 图像+视图（含 usage 标志规则：深度没有
> `TRANSFER_SRC` 连 `TRANSFER_SRC_OPTIMAL` 都到不了；必须走 `createImageView()` 否则 framebuffer 带脏句柄只在
> `vkCmdBeginRenderPass` 崩）。`buildOffscreenTarget` 202 → **89** 行，TU 1410→1399。**踩坑**：`targets` 表以
> 非 const `RenderTarget*` 为键 ⇒ helper 的 `depth_src` 形参不能是 `const`（`map::find` 报 lose const
> qualifier）；中途误加的 `if (targets.empty()) return;` 会提前返回整个函数、静默跳过 barrier/build key/log —— **加守卫前先想清楚它会不会跳过后续步骤**。验收同前 + 全量重建。

> 2026-09-12 **`passGraph` 决定/施工分家（设计 §40b，承 §40）**：新增 `Impl::PassPlan`（值）+ `Impl::planPass(t,
> key, target) const`（纯决策无副作用），两段决策理由（load-op 策略、提升状态）搬进它的文档；`passGraph` 只剩
> 取材 → 施工，且 `has_color`/`has_depth` 中间变量删掉（统一读 `plan.*`）。**这一步的全部风险是取值顺序**，已写成
> `planPass` 的契约：`current` 指向目标的 pass 表 ⇒ 必须在 `publishPass` 之前取；`planPassVariant` 读的
> `any_load_pass`/`depth_seeded`/`color_seeded` 正是发布新对象会改的 ⇒ 计划必须先于撤销提升与发布。**踩坑**：
> `PassObjects` 是 `Target` 嵌套类型 ⇒ `PassPlan` 里写 `const Target::PassObjects*`（否则 `unknown type name`）；
> `reconcileOffscreenOrder()` 是 `VsgRenderer` 成员，`Impl` 调不到 ⇒ 重排仍留在 `passGraph`。结果 `passGraph`
> **270 → 164** 行（两批合计 −39%），TU 1425→1410；`planPass` 58 / `makePassGraph` 30 / `reuseSteadyPass` 18 /
> `publishPass` 22 行。验收同前（证据逐字节相同 / VUID 0 / FAIL 0 / test_vsg 100 / test_graphics 158 / 门禁 PASS）。

> 2026-09-12 **`passGraph` 拆分（设计 §40）**：先数职责：`passGraph` 270 行 = **9 个职责**，其中约 110 行
> 是决策理由注释。拆出三步（各步独立验证）：①`Impl::makePassGraph(t, has_depth, clear_color)` —— 一个 pass
> 一个 RenderGraph，**清屏值按附件顺序**（颜色在前、深度最后；attachment 0 是本 pass 清屏色，额外 MRT 保持
> 透明黑，深度项是目标的深度清屏值）；②`Impl::reuseSteadyPass(...)` —— 稳态帧全部开销（`passVariantIsStale`
> + 清屏值更新），null = 清屏策略变了要重建，"clear value 0 只在真有颜色附件时是颜色项"（否则是 union 里的
> 深度值）随之成为其文档；③`Impl::publishPass(t, key, objects, has_color)` —— 记录入库 + 目标级不变量
> （`color_seeded`/`depth_seeded`/`any_load_pass`/`depth_sampleable`）。**踩坑**：`reconcileOffscreenOrder()`
> 是 `VsgRenderer` 成员（命令图只有它有），`Impl` 没有外层 `this` ⇒ **`Impl` helper 不能调它**，"顺序变更→
> 重排""新图入图→重排"留在 `passGraph`。结果 270 → **210** 行。**下一步（未做，风险更高）**：把"决定"整体
> 打成 `Impl::PassPlan`（`planPass(t,key,target)`）并把两段决策理由搬进其文档，预计再降到 ~135 行；风险点是
> `current` 指针与 `planPassVariant` 读的目标级标志必须在"发布"改写它们之前取值，顺序错会静默改掉 load-op。

> 2026-09-12 **可维护性整理（设计 §39；一条规则一处）**：①`SceneBridgePipeline.cpp` 里 `hashCombine`
> 的混合式抄了三份、顶点布局哈希抄了两份 ⇒ 合为一个 `hashCombine()` + `vertexLayoutHash()`（**模板**：
> `VertexChannel` 是 `SceneBridge` 私有嵌套类型，文件内自由函数不能命名它）+ 两个具名种子；**三份哈希不同步
> 不会报错，只会让缓存不再命中**（每帧重编译，无诊断）。同批给 MRT 两条规则命名：
> `colourAttachmentCount(shader_set)` 与 `applyOpaqueBlendForAttachments(states, n)`（G-buffer 必须不混合
> 写入：法线附件 alpha≈shininess/256，混合会把它缩到 12.5%）。`buildStateGroup` 216→**179**。
> ②`SceneBridgeGeometry.cpp`：通道形状检查（1..4 分量 / 整除 / 顶点数）原先写两遍（调用处 report 一遍、
> `makeTypedVertexData` 内再守一遍）⇒ `channelShape()` 判一次 + `ignoredChannelMessage()` 一处出消息 +
> 前置条件声明；`buildGeometryData` 225→**202**。③**缺陷实测**：loc1 法线被拒的报告用三元选格式，却按
> 第一条分支的顺序传参 ⇒ 第二条分支打印互换的数字（`%zu` 读 32 位值）——**编译通过、运行通过、验证层干净，
> 只是消息在撒谎**。修法：两分支各自出消息。**排查手段**：写了参数计数检查器（三元格式按**分支**核对，
> 否则这种"顺序错"看不见），插件 14 文件 **0 命中** ⇒ 该缺陷类只剩这一处；检查器入库
> `scripts/check_diagnostic_formats.py`（有怀疑退出码 1）。④`VsgRenderer::initialize` 的 38 行窗口 traits
> 构造搬成 `makeWindowTraits(host_handle)`，137→**104**。**有意不做**：两处"退化法线保持零"的写法没合并
> （倒数乘 vs `vsg::normalize` 除法，合并会改最后一位比特，而像素在证据行里）。验收同前 + 全量重建。
> 三个 TU：641→700 / 473→539 / 965→981（Doxygen 比省下的代码长，收益是单点定义）。

> 2026-09-12 **结构整理七（设计 §38；只改结构）**：`buildOffscreenTarget`（重建分支）与
> `releaseRenderTarget`（释放分支）各自写了一遍同一件**破坏性拆解**（摘 pass 图 → 等设备 →
> 逐个 content slot `bridge.clearCache()` + 摘出编译队列）⇒ 合成 `Impl::unhookTargetPasses(Target&)`，
> 两处各调一次；"为什么这里必须等待"（clearCache 释放共享对象注册表，停放实测报
> `vkDestroyPipeline-00765`）的推理也随之下沉到这一处。另外：`buildOffscreenTarget` 里**第四份**
> "槽的 view 记在哪个图"（局部 lambda + 只有 program 槽用的 `forget_view`）删除，改走既有
> `Impl::detachSlotView()`（顺带消掉一处死 `erase`：只有 content 槽排队编译）；深度共享 barrier
> 抽成 `Impl::makeDepthShareBarrier(source)`（含 combined depth/stencil 必须覆盖两个 aspect 的
> `VUID-VkImageMemoryBarrier-image-03320` 规则），字段 `Target::depth_share_barrier` 由
> `ref_ptr<Node>` 收紧为 `ref_ptr<PipelineBarrier>`；删掉死声明 `Impl::parkTargetObjects`（第十批
> 否决"停放"后残留，注释还与实测结论相反）。行数：`buildOffscreenTarget` 272→**234**、
> `releaseRenderTarget` 266→**253**、TU 1460→1423（另一 TU +21）。等价性有断言兜底：`policy churn:`
> 第二段每帧翻附件形态 ⇒ 每帧走重建 teardown，并断言"等待数 = 重建数"。验收同前 + **全量重建**
> （发现并修掉 `tests/test_vsg/ProgramSamplingTest.cpp` 第 46 行的外来残留 `}-10/2=`，编译错误）。

> 2026-09-12 **结构整理六（设计 §37；只改结构）**：`renderContentSlot`（内容槽的每帧热路径）
> 223 → **120** 行。四个文件内 helper：`updateSlotViewport`（presenting 填满目标 / 否则 pass
> 子视口 / 无子视口填满）、`seedSlotLight`（presenting 角色翻转时重置默认光 —— 方向光会把
> gizmo 从斜角照黑）、`beginLightsDroppedEpisode`（“宣告的灯全被丢掉”是**场景**属性而非帧属性
> ⇒ 每段只报一次，一旦有可用灯或本帧无灯立即重新武装；helper 只回答“现在要不要报”，真正的
> `reportFailure` 留在调用方 —— §31 那条“helper 不能持有 renderer 状态”的延伸）、
> `logContentSlotDiagnostics`（env 门控的 TEMP 诊断 `VINE_VSG_DIAG_MRT` 移出热路径；**它被两份
> 设计文档引用 ⇒ 不删，只搬**）。**踩坑**：helper 不能收 `ContentSlotRequest`（`Impl` 的嵌套
> 类型，文件内自由函数里不可命名）⇒ 改为传字段
> `(target, depth_mode, order, commands, created, root_children, variants)`。TU 547→550
> （含新 helper 的 Doxygen）。主流程现在读作“守卫 → 建槽/取图 → 重挂 → 重放属性 → 视口 →
> 相机/灯 → 同步命令 + 入队编译”七步。验收同前（`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 +
> test_vsg 100 / test_graphics 158 + 门禁 PASS）。

> 2026-09-12 **结构整理五（设计 §36；只改结构）**：两个 overlay 函数“节点造好之后”的部分也逐字重复
> （建视图 → 记入槽 camera/view/ready → 按 order 摆放 → 离屏则 `reconcileOffscreenOrder()`），
> 以及“slot 被 retire 过 → 重新挂上”分支；另发现 `makeCompiledOverlayView()` 的 `what` 形参
> **从无使用者**（死参数，§35 同类）。抽出：`placeOverlayView(dest, view, order)`
> （摆放 + “离屏目标就要 reconcile”这条规则写一处）+ `template <class Slot> installOverlayView(...)`
> （建视图/编译→失败报一次并返回 false，调用方丢自己的槽；成功则记入槽并摆放）；删死参数。
> 模板而非重载：两种槽只用共同字段 camera/view/order/ready；声明放私有区（形参无 `Impl` 类型
> ⇒ 公开头可写），定义在本 TU（两个实例化点都在此）。行数：`drawScreenTexture` 194→**169**、
> `drawScreenProgram` 222→**199**（相对 §32 之前 226/247 降了 57/48），TU 724→705。
> 有意保留的差异：源校验（PiP attachment 钳位 / program 深度提升报告）、key 构造、矩形策略
> （PiP 自动贴右下 / program 只钳位）、节点工厂与 `V_LOGI` 文案 —— 再合并只会把差异藏进参数。
> 验收同前：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS。

> 2026-09-12 **重构残留检查（设计 §35）**：§33 把一次性提交收进 `Impl::submitOneShot()` 后留下两处
> **死代码**（`readColorBuffer` 的 `queue_family`、`readDepthBuffer` 的 `physical`）—— 无编译错，但已无用户。
> 更值得记的是**守卫的位置**：原来两个函数各自判 `device/physical` 空才提交；提交搬进 `submitOneShot()`
> 后，这个判空就与**真正解引用设备的地方**分家了 ⇒ 守卫应该跟着使用者走：`submitOneShot()` 改成
> `[[nodiscard]] bool`（自判 `window`/`getDevice`/`getPhysicalDevice`），调用方只在不可用时
> `return false`（colour 仍需 `device` 查格式属性、`source` 判空；depth 只需 `device` 建 staging buffer）。
> 顺带修回同批里改错的一处：抽走提交语句时误删了紧邻的 `VkImageSubresource sub_resource{...}`
> （编译器只在真用到时报）。**流程结论**：每个“抽走一段逻辑”的批次收尾都要专门查三类残留 ——
> ①死局部量/死参数；②守卫与被守卫的使用是否还在一起；③被搬走语句**紧邻**的声明是否被带走。
> 验收：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS
> （且 `-Wunused-result` 证明两个调用点都消费了返回值）。

> 2026-09-12 **结构整理四（设计 §34；只改结构）**：`makeScreenTextureNode`（PiP）与
> `makeFullscreenProgramNode`（用户全屏程序）在设备眼里是**同一件东西**（全屏三角形 + 深度关 +
> 不混合 + overlay 视口 + set0 采样纹理 + 运行时编译的 shader）—— 配方里有三段逐字重复：
> ①`ShaderCompiler` 可用性→stage 创建→编译→`ShaderSet{vs,fs}`+`makeOverlayPipelineStates`；
> ②`config->init()`→`copyTo(StateGroup, SharedObjects{})`→push constant（仅 program）→`Draw(3,1,0,0)`。
> 抽出 `makeOverlayShaderSet(vs, fs, entry, extent, failure)` + `makeOverlayStateGroup(config, push_data)`
> （push_data 为 null 即 PiP 那种），两工厂只剩差异（描述符/纹理/采样器/push 范围）。
> 行数：`makeScreenTextureNode` 73→**51**、`makeFullscreenProgramNode` 182→**159**。
> **诚实说明**：本批**没有**减少总行数（新 helper 的 Doxygen 比省下的代码长，TU 758→780）——
> 收益是**单点定义**：“overlay drawable 的配方”只有一份，两个 pass 不会漂移（同理 §32）。
> 验收同前：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS。

> 2026-09-12 **结构整理三（设计 §33；只改结构）**：①**两个回读函数的公共前奏去重**
> （`readColorBuffer` 134→122、`readDepthBuffer` 123→113）：抽 `Impl::readbackTarget()`
> （会话/`attachments_built`/尺寸，**故意不等设备** —— 调用方先格式检查再付等待）、
> `Impl::hostVisibleMemory()`（“回读落地内存必须 HOST_VISIBLE|HOST_COHERENT”写一处；colour 落
> LINEAR 图像 / depth 落 staging buffer）、`Impl::submitOneShot()`（队列+fence+具名超时常量
> `kReadbackTimeoutNs`，原先两处各写 `100000000000` 且无解释）。
> ②**设备等待一律可数**：`shutdown()` / `readColorBuffer` / `readDepthBuffer` 原先直接
> `viewer->deviceWaitIdle()`，绕过了第十批引入的计数 ⇒ 全部改走 `Impl::waitForIdle()`；
> 现在 `grep deviceWaitIdle` 在插件里只剩该函数体本身，`policy churn:` 的“0 次设备等待”
> 断言覆盖了全部等待入口。验收同前：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 +
> test_vsg 100 / test_graphics 158 + 门禁 PASS。
> **仍剩**：`vsg_selftest/main.cpp`（~4600 行）拆 TU 需要**整段重写**数千行（工具链下代价高），
> 已记录待专门一轮；两个 overlay 函数尾部可模板化收敛。

> 2026-09-12 **结构整理续（设计 §32；只改结构，等价性判据同 §31：`[selftest]` 行逐字节相同 + VUID 0/FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS）**：①两个 overlay 函数头部的 55 行重复
> （viewport → 源校验 → **目的目标解析** → surf 尺寸 → retargetPass → passGraph）抽成
> `VsgRenderer::OverlayDestination` + `resolveOverlayDestination(source, key, what)`；
> 报错消息用 `formatDiagnostic(u8"%s: ...", what)` 保持逐字节不变。收益不只是行数：
> **“overlay 画到哪”的规则（反馈环 / 无附件 / 离屏重建）现在只有一份**。
> `drawScreenTexture` 226→**194**、`drawScreenProgram` 247→**222**。
> ②`reconcileOffscreenOrder` 215→**21** 行：一个值 `Impl::RecordPlan`（window_graph / graphs_of /
> present / order）+ 三个阶段 `fillRecordPlan`（收集，跳过 retired pass；同目标按显式 pass order）/`orderRecordPlan`（采样边 + 深度借用边 + `stableTopologicalOrder`）/`applyRecordPlan`
> （重挂 children + 借用方 barrier 在其源最后一图后 + 窗口图最后）。
> **过程教训**：大函数重构时**只替换头部会留下旧函数体**（编译期暴露）；拆函数要整段替换，
> 且 oldString 锚点要选在**两版真正不同的行**上（新旧 `pass_records` 注释几乎相同，只差折行与
> `Impl::Target&`/`Target&`）。**下一步候选**：`vsg_selftest/main.cpp` 4603 行按主题拆 TU +
> 共享 helper 头（判据现成：输出逐字节相同）；两个 overlay 函数尾部（建视图 → 记 camera/view/
> ready → placeViewByOrder → reconcile → 日志）可模板化收敛。

> 2026-09-12 **结构整理（设计 §31，只改结构、行为契约不动）**：判据是**机械重构等价性** ——
> 前后各跑一次独立 selftest，`diff` 全部 `[selftest]` 行**逐字节相同**（实测相同）+ VUID 0 / FAIL 0 +
> test_vsg 100 / test_graphics 158 + 门禁 PASS。①**三张槽表（content/screen/program）的遍历收敛成
> 访问器**：`Target::forEachSlot/visitSlot(key)/hasSlot(key)/eraseSlot(kind,key)` + `SlotKind`，
> 4 处双/三循环各变一个循环；kind 差异用 `if constexpr (requires { slot.bridge; })` 就地表达；
> `Impl::slotGraph()` 取代三份"槽的记录图在哪"的 lambda，并把 `retireInactivePassSlots` 的
> "预扫描 + 真扫描"合成一次遍历。②**`passGraph` 387 → 271 行**：抽出 `passAttachments()`（含
> `PassAttachments`）、`makePassObjects()`、`depthStillPromoted()`、`revokeDepthPromotion()`
> （均在 `Impl`）+ `VsgRenderer::dropDepthSamplingProgramSlots()`。③**`buildOffscreenTarget`
> 317 → 257 行**：借用校验抽成 `VsgRenderer::resolveDepthBorrow()`。④删掉重构后失效的重复：
> `releaseRenderTarget` 的局部 `graph_of_slot`、`VsgBackendUtility` 的自由 `waitForIdle`。
> **结构约束（记住）**：公开头只前置声明 `struct Impl;` ⇒ 内部 helper **不能**在公开头写
> `Impl::Target&` 形参（incomplete type）；要么做成 `Impl` 成员（内部头声明），要么做成
> `VsgRenderer` 私有方法且形参不出现 `Impl` 类型；`Impl` 内声明要晚于 `Target` 定义。
> 下一步候选：`drawScreenTexture`(226) 与 `drawScreenProgram`(247) 共享骨架（dest/矩形/stale/建视图/
> 摆放），`reconcileOffscreenOrder`(209) 三事混一。

> 2026-09-12 **方向登记：合并每 pass 的 `beginRenderPass`（dynamic rendering）被上游卡住（设计 §9.4）**：
> 一个 render pass 对象只能烧死一种 load-op 组合 —— 这就是"每 pass 一个 render pass"的唯一理由，
> 也是变体工厂 / "变体必须兼容"的子通道依赖约束 / 一次性 transient 变体 + 帧末换回 / D49 重建
> 这整批机制存在的原因。若能走 dynamic rendering（`vkCmdBeginRendering`：attachments 内联、load-op
> 变逐 pass 取值、布局转换改显式 barrier），这些**大部分可删**（`planPassVariant()` 的**决策**留下，
> selftest 相位作为行为契约不动）。**当前走不通**（vsg 1.1.16 实测）：①全树无
> `vkCmdBeginRendering`/`VkRenderingInfo`（记录路径只有 `RenderGraph.cpp:150/170` 的 begin/end
> render pass）；②无任意命令的记录钩子（无 `CustomCommand` 一类）；③
> `GraphicsPipeline.cpp:233` 硬绑 `pipelineCreateInfo.renderPass`，`pNext = nullptr`
> ⇒ 产不出 dynamic-rendering 形态的管线。触发条件：上游支持（或推 PR）/ 记录开销或 tile GPU 成瓶颈；
> 第一步是"一个 target 一个作用域"的 spike，判据复用 `clear flip:` + `policy churn:` + VUID 0。
> 收益性质是**简化**而非桌面性能（begin/end 在桌面/离屏开销很小）。

> 2026-09-12 **停放的边界：非破坏性停、破坏性等（第十批）**：（1）`SceneBridge::invalidateState()`
> **本来就**把状态包装停放（`retireNode`）⇒ **depth 模式变更**那处调用方等待是多余的，去掉（反证：
> 改回 → `policy churn:` 报 **14 次**等待 / 15 帧，因为该相位每帧翻一次 depth 模式）。相位现在同时翻
> colour clear / depth clear / **depth 模式** / pass 是否公告，四种都是 **0 等待**。
> （2）同批尝试把 **target 重建 / 槽 teardown** 的等待也换成"停放 view" ⇒ **被实测否决**：那两条路径会
> `clearCache()`，清空共享对象注册表，那里的管线/采样器没有存活节点兜底 ⇒ lavapipe 报 **12 条**
> `VUID-vkDestroyPipeline-00765` / `vkDestroySampler-01082`（`VkPipeline ... in use by VkCommandBuffer`）。
> 于是改回计数等待并写明原因。**结论：非破坏性路径停放 (0 等待)，破坏性路径（碰 clearCache / 丢图像）
> 保留计数等待** —— 这就是界。
> （3）相位加第二段：每帧翻 target 附件形态（depth promotion 属 build key）→ 每帧重建，断言
> "重建 = frames-1、等待 = 重建" ⇒ 破坏性路径每帧恰好一次 teardown 等待。
> （4）顺带量到语义不对称：**pass 请求**（清屏策略）当帧生效，**target 描述**变更在**下一帧 start**
> 才被采纳（相位计数按实测写成 `frames - 1`）。
> 验收：test_vsg 100 / test_graphics 158 / 独立 selftest VUID 0 + FAIL 0 / 门禁 `RESULT: PASS`。

> 2026-09-12 **策略变化帧不再停设备（渲染器自己的退役环）**：变体重建、提升撤销级联、被丢弃的
> program 节点、"某个 pass 本帧不再公告"的视图摘除，原来都在帧装配期 `waitForIdle()`。现在被换下的
> render pass / framebuffer / 节点停放进渲染器自己的退役环（`Impl::retireObject()` /
> `advanceRetireRing()`，环深与推进点跟 §8.2 的节点环一致：`kRetireRingDepth = 4`，`submitFrame()`
> 提交之后推进）。判定标准仍是 §3：**破坏性销毁**（槽 teardown / target 重建 / `bridge.clearCache()` /
> depth 模式变更的状态重建）继续显式等待。**视图摘除那处等待本来就是多余的** —— 那条路径只把 view
> 从图上摘下、槽（view/节点/管线）全部保留，没有任何对象被销毁。
> 判据 `[selftest] policy churn:`（`runPolicyChurnStressPhase`）：15 帧里翻颜色清屏、深度清屏、
> pass 是否公告，断言 `deviceWaitCount()` 增量 **0**、退役环**确实释放**（`retiredObjectCount()` > 0）、
> target 构建数**恰好 2**、末帧像素/深度仍符合末帧策略。反证（实测）：改回等待 → 同 15 帧 **29 次**
> 设备等待 + 环零释放；只把视图摘除那处改回 → **7 次**。为可数，所有刻意等待都走 `Impl::waitForIdle()`。
> 仍未覆盖：depth 模式变更（`SceneBridge::invalidateState()` 丢状态包装）与 `clearCache()` 系列 ——
> 第一版相位翻了 depth 模式，每帧都撞上这处等待（实测确认），故相位固定 depth 模式并写明边界。
> 验收：test_vsg 100 / test_graphics 158 / 独立 selftest VUID 0 + FAIL 0 / 门禁 `RESULT: PASS`
> （新增 `require_evidence "^\[selftest\] policy churn:"`）。

> 2026-09-12 **D47 同帧残留窗口（撤销提升必须同时丢弃"真正绑定深度"的 program 槽）**：撤销提升发生在
> 正在组帧的那一帧里，而该帧更早建好的全屏 program 槽若**绑定了**源深度，其描述符声明的是"提升后"的
> `SHADER_READ_ONLY` —— 撤销（重建提升型 pass + 本 pass）却把图像留在附件布局 ⇒ 本帧记录过期描述符
> （`VUID-vkCmdDraw-imageLayout-00344`，每条 draw 一条，宿主无感）。判据先落地：`depth sample:` 相位
> 第 3 段 —— 采样深度的 program pass（order 2）**先建**，保留型 pass（order 1）**后到**，且 P(order 0)
> 已先跑 ⇒ 问题被孤立在"槽"上而不是 pass 自己的布局；修复前实测 1 条 VUID + 2 条 FAIL（无丢弃报告 /
> 目标被写成采到的 (6,6,6)）。
> 修法：撤销级联里 `removeGraphChild` + 删槽 + 一条 `dropped for this frame` 报告；宿主下一次
> `drawScreenProgram` 按新的可采样性重建（不需要深度照常画，需要深度走既有的
> `MissingDescriptorBinding` 拒绝）。**只丢真正绑定了深度的槽**：`programSamplesDepth()`（纯函数，
> ABI 的深度绑定号 = 颜色附件数；`ProgramSamplingTest` 4 例钉住）判定 —— 纯颜色 program 的管线布局里
> 没有深度采样器，本帧照常绘制、下一帧也不需要重建。
> 有意**没有**走"撤销延后一帧"（要同时改 `depth_still_promoted` 判据、install 时机与
> `depth_sampleable` 语义，牵动 D47 诊断 / 借用校验 / `readDepthBuffer` 的 barrier 推导；而且撤销
> pass 自己就把深度交回附件布局，槽照样会过期 ⇒ 丢弃这一步无论如何都要做）。
> 验收：test_vsg **100**（+4）/ test_graphics **158** / 独立 selftest VUID 0 + FAIL 0 / 门禁
> `RESULT: PASS`（新增 `require_evidence "dropped for this frame"`）。

> 2026-09-11 **颜色 bootstrap 必须只清一次，且它有判据（设计 §30 "D49 补"）**：新目标的颜色图是
> UNDEFINED，第一个 pass 必须清一次才能被后续 LOAD；这一次清必须是**一次性变体**（帧末换回稳态
> LOAD），否则它就变成"这个 pass 永远清颜色"、每帧抹掉同目标早先 pass 的东西。反证两半都实测：
> 拿掉一次性变体 → `color bootstrap:` 相位报"填充被抹掉"；首帧直接记稳态变体 → 6 行 VUID。
> 另：一个目标的 pass 按它们记录 order 依次 build 只是个**假设**（`depth_read_only` 依赖它），
> 反序渲染实测为干净（VUID 0 / FAIL 0，原因：先建的是保留型 pass，走 seed 分支），该反序场景已
> 永久留在 `preserved depth:` 相位里。
>
> 2026-09-11 **depth-only 目标的布局制度 + 描述符绑定必须拒不可用者（设计 §30）**：（1）depth-only
> 目标（shadow-map 形态）的深度**永远**收在 `SHADER_READ_ONLY`（它存在的意义就是这个），所以
> ①`clearDepth=false` 必须真的 LOAD（`makeDepthOnlyRenderPass()` 也收 `depth_initial`）；
> ②它的 `depth_sampleable` **不得**被 LOAD pass 撤销 —— `readDepthBuffer()` 就是拿这个标志当
> "图像当前布局"用，谎报就得到 oldLayout 不符的 barrier（反证实测 8 行 VUID）。稳态布局改用
> `steady_depth_layout` 表达（depth-only ⇒ SHADER_READ_ONLY，否则 ATTACHMENT）。判据：
> `depth only preserve:` 证据行。
> （2）**fragment 声明的描述符绑定若本 pass 提供不了，必须在建节点时拒掉并上报**
> （`ProgramNodeFailure::MissingDescriptorBinding`）：全屏 program 只能提供"颜色附件 0..n-1 + 深度（仅当
> 真可采样）"，否则建的管线 layout 缺该 bind，错的是每帧一条 VUID、宿主却一无所知。vsg 1.1.16 无
> shader 反射 ⇒ 绑定从源码扫（`declaredBindings()`）。这条也把 D47 变成**可观测**：真去采样
> `binding = N`（深度）的 program 相位 `depth sample:`（正面：采到深度；反面：被拒 + 目标不被写）。
>
> 2026-09-11 **pass 粒度的 render pass 变体可以运行期互换，但有三个硬条件（D49/D51，设计 §30）**：
> （1）**子通道依赖必须逐字段相同** —— 渲染通道兼容性只豁免 initial/final layout 与 load/store op，
> 依赖不同就 `VUID-vkCmdDrawIndexed-renderPass-02684`；两个工厂的 `ext_to_sub.srcAccessMask` 因此
> 统一成与 load-op 无关的常量。
> （2）**LOAD depth 的 pass 必须声明图像"真实所在"的布局**：UNDEFINED（要先 CLEAR seed）/
> ATTACHMENT / **SHADER_READ_ONLY**（上一帧的提升型 pass 留在可采样布局）—— 第三种漏了就是
> `VUID-vkCmdDraw-None-09600`。`makeDepthLoadRenderPass()` 的 `initial_clear` 因此升级为
> `VkImageLayout depth_initial`，三种变体互相兼容、可随时换；"提升还没撤销、本帧又没别的 pass 先跑"
> 的那一帧记**一次性变体**，帧末 `submitFrame()` 换回常驻变体（同 seed 机制，字段更名
> `render_pass_transient` / `transient`）。
> （3）**重建判据比"宿主请求"（want_color_clear / want_depth_clear），不能比 load-op** —— 同一帧一个
> pass 会被建两次（`setupContentSlot()` + `render()`），颜色 bootstrap 会让第二次算出不同 load-op
> ⇒ 一帧内重建成"对 UNDEFINED 图像做 LOAD"。颜色 bootstrap 也是一次性变体，否则它变成"这个 pass
> 永远清颜色"。判据：selftest `depth only:` / `clear flip:` 证据行（各自反证后必红）。
>
> 2026-09-11 **`SceneView::setScene` 必须把新场景送到两个消费者（D50，设计 §30）**：默认 window pass 的内容是**绑在 pass 上**的（`addPass(pass, content, order)`），而惰性创建的默认 `OrbitCameraManipulator` 持场景的 **raw_ptr**（拾取 / `fitToScreen`）⇒ 只换 `scene_` 会让 viewer 继续画旧场景、并留下悬垂指针。修法：`bindPassContent(window, scene_)` + `dynamic_cast<OrbitCameraManipulator*>` 后 `setScene()`（不新增成员）。判据：帧内 `Scene::contentCollectCount()`（替换后走 1 遍 / 被替换 0 遍）+ manipulator 的 `scene()`。

> 2026-09-11 **共享对象表必须跟着缓存驱逐走（D40，设计 §27）**：`config->copyTo(stateGroup, shared_objects_)` 是**登记即持有** ⇒ 变体条目被 FIFO 逐出/abandoned 后，去重表**仍然**抓着那些 pipeline，只增不减（直到槽 teardown）⇒ D16 修好的缓存上限形同虚设。修法：`releaseAbandonedCaches()` 在**有驱逐的那一帧**调 `shared_objects_->prune()`（vsg 的 `prune()` 就是本项目自己的规则：`referenceCount()==1` = 除了表没人要 ⇒ 删，在用的变体经缓存 bind 命令继续持有而被保留）；触发面覆盖**两条**路径：abandoned 清扫 + 插入点 FIFO 裁剪（`noteEviction()` 记账）⇒ 只在驱逐帧走表，摊销 O(1)/驱逐。判据：test_vsg `SharedObjectsTableIsPrunedOnEvictionFramesOnly`（65 程序 ⇒ 裁剪 ⇒ 必须 prune；64 个相同变体仍须塌成 1 个 pipeline；再画必定命中的程序 ⇒ 不得 prune）；反证：停 prune → 首条红。**诚实记录**："重建被逐出变体会重新计数"依赖条目所有者何时放手（retire 环），单个 sync 内不可确定性断言 ⇒ 未写。另：把 render pass 下沉到 pass 粒度的**分阶段计划**已写入设计 §28（含 6 条必须同时成立的不变量），未实施。

> 2026-09-11 **目标描述中途改变必须重建：构建指纹（D39，设计 §26）**：`buildOffscreenTarget` 把附件数量/格式、深度格式、深度提升标志烧进图像 + render pass + framebuffer，但重建谓词只逐项列了尺寸/深度策略/借用项 ⇒ **没列到的属性静默失效**：中途 `attachColor()` → 新附件永远不存在（`readColorBuffer(t,1)` 报 "out of range"，听着像不支持、其实是丢了请求）；中途 `setDepthPromotion(true)` → **变更帧重建 0 次**（反证实测），且 §23 借用校验读的是构建时烧下的 `depth_sampleable` ⇒ 继续放行借用 → 借用方把一个已按采样布局收尾的深度当附件挂上（§23 拦的错误从后门回来）。修法：`Target::BuildKey`（附件数 + 各格式 + 深度有无/格式 + 提升标志）构建末尾记录、重建重置块清空、重建谓词整把比较；尺寸/深度策略仍是独立项（借用校验要读已建尺寸、`releaseRenderTarget` 靠清零逼重建）。判据：selftest `runTargetDescriptionChangePhase` 两段（加附件 → 附件 1 必须读回透明黑 + 恰好 1 次重建；开提升 → 源 + 借用方恰好 2 次重建、借用被拒报 1 次、远面又能画出来）；反证：停掉指纹项 → 4 条断言同时红。

> 2026-09-11 **深度借用的两个隐性缺陷（D38，设计 §25）**：借来的深度是直接烧进帧缓冲的，所以源一重建（**同尺寸**！混合深度策略收敛、尺寸变化都会）就换掉它的深度图像，而借用方只在**自己**重建时才重新校验 → 帧缓冲继续测**没人再写的旧图**（深度静默冻结）。修法：`Target::depth_source_view` 记住烧进帧缓冲的那张源视图，`render()` 比较"源当前的 `depth_view` != 记录值"即重建（重跑 §23 借用校验，带墓碑守卫防诊断/重建循环）。**另一半**：`reconcileOffscreenOrder` 的依赖边只来自采样（PiP / 全屏 program），深度借用**不是边**，所以源重建后被追加到末尾 → 借用方记录在源**之前** → 整帧用上一帧的深度（静默 1 帧滞后）；现在 `depth_source` 也是一条边。判据：selftest `runDepthShareOrderPhase`（借用方用 `TestOnly` 只测不写，否则写入会污染共享图），源第 2 帧混合重建、第 3 帧画更近的面 → 从第 3 帧起必须帧帧被拒（实测 `AAA---`）；反证：停 `borrow_stale` → `AAAAAA` 报红，停依赖边**在当前实现下不可观测**（借用方的重建会重新追加到末尾，等价修好顺序）→ 该边作为不变量保留并记录在案。

> 2026-09-11 **`DepthMode::TestOnly` 的语义断言（设计 §24）**：两条内置半透明 pass（forward /
> deferred 的 `forward_transparent`）都用 TestOnly，但以前**只驱动、从没量过**。新阶段
> `runDepthTestOnlyPixelPhase`：不透明 pass 先写近面深度；半透明 pass（不 clear、TestOnly）按序
> 画 近/中/远 三个四边形 → 断言**中心必须是中间那个（蓝）** + **深度读回仍是不透明 pass 的值**。
> 这一条中心断言同时盖住两半错误：写深度 → 更近的绿色（先画）会赢；不测试 → 最后画的灰色
> （在不透明面之后）会赢（反证实测：TestOnly→蓝 (5,10,46)、改 TestAndWrite→绿 (5,41,10)、改
> Disabled→灰 (43,43,43)）。harness 要求 `depth testonly:` ≥1 行。

> 2026-09-11 **前端也有诊断通道了（D37，设计 graphics-render-pipeline §13）**：`RenderEngine` 以前只
> 转发宿主的 sink，自己不能上报，于是"`ScreenPass` 声明的输入一个都没解析到 → 什么都不画"**完全静默**
> （生产者被禁用/移除/改名/排在后面都触发）。现在 `reportEngineProblem()` 送同一宿主 sink +
> `engineDiagnosticCount()`，`resolvePassInputs` 在**全部落空**时 `ContentSkipped` 上报（含 pass 名与
> 输入名）；每 pass 只报一次、解析成功即**重新武装**、`removePass`/`clearPasses` 清理记录（否则新 pass
> 复用同地址会被旧记录噤声）。契约：声明的输入必须由本帧**更早**运行的 pass 发布（注册表每帧清空），
> 多名字=备选链，只要一个命中就不报。

> 2026-09-11 **深度借用的可用性校验（D36，设计 §23）**：`shareDepth` 只在源深度图"原样可用"时才能
> 成立。三种不可用情形分**两类命运**：**瞬时**（源本帧还没深度图，如预热顺序）→ 本帧用自己的深度渲染
> + **源一出现就重试借**（`render()` 重建谓词新增一项）+ 每段只报一次；**持久**（尺寸不同 / 源把深度
> `setDepthPromotion(true)` 成可采样）→ 墓碑报一次 + 回落自有深度。三类过去都静默且都画错：尺寸不同 →
> 非法帧缓冲（llvmpipe 上还"看起来正常"）；源提升过 → 每帧 `VUID-VkImageMemoryBarrier-oldLayout-01197`
> 且什么都没画。**坑**：第一版把三类都当持久 → app 预热阶段把借永久关掉（表面能跑，半透明内容再也测不到
> 不透明深度）；selftest 的"借生效后远面必须被拒"用例当场报红。契约：源**同尺寸** + `setDepthPromotion(false)`
> + 源先于借方建好（否则下一帧重试）。harness 要求 `depth borrow:` ≥1 行。

> 2026-09-11 **内容收集的帧内复用（D27，设计 graphics-render-pipeline §12）**：
> `RenderEngine::frame` 开局给每个场景 `Scene::setContentFrame(token)`（幂等），同一帧内**同视图**的
> 多个 pass 共享一次全树遍历；memo 键是 `(projection*view, eye, 内容版本)`——**不是相机地址**
> （同视图的另一相机共享、相机就地编辑 miss、**堆栈相机安全**：用 `intrusive_ptr` 持键会
> `free(): invalid pointer`）；失效 = 帧边界 + 场景自身变更（同值 setter 不失效）+ `invalidateContent()`；
> `token==0`（自己调 `collectRenderCommands`）时**完全不缓存** → 零行为变更；返回的是**副本**
> （pass 的 `programOverride` 不泄漏）。实测 debug/2000 节点：遍历 17.1 ms vs 复用 0.10 ms（**170×**）。
> 残留：**同一帧两个 pass 之间**直接改节点要下一帧生效。可观测：`Scene::contentCollectCount()` /
> `contentCollectReuseCount()`（test_graphics 151 → 156）。**部署教训**：改 SDK 类布局（给 `Scene`/
> `RenderEngine` 加成员）后必须整体刷新 `dist/lib/*.so*` + `dist/plugins/vine/*.so` + `dist/bin/Vine`，
> 只换两个文件会让其它插件按旧布局分配对象 → `free(): invalid pointer`。

> 2026-09-11 **同目标多 pass 的深度策略："最后一次请求赢"是缺陷，已修（设计 §22，D35）**。
> 不透明 pass 每帧清深度 + 半透明 pass 保留深度是真实多 pass 管线的标准写法，而 `clearDepth`
> 曾是**目标**属性 → 目标只烧一个 depth load-op → 第二个 pass 的 `clearDepth=false` **吞掉**了
> 第一个 pass 的每帧清深度 → 旧帧深度留着 → 移开的物体继续遮挡（ghosting，静默错画）。
> 修法：`clearDepth` 同时是 **pass 作用域属性**（`PassRequest::clear_depth`）；同一目标出现不同
> 请求 → `Target::depth_policy_mixed`（sticky）→ `Target::wantsDepthLoad()` 改用 LOAD pass，
> **要清的人自己在 view 里、自己绘制之前插 `ClearAttachments`**（值 = `Target::depth_clear_value`，
> 与 render pass 同源）；该命令必须在**独立于 bridge root 的 group** 里（bridge 会重建 root 子节点）
> 且位于 view 内（`vkCmdClearAttachments` 只能写在 render pass 实例里）。已知残留：冲突到**第二个
> 请求到达时**才发现，**首帧仍按旧策略**，次帧起两者都满足（粘性标志不震荡）；借用深度的目标
> （`depth_source != nullptr`）既不用 LOAD pass 也不自清。

> 2026-09-11 **深度 LOAD / 共享深度的语义断言（设计 §21）**：两条此前只靠"无 VUID"覆盖的语义
> 现在有回读断言。**`runDepthLoadPixelPhase`**：`clearDepth` 是**目标**的 pass 属性（一个目标
> 一个 depth load op，"最后一次 clear 请求"就是该目标的策略），所以"同一目标两个 pass、一个
> CLEAR 一个 LOAD"是**模型外用法**（实测必败）；支持的用法是单 pass + 深度**跨帧** LOAD：
> 阶段 1 画近面（0.0249）播种，阶段 2 换成远面（0.0166）→ 必须 0 蓝像素、中心保持该 pass
> 清屏色、深度**一字不变**、目标只构建 1 次。**`runSharedDepthPixelPhase`**：出借方画近面，
> 借用者（`shareDepth`，只清颜色）画远面必须**一个像素都不变**（拒绝），再画更近面必须赢
> （接受）—— 两半都要，否则"全拒绝"也能通过。判据力做过反证（出借方改 `Disabled` → 立刻报
> 256 蓝像素）。harness 现在要求 `depth load:` ≥1 与 `shared depth pixels:` ≥1 行证据。

> 2026-09-11 **缓存收口：一套骨架、四种缓存（设计 §20，D16 / D34）**：`SceneBridge` 的四个
> 缓存不再各写一套语义 —— 几何缓存（自持 + 600 帧窗，**无容量上限**）、`program_stages_`
> （64 FIFO）、`program_shader_sets_`（64 FIFO）、`variant_cache_`（256 FIFO，**两个键都自持**）
> 都走 `OwnedCache.hpp`（`OwnedCacheEntry` / 新增 `OwnedPairCacheEntry` + `keyReleased()` 唯一
> 定义处）；"超限整表清空"删除（D16），改为插入时 FIFO 修剪 + 每帧 `releaseAbandonedCaches()`。
> **D34（身份靠地址但不持地址）已修**：`Item::material/program` 改为自持；两个哈希键缓存自持
> 键对象（variant 同时持 program + material）。**注意**：多缓存共享同一对象时 `abandoned()`
> 只在其它缓存也放手后才成立（`eraseAbandoned` 回收的是链尾，链长由 FIFO 上限界定）。
> `OwnedCache.hpp` 已从 `src/` 挪到插件 `include/vine/vsg/`（成员类型要出现在 `SceneBridge.hpp`
> 里）。新增 `SceneBridgeCacheOwnershipTest`（4 个测试，含"模板在阶段条目被逐出后仍持 program"
> 这个判别性用例）。

> 2026-09-11 **SceneBridge 拆分（设计 §19）**：1571 行 → 四单元：`SceneBridge.cpp` 512（会话态 +
> `syncRenderCommands` + 保留态 `Item`）、`SceneBridgeGeometry.cpp` 473（`buildGeometryData` +
> 顶点打包 helper）、`SceneBridgePipeline.cpp` 620（`getProgramShaderSet` / `buildStateGroup` +
> 编译/变体 helper）、`SceneBridgeInternals.hpp` 44（两 TU 唯一共享的 `VariantEntry`，内部头不
> 安装）。规则：**匿名 helper 只留在唯一使用者的 TU**（22 个 helper 里只有 `VariantEntry` 共享）
> → 不升级成公开接口。纯搬运：非空行多重集只差 include / 命名空间 / 内部头前言，零代码改写。
> include 按"符号驱动 + 编译验证"收窄（同步净减 1、几何净减 20、管线净减 12），判据是**插件 +
> selftest + test_vsg 三份编译命令同时通过**（三目标 include 上下文不同，只验一份会漏）。
> `test_vsg` / `vsg_backend_selftest` 的源列表已同步（MODULE 不能链接插件）。验收：67 + 151 全绿、
> 门禁 `RESULT: PASS`（0 VUID，六组证据齐全）。

> 2026-09-11 **深度回读 + 直接深度断言（设计 §18，顺带两个真缺陷）**：`VsgRenderer::readDepthBuffer`
> 落地 —— 只读无歧义格式（D32_SFLOAT / D16_UNORM），**打包 D24 诚实返回 false**；做法
> `vkCmdCopyImageToBuffer` → 宿主可见 buffer（深度不能 blit），拷完转回原布局。断言直接读深度值：
> 同一四边形 4 单位 vs 6 单位 → `near=0.0249 > far=0.0166 > 清屏 0`（比值 == 距离比，反 Z 的
> `z ≈ near/d`），`Disabled` 时中心仍 0（写入侧也关）。
> 顺带抓到的缺陷：**(A)** 离屏目标表条目**不自持** `RenderTarget` → 宿主销毁目标后同地址新目标
> **继承死目标的附件**（断言第一次跑就撞上：第二个 D32 目标读回打包 D24）；修法照搬缓存骨架
> （`Target::owner` + `Impl::entryFor()` + `submitFrame()` 回收 `useCount()<=1`）。
> **(B)** 深度图缺 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`（连转到 TRANSFER_SRC 都非法，
> `VUID-vkCmdCopyImageToBuffer-srcImage-00186`）——**只在开了 `VINE_VSG_DEBUG_LAYER=1` 的门禁里
> 才暴露**，本地跑的时候没开：本地验证必须与门禁同环境。
> harness 分别要求像素 ≥4 / 深度 ≥1 / MRT ≥2 行证据。

> 2026-09-11 **像素断言铺开到其余路径（设计 §17）**：`vsg_backend_selftest` 现在断言 PiP blit
> （子矩形中心=生产者内容、内缘=生产者清屏色、矩形外=消费者清屏色、**变化像素数恰好等于矩形
> 面积**）、deferred 全屏程序（整张目标填满程序输出）、**深度顺序**（近红远蓝按"画家算法会画错
> 的顺序"提交：开深度测试时蓝色像素必须为 0）、**深度模式权威**（`DepthMode::Disabled` 时中心
> 必须变蓝）、MRT 附件 0 必须收到几何。门禁打印逐条证据并**要求 ≥5 行 `[selftest] pixels:`**
> （断言被删掉不能读作通过）。
> 量到一条此前只写在注释里的规则：**MRT 附件 ≥1 一律清成透明黑**（附件 0 才拿 `clear()` 颜色）
> ——对采样额外附件的消费者（deferred 读 G-buffer 法线）可见，已登记 D31 待决策。
> 辅助设施：`PixelImage` / `readTarget(…, attachment)` / `driveContentPass` / `makeVisibleQuad(half, z)`。

> 2026-09-11 **反 Z 陷阱（归因实验结论，设计 §16）**：本后端 reverse-Z（近→NDC 1、远→0、
> 深度清 0、`COMPARE_OP_GREATER`）。**用户程序自己写 `gl_Position` 时若写 `z = 0`（非反 Z
> 直觉的"近平面"）实际落在远平面，与清屏深度相等 → 严格 greater 拒绝全部片元 → 绘制对象
> 完全消失，零 VUID、零诊断**。selftest 的变体探针（同四边形单变量）常驻打印
> `covered=0`（z=0）vs `covered=5916`（z=0.5）；`runPixelReadbackPhase` 用 z=0.5 的用户
> 程序断言该路径确实光栅化（`covered ≥ 1000`、中心 == (255,51,51)）。顺带补口：
> `assignArray` 未命中且着色器声明过该绑定时上报；`bindGraphicsPipeline == nullptr` 不再
> 静默记录。

> 2026-09-11 **像素回读 + 像素断言（D20 正面修补）**：`VsgRenderer::readColorBuffer` 落地
> （离屏 RGBA8：blit 到线性宿主可见图 + 按 rowPitch 收成紧凑 RGBA8；float 附件诚实返回 false
> + ContentSkipped；同步语义先 `deviceWaitIdle`，源图 SHADER_READ_ONLY↔TRANSFER_SRC 双向屏障）。
> selftest 新增 `runPixelReadbackPhase`（中心像素=被光照红四边形、角像素=清屏色、alpha=255、
> RGBA16F 返回 false），harness 把 `[selftest] FAIL` 当硬失败。**旧盲点**：原有辅助几何占 0 像素
> （裁剪空间 x 全相同 / Phong 通路下与世界视线共面），所以过去"无 VUID"从未证明光栅化。
> 后端在**首次 submit** 打印 `[VsgRenderer] device: ...`（vsg 懒创建 device，initialize 期间
> 拿不到），harness 显示为 `[info] Vulkan device:`。本机只有 llvmpipe（无 GPU 驱动），真机冒烟
> 仍未做。设计 §15。

## 后端契约（规范性在 `RenderBackend.hpp` 类注释；预算见设计文档 §14）
- **调用序**：`beginFrame` → 每启用 pass（order 升序）`beginPass`→`setPassOrder`→可选逐
  pass 状态→绘制→`endPass` → `endFrame` → `swapBuffers`（**唯一 present 点**）。首帧前有
  warm-up（启用且非清屏的 pass 先各跑一遍），所以"首帧前创建的东西"也要能摆对位置。
- **借用**：camera / commands / lights / target / program 只在当次调用内有效（commands 是本
  帧临时量），`beginPass` 的 pass 只在作用域内有效；**不得保留**，需要就拷贝/上传。
- **保留**：保留 GPU 状态必须按被服务对象寿命定键、由对应 `release*` 释放、**不随帧数增长**；
  宿主不必为正确性调 `release*`。
- **线程**：串行、不可重入、无需加锁；不得假设跨 `initialize`/`shutdown` 同线程；**诊断 sink
  是同步回调**，只能记录返回，不得回调后端。
- **失败**：接口内不抛异常；`false` ≠ 部分生效；**`initialize()` 返回 false 必须自己收拾残局**
  （引擎只在 initialize 成功后才调 `shutdown()`，见 `RenderEngine::shutdown`）。
- **保留预算**（本后端数字）：退役环 4（提交帧）、几何复用窗 600 帧、材质 256 条、
  变体/ShaderSet 超限整表清空、pass/target 槽靠 `releasePass`/`releaseRenderTarget`。

> 2026-09-11 **后端模块拆分（结构，行为零变更）**：`VsgRenderer.cpp` 3603 -> 901 行，
> 按职责拆成 `VsgRendererPasses/Targets/Overlay.cpp` + `VsgRendererImpl.hpp`（会话态）
> + `VsgPipelineFactory.{hpp,cpp}`（纯工厂，`vine::vsg::detail`）+ `VsgBackendUtility.*`
> （图手术/设备同步/策略）。置放规则：纯工厂只依赖显式参数、只返回失败原因；会话态改
> `VsgRendererImpl.hpp`；跨 TU 自由函数进 `detail`（各 TU `using namespace detail;`）。
> 顺手修正漂移的文档注释与 `LightPushBlock` 的编译期断言位置。设计 §13。
> 注意：`test_vsg` 与 `vsg_backend_selftest` 直接编译插件源码，加/删 .cpp 必须同步其
> 源列表。

> 2026-09-11 **后端缓存骨架
 + 材质缓存修复（D13/D19）**：新增
> `src/plugins/gfx_backend_vsg/src/OwnedCache.hpp` —— 保留型缓存统一"条目自持键对象
> （地址不可能在存活期内被复用）+ `eraseAbandoned()`（`useCount()<=1` ⇒ 除缓存无人能再查到
> ⇒ 立即回收）+ `trimToCapacity()`（FIFO，永不动 null 键默认条目）"两半不变量。
> `VsgMaterialManager` 切到该骨架：条目自持 `Material`（修掉同地址复用旧 Phong 值/descriptor）、
> `releaseAbandoned()` 由 `VsgRenderer::submitFrame()` 每提交帧调、`kMaxEntries = 256` FIFO 兜底；
> 刷新路径收敛为 `updateMaterial()` 唯一入口（`Entry` 记上次 `PhongParameters`，相等即不写/不 dirty/
> 不传输 —— 此前该接口全仓零调用点，SceneBridge 自己又写了一遍 = D19）。设计 §12、register D13/D19、
> 测试 `tests/test_vsg/MaterialManagerTest.cpp`（6 例，含"地址唯一性"确定性断言）。

> 2026-09-11 **pass 协议显式化**：7 个 `pending_*` 字段收敛为单一 `PassRequest`（`VsgRenderer::Impl`）
> + 显式作用域（`pass_open`）；**作用域属性**（pass/target/order/depth_mode/presenting）在作用域内
> 每次绘制都有效、`endPass()` 丢弃；**每次绘制属性**（viewport/lights）由紧随的绘制调用消费；
> `resetPassRequest()` 只有一句 `request = PassRequest{}`（新字段不会漏清）。违反协议（嵌套
> `beginPass` / 不配对 `endPass`）上报为 `DiagnosticCategory::PassProtocolViolation`（Warning）；
> 直连驱动（无作用域，自检/legacy 键）行为不变，`isPassScopeOpen()` 可断言状态。
> 改这条协议前先读设计文档 §11；测试见 `tests/test_vsg/PassProtocolTest.cpp`（**无需设备**：
> 测试目标已编入 `VsgRenderer.cpp`/`CameraBridge.cpp`）。

> 2026-09-11 **后端诊断通道（失败不再静默）**：新增 `vine/graphics/RenderDiagnostic.hpp`
> （`DiagnosticSeverity` / `DiagnosticCategory`（枚举、按后果分类）/ `RenderDiagnostic` /
> `DiagnosticSink`）+ `RenderBackend::setDiagnosticSink/diagnosticSink/diagnosticCount(category)`
> 与 `protected reportDiagnostic`（默认实现齐全 → 现有后端零改动）；`RenderEngine::setDiagnosticSink`
> 是宿主入口（引擎保存并应用到当前与之后的后端）。**vsg 后端：`VsgRenderer::reportFailure` 是唯一上报
> 权威**（stderr 追踪 + 计数器 + 宿主 sink），槽内 `SceneBridge` 经 `installDiagnosticRoute` 把发现
> 转发给它（保证 `diagnosticCount()` 诚实、无双份追踪）；自由函数 helper 改为**返回失败原因**由调用方
> 上报。已接线：几何拒绝 / 通道丢弃 / 着色回退（原 D9 全静默）/ 编译失败 / 离屏 target 失败 / 内容跳过 /
> 初始化失败；上报频次按 **revision**（不刷屏）。宿主侧 `RenderControl` 把诊断写进 `vine/logging`。
> 验证：test_graphics 151 / test_vsg 58 / `vsg_backend_selftest::runDiagnosticsPhase()`（真实设备上
> 1 条 GeometryRejected + 1 条 ShaderFallback 到达 sink）/ lavapipe 门禁 PASS。详见设计文档 §10。

> 2026-09-11 **状态一致性 / 资源生命周期终检**（详见 `.ai/design/vsg-pass-lifecycle.md` §8）：
> (1) **保留缓存的键必须指向活对象**：`SceneBridge` 的 `cache_`/`rejected_`/`program_stages_`
> 原按裸指针索引且不自持 → 对象销毁后地址复用会把死条目的保留状态（旧网格 / 旧 SPIR-V /
> 旧拒绝记录）喂给新对象（静默错误）。现条目**自持**所索引的几何/program，并把拒绝记录
> 合并进 `Item`；几何被 app 放弃（`useCount()==1`）时立即回收，仍被引用才走 600 帧复用
> 窗口。(2) **退役环**：活路径上被替换的保留节点（数据/状态包装/条目驱逐）先进
> `SceneBridge::retireNode()`，由 `advanceRetireRing()` 在每个**已提交**帧后推进，环深 4
> （=命令槽 3+1）→ 可能的槽已重新录制（其 fence 已等）后才能销毁，避免
> `VUID-vkDestroyPipeline-00765`/`vkDestroyBuffer-*` 类的在飞销毁。(3) **`Group::addChild`
> 拒绝成环**（祖先链检查，静默拒绝），否则递归遍历栈溢出并破坏包围盒缓存前提。
> 复查通过：单线程（无 thread/锁）、`shutdown()` 先 `deviceWaitIdle` 再整体重建、
> `clearCache()` 5 处调用点均先 `waitForIdle`。仍未做：D13（材质缓存无逐出 + 同地址
> 复用风险）、跨 pass 命令缓存、`VkPipelineCache`（三项逐项设计登记见该文档 §9）。验证：test_graphics 150 /
> test_vsg 55 / lavapipe 门禁 RESULT: PASS（含新 churn phase）。

> 2026-09-11 **遍历热路径 + 顶点属性 stride**：`Scene::collectRenderCommands` 每 pass 每帧全树走，
> 原实现有四处算法级重复：(1) `Node::worldMatrix()` 递归 O(depth²)；(2) 遍历中每节点再调一次
> `worldMatrix()`（O(n·depth)）；(3) 每节点调 `boundingBox()`，容器又要 union 子树（O(n·depth)）；
> (4) 排序比较器内做两次 `modelMatrix * Point3d` + 开方（O(n log n) 次矩阵乘）。
> 现已改为：`worldMatrix()` 单次折叠 O(depth)（`localTransformMatrix()` 提为 public 供自顶向下累积）、
> 遍历把父矩阵当参数下传、`BoundsCache` 每趟每节点 bbox 只算一次（叶子调虚函数，容器 union 子节点
> —— 场景图是树所以成立）、排序前算一次平方距离并排指针（保持 stable 语义）。另加
> `Group::childrenRef()`（热路径 5 处不再拷贝 NodePtr 向量）。实测同一 debug 构建：3 层/1080 命令
> 23.9→7.3 ms，5 层/9720 命令 375.7→83.9 ms（3.3~4.5x）。
> **stride 缺陷（正确性）**：`AttributeBuffer::components` 就是 stride，但 `Geometry::localBounds/
> positionCount/normalCount` 与 `RayIntersection::meshOfGeometry` 都按每顶点 3 float 读 → vec4 位置通道
> 的 AABB 错误（错剔除 / 错 fitToScreen）、计数错误、拾取失效；新增 `stride()/vertexCount()/xyz(i)`
> 并全部改用它。回归测试：GraphicsTest 新增 12 个（含 `CountingGeometry/CountingGroup` 证明每叶子
> bbox 只问一次），共 148 个。详见 `.ai/design/vsg-pass-lifecycle.md` §6.1。

> 2026-09-11 **pass 生命周期（架构级）**：新增 `RenderBackend::beginPass(pass)/endPass()/releasePass(pass)`
> （默认空实现，向后兼容）；**后端保留状态的槽身份从 `(camera, order)` 改为 pass 指针**
> （`VsgRenderer::SlotKey`；直接驱动后端、不调 beginPass 的调用方仍走历史键）。引擎在每个 enabled pass
> 前 beginPass、后 endPass，removePass/clearPasses 额外调 releasePass。修复：禁用 pass 仍绘制（幽灵）、
> 运行期换 camera/RT 残留旧槽、槽 depth/presenting 首帧冻结、同 (camera,order) 两 pass 互相覆盖、
> pending 泄漏到下一 pass。另修：`shareDepth`+`clearDepth=false` 每帧重建（H4）、释放被借深度者留悬垂屏障（H5）、
> **本帧必 present**（beginFrame 已 acquire 交换帧图像，跳过 submit → `VUID-vkAcquireNextImageKHR-07783`）。
> **DepthMode 权威化**：`RenderCommand::depthExplicit` + `SceneBridge::setContentDepthMode` → TestOnly/Disabled
> 真正进管线（此前被 `applyRenderStateObjects` 覆盖=失效）。**铁律：先 `deviceWaitIdle` 再删被提交命令缓冲引用的
> 对象**（否则 `VUID-vkDestroyPipeline-00765`/`vkDestroySampler-01082`，validation 实测抓到）。
> 新诊断 `VsgRenderer::offscreenBuildCount()`（稳态不增长）。详见 `.ai/design/vsg-pass-lifecycle.md`。
> 验证：test_graphics 135 / test_vsg 53 / `scripts/gfx_lavapipe_check.sh` RESULT: PASS；
> `ISSUES.md` / `REVIEW_FINDINGS.md` 顶部已加“已全部解决”状态表（原正文勿重复实现）。

> **世界坐标系约定：Z-up**（robotics：X 前、Y 左、Z 上）。OrbitCameraManipulator 与全部
> app_shell demo 已按此转换（内容映射 Y-up `(x,y_h,z)` → Z-up `(x,z,y_h)`；demo 相机 up=`(0,0,1)`）。
> vsg/glTF 生态原生 Y-up，将来在加载边界转换；URDF 原生 Z-up。

> 2026-09-07 **Design C（RenderEngine 瘦身 + SceneView）**：相机/导航/内容都不再放 engine，引擎
> 纯调度器（零内容零相机）。新增 graphics 概念 `SceneView`（宿主无关，组合**借用** engine）：owns
> Camera + 内容 Scene（`scene()` 返回 owning `intrusive_ptr<Scene>`）+ 默认 OrbitCameraManipulator
> （懒创建）+ 尺寸/aspect 维护；内容一律 per-pass 显式绑定（`addPass(pass, content, order)`）。
> `ensureWindowPass()` 注册 order-0 窗口 pass（3 参显式绑 view scene）除非 engine 已有「携带该
> camera 且 RT==null」的 pass。RenderEngine 删除 `setMasterCamera/masterCamera`、
> `setCameraManipulator/cameraManipulator`、mouse/scroll/key 三路 `pushEvent`、Resize 里的相机
> aspect 维护，**以及 `scene_/setScene/scene()`**（默认内容）；`hasWindowPass()` 无参
> →`hasWindowPass(raw_ptr<Camera>)`（结构化查询）。RenderControl 持 engine+view，输入/尺寸走 view；
> AppShell/test_plugin 改用 `render_control->view()->camera()/scene()`，内容 pass 显式绑
> `view->scene()`（gbuf/deferred/multislot/builder.setContent）；RenderPipelineBuilder 需显式
> setCamera/setContent。CameraManipulator 基类新增虚 fitToScreen()/home()（Orbit override）。
> test_graphics 121/121（SceneViewTest×4），全量构建绿。详见 /memories/repo/vine-sceneview-engine.md
> 与本文件下方旧 Design B 条目（已过时）。**新 .cpp 进构建需重跑 cmake configure**。
> 2026-09-07 布局最终态：engine `resize(w,h)`（由 `pushEvent(ResizeEvent)` 改名，唯一 resize
> 入口；RenderControl 一行 `engine->resize(w,sh)` 紧接 `view->onSurfaceResized(w,sh)`）只做
> swapchain+frame_ctx；RenderTarget 只有 setSize；新增 `Viewport` 值对象由 RenderPass 持有（未设=
> 全幅）；resize 布局走 SceneView::addSurfaceLayout 回调注册（创建方显式，每 effect 自己
> setSize/setViewport）。test_graphics 124/124。
>
> 2026-09-08 **统一主窗管线 preset**：`RenderPipelineBuilder::build(PipelinePreset, PipelineOptions)`
> → `intrusive_ptr<Pipeline>`（新 `RenderPipeline.hpp/.cpp`，RefCounted）。`Forward`=order0 窗口场景 pass；
> `Deferred`=order-3 gbuffer(MRT albedo/normal+shininess/spec/view-pos+D24, 发布"GBuffer")+order0 全屏延迟
> 光照 ScreenPass（带 camera+绑 content 转发灯光）为窗口 pass；阴影变体 ForwardShadowed/DeferredShadowed
> =占位（暂同无阴影）。Deferred **默认自带临时 shader**（builder 公共静态 `defaultGbufferGeometryProgram()`/
> `defaultDeferredLightProgram()`）与 canonical G-buffer（`defaultGbufferTarget(w,h)`），调用方零 GLSL；options 的
> gbuffer/lighting program 为可选覆盖；缺 camera/content 才返回 null。**SceneView::ensureWindowPass
> 默认 viewer 也改走 build(Forward)**（同一套 recipe），不再手搓 pass；行为不变。test_graphics 128/128。
>
> 2026-09-07 **Scene 收敛为单根**：`Scene` 只持一个根 `Node`（空场景 `root()==nullptr`，不渲染）。
> 删除 `addNode/removeNode/nodes()`；新增 `setRoot(intrusive_ptr<Node>)/root()`；`clear()` 释根
> （灯不受影响，仍用 `clearLights()`）。`findNode/boundingBox/collectRenderCommands` 自单根遍历。
> 消费迁移：`addBox`/demo 改收 `Group*`（单恒等根 Group + `addChild`，删 removeNode 再包 StateNode
> 的写法改为直接 state 包 box 后 addChild 根）；AxisGizmo 3 根并入一个根 Group；RayIntersection
> 两遍历器改单根；test_graphics 增 `setIdentityRoot(Scene&)` helper、SceneTest 改 RootSetClearAndFind/
> RootlessSceneIsEmpty。世界矩阵/bbox/剔除/透明度/状态折叠语义不变（根 Group 恒等）——纯 API 形态
> 收敛，与 osg/vsg "一个 scene = 一棵根子树" 对齐（graphics-scene-graph.md §5）。
> 旧多 root 表述仍留在 graphics-design.md §3.4/§7（已标注过时）。
>
> 2026-09-04 **已知缺陷已登记**（26 项 D1–D26，分级 🔴/🟡/🟢）：存
> `src/plugins/gfx_backend_vsg/vine-to-vsg-data-flow.md` §13（内存审计：两侧引用
> 计数、无环、真泄漏风险低；主要风险 = 只增不减留存 + 行为缺陷。🔴：D13 材质缓存
> 无逐出且 `releaseMaterial` 全仓零调用点；D10 `ShaderProgram` 无 revision → 改
> shader 不重编；D9 编译失败静默回退内建；D3 用户 loc6 顶点色被白覆盖；D1
> components 未当 stride）。
>
> 2026-09-04 **Material 透明度已移除 + 内建 ShaderSet 契约已归档**：透明只属
> scene/node(叶)/geometry 的 opacity（per-vertex alpha 通道 = vsg_Color.a@loc6），
> **Material 纯颜色**。删除 `Material::opacity()/setOpacity()`+`opacity_`（doc 注明
> diffuse alpha 恒 1 忽略）；`RenderCommand` ctor 不再用材质 opacity 播种（Scene
> 收集器重算）；Scene 有效透明度 = clamp(node_opacity)（叶 Geometry 自身即 node，
> 材质项移除）；vsg `VsgRenderer` no-cull 收集器去 `material_opacity` 项；材质默认
> 灰 Phong 在 `VsgMaterialManager` 里 force `diffuse.a=1`。测试改用叶/节点透明度：
> `CollectCommandsSortsTransparentBackToFrontAfterOpaque` 用 MatrixTransform
> setOpacity(0.5)，`CollectCommandsOpacityIncludesMaterial`→改名 `…LeafAndNode`
> (叶 0.5×祖先 0.5=0.25)；MaterialTest 删 opacity 断言。**test_graphics 113/113、
> test_vsg 10/10、全量构建、lavapipe 回归全绿**。材质颜色类型保持 `Colorf`(vec4)
> 不改 vec3（全 SDK 统一 + vsg PhongMaterial/UBO 本就 vec4；opaque 约定放渲染映射
> 层 force alpha=1）。内建 ShaderSet 输入契约（attribute loc/format/define +
> descriptor set/binding + pc 128B + "按名查找+define 变体"机制，flat/phong/pbr 共用
> 一张表）已核对进 `.ai/design/vsg-custom-shader.md` §9（供过渡期与 P0 自写对照）。
>
> 2026-09-03 **lavapipe 像素级渲染验证通过（抓帧）**：`vsg_color_probe` 增 `VINE_PROBE_CAPTURE=out.ppm`
> 抓帧（照 vsgExamples/app/vsgscreenshot 读回：blit 上一帧 swapchain → 线性 R8G8B8A8 → map）。
> custom 模式抓帧像素**正确**：中心 (255,108,89)=sRGB(线性 1.0,0.15,0.1 珊瑚色三角形)、角落
> (149,149,149)=sRGB(clear 0.3)——即**运行期编译的用户 program 真实光栅化**。PPM→PNG 用
> `scripts/ppm2png.py`（纯 stdlib），图已人工确认（灰底珊瑚色三角）。注：抓帧 barrier 曾报
> swapchain 无 TRANSFER_SRC 的 VUID（lavapipe 仍出正确像素）；抓帧目前仅在 vsg_color_probe 用。
> 用法：`VINE_PROBE_MODE=custom VINE_PROBE_CAPTURE=out.ppm …/vsg_color_probe`。
>
> 2026-09-03 **SceneBridge program 接线已落地**：`buildGeometry` 增 program 参数——`cmd.program` 非空时
> `buildProgramShaderSet()`（ShaderCompiler 运行期编译 stages→SPIR-V，手搭 ShaderSet：vsg_Vertex loc0 +
> addPushConstantRange("pc",0,128)+继承默认管线状态），只喂位置数组、跳过 material 描述符/per-vertex
> opacity；失败自动回退内置（坏 program 不伤场景）。`Item` 重建键含 program。验证：临时 VINE_DEMO_PROGRAM
> 钩子（box_side 挂用户 VS/FS）lavapipe 首帧 `created=1`（独立管线）、无验证错误（钩子已移除）；
> test_vsg 10/10、test_graphics 113/113、lavapipe 默认回归 PASS。遗留：真机像素级验证、把 program demo
> 固化为可复现用例。
>
> 2026-09-03 **SceneBridge program 接线卡点已解**（vsgExamples 官方证据）：
> - vsg 每 drawable 由 `RecordTraversal.cpp` 自动 push **push constant "pc" = { mat4 projection;
>   mat4 modelView }（0..128B）**；自定义 ShaderSet 声明同构 GLSL 块即拿到 view/model（modelView 含
>   场景 MatrixTransform 累计矩阵）。官方模板：`/opt/opensrc/vsgExamples/examples/utils/
>   vsgcustomshaderset/custom_pbr.cpp`（addAttributeBinding(vsg_Vertex,loc0) + addPushConstantRange
>   ("pc",0,128) + 可选 ViewDependentStateBinding(VIEW set1)/lightData）。
> - `vsg_color_probe custom` 已升级为官方契约（VS 用 pc.projection*pc.modelView*vsg_Vertex），
>   lavapipe 20 帧无错误。**SceneBridge 接线 = 直接镜像**：program!=null → ShaderCompiler 编译 stages
>   → ShaderSet(vsg_Vertex loc0 + pc range + 复用默认管线状态) → assign 位置数组 → mapper 状态 →
>   config.init；Item 重建键加 program。缺 v1 最小 demo/真机像素验证。
>
> 2026-09-03 三稿评审（graphics-scene-graph/state/shader + 旧 graphics-design.md）：补📋评审核对/
> 过时横幅——正文=写作时设计（MatrixNode/Drawable/primitive/"Scene 只持 root"均历史表述，以顶部
> ⚠/📋为准）；Scene 实现为多 root（有意）；program 槽 SDK 侧全落地、后端接线进行中。
> **SceneBridge program 接线卡点（关键）**：vsg 内置 phong ShaderSet 是**序列化 blob**（无文本），
> 且 vsg 内核/ViewDependentState 无"model/视图矩阵"标准注入名 ⇒ 无法安全镜像"自定义着色器如何拿到
> view/model"。下一步应先 `vsg_shader_dump` 打印 phong 的 stages/attributeBindings/descriptorBindings/
> pushConstantRanges 以逆向契约，勿盲改 SceneBridge。
>
> 2026-09-03 program 槽 P1 · 自定义 ShaderSet 装配探针（vsg_color_probe `VINE_PROBE_MODE=custom`）：
> 运行期 `vsg::ShaderCompiler`(glslang) 编译用户 VS/FS GLSL→SPIR-V → **手搭 `ShaderSet`**（1 个
> vsg_Vertex attributeBinding loc0、无 descriptor/push）→ `GraphicsPipelineConfigurator` 成管线 →
> BindVertexBuffers+Draw，直接 clip 空间输出（z=0.5、关深度）。lavapipe+验证层 20 帧**无错误**；
> 已纳入 `scripts/gfx_lavapipe_check.sh`（4 段含 custom）回归 PASS。这证明"装配半环"的最小契约可行。
> 剩余：把该机制接进 `SceneBridge::buildGeometry`（遇 `cmd.program` 建 ShaderSet 并泛化数组/描述符
> 绑定；视图矩阵需经 vsg ViewDependentState 绑定，需真机像素验证——勿盲改）；Item 重建键纳入 program。
>
> 2026-09-03 program 槽 P1 · glslang 运行期编译验证：`tests/test_vsg/GlslCompileTest.cpp`
> （+2）——`vsg::ShaderCompiler::supported()==true`，SDK `ShaderProgram` 的 VS/FS GLSL 逐 stage 经
> ShaderCompiler 成功编出 SPIR-V（module->code 非空，72ms，纯 CPU 无需 device）。test_vsg 10/10。
> 这验证了 program 槽后端装配的"编译半环"；"装配半环"（按用户 Program 建 ShaderSet（stages/
> attributeBindings/descriptorBindings）+ SceneBridge buildGeometry 泛化 + Item 重建键含 program +
> lavapipe/真机像素验证）仍待做——注意 vsg 的 phong ShaderSet 是序列化 blob（shaders/phong_ShaderSet.cpp
> 为 io.read_cast 数据），自定义需手搭 addDescriptorBinding(…, coordinateSpace) 等，视图矩阵经
> ViewDependentState 绑定，需 GPU/像素验证，勿盲改。
>
> 2026-09-03 program 槽 P1 第二步（解析链 + 命令携带）落地：`StateNode` 增
> `setProgram/clearProgram/program()`（子树级着色覆盖，独立于 render-state）；新增自由函数
> `effectiveProgram(node)`（叶子 Geometry program 优先 → 最近祖先 StateNode program → null=默认）；
> `Scene::collectRenderCommands` 每叶把 `cmd.program` 折好；`RenderCommand` 增 `ShaderProgramPtr
> program`。test_graphics **113 tests 全绿**（+4），全工程构建过。遗留：vsg 后端按 cmd.program
> 建 ShaderSet 装配（ShaderCompiler 可用）+ Item 重建键含 program。
>
> 2026-09-03 program 槽 P1 第一步（SDK 类型 + 挂点）落地：`ShaderProgram.hpp/.cpp` 新增
> `ShaderStageType{Vertex/Fragment/Compute}` + `ShaderStage{type,source,entryPoint="main"}`
> + `ShaderProgram : Object+RefCounted`（name/addStage/stageCount/stage/stages）；`Geometry` 增
> `program()/setProgram()`（null=引擎默认，零回归）。test_graphics **109 tests 全绿**（+2），
> 全工程构建过。遗留：StateNode 级 program + 解析链、RenderCommand 携带、vsg 后端按用户 Program
> 建 ShaderSet 装配（ShaderCompiler 已可用）。
>
> 2026-09-03 **glslang 已集成**（vsg 运行期 GLSL→SPIR-V 可用）：
> - gfx_backend_vsg 在 VINE_USE_FETCHCONTENT 分支**改源码构建 vsg**（FetchContent v1.1.16，链系统
>   glslang-dev 16.2）→ `VSG_SUPPORTS_ShaderCompiler 1`（build/_deps/vsg-build/include/.../Version.h），
>   ShaderCompiler.cpp 已编译；无 glslang 的本地 `/opt/opensrc/VSG` 仅 VINE_USE_FETCHCONTENT=OFF 用。
> - 依赖注记：本 build 的 `FETCHCONTENT_FULLY_DISCONNECTED` 曾缓存 ON（网络禁用、依赖预取）——
>   vsg 首次拉取须 `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF -DFETCHCONTENT_UPDATES_DISCONNECTED=ON`；
>   vsg 拉取后与其余依赖一样进 _deps。graphics-shader.md 顶部 ⚠ 已把"仅离线"决策更新为
>   "运行期可用（默认）+ 离线/SPIR-V 兜底"。
> - 验证：test_vsg 8/8、test_graphics 107/107、lavapipe 回归脚本 PASS（新 vsg）。
>
> 2026-09-03 program 槽 · 编译约束决策（graphics-shader.md 修订）：本 vsg **无 glslang**（运行期
> GLSL 编译不可用、不引入运行期依赖）→ 定 **"作者 GLSL、运行 SPIR-V、离线编译"**：`ShaderProgram`
> 作者用 GLSL 源，运行交付 SPIR-V（字节/.spv）；SDK 提供 `compileGlslToSpirv` 薄辅助（调开发机
> `/usr/bin/glslangValidator`，缺工具报清晰错误）；后端按 `ShaderSet.stages/attributeBindings/
> descriptorBindings/defaultGraphicsPipelineStates` 自描述装配 + GraphicsPipelineConfigurator；
> `program()==nullptr`→内置默认零回归。下一步 P1：SDK ShaderStage/ShaderProgram/Param 类型 +
> Geometry.setProgram + 后端装配。
>
> 2026-09-03 lavapipe 真机验证（Mesa 软件 Vulkan，llvmpipe CPU，Vulkan 1.4.335 + Khronos validation）：
> ①`vsg_color_probe` raw/box/flat 各 10-20 帧——无验证错误；②Vine 主程序默认 demo 端到端（VsgRenderer+
> SceneBridge）：每帧 sync 稳定 rootChildren=5、changed=0，无验证错误/VUID——**默认映射==现状零回归**成立；
> ③临时给 box_side 套 StateNode(PolygonMode::Line + 自定义 blend SrcColor/OneMinusSrcColor) 后重跑：
> 首帧 `created=1`（状态变→重建独立管线），后续稳定，**无验证错误**——非默认 RenderStateMapper 路径经
> driver 验证通过（验证后钩子已移除，demo 复原）。**可复用回归脚本：`scripts/gfx_lavapipe_check.sh`**
> （跑 raw/box probe + Vine 默认 demo，抓验证层错误；env 覆盖帧数/秒数，`VINE_SKIP_APP=1` 跳过 app）。
> 运行方式：`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./bin/Vine`。遗留：像素级（winding/
> blend 视觉效果）仍需人工真机确认。
>
> 2026-09-03 后端 renderState 消费（vsg）落地：`include/vine/vsg/RenderStateMapper.hpp`（纯、device-free）
> `makeRenderStateObjects(resolved)->{DepthStencil,Rasterization,ColorBlend,InputAssembly}` +
> `applyRenderStateObjects(config,…)`；SceneBridge::buildGeometry 按 cmd.renderState 装配、Item 增
> render_state（状态变→重建）；tests/test_vsg/RenderStateMapperTest 6 用例全绿（直跑 8/8，全工程构建过）。
> **两个历史决策落地**：①深度——vsg 投影是 reverse-Z(1→0)，SDK CompareOp 为"closer/farther"距离语义
> ⇒ 边界反转映射（Less→VK GREATER…），默认=vsg GREATER=现状零回归；②Blend——per-vertex-alpha 使 alpha
> blend 必须常开，blend.enabled 只是"是否用自定义因子"，vsg 后端无法经 StateNode 关闭 alpha blend。
> 映射：cull→cullMode(CCW front,默认 NONE)、polygon→polygonMode、topology→InputAssembly.topology。
> 遗留：cull winding/blend 因子真机视觉验证（无 GPU）。test_vsg 现 8/8（+6 映射）。
>
> 2026-09-03 场景图 R1 补强：新增 `MatrixTransformTest`（5）与 `GroupTest`（1）+ Scene 嵌套 world
> modelMatrix + GeometryTest 拓扑分离用例，**test_graphics 107 tests 全绿**（新增 8）。
> `vertexCount` = 纯数据统计，不随 StateNode Topology 变化（有测试钉住）；`triangleCount` 已于
> 2026-09-04 移除（Geometry 数据面不保证是三角网格；geometry 模块 `Mesh::triangleCount()` 保留）。
>
> 2026-09-03 场景图 R1 核心落地（test_graphics 99 绿、全工程构建过、test_vsg 直跑 2/2 绿）：
> **`Node` 拆成基类**（name/visible/opacity/parent()/虚 boundingBox()/worldMatrix()；无 children/
> 无变换）；**`Group`** 承接 addChild/removeChild/children()/boundingBox=children 并集；
> **`MatrixTransform : Group`** 为唯一变换源（matrix/setMatrix，经 protected 虚 localTransformMatrix()
> 参与 Node::worldMatrix() 父链累积）；`StateNode : Group` 不变。**`Geometry : Node` 叶子**：
> material 并入（name/visible/opacity 继承自 Node），boundingBox = loc0 本地盒 × worldMatrix()。
> **`Drawable.hpp/.cpp` 已删**；`RenderCommand.drawable`→`RenderCommand.geometry`(GeometryPtr)。
> Scene::collect/findNode、RayIntersection 三遍历器、AxisGizmo、SceneBridge、VsgRenderer no-cull
> walker、app_shell::addBox、TestRenderLiveCommand、GraphicsTest 全部迁移；`makeTriangleNode` 现返
> MatrixTransform(子=Geometry)。bbox 语义=**世界空间**（叶子世界盒；Group/MT=子世界盒并集）。
> 关键易错：`Mat4d()` 默认=identity（无 ::identity()）；Mat4d×Point3d 需 include math/Transform3.hpp；
> intrusive_ptr 析构需完整类型→RenderCommand.hpp 直接 include Geometry.hpp；加/删 .cpp 要重跑 cmake。
> 基线无关失败：test_cppstd/test_runtime/test_system 与本重构无关；test_vsg 的 ctest SegFault 为既有
> 退出时序问题（直跑干净）。
> 遗留：program 槽、renderState 后端消费、MatrixTransform 测试用例补强（现靠 Scene/Node 用例覆盖）。
>
> 2026-09-03 场景图 R1（进行中，当前绿点）：`MatrixNode` 更名/落成 **`MatrixTransform`**（真实变换节点：
> matrix()/setMatrix/worldMatrix（父链累积）+ boundingBox override）；`Node::boundingBox()` 已虚化。
> 下一步（R1 核心）：`Node` 拆成基类（name/visible/opacity/parent + 虚 boundingBox，去掉 children/
> transform/drawables）+ `Group`(children) + `MatrixTransform`(matrix) 语义到位 + `Geometry : Node`
> 叶子上树（material 收进来）→ 删 `Drawable` 与 `Node::drawables` → `RenderCommand.drawable` 改
> GeometryPtr → `Scene::collect`/`RayIntersection` 改树遍历。消费者/测试迁移量大，逐步保绿。
>
> 2026-09-03 场景图 S1（类型体系就位）：新增 `Group`（children 容器基类）与 `MatrixNode`（变换节点），
> `StateNode` 从 `Node` 重挂到 `Group`；Node→Group→{MatrixNode,StateNode} 骨架建立（vsg/文档对齐），
> 行为零回归（99 用例绿）。⚠ 过渡：Node 仍自带 children/transform/drawables（旧 API 保留）；后续 S2
> 把变换收敛到 MatrixNode、Geometry 上树为叶子、顺势删 Drawable。新 .cpp 加入需重跑 cmake 配置
> （glob 才拾取）。
>
> 2026-09-03 Geometry 重构为**开放属性列表**：单一 `location→AttributeBuffer`（打包 float +
> components），`addBuffer(loc)` 为唯一写入口，数量不限（仅受后端 max-attribute）；不再有
> positions_/normals_ 固定成员；`setPositions/setNormals` 降级为便捷（写 loc0/loc1），`positions()/
> normals()` typed 访问器已移除 → `positionCount()/normalCount()`；约定 loc0=position（bbox/计数/
> 拾取）。SceneBridge（物化 loc0/loc1）与 RayIntersection（GeometryMesh 物化 loc0）已适配；
> `GeometryTest` 7/7、全仓 99 用例绿。索引仍独立一条（index buffer）。AttributeBuffer.data 与索引
> 均为 shared_ptr（可共享/按身份缓存）。
>
> 2026-09-03 Geometry 通用属性缓冲已落地：`AttributeBuffer{data(floats),components}` + `setBuffer(loc)`
> /clearBuffer/hasBuffer/buffer/bufferLocations（loc≥2 自定义通道，0/1 保留给类型化 positions/normals）；
> 供点云色/尺寸等自定义 shader 通道的数据模型，CPU 可测（GeometryTest 7/7）。渲染消费仍后置。
>
> 2026-09-03 拓扑归位修正：`Geometry` 移除 PrimitiveType（曾误放），改为 **渲染状态项 `Topology`**
> （默认 Triangles；StateNode set/clear；与 PolygonMode 分清：点云=Topology::Points，线框=
> PolygonMode::Line）——同一数据可换状态变三角/点/线框，不换几何（vsg/Vulkan 拓扑属管线）。
>
> 2026-09-03 Geometry 纯数据化完成：移除 `shape_`/`shape()`——Geometry 只存 buffers
> （positions/normals/indices + revision）；新增转换器 `geometryFromShape()`（**2026-09-11：**
> `setShape(Shape)` 已删除——与转换器重复、非 Mesh 形状（Sphere/BRep）静默清空几何且连带清掉
> 自定义 loc 通道、签名以 `intrusive_ptr` 暗示持有却不持有；就地（重）填用
> `setPositions/setNormals/setIndices`）；SceneBridge（缓存键改 revision、建几何读 buffers）与
> RayIntersection（meshOfGeometry）已切 buffers；bbox/计数全从 buffers。⚠ 语义变化：不再借用
> Shape 的 Aabb 缓存（测试改名 BoundingBoxComputedFromBuffers）。未做：通用 setBuffer(loc)/Buffer
> 容器（留点云/Geometry 叶子切片）。注：test_vsg 的 ctest 在进程退出期 SegFault（用例全 PASS 后、
> 直跑正常）——疑为插件卸载既有问题，与本改动无关，待另查。
> 2026-09-03 Geometry 数据面（additive）已落地：raw `positions`(loc0)/`indices`；无 Shape 时
> vertexCount/bbox 回退 positions；Shape 主路不变。⚠ 与 graphics-scene-graph.md 草图偏差：暂用
> 类型化 `setPositions`（loc0），未引入通用 `setBuffer(loc)`/Buffer 容器——留到 Geometry 变叶子/点云
> 切片再定。renderer 仍只消费 Shape 路（raw 数据面待后端切片）。
>
> 2026-09-03 State 切片已落地：`StateNode` + 状态项（Depth/Cull/Blend/PolygonMode/**Topology**）
> + 折叠函数（collect/resolve/effectiveRenderState）；`RenderCommand.renderState` 已由 collect 折叠
> 填入（Scene 集成 3 用例全绿）。未接后端变体键（下一步）。
>
> 2026-09-03 场景图重构设计（评审稿）：`graphics-scene-graph.md`（Node→Group/MatrixNode/StateNode，
> Geometry 叶子 Node，去 Drawable/Shape，loc0=position）、`graphics-state.md`（StateNode 子树状态/
> 继承）、`graphics-shader.md`（用户写 GLSL 薄接口，取代 vine-shader.md §11）。分阶段落地。
>
> 2026-09-03 方向确认（SDK 第一准则）：**用户必须能写 GLSL**；SDK 着色契约先于后端，vsg（乃至自写
> Vulkan）只是可替换实现；内置 ShaderPreset 与用户 Program 同一契约；多 pass 意义 = pass 级 Program +
> 命名产出槽。契约草案（声明式 VineFrame/VineDraw 块、属性 location 表、pass 槽）见
> `.ai/design/vine-shader.md` §11（push 矩阵机制标注待契约化修订）。
>
> 2026-09-03 shader 设计稿：自写 VS/FS + UBO ABI（push 矩阵 + set0 帧/光 + set1 材质；世界空间光照；
> FlatShaded=unlit flag；离线 SPIR-V 内嵌，无 glslang）。设计见 `.ai/design/vine-shader.md`（待 P0 落地）。
>
> 2026-09-03 更新：渲染后端(vsg)走向"自定义着色器路线"——语义层(材质/光照/阴影/着色)
> 全部归 graphics，vsg 仅保留工程层(窗口/交换链/命令/管线构建/录制)。设计见
> `.ai/design/vsg-custom-shader.md`。同时补齐 vsg 资源生命周期闭环：
> `RenderBackend::releaseOverlay/releaseRenderTarget`(默认空实现) +
> `RenderEngine` 删除点接线(removeOverlay/clearOverlays/removePass/clearPasses/shadow 剪枝) +
> `VsgRenderer` 摘除 overlay View / offscreen graph / PiP slot 并 deviceWaitIdle。
> 遗留：Material 缓存释放未接线、Scene 几何靠 600 帧懒驱逐、Object 销毁钩子(自动兜底)未做。
> 测试：GraphicsTest 82 全绿（17 套件）。
>
> 2026-09-03 二更（Design B）：RenderEngine **不再有 main pass**，也不再自动建 scene/camera/pass。
> 引擎空启动：`scene()` 为可选默认内容(未绑 content 的 pass 用它，可 null)；`masterCamera()` 为
> 可选的交互主相机（manipulator 驱动它，不设则无效）。pipeline 完全显式——所有 pass 进统一
> `passes_` 注册表按 order 升序执行 + overlays 最后；"窗口 pass"= camera==masterCamera 且
> RT==null 的注册 pass（约定 order 0）。旧 `setCamera/camera`→`setMasterCamera/masterCamera`，
> `setMainPass/mainPass` 已删。RenderControl(fw) 作为"普通 viewer 引导层"：ctor 注入默认
> Scene+masterCamera，init() 在 passCount()==0 时注册默认窗口 pass。
> **阴影子系统已整体移出 RenderEngine**（addShadowPass/runShadowPasses/ShadowSlot/auto castShadow
> 调度全删，82 测试）：阴影 depth pass 现在就是普通注册 pass（光相机 + depth-only RT + order<0 +
> content）；Light::castShadow/ShadowSettings 保留为语义标志。
> ⚠ **路线决策**：shadow **排到最后**实现——前置 = 自定义 shader(buildVineShaderSet P0/P1) + 多 pass
> 完全成熟；届时才消费 castShadow/ShadowSettings。此前不跑任何半成品阴影：已剥除 vsg 内建
> HardShadows 探针(buildLightNode) + shadow-state 诊断 + VINE_VSG_SEED_LIGHTS_ONCE，demo sun 不再
> castShadow(VINE_VSG_NO_SHADOW 已删)。App 冒烟 exit=124。
>
> 2026-09-03 三更（着色预置）：graphics 加语义枚举 `ShaderPreset{StandardPhong, FlatShaded, Pbr,
> ShadowedPhong}`（`ShaderPreset.hpp`），由 RenderEngine（渲染配置）持有并在 initialize 前转发
> 后端（`RenderBackend::setShaderPreset` 默认 no-op）。vsg 后端映射：StandardPhong→phong ShaderSet、
> FlatShaded→flat ShaderSet（二者 "material" 描述符都是 PhongMaterialValue，SceneBridge 材质路径
> 通用，已验证）；Pbr/ShadowedPhong **预留**（Pbr 需 PbrMaterialValue，shadow 排最后）→ 暂回落
> Phong。builder 不掺和（preset 是着色轴，非 pass 拓扑轴）。GraphicsTest 84 全绿（+1 转发测试）。
>
> 2026-09-04 **Overlay 类删除 + 单列表统一**：`Overlay`（sdk + addOverlay/removeOverlay/clearOverlays）
> 整体删除；`RenderEngine` 只留统一 `slots_` 有序 pass 列表，`Slot{pass,content,order}`。顶部/HUD 层 =
> 高 order 普通 pass；内容经 addPass 绑定；显隐=RenderPass::setEnabled；子视口重排=新
> RenderPass::onSurfaceResized（引擎 resize 对每个 pass 调）；相机跟随=新独立 `CameraMirror.hpp`
> （`MirrorMode` + `applyCameraMirror(dst,src,mode)`）。`AxisGizmo` 改 `: public RenderPass` 自包含
> HUD pass（owned camera_/content_，execute 先镜像再画自己内容）。`RenderBackend::releaseOverlay`
> →`releaseWindowLayer(Camera*)`；`RenderEngine` 新增 `hasWindowPass()`（RenderControl 自动主 pass
> 条件由 passCount()==0 改为 !hasWindowPass()，只加 HUD pass 不再挤掉主视图）。后端 Checkpoint1 已把
> 主/叠加视图统一为 `window_layers`(Camera* 键)；释放主层时清空别名 vsg_camera/vsg_scene。
> 测试：GraphicsTest 113 全绿（HudPassTest/CameraMirrorTest/AxisGizmoTest 新语义）；test_vsg 10 绿。
> 设计稿见 .ai/design/graphics-overlay.md。
>
> 2026-09-04 C6（vsg 后端 Target 统一 + 去绑定，见 .ai/design/vsg-target-unification.md）：
> C6.1 三桶(window_layers/offscreen/screen_slots)→单表 `targets[RenderTarget*]`（nullptr=窗口，行为等价）；
> C6.2 `create()` 无参（不再绑 Vine Scene/Camera），主层惰性创建，主/顶(HUD) 由 `clear()` 标记判定
> （清屏→depth-on 主层；否则 depth-off+ambient 顶部层），窗口 RenderGraph init 空建、层随 render 加入；
> C6.3a `RenderPass::setProgramOverride`（逐 pass 整帧换 program）；C6.3b 窗口层键 (camera, content slot)
> `WindowKey` + `RenderPass::contentSlot`/`setContentSlot` + `releaseWindowLayer(camera, slot)`，
> 同相机非零槽=各自保留层顺序叠画（复用窗口图多 View），槽0 行为不变。
> 单测：GraphicsTest 114 全绿；test_vsg 10 绿；全量构建 0。App 冒烟（C6.2b/C6.3b）用户已确认正常。
> 遗留：逐槽 depth 策略(≤/write-off)、slot>0 运行期 demo、gfx_backend_vsg.md §7/11/12 旧文改写、C6.4 离屏多槽。

**模块职责**：场景图管理、可视对象、相机视图、渲染抽象层

## 核心类关系

```
Object
  ├─ Drawable (可绘制对象基类)
  │   └─ Geometry (网格/BRep/基本体)
  ├─ Material (材质: 颜色、纹理、光泽)
  ├─ Scene (场景容器: 树形结构)
  └─ View (相机: 投影、变换)
```

## 主要 API

### Scene（场景）
- `addDrawable()` / `removeDrawable()` — 树形管理
- `findDrawable()` — 名称查询
- `boundingBox()` — 递归计算边界
- `collectRenderCommands()` — 收集渲染指令

### View（相机）
- 参数：eye, target, up, near/far, FOV, 宽高比
- `viewMatrix()` / `projectionMatrix()` — 矩阵计算
- `screenToWorldRay()` — 拾取射线

### Drawable（可绘制对象）
- `localTransform()` / `worldTransform()` — 层级变换
- `isVisible()` — 可见性
- `material()` — 材质绑定
- `boundingBox()` — 局部 AABB

### Geometry（几何体）
- buffers：`setPositions/setNormals/setIndices`（loc 0/1 + 索引）、`addBuffer(loc, ...)` 开放通道
- `geometryFromShape()` — `Shape` → Vertex data 的唯一转换入口（非 Mesh 返回 null）

### Material（材质）
- RGB 颜色：diffuse, specular, ambient
- 参数：shininess, opacity (透明度)
- 可选：纹理文件路径

## 边界框类型（Aabbd）

- `Scene/Node/Drawable/Geometry::boundingBox()` / `computeBoundingBox()` 返回
  `vine::math::Aabbd`（`Rect3<double>` 别名，见 `vine/math/Rect3.hpp`）。
  原来的 `vine::graphics::BoundingBox` 已移除。
- **语义**：`Rect3` 默认构造为零点在原点的合法零盒；累积式构建必须用
  `Aabbd::empty()`（反转哨兵）起步，再用 `expandBy(Point3/Vector3/Rect3)`。
  空盒 `isEmpty()==true`、`isValid()==false`。
- **注意**：`Rect3.hpp` 只前向声明 `Point3/Vector3`，`min()/max()/center()/size()`
  的调用方需自行 include `vine/math/Point3.hpp`/`Vector3.hpp`；
  `vine::math::Point3d` 别名仅定义于 `Point3.hpp`。

## 设计特点

✓ **引用计数**：所有核心类（含 `CameraManipulator`）继承 `RefCounted<T>`，用
  `intrusive_ptr` 管理
✓ **借用/所有权分界**：getter 返回与“不 retain”的借用入参用 `vine::raw_ptr<T>`；
  会 retain（存入 owning 字段/容器）的 setter/add 入参用 `intrusive_ptr<T>`
  （by value + std::move，如 `setMaterial`、`Node::addChild`、`setScene`）；
  引擎持有操纵器经 `setCameraManipulator(intrusive_ptr<...>)`，`cameraManipulator()` 返 `raw_ptr`
✓ **Pimpl**：数据隐藏，二进制兼容性
✓ **无环依赖**：只依赖 Core、Global、Geometry；不反向依赖
✓ **后端抽象**：`RenderBackend` 接口支持多个实现（OpenGL/Vulkan）
✓ **命名规范**：无 `get` 前缀，`is`/`has` 布尔前缀，`set` setter

## RenderEngine 有序场景通道管线（2026-09 落地）

- 每帧执行：`pre passes(order<0)` → `main pass(order 0)` → `post passes(order>0)`
  → `overlays(升序 zOrder)`。
- `addPass(intrusive_ptr<RenderPass>, int order)` / `removePass(raw_ptr)` /
  `clearPasses()` / `passCount()`；同 order 稳定按插入序。
- **内容关联由 Engine 管理**：`addPass(pass, content, order)` 绑定显式场景，
  `bindPassContent` 重绑、`contentOf` 查询；执行按 `slot.content ?: engine.scene_`
  解析 → `setScene()` 对未绑定 pass 是单点更新。`RenderPass` 不携带 Scene。
- `RenderPass`：view(Camera)=借用(raw_ptr)；**输出 RenderTarget=持有(intrusive_ptr)**
  （析构在 .cpp 出外联）；null target=backbuffer；clear/viewport 在 pass 上。
- Overlay（HUD）始终最后；不 addPass 时行为与旧版一致。
- 未来 light：光源挂 Scene；shadow map = 以光源相机渲染的 order<0 通道。
- 衔接/数据传递（设计）：pass 输出=RenderTarget 纹理；将来后处理用“命名产出槽”
  (publish/resolve) 由 Engine 连接；当前阶段只需顺序 + 各自输出 target。
- v2a 平台层(2026-09-03)：RenderTarget 增 hasColor/hasDepth/colorFormat/depthFormat/valid()；
  RenderBackend 增 supportsRenderTargets()+离屏契约。vsg 离屏 scaffold 已实现（color±depth +
  createRenderPass/Framebuffer/RenderGraph + SceneBridge 同步，编译通过，GPU 未验证；默认路径不变），
  采样/合成属 v3。
- v2b(2026-09-03)：FrameContext 骨架(dt/尺寸, engine.frame/Resize 填充, frameContext() 暴露)；
  vsg 离屏 resize 时 deviceWaitIdle+从 CommandGraph.children 摘旧图重建；app_shell 提供
  VINE_VSG_OFFSCREEN=1 离屏验证入口（默认关）。
- v3(2026-09-03, lavapipe 实测)：命名产出槽 publish/resolve 落地 —— RenderPass
  setOutputName/addInputName + resolveInputTextures + execute 改 virtual；新 ScreenPass(默认不清屏)；
  RenderBackend.drawScreenTexture(source)；Engine 每帧帧首清 outputs_ 注册表、逐 pass
  执行前 resolve 输入/执行后 publish 输出（public publish/resolve/unpublish）。vsg：离屏
  renderpass 用 color finalLayout=SHADER_READ_ONLY+external 读依赖（makeSampleableRenderPass），
  离屏图插 command_graph 队首先录；drawScreenTexture 内嵌 GLSL(ShaderCompiler) 全屏纹理三角作主
  render_graph 第二 View 画 PiP（超面自动缩锚右下）。教训：ShaderCompiler 链接要求 VS/FS 接口变量
  同名；新增 View compile 失败须先从 render_graph 摘下再弃。验证：VINE_VSG_OFFSCREEN=1 右下 PiP
  正确显示离屏四色方块(偏暗=仅环境光)；GraphicsTest 68 全过(新增 5 用例)。
- v4a(2026-09-03, lavapipe 实测)：光源归属定案 **Scene 级**(不与 pass/node 绑定；node 级留 v5 升级,
  scene->lights() 换实现即可)。新增 Light(Ambient/Directional, Colorf+intensity+castShadow 预留)/
  Scene 光槽；RenderBackend.setLights(no-op 默认) 由 RenderPass::execute 在 render() 前从内容 scene
  下发(空=保留后端默认 headlight/ambient, 有光=替换)；vsg 主视图手工化(RenderGraph::create(window,
  main_view)+main_light_group(默认 createHeadlight)+vsg_scene)以拿 View 句柄挂/换灯, setGroupLights 每帧
  reconcile 各视图灯(灯节点无 GPU 资源, record 时收进 lightData, 无需重编译)。demo(env 门控) 给 engine
  scene 配 ambient0.25+sun 方向光 → 主/离屏同源、PiP 颜色与主一致(不再偏暗)。教训：vine String 不能赋给
  vsg std::string name。GraphicsTest 73 全过(新增 Light/Scene/RenderPass 传光 5 用例)。
- v4b-1(2026-09-03, CPU+lavapipe)：阴影=Scene 级光的属性+按需。Light::ShadowSettings(res/bias/filter)。
  Engine runShadowPasses()(帧首扫 castShadow 方向光→光正交相机(AABB 取景)+仅深度 RT+内部 pass, 先于主
  管线; 空 AABB/无消费跳过; 帧末释放) + 手动 addShadowPass(light,content)(显式注册, 自动让位, 不必
  castShadow)。vsg renderOffscreenTarget 支持仅深度目标(makeDepthOnlyRenderPass: storeOp=STORE+
  finalLayout SHADER_READ_ONLY+external→fragment 读依赖); clearValues 逐附件手填(勿用 setClearValues,
  其按 finalLayout==DEPTH_STENCIL 判型会误判 SHADER_READ depth)。实测日志见 1024x1024 shadow target
  先于颜色离屏, 无崩溃; GraphicsTest 77 全过(+4)。v4b-2 采样(自建 shadowed Phong)待做。

## 实现计划

1. **框架** → 头文件定义 + 空实现
2. **场景管理** → 树形结构 + 变换层级
3. **视图管理** → 相机矩阵 + 投影
4. **集成几何体** → Geometry 包装 Shape
5. **渲染后端** → OpenGL/Vulkan 实现

