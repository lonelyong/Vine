# graphics 的线程模型

> **状态**：草案（未落地；契约先定，实现按 `../backlog/graphics.md` 的触发条件推进）
> **最后核对**：2026-11-10
> **范围**：graphics 模块与它的宿主之间"哪一段工作在哪个线程"的契约 —— 内容的写者是谁、跨线程只有哪几种动作、
> 表面事件与交换链归谁、并行计算放在哪。**不含** appfw 的通用异步（`appfw-startup-phases.md`、`async-design.md`）
> 和 Vulkan 同步细节（`vsg-backend.md`）。
> **权威来源**：`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`（现有线程契约）、
> `src/viz/graphics/sdk/vine/graphics/OwnedFrame.hpp`（值的形态与它的三个消费者）、
> `src/viz/graphics/src/Scene.cpp`（bake / select）、`src/plugins/gfx_backend_vsg/include/vine/vsg/internal/SessionContent.hpp`（表面重读与交换链重建）。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

今天一帧的每一段都跑在 Qt 应用线程的一次调用栈里：`SurfaceWindow::renderFrame()` → `RenderEngine::frame()`
→ `compileFrame()`（收集 + 描述）→ `executor().executeFrame()`（上传、记录、提交、呈现）。
实测成本（Release）：2000 drawables 移动相机 **1597 µs/帧**（0.94 µs/命令，其中每条命令的构造/解析 ~1.3 ms = 71%），
后端记录基线 **33 次分配/录制三角形**，会话初始化 ~184 ms，且 vsg 在"这一帧什么都没录"时会在调用线程上睡 16 ms。
appfw 已经把应用线程占用当预算（启动门禁的 150 ms 项），所以**渲染与交互互斥**是这条线上的第一个问题。

`appfw-render-surface.md` §9.2 已经记了这一条："会话 init 仍在应用线程（~184 ms）；搬到池上需要重新设计 attach
与内容装配的并发，暂不做"。本文就是那个"重新设计"，并回答三个具体问题：鼠标交互怎么办、交换链归谁、场景怎么更新。

### 1.2 目标

- 一帧的 GPU 面工作（上传、记录、提交、呈现、驻留记账）跑在**引擎线程**上，应用线程不承担它。
- 应用线程**永不阻塞**在帧、交换链或队列上；它只发意图、读快照、发请求。
- **内容模型保持零锁**：并发不靠共享可变状态，靠"把纯计算搬走"与"在值上画边界"。
- 跨线程的动作收敛成**四种**，每一种都有明确的顺序与失效语义；其余一律禁止。
- 能增量落地：`inline` 模式（今天的行为）保留，测试与无头路径不必重写。

### 1.3 非目标

- 不做通用 job system / 任务图（`async` 与 `ThreadPool` 已有，够用）。
- 不给 `Node` / `Geometry` / `Buffer` 加锁（理由见 §7 D1）。
- 不做多设备共享同一份内容的并发编辑（两个窗口两份内容，见 §9.2）。
- 不做远程渲染 / 录制（`OwnedFrame` 的消费者，`OwnedFrame.hpp:101-105` 已划出这三类场景）。
- 不做按 pass 的并行记录（理由见 §7 D5）。

### 1.4 一句话结论

**配置期自由，运行期一个写者**：内容在 `start()` 时移交给引擎线程，之后跨线程只有"意图 / 交接 / 快照 / 请求"四种动作；
呈现与交换链只属于引擎线程，应用线程只发窗口事件；并行只发生在对固定快照的纯计算上（当前的候选就是收集的 select 一阶段）。

## 2. 设计要点

### 2.1 一个写者：内容归引擎线程，移交点是 `start()`

- **是什么**：`Scene` / `Node` / `Geometry` / `Buffer` / `Program` / `Material` / `Texture` / `Camera` 的**写**只允许一个线程 ——
  引擎线程。`start()` 之前（配置期）任意线程可以构造与修改；`start()` 是一次**所有权移交**，之后应用线程改内容即违约。
