# Graphics 待办总表（唯一清单，2026-10-19 盘点）

> **读法**：本文件是 graphics 待办的**唯一入口**。每条只写"是什么 / 触发条件 / 权威出处"，论证与设计在出处
> 文档里，不在这里重复。**已关闭的条目不重新列出**，只在 §5 留一行"为什么不再算待办"，防止下一个 session
> 把做完的事再做一遍。
>
> **2026-10-19 盘点范围**：`.ai/design/*` 与 `.ai/memory/*` 的全部登记点，逐条对现行代码核实
> （不是照抄旧文档）。共移除/合并 **13 条**过时项（§5），补登 **5 条**本轮新发现的缺口 —— 这 5 条已
> 全部处理完：**3 条落地**（§1）、**2 条按设计裁决关闭**（§5.1）。
>
> 设计正文在 `graphics-image-source.md` / `graphics-abstraction-review-2026-10-19.md` /
> `graphics-layers.md` / `vsg-reimplementation.md`（后端登记）等处；本文件只做索引与裁决。

## 1. 已落地（索引；明细见附录 A）

| 项 | 落地时间 | 记录 |
|---|---|---|
| K1 `Pipeline` → `RenderPipeline` | 2026-10-19 | 附录 A #0（提交 `5eccace2`） |
| E2 pass 输入统一 `ImageInput` / `ImageSource` + 收尾 #1–#4 | 2026-10-19 | 附录 A #1–#4 |
| Cube 作 pass 输入（无需改后端） | 2026-10-19 | 附录 A #5 |
| H1 `Capabilities::Feature` + `supports()` | 2026-10-19 | review §1.3 |
| I1 `DepthView` 值 + pass 声明 depth 边 | 2026-10-19 | review §1.3 |
| L1 `OwnedFrame`（拥有式帧值） | 2026-10-19 | review §1.3 |
| J1 `InputRouter` / `LayoutParticipant` | 2026-10-19 | review §1.3 |
| T2 / M6 一设备 N 交换链 | 2026-10-18/19 | 由 S10b 覆盖，见 `graphics-window-device-viewer.md` §7 |
| 能力数值上限（`补登 #1`） | 2026-10-19 | `Capabilities::max_anisotropy` / `max_color_attachments`，见 review §1.3 |
| `DepthView` 的层（`补登 #2`） | 2026-10-19 | 视图带 `layer()`；layered 声明被**拒绝并报一次**（分层渲染本体仍是 §3.2 的 L2） |
| 同一 target 冲突的 depth 声明（`补登 #3`） | 2026-10-19 | 首个声明立住，后续不同声明被拒并每对 `(target, image)` 只报一次 |
| B2 `execute()` 不再是扩展点 | 2026-10-19 | 改为非虚：只留 `describe()` + `before_draw` 两个扩展点，见 review §1.4 |
| B1 per-attachment clear 色 | 2026-10-19 | `ClearPolicy::extra_colors` / `PassDesc::clear_extra_colors` / `RenderPass::setClearColor(index, c)`；后端 `core::ClearPolicy::extra_color_values` + `planClearValues`；判据：`CoreClearPlanTest`、`RenderPassTest.EachAttachmentClearColourTravelsWithThePassPolicy`、`VsgBackendTest.EachAttachmentClearsToItsOwnColour`（像素） |
| D1 会话侧“画面已落地” | 2026-10-19 | `FrameProgress::presented`（窗口会话的 present 计数，单调）。**精确含义**：交到平台 present 并成功，不等于“已上屏”；判据 `VsgBackendTest.TheFrameReportSaysHowManyFramesWereHandedToPresent` |
| E1 appfw 布尔 getter 命名 | 2026-10-19 | `Control::isEnabled/isVisible`、`RibbonAction::isEnabled/isCheckable/isChecked`、`RibbonButton::isCheckable/isChecked`；并配套补 Doxygen、`VN_APPFW_PLUGIN_ABI_VERSION` 7u→8u |
| F5 借用与托管混在一起 | 2026-10-19 | 新增 `vn::Lease<T>`（`base/core/sdk/vine/Lease.hpp`）+ `RenderEngine::lease()`；`SceneView` / `RenderPipeline` 借引擎改成存租约（不碰引用计数），`RenderPipelineBuilder` 那句 `intrusive_ptr<RenderEngine>(engine_)` 删除。判据：`LeaseTest` 5 条、`RenderPipelineBuilderTest.APipelineOutlivingItsEngineNeitherHoldsNorCallsIntoIt`、`SceneViewViewportTest.AViewWhoseEngineWasReleasedReportsIt`（原 `RebuildingAfterTheHostReleasedTheEngineIsSafe` 改写，见 `appfw-render-view.md` §13） |
| A4 `Material` 没有 revision | 2026-10-19 | `Material::revision()` 自带（每个 shading setter **先比后 ++**，名字不算）；后端 `ContentStore::updateMaterial` 从"逐命令建块 + 逐成员比"变成"比一个整数"，`materialBlockAgreesWith` 删除。判据：`MaterialTest.TheRevisionMovesWhenAShadingValueChanges`、`ContentStoreTest.ATouchWithNoEditChangesNothing`（5 帧反复赋同值：0 build / 0 park）、`AMaterialEditReplacesItsEntryAndParksTheValue`（编辑 ⇒ revision+1 ⇒ 一条被替换+被暂存） |
| A6 的可观测面 | 2026-10-20 | `DiagnosticCategory::ResourcePressure` + `MaterialImages::evictions()`（与 `releaseAbandoned` 严格分开）+ `VsgBackend::sweepAbandonedContent` 的水位/`ReportOnce` 一条句子。判据：`MaterialImagesTest.TheBoundDropsTheOldestEntryAndCountsOnlyThat`、`VsgBackendTest.AWorkingSetLargerThanTheTextureCacheIsReportedOncePerEpisode`（变异"去掉 `shouldReport()` 守卫" ⇒ 1/1 红）。**剩余只是淘汰键策略**（§3.6） |
| S1 / SK2 `RenderPass` 只存一份声明 | 2026-10-20 | 节点持有 `PassDesc desc_`，`declaration()` 读它、`setDesc()` 整体采纳它；`RenderEngine::Slot` 只剩运行期事实（order / transient / record_micros）；`addPass(PassDesc)` 的 25 行逐字段抄写退场；`PassDesc` 新增 `output`（值形式第一次能表达细粒度输出）。见 `graphics-skeleton-review-2026-10-20.md` §6 |
| S3 `Camera::setViewMatrixAsLookAt` → `lookAt` | 2026-10-20 | 74 处调用点（src / tests / docs）一次改完，与 S1 同批（一次公开变更窗口） |
| S2 `Texture` 只剩 layer 词汇 | 2026-10-20 | `layerCount()` / `layer(i)` / `hasLayer(i)` / `setLayer(i, image)`；`faceCount` / `source` / `hasSource` / `setSource` 退场，`CubeMap::setFaceImage` 收敛成它的命名糖。判据 `TextureLayersTest.OneNumberedSpellingWalksAnyKind`。见骨架复核 §9.1 |
| SK3 视图/链条内容不一致的诊断 | 2026-10-20 | `RenderPipeline::draws(scene)` + `SceneView::checkPipelineContent()`（`ReportOnce`，`setScene` 修好后 re-arm）。判据 `SceneViewTest.AChainBuiltForOtherContentIsReportedOnceAndIsANewEpisodeOnceTheWireIsRight`。见骨架复核 §9.2 |
| SK1 区归引擎所有 | 2026-10-20 | `RenderEngine::createSurface()` 是唯一创建入口，引擎持 `intrusive_ptr<RenderSurface> area_`，`RenderSurface` 构造转私有（`friend class RenderEngine`）；`adoptArea`/`releaseArea` 两个回指协议删除，「第二个区」变成构造失败（`createSurface()` 回 null + 报一次）。判据 `RenderSurfaceTest.ASecondAreaIsAConstructionFailureNotASecondObject` / `ARefusedSecondAreaCannotReshapeTheFrame`。见骨架复核 §10 |
| 概念改名：`PassDescription` → `PassDraw` | 2026-10-20 | 与 `PassDesc` 只差一个词的两个概念终于分开了；`FrameDescription::Pass::draw`、`OwnedPassDraw`、`PassDrawTest` 一并改名。见 `graphics-abstraction-review-2026-10-20.md` §1.1 |
| F1 `Scene` 收集只剩一个入口 | 2026-10-20 | `collectRenderCommands()` 返回共享不可变表，复制版与其 `Shared` 拼法一起退场；要复制就显式写 `*list`。111 处测试调用点迁移。见 `graphics-abstraction-review-2026-10-20.md` §6 |
| F2a 程序覆盖不再改写命令 | 2026-10-20 | `PassDraw::program_override` + 后端在**记录时**解析（`FrameRecorder::render` / `snapshotCommands`），命令表保持共享不可变；`OwnedPassDraw` 同步。判据 `FrameRecorderTest.ThePassProgramOverrideIsResolvedWhenTheDrawIsRecorded`。见 `graphics-abstraction-review-2026-10-20.md` §7 |
| F2b 内容记忆体跨帧存活 | 2026-10-20 | `Node::announceContentChange()` + **根子树级** content epoch（2026-10-22 收窄；之前是进程级，见 memory 同日"夜七"条）；`setContentFrame` 改为"老化上一帧没人用的条目"。稳态每帧收集 **8.7 ms → 36 µs**（2000 drawable，调试构建）。判据 `SceneContentCacheTest.EveryMutationThatChangesACommandEndsTheMemo`（11 种改动；它抓到 `Geometry::setMaterial/setProgram` 漏公告）。见 review §9 |
| `FrameContext` 删除（含没人读的 `dt`） | 2026-10-20 | 该类型只有一个非每帧事实（表面尺寸），`dt` 写了零个读者；改成 `RenderEngine::surfaceWidth()/surfaceHeight()`，`frame(dt)` → `frame()` |
| `Camera::setProjectionMatrixAsPerspective/Ortho` → `setPerspective`/`setOrthographic` | 2026-10-20 | 52 处调用点，与类内 `ProjectionType{Perspective, Orthographic}` 对齐（上一轮的 `lookAt` 是同一类问题） |
| F2b 颜色：空间与数值域进类型 | 2026-10-21 | `vn::Colorf` = 线性光、`vn::Color` = sRGB 编码字节，一对有名转换（sRGB 分段曲线，alpha 不编码）；SDK 作者颜色只剩线性浮点一种，`clear_color` / `ClearPolicy` / `FramePlan` 从 `Color` 改成 `Colorf`，后端不再 `/255`，默认清屏色写成 `Color(51,51,51).toLinear()`。修掉“清屏灰亮一个 gamma”（#333333 → #7C7C7C）。判据含设备级 `VsgBackendTest.AClearColourIsAStoredLinearValueNotAnEncodedOne`（UNORM 清 0.2159 读回 55，编码过会是 128）。见 `graphics-colour-space.md` |
| F2b 颜色：文本与打包字节序（§3.1 收口） | 2026-10-21 | `Color::toString()` / `Color::parse()`，**一种语法**（CSS hex：`#rgb[a]` / `#rrggbb[aa]`；短式逐位重复，写侧只有规范形式；alpha 对只在非不透明时写；失败在 `std::optional` 里）；按顺序命名的 `Color::fromArgb`（`0xAARRGGBB`）/ `Color::fromBgr`（`0x00BBGGRR`，`COLORREF`），无通用移位入口、无反方向。判据 8 条 `ColorTest.*`（含 `HexTextIsOneTextPerValueAndTheEncodedHalf`：线性 0.5 → `#bc0000`）。见 `graphics-colour-space.md` §9 |

