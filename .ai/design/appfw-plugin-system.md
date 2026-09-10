# appfw 插件系统设计（2026-09-10 补齐禁用与反初始化）

代码：`src/fw/appfw/sdk/vine/appfw/PluginManager.hpp`、`src/fw/appfw/src/PluginManager.cpp`、
`src/fw/appfw/sdk/vine/appfw/Plugin.hpp`、`src/fw/appfw/src/Application.cpp`。
UI：`src/fw/appfw/src/gui/PluginManagerDialog.cpp`。
测试：`tests/test_gui/test_gui.cpp`（`PluginLifecycleTest` 19 例，含目录布局、卸载顺序、插件数据目录、安装注册文件、来源作用域）、
`tests/test_vsg/GfxBackendVsgPluginTest.cpp`（配置持久化往返 1 例）。

## 本次修改的出发问题

1. `Plugin::unload()` 声明存在、两个插件实现了它，但**全仓没有任何调用点**——
   `Plugin.hpp` 的类注释却承诺"unload() on shutdown"。即：关停时的反初始化是
   写了没接线的死代码。
2. 缺"禁用插件"能力：`PluginManager` 只有 `load()`/`loadAll()`，没有
   `unload`/`disable`；`ConfigRegistry::removeItemsForPlugin` 也只有测试在用。
3. 关闭路径（`Application::run()` / `GuiApplication::run()`）只做了总线优雅停机，
   注释里写的"before the subscribers (windows, plugins) start to be torn down"
   只兑现了总线那一半。

## 语义：发现（discovery）与加载（loading）分离

| 概念 | 行为 |
| --- | --- |
| 发现 | 扫描插件目录、加载动态库、调用 `vinePluginQuery()` 取元数据。**不创建实例**。 |
| 加载 | 创建实例（`vinePluginCreate()`）、跑 `preLoad/load/postLoad`、注册命令。 |
| 禁用 | 持久化的用户偏好。被禁用的插件**仍被发现**（元数据/库路径可见），但**永不加载**：不实例化、不跑生命周期、不注册命令。 |
| 跳过（skip list） | **宿主的进程内硬开关**（`setSkipList()` 等，静态 API，不持久化）。被跳过的插件**仍被发现并列出**（`PluginEntry::skipped = true`），但永不实例化；优先级高于一切其它输入。 |
| 卸载 | 关闭时对**已加载**插件调用 `unload()`，按依赖反序（依赖方先）。不改 DLL 映射（见下）。 |

由此推出的不变式（均有用例固定）：

- **禁用要重启生效**：`setPluginEnabled()` 只写偏好，不动运行态。已加载的插件继续
  运行到进程结束；被禁用的插件在下一次 `loadAll()`（即下次启动）才不加载。
  这避免了"运行时热卸载"带来的一整套问题（见文末"已评估但未采纳"）。
- **禁用的插件不是可满足的依赖**：`loadAll()` 做依赖解析时，被禁用的插件不进
  `available` 集合，因此依赖它的插件不会被加载，并且在日志里被区分为
  `disabled dependency`（不是 `missing dependency`）——修复方式不同（去启用 vs 去安装）。
  用例：`PluginLifecycleTest.DisabledDependencyBlocksDependents`（test_plugin 依赖
  app_shell，禁用 app_shell ⇒ `loadAll()` 返回 false 且两者都不加载）。
- **禁用的插件仍显示插件信息**：`pluginEntries()` 返回全部被发现的插件
  （`PluginInfo` + 库路径 + `enabled` + `loaded`），`libraryPath()` 对
  未加载的插件同样有效。插件管理器对话框据此列出并灰显禁用项。
