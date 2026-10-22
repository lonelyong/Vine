# .ai/ —— AI 辅助目录

存放面向 AI 助手（Copilot 等）的仓库内知识，随 git 版本化、团队可见。
`docs/` 既是 GitHub Pages 项目站点的根，也是仓库统一文档目录：
面向使用者的文档都在 `docs/guides/`（也随站点发布）；设计文档/笔记仍放 `.ai/`，不放 `docs/`。
2026-10-11 把这六份从各模块自带的 `docs/` 目录搬进 `docs/guides/`（那份目录里自带的说明是索引），
引用一并改成 `docs/guides/` 下的新路径，`check_ai_docs.py --strict` 的存量引用因此从 22 条降到 13 条。

**索引由机器钉住**（`scripts/check_ai_docs.py`，门禁阶段 `ai knowledge`）：
① `.ai/` 下每一份 `.md` 必须在本文里被点名（存在却没被点名 ⇒ 红）；
② 本文点到的名字必须真的存在；
③ `--strict` 另查"现行段落里的引用是否还存在"（单元名与仓库内路径）。
2026-09-27 之前本文只覆盖 15/62 份、note 里有 175 处现行引用指向已删/已改名的东西 —— 所以这三条现在是脚本的事，不是记性的事。

**写法约定**（第③条查的就是它）：
* **文档命名**：`<area>-<主题>[-design].md`，`<area>` 只能是**模块名**（`graphics` `appfw` `async` `robotics`
  `geometry` `imaging` `iobase` `window`）或**插件名**（`vsg`）；`-design` 留给"整模块 / 整子系统的设计"，其余用具体主题名。
  2026-09-27 修正了三份不合规的（旧名 → 新名）：`render-pipeline` → `graphics-pipeline.md`、
  `render-pipeline-builder` → `graphics-pipeline-builder.md`、`vine-shader` → `graphics-vine-shader.md`
  （当时的 45 份里只有这 3 份的 `area` 不是模块/插件名；旧名按惯例不以 `.md` 记，免得被当成现行文档名）。
* **结论在上、历史在下**：一份文件先写"现在是什么"，历史沿革放后面，并把小节标题标成
  `历史登记` / `不再更新` —— 这类小节里**允许**点名已不存在的东西（那正是它存在的意义）。
* 必须在**现行**段落里提一个已经不存在的东西时，给那一行加 `<!-- drift-ok -->`。

## design/ —— 完整设计文档（50）

