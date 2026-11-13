# graphics 帧管线（下一版）

> **状态**：草案 —— `graphics-next` 已采纳（ADR-2026-11-12），按 §9.3 分步替换现役形状；每步的状态见 §3.1。
> **最后核对**：2026-11-12
> **范围**：内容 → 快照 → 派生 → 执行 这四层的形状、身份、寿命与计划语义。不含几何/拾取算法、材质模型、
> 后端实现细节（`vsg-backend.md`）、线程契约的迁移期现状（`graphics-threading.md`）。
> **权威来源**：`src/viz/graphics/sdk/vine/graphics/ContentId.hpp`、`sdk/vine/graphics/backend/ResourceTable.hpp`、
> `src/viz/graphics/src/SceneSnapshot.hpp`（原型）、`Scene.hpp` / `RenderEngine.hpp`（现役入口）。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

今天"跨层的那份副本什么时候过期"有五套答案：地址 + 版本（引擎表）、镜像 + delta（`ContentMirror` /
`DeltaChannel`）、memo + epoch（`DrawListMemo` / `Version`）、引用计数（`markUsed`）、停车窗口
（`RetirementQueue` / `kept`）。桥与寿命层合计 2519 行，其中约 60% 在回答同一个问题；两次真实事故
（镜像在帧内出生没有帧令牌、`refreshMaterials` 只改 publish 会滞后）与一次 UAF
（`.ai/bugs/vsg-content-address-recycled-before-rows.md`）都发生在这一层。

### 1.2 目标

1. 跨层只有**不可变值**；可变对象只活在内容侧（`C1`）。
2. 发布是**一次原子指针交换**；版本、镜像、delta、memo 不再是概念（`R9`）。
3. 身份稳定且死后可判定；驻留由"谁还引用"**算**出来（`I2` / `I4`）。
4. 计划是**声明**，误用不可表达（`R21` / `R38`）。
5. 派生是**纯函数**：可并行、可复现、可 dump、可 replay（`R14` / `R30`）。

### 1.3 非目标

- 不做内容侧多线程写（`C2`）；不做 GPU-driven / bindless（没有第二个后端、没有实测瓶颈）。
- 不做本层的自动 barrier / render graph（同步归后端）。
- 不承诺 ABI 稳定；不改 vsg 内部，只改它与本层的接口形状。
- 不承诺本轮就把 `-design` 的全部算法（pose、后期依赖图）落地：它们按 `../backlog/graphics.md` 的触发条件进行。

### 1.4 一句话结论

> 内容侧 publish 出一次不可变快照，派生层把 `(快照, 视图)` 算成值，执行层只消费值并回一个值；
> 四层之间只有一条边，每条边的载荷都不是活对象。

## 2. 设计要点

### 2.1 四层，一条单向边

- 是什么：① AUTHORING（可变，唯一写者）→ `publish` → ② SNAPSHOT（不可变值）→ `derive` → ③ DERIVATION
  （纯值）→ `execute` → ④ EXECUTION（设备状态）→ `FrameResult`（值）回到 ①。
- 为什么：四条边各只有一个动词，每层只能看见上一层交给它的值 ⇒ "谁在读谁的写"可枚举（`G2`）。
  唯一的回边也是值，理由是查询必须瞄准"用户看到的那一帧"（`I7`）。
- 边界：`execute` 是本层唯一可以碰到设备的动词；后端内部的同步、分配、提交不进本层的概念（`G5`）。

### 2.2 八个名词

| 名词 | 一个定义 | 归属 | 寿命 |
|---|---|---|---|
| **Scene** | 作者树 + 灯集 + 写者角色与发布点 | 内容侧 | 宿主持有 |
| **Node** | 树里的**位置**：放置 + 子 + 状态贡献 + 载荷 | 内容侧 | 原型可外部构造；位置由 `Instance` 表达 |
| **Geometry / Material / Program / Texture** | 可复用的内容值，各有 `XxxId` + `revision` | 内容侧 | 引用计数 |
| **Snapshot** | 场景的**这一次发布** | 发布层（值） | 被在飞帧与 Session 引用 |
| **View** | 相机值 + 视口 + 层掩码（投影种类是取值） | 派生层（值） | 一次派生内 |
| **DrawList** | `(Snapshot, View)` 的有序绘制项，一切已解析成值 | 派生层（值） | 一次帧内 |
| **Session** | 设备 + 后端 + 句柄表 + 管线缓存 + arena + 唯一引擎线程 | 执行层 | 宿主持有 |
| **FrameResult** | present、耗时、诊断、呈现了哪个 Snapshot / View | 值 | 宿主读 |

- 为什么：一个概念一个定义、一个归属（`G1`）。**统一的三件套**——句柄（身份）· revision（值变了）·
  值快照（跨层）——只写一次，八处同构（`G6`）。
- 边界：骨架里**没有** render world 对象、镜像、delta 通道、memo + 版本、`addPass`/`removePass`（已删）、
  模式枚举、manager、control。这是设计的一部分，不是遗漏。

### 2.3 三个动词

| 动词 | 语义 | 谁调 | 约束 |
|---|---|---|---|
| `publish` | 把可变内容变成新 Snapshot 并原子换指针 | 内容线程 | 唯一的一次可变 → 不可变（`I3`） |
| `derive` | `(Snapshot, View) → DrawList → FrameValue`，纯函数 | 任意线程 | 同输入必同输出（`I5`） |
| `execute` | `FrameValue → 设备动作 → FrameResult` | 引擎线程 | 唯一碰设备的地方（`I6`） |

### 2.4 节点只贡献四类事实

- 是什么：`Node` 是**位置**不是种类，只贡献 放置 / 子节点 / 状态贡献 / 载荷；折叠规则一条：
  子 = 父 + 自己（放置相乘、状态按文档化规则合并、层取并、可覆盖顺序）。
- 为什么：折叠是值运算 ⇒ 线程安全、可单测、可 fuzz（`G11`）；新节点行为 = 加一个放置值，不动折叠核心（`G9`）。
- 边界：`StateNode` / `MatrixTransform` / `ScreenPass` 在**快照**里不是"种类"而是同一概念的不同取值；
  作者层糖类保留。隐藏是**快照表示**（不产生行），`Node::setVisible` 保留，由发布编译成结构（`R5`）。

### 2.5 身份：注册表与四种 id

| id | 是什么 | 何时产生 | 唯一范围 |
|---|---|---|---|
| `NodeId` | **原型**的身份 | 构造时 | 内容域 |
| `InstanceId` | 树里的**一个位置**（引用原型 + 本地放置/覆盖） | 插入时 | 内容域 |
| `RowIndex` | 快照内的稠密序号（SoA 下标） | 发布时 | 一份 Snapshot |
| `PassId` / `TargetId` / `SurfaceId` | 会话内部句柄 | 计划对齐时 | 一次 Session |

- 是什么：内容域一个 `ContentRegistry`（槽分配 + generation）；64 位 = `generation:24 | domain:8 | index:32`；
  释放即换 generation，回绕即退休该槽；**只有注册表能造 id**（`C4` / `C5`）。
- 为什么：外部构建时还没有 Session，跨窗口共享必须是同一个 id ⇒ 内容域唯一（`G1`）；陈旧 id 答 gone，
  永不悬垂（`I2`）。
- 边界：id 是**值**，注册表是内容侧的账本；引擎侧**从不解析 id**，只按 index 查表并用 revision 校验。

### 2.6 原型与实例：折叠一次

- 是什么：树里存 `Instance`（`NodeId` + 本地放置/覆盖）；`rows(instance) = Placement(实例链) ∘
  PrototypeLocalRows(原型, revision)`；原型内部折叠算一次，v1 物化的放置成本是 N × 原型行数。
- 为什么：一个原型被任意多个位置引用（含跨 Scene）（`R2`），改原型所有实例一起变；共享 local rows
  （B 方案）能把静态实例压到 `O(rows + N)`，但要等测量（`G8`）。
