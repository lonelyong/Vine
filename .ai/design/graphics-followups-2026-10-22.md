# graphics 待办登记（2026-10-22）：本轮没做的，以及为什么

只记**开放式**事项：每条写清"为什么现在不做 / 什么时候必须做 / 做完怎么验 / 设计在哪"。
已做完的一律不列（历史在 git 与各轮设计文档里）。本轮（`614d2508`、`a7a9cd6e`）落地的是
**S2 注册令牌（`PassGroup`）+ S3 overlay 值化 + S4 的改名部分**，见
`graphics-frame-picture-view-redesign-2026-10-22.md` §9。

## 1. 帧 / 画面 / 视图重设计剩下的三片

判据、旧→新迁移表、变异判据都在 `graphics-frame-picture-view-redesign-2026-10-22.md`（§3、§4、§5）。

| # | 事项 | 为什么本轮不做 | 触发条件 | 做完怎么验 |
| --- | --- | --- | --- | --- |
| F1 | **S1 引擎三个门面**：`passes()` / `outputs()` / `diagnostics()`（同一存储的三个视图，旧方法保留为转发） | 纯转发层，零行为收益；现在只有一个消费者（宿主自己），多出来的门面只是多一层间接 | 出现**第二个**门面消费者：后端要查同一张表、或诊断要按来源过滤、或宿主想遍历"我发布的输出" | 全量构建 0 error；四套件计数不变；6 个静态脚本 0 finding |
| ~~F2~~ | ~~**S4 的改名与归属**~~ **已关闭（2026-10-22 下午）**：改名按 `PassChain` / `PassChainBuilder` / `ChainDesc` / `SceneView::setChain` 落地（`ScenePicture` 被否：读成"场景图片"；`PassPipeline` 被否：重开 K1 的 PSO 歧义），`PipelineStage`→`PassStage` 并挪进 `PassStage.hpp`；"`RenderSurface` 问视图"在 S6 已是现状 | — | ~~下一件真正动公开面的事~~ 已做 | 全量构建 0 error；`test_graphics` 407 / `test_vsg` 481 / `test_gui` 261 / `test_core` 165；6 个静态脚本 0 finding；`RenderSurface` 的 `presentingPass()` / `presentedView()` 用例不改即绿 |
| F3 | **S5 调度上移**：引擎里做稳定拓扑排序，后端只物化；`backend::FrameGraph` 退场；`PipelineStage` 降为 tie-break | **唯一会改像素路径的一片**，与本轮"不动画面"的判据冲突；且需要把环/依赖用例从 `test_vsg` 迁到 SDK | 出现**跨阶段的真实依赖**（SSAO 要在 Shading 之前采样、阴影要在它之外排），或第二个后端出现（它不愿再实现一遍调度） | `scripts/vsg_rewrite_gate.sh` 逐字节不变；环/依赖用例迁到 SDK 且语义不变；`test_vsg` 计数不变 |

## 2. 对象模型复核（2026-10-21）里仍然开放的登记项

> **2026-10-22 下午补记**：`PassGroup` 已删除 —— 链自己就是注册范围（引擎槽位记 `owner`，链析构一次交还）。
> 这没有重开 R2：存储仍然只有引擎那张槽位表，链不存 pass 清单。下文提到 `PassGroup` 的句子是当时的形状，
> 现行写法见 `.ai/memory/graphics.md` 的 2026-10-22（下午）条。 <!-- drift-ok -->

R1 / R3–R10 的完整论证、业界依据、触发条件在
`graphics-object-model-audit-2026-10-21.md` §"登记项"，**不在这里复制**。本轮动了其中两行：

- **R2（`RenderPipeline` 同时是通用 pass 链与具体配方，第三种 overlay 要改类）**：**已关闭**
  （2026-10-22）。`PassGroup` 收掉了注册的第二份清单，`addOverlay` 让第三种 overlay 变成一次调用
  （`PassGroupTest.AHostRegistersAThirdOverlayWithoutTouchingThePipeline`）。剩下的"通用链 / 具体配方"两半
  是 F2 + F3。
- **R1（`backend/` 半里的 Vulkan 策略，以及证据夹具随 SDK 安装）**：只做了 `DeviceRequirements`（2026-10-21
  移入 vsg 插件）。剩下的部分 —— `Keys` 里的 `VkFormat` / `ImageLayout`、`TargetPlan` 的 subpass、夹具
  （`PhaseTable` / `AllocationGate` / `PixelProbe` / `DepthProbe`）随 SDK 安装 —— **仍然开放**，触发条件不变
  （第二个后端）。

## 3. 明确**取消**的提案（别再提）

- **`OverlayContent` 接口**（重设计 §3.4 原来的写法）：不需要。overlay 已经是"值 + role + layout"，
  第三种 overlay 不改类；再加一个接口只会多一层间接。原 §9 已记这条纠正的推理。
- 类型擦除的内容句柄：不需要。`Object` + `raw_ptr<Object>` + `obj_cast<T>` 是仓库既有的擦除**引用**，
  而 overlay 不需要擦除的**拥有者**（登记方持有，见 `AddOverlay` 的注释）。

## 4. 本轮已知取舍（不是缺陷，别当 bug 修）

- **role 不做唯一性强制**：`overlayPass(role)` / `overlayContent(role)` 答第一个匹配。理由：role 是登记方
  自己的标签，冲突是它的命名错误；强制唯一需要一条新的报告路径，而现在没有证据说明需要。
- **overlay 的拒绝报告共用一次 `ReportOnce`**：一条管线"没有这个 role"的多条查询只报第一句（消息里带 role）。
  与 `setShadingMode` 的"一次 episode"一致；要按 role 分别报，等真有宿主每帧轮询多个 role 再说。
- **`PassGroup::passes()` 是快照**（每次分配一个 vector）：它服务于"问一次"的读者（`draws()`、测试），
  不是每帧遍历。真要每帧遍历，答案是把遍历留在引擎里，而不是让它变懒视图。
