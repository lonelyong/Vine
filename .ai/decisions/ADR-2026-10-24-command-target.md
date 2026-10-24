# ADR-2026-10-24：命令的目标是「视图」，不是文档、也不是渲染控件

- 状态：已采纳（设计定稿；实现分四步，见「实现顺序」）
- 日期：2026-10-24
- 后续：决定 4 的后半（保留 `primaryRenderControl()` 派生查询）与"活动窗格"的命名已被
  `ADR-2026-10-24-command-target-no-window-query.md` 取代；其余决定仍然有效。
- 相关：`../design/appfw-render-view.md` · `../design/appfw-command-manager.md` ·
  `../design/appfw-document-model.md` · `../backlog/graphics.md`（T3）

## 背景

今天命令找"它要改的那一块 3D 区"只有一条路：`MainWindow::current()`（一个全局静态，最近活跃的主窗）+
`MainWindow::primaryRenderControl()`（一个**单槽**，由 `ModelRenderView` / `DemoView` 在**上屏时写入、下屏时清空**）。
九个调用点依赖它，`ModelRenderView::isActive()` 还拿 `primaryRenderControl() == control_` 当身份判断。

它坏在四处，且都不是"接线问题"：

1. **单槽**：两个视图同时可见（一个视图内两个窗格、或两个文档并排）时，后激活者占槽，另一个的命令与身份判断全错；
2. **用 GUI 门面当身份**：`RenderControl` 自己的头注释写明是 "the widget-side wrapper"（真正的 surface/engine/view/viewer 在私有
   `SurfaceWindow` 里）；门面可以有两个实例指向同一处（把一页弹成独立窗口），指针相等就不再是"同一块区"；
3. **粒度止于控件**：一个视图里两个窗格 = 两个 `RenderControl`，而 `primaryRenderControl()` 只能指一个；命令以控件为目标时，
   "哪个窗格"永远没人回答；
4. **发起者信息在派发时丢失**：`RibbonAction::triggered → commandManager()->executeDetached(name)` 只传命令名，UI 明明知道谁触发的。

外部框架的共同答案也指向同一处：Blender 把 `context`（含 active area/region）**显式传进**每个 operator；Qt Creator 用
widget 层级上的 `IContext` 栈回答"当前上下文"，动作按 context 启用；VS Code 的命令由**调用处**带参数、
贡献点声明 `when` 条件。三者都没有"命令自己去找当前窗口"。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 目标是**文档**（`DocTarget`） | 贴近"打开/保存/撤销"这类内容命令 | 一文档两视图/两窗格时答不出"哪一处"；而视图知道自己的文档（`DocumentView::document()`），反向不成立 |
| B. 目标是 **`RenderControl`**（≈今天的做法） | 零新概念 | GUI 门面当身份；资源所有权与呈现耦合；粒度止于控件 |
| C. 目标是 **`SceneView`**（图形层） | 答得出"哪块画" | appfw 命令还需要文档；答不出视图内部的焦点窗格 |
| D. 维持全局单槽 | 不动 | 上述四条全部保留 |
| **E. 目标是 `DocumentView`（视图），层可空、需求按命令声明（选定）** | 在能回答"哪一处"的最细处携带；文档由视图推；窗格归视图所有，框架不必知道窗格 | 一次插件 ABI bump；多引入一个目标类型 |

## 决定

1. **目标是 `DocumentView`**：命令作用于"某一处的某一页"，而不是"那份内容"。图形层对象（`RenderControl` / `SceneView` /
   `Viewer`）从它取；`RenderControl` 保留为"3D 区的 widget 宿主"，**不做**目标。
2. **三层都可空，命令声明所需层**：`CommandTarget{document, view}`；`CommandFlags` 增加
   `NeedsDocument` / `NeedsView`，框架在派发时**统一拒绝**并说清缺什么。控制台、无 GUI 测试、批量运行是**合法情形**：
   文档级命令照常跑，视图级命令得到"需要 3D 视图"的明确结果（今天每个视图级命令各写一遍空链）。
3. **目标在派发时解析**：发起控件（焦点 / 父链）优先；无发起者 → 当前文档 → 其视图 → 视图的**活动窗格**；再没有 → 空。
   解析只有一处实现，失败报一次。
4. **`primaryRenderControl()` 降级为派生查询**（当前视图 → 活动窗格 → 控件），不再是插件往里写的发布槽；`isActive()`
   改为"我是不是命令的目标"，不再拿全局槽做指针比较。
5. **视图内部的多窗格是视图自己的状态**：框架不建窗格注册表、不做布局与焦点路由（沿用 `appfw-render-view.md` §6.4）。
6. **一次 ABI bump**：`CommandExecutionContext` 追加虚函数、`CommandInfo` 追加字段，两者都在插件边界上，因此
   `VN_APPFW_PLUGIN_ABI_VERSION` 8 → 9 一次完成，不拆成多次小 bump。

## 后果

- 插件不再写 `setPrimaryRenderControl`；`ModelRenderView` / `DemoView` 的发布代码删除，`isActive()` 改写。
- 视图级命令少一段重复的空链判断；控制台/无 GUI 行为由框架统一，不再各写一句文案。
- 首次出现"一视图两窗格"时只需要视图自己暴露活动窗格，框架不改。
- 插件必须重编（ABI 9）；未重编的插件被现有版本检查拒绝，而不是错跑。

## 实现顺序

1. 类型与解析：`CommandTarget` + 解析函数 + `DocumentView::activeRenderControl()` + `primaryRenderControl()` 派生 +
   `NeedsDocument` / `NeedsView` + 统一拒绝（含无 GUI 单测）。**行为不变**，调用点不改。
2. 派发带目标：ribbon / 菜单 / 快捷键 / 控制台解析发起者；`CommandExecutionContext` 暴露 `target()`（同时 bump ABI）。
3. 迁移调用点：九个 `MainWindow::current()` / `primaryRenderControl()` 调用点改用目标；删除发布槽。
4. 收尾：文档与 backlog（T3 删除），两窗格一视图的端到端用例。

## 验证

- 无 GUI：有文档无视图时，`NeedsDocument` 命令成功、`NeedsView` 命令返回明确拒绝、`CommandInfo` 能提前表达"不可用"。
- 有 GUI：一个视图两个窗格，命令落到焦点窗格；`isActive()` 不再依赖全局槽。
- 回归：现有 `test_gui` 的文档视图用例（含"两份文档各自一份资源"）全绿。