- 边界：原型包含必须无环（插入时检测，`R7`）；行预算可观测（实例数 × 原型行数，超阈值报一条 episode，`R8`）；
  折叠缓存 v1 挂在 Snapshot 内。

### 2.7 驻留、管线与上传

| 决定 | 为什么 | 替掉什么 |
|---|---|---|
| 引擎表按**句柄 index** 索引，用 generation + revision 校验 | 数组、无哈希；旧帧读到旧版 | 地址 + `markUsed` 的寿命协商 |
| 引擎表只存**共享资源**，从不存每实例状态 | 表大小 = 原型数 × 版本数，不是实例数 | 每命令 touch 活体 |
| 驻留 ⟺ 被 Session 已接受或在飞的 Snapshot 引用它的 `(id, revision)`；宿主自持快照不钉住 | 精确规则，不看时间窗（`I4` / `R22`） | `parking` / `kept` 兜底 |
| 管线缓存键是**值**（program@rev, layout@rev, state, formats, samples） | 可复现、可共享、可单测 | 指针 + variant bits |
| 行/命令的载荷是**值**：`GeometryDescription` / `MaterialDescription` / `ShaderProgramDescription` + `ContentRef`；灯是 `LightValue` | 记录侧不读活对象，帧 N 画它描述时的那份值（`G1` / `G10`）；每命令省 3 次 `intrusive_ptr` 原子增减 | 活对象读（程序文本 / 材质 α 模式 / `Light` 字段） |
| 预热是一等入口：`prepare()` 答"需要什么" | 编译卡顿可预算，加载期可全预编译（`R23`） | 录制时才发现要编译 |
| 每帧 arena，帧内 arena 外零分配 | 分配上限从"规则"变常数（`R24`） | per-frame `vector` / `shared_ptr` |

### 2.8 计划是声明

- 是什么：`Pass = (name, View, Target, 状态覆盖, order, inputs)`；`Target = Surface{SurfaceId} |
  Transient{name, shape} | External{name}`；`inputs` 用稳定名字；一个设备 = 一个 Session = 多条 Surface。
- 为什么：宿主发布"我要的帧图"，Session 在帧边界按名字差分对齐 ⇒ 增删 pass 不需要任何 add/remove 调用，
  "帧在飞时改会话形状"**不可表达**（`R21`；`G12`）。
- 边界：`PassId` 只在 Session 内部；差分按**名字**不按指针（`C7`）；临时目标的生命周期由计划声明。
  计划只表达 `PassKind` 能表达的形状（`Scene` / `Screen`）：子类 pass 是 `describe()` 的逃生口，不在计划契约内
  ——同名同 kind 的声明原地更新会保留它（节点与 `describe()` 都活着），kind 变更会被 `applyPlan` 以
  `SubclassPassWouldBeRebuilt` 拒绝，而不是静默重建为基类。不建 `PassKind::Custom` 或工厂注册表（`G8`）。

### 2.9 派生是纯函数

- 是什么：`select(snapshot, view) → DrawList` 是纯函数；`record` 发生在后端 `execute` 内部；排序键是一个
  文档化的全序值（`draw_order, layer, opaque/transparent, 量化深度`）。
- 为什么：多视图并行、可 dump、可 replay、可跨后端比对（`R14` / `R30`）；"为什么这两帧不同"变成可重放的问题。
- 边界：视图相关行（billboard / 屏幕空间）逐视图重算，但不影响视图无关行的结果（`R16`）；后端若并行
  record，合并结果的确定性由后端自己证明（`R20`）。

### 2.10 五套过期答案的去向

| 今天 | 去向 | 由谁替代 |
|---|---|---|
| 地址 + 版本（引擎表） | 删 | 句柄 + generation + revision（2.5 / 2.7） |
| 镜像 + delta（`ContentMirror` / `DeltaChannel`） | 删 | 一次 `publish` 换指针（`R9`） |
| memo + epoch（`DrawListMemo` / `Version`） | 删 | Snapshot 是值，派生是纯函数（`R14`） |
| 引用计数（`markUsed`） | 已删 | 行级 epoch 回收（`F5`） |
| 停车窗口（`parked` / `kept`） | 留一半 | 仍有 GPU 对象替换（管线/描述符集/缓冲），但**驻留判定**不再靠它（`I4`） |

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 快照发布 | 分块写时复制的行表 + 素材值 + 空间索引 + 灯集，一次指针交换 | 部分：`detail::SceneSnapshot` 已发布并被 `SceneRows` 缓存，生产收集仍读 `SceneBake` |
| F2 | 纯派生 `select` | `selectSnapshot(snapshot, view)` 逐项等于现役收集 | 部分：实现存在，只有测试消费者 |
| F3 | 内容身份与世代 | `Handle` + typed id + `ContentRef`，陈旧句柄答 gone | 已落地 |
| F4 | 引擎表按 `(id, revision)` 驻留 | 事实表按 ref 建行；旧帧读旧版 | 已落地 |
| F5 | 行级 epoch 回收 | 行按"最后一次被计划命中的帧"回收；条目在"宿主不要了 ∧ 没有行"时离开 | 已落地 |
| F6 | 计划是值 | 帧边界按名字差分 + 计划校验稳定 code | 已落地（W9d，2026-11-13）：`Plan` + `validatePlan` + `applyPlan`（会话 / 作用域两种）+ `plan()` / `planFor(owner)`；`Target` 仍是显式 `RenderTarget` 句柄 |
| F7 | 预热入口 | `prepare()` 答需要什么；预热后录制 `builds` 增量 0 | 已落地：3a（`drawNeeds` 是唯一的键构造者）+ 3b（`RenderEngine::prepare()` → `prepareFrame(value)`，按同一份答案预建半片 / 集合 / 上传 / 管线身份） |
| F8 | 一 Session 多 Surface | `Target` / 帧请求带 `SurfaceId`；一个引擎线程驱动多个窗口 | 未做 |
| F9 | `publish` 返回结果 | 批内显式“未发布”，无 null | 已落地（5b-2a）：公开 `Scene::publish()` 三态（`Posted`/`Unchanged`/`InsideBatch`）；旧四态 `PublishOutcome` 已随 5b-2b 删除 |
| F10 | 原型与实例 | `Instance` 引用原型；折叠一次；无环与行预算 | 未做 |
| F11 | 全序排序键 | 文档化的全序值（含 layer），透明两段 | 未做 |
| F12 | 帧结果与查询 | `FrameResult` 带 snapshot / view / poses / 诊断 / present | 部分：`FrameStats` + `PresentOutcome` 已有，`query` 未做 |

### 3.2 F1 快照发布

- **行为**：`publish` 把可变场景变成一份不可变 Snapshot：行表按 32 行分块写时复制（只碰变了的块），
  素材值按 `(id, revision)` 取，空间索引与灯集随行一起发布。
- **约束**（`C1` / `C2`）：发布之后内容侧再改任何东西都不影响在飞的帧。
- **要求**（`R9` / `R10` / `R13`）：一次原子指针交换；成本 O(变了的块 + 变了的素材值)；快照可导出文本摘要。
- **验收**（`T1` / `T2` / `T3`）：`SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect`（≤ 250 µs，RelWithDebInfo）；
  在飞帧逐项不变。
- **注意**（`P2`）：元数据表（`id → revision`）不变时必须共享，否则 `K11` 会暴露整体拷贝。

### 3.3 F2 纯派生

- **行为**：`select(snapshot, view) → DrawList`，视图无关的折叠一帧一次，逐视图只付裁剪与排序。
- **约束**（`C8`）：跨层的值类型不带虚函数/继承。
- **要求**（`R14` / `R15` / `R16`）：同输入逐项相同；折叠计数 = 1；视图相关行逐视图重算。
- **验收**（`T8` / `T9`）：`SelectTest.SameInputSameList`（含并行）、`SelectTest.TheSortKeyIsATotalOrder`。
- **注意**（`P5`）：纯函数不等于免费——并行只在纯值上做（`I6`），设备侧仍是一条线程。

