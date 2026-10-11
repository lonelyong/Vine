# graphics / vsg 后端 —— 现役要点

只记**现役**：机制、判据、坑，以及"当时为什么这么定"。过程叙述与重写前那一代（`VsgRenderer`/`SceneBridge`/
`VsgContentSlot`/`OwnedCache` 等）已删除（2026-09-27 清掉 1839 行），历史见 git；分层与依赖方向见
`.ai/design/graphics-layering.md`，逐条实施记录见 `.ai/design/vsg-reimplementation.md`。

**读法**：每条是"一天一片"的记录，条目之间互相引用（`见 M11j`／`收掉 M3d-2`／`§11.16xx`），按时间倒序排。
**写新条目时**：结论在前、判据（门禁数字/变异 N/N 红）与坑不可省；出现已不存在的东西时——句子本身在讲
"旧名/已删"就加 `<!-- drift-ok -->`，整节都是记录就把标题标成 `（历史登记）`（`scripts/check_ai_docs.py` 会查）。

## 索引（按主题找，条目按时间倒序在下面）

| 主题 | 先看这些条目 |
|---|---|
| **模块布局与依赖** | 顶部 2026-09-27 四条：帧机器归属 `core`→`backend` / 诊断路线合并 / 帧时钟查询面 `FrameProgress` / 放置判据与 `frame/` 最终形态；规则全文在 `.ai/design/graphics-layering.md` |
| **一帧怎么走** | M11h 空绘制调用不是绘制调用 · M11m episode 归半片 · M11o 拒绝报告合并 · M11w 块预算按需增长 · M11x B5 触发规模 · M11i 租约顺序相位 · B8 跨帧差异 · B9 在飞槽数地板 |
| **着色器与 ABI 契约** | shader 文件命名 `builtin_<角色>.<阶段>` · `vn_`→`vine_` 前缀 · texcoord kind 显式化 · P0.A/P0.B1/P0.C1（前向着色归 SDK / location 表 / L1 块布局） · P12 文件化 + 构建期嵌入 · program 槽 P1 三步 |
| **证据与门禁** | M7 每帧编译 + 描述符（`PassDesc`）· M8 SDK 级设备夹具 + 像素判据（`SdkPixelTest`） · M11k 背景看写掩码 · M11l 第三条画面判据"整片背景灰" · FPS 窗口真平均 · ASan 两片（余量收口 / 全量 test_vsg 首通） · V3/V4 上游能力门禁 · R5 LSan 抑制 · M11g 计数分配器（**`skipped == 0` 才是跑过设备电池**） |
| **宿主窗口与 X11** | M11j 读窗口偶发红（VIEWABLE 之前呈递到不了 / 空帧不是 settle / 读错误 0 vs 8） · M11ae·M11af 首帧叠层与启动尺寸 |
| **SDK 与引擎形态** | 后端契约（规范性在 `RenderBackend.hpp`） · Design C（RenderEngine 瘦身 + SceneView） · Scene 收敛单根 · Material 透明度移除 · 拓扑归位 · State 切片 · 边界框 `Aabbd` |
| **面向使用者** | `docs/usage.md`（模块文档一节） |
| **重新设计（含后端契约）** | `.ai/design/graphics-redesign.md`（2026-10-11 提案）：病根 = 帧在引擎里已是值、边界上又被打散成一串有顺序的可变调用；**六片全部落地**：S1 / A / S2 前半 / S3（前半：帧由值驱动；之一：`executeFrame`；之二：预热删除；**收口：平坦路径与 `Protocol` 一起删除**）/ **S4b（窗口成为可选依赖：`surface()` + `RenderSurfaceBinding`）** / **S5（能力与观测各收成一个值：`Capabilities` / `FrameReport`）** / **S6（宿主侧：尺寸唯一 owner + `SceneView` 六角色 + 呈现知识归表）** |
| **待办与口径** | **2026-10-11 按"机制/组合"重评的待办清单**（`graphics-render-next.md` §5.1：**T1 表面尺寸扇出（已落地）** / T2 M6 / **T3 设备夹具探测（已落地）** / T4 布局归应用层 / T5 组合归应用层 / T6 便利边界 / T7 叠层跨重建）；这两条的落地记录见下面 2026-10-11 T1+T3 那条 |


## 2026-10-11 S6 落地：宿主侧收口（尺寸唯一 owner + `SceneView` 六角色 + 呈现知识归表）

- **尺寸唯一 owner**：`SceneView` 不再存表面尺寸的副本 —— `surface_width_/surface_height_/pixel_ratio_` 改名
  `own_*`（含义＝"**没有表面时**这个视图的尺寸"），挂上表面后几何**问**表面
  （`surfaceWidth()/surfaceHeight()/surfacePixelRatio()`）；`RenderSurface::resize`/`present` 不再推三个数，
  而是让视图重算矩形（`applySurfaceGeometry()`）；**摘下来时表面把尺寸交还**给视图（同矩形那条规矩）。
  `onSurfaceResized` 语义不变（挂着转发 / 没挂记自己那份）。
- **六角色分组**：`SceneView` 公开面按 recipe / camera and content / navigation / geometry / input / presentation
  重排并命名（对齐文件里既有的 `// ---- … ----` 分段，写进类注记）；**不改行为、不动 ABI**。
- **三条"不是宿主 API"点名**：`setSurface`（表绑/摘视图 + 交回尺寸）、`applySurfaceGeometry`（表在尺寸变化后
  让视图重算）、`ensureWindowPass`（表要的默认窗口 pass；**保持公开**，因为"没有表面的视图"是合法配置）。
  呈现知识（哪条 pass 呈现 / 矩形 / 清屏 / 顺序 / 尺寸）本来就全在 `RenderSurface`，本片只把它写进文档。
- **判据（实测）**：`test_graphics` **361**（360 + 新用例
  `SceneViewViewportTest.AViewReadsTheSurfaceSizeAndHoldsNoSecondCopyOfIt`）、`test_gui` **253**（不变，appfw 一行未改）、
  `test_vsg` 426（421 + 3 skip + 2 已知红）、appfw 32 / meshio 34 / iobase 94 全绿；全量 Release 0 error / 0 warning；
  五个 gate 0 finding（`--strict` 仍 22 条）。**变异 3/3 红**：视图读自己那份 ⇒ 3 条红；表面不扇出 ⇒ 2 条红；
  摘下来不交还 ⇒ 1 条红。
- **坑 / 记账**：两份实现在正常路径**行为等价**，所以判据只能是"挂上之前喂给视图的话在挂上之后失效"；同理
  "交还尺寸"只在"摘下来后改矩形"这条路上可观察（`setViewport` 对相同矩形提前返回）。

## 2026-10-11 S5 落地：能力与观测各收成一个值（`Capabilities` / `FrameReport`）

- **做了什么**：`RenderBackend` **删** `supportsRenderTargets()` 与三个出参
  （`frameCounters`/`retentionStats`/`frameProgress`），**换成两个值**：`capabilities()`
  （新头 `sdk/vine/graphics/Capabilities.hpp`，今天一个字段 `render_targets`）与 `frameReport()`
  （`FrameStats.hpp` 新增 `FrameReport`：三个部分各自 `std::optional`，**不保留＝不 engage**）。
  引擎同样两条（`capabilities()`/`frameReport()` 原样转发；无后端＝能力全假、报告全不 engage），
  `frame()` 改读 `capabilities().render_targets`（**仍然每帧问**）。插件答
  `{ .render_targets = true }`，报告由 `d->observe` + `Session::timeline()` 拼出。
- **两条判据（写进文档）**：① **能力是属性**（初始化期间不变、宿主计划一帧前必须知道）；**调用自己能答的不进
  `Capabilities`**（能不能读回 → `ReadbackResult::Unsupported`），否则是同一件事的第二处拼写。
  ② **"值放哪个头"看能否独立到达读者**：`Capabilities` 若跟 `ReadbackResult` 一起放契约头，
  引擎头就得包含整张后端契约（它今天只前置声明 `RenderBackend`）——所以它单独成头。
- **判据（实测）**：`test_graphics` **360**（不变；`FrameCountersForward…` 改写为
  `TheFrameReportCrossesTheEngineWholeAndAbsentPartsStayAbsent`：三部分 engage 与否 + 值穿过引擎 + 能力穿过）、
  `test_gui` **253**、`test_vsg` **426 = 421 + 3 skip + 2 已知红**（设备用例改名
  `TheHostReadsTheBackendsOwnFrameReport`；**在 `#if !defined(_WIN32)` 里，本机不编译**）、appfw 32 /
  meshio 34 / iobase 94 全绿；全量 Release 0 error / 0 warning；五个 gate 0 finding（`--strict` 仍 22 条，同一列表）。
  **变异 3/3 红**（各自单独实测）：引擎不转发报告 ⇒ 红；引擎忘记能力 ⇒ 红；契约默认"说零"而不是"沉默" ⇒ 红。
- **坑 / 记账**：收成一个值之后"三个各写一半"的中间状态**从存在变成不可能**，而"不保留"仍说得出来
  （不 engage）—— 删的是形态而不是信息。老条目里点名的旧符号/旧用例名按本仓规矩加 `<!-- drift-ok -->`。

## 2026-10-11 S4b 落地：窗口成为可选依赖（呈现拆成 `RenderSurfaceBinding`）

- **做了什么**：`RenderBackend` **删** `setWindowHandle`/`nativeHandle`/`resize`/`swapBuffers`，**新增** `surface()`
  （默认 null —— "窗口是可选依赖：只画离屏的后端答 null，四个入口一个都不实现"）；窗口那一半成了一张新契约
  `vn::graphics::RenderSurfaceBinding`（`setWindowHandle`/`nativeHandle`/`resize`/`present`），插件 `VsgBackend`
  同时实现两张契约、`surface()` 答 `this`（它一直有窗口：宿主的或自己建的），`swapBuffers()` 更名 `present()`。
  引擎把句柄与尺寸转发给它、`frame()` 末尾 `if (surface) surface->present()`；**宿主 API 与 appfw 一行未改**。
- **新可见性**：句柄公告给 `surface()==nullptr` 的后端 = 接线错误 → `PassProtocolViolation` **上报一次**
  （分裂前这个形态根本表达不出来：入口在契约上、人人有空壳）；反向的**尺寸公告不报**（它仍进引擎 frame context，
  pass 按它排布 —— 宿主对一张只离屏的表面报尺寸是合法的）。
- **判据（实测）**：`test_graphics` **360**（357 + 3，新增 `tests/test_graphics/HeadlessBackendTest.cpp`：只离屏后端
  **零窗口代码**跑完整帧 + 帧真的到达 pass 声明的那张 `RenderTarget` + 一处 present 都没有 / 句柄公告给它的上报 /
  **有**窗口那一半的同批公告静默到达 + 每帧一次 present〔对照组〕）；`test_gui` **253**、`test_vsg` **426**
  （421 + 3 skip + 2 已知红，**含 SDK 像素夹具 ⇒ 真设备上 present 路径逐项不变**）、appfw 32 / meshio 34 / iobase 94
  全绿；全量 Release 0 error / 0 warning；五个 gate 0 finding（`--strict` 22 条，与改动前同一列表）。
  **变异 3/3 红（各自单独实测）**：不问 `surface()` 也无 present ⇒ 3 条红；句柄收了却不交给表面 ⇒ 1 条红；
  句柄公告改回静默 ⇒ 1 条红。
- **坑 / 决策**：设计里那个 `format()` **取消**（全仓找不到读者：窗口的*实际*格式由插件的 `WindowTarget` 从平台读，
  宿主从没问过）—— 真要问的那天，加在 `RenderSurfaceBinding` 上是一行。判据里"并读回"那半句改成"帧到达 pass 声明的
  `RenderTarget`"（用双打读自己写下的字节只证明双打；真设备读回由 `test_vsg` 的既有窗口用例兜住）。X11 那半边的
  54 处窗口调用是**纯机械替换**，Windows 上不编译，未验（同 S3 的记账）。

## 2026-10-11 S3 后半收口落地：平坦路径与 `Protocol` 一起退场

- **做了什么**：契约**只留一个帧入口** —— `RenderBackend::executeFrame(frame)` 变**纯虚**，
  `beginFrame`/`endFrame`/`beginPass`/`endPass`/`setPassOrder`/`setPassState`/`setClearPolicy`/`setViewport`/
  `setLights`/`setPassInputs`/`render`/`drawScreenProgram` **12 个全删**（`RenderBackend.hpp` 的 CALL ORDER 一节
  整段消失：值里没有调用序列可写）。`setRenderTarget` **留下但改义**："pass 画进哪张目标"在值里
  （`PassDescription::target`），它只剩"宿主宣布一张它要用/回读的目标"（持握 + 懒建 + 上报）；
  `RenderPass::announceClear`、`announcePassTo`/`announceDrawsTo`、"默认 `executeFrame`"随平坦路径删除。
  **宿主自己驱动一条 pass** 落成 `singlePassFrame(pass, order, inputs, description)`（`FrameDescription.hpp`
  + `RenderBackend.cpp`）：`RenderPass::execute()` = 描述它 + 把这一帧交给后端 —— **引擎与宿主同一个入口、同一个形状**。
  引擎 `frame()` 去掉 `beginFrame()`/`endFrame()`（帧的身体在 `executeFrame` 里），`swapBuffers()` 仍是唯一呈现。
- **插件**：`beginFrame`/`endFrame` 的体 → 私有 `openFrameBooks`/`closeFrameBooks`；逐条入口的体 → 私有 helper
  （`openPassScope`/`closePassScope`/`announcePassOrder`/`announcePassInputs`/`announceTarget`/`announceClear`/
  `announcePassState`/`announceLights`/`collectContentDraw`/`collectScreenDraw`；目标那两半共享
  `adoptHostTarget`+`reportUnbuildableTarget`）；`executeFrame` 按值走它们。**"description 缺席 ⇒ 一个属性都不发"
  仍成立**（S1 那条门禁原地不动）。
- **`Protocol` 退场**：`backend/Protocol.hpp` + `Protocol.cpp` 删除。`FrameRecorder` 只留书（帧/scope 开合）
  与契约里**唯一不能被值表达**的规则 —— **被释放的目标**（scope 公告的目标被 `releaseRenderTarget` 掉 ⇒
  该 scope 的效果丢弃、绘制**跳过而不改画到窗口**；另外半条"释放别的目标不影响本 scope"从 `CoreProtocolTest`
  搬进 `FrameRecorderTest`）。**顺序判决 + 那套 per-episode 上报（含 `PassProtocolViolation` 的三句话）一并删除**：
  值按构造有序，"没有 scope 可归属的调用"不是错误而是**没有去处**（不生效、返回 false、不上报）。
- **判据（实测）**：`test_graphics` **357**（359 − 2：值→线序列冻结那条与"整帧一个值"那条被值驱动本身取代）、
  `test_gui` **253**（`--gtest_filter=-UserIOTest.PickingFilesFillsTheLineWithoutSubmitting`）、
  `test_vsg` **426 = 421 + 3 skip + 2 已知红**（433 − 7 = `CoreProtocolTest` 的 7 条；**含 SDK 像素夹具 ⇒
  真设备像素不变**）、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 构建
  0 error / 0 warning；五个 gate 0 finding（`check_ai_docs --strict` 仍 20 条历史引用）。
- **测试怎么改的**：`GraphicsTest` 的 `MockBackend` 改成**从值记录**（同一批见证量 + 新增 `frames_executed` /
  `pass_entries` / `entries_closed`）；`VsgBackendTest` 的平坦驱动改成**值驱动**（新增测试头
  `tests/test_vsg/TestFrame.hpp`：把"开 scope / 公告 / 绘制"的老拼写换成同一批事实的链式写法，两个测试文件共用）；
  `FrameRecorderTest` 5 条把上报断言改成"不生效、无上报"；`test_gui/RenderControlTest.cpp` 的 stub 换成
  `executeFrame`；`CoreProtocolTest` 7 条删除并记账（上一条"开工清单"里列的 3c 到此做完）。
- **坑（三条，都进设计）**：
  1. **`announcePassTo`/`announceDrawsTo` 不是"那条路的地基"**（3c 行原话如此）：它们是"描述 → 逐条调用"的映射，
     逐条调用没了，它们就没有读者 —— 宿主自驱一条 pass 只需要"造一个单 pass 的帧"。
  2. **X11 那半边的用例早已编译不过**：`VsgBackendTest.cpp` 里 `#if !defined(_WIN32)` 的 2800 行在 Windows 上不编译，
     而它们**自 M3 起**还在调已删的 `RenderBackend::setDepthMode()` —— Linux 上 `test_vsg` 从那时起不编译。
     本片顺手把这批驱动改成值（`TestFrame`）；**这台机器上没有 X display，它们仍无法在此编译/运行**（Linux 才验）。
  3. **`ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt` 偶发红的原因查明**：它断言"新实例 ≠ 旧指针地址"，
     而被释放的 `RenderTarget` 的**地址会被复用**（十几次里抓到一次）；已改成"活着的实例只有一张 + extent 是新形状"。


## 2026-10-11 S3 前半落地：帧由值驱动（`execute()` 退成宿主直呼）

- **做了什么**：`frame()` 的运行段**不再调 `pass->execute()`**，改为宣告编译期建好的那条 entry
  （`beginPass` / `setPassOrder(entry.order)` / `setPassInputs(entry.inputs)` /
  `announcePassTo`/`announceDrawsTo`（`entry.description == nullopt` ⇒ 一个属性都不发）/ `endPass`）；
  `drawScenePass` 删除。**每条 pass 每帧只被描述一次** ⇒ S2 那条"描述两次"的过渡代价消失，
  两条 memo 见证回到紧值（1/2、1）。**每条 pass 的耗时改在编译里量**（`buildFrameValue` 包住"钩子 + 输入 + 描述"）：
  `record_micros` 还是"这条 pass 上一帧的占比"，只是量在**记录**发生的地方（运行那半的耗时是后端的，见 `FrameCounters`）。
- **契约变化（这一片的实质）**：`RenderPass::execute()` **不再是引擎调用 pass 的方式**，它是"宿主自己驱动一条 pass"
  （预热 / 测试 / 宿主自绘）的拼写；想让引擎跑的活必须放进 `describe()` 或声明的 `before_draw` 钩子。
  写进了 `RenderPass::execute()` / `RenderEngine::frame()` / `FramePlan::record_micros` 的文档与 `usage.md` §2.4/§2.5。
