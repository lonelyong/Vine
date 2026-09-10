# Bug: 停靠面板布局切换（浮动/停靠/Tab 组合）的一组缺陷

- **日期**: 2026-09-11
- **模块**: third_party/DockingPanes + src/fw/appfw（DockPanel / DockPanelManager）
- **状态**: 已修复（拖放命中命中类路径无自动化用例，见"验证"）

## 现象

1. 浮动窗格可以拖到只剩几像素，抓不住；快速甩动能把窗口甩出屏幕，之后再也拖不回来。
2. 把一个 Tab 从 Tab 组里拖出来时，鼠标还按着，窗格却不再跟随，停靠指示器也不再出现。
3. 从 Tab 条拖出的浮窗，光标落在标题栏上的相对位置随 Tab 序号变化，拖动时会从光标下"跳"开。
4. 面板被用户拖到另一侧后，`setFloating(false)` / `restore()` 把它停靠回**创建时**的那一侧。
5. `removeDockPanel()` 后偶发崩溃或读到已释放的包装对象。

## 根因

- **几何规则各处自写**：产生浮动窗格有四条路径（标题栏、Tab 条、Flyout、公共 API），没有任何一条约束尺寸或位置。
- **拖动所有权没有交接**：Tab 拖出由 `DockingPaneTabbedContainer` 发起并持有鼠标抓取，而拖出过程会重排/可能销毁 Tab 组，抓取在拖动进行中被丢掉；原代码用 `move()` 纠正位置，掩盖了症状。
- **落点靠魔数**：`floatPane(QPoint(-deltaPos.x(), -10))` 把 Tab 标签内的局部 X 当偏移量，标题栏高度写死 `10`（实际是 `6 + fontMetrics().height()`）。
- **两处"真相"**：`DockPanel` 把停靠区域缓存在 `_vine_dockarea` 属性里（只在 `addDockPanel()` 写），而 `dockArea()` 早已改成动态查询，缓存过期后停靠回错误的一侧。
- **双重所有权**：包装对象与 `DockingPaneContainer` 都认为自己是所有者；`userData()` 在包装对象销毁后仍被库内关闭回调使用。

## 修复

库侧（`third_party/DockingPanes`，详见其 `BUGS.md` 第五节）：

| 项 | 修复 |
|---|---|
| 尺寸/边界 | 新增 `inc/DockingPaneGeometry.h`：浮动下限 120x80、标题栏至少 24 px 留在可用屏幕内；`floatPane(QRect)`、`dockPane()`、`floatingPaneEndMove()` 三处统一应用 |
| 拖动交接 | Tab 拖出时 `m_draggedPane->continueDrag(grabGlobal)` 把拖动交给窗格标题栏（`takeGrab()`），容器清空自己的拖动态并只在仍持有抓取时 `releaseMouse()` |
| 落点 | `floatPane(QPoint(0, 0))`（参数是偏移量）+ 新增 `titleHeight()`，按 `grabGlobal - QPoint(0, titleHeight() / 2)` 定位 |
| 空 Tab 组 | 取 Tab 名之前判空 + `qBound` 夹索引；组空则 `hide()` + `deletePane(this)` |
| 事件处理器异常 | `default:` 不再 `throw`，改 `qWarning()` 跳过停靠，且只在命中目标（`m_targetPosition >= 0`）时才 `dockPane()` |
| 悬停判定 | `DockAutoHideButton::onTimerElapsed` 用 `underMouse()` 取代 `QCursor::pos()`（Wayland 返回 (0,0)） |
| 自动隐藏飞窗定位（**已回退**） | 2026-09-11 按用户要求恢复原状：`DockingPaneFlyoutWidget.{h,cpp}` 与上游一致（`setPositionAndSize()` 仍为“先 move 再 resize + 5px/9px 贴合”），`openFlyout` 宿主仍为 `m_thisWidget`，`keepTitleBarReachable()` 保持两参数。结论：位置本身不动；若将来仍要处理，方向是把飞窗改成不受窗口管理器管理的窗口（X11 override-redirect），而不是改贴合基准 |

框架侧（`src/fw/appfw`）：

| 项 | 修复 |
|---|---|
| 区域真相 | 脱离停靠树之前（`setFloating(true)` / `pin()` / `collapse()`）用 `dockPositionOf()` 记录实时位置；停靠回来时用 `dockPositionFor(rememberedDockArea(...))` 还原 |
| 所有权 | `removeDockPanel()`：`closePane()` 后按 `state() != Hidden` 判断是否真被关闭回调否决；成功则 `setUserData(nullptr)` + `setOwnsImpl(false)` + `deletePane()`，最后 `delete panel` |
| 类型安全 | `static_cast<QWidget*>(impl())` 改为 `qobject_cast<QWidget*>()`（impl 不是 QWidget 时不再被误当成 QWidget） |
| 接口语义 | `setContent()` 明确"窗格拥有它显示的内容"（旧 client 由窗格 `deleteLater()`）；状态方法补 Doxygen |

## 验证

- `tests/test_gui/test_gui.cpp` 新增 5 个用例：`DockPanel_FloatRoundTripKeepsArea`、`DockPanel_TabDetachCollapsesGroup`、`DockPanelManager_RemoveDockPanel`、`DockGeometryTest.FloatingMinimum`、`DockGeometryTest.TitleBarStaysReachable`。
- 另加 `DockPanel_PinnedFlyoutIsAnchoredToDockArea`：四条边各自动隐藏后点击边条按钮，断言飞窗贴边坐标与停靠区几何一致（含内容最小尺寸大于飞窗请求尺寸的情况）。- `test_gui` 158 个用例全通过；`test_core` / `test_vsg` / `test_async` / `test_progress` / `test_math` / `test_logging` 全通过。
- ASan：`VINE_ASAN_FILTER='GuiTest.DockPanel*:GuiTest.DockPanelManager*:DockGeometryTest.*' scripts/asan_check.sh` → PASS。
- 严格告警扫描（`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`）在改动行上无新增告警。

## 遗留 / 有意未做

- **Tab 拖出/拖入的拖放命中、Flyout 拖出**：需要真实鼠标抓取与窗口管理器配合，无自动化用例；本次按代码路径核对（标题栏发出全局坐标，`floatingPaneStartMove` / `floatingPaneEndMove` 的调用点未变）。
- **飞窗在 WM 映射时被自行摆放**：无头环境无法覆盖。2026-09-11 的一轮改动（映射后纠偏 + 参考系改为整个停靠部件）已全部回退；保留 `GuiTest.DockPanel_PinnedFlyoutIsAnchoredToDockArea` 钉住库的原有贴合关系（5px 内缩 / 远边 9px 让开边条）。
- 浮动窗格仍用 `Qt::ToolTip | Qt::FramelessWindowHint`（改 `Qt::Tool` 会引入任务栏条目与 WM 装饰，需整个拖动/指示器机制配合）。
- 分数缩放下的 1px 抖动（Qt 分数缩放取整，非本库范围）。
- `DockingPaneFlyoutWidget::updateCursor()`、`DockingPaneGlowWidget` 中的 `QCursor::pos()`（无事件对象可依赖，宿主已强制 xcb）。
- 库内遗留：Tab 子窗格命中顺序依赖列表顺序（`updateFloatingPane`）、`DockFeatures` 缺 Floatable/Movable 特性位（见 `third_party/DockingPanes/BUGS.md` 第三节）。