### 3.4 F3 内容身份

- **行为**：`Node` 构造即得 `NodeId`；`Material` / `Program` / `Texture` 各有 typed id；`ContentRef{id, revision}` 是跨层键。
- **约束**（`C4` / `C5`）：只有注册表能造 id；generation 回绕退休该槽。
- **要求**（`R1` / `R31` / `R32`）：身份与是否在树里无关；引用带 revision；查询返回位置身份。
- **验收**（`T5` / `T17`）：`ContentRegistryTest.AStaleHandleAnswersGone`（已实现）；旧帧逐项 diff。
- **注意**（`P1`）：**只带 id 的引用不是不可变值**——它把版本问题推给引擎表，直接破坏 `R9`。

### 3.5 F4 引擎表与 F5 行级回收

- **行为**：事实表（几何 / 材质 / 程序）按 `(id, revision)` 建行；行按"最后一次被计划命中的帧"回收，
  条目的 arena / 上传 / 纹理驻留由声明索引回答。
- **约束**（`C3`）：只有引擎线程建表与回收。
- **要求**（`R22` / `R23`）：驻留 ⟺ 活快照引用；预热后可预测。
- **验收**（`T6` / `T25`）：`ResidencyTableTest.TheCollectableSetIsEnumerable`；
  Session 外持旧快照再提交不 UAF。
- **注意**（`P6`）：行与 GPU 资源的寿命不是一个数——行可能比资源晚走一帧（在飞计划还会读它）。

### 3.6 F6 计划是值

- **行为**：`Plan` 是一个值（有序的 `PassDesc` 列表，pass 的**名字**是它在计划里的身份）；
  `applyPlan(plan)` 按名字差分：新名字新增、同名更新**原地**（节点与后端状态存活）、消失的名字移除；
  `plan()` 把当前形状读回成同一类型（`applyPlan(plan())` 是幂等的）。校验给出 `PlanProblemCode` 稳定 code
  （`EmptyPassName` / `DuplicatePassName` / `UnknownInput` / `DependencyCycle` / `TooManyPasses`），
  并整份接受或整份拒绝；另有一条**形状相关**的拒绝（`validatePlan` 看不见）：kind 变更会重建子类 pass 时以
  `SubclassPassWouldBeRebuilt` 拒绝。
- **约束**（`C7`）：按名字不按指针（作用域形态下是 `(owner, name)`）。
- **要求**（`R21` / `R38`）：增删 pass 不调用任何 add/remove；每条校验一个 code；`SessionBusy` 回答
  “引擎里有帧在录”，不做事后补救。**名字说明**：SDK 里今天没有 `Session` 类型；这个 code 的拼写保留只因
  测试钉住字符串，含义是“引擎忙”，不是某个 `Session` 对象的状态（`graphics-architecture.md` §2.8）。
- **验收**（`T7` / `T30`）：`PlanTest.APassIsAddedWithoutAnyAddCall`、`PlanTest.AChangedTargetUpdatesTheSameNodeInPlace`（P7）、
  `PlanTest.*IsRefusedWithItsCode`（四类 code）、`PlanTest.TheCodeSpellingsAreStable`。
- **注意**（`P7`）：差分必须对**同名不同内容**也成立（改一个 pass 的 target 不是“删一个加一个”）。
  计划不命名的 pass 会被移除（整会话形态；作用域形态只做 upsert）——这是“计划即形状”的直接后果。
  子类 pass 不再是可表达的形状：自定义在**数据**里（程序覆盖、视口、深度带、钩子），没有 custom-kind 注册表。
### 3.7 F7 预热入口

- **行为**：`prepare()` 回答“这一帧需要哪些半片 / 描述符集 / 上传”，并**把它们全部建好**，不提交帧。
  公开入口是 `RenderEngine::prepare()`（引擎先编成 `FrameDescription`，与 `frame()` 同一份编译），后端入口是
  `RenderFrameExecutor::prepareFrame(value, present_to)`——一条线、两个动词，不引入新名词；答案是一个值
  （`PrepareReport`：outcome + 走了几 pass / 几个 draw + 这次调用建了几个半片 / 集合 / 上传 / 管线身份）。
  快照进这个输入是 5b-1 的事（已落地）。
- **约束**（`C3`）：预热在引擎线程；它不提交帧，也不 acquire 交换链图像（见 `P11`）。
- **要求**（`R23` / `R36`）：预热后录制的帧 `builds` 增量 0；缺可选能力要出现在报告里。
- **验收**（`T7` / `T10` / `T32` / `T33`）：`builds` 增量断言；`prepare()` 报的键集 = 这一帧实际构建的键集。
- **注意**（`P8` / `P11`）：预热是**查询**不是“编译一切”；也**不能**用“跑一帧不提交”实现（见 `P11`）。
  **缝**：`drawNeeds(compiled pass, draw)` 是唯一的键构造者——`prepare` 用它建、`ContentPass::record` 用它取，
  两边不可能漂移（`G1`）。
  **落地位置**（3a，2026-11-12）：`src/plugins/gfx_backend_vsg/{include/vine/vsg/internal,src/internal}/DrawNeeds.{hpp,cpp}`
  （`drawNeeds` 两个重载 + `DrawNeeds::keyFor`）；`ContentHalves::halvesFor`、`ContentPass::recordCommand` /
  `recordScreenDraw` 与 `describeMissingContentHalf` 都只读它，录制路径不再自己拼键。
  **落地位置**（3b，2026-11-12）：`sdk/vine/graphics/PrepareReport.hpp`、`RenderFrameExecutor::prepareFrame`、
  `RenderEngine::prepare()`、`VsgBackend::prepareFrame`。后端复用帧自己的两半——`collectFrame`（值 → 采集）与
  `recordContent(..., /*prepare_only*/)`（采集 → 半片 / 集合 / 采样输入集 / 上传）——两个动词在 `ContentPass` 里
  只有一个分支点（`Mode::Prepare` 停在“身份已确保”处：不建命令图、不写块）。**预热不写块，也不开渲染环**：
  `FrameRing` 的 slab 轮转是帧的，预热多吃一格会让下一帧写到最老在飞帧仍在读的 slab（`core::FrameRing` 的
  多一格正是为此）——`T33` 用 `BlockStorage::writes()` / `frames()` 钉住这条。
  它不调用 `Session::beginFrame()`（窗口会 acquire 图像），只从 `WindowTarget::facts()` 读形状；
  `growBlockStorageIfNeeded` / `growMaterialArenaIfNeeded` 在预热里先跑，免得太帧第一次就换存储、把刚建好的集合全部重指。

### 3.8 F8 一设备一 Session 多 Surface

- **行为**：`Target = Surface{SurfaceId}`；一个 Session 驱动多个窗口，一条引擎线程。
- **约束**（`I6` / `R27`）：任何实现需要第二条引擎线程就违反本设计。
- **要求**（`R21` / `R37`）：`Target` / 帧请求带 `SurfaceId`。
- **验收**（`T29` / `K10`）：一个 Session 两个 Surface 都正确 present。
- **注意**（`P4`）：表面事件只在帧首应用；交换链只在该表面自己的帧里重建。

### 3.9 F9 `publish` 返回结果

- **行为**：一次发布回答三种状态之一（`Posted` / `Unchanged` / `InsideBatch`），并带着"这次调用之后作为答案的
  那一份"（`ScenePublishResult::snapshot`）；`InsideBatch` 是批内显式"未发布"，答案仍是批开始时的那份。
  外层 `endEdit` 之后的那一帧才发布整批，`publishCount` 只 +1。没有 `Refused`：发布无处可拒——它不排队。
- **约束**（`C2`）：一个 Scene 同时只有一个写者。
- **要求**（`R11` / `R39`）：`publishCount` 只 +1；没有 null，也没有一个 `bool` 同时表示"没变"和"批内"。
- **验收**（`T3` / `T31`）：`PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch`（三态 + 句柄的
  revision 走向）、`SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture`。