- **判据（实测）**：`test_graphics` 356→**357**（新用例 `TheEngineRunsTheFrameValueAndNeverCallsExecute`：
  覆写 describe/execute 各计数，两帧后 describes = 2、**executes = 0**，且这条 pass 确实到了线上）。
  **变异 1/1 红**：运行段改回 `pass->execute(...)` ⇒ 3 条红。`TheFramePlanCarriesWhatEachPassLastCost` 的 `SlowPass`
  按新事实改写（"覆写 `execute()` 睡觉" → "覆写 `describe()` 睡觉"）；`test_gui` 253 / `test_vsg` 428(+3 skip
  +2 已知红，**含 SDK 像素夹具 = 真设备上像素不变**) / `test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94；
  全量 Release 0 error / 0 warning；五个 gate 0 finding。
- **坑**：这次落地把 S2 记录的那条发现**当场兑现了** —— 把 `frame()` 改成由值驱动的第一版，红的正是
  `TheFramePlanCarriesWhatEachPassLastCost`（覆写 `execute()` 的 `SlowPass` 静默不跑）。所以"改写这条见证"不是
  为了绿，而是**契约变化必须记账**：它的睡觉得搬到 `describe()` 里，才继续说明"计划里的耗时是这条 pass 的占比"。
- **还剩（S3 后半）**：平坦路径本身（10 个方法）+ `backend/Protocol.hpp` 的 8 条顺序规则 +
  `CoreProtocolTest` 的改写记账（**预热那一拍已删**，见下一条）。

## 2026-10-11 S3 后半：执行计划（3a 已取消，剩 3b / 3c）

- **这一片为什么不能一次做完**：它要求**引擎 / 插件 / 测试三边同时到位**（先删接口后迁移插件，或反过来，中间态都是
  一棵跑不起来的树 —— §5 禁止的形态）。拆成两步，每步都能独立验收：
  - **3b 插件吃值**：`VsgBackend::executeFrame` 覆写 = 值的循环 —— `d->passes.adopt(pass.pass)` 取身份 →
    recorder 的 per-pass/per-draw 入口 + **每 draw 的内容副作用**（`d->store->updateMaterial(...)` 那类，
    今天在 `VsgBackend::render()` 里）。逐条方法退成薄转发到同一批 helper，供 3c 删。
    判据：`test_vsg` 全量逐项不变（**含真设备像素夹具**）。
  - **3c 删契约**：`RenderBackend` 删那 10 个方法、`executeFrame` 变纯虚、`Protocol` 的 8 条规则退场、
    `CoreProtocolTest` 记账改写。`RenderPass::execute` + `announcePassTo`/`announceDrawsTo` **留在 SDK**
    （"宿主自己驱动一条 pass"那条路的地基）。
- **调查结论（省掉一步）**：原计划的"3a 给 recorder 加按值采集入口"**不必要** —— recorder 的入口本来就是
  per-pass/per-draw 的，且它们**不是契约**（是插件内部的采集层）；值按构造有序，所以"值 → 这些入口"只是换个调用者。
  而 `PassId` 的所有权在插件（`d->passes.adopt`），"按值采集"反而要往 recorder 里塞 key 函数。
- **3b 的判据已冻结（2026-10-11，test_graphics 359 绿）**：`TheFrameValueBecomesExactlyThisCallSequence`
  把"值 → 线上调用"的序列逐项钉死（帧括号 / 每 pass 的 scope〔身份·order·inputs〕/ 它声明的
  target·viewport·clear·state〔只发它说了的〕/ 每 draw 的 lights + 绘制 / scope 关闭 / 什么都不发的 pass
  只开合 scope / 被禁用的 pass 不在帧里）。`MockBackend` 加了一条 `wire` 调用日志。
  **插件自己的 `executeFrame` 必须产出同一序列** —— 这是"不改行为的重构"该有的安全带（先冻结，再动）。
- **3b 收尾 / 3c 的开工清单（下一次从这里开始）**：
  1. `VsgBackend.cpp`：把 `beginPass`/`endPass`/`setPassOrder`/`setPassInputs`/`setRenderTarget`/`setViewport`/
     `setClearPolicy`/`setPassState`/`setLights`/`render`/`drawScreenProgram` 的**体**逐个搬成私有 helper
     （`applyX(...)`），覆写方法退成一行转发；`executeFrame` 改调 helper。判据：**冻结的
     `TheFrameValueBecomesExactlyThisCallSequence` + `test_vsg` 全量（含真设备像素夹具）逐项不变**。
  2. 3c：`RenderBackend` 删那 10 个方法（`beginFrame`/`endFrame`/`beginPass`/`endPass`/`setPassOrder`/
     `setPassState`/`setClearPolicy`/`setViewport`/`setLights`/`setPassInputs`/`render`/`drawScreenProgram` 中
     属于契约的那些），`executeFrame` 变纯虚；`backend/Protocol.hpp` 的 8 条顺序规则退场；
     `CoreProtocolTest` 用例改写/删除并记账；`MockBackend`、`RenderControlTest` 的 stub、`SdkPixelFixture`
     里逐条驱动的地方同步迁移（**同一提交里改完两侧**）。
  3. 验收命令：`cmake --build build --config Release --target gfx_backend_vsg test_graphics test_vsg` →
     `test_graphics`（359）、`test_vsg`（433 = 428 + 3 skip + 2 已知红）、`test_gui --gtest_filter=-UserIOTest.*`（253）、
     五个 gate。

## 2026-10-11 S3 后半之二落地：预热那一拍删除

- **做了什么**：`RenderEngine::initialize()` 里"每个启用且不清屏的 pass 先执行一遍"的预热**整段删除**（理由留在注释里：
  它当年是为了"让后端在呈现第一帧前看到整个 pass 集合、并把内容编译好"，当时帧是一串调用、内容惰性构建；
  现在**一帧就是一个值**，后端动手前就拿到整个 pass 集合，主内容本来就在 `initialize()` 里预热）。
  于是 `pass->execute()` 在引擎里**一个调用点都不剩**（只剩宿主/测试直呼），与 S3 前半那条契约完全一致。
- **判据（实测）**：`test_graphics` **358** 与删之前逐项相同；**`test_vsg` 433 = 428 + 3 skip + 2 已知红，
  与删之前逐项相同** —— 含 **SDK 像素夹具（真设备渲染 + 读回像素）**，即"没有预热的第一帧画面不变"的实测；
  `test_gui` 253 / `test_appfw` 32 / meshio 34 / iobase 94；五个 gate 0 finding。
- **风险记账**：这条删除的风险点是"第一帧内容构建"（当年正是它引出预热）。实测（真设备像素夹具 + 全量 vsg）没有回归；
  若将来第一帧出现构建类回归，先看 `initialize()` 里那条注释。

## 2026-10-11 S3 后半之一落地：整帧交给后端（`executeFrame`）

- **做了什么**：契约新增 `virtual void RenderBackend::executeFrame(const FrameDescription&)`，**默认实现 = 原先那条
  逐条宣告序列**（每 pass：`beginPass`/`setPassOrder`/`setPassInputs`/pass 自己的事实/它的绘制/`endPass`；
  `description == nullopt` 的 entry 一个属性都不发）。引擎 `frame()` 里那段逐条循环**整体换成一次调用**
  `backend_->executeFrame(frame_value_)`。于是**边界上是值**：后端可覆写它直接从值构建（那时逐条调用一份都收不到），
  不覆写的后端（含 vsg 插件）**行为逐调用不变**（同一个序列，只是从一个输入来）。`RenderBackend.hpp` 的
  CALL ORDER 段、`usage.md` §2.5 已同步。
- **判据（实测）**：`test_graphics` 357→**358**（`ABackendIsHandedTheWholeFrameAsOneValue`：覆写 `executeFrame`
  的后端一帧收到**一次**调用，passes 按运行序带 order 与绘制数、被禁用的不在里面，且
  `beginPass`/`setPassOrder`/`render`/`drawScreenProgram` **一次都没被调**）；**变异 1/1 红**（引擎改回逐条发）。
  其余套件全绿（gui 253 / vsg 428 + 3 skip + 2 已知红，**含 SDK 像素夹具 ⇒ 真设备像素不变** / appfw 32 /
  meshio 34 / iobase 94）；全量 Release 0 error 0 warning。`MockBackend` 补了 `setPassOrder` 记录
  （`order_sets` / `orders`）。
- **意义**：那 10 个逐条入口**已经不再有"引擎的运行"这个读者**了（只剩默认实现、宿主自驱一条 pass、后端自己），
  所以删它们是一次**纯删**。
- **注意**：跑第一次全量时 `ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt` 红过一次，随后 4 次全绿
  （单跑也绿）—— 记在这里，若复现再查。

## 2026-10-11 S2 前半落地：帧的值（引擎产出，可观测）

- **做了什么**：`FrameDescription` 有了帧那一层（`FrameDescription::Pass` = pass 身份 / order / 解析后的 inputs +
  该 pass 自己的 `PassDescription`，nullopt = 这帧什么都不发）；`compileFrame()` 末尾 `buildFrameValue()`
  按运行序跑每条启用 pass 的 `before_draw` 钩子、告诉它解析好的 inputs、请它描述自己 —— **整帧的值在派发前存在**；
  `RenderEngine::frameDescription()` 观测它。`announcePassTo`/`announceDrawsTo` 从 `RenderPass.cpp` 匿名命名空间
  **搬到 `RenderBackend.cpp` 并导出**（声明在 `FrameDescription.hpp`）：描述→平坦调用的映射只有一份。
  **帧仍由 `execute()` 驱动**（`drawScenePass` 原样）；"帧由值驱动 + 新入口 `executeFrame(desc)`"归 S3/S2 后半。
  → **同日已由 S3 前半接上**（见上一条）：帧跑的就是这个值，`execute()` 退成"宿主直呼"。
- **判据（实测）**：`test_graphics` 354→**356**：`TheFrameValueIsEveryPassAndEveryDrawTheFrameRan`
  （5 条 pass 的帧：值里的 pass 与后端 `beginPass` 的逐条对齐、值声称的绘制数 = 后端收到的 `render`+`drawScreenProgram`；
  无内容档 = 有 entry 无 description、禁用档 = 没有 entry）、
  `TheFramesValueIsBuiltAfterThePerFrameHooks`（钩子改内容 ⇒ 值描述的是这一帧的版本）。**变异 2/2 红**
  （值不填 description ⇒ 4 条红；钩子不跑 ⇒ 4 条红）。其余套件与五个 gate 同前（gui 253 / vsg 428+3skip+2 已知红 /
  appfw 32 / meshio 34 / iobase 94；全量 Release 0 error 0 warning）。
- **三个发现**：①**`before_draw` 必须搬进编译期**（钩子写内容 ⇒ 描述要在它之后，否则值描述上一帧的画面；
  仓库里两条钩子都是"只写内容、不碰后端"，唯一变化是 `record_micros` 不再含钩子耗时）；
  ②**过渡代价：每条 pass 每帧被描述两次**（编译建值 + 运行 `execute()`）⇒ 两条 memo 见证的 reuse 数变大
  （树走的次数不变），用例已按新事实改写并注明 S3 会复原；
  ③**S3 的第一件事是回答"覆写 `execute()` 的 pass 怎么办"** —— `TheFramePlanCarriesWhatEachPassLastCost` 的
  `SlowPass` 就靠引擎调它的 `execute()` 睡觉来当耗时，帧一旦由值驱动，这个 pass 的工作会**静默消失**
  （把 `frame()` 改成由值驱动时当场红的就是它）。
- **坑**：`raw_ptr<T>` 是 `T*`，`.get()` 不存在（自己写用例时踩了两次）；`addInputTarget` 才是"按目标声明输入"
  （`addInput` 收的是 `ImageRef`）；`setPassInputs` 收的是非 const 指针表，所以值的 inputs 也用
  `raw_ptr<RenderTarget>`。

## 2026-10-11 A 落地：画面形状回到相机（重新设计的 A 片；取代 S4a，也把 S1 那道缝合掉）

- **口径（用户定）**：**一台相机 = 一张画面**。形状是 `Camera` 自己的状态（`setAspect(h/w 的反)` / `aspectRatio()`；
  `setAspect` 忽略非正数），投影、剔除、view block、拾取射线**读同一个数**；"同一台相机被两块形状不同的矩形用到"
  不再是"各自都对"，而是**画面被拉伸、引擎报出来**。行业是分裂的（VTK/Filament/Unreal 从矩形推；
  three.js/Babylon/Godot/Unity 放在相机上），本仓**文档**曾选前者、**用法**一直是后者 —— 现在统一到后者。
- **为什么**（评估过程）：M2c 换来的"相机可跨矩形复用"代价是形状成了**只有后端能答、且答案取决于之前公告了什么**的
  问题（`viewAspect()` 读开着的 scope），S2 在这条依赖解除前写不出来；而"复用"的实测用途（同相机两块矩形）在
  本仓根本不出现（分屏是每格一台相机）。
- **落地形态**：`Camera::{setAspect,aspectRatio,projectionMatrix(),screenToWorldRay(pos)}`；`Scene::collectRenderCommands*(camera)`
  （memo 仍以 view-projection 为键 ⇒ 两种形状两条 memo）；`RenderBackend::viewAspect()` **删除**（纯虚 9→8）；
  `PassDescription` 无形状字段；`FrameRecorder::setViewAspect` 删除（快照投影读相机）；`RenderPass`/`ScreenPass`
  合成**一个** `describe(scene, out)` 虚函数（S1 的那道缝没了）；`SceneView::applySurfaceGeometry` 把窗格形状
  push 给相机（与 `manip->onResize` 同一个矩形，一行）；`AxisGizmo` 声明 1:1、`FpsOverlay` 声明自己 box 的形状；
  `OrbitCameraManipulator::viewportAspect()` 读相机（无相机才回退自己的矩形 —— 否则交互与拾取射线读两个数）；
  **新增** `RenderEngine::collectPictureShapeMismatches()`：矩形（viewport 否则目标/表面）vs 画面（perspective =
  aspect，ortho = 窗口形状），全屏 pass（`ScreenPass`）不参与（它的三角形在 clip space 读不到投影），
  一条 pass 的错**每 episode 一次**（拖窗格会让句子里的数字变，所以按 pass 记 episode 而不是按句子）。
- **判据（实测）**：`test_graphics` 349→**354**（新 5 条：报告+episode+修好又坏 / 视口才是矩形且全屏 pass 不参与 /
  `SceneView` push 形状 / HUD box 形状 / 操纵器读相机；M2c 那三条**反转**：同相机两次 setAspect = 两次走树、
  `OneCameraCullsWithItsOwnShapeInEveryRectangle` 宽矩形不再变宽视锥且一个相机一次走树、
  `TheViewBlockIsProjectedForTheCamerasOwnShape`）；`test_gui` 253、`test_vsg` 428(+3 skip +2 已知红)、
  `test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 0 error / 0 warning；五个 gate 0 finding
  （`--strict` 仍 20 条历史引用）。**变异 4/4 红**（检查永不报 / 矩形不读 viewport / 不 push 相机形状 / 不 push box 形状）。
- **坑（值得记）**：①**改契约半必须重建插件** —— `viewAspect()` 一删，老 `gfx_backend_vsg.dll` 与新头文件不匹配，
  `VsgBackendPluginTest` 两条红得像"插件注册丢了"；而且**测试目标不会顺带构建插件**，得显式
  `--target gfx_backend_vsg`（已写进 redesign 的风险表）。②自己的 episode 状态别"先 rearm 再读" —— 那等于每帧都当新
  错误报（第一版就踩了：`shape_mismatches_last_frame_` 先统一 `rearm()`，carry-over 时读到的永远是 false）。
  ③文档同步点很散：`usage.md` 4 处、`graphics-render-next.md` 的 M2c 行（标已回退）、redesign 的 S4a 行/读者表/风险表。

## 2026-10-11 S1 落地：pass 描述它的绘制（重新设计的第一片）

- **做了什么**：新增导出的 `sdk/vine/graphics/FrameDescription.hpp`（`DrawDescription` + `PassDescription`；
  `ClearPolicy` **从 `RenderBackend.hpp` 移进来** —— 它是"一条 pass 描述自己"的东西，后端是它的读者之一，
  于是 `RenderBackend.hpp` 反过来 include 它）。`RenderPass` 拆出两个虚拟扩展点
  `describePass(scene, out)` / `describeDraws(scene, out)`，`execute()` 成为薄壳
  （描述 → **两半之间**问画面形状 → 描述绘制 → 按契约顺序宣告）；`ScreenPass` 不再覆写 `execute`。
  **（同日 A 之后：两半合成一个 `describe(scene, out)`，`execute()` 仍是薄壳 —— "描述 → 宣告"）**
- **判据（实测）**：`test_graphics` 346→**349**（`PassDescriptionTest` 三条：场景档 / 全屏档 / "什么都不发"，
  每条都把**描述的值**与记录型后端**收到的调用**逐项对照）；`test_gui` **253**、`test_vsg` **428**（+3 skip
  +2 已知红，逐项与落地前相同）、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 全绿；全量 Release 构建
  0 error / 0 warning；五个 gate 0 finding。**变异 1/1 红**：`describeDraws` 少填 `lights` ⇒ 只有场景档那条红。
- **两个坑（都写进设计了）**：①**顺序错了** —— `RenderBackend::viewAspect()` 的答案**取决于之前公告了什么**
  （读开着的 scope 的 target/viewport），所以"整帧描述在派发前拼好"（S2）在形状不再是后端查询之前**写不出来**：
  当时拆出 **S4a**（画面形状变编译期字段）并提到 S2 之前 —— **随后被 A 整体取代**（形状回到相机，见上一条）。
  ②**空场景的 pass 一个调用都不许发** —— 今天它在第一次公告前就返回；门禁必须放在 `describePass`（虚拟，
  因为"什么算没东西可发"每种 pass 不同），否则会静默多发 `setRenderTarget`/`setPassState`（多宣告 target、
  多一次清屏归属 = 行为变化）。有用例钉"连目标都不许发"（A 之后这条门禁在 `ScreenPass::describe` 里）。

## 2026-10-11 重新设计提案（**S1 已落地，见上一条；其余未落地**）

- 用户 2026-10-11 定"重构，重新设计"，边界含**后端契约**（`RenderBackend.hpp` + `backend/` 半 + vsg 插件）。
  文档：`.ai/design/graphics-redesign.md`（S0–S6，每片独立可逆；总判据 = 两棵树逐证据行相同）。
  **S1 已落地**；落地时把 S4 拆出 **S4a（画面形状变编译期字段）提到 S2 之前** —— 原因见上一条的坑①。
- 一句话病根（有代码证据）：**帧在引擎里已经是一个值（M7 的 `compileFrame`/`FramePlan`），但在边界上又被打散成
  ~15 个有顺序的可变调用（`RenderEngine.cpp:192,273-288` + `RenderPass.cpp:248-301`），后端再把它拼回一个值
  （`backend/FrameRecorder.hpp` 的 `FrameDescription`）。** 连带：窗口烧进帧（**8** 个纯虚里 3 个只有带窗口的后端有用）、
  顺序靠 `backend/Protocol.hpp` 的 8 条规则 + Allow/Drop/Refuse 兜、"能力/观测"各是 4 个 bool 出参。
- 本稿作废了 `graphics-render-next.md` §7 的"整块重写否决"（已在那边标作废），并保留其技术内核
  （配方/几何/开关三分不动）。**还没动一行代码**。

## 2026-10-11 T1 + T3 落地：表面尺寸扇出 / 设备夹具可用性探测（§5.1 的两条机制）

- **T1 表面尺寸扇出（M5b 的机制半边）**：表面尺寸从"宿主逐个喂视图"变成**表面的一件事**。
  - `RenderSurface::resize(w, h, ratio)` 记住尺寸并扇出给表里每条**呈现的视图**；`present` 把记着的尺寸**立刻**
    交给新呈现（新加的格子不用手喂）；`SceneView::onSurfaceResized` 在视图挂表面时**转发**给它（不挂表面时才
    是独立入口），新增 `SceneView::hasSurface()`；`SurfaceWindow::feedSurfaceSize` 改调表面，控件自己那个视图
    **不呈现**时才单独喂（`SceneView::applySurfaceSize` 是"记下来 + `applySurfaceGeometry()`"那一步，由表面
    调用 —— `RenderSurface` 是 `SceneView` 的 friend）。
  - 判据：`test_gui` **253** 绿（单视图路径一字不变）；`test_graphics` 345→**346**
    （`SceneViewViewportTest.APaneDeclaredOnASizedSurfaceIsSizedAtOnce`：present 后**零手喂**，layout 步收到
    640×720、窗口 pass viewport = {640, 0, 640, 720}）；`RenderControlTest` 里手写的 `right->onSurfaceResized(...)`
    全删（原稿说"两条"，实际**四条**分屏用例都有）。**变异 1/1 红**：`present` 不把尺寸给新呈现 ⇒ 那条新用例红。
- **T3 设备夹具的可用性探测**：`SdkPixelFixture::available()` 从"有台满足下限的物理设备"变成**共享门禁** =
  "设备 + 会话起得来 + 离屏像素给得回来"（进程内只答一次：`up()` + 一条手搭 pass 画一帧 + `read()`）。
  `BlockStorageTest` / `BlockDescriptorsTest` / `SessionTest` / `VsgBackendTest` 的**十处**设备门禁改问它；
  `getenv("DISPLAY")` 的四处假门禁删除（另外 5 处在 X11-only 的文件里、与紧随其后的 `xcb_connect` 重复，一并删）。
  - 判据：`test_vsg` **407 passed / 26 skipped → 428 passed / 3 skipped / 2 failed**。23 条误跳全部恢复运行，其中
    2 条**本就红**（`BlockDescriptorsTest.TheBindCarriesTheOffsetsInBindingOrderAndReusesOneSet` /
    `SessionTest.ADriveThatCommitsThroughTheSessionMarksWhatALostFrameWrote`，都是这些用例第一次在 Windows 上真跑
    才暴露的，**未修**，先记）；剩下 3 skip = 计数分配器 2 条 + `HostWindowReadTest` 1 条（X11-only，合理）。
  - 坑：`available()` 现在**会起一次设备**（静态缓存，整个进程只一次），所以它不再是"随手可调的便宜探测"；
    X11-only 的用例（`SessionContentTest` / `SessionMoveTest` / `GfxBackendVsgPluginTest` / `WindowCompositionTest`）
    **保留**各自的门禁 —— 它们本来就要真 X11，`DISPLAY` 对它们不是假代理，删的只是与 `xcb_connect` 重复的那几条。

## 2026-10-11 按"机制/组合"重评待办（`graphics-render-next.md` §5.1）

- **口径（用户 2026-10-11 定）**：底层提供**机制与接口**，复杂场景由业务自己组合、自己控制，交给应用层。
  已成文为不变量 6（§6）：SDK 配方只保留最常见的两三条（forward / deferred / shadow）；不造 SDK 自用的图，
  不为某个组合发明引擎政策。判据：删掉任一便利，机制路径（`engine->addPass(desc)` + 自己的 program / target）仍成立。
- **重评结果**：**M5b 拆两半** —— 机制半边 = **表面尺寸扇出**（T1，进 SDK：当时表面尺寸只喂控件自己那个视图，
  第二格靠宿主手写 `right->onSurfaceResized(...)`，`RenderControlTest` 四行；**T1 已落地 2026-10-11，见上一条**），
  组合半边 = 布局树 / 比例 / join / 格子生死（T4，**归应用层**；`SplitLayout` 那份定稿作废 —— 原稿把它当 SDK 片是错的）。
  ② 的定案同源
  （T5：多 transient 互借深度是应用组合）。其余：T2 M6（一设备 N 交换链，等触发）、T3 设备夹具可用性探测
  （删 `getenv("DISPLAY")` 假门禁，恢复 21 条被误跳的设备用例，其中 2 条本就红；**已落地 2026-10-11，见上一条**）、
  T6 便利边界（守）、
  T7 叠层对象跨重建存活（视图的实现细节，等观感需求）。
