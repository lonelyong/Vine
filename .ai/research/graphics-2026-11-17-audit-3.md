# graphics 模块与两个渲染后端 · 独立审计（2026-11-17，第 3 轮）

> **状态**：研究快照（非权威，非设计）。
> **日期**：2026-11-17 ｜ **基线**：`dev` @ `263adfbe`，工作树干净。
> **性质**：只读审计。本页是唯一写出的文件；`src/`、`tests/`、`scripts/`、`.ai/design/`、
> ADR、backlog 均未改动。
> **方法**：先读 `copilot-instructions.md`（G1–G13、命名/注释/数值/所有权规则）、
> `framework-design.instructions.md`、`.ai/design/` 全部 graphics 页、`.ai/backlog/graphics.md`、
> 两份 2026-11-16 ADR；再**重测**计数、AST 命名空间清单、门禁、测试、TSan 与 demo。
> 旧研究文档只用于"承诺 vs 今天"（§10），不作事实来源。
> **复核提示**：所有数字都注明命令；每个发现都能用 `file:line`、grep、ctest 或一次运行重现。
> **门禁状态**：`check_ai_docs.py --strict` 对本页只剩 1 条 finding——本页还需登记进 `.ai/README.md`；
> 本轮任务把可写文件限定为本页，故登记留给下一次提交（`check_design_docs.py` 0 error / 0 warning）。

---

## 1. 摘要

**一句话**：能力、用例与九道门禁都还在原位，18 个重设计批次确实落地了；但"TSan 三套 0 报告 / 门禁钉住接缝文件集 / 所有状态已收口"这三类句子今天各有实测反例，其中一处是**真实的 C++ 数据竞争**，另一处是**宿主默认后端由字母序隐式决定**。

最重要的 5 条：

1. **真实数据竞争（新）**：`TargetObjects::fresh` 是普通 `bool`（`TargetObjects.hpp:114`），
   帧体在 `Device.cpp:1733` 写它，`readColorBuffer` 的"等待前先答"阶段在 `Device.cpp:3484`
   无锁读它。今天 `VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` = **1 warning**（write T2853
   vs read T2854）。门内重查把可观察结果保住了，但这是 UB，且推翻了"TSan 三套 0 报告"的文档句。
2. **一条门禁并不存在**：`docs/guides/graphics-backends.md:95` 说接缝文件集由
   `check_include_hygiene.py` 的 `SEAM_HEADERS` / `SEAM_SOURCES` 钉住；两个常量在脚本里
   **不存在**（`grep -n 'SEAM' scripts/check_include_hygiene.py` 无输出），接缝文件集今天没有守卫。
3. **读回仍是两答案形状**：`RenderDevice.hpp:454/481/503/516` 四个公开签名仍是
   `bool` + `ReadbackResult*` 出参；第二份 ADR 决定 5 的 `S-10`（升级为返回的一部分）没有落地，
   `check_api_style` baseline 仍为 5（9 条记录里 7 条已死：`PassDesc` 的 `std::function` 已在 B16b 删除，6 条 `VisualUserIO` 在扫描目录之外）。
4. **用户文档点名了不存在的 API**：`docs/guides/graphics.md` 用
   `RenderCommand::modelMatrix` / `authoredState`（实际是 `model_matrix` / `authored_state`，`RenderCommand.hpp:127/182`）、
   `SceneView::layoutParticipant()`（实际 `addSurfaceLayout()`）、`RenderEngine::validateWiring()`
   （无此成员）。520 个反引号标识符里 22 个在 `src/`+`tests/` 找不到；
   `check_doc_symbols.py` 只守卫 `docs/guides/graphics-backends.md` 一页。
5. **默认后端是隐式的**：`SurfaceWindow::useDefaultBackend()` 取
   `RenderBackendRegistry::entries().front()`（`SurfaceWindow.cpp:448`），而 `entries()`
   按**名字**遍历 map（`RenderBackendRegistry.cpp:94`，`RenderBackendRegistry.hpp:154`）⇒
   今天 `vk` 因字母序胜出，不是经 ADR 的切换门；两份文档对"谁是生产后端"说法相反
   （`vk-backend.md:47` 说未达标前 `"vsg"` 是生产后端；`graphics-backends.md:16` 说 `vk` 是生产后端）。

补充一条结构事实：`vsg-backend.md` §2.9.1 说内部类型按 `api::` / `core::` / `support::` 分组；
AST 实测是 `vn::backend_vsg` 根 **120 个直接声明（48 类型 + 60 自由函数）**、`core` 70、
`api` 8、`detail` 90，`support` 只是目录不是命名空间。

**概念层（§11，对照 Filament/O3DE/UE/wgpu）**：没有需要重开结构的概念缺陷——视图、区、pass 作者、
目标声明、能力、设备/队列、资源寿命、命令记录与成熟框架同形；render graph / bindless / 多帧在飞 /
异步编译是**有理由的不借**。语义上最值得处理的是：**读回两套 SDK 拒绝枚举重叠**（C-A3-06，B4）、
**一概念三名 session/executor/driver**（C-A3-02）、**逐项/逐调用命名与业界相反**（C-A3-03）、
**插件内近名/隐名**（`ResidenceTable`/`ResourceTable`、`ProgramHalves`，B13）、以及
**接缝没有名字**（8 个机制住在宿主命名空间且无文件集门禁，C-A3-10/B3）。其余以译名表与
“假朋友”表解决（B11），不改公开名（G8）。

---

## 2. 清点（实测数字 + 命令）

| 项 | 实测 | 命令 |
|---|---|---|
| 文件 | graphics `118 hpp + 84 cpp`；vk `27 + 20`；vsg `76 + 62`；tests `24 / 27 / 78`（vsg 含 11 hpp） | `find … -name '*.hpp'` / `'*.cpp'` |
| LOC | graphics **38,033**（hpp 22,131 / cpp 15,902）；vk **17,444**（cpp 12,306）；vsg **35,391**（cpp 20,576）；tests **74,827** | `wc -l` |
| 安装面头文件 | **96** 个 `sdk/vine/graphics/*.hpp`（含 `graphics_global.hpp`） | `ls … \| wc -l` |
| 用例 | `test_graphics` **617 / 79 套**；`test_vk` **167 / 41 套**；`test_vsg` **572 / 83 套**；`BackendParityTest` **13** | `--gtest_list_tests`；三次运行全绿 |
| 公开标注 | `VN_GRAPHICS_API` **188** 次 / 87 个 SDK 文件；`VN_BACKEND_VK_API` 23；`VN_BACKEND_VSG_API` 45 | `grep -rho … \| wc -l` |
| 导出符号 | `libvnGraphicsd.so`：**1208** 个非弱 `T` + 7323 个弱（inline/template） | `nm -D --defined-only … \| awk '$2=="T"'` |
| 诊断 code | **124**（`vk` 65 / `vsg` 24 / `engine` 34 / `appfw` 1）；baseline 为空（只剩注释） | `grep -v '^#' scripts/diagnostic_codes.txt \| wc -l` |
| 宏 | 宿主 include 全部 96 个公开头后可见 **55 个 `VN_*`**，其中 **20 个函数式** | `clang++ -E -dM`（配方见 §4 末） |
| 命名空间 | `vn` 根 26 个命名空间级声明；`vn::graphics` **318**；`vn::backend_vk` **60**；`vn::backend_vk::detail` 15；`vn::backend_vsg` **120**；`::api` 8；`::core` 70；`::detail` 90 | clang AST + 解析器（§4 末配方） |
| 门禁 | 九道全绿：`check_api_style` **5 baseline**；`check_public_docs` **974 baseline / 0 new / 0 stale in 107 files**；`check_doc_symbols` 1 页 / 138 unit | 附录的命令 |
| ctest 边界 | `content_seam` + `vk_exposure` **2/2 passed** | `ctest -R 'content_seam\|vk_exposure'` |
| TSan | `test_graphics` **0**；`test_vsg`（Window/Probe/Plugin 过滤）**0**；`test_vk` **1**（真实竞争，§3 A3-01） | `scripts/tsan_check.sh` |
| ASan | `test_vk` 聚焦 + LeakSanitizer（`vn::backend_vk` 范围）**PASS** | `VN_ASAN_TARGET=test_vk VN_ASAN_LEAKS=1 VN_ASAN_LEAK_SCOPE='vn::backend_vk' scripts/asan_check.sh` |
| demo | `session up after 40 ms`、`surface Pending -> Presenting after 196 ms`、**`[error]` 0**、`[warning]` **3** | `timeout 25 ./Vine --open demo` |
| 诊断双向 | 用到 ↔ 登记：`check_diagnostic_formats` 0 suspicious；vk `contentRefusalCode` 无 `default`（`ContentRefusal.cpp`），vsg 读回映射无 `default`（`VsgBackend.cpp:484`） | gate + grep |
| 版本 | `dev` @ `263adfbe`；`git log --oneline -25` 与 ADR 的 B0–B17 对应 | `git log` |

**必须自己重测的六个数（本轮结论）**：unit **617/167/572**；公开标注 **188**；用例数同上；
诊断 code **124**；宏 **55**；根命名空间导出名见 §4（`vn::graphics` 根 **58 函数 + 1 函数模板**、16 常量、32 别名；AST 另报 1 条 `inline` 解析伪名）。

---

## 3. 发现表

> 类别：缺陷 / 词汇 / 归属 / 文档 / 能力缺口 / 命名空间 / 证据。
> 严重度：高 = 会错或会误导；中 = 契约/文档与实际不一致，或有 UB 但结果被兜住；低 = 维护性。
> "建议批次"指 §8；**没有消费者的项只登记，不主动改名（G8）**。

