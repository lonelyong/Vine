# appfw 渲染视图

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`RenderControl` / `Viewer` / `SceneView` / `PassChain` 的宿主面；表面生命周期见
> `appfw-render-surface.md`，图形层配方与 pass 调度见 `graphics-architecture.md`。
> **权威来源**：`RenderControl.hpp`、`Viewer.hpp`、`ViewerOptions.hpp`、`SceneView.hpp`、`PassChain.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

宿主想要一块 3D 区，过去要自己重复“建管线 + 跟窗口 resize + 填 DPR + 处理输入”的记账；图形 SDK 又把
“配方”（走哪条链、有哪些 overlay）和“几何”（相机、内容、视口）混在视图上。两边都需要一条清楚的分层。

### 1.2 目标

- 宿主用一行配置拿到一块能画、能缩放、能交互的 3D 区。
- 想要更深时能自己建链、自己声明主 pass，不被高层挡住。
- 配方变化只付一次建链；运行期开关（着色模式、overlay 显隐）零重建。
- 一块区只有一幅画，清屏/顺序/呈现没有第二种答案。

### 1.3 非目标

- 多格/分屏/PiP 进框架：由应用自己算矩形、给 pass 子视口。
- 一个文档一个顶层窗口：今天一个控件一块区。
- 框架内置选择/高亮/框选：拾取在 SDK，选择是业务。
- 超过 4 份同时存在的 3D 文档：`VSG_MAX_DEVICES=4`，更多要给 vsg 打补丁。

### 1.4 一句话结论

`RenderControl` 给宿主一块 3D 区；`Viewer` 把宿主请求（`ViewerOptions`）变成一条 `PassChain` 并采纳到
`SceneView`；宿主想要更深就自己 `PassChain::build()` + `view()->setChain()`，低层出现时高层让位。

## 2. 设计要点

### 2.1 对象与所有权

| 对象 | 拥有 | 不拥有 |
| --- | --- | --- |
| `RenderControl`（公开控件） | 私有 `SurfaceWindow`；转发 `engine()` / `view()` / `viewer()` / `surface()` / `init()` / `state()` | 不自己建链、不自己解释配方 |
| `SurfaceWindow`（私有实现） | 原生 `QWindow`、`RenderEngine`、`SceneView`、`Viewer`；attach 规则、延迟 resize、Qt 事件 → 视图输入 | 不存第二份表面/尺寸事实 |
| `RenderSurface`（图形层） | 一块区的呈现声明、尺寸、目标；只**解析**声明 | 不建链、不建视图、不猜默认 pass |
| `SceneView`（图形层） | 相机、内容场景、操纵器、二维组与输入、**采纳的那条链**（强引用） | 不拥有配方、不拥有表面 |
| `PassChain`（图形层） | 一条链的 pass、离屏目标、overlay、`ChainDesc` 的构建结果；析构时把 pass 还给引擎 | 不拥有引擎（租约）、不拥有视图 |
| `Viewer`（appfw） | **一块区的请求**（`ViewerOptions`）+ 建链/采纳/声明/重建搬运行期选择 | 不拥有呈现（`isActive()` 读区）、不拥有视图 |
| `RenderEngine`（图形层） | 设备、pass 槽、帧调度 | 不认识视图，也不认识链 |

**一条硬边**：`SceneView` → `PassChain` 是强引用；`PassChain` → `RenderEngine` 是借用租约；
`RenderEngine` → `SceneView` / `PassChain` 没有边（引擎只认识 pass）。

### 2.2 两层 API

**快速层**（`RenderControl` / `Viewer`）：`viewerOptions()` / `setViewerOptions()` / `setShadingMode()` /
`release(view)` / `view()` / `isActive()` / `takeArea()`。`ViewerOptions` 是值：`path`、`mode`、
`gizmo` / `stats`、`offscreen_width/height`、`layer_mask`。

**底层**（图形 SDK）：`PassChain::build(engine, desc)`、`view()->setChain(chain)`、
`surface()->present(pass)`、`view()->setManipulator(...)`。

**让位规则**：宿主在区上声明了自己的主 pass ⇒ `Viewer::isActive()` 为 false，`takeArea()` 不再建默认链；
要把视图还给宿主就 `release(view)`。低层一旦出现，高层不再替它做决定。

### 2.3 一块区只画一幅画

`RenderSurface` 一条呈现，第二次呈现被拒并报一次。多幅画面由应用自己算矩形、给 pass 子视口、自己决定清屏。
宿主接管时 `surface()->present(pass)` 声明自己的主 pass，默认链让位。

### 2.4 输入只有操纵器一条通道

`SceneView` 把窗口事件推给它的操纵器（`CameraManipulator`）；宿主/插件要做交互就换操纵器，没有
`setInputRouter` / `focusedView` 第二条路。拾取在 SDK（`Camera::screenToWorldRay` +
`RayIntersection::intersectScene`），选择/高亮/框选是业务。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 构造只组装，attach 有明确入口 | `new RenderControl()` 不选后端、不 attach；`init()` / `initAsync()` / 第一拍兜底才 attach | 已落地 |
| F2 | `Viewer` 把请求变成链 | `setViewerOptions()` 建链 + 采纳 + 在区上声明 | 已落地 |
| F3 | 配方变化才重建 | 改 `ViewerOptions` = 一次建链；着色模式/overlay 显隐零重建 | 已落地 |
| F4 | 一块区一幅画与宿主接管 | 第二次呈现被拒；宿主主 pass 让默认链让位 | 已落地 |
| F5 | 输入与拾取 | 事件只进操纵器；拾取在 SDK，选择在业务 | 已落地 |
| F6 | 多个 3D 文档各自设备 | 每个文档一块 `RenderControl`；上限 4，第 5 块给理由失败 | 已落地 |

### 3.1.1 F1 构造只组装，attach 有明确入口

- **行为**：构造建引擎、视图、查看器、诊断路由；不选后端、不 attach。第一次 attach 由第一拍事件循环兜底，
  或宿主显式 `init()` / `initAsync()`。
- **约束**（C1）：`engine()->setDevice()`、`viewer()->setViewerOptions()`、`view()->setChain()`、内容装配
  都必须能在构造之后、第一次 attach 之前同步跑完。
- **要求**（R1）：attach 之前不得建链、不得编译管线；attach 之前配置 = 零重建。
- **验收**（T1）：`RenderControlTest.ConstructionOnlyComposesAndTheFirstTurnAttaches`。
- **注意**（N1）：`initAsync()` 是 `co_return init()`，不挂起、不下池；设备/会话/管线与 warm-up 都在应用线程。

### 3.1.2 F2 `Viewer` 把请求变成链

- **行为**：`setViewerOptions(options)` 按 `ViewerOptions` 建 `PassChain`，采纳到 `view()`，并在 `RenderSurface`
  上声明呈现。没有请求时 `takeArea()` 用默认 `ViewerOptions` 建一条。
- **约束**（C2）：链必须为这个视图的相机/内容建；`setChain` 不检查，引擎在形状不一致时报告。
- **要求**（R2）：同一份请求重复设置不得重建（幂等）；区上已有宿主声明时不得建默认链。
- **验收**（T2）：`TheControlsSurfacePresentsItsOwnViewByDefault`、
  `ADeferredViewerSwitchesChainOnlyWhenTheRequestNeedsIt`。
- **注意**（N2）：`Viewer` 只拥有请求；`isActive()` 读区，不读记忆。

### 3.1.3 F3 配方变化才重建

- **行为**：`ViewerOptions` 是构建请求；改一次 = 一次建链。链已经能表达的 `ShadingMode` 只是
  `PassChain::setShadingMode()` 的一次 override；overlay 显隐是 `setGizmoVisible()` / `setStatsVisible()`。
- **约束**（C3）：`PassChain::effectivePath(desc)` 是“请求会建成哪条链”的唯一规则：Deferred 请求带非 Lit 模式时
  建 Forward，但 `desc.path` 保留，切回 Lit 会再建 Deferred。
- **要求**（R3）：运行期开关零重建；重建必须搬运行期选择（用户隐藏的 overlay 仍隐藏）。
- **验收**（T3）：`ARebuildCarriesTheLiveChainsRuntimeChoices`。
- **注意**（N3）：`setViewerOptions()` 重建后区上已有声明不动，声明会重新解析到新链的 presenting pass。

### 3.1.4 F4 一块区一幅画与宿主接管

- **行为**：`RenderSurface` 一条呈现；第二次呈现被拒并报一次。宿主 `surface()->present(pass)` 声明自己的主 pass 后，
  `Viewer::isActive()` 为 false，`takeArea()` 不动；`release(view)` 把视图还给宿主。
- **约束**（C4）：一块区不能同时有宿主 pass 和默认链；一个区两幅画没有正确的清屏/顺序答案。
- **要求**（R4）：宿主接管时 `Viewer` 不得建链、不得声明；`release(view)` 后链仍采纳在视图上，由宿主替换。
- **验收**（T4）：`AHostPassTakesTheAreaAndTheViewerStandsDown`、`ReleasingTheViewTakesItOutOfTheArea`。
- **注意**（N4）：多格/分屏不在框架；应用自己算矩形、给 pass 子视口。

### 3.1.5 F5 输入与拾取

- **行为**：`RenderControl` 的容器把 Qt 事件翻成视图输入；`SceneView` 推给操纵器。拾取用
  `Camera::screenToWorldRay` + `RayIntersection::intersectScene`，选择/高亮/框选在业务。
- **约束**（C5）：输入不能绕过操纵器；按下时路由，松开前捕获；拖拽结束不算点击（松弛量在操纵器里）。
- **要求**（R5）：导航行为在换操纵器时保持不变（拾取型操纵器原样转发给内层）。
- **验收**（T5）：`TheViewReceivesInputAndADragKeepsSteeringIt`、
  `ModelViewerTest.AClickSelectsTheGeometryUnderTheCursorAndHighlightsIt`。
- **注意**（N5）：点空白是合法的取消选择；要把选中前那份材质原指针还回去。

### 3.1.6 F6 多个 3D 文档各自设备

- **行为**：每个 3D 文档一块 `RenderControl`（自己的面/会话/设备/管线）；应用级共享的只有 DockPanel/Ribbon 外壳。
- **约束**（C6）：`VSG_MAX_DEVICES=4`；第 5 块由 `vsg::Device` 构造抛异常拒绝，`deviceFailureMessage()` 转成原因。
- **要求**（R6）：第 5 块失败必须带“at most 4 simultaneous render devices”的理由，不能抛到事件循环。
- **验收**（T6）：`ModelViewerTest.EachDocumentViewOwnsItsOwnRenderSurface`、
  `VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException`。
- **注意**（N6）：`<=4` 的固定数组分支不做边界检查，不能让第 5 块写下去。

## 4. 公开接口

### 4.1 `RenderControl`（appfw）

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `engine()` / `view()` / `viewer()` / `surface()` | 借用，控件拥有 | `view()` 在 `release` 后为 null |
| `viewerOptions()` / `setViewerOptions()` / `setShadingMode()` | 转发到 `Viewer` | 见 F2/F3 |
| `init()` / `initAsync()` | 同步、幂等 attach | `initAsync()` 不挂起 |
| `state()` / `failureReason()` | `Pending` / `Presenting` / `Failed` | `failureReason` 非空 ⟺ `Failed` |
| `renderFrame()` / `fitToScreen()` | 按需出帧 / 对准内容 | attach 前不得把状态推到 `Presenting` |

### 4.2 `Viewer`（appfw）

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `setViewerOptions(options)` | 建链 + 采纳 + 声明 | 失败保留旧链与旧请求 |
| `setShadingMode(mode)` | 先试活链开关，表达不了才重建 | 零重建优先 |
| `release(view)` | 撤声明、忘视图、放引用 | 链仍采纳在视图上 |
| `view()` / `isActive()` | 读区/读视图 | 不存第二份答案 |
| `takeArea()` | 无链就建默认链并声明；有宿主 pass 就让位 | attach 时调用 |

### 4.3 图形 SDK

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `PassChain::build(engine, desc)` | 唯一的建链入口；失败返回 null 并报一次 | 见 `PassChain.hpp` |
| `SceneView::setChain(chain)` | 采纳/替换/丢弃链 | 链必须为这个视图的相机/内容建 |
| `RenderSurface::present(view/pass)` | 声明呈现 | 一块区一条呈现 |
| `SceneView::setManipulator(...)` | 换输入处理 | 唯一输入通道 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 配置必须在第一次 attach 之前完成；attach 之前不得建链/编译管线。 |
| C2 | 链必须为这个视图的相机/内容建；`setChain` 不检查，引擎报形状不一致。 |
| C3 | `effectivePath(desc)` 是请求→链的唯一规则；`desc.path` 保留。 |
| C4 | 一块区只能有一条呈现；宿主 pass 与默认链不能并存。 |
| C5 | 输入只能走操纵器；没有第二条路由。 |
| C6 | `VSG_MAX_DEVICES=4`；第 5 块必须由后端拒并给理由。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | attach 之前配置 = 零重建；attach 之后配方变化 = 一次建链。 |
| R2 | 同一份 `ViewerOptions` 重复设置幂等；宿主已声明主 pass 时不得建默认链。 |
| R3 | 运行期开关（模式、overlay 显隐）零重建；重建搬运行期选择。 |
| R4 | 宿主接管时 `Viewer` 不建链、不声明；`release(view)` 把视图还给宿主。 |
| R5 | 换操纵器不得改变导航行为；拾取型操纵器原样转发。 |
| R6 | 设备上限失败必须带原因，不能抛到事件循环。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | 一块区只画一幅画；第二条呈现被拒 + 报一次。 |
| N2 | 一个视图最多采纳一条链；`setChain(nullptr)` 把视图移出帧。 |
| N3 | `Viewer` 只拥有请求，不拥有呈现；`isActive()` 读区，不读记忆。 |
| N4 | 引擎不认识视图/链；链只借引擎，不延长它的寿命。 |
| N5 | `Presenting` ⟺ 帧已经在表面里；之前区域保持隐藏。 |
| N6 | 公开面只有一层配置入口：`ViewerOptions` / `PassChain` 二选一。 |
| N7 | 宿主没有请求时，默认链由 `Viewer::takeArea()` 建；宿主声明了主 pass 就不建。 |

## 6. 注意事项与陷阱

### 6.1 链与视图的形状必须匹配

- 会怎么错：链按另一个相机/内容建，画面是别人的；每个 pass 单独看都合法，下游看不出。
- 怎么判：引擎报告 shape mismatch；`PassChain::draws(scene)` 可查链是否画这个场景。
- 怎么防：链在采纳前就为这个视图建；不要把链跨视图复用。

### 6.2 `initAsync()` 不是后台初始化

- 会怎么错：以为它把设备/会话/管线丢到池上，于是并发改场景或等错时机。
- 怎么判：它 `co_return init()`，不挂起；`InitAsyncCompletesWithoutSuspending`。
- 怎么防：把 `initAsync()` 当同步 attach 的协程拼法；内容装配在它返回之后。

### 6.3 宿主 pass 与默认链并存

- 会怎么错：一个区两幅画，清屏/顺序没有正确答案。
- 怎么判：`Viewer::isActive()` 为 false；`takeArea()` 不动。
- 怎么防：宿主先 `surface()->present(pass)` 声明主 pass，或先 `release(view)`。

### 6.4 多格/分屏的复杂度外溢

- 会怎么错：把分屏、PiP、焦点路由塞回框架，表面/输入/清屏各多一套答案。
- 怎么判：出现 `addView` / `setPaneRect` / `focusedView` 这类 API 就是在回退。
- 怎么防：应用自己算矩形、给 pass 子视口、自己决定清屏；框架只保证一块区一幅画。

## 7. 已否决选项

| # | 否决 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 多格/分屏进框架 | 复杂度应由需要它的业务层说清；框架只保证一块区一幅画。 | — |
| D2 | 全进程共享一个会话 + 每个文档一个 `SceneView` | 现行是每个 3D 文档自带 `RenderControl`/设备；共享会话那套 API 已删。 | — |
| D3 | 每个视图一个独立 widget | 中央区重父化会重建原生窗口；一块区一幅画、容器内切页更简单。 | — |
| D4 | 宿主 pass 与默认链并存 | 一个区两幅画没有正确的清屏/顺序答案。 | — |
| D5 | `setInputRouter` / `focusedView` 多路输入 | 输入只有操纵器一条通道；多格下线后没有第二个去处。 | — |
| D6 | 把配方塞回 `SceneView::Options` | 配方属于链，视图只采纳；两套配置面会分叉。 | — |
| D7 | 自动把 Deferred 请求降成 Forward | 请求与结果分开：`effectivePath()` 说清建了哪条，`desc.path` 不改。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 构造只组装、第一拍 attach | `RenderControlTest.ConstructionOnlyComposesAndTheFirstTurnAttaches` |
| T2 | 控件默认画自己的视图；请求变化才重建 | `TheControlsSurfacePresentsItsOwnViewByDefault`、`ADeferredViewerSwitchesChainOnlyWhenTheRequestNeedsIt` |
| T3 | 重建搬运行期选择 | `ARebuildCarriesTheLiveChainsRuntimeChoices` |
| T4 | 宿主主 pass 接管、释放视图 | `AHostPassTakesTheAreaAndTheViewerStandsDown`、`ReleasingTheViewTakesItOutOfTheArea` |
| T5 | 输入到视图、拖拽仍转向机 | `TheViewReceivesInputAndADragKeepsSteeringIt` |
| T6 | 每个文档视图自带渲染面；第 5 块给理由 | `ModelViewerTest.EachDocumentViewOwnsItsOwnRenderSurface`、`VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException` |

## 9. 状态与演进

### 9.1 当前状态

F1–F6 均已落地；多格/共享会话已删除。`Viewer` 是一块区的请求半边，`SceneView` 采纳一条 `PassChain`。

### 9.2 已知缺口 / 待办

- 超过 4 份 3D 文档需要给 vsg 打补丁；本树保持原版 + 门禁。
- 多窗口（一个文档一个顶层窗口）不在本设计范围。
- 逐实例属性 / GPU indirect 不在渲染视图层，见 `../backlog/graphics.md`。

### 9.3 兼容性 / 迁移

旧名 `Pipeline` / `PipelineOptions` 已改为 `PassChain` / `ChainDesc`；旧 API `RenderControl::present()`、
`SceneView::removeWindowPass()` 公开、`setInputRouter` / `focusedView` 均已删除。新代码只按 §4 的接口写。

## 10. 参考

### 10.1 代码

- `src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/Viewer.hpp`
- `src/fw/appfw/sdk/vine/appfw/gui/ViewerOptions.hpp`
- `src/viz/graphics/sdk/vine/graphics/SceneView.hpp`
- `src/viz/graphics/sdk/vine/graphics/PassChain.hpp`
- `src/viz/graphics/sdk/vine/graphics/RenderSurface.hpp`

### 10.2 文档与 ADR

- `appfw-render-surface.md`
- `appfw-startup-splash.md`
- `graphics-architecture.md`
- `../backlog/graphics.md`

### 10.3 外部资料

- 宿主用法：`docs/guides/render-views.md`
- 拾取参考实现：`src/plugins/model_viewer/src/ModelRenderView.hpp`
