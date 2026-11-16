# ADR-2026-11-16：重设计的是名字与归属，不是能力 —— 两后端图形栈的接缝、词汇与相位

- 状态：**已采纳（实施中）**。2026-11-16 批准；阶段三按 `../research/graphics-2026-11-16-redesign-r2.md` §10 的
  18 个批次位置执行，进度见本文末"实施状态"。
- 日期：2026-11-16
- 影响范围：`graphics/sdk/vine/graphics/backend/**`（25 → 7 个头：6 机制 + 1 守卫）、`graphics-architecture.md`（§2.9 / §2.11）、
  `graphics-sessions.md`（§2.1 / §2.3 的词汇与相位）、`graphics-threading.md`（§2.16 的驱动者身份）、
  `vk-backend.md`（§2.16 / §2.17 / §9 的状态）、`vsg-backend.md`（§2.9.1 的接缝声明）、
  `docs/guides/`（按后端分页 → 按能力分页）、`RenderFrameExecutor.hpp` / `RenderDevice.hpp` 的契约文本、
  `FrameStats.hpp`（`FrameReport::locking`，additive）。
- 相关：`ADR-2026-11-16-authoring-view-and-target-vocabulary.md`（第二轮的**概念与 API 形状**续篇：`View` /
  `RenderArea` / `Target`+`Output` / pass 作者形状 / 规范修订；两份提案一起批准或一起缓）、
  `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`（本文是它的**词汇与接缝**收口，不重开它的决定）、
  `ADR-2026-11-13-one-engine-one-surface.md`、`ADR-2026-11-12-adopt-graphics-next.md`、
  `ADR-2026-11-10-gpu-residency-pinned-by-content.md`、`ADR-2026-11-14-vk-becomes-the-default-target.md`。
- 审核与施工图：`../research/graphics-2026-11-16-redesign.md`（A 能力清单、B 逐项、C 横切、D 结论表、批次计划）。

## 背景

用户要求：**重新设计 graphics 与两个后端（概念 / 抽象 / 框架 / C++ 结构），功能不减。**
类型、接口、层次、命名、公开 API/ABI 都可以重设计；能力一项都不能丢；有意改变的行为逐条列出并经批准。

阶段一对约 4.4 万行 `.cpp`（`graphics/src` 1.48 万 + vk 1.20 万 + vsg 1.72 万）、2.1 万行 SDK 头、
1286 条用例、12 个设计页与 100 条诊断 code 做了只读审计，结论集中在一句话上：

> **能力很全、证据很厚；问题几乎全在名字与归属。**

最能说明问题的三条事实（都可以用 file:line 或 include 计数核对）：

1. **接缝名不副实**：`sdk/vine/graphics/backend/` 的 25 个头里（含 1 个包含守卫），`gfx_backend_vk` 只 include 6 个；
   18 个只有 `gfx_backend_vsg` 一个消费者，而它们的实现（18 个 `.cpp`）编在 `libGraphics` 里、
   随 `VineGraphicsBackendSdk` 组件安装。这层被两份设计页称作"后端契约"，实际是**第一个后端的
   可移植半边被提升为接缝**。
2. **"会话"是四个东西**：设计名词（= `RenderFrameExecutor`）、vk `Device::Session`（帧机器）、
   vsg `api::Session`（窗口 + 交换链）、vsg `VsgDriver`（帧驱动者）；身份类型还分成
   `DeviceClock::WindowId`(u32)、`ResidenceTable::SessionId`(u64)、`TargetClaims::SessionId`(u32)。
3. **同一件东西在两个后端各自发明**：帧首解析出的"每条绘制调用的值"在 vk 是
   `CommandPlan` + `ScreenPlan` + `ResolvedTextures`，在 vsg 是 `ContentPlan::Call`。
   两者都对，但形状与名字对不上；`graphics-sessions.md` 自己写"剩下的差别只是形状"。

审计还发现两处**工程债**（不是设计债）：22 处未编码报告（6 个文件，`diagnostic_code_baseline.txt`
的每文件上限）、6 处设计页句子已被实现推翻（`vk-backend.md` §2.17/§9.2、`graphics-threading.md` §2.16 末等）。

## 决定

**采纳"重新划分已有能力"（`G13`），不新增能力（`G8`）；分十批（0–9）迁移，每批一个可回滚提交。**

### 1. 词汇：一个概念一个名字

