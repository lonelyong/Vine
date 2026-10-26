# appfw 文档模型设计

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：文档身份/生命周期、打开与保存、类型元数据、视图层与中央客户区。
> **权威来源**：`Document.hpp`、`DocumentManager.hpp`、`DocumentView.hpp`、`tests/test_appfw` / `tests/test_gui`。
> **待办**：无（开放项见 `../backlog/`）

## 1. 概要设计

### 1.1 问题与背景

`RenderControl` 给一个控件 = 一个原生 surface = 一个 `RenderEngine` = 一个 `SceneView`；外壳拥有控件与会话，
内容插件往里填 scene。没有身份、没有生命周期、没有“关闭/保存/最近打开” ⇒ 那只是**视图内容**，不是文档。

### 1.2 目标

- 框架定义**文档身份 + 生命周期**：`Document` + `DocumentManager`。
- 文档类型由插件注册；打开按载荷类型路由；保存写回文档来源。
- 视图层按文档实例；中央区由宿主决定怎么摆。

### 1.3 非目标

- 不定义区域数量、标签、布局、单/多文档、展示类型集合。
- 不让 `core` 认识 `Vfs`/IO；`Document::save()` 无参数。
- 不做 `closeAll()`、`dirtyChanged`、按插件记 owner。
- 多窗口（一个文档一个顶层窗口）不在本设计范围。
- 默认宿主同一时刻只显示一个视图（tab 风格）；多个视图并排可见（MDI/分屏）不做，几块 3D 区并排由**视图自己的窗格**表达（见 `../decisions/ADR-2026-10-24-tabs-not-mdi.md`）。

### 1.4 一句话结论

框架只定义“文档”（身份 + 生命周期）与“文档类型”的注册；打开 = `open(payload)` 按载荷的 C++ 类型路由，
位置是 `Vfs + 虚拟路径`，保存写回 `DocumentSource`；有几个区、怎么摆、什么展示归具体 app。

## 2. 设计要点

### 2.1 三条不同的轴

| 轴 | 归谁 |
| --- | --- |
| surface + 设备 + attach 时机（会话） | 外壳（一个窗口一个会话，管“画在哪”） |
| camera + content scene + 操纵器（视图状态） | 文档类型提供的视图 |
| 身份 + 生命周期 + 数据（文档） | 框架持生命周期，插件提供类型 |

### 2.2 框架的面积

| 层 | 内容 |
| --- | --- |
| core SDK（无 Qt） | `Document` + `DocumentManager`（类型注册、`create`/`open`/`close`/`current`、三个事件） |
| gui SDK | `DocumentView` + `DocumentViewRegistry` + `CentralDocumentHost` |
| 具体 app（外壳插件） | 决定单/多文档、用不用默认宿主、区域怎么摆 |
| 插件（文档类型） | 注册 typeId + 视图工厂，决定展示形态 |

一条判据：只开表格文档的 app 不该为 3D 付钱；多文档 app 不该需要 N 个 device；单文档 app 不该写标签条代码。

### 2.3 所有权

- `DocumentManager` 进程一份，挂在 `Application::documentManager()`，建在 `initialize()`，早于插件加载。
- 管理器拥有文档；`current` 是其中一个或 nullptr。
- 视图层只在 `GuiApplication`；视图按文档实例，`content()` 是放进中央区的东西。
- 中央区是单槽，`setCentralWidget()` 会重父化（Qt 重建原生窗口）⇒ 放一个容器、只装一次、内部 `QStackedWidget` 切页。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 文档语义 | `Document` 纯数据 + 生命周期；`DocumentManager` 管集合/当前/事件 | 已落地 |
| F2 | 类型注册 | `registerType(type_id, display_name, factory)`；重名拒绝 | 已落地 |
| F3 | 打开 | `open(payload)` 按载荷 C++ 类型路由 | 已落地 |
| F4 | 保存 | `save()` 无参写回 `DocumentSource`；基类默认拒绝 | 已落地 |
| F5 | 视图层 | `DocumentView` + 注册表 + 中央宿主 | 已落地 |
| F6 | 类型元数据/面板 | `DocumentTypeInfo` + `DocumentManagerDialog` | 已落地 |

### 3.1.1 F1 文档语义

- **行为**：`Document` 有 `typeId/title/isDirty/canClose` + `source()`/`save()`；`DocumentManager` 有
  `create`/`open`/`close`/`current` 与 `opened`/`closed`/`currentChanged`。