- **已落地不需回退**：预设、内建程序 / 目标工厂（`defaultGbufferTarget`、`screenCopyProgram`…）、HUD 内容对象
  （`AxisGizmo` / `FpsOverlay`）、`addOffscreenToScreen` 都属"**可选便利**"，机制路径始终成立 —— 记为 T6 守边界，
  每次动它们自查一句"删掉它，机制路径还成立吗"。


## 2026-10-11 M7：Pass 描述符 + 每帧编译的 FramePlan（唯一的大改）

- **做了什么**：帧的真相从"声明动了才校验 + 运行时逐 pass 解析"换成**每帧编译**。`PassDesc`（一个值：name / tag / stage /
  kind / camera / content / writes(`writes_image`+`writes_shape`) / output_name / output_target / inputs / input_names /
  clear· / viewport / program / program_override / polygon_mode / depth_mode / enabled / `before_draw`）由
  `RenderEngine::addPass(const PassDesc&)` **一次性**变成那种 kind 的执行结点（`Scene`→`RenderPass`、`Screen`→`ScreenPass`），
  结点即声明的唯一存储；`compileFrame()`（新文件 `src/viz/graphics/src/EngineCompile.cpp`）在录制前把这一帧定下来：
  行（pass/content/order/解析后的 inputs）、transient 目标、以及这组声明的问题；`frame()` 只执行行。
- **删掉的东西（判据①）**：`RenderPass::wiringRevision()` + `bumpWiringRevision()`（12 个 setter 里）、
  `RenderEngine::wiringDeclarationsChanged()`、`wiring_validated_`、`wiring_validation_count_`、`wiringValidationCount()`、
  `validateWiring()`（420 行）、`WiringState` 里 10 组 seen/reported 集合、每帧的名字注册表（`outputs_`/`produced_targets_`
  变成编译期的一次走表：`plan_rows_` / `plan_produced_` / `plan_published_`）。
- **判据（实测）**：`test_graphics` **344**（341 旧 + 3 新 + builder 侧 2 条见下、一条旧用例改写：`TheWiringIsCheckedWhenADeclarationMovesAndNotPerFrame`
  → `ADeclarationChangeLandsInTheFrameThatFollowsIt`）、`test_vsg` 407、`test_gui` 253、`test_appfw` 32、`test_meshio` 34、
  `test_iobase` 94 全绿；demo 两条冒烟 11 pass 表**逐列与基线相同**、0 error/warn（判据④）；五个 gate 0 finding
  （`--strict` 仍 20 条基线）。
- **判据②③④ 的用例**：`APassCanBeDeclaredAsAValueAndRunsLikeAnyOther`（场景档：stage→order、tag 进 plan、`before_draw`
  每帧跑、禁用档不跑、output_name 当帧生效）；`AScreenPassCanBeDeclaredAsAValueToo`（全屏档：program + 名字输入，
  mock 后端看到 `last_program_source` 就是编译**解析出来的**那张目标）；`ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt`
  （一个声明一张目标、同身份同形状共享、稳态 4 帧指针不变、形状变则替换、`clearPasses` 后计数归零）。
- **变异 4/4 红**：①编译不解析输入（12 条红）②transient 每帧新建（4 条断言红）③去掉 episode 比较（14 条报告用例红）
  ④描述符的 `before_draw` 不落地（2 条红）。
- **两处记账（详见 `graphics-render-next.md` §3.4）**：①"一次一集"按**句子**比（问题就是它那句话；跨检查的那个抑制用一个
  per-compile 集合，语义与旧代码逐字相同）；②**一处行为变化**：旧代码在"两个 pass 抢一张未绑定图"那一集里把图记成"已报告"
  却从未说出口，于是碰撞消失后"图没绑定"永远不会被说 —— 编译模型里 episode 是"真的报过什么"，现在会说（用例已改写并注明）。
- **builder 侧（同片续做）**：`RenderEngine::addPass` 三个重载 + `Pipeline::addPass(const PassDesc&)` 都**交回注册的结点**，
  builder 的普通 pass（shadow / forward main / transparent / gbuffer / lighting / present / preview）已按描述符声明，
  order 一律由 `stage` 决定；结点只留在真要调的地方（`setWindowPass` / `setOffscreenTarget` / `setCompositeTarget`）。
  当时未做（其中 HUD 叠层随后续做，见下条）：HUD 叠层与宿主持有的 G-buffer。
  **新用例**：`TheRecipeIsAWrittenDeclarationAndThePlanSaysSo`（4 行的 tag/order = `GBuffer`/`Lighting`/`Transparent`/`Present`
  且 order == `pipelineStageOrder(stage)`）、`ADeclaredPassClearsTheWayItsKindDoesUntilItSaysOtherwise`（描述符不写 clear 就
  跟 kind：Screen 不洗、Scene 洗且洗到 `51,51,51`；写了就压过 kind）。**变异 4/4 红**：①clear 无条件落地（3 条红）
  ②`Pipeline::addPass(desc)` 不记账（8 条红）③present 的 stage 写错（1 条红）④`tag` 不落地（2 条红）。
- **一处默认值纠正**：`PassDesc` 的 `clear_enabled`/`clear_color`/`clear_depth` 改成 `optional` —— **不写就跟随 kind 自己的
  策略**。原因是 `ScreenPass` 构造里 `setClearEnabled(false)`/`setShouldClearDepth(false)`，描述符原来的 `true` 默认值会把
  它翻掉（搬 builder 时两条既有用例立刻变红）；描述符不能有第二套默认值。
- **② 的定案（2026-10-11，同一轮）**：G-buffer 不再"等搬"，**保持宿主创建就是对的**。把 deferred 配方
  （composite 借 G-buffer 深度那条）搬成"SDK 造两张 transient 互相借用"违反"机制在底层、组合在应用层"
  （`graphics-render-next.md` §6 不变量 6）：`PassDesc` 能声明"我要一张图"，表达不了"我这辆的深度是那辆的"，
  而那条组合是配方自己的事。SDK 这侧补齐**机制缺口**：新增 `RenderEngine::transientTarget(identity)`
  —— 拿回自己声明的那张图（"此刻活着的那一张"，形状一变即作废；这正是不该把实例长留在应用手里的原因）。
  用例：`ATransientTargetIsOnePerDeclaredImageAndTheCompileOwnsIt` 追加四条断言（首帧前 null / 首帧后等于
  plan 里那张 / 未声明的身份回答 null / 形状变后回答新实例且不再是旧的），变异 1/1 红（查询不看身份 ⇒ 该用例红）。
  用法（应用自己组 transient 配方）写进 `usage.md` §3.13。
- **叠层那一续（2026-10-11，① 完成）**：`AxisGizmo` / `FpsOverlay` 不再是 `RenderPass` 子类，降为**内容对象**
  （`Object + RefCounted`：自己的 scene、取景相机、盒子 `boxViewport()`、每帧 `update()`），由配方声明成一条
  HUD 档的 pass（`content`/`camera`/`viewport`/`clear_enabled = false`/`depth_mode = Disabled`/`before_draw`）——
  `PassDesc::before_draw` 注释里点名的两个用例到这一刻才真的用上。句柄分两个：`gizmo()`/`fpsOverlay()` 给对象，
  `gizmoPass()`/`fpsOverlayPass()` 给结点（可见性读结点）。盒子是值，布局它的地方（`Pipeline::resize`）在告知
  对象尺寸后把它重述到 pass 上。**一处行为变化**：盒子初值是零面积矩形（还没人报过尺寸），后端按空矩形跳过 ——
  从前那种 pass 没有 viewport，会整屏画一遍叠层。**如实记账**：叠层对象仍由 builder 每次构建时新建，`setOptions()`
  重建会重置 FPS 读数窗口。证据：`test_graphics` **345**、其余五套件同基线、demo 冒烟 plan 表逐列不变（HUD 两行的
  `content` 由 `-` 变成它自己的 scene）、变异 3/3 红。

## 2026-10-11 M8：SDK 级设备夹具 + 像素判据（`tests/test_vsg/SdkPixelFixture.hpp` / `SdkPixelFixture.cpp` / `SdkPixelTest.cpp`）

- **做了什么**：`test_vsg` 原来只对着 vsg 内部测（`RenderEngine`/`SceneView`/`RenderPipelineBuilder` 一次没用过），
  于是"该画的 pass 在画"（结构）与"画出来的像素对"之间没人守。M8 补那一段：夹具用 **SDK 自己的入口**（引擎 + 视图 +
  管线 + 一条配方）在**真设备**上画一帧到**离屏** `RenderTarget`（64×64，RGBA8+D24，清屏**黑**、内容**白** ⇒
  "覆盖" = `PixelProbe::nonBlackPixels()`），再用 `RenderBackend::readColorBuffer` 读回来判。
- **两条用例与实测数字**：
  - `SdkPixelTest.AWireframeModeDrawsTheEdgesOfTheSameGeometry`：**延迟配方 + 非 Lit 档**（链条按前向建）⇒
    `Mode::Unlit` 一版 filled=1296（36×36 的实心块），换 `Mode::Wireframe` 一版 lines=180 —— 同一个视图、同一台相机、
    **同一条 `windowPass()` 对象**、同一张目标，唯一变的是光栅化模式。180 条边**全部**落在实体块的一个像素之内；
    实体版 140 个轮廓像素**全部**有边走过。
  - `SdkPixelTest.TheFixtureAlsoDrawsContentWithNoViewAtAll`：**不经视图**、手搭一条 pass 画同一批内容 ⇒ 覆盖
    3364/4096 ≈ 82%（正交窗 81% 的预期），四角仍是清屏色（既证"画进去了"也证"没涂满整张"）。
- **坑一：引擎是引用计数对象，`Pipeline` 强持它 ⇒ 栈上的引擎会被管线析构时 `delete`**。症状不发生在那一刻：第一版夹具把
  `RenderEngine` 做成夹具的值成员，结果每次都崩在 `~SceneView → removeWindowPass → free`，报 `0xC0000374`（堆损坏），
  而崩溃前每一步 `HeapValidate(GetProcessHeap())` 都是好的 —— 引擎被成员析构**删了第二遍**。夹具已改
  `intrusive_ptr<RenderEngine>`；`usage.md` §3.1 的样例原来写的是栈上的 `RenderEngine engine;` + `builder.build()`
  （教了一个必崩的形态），一并改成 `make_intrusive<RenderEngine>()`。详见 `graphics-pipeline-builder.md` 同日校。
- **坑二：判据要容一个像素**。填充光栅器按**像素中心**覆盖、线光栅器按**线段触到的像素**覆盖，边界上两者选不同的像素
  （实测：轮廓那一圈整整齐齐落在实体块外一步，180 条边里只有斜边那 36 个在块里）。所以"同一批几何"写成"每个边像素都
  **紧贴**实体块"（另一个形状、或整体挪两个像素的同一形状都会红），而不是"像素集合包含"。
- **门禁问能力，不问 `getenv("DISPLAY")`**：老设备用例用 `DISPLAY` 代理"有没有窗口系统"，在 Windows 上恒为"没有"。
  新夹具问"有满足后端下限的设备 + 会话起得来 + 本后端服务离屏像素"，不满足就 `SKIP` 并说出原因。**实测把老那条放开**：
  这台机器 405/26 → **426 passed / 3 skipped / 2 failed**（`BlockDescriptorsTest.TheBindCarriesTheOffsetsInBindingOrderAndReusesOneSet`
  少了 bind、`SessionTest.ADriveThatCommitsThroughTheSessionMarksWhatALostFrameWrote` 读回全黑）——两条都是这些用例
  **第一次在 Windows 上真跑**才暴露的，**未修**（待办：按平台收窄或修实现）。
- **变异 3/3 都被像素判据杀**：①删 `SceneView::applyModeToGeometryPass()` 里 Wireframe 的
  `setPolygonModeOverride(Line)` ⇒ lines=1296 == filled ⇒ 红；②`SceneView::effectivePath()` 恒等（非 Lit 档不切前向）
  ⇒ 画面变成全屏光照 pass（4096/4096、轮廓=0）⇒ 红；③后端 `StateCommands.cpp::mapPolygon` 把 `Line` 也映成 `Fill`
  ⇒ lines=1296 == filled ⇒ 红（这一条证明**后端那一段**也在判据里）。**第一版用例里那句结构断言
  （`polygonModeOverride().has_value()`）会先红、抢掉像素那一刀，已删** —— 这一条只判像素，结构判据归 M1。
- **门禁**：`test_vsg` 407 passed / 26 skipped / 0 failed（基线 405 + 两条新用例；26 skipped 是设备/窗口环境的既有
  skip）；`test_graphics` 339、`test_gui` 253（扣掉既有挂死的 `UserIOTest.PickingFilesFillsTheLineWithoutSubmitting`）、
  `test_appfw` 32、`test_meshio` 34、`test_iobase` 94 全绿；五个 gate：`check_ai_docs` 0 finding（`--strict` 仍 20 条
  基线）、`check_doc_symbols` 95 单元一致、`check_include_hygiene` 878 文件、`check_export_annotations` 57/57、
  `check_diagnostic_formats` 43 文件；两条 demo 烟雾（`--open demo` 与 `VINE_FRAME_PLAN=1`）11 pass、0 error/warn。

## 2026-09-27 帧机器的归属：`gfx_backend_vsg/core` → `vine/graphics/backend`

- 帧机器（协议/编译/键/寿命/证据，25 头 25 源 ≈3.7k+4.1k 行）从插件的 `core/` 摘出来：先成独立模块
  `src/viz/rendergraph`（`vn::rendergraph`、`vn::RenderGraph` STATIC），**最终并入 graphics**：<!-- drift-ok -->
  `vine/graphics/backend/`、`vn::graphics::backend`、同一 target `vn::Graphics`（include 路径与命名空间在第二次
  迁移中没再变）。为什么是“一层”而不是“第二个模块”/“两个 target”，以及 A/B/C 三个复用桶，写在
  **`.ai/design/graphics-layering.md`**（开发者问“依赖哪个模块”就看它）。
- **名字：`frame` → `backend`（2026-09-27）**。判据：25 个单元里只有 6 个名字带 `Frame`（24%），`frame` 只描述了少数内容，
  还让 `frame::FrameArena` 这类限定名重复 6 次；`backend` 点的是**读者**（写后端的人），与一直用的措辞
  （“只有后端需要的那一半”）一致。**否决**：`detail`/`internal`/`impl`（**假契约**——这半边是明确支持的扩展面，
  标成“内部”等于对作者说“别依赖”）、`core`（说反“哪半是核心”，且与 `base/core` 撞名）、`support`/`kit`（没有判据）。
  改动一次性做完：50 个文件 `git mv` + 243 处 include + 560 处限定名 + 104 处命名空间声明 + 插件迁移桥 1 行；
  标识符同时正名：`CORE_LAYER_*` → `BACKEND_HALF_*`、`core_layer_findings()` → `backend_half_findings()`、
  `frame_audience_findings()` → `host_audience_findings()`。

## 2026-09-27 诊断路线合并：两个类型上 A 段，引擎持有唯一一条（`ReportOnce` + `Diagnostics`）

- **判据（可复用）**：提到 A 段的理由永远是"**宿主自己具名使用它**"，不是"它看起来通用"。按这条看，
  `ReportOnce` 的证据是硬的：引擎侧本来就有 **6 组**手写的"本帧见过 / 已报过"集合对
  （`unpublishable_passes_seen_this_frame_`/`_reported_`、`unresolved_inputs_reported_`、
  `output_collisions_reported_`、`mismatched_promises_reported_`、`unusable_inputs_reported_`、
  `unproduced_inputs_seen_this_frame_`/`_reported_`）——**同一条规则的第二份实现**，而 `ReportOnce` 的注释
  第一句就是"分两处写一条规则、谁也检查不了"。
- **粘连头按 §1.5 拆开**：`Diagnostics.hpp` 原先同时装着规则与收集器，现拆成 `vine/graphics/ReportOnce.hpp`
  （零依赖）+ `vine/graphics/Diagnostics.hpp`（路线：计数 + 转发），两者都进 A 段、命名空间 `vn::graphics`。
- **路线合并**：`RenderEngine` 持有唯一的 `Diagnostics`（原来的 `diagnostic_sink_` + `Wiring::engine_diagnostic_count_`
  一起消失）；`RenderBackend` 新增 `virtual void setDiagnosticsRoute(Diagnostics&)`，引擎在 `setBackend()` 里
  把同一实例交过去，`RenderBackend::setDiagnosticSink/diagnosticSink/diagnosticCount/reportDiagnostic` 全部改成
  转发到"**当前活跃路线**"（`owned_route_` ↔ `route_`）。宿主因此读**一个**数：
  `RenderEngine::diagnosticCount()` / `diagnosticCount(cat)` / `diagnosticsClean()`；
  旧的 `backendDiagnosticCount()` + `engineDiagnosticCount()`（注释里写着"要总数就自己加"）删除。
- **没被引擎驱动的后端不变**：`route_` 默认指向自己的 `owned_route_`，所以 `test_vsg` 直接构造 `VsgBackend`
  并 `setDiagnosticSink()`/`diagnosticCount()` 的用法一字未改。
- **插件零改动**：`VsgBackend` 的 session/recorder/compiler/executor 本来就报进插件自己那条路线，其 sink 转发
  `reportDiagnostic()` ⇒ 现在落到**引擎那条活跃路线**，所以宿主的 sink 和引擎的计数都看得到，没有重复上报。
  这也是本轮的判据：**先看管道是不是已经在转，再决定要不要动代码**（预期要大改插件，实测一行都不用改）。
- **行为语义变了的一处**（测试跟着改）：`engine.setBackend(nullptr)` 后计数**不再归零**——计数属于引擎的路线，
  后端走了不会把已经说过的话收回。钉子：`DiagnosticsTest.EngineForwardsSinkToCurrentAndFutureBackend`
  新增 `later->diagnosticCount() == engine.diagnosticCount()`（"一条路线就是一个数，两端看到同一个"）。

## 2026-09-27 帧时钟查询面：`FrameProgress`（`FrameTimeline` 的宿主面）

- 按"谁在签名里命名它"逐个量了 backend 半 **24 个头**：宿主半的引用**全部为 0** ⇒ 没有一个是硬需求（R2 不红）。
  于是只剩"值不值得开面"：结论是 **1 个该做、2 个等触发器、21 个不做**。
- **该做的那 1 个**：`FrameProgress`（`sdk/vine/graphics/FrameStats.hpp`，与 `FrameCounters`/`RetentionStats` 同形）
  ＝ `submitted`/`completed` + `inFlight()`；通路 `RenderBackend::frameProgress()`（默认 **false ＝“不记账”**，  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  别拿零当"没有在飞行"）＋ `RenderEngine::frameProgress()`；vsg 从 `Session::timeline()` 的两个水位回答。
  行业对照：`VkFence`/`vkWaitForFences`、swapchain images-in-flight、`ID3D12Fence::GetCompletedValue`、
  Unity `GraphicsFence`——**同步与计时是公开面**，而编译/图/缓存/竞技场/家族细节一律私有。
- **`FrameToken` 不提**（原计划要提，看了生产者后改）：它由 `Session::beginFrame()`（插件内部）铸出，
  **宿主拿不到**；提一个拿不到的值类型 = 凭空开一个面。触发器：哪天 `frame()` 之类把 token 交回宿主。
- **等触发器**：`PhaseTable` 只读快照（触发 = 宿主开始读相位耗时）；`TargetShape` 描述（触发 = 宿主自己表达借深度/复用）。
  **不做**：`Protocol`/`FrameCompiler`/`FrameGraph`/`FrameRecorder`/`Keys`/`StateRegistry`/`VariantPool`/
  `FrameArena`/`FrameRing`/`MaterialArena`/`Streams`/`DeviceRequirements`/`SessionMove`/`SlotProbe`/
  `AllocationGate`/`DepthProbe`/`PixelProbe`/`RetirementQueue`（机制、家族词汇或测试工具）。
  另外 `Readback` 不必再开面——宿主面（`ReadbackResult` + `readColorBuffer/readDepthBuffer`）早就在 `RenderBackend.hpp`。
- **变异反证（两处，都要红）**：引擎 `frameProgress` 直接 `return false` ⇒ `test_graphics` 红；  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  vsg 后端 `return false` ⇒ `test_vsg` 红。恢复后 292 / 453 全绿。
- **坑（本轮）**：搬走/新增 `src/*.cpp` 后，**Debug 树的 CMake 源文件表是陈旧的**（glob 不含 `CONFIGURE_DEPENDS`）⇒
  `ninja` 报 "`<搬动前的路径>.cpp` missing and no known rule to make it"。修法：`cmake -S . -B build` 重新配置（14.7 s），
  然后用 `grep -c ReportOnce.cpp build/build.ninja` 确认新文件真的进了表。Release 树没报错，不代表它也对——照检一遍。
