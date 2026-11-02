# appfw 配置子系统设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`ConfigManager` / `ConfigRegistry` / `gui::ConfigWindow` 的三层分工与契约。
> **权威来源**：`ConfigManager.hpp`、`ConfigRegistry.hpp`、`gui/ConfigWindow.hpp`、`tests/test_gui`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

配置有三件不同的事：值（运行期读写）、描述（显示树/类型/默认值/range/owner）、视图（编辑器）。
把它们混在一层会让插件注册、宿主读写和 UI 刷新互相污染。

### 1.2 目标

- 值层 `ConfigManager`：key → 值，线程安全，变化通知。
- 描述层 `ConfigRegistry`：`ConfigCategory → ConfigGroup → ConfigItem`，key 全树唯一。
- 视图层 `gui::ConfigWindow`：由描述树生成编辑器，值来自 `ConfigManager`。
- 需要重启的策略随 `ConfigItem` 走（`restartRequired()`），“现在是否有改动待生效”随本次运行走（`Application::pendingRestartKeys()`）。

### 1.3 非目标

- 不给 `ConfigManager` 加“存储类型查询”API；不为一个 GUI 细节扩公共面。
- 不做 fsync；只保证“要么旧内容要么新内容”。
- 不在 `ConfigWindow` 里监听注册表变化自动重建；窗口是快照。
- 不在配置层重启应用：框架只回答“是否待重启”，重启得走宿主自己的关停流程（未保存的文档、插件卸载顺序都归宿主）。

### 1.4 一句话结论

配置分三层：**值**归 `ConfigManager`、**描述**归 `ConfigRegistry`、**编辑器**归 `ConfigWindow`；
`changed` 只在值真的变化时发，空 key 表示“整体变化”。

## 2. 设计要点

### 2.1 值层：`ConfigManager`

- **存储类型**就是 API 能表达的类型：`String`/`bool`/`int`/`double` 及 vector 版本。int 就是 `int`
  （早先内部 `int64_t` 是死容量；JSON 超范围整数装载时夹取 + warning）。
- **`changed` 只在值真的变化时发**：同值写入、remove 不存在的键、clear 空配置、loadJson 内容相同都不发。
- **空 key = “整个配置变了”**：`clear()`，以及真的替换了内容的 `loadJson()`/`load()`；单键事件始终带 key。
- **读取永不抛异常**：键不存在或类型不符，返回调用方给的默认值。
- **线程安全**：内部 `shared_mutex`；写独占、读共享；`changed` 在释放锁之后触发；订阅/退订无锁。

### 2.2 描述层：`ConfigRegistry`

- **key 全树唯一**：`ConfigGroup::addItem` 通过 `ConfigRegistry::item()` 查重。
- **owner 是缓存**：`addItem` 记录；空 owner 清除；删除路径清理；`itemsForPlugin` 跳过已不在树上的项。
- **框架自己也注册项**：`Application::initialize()` 用空 owner 注册框架自有的项（今天只有 `general.language`，
  界面语言），所以它们不属于任何插件，也不会被 `removeItemsForPlugin()` 带走。
- `itemsForPlugin` 的顺序是 key 升序。
- **生命周期/线程**：查询返回树内节点裸指针，节点被删即失效；注册在启动/插件装载期完成，之后只读。

### 2.3 视图层：`gui::ConfigWindow`

- **构造时对描述树取快照**；编辑器与 key 成对保存，`refresh()`/`reset()` 按 key 查树，
  不按下标配对。
- **Choice**：显示存储值；从未写过时显示 `ConfigItem` 默认值；都匹配不到时显示未选中。
- **数值项未声明 range 时不夹取**：int 用整个 `int` 范围，double 用 ±1e15。
- 编辑立即写回 `ConfigManager`；`refresh()` 期间屏蔽编辑器信号。

### 2.4 需要重启的配置：策略在描述层，状态在运行层（G1、G4、G11）

