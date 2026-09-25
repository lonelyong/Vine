# 与上游（vsg 1.1.16）对齐审查（历史登记）

> **历史登记（2026-09-25 收口）**：2026-09-19 轮次 —— 逐条拿上游机制当尺子量当时的后端。旧实现已删除，**本文不再更新**；
全文（含每条 `file:line` 证据）在 git 历史：`git show 9312ea8:.ai/design/vsg-upstream-alignment.md`。

以下是其中**对上游的事实仍然成立**的部分，保留为"别再往这些方向找收益 / 别再顺手改回去"：

* **视口矩形是动态状态**：上游默认 `DYNAMIC_VIEWPORTSTATE`（录制时从 `renderArea` 同步、
  `pushView` 压相机矩形）；管线里烤进去的只有**目标表面尺寸**，而上游自己也这么烤 ⇒
  **目的地矩形不进重建身份**。
* **跨 render pass 复用管线 = 用错管线**（vsg 的复用只比 `_pipelineStates`、不比 render pass）⇒
  每槽私有的管线 / 描述符注册表保留。**可做的正确版本**：按 **render pass 对象**分表（同一
  render pass 下的两个 view 共享管线是安全的），触发 = 有 ≥3 个槽落在同一 render pass
  （收益估算 1–2 个管线 ≈ 几十 ms 启动），做之前先量。
* **退役环（park）保留**：它是原地 resize（2–36 ms）便宜的来处；**推进必须是帧的最后一步**。
* **自绘 HUD 保留**（自包含 pass，不依赖字体 / 纹理上传）；**Qt 集成自写 `VsgHostWindow` 保留**
  （所有权方向：渲染面归 Qt，`vsgQt` 是反过来的）。
* **同一段 GLSL 只过一次 glslang**（阶段去重表；上游同样把 ShaderSet 建一次复用）。