- **为什么**：后端的"事实行"每帧从**活内容**上读（`GeometryFacts.cpp:181` 读 primitive、`ContentStore.cpp:377`
  读 `geometry->revision()`），收集也要走场景图。把这段工作搬到另一个线程，就等于让两个线程同时读一个非原子的图 ——
  这只能靠锁解决，而锁会进到每一次读路径（`G1`、`G10`：概念一个 owner，成本按最坏路径算）。
- **边界**：`inline` 模式（§3 F7）下"引擎线程"就是调用线程，语义与今天完全相同；`threaded` 模式才移交。
  移交后，应用线程**读**内容也不允许（读也一样是竞争），要读就读快照（§2.3）。

### 2.2 四种跨线程动作，其余禁止

| 动作 | 方向 | 语义 | 顺序 / 失效 |
|---|---|---|---|
| **意图**（intent） | 任意线程 → 引擎 | 一次"要做的事"的**值**（输入、编辑、窗口事件）；无返回值 | 队列 FIFO；引擎在**帧首**一次 drain，同帧内按提交顺序应用 |
| **交接**（handoff） | 任意线程 → 引擎 | 所有权转移：新构造好的内容、或一整帧（`OwnedFrame`） | 交接后发送方不再触碰；必须经带边界的交接点（§2.6），不能靠 `intrusive_ptr` |
| **快照**（snapshot） | 引擎 → 任意线程 | 已发布的**不可变值**（收集结果、相机值、帧统计、帧计划） | 读到的永远是完整的某一版；带帧号，用于判断"多旧" |
| **请求**（request） | 任意线程 → 引擎 → 回原线程 | 需要当前内容才能答的查询（拾取、包围盒、帧计划） | 应答带请求 id 与**请求时的帧号**，不保证是当前帧 |

- **为什么是这四种**：它们恰好覆盖"改 / 交 / 读 / 问"，每一种都能用值或所有权表达；没有第五种需要共享可变状态。
  （`G4`：名字、参数、返回值、所有权、失效模式都说清楚，不留"用它应该没事"的余地。）
- **边界**：意图**没有**返回值、也不承诺"什么时候应用"（下一帧帧首）；需要结果的走请求。

### 2.3 帧是值、内容是身份：快照只发布不可变的东西

- **是什么**：应用线程能读到的一切都是不可变值。今天已经有两样现成的：收集结果
  `std::shared_ptr<const std::vector<RenderCommand>>`（`Scene.cpp`，命令持有自己的几何与材质）与 `FramePlan`
  （主机可读的计划，含 `record_micros`）。本文再加两样最小的：**相机值**（位置/朝向/投影参数，用于在应用线程算
  拾取射线）与**帧号 + 统计**。
- **为什么**：读快照让"应用线程要读内容"这件事消失 —— 它读的是上一帧的答案，而这也正是用户看到的那一帧（`G5`：变化留在模块内；
  `G12`：边界由类型表达，不由纪律表达）。
- **边界**：快照**不带**任何指向活内容的裸指针或可变句柄；要指针就得先持有（`intrusive_ptr`），而"持有并读"回到 §2.1 的禁令。
  发布用带 acquire 的原子指针（或受锁保护的 `shared_ptr` 双缓冲），不依赖 `intrusive_ptr` 的引用计数。

### 2.4 表面事件归应用线程，交换链归引擎线程

- **是什么**：**表面的创建/销毁在应用线程**（`QWindow` 的原生句柄、Qt 的 display 连接属于 Qt）；**acquire / present /
  交换链重建在引擎线程**（`Session` 拥有一个窗口、一个设备、一个交换链）。应用线程把窗口事件变成意图：
  `surfaceChanged{handle}` / `extentChanged{w,h}` / `visibility{exposed}` / `terminate`。
- **为什么**：present 必须在拥有设备的线程上（`RenderDevice.hpp:135-138` 只承诺"一个线程驱动"，不承诺"永远是同一个线程"，
  所以线程可以换，但同一时刻只能一个）；而平台窗口的原生状态只属于创建它的线程（`G2`：依赖方向决定谁调谁）。
