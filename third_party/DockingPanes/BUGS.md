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
| 记忆的停靠区域会过期 | `src/gui/DockPanel.cpp` | `_vn_dockarea` 只在 `addDockPanel()` 写，而 `dockArea()` 早已改成动态查询；现在改为脱离停靠树之前（`setFloating(true)` / `pin()` / `collapse()`）用 `dockPositionOf()` 记录实时位置，停靠回来时还原 |
| `static_cast<QWidget*>(impl())` 不安全 | `src/gui/DockPanel.cpp`、`src/gui/DockPanelManager.cpp` | 改为 `qobject_cast<QWidget*>()`，impl 不是 QWidget 时不再被误当成 QWidget |

新增测试（`tests/test_gui/test_gui.cpp`）：

- `GuiTest.DockPanel_FloatRoundTripKeepsArea`：四个区域各浮动/停靠一轮，校验区域不变、浮动下限生效、停靠后下限释放。
- `GuiTest.DockPanel_TabDetachCollapsesGroup`：从 Tab 组拖出（走等价的公共 API）后，组内剩余面板仍是正常停靠面板，被拖出的成为浮窗且能停靠回来。
- `GuiTest.DockPanelManager_RemoveDockPanel`：移除面板后管理器无残留，同组另一个面板仍可见可用。
- `DockGeometryTest.FloatingMinimum` / `DockGeometryTest.TitleBarStaysReachable`：几何规则（下限、不改小已有下限、越界拉回、屏内不动）的直接用例。
- ASan 门禁：`VN_ASAN_FILTER='GuiTest.DockPanel*:GuiTest.DockPanelManager*:DockGeometryTest.*' scripts/asan_check.sh` → PASS。

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

## 八、第四轮：自动隐藏飞窗“点边条按钮显示不出来”（2026-10-10）

现象：停靠面板标题栏点图钉 📌 收起（自动隐藏到侧边条）后，点侧边条上的还原按钮，面板显示不出来（或一闪而过）。

根因（两条，都要修）：

1. 飞窗是本轮改为顶层 `Qt::Tool` 的窗口，弹出时主窗口会短暂失焦：Qt 先发一次 `focusChanged(old, nullptr)`。`DockingPaneFlyoutWidget::onFocusChanged` 把“焦点被清空”当成离开飞窗，立刻 `Q_EMIT flyoutFocusLost()`，管理器随即 `restorePaneWidget() + deleteLater()` 把刚弹出的飞窗拆掉——表现就是“点了没反应、面板看不到”。
2. `DockingPaneManager::openFlyout` 对“同一面板的飞窗已存在”直接 `return`。若该飞窗已被隐藏（焦点丢失 / 拖动结束），之后每次点边条按钮都被静默吞掉，面板再也弹不出来。

修复：

- `DockingPaneFlyoutWidget::onFocusChanged`：`now == nullptr`（窗口激活切换中）不算离开飞窗，直接返回；只有焦点真的落到飞窗以外的控件才发 `flyoutFocusLost()`。
- `DockingPaneManager::openFlyout`：同一面板且飞窗**可见**时只 `raise()` 后返回；飞窗已隐藏则按“不存在”处理，先清理再重建。

回归用例（宿主侧新增 `tests/test_gui/TestDockingPanesRestore.cpp`，`DockingPanesRestoreTest.*`）：

- 收起 → 点边条按钮 → 飞窗与面板内容可见（停靠面板 / Tab 组 / 客户端区为真实 `VRenderWidget` 渲染表面各一例）
- 焦点丢失后再点边条按钮仍能显示；连续点两次仍可见；飞窗图钉还原后边条按钮被清掉
- 收起（`hidePane`）后重新停靠能显示回来
- `FlyoutSurvivesTransientFocusClear`：模拟弹出瞬间的 `focusChanged(old, nullptr)`，飞窗不得被拆掉