- **`load()` 拒绝被禁用的插件**：即使显式给出库路径也不实例化（否则"禁用"形同虚设）。
- **宿主跳过（skip list）是另一条轴，优先级最高**：`setSkipList()` 不落盘、进程退出即消失，
  所以它承载的是"这一次运行不要这个插件"（headless / safe mode / 启动参数），而不是用户偏好。
  它必须能盖过 `BuiltIn`（用户根本无权禁用自带插件）⇒ 判定顺序是
  跳过 ⇒ BuiltIn ⇒ 注册策略 ⇒ 用户偏好（`isPluginEnabled()`）。
  插件管理器对话框据此决定**是否提供**禁用/启用按钮：改不了就不显示（BuiltIn 与跳过），
  而不是显示一个按下去没反应的灰按钮。
- **跳过不是"看不见"**：被跳过的插件照常进入发现列表（元数据、库路径、`PluginEntry::skipped`），
  这样 UI/日志能解释"为什么它没跑"，而不是凭空消失。因此 `isSkipped()` 的判定在发现之后，
  不再在 `rememberDiscovered()` 之前 `continue`。
- **跳过的插件也不是可满足的依赖**：与禁用同样不进 `available`，但在日志里区分为
  `skipped by the host`（修复方式是让宿主别跳过，而不是启用或安装）。
- **跳过不卸载已加载的插件**：`removeFromSkipList()` 后下一次 `loadAll()` 就能加载，
  不需要重启（与禁用相反）。

## 卸载与关闭顺序

`Application::shutdown()`（`Application.cpp`）是 `run()` 与 `GuiApplication::run()`
共用的关闭序列，顺序有语义：

1. `PluginManager::unloadAll()` —— 按**依赖反序**调用 `Plugin::unload()`。此时**总线、
   CommandManager、ConfigManager、窗口都还活着**，插件可以正常收尾、最后一次发布
   事件或摘掉自己的订阅。
2. `EventBus::shutdownGracefully(...)` —— 投递退出前还在排队的事件（有界），然后停
   总线（原有逻辑）。
3. 配置落盘（`setConfigFile()` 设过路径时）。

`unloadAll()` 的性质：

- **顺序由声明的依赖算出，不看加载列表的位置**（`PluginManager::unloadOrder(entries)`，
  一个纯函数：只有 `loaded` 的条目参与，认 `PluginInfo::dependencies`（直接依赖，
  循环迭代即得传递闭包），每轮取"剩下的插件里没有任何剩余插件依赖它"的那个 ✓
  因此依赖方一定先于被依赖方 ✓✓）。
  - **为什么不能靠列表倒序**：`loadAll()` 的列表是拓扑序，但 `load()`（显式加载，如
    插件管理器对话框）不过依赖解析、直接追加到末尾 —— `load("child")` 后再
    `loadAll()` 加载它的 `parent`，列表就是 `[child, parent]`，倒序卸载会先卸
    `parent` ✗。现在无论怎么加载，结果都是 `[child, parent]` ✓（用例：
    `UnloadOrderIsReverseDependencyOrder` 的合成集合部分，"列表倒序"过不了）。
  - `load()` 本身虽然不拦依赖，但会为未加载的声明依赖记一条 warning ✓。
  - 未加载的条目、不在集合里的依赖都忽略；声明成环（只有显式 `load()` 造得出来，
    `loadAll()` 拒绝环）时回退为传入顺序，保证不死循环且全部卸载 ✓
    （实测日志：`gfx_backend_vsg` → `test_plugin` → `app_shell`，被依赖的
    `app_shell` 最后 ✓；`gfx_backend_vsg` 无依赖也无依赖方，位置任意）。
- 先出列表再调 `unload()`：插件在 `unload()` 里回调管理器时已"看不到自己"，且
  重入调用不会对同一实例调用两次 `unload()`。
- 单个插件抛异常只记日志，不影响其余插件卸载；返回 false 表示"有插件抛了"。
- **不做宿主侧注册回滚**：卸载只发生在关停路径，紧接着 CommandManager/
  ConfigRegistry 就被析构了，逐个撤销注册没有意义。`unload()` 的契约因此是
  "释放插件自己的资源（线程、worker、sink、订阅）"，不是"撤销宿主注册"。
