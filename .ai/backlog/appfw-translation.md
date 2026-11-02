# appfw 翻译（i18n）待办

> 只列开放项；没有触发条件就不做。关闭项直接删除。

## 1. 现在可做（无触发条件）

（无）

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | 运行期切换语言 | `GuiApplication::setLanguage()` + 每个控件在 `QEvent::LanguageChange` 里重设文本（含 SARibbon / DockingPanes 上游控件） | 产品提供“语言”设置项，且要求不重启生效 | `../design/appfw-translation.md` §2.8 |
| T2 | 剩下几个模块的英文目录 | `demo_plugin` / `test_plugin` / `gfx_backend_vsg` 与宿主（`src/apps/vine`）各建一本 `<id>_<locale>.ts`，用 `vn_add_translations()` 嵌入；`appfw`（164 条）、`app_shell`（57 条）、`model_viewer`（35 条）已完成 | 需要英语界面（外部用户或演示） | `../design/appfw-translation.md` §9.1、F5、C7 |
| T3 | 插件元数据翻译 | `PluginInfo` 的 name/description 走插件目录，显示点按插件上下文查 | 插件中心需要在英文界面下显示英文名 | 同上 D6 |
| T4 | `lupdate` 的 CI 检查 | CI 跑 `lupdate`，`.ts` 有 diff 就让提交失败 | 第一次出现“改了源串、忘了更新目录”导致的漏译 | 同上 P1 |
| T5 | 文案长度的版式检查 | 用最长的一种译文跑一遍界面截图门禁 | 出现第一次因为译文变长而截断 | 同上 P3 |
| T6 | 翻译文件的部署规则 | 默认内嵌、无散文件；只在需要热修时把 `<id>_<locale>.qm` 放到模块旁的 `translations/`（部署手册写清这个覆盖位与查找顺序） | 出现第一个“不改程序就要修一个错译”的需求 | 同上 §2.10、`../decisions/ADR-2026-11-01-translation-catalog-embedding.md` |
| T7 | 崩溃窗口的文案注入 | `crash::Config` 增加可选界面文案，由 appfw 用 `tr()` 的结果填；handler 保留中文默认 | 第一次需要向非中文用户展示崩溃窗口 | 同上 §2.9 |
| T8 | 崩溃报告字段名改英文 | `报告正文` 的 `崩溃报告/应用/时间/异常/调用栈` 等字段名改为英文（诊断契约） | 第一次把报告拿去跨语言检索或贴进英文 issue | 同上 §9.2 |