- **注意**（`P9`）：批不是事务——它只界定"一帧能看见什么"。公开 `Scene::publish()` 随快照（5b-2a，已落地）出现：
  在那之前，公开面没有一个能承载“上一份快照”的类型，先公开它只会多一个没人能用的版本号。

### 3.10 F10 原型与实例

- **行为**：`Instance` 引用原型；组装的 DAG 在发布时展平成行树；原型内部折叠算一次。
- **约束**（`C6`）：原型包含必须无环。
- **要求**（`R2` / `R3` / `R4` / `R7` / `R8` / `R33` / `R34`）：多位置引用、折叠一次、寿命明确、无环、行预算、
  同一写者域、不允许悬垂 `NodeId` 进入发布。
- **验收**（`T4` / `T21` / `T22` / `T23`）：折叠计数用例；共享原型发布；原型析构后仍有实例；实例化成本与 `K11`。
- **注意**（`P3`）：v1 的放置成本是 N × 原型行数，不是 N——不要把它写成"实例化免费"。
  A/B 的测量口径见 `../research/graphics-2026-11-12-instances.md`（`T23`）；B 只在数字满足判定线后成为默认。

### 3.11 F11 / F12 排序键与帧结果

- **行为**：排序键是**文档化的全序值**（`draw_order, layer, opaque/transparent, 量化深度`），不透明与透明两段；
  `FrameResult` 带呈现了哪个 Snapshot / View、耗时、诊断、`query`（拾取 / 锚点 / fit 吃它）。
- **约束**（`C8`）：键是值，可序列化、可比较。
- **要求**（`R17` / `R18` / `R19` / `R30`）：跨机器顺序一致；查询吃 `FrameResult.query`；返回位置身份。
- **验收**（`T9` / `T18` / `T19` / `T20`）：排序全序；多实例拾取；pose 后拾取；帧相机拾取。
- **注意**（`P10`）：`query` 里的 View 是**当时那一帧**的值；拿活相机去查是"瞄错"，不是"更新一下"。

## 4. 公开接口

> 目标是目标签名；每一条以落地后的头文件为准。迁移期按 `C11` 只增不改。
>
> **迁移 API 状态（`W2`，2026-11-13；W9b 后更新）**：`applyPlan` / `plan` / `prepare` 在 `src/` 里没有宿主
> 调用点（只有引擎内部的转发与自测），它们是**迁移入口，尚未成为宿主契约**；5b-2c 前不再新增公共入口。
> `bindSnapshot` 自 W9b 起只是**覆盖**（帧默认取 `Scene::publishedSnapshot()`），生产路径是
> `SceneView::publishContent` + `applyPlan`。`FramePlan` 的改名（→ `FrameTable`）与
> `SessionBusy` 的去留（随 `W7` 移出 `PlanProblemCode`）都在 5b-2c 窗口内一次改完
> （见 `graphics-architecture.md` §2.10）。

### 4.1 内容侧

| 接口 | 契约 | 备注 |
|---|---|---|
| `Scene::publish() → Result<SnapshotRef>` | 发布一次快照；批内返回显式"未发布" | 目标签名，见 `Scene.hpp` |
| `Scene::beginEdit()` / `endEdit()` | 批的边界；只有最外层释放版本 | 已存在 |
| `Node::id() → NodeId` | 构造即有效 | 已存在 |
| `Node::setVisible(bool)` | 隐藏由发布编译成"不产生行" | 已存在 |
| `ContentRegistry::allocate<Tag>() / release(handle) / alive(handle)` | 唯一 id 来源；陈旧句柄答 gone | 已存在（`ContentId.hpp`） |
| `ContentRef{Handle id, uint64 revision}` | 跨层键；`contentRefOf()` 是唯一构造 | 已存在 |
| `Geometry::description()` / `MaterialBase::description()` | 按 revision 缓存的不可变值 | 已存在 |
| `ShaderProgram::description() → ShaderProgramDescriptionPtr` | 程序文本 + id + revision；按 revision 缓存 | 已落地（W1） |
| `Light::value() → LightValue` | 灯的值半边；pass 描述自己时快照成 `DrawDescription::light_values` | 已落地（W1） |

### 4.2 派生层

| 接口 | 契约 | 备注 |
|---|---|---|
| `detail::selectSnapshot(const SceneSnapshot&, const CollectView&, ...)` | 纯函数；内部原型 | 已存在（`SceneSnapshot.hpp`） |
| 排序键 | 全序值 + 两段；比较规则文档化 | 目标，见 §3.11 |

### 4.3 会话侧

| 接口 | 契约 | 备注 |
|---|---|---|
| `RenderEngine::prepare()` → `PrepareReport` | 只建不提交；答所需半片/集合/上传（**迁移 API，尚未成为宿主契约**，`W2`） | 目标（后端拿到的是与 `executeFrame()` 同一个 `FrameDescription`；快照进这个输入是 5b-1，已落地） |
| `Session::frame(FrameRequest) → FrameResult` | 提交一帧；`FrameRequest` 带 `View` 与 `SurfaceId` | 目标（今天 `executeFrame` + `presentWindow`） |
| `FrameResult{ snapshot, view, poses, stats, diagnostics, present }` | 宿主读的值 | 目标（今天 `FrameStats` + `PresentOutcome`） |
| `Plan` / `Pass` / `Target` / `SurfaceId` | 帧图声明（W9d 起是唯一的形状；`FramePlan` 将在 W9e 改名 `FrameTable`） | 目标 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 跨层的一切类型不可变；可变对象只活在内容侧。 |
| C2 | 一个 Scene 同时只有一个写者；写者交接必须有 happens-before。 |
| C2b | 一个原型和所有引用它的 Scene 属于同一写者域；跨线程共享可变原型不支持。 |
| C3 | 只有一条引擎线程碰设备；并行只发生在派生层。 |
| C4 | 身份只由注册表生成，没有第二个入口。 |
| C5 | `generation` 回绕时退休该槽，不复用。 |
| C6 | 原型包含无环。 |
| C7 | 计划按稳定名字差分，不按指针。 |
| C8 | 跨层的值类型不得带虚函数/继承。 |
| C9 | 诊断走单一路由，带 episode。 |
| C10 | 不新增第三方依赖。 |
| C11 | 迁移期公开 API 只增不改。 |
| C12 | 每一步都必须能独立判死（阈值见 §8 与 ADR-2026-11-12）。 |

### 5.2 要求（R#）