- **策略**：`ConfigItem::restartRequired()` 声明“这个 key 只在启动时被读一次”。它是描述，不是请求——登记它的模块必须保证承诺成立：运行期再读它的人会拿到新值，而其它人还跑在旧值上。
- **状态**：`Application::pendingRestartKeys()` 回答“现在有没有改动待生效”。基准是 `initialize()` 刚载入配置时的 `ConfigSnapshot`；读值从不移动基准，改回启动值就不再待生效。
- **为什么必须分两层**：只有策略能挂在 `ConfigItem` 上；把策略当状态用（“此项目前待重启”）会在没有任何改动时就报警（G1）。
- **存储不是策略**：编辑立即写回（§2.3），所以**存的是新值、窗口显示的是新值、运行行为是旧值**；`ConfigWindow` 因此用“行标签提示 + 顶部提示条”表达后果，而不是延迟写入。
- **比较的是存储值**：key 缺失与“存着默认值”算不同。`general.language` 的 `system` 与“key 缺失”是同一意图的两种写法，所以改选“跟随系统”可能报一次待重启——宁可多报，不可漏报（N6）。
- **谁提醒**：`ConfigWindow` 是今天的唯一消费者（逐行提示 + 顶部条）；宿主可以用 `pendingRestartKeys()` 自己做状态栏或退出提示。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 值存储 | `ConfigManager` 读写 String/bool/int/double 及数组 | 已落地 |
| F2 | 变化通知 | `changed` 只在值变化/整体替换时发 | 已落地 |
| F3 | 保存/加载 | `QSaveFile` 原子写；JSON 装载整体替换 | 已落地 |
| F4 | 描述树 | `ConfigRegistry` 树、key 唯一、owner 缓存 | 已落地 |
| F5 | 配置窗口 | 快照 + key 配对 + 未声明 range 不夹取 | 已落地 |
| F6 | 需要重启的配置 | `restartRequired()` 声明策略；`pendingRestartKeys()` 给状态；窗口逐行提示 + 顶部条 | 已落地 |

### 3.1.1 F1 值存储

- **行为**：`ConfigManager` 存 String/bool/int/double 及 vector 版本；`get*` 读取，`set*` 写入。
- **约束**（C1）：存储类型就是 API 能表达的类型；int 就是 `int`。
- **要求**（R1）：读取永不抛异常；类型不符返回默认值。
- **验收**（T1）：`ConfigManager_Basic`、`ConfigManager_JsonRoundTripKeepsTypes`。
- **注意**（N1）：JSON 超范围整数夹取 + warning，不失败整份配置。

### 3.1.2 F2 变化通知

- **行为**：`changed` 在值真的变化时发；空 key 表示整体变化。
- **约束**（C2）：`changed` 在释放锁之后触发；处理函数可安全回调管理器。
- **要求**（R2）：同值写入/删除不存在键/clear 空配置/loadJson 内容相同都不发。
- **验收**（T2）：`ConfigManager_NotifiesOnlyOnRealChanges`、`ConfigManager_ChangedEvent`。
- **注意**（N2）：订阅/退订本身无锁，启动期接线。

### 3.1.3 F3 保存/加载

- **行为**：`save()` 用 `QSaveFile` 原子写；`loadJson()`/`load()` 解析在锁外完成，成功后整体替换。
- **约束**（C3）：`open`/`write`/`commit` 每步检查；失败时旧文件不变。
- **要求**（R3）：父目录必须存在（宿主负责创建）；文本不是 JSON 对象才返回 false。
- **验收**（T3）：`ConfigManager_SaveReportsFailureAndKeepsTheOldFile`、
  `ConfigManager_BadEntriesAreIgnoredNotFatal`。
- **注意**（N3）：不符合 `{"type":...,"value":...}` 形状的条目跳过并 warning。

### 3.1.4 F4 描述树

- **行为**：`ConfigRegistry` 管 `ConfigCategory → ConfigGroup → ConfigItem`；key 全树唯一。
- **约束**（C4）：owner 是缓存；空 owner 清除；删除路径清理。
- **要求**（R4）：`itemsForPlugin(p)` 的每一项都真实存在且 owner 是 p；顺序 key 升序。
- **验收**（T4）：`ConfigRegistry_Register` / `MetaAndOrder` / `StandardCategories` / `Ownership`。
- **注意**（N4）：查询返回裸指针，节点被删即失效；注册后只读。

### 3.1.5 F5 配置窗口

- **行为**：构造时对描述树取快照；编辑器与 key 成对；`refresh()`/`reset()` 按 key 驱动。
- **约束**（C5）：Choice 存储值/默认值都匹配不到时显示未选中；数值项未声明 range 不夹取。
- **要求**（R5）：编辑立即写回；`refresh()` 期间屏蔽编辑器信号。
- **验收**（T5）：`RefreshKeepsEachValueWithItsKey`、`ChoiceWithoutMatchShowsNoSelection`、
  `UnboundedNumbersAreNotClamped`。