- **边界**：extent 变化**只在帧边界应用**；应用前的那一帧丢弃（不上屏，不算错），重建走 `SessionContent` 已有的
  "re-read the surface + rebuild the swapchain"路径（重建会 wait device，计数在 `device_waits` 里，这是既有语义）。
  应用线程**永不**等待交换链；"这一帧上屏了"是一个事件（`Presented{frame_id}`），不是返回值。

### 2.5 并行只发生在对固定快照的纯计算上

- **是什么**：能并行的只有一类：一次 `collectRenderCommands(view)` 里 **bake 之后的选取**（层并集剪枝 + 视锥 + 命令构造 + 排序键）。
  输入是只读的 `content_bake_`（按 `contentEpoch` 烘焙，`Scene.cpp:975-981`），输出是本次调用的局部 `commands`
  与局部 `BoundsCache`；因此按行区段分块、每块写自己的 vector、**按块序合并**即可，排序键每命令预算（`:992-1000`）
  加 `std::stable_sort` 保证 tie-break 与串行**逐位相同**。
- **为什么**：这是唯一"共享只读 + 输出可复原顺序"的地方（`G7`：复杂性吸收在这里，不推给调用方；`G8`：只有它真有测量支撑）。
- **边界**：小场景不并行（200 drawables 141 µs，与投递/合并开销同量级）；`AllocationGate` 的窗口状态非原子
  （`AllocationGate.hpp:27-31`、`.cpp:103-158`），并行段默认**不进统计窗口**或按线程分桶。

### 2.6 空闲与唤醒：引擎线程等待，应用线程不排帧

- **是什么**：`threaded` 模式下连续渲染不再由应用线程的 `QTimer` 驱动；引擎线程在"下一个截止时刻 + 队列非空 + 事件到达"
  三者之一上阻塞等待，被唤醒才出一帧；`requestFrame()` 变成"投一个唤醒"。
- **为什么**：空闲时零 CPU 是既有性质（`RetirementQueue`/`FrameTimeline` 的时间语义都按帧计），换成轮询会让空闲的查看器
  占满一个核（`G10`：真实成本）。
- **边界**：唤醒是**合并**的（一帧之内的多次请求只出一帧），与今天 `requestFrame()` 用 `requestUpdate()` 合并的语义一致。

### 2.7 违例要立刻看得见

- **是什么**：`threaded` 模式下，内容写入口与设备入口在 Debug 里断言"当前线程 == 引擎线程"（一个线程 id 的比较，
  没有锁）；意图队列暴露深度；每帧记录 drain 数量与耗时。
- **为什么**："忘了移交"的失败形态是数据竞争 —— 它不报错、只偶尔画错或崩，正是 `G11` 说的"定位不到"（怎么判见 §6.3）。
- **边界**：断言只在 Debug；Release 下的判据是意图队列深度与 §8 的 TSan 构建。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 引擎线程与帧循环 | 一个会话一个引擎线程；帧 = drain 意图 → 应用表面事件 → 编译 → 执行 → 提交/呈现 → 推进时间线 → 发布快照 | 草案 |
| F2 | 意图队列（输入与编辑） | 任意线程投递，帧首一次 drain，FIFO | 草案 |
| F3 | 快照与请求 | 发布不可变值；请求/应答回原线程并带帧号 | 草案 |
| F4 | 表面事件与交换链 | 表面归应用线程，present/重建归引擎线程；extent 只在帧边界应用 | 草案 |
| F5 | 关闭与移交 | `start()` 移交所有权；`terminate` 在引擎线程上 shutdown 并 join | 草案 |
| F6 | 并行 select | 分块纯计算，按块序合并，与串行逐位相同 | 草案 |
| F7 | `inline` 模式 | 不建线程，调用线程即引擎线程；测试与无头路径用它 | 草案 |
| F8 | 违例可诊断 | 线程断言 + 队列深度 + drain 统计 | 草案 |

