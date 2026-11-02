# 文案翻译（i18n）

> **状态**：现行
> **最后核对**：2026-11-02
> **范围**：appfw GUI 与由 PluginManager 加载的 UI 插件（包括 app_shell）的用户可见文案；不含宿主可执行文件自有目录、日志与异常文案或运行期切换。
> **权威来源**：`src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`、`Application.hpp`、`src/fw/appfw/src/TranslationCatalog.hpp`、`src/fw/appfw/src/Application.cpp`；用例 tests/test_appfw/TranslationTest.cpp。
> **待办**：`../backlog/appfw-translation.md`

## 1. 概要设计

### 1.1 问题与背景

界面文案全部硬编码在源码里：appfw GUI 约 140 处 `QStringLiteral("…")`，app_shell 与插件同样，而且只有中文；界面也没有可用的语言设置。要支持另一种语言，今天只能改源码。

同时，底层库（base / viz / robotics / loaders）的日志与异常本来就是英文——那是给开发者的诊断，不是界面文案。两者被混在同一个问题里讨论，是这套机制要先划清的第一条线。

### 1.2 目标

- 只翻译**界面文案**：appfw 与其 UI 插件（包括 app_shell）；当前 Vine 可执行文件没有自有可翻译文案。
- 语言是配置项 `general.language`（跟随系统 / 中文 / English），未设置则跟随系统；没有对应译文时显示源串。
- 语言集合只有中文与英文两种：源串是中文，所以只有一本**英文**目录（C7）。
- 目录（`.ts`）是源码：可 diff、可评审；编译产物（`.qm`）随构建生成、嵌进它所属的模块，并可被同名文件覆盖（§2.10）。
- 不给任何下层库增加“知道翻译存在”的依赖。

### 1.3 非目标

- 不翻译日志（`VN_LOG*`）与异常消息（`Exception`、`io::IoError` 的描述）：诊断契约，见 2.2。
- 不翻译身份类字符串：命令名、页面 id、配置键、插件 id/uuid（G1：身份不是文案）。
- 不做运行期切换语言（G8：没有消费者；G10：每个控件都要实现重译，成本远高于收益）。
- 不为宿主可执行文件增加独立目录入口：出现自有可翻译文案时再设计（G8）。
- 不翻译插件元数据（`PluginInfo` 的 name/description，v1）：它们是插件自报的标识，见 §9.2。
- 本轮不处理底层的用户表面（崩溃窗口与崩溃报告）：维持现状（中文硬编码），见 §2.9 与 backlog T7/T8。
- 不自造复数/性别规则：需要时用 Qt 的 `n` 参数，中文没有复数。

### 1.4 一句话结论

**appfw 与 UI 插件的界面文案由 Qt `tr`/`.ts`/`.qm` 翻译；中文源、英文译文，语言在启动时选定，目录随模块并由拥有者持有，缺译文回退源串。**

## 2. 设计要点

### 2.1 分层即边界（G2、G5、G4）

- 是什么：可翻译文案只出现在 `src/fw/appfw` 与 UI 插件；`src/base`、`src/viz`、`src/robotics`、`src/loaders` 不调用 `tr()`、不包含翻译头文件。
- 为什么：翻译是呈现层的事。让底层库认识翻译，等于给每个库加一条与界面生命周期绑定的依赖（换语言要重刷），也与“base 不知道 UI”的既有分层方向相反。
- 边界：判断标准不是“用户看不看得见”，而是“这段字属于界面还是诊断”。日志、异常、断言、`Result` 的错误描述都属于诊断。

### 2.2 诊断文案永远英文（G4、G6）

- 是什么：`VN_LOG*`、`Exception::what()`、`io::IoError` 的名字与描述保持英文，不经过目录。
- 为什么：日志要被 grep、被门禁脚本匹配、被贴进 issue；异常文本是开发者之间的契约，翻译后同一个错误在两种语言下对不上号。用户该看到的是界面上的提示，而不是把异常原文翻一遍。
- 边界：面向用户的错误提示由界面层自己写（可翻译），需要时把诊断代码或原始细节附在后面（原文不翻译）。

