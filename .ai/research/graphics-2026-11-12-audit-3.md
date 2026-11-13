# graphics 第三轮审计：框架、公共 API 与设计哲学

> **状态**：调研审计（**非现行设计**；未落地、未决策）
> **日期**：2026-11-12
> **审计对象**：`graphics`（可视化渲染）与紧随其后的 `gfx_backend_vsg`（Vulkan / vsg 实现）
> **方法**：只读。逐条对照代码与 `graphics-*` 设计文档；消费者计数用 grep（命令随条给出）；行数 / 签名数用 `scripts/measure_graphics_claims.py`。
> **结论**：方向正确——发布链、快照、计划值是真实收益；但线程契约与代码相反（R3-1），三条新公共入口零生产消费者（R3-2），`Scene` 与 `backend/` 各有一个边界问题（R3-3 / R3-4）。
> **不重复**：2026-11-12 已收口的 6 项（半片/描述符集 pin 的 O(1) 索引、`ScenePublishResult` / `bindSnapshot`、删除单值 `FrameMode`、`PrepareReport` / `ContentDeclarationIndexes` / `plan()` 前置条件、`ResourceTable::visitValues`、`graphics-threading.md` 重写）。

---

## 0. 总体判断

1. 方向是对的：发布链、快照、计划值已经把“五套过期答案”压成一条边，代码里确有对应物；已收口的 6 项不重复评审。
2. 但**公共 API 比代码走得远**：`applyPlan` / `plan()`、`bindSnapshot`、`prepare()` 在 `src/` 里各有 **0 个生产消费者**，而 `addPass` 家族有 **25 个 src / 193 个测试调用点**——“计划是唯一形状”今天是一句承诺（R3-2）。
3. 更严重的是线程契约：文档说跨侧只有“不可变快照 + 两个原子 revision”，代码里快照行与命令仍带活 `Geometry` / `Material` / `ShaderProgram` / `Light`，`FrameRecorder` 在引擎线程读它们，而所有 revision **都不是 atomic**（R3-1）。
4. `backend/` 被 `install(DIRECTORY sdk/)` 装进公共 include，却被 `BackendHalfGuard` 禁止宿主包含；24 个头里 4 个仍是 Vulkan 拼写（R3-4）。
5. `Scene` 一个类住着作者树、派生缓存、发布值与批深度，且仍是公开的 `DrawListSource`；它的收集机器今天只有测试和“单 pass 直驱”一个测试消费者（R3-3）。
6. 5 个“计划 / 帧”名词覆盖 4 个层次，命名没表达层次；`Session` 被 `SessionBusy` 引用，但 SDK 里**没有 `Session` 类型**（R3-2 / R3-8）。
7. 结论：**先修代码错误（R3-1），再决定“新入口藏起来还是按 5b-2c 一次收口”（R3-2）**；`Scene` / `backend/` 的拆分有真实收益但要分步迁移；其余是词汇与扩展轴整理。
8. `5b-2c` 的真实改动面是 **25 src + 193 tests = 218 个调用点**，不是设计文档写的“约 314”；`K4` 分母 1096，5% 线 55。

**口径**：本轮未改代码、未跑测试。消费者计数用 `grep` 在 `src/` + `tests/`；行数与签名数用 `scripts/measure_graphics_claims.py`。

---

## 1. 审计轴与结论对照

| 轴 | 结论 | 落在哪条 |
|---|---|---|
| A 哲学：能否一句话说清、代码是否遵守 | 能说清；5 句里 2 句今天为假（值载荷、对象单侧） | R3-1 |
| B 公共 API 形状：同一件事几个名字 | 加 pass 6 种入口、画一帧 6 种、内容→引擎 3 条、计划 5 个名词 | R3-2 / R3-6 |
| C 抽象边界：四层是否单向、`backend/` 该不该公开 | 对象级有一条回边；`backend/` 是“装了但禁止包含”；`Scene` 三概念同居 | R3-3 / R3-4 / R3-5 |
| D 扩展点：应该沿哪个轴 | 作者面是数据（对）；逃生口是继承，且与计划互斥 | R3-7 |
| E 结果与错误：几种答案形状 | 7 个枚举 + 121/163 个 `bool`；3 组是重复拼法，5 组是真实语义 | R3-6 |
| F 单向依赖与“谁读谁” | 内容半段调引擎半段对象；线程策略机制 1 个文件、语义 9 个文件 | R3-5 / R3-8 |