## 2. 现在可做（无触发条件）

**空。** 盘点后没有任何一条"前提成立且今天就能做"的缺口——每条剩下的都缺一个真实消费方、一次真实负载，
或被上游卡住。这不是"没事可做"的托词：§3 里每条的触发条件都写成了**可观察的事实**（某个数字、某个调用点、
某台机器），命中就该做，未命中硬做只会增加第二套真相。

## 3. 触发条件型

### 3.1 SDK 抽象与类型

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| ~~F2b~~ **已落地 2026-10-21**（原行列作历史记录）：色彩空间与数值域未进类型 | `Colorf` 是线性光、`Color` 是 sRGB 编码字节，只由 `toLinear()` / `toColor()` 一对转换往来；SDK 的作者颜色只剩一种（`PassDesc::clear_color` / `ClearPolicy` / `FramePlan` 改成 `Colorf`，后端不再 `/255`）。原触发条件是“第二次同类缺陷”，**用户 2026-10-21 直接指定做** | ~~第二次同类色彩/数值域缺陷~~ 已收口 | `graphics-colour-space.md` |
| ~~颜色的**插值**~~ **已落地 2026-10-21**（用户判定“基础接口”；原行列作历史记录） | 落地形状：`static Colorf lerp(from, to, t)` —— 静态对称（跟 `Quaternion::slerp` 同形）、对通道原值插值（对 graphics 就是线性光下的 `mix`）、alpha 一起插、`t` 不夹紧；判据 `ColorfTest.LerpBlendsLightNotSamples`（黑白中点是 188 不是 128） | ~~第一处颜色动画 / 淡入淡出 / 主题过渡~~ 已收口 | `graphics-colour-space.md` §8 |
| ~~颜色的**十六进制字符串**读写（`#rrggbb[aa]`）~~ **已落地 2026-10-21**（用户点名做；短式同日按用户要求补上；原行列作历史记录） | 落地形状：`Color::toString()` + `static std::optional<Color> Color::parse(const String&)` —— **一种语法**（CSS hex：`#` + 3/4/6/8 位十六进制，大小写都收；短式逐位重复，`#CCC` = `#CCCCCC`），写侧只有规范形式（6/8 位、不透明不带 alpha 对），拒绝 `0x` 前缀 / 裸数字 / 2·5·7·9 位 / `rgba(...)` / 空白；文本是**编码那一半**（线性 0.5 写成 `#bc0000`）。判据 `ColorTest.HexText*` 四条 + `EverythingElseIsARefusedParseNotAGuess` | ~~第一次从外部文本读颜色~~ 已收口 | `graphics-colour-space.md` §9.1 |
| ~~`Color` 的**其他字节序**（`0xaarrggbb`、`0x00bbggrr`）~~ **已落地 2026-10-21**（同批；原行列作历史记录） | 落地形状：**带序的具名函数** —— `static constexpr Color fromArgb(0xAARRGGBB)` / `static constexpr Color fromBgr(0x00BBGGRR，高字节忽略、结果不透明)`；没有通用移位入口，也**没有** `toArgb32()`（反方向无读者）。判据 `ColorTest.TheOrderedFormsAreNotThePositionalOne` / `BgrIsColorRefOrderAndIgnoresItsHighByte` / `TheOrderedFormsAreUsableInConstantExpressions` | ~~第一次从持有那种 int 的 API 取值~~ 已收口 | `graphics-colour-space.md` §9.2 |