### 2.3 封装 Qt 的目录，不自造（G6、G9、G7）

- 是什么：使用 `QCoreApplication::translate()`、`.ts` / `lupdate`、`.qm` / `lrelease` 与 `QTranslator`；框架只加薄的调用与装载点，不重写目录格式和工具。
- 为什么：Qt Widgets 自带文案也依赖 Qt 的 translator；自造目录会产生两套查找、回退与编辑流程（G1、G7）。
- 边界：装载只依赖 QtCore，可在无头 `Application` 中完成；不另包 `Translator` 服务类。

### 2.4 源语言是中文（G8、G11）

- 是什么：源码里的 `tr(u8"关于")` 的源串就是今天的中文；目录是“中文 → 其它语言”。
- 为什么：现有约两百条文案已经是中文，改成英文源再翻回中文是纯粹的 churn；评审 `.ts` 时团队看得懂中文上下文。Qt 不要求源语言是英语。
- 代价：只用英语的贡献者读到的源码字符串不是英文；`lupdate` 抽取与翻译不受影响。
- ADR：`../decisions/ADR-2026-11-01-translation-scope.md`（含“英文源”的否决理由）。

### 2.5 一个公共概念：`translate(context, source)`（G1、G4）

- 是什么：`vn::appfw::translate(context, source)` 返回 `vn::String`；GUI 用 Qt 原生 `Q_DECLARE_TR_FUNCTIONS(appfw)`，直接得到 `QString`。
- 为什么：保留 Qt 函数名、参数顺序和源串键，保证 `lupdate` 提取并避免维护第二套 key（G1、P9）；`char8_t` 重载接收 C++20 UTF-8 字面量。
- 边界：QObject 子类不能裸用 `tr()`，否则 context 变成类名（P7）；入口只查已装目录，缺失返回源串。

### 2.6 装载一次，早于任何窗口（G3、G7）

- 是什么：`Application::initialize()` 里、配置文件读完之后、`GuiApplication::createWindows()` 建主窗之前，装载 Qt 与框架目录。语言取配置项 `general.language`，缺省或 `"system"` 取 `QLocale::system()`。
- 目录从哪来：模块文件所在目录的同名文件优先，其次是模块内嵌资源；默认不需单独部署 `.qm`。
- 为什么：窗口在构造函数里就 `setText`；装载晚于它就只能靠 `LanguageChange` 事后补，那正是 v1 不想做的复杂部分。
- 边界：`Application` 只负责“按语言装载目录”，不认识任何窗口；宿主可执行文件当前没有独立目录入口，UI 由 appfw 与插件提供。

### 2.7 插件的目录由加载器代管（G7、G5）

- 是什么：`PluginManager` 在调用 `plugin->load()` 前装载插件目录，卸载时移除；插件只在调用点写 `tr(u8"<id>", u8"…")`。
- 目录是一个 `TranslationCatalog` 值，随装载记录（`LoadedPlugin`）一起活：装载即安装、记录析构即移除（§2.11）。
- 没有清单：加载器不持有“哪些插件有目录”的表，只按 `<id>_<locale>` 约定查找（C6）。
- 为什么：每个插件都重写一遍“找目录、装载、卸载”就是把复杂度转移给每个插件（G7）；插件文件的位置只有加载器知道。
- 边界：context 与目录文件名都用插件 id（不是显示名）；目录缺失不是错误。

### 2.8 v1 不做运行期切换（G8、G10）

- 是什么：语言只在启动时确定；改语言 = 改配置项 `general.language` 后重启。
- 为什么：运行期切换要求每个控件在 `QEvent::LanguageChange` 里重设文本（appfw 有几十个构造时写死文字的控件，还有 SARibbon/DockingPanes 的上游控件），成本与风险远高于收益。
- 边界：不做运行期切换**是产品决定**，不是缺口。所以语言项声明 `restartRequired()`，配置窗口据此提示“重启后生效”，`Application::pendingRestartKeys()` 则回答“现在是否有一项待重启”（`appfw-config.md` F6）。

