# appfw 配置子系统设计（2026-09-11 审查轮）

代码：`src/fw/appfw/sdk/vine/appfw/ConfigManager.hpp`、`ConfigRegistry.hpp`、`ConfigItem.hpp`、
`ConfigCategory.hpp`、`ConfigGroup.hpp`、`ConfigStandard.hpp` 与 `src/fw/appfw/src/` 下的同名 `.cpp`，
视图层 `src/fw/appfw/sdk/vine/appfw/gui/ConfigWindow.hpp` / `src/fw/appfw/src/gui/ConfigWindow.cpp`。
测试：`tests/test_gui/test_gui.cpp`（`ConfigManager_*` / `ConfigItem_*` / `ConfigRegistry_*` /
`ConfigWindowTest.*`）。

## 三层分工

| 层 | 类型 | 存什么 | 谁写 |
| --- | --- | --- | --- |
| 值 | `ConfigManager` | key → String/bool/int/double 及其数组 | 宿主、插件（运行期） |
| 描述 | `ConfigRegistry`（`ConfigCategory` → `ConfigGroup` → `ConfigItem`） | 显示树、类型、默认值、range、选项、owner | 插件注册期（`PluginLoadContext::registerConfigItem`） |
| 视图 | `gui::ConfigWindow` | 由描述树生成的编辑器，值来自 `ConfigManager` | 宿主 UI（`ShowConfigWindowCommand`） |

`Application` 各持有一个 `ConfigManager` 与 `ConfigRegistry`；`Plugin::configItems()` /
`PluginLoadContext::registeredConfigs()` 是插件查询自己那部分描述树的入口。

## 契约（本轮固化）

### 值层：`ConfigManager`

- **存储类型**就是 API 能表达的类型：`String`/`bool`/`int`/`double` 及 `vector` 版本。
  int 就是 `int`（早先内部存 `int64_t`，但公开访问器只有 `int`，那段容量只能由 JSON 进入，
  等于死容量；现在由 JSON 进入的超范围整数在装载时夹取并记 warning）。
- **`changed` 只在值真的变化时发**：`set*` 写同值、`remove` 不存在的键、`clear` 空配置、
  `loadJson` 装载内容相同的配置都不发。处理函数里把值归一化回同值不会自激。
- **空 key = “整个配置变了”**：`clear()`，以及真的替换了内容的 `loadJson()`/`load()`。
  单个键的事件始终带该 key（空 key 因此有了明确含义，见 `ConfigChangedEventArgs` 的注释）。
- **读取永不抛异常**：键不存在或存储类型与访问器不符，一律返回调用方给的默认值。
- **线程安全**：内部 `shared_mutex`；写者独占、读者共享；`changed` 在**释放锁之后**触发，
  所以处理函数可以安全回调管理器。**订阅/退订本身无锁**（`vine::Signal` 无锁），启动期接线。
- **保存**（`save`）：`QSaveFile` 原子写（临时文件 + rename 覆盖目标），`open`/`write`/`commit`
  每一步的结果都检查；失败时磁盘上的旧文件保持不变，返回 false 一定是真的没写成。
  父目录必须存在（由宿主负责创建，`Application::shutdown` 已如此）。
- **加载**（`loadJson`/`load`）：解析在锁外完成，成功后整体替换；文本不是 JSON 对象才返回 false。
  不符合 `{"type":...,"value":...}` 形状的条目跳过并记 warning（手写文件里一个 typo
  不能把一份配置静默吃掉）。
- **JSON 表示**：整数写成 JSON 整数，API 能表达的值都无损往返。

### 描述层：`ConfigRegistry`

- **key 全树唯一**：`ConfigGroup::addItem` 通过 `ConfigRegistry::item()` 查重，不是组内唯一。
- **owner 是缓存**：`addItem` 记录；`owner` 为空时清除（重新注册不能把旧 owner 留给别人）；
  删除路径（`removeItem`/`removeCategory`/`clear`/`removeItemsForPlugin`）清理；
  `itemsForPlugin` 本身跳过已不在树上的项，所以任何删除路径都不会留下误判所有权。
- `itemsForPlugin` 的顺序是 **key 升序**（内部按 key 收集）。
- **生命周期 / 线程**：查询返回指向树内节点的裸指针，节点被删即失效；不做同步，
  注册在启动与插件装载期完成，之后只读。

