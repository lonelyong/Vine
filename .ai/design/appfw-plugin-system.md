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
<用户数据>/<organization>/<application>/
├── config/<application>.json      Application::defaultConfigFile()
├── logs/...                       宿主自己放日志（main.cpp 用 <data>/logs/vine.log）
├── plugins/<插件名>/              插件自己的文件，PluginLoadContext::ensureDataDirectory()
└── installed.d/<id>.plugin        插件注册文件
```

- `<用户数据>` = `QStandardPaths::GenericDataLocation`（Linux `~/.local/share`，Windows
  `%LOCALAPPDATA%`），取不到时退回临时目录。**平台差异全部由 Qt 处理，代码里不出现
  任何平台路径**（`userDataRoot()` 是一处收口），因此三平台同一套布局：

  | 平台 | 用户数据目录 | 系统数据根（AllUsers） |
  | --- | --- | --- |
  | Windows | `C:/Users/<user>/AppData/Local` | `C:/ProgramData` |
  | Linux | `$XDG_DATA_HOME`（默认 `~/.local/share`） | `/usr/local/share`、`/usr/share` |
  | macOS | `~/Library/Application Support` | `/Library/Application Support` |

  Linux 上完整路径即 `~/.local/share/Vine/Vine/{config,logs,plugins,installed.d}`；
  `$XDG_DATA_HOME` 有值时跟随它（Qt 行为）。系统根取自 `standardLocations()` 的第 2 项
  起（第 1 项是用户目录），列表顺序即优先级顺序。
- ⚠️ **没有中间的 `appdata` 一级**（2026-09-10 用户要求去掉）：Windows 下原来会得到
  `C:/Users/<user>/AppData/Local/appdata/Vine/Vine`，父目录本身就带 `AppData`，再套一层
  `appdata` 是冗余的。现在就是 `<用户数据>/<org>/<app>`，与 Qt 的 `AppDataLocation` 同构。
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

`PluginLoadContext::ensureDataDirectory()` → `<数据>/plugins/<PluginInfo::name>`，
`Application::pluginDataDirectory()` 是该根（`<数据>/plugins`）。命名用 `ensure*` 而不是
`dataDirectory()`：只有它会**创建**目录（原 `dataDirectory()` 名字读起来像纯访问器，
与 `Application::dataDirectory()` 语义冲突），惰性创建本身不变。约定：

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
| `User` | `<用户数据>/<org>/<app>/installed.d/` | 当前用户：可安装/卸载/禁用 |
| `AllUsers` | `<系统数据根>/<org>/<app>/installed.d/` | 管理员安装，所有用户可见；用 `enabled = false` 做机器级禁用策略 |

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
- 被 ABI 拒绝的库**不进发现列表**（它连元数据都不能安全读，没有名字可列），只能靠日志定位。
  若以后想让它也出现在插件管理器里，需要一个允许"无名条目"的列表形态。

## 插件元数据与图标（2026-09-10 新增）

`PluginInfo` 在 `uuid/name/display_name/version/description/vendor/dependencies` 之外新增三个可选字段，
目的是让插件管理器能"交代清楚一个插件是谁写的、去哪找人、长什么样"：

| 字段 | 用途 | 空值语义 |
| --- | --- | --- |
| `email` | 厂商联系方式；详情页渲染成 `mailto:` 链接 | 不显示该行 |
| `repo` | 仓库/问题跟踪地址；详情页 `信息` 页是一行可点开的链接 | 不显示该行 |
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

## 插件管理器对话框（2026-09-10 重做布局；同日改为 Tab 页）

左栏：筛选框 → 插件列表（图标 + 显示名 + 版本 + 状态后缀，不可用的行灰显）→ `加载插件…` 与
`安装插件`（下拉：仅当前用户 / 所有用户）。右栏：占位页或详情页（**放在 QScrollArea 里**，
小窗口不会把内容压扁）。详情页自上而下：

1. 头部：56px 图标 + 显示名（加粗放大）+ `标识 · 版本` + `厂商 · 邮箱链接` + **状态徽章**；
2. 状态说明（带边框的整句解释：为什么没在跑 / 正在跑）；
3. 动作行：`禁用/启用`、`卸载插件（作用域）`——**不能生效的一律隐藏而不是置灰**；
4. **`QTabWidget` 三个标签页**，取代原先堆叠的 `描述`/`信息`/`命令`/`配置` 四个 `QGroupBox`：
   - `信息`：`QFormLayout`，**描述是第一行**（原 `描述` 组并入此处），随后标识 / 版本 / 来源
     （含可卸载与否）/ 依赖 / UUID / 库路径 / **构建框架**（插件编译时的框架版本，tooltip 给出主机版本）/ 邮箱 / 仓库；
     表单后 `addStretch()`，行不会被拉散。
   - `命令`、`配置`：各一个撑满整页的 `QTableWidget`（`setMinimumHeight(120)`，否则在滚动区里
     会缩到只剩表头）；tab 页留 6px 内边距，表格本身 **`NoFrame` + 背景透明**。

⚠️ **表格如何融进 tab 页**（2026-09-10，用户反复要求"不要线条 / 表头与 tab 背景一致 / 表格透明"，
`blendIntoPage()` 一处收口）：

- `tabs->setStyleSheet("QTabWidget::pane { background: palette(window); border: 1px solid
  palette(mid); }")`：**主动给 pane 上色**。不这样做时 Qt 样式画的 pane 是它自己的灰
  （Fusion 下实测 `#FBFBFB`），而 `palette(window)` 是 `#F5F5F5`，表头怎么调都对不上。
  改由我们自己指定后，pane 与表头同源，天然一致。