**graphics / vsg（27）**
- `graphics-design.md` —— Graphics 模块设计（现代化架构）。
- `graphics-layering.md` —— graphics 的两半：契约与后端工具层（分层、依赖方向、机器规则 R1/R2/R3）。
- `graphics-render-pipeline.md` —— 多 Pass 渲染管线设计（pass 调度 / 命名产出槽 / 与内容关联）。
- `graphics-scene-graph.md` —— 场景图设计（Node 派生 / OSG-vsg 风）。
- `graphics-state.md` —— 渲染状态设计（StateNode / 状态继承）。
- `graphics-shader.md` —— 可编程着色设计（用户写 GLSL / ShaderProgram）。
- `graphics-lighting.md` —— 光源系统设计（v4）。
- `graphics-shadow.md` —— 方向光阴影设计（v4b）。
- `graphics-mrt-gbuffer.md` —— MRT / GBuffer 设计（一次几何遍历产出多张图）。
- `graphics-deferred-composite.md` —— 延迟 lit 不透明 + 深度正确的 forward 透明/叠层。
- `graphics-overlay.md` —— HUD / Top Pass（叠加绘制）设计。
- `graphics-vsg-audit.md` —— graphics / vsg 后端审查（历史登记）。
- `graphics-pipeline.md` —— `RenderPipeline` 对象（管线的形状、效果与生命周期）；**与多 Pass 那份不同**：那份定 pass 怎么调度，这份定调用方拿到的管线对象长什么样。
- `graphics-pipeline-builder.md` —— `RenderPipelineBuilder`（管线配方层，preset → 管线）。
- `graphics-render-next.md` —— 渲染栈的**重新设计提案**（M1 已落地 2026-10-11，其余未落地）：保留什么、重切四条（调试覆盖挪到几何阶段 ✓ / 呈现契约 Pane+Presenter / `resolveRenderState` 状态单点 / Pass 描述符+FramePlan），横切三条（所有权 Lease、能力查询、观测 dump），以及 M1–M8 迁移切片与逐片验收判据、触发条件。（§7 的"整块重写否决"**已于 2026-10-11 作废**，由下一份接手。）
- `graphics-redesign.md` —— graphics 的**重新设计**（2026-10-11 定，**S1–S7 已全部落地**；范围含后端契约 `RenderBackend.hpp` + `backend/` 半 + vsg 插件）：病根是"帧在引擎里已是值（M7），边界上又被打散成 ~15 个有顺序的可变调用，后端再拼回一个值"；目标形态是三张契约（设备 / 帧 / 呈现）与一个 `FrameDescription` 值；每片独立可逆，总判据 = 两棵树逐字节相同。
- `graphics-window-device-viewer.md` —— **下一轴的目标设计**（**S8、S9、S10a 已 落地 2026-10-11，其余为提案**）：窗口 / `RenderDevice` / `RenderEngine` / 视图 / 离屏 / 多窗口 / `Viewer` 管多视图 —— 名词与职责、六个问题的裁决（设备要不要窗口、一条帧一个引擎、离屏不是模式、谁管多视图、设备共享策略）、生命周期 10 条、契约变化表、切片 S8–S11、否决清单、§11 行业对照（设备 / 窗口 / 帧驱动 / 删除延迟）。
- `device-is-not-a-window.md` —— **S10b 收口（2026-10-18）**：设备持有并发放可呈递目标，但**设备不是窗口**（`RenderBackend` 不再兼 `WindowPresentTarget`，每块窗口一个目标对象）；`executeFrame(frame, nullptr)` 的含义、改动清单、判据与变异，以及落地状态与剩余步骤（`VsgBackendTest` 85 处改写）。
- `graphics-api-review-2026-10-12.md` —— **框架级 API 复核（骨架与抽象，2026-10-12）**：一次独立会话的只读复核的 6 条发现与处置（引擎 : 区 = 1:1 的静默顶替 / `present(pass)` 不校验 + 呈现永不失效 / 借来的目标没有撤销点 / 可空引擎的区 / 公开头注释漂移 / appfw 布尔 getter 命名）、"看着可疑但确实合理"清单、"值得做但现在不做"与总体判断（最脆 = 区 / 引擎 / 目标的所有权与基数）。落地记录见 `.ai/memory/graphics.md` 顶部同日条。
- `framework-api-review-2026-10-13.md` —— **框架级 API 复核：骨架的三问（2026-10-13，跨模块）**：接着上一份把**没挖过的那一半**（`base/**`、`fw/di`、appfw 的命令 / UserIO / Config、robotics、loaders）扫完，量公开 SDK 332 个头加 24 个 CTest 套件，回答"是否符合框架级代码 / API 协调性 / 概念与抽象是否优秀"：依赖方向单向无环、导出与模板实例化纪律正确、graphics 的公开面最干净（成组无 `@brief` 仅 2 处）；13 条发现里**已落地 5 条**（门禁命令 `ctest` 修前是红的 —— Release 树没部署 offscreen 插件、全量 23/24，修后 24/24；§4.4 的过期句与两个不存在的脚本名；**新增公开面文档基线门禁** `scripts/check_public_docs.py` + 1164 条义务基线，已并进 `vsg_rewrite_gate.sh`；**遗留指针族已整族删除**，2026-10-17），其余待裁决（命名两套口径 / 所有权三分法无成文口径 / 自建 std 无成文理由 / 配置两条读取路径），另有 `test_async` 间歇挂死这条既有 flake。
- `graphics-object-model-vs-3d-frameworks.md` —— **对象模型 vs 3D 框架口径（2026-10-12）**：把 graphics 的公开对象逐个对到 VTK / OSG / three.js / Filament / Qt3D（场景图、相机、视图、pass、命令、表面/目标、材质、HUD、操纵器），标出**刻意偏离**（一区一幅画、相机带 aspect，含代价）与**能力缺口**（同 pass 内优先级、实例化、LOD/空间划分、`Pipeline` ≠ PSO 的命名风险，含触发条件）。不要求改任何东西。
- `graphics-abstraction-review-2026-10-19.md` —— **抽象层优化清单（2026-10-19）**：按“框架 / 三维实现 / API 骨架”三镜头的复核收口；记录本轮已落地的契约文字（帧 IR 的 borrowed 语义与顺序权威、Pass 的双扩展路径、`Pipeline` ≠ PSO、`FramePlan` 与 `FrameDescription` 的读者、`ImageRef` 与 `Texture` 的图源分裂、设备像素边界），以及带触发条件的结构性待办（`RenderEngine` 拆分、能力预检、强类型单位/色彩、owned 帧、`shareDepth` 的资源图边等）。
- `graphics-image-source.md` —— **图源统一提案（2026-10-19，内容路径已落地）**：`ImageRef`（帧内产出）与 `Texture`（导入）两种生命周期规则不合并，只把“pass 的一张输入图”提升为 `ImageSource` 值；给出声明/解析/后端绑定切片、备选方案与否决理由、触发条件。
- `graphics-api-simplification-2026-10-20.md` —— **公开面瘦身（2026-10-20，零行为变化）**：删掉没人读的旋钮（阴影 filter、`ImageRef::unbind`）、把一个事实的多种拼法收成一种（`Group` 的双 getter、`Geometry` / `RenderPass` / `RenderTarget` / `RenderSurface` / `SceneView` / `ImageSource` 的成对 overload、`LayoutParticipant` 的两条 step 列表）、按仓库命名规矩改布尔 getter 与公开字段（`isCastingShadow` / `isDepthPromoted` / `isValid` / `Camera::aspect` / `RenderCommand` 的 snake_case），以及交给下一次的三条登记（`RenderPass`↔`PassDesc` 收敛、`Texture` 双拼法、`Camera::lookAt`）。
- `graphics-abstraction-review-2026-10-20.md` —— **抽象 / 概念 / API / 数据流复核（2026-10-20 第二轮）**：
  一条概念一张表（声明 `PassDesc` / 本帧画什么 `PassDraw` / 借用帧 / 拥有帧 / 执行计划 / 宿主投影 / 三种图源身份）；
  一帧六跳的拥有与复制成本表；本轮零行为改动的三组改名（相机投影、删掉 `FrameContext` 与没人读的 `dt`、
  `PassDescription`→`PassDraw`）；数据流上登记的三个成本点（复制版 `collectRenderCommands`、program override
  的整表复制、每 pass 的灯光列表复制）与各自的触发条件。