- 三条机器规则：**R1** backend 半不许 include 任何后端；**R2** 除 `gfx_backend_*`、`test_vsg`、backend 半自身外，
  任何地方不许 include `vine/graphics/backend/`（R1/R2 都在 `check_include_hygiene.py`，双向都做过变异验证）；
  **R3** backend 半里凡有 out-of-line 定义的类型/自由函数必须带 `VN_GRAPHICS_API`
  （`scripts/check_export_annotations.py`，37/37；类型与自由函数各做过一次变异，都会变红）。
- 插件侧 `vsg_global.hpp` 的 `namespace core = ::vn::graphics::backend;` 是**迁移桥**，保住 1800+ 处
  `core::X` / `vsg::core::X` 拼写；清理它是独立的一步。
- 本轮踩到的三个坑：①`graphics_global.hpp` 的**块注释**里写 `frame/*.hpp` 会触发 195 条 `-Wcomment`；
  ②注入导出宏后，用 `^class\s+(\w+)` 抓名字会把宏本身抓成名字（检查脚本的 `DECL` 要允许 `VN_GRAPHICS_API`）；
  ③模块 global 头的约定是“**每模块一个**”，嵌套子命名空间的宏由父宏组合（照 `appfw_global.hpp` 的
  `VN_APPFWGUI_NS_BEGIN = VN_APPFW_NS_BEGIN + namespace gui {`），给子命名空间单开 global 头是跑偏。
- 验证：全量 `ninja -C build` 0 error/0 warning；`test_vsg` 452、`test_graphics` 291 全绿；
  三个卫生脚本干净；gate 见 `.ai/design/vsg-reimplementation.md` §11 的实施记录。

### 2026-09-27 续：放置判据（用户纠正）与 `frame/` 目录三分

- **放置判据只有一条：是否跨 DLL（是否导出）**。导出的 ⇒ 必须在 `sdk/`（`install(DIRECTORY sdk/)`
  就是"导出面 = 安装面"）；只在本 DLL 内用的 ⇒ 私有头与 cpp 同目录放 `src/`（appfw 的
  `src/fw/appfw/src/gui/{SurfaceWindow,RenderControl}.hpp`；`src/` 不在任何 include 根上 ⇒ 外部**物理上**
  include 不到）。**"给谁用"是规则判据（R2），不是放置判据**——我曾用"frame 不是宿主的 API"把它挪到
  `include/`，那是错的：`include/` 是**插件的**目录（插件整体不安装头），库不用它
  （`base/core` 只有 `sdk/` + `src/`）。
- 已按此回退：`frame/**` 回 `sdk/vine/graphics/backend/**`（include 拼写不变 ⇒ 0 处源码改动）。
- `frame/` 目录三分（只分组，**命名空间仍是 `vn::graphics::backend`** ⇒ 调用点零改动）：
  `device/`（DeviceRequirements SessionMove SlotProbe Readback DepthProbe）、
  `storage/`（FrameArena FrameRing MaterialArena Streams）、
  `evidence/`（Observe PhaseTable PixelProbe AllocationGate Diagnostics）；根留 11 个
  （"一帧怎么走" + 后端身份与寿命）。
- **四个"粘连"单元要拆**：`ClearPlan`/`TargetPlan`（策略 vs 应用）、`FrameTimeline`（查询 vs 账本）、
  `Diagnostics`（`ReportOnce` 工具 vs 收集器）。清单、判据与顺序写在
  `.ai/design/graphics-layering.md` §1.5（并已把 §0 的"三区"改成"两层放置 + 三条规则"）。
- 坑：**检查脚本对"搬走的路径"会报 `0/0` 假绿**（`check_export_annotations.py` 已改成"半区不存在 = finding"，
  并把 `glob` 换成 `rglob` 以支持子目录）。变异：把 src 路径写错 ⇒ 立刻 1 条 finding。

#### 同日：A 段提级（帧计数进宿主 API）

- `FrameCounters` / `RetentionStats` → **`vine/graphics/FrameStats.hpp`**（模块公共、随 SDK 安装）；
  通路 = `RenderBackend::frameCounters()/retentionStats()`（**默认返回 false ＝“不保留计数”**，与“空帧”区分）  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  ＋ `RenderEngine::frameCounters()/retentionStats()`；vsg 后端从 `frame/evidence/Observe` 回答**同一批数字**
  （不再是第二处拼写）。
- 钉子：`RenderEngineTest.FrameCountersForwardTheBackendsAnswerAndSaySoWhenThereIsNone`（无后端 / 不保留 /  <!-- drift-ok: S5 把这三个出参收成 frameReport() / 用例改名 -->
  转发各一条），`VsgBackendTest.TheHostReadsTheBackendsOwnFrameCounters`（真设备：空帧全零 → 画一帧
  `passes=1 draws=1`；**变异**：把 `counters = d->observe.counters();` 改成 `{}` ⇒ 红 ⇒ 证明测的是**值**）。
- **A 段未做**：`ReportOnce`（接进引擎要“每站点一个 episode”，且会改诊断流量 ⇒ 独立一轮）；
  `FrameToken` / `TargetShape` / 计划数据（与 S3/S4/S5 一起）。**B/C 段（Keys/设备/存储）不可提级**——
  它们是家族形状与设备机制，见 `.ai/design/graphics-layering.md` §4。

#### 同日：`frame/` 最终形态 = **扁平一个目录**（判据记在 layering §1.5）

- 结论：**25 个单元平铺在 `frame/` 下**，不分组、也不提为 `frame/` 的同级。四条判据逐条否决拆分：
  ①放置看"是否导出"（都导出 ⇒ 都必须在 `sdk/vine/graphics/**`，在哪个目录是组织问题）；
  ②**同级不改变权限**（要让宿主/引擎用只能提到 `graphics/*.hpp`）；
  ③**同级不降低深度**（`frame/X.hpp` 与 `device/X.hpp` 同深度——"层级太深"用同级解决不了）；
  ④**同级不区分读者**（frame/device/storage/evidence 的读者都是后端）。
- 唯一站得住的反对理由只有"目录名说实话"（`frame/DeviceRequirements.hpp` 会误导），代价是多一个顶层名字 ⇒ 不值得。
- 弯路记录：我先做成 `frame/{device,storage,evidence}/`（子目录），又抬成同级——**两次都不是问题本身**；
  问题只是"要不要拆"，答案是不要。分类仍然成立（设备/存储/证据/一帧怎么走），但只写在文档里、不落成目录。


> 2026-09-26 **FPS 读数改成窗口真平均（设计 graphics-overlay.md 新节）**
> · 旧形：每帧瞬时 1/dt 的 EMA(0.2) + 0.15 s 刷新 ⇒ 报“样本的平滑值”：时间常数 1/(0.2·fps)（60fps≈83ms、
>   200fps≈25ms，依赖被显示的数）；真实 60→30 阶跃印 **40/33**（没发生过的值）；拖动流 10 s 改值 75 次、单步
>   最大 17 fps。
> · 新形：**窗口内数帧/窗口墙钟**、关窗即发布、**零混合**；同阶跃印 60,60,60,60,60,**54**,30,30（54=真的前60后30
>   那个半秒的真均值）；50 ms 卡顿如实 56.25。旋钮= `Sampler::window_seconds`（默认 0.5 s、2 Hz、整数死区）。
>   实测取舍：0.5 s ±3.0 fps / 响应 0.5 s；≥2 s 摊平真实短事件；<0.25 s 噪声 ≥±5。
> · 行业（PresentMon 一手）：主量=帧时间 ms，FPS 派生；capture=逐帧 CSV+摘要（avg/min/max + 90/95/99th 分位）；
>   overlay 画图。⇒ 一个数字做不到既可读又不撒谎，我们选可读那半并保证不撒谎。
> · 门禁：`FpsOverlayTest` 5 例 + 变异 2/2 红（每帧瞬时 / 跨窗混合）；test_graphics 287→**291**；两棵树门禁
>   cases=451、应用判图逐字不变。

> 2026-09-26 **ASan 余量收口（设计 §11.16dg）——单一共享运行时 + 判词分层**
> · `asan_check.sh` 旗标改 `-shared-libasan`（+`-Wl,-rpath,$(clang -print-runtime-dir)`，clang 不加 rpath）⇒
>   插件侧分配与主程序同账本，报告可读；整树重建一次，默认电池与 test_vsg 复验绿。
> · 读明白后的结论：严格 test_vsg 剩 **零 Direct、全 Indirect** ⇒ 直接根全被豁免，其余是同一保留的子树
>   （分配帧= glslang `spv::Builder` 构造里的扩展类型指令，~12 条/672 B 每 builder）。判词分层：Direct=永远红；
>   Indirect 条目 PASS 并把计数+豁免表打印（不隐藏）；新增 `leak:spv::Builder::` 带理由豁免（解出符号时命中）。
> · 实测：严格 test_vsg exit 0（间接 106 块/5,936 B/28 条）；`test_gui '*' + 泄漏` 仍红=其自身 13 个 Direct 根
>   （GuiTest::SetUp/buildDock），非本线。边界：插件帧符号化只部分（豁免根据=LSan 的结构性 INDIRECT 标记）。

> 2026-09-25 **ASan 全量 test_vsg 首次跑通（设计 §11.16df）——一雷一 UAF 一卫生**
> · volk 静态进插件**所有树都没有 -fPIC**（Debug/Release 靠历史对象在绿；ASan 树 ld.bfd 直接拒链）⇒
>   `POSITION_INDEPENDENT_CODE ON`，三棵树重链验证；ASan 树重生成要 `env -u http_proxy`（FetchContent 更新步）。
> · **真 UAF**：`ContentStore::tablesFor` 查表前读 `geometry->revision()`——停靠契约（"窗口内旧计划仍应答"）与同文件
>   三处的"先查后读、持 share 才准读"违背 ⇒ 成员检查移进 `liveGeometry`；修前 ASan 红、修后 0 ASan 错。
> · 泄漏主体 = 22 个设备用例不 `xcb_disconnect`（685,560B/395 处）⇒ `TestXConnection` RAII + 22 处插入 ⇒ **6,048B/108 处**；
>   分配门禁 mallinfo2 用例 ASan 下 GTEST_SKIP（量具对 ASan 分配器不可见）。余 6KB 栈穿插件、符号错乱（插件与主程序
>   各带一份静态 ASan 运行时）⇒ 收口（-shared-libasan / 逐条豁免）**登记待决**。
> · 提交 `28f1332` + `6dc23d1`；两棵树 451/451、门禁 cases=451 vuid=0、应用行逐字不变。

> 2026-09-25 **B9 在飞槽数地板 + 学到更深即重布局（收口）**
> · ① 构造地板：`BlockStorage::create` 以 `layoutForInFlight(layout, kAssumedInFlightSlots)` **上举**五个 per-frame
>   形态（只举不缩）；`hasSlabsForInFlight` 是判断面。旧字面量 `slots{3}` 从此造不出存储——**上举而非拒绝**
>   （create 的 null 是设备语言，太浅的存储没有正确的服务方式）。
> · ② `VsgBackend::growBlockStorageIfNeeded` 每帧读 `Session::slots()` 并把“槽数太浅”与“预算增长”**合并**一次
>   `create`+`adoptBlockStorage`——分开做会丢另半请求（替换身 `growthNeeded` 从零起）。Info 三个文案变体；
>   预算变体前缀照旧含 "grew"。测试接缝 `BackendContentAccess::assumeInFlightSlots`（本机框架报的恰=假设 3）。
> · 门禁四条：无设备策略、设备建造地板、接缝触发重布局（4 ⇒ 五形态 5、恰一条 Info、次帧不再换）、原增长用例。
>   变异 4/4 红：地板移除 / 上举失效（策略+地板+接缝；批量跑的插件黑帧单跑绿=环境模式）/ 接线断 / 谓词过宽（+套件崩）。
>   test_vsg 448→**451**、门禁 cases=451 vuid=0 hazard=0、应用阶段逐字不变。
> · 诚实边界：本机学到的在飞数恰=假设 3；“真 N≥4”经接缝可走同一条分支，但本机未自然观测到（真机首现时复核文案与内存）。

> 2026-09-25 **B8 跨帧差异相位（部分）：可读回子类守住，F−N 洞记实**
> · 新相位 `VsgBackendTest.TheLastFrameOfAMovingSequenceKeepsItsOwnViewBlockValue`（test_vsg 447→**448**；门禁 cases=448/vuid=0、
>   应用阶段逐字不变）：每帧动相机（0.95..0.75）、**末帧 0.1**；片元 `step(0.75,|cam_pos.x|)` 把红通道变成 0/1 两类
>   （linear/sRGB 同字节，判据与色彩空间无关）⇒ 末帧只要读到别帧数据（绑定冻结/过期、错 slab）就整片翻 255。
>   变异（`ContentPass::recordCommand` 读侧绑定落后一 slab）**2/2 红**；写侧"落后一 slab"变异自洽、抓不到（读写共用同一 `view.offset`）。
> · **为什么抓不到 §11.16dd 本体**：受害帧 = 最旧在飞帧（F−N），readback 只见最新帧；探针实测（`writeView` 帧号/slab/偏移/时间戳、
>   `Session` 学到槽数 = 3）本机框架逐帧节流 ≈ 1 GPU 帧（submit/回拷阻塞），F−N 总在覆盖写之前收尾；负载压到 4096²×64 遍
>   （≈1G 像素/帧）仍无重叠。⇒ 该洞继续由结构性旋转用例（`BlockStorageTest.TheSlabsRotate…`）承担；登记 B8 行已改写。
> · 方法记：临时探针跑完即撤；"读是活的"用两次判别实验钉的（全 0.9 ⇒ 255；0.9×5+0.1 ⇒ 0 ⇒ 读自己帧的 slab）。

> 2026-09-25 **M11ah：节点四种变更行为钉住（换父 / 换材质 / 换着色器 / 移除；设计 §10.2）**
> · 新套件 `SceneMutationTest.*`（4 例，无设备，收集层）：①换父 = 单父结构 + 命令世界矩阵跟随 + **新旧两链都重算**
>   （`boundsRecomputeCount()` 证人）+ 旧父 box 缩回；②换父跨越 `StateNode` ⇒ 材质/程序/不透明度按新链整体切换
>   （移回即还原）；③同一 `Geometry` 换材质/换程序 ⇒ **下一次收集**带新身份，清空回继承；④`removeChild` ⇒
>   命令消失、旧链重算、重加回仍可画（移除 ≠ 销毁）。
> · **修正过的判断**：漏公告后果**不对称**——漏入库侧（新父 `addChild`）才是"物体消失"（陈旧空盒剔掉内容）；
>   漏出库侧（旧父 `removeChild`）只是陈旧**超集**盒（多走空子树，性能级，画面仍对）⇒ 抓出库侧必须用重算计数
>   证人；`scene.boundingBox()` 是新鲜走线，照不出漏公告（第一版用例被这点骗过：M1「removeChild 不失效」跑绿）。
>   变异 3/3 红（两侧静默 / 换父不摘旧父），恢复绿。
> · `test_graphics` 283→**287** 两树；ctest 23/23 两树；门禁两树全阶段干净（cases=446、app 行逐字同）；无源改动。

> 2026-09-25 **M11ag：逐 draw compare op 进动态层（收掉 M3d-2 的登记待办）**
> · 背景（旧登记）：`core::DynamicState` 没有 compare 字段，M2c 的 `ContentPipeline` 把 `GREATER` 烘死
>   （§11.11 的 reverse-Z 约定），而旧实现按 `DepthState.compare` 逐 draw 映射 ⇒ **内容自定比较算子**的
>   画面新旧不同。落地：`DynamicState` 加 `compare`（默认 `Less`，距离语义）；`resolveDynamicState` 直接
>   拷贝 `state.depth.compare`（pass 早退分支也拷 —— 比较与深度策略正交）；`makeDynamicStateCommand` 以
>   `RenderStateMapper::mapCompareOp` 做边界反转（`Less→GREATER` / `LessEqual→GREATER_OR_EQUAL` / `Greater→LESS`…）。
>   默认 `Less→GREATER` 与烘死时逐字同值 ⇒ **无 StateNode 的场景管线与画面不变**（门禁 app 判据逐字复现）。
>   `Keys.hpp` 审计表 `DynamicState` 行补 `+ compare`；文件头 "deliberately NOT resolved" 段改写为距离语义。
> · **另修一处静默**：`DynamicState::operator==` 补上 compare ⇒ 比较算子变了现在会被 variant 的"动态变了"
>   看见（此前改了它不会重发 set 命令，画面停旧值）。
> · 用例：`ContentDrawTest.TheDynamicMappingFollowsTheEngineConventions`（默认与自定义两向）+ 
>   `FrameCompilerTest.TheAuthoredCompareOpTravelsIntoTheDynamicState`；变异 3/3 红（烘死回归 / 解析不拷 / 翻转表破）。
>   `test_vsg` 两棵树全绿；门禁 cases **445→446**、vuid=0、app 画面逐字同。

> 2026-09-25 **M11ae/M11af：首帧叠层收口 + 应用启动尺寸定下来（设计 §11.16db/dc）**
> · **首帧叠层**（旧登记"一帧没有工具叠层"）：探针（`AxisGizmo::onSurfaceResized/execute` 前 8 次，跑完即撤）把
>   旧读法"表面尺寸未知"改写了——预热帧（160×160）叠层**有效**（vp 96×96）；第一个**上屏**帧画在 Qt 布局**瞬态**
>   尺寸 **100×30** 上（`Startup frame going away` 之后、`-> Presenting` 之前），角落盒放不下 ⇒ 旧算式
>   `dev_h-2*margin` = **-2** 给出**负视口**（-2×-2），后端空矩形守卫跳过并记一条 Info（日志里那条
>   "its rectangle is empty" 就是它）。修：`side = fitted>0 ? min(size_px_,fitted) : 0`——零面积=说出来的"画不下"。
>   `FpsOverlay` 同类边界**保持原样**（放不下留上一矩形、被夹进目标；两拼法未统一，此句即登记）。
>   钉子 `AxisGizmoTest.ASurfaceTooSmallForTheBoxNeverYieldsANegativeViewport`；变异 1/1 红（放回旧算式）。
>   `test_graphics` 两棵树 **283**。
> · **启动尺寸**（旧登记"窗口随日志长大 = 产品决定"）：`MainWindow` 在 `setMinimumSize(800,600)` 旁加
>   `resize(800×600)`（= 最小值 = 文档默认 = 门禁判图尺寸），首 show 不再走"按布局 sizeHint 定尺寸"。
>   **诚实记录：今天本机没能复现漂移**——修前后各一批 + 两次强制宽控制台对照都读 **378×247**（顶层链：
>   渲染区 378×247 ← 容器 800×600 ← 顶层 864×664）；§11.16cs 那组"六次启动六个渲染区（最大 3418×1110）"
>   仍是漂移存在的记录，改动按**构造**去机制。钉子 `MainWindowTest.TheOpeningSizeIsStatedInsteadOfInheritedFromTheLayout`
>   （构造后 min==size==800×600 且 `WA_Resized` 已置；hint 跟随在本机两平台都不可复现，所以只钉构造决定）；
>   变异 1/1 红（删 resize 行 ⇒ 回 Qt 默认 640×480）。`test_gui` 两棵树 **208**。
> · **顺带的既有红**：`build-release` 跑 `test_gui` 全量露出 `PluginLifecycleTest.HandwrittenRegistrationCanDisableForAllUsers`
>   一直红（断言写死 Debug 后缀 `test_plugind`，`808bbd2` 起；Release 库叫 `test_plugin.so`）⇒ 改成"注册路径必须点名
>   沙箱拷贝的文件名"（期望值从拷贝自身推出）。教训：**测试里写死构建后缀 = 另一棵树必红**。

> 2026-09-25 **M11x：B5 的触发规模做成配方（设计 §11.16cu）**
> · `VsgBackendTest.TheDocumentedScaleGrowsTheDrawBudgetOnceAndThenServesEveryCommand`：一次 `render()` 带
>   **2000 条命令**（文档点名的规模），默认预算。帧 1 拒 **976 个 draw 块**（首条 + 汇总两条诊断）；同 call 的
>   light/shadow 各 1 块装得下 ⇒ 撞顶只在 draw region。帧 2 换存储（draws 1024→2048）后全部装下、帧 3 稳态，
>   全书 3 条诊断、**一次**增长事件。record ≈2–5 ms、commit ≈84–179 ms（打印）。
> · 口径教训：一次 `render()` = **一个 drawing call**（灯/影每 call 一块），命令只是它的 draw 块 —— 我按"每条命令一个 call"
>   预测成三帧三次增长，实测全相反。套件 **440**、两棵树门禁 vuid=0；本片只加配方与记录（无源改动）。

