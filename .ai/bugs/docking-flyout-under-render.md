# Bug: auto-hide 临时 flyout 被中央渲染区遮挡（历史登记）

- **日期**: 2026-09-02
- **模块**: third_party/DockingPanes（DockingPaneFlyoutWidget）
- **状态**: 已修复

## 现象

面板 pin（auto-hide）后点击边缘按钮临时展开，flyout 与中央 vsg 渲染区重叠的
部分被渲染区盖住（看起来像 z-index 落后）。

## 根因

中央渲染区（`RenderControl`）是用 `QWidget::createWindowContainer` 嵌入的原生
`QWindow`（Vulkan 表面 = 子 HWND）。Windows 上**原生子窗口永远画在父窗口自绘
内容之上**。而 `DockingPaneFlyoutWidget` 原本是无窗口句柄的普通子控件，raise()
对原生兄弟无效 → 一旦与渲染区重叠必被盖住。

## 修复（最终方案：顶层 Tool 窗口）

1. flyout 改为**顶层无边框工具窗口**
   `setWindowFlags(Qt::Tool | Qt::FramelessWindowHint)` —— 天然浮在主窗口
   （含其所有原生子窗口）之上，且完全不扰动渲染表面。
2. `setPositionAndSize()` 的几何由“父控件本地坐标”改为**屏幕坐标**
   （`parentWidget()->rect()` 经 `mapToGlobal`），否则顶层窗口定位跑偏。
3. `openFlyout()`（DockingPaneContainer / DockingPaneTabbedContainer）在
   `show()` 后 `raise()`。

## 走过的弯路（勿再走）

- 试过 `Qt::WA_NativeWindow`（把 flyout 变原生子窗口 + raise）：
  **副作用大**——flyout 关闭/恢复后中央渲染区直接不可见（原生子窗口搅动了
  渲染表面/兄弟层级）。结论：不要用原生子窗口与 createWindowContainer 的
  渲染面做兄弟叠放。

## 验证

- pin 后点 auto-hide 按钮，flyout 正常盖在渲染区之上；
- 关闭/恢复后中央渲染区仍正常显示；
- flyout 位置正确贴边、可拖动/改大小。