- 幂等：第二次调用是空操作。

**DLL 永不 `dlclose`**：`DynamicLibraryLoader` 有意 `d.release()`（"keep the
process-lifetime plugin code mapped"），插件里的静态工厂、元对象、vtable 都被宿主
长期引用。所以"卸载"是**反初始化**，不是"卸载代码"。

## 配置持久化与目录布局

目录布局（`Application` 提供，宿主不传路径）：

```
<用户数据>/appdata/<organization>/<application>/
├── config/<application>.json      Application::defaultConfigFile()
├── logs/...                       宿主自己放日志（main.cpp 用 <data>/logs/vine.log）
└── plugins/<插件名>/              插件自己的文件，PluginLoadContext::dataDirectory()
```

- `<用户数据>` = `QStandardPaths::GenericDataLocation`（Linux `~/.local/share`，Windows
  `%APPDATA%`），取不到时退回临时目录。
- **org 由 `Application` 构造时默认设置**（`Application::defaultOrganizationName()` =
  `Vine`）：宿主不设也能得到合法路径；宿主自定义则用 `AppConfig::organization`
  （builder 后设，覆盖默认）。Application 只在当前 org 为空时才设，不覆盖宿主已设的值。
- **应用名由每个 app 的 main 提供**（`AppConfig::name`），框架推导其余路径。
- **builder 默认打开持久化**：`applyAppConfig()`（`src/fw/appfw/src/AppBuilderSupport.hpp`，
  `createApplication` 与 `createGuiApplication` 共用）→ `setConfigFile(config_file 非空 ?
  config_file : defaultConfigFile())`。`AppConfig::persist_config = false` 则不读不写
  （测试/短命工具）；显式 `config_file` 始终优先。
- 禁用列表存在同一个 ConfigManager 里，键 `PluginManager::disabledConfigKey()` =
  `plugins.disabled`（字符串数组，点分路径 ⇒ 嵌套 JSON `plugins.disabled`）。
- `Application::setConfigFile(path)` 打开时读回、`shutdown()` 时写回（自动建父目录）；
  空路径 = 关闭。
- 端到端验证：`VsgBackendPluginTest.ConfigFileRoundTripPersistsDisabledPlugins`
  （禁用某插件 → `run()` → 文件里读到 `plugins.disabled` → 下次启动据此解析）；
  布局验证：`PluginLifecycleTest.DefaultDataDirectoryLayout`（含"builder 已启用默认文件
  且本进程未 run() ⇒ 不落盘"）。

测试隔离：test_gui 的 GuiEnv 先 `QStandardPaths::setTestModeEnabled(true)` 再走
`createGuiApplication()`（与真实应用同一条路径），因此既不读写开发机上的真实配置，
又覆盖了 builder 的默认接线；进程从不 `run()`，所以不会有文件被写出去。

## 插件自己的数据目录（每插件一目录）

`PluginLoadContext::dataDirectory()` → `<数据>/plugins/<PluginInfo::name>`，
`Application::pluginDataDirectory()` 是该根（`<数据>/plugins`）。约定：

- **惰性创建**：只有插件真的调它才建目录（`load()` 不预先建）✓ 没这个需求的插件一个
  字节都不落盘 ✓。
- **键是插件名**，不是库路径、也不是磁盘目录名：插件被搬到别处、从用户目录安装、同一
  插件升级，拿到的都是同一个目录 ✓（这是下面"用户插件"方向能成立的前提）。
- **只放文件**：插件的配置值仍走宿主 `registerConfigItem()`/`ConfigManager` —— 这样
  禁用/启用、配置窗口、按 owner 清理都还是宿主语义 ✓（目录里的文件由插件自己管）。
- **卸载/禁用都不删**：用户数据不该因一次卸载或一次禁用丢掉；清理是策略问题，单独定
  （当前行为：保留）。
