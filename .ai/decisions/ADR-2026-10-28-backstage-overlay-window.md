# ADR-2026-10-28：Backstage 用顶层 Tool 窗口，不用 SARibbon 的子控件覆盖层

- 状态：已采纳
- 日期：2026-10-28
- 相关：`../design/appfw-backstage.md`、`../bugs/docking-flyout-under-render.md`、`tests/test_gui/test_gui.cpp`

## 背景

应用按钮需要一个类似 Office Backstage 的整窗页面。SARibbon 提供两个部件：

- `SARibbonApplicationButton`：按钮本身；
- `SARibbonApplicationWidget`：一个跟随 `SARibbonMainWindow` 大小、Esc 隐藏的 `QFrame` 壳，没有页面/导航/内容。

Vine 的主窗口中央是 `createWindowContainer` 的原生 3D 表面。飞窗修复已经证明：**Qt 子控件永远盖不住原生子窗口**，
`WA_NativeWindow` + raise 会扰乱渲染表面（见 `../bugs/docking-flyout-under-render.md`）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 直接用 `SARibbonApplicationWidget` 作为主窗口子控件 | 代码最少，Esc/resize 现成 | 文档区被原生 3D 表面遮挡；要么接受缺一块，要么打开时隐藏/重建渲染表面 |
| B. 顶层 `Qt::Tool | FramelessWindowHint` 窗口（父窗口 = 主窗口），几何跟随主窗口客户区（选定） | 不被原生表面遮挡；与飞窗修复同一路线；不引入全局 topmost | 需要自己同步 move/resize/minimize/restore、激活与 Esc |
| C. `WA_NativeWindow` 子窗口 + raise | 看似重叠正确 | 飞窗实测会扰乱渲染表面，关闭后中央区不可见；禁止再走 |
| D. 打开时隐藏 `RenderControl` 容器，关闭再显示 | 子控件方案可行 | 表面隐藏/重建有闪烁与会话代价；Backstage 不该影响渲染生命周期 |

## 决定

采用 B：

- `Backstage` 的 native widget 是**父窗口为主窗口的顶层 Tool 窗口**（不是主窗口的子控件）；`Qt::Tool` 的 transient 关系让它始终在主窗口之上，又不会像 `WindowStaysOnTopHint` 那样压住其它应用；
- SARibbon 只提供应用按钮与点击信号；Backstage 自己实现几何跟随、Esc、返回按钮；
- 页面/条目模型由 appfw 提供，条目复用 `RibbonAction`（命令名即身份）。

## 后果

- 好的：
  - Backstage 不会被 3D/原生子窗口遮挡；
  - 不触碰渲染表面生命周期；
  - SARibbon 只被“浅封装”，不依赖其 Backstage 壳的隐含父子语义。
- 坏的 / 代价：
  - 自己维护窗口几何与激活；
  - 覆盖的是客户区，不覆盖系统标题栏（与真正 Office Backstage 的视觉略有差异）；
  - 多屏/分数缩放时可能出现平台取整的 1px 偏差。
- 触发条件（什么时候重开）：
  - 需要覆盖系统标题栏且与主窗口完全一体化（此时应考虑上游 `SARibbonApplicationWidget` 的改造，而不是再叠一层）；
  - 出现多窗口（每窗口一个 Backstage）需求。

## 证据

- 飞窗修复：`.ai/bugs/docking-flyout-under-render.md`（原生子窗口永远在上；顶层 Tool 窗口是可行方向）。
- 用例：`GuiTest.Backstage_ApplicationButtonOpensThePage`、`Backstage_PagesAndNavigation`、
  `Backstage_PageItemsAreCommandBound`、`Backstage_FollowsMainWindowSize`、`Backstage_ClosesOnEscapeOrClose`、
  `Backstage_IsOwnedByMainWindow`。
- 代码：`src/fw/appfw/src/gui/Backstage.cpp`（顶层窗口 + 事件过滤器）、
  `src/fw/appfw/src/gui/RibbonBar.cpp`（事件与菜单/Backstage 二选一）。