### 2.9 底层的用户表面：本轮维持现状，将来注入文案（G8、G2、G10）

- 是什么：base 里面向用户的表面只有一个——崩溃窗口与崩溃报告（`src/base/crash/handler`，独立进程）。**本轮不动它**：窗口标题/正文/按钮与报告字段名继续是硬编码中文，不在翻译范围内。
- 将来落地的形状（见 backlog T7）：`crash::Config` 提供一组可选的界面文案，由 appfw 在 `CrashHandler::install()` 时用 `tr()` 的结果填，handler 保留中文默认值。注入的是**字符串**，不是语言、不是目录。
- 为什么不让 base 自己装载目录：base 目前**零 Qt 依赖**（core / global / logging / iobase / crypto / crash 只用 std、spdlog、zlib/libzip、wolfssl、X11/Xft）；引入 `QTranslator` 会把 QtCore 拖进最底层，而且崩溃时刻正是不该依赖应用框架的时刻（handler 是独立进程，Linux 版刻意只用 Xlib + Xft）。
- 边界：报告 `.txt` 与窗口正文不同源——报告是给开发者的诊断（字段名应英文），窗口标题/正文/按钮才是界面文案；今天两者都是中文，一并留在 backlog（T8）。

### 2.10 目录的归属与形态（G5、G9、G7）

- 是什么：翻译文件随模块走。框架（`appfw`）与每个 UI 插件各有一本 `<id>.ts`，放在**模块自己的 `res/translations/`** 下，编译成 `<id>_<locale>.qm`，在**自己的构建里**用 `vn_add_translations()` 嵌进自己的二进制（资源前缀 `/i18n`）。
- 为什么归属在模块：文案、`.ts` 与代码同目录，改字符串与更目录落在同一个提交（P1 的预防）；新插件不修改 appfw 里的任何清单，加载器只按约定查找（G9）——appfw 不知道插件集合（G2）。
- 为什么内嵌：安装树里不出现散落的 `.qm`，也不会被误删或换成坏文件；路径只剩一条约定，不再乘上“构建树 / 安装树 × Windows / Linux”（`vn_add_plugin` 已经在处理这些路径）。
- 查找顺序：先 `<模块文件所在目录>/<id>_<locale>.qm`，再模块内嵌的 `:/i18n/<id>_<locale>.qm`，都没有则回退源串（C3）；不扫描 `translations/` 子目录。
- context 约束：`appfw` 保留给框架；插件 ID 是插件 context，必须唯一且不得为 `appfw`。当前没有宿主可执行文件目录，因此无需另设 host context。
- 边界：没有“集中收集所有 `.qm`”的步骤，`lrelease` 只处理本模块的 `.ts`；跨模块的术语一致靠词汇表与评审，不靠代码（G1：一本目录只说明一个模块的文案）。

### 2.11 目录的所有权跟随安装者（G1、G10、G11）