| 概念 | 名字 | 备注 |
|---|---|---|
| 帧执行者 | **FrameDriver**（类型仍是 `RenderFrameExecutor`） | 设计名词 `Session` 退役；`createSession()` 动词保留，文档改成"借一个驱动者" |
| 驱动者身份 | **`FrameDriverId`**（u32） | 替掉 `WindowId` / 两个 `SessionId`；`WindowId` 留一版 `@deprecated` 别名 |
| 一个窗口的交换链会话 | **WindowSession**（原 vsg `api::Session`） | 它是窗口，不是帧驱动者 |
| 帧首解析出的值 | **FrameHead** + **FrameHead::DrawPlan** | vk 三个类型合并、vsg `ContentPlan`/`Call` 改名 |
| 一帧的提交策略 | **`Submission`**（参数/枚举，不建类型） | `Frame` / `TransferOnly`（prepare）；把散文里的"transfer batch"变成名词 |

**不改**：`RenderDevice` / `RenderFrameExecutor` / `RenderBackend` / `PresentTarget` / `RenderEngine` /
`Scene` / `Plan` / `PassChain` 的动词与签名；`ContentCaches` / `ContentStore` 等实现类型名（改名只有 churn）。

### 2. 接缝归位：`backend/` 只留共享机制

保留 6 个（两个后端真的都在用）：`FrameGate`、`DeviceClock`、`RetirementQueue`、`Readback`、
`Keys`、`TargetPlan`；新增 `DiagnosticDraft`。
其余 18 个头 + 18 个 `.cpp` **搬进 `gfx_backend_vsg/internal/`**（纯移动，零行为）。
`VineGraphicsBackendSdk` 组件随之缩小；`libGraphics` 不再编译 vsg 专用机制。

### 3. 一帧三相位，两后端同形

```
openFrameHead(value, present_to)   // 1 次设备锁：解析目标/输入/内容键/上传 → FrameHead
record(frame_head, cmd)            // 0 次设备锁：只读 FrameHead
commit(cmd, target)                // 1 次设备锁：提交、present、帧账
```

`prepareFrame` = 同一流程 + `Submission::TransferOnly`。相位的**次数**由 `FrameReport::locking`
（新可选字段：`acquisitions` / `waits`，缺席 = 不测）跨后端可读。

### 4. 拒绝只有一种形状

`backend/DiagnosticDraft`：`category + code + subject + limit + message`，`refuse()` 唯一写者、
先到者胜。vk `ContentRefusal` 变成它的别名 + "枚举 → code 的无 `default` switch"（学 vsg 的
`HostReadbackRefusal`）；引擎侧 20 条报告补 code，`diagnostic_code_baseline.txt` 清零。

### 5. 目标与引擎：各自归位

- 目标：vk 抽 `TargetTable`（map + 形状缓存 + 构建/重建 + claim + park），`Device.cpp` 只装配与转发；
  vsg 的 `VsgTargetRegistry` + `HostTargets` 按同一概念命名。**claim 是目标表的一个属性**，不是第三张表。
- 引擎：抽 `detail::SessionShape` / `FramePump` / `SurfaceLedger`，`RenderEngine` 退回组合根；
  公开面零变化是验收条件。
- 收集：**一份实现**（快照 + `selectSnapshot`），删除 `ScenePublisher` 里的第二份行走与 memo。
  先量后改，可整批回滚。

## 被否决的路（与理由）

| 选项 | 为什么不做 |
|---|---|
| 把 `RenderDevice` 拆成六个接口 | 只有测试替身受益（它们已靠默认实现只覆写 5 个纯动词）；第二个 API 家族才有被迫的形状（`G8`） |
| 两个后端共用一个 `ResolvedDraw` 类型 | 只能类型擦除，把复杂度推给每个读者（`G7` 反面）；统一形状足够 |
| `ResidenceTable` 与 `ResourceTable` 合成一套 | 一个回答"谁可以建这个键"，一个回答"这份驻留能不能放"；合并会丢三态或丢 pin（`G1`） |
| 用 `Session` 取代 `RenderFrameExecutor` 作为公开类型 | 为一个词改公开 API，迁移面大、收益零（`G8`） |
| `RenderBackend` 改成两个指针的值（拆 T32） | 破坏 2 个后端 + 5 个测试替身，今天没有消费者（`G8`）；记为触发条件 |
| 立刻删直接收集路径 | `T34` 是等价性证据，不是性能证据；先量后改 |
| 统一五套失败枚举（A10） | 它们回答不同问题，统一丢精度（`G8`） |
| 本次退役 vsg | 产品决策，必须单独提出；用户已声明两个后端都保留 |
| 一次大提交 | 不可回滚、不可分批验证 |

## 后果

**好的**

- 第四个后端第一次能读一份**只含 6 个机制**的接缝，不必学 vsg 的 `FrameCompiler` / `VariantPool` / `PhaseTable`；
  也不必学 Vulkan 的 `subpass`（若第 8 批落地）。
- "一帧三相位"从两段散文变成契约 + 跨后端可读的数字；"记录路径零锁"第一次有共同读数。
- 一个概念一个名字：新人不必先学"session 在这里指窗口"。
- 诊断面从三种形状收成一种；engine 侧 baseline 归零。
- 引擎的线程策略改动落在 `FramePump`，不再外溢到 9 个文件；收集规则只有一份。
- 净删代码（第二份选择、第二套拒绝形状、vk 的两个 plan 类型、一份 gate）。