| # | 要求 | 判据 |
|---|---|---|
| R1 | `Node` 的身份构造时存在 | 构造后即可取 `NodeId`，插入前后相同 |
| R2 | 原型可被任意多个位置引用 | 同一 `NodeId` 两处，改原型两处同时变 |
| R3 | 原型内部折叠算一次；v1 放置成本 N × 原型行数 | `T4` 分开计 |
| R4 | 树里存位置不存节点本体；换父不改身份 | `T22` |
| R5 | 隐藏 = 不产生行 | 隐藏子树无行；作者 API 不变 |
| R6 | 四类事实是值；折叠是值运算 | 折叠函数无副作用 |
| R7 | 原型包含无环 | 自包含被拒并报诊断 |
| R8 | 行预算可观测 | 超预算一条 episode |
| R9 | 发布是一次原子指针交换；旧帧读旧值 | `T2` / `T17` |
| R10 | 发布成本 O(变了的块 + 素材值) | `T1` / `N2` / `K11` |
| R11 | 批内不发布；一次批 = 一次发布 | `T3` / `T31` |
| R12 | 素材改动不需要调用方通知 | 发布时按 `id → revision` 比对 |
| R13 | 快照可导出文本摘要 | 一条命令可打印并被断言 |
| R14 | `select` 是纯函数 | `T8` |
| R15 | 视图无关折叠一帧一次 | `T8` 计数 |
| R16 | 视图相关行逐视图重算 | 混合用例 |
| R17 | 排序键是全序值并文档化 | `T9` |
| R18 | 查询吃 `FrameResult.query` | `T19` / `T20` |
| R19 | 拾取返回位置身份，不含指针 | `T18` |
| R20 | 后端并行 record 时合并结果确定 | 并行 = 串行逐项 |
| R21 | 会话形状是声明，按名字差分；`Target` 带 `SurfaceId` | `T7` / `T29` |
| R22 | 驻留 ⟺ 活快照引用；宿主自持快照不钉住 | `T25` |
| R23 | 管线键是值；预热是一等入口 | 预热后 `builds` 增量 0 |
| R24 | 帧内 arena 外零分配 | 分配计数 |
| R25 | 能力分必选/可选，创建时协商并说明 | `T10` |
| R26 | 关闭有显式相位，可重入 | `T11` |
| R27 | 一个设备一个 Session，可驱动多表面 | `T29` / `K10` |
| R28 | 诊断带稳定 code 与结构化字段 | 按 code 断言 |
| R29 | 单位与颜色空间是类型/值 | 传错编译失败 |
| R30 | 帧值可 replay 出同一命令序列 | `T12` |
| R31 | 内容引用带 revision | `T17` / `K7` |
| R32 | 拾取/诊断的身份是位置 | `T18` |
| R33 | 共享原型属于一个写者域；发布能发现原型 revision 变化 | `T21` / `K8` |
| R34 | `Instance` 对原型有存活责任；不允许悬垂 `NodeId` | `T22` |
| R35 | `PoseSet` = 实例 local 覆盖 + 子树传播 | `T26–T28` / `K9` |
| R36 | 预热是会话一等入口；缺可选能力出现在报告里 | `T7` / `T10` |
| R37 | 一个 Session 多 Surface；`Target`/帧请求带 `SurfaceId` | `T29` / `K10` |
| R38 | 计划校验有稳定 code | `T30` |
| R39 | `publish` 在批内不返回 null | `T31` |

### 5.3 不变量（I#）与可测阈值（N#）

| # | 不变量 | 它杀哪类 bug |
|---|---|---|
| I1 | 跨层一切都是不可变值；可变对象只活在内容侧 | 活对象过帧 |
| I2 | 每个句柄带 generation；陈旧句柄答"已消失" | UAF / 地址复用 |
| I3 | 一个 Scene 同时只有一个写者；发布是一次指针交换 | 批中泄漏、并发写 |
| I4 | GPU 资源存活 ⟺ 被 Session 已接受或在飞的 Snapshot 引用 | 驻留猜、`kept` 兜底 |
| I5 | 派生是纯的：同输入 ⇒ DrawList 逐项相同 | "为什么这两帧不同" |
| I6 | 只有一条引擎线程碰设备 | 设备层竞争 |
| I7 | 屏幕性查询吃 `FrameResult` 里的 View 值 | 瞄准错 |

| # | 阈值 | 怎么测 |
|---|---|---|
| N1 | 相机移动的 collect ≤ 今天 2160 µs × 1.2 | `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` |
| N2 | 改一行的发布 ≤ 250 µs @ 2000 行 | `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` |
| N3 | 快照常驻 ≤ 2× 行表字节数 | `alloc_trace.c` / RSS |
| N4 | TSan 0 条警告 | `scripts/tsan_check.sh` |
| N5 | 既有用例语义断言改动 ≤ 5% | 直接数 |
| N6 | 帧内 arena 外 0 次分配 | 分配计数 |
| N7 | 头文件依赖不增 | `check_include_hygiene.py` |
| N8 | Windows / Linux 同一套代码 | CI |
| N9 | 不引入第三方依赖 | review + `_vcpkg.json` |
| N10 | 每帧可打印摘要，成本 < 0.1% | 计时 |

## 6. 注意事项与陷阱

### 6.1 P1 只带 id 的引用

- 会怎么错：旧帧按 id 解析到新值，画面里一半旧一半新，且没有任何拒绝。
- 怎么判：`T17` 逐项 diff 在飞帧命令。
- 怎么防：键里必须有 revision（`R31`）；`ContentRef` 是唯一构造入口。

### 6.2 P2 元数据表被整体拷贝

- 会怎么错：行 CoW 很漂亮，发布成本却随行数线性上升，`K11` 失败。
- 怎么判：5000 原型 1 次编辑的 publish 时间与字节。
- 怎么防：表未变时共享 `shared_ptr<const Table>`；变时只替换受影响的表（`R10`）。

### 6.3 P3 把实例化当免费

- 会怎么错：5000 行原型插 10 次，被写成 10 次折叠，实际是 5 万次放置乘法。
- 怎么判：`T4` 把"折叠次数"与"放置乘法次数"分开计。
- 怎么防：v1 明确声明 N × 原型行数；B 方案（共享 local rows）要等 `T23` 测量后再选。

### 6.4 P4 表面事件进帧中

- 会怎么错：交换链在录制中途重建，在飞命令引用已销毁的视图。
- 怎么判：多表面 + resize 用例；TSan。
- 怎么防：表面事件只在帧首应用（既有规则，升格为设计的一部分）。

### 6.5 P5 把"纯函数"读成"可以随便并行"

- 会怎么错：把 record 也并行，两个线程同时碰 `vsg::Context`。
- 怎么判：`I6` 的代码审查 + TSan。
- 怎么防：并行只在派生层；后端并行自己的 record 时必须自证确定性（`R20`）。

### 6.6 P6 行与资源的寿命混为一谈

- 会怎么错：行还在答（在飞计划要读），资源先被放掉。
- 怎么判：`T25`（Session 外持旧快照再提交）。
- 怎么防：驻留判据只看"活快照引用"（`I4`），行只决定"事实还在不在"。

### 6.7 P7 计划差分对"同名不同内容"失效

- 会怎么错：改一个 pass 的 target 被当成"删一个加一个"，管线与目标每帧重建。
- 怎么判：`T7` 的 `builds` 增量断言。
- 怎么防：差分比较整条 `Pass` 值（含 `Target` / `inputs` / 状态覆盖），名字只是身份。

### 6.8 P8 预热被做成"编译一切"

- 会怎么错：加载期把全部变体都编一遍，冷启动炸掉。
- 怎么判：预热后 `builds` 增量与预热耗时。
- 怎么防：`prepare()` 只答这一份输入要的（`R23`）。

### 6.9 P9 批被当成事务

- 会怎么错：调用方期望批内失败回滚，而批只界定可见性。
- 怎么判：`T31` 的语义。
- 怎么防：文档写清"批不是事务"（`Scene::beginEdit` 的既有注释）。

### 6.10 P10 拿活相机查询

- 会怎么错：相机在帧间移动，查询用新相机回答上一帧的画面。
- 怎么判：`T20`。
- 怎么防：查询吃 `FrameResult.query` 的 View 值（`I7`）。

### 6.11 P11 用“跑一帧但不提交”实现预热

- 会怎么错：窗口帧的 `Session::beginFrame()` 里 `viewer->advanceToNextFrame()` **acquire** 一张交换链图像；
  不提交就再也不会有图像可 acquire（下一帧饿死），提交则已经不是预热。而画到窗口的 pass 只能画进那张图像，
  所以“跑一遍录制再丢掉”连它都覆盖不到 —— 预热会在看起来通过的像素用例下留下一条 acquire 饥饿。
- 怎么判：`builds` 增量断言不成立，或第二帧 `beginFrame()` 回空 token / 出现 acquire 超时的 VUID。
- 怎么防：预热只做**值**这一侧（半片 / 集合 / 上传），键由 `drawNeeds` 算；窗口目标的格式由
  `WindowTarget::facts()` 回答（const，不需要 acquire）。3b 的实现在 `VsgBackend::prepareFrame`：它走 `collectFrame`
  + `recordContent`，**从不**调用 `Session::beginFrame()` / `PresentTarget::present()`，帧进度水位（`submitted` /
  `presented`）因此不动——`T33` 断言的正是这两条。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 让内容模型本身不可变（全持久化场景图） | 作者要可变；把复杂度推给每个使用者（`G7`） | ADR-2026-11-12 |