### 3.1.1 F1 引擎线程与帧循环

- **行为**：每会话一个线程，循环体固定为：drain 意图 → 应用表面/尺寸事件 → 编译（收集 + 描述）→ 执行（上传、记录、提交、呈现）
  → 推进 `FrameTimeline` / `RetirementQueue` → 发布快照 → 等待下一次唤醒。等价于今天 `RenderEngine::frame()` 的整段，
  只是换了线程且拆成"可唤醒的一步"。
- **约束**（C1）：一帧一次 drain，同帧内意图按提交顺序应用；一帧之内不再看队列（避免"这一帧看到的和下一帧不同"）。
- **要求**（R1）：应用线程调用任何引擎入口都不阻塞等待帧结束。
- **验收**（T1）：帧在引擎线程上完成（线程 id 断言）；无请求的静止场景 CPU 占用 ≈ 0。
- **注意**（N1）：帧号是唯一的时序标尺；快照与应答都带它。

### 3.1.2 F2 意图队列（输入与编辑）

- **行为**：`post(intent)` 接受**值**：`orbit{dx,dy,mods}`、`pan`、`zoom`、`setTransform{node,matrix}`、`setVisible{node,bool}`、
  `addChild{parent,child}`、`removeChild{...}`、`setBuffer{geometry,location,buffer}`、`setMaterialValue{...}`、
  `surfaceChanged`、`extentChanged`、`visibility`、`terminate`。
- **约束**（C2）：每个意图都必须是可拷贝的值 + 不可变句柄；不得携带"还会被发送方继续改"的对象。
- **要求**（R2）：队列是 MPSC 且**不阻塞生产者**（应用线程投递不得等引擎）；drain 一次拿完**当时**已有的条目。
- **验收**（T2）：N 个线程并发投递 M 条，引擎线程收到的总数与顺序满足"每线程 FIFO、总量精确"（不丢不重）。
- **注意**（N2）：意图**没有**返回值 —— 需要结果的走 F3。

### 3.1.3 F3 快照与请求（拾取、查询）

- **行为**：引擎每帧发布（a）`CameraSnapshot`（视图/投影/eye/up，用于应用线程算 ray）、（b）上一帧的收集结果（已有的不可变列表）、
  （c）`FrameStats` / `FrameCounters` / `FramePlan`（已有）。需要当前内容才能答的走请求：`pick{ray}`、`bounds{node}`、`plan{}`。
- **约束**（C3）：请求的应答带**请求时**的帧号；调用方不得把应答当成"现在"。
- **要求**（R3）：拾取的两个层次（§6.5）：快路径用快照 + 已发布的 BVH 只读查询；准路径用请求。
- **验收**（T3）：请求在引擎线程执行；应答回到请求方线程；帧号诚实。
- **注意**（N3）：`GeometryPickStats` 是拾取器**写**的计数器（`Geometry.hpp:906-915`）—— 并发拾取要么按线程计，要么关统计。

### 3.1.4 F4 表面事件与交换链

- **行为**：应用线程在窗口事件里投递 `surfaceChanged{handle}` / `extentChanged{w,h}` / `visibility{exposed}`；
  引擎线程在帧首应用：句柄变了重绑表面，尺寸变了重建交换链，未暴露则本轮不出帧（也不 present）。
- **约束**（C4）：present 只发生在引擎线程；应用线程不持有交换链句柄，不查询 present 结果。
- **要求**（R4）：重建期间的帧丢弃且不计错；重建后第一帧恢复正常；`device_waits` 可见（既有计数器）。
- **验收**（T4）：连续 resize（变小/变大/重建窗口）后画面正确、无崩溃、应用线程无阻塞。
- **注意**（N4）：Wayland 的"未映射拿不到 extent"回退仍在应用线程决定（`appfw-render-surface.md` F5），引擎线程只看结果。

### 3.1.5 F5 关闭与移交

- **行为**：`start()` 移交（内容与设备从此刻归引擎线程）；`terminate` 意图让引擎线程自己 shutdown（设备、会话、表）
  并退出，`stop()` 在调用方 join（有界等待，超时报错而不是静默）。
