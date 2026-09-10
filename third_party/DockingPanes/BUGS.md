# DockingPanes 缺陷记录与修复说明

> 整理时间：2026-08-19
> 基线：上游 KestrelRadarSensors/dockingpanes（master）
> 用途：记录在 Vine 项目（Windows 11 / WSLg）集成、使用过程中发现并修复的缺陷，供后续维护与升级时对照。

## 一、库内缺陷（已修复）

### BUG-1 深色主题下窗格不变黑（硬编码浅色）
- 现象：Windows 11 深色模式下，停靠窗格标题栏、边框、文字、图标仍是浅色。
- 根因：
  - 库内大量硬编码浅色：标题栏 `#007acc` / `#eeeef2`（`DockingPaneContainer`、`DockingPaneFlyoutWidget` 的 `setActivePane` 用样式表写死）、边框 `#cccedb` / `#aaaaaa`、标题文字黑白固定、关闭/固定按钮图标为黑色位图。
  - 标题栏背景用 `setStyleSheet` 设置，无法跟随调色板变化。
- 修复：
  - 新增 `inc/DockingPaneTheme.h`：所有颜色从 `QApplication::palette()` 派生（`Highlight` / `Window` / `HighlightedText` / `WindowText` / `Mid`，边框由 `Window` 亮度派生）。
  - 标题栏背景改为在 `paintEvent` 中用调色板绘制，不再用样式表。
  - 各绘制控件重写 `changeEvent`，收到 `QEvent::PaletteChange` / `ApplicationPaletteChange` 时 `update()`，运行中切主题自动重绘。
  - 涉及：`DockingPaneContainer`、`DockingPaneFlyoutWidget`、`DockingPaneTabbedContainer`、`DockingPaneTitleWidget`、`DockAutoHideButton`、`DockingToolButton`。

### BUG-2 标题栏拖动路径没有抓取鼠标
- 现象：拖动浮动窗格到停靠指示器上松手，有时无效。
- 根因：
  - `DockingPaneTitleWidget::mousePressEvent` 未 `grabMouse()`；而库内其它拖动路径（Tab 拖出、Flyout 拖出、边缘缩放）都抓取了鼠标。
  - 停靠指示器（中心 sticker、4 个边框 sticker、蓝色预览 `DockingTargetWidget`）都是独立的 `Qt::ToolTip | WindowStaysOnTopHint` 置顶窗口；光标压到它们上面时，move/release 事件发给了指示器窗口而不是标题栏，`titleBarEndMove` → `floatingPaneEndMove` 永远不触发。
- 修复：按下时 `grabMouse()`、松开时 `releaseMouse()`，用 `m_grabbing` 记录抓取状态。

### BUG-3 停靠窗格拖出成浮动时抓取丢失
- 现象：从停靠状态拖出一点点后，拖动时断时续、指示器图标"经常不显示"（WSLg 延迟下尤为明显）。
- 根因：拖过 5px 阈值后 `floatPane()` 会 `closePane` + `setWindowFlags` + `setParent`，**重建原生窗口**，按下时抓取的 X 指针抓取随旧窗口销毁而丢失；之后移动事件只有光标恰好压在 20px 高的标题栏上才送达。
- 修复：新增 `DockingPaneTitleWidget::reacquireGrab()`，`DockingPaneContainer::onMoveDragTitle` 在 `floatPane()` 之后重新抓取新窗口。
- 遗留：**Flyout（自动隐藏窗格）拖出路径存在同样的抓取丢失**（`beginDrag()` 隐藏 flyout 会释放抓取），尚未按同样方式修复，见"遗留问题"。

### BUG-4 floatingPaneEndMove 忽略松手位置
- 现象：快速甩动到指示器上松手，停靠无效。
- 根因：函数开头 `Q_UNUSED(cursorPos)`，只信任最后一次 move 事件算出的 `m_targetPosition` / `m_targetPane`；快速拖动时 move 事件被 Qt 合并，最后一次被处理的 move 可能在光标进入指示器之前，目标还是 `-1`，静默无操作。
- 修复：松手时用 `cursorPos` 重新做一遍命中检测再停靠；结束后重置 `m_targetPosition` / `m_targetPane`。

### BUG-5 指示器显隐顺序错误 + 静态状态泄漏
- 现象：指示器组闪烁、时有时无；离开区域后残留。
- 根因：
  - `floatingPaneMoved` 对每个候选 pane 调用 `updateFloatingPane`，不命中的 pane 走 `else` **无条件**隐藏指示器和预览，把前面刚命中 pane 显示出来的又藏掉。
  - `updateFloatingPane` 内 `static QRect lastHitRect, lastStickerRect` 跨拖动泄漏状态。
