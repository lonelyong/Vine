# appfw 插件系统设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：插件发现/加载/卸载、依赖、配置持久化、元数据与 ABI 握手。
> **权威来源**：`Plugin.hpp`、`PluginManager.hpp`、`tests/test_appfw`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

`Plugin::unload()` 声明存在却没有调用点；`PluginManager` 只有 `load()`/`loadAll()`，没有禁用/卸载；
关闭路径只停了 EventBus，插件的窗口/订阅没有反初始化。插件元数据也缺少联系方式与图标。

### 1.2 目标

- 发现与加载分离：扫描/读元数据不实例化。
- 禁用/跳过/卸载三条轴语义清楚，依赖反序卸载。
- 配置持久化、插件数据目录、安装注册文件。
- `PluginInfo` 带 email/repo/icon；ABI 握手。

### 1.3 非目标

- 不做运行时热卸载；禁用要重启生效。
- 不 `dlclose` 插件库；卸载是反初始化。
- 不做宿主侧注册回滚（关停后管理器随即析构）。
- 不做每插件库子目录/安装时复制文件（见 §9）。

### 1.4 一句话结论

发现只读元数据，加载才实例化；跳过 > BuiltIn > 注册策略 > 用户偏好；卸载按依赖反序、不 `dlclose`；
禁用只写偏好，下次启动生效。

## 2. 设计要点

### 2.1 四条轴

| 概念 | 行为 |
| --- | --- |
| 发现 | 扫描目录、加载动态库、`vinePluginQuery()` 取元数据。**不创建实例**。 |
| 加载 | `vinePluginCreate()`、跑 `preLoad/load/postLoad`、注册命令。 |
| 禁用 | 持久化用户偏好；仍被发现/列出，但永不实例化/跑生命周期/注册命令。 |
| 跳过 | 宿主进程内硬开关（`setSkipList()`，不持久化）；优先级最高，仍被列出。 |
| 卸载 | 关停时按依赖反序调 `unload()`；不改 DLL 映射。 |

判定顺序：**跳过 ⇒ BuiltIn ⇒ 注册策略 ⇒ 用户偏好**。跳过/禁用都不是可满足的依赖，
日志分别区分 `skipped by the host` / `disabled dependency` / `missing dependency`。

### 2.2 依赖与卸载顺序

- `unloadOrder(entries)` 是纯函数：只有 `loaded` 条目参与，认 `PluginInfo::dependencies` 的传递闭包；
  每轮取“剩下的插件里没有任何剩余插件依赖它”的那个。
- 不能靠列表倒序：显式 `load("child")` 后再 `loadAll()` 加载 `parent`，列表是 `[child, parent]`，
  倒序会先卸 `parent`。
- 声明成环（只有显式 `load()` 造得出来）时回退传入顺序，保证不死循环且全部卸载。
- 先出列表再调 `unload()`；单个插件抛异常只记日志，返回 false 表示“有插件抛了”。
- `DynamicLibraryLoader` 有意 `d.release()`，插件代码保持映射 ⇒ 卸载是反初始化，不是卸载代码。

### 2.3 关闭顺序

`Application::shutdown()`（protected，只有 `run()` 收尾路径调；宿主/命令/插件停进程用 `exit()`）：

1. `PluginManager::unloadAll()`：依赖反序；此时总线/CommandManager/ConfigManager/窗口都还活着。
2. `EventBus::shutdownGracefully(...)`：有界投递后停总线。
3. 配置落盘（`setConfigFile()` 设过路径时）。

循环还在跑时调、或从非应用线程调：各留一条只警告不拒绝的日志。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 发现 | 扫描 + 读元数据，不实例化 | 已落地 |
| F2 | 加载 | 创建实例、跑三阶段、注册命令 | 已落地 |
| F3 | 禁用/跳过 | 持久化偏好/进程内硬开关；仍列出 | 已落地 |
| F4 | 卸载 | 依赖反序 `unload()`；不 `dlclose` | 已落地 |
| F5 | 配置/数据目录 | `<data>/<org>/<app>/...`；每插件一目录 | 已落地 |
| F6 | 安装/注册 | `installed.d/<id>.plugin`；BuiltIn/User/AllUsers | 已落地 |
| F7 | 元数据/图标 | email/repo/icon（内联 SVG） | 已落地 |
| F8 | 管理器对话框 | 列表/详情/禁用/安装 | 已落地 |

### 3.1.1 F1 发现

- **行为**：扫描 BuiltIn/User/AllUsers 目录，加载库，`vinePluginQuery()` 取 `PluginInfo`。
- **约束**（C1）：发现不实例化；ABI 不匹配的库不进发现列表（无法安全读元数据）。
- **要求**（R1）：同名只发现一次，BuiltIn > User > AllUsers。
- **验收**（T1）：`PluginLifecycleTest` 的发现/去重用例。
- **注意**（N1）：被 ABI 拒绝的库只能靠日志定位。

### 3.1.2 F2 加载