- **约束**（C5）：引擎线程**不得**同步等应用线程（否则与"应用线程不得等 worker"对称的另一半会互相等死）。
- **要求**（R5）：`stop()` 之后引擎线程内分配的一切已释放（ASan 判据）。
- **验收**（T5）：`terminate` → join 无泄漏；队列里 pending 的意图被丢弃且可观测（计数）。
- **注意**（N5）：析构顺序 —— 引擎线程必须在设备/表之前结束。

### 3.1.6 F6 并行 select

- **行为**：`threaded`/`inline` 都可用；帧内按阈值（drawable 数）决定并行度 P；按 bake 行区段切块，块内局部 `commands` 与局部
  `BoundsCache`，按块序拼接；排序键单线程预算（或分块预算 + 稳定归并）。
- **约束**（C6）：并行度 0/1 时**必须**与串行逐位相同；P 变化不改变输出。
- **要求**（R6）：小场景不并行（阈值以 2000 drawables / 141 µs 为起点，按实测调）。
- **验收**（T6）：同一场景同一视图，并行与串行命令序列逐元素相同（值比较，不只比长度）；`record_micros` 下降。
- **注意**（N6）：`AllocationGate` 的窗口非原子 ⇒ 并行段不进统计窗口（或按线程分桶）。

### 3.1.7 F7 `inline` 模式

- **行为**：不建线程，`start()` 是空操作，所有意图立即在调用线程应用（或退化为直接调用）；`frame()` 语义与今天完全一致。
- **约束**（C7）：`inline` 下线程断言恒真；`threaded` 下内容写入口断言引擎线程。
- **要求**（R7）：既有 537 个 vsg 用例与无头路径继续用 `inline`，不改一行。
- **验收**（T7）：同一套用例在两种模式下行为一致（`inline` 必跑；`threaded` 跑一个子集）。
- **注意**（N7）：模式是**启动参数**，不是两种实现；两条路径共用同一份帧循环。

### 3.1.8 F8 违例可诊断

- **行为**：Debug 断言（内容写入口、设备入口、表面入口检查线程 id）；意图队列暴露 `depth()` / `drained()`；帧内 drain 耗时进 `record_micros`。
- **约束**（C8）：断言不得引入锁或原子（线程 id 比较用 `std::this_thread::get_id()` 缓存）。
- **要求**（R8）：误用（应用线程改内容）在第一次就断言，而不是偶发画错。
- **验收**（T8）：一条故意违约的用例在 Debug 下触发断言（`EXPECT_DEATH`）。
- **注意**（N8）：Release 里靠 TSan 构建兜（T9）。

## 4. 公开接口

> 本章是**草案**：名字与形状是这一版的提议，落地时以头文件为准。

### 4.1 `vn::appfw::RenderWorker`（appfw/gui 半，草案）

| 接口 | 契约 | 备注 |
|---|---|---|
| `start(mode)` | `inline` 或 `threaded`；`threaded` 是所有权移交点 | 移交后应用线程不得写内容 |
| `post(Intent)` | 任意线程；不阻塞；帧首 drain | 值 + 不可变句柄 |
| `handoff(OwnedContent / OwnedFrame)` | 所有权转移 | 经队列，不经引用计数 |
| `snapshot()` | 返回最近发布的不可变值（相机/统计/计划/收集结果） | 带帧号 |
| `request(Query, callback)` | 应答回请求方线程 | 带请求 id + 帧号 |
| `stop()` | 有界 join；超时报错 | 引擎线程自己 shutdown |

### 4.2 既有接口保持不变的部分