- 上下文只在生命周期调用（`preLoad/load/postLoad/unload`）期间有效：插件要在别处用这个
  路径，就在 `load()` 里存下来，或自己算
  `Application::pluginDataDirectory()/<PluginInfo::name>` ✓（`Plugin::info()` 可用）。

验证：`PluginLifecycleTest.PluginDataDirectoryIsPerPlugin`（路径组成、首次调用即创建、
两插件互不覆盖、无 Application/无插件名时返回空路径不崩、用例结束清理）。

## 插件来源与安装注册（2026-09-10 定稿）

### 三种来源（PluginScope）

| 来源 | 位置 | 谁能动它 |
| --- | --- | --- |
| `BuiltIn` | `PluginManager::builtInPluginDirectory()`（`<exe>/plugins/<app>`） | **程序**：随包发布、用 `setSkipList()` 关掉或不构建；**用户不能禁用/卸载** |
| `User` | `<用户数据>/appdata/<org>/<app>/installed.d/` | 当前用户：可安装/卸载/禁用 |
| `AllUsers` | `<系统数据根>/appdata/<org>/<app>/installed.d/` | 管理员安装，所有用户可见；用 `enabled = false` 做机器级禁用策略 |

- **程序自带插件不可禁用/卸载**：`setPluginEnabled()` 对它返回 false（并记 warning），
  `uninstallPlugin()` 找不到注册自然失败；对话框对 BuiltIn 隐藏这两个动作。理由：那是程序的
  决定（`setSkipList()` 才是程序的开关），否则用户可能把 UI 壳关掉且无法恢复。
- 优先级 **BuiltIn > User > AllUsers**：`loadAll()` 按这个顺序扫，同一个插件名只发现一次、
  只实例化一次。自带插件永远不会被用户装的同名插件顶替。

### 注册文件（installed.d/<id>.plugin）

安装 = **写一个文件**，卸载 = **删那个文件**，因此：

- 不需要跑应用，安装器/脚本/包管理器直接写 ✓；一个插件一个文件 ⇒ 没有"读-改-写"同一个列表
  的并发覆盖问题 ✓。
- 格式：`key = value`，`#`/`;` 注释；`path` 必需（库文件或目录，目录按一层平铺扫），
  `name`/`uuid`/`enabled` 可选。未知键只记 info，不报错（安装器可以放自己的记账字段）。
- **身份以插件库里的 `PluginInfo` 为准**（`uuid` + `name`），文件名/`name=` 只做校验与显示；
  不一致只记 warning。`uuid` 是 `V_DECLARE_PLUGIN` 里硬编码的 `vine::Uuid`（空 = 未声明，
  退回用名字），用来识别"两个不同插件重名"✓（扫描时 warning）。
- 写入方式是 tmp + rename（原子），并按需创建目录。
- 读回顺序：用户目录在前、系统目录在后，各自按文件 id 排序（可预测 ✓）。
- `installPlugin(path, scope)` 返回 id（库文件里只有一个插件时用插件名，否则用位置名）；
  `AllUsers` 需要系统目录可写，普通用户会失败 ⇒ UI 提示改用"仅当前用户"。
  `uninstallPlugin(id, scope)` 删除对应文件。

### 还没有做的

- 每插件一个**库子目录**（带私有依赖用）。install RPATH 已经是 `$ORIGIN;$ORIGIN/../lib`，
  届时把 `pluginLibrariesIn()` 扩一层并保留平铺兼容即可。
- 安装时复制插件文件到应用自己的目录（现在是"就地注册路径"，路径失效会在下次启动记 warning）。

## 插件元数据与图标（2026-09-10 新增）

`PluginInfo` 在 `uuid/name/display_name/version/description/vendor/dependencies` 之外新增三个可选字段，
目的是让插件管理器能"交代清楚一个插件是谁写的、去哪找人、长什么样"：