- 是什么：没有“目录注册表”。一本目录就是一个 `TranslationCatalog` 值（`src/TranslationCatalog.hpp`）：构造即装载并安装到 `QCoreApplication`，析构即移除。`ApplicationData` 持有应用级两本（Qt 的与框架自己的），每条 `LoadedPlugin` 持有一本插件目录。
- 为什么：`QCoreApplication` 只保存 translator 的指针、不拥有它，所以“谁装谁持有”是唯一不会漏的写法——没有 `remove` 调用要记得写；装载失败（`create()` 返回空或抛异常）时局部值随栈展开释放（C9、R4）。进程级静态更做不到：它拿不到语言（宏注册早于读配置），还会让同一进程里的第二个 `Application` 继承前一个的目录（D9）。
- 边界：查找顺序由三个命名构造固化（`forQt` / `forFramework` / `forModule`），没有对外的候选路径参数；“没有目录”不是一个错误码，而是一个空值（C3）。
- 代价：`QTranslator` 析构会向 `QCoreApplication` 发 `LanguageChange`，所以应用级两本声明在 `ApplicationData::app` 之后、`user_io` 之前——窗口先销毁、Qt 应用对象最后销毁，移除发生在两者之间。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 框架文案 | `tr(u8"…")` 查 `appfw` 目录 | 机制已实现 |
| F2 | 插件文案 | `tr(u8"<id>", u8"…")` 查插件目录 | 机制已实现 |
| F3 | 语言选择 | 配置项 `general.language`（通用/语言）：跟随系统 / 中文 / English | 已实现 |
| F4 | 缺译文回退 | 返回源串；不抛、不记日志 | 已实现 |
| F5 | 目录的构建与形态 | `.ts` 进 git；`lrelease` 产出 `.qm` 并嵌进所属模块；模块旁的同名文件可覆盖 | 已实现 |
| F6 | 不翻译清单 | 日志、异常、ID 不进目录（约定，靠评审与 grep 守） | 约定 |
| F7 | context 唯一性 | 插件 ID 充当 context，保留 `appfw` 给框架 | 已实现 |

## 4. 公开接口

### 4.1 翻译入口（新增头文件，SDK 层）

| 接口 | 契约 | 备注 |
|---|---|---|
| `String translate(const char* context, const char* source)` | 在 context 里查 source；无译文返回 source | 兼容现有窄字符串调用；名字与参数顺序跟 `QCoreApplication::translate` 一致（P9） |
| `String translate(const char8_t* context, const char8_t* source)` | 在 UTF-8 context 里查 UTF-8 source；无译文返回 source | 可直接传入 C++20 `u8"..."` 字面量，转发到同一 Qt 查找（P9） |
| `Q_DECLARE_TR_FUNCTIONS(appfw)` + `tr(const char8_t*)` | GUI 类内用：同样的查找，直接返回 `QString` | 不是我们的接口，是 Qt 的宏；一行在类里，调用点最短（P7） |

### 4.2 配置

| 接口 | 契约 | 备注 |
|---|---|---|
| `languageConfigKey()` | 返回键 `general.language` | 元数据由框架自己注册到 `General / Language`，owner 为空 |
| `languageConfigItem()` | Choice 三项（`system` / `zh_CN` / `en`），默认 `system` | 标签走 `tr()`，所以必须在语言生效之后注册 |
| `ConfigManager` 里的值 | 类型 String；缺省即“跟随系统” | 启动时读一次；用户在配置窗口改，下次启动生效 |

### 4.3 内部

| 接口 | 契约 | 备注 |
|---|---|---|
| `TranslationCatalog`（`src/` 下） | 一本已装载目录的所有者：构造即安装，析构即移除 | 不导出、不安装；`Application` 持有应用级两本，`LoadedPlugin` 每插件一本（C8、C9、§2.11） |

### 4.4 构建接口（新 CMake 函数）