构建注意：本轮库源码更新时文件时间戳被保留，MSBuild 增量构建不会重编译（`build/third_party/DockingPanes` 下仍是旧 `.obj`），表现为“更新了版本还是不行”。改库后需 touch 源文件或清理该目标再全量构建，否则运行的程序仍是旧实现。

## 九、第五轮：状态机与关键操作审查修复（2026-10-10）

审查范围：停靠/还原/浮动/收起/Tab 切换、位置计算、鼠标事件链路。先写复现用例（`DockingPanesRestoreTest.*`），再按论证结论修复。

### BUG-14 从“收起的 Tab 组”飞窗里关闭一页后，该页再也显示不出来

- 现象：Tab 组收起（图钉）→ 点边条按钮开飞窗 → 点飞窗 ✕ 后，这一页既不在任何 Tab 组里、状态却仍是 `Tabbed`；`showPane()` 找不到所属组，宿主的「还原」也因 `isPaneVisible()==Tabbed` 早退 ⇒ 面板回不来（只能“隐藏→还原”两步手动救回）。
- 根因：`DockingPaneTabbedContainer::onCloseContainer` 把页从 `m_paneList` 摘掉并归还控件，却**从不设置该页状态**（非 Tab 路径 `closePinnedPane → closePane` 是有 `setState(Hidden)` 的）。
- 修复：`removePinnedButton()` 之后补 `pane->setState(DockingPaneBase::Hidden)`（两个分支都覆盖）。
- 用例：`ClosingATabFromPinnedFlyoutLeavesThePaneRestorable`（修复前 4 条断言失败：状态、残留飞窗、两处可见性）。

### BUG-15 关掉 Tab 组飞窗后残留隐藏飞窗（悬垂容器指针）

- 现象：`onCloseContainer` 的 `m_paneList.count() <= 1` 分支缺少 `m_flyoutWidget->endDrag()`（`isEmpty` 分支有），`deletePane(this)` 之后管理器仍持有一个隐藏飞窗，其 `m_container` 指向已删除的标签组。
- 影响：当前因 `m_dragMode == true` 挡住了 `restorePaneWidget()` 解引用，不会立刻崩，但属于随时可能被后续改动引爆的易碎状态。
- 修复：在 `deletePane(this)` 前补 `m_flyoutWidget->endDrag(); m_flyoutWidget = nullptr;`，与 `isEmpty` 分支一致（管理器经 `flyoutFocusLost` 清理自己的指针）。
- 用例：`ClosingATabFromPinnedFlyoutLeavesThePaneRestorable` 中断言 `Flyout(manager) == nullptr`。

### BUG-16 dockTab 的邻居不是面板容器时空指针崩溃

- 现象：`dockPane(pane, dockTab, neighbour)` 的 neighbour 若是客户端面板/分隔器/浮动面板（都不是 `DockingPaneContainer`），会走到 `newContainer->addPane(nullptr)` ⇒ `child->clientWidget()` 解引用空指针。
- 证据：用例 `DockTabWithNonContainerNeighbourDoesNotCrash` 修复前报 `SEH exception with code 0xc0000005`。
- 修复：`dockTab` 分支在 `parentContainer == nullptr` 时 `qWarning` 并原样返回，不改动调用方面板。

### BUG-17 以“不在停靠树里的面板”为邻居停靠会让面板静默消失

- 现象：neighbour 是浮动面板（或当前在 Tab 组里、已 `setParent(nullptr)` 的页）时 `getDockingParent()` 返回空 ⇒ 新分隔器照样建、`addPane` 照样搬控件，但**从不插入任何父级** ⇒ 两个面板都不可见；且 `dockPane` 末尾仍把被停靠页置为 `Docked` ⇒ 宿主认为“可见”，「还原」早退 ⇒ 不可恢复。
- 证据：用例 `DockingAgainstAFloatingNeighbourKeepsPanesVisible` 修复前 4 条可见性断言全失败。
- 修复：`parentSplitter == nullptr` 时 `qWarning` 并依次回退为「相对客户端面板（停靠区边缘）」→「根面板（重建根分隔器）」；同时不再对调用方传入的邻居做 `setParent(nullptr)` 副作用。