- 表头 `QHeaderView::section`：背景 `palette(window)`（= pane 色）、无边框、仅一条
  `border-bottom: 1px solid palette(mid)`、`padding: 4px`（**必须显式给 padding**，QSS 一接管
  section 就不再套用样式默认内边距，文字会贴边）。
- 表格 `background-color: transparent`：整块表体透出 pane 底色，只留网格线（用户要求）。
- ⚠️ **设了 QSS 之后网格线会消失**（Qt 行为），必须显式补 `gridline-color: palette(mid)` 才回来；
  同理选中行也要显式写 `QTableWidget::item:selected { background-color: palette(highlight);
  color: palette(highlighted-text); }`，否则选中高亮不显示。
- `setFrameShape(QFrame::NoFrame)`：pane 已经画了边框，表格再画一层就成了"双线夹一条缝"。
- `verticalHeader()->setVisible(false)`：行号列对只读列表是噪音，还自带两条竖线。
- 全部颜色走 `palette(...)`（QSS 运行时求值），跟随浅/深主题；这点与徽章写死颜色**相反**
  （徽章那条是特例，见文末）。
- 验证手段：临时 test 里 `root->grab().save(png)`（offscreen 平台可离屏渲染），再用
  `System.Drawing` 逐像素扫颜色变化，确认 pane/表头同色、只剩一条边框线、网格与选中高亮都在。
  ⚠️ 扫图**记得 `$bmp.Dispose()`**：GDI+ 会锁住 PNG，导致下一次 `save()` 失败。

底部：左侧一行反馈文字（加载/安装/卸载/启禁的结果），右侧 `刷新` `关闭`。
右键菜单与按钮同规则（不能生效的项用 `setVisible(false)` 隐藏），另加"复制库路径"。

⚠️ **`打开仓库` 按钮已删除**（用户要求）：仓库地址只作为 `信息` 页的一行，靠
`repoLabel->setOpenExternalLinks(true)` 保证仍可点开浏览器（删按钮不该丢掉这个能力），
`QDesktopServices`/`QUrl` 的 include 随之删掉。

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

## 命令注册队列属于模块（第八轮修正，2026-09-11）