---

## 2. 发现（按严重度降序）

### R3-1 ｜ 严重 ｜ 轴 A（哲学）+ F（单向依赖） ｜ 判定：改代码，并收回文档里的错误断言

**证据**

- `Row`（`src/viz/graphics/src/DrawListBake.hpp`）携带 `MaterialBasePtr own_material`、`ShaderProgramPtr own_program`、`GeometryPtr geometry`、`MaterialBasePtr material`、`ShaderProgramPtr program`；`RenderCommand`（`RenderCommand.hpp`）同时带活 `geometry` / `material` / `program` 与不可变 `description` / `material_description`。
- `RenderPass::describe` 把活 `Scene` 写进帧值（`RenderPass.cpp:219`），把活 `Light*` 写进（`:241`）。
- `FrameRecorder` 在 `executeFrame`（引擎线程）里读它们：`programRef` 调 `program->id()` / `program->revision()`（`FrameRecorder.cpp:14-19`）、`source.material->revision()`（`:421`）、`source.isTransparent()` → `material->alphaMode()`（`:432`）、`snapshotLights` 逐字段读 `Light`（`:348-370`）。
- 线程契约 §2.4 写的是“`Buffer::revision()` 与 `Geometry::revision()` 是 `std::atomic<std::uint64_t>`（relaxed 读）”；`grep -rn atomic` 在 `Buffer.hpp` / `Geometry.hpp` / `Texture.hpp` / `ShaderProgram.hpp` / `Material.hpp` 里是 **0**，`revision_` 都是普通 `std::uint64_t`。`StreamUploads.cpp:224` 还在引擎线程比较活 `Buffer` 的 `revision()`。
- 程序与灯**没有**不可变值描述：`ContentStore` 在引擎线程上调用 `buildProgramFacts(*source, ...)`，读 `program.stages()` 与 `program.revision()`；几何与材质有 `GeometryDescription` / `MaterialDescription`，程序和灯没有对应物。

**判断**

不是“文档写少了”，是契约与代码相反。跨层载荷至今带活对象；`Light` 是每帧每 draw 必读，`program` / `material` 是每 draw 必读。`EngineThread` 模式下 UI 改灯 / 材质 / 程序与引擎线程读同一非原子字段 = UB；TSan 只在测试覆盖该交错时才报。违反 `G1`（同一事实两个家）、`G10`（竞态 + 每行 3 个 `intrusive_ptr` 的原子增减）、`G4`（`DrawDescription` 说“值 + 身份”，实际带所有权）。设计哲学第 1、2 句因此**今天不成立**——该改的是代码，不是哲学。

**建议 + 代价**

- 发布侧把 `program@rev` / `material@rev` / `geometry@rev` 折成 `ContentRef` 放进 `Row`；`RenderCommand` 只留 ref + 不可变 description；`isTransparent()` 改读 `MaterialDescription` 的 alpha mode。
- 新增 `ShaderProgramDescription`（stages + revision，不可变，按 revision 缓存）作为程序的“值半边”，与 `GeometryDescription` 同构；`ContentStore::ensureProgram` 改为吃它。
- 灯改成 `PassDraw` / `DrawDescription` 携带 `LightValue` 数组（`describe` 时快照）；删掉 `FrameRecorder::snapshotLights` 对活对象的读。
- 分两步落：先加“值通道”（加法，`C11` 允许），后端只读值；5b-2c 再删活指针字段。
- 代价：`RenderCommand` / `DrawDescription` 是公开结构，最终变形要搭 5b-2c 的车；每行省 24 B + 3 次原子操作；备选“把 revision 改 atomic”只修一个字段、不修灯与程序文本，**不推荐**。
- 顺带：pin 的残余（topology / 附件形状 / variant 变更留旧半片）按 `graphics-content-lifecycle.md` §2.9 既有“已知残余”继续记账，本轮不扩大。

**验收**

新 `T35`——`EngineThread` 下帧 N 在飞时改 `ShaderProgram` / `Material` / `Light`：帧 N 逐项不变、帧 N+1 用新值、`scripts/tsan_check.sh` 0；`T17` 保持绿；grep 断言 `FrameRecorder.cpp` 不再出现 `->revision()` / `->isTransparent()` / `Light*` 解引用。

---

### R3-2 ｜ 高 ｜ 轴 B（词汇） ｜ 判定：先改文档状态；收敛走 5b-2c，不在其前再加公共入口