> 2026-09-25 **M11w：块预算按需增长（收掉 B5 后半；设计 §11.16ct）**
> · 某帧写超预算仍**按帧**拒写（有报告 + 计数），但 storage 记住"最坏一帧试了多少"（`growthNeeded()`，max over
>   frames、不重置）；下一次 `beginFrame()` 用**新 buffer**（预算 `max(need, 2×budget)`）整体替换，旧 storage 与
>   **被换下的描述符集**都进退役队列停放，并以一条 Info/`ContentSkipped` 报"拒绝为什么停了"。
> · **踩到并修掉的真缺陷（03047）**：`BlockDescriptors::repoint` 换 set 时若不停放旧的，池会把已被释放 set 的
>   `VkDescriptorSet` 句柄发回，新 set 在下一帧编译时 `vkUpdateDescriptorSets` 一个挂起命令缓冲仍在用的句柄 ——
>   用例全绿、验证层 1 条（M4 变异专门钉它）。命令缓冲对 vsg 对象的引用**保不住句柄**。
> · 判据：`test_vsg` 437→**439**（两个 `BlockStorageTest` 用例 + `VsgBackendTest.AFrameThatRanOutOfBudgetGrows
>   TheStorageBeforeTheNextFrame`，端到端两层像素）；变异 **4/4 红**；两棵树门禁 `cases=439 vuid=0 hazard=0`、
>   应用行与历史逐字相同。测试接缝：`BackendContentAccess::{storage, useBlockStorage}`。

> 2026-09-24 **M11i：租约的顺序有了设备相位（设计 §11.16ce，收掉 M10c/M10d/M10e/M10f 反复登记的一条）**
> · 新相位 `runLeasedTargetOrderPhase`（`DevicePhaseTest` 第七行）：出借方 8×4 + 借方（自己的颜色、出借方的深度），
>   计划把**借方那趟放第一位**（走计划顺序 = 走错顺序），事实里两个都要求长到 16×12 ⇒ 两趟 `ResizeInPlace`；
>   判据：`resized == 2`、`refused+failed == 0`、两边 extent 16×12、`queue.pending() == 2`、两个目标在新 extent 上
>   各自的清屏色、诊断干净。第一帧是 bootstrap（`Repair` 那一臂压过 resize，所以先按原尺寸渲染一帧）。
> · **写它当场抓到的**：执行者的排序**由事实驱动** —— 手工拼 `TargetFacts` 时漏了租约（`depth.borrowed/source`）
>   ⇒ 它按计划顺序走、借方被拒（`resized==1`）。填法照 `OffscreenTarget::depth()`：`has_depth/borrowed/source/
>   promotion/any_pass_preserves_depth`。任何自己拼 facts 的调用方都要照这条。
> · `counters.parked` / `counters.plan_applied` 是**相位体自己加的**（不是读队列）⇒ 新相位要加；表尾合计跟着更新
>   （frames 12→14、targets_built 8→10、resizes 2→4、plan_applied 2→4、parked 4→6）。
> · 证据：两棵树门禁 `cases=431 failed=0 vuid=0 hazard=0 skipped=0`、**相位 12 行 / 2 次运行**（新行
>   `[selftest] leased targets: … lender first …`）、应用 `vuid=0 warnings=0`。诚实记录：应用画面参数这次 Debug 跑
>   是 `86.53%/242`、Release 是 `87.04%/244`（历史各跑都是后者）⇒ 那几个数字跑与跑之间有 ~0.5% 抖动（FPS 数字 /
>   抓帧时机），门禁判的是阈值。
> · 变异 1/1：`indexOf(lenderOf(...))` 改成"没有出借方"（按计划顺序走）⇒ 相位红（`resized==1`、借方停 8×4、
>   行文本 FAILED）；恢复后 7 行全绿。

> 2026-09-25 **M11o：拒绝报告"先说一遍，再说一共几条"（B5 前半；设计 §11.16ck）**
> · 老问题：拒绝路径**逐命令**上报（~35 个 `reportRefused` 调用点），场景里有坏内容时刷屏；登记的形状
>   = "每 pass 每原因一次 + 后续只计数"。
> · 改法：`ContentPass` 一张**本趟账本**（`RefusalRow{what,why,count}`）——首次照旧完整报（点名那条命令），
>   同 `what` 后续只计数；pass 结束时（`record` 里 **RAII 守卫**，任何 return 都跑）每个 count≥2 的
>   原因报一行 "N drawing call(s) were not drawn for one reason - …"；诊断计数跟着消息走。
> · 证据：新真设备用例 `ManyRefusalsForOneReasonAreOneLinePlusACount`（3 条同因被拒 ⇒ **恰好 2 条**消息 +
>   计数==2）。变异 1/1 红（`noteRefusal` 恒 true ⇒ 4 条消息）。两棵树门禁
>   `cases=438 failed=0 vuid=0 hazard=0 skipped=0`。
> · **B5 后半（块预算 1024/256）不改**：`BlockStorage.hpp` 的文件注记写明这是**有意**的（"growing the buffer
>   would move bytes a submitted command buffer still names"）；登记里"按需增长"的方向与实现冲突，若要真的
>   增长要走"新 buffer + retirement"（同 `MaterialArena` 轮转），是另一片的量级。

> 2026-09-25 **M11n：高光强度真的接上了（收掉 D2；设计 §11.16cj）**
> · 登记：`Material::specular()` 文档写"A 是强度"，但**没有着色器读它**（前向与 G-buffer 都只读 `.rgb`）。
>   选**接线**（改文档会留一个哑字段）：前向 `... * material.specular.rgb * clamp(material.specular.a,0,1) * ndl`；
>   G-buffer `out_specular = vec4(rgb * a, 1.0)`（把强度折进彩色 ⇒ 光照侧一行不改，两条路按构造同乘积）。
> · 文档同步：`Material.hpp` 的 `@brief`、`usage.md` 的规范 G-buffer 清单。
> · 证据：新真设备用例 `ContentPassTest.ASpecularIntensityScalesWhatTheSurfaceReflects` —— 材质选成
>   "答案只差一次乘法"（albedo 全黑、高光白、太阳在镜头后方 ⇒ `dot(n,h)=1`、`pow=1`），四趟里三档读数
>   （前向 α=0 ⇒ 0、前向 α=0.25 ⇒ 0.125、G-buffer→光照 α=0.25 ⇒ 0.125）。变异 2/2 红（两条路各去掉强度 ⇒
>   读回 0.5）。两棵树门禁 `cases=437 failed=0 vuid=0 hazard=0 skipped=0`、`vine_shader_check.sh` PASS。
> · **顺手量到**：演示证据行**逐字不变**（87.04%/85.14%, preview 244）——demo 的材质全用 `alpha=1.0`
>   ⇒ 这缺陷能活到现在的原因就是"没人用过分数 alpha"。

> 2026-09-25 **M11m：一句话只说一次——半分片的插话归半片所有（设计 §11.16ci）**
> · 登记过两次的老问题：`ContentPass` **每帧新建** ⇒ 它的 `ReportOnce` 插话每帧重置（`shadow_map` 那条
>   警告每帧重复）。`Scope` 的注释早就写着契约"episode 的结束是**调用方**的选择"，缺的是有人真给一个
>   活得更久的 scope。
> · 三处插话各归各处：半片的两种句子（content half 不能服务 / `reportShadowNotSampled`）归 **half**
>   （`ContentHalves::Data::Half` 的两个 `ReportOnce`，`halvesFor` 填进 `Scope::Entry`）；光掉落归
>   **每 pass**（`ContentAssembly::Data::lights_dropped`，按 `PassId` 一行，id 不回收 ⇒ 随 pass 数长、
>   不随帧长）；空矩形归**会话**。
> · 契约写进类型且**向后兼容**：`Entry` 尾部追加 `reported/shadow_reported`，`Scope` 追加
>   `lights_dropped_episode/empty_rectangle_episode`——**空 = 旧行为**（一帧一插话），手工拼 Entry 的
>   老用例一行不改。
> · 证据：新**无设备**用例 `ContentHalvesTest.TheHalvesEpisodeStateIsTheHalvesOwn`（跨帧同一个对象、
>   第一帧说过第二帧静默、另一个 pass 也不再说、`rearm()` 后再说、两种句子互不干扰）+ 真设备用例扩展
>   （第二帧静默、重臂后再报）。变异 2/2 红（记录器忽略调用方状态 / halves 不再交出状态）。
> · 门禁两棵树 `cases=436 failed=0 vuid=0 hazard=0 skipped=0`、应用 `vuid=0 warnings=0`。
> · 未做（登记）：`lights_dropped_episode` / `empty_rectangle_episode` 的管线只有代码审查，没有自己的
>   用例（触发要真设备 + 一次"灯装不下"/"矩形为空"的计划）。

> 2026-09-25 **M11l：门禁的应用阶段有了第三条画面判据——"整片背景灰"（设计 §11.16ch）**
> · 起因 = M11k 里那条 M3 登记：D4 契约的另一半（G-buffer 写 `w = 1`）没有用例看着 —— 把 `w` 改成 0，
>   演示窗口**看得见**（14 440 px 平色 `(69,69,69)`、平均色 (101,112,121)→(84,91,99)），但门禁两条判据
>   （非黑占比 ≥30%、预览条 max ≥64）**都通过**。
> · 量出来的分离度：健康画面里平背景色 **2 px**（±4）/ **1 px**（线性拼法）；变异画面 **13 565 px = 14.53%**。
> · 改法：`scripts/ppmprobe.py` 多一个**可选**颜色参数（+容差，默认 ±4），给了颜色就多打一行
>   `share r,g,b +-t: N pixel(s) (P%)`（原输出不变）；应用阶段多一条判据 = **预览条以下"视口带"**里平背景色的
>   占比（两种编码取大者）≤ `VINE_GATE_APP_MAX_BACKGROUND`（默认 **5%**），两个样本都判、写进证据行。
> · 证据：健康时两棵树 `background 0%`；变异（G-buffer `w = 0`）⇒ 应用阶段红
>   （`before … background 25.64% … the picture is one flat surface`）。
> · **限度（诚实记录）**：resize **之后**那个样本不敏感（698×132 里四个预览槽盖住视口大部分 ⇒ 同一次失败只读
>   3.97%）⇒ 敏感的是 resize **前**那个样本；整窗版 15.47% vs 视口带版 25.64% ⇒ 判据选视口带。
> · 顺带登记：Debug 门禁**首跑**有一次 `BlockStorageTest.TheRegionsAreLaidOutOnceAndDoNotOverlap` 红、重跑绿
>   （门禁把首跑失败具名写进证据行）；此前不在已知偶发名单 ⇒ 第二次出现就按 §11.16cf 的办法先分环境/真缺陷。

> 2026-09-25 **M11k：延迟光照的"背景"改看写掩码（收掉 D4；设计 §11.16cg）**
> · 旧判据 `dot(pos,pos) < 1e-6`（`builtin_deferred_lighting.frag`）在**相机贴住几何**时把视图位置 ≈(0,0,0)
>   的**写过的**像素当成背景 ⇒ 固定的 0.06 色洞。新判据 = G-buffer 位置附件的 **w**：写过的像素
>   `w = 1`（`builtin_gbuffer.frag` 本来就这么写），没写过的保持**透明黑**（后端 `core::planClearValues`
>   Rule 4：只有附件 0 收到 pass 颜色，其余彩色附件保持构造值）⇒ `if (pos4.a < 0.5) 背景;`。
> · **契约的两半**写进两个 shader 的注释 + `usage.md` §3.4 手搭延迟管线清单（位置的 w 不是备用通道）。
> · 证据：新真设备用例 `ContentPassTest.TheLightingBackgroundIsDecidedByWhetherTheGbufferWasWritten` ——
>   **手写** G-buffer writer 覆盖左半、位置**精确**写成 `vec4(0,0,0,1)`（光栅化几何不可能可靠落在
>   `|pos|² < 1e-6` 上，所以必须手写值），右半保留清除；两探针：写过的贴脸像素 = `albedo × ambient` =
>   (0.5,0.125,0.125)，没写过的 = 0.06 平背景。变异 2/2 红（判据改回距离 ⇒ 红，消息自带 "(15,15,15) 说明
>   判据又在读距离"；writer 写 w=0 ⇒ 红 ⇒ 用例读的是写掩码）。
> · **M3 诚实记录**：把**引擎 G-buffer** 的 `w` 改成 0 —— 演示窗口**看得见**（天空渐变消失、14 440 px 平色
>   `(69,69,69)` = sRGB 编码的 0.06、平均色 (101,112,121)→(84,91,99)），但**门禁判据看不见**（content 两次
>   都 87.04%，阈值 30%）⇒ 登记"应用阶段画面判据对'整片天空变平背景'不敏感"，方向 = 加平色/颜色种类判据。
> · 门禁两棵树 `cases=435 failed=0 vuid=0 hazard=0 skipped=0`、相位 12 行 2 次、应用与修前**逐字相同**
>   （87.04%/85.14%, preview 244）—— 演示里没有一个可见像素的视图位置接近 0，这就是缺陷活到现在的原因。
>   `scripts/vine_shader_check.sh` PASS（9 shader 编译 + 内嵌副本逐字节同步）。

> 2026-09-24 **M11j：读窗口的偶发红 = "窗口还没被服务端报可用就呈递"（测试宿主加了同步点；设计 §11.16cf）**
> · 先把黑拆成两种事实：`TestHostWindow::readError()` 记下每次读自己的 X 错误 ⇒ `read-error 8`（`BadMatch`，
>   服务端**拒绝**了读）与 `read-error 0`（读被服务、窗口里就是没画面）。改前两者都是 `{0,0,0}`，
>   所以被当成"画面错误"读了好几轮。设备无关的新用例组 `HostWindowReadTest`（纯 X，不碰设备）钉住两半 +
>   有界等待的两种行为。
> · 20 次探针的时间线：`@501ms read-error=8`（窗口还没 VIEWABLE）→ 4 次 `read-error=0` 仍黑（≈1.3 s）→
>   **再呈递一次自己的图**后立刻 `(0,137,0)`；另一跑 5 s 内始终没到 ⇒ ①**窗口被报 VIEWABLE 之前的呈递永远
>   到不了**（等多久都没用）②同一张画面再呈递一次能落地。
> · **plan-free 帧不是 settle**：会话空帧呈递的是**会话自己的图**，会把窗口重画成它那张（清屏色）⇒
>   "驱动两帧空计划再读"是在**盖掉**画面。三处 settle 改成再呈递同一张画面（`VsgBackendTest` 的
>   `settle()` 直接再 `drive()`；两个会话套件在循环里重新 `assignFrameGraphs`——分配只供**下一次**
>   `commitFrame` 用，必须进循环）。
> · 根因（设备无关）：`TestHostWindow` 构造先等"服务端报 VIEWABLE"（原 2 s 期限），**只跑 X 的
>   `HostWindowReadTest` 20 次红 2 次**，红的就是 `wasViewableAtCreation()` ⇒ **服务端有时 >2 s 才把刚 map
>   的窗口报成 viewable**；用例在那之前呈递的帧全作废。抖动**成片**出现（另一时段 30 次一次不迟）。
>   ⇒ 期限放宽到 10 s（迟到会打 `[host-window] … after N ms`），花销只在服务端迟到的那几跑。
> · 证据：两棵树门禁 `cases=434 failed=0 vuid=0 hazard=0 skipped=0`、hygiene `0/793`、相位 12 行 2 次、应用
>   `vuid=0 warnings=0`（`87.04%/85.14%, preview 244`）。速率：单跑 4/12 红 → 0/12、三套件组 2/3 红 → 3/3 绿、
>   全量 4 跑（3 跑 3~7 条红）→ 4 跑全绿 434。
> · 变异：M3（去掉构造的等待）⇒ `wasViewableAtCreation` 用例确定性红，恢复即绿；M4 **没证成**（`kViewDeadline=0`
>   重跑 12 次不红——那时服务端已不迟到）⇒ 速率对比是**跨时段**量的，机制由探针直接测到。
> · 未做（登记）：产品侧"画面已落地"的事实（会话 presented/displayed 计数或 present 完成事件）；构造的等待只
>   保证"呈递发生在窗口可用之后"，不保证"读的时候画面已落地"。

> 2026-09-24 **M11h：空绘制调用不是绘制调用 —— 应用日志最后一条 warning 消失（设计 §11.16cd）**
> · 定位手法：在 `ContentAssembly::record` / `ContentHalves::halvesFor` / `ContentPass::record` 的拒绝点插 `[probe]`，
>   第一帧第 7 趟 pass 是 `content=1 entries=0` 且**没有任何"事实缺失"的 skip 输出** ⇒ 那条 draw 本身
>   `commands=0`；再在 `beginPass` 打印身份 ⇒ `id=7 name=`（无名 = `applyOverlays` 的 gizmo/fps 叠层），
>   `AxisGizmo` 在 `surface_w_ <= 0` 时不设视口 ⇒ 那一帧的场景收集是空的（引擎侧行为，合法）。
> · 缺陷链（三跳，只有第二跳是错的）：空调用合法 → **录制器把调用记成绘制**（计划描述一件不存在的工作）→
>   内容层看到"内容绘制"却没有半片 ⇒ 拒绝**整趟**（连清屏）+ 一条 warning。修生产者，不给消费者加容忍。
> · 修法：`FrameRecorder::render` 只在 `commands` 非空时记绘制；调用仍消费 viewport/lights 公告（一条公告一次
>   调用不变）；`FrameCounters::draws` 是"draw calls **recorded**" ⇒ 空调用不计数；既没画也没清屏的 pass 照旧不存在。
>   契约注记补在 `FrameRecorder.hpp` 的消费规则第二条。
> · 守卫（无设备）`FrameRecorderTest.ADrawingCallWithNothingToDrawIsNotADrawAtAll`：空调用不留 draw、公告被它
>   消费、计数器 == 1、诊断干净；空调用 + 无清屏 ⇒ 计划里没有这趟 pass。变异 1/1：`if (true || !commands.empty())`
>   ⇒ 守卫红 **且** 应用 warning 回到 1；恢复后两处都绿。
> · 证据：`build` 与 `build-release` 同一道门禁 —— **431 用例 0 failed / 0 VUID / 0 SYNC-HAZARD / skipped=0**、
>   hygiene `0/792`、相位 11 行 2 次收尾、应用 **`vuid=0 warnings=0`**（修前 1），画面参数与修前**逐字相同**
>   （`87.04% / 85.14%, preview 244`）；gizmo 从第二帧起照旧可见（首帧它本来就没有命令可画）。

> 2026-09-24 **M11g：计数分配器把 4 对齐的分配变成 `bad_alloc` ⇒ 设备电池全灭（已修；设计 §11.16cc）**
> · 症状：**带设备**的 `test_vsg` 在第一个设备用例上 abort（`LLVM ERROR: out of memory / Buffer allocation failed`）；
>   其实是"分配被拒"而不是"没内存"（0.03 s、峰值 RSS 85 MB）。gdb 断 `posix_memalign` ⇒ 最后一次请求
>   **`align=4 size=512`**；栈 `llvm::allocate_buffer` ← `EngineBuilder::selectTarget` ← lavapipe 的 JIT。
> · 根因：`tests/test_vsg/AllocationCounter.cpp`（§11.16bx 加的**计数分配器**）把**每个** aligned 分配转给
>   `posix_memalign`，而它对 `alignment < sizeof(void*)` 回 EINVAL ⇒ 我们按契约抛 `bad_alloc`，libLLVM 不接 ⇒ abort。
> · 代价：**M11b–M11f（六片）的证据行（"passed / 24 skipped"）都是不带设备的那一半** —— 设备电池事实上停了几片，
>   没人发现。规则：**证据行里的 skipped 条数就是设备电池的缺席**（"跑过了、跳过几条"是误读）；每次收尾都要
>   `VK_ICD_FILENAMES=<lvp_icd.json>` 跑套件并断言 `skipped == 0`（门禁脚本本来就这么要求，前提是人去跑它）。
> · 修法：对齐 **≤ `alignof(std::max_align_t)` 用 `std::malloc`**（"aligned"不等于"更大"）；分支只放 POSIX 半，
>   因为 MSVC 的 `_aligned_malloc` 内存必须由 `_aligned_free` 释放（在 Windows 上返回 `malloc` 内存 = 堆损坏）。
>   守卫 `CoreAllocationGateTest.EveryAlignmentTheLanguageAllowsIsServed`（1..128 逐对齐 + 计数窗口；
>   **检查必须在窗口外** —— gtest 的消息自己会分配，实测给窗口加了 160 B 噪声）。
> · 同一族的**第二处**（只有跑 Release 才露出）：`-O2` 会**省略**对可替换全局分配函数的调用（`[expr.new]` 的省略规则）
>   ⇒ `TheCountedHalfSeesChurnTheHeapReadingCannot` 在 Release 里数到 **0/64**，也就是说这条"正对照"只在 Debug 成立。
>   修法：新 `keepAllocation()`（本文件匿名命名空间的 `noinline` 函数：POSIX `asm volatile("" : : "r"(block) : "memory")`、
>   MSVC `__declspec(noinline)` + volatile 存储）把块地址交给优化器看不穿的代码（**volatile 存储不够**：语言不要求地址值互不相同）。
>   变异：删那一行调用 ⇒ **Release 红（0/64）、Debug 绿（64/64）**。
> · 变异 2/2：删掉那一支 ⇒ 守卫红（exit 1）**且**第一个设备用例 abort（exit -6）；恢复后两处都绿。
> · 现状（2026-09-25，lavapipe + `DISPLAY=:0`）：`test_vsg` **435 passed / 0 skipped / 0 failed**（Debug 与
>   Release 两棵树都是）；两棵树的整道门禁阶段行与画面参数**逐字相同**（0 VUID / 0 SYNC-HAZARD /
>   hygiene `0/793` / 相位 **12** 行 2 次收尾 / 应用 `vuid=0 warnings=0`）。**Release 树别忘了建插件**
>   （`ninja -C build-release gfx_backend_vsg`：没有它三条 `VsgBackendPluginTest` 报空后端，与 M10h 的教训同源）。
>   应用那条 warning（首帧 "no compiled content half"）已由 M11h 收掉；读窗口的偶发红由 M11j 收掉大半
>   （**计数器**那半仍未做，见 M11j 最后一条）。