| ID | 类别 | G# | 证据（file:line / 命令） | 影响 | 严重度 | 建议批次 |
|---|---|---|---|---|---|---|
| A3-01 | 缺陷（并发/UB） | G11 / G10 | `TargetObjects.hpp:114` `bool fresh{ true };`；写 `Device.cpp:1733`；读 `Device.cpp:3484`；TSan：`VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` → `WARNING: ThreadSanitizer: data race`（write T2853 @1733 vs read T2854 @3484） | 普通 bool 被帧体与宿主动词无锁共享；门内重查保住结果，但标准意义上是 UB，TSan 网关红 | 中 | B1 |
| A3-02 | 证据 / 文档 | G11 | 同上；`.ai/design/graphics-threading.md` §5.2 R18 与 `graphics-sessions.md` S9 写 "TSan 三套 0 报告（2026-11-16）" | 并发证据句今天不成立；审阅者会据此跳过复查 | 中 | B1 + B2 |
| A3-03 | 契约（API 形状） | G4 / G6 | `RenderDevice.hpp:454/481/503/516`（`python3 scripts/check_api_style.py --list` 的 4 条 baseline）；ADR-2026-11-16-authoring §5 决定 5（`S-10` 试点） | 读回仍是"bool + 出参枚举"两个答案；baseline 无法清零（9 条记录里 7 条已死：`PassDesc` 的 `std::function` 已在 B16b 删除，6 条 `VisualUserIO` 在扫描目录之外；见 `scripts/check_api_style.py:54-70`） | 中 | B4 |
| A3-04 | 文档 / 门禁 | G4 / G11 | `docs/guides/graphics-backends.md:95` 声称 `check_include_hygiene.py` 的 `SEAM_HEADERS`/`SEAM_SOURCES` 钉住接缝文件集；`grep -n 'SEAM' scripts/check_include_hygiene.py` 无输出；ADR-2026-11-16 §批次 3 也写了同一句 | 文档描述了一条不会触发的门禁；接缝文件集（今天 8 个机制头）无守卫，加文件不会被拒 | 中 | B2（删句）→ B3（补门禁或明确不做） |
| A3-05 | 文档 / 归属 | G1 / G12 | `.ai/design/vsg-backend.md:58/62/143/144` 的 `api::` / `core::` / `support::` 分组；AST：`vn::backend_vsg` 根 48 类型 + 60 函数，`api` 6 类型 + 2 函数，`core` 40 类型，`support` 无命名空间；宏用量 `VN_BACKEND_VSG_NS_BEGIN` 103 vs `VN_BACKEND_VSG_CORE_NS_BEGIN` 36 | 设计页的命名空间布局与代码相反，读者会去 `api::` 找不存在的类型 | 中 | B2 |
| A3-06 | 命名空间（ADL/前瞻） | G1 / G5 | `src/plugins/gfx_backend_vsg/include/vine/backend_vsg/backend_vsg_global.hpp:37`：`namespace core { using namespace ::VN_ROOT_NS::graphics; }` | `core` 内所有 `vn::graphics` 名字无限定可见；按 [basic.lookup.argdep]，任何 `core::X` 实参的 ADL 也会考虑 `vn::graphics` ⇒ SDK 新增一个自由函数可能悄悄改变插件内的查找/二义 | 中低 | B6（或只在文档写清） |
| A3-07 | 词汇（旧名残留） | G12 | `src/viz/graphics/shaders/vine_material.glsl:20` 写 `ContentSets`；`vine_params.glsl:26` 写 `ContentPush.hpp`（旧头名，文件已不存在）<!-- drift-ok -->；`src/plugins/gfx_backend_vk/include/.../ContentAbi.hpp:94` `detail::ContentPushConstants`（B17 已把 `ContentPush` 改名 `PushRange`） | 旧词汇出现在**发布出去的 shader 源**与 vk 内部类型里，读者会去找不存在的头/概念 | 低 | B2（注释）+ B5（类型名，若决定） |
| A3-08 | 文档（用户面） | G4 | `docs/guides/graphics.md:67/69`（`RenderCommand::modelMatrix`/`authoredState`）、`:928`（`SceneView::layoutParticipant()`）、`:595`（`RenderEngine::validateWiring()`）；实际 `model_matrix`/`authored_state`（`RenderCommand.hpp:127/182`）、`addSurfaceLayout`（`SceneView.hpp:202`）、无 `validateWiring`；计量：520 个代码标识符中 **22 个**在 `src/`+`tests/` 找不到 | 用户照抄不编译；`check_doc_symbols.py` 只守 `docs/guides/graphics-backends.md`（1 页 / 138 unit），`graphics.md` 无守卫 | 中 | B2，并扩 B3 的守卫 |
| A3-09 | 文档（设计页自相矛盾） | G1 / G11 | `.ai/design/graphics-frame-pipeline.md:156-157` 写 F1/F2 "部分：生产收集仍读 `SceneBake` / 只有测试消费者"；同页 `:611` 写"快照已是生产收集（5b-1/5b-2a）"；代码 `ScenePublisher::collectRenderCommands` 注释 "no second walk over the live rows" | 同一页两个事实；按 §3.1 读会低估已落地程度 | 中低 | B2 |
| A3-10 | 文档（计数/日期） | G11 | `graphics-sessions.md:3` 状态"T48 进行中"，而 §9.1/§9.3 与两份 ADR 均"全部落地"；`vk-backend.md` 头"最后核对 2026-11-15"却有 11-16/17 内容；`:737` 对拍 **12 例**（实为 13）；`:235` **58 个 code**（实测 `vk.*` **65**）；`:758` 写 `test_vsg` 571 / `test_graphics` 614（今天 572/617）；`graphics-architecture.md:94` 像素格式行列 `toVkFormat`（实际 `ContentImages.cpp` 的 `formatOf`） | 读者拿到的"今天"是过去某一天 | 低（逐条低） | B2 |
| A3-11 | 行为（默认后端） | G1 / G4 | `SurfaceWindow.cpp:448` `entries.front().factory->create()`；`RenderBackendRegistry.cpp:94` 遍历 map；`RenderBackendRegistry.hpp:154` "ordered by name"；`vk-backend.md:47` vs `graphics-backends.md:16` 对"生产后端"说法相反；demo 日志显示实际跑 vk | 默认后端由字母序决定：新增 `gfx_backend_d3d12` 会静默成为宿主默认；ADR 的切换门被这条 fallback 绕过 | 中 | B7（显式默认）或至少 B2 统一措辞 |
| A3-12 | 诊断（一致性） | G6 / G4 | vk `TargetTable.cpp:358` `vk.target.size_unknown`、`:306` `vk.target.borrow_extent_mismatch`；vsg 同类条件只有 `vsg.target.build_failed`（`VsgBackend.cpp:2259` 起的窄化分支不含 extent/size）；vsg 的 extent 检查在 resize（`TargetObjectsTest.cpp:1252` `ARePointIsRefusedWhenTheLenderShrankBelowTheBorrower`） | 同一角色、同一拒绝、不同精度；没有文档说明是否"有意" | 低 | B3（登记为有意差异或补 code） |
| A3-13 | 证据（validation） | G4 / G11 | `timeout 25 ./Vine --open demo` → `[warning]` **3**：2 条 `fragment writes Location 0 ... no pColorAttachments[0]`（已知）+ 1 条新 `Vertex attribute at location 1 not consumed by vertex shader`；`vk-backend.md` §1.2 R1/§8 T5 声称 validation 静默 | "干净"与"能跑"的分界被打破一条；新的属性告警没有记录 | 中低 | B8 |
| A3-14 | 证据（成本） | G10 | 无当前测试/门禁证明"整帧约 97 次分配/帧"（该数只在会话记忆与提交正文里）；现有证据：`CoreAllocationGateTest.ASteadyWindowGrowsTheHeapByNothing`、`…TheRecordPathsBookkeepingCostsAFewSmallVectorsPerPass`、`CoreStreamsTest.ASteadyFrameAllocatesNothing`、`VkFrameHeadTest.ASteadyFrameDoesNotGrowTheHeadsStorage`（只证明头部存储） | 成本句无法复核；读者无法区分"97"与"头部已归零" | 中低 | E2 → B9（测量）或删句 |
| A3-15 | 测试形状 | G11 | `tests/test_graphics/GraphicsTest.cpp` **15,158 行**；`tests/BackendSeamTest.cpp` 编进三个二进制（`tests/{test_graphics,test_vk,test_vsg}/CMakeLists.txt`），重复用例名 26（graphics∩vsg）/ 29（vsg∩vk）——刻意；每个测试层测的东西总体正确 | 单文件维护成本；seam 测试 3× 运行（有意，见 §4 末注释） | 低 | 登记 |
| A3-16 | 能力缺口（无消费者） | G8 | `OwnedFrame`（`OwnedFrame.hpp:125`）生产消费者 0（`grep -rn 'OwnedFrame' src` 仅自身头）；backlog `T18` 已如此记录 | 公开类型无消费者；维持现状即可 | 低 | 登记（不改） |
| A3-17 | 词汇（改名候选） | G4 | `EngineNotReady`（`EngineNotReady.hpp:23`）是**原因**枚举（`NoDevice`/`NotInitialized`），却按状态命名，使用点 `EngineNotReady reason{…}`（`PrepareReport.hpp:53`），22 处引用 | 名字读起来像 bool/状态；改成 `EngineNotReadyReason` 才是"名字说领域" | 低 | 公开 API 批次（B4/B5，单独） |
| A3-18 | 正确性（所有权拼写） | G4 / G10 | 8 处 `const_cast` 用于把 `const T*` 组回可变 owner：`Scene.cpp:46`、`RenderDevice.cpp:13`、`DrawListBake.cpp:247`、`RayIntersection.cpp:162`、`VsgBackend.cpp:102`、`FrameRecorder.cpp:239`、`VsgBufferView.hpp:85/95` | `intrusive_ptr<const T>` 的"不可变共享"承诺在这些路径上被绕过；今天每个点都指向可变对象，但这是一条靠调用者纪律的约定 | 低 | 登记；逐点决定（B9 可选） |
| A3-19 | 归属（detail） | G1 | `RenderPass.hpp:506` `struct VN_GRAPHICS_API detail::PassNode`；消费者：`SessionShape.cpp:225/257/381`、`EngineCompile.cpp:356/503`、`PassTimingTest.cpp:58`；`vn::graphics::detail` 其余 12 个都是前向声明 | 导出但住在 `detail`：DLL 拆分要求导出，名字表达"非宿主入口"——**结论：保留，无缺陷** | — | — |
| A3-20 | 命名空间（导出未点名） | G1 / G4 | 导出但公开文档未点名的"内部味"函数：`keyAuditTable()`（`DrawState.hpp:349`；消费者 `DrawState.cpp` + `BackendSeamTest.cpp`）、`refusalName()`（`Readback.hpp`；`Readback.cpp` + `BackendSeamTest.cpp`）、`planCodeText()`（`Plan.hpp:234`；3 src + 1 test）、`renderApiToString()`（`RenderBackendRegistry.hpp`；2 src + 1 test）；计量：`docs/guides` 只点名 25/59 根函数、6/32 别名、2/16 常量 | 事实上的 API 里有 4 个只为模块自身 + 接缝测试存在；G8/G4 上像"意外 API" | 低 | B5（降级或文档化，公开 API 单独批） |
| A3-21 | 命名空间（宏） | G6 | 55 个可见 `VN_*`（20 函数式），无 `#undef`；`VN_ENABLE_ENUM_FLAGS` / `VN_DISABLE_COPY*` / `VN_D`/`VN_V` 可换成模板/基类/成员；全部 `VN_` 前缀，未见系统/宿主撞名 | 见 §5；低价值 churn，登记 | 低 | 登记（G8） |
| A3-22 | 接缝（无缺陷） | G6 | 8 个机制头在 SDK 根：`FrameGate`/`DeviceClock`/`FrameTimeline`/`RetirementQueue`/`Readback`/`DrawState`/`TargetPlan`/`DiagnosticDraft`；直接 include：7 个两边都用，`FrameTimeline` 只 vsg 直接 include（vk 经 `DeviceClock` 组合） | 与 ADR 的判定一致；"接缝 = 两个后端都用"成立 | — | — |
| A3-23 | 契约/ABI | G6 | `check_export_annotations.py` 0 finding；`detail::PassNode` 是唯一 `detail` 导出；`capabilities()`（`Features`/`Limits`）与 `RenderDiagnostic::origin` 均已落地（gate + `RenderEngineTest.AHostReportIsCountedApartFromTheEnginesOwnFindings`） | 导出面受门禁覆盖 | — | — |
| A3-24 | 文档（两后端定位） | G4 | 同 A3-11；另 `graphics-backends.md:16` 与 `vk-backend.md:47` 冲突 | 读者无法判断"今天到底谁上线" | 中低 | B2/B7 |

### 3.1 逐维结论（含"未发现问题"）