**代价 / 风险（如实）**

- 第 3 批是 36 个文件（18 个头 + 18 个实现）的大搬家，漏一个 `.cpp` 就是链接错；回滚粒度大。
- 第 2 批碰线程与锁，必须 TSan/ASan 全跑；vk 与 vsg 的 gate 合成一个时，语义细节（租约、递归、诊断重入）
  要逐条对齐，任何一条不同都是行为变化。
- 第 7 批可能变慢（publish 进入查询路径）；只有测量允许它落地。
- 公开面虽然"签名不变"，但 `FrameReport` 加字段、18 个头搬家都是**源码级破坏**（仓库外消费者）；
  第 3 / 4 批各自的文档与 ADR 状态必须同批更新。
- 本机（lavapipe）测不出并发收益——本文不用它证明收益，只用结构证明成本（热路径零锁、miss 才付费）。

**触发条件（什么时候重开被否决的项）**

1. 第二个 **API 家族**的后端（D3D12 / Metal）出现 ⇒ 拆 `RenderDevice`（D1）与去 Vulkan 拼写（第 8 批）一起做。
2. 第一个"只想返回 `RenderDevice`"的工厂，或第三个后端 ⇒ 拆 `RenderBackend` 聚合（T32）。
3. 出现"跨帧持有帧值"的消费者（render graph / 离线渲染）⇒ 拆 `FrameValue` / `FrameHandles`。
4. 真实宿主"一边编辑一边烘焙"且第 2 批的等待进入帧预算 ⇒ 回到 `ADR-2026-11-15` 的多会话扩展。

**明确不做**

- 不新增没有消费者的扩展点（`G8`）；不引入第三方依赖。
- 不改两个后端**有意不同**的能力（借深度释放语义、内存池、管线缓存文件、GPU 计时）——它们在用户文档的
  差异表里被写明，而不是被抹平。
- 不在本 ADR 里顺带做 backlog 的任何触发型项（多帧在飞、dynamic rendering、VMA、LOD、点光阴影……）。

## 实施状态

- **批准（2026-11-16）**：两份 ADR 与全部批次一次批准。三条行为变更**全批**（BC-08 `before_draw` 不再隐式调用、
  BC-09 vk `setRenderTarget` 预建、BC-10 宿主报告不计入引擎 `diagnosticCount`）；`A-01` 走**方案 1**
  （vk 也预建，补 CAP-86），不退役动词。
- **阶段一（审核）**：完成（`.ai/research/graphics-2026-11-16-redesign.md` §2–§5）。
- **阶段二（目标设计 + 本 ADR）**：完成并批准。
- **批次 0（characterization）+ 批次 10（规范清扫）已落地（2026-11-16）**：CAP-44 / CAP-45 / CAP-86 的三条新用例；
  四条规范文字（字段一行 `///`、禁止 section comment、枚举底层类型、数值类型与访问器/命令、所有权词汇表）与 26 处
  section comment 的清扫。证据：`test_vk` 140、`test_vsg` 564 全绿，九道门禁零新增。
- **批次 1（词汇与身份）已落地（2026-11-16）**：`backend::FrameDriverId`（u32）成为帧驱动者身份的唯一定义，
  `TargetClaims::SessionId`（u32）与 `ResidenceTable::SessionId`（u64）合并进它（后者收窄到 u32，全仓无 64 位来源）；
  vsg `api::Session` → `api::WindowSession`（文件、include、`tests/test_vsg/CMakeLists.txt` 清单、`SessionOptions` →
  `WindowSessionOptions`）；vk `Device::Session` → `Device::FrameDriver`；`DeviceClock::WindowId` 留一版 `@deprecated`
  别名。**纯改名，零行为**：三套用例的名字一个没动（它们是指纹），`VkMultiSessionTest.TwoDriversShadeWithTheirOwnDefaultProgram` /
  `VsgBackendTest.TwoDriversKeepTheirOwnDefaultProgram` / `HeadlessBackendTest.OneDeviceServesSeveralAreasWithoutCrossingThem`
  改名后仍绿；`DeviceClock` 的**动词**仍是 `addWindow` / `windowCompleted`（只改类型，动词留给下一个批次决定）。
