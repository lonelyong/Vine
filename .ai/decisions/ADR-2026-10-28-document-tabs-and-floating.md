# ADR-2026-10-28：文档的 tab 在中央区，浮动是一个自己的顶层窗口（不进停靠树）

- 状态：已采纳
- 日期：2026-10-28
- 相关：`../design/appfw-document-model.md`（§1.2、§3.1.5、§5.3） ·
  `../decisions/ADR-2026-10-24-tabs-not-mdi.md`、`../decisions/ADR-2026-10-24-command-target.md` ·
  `../design/appfw-render-surface.md`（F3/C3 重父化）

## 背景

多文档要有三件事：**中央客户区里多格 tab**、**视图能浮到自己的窗口**、**只能停回中央区，不能停到任何停靠区**。

现仓库的 DockingPanes（vendored，含本地补丁）表达不了前两件：

- 中央客户区是一个 `DockingPaneClient`，`setClientWidget()` 只装一个 widget；
- 浮动 pane 拖到客户区上方时，`updateFloatingPane()` **显式关掉 tab 指示器**（`setTabVisible(false)`）；
- `dockPane(dockTab, …)` 的邻居必须是已停靠的 `DockingPaneContainer` / `DockingPaneTabbedContainer`，客户区不是；
- `DockingPaneTabbedContainer` 只能由 manager 内部的 `dockPane(dockTab, …)` 造出，`createPane()` 永远造单个 pane；
- 失去 pane 覆盖的光标由 `handleFrameEdgeHit()` 停靠到四条边，**没有任何允许目标的白名单**。

所以“文档 tab 在中间 + 只能停回中间”要么自己撑着，要么改三方代码。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 宿主自带 tab 条 + 自己的浮动窗口（选定） | “不能停到别处”是**结构**保证：文档根本不是 pane，没有目标可命中；不碰三方代码 | 浮动/停回的手势要自己写（约一个窗口类）；拿不到库的停靠指示器 |
| B. 改 DockingPanes：让 `TabbedContainer` 成为客户区，并给停靠目标加白名单 | 直接用库的 tab UI、tab 拖出/拖回 | 三处私有命中测试都要改；文档会进 `saveLayout()`；上游合并时补丁悄悄失效（失效方式 = “能停到左边了”） |

## 决定

1. **`CentralDocumentHost` 自带 `QTabBar` + `QStackedWidget`**：每个打开的文档一格（视图仍懒建），只有一份时不显示条。
2. **文档永不进入停靠树**：`setFloating()` 建的是一个本宿主拥有的顶层 `QWidget`，不是 `DockingPaneContainer`。
3. **停回只有三条路**：浮动窗口的“停靠”按钮、`setFloating(doc, false)`、关闭文档。
4. **tab 选中与 current 只有 `DocumentManager` 一个所有者**：选 tab 调 `setCurrent()`，`currentChanged` 反选 tab。
5. **关 tab / 关浮动窗口都走 `DocumentManager::close()`**，`canClose()` 的否决照常生效。
6. **焦点跟随文档**：浮动窗口激活即把它设为 current，于是主窗口 ribbon 的命令落在“最后干活的那个文档”上，
   `resolveCommandTarget(window)` 的既有解析不需要改。
7. **文档不是布局**：`DockingPaneManager::saveLayout()` 只保存工具面板；打开哪些文档属于会话，另做。

## 后果

- 好的：一个概念（文档 = tab 或自己的窗口），没有“允许/禁止停靠位置”的隐形规则；DockingPanes 保持原样；
  单文档应用看不到 tab 条。
- 坏的 / 代价：浮动时页控件 reparent ⇒ 3D 原生窗口重建一次（框架已支持：只换表面、不换会话，
  见 `appfw-render-surface.md` F3/C3），会闪一下；浮动窗没有停靠预览/吸附。
- 触发条件（什么时候重看 B）：出现“文档要能和工具面板 tab 混排”或“文档要吸到边缘成停靠区”的真实需求 ——
  那时两件事一起做：客户区可作 `dockTab` 邻居 + 停靠目标白名单。

## 证据

- `tests/test_gui/DocumentViewTest.cpp`：`EveryOpenDocumentGetsATabAndATabSelectionSelectsTheDocument`、
  `SwitchingTabsReusesEachViewAndNeverReinstallsTheCentralArea`、
  `ADocumentFloatsIntoItsOwnWindowAndDocksBackIntoItsTab`、
  `ClosingAFloatedDocumentsWindowGoesThroughTheManagerSoAVetoKeepsIt`。
- 三方事实：`third_party/DockingPanes/src/DockingPaneManager.cpp` 的 `updateFloatingPane()`（`setTabVisible(false)`）、
  `dockPane()` 的 `dockTab` 分支、`handleFrameEdgeHit()`。
