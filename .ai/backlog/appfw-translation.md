# appfw translation (i18n) backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

(none)

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T3 | Plugin metadata translation | `PluginInfo`'s name/description go through the plugin directory, and the display looks them up in the plugin's context | the plugin centre has to show an English name in an English UI | `../design/appfw-translation.md` §1.3, D6 |
| T5 | Layout check for text length | run the UI screenshot gate with the longest translation | the first truncation caused by a longer translation | `../design/appfw-translation.md` §6.3 |
| T7 | Injecting text into the crash window | `crash::Config` gains optional UI text, filled by appfw from the result of `tr()`; the handler keeps its Chinese default | the first time a crash window has to be shown to a non-Chinese user | ibid. §2.9 |
| T8 | English field names in the crash report | the field names of `报告正文` - `崩溃报告/应用/时间/异常/调用栈` and friends - become English (a diagnostic contract) | the first time a report is searched across languages or pasted into an English issue | ibid. §9.2 |
| T9 | The host executable's catalogue | once `src/apps/vine` carries its first translatable string of its own, design an independent context and a startup-time catalogue entry | `lupdate` finds non-zero strings in the host module | ibid. §2.6, §9.2 |