`V_DECLARE_COMMAND` 在插件库**被 dlopen 时**排队注册器（`inline static AutoRegistrar`），
`vinePluginRegisterCommands()` 在宿主加载该插件时把队列刷进 `CommandManager`。前提是
"每个模块各有一份队列"——但旧实现用的是头文件里的 **inline 函数 + 函数局部 static**，
这在 ELF 上**全进程只有一份**：

- 编译器给函数局部 static 的是 **GNU unique** 绑定（`nm` 里的 `u`），动态链接器会让
  *所有* 已加载模块看到同一个对象；inline 函数本身是 `W`（弱、可抢占）符号，第一个加载的
  模块胜出。
- 最小复现（两个 .so 各自 `inline` 取队列，宿主 dlopen 两个）：地址相同、两个模块的
  `push` 进了同一个容器（各自 `size()` 都读到 2）。同一份代码加 `visibility("hidden")`
  后变成各模块私有的 `t`/`b` 局部符号，地址不同、各自 `size()==1`。

后果（两條都是用户可见的）：

1. **被禁用/被宿主跳过/依赖未满足的插件，其命令照样被注册**：它们在"发现"阶段就被 dlopen
   （取元数据），注册器已入队；同批第一个被加载的插件一 flush，就把这些命令一并注册了。
   `setSkipList()` 和禁用对"命令"这条轴完全失效，命令还能被执行。
2. **归属错乱**：`RegistrationOwnerScope` 把这些命令算到“当前加载的那一个插件”头上，
   `commandInfosForPlugin()` 与插件对话框“命令”页张冠李戴。
   实测：`test_plugin` 被禁用且未实例化时，它的 5 个命令全部出现在 `app_shell` 名下。

修法：队列成为模块自己的东西，而且是**代码层面**的，不依赖任何构建选项。

- `command_export.hpp` 声明 `detail::moduleCommandQueue()`（非 inline，带 `V_MODULE_LOCAL`）；
  `V_DEFINE_MODULE_COMMAND_QUEUE()`（由 `V_DECLARE_PLUGIN()` 展开）用**限定名**定义它，
  因此不论宏在 `vine::appfw` 里还是全局作用域展开都对；
- `V_MODULE_LOCAL` = `__attribute__((visibility("hidden")))`（MSVC 下为空：Windows 上
  非 dllexport 的符号本来就是 DLL 私有的）。hidden 正是关键：不导出就不会被合并/抢占；
- flush 改成 `flushQueuedCommands(moduleCommandQueue(), manager)`：容器由调用方（插件自己的
  入口点）显式取出，刷新逻辑只依赖参数，因此也没有可被其它库抢占的中间函数；
- 先 `move` 出批次再逐个执行：注册器里可能再加载插件，那些新入队的命令不能被本次一起刷掉。

`vinePluginRegisterCommands(void(CommandManager*))` 的**签名不变**（宿主的 `dlsym` 代码
不用动），但宏体变了 ⇒ **插件必须重编**（与之前加 uuid/icon 字段同一约定；旧 `.so` 会把
命令注册进它自己那份旧队列，宿主看不见）。

## 第八轮审查（2026-09-11）

已修：