**证据（可复现）**

- `grep -rn -e "\.applyPlan(" -e "->applyPlan(" src` → 0；`grep -rn -e "\.plan()" -e "->plan()" src` → 仅 `EngineCompile.cpp` 内部。
- `grep -rn -e "\.bindSnapshot(" -e "->bindSnapshot(" src` → 0（仅 `FrameWorker` 内部实现）。
- `grep -rn -e "\.prepare()" -e "->prepare()" src` → 0（仅测试）。
- `grep -rn "addPass(" --include=*.cpp --include=*.hpp src tests | wc -l` → src 25 个调用点 / tests 193 个；头部提及共 50。
- `framePlan()` → 1 个生产调用（`DemoScene.cpp` 一行日志）；`presentRecord` / `presentedCamera` → appfw 3 处；`frameReport()` → 仅后端与测试。

| 语义 | 今天的名字 / 入口 | 谁在用 |
|---|---|---|
| 加一个 pass | `RenderEngine::addPass` ×3、`PassRegistry::add` ×3、`PassChain::addPass`、`ChainBuilder`、`Plan::passes` + `applyPlan` | `addPass` 218 处；`Plan` 0 生产 |
| 画一帧 | `frame()`、`prepare()`、`executeFrame()`、`RenderPass::execute()`、`FrameRecorder::beginFrame/endFrame/swapBuffers`、`RenderSurface::present(view)` | `frame` / `executeFrame` 生产；`RenderPass::execute` 1 测试；`FrameRecorder` 三阶段在后端 SDK 公开 |
| 内容 → 引擎 | `Scene::publish()` + `bindSnapshot()`、帧首隐式发布、`DrawListSource::collectRenderCommands` | 显式 0 生产；隐式生产；直驱 1 测试 |
| 这一帧画了什么 | `framePlan()`、`frameDescription()`、`FrameReport`、`PresentRecord`、`CollectedFrame`、`CompiledFrame` | 1 日志 / 调试 / appfw / 后端 |
| 计划名词 | `Plan`、`FramePlan`、`FrameDescription`、`CollectedFrame`、`CompiledFrame` | 5 个类型、4 个层次 |

**判断**

谁是对的？

- 加 pass：`PassChain` / `PassDesc` 是产品唯一在用的形状（`Viewer.cpp`），`Plan` 是目标；今天两套都公开，宿主必须学会“混用会被 `EmptyPassName` 拒绝”。
- 画一帧：`frame()`（宿主）与 `executeFrame()`（后端）是不同受众，保留；`RenderPass::execute()` 只有 1 个测试消费者；`FrameRecorder` 三阶段是**后端内部语法**，却公开在 SDK 里。
- 内容→引擎：对的是 `publish` + `bindSnapshot`，另两条是迁移路径。
- 计划：`Plan`（声明）与 `FramePlan`（编译后的宿主视图）不是同义词，后者更像“帧表”而不是“计划”；5 个名词没有表达“声明 / 宿主视图 / 后端线 / 后端中间”的层次。

代价：一个宿主今天要学会 4 组区分才能画一帧并改一帧。按仓库自己的 `G8`，三条无消费者的新入口不该已经是公共 SDK；`G1` / `G4` / `G6` 同时被违反。

**建议 + 代价**

- 不改代码、只改状态：把 `applyPlan` / `plan` / `bindSnapshot` / `prepare` 在 `graphics-frame-pipeline.md` §4 标为“迁移 API，尚未成为宿主契约”，并在 `graphics-architecture.md` §1 写“5b-2c 之前不再新增公共入口”。
- 5b-2c 一次收口：删 `addPass` 家族（25 src + 193 tests）、删隐式发布、把 `FrameRecorder` 三阶段降为“后端组件”（见 R3-4）、`FramePlan` 改名 `FrameTable`（或 `Plan` 改名 `SessionShape`——文档本来就叫“会话形状”）。
- 成本：`ChainBuilder` / `ChainPresets` 要改成构造 `Plan`；`K4` 分母 1096、5% = 55；真实调用点 218，不是 314。

**验收**

`K4` / `K5` / `T7` / `T30` 绿；设计文档出现上述状态标记；下一版公共头不再新增“无生产消费者”的入口。

---

### R3-3 ｜ 高 ｜ 轴 C（抽象边界） ｜ 判定：改代码（分两步），文档同步

**证据**