| 字段 | 用途 | 空值语义 |
| --- | --- | --- |
| `email` | 厂商联系方式；详情页渲染成 `mailto:` 链接 | 不显示该行 |
| `repo` | 仓库/问题跟踪地址；详情页是可点击链接，并有"打开仓库"按钮 | 不显示该行与按钮 |
| `icon` | **内联 SVG 源码**（不是路径、不是 QIcon） | 用宿主内置的默认图标 |

为什么用字符串而不是 QIcon/文件路径：

- `vinePluginQuery()` 是 C 入口，`PluginInfo` 必须保持 Qt 无关（塞 QIcon 会把 Qt 版本耦合进 ABI）；
- 不引入"图标文件在哪"的打包/查找问题——图标跟库走，没有外部资源；
- 渲染失败/第三方插件写了非法 SVG 时，回退到默认图标（`QSvgRenderer::isValid()` 判）。

两个实现细节（都是踩过的坑）：

1. **SVG 绝不能内嵌到宏参数里**：`V_DECLARE_PLUGIN` 是宏，圆括号外层的逗号会把参数切开，
   而 SVG 里 `stroke-dasharray="3,2"`、`rotate(90, 12, 12)` 这类逗号很常见。
   插件应该先把 SVG 放进命名常量再传：`constexpr const char8_t* s_plugin_icon = u8R"SVG(...)SVG";`
   （`app_shell` 就是这么写的，它的 SVG 专门带了一个逗号来钉住这条约定）。
2. **Appfw 因此要链 `Qt6::Svg`**（`QSvgRenderer`）。SVG 是库而不是插件，
   所以 offscreen 平台下也能渲染（用例 `ManagerDialogShowsMetadataAndIcons` 断言列表行图标非空）。
   Windows 打包含自动带上（`VineDeployQt.cmake` 跑 windeployqt）。

`V_DECLARE_PLUGIN` 的参数顺序随之变成：
`(Class, Uuid, Name, DisplayName, Version, Description, Vendor, Email, Repo, Icon, Dependencies)`
—— **参数个数变了，所有插件必须重编**（和之前加 uuid 一样；旧 `.so` 会让宿主按新布局读旧结构）。

## 插件管理器对话框（2026-09-10 重做布局）

左栏：筛选框 → 插件列表（图标 + 显示名 + 版本 + 状态后缀，不可用的行灰显）→ `加载插件…` 与
`安装插件`（下拉：仅当前用户 / 所有用户）。右栏：占位页或详情页（**放在 QScrollArea 里**，
小窗口不会把四组信息压扁）。详情页自上而下：

1. 头部：56px 图标 + 显示名（加粗放大）+ `标识 · 版本` + `厂商 · 邮箱链接` + **状态徽章**；
2. 状态说明（带边框的整句解释：为什么没在跑 / 正在跑）；
3. 动作行：`禁用/启用`、`卸载插件（作用域）`、`打开仓库`——**不能生效的一律隐藏而不是置灰**；
4. `描述` 组；
5. `信息` 组（QFormLayout）：标识 / 版本 / 来源（含可卸载与否）/ 依赖 / UUID / 库路径（等宽字体、
   可选中、带 tooltip）/ 邮箱 / 仓库；
6. `命令`、`配置` 两个表格组（各 `setMinimumHeight(120)`，否则在滚动区里会缩到只剩表头）。

底部：左侧一行反馈文字（加载/安装/卸载/启禁的结果），右侧 `刷新` `关闭`。
右键菜单与按钮同规则（不能生效的项用 `setVisible(false)` 隐藏），另加"复制库路径"。

徽章配色写死（浅色主题下的绿/橙/紫/灰）：`palette()` 角色在样式表里不可靠，
而徽章必须在深色主题下也能读作一个状态。

## 一条硬约束：一个插件库在一个进程里只能被创建一次实例

`V_DECLARE_COMMAND` 的注册会写 **vine 类型注册表**（`vine::Type`），它是进程级且**不可撤销**
的：同一个插件从**两个不同文件**（例如程序目录里的原件和用户装的拷贝）各加载一次，第二次
`vinePluginRegisterCommands()` 会因为类型重名抛异常。

