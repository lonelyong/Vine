# ADR-2026-11-01：只有界面层可翻译，源语言是中文，目录用 Qt Linguist

- 状态：已采纳
- 日期：2026-11-01
- 相关：`../design/appfw-translation.md`、`../backlog/appfw-translation.md`、`src/fw/appfw/sdk/vine/appfw/AppConfig.hpp`

## 背景

界面文案全部硬编码为中文（appfw GUI 约 140 处，app_shell 与插件同样），`AppConfig::language` 一直是“留给 i18n 的占位、还没接线”。要支持第二种语言，需要先定三件事：**哪些层可翻译**、**源语言是哪一种**、**用哪套目录机制**。底层库（base / viz / robotics / loaders）的日志与异常已经是英文，它们是否进入翻译范围，直接决定分层是否被破坏。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 只有 appfw 及以上层可翻译；源语言中文；Qt Linguist（选定） | 分层不变；两百多条现有文案零改动；`.ts` 可 diff、可评审；`lupdate` 自动抽取 | 源码里不是英文源；需要 `.ts` 维护与 CI 检查 |
| B. 全树可翻译，含 base 的日志与异常 | “一套机制覆盖全部文字” | 底层库被绑上界面生命周期（换语言要重刷日志点）；日志/异常被翻译后 grep、门禁脚本、issue 检索对不上；base 还要依赖翻译头（G2、G4） |
| C. 英文作为源语言，中文作为译文 | 符合 Qt 社区惯例，外部贡献者友好 | 现有约两百条中文文案要译成英文再翻回中文，纯 churn；`.ts` 评审语言与团队不一致（G8） |
| D. 自造 JSON/CSV 目录 + 自己的查找 | 完全可控，不依赖 Qt 工具链 | 要重新实现 context、复数、回退、查找顺序与编辑器（G7）；失去 `lupdate`/Linguist |
| E. gettext（`.po`/`msgfmt`） | 工具成熟，译者熟悉 | 新依赖；与 `QCoreApplication::translate()` 不共享查找，插件还要各带一套 |
| F. `.qm` 编进资源文件**作为唯一手段** | 部署简单，不会缺文件 | 改一个字就要重编程序，翻译更新变成发版（G9）。已取代：保留其部署优点，但改为“内嵌为默认 + 模块旁文件覆盖”→ `ADR-2026-11-01-translation-catalog-embedding.md` |

## 决定

采用 A：

- **范围**：只有 appfw 与它以上的层（宿主、插件）写可翻译文案。判断标准是“这段字属于界面还是诊断”，而不是“用户看不看得见”。
- **源语言**：中文。目录的键是现有中文源串，译文是其它语言；缺译文回退源串。
- **机制**：`QCoreApplication::translate()` + `.ts`（进 git）+ `lrelease` 产出的 `.qm`（构建产物）。目录归属与形态（随模块走、内嵌、可被文件覆盖）由 `ADR-2026-11-01-translation-catalog-embedding.md` 决定。
- **装载**：启动时在 `Application::initialize()` 里装框架与宿主目录（早于任何窗口），插件目录由加载器在 `load()` 前代装、卸载时移除。
- **语言的归属**：语言是配置项 `general.language`（`ConfigManager` 的值 + `ConfigRegistry` 里的元数据，显示在“通用 / 语言”），不是 `AppConfig` 的字段——它要和其它设置同一个家：用户能在配置窗口改、能写进配置文件、能持久化。
- **v1 不做运行期切换**：语言在启动时确定，改语言 = 改配置项 `general.language` 后重启。

## 后果

- 好的：
  - 分层不动：底层库不知道“翻译”存在，日志与异常保持英文、可 grep；
  - 现有文案不用改一个字，只把 `QStringLiteral("…")` 换成 `tr(u8"…")`；
  - 目录是文本，翻译可以单独提交、单独评审，不必等代码发版。
- 坏的 / 代价：
  - 源码里的字符串不是英文；
  - `.ts` 需要随文案变更更新（obsolete/missing 检查进 CI）；
  - 译文变长会挤占版式（Backstage 行高、按钮宽度都按中文排过），需要目视检查每种语言。
  - 崩溃窗口与崩溃报告仍是硬编码中文（base 零 Qt 依赖，本轮不管；见 backlog T7/T8）。
- 触发条件（什么时候重开）：
  - 需要运行期切换语言（此时补 `setLanguage()` 与全部控件的 `LanguageChange` 处理）；
  - 出现大量外部（非中文）贡献者，届时再评估选项 C；
  - 需要翻译插件元数据（名字/描述）。

## 证据

- 现有文案分布：`src/fw/appfw/src/gui` 下约 140 处中文字面量；`app_shell` 与 `model_viewer` 同样。
- 语言设置的现状：没有可用的语言字段——`AppConfig` 里的 `language` 一直只是“留给 i18n 的占位、还没接线”，本轮连同它一起撤掉，改由配置项 `general.language` 承担。
- 占位字段：`src/fw/appfw/sdk/vine/appfw/AppConfig.hpp` 的 `language`（注释即“not wired yet”）。
- 装载时机：`src/fw/appfw/src/gui/GuiApplication.cpp`（`initialize()` 之后才 `createWindows()`，主窗在构造时就 `setText`）。
- 设计：`../design/appfw-translation.md`。