- `Scene.hpp` 一个类里：作者世界（`root_` / `lights_` / `visible_` / `opacity_` / `findNode` / `boundingBox`）、派生缓存（`detail::SceneBake content_bake_`、`detail::DrawListMemo content_memo_`、`contentCollect/Reuse/BakeCount`、`invalidateContent`、`setContentFrame`）、发布（`published_snapshot_`、`edit_depth_`、`published_epoch_/boxes_/materials_`、`publish()`）、查询（`RayIntersection` 走它）。
- 它还是**唯一公开的 `DrawListSource` 实现**，而生产收集已改走 `SnapshotSource`；`Scene::collectRenderCommands` 的生产消费者为 0，只剩 `RenderPass::execute`（1 条测试）与等价性用例。
- 规模：`Scene.hpp` 512 + `Scene.cpp` 624 = 1136 行；测试里与 memo / 计数相关的引用约 108 处。

**判断**

不是一个概念，是三个概念住在一起。`G1`（一个概念一个归属）、`G5`（改发布或改收集都要动同一个类）。**`G13` 不适用**：这里不是“建模 vs 几何”，而是“作者层 vs 派生层”。对照 Godot（`World3D` / `RenderingServer`）、Unreal（`UWorld` / `FScene`）、Filament（`Scene` 在 GPU 侧、作者数据在 `EntityManager`）——三家都把人改的和渲染器算的分开。今天能工作，是因为生产路径已绕开 `collectRenderCommands`；代价是 `Scene` 同时是“旧路径的真相”和“新路径的输入”，`F11` 并行 select 也会被它的 memo 锁住。

**建议 + 代价**

- 把派生 + 发布搬进 `detail::ScenePublisher`（实现 `DrawListSource`，持有 bake / memo / snapshot / 批深度），`Scene` 只留作者树 + 灯 + 查询；`Scene::publish()` 委托，公开 API 不变。`Scene` 不再继承 `DrawListSource`，`RenderPass::describe` 不再把活 `Scene` 写进帧值（R3-1 的根因此消失）。
- 两步：先让 `ScenePublisher` 成为唯一收集实现（`Scene` 委托），再删继承。
- 代价：拆 624 行、`SceneRows` / `SnapshotSource` 改指向、108 处测试引用迁移；风险中，可分步回退。

**验收**

`T34`（600 帧逐项相等）继续绿；新增断言 `Scene` 不再 override `collectRenderCommands`；`K2` 常驻不升。

---

### R3-4 ｜ 中高 ｜ 轴 C（`backend/` 该不该在公共 SDK） ｜ 判定：改打包 + 改文档，不改分层

**证据**

- `sdk/vine/graphics/backend/` 24 个头；`cmake/VineLibraryHelper.cmake` 的 `install(DIRECTORY ${sdk_dir}/ ...)` 把它们装进公共 include；`BackendHalfGuard.hpp` 让宿主包含变成**编译错误**；`scripts/check_include_hygiene.py` 的 `BACKEND_HALF_AUDIENCE` 只允许 `gfx_backend_*`、`test_vsg` 与 `libGraphics` 自己。
- 24 个里有 4 个仍是 Vulkan 拼写：`Keys`（`VkFormat` + `ImageLayout`）、`ClearPlan`（`ImageLayout`）、`TargetPlan`（`ImageLayout` / subpass / device format）、`FrameCompiler`（subpass / device format）。

**判断**

作为“驻留与帧纪律”它把复杂性吸收在后端（`G7` 做对了）；作为**公共 SDK** 它把内部机制抬成了公共词汇（`G4` / `G6`），而“装进 include 但禁止包含”本身是矛盾的（`G1`）。**`G13` 不适用**。第二个后端：帮它的是 20/24（`ResourceTable` / `RetirementQueue` / `FrameTimeline` / `FrameArena` / `FrameRing` / `Observe` / 证据夹具）；绑它的是那 4 个 Vulkan 拼写——D3D12 / WebGPU 要么把 `uint32_t` 解释成自己的格式码，要么改头。结论：**帮一个 Vulkan 家族的后端，绑一个非 Vulkan 后端**。

**建议 + 代价**