| 接口 | 契约 | 备注 |
|---|---|---|
| `vn_add_translations(<target> CONTEXT <id> TS_FILES <files…>)` | 对目标自己的 `.ts` 跑 `lrelease`，把 `.qm` 嵌进目标的 `/i18n` 资源 | `CONTEXT` 必须等于 `tr()` 的 context 与文件名前缀（不匹配直接报错）；缺 `LinguistTools` 时只警告、不嵌（P6）；目标必须自己链 `Qt6::Core`；只跑 `lrelease`，不跑 `lupdate`（改 `.ts` 是人的动作） |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- C1：`src/base`、`src/viz`、`src/robotics`、`src/loaders` 不得包含翻译头文件或调用 `tr()`（G2）。
- C2：目录只在 `Application::initialize()`（Qt + 框架）与插件加载器装载；窗口、对话框不装载。
- C3：目录缺失是正常状态：回退源串，不算错误、不弹窗。
- C4：`.ts` 是源码（可 diff、可评审）；`.qm` 是构建产物，不进 git。
- C5：底层的用户可见表面（今天只有崩溃窗口）只接收宿主注入的文案；base 不得引入 Qt、不得装载目录（§2.9）。
- C6：翻译文件随模块走（`<id>.ts` → `<id>_<locale>.qm`）；`PluginManager` 不持有插件翻译清单，只按约定查找（§2.10）。
- C7：语言集合只有 `zh_CN` 与 `en`。源串本身是中文，所以**只有一本英文目录**，`zh_CN` 不产生 `.qm`；Qt 自带控件的文案仍由 Qt 的 `qtbase_zh_CN.qm` / `qtbase_en.qm` 提供（§2.3）。
- C8：“私有”由构建保证，不靠约定：`TranslationCatalog` 这类实现放在 `src/`（PRIVATE include 目录、不安装、不导出），SDK 只暴露 `translate()` 与配置项；GUI 用同一个 `translate("appfw", …)`（返回 `QString` 的内部重载），不自造包装函数名（P9）。
- C9：目录的所有权跟随安装者：应用级两本在 `ApplicationData`，插件级每本在对应的 `LoadedPlugin`；不存在进程级目录注册表（§2.11）。
- C10：`appfw` 是保留 context，插件 ID 唯一且不得占用它；当前没有宿主可执行文件自己的目录 context。

### 5.2 要求（R#）

- R1：装载完成早于任何窗口构造，界面文案在第一次 `setText` 时就是目标语言。
- R2：同一语言下，缺某一条译文不影响其余条目（按条目回退，不是整本目录失效）。
- R3：插件卸载后它的目录不再影响其它层的文案（translator 随卸载移除）。
- R4：装载失败（`create()` 返回空或抛异常）不留下已安装的目录；下一次启动从干净状态开始。
- R5：CTest 的 `check_translations` 必须找到 `lupdate` 并验证所有目录；缺工具不得让门禁静默通过。

### 5.3 不变量（N#）

- N1：`translate()` 的返回值永远非空，且在没有目录时逐字等于源串。
- N2：context 与目录文件名、插件 id 三者一致。
- N3：日志与异常文本不经过 `translate()`。
- N4：可翻译的是**整句**。拼接片段（`“已注册（” + x + “）：”`）能翻，但逐段翻译的语序不一定对；新文案一律写成带 `%1` / `%2` 的整句，现有片段属于待偿债务（§9.1）。
- N5：context 唯一；框架固定 `appfw`，插件加载器拒绝同名插件 ID。

## 6. 注意事项与陷阱

### 6.1 改了源串却没跑 `lupdate`（P1）
- 会怎么错：目录里旧源串失效，新文案回退成中文（在英文界面里露出中文）。
- 怎么判：`ctest --test-dir build -R '^check_translations$'`；它对每个目录跑 `lupdate` 并比较 `(context, source)` 与译文完整性。
- 怎么防：改文案的提交与 `.ts` 更新放同一个提交；CTest 用 `--require-lupdate`，缺工具即失败。

### 6.2 装载顺序（P2）
- 会怎么错：context 重名时，后装 translator 的同源条目可能覆盖先装目录的译文。
- 怎么判：同一句在两个层里被翻成不同语言。
- 怎么防：框架固定 `appfw`，插件 ID 唯一且不可占用该名；插件目录在其代码执行前装载。

### 6.3 译文比原文长（P3）
- 会怎么错：导航行、按钮、Backstage 条目的文字变长后截断或顶出边界（我们按 40 px 行高、20 px 图标位排过版）。
- 怎么判：切到英文目视检查每个界面。
- 怎么防：控件一律允许省略（`elide`/`ElideRight`），不写死文字宽度；给最长语言的译文留余量。