| 接口 | 契约 | 备注 |
|---|---|---|
| `RenderEngine::frame()` | `inline` 模式下一次同步出帧；`threaded` 下退化为"请求一帧" | `RenderEngine.hpp` |
| `RenderEngine::releaseContent()` | 仍在帧线程调用；`threaded` 下由意图转交 | `RenderEngine.hpp:500` |
| `Scene::collectRenderCommands(view)` | 仍是**纯函数**（除 memo 记账），返回不可变列表 | `Scene.cpp`；并行化的前提 |
| `RenderDevice` / `RenderFrameExecutor` | 线程契约不变：一个线程驱动，不并发、不重入 | `RenderDevice.hpp:135-138` |
| `OwnedFrame` | 跨线程帧的唯一形态 | `OwnedFrame.hpp` |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 一帧一次 drain；同帧内按提交顺序应用意图。 |
| C2 | 意图必须是值 + 不可变句柄，不得携带发送方还会改的对象。 |
| C3 | 快照与应答带帧号，不承诺"当前"。 |
| C4 | present 只在引擎线程；应用线程不持有交换链。 |
| C5 | 引擎线程不得同步等应用线程。 |
| C6 | 并行度 0/1 时与串行逐位相同。 |
| C7 | `inline` 模式线程断言恒真；`threaded` 下内容写入口断言引擎线程。 |
| C8 | 诊断断言不引入锁/原子。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 应用线程调用引擎入口不阻塞等待帧。 |
| R2 | 意图队列 MPSC、生产者不阻塞、drain 拿完当时全部。 |
| R3 | 拾取有快路径（快照 + BVH 只读）与准路径（请求）两条，且都可用。 |
| R4 | 重建期间的帧丢弃且不计错；重建后恢复。 |
| R5 | `stop()` 后引擎线程分配的一切已释放。 |
| R6 | 小场景不并行。 |
| R7 | 既有用例与无头路径在 `inline` 下不改一行。 |
| R8 | 违约在第一次断言，而不是偶发画错。 |

### 5.3 不变量（N#）

| # | 不变量 |
|---|---|
| N1 | 一个 `Geometry`/`Scene`/`Buffer`/`Camera` 的写者只有一个线程。 |
| N2 | 读内容只在引擎线程；其他线程只读已发布快照。 |
| N3 | 一处 `present` 只对应一次 `acquire`，两者同线程。 |
| N4 | 设备与内容的重建/销毁只在引擎线程。 |
| N5 | 稳态零分配的性质不被并行段破坏（并行段的分配按线程计并可见）。 |

## 6. 注意事项与陷阱

### 6.1 应用线程与引擎线程互等 = 死锁

- **会怎么错**：引擎线程为了答一个请求回调到应用线程并同步等待，而应用线程正在 `invokeOnMainThread` 等引擎。
- **怎么判**：双方都卡住；`gdb` 两栈各自等在对方。
- **怎么防**：C5 —— 引擎线程**只投递不等待**（`postToMainThread` 语义）；请求应答一律异步。

### 6.2 `intrusive_ptr` 没有 happens-before

- **会怎么错**：worker 构建好 `Buffer`/`Geometry`，直接把 `intrusive_ptr` 塞进容器交给引擎线程 —— 引用计数是 relaxed 加、
  acq_rel 减（`src/base/core/sdk/vine/RefCounted.hpp:62-83`），**没有** acquire，于是读线程可能看到"还没写完的字段"。
- **怎么判**：偶发读到 0 长度/半截数据；TSan 报 race。
- **怎么防**：交接必须过带边界的点（`std::mutex` 保护的队列、`std::atomic<T*>` 或 `AsyncQueue`），在那里建立释放/获取边。

### 6.3 "忘了移交"在 Release 里不报错

- **会怎么错**：宿主在应用线程继续 `scene->addChild(...)`（今天完全合法），引擎线程同时在收集 —— 数据竞争。
- **怎么判**：画面偶发缺件/崩，`gdb` 栈两线程同在 `Scene.cpp`；Debug 下断言。
- **怎么防**：F8 的断言 + §8 的 TSan 构建；文档里把"`start()` 之后不许写"写在 `RenderWorker::start` 的 `@warning`。

### 6.4 表面句柄与 display 的归属