- **约束**（C1）：`create` 不动 `current`；`setCurrent` 只接受本管理器拥有的文档；值没变不发事件。
- **要求**（R1）：`close(doc)` 先问 `canClose()`；被拒返回 false，集合与 current 不动。
- **验收**（T1）：`tests/test_appfw/DocumentTest.cpp`。
- **注意**（N1）：被关文档先出集合再发事件；事件期间对象仍活着；`currentChanged(null)` 先于 `closed`。

### 3.1.2 F2 类型注册

- **行为**：`registerType(type_id, display_name, factory)`；一个 id 一个类型。
- **约束**（C2）：重名拒绝；空 id / 空工厂拒绝。
- **要求**（R2）：工厂返回的文档 `typeId()` 与注册不符 ⇒ nullptr + warning。
- **验收**（T2）：`DocumentTest` 的注册/重复注册用例。
- **注意**（N2）：注册必须在插件 `load()` 里；管理器早于插件加载建好。

### 3.1.3 F3 打开

- **行为**：`open(payload)` 只收 `raw_ptr<const Object>`；键 = `payload->getType()`；按载荷类型找打开器。
- **约束**（C3）：载荷派生 `Object` 且有自己的 `desc()`；框架不解释/不拷贝/不持有载荷对象。
- **要求**（R3）：具体者优先 → `refine` 分数 → `priority` → 登记顺序；无人受理返回 nullptr + warning。
- **验收**（T3）：`AnOpenerBuildsADocumentFromAPayload…`、`TheFrameworkDoesNotCopyAPayload`。
- **注意**（N3）：临时载荷要先起局部再取地址；`open()` 只考虑开始时已登记的打开器。

### 3.1.4 F4 保存

- **行为**：`Document::save()` 无参，写回自己的 `DocumentSource`；基类默认拒绝。
- **约束**（C4）：`DocumentSource` 是值类型 `{scheme, address}`，不派生；“没有来源”= 空值。
- **要求**（R4）：core 不认识 `Vfs`；另存为归类型自己的接口。
- **验收**（T4）：`DocumentTest` 的保存/来源用例。
- **注意**（N4）：失败不用 bool 丢信息；类型接口可用 `io::Result`/`IoError`。

### 3.1.5 F5 视图层

- **行为**：`gui::DocumentView`（`Control` 薄包装 + `document()` + `content()` + `activate()`/`deactivate()`）；
  `DocumentViewRegistry`（type_id → 工厂，一类型一个）；`gui::CentralDocumentHost`。
- **约束**（C5）：视图按文档实例；`content()` 是放进中央区的东西；2D/3D 都返回自己。
- **要求**（R5）：宿主拥有视图对象、容器拥有页控件；交给宿主的视图必须 `owns=false` 包控件。
- **验收**（T5）：`tests/test_gui/DocumentViewTest.cpp`（5 条）。
- **注意**（N5）：中央区单槽、`setCentralWidget()` 重父化；放容器、只装一次、内部切页。

### 3.1.6 F6 类型元数据/面板

- **行为**：`DocumentTypeInfo`（owner/icon/can_create/payload_types/source_schemes）+
  `gui::DocumentManagerDialog` + `app_shell` 的 `show_document_types` 命令（只读）。
- **约束**（C6）：类型没有启用/禁用开关；owner 来自 `Application::registrationOwner()`。
- **要求**（R6）：面板如实显示“只能打开/可创建”，不假装。
- **验收**（T6）：`DocumentManagerDialogTest`。
- **注意**（N6）：面板注册表不存在 ⇒ 面板只能插件自己建自己挂。

## 4. 公开接口

### 4.1 core

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `Document` | `typeId/title/isDirty/canClose` + `source()`/`save()` | 纯数据 + 生命周期 |
| `DocumentManager::registerType` | 一 id 一类型 | 重名/空 id/空工厂拒绝 |
| `DocumentManager::create/open/close/current` | 集合与当前 | 事件见 §5 |
| `DocumentManager::registerOpener<Payload>` | 按载荷类型登记 | `DocumentOpenerRegistration` |
| `opened/closed/currentChanged` | 事件 | 应用线程 |