- **注意**（N5）：窗口是快照，构造后注册的项要重建窗口才出现。

### 3.1.6 F6 需要重启的配置

- **行为**：带 `restartRequired()` 的项在行标签后加“（重启后生效）”；当 `pendingRestartKeys()` 非空时，窗口顶部显示一条“有改动需要重启才能生效”。
- **约束**（C6）：状态只在 `Application` 上，窗口每次问它——两个窗口给同一个答案，后开的窗口也知道先前改过什么。
- **要求**（R6）：改回启动值后不再待生效；不需要重启的 key 永远不进列表；列表为空时提示条隐藏。
- **验收**（T8）：`ConfigTest.OnlyRestartRequiredItemsCanBePending`、`ConfigTest.TheBaselineIsThisRunNotTheProcess`、
  `GuiTest.ConfigWindowMarksRestartRequiredItemsAndReportsThePendingChange`。
- **注意**（N6）：比较的是存储值，不是效果；`system` 与 key 缺失算不同。

## 4. 公开接口

### 4.1 `ConfigManager`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `getString/getBool/getInt/getDouble` 及数组版 | 读取；永不抛 | 类型不符返回默认值 |
| `set*` | 写入；值变化才发 `changed` | 同值静默 |
| `clear()` / `remove(key)` | 整体/单键删除 | 空配置 clear 不发 |
| `save()` / `loadJson()` / `load()` | 原子写/整体替换 | 父目录由宿主创建 |
| `changed` | `Signal<ConfigChangedEventArgs>` | 空 key = 整体变化 |
| `snapshot()` | 拷贝当前值，与后续写入无关 | 值层；`ConfigSnapshot` 自己不知道 item |

### 4.2 `ConfigRegistry`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `addCategory/addGroup/addItem` | 建树；key 全树唯一 | owner 缓存 |
| `item(key)` / `itemsForPlugin(p)` | 查询 | 裸指针；key 升序 |
| `restartRequiredKeys()` | 带重启策略的 key，按名排序 | 只答策略，不答状态 |
| `removeItem/removeCategory/clear` | 删除并清理 owner | 节点失效 |

### 4.3 `ConfigItem` / `ConfigWindow`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| fluent builder（`description` / `range` / `choices` …） | 描述项 | 见 `ConfigItem.hpp` |
| `restartRequired()` / `restartRequired(bool)` | 声明/读取重启策略 | 与 `readOnly()` 同形 |
| `gui::ConfigWindow` | 由描述树生成编辑器 | 快照 + key 配对 |
| `Application::pendingRestartKeys()` | 与启动快照不同的、带策略的 key | 状态随本次运行 |
| `ConfigSnapshot::divergedKeys()` | 两组值在给定 key 上的差异 | 比较两个快照 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 存储类型就是 API 能表达的类型；int 就是 `int`。 |
| C2 | `changed` 在锁外触发；订阅/退订无锁。 |
| C3 | `save()` 每步检查；失败时旧文件不变。 |
| C4 | key 全树唯一；owner 是缓存，删除路径清理。 |
| C5 | Choice 不匹配显示未选中；未声明 range 不夹取。 |
| C6 | 重启策略在描述层（`ConfigItem`），待重启状态在运行层（`Application`）；任一单方不能代替另一方。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 读取永不抛异常；类型不符返回默认值。 |
| R2 | 同值/删除不存在键/clear 空配置/loadJson 内容相同都不发通知。 |
| R3 | 父目录由宿主创建；非 JSON 对象才返回 false。 |
| R4 | `itemsForPlugin(p)` 每项真实存在且 owner 是 p。 |
| R5 | 编辑立即写回；`refresh()` 屏蔽编辑器信号。 |
| R6 | 改回启动值就不待重启；无策略的 key 永不出现在 `pendingRestartKeys()`；无待生效项时窗口不显示提示条。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 事件触发时值已经落定；处理函数回调管理器不会死锁。 |
| N2 | `save()` true ⇒ 文件完整；false ⇒ 旧文件没被破坏。 |
| N3 | `loadJson()` true ⇒ 存储已整体替换（内容相同则不通知）。 |
| N4 | 编辑器只显示“该 key 的存储值”或“该 item 的默认值”。 |
| N5 | 窗口是快照：构造后注册的项不会凭空出现。 |
| N6 | 待重启比较的是存储值而非效果：同一意图的两种写法（`system` / key 缺失）算不同。 |

## 6. 注意事项与陷阱