### 6.4 把日志当文案翻（P4）
- 会怎么错：日志被翻成中文后，门禁脚本与报错检索都对不上；同一错误在两份日志里文本不同。
- 怎么判：`grep` 不到既有英文日志行。
- 怎么防：C1 与评审；底层库不需要、也不允许包含翻译头文件。

### 6.5 把内嵌当作保密（P5）
- 会怎么错：以为 `.qm` 不落盘就“看不到译文/SDK 不外泄”，于是把不该进目录的东西（内部代号、未发布特性名）写进文案。
- 怎么判：`lconvert -i x.qm -o x.ts` 一秒还原成可读文本，内嵌只改变**放在哪**，不改变**谁能读**。
- 怎么防：内嵌的目的是安装树干净、不会被误删或换坏；真要保密就不写进文案。

### 6.6 机器上没有 `LinguistTools`（P6）
- 会怎么错：`vn_add_translations()` 依赖 `lrelease`；缺组件时配置阶段就失败，或产出一个没有目录的二进制而无人发现。
- 怎么判：构建日志里没有 `lrelease` 行；跑到英文界面发现全是中文。
- 怎么防：`vn_add_translations()` 在缺工具时打 `WARNING` 并跳过嵌入（源码与构建仍能用）；CTest 翻译门禁要求安装 `lupdate`。

### 6.7 裸写 `tr` 被 `QObject::tr` 遮蔽（P7）
- 会怎么错：在 `QObject` 派生类（几乎所有窗口、面板、Ribbon 子类）的成员函数里写 `tr(u8"…")`，非限定查找在类作用域就停住了：`QObject::tr` 的签名是 `(const char*, const char*, int)`，于是要么直接编译失败，要么用上 Qt 那个“拿类名当 context”的版本——目录里没有 `BackstageWindow` 这种 context，译文静默失效。
- 怎么判：编译失败是显式的；静默失效则表现为“译文本该在这却没出现”，而 `lupdate` 报的 context 是类名。
- 怎么防：GUI 类里写一行 `Q_DECLARE_TR_FUNCTIONS(appfw)`——它把 `tr` 变成 context 为 `appfw` 的静态成员，遮蔽问题消失、`lupdate` 也认得（已验证）；非类的自由函数用 `translate(u8"appfw", …)`。`.ts` 里出现类名 context 就是踩上了这条。

### 6.8 自定义包装函数，lupdate 抽不到（P9）
- 会怎么错：把调用点写成自己的短名（`guiTr(u8"…")`），或者把参数顺序改成 `tr(context, source)`——运行没问题，但 `lupdate` 只认已知函数名（`tr`、`translate`、`QT_TRANSLATE_NOOP` 等），产物要么漏条目，要么把 context 当成源串。
- 怎么判：`lupdate` 的 “Found N source text(s)” 远小于实际调用点数；或 `.ts` 里 `<source>` 是一个 id。
- 怎么防：名字和签名直接用 Qt 的（`translate(context, source)`；GUI 用 `Q_DECLARE_TR_FUNCTIONS`）。改名字或改签名之后立刻跑一次 `lupdate` 核对计数；**context 也必须是字面量**——写成常量（`kFrameworkContext`）时那条抽不到，编译却毫无提示（`languageConfigItem()` 的五条文案就这样漏了一整轮）。

### 6.9 部署只带了 `qt_*.qm`（P8）
- 会怎么错：`windeployqt` 复制的是 Qt 的 **meta** 目录 `qt_<locale>.qm`（几十字节，里面只有依赖名），真正的 `qtbase_<locale>.qm`（一百多 KB）没有。于是我们自己的文案翻译了，`QMessageBox` 按钮、`QFileDialog`、输入框右键菜单还是英文。
- 怎么判：`ls <exe>/translations` 只有 `qt_*.qm`；启动日志里 `UI language 'zh_CN': Qt catalogs loaded, framework catalog absent` 而 `qtbase_zh_CN.qm` 不在。
- 怎么防：装载顺序把 `qtbase_<locale>.qm` 放在 `qt_<locale>.qm` 之前（meta 缺依赖时它什么也不改）；部署由 `cmake/VineDeployQt.cmake` 把 `qtbase_<locale>.qm` 拷到 `<exe>/translations`（`VN_UI_LANGUAGES`，默认 `zh_CN`）。

