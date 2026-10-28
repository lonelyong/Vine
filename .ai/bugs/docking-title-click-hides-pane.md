# Bug: 单击停靠面板标题栏后面板看不见（空间还在，改窗口大小才回来）

- **日期**: 2026-10-28
- **模块**: third_party/DockingPanes（DockingPaneGeometry / DockingPaneManager / DockingPaneTitleWidget）
- **状态**: 已修复

## 现象

- 单击（不拖动）任意停靠面板的标题栏，面板内容（整块面板）看不见了，但停靠区仍给它留着空间；
  把主窗口拉大/拉小一次，面板又出现在原处。
- 触发条件：主窗口位于**副屏**、或窗口比屏幕大时必现；窗口完全落在主屏 `(0,0)` 内时不显现。

## 根因

两层叠加，缺一不可：

1. `DockingPaneManager::floatingPaneEndMove()` 的“松手未命中停靠目标”分支只按**类型**判断
   （`qobject_cast<DockingPaneContainer*>`），不按**状态**：单击标题栏时面板仍是 `Docked`，
   也会调用 `DockingPaneGeometry::keepTitleBarReachable()`。
   停靠面板是 `QSplitter` 里的**子控件**，`frameGeometry()` 是父级局部坐标；该函数却用
   **屏幕全局矩形** 去夹取并 `pane->move()`。`screen()->availableGeometry()` 的 `left/top`
   不是 0（副屏）时，局部坐标被夹成屏幕坐标，面板被移出分割格 → 看不见，格子空间还在；
   下一次布局（改窗口大小）把它放回原处。
2. 纯点击也走完了整个拖放流程：按下时已 `floatingPaneStartMove()`；松手时
   `updateFloatingPane()` 会把停靠指示器簇 `show()` 再 `hide()`（闪烁），落点恰好压在
   sticker 上时还会把面板重新停靠。

实测（用例把窗口放大到超过屏幕，等价于副屏/负坐标）：右面板几何从 `x=1186` 被改成
`x=775`（= `available.right() - 24`）。

## 修复

- `DockingPaneGeometry::keepTitleBarReachable()`：只对**窗口**（浮动面板）生效，
  `!pane->isWindow()` 直接返回 `false`；文档写明子控件为什么不能移（局部坐标 vs 屏幕坐标）。
- `DockingPaneManager::floatingPaneEndMove()`：只有 `state() == DockingPaneBase::Floating`
  时才应用该规则。
- `DockingPaneTitleWidget`：记录按下后是否发生过移动；**纯点击不再发 `titleBarEndMove`**，
  即不触发落点扫描。拖出交接 `takeGrab()`（Tab 拖出 / Flyout 拖出改浮动）会置位，真实
  拖动路径不变。

## 涉及文件

- `third_party/DockingPanes/inc/DockingPaneGeometry.h`
- `third_party/DockingPanes/inc/DockingPaneTitleWidget.h`
- `third_party/DockingPanes/src/DockingPaneTitleWidget.cpp`
- `third_party/DockingPanes/src/DockingPaneManager.cpp`
- `third_party/DockingPanes/BUGS.md`（第十三节，BUG-21）

## 验证

- `GuiTest.DockPanel_TitleBarClickDoesNotChangeLayout`：把主窗口放大到超过屏幕后，依次点击
  左/右/上三个面板的标题栏，断言父级、几何、可见性、停靠区域都不变（修复前右面板几何断言失败）。
- `DockGeometryTest.KeepTitleBarReachableDoesNotMoveChildWidgets`：子控件即使坐标在屏幕外
  也不得被移动。
- `test_gui` 278 例（277 通过 + 1 既有 skip）。

## 教训

屏幕矩形只能和**窗口**坐标比较；任何“能作用于任意 pane”的几何约束都要先问“它现在是窗口
还是布局里的子控件”。`move()`、`mapToGlobal()`、`frameGeometry()` 组合起来最容易把两套
坐标系混在一起。