### 视图层：`gui::ConfigWindow`

- **构造时对描述树取快照**：构造之后注册的项不会出现在已有窗口里（要重建窗口），
  但**编辑器与 key 成对保存**、`refresh()`/`reset()` 按 key 查描述树，
  所以“注册表遍历顺序变了”再也不会把 A 的值塞进 B 的编辑器（旧实现按下标访问平行数组）。
- **Choice 编辑器**：显示存储值；键从未写过时显示 `ConfigItem` 的默认值（即 `reset()` 会恢复的
  那个）；两者都匹配不到选项时显示**未选中**，而不是谎报第 0 项。
- **数值项未声明 range 时不夹取**：int 用整个 `int` 范围，double 用 ±1e15；
  声明了 range 才受 range 约束（旧实现对无 range 的项用 0..1000000，会把负值悄悄改成 0）。
- 编辑立即写回 `ConfigManager`（`changed` 由值层发）；`refresh()` 期间屏蔽编辑器信号，
  不会把“刷新”写成一次修改。

## 本轮审查（15 点：缺陷 → 修复）

| 编号 | 缺陷 | 证据 | 修复 |
| --- | --- | --- | --- |
| C1 | `save()` 丢弃 `QFile::write` 结果，写失败仍返回 true | 探针：`save(/dev/full)` 返回 1；磁盘满时 `Application::shutdown` 的告警永不触发 | `QSaveFile` + 检查 `open/write/commit`，见 `ConfigManager::save` |
| C2 | 就地截断写，失败/崩溃留下半份 JSON | 同上；`writeRegistrationFile` 早已用 tmp+rename | 同上（临时文件 + rename） |
| C3 | `loadJson()`/`load()` 整体替换不发 `changed` | 探针：装载新键后事件数 0 | 内容真的变了就发一次空 key 事件 |
| C4 | 同值写入也发 `changed` | 探针：同值 `setInt` 事件 1→2 | `assignValue` 只在真变化时返回 true |
| C5 | `ConfigItem::step()` 注释与实现不符（“no range” vs “no step”） | 代码 + `ConfigItem_RangeAny` | 修注释，并说明 range 会重置 step |
| C6 | `owners_` 在删除路径不更新，可残留错误所有权 | 删分类后重新注册且不声明 owner：`itemsForPlugin` 仍返回该项 | `pruneOwners()` + 空 owner 清除 + 删除路径清理 |
| C7 | `ConfigWindow` 编辑器按下标与遍历顺序配对，注册表一变就显示错值 | 探针：注册 order=-1 的分组后，key=a 的编辑器显示 key=z 的值 | 编辑器与 key 成对，`refresh/reset` 按 key 驱动；`ConfigWindowTest.RefreshKeepsEachValueWithItsKey` |
| C8 | Choice 存储值不在选项里时显示第 0 项且不回写 | 代码 `idx >= 0 ? idx : 0` | `choiceIndex()`：不匹配返回 -1；`ConfigWindowTest.ChoiceWithoutMatchShowsNoSelection` |
| C9 | 内部 `int64_t` 但公开只有 `int`，且整数写成 double，“无损往返”不成立 | 探针：`1234567890123456789` 回写成 `...800` | 存储与 JSON 都用 `int`，整数按 JSON 整数写；超范围夹取 + warning |
| C10 | 畸形 JSON 条目静默丢弃 | 代码：`continue` / “unknown type: skip this key” | 两类跳过都 `V_LOGW` 带 key；`ConfigManager_BadEntriesAreIgnoredNotFatal` |
| C11 | 空 key 语义未文档化（与合法空键冲突） | 头文件原本只写“carrying the key” | `ConfigChangedEventArgs` 文档写明空 key = 整体变化 |
| C12 | `save()` 不建父目录，与插件注册写入器不一致 | 代码对比 | 文档写明父目录由宿主创建（保持既有的 `Application` 行为） |
| C13 | 描述树无生命周期/线程契约（裸指针 + 无同步） | 头文件缺注释 | 三个类都补 `@note Lifetime` / `@note Threading` |
| C14 | 缺少本设计文档 | `.ai/design/` 里另三个 appfw 模块都有 | 本文 |
| C15 | 未声明 range 的 int/double 编辑器夹取显示值（负值变 0） | 收尾审查时发现：`setRange(0, 1000000)` | 无 range 时不约束；`ConfigWindowTest.UnboundedNumbersAreNotClamped` |