| D2 | 内容侧多线程写（给场景图加锁） | 收益为零、错误成本极高；改为单写者 + happens-before | 同上 |
| D3 | 允许 `Node` 多父（OSG 式 DAG） | 状态沿多路径累积、遍历按实例指数扩张、行身份只能靠路径 | 同上 |
| D4 | 保留"地址即身份" | 省一张注册表，卖掉的是一整类 UAF | 同上 |
| D5 | id 由 Session 分配 | 外部构建时还没有 Session；跨窗口共享必须是同一个 id | 同上 |
| D6 | 默认 bindless / GPU-driven | 没有第二个后端、没有实测瓶颈（`G8`） | 同上 |
| D7 | 本层做自动 barrier 的 render graph | 同步归后端（`G2`） | 同上 |
| D8 | 保留 `addPass` 家族 + 运行时拒绝 | 计划是值之后误用不可表达；拒绝路径本身是复杂度 | 同上 |
| D9 | 内容引用只带 id、不带 revision | 旧帧会解析到新值，破坏 `R9`/`I1` | 同上 |
| D10 | 只做已落地的身份/寿命层，其余留作提案 | "发布 = 一次指针交换"不成立，镜像/delta/memo 继续在轨 | ADR-2026-11-12 §选项 D |
| D11 | 用“跑一帧但不提交”实现预热 | 窗口帧的 `beginFrame` acquire 图像：不提交则饥饿，提交则不是预热；画到窗口的 pass 连覆盖都做不到（`P11`） | ADR-2026-11-12 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 | 状态 |
|---|---|---|---|
| T1 | 发布是 O(变更) | `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` | 已实现 |
| T2 | 在飞帧不受内容改动影响 | `RenderEngineTest.AMaterialEditLandsOnTheNextFrame...` + 场景版 | 已实现 |
| T3 | 批 = 一次发布 | `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture` + `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` | 已实现（2026-11-12） |
| T4 | 原型折叠一次、放置成本单计 | `SnapshotTest.APrototypeFoldsOnceAndIsPlacedNTimes` | 未做 |
| T5 | 陈旧句柄答 gone | `ContentRegistryTest.AStaleHandleAnswersGone` | 已实现 |
| T6 | 驻留 = 活快照引用 | `ResidencyTableTest.TheCollectableSetIsEnumerable` | 已实现 |
| T7 | 计划差分 | `PlanTest.APassIsAddedWithoutAnyAddCall` | 已实现 |
| T8 | 派生纯 | `SelectTest.SameInputSameList`（含并行） | 未做 |
| T9 | 排序全序 | `SelectTest.TheSortKeyIsATotalOrder` | 未做 |
| T10 | 能力协商 | `SessionTest.AMissingRequiredCapabilityIsRefusedWithACode` | 未做 |
| T11 | 关闭相位 | `SessionTest.ShutdownIsIdempotentInEveryPhase` | 未做 |
| T12 | replay | `ReplayTest.ARecordedFrameReplaysToTheSameCommands` | 未做 |
| T13 | 并发 | `scripts/tsan_check.sh` | 每步收尾跑 |
| T14 | 既有行为不变 | `ctest`（30 套） | 30/30 |
| T15 | 文档门禁 | 八个 `scripts/check_*.py` + `check_vsg_content_seam.cmake` | 全清 |
| T16 | 量化"过/不过" | `../research/graphics-2026-11-11-next-benefits.md` 收益表 | 逐步记录 |
| T17 | 旧帧读旧 revision | 两帧在飞时改材质/几何/程序，逐项 diff | 已实现（`SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith`，2026-11-12） |
| T18 | 多实例拾取 | 同一原型两处实例 | 未做 |
| T19 | pose 后拾取 | 父关节 pose 点其子 | 未做 |
| T20 | 帧相机拾取 | 相机移动后按下 | 未做 |
| T21 | 共享原型发布 | 两 Scene 共享原型 | 未做 |
| T22 | 原型寿命 | 原型析构后仍有实例 | 未做 |
| T23 | 实例化成本（A/B 对照） | 5000 行原型 × 10 实例；报 `snapshot_rows` / `draw_rows` / `placement_multiplications` / publish / select / 常驻；口径见 `../research/graphics-2026-11-12-instances.md` | 未做（research） |
| T24 | 块级剔除正确 | 一块两段不同父变换 | 未做 |
| T25 | 回收后旧快照 | Session 外持旧快照再提交 | 未做 |
| T26 | pose 子树传播 | 父关节 pose | 未做 |
| T27 | pose 拾取 | pose 后拾取子节点 | 未做 |
| T28 | pose 剔除 | pose 子树部分出视 | 未做 |
| T29 | 多表面 | 一个 Session 两个 Surface | 未做 |
| T30 | 计划校验 | `PlanTest.*IsRefusedWithItsCode`（四类）+ `PlanTest.TheCodeSpellingsAreStable` + `PlanTest.AKindChangeOnACustomPassIsRefusedWithItsCode` | 已实现 |
| T31 | 批内 publish | `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch`（`Posted` → `Unchanged` → `InsideBatch` → `Posted`）+ `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture` | 已实现（2026-11-12） |
| T32 | 预热的答案 = 帧的构建 | `ContentHalvesTest.TheReportedNeedsAreWhatTheFrameBuilds`（键集相等，设备无关）+ `ContentPassTest.AMultiLayoutScopeServesEveryHalfItWasBuiltFor` 的键集断言（录制侧） | 已实现（2026-11-12） |
| T33 | 预热后录制 `builds` 增量 0，且预热不提交 / 不呈现 | `VsgBackendTest.PrepareBuildsEveryIdentityAndTheNextFrameThenBuildsNothing`（`halves().builds` / `sets().builds` / `uploads().uploads` / `inputSetBuilds` / `pool.created` 全 0，`progress->submitted` 不动，像素仍画出）+ `HeadlessBackendTest.PrepareHandsOverTheSameFrameTheNextFrameRunsAndRunsNothing`（引擎侧：同一个 value、不执行、不取帧号） | 已实现（2026-11-12） |
| T34 | 快照源与直接收集逐项相等 | `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames`（600 帧，transform / opacity / material / placement 编辑，逐项 diff）+ `FrameExpandTest.AFrameExpandsTheSamePictureTheSceneWould`（绑定后的帧级接缝） | 已实现（2026-11-12） |

### 8.1 判死阈值（`K#`）

每一步的**验收阈值与回退条件**（ADR-2026-11-12 决定 `K1–K11` 的身份从此是这一条，而不是"否决提案"）。
某一步击穿它自己的 `K` ⇒ 回退该步并把结论写回 ADR，**不重开"是否采纳"**。

| # | 阈值（击穿 = 回退该步） | 怎么测 |
|---|---|---|
| K1 | 2000 行场景改 1 行，`publish` > 500 µs（CoW 退化成整表） | `SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` |
| K2 | 快照常驻内存 > 3× 行表字节数 | `alloc_trace.c` / RSS |
| K3 | 相机移动的 collect > 2160 µs 的 1.2 倍 | `SceneTest.MeasureWhatOneCollectionCosts*` |
| K4 | 该子步开始时的用例里，> 5% 需要改**语义断言**才通过（纯改名/机械迁移不计；口径见下） | 直接数 |
| K5 | 代码量不降反升（桥 + 寿命层 > 今天的 100%，与 2519 行同口径） | `wc -l` |
| K6 | TSan 出现任何**新的**数据竞争报告 | `scripts/tsan_check.sh` |
| K7 | 在飞帧的旧内容被新发布改变（材质/几何/程序任一） | `T17` 逐项 diff |
| K8 | 两个 Scene 共享原型时数据竞争，或跨线程用例无法被拒绝 | `T21` + TSan |
| K9 | pose 后的拾取/剔除与画面不一致 | `T19` / `T27` / `T28` |
| K10 | 多表面必须靠两个 Session / 两条引擎线程才能成立 | `T29` |
| K11 | 5000 原型、1 次编辑的 publish 超过预算（元数据整体拷贝暴露） | `N19` ≤ 50 µs 或字节上限 |