因此：

- `loadAll()` 必须按**插件名**去重（已做 ✓），否则用户装一份自带插件的拷贝就会让程序起不来；
- "换路径热重载同一个插件"不是支持场景（`DynamicLibraryLoader` 也从不 `dlclose`）；
- 测试里也不能让同一个插件从两个路径各创建一次（`test_gui` 用一个固定的拷贝目录，
  `test_vsg` 只加载程序目录）。

## 关闭路径为什么必须接线（不是可选项）

`ConsoleLogRouter` 的 sink 回调捕获了宿主 `ConsolePanel` 的裸指针，唯一开关是配置
原子 `alive`；沿用的 sink 对象有意泄漏常驻 logger。`AppShellPlugin::unload()` 做的
正是把 `alive` 置 false（并摘掉 config-changed handler）。

即：**插件作者已经写好了"关停时别再碰宿主 UI"的正确代码，但此前没有任何代码调用
它**——面板随窗口销毁后，任何一行日志（析构期、后台线程、静态析构）都会写进已销毁
的 widget。接线 `unloadAll()` 后该窗口期被关闭。
（窗口与 `ApplicationData` 的销毁先后未逐行追证，故保守表述为"存在 UAF 窗口"。）

## 修正的注释与实现不符

| 位置 | 原注释 | 实际 |
| --- | --- | --- |
| `Plugin.hpp` 类注释 | "unload() on shutdown" | 当时无调用点（本次接线后成立） |
| `GfxBackendVsgPlugin.hpp` | "Removes the backend registration on unload" | 实现是空操作：`RenderBackendRegistry` 无注销 API、不持有工厂、库常驻 |
| `plugin_export.hpp`（更早一轮） | 暗示 `preLoad()` 注册命令 | 实际由 `vinePluginRegisterCommands` 注册（`preLoad()` 基类为空） |

## 测试映射

| 用例 | 固定的事实 |
| --- | --- |
| `PluginLifecycleTest.DisabledDependencyBlocksDependents` | 依赖被禁用 ⇒ 整体失败、两者都不加载、`disabled dependency` 分类 |
| `PluginLifecycleTest.SkippedDependencyBlocksDependents` | 依赖被宿主跳过 ⇒ 整体失败、两者都不加载、`skipped by the host` 分类 |
| `PluginLifecycleTest.SkippedPluginStaysVisibleAndIsNeverInstantiated` | 跳过 ⇒ 不实例化，但元数据/库路径/`skipped` 标记可见（跳过优先于用户偏好）；`removeFromSkipList()` 后无需重启即可加载 |
| `PluginLifecycleTest.DisabledPluginIsListedWithMetadataButNotLoaded` | 禁用 ⇒ 不加载/不注册命令（`test_hello` 不在注册表），但元数据与库路径可见；启用后加载 |
| `PluginLifecycleTest.DisableIsPersistedInConfig` | 写进 `plugins.disabled`、重复禁用不重复、JSON 里可见、启用后清除 |
| `PluginLifecycleTest.ConfigFileIsOptIn` | `setConfigFile` 路径语义：不存在不算错、空路径 = 不持久化 |
| `PluginLifecycleTest.DefaultDataDirectoryLayout` | `<data>/appdata/<org>/<app>/config/<app>.json` 布局 + builder 默认启用 + 不落盘 |
| `PluginLifecycleTest.ManagerDialogListsDisabledPlugins` | 对话框列表来自发现（禁用项在列，状态为"已加载 + 已禁用"），详情/刷新不崩 |
| `PluginLifecycleTest.ManagerDialogHidesToggleWhenItCannotTakeEffect` | 禁用/启用按钮只在能生效时**显示**：被宿主跳过 ⇒ 隐藏（不是灰按钮）；普通 User 插件 ⇒ 显示且可用 |
| `PluginLifecycleTest.ManagerDialogShowsMetadataAndIcons` | email/repo/icon 经 `V_DECLARE_PLUGIN` → `PluginInfo` → UI 全程贯通；未声明 icon 用内置默认 SVG（每行图标非空）；筛选框只留匹配行 |
| `PluginLifecycleTest.UnloadOrderIsReverseDependencyOrder` | 真实集合：每个已加载插件恰好一次且依赖方在前；合成集合：结论与传入顺序无关、三层链、未加载项不参与、成环不死循环 |
| `PluginLifecycleTest.PluginDataDirectoryIsPerPlugin` | 插件数据目录 = `<data>/plugins/<插件名>`（首次调用才创建、两插件不互相覆盖、无宿主/无名返回空） |
| `PluginLifecycleTest.InstallWritesRegistrationFile` | 安装 = 写 installed.d/<id>.plugin；路径不存在/空/BuiltIn 被拒；重复安装幂等；卸载 = 删文件 |
| `PluginLifecycleTest.SystemRegistrationDirectoriesAreListed` | 系统注册目录与用户目录布局一致且互不相同（只读检查） |
| `PluginLifecycleTest.HandWrittenRegistrationCanDisableForAllUsers` | 手写注册文件被读到；`enabled = false` 是策略，用户启用无效 |
| `VsgBackendPluginTest.BuiltInPluginsCannotBeDisabledOrUninstalled` | 程序自带插件 scope = BuiltIn、不可禁用、不可卸载、不能以 BuiltIn 作用域注册 |
| `PluginLifecycleTest.InstalledLocationIsDiscoveredAndDeduplicated` | 只靠注册位置也能发现；两处提供同一插件只出现一次 |
| `VsgBackendPluginTest.ConfigFileRoundTripPersistsDisabledPlugins` | `run()`/`shutdown()` 真把配置写盘并能读回（重启生效的完整链路） |