| 维度 | 结论 | 证据 |
|---|---|---|
| 1 正确性与生命周期 | 一处真实 UB（A3-01）；A3-18 是所有权拼写约定而非缺陷；**未发现**裸 `new`/`delete` 配对错误、`strcpy`/`sprintf`/`memset`、公开头 `static` 全局；租约（`FrameGate::Shared/Exclusive`）都是 RAII；ASan + LeakSanitizer（vk 范围）今天 **PASS** | `grep` 计数（`delete`=0 等）；`VN_ASAN_TARGET=test_vk VN_ASAN_LEAKS=1 VN_ASAN_LEAK_SCOPE='vn::backend_vk' scripts/asan_check.sh` → `RESULT: PASS`；`FrameGate.hpp` |
| 2 并发 | 帧闸两侧 + 每闸每线程嵌套栈 + 重入拒绝的设计完整，两个后端各有 `*.device.reentry` code；**唯一真实竞争 = A3-01**；TSan 三套今天 `0 / 0 / 1` | `FrameGate.hpp`；`diagnostic_codes.txt:66/110`；§2 TSan 行 |
| 3 契约诚实 | 头注释抽查（`FrameGate` / `DiagnosticDraft` / `Readback` / `Plan` / `PassNode`）与实现一致；**不一致集中在文档页**（A3-04/05/08/09/10） | 各节 file:line |
| 4 分层与归属（G1/G2/G5/G13） | SDK/接缝/插件/引擎边界成立：`check_include_hygiene` + `check_vk_exposure` 0 finding；插件头无 `install()` 规则；插件内部类型与 SDK 类型不互 include；`detail` 只有 1 个导出（A3-19）；**未发现回边或跨层捷径** | 九门禁；`src/viz/graphics/CMakeLists.txt` 注释与 include 规则；§4 |
| 5 命名与词汇（G1/G4/G12） | 3 个需要动的词（A3-07 旧名、A3-17 状态名、A3-20 内部味导出）；**未发现新的"一概念多名"**；改名都有消费者证据或"无消费者只登记" | §4.2/4.6 |
| 6 抽象与克制（G7/G8/G9） | 扩展点成本表 13 个路径**全部存在**，1 个函数名过时（A3-10）；`OwnedFrame` 零消费者（A3-16，已记 backlog）；**未发现无消费者的新扩展点** | `for f in …; [ -f ]`；`grep OwnedFrame` |
| 7 真实成本（G10） | 分配句无当前测量（A3-14）；性能句未复核（E5）；但成本证人（ring/set/池/存储/锁计数）都有可读数字与用例；**没有发现"无证人的优化"** | `VkFrameHeadTest`、`CoreAllocationGateTest`、`VkMemoryPoolTest` |
| 8 证据覆盖 | 见 §6；缺口集中在平台（Win32/Wayland）、S6、第 5 块面、97 分配、真实 GPU 计时 | §6 |
| 9 两后端一致（G6） | 同角色类型名已对齐（`FrameHead`/`TargetTable`/`TargetObjects`/`WindowTarget`/`LightSet`）；有意差异（内存池、管线缓存、WSI 所有权）在用户页有理由；**未说明的差异只有 A3-12（code 精度）** | §4/§6；`docs/guides/graphics-backends.md` §3 |
| 10 诊断与失败面 | 124 code 双向对齐、baseline 空、两个后端无 `default` switch、`DiagnosticOrigin` 落地、`ReportOnce` 有测试（`ReportOnceTest`、`VkContentRefusalTest`）；读回用拒绝值，未发现异常逃逸 | §2；`scripts/diagnostic_codes.txt`；`RenderDiagnostic.hpp` |
| 11 用户文档真实性 | `docs/guides/render-views.md` 45/45 标识符存在；`docs/guides/graphics-backends.md` 3/170 缺失（2 真 + 1 是 ctest 名）；`docs/guides/graphics.md` 22/520 缺失，其中 4 个是 API 名（A3-08） | §2 的标识符脚本 |
| 12 测试形状 | 设备无关 / 后端单元 / 逐字节对拍三层测各自该测的；seam 测试 3× 编译是有意（`BackendSeamTest.cpp` 注释）；`GraphicsTest.cpp` 15k 行是维护性风险 | `tests/*/CMakeLists.txt`；A3-15 |
| 13 命名空间污染 | 安装头里无 `using namespace`、无 `static` 全局、无匿名命名空间；55 个宏全部 `VN_` 前缀；`vn::graphics` 根命名空间级 318 个声明（§4）；旧名残留 6 处（§4.6） | §4/§5 |

---

## 4. 根命名空间清单