**`K4` 的计数口径**（5a / 5b-1 / 5b-2a / 5b-2b / 5b-2c 各自适用）：

- **分母**：该子步开始时 `test_graphics` + `test_vsg` 的 `--gtest_list_tests` 用例并集；`DISABLED_` 不计，
  运行时 SKIP 计；不数源码里的 `TEST` 宏（今天 `test_vsg` 源码宏 544、可执行 542）。
- **分子**：必须改期望值 / 可观察行为才通过的用例；改名、别名、签名/类型机械迁移、文件搬移不计。
- 只钉被删机制、没有后继语义的用例随机制删除，但每条要写明由哪条新用例承接同一可观察行为；
  写不出承接的，算 1 条。
- **5a** 的既有语义断言改动必须为 **0**（影子路径；出现 1 条就回退 5a，不进 5b-1）；
  **5b-1 / 5b-2a / 5b-2b / 5b-2c** 的分母各自在开始时重取，不沿用上一步。
- 今天（3b 收尾）实测 572 + 542 = **1114**，5% 线为 **55 条**（第 56 条越线）；这个数字只作初判，不写死。

## 9. 状态与演进

### 9.1 当前状态

- **已落地**：F3（身份）、F4（引擎表按 ref）、F5（行级 epoch 回收）、F6（计划是值）、F7（预热入口，3a + 3b）。
  前三项在 `ContentId.hpp` / `ContentStore.cpp` / `ResourceTable.hpp` 里是现役代码，`test_graphics` 与 `test_vsg`
  在钉它们；F6 在 `Plan.hpp` / `Plan.cpp` / `EngineCompile.cpp`（`applyPlan`）里，由 `PlanTest` 钉住；F7 在
  `DrawNeeds.*`（身份）与 `RenderEngine::prepare()` / `RenderFrameExecutor::prepareFrame` / `VsgBackend::prepareFrame`
  （预建）里，由 `T32` / `T33` 钉住。
  F6 的**半边**：`Target` 还不是值（仍是显式 `RenderTarget` 句柄）；`addPass` 家族已删（W9d）。
- **快照已是生产收集（5b-1 / 5b-2a，2026-11-12）**：F1（快照发布）、F2（纯派生）由 `SnapshotSource` 回答
  Published 模式的每个 draw；`Scene::collectRenderCommands` 仍是 Direct 模式的直接路径；`K1`（203 µs）、
  `K2`（1.03×）、`K3` 已由 `SnapshotTest` 证明。
- **5a / 5b-1 / 5b-2a / 5b-2b 已落地（2026-11-12）**：`SnapshotSource`（快照 → `DrawListSource`）、
  `DrawListDiff`；`T34` 由 `SnapshotSourceTest` 的 600 帧与 `FrameExpandTest` 的帧级比较钉住，`T17` 的
  snapshot 侧由同一套件的旧快照用例钉住；公开 `Scene::publish()`（`Posted`/`Unchanged`/`InsideBatch`）；
  `PublicationStore` 直接持有 `SnapshotSource`（W3 拆自 `FrameWorker`）。**5b-2b 删除了 `ContentMirror` / `DeltaChannel` / `RenderWorld` /
  `RenderWorldSource` / `PublishOutcome` 与 23 条 LEGACY 用例**；`test_graphics` 554 / `test_vsg` 542。
- **未做**：F8、F10、F11 与 F12 的 `query` 半边。F7 的两半都已落地：3a 的答案（`drawNeeds` 是唯一的键构造者）
  与 3b 的预建（`RenderEngine::prepare()` → `prepareFrame(value)`；预热后录制的帧 `builds` 增量 0，见 `T33`）。
  F9 的三态公开答案（`Posted` / `Unchanged` / `InsideBatch`）与它携带的快照句柄已落地（5b-2a + 2026-11-12 的
  `SceneSnapshotRef`）。**帧内隐式发布已删除（W9b，2026-11-13）**：发布是内容写者的动词，帧在 build 时取
  `Scene::publishedSnapshot()`；`bindSnapshot` 只剩覆盖一个含义（§2.13 of `graphics-threading.md`）。
- **迁移期的事实**：`graphics-threading.md` 已于 2026-11-12 重写为**当前**契约（发布取代镜像/delta，
  `C1–C15` / `N1–N14` 作为历史编号退役不复用）；`graphics-scene.md` 的收集描述仍带镜像时代的措辞，等它
  下一次重写时对齐。本文件是**目标**，两者冲突时以状态字段为准，接管顺序见 §9.3。

### 9.2 已知缺口

- `N2` 的 50 µs 不在本轮：它需要内容侧 dirty 通知，与"Node 无 Scene 反向指针"（`C2`）直接冲突。
  当前门是 ≤ 250 µs（原型实测 203 µs）；50 µs 见 `../backlog/graphics.md`（触发条件型）。
- `-design` 的 pose（`PoseSet`）、后期依赖图、replay 后端的算法细节仍只在
  `../research/graphics-2026-11-11-next-hot-paths.md` / `../research/graphics-2026-11-11-next-design.md`，
  按步吸收，不预先建体系（`G8`）。

### 9.3 兼容性 / 迁移

每一步一个可回退提交，收尾沿用仓库节奏（全量构建 → `test_graphics` / `test_vsg` / `ctest` →
`scripts/tsan_check.sh` → 文档门禁）。次序即依赖：

| 步 | 内容 | 删掉什么 | 判据 / 回退条件 |
|---|---|---|---|
| 1 | 计划是值：`Plan` + 帧边界差分 + 校验 code · **已落地 2026-11-12** | 本步不删公开入口（`C11`）；`compileFrame` 的行从“`slots_` 的投影”变成“计划的投影” | `T7` / `T30` / `K4`；击穿则回退该步 |
| 2 | `publish` 返回结果（四态）· **已落地 2026-11-12（内部半边）** | 内部 `bool` 发布（旧 `FrameWorker` / `ContentMirror`，前者已拆为 `PublicationStore` + `FrameExpand`）；**帧内隐式发布随 5b-2c 消失** | `T31` |
| 3a | `prepare()` 的**答案**：抽出 `drawNeeds`，`prepare` 与 `record` 共用它；断言答案 = 这一帧实际构建的键集 · **已落地 2026-11-12** | 录制里重复的键构造 | `T32`；击穿则回退该步 |
| 3b | `prepare()` 的**预建**：按答案建半片 / 集合 / 上传 · **已落地 2026-11-12** | “录制时才发现要编译” | `T7` / `T10` / `T33`，预热后 `builds` 增量 0 |
| 4 | 一 Session 多 Surface | 每窗口一个 Session 的驱动路径 | `T29` / `K10` |
| 5a | **影子切换**：`selectSnapshot` 与旧 mirror 路径同帧同输入跑，逐项 diff；旧路径仍是渲染真相 · **影子接缝已落地 2026-11-12** | 本步不删生产形状；不公开 `Scene::publish()`；不删 `addPass` 家族（`C11`） | `T34` / `T17`（snapshot 侧）已由 `SnapshotSourceTest` 钉住，CI 常开；`K1` / `K2` / `K3` / `K7` 绿、既有语义断言改动为 0 才进 5b-1 |
| 5b-1 | **切换真相**：Published 模式绑定 `SnapshotSource`；旧 mirror 降为可选 LEGACY 影子，默认关闭 · **已落地 2026-11-12** | 无（旧路径保留作影子，不删） | `T34` / `T17` / `K1` / `K2` / `K3` / `K7`；`test_graphics` 577 / `test_vsg` 542 / ctest 30/30 / TSan 0 |
| 5b-2a | **公开入口 + 源归位**：公开 `Scene::publish()` 三态，`PublicationStore` 直接持有 `SnapshotSource`（W3 拆自 `FrameWorker`）· **已落地 2026-11-12** | 无（LEGACY 文件仍在树内，但已不在生产路径） | `T31` / `test_graphics` 577 / `test_vsg` 542 / ctest 30/30 / TSan 0 |
| 5b-2b | **删除旧路径**：删镜像/delta/版本与镜像的 refresh/delta 入口 · **已落地 2026-11-12** | `ContentMirror`、`DeltaChannel`、`RenderWorld`、`RenderWorldSource`、`PublishOutcome`、`SceneRows::version/publish/editing`（约 2000 行，含 23 条 LEGACY 用例）；`DrawListMemo` **保留**为 `SnapshotSource` 的纯 per-view 缓存；`Scene::refreshBoxes/refreshMaterials` 保留（快照的输入） | `test_graphics` 554 / `test_vsg` 542 / ctest 30/30 / TSan 0 |
| 5b-2c | **收口 pass 声明**：删 `addPass` 家族与 `PassRegistry` 的运行时拒绝，计划是唯一的形状 · **W9d 已落地 2026-11-13** | `addPass` / `removePass` / `clearPasses` 删除；`PassChain` / `DemoScene` / 链预设 / 测试改走 `applyPlan`；作用域形态让一个引擎可以有多个贡献者 | `K4` / `K5` / `T7` / `T30` |
| 6 | 原型与实例（单独实验，B 方案先测） | 无（新增能力） | `T23` / `K11`；数字不好就停在“原型 DAG 共享” |