| 编号 | 缺陷 | 修复与证据 |
|------|------|-----------|
| P1 | 命令注册队列实际全进程一份（见上一节）→ 禁用/跳过插件的命令照常注册，且被算到别的插件名下 | 队列改为模块私有（hidden 可见性 + 限定名定义）。证据：`nm -C` 插件 `.so` 现在是 `t`/`b` 且无动态符号（修前是 `u` + `W`）；`PluginLifecycleTest.DisabledPluginIsListedWithMetadataButNotLoaded` 修前单跑即失败，修后过；新增 owner 断言（`app_shell` 名下不得出现 `test_*`） |
| P2 | `load()` 对**跳过列表里的名字**直接早退：既不解析也不发现，与“跳过不是看不见”的契约矛盾（`loadAll()` 路径是对的） | 删掉早退分支，改成先解析/发现/查询，再按 跳过 ⇒ 策略 ⇒ 用户偏好 拒绝并分类记日志 |
| P3 | 管理员策略（注册文件 `enabled = false`）在 `load()` 上不生效：`policy_disabled` 只在 `loadAll()` 扫描时填，新进程里先 `load()` 会绕过策略 | 新增 `isPolicyDisabled(name, library)` 私有查询：缓存未命中时读注册文件，按名字或“库文件/所在目录”匹配。用例：`HandWrittenRegistrationCanDisableForAllUsers` 里加“未跑过 loadAll 时 `load()` 也必须拒绝” |
| P4 | `installPlugin()` 把调用方给的路径**原样**落盘，相对路径会在下次启动按另一个 CWD 解析 | 落盘前做 `absolute().lexically_normal()`（有意不解析符号链接：记的是用户指的位置）。用例：`InstallWritesRegistrationFile` 加相对路径注册 ⇒ 文件里必须是绝对路径 |
| P6 | `uninstallPlugin(..., AllUsers)` 的注释说“可能合法地落到 per-user 文件”，实现只查系统目录 | 按实现改写注释 |

仅记录、未改：

| 编号 | 观察 | 为什么不改 |
|------|------|-----------|
| P5 | 没有插件 ABI 版本闸：旧 `.so` 的 `PluginInfo` 会被错位读 | 超出本轮范围，属于接口设计；已把方案写进“还没有做的”（加 `api_version` + 查询后校验） |
| P7 | `load()` 里“复用已加载实例”的分支在启用检查**之前** | 有意为之：已经加载的插件继续跑到进程结束是“禁用要重启生效”的语义；策略/偏好只约束下一次加载 |

ASan 门（插件套件）：`VINE_ASAN_FILTER='PluginLifecycleTest.*' scripts/asan_check.sh`。
本轮它先报了一个 **heap-use-after-free**，查下去是**新加的测试代码**把 `findPluginEntry()`
的返回值指向了临时 `vector<PluginEntry>`（已修：先把 `pluginEntries()` 的返回值绑到局部变量）。
修后 19/19 在 ASan 下干净。

## 命名与简化（2026-09-11）

命名：按仓库约定（Qt 风格访问器、布尔 getter 用 `is`/`has`、内部字段 snake_case）核对一遍，
改了这几处：

| 旧名 | 新名 | 理由 |
|------|------|------|
| `PluginLoadContext::configs()` | `configRegistry()` | 仓库里同一个东西到处叫 `Application::configRegistry()`/`PluginManager::pluginRegistries` 风格；`configs()` 是全仓唯一一个复数名字的访问器 |
| `PluginLoadContext::dataDirectory()` | `ensureDataDirectory()` | 它会**创建**目录；与纯访问器 `Application::dataDirectory()` 同名不同义，`ensure` 把副作用写进名字 |
| `detail::moduleCommands()` | `detail::moduleCommandQueue()` | 与创建它的宏 `V_DEFINE_MODULE_COMMAND_QUEUE` 对齐 |
| `PluginManager::load(const String& str)` / `resolvePluginPath(const String& str)` | `const String& name_or_path` | 参数名的含义就是“插件名或库路径”（头文件注释也是这么写的） |
| `Impl::entries` | `Impl::discovered` | 它是**发现**列表（不是所有都被加载），文档里一直叫 discovery list |
| `Impl::disabled` | `Impl::disabled_fallback` | 只在没有 ConfigManager 时才用；与 CommandManager 那边同名同义 |
| `Source::policy_enabled` | `Source::registration_enabled` | 它存的就是注册文件的 `enabled` 字段，旧名读起来像另一种策略 |
| `resolveEnabled(...)` 的 `built_in/skipped/policy_disabled/disabled` | `is_built_in/is_skipped/policy_disables/user_disabled` | 布尔参数带 `is_`、两个“disabled”分开叫（策略 vs 用户偏好） |
| `QueriedLibrary::valid()` | `isValid()` | 布尔 getter 前缀约定 |