- 不改源树（libGraphics 要编译它），改安装与文档：`install(DIRECTORY sdk/)` 拆成 host component（`vine/graphics/*.hpp`）与 backend component（`vine/graphics/backend/*.hpp`），后者只由 `gfx_backend_*` 的包配置消费。
- `graphics-architecture.md` §2 加一行：“`backend/` 是后端接缝，其 ABI 随后端变、不随宿主变。”
- 第二个后端落地前，把 `Keys` 的 device format 明确成“设备自己的不透明格式码”，把 `ImageLayout` / subpass 的 Vulkan 语义写回 `vsg-backend.md`。
- 代价：一个 CMake 组件 + 文档；不改 API。

**验收**

`cmake --install` 后 host include 目录不含 `backend/`；`check_include_hygiene.py` 全绿；`T15`（`check_vsg_content_seam`）绿。

---

### R3-5 ｜ 中高 ｜ 轴 F（单向依赖） ｜ 判定：改代码（小）

**证据**

`EngineCompile.cpp:295-300` 的 `buildFrameValue`（内容半段）调 `worker_->publish(*scene)`；而 `FrameWorker.hpp` 的类注释是“THE ENGINE SIDE OF ONE FRAME”，`publish()` 按 `graphics-threading.md` §2.2 是**内容侧的动词**。`publications_mutex_` 横跨两半；同一场景 N 个 pass 会调 N 次 `publish()`（后 N-1 次 `Unchanged`，仍是一次锁 + 一次 epoch 比较）。 <!-- drift-ok -->

**判断**

不是线程方向的回边（同一条内容线程），是**所有权 / 命名上的回边**：内容半段必须知道引擎半段的私有对象才能发布。`G2`（对象级依赖指向错误一侧）、`G12`（名字说的是引擎半段，做的事一半是内容侧）。这也解释了为什么“发布 = 一次指针交换”在代码里是“每 pass 一次检查”。

**建议 + 代价**

- 拆 `FrameWorker` 为 `PublicationStore`（内容侧：`publish` / `bind` / `hasExplicitSnapshot` / `SnapshotSource` 表，锁在这里）与 `FrameExpander`（引擎侧：`runPass` / `run`，只读 store）。
- `buildFrameValue` 调 store；`expandFrameValue` 调 expander；顺带把发布提到“每帧每场景一次”。
- 代价：162 + 182 行拆两个文件；`FrameWorkerTest` / `HeadlessBackendTest` 改 include；5b-2c 删隐式发布时只删 store 的调用，不碰 expander。

**验收**

`T34` / `T17` 绿；`FrameRunnerTest.ManyFrames...` 绿；grep 断言内容半段不再出现 `FrameWorker`。

---

### R3-6 ｜ 中 ｜ 轴 E（结果与错误） ｜ 判定：先写规则进文档，5b-2c 时改代码

**证据**

`FrameOutcome{Ran, Submitted, NotInitialized, NoDevice}`、`PrepareReport::Outcome{Prepared, NoDevice, NotInitialized, Refused, Unsupported}`、`PresentOutcome{Presented, Skipped, Lost}`、`PlanProblemCode{…, SessionBusy}` + `vector<PlanProblem>`、`ScenePublishStatus{Posted, Unchanged, InsideBatch}`、`DiagnosticSeverity/Category` + `ReportOnce`、`FactResult<T>/FactMiss`。另有私有 `CompileProblem`（按句子比较 episode）与公开 `PlanProblem`（按 code）两套问题词汇；`FrameStats.hpp` 里没有 `FrameStats` 类型；SDK 里 121 个（含 backend 163 个）`bool` 返回。

| 失败种类 | 今天的表达 | 应该的表达 |
|---|---|---|
| 引擎没起来 / 没设备 | `FrameOutcome` 与 `PrepareReport::Outcome` 各写一遍 | 一个 `NotReady{reason}`，各动词复用 |
| 合法请求但此刻不能做（帧在录） | `PlanProblemCode::SessionBusy` + `PassProtocolViolation` 诊断 + `Outcome::Refused` | 单一 `Refused{reason}`，**不进**“形状缺陷”枚举 |
| 形状本身不合法（可多错） | `vector<PlanProblem>` + 稳定 code | 保留（正确形状） |
| 内容没变 / 批内 | `ScenePublishStatus` | 保留（内容事实，不是失败） |
| 这一帧的图成了没有 | `FrameOutcome::Ran/Submitted` 与 `PresentOutcome` 混在一起 | `frame()` 只答 Ran / Submitted；picture 只用 `PresentOutcome` |
| 某个 draw 服不了 | 诊断流 | 保留（流，不是返回值） |
| 查表没命中 | `FactResult/FactMiss` | 保留（内部，有 cause） |
| 有没有 / 多少 | `optional`（absent ≠ zero） | 保留（正确） |

