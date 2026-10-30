# 应用身份与应用信息（AppConfig / AppInfo）

> **状态**：现行
> **最后核对**：2026-10-30
> **范围**：应用名/版本/描述/版权/许可/链接/致谢归谁所有、怎么读；窗口标题与「关于」的取值路径。不含版本号由构建系统生成、不含清单文件（见 §9.2）。
> **权威来源**：`src/fw/appfw/sdk/vine/appfw/AppInfo.hpp`、`AppConfig.hpp`、`Application.hpp`、`src/fw/appfw/sdk/vine/appfw/gui/AboutPanel.hpp`；用例 `tests/test_gui/test_gui.cpp`。
> **待办**：`../backlog/appfw-app-info.md`

## 1. 概要设计

### 1.1 问题与背景

关于信息此前散落在三处：`AboutCommand` 里四个字符串、`AppShellUi` 里关于页再写一遍、`MainWindow` 标题写死 `"Vine"`；而构建系统早在 `project(VERSION ...)` 里声明过版本。`AboutDialog` 与 `AboutPanel` 还各有一套 `setName/setVersion/...`——同一件事两个来源。

### 1.2 目标

一处声明、一个持有者、多个消费者只读。应用身份（进程名）与关于内容不能各说各话。

### 1.3 非目标

- 不让版本号从构建系统生成（`project()` → 生成头），不含清单文件读取 —— 见 §9.2 与 backlog。
- 不做许可合规流程、安装器、更新检查。

### 1.4 一句话结论

**应用信息是应用级的只读值对象 `AppInfo`：宿主在 `AppConfig::info` 里声明一次，`Application::appInfo()` 持有，`AboutPanel`/`AboutDialog`/窗口标题只渲染它。**

## 2. 设计要点

### 2.1 `AppInfo` 是值对象，由 `Application` 持有（G1、G4）

- 是什么：纯数据（无 Qt、无 I/O），随 `AppConfig` 交给构造函数，`Application::appInfo()` 返回 `const AppInfo&`。
- 为什么：它描述的是这个进程代表的应用，生命周期与 `Application` 相同；做成值对象就没有单例，"一个进程跑两个应用"的测试仍然成立（G11）。
- 边界：图标是路径（读者解析），`AppInfo` 不认识 `Icon`/`QIcon`。

### 2.2 空的 `name` 回落到 `AppConfig::name`（G4）

- 是什么：`AppConfig::info.name` 为空时，`Application` 用 `AppConfig::name` 填上；两者都空才用框架兜底名。
- 为什么：进程身份（`QCoreApplication::applicationName`、数据/配置目录）与关于内容必须同源，否则改一处忘一处就漂移。
- 边界：显式给出 `info.name` 时以它为准（OEM 变体）。

### 2.3 面板是渲染器，不是存储（G7）

- 是什么：`AboutPanel` 只有一个 `setAppInfo()`（外加 `appInfo()` 读回），默认在构造时读 `Application::appInfo()`；`AboutDialog` 是它的薄壳。
- 为什么：分成"面板字段"和"应用字段"两套，等于让每个宿主把同一批字符串再搬一遍——这正是重构前发生的事。
- 边界：`setAppInfo()` 仍然存在，必要时（第二身份）可以覆盖，但不是常规路径；再次设置会整份替换（旧的事实/链接/致谢不会残留）。

### 2.4 标题与关于读同一份（G6）

- 是什么：`MainWindow` 的标题取 `Application::appInfo().name`（没有应用时回落到框架名）。
- 为什么：标题栏和关于页各写一个名字是最常见的不一致来源；同一份数据让它们不可能分叉。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 应用信息 | `AppInfo` + `AppConfig::info` + `Application::appInfo()` | 已落地 |
| F2 | 关于面板 | `AboutPanel::setAppInfo()`；默认读应用信息 | 已落地 |
| F3 | 关于对话框 | `AboutDialog` 薄壳，同一块面板 | 已落地 |
| F4 | 窗口标题 | 取 `appInfo().name` | 已落地 |
| F5 | 版本号来自构建系统 | `project()` → 生成头 | 未做，见 backlog |
| F6 | 清单文件 / 第三方致谢生成 | `appinfo.json` → 构建期生成 | 未做，见 backlog |

### 3.1.1 F1 应用信息

- **行为**：`AppInfo` 携带 name/version/description/copyright/license/homepage/issues_url/credits/logo/build_facts/links；`Application` 构造时复制 `AppConfig::info` 并补全空名字。
- **约束**（C1）：值对象，不持有 Qt 类型、不做 I/O。
- **验收**（T1）：`AboutDialog_ShowsTheApplicationInfo`（默认取应用信息、名字回落到 `AppConfig::name`）。

### 3.1.2 F2/F3 面板与对话框

- **行为**：面板把信息排成"身份行（logo/名称/版本/描述）+ 分隔线 + 事实行 + 链接 + 致谢块 + 版权行"；对话框把面板放进 `QDialog`，默认展示同一份。
- **约束**（C2）：颜色一律 `palette(...)` 角色；事实行/链接/致谢为空时对应区域隐藏，分隔线随之隐藏。
- **验收**（T2）：`AboutPanel_ShowsTheApplicationInfo`（含"换一份 info 不残留旧细节"）。

