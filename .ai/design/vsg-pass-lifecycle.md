# pass 生命周期与槽身份（历史登记）

> **历史登记（2026-09-25 收口）**：本文曾是旧后端（SceneBridge 时代）的 pass 生命周期、槽身份、诊断通道与缓存骨架的设计
+ 逐批实施记录（3550 行，2026-09-11 起）。这些类与机制已随旧渲染器 2026-09-24 整体删除，
**本文不再更新**；全文在 git 历史：`git show 0b2f58c:.ai/design/vsg-pass-lifecycle.md`。

同一批问题的**新家**：

| 旧主题 | 新家 |
| --- | --- |
| 帧 / pass 生命周期、槽位时钟 | `Session` / `FrameTimeline` —— `docs/data-flow.md` §4 |
| pass 协议的显式化、无 scope 拒绝、`swapBuffers` 关帧 | 设计文档 §5.1（`Protocol`） |
| 诊断通道（severity × category、episode 规则） | `docs/backend.md` §6（`Diagnostics`） |
| 缓存骨架、槽身份、退役 | 设计文档 §5.2 / §5.6（`ContentStore`、`RetirementQueue`） |
| 缺陷表 D8/D14/D16/D22…（旧 `docs/data-flow.md` §13 的编号） | 只存在于旧文（git 历史） |

> 文中"过程与脚本"类教训（闸门脚本、整包 build、切割脚本护栏、CMake tab、clang 瞬时崩溃、
> 本机 proxy / glslang 环境）已并入 `.ai/design/vsg-reimplementation.md` §5.9。