**本节已清空（2026-10-21）**：颜色那五件（类型分空间 / 反色 / 插值 / 常用预设 / 文本 / 字节序）都已落地，
剩下两条当时带触发条件的颜色登记也都收口了。反色（**2026-10-21，用户判定“基础接口”**）的一对拼法跟 math 类型一致
（`invert()` 就地 / `inverted()` 副本），`Colorf` 补线性光、`Color` 补编码样本，两者是两个数。
`Color::toRgba32()` 仍只有自己的用例在读，保留为打包 ctor 的自然另一半；要严格就一起删，别只加不减。

### 3.2 资源与目标

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| per-attachment clear 色 | （已落地 2026-10-19，见 §1）—— `ClearPolicy::extra_colors` 让每个附件拿自己的颜色，未写的仍为透明黑 | ~~G-buffer 需要按附件不同清色~~ 已收口 | `graphics-mrt-gbuffer.md` S4 剩余 |
| L2a / L2b 目标层 | 分层渲染（`layerCount` + pass 的 `first_layer` / `layer_count`）；`DepthView::layer()` 已在位并**拒绝**未支持的分层声明 | 点光立方体阴影 / CSM / VR 双眼 / 实例化被触发 | `graphics-layers.md` §6 |

### 3.3 内容、场景图与选择

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| 实例化 / 批处理 | 一条 `RenderCommand` 一个几何 | 同一几何 N 份（粒子、植被）；与 L2b 同源 | object-model §3 缺口 2 |
| LOD / 空间划分 | 无 LOD 节点、无八叉树 / 可见性集合 | 大场景“每帧收集本身成为瓶颈” | object-model §3 缺口 3 |
| 选择态（描边 / 框选 / 多选） | 拾取是机制（已有），选择不在框架 | 多处调用点各自重写同一套选择与高亮 | object-model §2.5 |
| 单图元便捷拼写（过渡） | `Geometry` 的 `setIndices` / `hasIndices` / `indices` / `firstIndex` / `indexCount` / `indicesBuffer` / `material` / `setMaterial` / `program` / `setProgram` 是"读写 primitive 0"的便捷写法（材质/程序写给所有 primitive） | 第二个宿主稳定使用多图元、且单 draw 拼写开始造成两种真相时；或决定它是永久便利（那就把设计文档里的"过渡"改掉） | `graphics-primitives-2026-10-22.md` §2 |