### 4.2 gui

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `DocumentView` | `document()` / `content()` / `activate()` / `deactivate()` | 按文档实例 |
| `DocumentViewRegistry` | type_id → 工厂 | 重复/空 id/空工厂拒绝 |
| `CentralDocumentHost` | 中央区页容器 | 六条契约见头文件 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | `create` 不动 `current`；`setCurrent` 只接受本管理器文档。 |
| C2 | 类型 id 唯一；空 id / 空工厂拒绝。 |
| C3 | 载荷派生 `Object` 且有 `desc()`；框架不解释/不拷贝/不持有。 |
| C4 | `DocumentSource` 是值类型，不派生；没有来源 = 空值。 |
| C5 | 视图按文档实例；`content()` 是放进中央区的东西。 |
| C6 | 类型没有启用/禁用开关；owner 来自 `registrationOwner()`。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | `close` 先问 `canClose()`；被拒不动集合/current。 |
| R2 | 工厂返回的 `typeId()` 必须与注册一致。 |
| R3 | 打开器选择顺序：具体者优先 → refine → priority → 登记顺序。 |
| R4 | core 不认识 `Vfs`；`save()` 无参。 |
| R5 | 宿主拥有视图对象、容器拥有页控件；`owns=false`。 |
| R6 | 面板如实显示可创建/只能打开。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 被关文档先出集合再发事件；事件期间对象仍活着。 |
| N2 | `Vfs` 借用且比文档活得久；同一实例不可两线程同碰。 |
| N3 | 载荷只在 `open()` 期间有效；文档不得存指向载荷的指针。 |
| N4 | 视图按文档实例；切回复用同一个，关闭才销毁。 |
| N5 | 中央区单槽、重父化；容器只装一次。 |

## 6. 注意事项与陷阱

### 6.1 载荷生命周期

- 会怎么错：文档存了载荷指针，`open()` 返回后载荷是临时量 → UAF。
- 怎么判：ASan；文档里出现 `raw_ptr<const Payload>` 成员。
- 怎么防：打开器把需要的指针/句柄拷进文档或来源；大块数据放进载荷字段并拷 `shared_ptr`。

### 6.2 关文档的事件次序

- 会怎么错：`closed` 里挑下一个当前文档，被随后的 `currentChanged` 覆盖。
- 怎么判：`currentChanged(null)` 必须早于 `closed`。
- 怎么防：按 §3.1.1 的次序实现；`close()` 返回时才销毁。

### 6.3 `Vfs` 寿命与线程

- 会怎么错：关文档就销毁 `Vfs`，文档的 `save()` 还要用它。
- 怎么判：`Vfs` 借用、无内部锁。
- 怎么防：宿主按“先关文档、再销毁 Vfs”；同一 `Vfs` 不跨线程。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 用 `path` 而不是 `Vfs + 虚拟路径` | 网络/数据库/私有格式无法表达。 | — |
| D2 | `DocumentSource` 派生 | 两套种类层次；失去值语义；负担推给每个文档类型。 | — |
| D3 | 载荷用 `shared_ptr` | 保活的是描述符，不是它指向的 Vfs/缓冲；框架不知道该持有多久。 | — |
| D4 | 按插件记 owner | 等真实需求再加。 | — |
| D5 | `dirtyChanged` 事件 | 文档类型自己发变化事件，宿主订阅即可。 | — |
| D6 | `closeAll()` | 宿主三行循环；“部分失败”策略归宿主。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 文档语义/事件次序/关闭 | `tests/test_appfw/DocumentTest.cpp` |
| T2 | 类型注册/重复/空 | 同上 |
| T3 | 打开器选择/不拷贝 | `AnOpenerBuildsADocumentFromAPayload…`、`TheFrameworkDoesNotCopyAPayload` |
| T4 | 保存/来源 | `DocumentTest` |
| T5 | 视图层/中央宿主 | `tests/test_gui/DocumentViewTest.cpp`（5 条） |
| T6 | 类型元数据面板 | `DocumentManagerDialogTest` |

## 9. 状态与演进

### 9.1 当前状态

core 文档模型、打开/保存、类型元数据、视图层与中央宿主均已落地。

### 9.2 已知缺口 / 待办

- `MeshLoader::load()` / `loadMerged()` 只吃路径与流，没有 `Vfs` 入口 ⇒
  “`Vfs` + 虚拟路径”的模型文件读不进。
- 面板注册表不存在 ⇒ 面板只能插件自己建自己挂。
- 多窗口不在本设计范围。

### 9.3 兼容性 / 迁移

`DocumentSource` 是值类型；`Document::save()` 无参；新代码按 §4 的接口写。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/Document.hpp`
- `src/fw/appfw/sdk/vine/appfw/DocumentManager.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/DocumentView.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/CentralDocumentHost.hpp`
- `tests/test_appfw/DocumentTest.cpp`、`tests/test_gui/DocumentViewTest.cpp`

### 10.2 文档与 ADR

- `appfw-render-view.md`
- `appfw-userio.md`
- `../backlog/`

### 10.3 外部资料

- `src/base/iobase` 的 `Vfs` / `Result` / `IoError`。
- `src/plugins/model_viewer/` 的参考实现。
