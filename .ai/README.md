# .ai/ —— AI 辅助目录

面向 AI 助手与团队的仓库内知识，随 git 版本化。`docs/guides/` 是使用者文档；设计文档与笔记放这里。
已关闭、已明确不做、已被取代的内容不在树内保留（需要时看 git），只保留现行设计、
决策记录、开放待办与模块要点。

**索引由机器钉住**（`scripts/check_ai_docs.py`）：`.ai/` 下每一份 `.md` 必须在这里被点名，点到的名字必须存在。
**写法**：结论在上、历史在下；现行段落提到已不存在的东西时加 `<!-- drift-ok -->`。

**分层**：`design/` 只写**现在是什么**（现行、无日期、≤300 行）；`decisions/` 写**为什么这么定**
（ADR，日期不可变）；`backlog/` 只写**还没做、有触发条件**的事；`memory/` 写短要点；`bugs/`
一 bug 一文件。历史归 git，不放进 `design/`。

## design/ —— 现行设计文档（28）

每份设计文档的统一结构：**一句话 → 设计要点（1.x）→ 功能点（F#）→ 公开接口 → 注意项（N#）→
否决项（D#，一行 + ADR 链接）→ 判据（T#）→ 相关文档**。

**graphics / vsg（9）**
- `graphics-architecture.md` —— 分层、设备/窗口/引擎/链/视图、一帧与调度。
- `graphics-scene.md` —— 节点、状态、收集、层、视图上下文、拾取。
- `graphics-geometry.md` —— 通道、`Primitive`、实例化、revision、包围盒、存储与共享。
- `graphics-rendering.md` —— 目标、MSAA、图源、颜色、着色、阴影。
- `graphics-content-lifecycle.md` —— 内容释放与缓存策略。
- `vsg-backend.md` —— vsg 后端架构 / 类图 / 契约 / 键与状态 / 坑索引 / 否决项 / 门禁。
- `vsg-pitfalls.md` —— vsg 后端 P1–P82 坑表，按九个风险域分组，每条带症状与判据。
- `graphics-shader-params.md` —— 着色参数 ABI：五个块每份声明一个 owner，GLSL 去重、名字只在接线期。
- `graphics-material.md` —— 材质：metallic-roughness PBR、纹理集、资产/实例、两条路径一致（草案）。

**appfw（11）**
- `appfw-startup-phases.md` —— 启动阶段三拍与启动工作线程。
- `appfw-startup-splash.md` —— 启动框与启动进度。
- `appfw-render-surface.md` —— 渲染表面生命周期与平台回退。
- `appfw-render-view.md` —— 渲染视图设计（四层 API、配方归链、输入路由、着色模式）。
- `appfw-command-manager.md` —— CommandManager。
- `appfw-eventbus.md` —— EventBus。
- `appfw-plugin-system.md` —— 插件系统（含禁用与反初始化）。
- `appfw-config.md` —— 配置子系统。
- `appfw-progress.md` —— 环境进度（`ProgressHost`）。
- `appfw-userio.md` —— UserIO。
- `appfw-document-model.md` —— 文档模型（身份 + 生命周期 + 类型注册）。

**base 与其他（8）**
- `async-design.md` —— `src/base/async` 设计（三条必守规则）。
- `imaging-design.md` —— imaging 模块（CPU 像素数据）。
- `imageio-design.md` —— imageio 的公开面（容器能力表、`Result`/`IoError`、无状态自由函数）。
- `iobase-vfs-design.md` —— IOBase（`vn::io`）VFS 与 Stream。
- `meshio-brepio-design.md` —— meshio / brepio 的公开面（草案：格式枚举、`Result`/`IoError`、无单例）。
- `robotics-io-design.md` —— Robotics IO（XML、VFS 打包、材质库）。
- `robotics-proximity-design.md` —— Robotics proximity（接口、VMR 对照、FCL 接入点）。
- `window-design.md` —— window 模块的跨平台窗口上下文。

## decisions/ —— 决策记录（ADR）（15）

一份 ADR 只记一个决定：背景、选项、决定、后果、证据。状态为 `已采纳` / `已取代` / `已否决`，
日期不可变；被取代时在旧文件标 `已取代` 并链接新 ADR。