### 3.4 帧、线程与证据

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| （空）| 会话侧“画面已落地”已于 2026-10-19 落地（`FrameProgress::presented`），见 §1 | — | — |

### 3.5 宿主与输入

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| `ViewContext`（F6） | 命令拿到“要改的那个视图”现在靠 `MainWindow::primaryRenderControl()` 一个槽 + `MainWindow::current()`；单窗口/单个 3D 区下这就是上下文，抽出值类型只是多一层 | **一个窗口出现第二个 3D 区**（分屏回到台面），或**命令要指向非活动窗口** | `graphics-render-next.md` §1 F6 |

### 3.6 后端性能与资源（权威：`vsg-reimplementation.md` §6）

| 项 | 内容 | 触发条件 |
|---|---|---|
| ~~B6~~ | ~~`StreamKey.revision` 取几何 revision ⇒ 任一通道改动重传全部通道（已量化 16.98 MiB / +30 ms 一帧）~~ **已关闭（2026-10-22 晚）**：**形状归几何、字节归 buffer，没有兜底**——`StreamKey` 的 revision 只取该 buffer 自己的（`Buffer::setRevision` 是唯一的重传公告），几何 revision 只驱动事实行重建（layout/通道集/计数）；事实行不再拷贝字节，键在录制时拼、字节在上传时读，所以"改一条通道"既不需要重建行也不需要重传别的流。实测（512×512、4 通道 + 索引、整份 16.98 MiB）：稳态 0 上传；单通道就地编辑 **1** 上传/帧、0 rebuilds；换 buffer **1**/帧 + 1 rebuild；四通道就地编辑 **4**/帧、0 rebuilds（旧契约一律 5/帧）。契约变更：`Geometry::setRevision()` 只表示形状，改字节必须 `buffer->setRevision()`。设计见 `graphics-geometry-revision-2026-10-22.md`，判据 `StreamRevisionTest`（5 条）+ 设备级那条测量用例 | ~~宿主报上传带宽~~ 用户 2026-10-22 直接指定做（框架问题，不等触发） |
（复盘 §3.1）。真要省带宽时的正确形状（`setChannelRevision` + 懒拷贝）记录在复盘 §4，**不需要派生版本号** | 宿主报上传带宽（仍是**唯一**触发；没有任何宿主报过） |
| A6 | `MaterialImages` 淘汰键是插入序（FIFO）而非“最近 acquire 帧号” | 活纹理数接近 256 **且**看到“用了很久的贴图被重建”——**已可观测**（`ResourcePressure` + `evictions()`，见 §1）；注意 LRU 不是已量化形状的解 |
| `BlockStorageTest` 偶发 | `TheRegionsAreLaidOutOnceAndDoNotOverlap` 首跑红重跑绿 | 第三次出现 |

### 3.7 上游阻塞（有门禁脚本盯着）

| 项 | 内容 | 触发条件 |
|---|---|---|
| V2 | `forget(view)` 依赖 vsg `protected` 池 | 升级 vsg 时与 V1 一起复核 |
| V3 | 拿不到 `VkPipelineCache`（pinned 与 upstream master 都把第三实参写死 `VK_NULL_HANDLE`） | 推上游；影响集中在冷启动与换会话 |
| V4 | dynamic rendering（`vkCmdBeginRendering`）vsg 记录路径不存在 | 上游支持后合并每 pass 的 begin |

### 3.8 平台与验证

| 项 | 内容 | 触发条件 |
|---|---|---|
| Windows 读像素等价物 | 后端没有 Windows 上的读像素路径 | 有 Windows 机器时 |
| Wayland 回退实测 | 只按 WSI 契约实现，未在真 Wayland 上跑过 | 手上有 Wayland 会话 |
| H2 可选目视 | `pollEvents` 偷 Qt 事件已被三层构造排除；只剩“边缩放边点菜单”的桌面目视 | 有真实交互桌面时（可选） |

### 3.9 公开面收口（2026-10-20 登记，见 `graphics-api-simplification-2026-10-20.md`）