- 修复：
  - `updateFloatingPane` 改为返回是否命中该 pane；只有光标不在任何 pane 上才隐藏指示器组；未命中指示器时只隐藏蓝色预览、保留指示器组作为引导。
  - 删除静态变量；显示后补 `raise()` 保证 z 序。

### BUG-6 图标着色失败（Format_Indexed8）
- 现象：对关闭/固定按钮图标重新着色时打印 `QPainter::begin: Cannot paint on an image with the QImage::Format_Indexed8 format`，深色主题下图标仍为黑色、不可见。
- 根因：PNG 资源加载为 `QImage::Format_Indexed8`，不能直接绘制。
- 修复：`DockingToolButton::paintEvent` 中着色前 `convertToFormat(QImage::Format_ARGB32)`。

## 二、主机侧配套修复（不在本库内，但与本库问题直接相关）

| 问题 | 位置 | 说明 |
|---|---|---|
| 应用强制浅色方案 | `src/fw/appfw/src/gui/GuiApplication.cpp` | 原来 `setColorScheme(Qt::ColorScheme::Light)` 强制浅色；删除后跟随系统；Qt<6.5 Windows 下读注册表 `AppsUseLightTheme` + Fusion 深色调色板 |
| `argc` 悬垂引用崩溃 | `src/fw/appfw/src/gui/GuiApplication.cpp` | `new QApplication(c, argv())` 传局部 `int c`，`QCoreApplication` 按引用保存；xcb 下 `QXcbWindow::create()` 计算 WM_CLASS 时读悬空引用 → `strlen` 段错误。改为传成员 `d->argc` |
| WSLg 下拖动无效 | `src/fw/appfw/src/gui/GuiApplication.cpp` | WSLg 下 Qt 默认走 Wayland 插件：窗口 `move()` 不生效、`QCursor::pos()` 返回 (0,0)，拖动机制完全失效；WSL + DISPLAY 存在且未显式指定平台时强制 `QT_QPA_PLATFORM=xcb` |
| `DockPanel::setFloating` 只改状态 | `src/fw/appfw/src/gui/DockPanel.cpp` | 原来只 `setState`，窗格仍留在停靠树：拖动走"已浮动"分支跳过 `closePane`，中央区不回收空间，旧占位区域上不显示指示器。改为真正 `floatPane(QPoint(0,0))` / `dockPane` 停靠回去。注意不能手动 `closePane + floatPane(QRect())`：`closePane` 的 `setParent(NULL)` 不映射屏幕坐标，窗格会落到错误位置 |

## 三、遗留问题（未修复）

1. **Tabbed 子窗格区域重叠**：`updateFloatingPane` 按 `m_dockingPaneList` 顺序命中，"后命中覆盖先命中"；Tab 容器与其子 pane 重叠时停靠目标选择依赖列表顺序。
2. **框架层 `DockFeatures` 缺特性位**：枚举只有 `Closable`，注释中提到的 Floatable / Movable 未定义，特性约束未落地到拖动行为（`src/fw/appfw/sdk/vine/appfw/gui/Gui.hpp`）。

> 第一轮遗留的“Flyout 拖出抓取丢失”、“`qApp->setActiveWindow` 无效调用”、“`DockPanel` 状态方法只改标志”三项已在第二轮修复，见第五节。

## 四、本次改动涉及的库文件清单

- `inc/DockingPaneGeometry.h`（新增，第二轮）
- `inc/DockingPaneTheme.h`（新增）
- `src/DockingPaneContainer.cpp`
- `src/DockingPaneFlyoutWidget.cpp`
- `src/DockingPaneTabbedContainer.cpp`
- `src/DockingPaneTitleWidget.cpp`
- `src/DockAutoHideButton.cpp`
- `src/DockingToolButton.cpp`
- `src/DockingPaneManager.cpp`
- `inc/DockingPaneManager.h`、`inc/DockingPaneContainer.h`、`inc/DockingPaneFlyoutWidget.h`、`inc/DockingPaneTitleWidget.h`、`inc/DockAutoHideButton.h`、`inc/DockingToolButton.h`

## 五、第二轮：布局切换审查（2026-09-11）

审查范围：停靠 ↔ 浮动切换、Tab 拖入/拖出、浮动窗格越界与尺寸约束、Qt 事件处理器里的异常与全局光标查询。

