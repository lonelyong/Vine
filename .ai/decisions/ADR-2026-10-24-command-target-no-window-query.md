# ADR-2026-10-24：目标只走一条路 —— 不留窗口级的"当前 3D 区"查询

- 状态：已采纳（部分取代 `ADR-2026-10-24-command-target.md` 决定 4 的后半）
- 日期：2026-10-24
- 相关：`../design/appfw-render-view.md`（§9.2） · `../design/appfw-command-manager.md`（§2.0） ·
  测试 `DocumentViewTest.NoActiveDocumentMeansNoCommandTarget`、
  `DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast`、
  `ModelViewerTest.TheCommandTargetTravelsWithTheExecution`

## 背景

`ADR-2026-10-24-command-target.md` 决定 4 写的是：`MainWindow::primaryRenderControl()` **降级为派生查询**
（当前视图 → 活动窗格 → 控件），不再是插件往里写的发布槽。实现走到第 3 步时这个折中不成立了：

1. 迁移完两个真正的消费者（`ShadingModeCommand`、`TestRenderLiveCommand`）之后，`src/` 里**零调用者**；
2. 它和 `gui::resolveCommandTarget()` 是同一条遍历（中央区 → 文档宿主 → 当前视图 → 控件）的**第二份实现**，
   差别只在"答案归窗口"还是"答案归这次执行"；
3. 而"答案归窗口"恰恰是被否决的那一半：一个窗口出现第二个 3D 区时，窗口级的单值回答必然错一个，而执行级的目标
   天然各答各的。

同一时期还有一个命名问题：那个"视图回答哪块 3D 区"的访问器最初叫 `activeRenderControl()`，与同一个类里的
`activate()` / `deactivate()`（**上屏/下屏**）同词不同层，而"活跃"在仓库里已经是文档层的词
（`DocumentManager::current()`）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 保留 `primaryRenderControl()` 作派生查询 | 非命令代码（面板、状态栏）少写三行 | 同一个问题两条路：命令可以绕过目标去问窗口，窗口级回答在第二个 3D 区出现时就是错的；今天没有任何调用者 |
| B. 保留但标 `[[deprecated]]` 并保留一个 ABI 符号 | 形式上前向兼容 | ABI 9 尚未发布，没有"已发布的插件"需要兼容；留一个只为兼容而存在的符号是纯负债 |
| **C. 整个删除，只留执行目标（选定）** | 一个问题一条路；"没激活文档 = 没有目标"成为唯一且可推导的答案 | 非命令代码要自己解析（`gui::resolveCommandTarget(window)`）；多窗口时解析输入仍不完整 |
| D. 访问器继续叫 `activeRenderControl()` | 名字里带"多窗格里选哪块" | 一个类里第三个"active"含义；两个插件本来就有同签名的 `renderControl() const`，多出来的是重复品 |
| E. 访问器叫 `renderControl() const`（选定） | 与两个插件已有的 getter 同名同义，多出来的访问器连同名字一起消失；"active"只属于文档层 | 名字不再暗示"多窗格里哪一块"，改由契约文字说明 |

## 决定

1. **不留窗口级查询**：`MainWindow::setPrimaryRenderControl()` 与 `primaryRenderControl()` 都删除。
   "哪块 3D 区"只有两个出口：**派发时的解析**（`gui::resolveCommandTarget()`，结果随执行走）与
   **视图自己的回答**（`DocumentView::renderControl()`）。
2. **视图的访问器是 `renderControl() const`**，默认返回 null（2D 视图 = 没有 3D 区）；覆盖它只是把"哪一块"
   说出来，不引入新状态。多窗格时由视图自己回答"用户最后在哪块里干活"。
3. **"没激活文档 = 没有目标"** 是唯一规则：没有当前文档、中央区不是文档宿主、或视图没有 3D 区，解析结果都是空目标，
   `CommandFlags::NeedsDocument` / `NeedsView` 据此统一拒绝。

## 后果

- 好的：命令不可能"绕过目标去问窗口"——那条路不存在；`renderControl()` 与两个插件已有的 getter 合一，虚表条目
  没有增加新的概念；删除后 ABI 表面更小（`MainWindow` 少一个方法）。
- 坏的 / 代价：非命令代码想要"当前那块 3D 区"必须自己调 `gui::resolveCommandTarget(window)`，这是**有意的摩擦**
  ——它逼调用者说清"这是给谁解析的"。多窗口仍是已知缺口：解析输入还没有发起者所在的窗口（设计文档 §9.2 记着）。
- 触发条件（什么时候重开）：出现第一个**非命令**的消费者（例如状态栏要显示"当前 3D 区信息"），或第二个窗口落地
  需要把"发起者的窗口"作为解析输入。

## 证据

- 代码：`src/fw/appfw/sdk/vine/appfw/gui/CommandTarget.hpp`（解析与唯一那次 cast）；
  `src/fw/appfw/sdk/vine/appfw/gui/DocumentView.hpp`（`renderControl() const` 的契约）；
  `src/fw/appfw/src/CommandManager.cpp`（拒绝在接管与串行门之前）。
- 测试：`DocumentViewTest.AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast`（目标带视图、视图答窗格）、
  `ModelViewerTest.TheCommandTargetTravelsWithTheExecution`（目标经真实派发到达命令体，缺目标时拒绝先于命令体）。
- 门禁：`python3 scripts/check_ai_docs.py --strict`、`python3 scripts/check_design_docs.py --strict` 均为 0。