> 2026-10-20 那一轮做完了"删旋钮 / 收 overload / 改 getter 命名"；下面三条是**同一类问题里改动半径
> 最大**的，单项就要一轮，所以先登记形状与触发条件。

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| ~~S1~~ **已落地 2026-10-20**（原行列作历史记录）：`RenderPass` ↔ `PassDesc` 双份 | 一个 pass 声明有**值形式**（`PassDesc`）和**节点形式**（`RenderPass` 的 ~50 个 getter/setter），每个字段两边各一份，引擎在 `addPass` 里逐字段抄一遍——漂移风险与阅读成本都压在这里。方向：节点只持有值 + `desc()`，镜像访问器退场 | **下一次必须动 pass 面的公开变更窗口**（例如新增 `PassKind` 或 pass 级状态），或出现"值形式与节点形式不一致"的真实缺陷 | 本轮复核（`.ai/design/graphics-api-simplification-2026-10-20.md` §2） |
| ~~S2~~ **已落地 2026-10-20**（原行列作历史记录）：`Texture` 的 face/layer 双拼 | `faceCount()` / `layerCount()`（同一个数 1 / 6）与 `source(face)` / `layer(index)`（同一张图）两套说法，头文件自己写着"kind-agnostic spelling"；牵动 vsg 上传路径 60+ 处 | 下一次碰纹理面（新纹理类型 / 上传路径改动 / 分层纹理） | 同上 |
| ~~S3~~ **已落地 2026-10-20**（原行列作历史记录）：`Camera::lookAt` | 收三个向量却叫"设置视图矩阵"；正名 `lookAt(eye, target, up)`。74 处调用点，纯命名收益 | 下一次必须动相机面的公开变更窗口（与 S1 同批做，一次窗口花在一次破坏上） | 同上 |
| ~~SK1~~ **已落地 2026-10-20**（原行列作历史记录）：区归引擎所有 | `RenderEngine::createSurface()` + 引擎持 `intrusive_ptr<RenderSurface>`，区构造转私有；1:1 由类型保证、"第二个区"变构造失败、消灭"区必须先于引擎销毁"的隐式前提（`~RenderSurface` 已经不再解引用已销毁引擎，因为 SK0 把它换成 `Lease`） | **下一次动区/引擎生命周期**（多窗口、离屏会话、或任一宿主需要第二个区）——或 S1 落地时顺手一起做（同一块面，一次窗口） | `graphics-skeleton-review-2026-10-20.md` §4 SK1 |
| ~~SK3~~ **已落地 2026-10-20**（原行列作历史记录）：视图场景不在管线里的诊断 | `SceneView::setPipeline` 时报一次"这条管线没有任何 pass 画这个视图的场景"（只诊断，不改结构：一个管线画多个场景是合法的） | 第一次出现"视图有内容但窗口空"类缺陷，或 `Viewer` 增加第二种建管线方式 | 同上 §3.1 / §4 SK3 |

### 3.10 数据流成本（2026-10-20 复核登记，见 `graphics-abstraction-review-2026-10-20.md`）

**全部收口 / 关闭**：F1（收集只剩一个入口）、F2a（程序覆盖成为 pass 的事实）、F2b（内容记忆体跨帧存活）、
F3（灯光列表复制，按成本/收益关闭）。**本节不再有待办。**

## 4. 明确不做（有理由，不是待办）

- **S11** 一帧跨多窗口（只有“一场画面铺满两块屏”才做）。
- **T4 / T5 / T6**：分屏布局、多 transient 互借深度、便利对象的边界——组合归应用层；T7 叠层跨重建明确不做。
- **`PassDesc` 之外的便利入口**（`graphics-image-source.md` §5.5）：没有消费方，`addInput` 已经够短。
- **零灯自动造头灯、把 `SceneView` 合并进引擎、`RenderControl::Config`、控件上的一行工厂**：见
  `appfw-render-view.md` 的否决清单（隐式状态 / 两个配置面 / 模糊所有权）。
- **不为“将来可能”加空能力标志**；**不把 `Texture` 纳入 producer 规则**；**不改 `RenderTarget` 名字**。

## 5. 本轮确认“不再是待办”（不要再捡）