### BUG-7 浮动窗格没有尺寸下限，也没有屏幕边界约束
- 现象：浮动窗格可以拖到只剩几像素；快速甩动可把标题栏拖出屏幕，窗格此后再也抓不回来（只能重启应用）。
- 根因：三条产生浮动窗格的路径（标题栏拖出、Tab 拖出、Flyout 拖出）加公共 API，各自都没有任何尺寸/位置约束。
- 修复：新增 `inc/DockingPaneGeometry.h`（命名空间 `DockingPaneGeometry`）集中三条规则：浮动下限 `minimumWidth = 120` / `minimumHeight = 80`；标题栏至少 `titleBarMargin = 24` px 留在可用屏幕内。`floatPane(QRect)` 应用下限 + `keepTitleBarReachable()`；`DockingPaneManager::dockPane` 进入时清除下限（停靠状态下不能妨碍分割条压缩）；`floatingPaneEndMove` 松手未命中任何目标时对浮动窗格再约束一次。

### BUG-8 从 Tab 组拖出的标签页会“拖动中断”
- 现象：把 Tab 拖出成浮窗后，鼠标仍按住但窗格不再跟随，停靠指示器也不再出现，必须松手重拖。
- 根因：拖出分支所在的 `DockingPaneTabbedContainer` 是**抓着鼠标的那个控件**，而 `floatPane()` 会 `closePane` 掉被拖出的窗格、进而动摇 Tab 组结构；拖动后续事件的接收者不稳定。旧的补救是在 `floatPane()` 之后手工 `move()` 校正位置，掩盖了症状。
- 修复：拖出时把拖动**交接**给被拖出窗格自己的标题栏（`m_draggedPane->continueDrag(grabGlobal)` → `DockingPaneTitleWidget::takeGrab()`），容器随即清空拖动态（`m_draggedPane = nullptr; m_fromMousePressEvent = false;`），仅在自身仍持有抓取时才 `releaseMouse()`。标题栏发出的仍是全局坐标，`floatingPaneStartMove` / `floatingPaneEndMove` 照常收到，拖放命中与松手停靠流程不变。

### BUG-9 Tab 拖出的落点靠两个魔数 `10` 拼凑
- 现象：从 Tab 条拖出的浮窗，光标在标题栏上的相对位置随 Tab 序号变化，拖动时窗格会从光标下“跳”开。
- 根因：`floatPane(QPoint(-deltaPos.x(), -10))` 把 Tab 标签内的局部 X 当成偏移量，再用 `move(grabGlobal - QPoint(0, 10))` 手工纠正 Y；`10` 是猜的常量，而标题栏高度其实是 `6 + fontMetrics().height()`（随字体/DPI 变化）。
- 修复：按语义调用 `floatPane(QPoint(0, 0))`（参数是偏移量，不是目标坐标），再用新增的 `DockingPaneContainer::titleHeight()` 把窗格放到光标处：`grabGlobal - QPoint(0, titleHeight() / 2)`。

### BUG-10 Tab 拖出时“组已空”分支的越界访问
- 现象：拖出组内最后一个 Tab 时，`m_paneList.at(m_stackedWidget->currentIndex())` 在空列表上取值（`QList::at` 越界为未定义行为）。
- 修复：取名字前先判断列表非空，并用 `qBound` 夹住索引；拖出后若组已空，`hide()` + `deletePane(this)` 回收空容器，避免空的 Tab 容器永远留在布局与面板列表里。
- 说明：该分支需要真实的鼠标抓取才能触发，没有自动化用例覆盖（见第六节）。

### BUG-11 事件处理器里抛异常
- 现象：`floatingPaneEndMove` 的 `default:` 分支 `throw std::runtime_error`；Qt 事件循环不接异常，未捕获即 `std::terminate`。
- 修复：未知停靠目标改为 `qWarning()` 并跳过停靠（防御分支不承担崩溃代价），并且只在确实命中目标（`m_targetPosition >= 0`）时才调用 `dockPane`；同时去掉对子控件无效的 `activateWindow()`，只保留 `setFocus()`。

### BUG-12 自动隐藏按钮用全局光标位置判断悬停
- 现象：Wayland 会话下 `QCursor::pos()` 返回 (0, 0)，定时器驱动的悬停检测失效，自动隐藏按钮不会自动消失。
- 修复：`DockAutoHideButton::onTimerElapsed` 改用 `this->underMouse()`（Qt 由平台 enter/leave 事件维护，跨平台一致）。

### 本轮有意未改
- 浮动窗格仍用 `Qt::ToolTip | Qt::FramelessWindowHint`：改成 `Qt::Tool` 可以拿到焦点，但会引入任务栏条目与 WM 装饰，需要整个拖动/指示器机制配合，改动面过大。
- 分数缩放（125%/150%）下的 1px 抖动：Qt 分数缩放取整导致，非本库范围。
- `DockingPaneFlyoutWidget::updateCursor()`、`DockingPaneGlowWidget` 中的 `QCursor::pos()`：调用点没有事件对象可依赖，且宿主已强制 xcb。

## 六、第二轮测试与框架侧修复

框架侧（`src/fw/appfw`）：