### 论证后被撤回的两条审查结论（保留证据用例）

审查时列为“交互缺口 / 需实测”的两条，写用例实测后被**推翻**，据实撤回：

| 原结论 | 论证结果 |
|---|---|
| “无法把浮动面板拖进已有 Tab 组” | **不成立**。`DockingPaneTabbedContainer` 本身派生自 `DockingPaneContainer` 且停靠后 `state()==Docked`，所以它会被 `updateFloatingPane` 命中，组中心就是 Tab 指示器 ⇒ 拖到中心松手会正常并组（`FloatingPaneDroppedOnTabGroupJoinsTheGroup` 通过） |
| “`Qt::ToolTip` 浮动面板里的输入框拿不到键盘焦点” | **不成立**。同环境对照组（`Qt::Tool`）可拿焦点、浮动面板（`Qt::ToolTip`）里的 `QLineEdit` 同样能拿到 `hasFocus()`（`FloatingPaneInputCanTakeFocus` 通过） |

顺带确认：标签条带（组顶部）不是指示器图标，拖到那里松手不会停靠、也不会破坏面板与目标组——这是“库只认指示器图标”的设计，已由 `FloatingPaneDroppedOnTabStripStaysIntact` 守护（同时纠正了我原先对该区域行为的描述）。

### 其余审查结论（本轮不改，列为遗留）

| 位置 | 问题 |
|---|---|
| `DockingPaneManager::floatingPaneEndMove` / `updateFloatingPane` | 忽略命中返回值，按 `m_dockingPaneList` 创建顺序“后中覆盖先中”，重叠面板（Tab 组、0 宽分隔器）落点可能不是视觉目标 |
| `DockingPaneFlyoutWidget::mouseMoveEvent` | 飞窗缩放 `m_size += delta` 无下限（可为负），仅靠 `setPositionAndSize` 的 `minimumWidth` 兜住显示 |
| `DockingPaneTabbedContainer::saveLayout` / `openFlyout` | `m_paneList[currentIndex]`、`stackedWidget->widget(indexOf(pane))` 无边界检查（空组/-1 时 UB、`addWidget(nullptr)` 断言） |
| `DockingPaneManager::dockPane` 尾部 | `dockedPane->setFocus()` 对默认 `NoFocus` 的面板是空操作，停靠后键盘焦点不进入面板 |
| `DockingPaneManager::floatingPaneEndMove` 的 `default:` 分支 | 注释写“dropping the pane in place”，但没有重置 `m_targetPosition`；当前 9 个指示器值全被映射，属不可达的整洁性问题 |
| `inc/DockingPaneBase.h` | 库自述 `setState()` 只有赋值、无副作用 ⇒ “以状态为真源”不成立，BUG-14 属这一类；建议后续把状态迁移集中到管理器收口 |

用例：`tests/test_gui/TestDockingPanesRestore.cpp`（15 例），`test_gui` 154/154、`test_scene` 47/47 通过。

## 十、第六轮：停靠区边缘指示器不可用（BUG-18）+ 落点目标复核（2026-10-10）

### BUG-18 拖到停靠区边缘的箭头指示器上松手不会停靠

- 现象：把浮动面板拖到窗口最左/右/上/下边缘的箭头指示器上松手，面板原地飘回——边缘停靠完全不可用（`DockingPaneManager` 里 `frameLeft/Right/Top/Bottom` 那几段分支看起来像死代码）。
- 根因（三处叠加，缺一不可）：
  1. 四个边缘贴纸虽是 89×89 指示器覆盖层的**子控件**，却被 `DockingFrameStickers::setFrameRect()` 用**屏幕坐标**摆放（它们的 `updateCursorPos()` 也是按顶层窗口语义写的 `mapFromGlobal`），于是落点在覆盖层之外 ⇒ 既不可见也命中不到。
  2. `DockingFrameFrameSticker` 构造函数把 `setMaximumSize()` 写了两遍，缺 `setMinimumSize()` ⇒ 贴纸窗口没有确定尺寸，命中矩形与图标不符。
  3. 边缘贴纸压在停靠区边框上，而客户端面板等是**内缩**的（实测 `client pane` 比停靠区左移 5px）：光标落在边缘带内时不在任何面板里，`updateFloatingPane()` 在 `paneRect.contains()` 就早退 ⇒ 边缘命中分支根本没机会执行。