### 6.10 用 grep 数 `translate(`，把 `vn::math::translate` 当文案（P10）
- 会怎么错：`grep -c 'translate('` 会把 `vn::math::translate(Vec3d…)` 这类矩阵/向量函数也算进去，于是“这个模块有 N 处文案”是假的——照这个数字去建目录，得到的是一本空目录，或者把一个本来不欠目录的模块记成待办。
- 怎么判 / 防：只认 `lupdate` 的 `Found N source text(s)` 与 `python3 scripts/check_translations.py` 结尾的 `N module(s) without one`，不认 grep（§9.1 曾经欠的清单就是因为这条记错的）。

### 6.11 插件占用保留 context（P11）
- 会怎么错：插件与框架使用相同 context/source，后安装的 translator 可能覆盖框架译文。
- 怎么判：加载插件时出现保留 context 拒绝 warning。
- 怎么防：插件 ID 全局唯一且不得为 `appfw`；加载器在实例化之前拒绝冲突。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 英文作为源语言、中文作为译文 | 两百多条现有文案要重写一遍再翻回中文，评审语言与团队不一致（G8） | `../decisions/ADR-2026-11-01-translation-scope.md` |
| D2 | 自造 JSON/CSV 目录 | 要自己实现 context、复数、查找顺序、回退与编辑器（G7）；失去 lupdate 与 Linguist | 同上 |
| D3 | gettext（`.po`/`msgfmt`） | 新依赖；与 Qt 的 `translate()` 不共享查找，插件还得各带一套 | 同上 |
| D4 | `.qm` 只内嵌 | 翻译热修必须重编程序（G9）；改为内嵌默认、模块旁文件覆盖（§2.10、嵌入 ADR） | 同上 + 嵌入 ADR |
| D5 | v1 就做运行期切换 | 每个控件都要实现重译，SARibbon/DockingPanes 也要（G10）；今天没有消费者（G8） | 同上 |
| D6 | 插件元数据也翻译 | v1 没有消费者；名字是插件自报的标识，翻译后同一插件在不同语言下叫不同名字（G1） | — |
| D7 | 让 base（含崩溃 handler）自己装载目录 | base 零 Qt 依赖、崩溃进程不能依赖应用框架、诊断文案要可 grep（G2、G10）；改由宿主注入文案（§2.9） | 同上 |
| D8 | 在 SDK 暴露 `Application::uiLanguage()` / 可重入的 `applyLanguage()` | 今天唯一的消费者是测试，而“语言生效了没有”用 `tr()` 的行为就能量（见 T7）；暴露它等于允许子类在窗口建好之后第二次装目录（§2.6 的时机变成靠约定守着）。真出现“要显示当前语言”这类消费者时再加（G8） | — |
| D9 | 进程级目录注册表（三个自由函数 + 文件局部静态） | 拿不到语言（静态注册早于读配置）、同一进程里的第二个 `Application` 会继承前一个的目录、装载失败路径靠“记得调 remove”守着（G1、G10、G11）；改为目录值随安装者持有（§2.11） | — |
| D10 | 现在给宿主可执行文件增加独立 context 与目录入口 | 当前宿主无自有可翻译 UI 文案，app_shell 已以插件身份携带目录；先扩 API 会违反 `G8` | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 无目录时 `tr()` 逐字返回源串 | `TranslationTest.NoCatalogKeepsTheSourceText` |
| T2 | 装载目录后，条目按 context 命中，未命中的条目仍回退 | `TranslationTest.APluginCatalogComesFromThePluginItself` |
| T3 | 语言标签解析：空/`system` → 系统语言，`en`/`zh_CN` 原样 | `TranslationTest.LanguageTagsResolveToTheTwoSupportedLanguages`、`SystemAndUnknownSettingsFollowTheSystemLocale` |
| T4 | 下层库不含翻译依赖 | `grep -rn "Translation.hpp" src/base src/viz src/robotics src/loaders` 为空 |
| T5 | 一个松散文件都没有时，内嵌目录仍能命中 | `TranslationTest.APluginCatalogComesFromThePluginItself`（插件旁没有 `.qm`，`:/i18n` 仍命中） |
| T6 | 模块旁的同名 `.qm` 覆盖内嵌版本 | `TranslationTest.ALooseFileOverridesTheEmbeddedCatalog`（构建期 `lrelease` 产出 `override/i18n_fixture_en.qm`） |
| T7 | 插件目录在它自己的代码之前装好，卸载后移除 | `TranslationTest.APluginCatalogComesFromThePluginItself`（夹具插件内嵌 `:/i18n/i18n_fixture_en.qm`） |
| T8 | 批量装载失败不留下目录 | `TranslationTest.AFailedBatchLoadLeavesNoCatalogInstalled`（夹具插件构造即抛，内嵌 `:/i18n/i18n_throwing_en.qm`） |
| T9 | 目录与源码一致；缺少 `lupdate` 也必须失败 | CTest `check_translations` |
| T10 | 插件不能占用框架 context | `TranslationTest.PluginsCannotClaimTheFrameworkTranslationContext` |

