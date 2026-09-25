# Target 统一 + 去 Scene 绑定（历史登记）

> **历史登记（2026-09-25 收口）**：旧后端重构立项（v6，2026-09-04 完成：`Impl` 三桶收敛为统一 `Target`、SDK 不再绑
Vine Scene/Camera）。旧实现已删除，**本文不再更新**；全文在 git 历史：
`git show 248c3f3:.ai/design/vsg-target-unification.md`。

结论仍然成立（且就是今天 SDK 的样子）：`initialize()` 无参、不绑场景；宿主句柄与尺寸靠**公告**
交上来；那批公共 API 破坏性变更已落进 `RenderBackend.hpp`。目标模型的新家 =
`docs/data-flow.md` §4（`WindowTarget` / `OffscreenTarget` / `HostTargets`；计划与应用）
与设计文档 §5.6。