> 2026-09-17 **V3/V4 收口：把"等上游"从假设变成有门禁的事实（附一个表格形状的坑）**
> 停在"被上游阻塞"上的条目，危险的不是它没做，而是**前提悄悄失效后没人再看它**。
> · **事实（现抓，不是回忆）**：pinned v1.1.16 与**上游 master** 都是 `vkCreateGraphicsPipelines(*device, **VK_NULL_HANDLE**, …)`
> ——`src/vsg/state/GraphicsPipeline.cpp:260`，compute `ComputePipeline.cpp:110`、rt `RayTracingPipeline.cpp:168` 同样；
> vsg 自己的代码里除 Vulkan 头文件（`include/vsg/vk/vulkan.h` 的 typedef）外**不出现** `VkPipelineCache`，也没有
> `PipelineCache` 类型 ⇒ **升到 master 也拿不到**。V4 同理：`src/` 全树无 `vkCmdBeginRendering`，只有
> `vkCmdBeginRenderPass`（`src/vsg/app/RenderGraph.cpp`）。
> · **门禁**：新增 `scripts/check_vsg_upstream_capabilities.py`（7 条断言；无 vsg 树则 SKIP 退 0，照 `check_vsg_window_surface_state.py`
> 的样子）。**变异 4 条全红**：让 vsg 传一个真 cache 句柄 / 在 vsg 头里加 `VkPipelineCache` / 往 `RenderGraph.cpp` 塞
> `vkCmdBeginRendering` / 造一个 `PipelineCache.h`。变异后 vsg 树**逐字节复原**（`diff -q` 验过）。
> · **推上游的最小形状**照 vsg 自己的先例：`Context` 已有 `getOrCreateShaderCompiler()`（`include/vsg/vk/Context.h:88`）
> ⇒ 要一个 `getOrCreatePipelineCache()`（默认 `VK_NULL_HANDLE`，零破坏），`Implementation` 里把那第三实参换成它。
> · **坑**：backlog 里 V3/V4 两行**原本就多带一个空的尾单元格**（9 parts），我按"6 列 = 8 parts"断言，替换后才发现行变成 8 个 `|`。
> ⇒ **表格行替换要在替换后复查 pipe 数**（或者按"目标形状"重建整行），不要只断言替换前。

> 2026-09-17 **P10 结案：缺陷已修且已钉，剩下的 B2 触发条件**未到**（顺手纠正本行一句陈旧话 + 给设计提个醒）**
> 先查"剩余"是什么：本行说`内建路径仍用载体（vsg phong 读 `vine_Color.a`）`，查下去发现这句话**两重陈旧** ——
> vsg 内建 set 这条路径 **2026-09-13 已删**（现在只有引擎自己的 set）；剩下的"白载体"是**静态兜底**，给没写 loc2 色的
> 几何一个合法的 `vine_Color` 属性（乘 albedo 等于没乘），**不承载 opacity**。opacity 今天的唯一来源是每 drawable 的
> `draw.params.x`（渐变片 `alpha = draw.params.x`，顶点色只出 `.rgb`），并被三条**具名**测试钉住：
> `OpacityIsNotPartOfTheVariantIdentity`、`OpacityDoesNotRideTheVertexColour`、`OpacityEditRebuildsNothing`。
> · **B2 按自己的触发条件暂缓**（§12.6："要等第二个标量（材质值或用户参数）出现，否则会把一个 float 包装成一整套 API"），
> 实测条件**未到**：`VineDrawBlock.params` 只有一个活标量（`.x`），`.yzw` 在渐变片里明写 reserved；阴影块的
> `VineShadowBlock` 属于**另一个块**（它自己的 `params.w` 自 2026-09-18 起 = 所属灯的槽位），不算第二个标量。
> · **给将来的 B2 留一句**：§12.6 把"材质值进 `VineDrawBlock.params`"当首个候选消费者，但 `params` 是**每 drawable** 的，
> 材质是**每材质**的（按地址键、跨 drawable 共享、已有 `set0/b0` 材质块）—— 搬进去等于按 drawable 复制材质值，还和材质身份
> 键打架。已在 `.ai/design/graphics-shader.md` §12.6 加一条日期注记。
> · 本次**无代码改动**（缺陷已修、API 按设计暂缓），只改了 backlog 一行 + 设计文档一条注记。

> 2026-09-16 **R5 结案：appfw 插件注册表的 LSan 报告 —— 抑制，但把“为什么是有意的”一并写进去**
> 先查证它到底是不是缺陷：`~DynamicLibraryLoader` 的 `d.release()` **是有意的**（注释写明：若按静态析构序在退出时 dlclose 掉插件代码，而 `CommandManager` / `RenderBackendRegistry` 还持着指进插件的 callable / 工厂指针 ⇒ SIGSEGV）。所以这是**保留决定**，不是忘记释放 —— 而 `asan_leaks.supp` 原来的规则（“框架自己的分配一律不许抑制”）恰好把这种情况也堵死了。
> · 处置：给那份文件加一条**带条件的例外**（“有意保留 **且** 决定写在代码里，可以入表；‘只有几个字节’不是理由”），并新增 `leak:vn::runtime::DynamicLibrary` + 实测数字。
> · 收益：后端两条跑法（`test_vsg` **292** 全绿 + 设备自检 55 行证据全跑完）现在都用**全或无**泄漏判据 PASS —— 比 R2 当时的 `VINE_ASAN_LEAK_SCOPE` **更强**（scope 会把同一轮里别的泄漏一起放过）。scope 开关保留，但降级为“还没 excuse 的泄漏”的备用手段。
> · **证据**：`LSAN_OPTIONS=…:print_suppressions=1` ⇒ `Suppressions used: count 16 bytes 808 template vn::runtime::DynamicLibrary`（**只有**这一类被匹配）。

> 2026-09-14 **着色器内的插值变量前缀 `vn_` → `vine_`**（补上 2026-09-13「着色器内标识符前缀统一 `vine_`」的尾巴）
> - `src/viz/graphics/shaders/` 下 9 个源文件全改：`vn_uv` → `vine_uv`、`vn_view_pos` → `vine_view_pos`、
>   `vn_view_normal` → `vine_view_normal`、`vn_color` → `vine_color`、`vn_texcoord` → `vine_texcoord`、`vn_dir` → `vine_dir`。
>   顶点输入本来就是 `vine_Vertex` / `vine_Normal` / `vine_Color` / `vine_TexCoord0`，现在着色器里的标识符
>   不分输入 / 输出 / 插值，一律 `vine_`。
> - 同步改：**ABI 文档**（`BuiltinShaders::fullscreenVertexProgram`、`ScreenPass::setProgram`、
>   `VsgPipelineFactory` 的用户 program 段）与**钉住成品文本的门禁**（`EmbeddedShadersTest`、
>   `ForwardShaderSetTest`、`OverlayStagesTest`，以及 `GraphicsTest` 里照全屏 ABI 写的宿主 program 样例）。
> - **行为中性**：只换标识符，location / 绑定 / 取值范围 / Y 方向一个不动 ⇒ 画面不变（任何 shading 的 ABI 都没变）；
>   `vine_shader_check.sh` 9 shader × 各 define 组合照旧全编。
> - 教训：**插值名虽然只在顶点/片元两段之间可见，仍是 ABI 的一部分**——文档与单测都会钉住它，改名要一起改；
>   漏改一处不是编译错，而是某个门禁按旧名 find 不到（前者喧哗，后者难查）。

> 2026-09-14 **texcoord 的 kind 显式化：`VINE_TEXCOORD_CUBE`（缺省 = UV）→ `VINE_TEXCOORD_UV` / `VINE_TEXCOORD_CUBE`**（用户口径：“不要隐式，要显式”）
> - **两个轴，别再混**：`VINE_DIFFUSE_MAP` 是**门**（有没有贴图要采）——它由 vsg 的赋值门控置位（`ArrayConfigurator::assignArray`：一赋数组/描述符就置位），
>   并且决定 pipeline layout 里那个属性/描述符**存不存在**（`ShaderSet::createDescriptorSetLayout` 按 `defines.count(binding.define)` 过滤）。**kind** 是“槽是哪一种”，
>   由后端按**数据宽度**手插（三标量 ⇒ CUBE，否则 UV）。一个 binding 只能挂一个 define、`getAttributeBinding` 是首个匹配 ⇒ 门不可能被 kind 名取代。
> - 四个内容 shader（forward / gbuffer 两对）的 kind 链改成 `#if CUBE / #elif UV / #else #error`：**采样了槽却没说 kind 的变体现在编译不过**，
>   不再静默当 UV。
> - 后端 `SceneBridgePipeline`：**每个 variant 一定插恰好一个 kind**；自定义 program 的 opt-in 判据改成“问它被给的那个 kind”（SDK 的 gbuffer 两个名字都声明）。
>   原来只问 `VINE_TEXCOORD_CUBE`，于是“声明 CUBE”的程序在 2-wide 槽上也套用了槽规则——现在按 kind 分别生效。
> - **天空盒例外**：`builtin_skybox.*` 没有门，而 `VsgPipelineFactory` 的**程序级编译是零 define**（`compiledProgramStages`；variant 由 `ShaderSet::getShaderStages(scs)` 按 defines 重编）
>   ⇒ 对它“没有 kind”是真实构建状态，`#error` 会把整个 skybox program 变成 declined。所以它保留“配对”分支，并在注释里点名 `VINE_TEXCOORD_UV`。
> - 为什么不反过来（采样器 → 坐标）：顶点侧宽度只能由**数据**决定（vsg 取数组自己的 format 建顶点输入，Vulkan 要求 `in` 类型与之兼容），
>   不一致时只能让**纹理**让步（白 fallback + 报告）；让数据让步就得更凭空造分量。反方向还要两个 define（采样器 kind + 通道宽度），名字更多。
> - 门禁：`vine_shader_check.sh` 矩阵 = on/off define 组合 × **恰好一个 kind**（每 shader 16 组）；`ForwardShaderSetTest.ASampledTexcoordSlotMustStateItsKind`
>   用 glslang 直接钉（带门无 kind ⇒ 必须失败；UV / CUBE / 零 define ⇒ 必须过）；`ProgramSamplingTest` 钉 per-kind opt-in。
> - 判据：build 0 error（仅 `OverlayStagesTest` 两条**既有**的 `const const` 警告）；`vine_shader_check.sh` PASS（9 shader × 16 组合 + 嵌入字节一致）；
>   test_vsg **250**、test_graphics 259 全过；`gfx_lavapipe_check.sh` PASS —— selftest 证据基线 **55 行逐字节不变** + app 阶段 validation clean（只换分支名，画面没动）。

> 2026-09-13 **shader 文件命名规则：目录里每个文件都是 `builtin_<角色>.<阶段>`**（本条覆盖此前两次取名）
> - 现名：`src/viz/graphics/shaders/` 下 `builtin_forward.{vert,frag}`（原 `std_forward.*`，更早 `vine_forward.*`）、
>   `builtin_gbuffer.{vert,frag}`（原 `gbuffer_geometry.*`）、`builtin_deferred_lighting.frag`（原 `deferred_light.frag`）、
>   `builtin_skybox.{vert,frag}`、`builtin_screen_copy.frag`、`builtin_fullscreen.vert`。
> - 规则：`builtin_` 划分归属（引擎文本 vs 宿主 shader）；角色是**渲染器**的词，不重复阶段或目标（`gbuffer` 不必写 `_geometry`）
>   且不带产品名或 C++ 的词（`std_forward` 读起来是 `std::forward`，弃用）；常量按文件名推导
>   （`kBuiltinForwardVert`）；**program 名 = 文件名去后缀**（`builtin_forward` / `builtin_gbuffer` / `builtin_deferred_lighting`，
>   多 program 加后缀 `_flat` / `_shadowed` / `_<N>`）；**pass 名不带前缀**（`gbuffer` / `deferred_lighting`，
>   因为 pass 画的是宿主可以替换的 program，见 `RenderPipelineBuilder` 的 `gbuffer_program` / `lighting_program`）。
> - 门禁：`test_graphics::EmbeddedShadersTest.EveryShaderNameFollowsTheInventorysRule`（规则 + 反例都测）。
> - 行为中性：证据基线 55 行逐字节不变，`vine_shader_check.sh` 9 shader PASS，test_graphics 258 → **259**。
> - 历史章节沿用当时的名字。

> 2026-09-13 **P0.C1：L1 数据块布局（SDK）+ 命名定案**：L1 块统一命名 **`Vine<Role>Block`**（与既有 `VineLightsBlock` 一致；
> `Block` 明示内存布局、与 `FrameContext`/`RenderCommand` 区分），草稿的 `VineFrame` 改成 **`VineViewBlock`**（per-view 语义，避开 `FrameContext`）。
> `ShaderAbi.hpp` 新增 `VineViewBlock`(288B) / `VineDrawBlock`(80B)（16B 对齐、全 mat4/vec4 ⇒ std140 与 D3D cbuffer 同布局）+ `static_assert`；
> `tests/test_graphics/ShaderAbiTest.cpp` 钉 sizeof/offsetof。口径：行为中性；test_graphics 236 → **239**。下一步 C2（vsg 标注 push ≡ 子集）。

> 2026-09-13 **P0.B1：SDK 显式属性 location 表**：新增 `sdk/vine/graphics/ShaderAbi.hpp`（`VertexAttribute{Position,Normal,Color,TexCoord0}`
> + `attributeLocation()`，值 **0/1/2/8**）；vsg 的 `buildVineShaderSet` / `assembleProgramShaderSet` 用它替代字面量。
> 契约与 DX 映射写在 `.ai/design/graphics-shader.md` §11（L1/L2/L3 + B1..B4 分期）。
> 口径：两条证据基线 47 行不变；test_graphics 235→**236**（+1：表值 ↔ shader 文本声明的 location 一致）；lavapipe PASS。
> 下一步 B2：`ShaderProgram` 参数表 + 命名槽声明。**口径决策见 `graphics-shader.md` §12**：B3 采用选项 C
> （L1 声明式块 + vsg push 作内部优化；C1 SDK 块布局 / C2 标注等价），L2 shim 等第二个后端，**B2 暂缓**（无消费者前不加 `addParam`/`addInputSlot`）。

> 2026-09-13 **P0.A：内建前向着色归 SDK（行为中性）**：`std_forward.{vert,frag}` 从 `gfx_backend_vsg/shaders/` 搬到
> `src/viz/graphics/shaders/`，清单随之移动（嵌入数 graphics 3→5、vsg 4→2）。新增 SDK `BuiltinShaders.hpp/.cpp`：
> `builtinProgram(ShaderPreset)`（**已被 `forwardProgram()` / `flatForwardProgram()` 取代**）+ `gbufferGeometryProgram`/`deferredLightProgram`
> （从 `RenderPipelineBuilder` 搬来，builder 的两个静态工厂改转发，公开 API 不变）。
> 后端 `VsgPipelineFactory::compiledStages(preset)` 取 SDK program 编译（每 preset 缓存一次），`buildVineShaderSet` 不再自带 GLSL。
> 口径：两条证据基线 47 行逐字节不变；`vine_shader_check` PASS（7 shader）；test_graphics 234→**235**、test_vsg 237→**238**；lavapipe PASS。
> 边界：SDK 拥有**着色文本**（L3），后端拥有**编译 + ABI + 管线**（L2）；`ShaderSet` 仍只属 vsg 后端。下一步 P0.B（ABI 契约移入 SDK）。

> 2026-09-13 **着色器文件化 + 构建期嵌入（P12）**：产品 shader 从 C++ 字符串搬进真文件，构建期嵌进二进制；死文件
> `flat.*`（含两个提交进仓库的 `.spv`）删除。清单在**顶层** `cmake/VineShaders.cmake`（生成规则必须在顶层：`tests/test_vsg`
> 直接编译插件源码，要能依赖同一个生成头文件）→ 机制 `cmake/VineShaderHelper.cmake`（`vn_declare_embedded_shaders` /
> `vn_use_embedded_shaders`）→ 生成器 `cmake/VineEmbedShaders.cmake`（`cmake -P`，写 `inline constexpr std::u8string_view`
> + `Entry{name,hash,bytes}` 表）。为什么不用 `file(READ)` + `CMAKE_CONFIGURE_DEPENDS`：那样每次改 shader 都整包
> reconfigure（实测 ~15s），而 `-P` + `add_custom_command` 拿的是 ninja 原生依赖追踪（改 `.glsl` 只重编依赖它的 TU，
> 改生成器本身也会重新生成），且内容没变就不重写头文件（否则 touch 一下 `.glsl` 引发一串重编）。
> 类型口径：`ShaderStage::source` 是 `vn::String`（内部 `std::u8string`）⇒ `String(kX)`；vsg 侧要 `std::string`
> ⇒ `asShaderSource(kX)`（`vine/vsg/support/VsgUtils.hpp`，GLSL 是 ASCII 的逐字节视图）。
> **新门禁** `scripts/vine_shader_check.sh`：每个 shader × 4 种 define 变体（`VINE_VERTEX_COLOR` / `VINE_DIFFUSE_MAP`）
> 过 glslangValidator + 嵌入副本的 SHA-256 前缀与字节数必须与磁盘一致 + 每个 `*/shaders/*` 文件必须在清单里。
> 生成器两条**构建期**守卫（都做过 mutation）：源里出现 CR ⇒ 报错（`file(READ)` 会**静默**把 CRLF 归一化成 LF，所以要
> 用 `HEX` 读原始字节判）；源里出现 `)VINE_GLSL"` ⇒ 报错（否则 raw string 提前结束）。环境事实：本机 CMake 4.2.3 的
> `string(SHA256 …)` 恒返回空串 ⇒ 必须用 `file(SHA256 <path>)`；`glslangValidator -V` 不带 `-o` 会把 `vert.spv`/`frag.spv`
> 写进**当前目录**（门禁曾自己污染工作区，已加 `-o <tmp>`）。
> 判据：迁移**行为中性** —— selftest 证据 47 行逐字节相同、lavapipe 0 VUID、test_graphics 230 → **234**、test_vsg 220 → **224**、
> 诊断格式 0 suspicious；四条 mutation（非法 GLSL / 只改不重建 / 未入清单 / hash 截断）各自恰好目标门禁红。
> 设计与"加一个 shader 的步骤"见 `.ai/design/vsg-custom-shader.md` §10。

## 后端契约（规范性在 `RenderBackend.hpp` 类注释；预算见设计文档 §14）
- **调用序**：`beginFrame` → 每启用 pass（order 升序）`beginPass`→`setPassOrder`→可选逐
  pass 状态→绘制→`endPass` → `endFrame` → `swapBuffers`（**唯一 present 点**）。首帧前有
  warm-up（启用且非清屏的 pass 先各跑一遍），所以"首帧前创建的东西"也要能摆对位置。
- **借用**：camera / commands / lights / target / program 只在当次调用内有效（commands 是本
  帧临时量），`beginPass` 的 pass 只在作用域内有效；**不得保留**，需要就拷贝/上传。
  * `releasePass()` / `releaseRenderTarget()` **同时宣布宿主可以立刻销毁该对象** ⇒ 队列里的“宣布”（
    `request.pass` / `request.target`）必须一起作废：直接驱动路径的请求**跨帧存活**，留着就是悬垂指针；
    接下来那一次画不了 ⇒ **拒画 + 分集报一次**（`PassProtocolViolation`，消息带哪个调用点被跳过），
    **不得静默改画到窗口**。同理：0 尺寸的 target 建不了 ⇒ `TargetBuildFailed` 分集报一次，
    不静默什么都不画。
- **保留**：保留 GPU 状态必须按被服务对象寿命定键、由对应 `release*` 释放、**不随帧数增长**；
  宿主不必为正确性调 `release*`。
- **线程**：串行、不可重入、无需加锁；不得假设跨 `initialize`/`shutdown` 同线程；**诊断 sink
  是同步回调**，只能记录返回，不得回调后端。