简化：`load()` 与 `loadAll()` 原本各写一遍“加载库 + 解析 `vinePluginQuery` + 取元数据”
和一遍三分类的拒绝日志，现在共用两个东西：

- `queryLibrary(path)` → `QueriedLibrary{lib, info}`：全仓唯一一处知道“什么样的库算 Vine 插件”，
  `installPlugin()` 与两条加载路径都走它；
- `logRefusal(name, policy_disables)`：`load()`/`loadAll()` 的“被跳过 / 被策略禁用 / 被用户禁用”
  三分类日志合到一处（同一原因同一条措辞）；
- `disabledNames(fallback)`：`pluginEntries()` 与 `isPluginEnabled()` 不再各写一遍
  “有 ConfigManager 读配置、否则用进程内列表”。

未改（有意）：`Application::dataDirectory()` 之类纯访问器保持原名；`PluginManager` 的
`d` 作为 PImpl 成员名符合仓库约定（只要没有尾下划线）；`unloadAll()` 里用
`PluginEntry` 占位去调 `unloadOrder()` 看着略笨，但那是公开可测的 API 形状，不值得为
省两行而分叉出第二套卸载排序实现。

## 插件 ABI 握手（2026-09-11，修 P5）

**问题**：宿主按*自己*的 `PluginInfo` 布局去读插件 `.so` 里的静态结构。插件如果是用另一版
SDK 编的，字段就错位——`String` 在错误的偏移上是一对（长度、指针）垃圾，静态读不出错，
后果晚到：UI 里显示乱码、或者干脆崩在别处。原来只有一条口头约定“改字段就得重编所有插件”。

**做法**：不把版本放进 `PluginInfo`（读它正是那个不安全动作），而是加一个**签名永远稳定**的
握手入口，让宿主在“信不信这个布局”之前先问一句：

```cpp
extern "C" const vine::appfw::PluginAbi* vinePluginAbi();      // V_DECLARE_PLUGIN 自动生成

struct PluginAbi {
    std::uint32_t abi_version;      // 必须排第一个：对不上之前只读它
    const char*   framework_version; // 编译时的框架版本（V_APPFW_VERSION），纯诊断
};

#define V_APPFW_PLUGIN_ABI_VERSION 1u   // Plugin.hpp，命名空间块之外
```

规则（都写在 `Plugin.hpp` 里）：

- `abi_version` **永远第一个成员**，且宿主在它匹配之前不许读别的成员（两条 `static_assert`
  钉住：标准布局 + 偏移 0）；
- 成员只能**往后加**，不重排不删除，而且不能用布局会变的 SDK 类型（只能整数/`const char*`）；
- `V_APPFW_PLUGIN_ABI_VERSION`（现为 `1u`）在任何插件可见面变化时 +1：`PluginAbi`、`PluginInfo`、
  `Plugin`/`PluginLoadContext`、入口签名、命令注册 ABI。

**这个常量为什么定在 `Plugin.hpp`**（而不是别处）：

- **性质不同**：`V_APPFW_VERSION` 是框架级发布版本（整个 appfw 一个，构建注入），放
  `appfw_global.hpp` 是对的；ABI 号是**一个子契约**的版本，只描述插件可见面。今天插件 ABI 恰好是唯一
  的跨模块契约，但那是巧合——将来再出现第二个契约（比如 GUI 插件的 ABI），每个契约的版本号应当跟着
  它自己走，而不是在全局头里堆积；
- **bump 触发点在同一文件**：最常见的触发是 `PluginInfo` 布局变化，而 `PluginInfo`/`PluginAbi` 都在
  `Plugin.hpp`——写在一起，“改了这里要 +1”一眼可见（`PluginInfo` 的注释里也点了这一句）。放全局头就
  得跨文件想起来；
