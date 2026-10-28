# ADR-2026-10-28：中央区只显示当前文档（浮动的当前文档用说明页占位）

- 状态：已采纳
- 日期：2026-10-28
- 相关：`../design/appfw-document-model.md`（§3.1.5、§5.3 N8、§6.6、§7 D7） ·
  `ADR-2026-10-28-document-tabs-and-floating.md`、`ADR-2026-10-24-tabs-not-mdi.md`、
  `ADR-2026-10-24-command-target.md`

## 背景

多文档已经有 tab 条与“浮动到自己的顶层窗口”。原来的宿主在浮动时让中央区保留“最后一块停靠页”，
于是出现三个各自为政的答案：

- tab 条选中当前文档 B（它已经浮出去了）；
- 中央区还在显示 A 的页；
- `resolveCommandTarget()` 取的是当前视图 B 的渲染控件。

只要用户激活任意一个文档窗口，“哪个是当前文档”就会出现第四个候选答案。若让主窗激活反切 current，
宿主就得监听它并不拥有的主窗口，而且切换瞬间仍有“中央区显示 A、current 是 B”的窗口期。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 中央区保留最后一块停靠页 | 浮出去时视觉连续 | 两套真源；主窗激活后命令仍落在浮动文档上 |
| B. 中央区只显示当前文档；当前文档浮动时显示说明页（选定） | 选中 tab、current、中央内容、命令目标永远一致；主窗激活不需要钩子 | 浮出去时中央区变成说明页，不再“顺便看着上一份” |
| C. 主窗激活时把中央区最后显示的文档设为 current | 视觉连续且 current 跟随 | 宿主必须监听主窗；与“激活浮动窗口即 current”形成两条规则，切换瞬间仍可能分歧 |

## 决定

1. **`DocumentManager::current()` 是唯一所有者**。宿主不保存“当前文档”副本，只在
   `syncToCurrentDocument()` 一处派生 tab 选中、中央内容、被 `activate()` 的视图。
2. **中央区要么是当前文档的页，要么是说明页**：没有当前文档 / 当前文档在独立窗口 / 类型没有视图。
   说明页要说出是哪一种（浮动的还要说出文档标题）。
3. **激活即请求**：浮动窗口被激活只调用 `setCurrent()`；点 tab 同样只调用 `setCurrent()`；API `setCurrent()`
   反向让 tab 与中央区跟上。宿主不是第二个所有者。
4. **浮动文档仍是 tab**：不进入停靠树（C7 不变），停回只有三条路——窗口的“停靠”按钮、tab 右键菜单/API、关文档。
5. **关掉当前文档时宿主请求选中接替它的那一格**（同位置，否则前一个）。`QTabBar` 在非空时总有一个 current
   （有 tab 时 `setCurrentIndex(-1)` 是空操作，Qt 6.10 实测），“无选中”画不出来，所以模型也不能停在“有文档、
   没有当前文档”，否则条上高亮与中央区内容必然分歧（N8）。

## 后果

- 好的：任意时刻“tab 选中 / 中央内容 / 命令目标”三者一致，可被单条用例钉住；主窗不需要激活钩子；
  多窗口下“哪个窗口激活哪个就是当前文档”不需要额外状态；关掉当前文档后接替的那一份直接显示，不留空中央区。
- 坏的 / 代价：当前文档浮出时中央区不再显示另一份文档（这是有意的）；用户想同时看两份，只能让其中一份停在
  中央区（多视图不在本设计范围）。
- 触发条件（什么时候重看 A/C）：出现“中央区要并排显示多份文档”或“主窗与浮动窗各自有独立命令目标”的真实
  需求——那时“一个应用一个 current”本身要重新设计。

## 证据

- `tests/test_gui/DocumentViewTest.cpp`：
  `EveryOpenDocumentGetsATabAndATabSelectionSelectsTheDocument`（鼠标换 tab 产生 currentChanged）、
  `TheCentralAreaOnlyEverShowsTheCurrentDocument`（不变量）、
  `ActivatingAFloatingDocumentsWindowSelectsIt`（激活即当前）、
  `ADocumentFloatsIntoItsOwnWindowAndDocksBackIntoItsTab`（浮动/停回）。
- `src/fw/appfw/src/gui/CentralDocumentHost.cpp` 的 `syncToCurrentDocument()`。