- `graphics-skeleton-review-2026-10-20.md` —— **骨架复核（2026-10-20）**：先画四张图（宿主/区/视图/引擎/设备的所有权与借用、内容模型、一帧怎么走、配方层），再按“它担了几件事 / 业界同位置是谁”逐对象审 13 个公开对象；落地 S0（区对引擎的借用改 `Lease`，修掉 `~RenderSurface` 的悬空解引用），并把 S1（区归引擎所有）/ S2（`RenderPass` 只存一份 `PassDesc`）/ S3（视图场景不在管线里的诊断）排成切片，另附“不重开”的六条已裁决题与命名裁定表。
- `graphics-object-model-audit-2026-10-21.md` —— **对象模型 / 层级 / 架构复核与落地（2026-10-21）**：以行业口径（OSG/vsg/VTK/three.js/Unity/Filament/bgfx）加仓内已裁决偏离为尺子，逐条论证"业界同位置是谁 / 为什么这里是缺陷而不是取舍 / 动作与判据"；本轮落地 **B1**（`BlendState::enabled` 按契约执行：`FrameCompiler` 把 `opacity<1` 并入混合使能，后端不再恒开）、**G1**（补上 `geometry::Model → Scene` 的最后一跳：`ModelScene.hpp` 的 `sceneFromModel()` + 材质桥，此前场景图层级与材质模型没有生产调用点；**同批把消费者也接了**——`model_viewer` 改持模型树、文件打开器改 `loadModel` + 补法线、视图改 `sceneFromModel`）、**N1**（后端半的 `FrameDescription`/`ClearPolicy` 改名 `CollectedFrame`/`ClearValues`），并**撤回**一条复核误判（`RenderEngine::device()` 是 S10a 的前向命名，留给 S10b），其余九条能力/结构缺口登记触发条件。
- `graphics-frame-picture-view-redesign-2026-10-22.md` —— **帧 / 画面 / 视图重设计（2026-10-22 提案；§9 记录本轮已落地 S2 注册令牌 + `presentingPass` 改名，其余带触发条件推迟）**：按"总体框架"重切 graphics 的四个大对象 —— 引擎拆出 `passes()`/`outputs()`/`diagnostics()` 三个门面（同一存储的三个视图），配方与注册句柄集分开（`ScenePicture` + `PassGroup`，`Slot` 记 `role`/`group`，**不复制指针清单**），overlay 由 `OverlayContent` 接口统一（第三种 overlay 不改类），呈递归视图/表面回答，**调度只留引擎一处**（`backend::FrameGraph` 退场，`PipelineStage` 降为 tie-break）；附旧→新调用点迁移表、S1–S6 切片与变异判据、三条待拍板（命名 / S5 同轮 / `PipelineStage` 去留）。
- `graphics-followups-2026-10-19.md` —— **下一 session 的 graphics 任务列表（2026-10-19）**：P1（`Pipeline` 改名 / 能力预检 / owned 帧 / `shareDepth` 资源图边 / `SceneView` 拆分 / 逃生口登记）在前，F2b 明确不做；随后是 E2 收尾（全屏 ABI imported 输入、按 attachment 绑定、revision 失效、sampler state、Cube），每项带动作、判据与风险。
- `graphics-colour-space.md` —— **颜色：把“色彩空间 + 数值域”放进类型（2026-10-21）**：`Colorf` 是线性光、`Color` 是 sRGB 编码字节，两者只由一对有名转换桥接（`Color::toLinear()` / `Colorf::toColor()`，IEC 61966-2-1 分段曲线、alpha 不参与编码）；SDK 的作者颜色因此只剩一种（`PassDesc::clear_color` / `ClearPolicy` / `FramePlan` 从字节改成线性浮点，后端不再 `/255`），默认清屏色写成“#333333 的原意 + 解一次”，判据含一条设备级“读回 55 而不是 128”；另登记“不做”的四条（`UnitScalar`、目标格式加 sRGB 变体、两个颜色类型改名、8 位图色彩空间）与 robotics / geometry 侧 `Colorf` 输入空间未声明的交互。
- `graphics-layers.md` —— **层与分层渲染（L1 内容掩码已落地 2026-10-12；L2 设计定稿，裁决现阶段不做、等触发）**：**L1**（对象声明 `Node::setLayers` + pass 掩码 `setLayerMask` 两级；收集与**记忆化 memo key** 的坑；配方入口、0 的退化语义、落点清单、判据与 5 条变异实测）与 **L2 目标层**（`layerCount` + pass 的层区间，L2a 逐层先落、L2b 一次写完等触发；读侧先行的理由；§6 开头是"不做"的四条理由），两者的行业依据（Ogre-Next 的三级掩码 + slice/array 目标 + render queue 子组）、与既有裁决的关系、以及"不做的话今天的替代与代价"；**§9（2026-10-13：拾取 / 包围盒掩码参数已落地，各配判据；"子树层并集"整段跳过仍登记）**。
- `vsg-reimplementation.md` —— vsg 后端"从零重新实现"：契约、坑与实施记录（本仓库最长的设计文档）。
- `vsg-design.md` —— vsg 渲染后端设计 v5（历史登记）。
- `vsg-pass-lifecycle.md` —— pass 生命周期与槽身份（历史登记）。
- `vsg-pipeline-sharing.md` —— 管线共享 / 变体缓存（历史登记）。
- `vsg-target-resize-in-place.md` —— 离屏目标尺寸变化的"原地化"（resize in place）。
- `vsg-target-unification.md` —— Target 统一 + 去 Scene 绑定（历史登记）。
- `vsg-texture-upload.md` —— 纹理上传：Vine 的 `Texture` → vsg 的 GPU 图像。
- `vsg-custom-shader.md` —— 着色器文件、嵌入与命名（`builtin_*` 机制）。
- `vsg-custom-attributes.md` —— 自定义顶点属性（历史登记）。
- `vsg-upstream-alignment.md` / `vsg-selection-highlight.md` / `vsg-user-mutation-strategy.md` —— 与上游对齐审查、元素级选择草案、用户端可变策略（均为历史登记）。