- 修复：
  - 贴纸改为自身顶层工具窗口（无父对象）+ 固定尺寸 = 图标尺寸 + 由覆盖层析构时删除（`~DockingFrameStickers()`），保持 `setFrameRect()` 的屏幕坐标语义。
  - 新增 `DockingFrameStickers::getFrameHit()`（原 `getHit()` 里的 4 段边缘判定抽出来，`getHit()` 末尾改为委托它，避免两份实现）。
  - 管理器新增 `handleFrameEdgeHit()`：在「光标不在任何面板内」时独立做边缘命中并设置落点与预览；`floatingPaneEndMove()` 的停靠守卫由 `m_targetPane` 改为 `m_targetPosition >= 0`，使「只命中边缘、无面板锚点」也能落位。
- 用例：`FloatingPaneDroppedOnFrameEdgeStickerDocksToTheDockAreaEdge`（修复前 `state` 仍是 Floating、面板原地不动；修复后停靠成停靠区最左一列）。

### 落点目标复核：#4“后中覆盖先中”降级为理论风险

`updateFloatingPane()` 只在**指示器图标命中**时才写 `m_targetPane`（BUG-5 的修法），因此“列表顺序覆盖”要发生，需要两个候选面板的**指示器簇相互重叠**（簇居中于各自面板中心）。用例 `FloatingPaneDropTargetsThePaneUnderTheIndicator`：在“左侧标签组 + 右侧面板”布局下把浮动面板丢到右侧面板的顶部指示器，断言落点确实相对**该面板**（同列、位于其上方），当前通过 ⇒ 未观察到错位，结论降级为理论风险并保留该守护用例。

用例：`tests/test_gui/TestDockingPanesRestore.cpp`（17 例），`test_gui` 156/156、`test_scene` 47/47 通过。

## 十一、第七轮：Tab 组飞窗的空控件崩溃（BUG-19）与两项论证结论（2026-10-10）

### BUG-19 给“不在组里”的面板开标签组飞窗会空指针崩溃

- 现象/证据：`DockingPaneTabbedContainer::openFlyout()` 用 `m_stackedWidget->widget(m_paneList.indexOf(pane))` 取内容控件；面板不在组里时 `indexOf` 返回 -1 ⇒ `widget(-1)` 为 null ⇒ 进入飞窗构造函数后 `QGridLayout::addWidget(nullptr)`。用例 `TabbedFlyoutForAForeignPaneIsRejected` 修复前报 `SEH exception with code 0xc0000005`。
- 修复：`openFlyout()` 先算 `paneIndex`，`paneIndex < 0` 或对应控件为空时 `qWarning` 并返回 `nullptr`（管理器侧对空返回是安全的：连接空对象只会告警，条带按钮的路径本就传的是组内面板）。
- 用例：同上（修复后断言返回 `nullptr` 且管理器未留飞窗）。

### 同类索引加固（本次未能构造复现，按加固处理）

| 位置 | 处理 |
|---|---|
| `DockingPaneTabbedContainer::saveLayout()` | `m_paneList[stackedWidget->currentIndex()]` 改为带范围判断的 `at()`，越界时写空 `selectedTab`（原写法在 0/-1 越界时是 UB） |
| `DockingPaneTabbedContainer::onCloseContainer()` | `setName(m_paneList.at(currentIndex))` 改为 `qBound(0, currentIndex, count-1)`，与同文件 `closePane()` 的既有写法保持一致 |