### 3.1.3 F4 窗口标题

- **行为**：`MainWindow` 标题 = `appInfo().name`；无应用时 `"Vine"`。
- **约束**（C3）：标题只读应用信息，不从插件或窗口自己拿。

## 4. 公开接口

### 4.1 `AppInfo`（appfw，无 Qt）

| 接口 | 契约 | 备注 |
|---|---|---|
| `AppInfo` / `AppInfoRow` / `AppInfoLink` | 应用元数据值对象 | 完整字段见头文件 |
| `AppConfig::info` | 声明入口 | 空 name 回落到 `AppConfig::name` |
| `Application::appInfo()` | 读取入口 | 生命周期同 Application |

### 4.2 `gui::AboutPanel` / `gui::AboutDialog`

| 接口 | 契约 | 备注 |
|---|---|---|
| `AboutPanel::setAppInfo(const AppInfo&)` | 渲染这份信息 | 整份替换 |
| `AboutPanel::appInfo()` | 读回当前渲染的信息 | 含已解析的名字 |
| `AboutDialog::setAppInfo/appInfo` | 同上，转发给内部面板 | 默认展示进程信息 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- C1：`AppInfo` 是纯数据，不含 Qt 类型、不做 I/O。
- C2：关于内容只用 `palette(...)` 角色着色，随明暗主题变化。
- C3：窗口标题只读 `Application::appInfo()`，不从插件或窗口自身取值。

### 5.2 要求（R#）

- R1：应用身份（进程名）与关于内容同源，不得各自硬编码。
- R2：一份信息在对话框与 Backstage 页面里显示一致。
- R3：没有应用时（无头测试）构造面板不得崩溃，显示为空或由 `setAppInfo()` 填充。

### 5.3 不变量（N#）

- N1：`appInfo().name` 非空（构造时必被填好）。
- N2：`setAppInfo()` 之后屏幕上只剩这份信息的内容，不含上一份的残留。

## 6. 注意事项与陷阱

### 6.1 面板与宿主的生命周期
- 会怎么错：面板的 `Control` 包装在页面 widget 之前释放，`setBody()` 之后宿主仍留着旧的指针。
- 怎么判：关于页打开即崩溃/空白。
- 怎么防：包装比页面 widget 活得久（`app_shell` 交给进程生命期），或让创建者持有它。

### 6.2 图标路径的可用性
- 会怎么错：`AppInfo::logo` 指向某插件的 Qt 资源，但面板在资源注册前解析。
- 怎么判：关于页没有图标（不崩，只是空）。
- 怎么防：把它当"可选装饰"；需要时用 `QApplication::windowIcon()` 兜底（面板已实现）。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 运行期读 `appinfo.json` | 新增失败方式，而构建期读能把同样的错误变成 configure 失败（G11） | — |
| D2 | `AppInfo::instance()` 全局单例 | 与"一个进程跑两个应用"冲突；`Application` 持有已足够（G1） | — |
| D3 | 面板/对话框各留一套字段 setter | 两个事实源，宿主每次都要搬字符串（G7） | — |
| D4 | 关于信息塞进 `PluginInfo` | 它是**应用**的属性；复用会随插件加载状态变化（G8） | — |
| D5 | 版本号写死在源码 | 与 `project()` 声明必然漂移，漂移的版本号是最贵的错误（G10） | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 对话框默认取应用信息，名字回落到 `AppConfig::name` | `GuiTest.AboutDialog_ShowsTheApplicationInfo` |
| T2 | 面板渲染身份/事实/链接/致谢，换 info 不残留 | `GuiTest.AboutPanel_ShowsTheApplicationInfo` |

## 9. 状态与演进

### 9.1 当前状态

F1–F4 落地：`app_shell` 的「关于」页是 `setBody(AboutPanel)`，`about` 命令打开该页（无 Backstage 时退回对话框），`MainWindow` 标题取 `appInfo().name`。

### 9.2 已知缺口 / 待办

见 `../backlog/appfw-app-info.md`：版本与构建事实从 `project()` 生成（F5）、`appinfo.json` + 第三方致谢生成（F6）。

### 9.3 兼容性 / 迁移

- `AppConfig` 只加成员 `info`；`Application` 只加 `appInfo()`。
- `AboutDialog` 的 `setAppName/setVersion/setDescription/setCopyright` 已移除，改为 `setAppInfo()`；`AboutPanel` 的这一批 setter 从未发布。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/AppInfo.hpp`、`src/fw/appfw/sdk/vine/appfw/gui/AboutPanel.hpp`、`src/fw/appfw/src/gui/AboutDialog.cpp`、`src/apps/vine/src/main.cpp`（应用声明处）。

### 10.2 文档与 ADR

- `appfw-backstage.md`（页面主体）、`appfw-startup-splash.md`（副标题）、`../backlog/appfw-app-info.md`。

### 10.3 外部资料

- Windows `VERSIONINFO` 资源、macOS `Info.plist`、Qt `QCoreApplication::setApplicationVersion`、VS Code `package.json` 元数据、.NET `AssemblyInformationalVersion`。