**appfw（12）**
- `appfw-startup-phases.md` —— 启动阶段三拍与启动工作线程。
- `appfw-startup-splash.md` —— 启动框与启动进度（`BootSplash`/`StartupProgress`、显式 `finishStartup()`）。
- `appfw-startup-next.md` —— 启动流程下一步（启动事件 / 主窗后显示 / 渲染先就绪；待办）。
- `appfw-render-surface.md` —— 渲染表面生命周期（宿主驱动 attach 的状态机、跟句柄重建、平台回退）。
- `appfw-render-view.md` —— 渲染视图的最终设计 v2（**已落地 S0–S4 + 输入路由 + 着色模式（含线框）+ 连续渲染 + S7 改形**（2026-10-11）：四层 API 与四条缝、**配方归 `Pipeline`（视图只采纳，`ViewerOptions` 是 appfw 那层预设）**、`setViewport` 几何 vs `setShadingMode`/`setStatsVisible` 开关与模式、`LightRig`、"构造只组装"的初始化时机、多视图/多窗口立场与所有权分析；余下一轴（共享设备）仍为定缝）。
- `appfw-command-manager.md` —— CommandManager 设计。
- `appfw-eventbus.md` —— EventBus 设计。
- `appfw-plugin-system.md` —— 插件系统设计（含禁用与反初始化）。
- `appfw-config.md` —— 配置子系统设计。
- `appfw-progress.md` —— 环境进度（`ProgressHost`：搬迁 + 推送模型）。
- `appfw-userio.md` —— UserIO 设计。
- `appfw-document-model.md` —— 文档模型：框架只定"文档"（身份 + 生命周期）与类型注册，区域/单多文档/展示形态归 app。

