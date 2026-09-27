# Bug: 拖 console 停靠成 tab 崩溃 / console 消失（clientWidget 野指针）（历史登记）

- **日期**: 2026-09-02
- **模块**: third_party/DockingPanes（DockingPaneContainer）
- **状态**: 已修复

## 现象

把底部 console 面板拖到左侧「项目」或右侧「属性」上组成 tabbed 时：

- 原始版本：`DockingPaneTabbedContainer::addPane` 里
  `QStackedWidget::addWidget(client)` 崩溃（`QObject::parent` 读野指针，
  `DockingPaneTabbedContainer.cpp:170` / `DockingPaneManager.cpp:767`）。
- 曾用 `QPointer` 兜底后：不崩了，但 console 面板整个消失（addPane 拿到
  null client 后静默跳过，没把 console 加进 tab）。
- 右键→左键组 tab 不崩（左右面板无内容，不触发该路径）。

## 根因

`DockingPaneManager::createPane(id, title, widget, ...)` 用调用方传入的 widget
构造 `DockingPaneContainer`，**`m_clientWidget` 只在构造函数里被赋值为该 widget
一次**。宿主（Vine 的 `DockPanelManager::addDockPanel`）对有内容的面板先建一个
临时 `placeholder` 作为 client，之后再用
`setClientWidget(真正内容)` 换入真内容——但
**`DockingPaneContainer::setClientWidget()` 只操作布局，从不更新 `m_clientWidget`
成员**。于是：

1. `m_clientWidget` 一直指向最初的 `placeholder`；
2. `placeholder` 随后被 `deleteLater()` 删除 → `clientWidget()` 返回野指针；
3. 组 tab 时 `addPane` 解引用 → 崩溃（`QPointer` 方案则自动置空 → console
   静默消失，属掩盖而非修复）。

左右面板没有在创建后换内容 → placeholder 就是真内容且不会被删 → 不触发。
console 是唯一在创建时带内容（`ConsolePanel`）的面板 → 必现。

## 修复

`DockingPaneContainer::setClientWidget()`（`DockingPaneContainer.cpp`）在清空
布局后把成员同步为真正放入布局的控件：

```cpp
m_clientWidget = widget;   // 置于空布局清理之后、null 早退之前
```

这样 `clientWidget()` 永远返回实际显示的内容；placeholder 被删后不再有残留
引用，`addPane`/`openFlyout`（auto-hide 预览）都拿到正确内容。

## 验证

- console 拖到左/右组成 tabbed：不崩溃，console 以 tab 出现，内容正常。
- 右键→左键组 tab 仍正常。
- 顺带修正了 auto-hide（flyout）预览此前可能显示 placeholder/野指针的问题。

## 教训

面板容器的 client 可以事后被替换；凡是在 ctor 里缓存“内容控件”指针的成员，
都必须在替换内容的 setter 里同步更新，否则会留野指针。排查同源问题时重点看：
ctor 只赋值一次、但存在“换内容”setter 的成员（DockingPanes 里 `focusProxy` 也
是同类，已一并处理）。