**判断**

8 行里 5 行是真实语义，3 行是“同一个东西的不同拼法”。`G6`（一致）、`G4`（`SessionBusy` 把运行时前置条件塞进形状缺陷码）。另外“这帧声明有毛病”有两套答案：公开 `PlanProblem`（code）与私有 `CompileProblem`（句子），后者按措辞判 episode——messages 不是契约。

**建议 + 代价**

`frame()` 只答 `Ran|Submitted`；`applyPlan` 返回 `{applied, problems, refused}` 三态；`SessionBusy` 移出 `PlanProblemCode`；在 `graphics-frame-pipeline.md` 加一张“失败种类 → 表达”表。代价：公开 API 变化，必须等 5b-2c 窗口；`TheCodeSpellingsAreStable` 要跟着改；121/163 个 `bool` 不必一次改，只改这三组。

**验收**

`PlanTest` 新断言“busy 不是 plan problem”；`FrameOutcome` 用例改两值；表进设计文档。

---

### R3-7 ｜ 中 ｜ 轴 D（扩展点） ｜ 判定：改文档 + 加一条诊断；不做扩展注册表

**证据**

作者面 = `PassKind` 封闭集 + `PassDesc` 数据；逃生口 = `RenderPass::describe()` 虚函数（`ScreenPass` 是唯一子类）；后端面 = `RenderBackend` / `RenderDevice` / `RenderFrameExecutor` 虚接口。加一种 pass 形状：`grep -rln PassKind src/viz/graphics` = 9 个文件，`grep -rln DrawKind src` = 17 个文件（30 个 `DrawKind::` 分支）。加一个后端：实现 4 个接口 + 复用 24 个 `backend/` 头（受 R3-4 的 4 个 Vulkan 拼写约束）。

**判断**

**主扩展轴是数据**（和 Godot / Filament / bgfx 同向，好）；**第二轴是继承**，且与“计划是唯一形状”互斥——`applyPlan` 对同名同 kind 原地更新会保留子类节点，但删除后重建或 kind 改变会**静默降级成基类**，`Plan` 里没有任何字段能说“这个名字是自定义 pass”。`G7`（`PassDraw` 要填什么、何时被 expansion 采集，这些知识推给调用者）、`G4`（计划不能表达它声称能表达的集合）。

**建议 + 代价**

- 文档写明：“计划只表达 `PassKind` 能表达的形状；子类 pass 不在计划契约内，`applyPlan` 遇到会被重建的名字应拒绝并报一条稳定诊断。”
- 不加 `PassKind::Custom` / 工厂注册（`G8`：今天只有一个子类、一个测试消费者）；真有宿主自定义 pass 时再加。
- 代价：文档 + 一条诊断；若将来加注册表，才是大改。

**验收**

`PlanTest` 新用例：子类 pass 经 remove / re-add 被拒绝（或保留子类），code 稳定；`T7` / `T30` 保持绿。

---

### R3-8 ｜ 中低 ｜ 轴 F（G5）+ B ｜ 判定：改文档（声明组合根）；拆分等真实消费者

**证据**

`RenderEngine.hpp` 1462 行、**56 个公共签名**、被 23 个文件包含；一个类里 8 件事：帧泵（`frame` / `waitForFrame` / `setFrameThreading` / `frameNumber`）、计划表（`addPass` ×3 / `removePass` / `clearPasses` / `plan` / `applyPlan` / `passOrder`）、编译（`compileFrame` / `scheduleRows` / `buildFrameValue` / `collectPlanProblems`）、诊断路由（`diagnostics()` / `setDiagnosticSink` / `reportEngineProblem`）、线程生命周期（`runner_`）、lease 提供者（`lease_cell_`）、设备借用与区（`device` / `initialize` / `shutdown` / `createSurface`）、驻留释放（`releaseContent` ×3）与报告（`frameReport` / `presentRecord` / `presentedCamera` / `transientTarget` / `frameDescription`）。改线程策略的**机制**只在 `FrameRunner`（123 行）+ 5 个调用点，但**语义契约**外溢到 9 个文件（`RenderEngine.hpp/.cpp`、`FrameRunner.hpp/.cpp`、`EngineFrame.hpp`、appfw `RenderControl.hpp/.cpp`、`SurfaceWindow.cpp`、2 个测试）。