两处都无法在现有流程里构造出非法状态（空组会被 `closePane` 直接删除），因此只做防御性收口、未附加“红转绿”用例。

### 论证未成立：飞窗缩放“无下限”（原遗留 #6）

用 `QApplication::sendEvent()` 直接驱动飞窗的鼠标处理验证：左边缘按下 → 向右拖出 `面板宽度 + 600` → 实测 `记录尺寸 = 100`、`显示宽度 = 55`、`最小宽度 = 50`，**记录尺寸始终为正**（Qt 侧已把负值夹住）。按“先论证、再修复”，本轮**不改缩放代码**，只保留守护用例 `FlyoutResizeCannotGoNegative`（断言记录尺寸为正、显示宽度不小于最小宽）。该条留在遗留清单并标注“不可复现”。

用例：`tests/test_gui/TestDockingPanesRestore.cpp`（19 例），`test_gui` 158/158、`test_scene` 47/47 通过。

## 十二、第八轮：把面板以 Tab 方式停靠到浮动面板上会让两个面板一起消失（BUG-20）（2026-10-10）

### BUG-20 dockTab 的邻居不在停靠树里 ⇒ 标签组变成隐藏的孤儿窗口

- 现象（宿主 `VMainWindowBase::showPaneInGroup`）：一个 Tab 组里「设备信息」被收起（隐藏），另一成员「材质列表」被拖成浮动面板；此时还原设备信息 ⇒ 设备信息没有出现，浮动的材质列表也一起从界面上消失（内容没丢，但两个面板都看不见）。
- 根因（两层叠加）：
  1. 宿主 `showPaneInGroup()` 只按“可见”在 `group` 里挑 Tab 邻居，而 `isPaneVisible()` 把 `Floating` 也算可见（`VBuilderWindowBase::syncViewActions` 的 Ribbon 勾选依赖这个语义，不能改）⇒ 唯一的可见成员是浮动面板，于是发出 `dockPane(pane, dockTab, floating_pane)`。
  2. 库 `dockPane()` 的 `dockTab` 分支只挡了“邻居不是容器”（BUG-16）；邻居是容器但不在停靠树里（`Floating` / `Pinned`）会走到 `replacePane(neighbour, newContainer)`，而 `replacePane()` 对既不在分隔器里、又不是根的面板只能 `closePane(oldPane)` ⇒ 浮动面板被关闭，新建的 `DockingPaneTabbedContainer` 是**无父对象的顶层窗口且从未 show**，两个面板都成了这个孤儿窗口里的标签页。
- 证据：临时诊断打印（修复前）——`paneCount` 由 2 变 3；`DockingPaneTabbedContainer state=Docked vis=0 parent=-`；两个容器都是 `state=Tabbed vis=0`。用例 `ShowPaneInGroupIgnoresFloatingSiblings` 修复前 4 条断言全失败。
- 修复：
  - 库（`DockingPaneManager::dockPane`）：`dockTab` 分支把“邻居不在停靠树里”（`Floating` / `Pinned`）与“邻居不是容器”同等对待 —— `qWarning` + 原样返回，不改动任何面板（补齐该分支注释里本来就已经写明的意图）。
  - 宿主（`VMainWindowBase::showPaneInGroup`）：Tab 邻居只接受 `Docked` / `Tabbed` 的成员；没有合格邻居（全隐藏、或可见成员都已浮动）时回退 `showPaneAt(pane, base_position, nullptr)`，即还原到该面板原来的停靠方位。
  - 未改 `isPaneVisible()`：它把浮动画板算作可见是 Ribbon 勾选同步依赖的语义。
- 用例：
  - `ShowPaneInGroupIgnoresFloatingSiblings`（宿主路径；修复后设备信息回到原停靠列、材质列表仍是可见的浮动面板）。
  - `DockTabAgainstAFloatingNeighbourIsIgnored`（库路径；直接 `dockPane(hidden, dockTab, floating)` ⇒ 断言 `paneCount` 不变、请求被忽略、浮动面板与其内容仍可见）。