| 项 | 为什么不再算 |
|---|---|
| **B9** 在飞槽数地板与重布局 | 已收口（`vsg-reimplementation.md` §11.16de），用例与变异都在 |
| **A2 残留**（共享流释放半边） | 旧实现已整体退场，寿命改为“帧点名 + `releaseUnseen`” |
| **首帧叠层空白** | 已查明（首帧画在 Qt 布局瞬态尺寸上）并收口（§11.16db） |
| **窗口随日志长大** | `MainWindow` 已显式 `resize(800×600)`（§11.16dc） |
| **B1/B2/B3** 内容表查找 | 已二分并落地（§11.16bz/ca）；剩余证据强度问题按测量结论维持 |
| **B7** 管线销毁竞态 | **已关闭（2026-10-22）**：破坏路径的计数 device idle 本来就有；本轮补上 headless 的 `OffscreenSession`（唯一 frame source），`releaseContentWorld()` 现在窗口与离屏都 idle。20 次单例 + 2 次全量套件 + 两棵树门禁 **0 VUID**；判据 `OffscreenSessionTest.TheDestructiveWaitIsCountedOnce`。再现时按新问题重开，不重捡本行 |
| **B8** F−N 可读回洞 | **已关闭（2026-10-22）**：不是生产设计缺陷——`perFrameCopies(n) = n + 1`，当前 3 槽 / 4 slab，最旧在飞帧与当前帧永不同 slab；`BlockStorageTest.TheSlabsRotateSoASteadyFrameWritesWhereTheFramesInFlightDoNot` 钉住轮转。F−N 只是 readback 只看最新帧的观测缺口；加旧帧读回是测试接缝，不是正确性修复，不做 |
| **T2 / M6** 一设备 N 交换链 | 由 S10b 覆盖并关闭（2026-10-19），形状是“每窗口一个 `createSurfaceTarget`” |
| **Cube 作 pass 输入** | 两条真实设备用例证明，无需改后端（2026-10-19） |
| **>3 个方向光** | 新后端走 lights 块（1024 槽），不设限（2026-09-25 对账） |
| **内容掩码 / `Pipeline` 命名风险 / 外部图像接线** | L1 掩码、K1 改名、E2 + cube 均已落地（review §1.3、§2.1） |
| **MRT 文档的“deferred 默认被 env 门控”** | 属旧 `VsgRenderer` 世代；现行 SDK 默认 `ShadingPath::Forward`，deferred 由 `RenderPipelineOptions` 选择 |
| **`graphics-perf-backlog.md` §1 的 P1–P17 / R1–R5 / V1–V7** | 老渲染器时代结论，重写后作废（该文件头部已标注；仅留 V2/V3/V4 与 H1 残留作对照） |
| **`graphics-lighting.md` / `graphics-render-pipeline.md` 的未验证项** | 两文件是 v3/v4 世代设计稿，现行实现见 `graphics-state.md` / `graphics-image-source.md`；不再按它们改代码 |

### 5.1 本轮补登的 5 条：3 条落地、2 条按设计关闭

| 补登项 | 结论 |
|---|---|
| 能力数值上限 | **落地**：`Capabilities::max_anisotropy` / `max_color_attachments`；vsg 从 physical device 回答，anisotropy 用与 sampler 同一条夹紧（`anisotropyFor`）。判据：`CapabilitiesTest.TheLimitsTravelBesideTheFeatureBits`；`VsgBackendTest.TheCapabilitiesAnswerTheFeaturesTheDeviceActuallyHas`（`supports(AnisotropicFiltering) == max_anisotropy > 1`、至少 4 个彩色附件）。 |
| `DepthView` 的层 | **落地（声明 + 拒绝）**：`DepthView::layer()`；`RenderPass` 只为 `layer == 0` 建立机制；引擎对 layered 声明报一次并拒绝，不静默退回整张图。分层渲染本体仍是 §3.2 的 L2。判据：`DepthViewTest.ALayeredDepthViewIsRefusedRatherThanPointingAtTheWholeImage`。 |
| 同一 target 的冲突声明 | **落地**：首个声明立住；不同声明被拒，并按 `(target, image)` 只报一次；与 target 一致的声明照常接受。判据：`DepthViewTest.TwoDifferentDepthViewsForOneTargetAreRefusedOnce`。 |
| `OwnedFrame` 的生产者 / 序列化 | **按设计关闭**：引擎编译 borrowed 是它的便宜路径；拥有式帧由**持有这些值的那一方**构建（`OwnedFrame.hpp` 写明"为什么没有引擎入口"与"为什么没有格式"）。序列化缺消费方（文件 / 线 / 回放）；有消费方时是它的事。 |
| `InputRouter` 的工具选择 | **按设计关闭**：只有一个工具，规则表只有一行且没有出错方式——正是本模块拒绝的"空抽象"。选择语义（选中集 / 描边）本来就不在框架（object-model §2.5）。第二个工具到来时规则进 router，`SceneView` 仍是门面。 |

### 5.2 F1–F7 重审（2026-10-19）：5 条关闭、1 条落地、1 条留触发

`graphics-render-next.md` §1 的七条摩擦是本轮起点。逐条对现行代码核实后，**只有 F5 还需要动手**，其余六条
要么已被后来的切片落地（M1/M2a/M3/M7），要么所指的东西那时已经不存在：

| 条 | 结论 | 证据（代码为准） | 对照其它引擎 |
|---|---|---|---|
| F1 状态优先级住在名字和文档里 | **已关闭（M3）** | 一个函数决定一个 draw 的最终状态：`resolveRenderState(authored, pass)`（`StateNode.cpp`），带 `RenderStateOrigin` 说每一项由哪层决定；判据 `GraphicsTest` 的 `resolveRenderState` 用例族 | Filament 的 per-primitive state 由 `RenderableManager` + `Material` 拼出，也是“一个函数一张表”；Vine 多了一层“来源”可查（更强） |
| F2 呈现是运行时争夺 | **已关闭（M2a）** | `RenderEngine::hasWindowPass()` 已删；呈现是**声明**：`RenderSurface::present(view)` / `present(pass)`，一张表一个主人 | wgpu 的 surface/swapchain 由 `Surface` 拥有；Filament 的 present 由 `Renderer` 一次性完成——两者都不存在“谁负责最后一步”的发现 |
| F3 调试覆盖贴在最后一步 | **已关闭（M1）** | 模式落在**几何阶段**（`applyModeToGeometryPass`），所以 Deferred 上四档全部可用 | Unity/filament 的“view mode”都是 material/geometry 层覆盖，不是合成层 |
| F4 声明就地可变 ⇒ 引擎只能事后重查 | **已关闭（M7）** | `PassDesc` 描述 → 每帧 `FramePlan` 编译；`wiringRevision()` / `validateWiring()` 代码里已不存在（只剩两处注释，本轮已改）；`execute()` 自 B2 起非虚 | 这就是 RDG / FrameGraph（O3DE Atom `RPI::FrameGraph`）的模型：声明与执行分开、每帧编译 |
| F5 借用与托管混在一起 | **已落地** | 见 §1 与 `appfw-render-view.md` §13 | **Filament / O3DE RHI：父持有子，子只借**（Vine 取这条）；wgpu 取“资源持 `Arc<Device>` + 设备注册表”，Vine 无注册表，不取 |
| F6 “当前视图”是可变全局槽 | **留触发（§3.5）** | `setPrimaryRenderControl()` 是**每个窗口**一个槽，命令取“活动窗口的那个”；分屏已下线，单 3D 区下这就是上下文 | Qt Creator 是 `ICore::currentContextObject()`（也是全局当前）；Blender 把 `bContext` 当参数传——但它同时有多区/多窗口，判据不同 |
| F7 生命周期靠被动发现 | **已关闭** | 事件协议：`RenderControl::SurfaceState`（Pending/Presenting/Failed）+ `state_changed`；能力面：`Capabilities` / `supports(Feature)`；`init()`/`initAsync()` 是显式入口，`QTimer::singleShot(0)` 只是便利兜底 | Filament 的 `Engine::init()` 同样只回状态码 + 日志；“能力预先可问”这半边 Vine 现在有了 |