- **声明**：输出的三种声明（`setRenderTarget` 画到哪 / `setOutput`·`setOutputTarget` 对象声明（校验 + 对象侧解析）/ `setOutputName` 查表键）**互不覆盖**，一个错误只报一次；**发不出去的必须报** —— 名字表只能交付可采样 target，所以"渲染进窗口却声明发布名"和 `publish(name, nullptr)` 都**分集上报**（过去是静默丢弃，消费者随后被告知"本帧没人产出它"，指向错的一方）。
- **pass 的前置条件（接线期一次性检查）**：`ScreenPass` 必须有输入（否则永远不画）、只声明深度的 texture 路径会被**静默**换成彩色 0、**有 program 就必须有相机**（program 路径要先建 view）——这三条都在 `validateWiring()` 报一次（分集），因为 host 侧看到的是"这个 pass 从来不出现"。
- **声明的 `ImageRef` 必须 `bind` 到一张 target**：未绑定的身份**没有地址** ⇒ 消费者解析不到、承诺校验也无从比较（旧注释写着"接线期已报"其实没报）⇒ 整根线静默。现按**每张图**报一次（分集），且**已被冲突报过的不再报**（一个错误一条消息）。
- **失败**：接口内不抛异常；`false` ≠ 部分生效；**`initialize()` 返回 false 必须自己收拾残局**
  （引擎只在 initialize 成功后才调 `shutdown()`，见 `RenderEngine::shutdown`）。
- **保留预算**（本后端数字）：退役环 4（提交帧）、几何条目按“外侧是否仍持有”回收（无时间窗，2026-09-14 删）、材质 256 条、
  变体/ShaderSet 超限整表清空、pass/target 槽靠 `releasePass`/`releaseRenderTarget`。

> 2026-09-11 **后端诊断通道（失败不再静默）**：新增 `vine/graphics/RenderDiagnostic.hpp`
> （`DiagnosticSeverity` / `DiagnosticCategory`（枚举、按后果分类）/ `RenderDiagnostic` /
> `DiagnosticSink`）+ `RenderBackend::setDiagnosticSink/diagnosticSink/diagnosticCount(category)`
> 与 `protected reportDiagnostic`（默认实现齐全 → 现有后端零改动）；`RenderEngine::setDiagnosticSink`
> 是宿主入口（引擎保存并应用到当前与之后的后端）。**vsg 后端：`VsgRenderer::reportFailure` 是唯一上报
> 权威**（stderr 追踪 + 计数器 + 宿主 sink），槽内 `SceneBridge` 经 `installDiagnosticRoute` 把发现
> 转发给它（保证 `diagnosticCount()` 诚实、无双份追踪）；自由函数 helper 改为**返回失败原因**由调用方
> 上报。已接线：几何拒绝 / 通道丢弃 / 着色回退（原 D9 全静默）/ 编译失败 / 离屏 target 失败 / 内容跳过 /
> 初始化失败；上报频次按 **revision**（不刷屏）。宿主侧 `RenderControl` 把诊断写进 `vine/logging`。
> 验证：test_graphics 151 / test_vsg 58 / `vsg_backend_selftest::runDiagnosticsPhase()`（真实设备上
> 1 条 GeometryRejected + 1 条 ShaderFallback 到达 sink）/ lavapipe 门禁 PASS。详见设计文档 §10。