- **不能放 `plugin_export.hpp`**：那里的头文件契约写着“只给插件作者用，appfw 自己不许包含”，而宿主
  必须能读这个宏；
- **两个号没有必须一致的约束**（实现变了可以不发版，布局变了可以只 +ABI），并列放一起反而暗示有关；
- 宏**写在 `V_APPFW_NS_BEGIN` 之外**：宏没有作用域，写在命名空间块里容易被读成有作用域
  （与 `V_MODULE_LOCAL` 同样处理）；
- 名字里带 `PLUGIN` 是有意的：它只管插件 ABI，**不等于“宿主自己的 ABI”**（宿主侧二进制是普通的
  全量重编规则），也不是发布版本（`V_APPFW_VERSION`，只做诊断）。

`queryLibrary()` 的顺序是：加载库 → 解析并调用 `vinePluginAbi()` → 校验 → 才解析
`vinePluginQuery()` 并按当前布局读 `PluginInfo` → 才允许 `vinePluginCreate()`。三种结果：

| 情况 | 行为 |
|------|------|
| 不是 Vine 插件（没有 `vinePluginQuery`） | 静静地跳过（和以前一样） |
| 有 `vinePluginQuery` 但没有握手（比这套 SDK 更旧的库） | **拒绝** + 警告：“built against a framework older than this one. Rebuild…” |
| 握手在但 ABI 号不同（更旧或更新的 SDK） | **拒绝** + 警告，并给出两边的 ABI 号与框架版本，还标明 who is newer |
| 握手匹配 | 正常发现/加载；插件报的框架版本进 `PluginEntry::framework_version`，插件对话框“信息”页多一行`构建框架`（tooltip 写本程序内置版本），并进日志 |

其它后果：

- **拒绝的库不实例化**：顺序上 `vinePluginCreate()` 在握手之后，也就是不会用错布局去构造
  `Plugin` 子类（那才是真正会崩的地方）。`installPlugin()` 也不注册它（否则以后每次启动多一条警告）。
- **版本从构建来**：`src/fw/appfw/CMakeLists.txt` 把 `V_APPFW_VERSION="${PROJECT_VERSION}"`（现为
  `1.0.0`）作为 **PUBLIC** 编译定义给所有链接 `vi::Appfw` 的目标——应用、插件、测试都是同一个值，
  所以插件报的就是它编译时的框架版本；用外部安装的 SDK 编且没拿到定义时退化为 `"unknown"`
  （只影响诊断文本，兼容性由 `V_APPFW_PLUGIN_ABI_VERSION` 决定）。
- **只盖插件**：宿主自己的二进制（应用、库、测试）之间仍是普通的“全量重编”规则。
  本轮就真实撞到过这一点：只重建了库和插件、没重建 `test_vsg`，那个旧二进制用的还是旧的
  `PluginEntry` 布局，直接段错误；重编即好。这正是握手要给插件防掉的那类失效，只是插件这边
  现在会被一句话拦住，而不是崩在别处。

用例（夹具由 CMake 建，路径注入 `test_gui`）：

- `PluginLifecycleTest.PluginsWithoutCompatibleAbiAreRefused`：两个夹具库分别模拟“没有握手”
  与“ABI 号 999（框架 99.0.0）”，都必须被 `load()` 拒绝、不得 `isLoaded`、不进入
  `pluginEntries()`，并且 `installPlugin()` 也拒绝。
- `PluginLifecycleTest.PluginEntryReportsTheFrameworkItWasBuiltWith`：真插件报回的版本与
  编译期 `V_APPFW_VERSION` 相等——证明整条链路（`V_DECLARE_PLUGIN` → `PluginAbi` →
  `queryLibrary()` → `PluginEntry` → 对话框）没有丢值。

## 依赖不满足时到底会发生什么（2026-09-11 核查并修正）

「某个插件被禁用 / 被宿主跳过 / 根本没装」时，依赖它的插件会不会加载，取决于走哪条路径
（每条都有用例或探针证据）：