## 9. 状态与演进

### 9.1 当前状态

机制已落地：Application 在构造窗口前安装 Qt/appfw 目录，PluginManager 在插件代码执行前安装其目录并随卸载移除。插件 ID 不得占用 `appfw` context。目录完整性由 CTest `check_translations` 验证；宿主可执行文件当前没有自有可翻译文案。

还差：把拼接式文案（`“已注册（” + x + “）：”` 这类）改成带 `%1` 的整句（N4）。宿主 `src/apps/vine` 当前没有自有可翻译文案；出现时再设计独立 context 和入口（G8）。

### 9.2 已知缺口 / 待办

见 `../backlog/appfw-translation.md`：运行期切换、插件元数据、崩溃窗口的文案注入（T7）与报告字段名（T8）。本轮不碰崩溃窗口：它的界面文案与报告字段名仍是硬编码中文。

### 9.3 兼容性 / 迁移

- 新增头文件 `Translation.hpp`；配置体系新增一项 `general.language`（默认 `system`）。`AppConfig` **不**增加字段：语言只有这一个家（C1、G1）。
- 宿主无需改动：不设该配置项就是今天的行为（跟随系统）。宿主想预设语言，就带一份配置文件（同 `AppConfig::config_file`）。

## 10. 参考

- 代码：`src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`、`src/fw/appfw/sdk/vine/appfw/Translation.hpp`、`src/fw/appfw/src/TranslationCatalog.hpp`、`src/fw/appfw/src/Application.cpp`、`src/fw/appfw/src/gui/GuiApplication.cpp`、`src/fw/appfw/src/PluginManager.cpp`。
- 构建：`cmake/VinePluginHelper.cmake`（插件目录与安装路径）、`cmake/VineEmbedShaders.cmake`（“真实文件、装进二进制”的同类先例）、`cmake/VineLibraryHelper.cmake`（薄封装的写法参照）。
- 脚本：`scripts/check_translations.py`（目录与源码的一致性门禁：缺、陈旧、未译、无目录，P1、P10、T9）。
- 文档：`appfw-backstage.md`（界面文案的现有排布与文案长度约束）、`appfw-plugin-system.md`（插件加载顺序）。
- 外部：Qt Linguist 手册（`lupdate` / `lrelease` / `QTranslator` / `QCoreApplication::translate`）。