- **会怎么错**：在引擎线程创建/销毁表面（`vkCreateXcbSurfaceKHR` 需要创建窗口的那个 `xcb_connection_t`）。
- **怎么判**：`VK_ERROR_INITIALIZATION_FAILED` 或偶发崩溃在 surface 创建。
- **怎么防**：§2.4 —— 表面在应用线程创建/销毁，只把**值**（`void*` 句柄 + 尺寸）交给引擎线程。

### 6.5 拾取：快路径与准路径不是一回事

- **会怎么错**：hover 每次都发请求（每帧一次往返，延迟一帧且抖动），或者点选用快照（在相机正被拖动时点偏）。
- **怎么判**：hover 走请求时 vsync 抖动；点选在拖动中偏移一个帧的位移。
- **怎么防**：hover 用快照 + BVH（`gs_*` 查询是 const、非分配、可并发 —— `.ai/design/geometry-bvh.md:155-158`）；
  点选用请求。两者都用**同一份** ray 构造（`Camera::screenToWorldRay`），避免两套投影。

### 6.6 `GeometryPickStats` 是写者

- **会怎么错**：应用线程并发拾取同一几何，两个线程写统计计数器。
- **怎么判**：TSan；统计数偶尔偏小。
- **怎么防**：关掉统计或按线程计（`GeometryPickStats` 的写侧是拾取器的，`Geometry.hpp:906-915`）。

### 6.7 `AllocationGate` 的窗口是单线程的

- **会怎么错**：把并行 select 放进统计窗口，窗口计数被两个线程交错更新（状态非原子）。
- **怎么判**：门禁数字抖动/负数。
- **怎么防**：并行段不进窗口，或按线程分桶（§2.5）。

### 6.8 相机快照用错帧

- **会怎么错**：应用线程算出 ray，引擎在**更晚**的相机上求交 —— 用户看到的是"点偏了"。
- **怎么判**：快速拖动时偏移量与帧率相关。
- **怎么防**：应答带帧号；若应答帧号 ≠ 请求参考的帧号，调用方自选"用旧结果"或"重发"。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 给 `Node`/`Geometry`/`Buffer` 加锁 | 同一概念两个 owner；锁进每条读路径；稳态零分配与零竞争一起丢（`G1`/`G10`）。 | `../decisions/ADR-2026-11-10-render-thread-owns-content.md` |
| D2 | 应用线程拥有内容，渲染线程吃每帧的描述快照 | 描述（事实行）今天在 `executeFrame` 里从活内容上构建，把它搬到应用线程 = 把 71% 的逐命令记账挪到 UI 线程；且事实与跨帧的驻留表身份耦合（表要在两个线程间共享）。 | 同上 |
| D3 | 双缓冲世界（每编辑克隆场景后原子换） | 每次编辑 O(content)，大场景不可接受。 | 同上 |
| D4 | 每帧起线程 / 每帧 join | 破稳态零分配，破 `AllocationGate` 门禁；调度延迟大于收益。 | 同上 |
| D5 | 按 pass 并行记录 | 记录读 `ContentStore`/`ContentHalves`/`ContentSets`/`StreamUploads`/`MaterialArena`，这些表按 pass 交错写；分区或加锁都与 D1 同罪，而今天 pass 通常 1–2。 | 同上 |
| D6 | 打开 vsg 的 `Viewer::setupThreading()` | 它并行的是 vsg 的记录任务，但每个任务仍要读我们按 pass 交错写的表 —— 不解决分区，只把竞争挪进 vsg 的线程。 | 同上 |
| D7 | 引擎线程同步等应用线程（答请求时回调并等待） | §6.1 的死锁形态；与 appfw"应用线程不得等 worker"对称。 | 同上 |
| D8 | 每帧把所有内容序列化成一份描述再交给后端 | 序列化成本 O(content)；`FrameDescription` 已经是借用快照，额外一层没有消费者（`G8`）。 | 同上 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 帧在引擎线程完成；静止时 CPU ≈ 0 | 线程 id 断言 + 空闲占用测量（沿用 `scripts/callgrind_diff.py`） |
| T2 | 意图不丢不重、每线程 FIFO | 并发投递用例（`test_vsg`） |
| T3 | 请求在引擎线程执行、应答回原线程、帧号诚实 | `RenderWorkerTest.RequestRunsOnTheEngineThread` |
| T4 | 连续 resize / 重建表面后画面正确、无阻塞 | 现有 `RenderControlTest` 系列 + 新增 resize 协议用例 |
| T5 | `stop()` 无泄漏、pending 意图可观测 | ASan 构建 + `test_appfw` |
| T6 | 并行 select 与串行逐位相同；`record_micros` 下降 | `SceneTest.CollectIsIdenticalAtEveryParallelism` |
| T7 | `inline` 与 `threaded` 行为一致 | 同一子集两模式各跑一次 |
| T8 | 违约在 Debug 下断言 | `EXPECT_DEATH` 用例 |
| T9 | 无数据竞争 | 新增 `-fsanitize=thread` 构建树跑 `test_vsg` 与 `test_appfw` |