`C11`（迁移期公开 API 只增不改）适用于第 1–4 步、5a、5b-1、5b-2a 与 5b-2b：新入口先与旧入口并存，5b-2c 收口时再删旧形状。
所以“`addPass` 家族在第 1 步删”与 `C11` 自相矛盾，本表已改成“5b-2c 删”：第 1 步只改变投影方向
（计划是输入，`slots_` 是它的应用结果），这也正是 `T7` 能成立的最小充分条件。
5a 是**等价性证明**，5b-1 是**切换真相**，5b-2a 是**公开入口 + 源归位**，5b-2b 是**删旧镜像**，
5b-2c 只做**删 pass 声明旧形状**；各自一个可回退提交，理由见 `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`。

**W9a（W6b）已落地 2026-11-13**：`Scene` 不再实现 `DrawListSource`，`RenderPass::describe` 不再把活 `Scene`
写进帧值，绑定移到内容半段（`detail::bindSceneDraws`），`FrameExpander` 随之删除（它只有 bind 一个职责）。
帧值跨线程时只装发布源；未发布的场景什么也不画（`FrameExpandTest.ADrawNoPublicationAnswersDrawsNothing`）。

**W9b（隐式发布）已落地 2026-11-13**：`RenderEngine` 不再发布任何场景。`Scene::publishedSnapshot()` 是帧默认取的
那一份；`PublicationStore` 按 `(content, revision)` 一个源，所以在飞帧不会被新发布扰动；`bindSnapshot` 降为覆盖。
宿主侧：`SceneView::publishContent()`（视图 + 链拥有的场景）、`PassChain::publishContent()`，`SurfaceWindow::renderFrame`
每帧调用前者；overlay 的 `before_draw` 钩子在 `update()` 里自行发布。没发布的场景不画并报一次 `ContentSkipped`。
`PublicationStore::publish` / `hasExplicitSnapshot` 删除。
**W9c（活指针门）已落地 2026-11-13**：灯的活指针通道删除——`DrawDescription::lights` 与 `FrameRecorder` 的对象形
`setLights` 一并消失，`light_values` 是唯一通道（`MockBackend` / `OwnedFrame` / 像素测试都改读值）。`RenderCommand`
的 `geometry/material/program` 活指针**保留**：它们只做身份（id）与寿命（流绑定、驻留账、`releaseContent`），记录侧
读到的 revision / 透明判定 / 灯数值全部来自值；删除它们需要一条内容注册通道，触发条件见 T30/T22（`G8`）。
**W9d（`addPass` 家族）已落地 2026-11-13**：公开 `addPass` / `removePass` / `clearPasses` 删除；`PassChain` 只收集声明、
一次 `apply()` 提交（整计划校验），宿主可以点名作用域（`PassChain::build(engine, desc, owner)`）以便**重建原地更新**；
`DemoScene` / 链预设 / 测试走 `applyPlan`；`RenderEngine::pass(name)` / `passFor(name, owner)` 补上节点回读。
`PassProblemCode::SubclassPassWouldBeRebuilt` 随子类 pass 一起退役（计划只表达 `PassKind`）。
剩余：W9e（W7 结果形状 + `FramePlan`→`FrameTable`）。

## 10. 参考

### 10.1 代码

- `src/viz/graphics/sdk/vine/graphics/Plan.hpp` / `src/viz/graphics/src/Plan.cpp`（计划的契约、校验、`describe`）
- `src/viz/graphics/src/EngineCompile.cpp`（`applyPlan` / `applyPlanRows` / `plan` / `makePassNode`）
- `src/viz/graphics/sdk/vine/graphics/PrepareReport.hpp`（预热入口的答案）+
  `src/viz/graphics/src/RenderEngine.cpp`（`RenderEngine::prepare()`：编译 + 展开，再交给 `prepareFrame`）
- `tests/test_graphics/PlanTest.cpp`（`T7` / `T30` 的全部用例）
- `tests/test_graphics/SnapshotShadowTest.cpp`（`SnapshotSourceTest`：600 帧、批内旧图、T17 旧快照与空快照变异）
- `src/viz/graphics/sdk/vine/graphics/ContentId.hpp`（身份、`ContentRef`、注册表）
- `src/viz/graphics/src/SceneSnapshot.hpp` / `SceneSnapshot.cpp`（快照原型、`selectSnapshot`）
- `src/viz/graphics/src/SnapshotSource.hpp` / `SnapshotSource.cpp`（快照的 `DrawListSource` 面：5b-1 生产源；5a 时是影子）
- `src/viz/graphics/src/DrawListDiff.hpp` / `DrawListDiff.cpp`（5a 的字段级比较）
- `src/viz/graphics/src/SceneRows.cpp`（发布的缓存侧）
- `src/viz/graphics/src/DrawListBake.cpp`（现役收集与排序）
- `src/viz/graphics/sdk/vine/graphics/backend/ResourceTable.hpp`（按键驻留）
- `src/plugins/gfx_backend_vsg/include/vine/vsg/internal/DrawNeeds.hpp` / `src/internal/DrawNeeds.cpp`（`drawNeeds`：预热与录制共用的唯一键构造者）
- `src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp`（事实行与 epoch 回收）

### 10.2 文档与 ADR

- `../decisions/ADR-2026-11-12-adopt-graphics-next.md`（采纳决定、被否决的选项）
- `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`（5a 影子 / 5b-1 翻源 / 5b-2a 公开入口 / 5b-2b 删除的拆分与先后）
- `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md`（`I4` 的来源）
- `../decisions/ADR-2026-11-11-content-identity-is-a-handle.md`（`I2` / `R31` 的来源）
- `../design/graphics-threading.md`（迁移期前的线程契约）、`../design/graphics-scene.md`（现役场景与收集）
- `../research/graphics-2026-11-12-instances.md`（`T23` 的 A/B 测量口径；未采纳前只作研究）
- `../research/graphics-2026-11-11-next*.md`（提案原文与 `-design` / `-api` / `-hot-paths` / `-requirements` / `-benefits`；
  按 §9.3 逐份吸收后删除。`-audit` 的两轮审计是 `R31–R39` 与 `K7–K11` 的出处，也被两份 ADR 引为证据，
  所以它留到那些引用被历史取代为止）

### 10.3 外部资料

- 无（不引入第三方；结论全部由本仓库的测量与用例支撑）。