**为什么 F6 不做**：本仓自己的规矩是“触发条件没有命中就不要动”（§6）。抽 `ViewContext` 不会错，但也不会
让今天任何一个调用点变短（两处 `window->primaryRenderControl()`），而真正的设计（上下文随触发者流下去，
不是被查出来）需要动 `CommandManager` 的执行通道——那是“第二个 3D 区”到了才付得起的账。

### 5.3 后端性能与资源 7 条重审（2026-10-19）：A4 落地，其余 6 条各有"不做的理由"

| 项 | 结论 | 理由（以代码为准） | 对照其它引擎 |
|---|---|---|---|
| **A4 材质无 revision** | **已落地** | 前提成立：`Material` 确实是唯一没有 revision 的内容类型，于是后端只能逐命令建块 + 逐成员比。而材质的每个 shading 值都走 setter，它**能自己数** → 加 `revision()`（先比后 ++），后端比一个整数。 | three.js `Material.version` 就是这条（setter 自增、渲染器按 version 缓存）；Filament 的 material 也按实例/变体身份缓存，没有"每帧读回值"这一招 |
| **B6 逐通道 revision** | **保持登记** | 前提成立（`GeometryFacts.cpp` 里 `facts.key.revision = geometry.revision()`，所有通道共用一个数）。但"修法"不是免费的：`Geometry::addBuffer/removeBuffer` 的**契约**写的是"改动不公告，用 geometry 的 `setRevision()` 报"。改成逐 `Buffer::revision()` 会让按该契约公告的宿主**静默拿到旧字节**——这是把"多传"换成"画错"，不是优化。先定契约（谁公告），再谈省带宽；触发仍是宿主报上传带宽（16.98 MiB ≈ +30 ms 已量化）。 | 现代引擎里上传键普遍是"资源 + 资源自己的版本"（wgpu `Buffer` 更新即版本、O3DE RHI 资源有 version），但它们**没有**第二套"父对象公告"的契约——所以这不是抄不抄的问题，是 Vine 两套契约只能留一套 |
| **A3** 无内容释放入口 | **已落地（2026-10-22）**：`RenderDevice::releaseContent(raw_ptr<const Object>)` + `RenderEngine` 转发；释放 GPU 驻留而非对象，可选四种内容，token 可取消，纹理 cache 立即删，行/编译产物走 park；判据 `ContentStoreTest.*Release*`（6 条）、`MaterialImagesTest.Release*`（2 条）、`VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt`、`HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice` | 设计见 `graphics-content-release-2026-10-22.md`；原先“宿主报内存压力”的触发由用户 2026-10-22 直接指定做 |
| **§9.3 子树层并集剪枝** | **已落地（2026-10-22）**：`Node::subtreeLayerUnion(inherited)` 延迟缓存（layer epoch），收集 / 拾取 / 包围盒三条 walk 在可见性之后先判 `(union & mask) == 0`；判据 3 条 `SceneTest.TheSubtreeLayer*`；`test_graphics` 434 → **437** | 设计见 `graphics-layers.md` §9.3；用户 2026-10-22 直接指定做 |
| **A6 FIFO 淘汰 + 无容量上界** | **保持登记，且要改"修法"** | 已量化的形状（200 张 = 0 缺失，300 张 = 每轮全重建）说穿了是**工作集 > 容量**：在"同序循环访问"下 LRU 与 FIFO 表现**相同**（插入序 = 使用序），所以"改 LRU"不会变好。真解法是容量可配 + 淘汰可见（诊断一次），或按字节上界。触发：同屏活纹理逼近 256。 | 没有引擎用无限缓存：wgpu/Vulkan 应用普遍自行管理池；Filament 的 `Texture` 由调用方持有、引擎不缓存 |
| **B7 管线销毁竞态** | **已关闭（2026-10-22）** | 破坏路径的计数 device idle 保留；本轮补上 headless 的 `OffscreenSession`，`releaseContentWorld()` 现在窗口与离屏都 idle。20 次单例 + 2 次全量套件 + 两棵树门禁 **0 VUID**；判据 `OffscreenSessionTest.TheDestructiveWaitIsCountedOnce`。 | — |
| **B8 F−N 可读回洞** | **已关闭（2026-10-22），不是生产缺陷** | `perFrameCopies(n) = n + 1`，当前 3 槽 / 4 slab，最旧在飞帧与当前帧永不同 slab；结构性旋转用例守着。F−N 只是 readback 只看最新帧的观测缺口，不改变正确性；旧帧读回接缝不做。 | 同族问题在所有“每帧轮转 per-frame UBO”的引擎里都存在，解法都是“槽数 ≥ 在飞帧数 + 1”（Vine 已是 `slots + 1`） |
| **`BlockStorageTest` 偶发** | **保持登记** | 首跑红重跑绿两次，证据已预置（打印 alignment + regions + strides）。触发：第三次出现。 | — |