### 6.1 空 key 的语义

- 会怎么错：把空 key 当合法键，和“整体变化”混淆。
- 怎么判：`ConfigChangedEventArgs` 的 key 为空。
- 怎么防：空 key 只表示整体变化；单键事件始终带 key。

### 6.2 未声明 range 的数值

- 会怎么错：旧实现对无 range 的项用 0..1000000，负值被悄悄改成 0。
- 怎么判：`ConfigWindowTest.UnboundedNumbersAreNotClamped`。
- 怎么防：int 用整个 `int` 范围，double 用 ±1e15；声明了 range 才夹取。

### 6.3 把存储值当运行值（P3）

- 会怎么错：以为改了存储值就等于行为变了。实际是**存的是新值、跑的是旧值**，直到下次启动；反过来，用“运行值”去刷编辑器又会让用户以为自己没改成。
- 怎么判：同一 key 在窗口里显示的值与行为不一致；或 `pendingRestartKeys()` 为空但用户刚改过一项。
- 怎么防：编辑器永远显示存储值（N4）；后果只用提示表达（F6）；“运行值”只在 `Application` 内部存在，不对外显示。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | `ConfigManager::type(key)` | 为一个 GUI 细节扩公共 API 不划算。 | — |
| D2 | fsync | `QSaveFile` 已保证旧/新二选一；写入原子性才是要紧的。 | — |
| D3 | 给 `getStringArray` 补默认值参数 | 数组语义下“默认值”意义不大。 | — |
| D4 | 超范围整数报错 | 会让整份配置装载失败；夹取 + warning 更可控。 | — |
| D5 | `ConfigItem` 的 `std::any` 换 `optional` | 会改掉已公开的 `bad_any_cast` 契约。 | — |
| D6 | `ConfigWindow` 监听注册表自动重建 | 窗口是快照，重建由宿主决定。 | — |
| D7 | 把“需重启”写进每个 item 的 `description()` | 每个当事人各写一遍，漏一个就静默失效；tooltip 要悬停才看得到（G7）。 | — |
| D8 | 延迟写入（重启前一直存旧值） | 窗口会显示旧值，用户以为没改成；需要草稿层与崩溃时的取舍（G7）。 | — |
| D9 | 框架自己重启进程 | 未保存的文档与插件卸载顺序是宿主的责任（G10）。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 值/数组/层级 key/JSON 往返 | `ConfigManager_Basic` |
| T2 | 保存失败保留旧文件 | `ConfigManager_SaveReportsFailureAndKeepsTheOldFile` |
| T3 | 通知只在真变化时发 | `ConfigManager_NotifiesOnlyOnRealChanges` |
| T4 | 类型无损/旧格式/超范围夹取 | `ConfigManager_JsonRoundTripKeepsTypes` |
| T5 | 坏条目忽略 | `ConfigManager_BadEntriesAreIgnoredNotFatal` |
| T6 | 描述树/owner/排序 | `ConfigRegistry_*` |
| T7 | 编辑器按下标配对/Choice/无 range | `ConfigWindowTest.*` |
| T8 | 待重启只在策略项上、基于本次运行、可回退 | `ConfigTest.OnlyRestartRequiredItemsCanBePending`、`ConfigTest.TheBaselineIsThisRunNotTheProcess` |
| T9 | 窗口逐行提示 + 顶部条随值出现/消失 | `GuiTest.ConfigWindowMarksRestartRequiredItemsAndReportsThePendingChange` |

## 9. 状态与演进

### 9.1 当前状态

三层已落地；值层线程安全、原子保存、描述树与配置窗口均已测试。

### 9.2 已知缺口 / 待办

无；`RenderTarget` 枚举与 `PixelFormat` 的重复不属于本模块。

### 9.3 兼容性 / 迁移

`ConfigItem` 的 `std::any` 与 `bad_any_cast` 契约保留；`ConsoleProgressOptions::interval` 等
其它模块的迁移不在本文范围。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/ConfigManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConfigRegistry.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConfigItem.hpp`
- `src/fw/appfw/sdk/vine/appfw/ConfigSnapshot.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/ConfigWindow.hpp`
- `tests/test_gui/test_gui.cpp`
- `tests/test_appfw/ConfigTest.cpp`

### 10.2 文档与 ADR

- `appfw-plugin-system.md`
- `appfw-progress.md`
- `../backlog/`

### 10.3 外部资料

- Qt `QSaveFile` 的原子写语义。