收尾还核实了一点：`PluginManager::unloadOrder()` 之前怀疑“成环会死循环”，**核实后不成立**
（无候选时退回发现顺序并 `break`）。本轮补 `PluginLifecycleTest.UnloadOrderTerminatesOnACycle`
把它固化下来。

## 不变量

1. 事件触发时值已经落定（锁外触发），处理函数回调管理器不会死锁。
2. `changed` 只在值变化时发生；空 key 只表示“整体变化”。
3. `save()` 返回 true ⇒ 文件内容完整；返回 false ⇒ 磁盘上的旧文件没被破坏。
4. `loadJson()` 返回 true ⇒ 存储已被整体替换（可能内容与原来相同，此时不通知）。
5. 编辑器与 key 一一对应，只显示“该 key 的存储值”或“该 item 的默认值”。
6. `itemsForPlugin(p)` 返回的每一项都真实存在于树上且 owner 是 `p`。

## 测试映射（tests/test_gui/test_gui.cpp）

| 用例 | 覆盖 |
| --- | --- |
| `ConfigManager_Basic` | 值、数组、层级 key、JSON 往返、宿主单例 |
| `ConfigManager_SaveReportsFailureAndKeepsTheOldFile` | C1/C2：成功可读回、不留临时文件、失败三种路径、失败时旧配置完好 |
| `ConfigManager_NotifiesOnlyOnRealChanges` | C3/C4/C11：同值静默、loadJson 通知与幂等、clear 语义 |
| `ConfigManager_JsonRoundTripKeepsTypes` | C9：类型无损、旧格式兼容、超范围夹取 |
| `ConfigManager_BadEntriesAreIgnoredNotFatal` | C10：坏条目忽略、好条目生效、非 JSON 失败 |
| `ConfigManager_ChangedEvent` | 事件载荷与 key |
| `ConfigItem_Descriptor` / `DefaultTypeCheck` / `RangeAny` / `TypedChoices` | 描述器与 fluent builder 契约（含 `step()` 默认值） |
| `ConfigRegistry_Register` / `MetaAndOrder` / `StandardCategories` / `Ownership` | 树结构、排序、标准分类、owner |
| `ConfigRegistry_OwnershipDoesNotOutliveTheItem` | C6：删除路径清理 owner、空 owner 清除 |
| `ConfigWindowTest.RefreshKeepsEachValueWithItsKey` | C7 |
| `ConfigWindowTest.ChoiceWithoutMatchShowsNoSelection` | C8 |
| `ConfigWindowTest.UnboundedNumbersAreNotClamped` | C15 |
| `PluginLifecycleTest.UnloadOrderTerminatesOnACycle` | 插件侧收尾：成环输入必须终止 |

## 已评估但**未采纳**（留档，避免重复建议）

- **不给 `ConfigManager` 加“存储类型查询”API**（`type(key)`）：Choice 编辑器在“存储值类型与选项
  类型不一致”时，仍按各选项自身的类型读取后近似匹配。为一个 GUI 细节扩公共 API 不划算；
  真要做，前提是先有类型访问器。
- **不 fsync**：`QSaveFile` 保证“要么旧内容要么新内容”，不保证崩溃后已落盘，
  与 `PluginManager` 的注册文件写入器同级（进程崩溃罕见，写入原子性才是要紧的）。
- **不给 `getStringArray` 等补默认值参数**：保持既有签名（数组语义下“默认值”意义不大）。
- **超范围整数夹取而不报错**：报错会让整份配置装载失败，夹取 + warning 更可控。
- **`ConfigItem` 的 `std::any` 保持**：`@throws std::bad_any_cast` 是已公开的契约，
  换成 `std::optional` 会改掉这个异常类型。
- **不在 `ConfigWindow` 里监听注册表变化自动重建**：窗口是快照，重建由宿主决定（`refresh()`
  已保证“快照之外的变化不会污染已有编辑器”）。