## 6. 下一 session 的第一条命令

`git log --oneline -1` 确认在 `dev` 上，然后**只从这里取一条**：§3 里任一触发条件命中 → 按出处文档的设计做，
判据写进 `.ai/memory/graphics.md`；全部未命中 → 不做，不要“顺手把清单清空”。

---

## 附录 A：E2 / Cube 落地记录（2026-10-19，历史）

> 保留判据与结论，供回溯用；不是待办。

0. **K1：`Pipeline` → `RenderPipeline`（已落地 2026-10-19）**
   - 结论：公开类 `Pipeline` → `RenderPipeline`，`PipelineOptions` → `RenderPipelineOptions`，头/源文件改为
     `RenderPipeline.hpp/.cpp`；`PipelineStage` / `pipelineStageOrder` 保留（它们是“阶段”，不是 PSO）。
     公共头、builder、engine、appfw/model_viewer/demo、测试与用户指南同步。
   - 判据：全量构建 0/0；`test_vsg` 463；`check_public_docs.py` 0 new；`check_doc_symbols.py` 通过；8 条门禁 0 finding。
   - **风险已发生**：这是一次公开源码/ABI 破坏，下一次公开面变更窗口已用掉。

1. **全屏 ABI 支持 imported texture（已落地 2026-10-19）**
   - 结论：source 的颜色附件占 0..C-1，depth 跟随（跳过文本自己声明的 binding）；imported 图按输入声明序
     填程序已经声明的其余 sampler binding（升序）。数量对不上由 backend 报 refusal；`ScreenPass` 不再拒绝
     imported 输入。
   - 判据：`VsgBackendTest.AnImportedTextureInputReachesAScreenProgram`（像素）+ 既有全屏用例不回归。

2. **produced 输入按 attachment 绑定（已落地 2026-10-19）**
   - 结论：`CompiledInput` 带 `attachment` / `depth`；`ProducedImage` 的 `color_attachments` 跟随单图（彩色 1、
     depth 0），`ProducedTarget` 仍是整 target。`recordContent` 只 push 声明的那个 view，内容 pass 的 sampled set
     因此只有一张图。全屏 ABI 的 "binding N = attachment N" 作为显式兼容路径保留：`InputImages::screen_colors`
     携带 source 的整 target，`recordScreenDraw` 优先用它；`ProducedTarget` 的旧路径不变。
   - 判据：`VsgBackendTest.AProducedInputBindsOnlyTheAttachmentItNames`；`ScreenDrawTest.TheBindingNamesTheAttachment...`
     与 `SdkPixelTest.TheDeferredPresetFillsEveryGbufferAttachment` 不回归。

3. **imported input revision 失效用例（已落地 2026-10-19）**
   - 结论：`(Texture*, revision)` 缓存键已在位；补了真实设备用例证明第二帧取到新上传。
   - 判据：`VsgBackendTest.ARefilledImportedTextureIsReuploadedForTheNextFrame`（第一帧品红、`setImage` 后
     第二帧黄色；`MaterialImages::count() == 1` 前后不变）。

4. **imported input 的 sampler state（已落地 2026-10-19）**
   - 结论：新增 `Sampler` 值类型，随 `ImageInput(texture, sampler)` 进入声明；`MaterialImages` 上传仍按
     `(Texture*, revision)` 共享，状态按值缓存；`InputSetCache` 的键含 sampler。
   - 判据：`VsgBackendTest.TwoInputStatesOverOneTextureSampleDifferently`；`MaterialImagesTest` 两条；
     `FrameCompilerTest.AnImportedTextureInputCarriesItsDeclaredSampler`；
     `RenderPipelineBuilderTest.AnImportedTextureIsAnInputInDeclarationOrder`。

5. **Cube 作 pass 输入（已落地 2026-10-19）**
   - 结论：不需要改后端。上传按 kind 建 `VK_IMAGE_VIEW_TYPE_CUBE`、失败时 `whiteCube()`；输入 set 的 layout
     来自程序声明，`VkSampler` 不带维度 —— 程序侧只要在该 binding 声明 `samplerCube`。
   - 判据：`VsgBackendTest.ACubeMapInputReachesAContentProgramAsACubeSampler`（+X 面绿，固定方向采样读回
     `(0,1,0)`，`diagnosticCount() == 0`）与 `AnIncompleteCubeInputBindsTheWhiteCubeAndSaysWhy`（白 cube +
     一条 Warning）。
   - **array 仍不存在**：`Texture::Kind` 只有 `D2` / `Cube`；分层目标是 §3.2 的 L2。