- **行为**：`loadAll()` 做依赖解析；`load(name)` 显式加载不拦依赖但记 warning。
- **约束**（C2）：钩子是 `Task<void>`，每条出口 `co_return;`；`load()` 建控件/视图/原生窗口，必须在应用线程。
- **要求**（R2）：被禁用/跳过的插件不实例化；加载失败按 `disabled dependency`/`skipped by the host`/
  `missing dependency` 分类。
- **验收**（T2）：`PluginLifecycleTest.DisabledDependencyBlocksDependents` 等。
- **注意**（N2）：`loadAll()` 拒绝环；显式 `load()` 可造环，卸载回退传入顺序。

### 3.1.3 F3 禁用/跳过

- **行为**：`setPluginEnabled()` 只写 `plugins.disabled` 偏好；`setSkipList()` 是进程内硬开关。
- **约束**（C3）：禁用要重启生效；跳过不落盘、进程退出即消失。
- **要求**（R3）：跳过/禁用仍出现在 `pluginEntries()`，带 `enabled`/`loaded`/`skipped`。
- **验收**（T3）：`DisableIsPersistedInConfig`、`SkippedPluginStaysVisibleAndIsNeverInstantiated`。
- **注意**（N3）：BuiltIn 不可禁用/卸载；对话框改不了就不显示按钮。

### 3.1.4 F4 卸载

- **行为**：`unloadAll()` 按依赖反序调 `unload()`；幂等。
- **约束**（C4）：先出列表再调 `unload()`；单个异常不影响其余。
- **要求**（R4）：`unload()` 只释放插件自己的资源，不撤销宿主注册。
- **验收**（T4）：`UnloadOrderIsReverseDependencyOrder`、`UnloadOrderTerminatesOnACycle`。
- **注意**（N4）：不 `dlclose`；插件代码保持映射。

### 3.1.5 F5 配置/数据目录

- **行为**：`Application` 推导 `<用户数据>/<org>/<app>/{config,logs,plugins,installed.d}`。
- **约束**（C5）：平台差异全由 Qt 处理；没有中间的 `appdata` 一级。
- **要求**（R5）：持久化默认打开；`persist_config = false` 则不读不写；显式 `config_file` 优先。
- **验收**（T5）：`DefaultDataDirectoryLayout`、`ConfigFileRoundTripPersistsDisabledPlugins`。
- **注意**（N5）：`ensureDataDirectory()` 惰性创建 `<data>/plugins/<name>`；卸载/禁用不删。

### 3.1.6 F6 安装/注册

- **行为**：安装 = 写 `installed.d/<id>.plugin`；卸载 = 删文件；`path` 必需，`name/uuid/enabled` 可选。
- **约束**（C6）：身份以库里的 `PluginInfo` 为准；文件写入 tmp + rename。
- **要求**（R6）：同名不同 uuid（第一个胜）、不同名同 uuid（都留）、注册文件与库不符（库为准）都只 warning。
- **验收**（T6）：`InstallWritesRegistrationFile`、`HandWrittenRegistrationCanDisableForAllUsers`。
- **注意**（N6）：AllUsers 需要系统目录可写；普通用户失败时提示改用“仅当前用户”。

### 3.1.7 F7 元数据/图标

- **行为**：`PluginInfo` 带 `email`/`repo`/`icon`；icon 是内联 SVG 源码。
- **约束**（C7）：`PluginInfo` 必须 Qt 无关；SVG 不能内嵌到宏参数里（逗号会切参数）。
- **要求**（R7）：非法 SVG 回退默认图标；`QSvgRenderer::isValid()` 判。
- **验收**（T7）：`ManagerDialogShowsMetadataAndIcons`。
- **注意**（N7）：`VN_DECLARE_PLUGIN` 参数个数变了，所有插件必须重编。

### 3.1.8 F8 管理器对话框

- **行为**：左栏筛选/列表/加载/安装；右栏详情（QScrollArea）。
- **约束**（C8）：BuiltIn/跳过不显示禁用按钮；不可用行灰显。
- **要求**（R8）：详情显示 icon/email/repo/依赖/状态。
- **验收**（T8）：`ManagerDialogListsDisabledPlugins`、`ManagerDialogHidesToggleWhenItCannotTakeEffect`。
- **注意**（N8）：对话框只读/操作元数据，不加载插件代码本身。

## 4. 公开接口

### 4.1 `Plugin` / `PluginInfo`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `preLoad()` / `load()` / `postLoad()` / `unload()` | `Task<void>` 生命周期钩子 | 每条出口 `co_return;` |
| `PluginInfo` | uuid/name/display_name/version/description/vendor/email/repo/icon/dependencies | Qt 无关 |
| `VN_DECLARE_PLUGIN` | 声明元数据 | 参数个数变更需重编 |

