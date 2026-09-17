# .ai/ —— AI 辅助目录

存放面向 AI 助手（Copilot 等）的仓库内知识，随 git 版本化、团队可见。
`docs/` 保留给 GitHub Pages 项目站点，不在其中放置设计文档/笔记。

- `memory/` —— 精炼模块要点（对应 VS Code 记忆系统仓库级笔记的可提交副本）。
  - `robotics-proximity.md` —— Robotics proximity 模块要点。
  - `graphics-perf-backlog.md` —— graphics / vsg 后端性能待办（已核实的机制 + 待办项 + 待实测数字）。
  - `async.md` —— `src/base/async` 协程模块要点（三条铁律、契约边界、重复实现）。
  - `appfw.md` —— appfw 跨文档要点（三条横切规则：线程 / 锁内不跑用户代码 / 生命周期契约，易记错的 API 语义）。
- `design/` —— 完整设计文档。
  - `robotics-io-design.md` —— Robotics IO 设计（XML 序列化、VFS 打包、5 版本 API、材质库、无状态重构）。
  - `robotics-proximity-design.md` —— Robotics proximity 设计（接口清单、设计决策、VMR 对照、FCL 接入点、测试）。
  - `async-design.md` —— `src/base/async` 设计（三条必守规则、与 cppcoro/P2300 的差异对照、已知风险、验证配方）。
- `bugs/` —— 已修复 Bug 记录（现象 / 根因 / 修复 / 涉及文件 / 验证）。
  - `vsg-embedded-init-crash.md` —— 嵌入式后端初始化崩溃（关闭窗口/Qt 重建 surface 时）。
  - `vsg-embedded-blank-render.md` —— 嵌入式渲染视图空白（SceneBridge 内容不显示）。
  - `vsg-resize-distortion.md` —— 窗口缩放时几何体变形（挤压/拉伸）。
  - `vsg-maximize-black-band.md` —— 放大窗口后新露出的区域先黑，以及中间帧的取舍（2026-09-17）。
  - `shared-task-batch-resume-uaf.md` —— SharedTask 唤醒等待者时 resume 已释放的帧（2026-09-17）。

约定：`memory/` 保持简短要点式；详细设计放 `design/`；两者内容互补。
Bug 修复按条记录在 `bugs/`，一 bug 一文件。