> 2026-09-07 **Design C（RenderEngine 瘦身 + SceneView）**：相机/导航/内容都不再放 engine，引擎
> 纯调度器（零内容零相机）。新增 graphics 概念 `SceneView`（宿主无关，组合**借用** engine）：owns
> Camera + 内容 Scene（`scene()` 返回 owning `intrusive_ptr<Scene>`）+ 默认 OrbitCameraManipulator
> （懒创建）+ 尺寸/aspect 维护；内容一律 per-pass 显式绑定（`addPass(pass, content, order)`）。
> `ensureWindowPass()` 注册 order-0 窗口 pass（3 参显式绑 view scene）除非 engine 已有「携带该
> camera 且 RT==null」的 pass。RenderEngine 删除 `setMasterCamera/masterCamera`、
> `setCameraManipulator/cameraManipulator`、mouse/scroll/key 三路 `pushEvent`、Resize 里的相机
> aspect 维护，**以及 `scene_/setScene/scene()`**（默认内容）；`hasWindowPass()` 无参
> →`hasWindowPass(raw_ptr<Camera>)`（结构化查询）。RenderControl 持 engine+view，输入/尺寸走 view；
> AppShell/test_plugin 改用 `render_control->view()->camera()/scene()`，内容 pass 显式绑
> `view->scene()`（gbuf/deferred/multislot/builder.setContent）；RenderPipelineBuilder 需显式
> setCamera/setContent。CameraManipulator 基类新增虚 fitToScreen()/home()（Orbit override）。
> test_graphics 121/121（SceneViewTest×4），全量构建绿。详见 /memories/repo/vine-sceneview-engine.md
> 与本文件下方旧 Design B 条目（已过时）。**新 .cpp 进构建需重跑 cmake configure**。
> 2026-09-07 布局最终态：engine `resize(w,h)`（由 `pushEvent(ResizeEvent)` 改名，唯一 resize
> 入口；RenderControl 一行 `engine->resize(w,sh)` 紧接 `view->onSurfaceResized(w,sh)`）只做
> swapchain+frame_ctx；RenderTarget 只有 setSize；新增 `Viewport` 值对象由 RenderPass 持有（未设=
> 全幅）；resize 布局走 SceneView::addSurfaceLayout 回调注册（创建方显式，每 effect 自己
> setSize/setViewport）。test_graphics 124/124。
>
> 2026-09-08 **统一主窗管线 preset**：`RenderPipelineBuilder::build(PipelinePreset, PipelineOptions)`
> → `intrusive_ptr<Pipeline>`（新 `RenderPipeline.hpp/.cpp`，RefCounted）。`Forward`=order0 窗口场景 pass；
> `Deferred`=order-3 gbuffer(MRT albedo/normal+shininess/spec/view-pos+D24, 发布"GBuffer")+order0 全屏延迟
> 光照 ScreenPass（带 camera+绑 content 转发灯光）为窗口 pass；阴影变体 ForwardShadowed/DeferredShadowed
> =占位（暂同无阴影）。Deferred **默认自带临时 shader**（builder 公共静态 `defaultGbufferGeometryProgram()`/
> `defaultDeferredLightProgram()`）与 canonical G-buffer（`defaultGbufferTarget(w,h)`），调用方零 GLSL；options 的
> gbuffer/lighting program 为可选覆盖；缺 camera/content 才返回 null。**SceneView::ensureWindowPass
> 默认 viewer 也改走 build(Forward)**（同一套 recipe），不再手搓 pass；行为不变。test_graphics 128/128。
>
> 2026-09-07 **Scene 收敛为单根**：`Scene` 只持一个根 `Node`（空场景 `root()==nullptr`，不渲染）。
> 删除 `addNode/removeNode/nodes()`；新增 `setRoot(intrusive_ptr<Node>)/root()`；`clear()` 释根
> （灯不受影响，仍用 `clearLights()`）。`findNode/boundingBox/collectRenderCommands` 自单根遍历。
> 消费迁移：`addBox`/demo 改收 `Group*`（单恒等根 Group + `addChild`，删 removeNode 再包 StateNode
> 的写法改为直接 state 包 box 后 addChild 根）；AxisGizmo 3 根并入一个根 Group；RayIntersection
> 两遍历器改单根；test_graphics 增 `setIdentityRoot(Scene&)` helper、SceneTest 改 RootSetClearAndFind/
> RootlessSceneIsEmpty。世界矩阵/bbox/剔除/透明度/状态折叠语义不变（根 Group 恒等）——纯 API 形态
> 收敛，与 osg/vsg "一个 scene = 一棵根子树" 对齐（graphics-scene-graph.md §5）。
> 旧多 root 表述仍留在 graphics-design.md §3.4/§7（已标注过时）。
>
> 2026-09-04 **Material 透明度已移除 + 内建 ShaderSet 契约已归档**：透明只属
> scene/node(叶)/geometry 的 opacity（per-vertex alpha 通道 = vine_Color.a@loc6），
> **Material 纯颜色**。删除 `Material::opacity()/setOpacity()`+`opacity_`（doc 注明
> diffuse alpha 恒 1 忽略）；`RenderCommand` ctor 不再用材质 opacity 播种（Scene
> 收集器重算）；Scene 有效透明度 = clamp(node_opacity)（叶 Geometry 自身即 node，
> 材质项移除）；vsg `VsgRenderer` no-cull 收集器去 `material_opacity` 项；材质默认
> 灰 Phong 在 `VsgMaterialManager` 里 force `diffuse.a=1`。测试改用叶/节点透明度：
> `CollectCommandsSortsTransparentBackToFrontAfterOpaque` 用 MatrixTransform
> setOpacity(0.5)，`CollectCommandsOpacityIncludesMaterial`→改名 `…LeafAndNode`
> (叶 0.5×祖先 0.5=0.25)；MaterialTest 删 opacity 断言。**test_graphics 113/113、
> test_vsg 10/10、全量构建、lavapipe 回归全绿**。材质颜色类型保持 `Colorf`(vec4)
> 不改 vec3（全 SDK 统一 + vsg PhongMaterial/UBO 本就 vec4；opaque 约定放渲染映射
> 层 force alpha=1）。内建 ShaderSet 输入契约（attribute loc/format/define +
> descriptor set/binding + pc 128B + "按名查找+define 变体"机制，flat/phong/pbr 共用
> 一张表）已核对进 `.ai/design/vsg-custom-shader.md` §9（供过渡期与 P0 自写对照）。
>
> 2026-09-03 program 槽 P1 · glslang 运行期编译验证：`tests/test_vsg/GlslCompileTest.cpp`
> （+2）——`vsg::ShaderCompiler::supported()==true`，SDK `ShaderProgram` 的 VS/FS GLSL 逐 stage 经
> ShaderCompiler 成功编出 SPIR-V（module->code 非空，72ms，纯 CPU 无需 device）。test_vsg 10/10。
> 这验证了 program 槽后端装配的"编译半环"；"装配半环"（按用户 Program 建 ShaderSet（stages/
> attributeBindings/descriptorBindings）+ SceneBridge buildGeometry 泛化 + Item 重建键含 program +
> lavapipe/真机像素验证）仍待做——注意 vsg 的 phong ShaderSet 是序列化 blob（shaders/phong_ShaderSet.cpp
> 为 io.read_cast 数据），自定义需手搭 addDescriptorBinding(…, coordinateSpace) 等，视图矩阵经
> ViewDependentState 绑定，需 GPU/像素验证，勿盲改。
>
> 2026-09-03 program 槽 P1 第二步（解析链 + 命令携带）落地：`StateNode` 增
> `setProgram/clearProgram/program()`（子树级着色覆盖，独立于 render-state）；新增自由函数
> `effectiveProgram(node)`（叶子 Geometry program 优先 → 最近祖先 StateNode program → null=默认）；
> `Scene::collectRenderCommands` 每叶把 `cmd.program` 折好；`RenderCommand` 增 `ShaderProgramPtr
> program`。test_graphics **113 tests 全绿**（+4），全工程构建过。遗留：vsg 后端按 cmd.program
> 建 ShaderSet 装配（ShaderCompiler 可用）+ Item 重建键含 program。
>
> 2026-09-03 program 槽 P1 第一步（SDK 类型 + 挂点）落地：`ShaderProgram.hpp/.cpp` 新增
> `ShaderStageType{Vertex/Fragment/Compute}` + `ShaderStage{type,source,entryPoint="main"}`
> + `ShaderProgram : Object+RefCounted`（name/addStage/stageCount/stage/stages）；`Geometry` 增
> `program()/setProgram()`（null=引擎默认，零回归）。test_graphics **109 tests 全绿**（+2），
> 全工程构建过。遗留：StateNode 级 program + 解析链、RenderCommand 携带、vsg 后端按用户 Program
> 建 ShaderSet 装配（ShaderCompiler 已可用）。
>
> 2026-09-03 **glslang 已集成**（vsg 运行期 GLSL→SPIR-V 可用）：
> - gfx_backend_vsg 在 VINE_USE_FETCHCONTENT 分支**改源码构建 vsg**（FetchContent v1.1.16，链系统
>   glslang-dev 16.2）→ `VSG_SUPPORTS_ShaderCompiler 1`（build/_deps/vsg-build/include/.../Version.h），
>   ShaderCompiler.cpp 已编译；无 glslang 的本地 `/opt/opensrc/VSG` 仅 VINE_USE_FETCHCONTENT=OFF 用。
> - 依赖注记：本 build 的 `FETCHCONTENT_FULLY_DISCONNECTED` 曾缓存 ON（网络禁用、依赖预取）——
>   vsg 首次拉取须 `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF -DFETCHCONTENT_UPDATES_DISCONNECTED=ON`；
>   vsg 拉取后与其余依赖一样进 _deps。graphics-shader.md 顶部 ⚠ 已把"仅离线"决策更新为
>   "运行期可用（默认）+ 离线/SPIR-V 兜底"。
> - 验证：test_vsg 8/8、test_graphics 107/107、lavapipe 回归脚本 PASS（新 vsg）。
>
> 2026-09-03 后端 renderState 消费（vsg）落地：`include/vine/vsg/support/RenderStateMapper.hpp`（纯、device-free）
> `makeRenderStateObjects(resolved)->{DepthStencil,Rasterization,ColorBlend,InputAssembly}` +
> `applyRenderStateObjects(config,…)`；SceneBridge::buildGeometry 按 cmd.renderState 装配、Item 增
> render_state（状态变→重建）；tests/test_vsg/RenderStateMapperTest 6 用例全绿（直跑 8/8，全工程构建过）。
> **两个历史决策落地**：①深度——vsg 投影是 reverse-Z(1→0)，SDK CompareOp 为"closer/farther"距离语义
> ⇒ 边界反转映射（Less→VK GREATER…），默认=vsg GREATER=现状零回归；②Blend——per-vertex-alpha 使 alpha
> blend 必须常开，blend.enabled 只是"是否用自定义因子"，vsg 后端无法经 StateNode 关闭 alpha blend。
> 映射：cull→cullMode(CCW front,默认 NONE)、polygon→polygonMode、topology→InputAssembly.topology。
> 遗留：cull winding/blend 因子真机视觉验证（无 GPU）。test_vsg 现 8/8（+6 映射）。
>
> 2026-09-03 场景图 R1 补强：新增 `MatrixTransformTest`（5）与 `GroupTest`（1）+ Scene 嵌套 world
> modelMatrix + GeometryTest 拓扑分离用例，**test_graphics 107 tests 全绿**（新增 8）。
> `vertexCount` = 纯数据统计，不随 StateNode Topology 变化（有测试钉住）；`triangleCount` 已于
> 2026-09-04 移除（Geometry 数据面不保证是三角网格；geometry 模块 `Mesh::triangleCount()` 保留）。
>
> 2026-09-03 场景图 R1 核心落地（test_graphics 99 绿、全工程构建过、test_vsg 直跑 2/2 绿）：
> **`Node` 拆成基类**（name/visible/opacity/parent()/虚 boundingBox()/worldMatrix()；无 children/
> 无变换）；**`Group`** 承接 addChild/removeChild/children()/boundingBox=children 并集；
> **`MatrixTransform : Group`** 为唯一变换源（matrix/setMatrix，经 protected 虚 localTransformMatrix()
> 参与 Node::worldMatrix() 父链累积）；`StateNode : Group` 不变。**`Geometry : Node` 叶子**：
> material 并入（name/visible/opacity 继承自 Node），boundingBox = loc0 本地盒 × worldMatrix()。
> **`Drawable.hpp/.cpp` 已删**；`RenderCommand.drawable`→`RenderCommand.geometry`(GeometryPtr)。
> Scene::collect/findNode、RayIntersection 三遍历器、AxisGizmo、SceneBridge、VsgRenderer no-cull
> walker、app_shell::addBox、TestRenderLiveCommand、GraphicsTest 全部迁移；`makeTriangleNode` 现返
> MatrixTransform(子=Geometry)。bbox 语义=**世界空间**（叶子世界盒；Group/MT=子世界盒并集）。
> 关键易错：`Mat4d()` 默认=identity（无 ::identity()）；Mat4d×Point3d 需 include math/Transform3.hpp；
> intrusive_ptr 析构需完整类型→RenderCommand.hpp 直接 include Geometry.hpp；加/删 .cpp 要重跑 cmake。
> 基线无关失败：test_cppstd/test_runtime/test_system 与本重构无关；test_vsg 的 ctest SegFault 为既有
> 退出时序问题（直跑干净）。
> 遗留：program 槽、renderState 后端消费、MatrixTransform 测试用例补强（现靠 Scene/Node 用例覆盖）。
>
> 2026-09-03 拓扑归位修正：`Geometry` 移除 PrimitiveType（曾误放），改为 **渲染状态项 `Topology`**
> （默认 Triangles；StateNode set/clear；与 PolygonMode 分清：点云=Topology::Points，线框=
> PolygonMode::Line）——同一数据可换状态变三角/点/线框，不换几何（vsg/Vulkan 拓扑属管线）。
>
> 2026-09-03 State 切片已落地：`StateNode` + 状态项（Depth/Cull/Blend/PolygonMode/**Topology**）
> + 折叠函数（collect/resolve/effectiveRenderState）；`RenderCommand.renderState` 已由 collect 折叠
> 填入（Scene 集成 3 用例全绿）。未接后端变体键（下一步）。
>
## 边界框类型（Aabbd）

- `Scene/Node/Drawable/Geometry::boundingBox()` / `computeBoundingBox()` 返回
  `vn::math::Aabbd`（`Rect3<double>` 别名，见 `vine/math/Rect3.hpp`）。
  原来的 `vn::graphics::BoundingBox` 已移除。
- **语义**：`Rect3` 默认构造为零点在原点的合法零盒；累积式构建必须用
  `Aabbd::empty()`（反转哨兵）起步，再用 `expandBy(Point3/Vector3/Rect3)`。
  空盒 `isEmpty()==true`、`isValid()==false`。
- **注意**：`Rect3.hpp` 只前向声明 `Point3/Vector3`，`min()/max()/center()/size()`
  的调用方需自行 include `vine/math/Point3.hpp`/`Vector3.hpp`；
  `vn::math::Point3d` 别名仅定义于 `Point3.hpp`。

## 模块文档（面向使用者）

`src/viz/graphics/docs/usage.md`（仿 `src/base/math/docs/Eigen.md` 的"模块自带 docs 目录"约定）：分层架构、属性沿树折叠的四条规则、每帧数据流与"顶点被别名而非复制"、生命周期与变更契约（**数据变更一律 `Geometry::setRevision()` 公告**）、最小宿主用法，以及现成例子的**构建与运行方式**（`VINE_PIPELINE=forward|deferred|forward_shadowed|deferred_shadowed`（默认 deferred）、`VINE_VSG_GBUFFER` / `VINE_VSG_DEFERRED` / `VINE_VSG_OFFSCREEN_MULTISLOT` / `VINE_VSG_SLOT_DEMO` / `VINE_SHADER_PRESET`、无头门禁）。
文档如实标注 **forward_shadowed / deferred_shadowed 目前是占位（等同无阴影预设）**：阴影切片（order<0 深度 pass + 阴影光照）未实现，只有 `Light::castShadow()/shadow()`、`ShadowSettings`、`ShadowFilter` 已就位，计划见 `.ai/design/graphics-shadow.md`。
**2026-09-13**：占位不再默不作声——`RenderPipelineBuilder::build` 会报一条 `DiagnosticCategory::UnsupportedRequest`；`ShaderPreset`（含 `ShadowedPhong` 那个“保留项”）已删除，内容着色只能显式命名 program（见本文件顶端条目）。
- **着色模式（2026-10-11）**：`SceneView::Options::Mode{Lit, Flat, Unlit, Normals, Wireframe}` 落在**pass 属性**上
  —— 前四种是**画面那条 pass** 的 **program override**（`RenderPass::setProgramOverride`），第五种再多一个**光栅化覆盖**
  （`RenderPass::setPolygonModeOverride(PolygonMode::Line)`）⇒ 同一条链条上换模式**零重建**（同一条管线、同一张 pass 表）。
  四种着色模式是**同一段** `builtin_forward.frag` 文本 + 一个 `#define`：`VINE_FLAT`（已有的逐面法线）/
  `VINE_UNLIT`（只写 albedo × opacity，光照前 return）/ `VINE_NORMALS`（写 `n*0.5+0.5`，alpha 1）；线框用无光照
  程序 + 线填充（wireframe 的惯例：看结构不看明暗）。
  **模式作用在几何阶段（M1，2026-10-11 已落地）**：延迟配方的画面是"全屏光照"那条 pass 产出的（没有几何可着色），
  所以非 Lit 档期间 `SceneView` 把链条按**前向**建（`Options::path` 字段不动 —— 请求归请求、生效归生效；`effectivePath()`），
  于是四档在两条配方上都能画出来。旧行为（延迟路径报一条 `UnsupportedRequest` 并保持 Lit）与那条诊断一起删除。
  代价：延迟配方**进出**非 Lit 档是一次重建（链条变了），链条内换档仍零重建；判据
  `test_graphics` 的 `SceneViewViewportTest.ANonLitModeOnADeferredRecipeBuildsTheForwardChain` +
  `SwitchingBetweenNonLitModesOnADeferredRecipeKeepsTheChain`（变异：让 `effectivePath()` 恒等于配方 ⇒ 第一条失败）。
  门禁：`test_vsg/GlslCompileTest.EveryBuiltinForwardProgramCompilesToSpirv` **真的用 glslang 编这四个程序**
  （define 里写坏的 GLSL 会被后端当成"这个预设没有 SDK 程序"而静默退化——flat 一开始就是这么错的），
  `ProgramAbiTest.EveryModeOfTheForwardProgramDeclaresTheSameAbi` 钉住四者绑定一致（内容集是按 program 绑的）。
- **pass 级光栅化覆盖（2026-10-11）**：`PolygonMode{ Fill, Line, Point }` 从 `StateNode.hpp` 抽到
  **`PolygonMode.hpp`**（两个作用域共用同一个类型：逐子树的 `StateNode::setPolygonMode` 与逐 pass 的
  `RenderPass::setPolygonModeOverride`/`clearPolygonModeOverride`/`polygonModeOverride()`）。`std::optional` 表达"没有覆盖"≠"强制 Fill"：
  清掉覆盖 = 回到内容自己的状态。应用点就在 `RenderPass::execute()` 里那次"需要自己的命令表"的拷贝（与
  program override 同一处，一次拷贝满足两者）。**零后端改动**：后端早就把多边形模式当 EDS3 **动态状态**
  （`VK_DYNAMIC_STATE_POLYGON_MODE_EXT` + `vkCmdSetPolygonModeEXT`，`fillModeNonSolid` 本来就在 `DeviceFeatures` 里请求）。
  **命名/优先级规则**（`graphics-state.md` §4.1）：同一个状态两处出现时，pass 级里**赢了内容**的名字带 `Override`
  （`setProgramOverride` / `setPolygonModeOverride`），pass 级里**会被内容压过**的不带（`setDepthMode`，靠
  `RenderCommand::depthExplicit`）。线框必须是覆盖：否则内容自己写过的 Line 会把"回实面"这个开关永久钉住。
- **「谁呈现这块表面」有了 owner（M2a，2026-10-11 已落地）**：新类 `vn::graphics::RenderSurface`
  （`sdk/vine/graphics/RenderSurface.hpp`，`RefCounted`，**借用**引擎 ⇒ 必须晚于引擎析构）持一块表面的
  **呈现表**：`present(SceneView&)`（顺带 `ensureWindowPass()`）/ `present(raw_ptr<RenderPass>)` / `dismiss` /
  `hasPresentation()` / `presentations()` —— 后者返回 `Presentation{pass, view}`，按**引擎执行序**排序
  （`std::stable_sort`，同序保持声明序；引擎不认识的 pass 落选，所以答案描述的是"现在这一帧谁在画"）。
  视图那条呈现**按需解析** `view->pipeline()->windowPass()`（每次读），所以视图重建后表自动跟着走
  （钉子 `RenderSurfaceTest.TheTableFollowsARebuiltPipeline`）。
  **同时删掉 `RenderEngine::hasWindowPass(camera)`**：那条"任何携带这台相机、RT==null 的 pass 都算有人占了"
  的全局查询是 M2a 要消掉的推断本身（顺序敏感，且会静默跳过共用一台相机的第二个视图）；`RenderEngine` 新增
  `passOrder(raw_ptr<RenderPass>) const -> std::optional<int>`（顺序住在引擎的槽里，这条查询同时是"这条 pass
  还在不在"）。`SceneView::ensureWindowPass()` 从此不问引擎任何事。
  一块表面**恰好一口清屏**、矩形归呈现条目都还是设计（`graphics-render-next.md` §3.2 的 M2b/M2d）——
  **M2b 见下条，M2c 见"aspect 出相机"那条**。
- **清屏归表面（M2b，2026-10-11 已落地）**：`RenderSurface` 决定**谁**清 —— 呈现表（按引擎执行序）**第一条**保留
  自己的清屏，其余条 `setClearEnabled(false)`（`applyClear()`，私有）。从前是"谁先公告谁赢 + 后端只应用第一条公告"，
  于是第二条的 `clearColor()` 与画面**不符**却没人被告知。**没有** `Surface::setClear(ClearOptions)`：那三样
  （enabled/color/depth）已经住在 `RenderPass` 上且是**一份真相**（`announceClear()` 从那儿读），搬一份上去就是
  "同一个状态两处"+ 三态优先级；表面只搬**决定**，值留在 pass（延迟配方那条 `present` 屏 pass 自己说不清 ⇒
  照样不清，不会被"第一条"逼出一次多余清屏）。
  每条呈现自己的"ask"（清不清）被记住（`Entry::asked_of` + `Entry::clears`），所以①第一条被 `dismiss` ⇒ 下一条
  恢复自己的 ask；②视图**重建**（`setOptions` 换了管线）⇒ 新窗口 pass 又是 enabled，靠视图回调
  `surface_->applyClear()`（`SceneView` ↔ `RenderSurface` 互为 `friend`）把决定落回去；③同一个 pass 被两条呈现
  指到（视图那条 + 直接声明那条）只算一次决定。
  **生命周期**：视图与表面互持 `raw_ptr`，两边都自己解绑 —— `~SceneView` 先 `surface_->dismiss(this)`（此时它的
  `default_pipeline_` 还握着引擎，`dismiss` 里要读引擎的执行序）；`~RenderSurface` 把每个视图的 `surface_` 清空。
  任一先死都不留悬垂（用例 `APresentedViewThatDiesLeavesTheSurface` / `AViewThatOutlivesItsSurfaceDoesNotCallIntoIt`）。
  全部判据在 `tests/test_graphics/RenderSurfaceTest.cpp`（变异验证：去掉 `applyClear()` 调用 ⇒ 4 条失败；
  去掉 `entry.clears` / 去掉视图重建回调 ⇒ 各 1 条失败；去掉 `~SceneView` 的自解绑 ⇒ 1 条失败）。
- **aspect 出相机（M2c，2026-10-11 已落地；⛔ 同日已回退 —— 见下面 2026-10-11 A 那条，本节只作历史记录）**：`Camera` 只持**观看**（eye/up/target）+ **投影参数**（fov / near / far /
  类型 / 正交盒），**不持 aspect、也不缓存投影矩阵** —— `setProjectionMatrixAsPerspective(fovy, near, far)`
  （少一个形参）、`projectionMatrix(double aspect)` 现算（正交**忽略**该参数：盒自带比例）、`aspectRatio()` 删除、
  `screenToWorldRay(screenPos, aspect)` 多收一个（**提案漏算的第五个读者**）。为什么：view matrix 只依赖眼睛、
  可跨矩形复用，projection 依赖 fov **和 aspect**、不可 —— 差别就在 aspect，于是"矩形变了投影没跟上"这一整类
  静默错（`.ai/bugs/vsg-resize-distortion.md` 记的那次）在结构上消失，同一台相机可进两块不同形状的矩形
  （镜像 / PiP / 立体 / 编辑器两个窗格）。
  **唯一一处推导**：新契约 `RenderBackend::viewAspect() const`（纯虚）—— pass 的 viewport 优先，否则它画进的
  目标尺寸，再否则表面尺寸，什么都没有 ⇒ 1（"还没有画面"，不是猜一个）；vsg 后端 `beginPass` 清掉上一条 pass 的
  矩形，`setRenderTarget`/`setViewport` 填它，`render()` 前 `recorder.setViewAspect(viewAspect())`。
  **两类读者读同一个数**：①画面 —— `RenderPass::execute()` 在收集命令**之前**问一次（后端刚被告知这条 pass 的
  目标与矩形），转给 `Scene::collectRenderCommands*(camera, aspect)`；②view block —— `FrameRecorder::setViewAspect()`
  （scope 属性，像清屏与深度模式；`beginPass` 复位成 1.0；非正 / 非有限拒收）让 `snapshotCamera()` 用
  `camera->projectionMatrix(pass_aspect_)`。剔除的视锥与画面的投影因此是**同一个矩形**推出来的。
  `Scene` 的帧内 memo 以 **view-projection** 为键 ⇒ 同一台相机两种形状 = 两条 memo 项、两次走树、互不冒充。
  `OrbitCameraManipulator` 自持 `viewportAspect()`（`onResize` 不再往相机里塞 aspect）、`SceneView::applySurfaceGeometry`
  里那段"推 aspect 进相机"删除、`FpsOverlay`/`AxisGizmo` 不再设（它们的 HUD pass 的 viewport 就是形状）、
  方向光阴影照旧 `projectionMatrix(1.0)`（正交）。
  判据（`test_graphics` 330 绿、`test_vsg` 405 绿、其余套件计数不变）：`CameraTest.TheProjectionIsBuiltForTheShapeTheCallerNames`、
  `SceneTest.OneCameraInTwoRectanglesIsTwoPicturesAndTwoWalks`、`RenderEngineTest.EachPassCullsWithTheShapeOfItsOwnRectangle`
  （同一台相机：1600×600 的那条 pass 画出画面外侧那颗、600×600 的剔掉它）、
  `FrameRecorderTest.TheViewBlockIsProjectedForTheShapeTheScopeAnnounced`（公告 2.0 ⇒ 快照投影按 2.0；下一条 pass
  回到默认；0 / NaN 被拒）。变异验证：`execute()` 写死 1.0 ⇒ 1 条失败；memo 去掉 view-proj 项 ⇒ 1 条失败；
  `projectionMatrix` 忽略参数 ⇒ 2 条失败；recorder 固定 1.0 / `beginPass` 不复位 ⇒ 各 1 条失败。
  **回退后的现状**：`Camera::setAspect/aspectRatio/projectionMatrix()/screenToWorldRay(pos)`、
  `collectRenderCommands*(camera)`、`RenderBackend` **没有** `viewAspect()`、`FrameRecorder` **没有** `setViewAspect`、
  `SceneView` 把窗格形状 push 给相机、HUD 相机自己声明形状、矩形与相机不一致由
  `RenderEngine::collectPictureShapeMismatches()` 报出来（详见上面 2026-10-11 A 那条）。
  本节以下提到的用例名 `TheProjectionIsBuiltForTheShapeTheCallerNames` /
  `EachPassCullsWithTheShapeOfItsOwnRectangle` / `TheViewBlockIsProjectedForTheShapeTheScopeAnnounced` 已在 A 里改名或反转
  （`TheProjectionIsBuiltForTheCamerasOwnShape` / `OneCameraCullsWithItsOwnShapeInEveryRectangle` /
  `TheViewBlockIsProjectedForTheCamerasOwnShape`）。
- **状态解析单点（M3，2026-10-11 已落地）**：**一个函数**是优先级表的唯一实现 ——
  `ResolvedCommandState resolveRenderState(const RenderState& authored, const PassRenderState& pass)`
  （`StateNode.hpp`）→ `{state, origin}`，`origin` 逐项回答"这一项是谁给的"（`StateOrigin::Default/Content/Pass`）。
  命令侧只带**内容作者写的那份**（`RenderCommand::authoredState`，每项 optional；`renderState` 的解析值 +
  `depthExplicit` 旗标都删了），pass 侧只带**它自己那份**（`RenderPass::stateOverrides()` →
  `RenderBackend::setPassState()`）。**合并规则**：光栅化覆盖（pass）> 内容 > 默认；**depth 不对称** —— pass 给
  test/write **策略**（`DepthMode` 没有 compare），内容写过就整项归内容（compare 一起），compare 否则跟
  内容/默认走。`program` **不在状态里**（换程序是"用另一套程序画这份内容"，引擎仍旧逐命令改写 ⇒ 后端
  "每条命令一个管线键"的口径不动）；光栅化覆盖**不再改写命令**（跟着 pass 状态走、按命令解析，于是"只切线框"
  的 pass 一次拷贝都不付）。后端侧：`resolveDynamicState(state)` 只剩纯映射，
  `CollectedPass::state` / `CollectedCommand::authoredState` 记的就是这两份真相。
  **行为不变的证据**：既有用例**一条没改语义**就跑绿（`test_graphics` 339、`test_vsg` 405、`test_gui` 253…），
  含 M1 那三条光栅化覆盖用例与 `FrameCompilerTest.ThePassDepthAppliesOnlyWhereTheContentAuthoredNone`；
  新增 `RenderStateTest.ThePriorityTableIsOnePlaceAndSaysWhoDecidedEachItem`（五项逐一 + depth 不对称 + 空内容
  ⇒ 全 default 且 origin 说 default）、`RenderEngineTest.APassAnnouncesItsOwnStateForTheContentItDraws`
  （公告 == `stateOverrides()`，且命令**没**被改写）。变异验证：不公告 pass 状态 ⇒ 2 条失败；解析忽略光栅化
  覆盖 ⇒ 2 条失败；pass 深度反过来压过内容 ⇒ 2 条失败。
- **输入按矩形走 + 焦点视图（M5a，2026-10-11 已落地）**：分屏的两半都落在这张**表面**的表上 ——
  * **焦点**：`RenderSurface::focus(view)` / `focusedView()`。它和矩形、清屏住在一起，因为三件事问的是同一个问题
    （"这块表面被分成什么、谁在画、用户在哪儿"）。默认（没人按过）= **第一条呈现**，所以一个表面永远答得出"当前
    视图"；`focus` 只接受**确实呈现这块表面**的视图（不在表里 = 没有矩形可聚焦）；`focusedView()` 还要**验活**：
    撤掉的呈现、或 pass 已经不在引擎帧里的呈现都不算（否则"当前视图"会答成一张没人画的画面）。
    `dismiss` 顺手清掉 `focused_` —— 对*结果*冗余（存活检查给同样答案），挡的是"指针留着、被后来同地址的视图继承"。
  * **输入路由**：`SurfaceWindow::routedView` 没装 router 时问表面（`viewAt`，逻辑像素 ÷ 表面尺寸），
    **按下即焦点**且**按下总是捕获**（分屏拖拽越界不断线），**键盘**去焦点（没有位置可路由）；
    `setInputRouter` 降级成覆盖。`RenderControl::focusedView()` = 表面焦点，表面没有呈现视图时回退控件自己的视图。
  * **命令**：五条显示模式命令改 `control->focusedView()` —— `setPrimaryRenderControl` 只回答"窗口的 3D 区在哪块
    控件上"。钉子：`RenderControlTest.TheSurfacesRectanglesRouteInputWithNoRouterAtAll`、
    `ModelViewerTest.TheShadingModeCommandsDriveTheCurrentModelView`（追加一段：焦点在第二格 ⇒ 命令改第二格、
    主视图不动；转回来再改一次）。六处变异全部被杀（见 `graphics-render-next.md` §5 的 M5a 行）。
- **矩形放呈现条目（M2d，2026-10-11 已落地）**：**分屏**是这张表的下一层 —— 每条呈现带自己的**归一化**矩形
  （`Presentation::rect`，`optional`），矩形由**表面**持有、**视图读**它：
  `RenderSurface::present(view, rect)` / `present(view)`（沿用视图当前那块）/ `setViewport(view, rect)`；
  `SceneView::setViewport` 在**绑了表面时转发**到表上（没绑时留在自己身上 —— 一个视图单独用不需要表面），
  `SceneView::viewport()` 由 `const NormalizedViewport&` 改成**按值**返回并按需读表（`viewportOf`）：
  一份矩形，两处拼写。`dismiss` / `~RenderSurface` 把矩形**还给**视图（它接着画同一块地方，只是从"表面的声明"
  变成"视图自己的声明"），`~RenderSurface` 的顺序仍是"先解绑再交矩形"（否则视图会去一张已经没有它条目的表里读）。
  `present(view, rect)` 里的矩形经 `applySurfaceGeometry()` 落成设备像素（窗口 pass 的 viewport、targets、HUD 盒、
  layout 步）—— 形状（aspect）不用另说：后端从那条 pass 的 viewport 推（M2c）。
  **同时**：`RenderSurface::viewAt(nx, ny)`（点归哪条呈现；**归一化**坐标，反序扫描 ⇒ 重叠时**最后画的**赢，
  `NormalizedViewport::contains` 让东/南邻拥有共享边；宿主自建的 pass 不中选）；"一个视图呈现两块表面"被拒
  （一个视图画一张画，而它的矩形是向它呈现的那块表读的）—— 与"跨引擎"同一条诊断。
  **边界（本片不做）**：表里**没有表面尺寸**（只有归一化矩形）⇒ 宿主自建的 pass 没有归一化矩形（它自己给
  `pass->setViewport()` 设备像素），`viewAt` 用归一化坐标。把尺寸也搬进表要先回答"尺寸在表面与视图哪一份"——
  两份正是本设计要消的病，而没有验收判据需要它。
  判据（`test_graphics` 336 绿）：`RenderSurfaceTest.TheSurfaceOwnsTheRectangleAndTheViewReadsIt`（两种拼写写同一块；
  invalid ⇒ 铺满；没呈现的视图 `setViewport` 返回 false；`dismiss` 之后视图接着画同一块 —— 用的值 0.7 视图自己
  从没存过）、`PresentingAViewWithARectangleStatesTheSplitInOneCall`、`AViewIsFoundByThePointItsRectangleHolds`、
  `ThePointGoesToThePresentationDrawnLast`、`AViewThatAlreadyPresentsAnotherSurfaceIsRefused`、
  `SceneViewViewportTest.ASurfaceSplitStatesEachViewsRectangleAndNothingIsRebuilt`（半宽 = 640×720、右上四分之一 =
  640×360：两块形状不同；改矩形不重建（句柄 + pass 数不变））。变异验证：视图不读表 ⇒ 2 条失败；`viewAt` 不看矩形
  ⇒ 2 条失败；`viewAt` 正序 ⇒ 1 条失败；`dismiss` 不还矩形 ⇒ 1 条失败；去掉"已在别的表面"的拒绝 ⇒ 1 条失败；
  `SceneView::setViewport` 不转发 ⇒ 1 条失败。

## 帧计划：一眼看出这一帧画了什么（M4，2026-10-11）

- **新单元 `FramePlan.hpp/.cpp`**：`struct PassPlan`（`pass` 指针 + `order`/`name`/`content`/`camera`/`target`/
  `enabled`/`clears`/`clear_color`/`clears_depth`/`viewport`（`optional`，空 = 铺满）/`inputs`/`output`/`record_micros`）
  + `struct FramePlan{ passes, describe() }` —— 全是**值的拷贝**（两个指针是借用的），也**不回答**"这条 pass 是不是
  某块表面的呈现"（那是 `RenderSurface` 的事，引擎不认识表面）。
- **`RenderEngine::framePlan() const`**：按执行序读一遍注册表，只读、不建任何东西。它是"测试别再数 `passCount()`、
  别比指针身份"的替代品，也是打印给人看的那份。
- **`Slot::record_micros`**：`frame()` 里每个 pass 的整个 scope（`beginPass` → `endPass`）前后各读一次
  `steady_clock`（每 pass 一次，不是每次 draw）；**没跑的 pass 清零**。计划讲的是"下一帧会发生什么"，唯一一个讲过去的
  数就是它，所以它旁边写着 `last=` 而不是混在别的列里。
- **`describe()`**：一行一 pass 的 key=value 表（`target=window` = 画到窗口，`clear=-` = 它不清屏，
  `viewport=full` = 铺满）。真机实测（demo 默认 Deferred，`VINE_FRAME_PLAN=1`）11 条：
  `shadow`(-100) → `gbuffer`(-40) → `deferred_lighting`(0) → `forward_transparent`(20) → `present`(40，唯一画到窗口的那条)
  → `axis_gizmo`(70) → `fps_overlay`(90) → 四个 `gbuffer_preview`(120..123)；每条的上一帧记录耗时 4~436 µs 同表可见。
- **顺手给两条内建 HUD pass 起名**（`axis_gizmo` / `fps_overlay`，与 `main` / `gbuffer` / `present` 一致）：没有名字的表里
  那两行只写得出 `(unnamed)`，也就没法拿"名字序列"当结构断言。
- 钉子：`RenderEngineTest.FramePlanDescribesThePassesInTheOrderTheyWillRun`（每项事实 + `describe()` 的文本）、
  `TheFramePlanIsWhatTheFrameActuallyRan`（**plan 里 enabled 的序列 == 记录型后端被开过 scope 的序列** —— 这条让 dump
  不可能变成"另一个故事"）、`TheFramePlanCarriesWhatEachPassLastCost`（2 ms 的慢 pass ≥ 1000 µs、没跑的 = 0）；
  另两条老用例改成结构断言：`SceneViewViewportTest.TheRecipeDecidesWhichOverlayPassesExist`（断言 pass 名字序列）、
  `RenderPipelineBuilderTest.ForwardPathBuildsWindowPass`（断言 plan 那一条的全部事实）。
- **边界**：M4 只读**当前注册表** —— 不编译图、不分配 transient、不做依赖排序（那是 M7 的 FramePlan 编译）；
  `describe()` 是文本，结构性断言请直接读 `passes`。