**base 与其他（11）**
- `async-design.md` —— `src/base/async` 设计（三条必守规则、与 cppcoro/P2300 差异对照、已知风险）。
- `async-next.md` —— async 下一步（P2）：取消的环境令牌、结构化作用域、帧分配的 go/no-go 判据。
- `geometry-attribute-storage.md` —— geometry 属性存储：一份分配，两侧共用。
- `imaging-design.md` —— imaging 模块设计（CPU 像素数据）。
- `iobase-vfs-design.md` —— IOBase（`vn::io`）核心能力设计 —— VFS 与 Stream。
- `window-design.md` —— window 模块设计（跨平台窗口上下文抽象）。
- `graphics-vine-shader.md` —— Vine 自写 shader（引擎内置 shader 的 P0/P1 落地稿；**重写前那一代的记录**，今天的着色设计看 `graphics-shader.md`）。
- `robotics-io-design.md` —— Robotics IO 模块设计（XML 序列化、VFS 打包、5 版本 API、材质库、无状态重构）。
- `robotics-proximity-design.md` —— Robotics proximity 设计（接口清单、设计决策、VMR 对照、FCL 接入点、测试）。

## memory/ —— 精炼模块要点（9）

- `graphics.md` —— graphics / vsg 后端要点（**只记现役**，顶部带按主题索引：一帧怎么走、着色器与 ABI 契约、证据与门禁、宿主窗口与 X11、SDK 与引擎形态）。条目按时间倒序、互相交叉引用（`见 M11j`／`§11.16xx`），所以只搬不重排。2026-09-27 清理：删掉讲重写前那一代的 1839 行（旧 vsg 渲染器层 + 已删脚本/测试 + 与设计文档重复的段落），历史见 git；
  清理判据与逐节裁决留在了提交信息里。
- `graphics-perf-backlog.md` —— graphics / vsg 后端性能待办（只留仍为真的 vsg/工具事实 + 活跃登记 H1/H2/V2/V3/V4；已结的 P1–P17、R1–R5、V1/V5–V7、H3–H9、P18 与两轮"已否决"整块删除，历史见 git）。
- `appfw.md` —— appfw 跨文档要点（三条横切规则：线程 / 锁内不跑用户代码 / 生命周期契约）。
- `async.md` —— `src/base/async` 协程模块要点（三条铁律、契约边界、重复实现）。
- `imaging.md` —— imaging 模块笔记。
- `imageio.md` —— imageio 模块笔记。
- `loaders.md` —— loaders/ 层模块索引。
- `robotics-kinematics.md` —— Robotics kinematics 模块要点。
- `robotics-proximity.md` —— Robotics proximity 模块要点。

## bugs/ —— 已修复 Bug 记录（8）

现象 / 根因 / 修复 / 涉及文件 / 验证，一 bug 一文件。**标题一律带 `（历史登记）`**：记录里的单元名是"当时"的名字，
改名/搬家不该迫使 bug 记录跟着改——`check_ai_docs.py` 因此豁免整份文件（标题带历史标记 ⇒ 全文件豁免）。

- `vsg-embedded-init-crash.md` —— 嵌入式后端初始化崩溃（关闭窗口 / Qt 重建 surface 时）。
- `vsg-embedded-blank-render.md` —— 嵌入式渲染视图空白（SceneBridge 内容不显示）。
- `vsg-resize-distortion.md` —— 窗口缩放时几何体变形（挤压 / 拉伸）。
- `vsg-maximize-black-band.md` —— 放大窗口后新露出的区域先黑，以及中间帧的取舍。
- `shared-task-batch-resume-uaf.md` —— SharedTask 唤醒等待者时 resume 已释放的帧（use-after-free）。
- `docking-flyout-under-render.md` —— auto-hide 临时 flyout 被中央渲染区遮挡。
- `docking-layout-transitions.md` —— 停靠面板布局切换（浮动 / 停靠 / Tab 组合）的一组缺陷。
- `docking-tab-dangling-client.md` —— 拖 console 停靠成 tab 崩溃 / console 消失（clientWidget 野指针）。

约定：`memory/` 保持简短要点式；详细设计放 `design/`；两者内容互补。
Bug 修复按条记录在 `bugs/`，一 bug 一文件。