### 本轮移除的用例

`DockedPaneTakesKeyboardFocus` 是“停靠后焦点进入面板内容”的守护用例：本环境的 SARibbon 主窗口内控件拿不到键盘焦点，该例只能整例 `GTEST_SKIP`（无判定力），已删除；对应条目仍留在遗留表（`dockPane` 尾部 `setFocus()` 对默认 `NoFocus` 面板是空操作）。

用例：`tests/test_gui/TestDockingPanesRestore.cpp`（21 例），`test_gui` 160/160、`test_scene` 47/47 通过。

## 十三、第十一轮：单击标题栏把停靠面板移出分割格（BUG-21）（2026-10-28）

### BUG-21 单击（不拖动）停靠面板标题栏后，面板看不见，但占用的空间还在

- 现象（用户报告）：单击任意停靠面板的标题栏，面板内容（整块面板）看不见了，停靠区却仍然给它留着空间；把主窗口拉大/拉小一次，面板又出现在原处。
- 触发条件：主窗口位于**副屏**、或窗口比屏幕大时**必现**；窗口完全落在主屏 (0,0) 内时不显现（因此此前测试未覆盖到）。
- 根因（两层）：
  1. `DockingPaneManager::floatingPaneEndMove()` 的“自由桌面”分支只按类型判断（`qobject_cast<DockingPaneContainer*>`），不按状态：单击标题栏时面板仍是 `Docked`，也会调用 `DockingPaneGeometry::keepTitleBarReachable()`。停靠面板是 `QSplitter` 里的**子控件**，`frameGeometry()` 是父级局部坐标，而该函数用 **屏幕全局矩形** 去夹取并 `pane->move()`。当 `screen()->availableGeometry()` 的 `left/top` 不是 0（副屏）时，局部坐标被夹到屏幕坐标，面板被 `move()` 到分割格之外 → 看不见，格子空间还在；下一次布局（改窗口大小）把它放回原处。
     实测（用例把窗口放大到超过屏幕，等价于副屏/负坐标）：右面板几何从 `x=1186` 被改成 `x=775`（= `available.right() - 24`）。
  2. 纯点击也走完了整个拖放流程：按下时已 `floatingPaneStartMove()`；松手时 `updateFloatingPane()` 会把指示器簇 `show()` 后再 `hide()`（闪烁），落点恰好压在 sticker 上时还会把面板重新停靠。
- 修复：
  - `DockingPaneGeometry::keepTitleBarReachable()`：只对**窗口**（浮动面板）生效，`!pane->isWindow()` 直接 `return false`；文档写明子控件为什么不能移（局部坐标 vs 屏幕坐标）。
  - `DockingPaneManager::floatingPaneEndMove()`：只有 `state() == DockingPaneBase::Floating` 时才应用该规则。
  - `DockingPaneTitleWidget`：记录按下后是否发生过移动；**纯点击不再发 `titleBarEndMove`**，即不触发落点扫描。拖出交接 `takeGrab()`（Tab 拖出 / Flyout 拖出改浮动）会置位，真实拖动路径不变。
- 用例：
  - `GuiTest.DockPanel_TitleBarClickDoesNotChangeLayout`：把主窗口放大到超过屏幕后，依次点击左/右/上三个面板的标题栏，断言父级、几何、可见性、停靠区域都不变（修复前右面板几何断言失败）。
  - `DockGeometryTest.KeepTitleBarReachableDoesNotMoveChildWidgets`：子控件即使坐标在屏幕外也不得被移动。
- 验证：`test_gui` 278 例（277 通过 + 1 既有 skip）。

### 教训（坐标系）

屏幕矩形只能和**窗口**坐标比较；任何“能作用于任意 pane”的几何约束都要先问“它现在是窗口还是布局里的子控件”。同样的坑在 `move()`、`mapToGlobal`、`frameGeometry()` 组合里都能重现。