| 路径 | 行为 |
| --- | --- |
| `loadAll()`（启动路径） | **依赖不满足的那一簇不加载，其余照常加载**，`loadAll()` 返回 false。直接依赖：`DisabledDependencyBlocksDependents`；无关插件不受影响：`UnresolvablePluginsAreSkippedWhileTheRestLoads` |
| `loadAll()` 的**传递**依赖（A ← B ← C） | 同上：整条链被剪掉（`DisabledDependencyBlocksDependentsTransitively`） |
| `loadAll()` 的**声明成环** | 环那一簇被剪掉、其余照常加载，并在报告里点名（`DeclaredDependencyCycleIsPrunedNotFatal`） |
| `load()`（显式加载，如对话框试用） | **照常加载**，只记一条警告 `declares dependency 'app_shell', which is not loaded`：它有意不解析依赖（见头文件对 `load()` 的说明） |
| 运行时的 `setPluginEnabled(false)` | **不影响已加载的插件**：偏好只作用于下一次 `loadAll()`，已加载的依赖方跑到进程结束 |

实现是一处**可加载闭包**的不动点计算（`loadAll()` step 3）：一个候选只有在它的依赖都已加载或已在集合里时才加入集合，因此一次性得到三样东西：

1. **加载顺序**：加入顺序天然是依赖序，原来那段 Kahn 拓扑排序连同它的环检测一起删掉了；
2. **完整报告**：集合之外的每个插件都带着自己的原因出现，而且链条可读——
   ```
   Plugin dependency resolution: 2 plugin(s) will not be loaded (the rest still loads):
     Plugin 'test_plugin':
       - disabled dependency: app_shell
     Plugin 'chain_plugin':
       - dependency not loadable: test_plugin
   ```
   四类原因分开：`missing dependency`（装）、`disabled dependency`（启用）、`skipped by the host`（别跳过它）、`dependency not loadable`（看它自己那行）；环那簇额外加一句 `These plugins depend on each other in a cycle.`
3. **剪枝而非整批放弃**：一个第三方插件的坏依赖不再让应用连自带 `app_shell` 都没有。代价是语义从「全有或全无」改成「剪掉不可加载的那一簇」，`loadAll()` 仍返回 false 让宿主自己决定怎么报（`main.cpp` 已经会打印一行）。

为什么**只在这一层**改成剪枝：依赖不可满足是集合的静态属性，可以精确到具体插件及其下游；而**实例化失败或生命周期抛异常仍然回滚整批**（原有语义不变，也仍有用例）——一个跑起来才失败的插件可能已经留下了全局状态，把它当成“可隔离”反而危险。

## 测试映射

## 测试映射

