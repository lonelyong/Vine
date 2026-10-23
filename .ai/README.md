# .ai/ —— AI 辅助目录

面向 AI 助手与团队的仓库内知识，随 git 版本化。`docs/guides/` 是使用者文档；设计文档与笔记放这里。
2026-10-23 清理：已关闭、已明确不做、已被取代的历史设计文档已删除（需要时看 git），只保留现行设计、
唯一待办清单与模块要点。

**索引由机器钉住**（`scripts/check_ai_docs.py`）：`.ai/` 下每一份 `.md` 必须在这里被点名，点到的名字必须存在。
**写法**：结论在上、历史在下；现行段落提到已不存在的东西时加 `<!-- drift-ok -->`。

## design/ —— 现行设计文档（26）

每份设计文档的统一结构：**一句话 → 设计要点（1.x）→ 功能点（F#）→ 公开接口 → 注意项（N#）→
否决项（D#）→ 判据（T#）→ 相关文档**。

**graphics / vsg（7）**
- `graphics-architecture.md` —— 分层、设备/窗口/引擎/链/视图、一帧与调度。
- `graphics-scene.md` —— 节点、状态、收集、层、视图上下文、拾取。
- `graphics-geometry.md` —— 通道、`Primitive`、实例化、revision、包围盒。
- `graphics-rendering.md` —— 目标、MSAA、图源、颜色、着色、阴影。
- `graphics-content-lifecycle.md` —— 内容释放与缓存策略。
- `graphics-followups.md` —— **唯一待办清单**：只留开放的触发条件型条目。
- `vsg-backend.md` —— vsg 后端架构 / 类图 / 契约 / 键与状态 / 坑 / 否决项 / 门禁。

**appfw（12）**
- `appfw-startup-phases.md` —— 启动阶段三拍与启动工作线程。
- `appfw-startup-splash.md` —— 启动框与启动进度。
- `appfw-startup-next.md` —— 启动流程下一步（触发条件型）。
- `appfw-render-surface.md` —— 渲染表面生命周期与平台回退。
- `appfw-render-view.md` —— 渲染视图设计（四层 API、配方归链、输入路由、着色模式）。
- `appfw-command-manager.md` —— CommandManager。
- `appfw-eventbus.md` —— EventBus。
- `appfw-plugin-system.md` —— 插件系统（含禁用与反初始化）。
- `appfw-config.md` —— 配置子系统。
- `appfw-progress.md` —— 环境进度（`ProgressHost`）。
- `appfw-userio.md` —— UserIO。
- `appfw-document-model.md` —— 文档模型（身份 + 生命周期 + 类型注册）。

**base 与其他（7）**
- `async-design.md` —— `src/base/async` 设计（三条必守规则）。
- `async-next.md` —— async 下一步（取消、作用域、帧分配的 go/no-go）。
- `imaging-design.md` —— imaging 模块（CPU 像素数据）。
- `iobase-vfs-design.md` —— IOBase（`vn::io`）VFS 与 Stream。
- `robotics-io-design.md` —— Robotics IO（XML、VFS 打包、材质库）。
- `robotics-proximity-design.md` —— Robotics proximity（接口、VMR 对照、FCL 接入点）。
- `window-design.md` —— window 模块的跨平台窗口上下文。

## memory/ —— 模块要点（9）

- `graphics.md` —— graphics / vsg 现役要点：机制、判据、坑与“当时为什么这么定”。
- `appfw.md` —— appfw 横切规则（线程 / 锁内不跑用户代码 / 生命周期）。
- `async.md` —— `src/base/async` 要点。
- `imaging.md` —— imaging 要点。
- `imageio.md` —— imageio 要点。
- `loaders.md` —— loaders 层索引。
- `robotics-kinematics.md` —— Robotics kinematics 要点。
- `robotics-proximity.md` —— Robotics proximity 要点。

## bugs/ —— 已修复 Bug 记录（8）

现象 / 根因 / 修复 / 涉及文件 / 验证，一 bug 一文件；标题带历史标记，内容按当时名字保留。

- `vsg-embedded-init-crash.md`
- `vsg-embedded-blank-render.md`
- `vsg-resize-distortion.md`
- `vsg-maximize-black-band.md`
- `shared-task-batch-resume-uaf.md`
- `docking-flyout-under-render.md`
- `docking-layout-transitions.md`
- `docking-tab-dangling-client.md`

约定：`memory/` 保持简短要点式；详细设计放 `design/`；bug 一条一文件。