## 9. 状态与演进

### 9.1 当前状态

草案。本文只定契约与阶段，未改代码；`inline` 模式即今天的行为。

### 9.2 已知缺口 / 待办

- 落地阶段与触发条件见 `../backlog/graphics.md`（"渲染线程"条目与 T24）。
- 多设备共享同一份内容的并发编辑不做：两个窗口 = 两份内容（与 `appfw-document-model.md` 的"一个文档一个控件"一致）。
- 远程渲染 / 录制等 `OwnedFrame` 的真实消费者仍未出现（T18）。

### 9.3 兼容性 / 迁移

- **P1（无触发条件即可做）**：`inline`/`threaded` 两模式骨架 + 线程断言 + 意图队列；`RenderEngine` 不变。
  这一步就能让 T8/T2 有判据，且不改变任何既有调用方。
- **P2**：`threaded` 下的编辑与输入意图（transform / visibility / 结构增删 / 相机）、快照发布。
- **P3**：表面事件与交换链协议（F4），以及 `RenderControl` 的 `state()` 语义在异步下的重新表述。
- **P4**：并行 select（可由 T24 单独触发，与 P2/P3 解耦）。
- 与 `appfw-render-surface.md` 的关系：该文 §9.2 说"搬到池上需要重新设计 attach 与内容装配的并发，暂不做"；
  本文是那个设计的**草案**，落地前该文的状态不变；落地后由本 ADR 与本文件接管线程契约，
  该文只保留"attach 的时机/次序/可见性"。

## 10. 参考

### 10.1 代码

- 现有线程契约：`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp:135-138`、
  `RenderFrameExecutor.hpp:52-55`、`PresentTarget.hpp:24-25`。
- 值的形态：`src/viz/graphics/sdk/vine/graphics/OwnedFrame.hpp`（含三个消费者与"引擎不产 owned frame"的理由）。
- 收集：`src/viz/graphics/src/Scene.cpp`（bake `:975-981`、select、排序键 `:992-1000`）。
- 表面/交换链：`src/plugins/gfx_backend_vsg/include/vine/vsg/internal/SessionContent.hpp:76`、
  `src/plugins/gfx_backend_vsg/src/internal/Session.cpp:699-712`。
- 引用计数：`src/base/core/sdk/vine/RefCounted.hpp:62-83`。
- 宿主侧：`src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp`、`src/fw/appfw/src/gui/SurfaceWindow.cpp:832-861`。
- 并发查询的先例：`.ai/design/geometry-bvh.md:155-158`。

### 10.2 文档与 ADR

- `../decisions/ADR-2026-11-10-render-thread-owns-content.md`（本文的决策与否决）。
- `appfw-render-surface.md`（attach 与可见性）、`appfw-startup-phases.md`（应用线程不得等 worker）、
  `async-design.md`（协程与调度器）、`vsg-backend.md`（后端记账成本）。

### 10.3 外部资料

- Vulkan 规范"Threading: host access to device objects is externally synchronised"（`VkQueue` 与 WSI 的线程规则）。