| 问题 | 位置 | 说明 |
|---|---|---|
| `removeDockPanel` 双重释放 + `userData` 悬垂 | `src/gui/DockPanelManager.cpp` | 包装对象与容器双方都认为自己是所有者；`closePane` 之后按 `state() != Hidden` 判断是否真被关闭回调否决，成功才 `setUserData(nullptr)` + `setOwnsImpl(false)` + `deletePane()`，最后 `delete panel` |
| 记忆的停靠区域会过期 | `src/gui/DockPanel.cpp` | `_vine_dockarea` 只在 `addDockPanel()` 写，而 `dockArea()` 早已改成动态查询；现在改为脱离停靠树之前（`setFloating(true)` / `pin()` / `collapse()`）用 `dockPositionOf()` 记录实时位置，停靠回来时还原 |
| `static_cast<QWidget*>(impl())` 不安全 | `src/gui/DockPanel.cpp`、`src/gui/DockPanelManager.cpp` | 改为 `qobject_cast<QWidget*>()`，impl 不是 QWidget 时不再被误当成 QWidget |

新增测试（`tests/test_gui/test_gui.cpp`）：

- `GuiTest.DockPanel_FloatRoundTripKeepsArea`：四个区域各浮动/停靠一轮，校验区域不变、浮动下限生效、停靠后下限释放。
- `GuiTest.DockPanel_TabDetachCollapsesGroup`：从 Tab 组拖出（走等价的公共 API）后，组内剩余面板仍是正常停靠面板，被拖出的成为浮窗且能停靠回来。
- `GuiTest.DockPanelManager_RemoveDockPanel`：移除面板后管理器无残留，同组另一个面板仍可见可用。
- `DockGeometryTest.FloatingMinimum` / `DockGeometryTest.TitleBarStaysReachable`：几何规则（下限、不改小已有下限、越界拉回、屏内不动）的直接用例。
- ASan 门禁：`VINE_ASAN_FILTER='GuiTest.DockPanel*:GuiTest.DockPanelManager*:DockGeometryTest.*' scripts/asan_check.sh` → PASS。

**无自动化用例的部分**：Tab 拖出/拖入的拖放命中、Flyout 拖出，需要真实鼠标抓取与窗口管理器配合，只能手工验证（本次改动按代码路径逐条核对：标题栏发送的仍是全局坐标，`floatingPaneStartMove` / `floatingPaneEndMove` 的调用点未变）。

## 七、第三轮：自动隐藏飞窗（Flyout）定位（2026-09-11）——已回退

现象（WSL 下可复现，Windows 正常）：面板自动隐藏后点击边条上的按钮，飞窗能弹出，但位置与预期不一致（实机截图：飞窗被整体平移，且与窗口边缘之间有一圈空带）。

排查结论（保留供后续参考）：

1. 飞窗是顶层 `Qt::Tool` 窗口，几何在 `DockingPaneFlyoutWidget::setPositionAndSize()` 中用全局坐标计算，参考系是**停靠区**（`m_thisWidget`，不含自动隐藏边条）：四周内缩 5px、远边再让开 9px 给边条。**这是库的原有行为，Windows 与 WSL 一致**。
2. 平台确实会在窗口首次映射时自行摆放窗口（WSL 下是 Xwayland + Weston），这部分与 Windows 不同。

本轮试过的三个改法——**已全部回退，用户要求恢复原状**：

| 改法 | 结果 |
|---|---|
| `showEvent()` 0ms/150ms + `moveEvent()` 位置不一致就重新贴合（每次 show 最多 4 次，拖动时不打扰） | 能纠正平台摆放，但叠加下面的几何改动后观感变化明显 |
| `setPositionAndSize()` 改为“先定尺寸再定位置” | 与“先 move 再 resize”几何等价，无实际收益 |
| 参考系改为整个停靠部件（盖住边条、四面齐平） | **观感回归**：底部飞窗横跨整个停靠部件、左右飞窗纵向占满，把其它边条也盖住了 |

回退范围：`DockingPaneFlyoutWidget.{h,cpp}` 恢复为上游原样（`git diff` 为空）；`DockingPaneManager::openFlyout` 的宿主恢复为 `d->m_thisWidget`；`DockingPaneGeometry::keepTitleBarReachable()` 去掉本轮加的可选 `QScreen*` 参数。

保留的回归用例：`GuiTest.DockPanel_PinnedFlyoutIsAnchoredToDockArea`（四条边各自自动隐藏后点击边条按钮，断言飞窗与停靠区的原有贴合关系，即上文的 5px / 9px）。

后续建议：如果还要动这个问题，方向应该是“把飞窗变成不受窗口管理器管理的窗口”（X11 下 override-redirect），而**不是**改贴合基准——后者会改变整体外观。