| 用例 | 固定的事实 |
| --- | --- |
| `PluginLifecycleTest.DisabledDependencyBlocksDependents` | 依赖被禁用 ⇒ 整体失败、两者都不加载、`disabled dependency` 分类 |
| `PluginLifecycleTest.SkippedDependencyBlocksDependents` | 依赖被宿主跳过 ⇒ 整体失败、两者都不加载、`skipped by the host` 分类 |
| `PluginLifecycleTest.SkippedPluginStaysVisibleAndIsNeverInstantiated` | 跳过 ⇒ 不实例化，但元数据/库路径/`skipped` 标记可见（跳过优先于用户偏好）；显式 `load()`（带名字）同样先发现再拒绝；`removeFromSkipList()` 后无需重启即可加载 |
| `PluginLifecycleTest.DisabledPluginIsListedWithMetadataButNotLoaded` | 禁用 ⇒ 不加载/不注册命令（`test_hello` 不在注册表），但元数据与库路径可见；命令**不得**算到另一个插件名下（owner 断言）；启用后加载且命令归属自己 |
| `PluginLifecycleTest.DisableIsPersistedInConfig` | 写进 `plugins.disabled`、重复禁用不重复、JSON 里可见、启用后清除 |
| `PluginLifecycleTest.ConfigFileIsOptIn` | `setConfigFile` 路径语义：不存在不算错、空路径 = 不持久化 |
| `PluginLifecycleTest.DefaultDataDirectoryLayout` | `<data>/<org>/<app>/config/<app>.json` 布局 + builder 默认启用 + 不落盘 |
| `PluginLifecycleTest.ManagerDialogListsDisabledPlugins` | 对话框列表来自发现（禁用项在列，状态为"已加载 + 已禁用"），详情/刷新不崩 |
| `PluginLifecycleTest.ManagerDialogHidesToggleWhenItCannotTakeEffect` | 禁用/启用按钮只在能生效时**显示**：被宿主跳过 ⇒ 隐藏（不是灰按钮）；普通 User 插件 ⇒ 显示且可用 |
| `PluginLifecycleTest.ManagerDialogShowsMetadataAndIcons` | email/repo/icon 经 `V_DECLARE_PLUGIN` → `PluginInfo` → UI 全程贯通；未声明 icon 用内置默认 SVG（每行图标非空）；筛选框只留匹配行 |
| `PluginLifecycleTest.UnloadOrderIsReverseDependencyOrder` | 真实集合：每个已加载插件恰好一次且依赖方在前；合成集合：结论与传入顺序无关、三层链、未加载项不参与、成环不死循环 |
| `PluginLifecycleTest.PluginDataDirectoryIsPerPlugin` | 插件数据目录 = `<data>/plugins/<插件名>`（首次调用才创建、两插件不互相覆盖、无宿主/无名返回空） |
| `PluginLifecycleTest.InstallWritesRegistrationFile` | 安装 = 写 installed.d/<id>.plugin；路径不存在/空/BuiltIn 被拒；重复安装幂等；相对路径也被落盘为**绝对路径**；卸载 = 删文件 |
| `PluginLifecycleTest.SystemRegistrationDirectoriesAreListed` | 系统注册目录与用户目录布局一致且互不相同（只读检查） |
| `PluginLifecycleTest.HandWrittenRegistrationCanDisableForAllUsers` | 手写注册文件被读到；`enabled = false` 是策略，用户启用无效；**没跑过 `loadAll()` 时显式 `load()` 也必须被策略拒绝** |
| `VsgBackendPluginTest.BuiltInPluginsCannotBeDisabledOrUninstalled` | 程序自带插件 scope = BuiltIn、不可禁用、不可卸载、不能以 BuiltIn 作用域注册 |
| `PluginLifecycleTest.InstalledLocationIsDiscoveredAndDeduplicated` | 只靠注册位置也能发现；两处提供同一插件只出现一次 |
| `PluginLifecycleTest.DisabledDependencyBlocksDependentsTransitively` | A←B←C 链：链头被禁用 ⇒ 整条链都不加载（夹具 `chain_plugin` 依赖 `test_plugin`） |
| `PluginLifecycleTest.UnresolvablePluginsAreSkippedWhileTheRestLoads` | 只剪掉不可加载的那一簇：禁用链中段时 `app_shell` 照常加载，`chain_plugin` 不加载，`loadAll()` 报 false |
| `PluginLifecycleTest.DeclaredDependencyCycleIsPrunedNotFatal` | 声明成环 ⇒ 环那一簇不加载、无关插件照常加载（夹具 `loop_a`/`loop_b`） |
| `PluginLifecycleTest.PluginsWithoutCompatibleAbiAreRefused` | 没有 ABI 握手 / ABI 号对不上的库必须被拒绝、不实例化、不进发现列表，安装也拒绝（夹具库由 CMake 建） |
| `PluginLifecycleTest.PluginEntryReportsTheFrameworkItWasBuiltWith` | 插件报回的构建框架版本等于 `V_APPFW_VERSION`（V_DECLARE_PLUGIN → PluginAbi → PluginEntry 全程贯通） |
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