**方法（可复核）**：对公开头/插件头各建一个 TU，用
`clang++-23 -Xclang -ast-dump -Xclang -ast-dump-filter=vn`（`-DVN_ROOT_NS=vn`，include 路径取
`build/compile_commands.json`）导出 AST 文本，再用脚本只统计**直接父节点是 `NamespaceDecl`** 的
声明（函数内的局部 `VarDecl` 已排除；AST 文本里带 `|-`/`` `- `` 前缀，深度 = 前缀宽度/2+1）。
SDK TU 收录全部 96 个 `sdk/vine/graphics/*.hpp`；插件 TU 分别收录 `include/vine/backend_vk`（27 头）
与 `include/vine/backend_vsg`（76 头）。Dump 在 `/tmp`，不进仓库。

### 4.1 总表

| 命名空间 | 命名空间级直接声明（实测） | 导出？ | 今天谁在包含/使用 | 判决 |
|---|---|---|---|---|
| `vn` | 26：2 自由函数、7 函数模板、7 类型、1 枚举、1 别名、5 类模板、3 concept | 是（base） | 全仓基础词汇 | 不属 graphics；登记（§9） |
| `vn::graphics` | **318**（含 1 条 `inline` 解析伪名，实为 317）：58 自由函数、1 函数模板、16 常量、32 别名、167 类型、42 枚举、1 类模板 | 是（188 标注） | 宿主 + 两个后端 | 保留（下 4.2/4.3） |
| `vn::graphics::detail` | 13（12 前向声明 + `PassNode`） | `PassNode` 是 | 引擎实现 + 1 测试 | 保留（A3-19） |
| `vn::math` | 128：1 自由函数、24 函数模板、73 别名、13 变量、2 类型、9 类模板、6 concept | 是 | base 层 | 不属 graphics；登记 |
| `vn::imaging` | 7：5 函数、1 类型、1 枚举 | 是 | base 层 | 不属 graphics |
| `vn::backend_vk` | **60**：20 自由函数、28 类型、6 枚举、3 别名、3 常量 | 插件导出宏 | 插件内部 + `test_vk` + 对拍静态编译 | 保留（4.4） |
| `vn::backend_vk::detail` | 15：4 函数、10 常量/变量、`ContentPushConstants`（含局部名噪声时 18） | 否 | vk 内部 | 保留；`ContentPushConstants` 见 A3-07 |
| `vn::backend_vsg` | **120**：60 自由函数、48 类型、9 枚举、1 别名、1 常量 | 插件导出宏 | 插件内部 + `test_vsg` | 保留（4.5） |
| `vn::backend_vsg::core` | **70**：14 函数、1 函数模板、40 类型、5 枚举、5 常量、3 别名、1 类模板、1 using-directive | 是 | vsg 内部 | 4.5 + A3-06 |
| `vn::backend_vsg::api` | 8：6 类型 + 2 探测函数 | 是 | vsg 内部 | 保留 |
| `vn::backend_vsg::detail` | 90：55 函数、10 类型、17 变量、3 枚举、2 别名、1 类模板、1 函数模板（集中在 `support/` 头） | 否 | vsg 支撑层 | 保留 |
| 旧名 `namespace vn::vsg` | **0** | — | — | 已消失 |
| `namespace vsg`（vsg 插件头） | 若干**前向声明**（`VsgFwd.hpp:24`、`internal/*.hpp`） | 否 | 避免重量级 include | 合法，不是旧名残留 |
| 文档里的 `api::` | `.ai/design/vsg-backend.md:58/62/143/144` | — | — | 与 AST 不符（A3-05） |

### 4.2 `vn::graphics` 根自由函数（58 + 1 模板）——按角色分组，逐个回答四问

**四问模板**：① ADL 会不会从 `vn::graphics` 类型上找到？ ② 名字是否过于通用？ ③ 是否有更合适的归属？
④ 导出但未在公开文档点名？

| 组 | 名字 | ①ADL | ②通用? | ③归属 | ④文档点名 | 判决 |
|---|---|---|---|---|---|---|
| 程序工厂（10） | `forwardProgram` `unlitForwardProgram` `flatForwardProgram` `normalsForwardProgram` `gbufferGeometryProgram` `deferredLightProgram` `shadowedDeferredLightProgram` `screenCopyProgram` `skyboxProgram` `fullscreenVertexProgram` | 会 | 否（领域） | 可作 `BuiltinShaders` 的静态成员，但会造回边 | 部分 | 保留 |
| 预设/帧工厂（7） | `defaultGbufferTarget` `defaultGbufferGeometryProgram` `defaultDeferredLightProgram` `singlePassFrame` `addOffscreenToScreen` `directionalShadowMatrix` `spotShadowMatrix` | 会 | 否 | `ChainPresets` 的领域函数 | 部分 | 保留 |
| ABI 打包（6） | `packDrawBlock` `packMaterialBlock` `packLightBlock` `packedSlotOf` `packAttribute` `packIndices` | 会 | 否 | 设计固定为 SDK 填充面（C8） | 部分 | 保留 |
| 状态/计划（10） | `resolveRenderState` `collectRenderState` `effectiveRenderState` `effectiveMaterial` `effectiveProgram` `resolveDynamicState` `planTarget` `depthPlan` `validatePlan` `passHasEffect` | 会 | **是**（`validatePlan`/`planTarget`/`depthPlan`/`passHasEffect` 是通用动词） | 候选：进 `detail::` 或成为值类型成员（G13 的反面先例在 `vn::math` 已发生过） | 部分 | 保留（无撞名证据）；登记改名/搬家候选 |
| 读回（5） | `readbackOf` `colorReadbackOf` `depthReadbackOf` `decodeDepth` `refusalName` | 会 | 中（`refusalName` 通用） | 后端与用例在用；`refusalName` 只有模块+接缝测试 | 部分 | `refusalName` 候选降级（A3-20） |
| 几何（8） | `geometryFromShape` `depthViewOf` `pickTreeBuilder`（`inline`） `localToWorldForView` `applyCameraMirror` `attributeLocation`（inline） `instanceMatrixLocation`（inline） `isCanonicalAttributeLocation`（inline） | 会 | 中 | 领域算法 | 部分 | 保留 |
| 材质/透明（4） | `alphaModeSortsTransparent` `materialFrom` `materialItemBit`（inline） `materialItemOf`（inline） | 会 | 中（`materialFrom`） | 领域 | 部分 | 保留 |
| 判定/分类（4） | `isDrawingCall`（inline） `isTriangleTopology`（inline） `passStageOrder`（inline） `frameShapeOf`（inline） | 会 | **是**（`isDrawingCall`/`frameShapeOf`） | 候选成员化 | 部分 | 保留；登记 |
| 文本/审计（3） | `planCodeText` `renderApiToString` `keyAuditTable` | 会 | 中 | **只服务模块 + 接缝测试**（A3-20） | 否 | 候选降级 |
| 场景（1） | `sceneFromModel` | 会 | 否 | 领域 | 是 | 保留 |
| 模板（1） | `contentRefOf` | 会 | 否 | 唯一的 `ContentRef` 构造者（设计如此） | 否 | 保留 |

**结论**：每个根函数都至少有 1 个真实消费者（系统性扫描：`pickTreeBuilder` 只被同头默认实参使用，
其余都有 `.cpp` 或测试）。没有"零消费者"的导出函数。真正值得动的是 **A3-20 的 4 个"内部味"导出**
与 **A3-06/A3-07 的词汇**；`validatePlan`/`planTarget` 一类通用动词**今天没有撞名证据**，按 G8 只登记。

### 4.3 `vn::graphics` 根的变量与别名

- **常量 16**：`kAllLayers`、`kDefaultLayer`、`kInstanceMatrixColumns/Components`、`kLightKindEmpty`、
  `kMaterialAlphaModeShift`、`kMaterialFlag*`（5）、`kMaxLights`、`kMaxPlanPasses`、`kPickTreeLeafSize`、
  `kResidencyKindCount`、`kTextureSlotCount`。全部 `inline constexpr`、`k` 前缀，**无宏**；
  判决：保留（ABI 词汇）。
- **别名 32**：`*Ptr`（`NodePtr`/`ScenePtr`/`GeometryPtr`/…）与 id 别名（`NodeId`/`GeometryId`/`MaterialId`/
  `TextureId`/`ProgramId`/`InstanceId`）以及 `FrameDriverId`、`LightSetPtr`、`DiagnosticSink`、
  `ViewPlacementPtr`。判决：保留；`LightPtr`/`MaterialPtr`/`ShaderProgramPtr` 三个在多个头里重复声明
  （同一类型，可接受）。
- **没有**：`inline` 变量赋值、`static` 全局、匿名命名空间、`using namespace`、把 `vn::X` 引到全局的 `using`。

### 4.4 `vn::backend_vk` 根（60）

- 自由函数 20：`barrierBetween` `barrierFor` `buildTarget` `colorFormatOf` `compileStage` `contentRefusalCode`
  `depthAspectMask` `depthFormatOf` `destroyTarget` `ensureBuildableSamples` `largestSampleCount` `loadOpOf`
  `numberText` `requiresTransition` `resizeTarget` `stateOf` `storeOpOf` `transferReadState` `variantOf` `vkHandles`。
- 别名 3：`BatchId` `ContentRefusal`(=`DiagnosticDraft`) `FrameDriverId`(=`graphics::FrameDriverId`)；
  另有内部别名 `Key`/`Park`/`Target` 等落在 `detail`。
- 常量 3（+ `detail` 9）：`kFileName` `kFormatVersion` `kFrontFace` `kNoMemoryType` `kSampleCounts`
  `kSingleSubpassRenderPass`（AST 把两处归到父命名空间，实际语义都是常量）。
- 四问：ADL 只影响插件自身类型；名字多为领域（`barrierBetween`/`stateOf`）但 `variantOf`/`numberText`/
  `vkHandles` 偏通用；插件头**不安装**，宿主风险 0。判决：保留；`variantOf` 与 vsg 的同名实现
  是**有意的两份契约实现**（M8 的注释写明不能 include 彼此的插件头）。

### 4.5 `vn::backend_vsg` 根（120）/ `core`（70）/ `api`（8）/ `detail`（90）

- 根自由函数 60，几乎全是领域算法（`buildGeometryFacts` `buildProgramFacts` `planClearValues` 在 `core`；
  根上的 `orderProgramRows` `drawNeeds` `samplesToVk` `mapTopology` `makePipelineStateObjects` 等）。
- `core` 的 14 个函数 + 1 模板里有**通用名**：`copy` `acquire` `copyValue` `retireIf` `retireUnpinned`
  `forgetUnpinned` `visitValues` `perFrameCopies`；它们的实参是 `ResourceTable` 等 `core` 类型，
  ADL 在插件内部可见；宿主零风险（头不安装）。
- `core` 的 `using namespace ::vn::graphics`（A3-06）是唯一跨命名空间放大点。
- `support/` 头的 `detail` 90 个声明全是 vsg 适配（`VsgSceneRules` / `VsgBufferView` / `RenderStateMapper`），
  不进安装面。
- 判决：保留（G8）；只有 A3-05 的**文档**必须改。

### 4.6 旧名残留（grep 计数）

| 名字 | 代码残留 | 设计/研究页 | 判决 |
|---|---|---|---|
| `ContentHalves` | **0** | 仅历史叙述 | 已消失 |
| `ContentSets` | **1**（`vine_material.glsl:20` 注释） | 0 | A3-07，注释改 `BindingSets` |
| `ContentPush` | **5**（`vine_params.glsl:26` 注释 + vk `ContentPushConstants` 3 处） | 1（历史） | A3-07 |
| `ContentPushMember` | 0 | 0 | 已消失 |
| `refreshMaterials` | 0 | 1（迁移表） | 已消失 |
| `RenderSurface` | 0 | 1（迁移叙述） | 已消失 |
| `CollectView` | 0（`View.hpp:20` 注释提到旧名） | 0 | 已消失 |
| `FramePlan` / `PassPlan` | 0 | 1（迁移表） | 已消失 |
| `ContentPlan` | 0 | 1（S2b 前叫法） | 已消失 |
| `namespace vn::vsg` | 0 | 0 | 从未/已消失 |

---

## 5. 宏清单

**实测**：宿主 `#include` 全部 96 个公开头后可见 **55 个 `VN_*`**，其中 **20 个函数式**。
来源：`vn_global.hpp` 22 个、`core_global.hpp` 14 个、`graphics_global.hpp` 4 个，
其余来自 geom/math/imaging/models/window 的模块 global（经公开头传递包含）。
**全部带 `VN_` 前缀；仓库里没有一处 `#undef VN_`；未见与系统/宿主宏撞名。**

| 宏 | 定义处 | 安装头可见 | 能否 `inline constexpr` | 撞名风险 | 判决 |
|---|---|---|---|---|---|
| `VN_GRAPHICS_API` / `VN_BACKEND_VK_API` / `VN_BACKEND_VSG_API` | 各模块 global | 是 | 否（dllimport/export） | 低（前缀） | 保留 |
| `VN_EXPORT` / `VN_IMPORT` / `VN_GRAPHICS_LIB` | `vn_global.hpp` / graphics global | 是 | 否 | 低 | 保留 |
| `VN_GRAPHICS_NS_BEGIN/END`（+ geom/math/… 同族） | 各 global | 是 | 否（开括号） | 低 | 保留 |
| `VN_ROOT_NS` / `VN_ROOT_NS_BEGIN/END` | `vn_global.hpp` | 是 | 否 | 低 | 保留 |
| `VN_ARCH_64` / `VN_CC_CLANG` | `vn_global.hpp` | 是 | 否（编译事实） | 低 | 保留 |
| `VN_FORCE_INLINE` / `VN_NO_INLINE` | `vn_global.hpp` | 是 | 否（属性拼写） | 低 | 保留 |
| `VN_ENABLE_ENUM_FLAGS(Enum)` | `vn_global.hpp` | 是 | **可以**（`template<class E> constexpr E& operator\|=(E&, E)`，但会作用于所有枚举） | 低 | 登记；不建议为"像函数"而改（G8） |
| `VN_DISABLE_COPY/MOVE/COPY_MOVE` | `vn_global.hpp` | 是 | 可以换成基类 | 低 | 登记 |
| `VN_D` / `VN_V` | `core_global.hpp` | 是 | 否（语句宏） | 低 | 登记 |
| `VN_DECLARE_*` / `VN_DEFINE_*` / `VN_OBJECT_META*` / `VN_TMPL_OBJECT_META_IMPL` | `core_global.hpp` / `Object.hpp` | 是 | 否（生成成员） | 低 | 保留 |
| `VN_GEOM_*` / `VN_MATH_*` / `VN_IMAGING_*` / `VN_MODELS_*` / `VN_WINDOW_*` | 各自 global | 是（经传递包含） | 否（命名空间/导出） | 低 | 保留 |

**函数式宏的参数安全抽查**：`VN_D(ClassName)` 展开为 `auto* const d = getDPtr();`——吞掉参数
（`ClassName` 未用），但这是 PImpl 惯用法，且只在类内使用；`VN_DISABLE_COPY` 正确加了访问说明与
括号；没有发现缺括号/多次求值的宏。**结论**：宏面无需变更（A3-21）。

---

## 6. 能力表（能力 → 今天的证据）

> `CAP-A3-xx` 是本轮编号（不与重设计文档的 85 条 `CAP` 混用）。

| CAP | 能力 | 今天的证据（用例名 / 门禁） | 缺口 |
|---|---|---|---|
| CAP-A3-01 | 内容绘制（索引/非索引、点/线/三角） | `VkContentDrawTest`、`ContentDrawTest`、`PrimitiveDrawTest`、`AnUnindexedPointCloudDrawsThroughThePointsPipeline` | — |
| CAP-A3-02 | MRT、逐附件清屏、深度附件 | `VkContentTargetTest`、`MrtTargetTest`、`VsgBackendTest.EachAttachmentClearsToItsOwnColour` | — |
| CAP-A3-03 | MSAA + 深度 resolve | `VkMsaaTest`（5 例）、`MsaaTargetTest`、`TheSampleLimitsAgreeBetweenTheBackends`（对拍） | — |
| CAP-A3-04 | 借用深度（shareDepth） | `VkContentTargetTest`、`SharedDepthTest`（5 例）、`TargetObjectsTest.ARePointIsRefusedWhenTheLenderShrankBelowTheBorrower` | 拒绝 code 不对称（A3-12） |
| CAP-A3-05 | 材质/纹理/立方体贴图/白图兜底 | `VkContentDrawTest`、`ContentDrawTest`、`VsgBackendTest.ACubeMapInputReachesAContentProgramAsACubeSampler` | — |
| CAP-A3-06 | 灯（类型化数组 + 环境光） | `LightBlockTest`、`ContentDrawTest`、`ValueChannelTest.EveryPassOfOneFrameHoldsTheSameLightSetUntilALightMoves` | 逐对象选灯未做（backlog S7） |
| CAP-A3-07 | 方向光阴影 | `VkShadowTest`、`ShadowPixelTest`、对拍 `TheSameShadowMapIsTheSamePictureOnBothBackends` | 点光 cube 阴影未做（S6） |
| CAP-A3-08 | 延迟链 | `VkDeferredTest`、对拍 `TheSameDeferredFrameIsTheSamePictureOnBothBackends` | — |
| CAP-A3-09 | 全屏/screen draw + push 范围 | `ScreenDrawTest`、`VkContentDrawTest.AScreenCopyIsDrawnWhateverThePassDepthPolicySays` | — |
| CAP-A3-10 | 实例流 | `VkBuiltinProgramTest`、`ProgramVariantTest`、`GeometryFactsTest` | — |
| CAP-A3-11 | 变体（textured/vertex_color/cube/instanced） | `VkVariantTest`、`VariantTest`、`ProgramVariantTest` | 两份实现（vk/vsg 各自），由对拍+用例对齐 |
| CAP-A3-12 | 读回（颜色 RGBA8 / 纯深度） | `OffscreenClearTest`、`VkContentTargetTest`（3 例）、`VkMultiSessionTest` 读回 3 例、`HostWindowReadTest` | 区域读回未做（无消费者） |
| CAP-A3-13 | 窗口呈现（X11） | `VkWindowPresentTest`、`VkProbeTest`、vsg 窗口套件 | Win32 实机 / Wayland 实测缺失（backlog T12/T13） |
| CAP-A3-14 | 预热（build without submit） | `VkPrepareUploadTest`（2 例）、`PrepareUploadTest`（2 例）、`HeadlessBackendTest.PrepareHandsOverTheSameFrame…` | — |
| CAP-A3-15 | 驻留/释放/重建 | `VkResidencyTest`、`VkResidenceWireTest`（4 例）、`ResidencyTest`、`ContentStoreTest.*Release*`、`UploadScratchTest` | — |
| CAP-A3-16 | 多驱动者/共享设备并发 | `VkMultiSessionTest`（6+ 例）、`VkConcurrentFrameTest`、`VsgBackendTest.ACompilingFrame…/ARecordingFrame…/TwoDriversOnTwoThreads…` | TSan vk 1 报告（A3-01） |
| CAP-A3-17 | 管线缓存文件（仅 vk） | `VkPipelineCacheFileTest`（5）、`VkPipelineCacheSessionTest`（4） | vsg 无（backlog T10，有意） |
| CAP-A3-18 | 设备内存池（仅 vk） | `VkMemoryPoolTest`、`VkMemoryAllocatorTest`、`VkMemoryFailureTest` | vsg 交给 vsg，有意 |
| CAP-A3-19 | GPU 计时（帧 + pass） | `VkDeviceClockTest.AFrameReportsItsDeviceTime`、`VkPassTimingTest.TheReportAttributesTheFrameToThePassesThatSpentIt` | 只在 lavapipe 上量过；真实 GPU 读数未记录 |
| CAP-A3-20 | 线框/点大小/线宽/各向异性 | `AWireframeModeDrawsTheEdgesOfTheSameGeometry`、`APointSize…`、`ALineWidth…`、`AnisotropyIsClamped…`、`TheViewSwitchesAreServedInsteadOfRefused` | 按设备能力 |
| CAP-A3-21 | 诊断 code + 来源 | `check_diagnostic_formats`（124 code 双向）、`RenderEngineTest.AHostReportIsCountedApartFromTheEnginesOwnFindings`、vk/vsg 无 `default` switch | vsg 的 extent 精度差异（A3-12） |
| CAP-A3-22 | 暴露边界 / 内容接缝 | ctest `vk_exposure`、`content_seam`（2/2 passed） | 接缝文件集无门禁（A3-04） |
| CAP-A3-23 | 宿主路径（demo） | `timeout 25 ./Vine --open demo`：session up + Pending→Presenting、0 `[error]` | 3 条 validation warning（A3-13） |
| CAP-A3-24 | 第 5 块 vsg 渲染面 | 无守卫（`VSG_MAX_DEVICES` = 4，设计页 §2.11/§9.2 已记） | 未做（无消费者） |

---

## 7. 风险与不确定项（每条给最小实验）

| # | 结论 | 需要的最小实验 | 成本 |
|---|---|---|---|
| E1 | A3-01 是唯一真实竞争 | 修 `fresh`（`std::atomic<bool>`，或只在门内读）后重跑 `VN_TSAN_TARGET=test_vk scripts/tsan_check.sh`；期望 0 warning、`test_vk` 167 绿 | 1 个提交 |
| E2 | "整帧约 97 次分配"无当前证据 | 用 `tests/test_vsg/AllocationCounter.cpp` 的同一装置在 `VN_BENCH_FRAMES` 的 demo 帧外包一层计数（或给 vk 帧加计数入口），打印一次；有数就写进设计页，没有就删句 | 半天 |
| E3 | vsg 的 extent 拒绝只在 resize 不在 build | 变异：borrower 32×32、lender 16×16，直接跑一帧；观察是否 0 VUID（若 vsg 的 framebuffer extent 取 max 则合法，若取 borrower 则应为 VUID） | 1 条测试 |
| E4 | 用户文档 22/520 标识符缺失 | 把 `graphics.md` 加进 `check_doc_symbols.py` 的覆盖（或只查"API 形"名字：`Class::member`），先 `--list` 出 baseline，再清 4 条 | 1 个提交 |
| E5 | 性能句（8.63–8.94 ms / −10%）不可复核 | `VN_BENCH_WARMUP=1 VN_BENCH_FRAMES=48 ./Vine --open demo`（lavapipe）跑一次，记录 median；把命令与日期写回 `vk-backend.md` §2.3/§2.4 | 一次运行 |
| E6 | 默认后端由字母序决定 | 已由代码读取证明（`entries()` 遍历 map）。若要行为证明：注册一个名字排序在 `vk` 之前的假 backend，跑 `SurfaceWindow::useDefaultBackend`，断言它被选中 | 1 条测试 |
| E7 | `FrameGate` 的每线程条目上限是 8 | 已持有 8 个闸后再进第 9 个：`enterShared` 返回空租约；用例证明这是"拒绝"而不是"死锁"（今天的嵌套只有 2–4 层，够用） | 1 条测试（低优先） |
| E8 | 两个后端对"同一拒绝"的 code 覆盖 | 对照表：`vk.target.*` 10 vs `vsg.target.*` 5；逐个条件问"vsg 是否也有且静默"；产出"有意差异"清单写进 `docs/guides/graphics-backends.md` | 半天 |

---

## 8. 批次计划（每批一个可回滚提交）

> 原则：**改名/搬家不混批**；**公开 API 单独一批**；每批给出验收命令。
> **B1** 是唯一的并发/正确性修复；**B7/B8** 是行为与验证改动；其余按“文档 → 门禁 → 公开 API → 宿主策略 → 验证”排序。

| 批 | 内容 | 风险 | 验收 |
|---|---|---|---|
| **B1** | 修 A3-01：`TargetObjects::fresh` 改 `std::atomic<bool>`（或把相位一读取挪到门内；二选一并写注释说明为什么相位一可以答 `NotReady`） | 低（一处并发语义） | `cmake --build build --target test_vk && ./build/bin/test_vk`；`VN_TSAN_TARGET=test_vk scripts/tsan_check.sh` = 0；`test_graphics`/`test_vsg` 不变 |
| **B2** | 文档真话清扫（零代码）：A3-04（删 `SEAM_HEADERS` 句或改成"无文件集门禁"）、A3-05（vsg 命名空间表按 AST 重写）、A3-07（shader 注释）、A3-08（4 个 API 名）、A3-09（F1/F2 状态）、A3-10（日期/计数/`toVkFormat`）、A3-24（生产后端措辞统一） | 低 | `python3 scripts/check_ai_docs.py --strict && python3 scripts/check_design_docs.py && python3 scripts/check_doc_symbols.py && python3 scripts/check_public_docs.py` |
| **B3** | 守卫补齐（与 B2 不混）：给接缝文件集补一条门禁（`check_include_hygiene.py` 的集合，或 CTest 脚本），并加"多一个文件即红"的反向用例；同时把 `graphics.md` 的 API 名纳入 `check_doc_symbols`（先 `--list` 建 baseline，再逐条清） | 中（会新增红） | 门禁自身变异：向接缝集合加一个文件 ⇒ 红；删回 ⇒ 绿；`ctest -R 'content_seam\|vk_seam'` |
| **B4** | 公开 API（读回形状）：新增 `ReadbackOutcome` 返回值（`std::variant`/结构），旧 `bool + ReadbackResult*` 保留一版并标记迁移；清 `check_api_style` 的 4 条 `ReadbackResult` baseline 与 7 条死记录 | 中（公开 API） | 全三套 + `check_api_style` baseline 降到 1（`ChainBuilder` 前向声明那一条另议）+ 迁移说明 |
| **B5** | 公开 API（降级意外 API）：`keyAuditTable` / `refusalName` / `planCodeText` / `renderApiToString` 移入 `detail::`（或加 `@internal` 并写进 baseline）；`ContentPushConstants` 是否改名一并决定（A3-07） | 中（ABI 源码级） | `check_export_annotations`、`check_public_docs`、`test_graphics`/`test_vk`/`test_vsg`；`test_vsg` 的接缝测试同步 |
| **B6** | 命名空间收窄（可选）：把 `core` 的 `using namespace ::vn::graphics` 换成 8 个机制别名（`FrameGate`/`DeviceClock`/…），或保留并在 global 头写清 ADL 后果 | 中（插件内查找面） | `test_vsg` 全绿 + `content_seam`；无实例化/二义新增 |
| **B7** | 宿主默认后端显式化：`SurfaceWindow` 不再取 `front()`；改成 `DeviceDesc`/配置里的后端名（缺省仍可 vk，但规则说得出） | 中（宿主行为） | demo 0 `[error]`；新增"注册顺序不影响默认"用例 |
| **B8** | validation 收干净（A3-13）：深度-only pass 用无颜色输出的 fragment 变体；查掉 "location 1 not consumed"；demo 0 warning | 低–中 | `timeout 25 ./Vine --open demo`：`[warning]` = 0；`test_vk` 全绿 |
| **B9** | 成本测量（E2）+ 性能句复核（E5）；把数字或删句写回 `vk-backend.md` | 低 | 一次 bench 输出贴进提交正文 |
| **B10** | 可选：A3-17（`EngineNotReadyReason` 改名）、A3-18（const_cast 决策）——各自独立、各自可回滚 | 中（公开 API） | 每批单独跑三套 + 九门禁 |
| **B11** | 概念文档（零代码，C-A3-01/03/04/05/11/13/16）：译名表（Engine/Device、Renderer/Driver）、“假朋友”表（RenderPass/RenderCommand）、“帧值”表（Description/Table/Result/Outcome/Query）、目标角色表、“引擎是 frame compiler、不是 render graph”、`SceneView` 的 per-window controller 定义 | 低 | `check_ai_docs`/`check_design_docs`；读者能用一页对上 Filament/O3DE 词 |
| **B12** | 公开词汇统一（**默认不做**，需明确 go；每词单独提交）：`createSession`→`createDriver`、`DrawItem`/`DrawCall`、`MaterialValues`、`releaseResidency`、`EngineNotReadyReason` | 中–高（公开 API/源码破坏） | 每个词一条迁移说明；三套 + 九道门禁；baseline 递减 |
| **B13** | 插件内改名（C-A3-07/09）：`ProgramHalves`→`CompiledPrograms`、`ResourceTable`→`PinnedResources`、`ResidenceTable`→`ResidencyClaims`；同步 `tests/test_vsg/CMakeLists.txt`、`content_seam` 清单与对拍 | 低–中（插件内部） | 用例名不变；`test_vsg` 572、对拍 13、`content_seam`；九道门禁 |
| **B14** | 接缝身份（可选，C-A3-10）：`vn::graphics::backend` 别名组（8 个机制），或保持现状并靠 B3 的文件集门禁 | 中（命名空间查找面） | 三套全绿；无新增二义/实例化；若纯别名，ABI 不变 |

---

## 9. 不建议做（与理由）

| 项 | 理由 |
|---|---|
| 把 `vn::graphics` 根的 58 个函数"统一搬家"到 `detail::` | 它们有真实消费者（宿主/后端/测试）；没有撞名证据；纯 churn（G8）。只动 A3-20 的 4 个"内部味"导出 |
| 把 `EngineNotReady` 立即改名 | 22 处引用 + 公开 API；作为独立公开 API 批（B10）而非顺带 |
| 把 `VN_ENABLE_ENUM_FLAGS` / `VN_DISABLE_COPY` 改成模板/基类 | 会改变所有枚举/类的重载与继承语义；收益只是"少一个宏"（G8） |
| 合并 `ContentCaches`（vk）与 `ContentStore`+`ContentFacts`（vsg） | 两个后端的分工不同（一个是行+命令计划同住，一个是分开）；合并会抹掉有意的名字差异（`graphics-architecture.md` §2.12 已论证） |
| 重开 `SEAM_HEADERS` 作为"路径集合" | 目录/文件集守卫已被 ADR-2026-11-16-no-backend-half 删过；正确做法是**补一条说得出性质的检查**（B3），不是恢复旧集合 |
| 拆 `GraphicsTest.cpp`（15,158 行） | 只有维护性收益，没有消费者压力；等下一次大改顺带拆（G8） |
| 多帧在飞 / dynamic rendering / VMA / bindless / LOD / 点光阴影 / 退役 vsg | 各有 ADR 或 backlog 触发条件，今天未命中（G8） |
| 把 `ImageLayout` 立刻从接缝清掉 | 设计已把它列为 C6 的下一次推进；没有第二个 API 家族，先不碰 |
| 为"97 次分配"引入新分配器 | 先测量（B9）；没有数就没有优化目标（G10） |

---

## 10. 与上一轮报告的差异

### 10.1 已消失的旧发现

| 旧发现（出处） | 今天的证据 |
|---|---|
| 接缝 25 头 / 18 头只有 vsg 一个消费者（redesign §1.1） | `sdk/vine/graphics/backend/` **目录不存在**；8 个机制头在 SDK 根；`VineGraphicsBackendSdk` 组件/宏在仓库里 **0 命中** |
| 驱动身份三种拼写（`WindowId` u32 / 两个 `SessionId`） | `FrameDriverId` 唯一定义；`DeviceClock::WindowId` 是 `[[deprecated]]` 别名（`DeviceClock.hpp:65`） |
| 帧首三个计划类型（vk）vs 一个（vsg） | 两边都叫 `FrameHead` + `DrawPlan`（AST 可见 `vn::backend_vk::FrameHead`、`vn::backend_vsg::FrameHead`） |
| `ContentHalves` / `ContentSets` / `ContentPush` 命名 | 代码里 0 / 1（shader 注释）/ 5（shader 注释 + vk `ContentPushConstants`）；见 A3-07 |
| `refreshMaterials` / `RenderSurface` / `CollectView` / `FramePlan` / `PassPlan` | 代码 0 命中（只在迁移表/注释里） |
| 诊断无 code / baseline 非空（11-15 A3） | 124 code 双向对齐；`diagnostic_code_baseline.txt` 只剩注释；两个后端无 `default` switch |
| 分配失败吞设备丢失（11-15 A1） | `VkMemoryFailureTest.ALostDeviceDuringAnAllocationEndsTheSession` 在跑 |
| 11-15 的 A1–A14 | 除 A7（`RenderDevice` 18 动词，裁定"不触发"）与 A13（无设备 skip，按契约）外，全部落地 |

### 10.2 仍在的旧发现（今天的证据）

| 项 | 今天 |
|---|---|
| `OwnedFrame` 零生产消费者（backlog T18） | 仍只有 `OwnedFrameTest`（`GraphicsTest.cpp:7668`） |
| Win32 实机 / Wayland 实测（T12/T13） | 仍缺；`graphics-backends.md:16` 把 Win32 标为"待实现" |
| 点光 cube 阴影（S6） | 仍无；两后端都没有 |
| vsg 第 5 块渲染面无守卫（`VSG_MAX_DEVICES`） | 仍无（设计页 §2.11/§9.2 已记） |
| "整帧约 97 次分配" | 仍无当前测量（A3-14 / E2） |
| 接缝上仍有 `ImageLayout` | 只剩 `DrawState.hpp`（grep 确认） |
| 对拍 13 例 | 仍 13（文档 `vk-backend.md:737` 还写 12） |

### 10.3 本轮新出现（旧报告没有的）

- **A3-01** `TargetObjects::fresh` 数据竞争（TSan 今天才红；11-16 的记忆写 0 报告）。
- **A3-11** 宿主默认后端 = 名字序 `entries().front()`（重设计文档只讨论过注册表覆盖，没有讨论顺序）。
- **A3-03** 读回 `S-10` 试点未落地（第二份 ADR 的决定 5 只在文字里）。
- **A3-04/A3-08** 两条"文档说门禁做了 X，实际没有"（`SEAM_HEADERS`、`graphics.md` 未被符号门禁覆盖）。
- **A3-05** vsg 命名空间布局与 AST 不符。
- **A3-14** 成本句仍不可复核（旧报告把它标为"已知未做"，本轮确认**今天也没有测量装置接线**）。

---

## 11. 概念与语义再审（对照成熟引擎）

> 本章回答的是"这些概念本身合不合理"，不是"实现对不对"。判据是 `G1`（一个概念一个定义/归属/边界）、
> `G4`（名字、参数、返回、所有权、失败方式说到做到）、`G12`（名字说出领域），外加一条读者判据：
> **来自另一个引擎的人能不能一步把概念对上**。
>
> 对照对象只引用**公开文档里的形状**（Filament、Unreal、O3DE RPI/RHI、wgpu/WebGPU、bgfx、Godot、
> VulkanSceneGraph、Qt Quick），它们不是本仓库可复核的事实；本仓库自己的对照表在
> `.ai/research/graphics-2026-11-16-redesign-r2.md` §8，本章是**落地之后**的重判，不重复那份计划。
>
> 计量（AST，命名空间级）：`vn::graphics` 共 **241 个类型/枚举**；按前缀 `Frame*` 17、`Render*` 17、
> `Pass*` 10、`Readback*` 7、`Material*` 7、`Target*` 6、`Light*`/`Plan*`/`Scene*` 5、`View*`/`Residency*` 4、
> `Draw*`/`Present*` 3。

### 11.1 概念地图（今天是什么 ↔ 别人叫什么）

| 概念 | 今天的一句话（证据） | 成熟引擎的对应 | 关系 |
|---|---|---|---|
| 设备 | `RenderDevice` = GPU 设备 + 队列 + 资源缓存；**创建者** initialize/shutdown，引擎**借用**（`RenderDevice.hpp:153`） | Filament `Engine`；O3DE `RHI::Device`；wgpu `Device`+`Queue` | 同形 |
| 引擎 | `RenderEngine` = 一扇窗口的帧循环 + 会话形状 + 表面账（`graphics-architecture.md` §2.8） | Filament `Renderer`+`SwapChain`+`View`；O3DE `RPI::Scene`+`View` | **词反了**：别人的 Engine 是设备所有者 |
| 区 | `RenderArea` = 一扇窗口要画的那块面 + 呈递声明（`RenderArea.hpp` 类注） | Godot `Viewport`；swapchain+surface 绑定 | 同形 |
| 视图值 | `View` = 相机值 + 设备像素视口 + 层掩码（`View.hpp` 类注） | Filament `View` 的每帧快照；UE `FSceneView` 的值半边 | 同形 |
| 视图对象 | `SceneView` = 相机 + 内容场景 + 操纵器 + 采纳的链 + 布局回调（`SceneView.hpp` 类注） | Filament `View`（相机+场景）；UE `FSceneViewFamily` 的宿主侧 | 同形，名字只说了一半 |
| 场景 | `Scene` = 作者树 + 灯 + 发布；发布是**不可变快照**（`graphics-threading.md` §2.2） | Filament `Scene`；UE `UWorld` → `FScene`；Godot 节点 → `RenderingServer` | 同形（值化更强） |
| pass 声明 | `PassDesc` 是唯一作者形状；`RenderPass` 是只读活句柄（`RenderPass.hpp` 类注） | O3DE `PassDescriptor`；Filament `RenderPass::Builder`；wgpu 不可变管线描述 | 同形 |
| 编译表 | `Plan` → `PassRow`/`FrameTable` → `FrameDescription` | UE `FRDGPass` + 参数；O3DE `Pass` 编译结果 | 同形（名字偏多，见 11.3） |
| 链 | `PassChain` = 配方 + 注册归属 + overlay 更新（`PassChain.hpp`） | O3DE `PassTemplate`；Filament 无对应（应用自己搭） | Vine 特有，合理 |
| 驱动者 | `RenderFrameExecutor`（类型）+ `createSession()`（动词）+ 文档里的 "driver"（`RenderDevice.hpp:239-250`、`RenderFrameExecutor.hpp:61-69`） | Filament `Renderer`（每线程一个）；O3DE `RHI::CommandList`+`Queue`；wgpu `CommandEncoder`+`Queue` | **一概念三名** |
| 目标 | `RenderTarget`（宿主对象）/ `Target`（pass 声明）/ `TargetShape`+`TargetDesc`（兼容性）/ `TargetInstance`+`TargetDecision`（计划）/ `TargetObjects`（GPU）/ `TargetTable`（表）/ `TargetLease`（租约） | wgpu `Texture`+`TextureView`+`RenderPassColorAttachment`；O3DE `RHI::FrameBuffer`+`PassAttachment`；Filament `RenderTarget` | 角色分得清，**名字数量偏多** |
| 产出 | `Output` = 名字 + 至多一个覆盖承诺（`Output.hpp:33`） | O3DE `PassAttachment` 的名字；wgpu 无跨 pass 名字 | Vine 特有，合理 |
| 一条绘制 | `RenderCommand` = 一个 drawable 的一次绘制（`RenderCommand.hpp:28`）；`DrawDescription` = 一个 pass 的一次绘制调用（`FrameDescription.hpp:101`） | O3DE `DrawItem`（逐对象）vs `DrawPacket`（逐 pass）；UE `FMeshBatch`（逐 pass）vs `FMeshDrawCommand`（逐 draw） | **层级与业界相反** |
| 内容 | `ContentRef`/`ContentRegistry`（公开）；`ContentStore`/`ContentCaches`/`ContentDraw`/`ContentImages`/`ContentPipeline`/`ContentPass`/`ContentAssembly`/`ContentFacts`/`ContentSources`/`ContentAbi`（实现） | Filament 资源对象 + `ResourceManager`；O3DE RPI 资源 + RHI 池 | 前缀不总是说出角色（§11.3） |
| 驻留 | `Residency*` 报告；vsg `ResourceTable`（pin）；vk `ResidenceTable`（谁可以建） | D3D12 residency manager（报告）；wgpu 内部引用；O3DE RHI 池 | 两问两表合理，**名字太近** |
| 诊断 | code + subject + limit + `DiagnosticOrigin` + 一次情节（`RenderDiagnostic.hpp`） | D3D12 info queue；Vulkan debug utils | **强于多数** |
| 读回 | `ReadbackRequest/Kind/State/Answer/Format` + `ReadbackRefusal`（seam）+ `ReadbackResult`（宿主）+ 后端 `HostReadbackRefusal`/`StagingRefusal` | wgpu map-async 一个状态 | **两套 SDK 枚举重叠** |
| 发布 | `SceneSnapshotRef` + `PublicationStore` + `SnapshotSource` | UE `FScene`（渲染线程代理）；Godot `RenderingServer`；Qt Quick `sync()` | 同形/更强（无每帧 sync） |

### 11.2 概念判定（C-A3-xx）

| ID | 概念/语义问题 | 证据 | 对照 | 判定 | G# | 批次 |
|---|---|---|---|---|---|---|
| C-A3-01 | **Engine/Device 的词与业界相反**：Filament/UE/O3DE 的 "Engine/Device" 是设备所有者，Vine 的 `RenderEngine` 是一扇窗口的帧循环 | `RenderDevice.hpp:153`（设备所有者）；`graphics-architecture.md` §2.8（一区一引擎） | Filament `Engine`→`Renderer`；O3DE `RHI::Device`→`RPI::Scene` | **名字保留（公开 API/G8）**；用户文档补"假朋友"翻译表（C-A3-11） | G1/G12 | B11 |
| C-A3-02 | **一概念三名**：`createSession()` 返回 `RenderFrameExecutor`，文档称 "driver" | `RenderDevice.hpp:250`；`RenderFrameExecutor.hpp:71`；`docs/guides/graphics-backends.md` 共同契约表 | Filament `createRenderer`（动词与类型同词） | 文档先统一为 **frame driver**；API 改名留 B12（需明确 go） | G1/G4 | B11 + B12 |
| C-A3-03 | **一条绘制/一次调用的命名与业界相反**：`RenderCommand` 是逐 drawable 的项，`DrawDescription` 是逐 pass 的调用 | `RenderCommand.hpp:28`；`FrameDescription.hpp:101` | O3DE DrawItem/DrawPacket；UE FMeshBatch/FMeshDrawCommand | **建议改名** `DrawItem`/`DrawCall`（未来公开 API 批，B12）；今天只登记 | G1/G12 | B12 |
| C-A3-04 | **五个"帧值"**：`FrameDescription`（后端输入）/`FrameTable`（编译行）/`FrameResult`（已答帧）/`FrameOutcome`（这次调用）/`FrameQuery`（那一帧的世界） | `FrameDescription.hpp:78`；`FrameTable.hpp:84`；`FrameResult.hpp:13/34`；`RenderEngine.hpp:79` | UE `FSceneRenderer`/RDG/`FViewInfo`；无一一对应 | **合理**（各答不同问题，`FrameOutcome` 的 note 明确"不是第三种结局"）；补一页"帧值表"（B11），不改名 | G1/G11 | B11 |
| C-A3-05 | **目标词 6+3 个**，且设计词 `TargetPlan` 与类型 `TargetDecision` 不一致 | `Target.hpp:72`；`TargetPlan.hpp:99/109/117`；`src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/TargetTable.hpp` | wgpu Texture/View/Attachment；O3DE FrameBuffer/PassAttachment | **概念合理**（宿主/声明/兼容/计划/GPU 五个角色）；公共文档补角色表，`TargetPlan`→`TargetDecision` 的措辞统一（B11） | G1/G4 | B11 |
| C-A3-06 | **读回有两套 SDK 拒绝枚举且语义重叠**：`ReadbackRefusal`（UnknownAttachment/UnreadableFormat/NotCaptured）与 `ReadbackResult`（Invalid/Unsupported/NotReady/Failed） | `Readback.hpp:59`；`RenderDevice.hpp:39`；后端各一套 | wgpu map-async 一个状态 | **应合并**：宿主面只留 `ReadbackResult`，`ReadbackRefusal` 降为 seam 映射；与 A3-03 同批（B4） | G1/G4 | B4 |
| C-A3-07 | **`ProgramHalves` 说不清"half of what"**：它是每 (program, rev, layout, variant) 的编译产物（管线层 + 录制器）；行业词是 pipeline variant / compiled program | `ProgramHalves.hpp:18-39`；`ContentFacts.hpp:134`（pin of a compiled half） | Filament `Program`/variant；O3DE `Pipeline` | **建议改名（插件内，低成本）** `CompiledPrograms`/`ProgramVariants`；`BindingSets`/`PushRange` 保留 | G12 | B13 |
| C-A3-08 | **`MaterialBase` 是接口却按实现基类命名**：它是"材质值的公共读面"，`Material` 是资产、`MaterialInstance` 是实例 | `MaterialBase.hpp:226-234` | Filament `Material`/`MaterialInstance`；O3DE `Material`/`MaterialAsset` | **文档澄清**（"MaterialBase = 值的读面"）；改名 `MaterialValues` 留 B12 | G12 | B11 + B12 |
| C-A3-09 | **三个"驻留"名字太近**：`Residency*`（报告）、`ResourceTable`（pin）、`ResidenceTable`（谁可以建） | `Residency.hpp`；`vsg/internal/ResourceTable.hpp:58`；`vk/internal/ResidenceTable.hpp:54` | D3D12 residency manager；wgpu 内部引用 | SDK 报告名保留；两个插件表**建议改名** `PinnedResources` / `ResidencyClaims`（B13） | G1/G12 | B13 |
| C-A3-10 | **接缝没有名字**：8 个"两个后端都用"的机制住在宿主命名空间 `vn::graphics`，文件集又无门禁 | §4.1 机制表；A3-04；`graphics-architecture.md` §2.9 | O3DE 用 `RHI` vs `RPI` 两个命名空间分开 | **需澄清**：要么文档分组 + 文件集门禁（B3），要么命名空间别名组（B14）；今天先不做结构变更 | G1/G2 | B3/B14 |
| C-A3-11 | **假朋友**：`RenderPass` 不是 Vulkan render pass（类注已写明）；`RenderCommand` 不是 command buffer；`RenderPassKey`/`RenderPassCompatibility` 才是 Vulkan 形状 | `RenderPass.hpp` 的 "Naming" note；`DrawState.hpp:148`（`RenderPassKey`）；`RenderPassCompatibility` | Vulkan `VkRenderPass`/command buffer | **保留**（已文档化）；用户文档补一张"不是那个"表（B11） | G1/G12 | B11 |
| C-A3-12 | **一引擎一表面** 与 Filament/UE 的多 View 不同 | ADR-2026-11-13；`graphics-architecture.md` §2.5/F19 | Filament 多 `View`/多 `SwapChain`；UE 多 `FSceneView` | **合理的产品裁剪**（每窗口一引擎）；分屏用 pass 子视口；不改 | G8 | — |
| C-A3-13 | **没有 render graph**，但有 `Plan`/`FrameTable`/transient 名字 | `graphics-frame-pipeline.md` F6/§2.8；`graphics-architecture.md` D5 | UE RDG；Frostbite FrameGraph；O3DE FrameGraph | **正确的不借**（无消费者，G8）；文档写清"引擎是 frame compiler，不是 render graph"（B11） | G8 | B11 |
| C-A3-14 | **发布模型**：不可变快照 + 发布源；无每帧 extract/sync | `graphics-threading.md` §2.2/§2.9 | UE `FScene`、Godot `RenderingServer`、Qt Quick `sync()` | **已同形/更强**；不改 | G1 | — |
| C-A3-15 | **诊断**：稳定 code + subject + limit + origin + 一次情节 | `scripts/diagnostic_codes.txt`；`RenderDiagnostic.hpp` | D3D12 info queue；Vulkan debug utils | **强于多数**；不改（可选借鉴 object pointer，已有 subject） | G4/G11 | — |
| C-A3-16 | **`SceneView` 名字只说了一半**：它同时是相机+场景、链的采纳者、布局回调的宿主 | `SceneView.hpp` 类注；`addSurfaceLayout` | Filament `View`；Godot `Viewport` 的宿主侧 | **文档澄清**（per-window controller）；改名留 B12 | G12 | B11 |

### 11.3 语义细节（同词多义 / 异词同义 / 动词承诺 / 生命周期）

**同词多义（都在公开或插件 API 里出现）**

| 词 | 义一 | 义二 | 处理 |
|---|---|---|---|
| `Plan` | pass 声明列表（`Plan.hpp`） | `FrameHead::DrawPlan` 的"已解析的一条绘制" | 已由限定名区分；文档在 B11 里点明，不改名（两个后端各持一份值计划，G8） |
| `Session` | `createSession()` 的动词（借一个驱动者） | 设计散文里的"一次执行"（SDK 无此类型） | B11 文档统一为 driver；B12 评估动词改名 |
| `Pass` | 引擎级渲染阶段（`RenderPass`） | Vulkan render pass（`RenderPassKey`/`RenderPassCompatibility`） | 类注已写明"NOT a Vulkan render pass"；补"假朋友"表 |
| `Command` | 一条 drawable 绘制（`RenderCommand`） | Vulkan command buffer（后端内部） | C-A3-03/11 |
| `Target` | pass 的声明（`Target`） | 宿主对象（`RenderTarget`）/ GPU 对象（`TargetObjects`）/ 表（`TargetTable`） | 层后缀已存在；B11 补角色表 |
| `Content` | 内容模型与身份 | 缓存/录制器（`ContentDraw`/`ContentImages`/`ContentPipeline`/`ContentPass`/`ContentAssembly`） | 设计 §2.12 已裁决"保留（churn）"；登记不改（G8） |
| `Half` | 编译产物（`ProgramHalves`） | 常被读成 vertex/fragment 两个 half | C-A3-07（插件内改名） |
| `Residency` | 报告行（`ResidencyKind`） | 建造权（vk `ResidenceTable`）/ pin（vsg `ResourceTable`） | C-A3-09（插件内改名） |

**异词同义/重叠**

- `createSession` / `RenderFrameExecutor` / "driver" —— C-A3-02。
- `ReadbackRefusal` / `ReadbackResult`（+ 后端两枚举）—— C-A3-06。
- `TargetPlan`（设计词）/ `TargetDecision`（类型）/ `planTarget`（动词）—— C-A3-05。
- `Features`/`Limits` 与旧 `Capabilities` 的内联转发（BC-11）——旧拼写还在，是有意的迁移窗口，不是新概念。

**动词承诺与名字**

| 动词 | 名字承诺 | 实际 | 判定 |
|---|---|---|---|
| `releaseContent(object)` | 释放"内容"（听起来销毁对象） | 只放 GPU 驻留，对象仍可用（`graphics-content-lifecycle.md` §2.2） | **名字有误导**；改名 `releaseResidency` 留 B12；文档先写清 |
| `releasePass` | 释放 pass | 只忘掉以 pass 身份为键的状态（ADR 决定 5） | 已收窄契约并写明；保持 |
| `releaseRenderTarget` | 释放目标 | 释放附件并允许重建（vk）/ 忘记录（vsg） | 契约写明；保持 |
| `publish()` | 发布场景 | 一次指针交换，返回三态 + 句柄 | 与 Qt/UE 的 publish 同义；保持 |
| `prepare()` | 准备一帧 | 只建不提交；可提交纯传输批（ADR-2026-11-14） | 已写明；保持 |
| `setRenderTarget` | 设置目标 | 两后端都**预建**（BC-09） | 已变真；保持 |

**生命周期语义（与成熟引擎对照）**

- Vine 的 `intrusive_ptr`（共享拥有）/`raw_ptr`（借用）/`shared_ptr<const>`（不可变共享值）/`span`（非拥有）
  是显式词汇表（`graphics_global.hpp` 顶部），Filament 用裸指针 + 显式 destroy，wgpu 用内部引用计数。
  Vine 的模型更接近 UE 的 `TRefCountPtr` + 值化资源；**合理**。残余是 8 处 `const_cast` 组成可变 owner（A3-18）。
- 释放语义是"驻留归后端、对象归宿主"（`graphics-content-lifecycle.md` §2.1）：比 Filament 的
  `Engine::destroy` 更细，比 O3DE 的 RHI 资源生命周期更显式；**合理**，但动词名 `releaseContent` 要改（B12）。

### 11.4 与成熟引擎的结构差异（落地后的"借/不借"）

| 概念 | 框架怎么做 | 今天 | 判定 |
|---|---|---|---|
| 视图 | Filament `View` = camera+viewport+layers+target；O3DE `RPI::View` | `View` 值 + `SceneView` 控制器 | **已同形** |
| 区域 | Godot `Viewport`；平台 surface 是另一回事 | `RenderArea` + `WindowPresentTarget` | **已同形**（重设计 B13） |
| pass 作者 | Filament/O3DE/wgpu 的不可变描述 + Builder | `PassDesc` 唯一作者，`RenderPass` 只读 | **已同形**（重设计 B16c） |
| 目标声明 | UE `FRenderTargetBinding`；O3DE `PassAttachment`；wgpu attachment | `Target` + `Output` | **已同形**（重设计 B14） |
| 能力 | WebGPU/wgpu `Features`+`Limits`；D3D12 特性查询 | `Features`+`Limits` | **已同形**（重设计 B15） |
| 诊断 | D3D12 info queue / Vulkan debug utils | code + subject + limit + origin + episode | **强于多数** |
| 设备/队列/表面 | wgpu Device/Queue/Surface；D3D12 Device/Queue/Fence | 设备池 + 每驱动帧机器 + 窗口目标 | **已同形**（T47/T48） |
| 资源寿命 | wgpu 内部/用户引用；D3D12 residency；Vulkan memory report | 三态 + park（vk）/ pin + 声明索引（vsg） | **已同形**；两机制保留 |
| 命令记录 | D3D12 command list/allocator；wgpu encoder | 三相位；记录零锁 | **已同形** |
| **宿主 API vs 后端接缝** | O3DE `RPI`（应用）/`RHI`（设备）两个命名空间；wgpu 一个 API 但无场景层 | 8 个机制住在 `vn::graphics` 宿主命名空间 | **仍落后一处**（C-A3-10） |
| render graph | UE RDG；Frostbite FrameGraph；O3DE FrameGraph | 平面调度 + transient + 借用深度；屏障归后端 | **不借**（G8） |
| bindless / 描述符堆 | D3D12 heap；`VK_EXT_descriptor_buffer` | 每 draw 的 set + dynamic offsets | **不借**（无实测瓶颈） |
| 多帧在飞 | 多数框架 2–3 帧 | 一帧在飞（设备占 ~90%） | **不借**（ADR-2026-11-15） |
| 异步编译 | PSO library / `VK_EXT_pipeline_creation_cache_control` | `prepare()` + 会话内/跨会话缓存 | **不借额外机制**（G8） |

### 11.5 结论

1. **没有发现需要重开结构的概念缺陷**：视图、区、pass 作者、目标声明、能力、设备/队列、资源寿命、
   命令记录已经与成熟框架同形；render graph、bindless、多帧在飞、异步编译是**有理由的不借**。
2. 真正值得处理的是**语义**，不是结构：
   - **C-A3-06 读回两套拒绝枚举重叠**（公开 API，B4）——最值得修的一条；
   - **C-A3-02 一概念三名**（session/executor/driver）——文档先统一，API 留 B12；
   - **C-A3-03 绘制两层命名与业界相反**——登记，未来 API 批（B12）；
   - **C-A3-09/07 插件内近名/隐名**（ResidenceTable/ResourceTable、ProgramHalves）——低成本改名（B13）；
   - **C-A3-10 接缝没有名字**——文档分组 + 门禁（B3），或命名空间别名（B14）。
3. **C-A3-01/11 的"词反了/假朋友"不是缺陷，是读者成本**：用一张翻译表和一张"不是那个"表解决（B11），
   不改公开名（G8）。
4. 概念密度（`Frame*` 17、`Render*` 17、`Pass*` 10）本身不是问题：每个名字有今天的消费者与一段契约
   （§4）；问题只在于**宿主面与接缝面没有分组**（C-A3-10）。

---

## 12. 任务列表（可执行）

> 优先级：**P0** 立即修（正确性）；**P1** 下一批（契约/文档/证据被推翻）；**P2** 独立可做；
> **P3** 只登记或未来公开 API（需明确 go）。"来源"指向 §3 的 A3-xx 或 §11 的 C-A3-xx。

| ID | 任务 | 来源 | 类型 | 优先级 | 批次 | 验收 |
|---|---|---|---|---|---|---|
| T-00 | 把本页登记进 `.ai/README.md`（本轮任务禁止写该文件） | 门禁 | 文档 | P1 | 下一次提交 | `check_ai_docs.py --strict` 0 finding |
| T-01 | `TargetObjects::fresh` 改原子或把相位一读取挪到门内，并写注释说明"相位一可以答 NotReady" | A3-01 | 缺陷 | **P0** | B1 | `test_vk` 167 绿；TSan vk 0 |
| T-02 | 把 "TSan 三套 0 报告" 的句子改成今天的实测（含日期与命令） | A3-02 | 文档 | P1 | B1 | 两页设计文档同步；门禁绿 |
| T-03 | 文档真话清扫：`SEAM_HEADERS` 句、vsg 命名空间表、shader 旧注释、`graphics.md` 四个 API 名、F1/F2 状态、日期/计数/`toVkFormat`、生产后端措辞 | A3-04/05/07/09/10/24 | 文档 | P1 | B2 | 四道文档门禁 0 new |
| T-04 | 守卫补齐：接缝文件集门禁 + `graphics.md` 的 API 名纳入符号门禁（先建 baseline） | A3-04/08 | 门禁 | P1 | B3 | 反向变异（加文件/删符号）红，删回绿 |
| T-05 | 读回：返回值形状 + **两套 SDK 拒绝枚举收敛** | A3-03 + C-A3-06 | 公开 API | P1 | B4 | 三套绿；`check_api_style` baseline 降到 1；迁移说明 |
| T-06 | 降级"内部味"导出（`keyAuditTable`/`refusalName`/`planCodeText`/`renderApiToString`） | A3-20 | 公开 API | P2 | B5 | 导出/文档门禁；三套绿 |
| T-07 | 决定 `core` 的 using-directive：收窄为 8 个别名，或保留并写清 ADL 后果 | A3-06 + C-A3-10 | 命名空间 | P2 | B6 | `test_vsg` 全绿 + `content_seam` |
| T-08 | 宿主默认后端显式化（不再取名字序 `front()`） | A3-11 | 行为 | P1 | B7 | demo 0 error；新增"顺序不影响默认"用例 |
| T-09 | validation 收干净（深度-only 无颜色变体 + "location 1 not consumed"） | A3-13 | 验证 | P1 | B8 | demo `[warning]` 0 |
| T-10 | 成本测量：97 分配/帧接上测量装置；性能句复核并写回日期 | A3-14 | 证据 | P2 | B9 | 一次 bench 输出进提交正文 |
| T-11 | 概念文档：翻译表 + "假朋友"表 + 五个帧值表 + 目标角色表 + "frame compiler 不是 render graph" | C-A3-01/03/04/05/11/13/16 | 文档 | P2 | B11 | 读者能从 `graphics.md` 一步对上 Filament/O3DE 词 |
| T-12 | 公开词汇统一（每词单独提交、需明确 go）：session→driver、DrawItem/DrawCall、MaterialValues、releaseResidency、EngineNotReadyReason | C-A3-02/03/08 + A3-17 | 公开 API | P3 | B12 | 每个词一条迁移说明；默认**不做** |
| T-13 | 插件内改名：`ProgramHalves`→`CompiledPrograms`、`ResourceTable`→`PinnedResources`、`ResidenceTable`→`ResidencyClaims` | C-A3-07/09 | 归属 | P2 | B13 | 用例名不变；对拍 13；`content_seam`；CMake 清单同步 |
| T-14 | 接缝身份（可选）：`vn::graphics::backend` 别名组，或保持并靠 B3 的门禁 | C-A3-10 | 结构 | P3 | B14 | 三套绿；无二义/实例化新增 |
| T-15 | vsg 的 extent/size 拒绝精度：补 code 或写成"有意差异" | A3-12 | 诊断 | P3 | B3 | 用户页差异表有这一行 |
| T-16 | 登记不改：`OwnedFrame`、`GraphicsTest.cpp` 拆分、宏面、`const_cast` 逐点决定、`Frame*` 密度 | A3-15/16/18/21 + C-A3-04/12/14/15 | 登记 | P3 | — | 本页 §9 即记录 |

---

## 附：本轮使用的复核配方

```bash
# 1) 计数与用例
./build/bin/test_graphics --gtest_list_tests | grep -c '^  '      # 617
./build/bin/test_vk       --gtest_list_tests | grep -c '^  '      # 167
./build/bin/test_vsg      --gtest_list_tests | grep -c '^  '      # 572
./build/bin/test_vsg      --gtest_list_tests | sed -n '/^BackendParityTest/,/^[A-Z]/p' | grep -c '^  '  # 13

# 2) 门禁
python3 scripts/check_ai_docs.py --strict; python3 scripts/check_design_docs.py
python3 scripts/check_doc_symbols.py;      python3 scripts/check_include_hygiene.py
python3 scripts/check_api_style.py;        python3 scripts/check_diagnostic_formats.py
python3 scripts/check_public_docs.py;      python3 scripts/check_export_annotations.py
python3 scripts/check_vk_exposure.py
ctest --test-dir build -R 'content_seam|vk_exposure' --output-on-failure

# 3) 并发（今天的结论：vk 1，其余 0）
scripts/tsan_check.sh
VN_TSAN_TARGET=test_vk scripts/tsan_check.sh
VN_TSAN_TARGET=test_vsg VN_TSAN_FILTER='-*Window*:*Probe*:*Plugin*' scripts/tsan_check.sh
VN_ASAN_TARGET=test_vk VN_ASAN_LEAKS=1 VN_ASAN_LEAK_SCOPE='vn::backend_vk' scripts/asan_check.sh

# 4) 宿主路径
cd build/bin && timeout 25 ./Vine --open demo        # session up + Presenting；[error] 0；[warning] 3

# 5) AST 命名空间清单（配方）
{ echo '#include <vine/graphics/graphics_global.hpp>'; \
  for h in src/viz/graphics/sdk/vine/graphics/*.hpp; do b=$(basename $h); \
  [ "$b" = graphics_global.hpp ] || echo "#include <vine/graphics/$b>"; done; } > /tmp/ast_all.cpp
clang++-23 -DVN_GRAPHICS_LIB -DVN_ROOT_NS=vn \
  -I src/viz/graphics/include -I src/viz/graphics/sdk -I build/generated \
  -I src/base/core/include -I src/base/core/sdk -I src/base/global/sdk \
  -I src/base/geom/include -I src/base/geom/sdk -I src/base/math/include -I src/base/math/sdk \
  -I src/base/models/include -I src/base/models/sdk -I src/base/window/include -I src/base/window/sdk \
  -I src/base/imaging/include -I src/base/imaging/sdk \
  -std=gnu++20 -Xclang -ast-dump -Xclang -ast-dump-filter=vn -fsyntax-only /tmp/ast_all.cpp > /tmp/vn_ast.txt
# 插件 TU 用各自的 include/ 与 compile_commands.json 的 include 集合同理；
# 只统计直接父节点是 NamespaceDecl 的声明（函数内局部变量已排除）。

# 6) 宏
clang++-23 … -E -dM /tmp/ast_all.cpp | grep -E '^#define VN_' | wc -l   # 55（20 个函数式）
```
