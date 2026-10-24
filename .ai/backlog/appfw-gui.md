# appfw GUI 待办：视图与命令目标

> 现行设计：`../design/appfw-render-view.md`（视图/宿主）、`../design/appfw-command-manager.md`（命令与目标）。
> 为什么这么定：`../decisions/ADR-2026-10-24-command-target.md`、
> `../decisions/ADR-2026-10-24-command-target-no-window-query.md`。
> 本文件只列**开着、且有触发条件**的项；做完或明确不做就整行删除（git 是档案）。

| # | 项 | 现在的状态 | 触发条件 | 参考 |
|---|---|---|---|---|
| V1 | MDI / 分屏：一个窗口里多块视图同时可见 | 宿主是**单页**的（`CentralDocumentHost::shown` 只有一个），命令目标的解析读的是"宿主当前的视图"—— 对 tab 恒等（同时只可见一个），对 MDI/分屏不够：它答不出命令是从哪一块发出的 | 出现 MDI/分屏需求（同一窗口里要同时看到两块视图） | `../decisions/ADR-2026-10-24-command-target-no-window-query.md`、`../design/appfw-render-view.md` §9.2 |

## V1 落地时要一起改的两半

1. **宿主**：多页同时可见（现在的 `CentralDocumentHost` 一次只 show 一个视图"页"）。
2. **解析**：`gui::resolveCommandTarget()` 加"发起控件优先"（从发起 widget 的父链找到它所在的
   `DocumentView`；找不到才回落到宿主当前的视图）。只有第二半而没有第一半时没有可观测差异，所以两半一起做。

验收：一条命令在左窗格触发、右窗格是"当前"时，作用在左窗格；tab 下行为不变。