- `ADR-2026-10-24-command-target.md` —— 命令的目标是视图（`DocumentView`），层可空、需求按命令声明；`RenderControl` 不做目标。
- `ADR-2026-10-28-document-tabs-and-floating.md` —— 文档的 tab 在中央区，浮动是自已的顶层窗口（不进停靠树）。
- `ADR-2026-10-28-central-area-shows-current-document.md` —— 中央区只显示当前文档，浮动的当前文档用说明页占位（一个 current、三处一致）。
- `ADR-2026-10-24-command-target-no-window-query.md` —— 目标只走一条路：不留窗口级的"当前 3D 区"查询，视图的访问器叫 `renderControl()`（部分取代上一条的决定 4）。
- `ADR-2026-10-24-tabs-not-mdi.md` —— 一个窗口同一时刻只显示一个视图（tab）；并排看几块 3D 区由视图自己的窗格表达，不做 MDI/分屏。
- `ADR-2026-10-24-material-model.md` —— 材质改为单一 metallic-roughness PBR + 稀疏实例；alpha 模式进材质，G-buffer 重排。
- `ADR-2026-10-24-light-block-typed-lights.md` —— 灯块改为带类型的灯数组（位置/范围/锥角），两条路径共用 UBO。
- `ADR-2026-10-24-shader-parameter-abi.md` —— 着色参数手写 + 去重，不做反射与材质 DSL。
- `ADR-2026-10-23-no-multi-parent-dag.md` —— 不引入多父，严格树 + 实例化。
- `ADR-2026-10-23-instancing-per-primitive.md` —— 实例化挂在 `Primitive`，不做作用域。
- `ADR-2026-10-23-l2a-reverted.md` —— L2a 回撤，分层目标等 L2b 一起做。
- `ADR-2026-10-22-engine-schedules-passes.md` —— pass 调度只在引擎。
- `ADR-2026-10-22-cache-unbounded-explicit-release.md` —— 缓存不设上限，释放显式管理。
- `ADR-2026-10-22-msaa-target-property.md` —— MSAA 是目标属性，颜色/深度各解析一张图。
- `ADR-2026-10-21-colour-space-in-types.md` —— 颜色空间与数值域放进类型。
- `ADR-2026-10-19-produced-vs-imported-image-sources.md` —— produced 与 imported 图源不合并。

## backlog/ —— 开放待办（3）

只留**还没做、有触发条件**的条目；做完就删，决定不做也删（理由进 ADR）。

- `graphics.md` —— graphics / vsg 的开放待办（内容、后端资源、平台、对象模型）。
- `appfw-startup.md` —— 启动流程剩余项（Windows 半边）。
- `async.md` —— async 下一步（取消环境、作用域、帧分配的 go/no-go）。

## memory/ —— 模块要点（8）

- `graphics.md` —— graphics / vsg 现役要点：机制、判据、坑与“当时为什么这么定”。
- `appfw.md` —— appfw 横切规则（线程 / 锁内不跑用户代码 / 生命周期）。
- `async.md` —— `src/base/async` 要点。
- `imaging.md` —— imaging 要点。
- `imageio.md` —— imageio 要点。
- `loaders.md` —— loaders 层索引。
- `robotics-kinematics.md` —— Robotics kinematics 要点。
- `robotics-proximity.md` —— Robotics proximity 要点。

## bugs/ —— 已修复 Bug 记录（9）

现象 / 根因 / 修复 / 涉及文件 / 验证，一 bug 一文件；标题带历史标记，内容按当时名字保留。

- `vsg-embedded-init-crash.md`
- `vsg-embedded-blank-render.md`
- `vsg-resize-distortion.md`
- `vsg-maximize-black-band.md`
- `shared-task-batch-resume-uaf.md`
- `docking-flyout-under-render.md`
- `docking-layout-transitions.md`
- `docking-tab-dangling-client.md`
- `model-viewer-fit-not-framing.md`

约定：`memory/` 保持简短要点式；详细设计放 `design/`；bug 一条一文件。