**判断**

`G5` 的判据对**机制**成立（`FrameRunner`），对**语义**不成立（`waitForFrame` / 一帧延迟被 appfw 与测试直接使用）。这不是马上要拆，而是要把“组合根”作为设计事实写下来，否则下一次改动会继续往这个类加东西。另外 `Session` 作为名词被 `PlanProblemCode::SessionBusy` 引用，但 SDK 里没有 `Session` 类型——`G1` 的空洞。

**建议 + 代价**

- `graphics-architecture.md` §2 加一条：“`RenderEngine` 是组合根：它拥有帧泵、会话形状与设备借用；新的机制属于 `detail::`，不属于它。”
- 短期不拆。若 5b-2c 删掉 `addPass` 家族，`slots_` / `applyPlanRows` 可以自然沉到 `detail::SessionShape`（有真实消费者的拆分）。
- `SessionBusy` 的字符串保留（测试钉住），文档改称“引擎里有帧在录”；`Session` 成为类型等 F8（一 Session 多 Surface）落地。
- 代价：零代码；收益是下一次加功能时不再默认往 `RenderEngine` 塞。

**验收**

设计文档出现组合根声明；`RenderEngine.hpp` 行数在 5b-2c 后不回升；`SessionBusy` 的文档不再指一个不存在的类型。

---

## 3. 修复任务表

已移入 `../backlog/graphics.md` §1.1（W1–W9）：研究稿只留证据与论证，待办只有一个家。
执行顺序：W8 / W5 → W1 → W3 → W4 → W6a → W2 → W9（W7 / W6b 同窗）。

### 3.1 现在不做

这些项在 backlog 里已有自己的触发条件，这里只给指针，不重复定义：
- pin 覆盖 topology / 附件形状 / variant → `graphics.md` T27（显存预算）。
- F11 派生层并行 → `graphics.md` T24（收集 > 2 ms 或 1 万 drawables > 4 ms）。
- 第二个后端 / `backend/` 提成独立库 → `graphics.md` T22 / T15。

---

## 4. 不在本轮范围

- 5b-2c 的实际执行（本轮只审计；W9 已在 backlog §1.1，触发条件未变）。
- pin 的覆盖面与显存预算（按既有“已知残余”走测量）。
- F11 派生层并行（前置是纯函数化与 memo 线程模型）。
- Vulkan 同步细节与 vsg 坑表（`vsg-backend.md` / `vsg-pitfalls.md`）。

---

## 5. 参考

### 5.1 代码

- `src/viz/graphics/src/DrawListBake.hpp`、`src/viz/graphics/sdk/vine/graphics/RenderCommand.hpp`（载荷）
- `src/viz/graphics/src/RenderPass.cpp`、`src/viz/graphics/src/backend/FrameRecorder.cpp`（跨线程读）
- `src/viz/graphics/src/EngineCompile.cpp`、`src/viz/graphics/src/FrameWorker.cpp`（发布与半段） <!-- drift-ok -->
- `src/viz/graphics/src/Scene.cpp`、`src/viz/graphics/src/SceneRows.hpp`、`src/viz/graphics/src/SnapshotSource.cpp`（场景与派生）
- `src/viz/graphics/sdk/vine/graphics/backend/`（24 个头）
- `src/viz/graphics/sdk/vine/graphics/Plan.hpp`、`PassDesc.hpp`、`FramePlan.hpp`、`FrameDescription.hpp`（计划词汇）<!-- drift-ok -->
- `src/viz/graphics/sdk/vine/graphics/PrepareReport.hpp`、`PresentTarget.hpp`、`Scene.hpp`、`RenderEngine.hpp`（结果 / 入口）
- `scripts/measure_graphics_claims.py`、`scripts/check_include_hygiene.py`、`cmake/VineLibraryHelper.cmake`

### 5.2 文档与 ADR

- `graphics-architecture.md`、`graphics-frame-pipeline.md`、`graphics-threading.md`、`graphics-scene.md`、`graphics-content-lifecycle.md`、`vsg-backend.md`
- `ADR-2026-11-12-adopt-graphics-next.md`、`ADR-2026-11-12-snapshot-takes-over-collection.md`、`ADR-2026-11-09-address-keys-hold-the-object.md`

### 5.3 前序审计

- `graphics-2026-11-11-next-audit.md`（第二轮：`A1–A13` / `X1–X10` / `K7–K11`）