用例间有顺序依赖（`test_gui` 里禁用必须发生在"该插件从未被加载"之前；一旦某个插件库
被实例化过，它的命令就永远留在进程注册表里，因此"未注册"类断言只能用在首次加载之前），已在测试
注释里写明；`PluginLifecycleEnv`（全局环境）保证卸载发生在 GuiApplication 销毁之前。

## 必须由调用方/插件保证

- 插件的 `unload()`：幂等、不抛异常优先（抛了会被捕获记录）、不销毁宿主拥有的
  widget/窗口，不假设宿主已撤销其注册。
- 不在 `load()` 之外操作 UI 所有权；卸载 ≠ 拆 UI（Ribbon 标签/停靠面板在
  `unload()` 后仍在，这是有意的）。
- 宿主没有 `dlclose`，插件的静态对象活到进程结束。

## 已评估但**未采纳**（留档，避免重复建议）

- **运行时热卸载 / 禁用立即生效**：需要插件 `load()` 可重入（现有插件不是：
  `installConsoleLogSink` 的 `addSink` 在 `if (sink == nullptr)` 之外，重载会重复挂
  sink；`AppShellPlugin::load` 会重复建 Ribbon/Dock），还要撤销命令/配置/UI/后端
  注册并回答"命令正在跑时能否卸载"。用户明确要求"禁用要重启生效"，故不引入。
- **`RenderBackendRegistry` 增加注销 API**：超出本次范围；当前由"库常驻 + 工厂为
  插件内静态对象"支撑其正确性，注释已按事实改写。
- **`~PluginManager()` 中兜底 `unloadAll()`**：`~Application()` 先把
  `Application::current()` 置空、再析构 `ApplicationData`，兜底调用时拿不到宿主
  上下文，插件的 `unload()` 会拿到空 context。改为在关闭序列中显式调用。
- **把 skip list 合并进禁用列表**：skip list 是既有公开 API（进程级程序化过滤，
  连发现都不参与），与"用户偏好、仍要显示"的禁用语义不同，保留两者并各自写清语义。