- **批次 2（一帧三相位 + 帧闸）已落地（2026-11-16）**：`backend::FrameGate` 只剩一份实现（`enterShared` /
  `enterExclusive` + 租约 + 每闸每线程嵌套栈 + `Counters`），vk 的设备体闸与 vsg 的 `internal::DeviceGate` 都换成了它；
  13 个 vk 宿主动词从"同线程死锁"改成拒绝值 + 一条 `vk.device.reentry`；`FrameReport::locking` 落地（两后端各一条
  "稳态常数 + waits 0"的用例）。**批次 2b（`FrameHead` 词汇）同日落地**：帧首解析出的值在两后端都叫 `FrameHead` +
  `FrameHead::DrawPlan`——vsg 的 `internal/ContentPlan`（类 `ContentPlan` / `Call`）改名，
  vk 的 `CommandPlan` / `ScreenPlan` / `ResolvedTextures` 合并进新头 `internal/FrameHead.hpp`（`planDraw` 一个动词
  两个重载、帧级纹理/采样器事实挂 `FrameHead`，screen 计划也是 `DrawPlan`、用 `kind` 区分）。**不建共享头**，统一的是
  名字与形状（本文"被否决的路"里的 `ResolvedDraw`）。纯改名/合并，零用例改名：`test_graphics` 607 / `test_vk` 159 /
  `test_vsg` 571、对拍 13；九道门禁与 `content_seam` / `vk_exposure` 全绿。
- **批次 3（接缝归位）已落地（2026-11-16）**：`sdk/vine/graphics/backend/` 只留 7 个机制 + 守卫——`FrameGate` /
  `DeviceClock` / `FrameTimeline` / `RetirementQueue` / `Readback` / `Keys` / `TargetPlan`；17 个头与 16 个 `.cpp`
  （原文的 18 + 18 里，`FrameTimeline` 留下、`ResourceTable` 本来就没有实现）搬进
  `gfx_backend_vsg/{include/vine/backend_vsg/internal,src/internal}`。`FrameTimeline` 的判据：`DeviceClock` 持有它、
  `RetirementQueue::retire/advance/retirePoint` 以它为参数——搬它是接缝重构，不是搬家（`G1`）；vk 在 Device.cpp 的
  注释里也引用它。搬走的类换成 `VN_BACKEND_VSG_API`（`VN_GRAPHICS_API` 在插件里会展开成 dllimport，Windows 链接错），
  不再包含 `BackendHalfGuard.hpp`（它们不再是那一半）。`libGraphics` 的 `src/backend/` 只剩 6 个 `.cpp`；新门禁
  `check_include_hygiene.py` 钉住接缝的**文件集**（多一个文件就红）。`CoreTimelineTest`（5 例）随 `FrameTimeline`
  搬进 `tests/BackendSeamTest.cpp`（三个测试二进制都编它）；**零用例改名**，`test_graphics` 612 / `test_vk` 164 /
  `test_vsg` 571、对拍 13，九道门禁全绿。
- **批次 4（拒绝只有一种形状 + 诊断来源）已落地（2026-11-16）**：接缝新增 `backend/DiagnosticDraft`（category + code +
  subject + limit + message，`refuse()` 唯一写者、先到者胜，`diagnostic()` 转成 SDK 的值）；vk 的 `ContentRefusal` 成为它的
  别名，且原因改成 `ContentRefusalCause` 枚举 + `contentRefusalCode()` 的**无 `default` switch**（D-22：加一个原因不写 code
  就编不过，`VkContentRefusalTest.EveryCauseNamesAStableCodeAndTheDraftIsTheSharedShape` 走一遍全表并验形状）；vsg 的读回拒绝
  （`HostReadbackRefusal`）也经 draft 产出同一个值。引擎侧 20 条报告全部带 `engine.*` code（`detail::engineProblem` 是唯一
  构造者），宿主一条带 `appfw.viewer.foreign_view`；`scripts/diagnostic_code_baseline.txt` **清零**（门禁：122 个 code
  双向对齐、0 条未编码报告）。**`RenderDiagnostic::origin`（Engine / Device / Host）** 落地（A-06 / CAP-87）：`reportEngineProblem`
  的三实参形式是宿主入口、带 `Host`；`diagnostics().report` 是引擎/配方入口、带 `Engine`；设备的报告默认 `Device`。于是
  `diagnosticCount()` / `diagnosticsClean()` / `diagnosticCount(category)` 只算引擎 + 设备（**BC-10 落地**），宿主另有
  `diagnosticCount(DiagnosticOrigin::Host)`；见证 `RenderEngineTest.AHostReportIsCountedApartFromTheEnginesOwnFindings`。
  零用例改名：`test_graphics` 613 / `test_vk` 165 / `test_vsg` 571、对拍 13。
- **阶段三（迁移）**：**实施中**。批次表与顺序以 `../research/graphics-2026-11-16-redesign-r2.md` §10 为准；
  每批一个可回滚提交，每批结束在提交正文列出"哪些能力 ID 从旧证据切到新证据"——git 是批次进度的档案，
  本文只记状态。
- **本 ADR 在最后一批落地时改为"已采纳"**；若某批被放弃，在同一行注明并链接原因。
