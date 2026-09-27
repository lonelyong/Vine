# .ai/ —— AI 辅助目录

存放面向 AI 助手（Copilot 等）的仓库内知识，随 git 版本化、团队可见。
`docs/` 保留给 GitHub Pages 项目站点，不在其中放置设计文档/笔记。

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

## design/ —— 完整设计文档（46）

**graphics / vsg（24）**
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

**appfw（11）**
- `appfw-startup-phases.md` —— 启动阶段三拍与启动工作线程。
- `appfw-startup-splash.md` —— 启动框与启动进度（`BootSplash`/`StartupProgress`、显式 `finishStartup()`）。
- `appfw-startup-next.md` —— 启动流程下一步（启动事件 / 主窗后显示 / 渲染先就绪；待办）。
- `appfw-render-surface.md` —— 渲染表面生命周期（宿主驱动 attach 的状态机、跟句柄重建、平台回退）。
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