### 4.2 `PluginManager`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `discover()` / `pluginEntries()` | 发现/列出 | 不实例化 |
| `load(name)` / `loadAll()` / `loadAllAsync()` | 加载 | 依赖解析/分类 |
| `unloadAll()` | 依赖反序卸载 | 幂等 |
| `setPluginEnabled()` / `isPluginEnabled()` | 用户偏好 | 重启生效 |
| `setSkipList()` / `isSkipped()` | 进程内硬开关 | 优先级最高 |
| `installPlugin()` / `uninstallPlugin()` | 注册文件 | BuiltIn 拒绝 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 发现不实例化；ABI 不匹配的库不进列表。 |
| C2 | 钩子 `Task<void>`；每条出口 `co_return;`；加载在应用线程。 |
| C3 | 禁用重启生效；跳过不落盘。 |
| C4 | 先出列表再 `unload()`；单个异常不影响其余。 |
| C5 | 平台路径由 Qt 收口；无 `appdata` 中间层。 |
| C6 | 身份以库 `PluginInfo` 为准；注册文件 tmp + rename。 |
| C7 | `PluginInfo` Qt 无关；SVG 先放常量再传宏。 |
| C8 | BuiltIn/跳过不显示禁用按钮。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 同名只发现一次，BuiltIn > User > AllUsers。 |
| R2 | 禁用/跳过不实例化；依赖失败分类清楚。 |
| R3 | 跳过/禁用仍出现在 `pluginEntries()`。 |
| R4 | `unload()` 只释放插件自己的资源。 |
| R5 | 持久化默认打开；`persist_config=false` 不读不写。 |
| R6 | 身份冲突只 warning，库为准。 |
| R7 | 非法 SVG 回退默认图标。 |
| R8 | 详情显示 icon/email/repo/依赖/状态。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 卸载是反初始化，不 `dlclose`。 |
| N2 | 禁用不是可满足的依赖；跳过也不是。 |
| N3 | `loadAll()` 拒绝环；显式 `load()` 可造环，卸载回退传入顺序。 |
| N4 | 插件数据目录键是插件名，不是库路径。 |

## 6. 注意事项与陷阱

### 6.1 SVG 图标里的逗号

- 会怎么错：`VN_DECLARE_PLUGIN` 宏参数被 SVG 里的逗号切开。
- 怎么判：编译错误/图标错乱。
- 怎么防：先把 SVG 放进 `constexpr const char8_t*` 常量再传。

### 6.2 依赖卸载顺序

- 会怎么错：按加载列表倒序卸，先卸被依赖的 `parent`。
- 怎么判：`UnloadOrderIsReverseDependencyOrder` 的合成集合。
- 怎么防：用 `unloadOrder()` 纯函数，不看列表位置。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 运行时热卸载 | 禁用/跳过/依赖/命令注册的复杂度远超收益。 | — |
| D2 | `dlclose` 插件库 | 静态工厂/元对象/vtable 被宿主长期引用。 | — |
| D3 | 宿主侧注册回滚 | 关停后管理器随即析构，回滚无意义。 | — |
| D4 | 用 `QIcon`/文件路径做插件图标 | 会把 Qt 版本耦合进 ABI。 | — |
| D5 | 安装时复制插件文件 | 现在是就地注册路径；路径失效下次启动 warning。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 发现/去重/来源优先级 | `PluginLifecycleTest` |
| T2 | 依赖/禁用/跳过加载分类 | `DisabledDependencyBlocksDependents` 等 |
| T3 | 禁用持久化/跳过可见 | `DisableIsPersistedInConfig`、`SkippedPluginStaysVisibleAndIsNeverInstantiated` |
| T4 | 卸载顺序/成环 | `UnloadOrderIsReverseDependencyOrder`、`UnloadOrderTerminatesOnACycle` |
| T5 | 目录布局/配置往返 | `DefaultDataDirectoryLayout`、`ConfigFileRoundTripPersistsDisabledPlugins` |
| T6 | 安装/注册 | `InstallWritesRegistrationFile`、`HandWrittenRegistrationCanDisableForAllUsers` |
| T7 | 元数据/图标/对话框 | `ManagerDialogShowsMetadataAndIcons` |

## 9. 状态与演进

### 9.1 当前状态

发现/加载/禁用/跳过/卸载、配置/数据目录、安装注册、元数据/图标、管理器对话框均已落地。

### 9.2 已知缺口 / 待办

- 每插件库子目录（带私有依赖）未做；install RPATH 已预留。
- 安装时复制插件文件未做（就地注册路径）。
- 被 ABI 拒绝的库不进发现列表（只能靠日志）。

### 9.3 兼容性 / 迁移

`VN_APPFW_PLUGIN_ABI_VERSION` 当前 **8u**；`VN_DECLARE_PLUGIN` 参数个数变更后所有插件必须重编。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/Plugin.hpp`
- `src/fw/appfw/sdk/vine/appfw/PluginManager.hpp`
- `src/fw/appfw/src/PluginManager.cpp`
- `src/fw/appfw/src/gui/PluginManagerDialog.cpp`
- `tests/test_appfw`、`tests/test_gui`

### 10.2 文档与 ADR

- `appfw-startup-phases.md`
- `appfw-config.md`
- `../backlog/`

### 10.3 外部资料

- `QStandardPaths` 的目录布局。
- `QSvgRenderer` 的 SVG 校验。
