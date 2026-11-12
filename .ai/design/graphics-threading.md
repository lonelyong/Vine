# graphics 的线程模型（当前契约）

> **状态**：现行。**2026-11-12 重写**：镜像 / delta / 三条通道 / memo+epoch 那一代形状随 graphics-next 5b-2b 删除，
> 跨线程的载荷只剩「一次发布出的不可变快照」与「两个原子 revision」。历史（为什么曾经那样、哪条被否决）见
> `../decisions/ADR-2026-11-12-adopt-graphics-next.md`、`../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`
> 与 git 历史；`C1–C15` 与 `N1–N14` 里只服务于被删机制的条目已退役，编号不复用（见 §5.1、§5.3 的说明）。
> **最后核对**：2026-11-12
> **范围**：一帧的工作在两条线程上各归谁 —— 内容侧与引擎侧各拥有什么、跨侧只允许过什么值、表面与交换链归谁、
> 并行放在哪、关闭怎么收。
> **不含** appfw 的通用异步（`appfw-startup-phases.md`、`async-design.md`）与 Vulkan 同步细节（`vsg-backend.md`）。
> **权威来源**：`src/viz/graphics/src/PublicationStore.{hpp,cpp}`（发布与绑定的内容侧表）、
> `src/viz/graphics/src/FrameExpander.{hpp,cpp}`（引擎半段：bind + expand）、`src/viz/graphics/src/SnapshotSource.{hpp,cpp}`
> （快照的 `DrawListSource` 面）、`src/viz/graphics/src/FrameRunner.{hpp,cpp}`（引擎线程）、
> `sdk/vine/graphics/Scene.hpp`（`publish`）与 `sdk/vine/graphics/RenderEngine.hpp`（`bindSnapshot` /
> `setFrameThreading` / `waitForFrame` / `presentRecord` / `presentedCamera`）。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

内容（场景图、几何、材质、相机）由宿主在 UI 线程上同步编辑 —— 这是文档编辑器的核心手感，不能变成异步；
而一帧的 GPU 面工作（编译、上传、记录、提交、呈现、驻留记账）必须能和交互并行，否则 UI 与渲染互斥。
实测（Release）：2000 drawables 移动相机 1597 µs/帧，后端记录基线 33 次分配/一个被录制的三角。

难的不是"开线程"，而是**内容归谁写**：后端的"事实行"从活内容上读（`Geometry::revision()`、primitive 的实例句柄），
收集要走场景图。只要渲染在另一条线程，两侧就必须有一种关系：共享（锁）、拷贝（每帧快照）、或移交（一个写者）。

本模块选的是：内容侧在**发布**的那一刻把可变树折成一份**不可变值**，之后两侧只通过这个值相遇。
2026-11 期这一层曾同时存在镜像 + delta + memo + 引用计数 + 停车窗口五套"这份副本什么时候过期"的答案；
现在只剩一套（见 §2.2）。

### 1.2 目标

- 一帧的 GPU 面工作（上传、记录、提交、呈现、驻留记账、剔除）跑在**引擎线程**上，内容线程不承担它。
- **内容的编辑 API 不变**：宿主继续在 UI 线程同步读写场景图。
- 跨侧只传**不可变值**与**报告值**，没有一处共享可变状态。
- 一次发布是**一次原子指针交换**；版本、镜像、delta、memo 不再是概念。
- 画面与内容的关系是**可计算**的：一帧画的是它开始那一刻绑定的那份发布，而不是"跑的时候内容恰好是什么"。
- `inline` 与 `engine thread` 两种运行位置共用同一份代码路径。

### 1.3 非目标

- 不做通用 job system / 任务图（`async` 与 `ThreadPool` 已有）。
- 不给 `Node` / `Geometry` / `Buffer` 加锁（D2）。
- 不把内容编辑变成异步（D1）。
- 不在本层做按 pass 的并行记录（§2.7）；并行只在派生层的纯值上。
- 不做远程渲染 / 录制（`OwnedFrame` 的消费者已划出三类场景）。

### 1.4 一句话结论

> 内容侧把可变树**发布**成一份不可变快照（一次指针交换），引擎半段只读这个值；
> 两条线程各有写者，跨边界只有"已发布的值"与"完成报告"，字节内容靠两个原子 revision 轮询。

## 2. 设计要点

### 2.1 两个世界，各自一个写者

- **是什么**：**内容世界** = `Scene` / `Node` / `Geometry` / `Buffer` / `Program` / `Material` / `Texture` / `Camera`，
  只被内容线程写；**引擎世界** = 快照的 `SnapshotSource`、事实表（`ContentStore`）、驻留表（`StreamUploads` /
  `ContentHalves` / `ContentSets` / `MaterialImages`）、vsg 命令图、交换链，只被引擎线程写。
- **为什么**：两个写者共享一个可变图，只能靠锁 —— 锁会进到每一次读路径，且与"稳态零分配"直接冲突（`G1` / `G10`）。
  各自一个写者 + 值通道，是唯一能让两边都保持零锁的形状。
- **边界**：唯一的跨侧读取是“已发布不可变值”（§2.2）与随 pass 快照的值（§2.14）。引擎侧**不读**内容对象的任何可变字段。

### 2.2 跨线程的唯一载荷：一次发布的不可变快照

- **是什么**：`Scene::publish()` 把可变树折成一份不可变快照（行表按 32 行分块、写时只复制变了的块），
  并返回一个 `SceneSnapshotRef` 句柄；`RenderEngine::bindSnapshot(scene, ref)` 说"这个引擎从这一份开始画"。
  引擎半段把每个 draw 的 `content` 换成该内容的 `SnapshotSource`，之后**这个帧不再命名内容拥有的任何东西**。
- **为什么**：一份值取代五种"副本过期"的答案（`G1`）；发布是唯一一次可变 → 不可变的转换，所以"旧帧画旧值"
  不是约定，而是值语义；派生是纯函数，因此可并行、可 dump、可 replay（`G11`）。
- **边界**：快照的内容、分块与选择器是**实现**（`detail::SceneSnapshot`）；公开面只有一个不透明句柄加 `revision()`。
- **行里装的是什么值**：每条命令携带 `geometry_ref` / `material_ref`（身份 + 版本）与三份不可变描述
  （`GeometryDescription` / `MaterialDescription` / `ShaderProgramDescription`）；pass 描述自己时另快照
  `LightValue` 数组与程序 override 的文本。这就是 §2.14 的载荷，记录侧只读它。
- **一次发布是谁的动词**：`publish()` 是内容侧的动词（内容线程，唯一写者）；`bindSnapshot()` 说"这个引擎画
  这一份"（宿主在帧与帧之间调用，或引擎的隐式发布代劳 —— 见 §2.13）。两个动词都落在 `detail::PublicationStore`
  上，引擎半段只读这张表（见 §2.15）。两者合起来才是完整的交接。

### 2.3 两条通道：发布的快照 与 完成报告

| 通道 | 方向 | 装载什么 | 失效形态 |
|---|---|---|---|
| **publication**（快照） | 内容 → 引擎 | 不可变值：行表 + 素材值 + 盒子 + 灯集，带 `revision` | 发布落在帧中间也不会撕裂：帧用的是它开始那一刻绑定的那一份 |
| **report**（报告） | 引擎 → 内容/UI | 值：`PresentRecord`（帧号 + 呈现结局）、`presentedCamera()`、`FrameReport`、诊断 | 宿主读到的一定是完整的一版，带帧号 |

- **为什么只有两条**：镜像时代还有"delta"（改动公告）与"handoff"（所有权移交）。delta 随镜像一起删除（改动就是
  下一次发布的内容，没有第二条路可说"我改了什么"）；handoff 仍是**线程池/加载器**的通用形状，不是本模块的通道。
  没有消费者的通道不做（`G8`）。
- **边界**：publication 没有返回值，也不排队 —— 它是**一个指针交换**，没有"满了"这种状态。

### 2.4 字节内容：一个已知残余，不是第二条值通道

- **是什么**：几何的**形状**（通道集合、切片、primitive）走发布的描述值；几何/纹理的**字节**不走 —— 字节在任何线程写
  （加载 worker、流式写入），驻留条目问“我上传的 revision 还是不是它上传时那个”（`StreamUploads` 的 live 判据）。
  这条轮询**今天读的是活对象的 `revision()`**，而那个计数器**不是** `std::atomic`（2026-11-12 核对：`Buffer` /
  `Geometry` / `Texture` / `ShaderProgram` / `Material` 里都是普通 `std::uint64_t`）。
- **为什么保留**：把每次 `Buffer::edit()` 变成一次发布会让“编辑”与“帧”强耦合，且发布要承受每帧上千条。
- **边界**：这是**已知残余**（backlog T31）：当流式写入真的与引擎线程并发时，轮询本身是未定义行为。修法不在本轮
  （它需要“写入声明一条值”而不是把计数器改 atomic —— 后者只修一个字段）：见 T31 的触发条件。
  帧载荷的**内容**（形状、材质值、程序文本、灯）不靠这条通道，它们全部是 §2.14 的值。

### 2.5 发布是一次指针交换，不是一次事务

- **是什么**：`Scene::publish()` 在批外比较当前行的 `(epoch, boxes, materials)`，未变则答 `Unchanged` 并返回同一份值；
  变了则构建新快照（共享未变的块）并换掉 `Scene` 持有的那个句柄。`beginEdit()` / `endEdit()` 只界定**可见性**：
  批内发布答 `InsideBatch`，批外第一次发布把整批作为**一次**发布落地。
- **为什么**：批不是事务（没有回滚），它只界定"一帧能看见什么"（P4）；把发布做成一次交换，才让"在飞帧读旧值"
  不需要任何寿命协商。
- **边界**：`before_draw` 钩子在内容半段跑，它写的内容在下一次发布里可见；同一个帧的所有 pass 看到**同一份**发布
  （N16），因为绑定发生在帧首而不是每个 pass 里。

### 2.6 表面归内容侧（UI），交换链归引擎侧

- **是什么**：表面的创建/销毁在拥有原生窗口的线程（Qt 的句柄与 display 属于 Qt）；`acquire / present / 交换链重建`
  只在引擎线程。宿主把尺寸作为**值**交给面积（`RenderSurface::resize`），引擎在**帧首**把"这一帧的尺寸"交给 target，
  且只在数字变了时。
- **为什么**：句柄的生命周期是宿主的，交换链的生命周期是设备的；两者交叉的那一点（重建）必须只有一个写者（`G2`）。
- **边界**：设备动词（`createSurfaceTarget` / `setWindowHandle` / `device->initialize`）是**宿主在帧与帧之间**的操作，
  帧在飞时做属于未定义行为 —— 宿主先 `waitForFrame()`（P3）。

### 2.7 并行只在派生层

- **是什么**：唯一可并行的候选是"选择"这一段（层剪枝 + 视锥 + 命令构造 + 排序键），它作用在**不可变值**上。
- **为什么**：纯值是并行的前提；设备侧仍然只有一条线程（C16），因为 vsg 的 context 不是共享写安全的。
- **边界**：后端若自己并行 record，合并结果的确定性由后端自证（`graphics-frame-pipeline.md` R20）。

### 2.8 空闲时引擎线程等待，UI 不排帧

- **是什么**：引擎线程在"下一个截止时刻 / 新的一帧到达"上阻塞等待；没有帧时它什么也不做。
- **为什么**：一帧的来源只有一个（`frame()`），所以"有没有活"不需要队列来表达；UI 的绘制节拍不被设备反向控制。
- **边界**：`waitForFrame(n)` 是唯一会让调用者等设备的入口（截图、resize 稳定、退出、测量）。

### 2.9 与其它库的对照：为什么是"一次发布的值"，而不是"每帧 extract"

| 库 | 内容侧 | 渲染侧 | 关系 |
|---|---|---|---|
| Unreal | 游戏线程（`UWorld`/actor） | 渲染线程的 `FScene` + proxy，变更经 `ENQUEUE_RENDER_COMMAND` | 同形（§2.2） |
| Qt Quick | GUI 线程拥有 `QSGNode` | 渲染线程每帧 `sync()` 重建自己那份 | **每帧 O(n) 同步**，本设计用发布避开 |
| Godot | 主线程节点 | `RenderingServer` 经 `CommandQueueMT` + RID | 同为命令队列 + 稳定句柄；RID ≈ 本设计的 `NodeId` |
| Unity | `Transform` 只是句柄，原生层权威 | 渲染侧按批处理变换变更 | 同为"批处理变更"，验证了"按批发布"的必要性 |
| wgpu / Dawn | 应用线程提交命令 | 内部追踪资源寿命 | 借"内部引用与用户引用分开"；本仓库是 `ResourceTable` + 退休队列 |

- **为什么不用每帧 extract**：extract/`sync()` 的形状是每帧 O(drawables) 的拷贝，而本设计的目标恰恰是把**每帧**的
  成本从 UI 线程拿走；一次发布只在**编辑**时付 O(变化的部分)。

### 2.10 帧先是被描述的值，展开是另一步

- **是什么**：内容半段产出 `FrameDescription`（每个 pass 的视图、输入、它要画的东西），引擎半段把它展开成命令列表
  并执行、呈现。两步之间只传值。
- **为什么**：这条缝就是线程边界（`G3` / `G7`）：内容半段可以留在内容线程，引擎半段可以搬到引擎线程，而
  "一帧是什么"只有一个定义。
- **边界**：`FrameDescription` 里的指针都是借用（camera、commands、target、program），只在这个调用期间有效；
  记录侧读的却是它旁边那份**值**（`light_values`、`program_override_description`、命令里的三份 description），
  所以借用指针失效也不改变“这一帧画的是什么”。

### 2.11 一帧的所有权：三样东西，各自一个所有者

| 东西 | 持有者 | 唯一写者 | 跨边界的形态 |
|---|---|---|---|
| `Scene` / `Node` / 几何 / 材质 / 相机 | 宿主（内容线程） | 内容线程 | 发布出不可变快照 |
| `SceneSnapshotRef` + `SnapshotSource` 的当前值 | 引擎（每个场景一份，见 §2.13） | 发布方（内容线程），引擎线程只读 | 值（共享的不可变快照） |
| 事实表 / 驻留表 / 帧值 / 退休队列 / 命令图 / 交换链 | 引擎（会话） | 引擎线程 | — |

- **为什么**：一样东西一个所有者（`G1`）；两界之间没有"半个所有权"的物体，也就没有"谁先释放"的问题（`G10`）。
- **边界**：引擎侧持有快照的 share 只是"读到的是完整的一版"，**不**因此拥有内容；内容改了不影响它手里的那一版。

### 2.12 一帧的两个半段，各归一条线程（`G2` / `G4` / `G10`）

- **所有权表**（`G1`：一样东西一个定义、一个写者）

| 东西 | 持有者 | 唯一写者 | 跨边界的形态 |
|---|---|---|---|
| 原生窗口句柄 / display | UI（Qt） | UI | 值（尺寸、可见性） |
| `RenderSurface`（面积） | 引擎（`createSurface` 给的 share） | 引擎线程 | — |
| `WindowPresentTarget` + 交换链 | 引擎（设备造，`createSurfaceTarget`） | 引擎线程 | — |
| `RenderDevice` / 会话 / 帧值 / 退休队列 | 引擎 | 引擎线程 | — |
| 帧的**内容半段**：帧号、`compileFrame`、`before_draw` 钩子、发布（每帧每场景一次）、`describe` | 内容线程 | 内容线程 | 帧值交出去 |
| 帧的**引擎半段**：表面事件、`FrameExpander::runPass`（bind + expand）、`executeFrame`、present | 引擎线程 | 引擎线程 | — |

- **一帧两个半段，每侧一帧在飞**（已落地）：内容线程构建帧 N+1 时，引擎线程正在跑帧 N，**所以画面比内容晚一帧**
  （N15）。这是唯一的语义代价，换来的是 UI 侧调用只花 O(passes)：不 expand（O(drawables)）、不 present
  （可能等垂直同步）。帧值因此有两份轮换（`frame_slots_`），帧号随帧走（`detail::EngineFrame::number`），
  `frame()` 在构建前**只等上一帧的引擎半段**（`waitIdle`，通常已完成），并把完成状态作为 `waitForFrame` 的答案。
- **这一帧领先在交互上意味着什么**（写下来，因为它是可以感觉到的）：
  - 相机是内容、`CameraManipulator` 是**事件驱动**的（没有惯性、没有 dt），所以**操作立刻生效**，延迟恒定一帧；
  - 画面显示的是**上一帧取下的相机快照**。60 fps 下 16 ms 感觉不到；10 fps 下是 100 ms，观感就是"拖一步跳一格"
    —— 低帧率本来就如此，多出来的只是这一帧；
  - **内容查询必须按"用户看到的那一帧"的相机算**（N17）：`presentedCamera()` 发布被答完那一帧的相机快照，
    宿主经 `SceneView::setPresentedCamera` 交给 manipulator；没有发布值时退回当前相机（静止画面、inline）。
  - **UI 只是"不再付展开与 present"，不是"不再被渲染节流"**：每次请求帧会等**上一帧的引擎半段**，所以帧率低时
    UI 的绘制节拍被拉着走；输入事件仍在两次请求之间正常落地（R8 的准确读法）。
- **"等到这一帧"是显式动作**（`G4`）：`waitForFrame(frame_id)` —— 截图、resize 稳定后、退出、测量用；默认路径
  `frame()` 只保证"构建完成并交出去了"。
- **完成通知是报告，不是回调**（`G10`）：一帧的**呈现结局**是 `PresentOutcome` 的值，与它的帧号一起作为
  `RenderEngine::presentRecord()` 发布（一个原子字打包，任何线程任何时刻可读）。引擎线程**从不**回调 UI、也从不
  回头读 UI。要"等第 N 帧上屏"就 `waitForFrame(N)`，要"这一帧怎么了"就读 `presentRecord()`。
- **尺寸随帧走，交换链在帧首跟随**（已落地）：`RenderSurface::resize` 只做两件事 —— 记录这个面积自己的尺寸、
  把几何交给 presenting view —— 而**不碰** target；`frame()` 在装配完之后、执行之前，把"这一帧的尺寸"交给 target，
  且**只在数字变了时**。于是：宿主在一次帧中间 resize 也不等设备，两帧之间的多次 resize 只付一次交换链重建。
- **关闭**（F10）：`stop()` 投"不要再要帧" → 引擎半段跑完手上这一帧（或丢弃）→ 在引擎线程释放 present target、
  会话、设备 → join（有界，超时报错）。析构顺序沿用 N11 的规则（线程先于设备与表结束）。UI 此时可以自由销毁自己的
  窗口，因为引擎侧已经没有回头的边。
- **一帧只有一份发布**：钩子在内容半段跑、发布按 pass 发生，但引擎半段的 bind 在**帧首**读一次当前快照 ⇒ 该帧所有
  pass 都对着同一份值展开。也就是说**一个钩子写的内容对该帧所有 pass 可见**，而不是"只对排在它后面的 pass 可见"（N16）。
  若某个 pass 必须看不见某段内容，那是**声明**问题（给它自己的场景），不是调度问题。
- **否决**：D8（只搬 expand、`executeFrame` 留 UI —— 半个线程）、D9（构建完就地跑完 —— 那就是 inline）、
  D10（流水线深度 > 1）、D11（UI 线程回调 `onPresented`）。理由见 §7。
- **验收**：T9 / T10 见 §3.1.3、§3.1.4，另加"两种运行位置同图"（T12）。

### 2.13 发布可以由宿主接管

- **是什么**：宿主可以 `Scene::publish()` 自己发、再把值交给 `RenderEngine::bindSnapshot(scene, ref)`；
  一旦绑定，这个引擎**不再**隐式发布该场景（否则宿主指定的那份会在下一个帧首被覆盖，绑定就成了空操作）。
  没绑定过的场景由引擎在帧首隐式发布（迁移期的便利，5b-2c 删除）。
- **为什么**：发布是内容侧的动词、绑定是引擎侧的动词（`G2`）；把两个动词分开，宿主才能说"这一帧画的是我刚刚发的
  那一份"，而不是"跑的时候恰好是什么"（`G4`）。
- **边界**：一个引擎对一份场景只保留一个绑定；绑定是**每个引擎**的，所以两个引擎可以各画各的一份（编辑器 + 离屏烘焙）。

### 2.14 跨侧载荷是值：三份 description + `LightValue` + `ContentRef`

- **是什么**：内容对象各有“值半边”，与身份一起过界：
  - `GeometryDescription`（形状）、`MaterialDescription`（材质值）、`ShaderProgramDescription`（程序文本 + id + revision，
    新增）；三者都是 **按 revision 缓存**的不可变值（`X::description()`）。
  - `ContentRef`（`id + revision`）是事实表的键：`RenderCommand` 带 `geometry_ref` / `material_ref`，`program_description`
    自带 id+revision。
  - `LightValue`（新增）是灯的值半边；pass 描述自己时（`RenderPass::describe` / `ScreenPass::describe`）把场景的灯快照成
    `DrawDescription::light_values`，程序 override 同时快照成 `PassDraw::program_override_description`。
- **为什么**（`G1` / `G4` / `G10`）：**唯一一类真实 UB** 是 UI 改灯/材质/程序与引擎线程读同一非原子字段（R3-1）。
  值通道让“帧 N 画它描述时的那份值”成为值语义，而不是约定；每条命令少 3 次 `intrusive_ptr` 原子增减。
  “把 revision 改 atomic”只修一个字段、不修灯与程序文本，**已否决**（`D12`）。
- **边界**：活指针字段（`RenderCommand::geometry/material/program`、`PassDraw::program_override`、
  `DrawDescription::lights`）**保留到 5b-2c**（`C11`：公开 API 只增不改）；记录侧与事实表只读值，活指针只做身份与寿命。
  程序/材质的值是通过 `detail::refreshValues` 在发布边界重取的（它们不经节点 mutator，epoch 看不见）。

### 2.15 发布与展开是两个对象（`G2` / `G12`）

- **是什么**：`detail::PublicationStore` 持有"每个内容一份 `SnapshotSource`"的表与那把锁，动词是内容侧的
  `publish` / `bindSnapshot` / `hasExplicitSnapshot`（外加表级 `beginFrame` 与展开时用的 `sourceOf` 查找）；
  `detail::FrameExpander` 只读这张表，动词是引擎侧的 `runPass` / `run`（bind 每个 draw + expand），并记 `unbound()`。
- **为什么**：内容半段（`buildFrameValue`）要发布，引擎半段要展开。两者放在一个 `FrameWorker` 上时，内容半段必须
  先拿到引擎半段的对象才能发布——对象级依赖指向了错误的一侧（`G2`），类名说的又是引擎半段（`G12`）。拆开后依赖
  只有一条边：expander → store，store 不认识 expander。
- **边界**：拆的是对象，不是线程。`FrameRunner` 仍只把 `FrameExpander::run(...)` 递给引擎线程；`bindSnapshot` 仍
  必须发生在帧与帧之间（`PublicationStore` 的类注释钉住这条不变量），所以"发布创建"进握手之前不允许帧重叠。
- **发布粒度**：`buildFrameValue` 先跑完本帧所有 `before_draw`，再按"每帧每场景一次"发布，最后才 describe。同一
  场景 N 个 pass 因此只发布一次（旧形状是 N 次，后 N-1 次 `Unchanged` 仍付一次锁 + 一次 epoch 比较）；而"先发布、
  再跑后面的钩子"会把后面钩子写的内容漏出本帧，所以钩子必须全部先跑完（N16 的"所有 pass 看到同一份"由此成立）。
- **验收**：`T34` / `T17` / `FrameRunnerTest.ManyFramesThroughTheEngineThreadStayEquivalent`；内容半段不再出现
  旧 `FrameWorker` 这个词（该类已删除）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 两个世界的建立 | 宿主建好内容；引擎半段在帧首拿到该内容的已发布快照 | 已落地 |
| F2 | 发布协议 | 一次发布 = 一次指针交换；三态答案 + 快照句柄 | 已落地（5b-2a） |
| F3 | 场景图结构更新 | attach / detach / 重父化 / 换根**不需要任何通知**：下一次发布的内容就是新的结构 | 已落地 |
| F4 | 节点状态更新 | placement / opacity / layers / drawOrder 折叠进下一次发布；隐藏 = 不产生行 | 已落地 |
| F5 | 几何、材质与程序值的更新 | 行携带 `(id, revision)` 与值；旧帧读旧值 | 已落地 |
| F6 | 相机更新 | 相机是内容、事件即刻生效；查询用被答完那一帧的快照（N17） | 已落地 |
| F7 | 字节内容与流式 | `Buffer::edit()` 只推进 revision；引擎侧按驻留条目轮询（已知残余，见 §2.4） | 已落地 |
| F8 | 每帧 | 内容半段：钩子 → 发布 → describe；引擎半段：bind → expand → execute → present → 报告 | 已落地 |
| F9 | 表面事件与交换链 | 尺寸是帧自己的值；设备动词是帧间操作；呈现结局是值 | 已落地 |
| F10 | 关闭与拆解 | 停帧 → 引擎半段收尾 → 引擎线程释放 → join | 已落地 |
| F11 | 并行 select | 派生层按块并行，与串行逐位相同 | 未做（纯函数是它的前提） |
| F12 | `inline` 模式 | 不建线程，"引擎线程"就是调用线程 | 已落地（默认） |
| F13 | 违例可诊断 | 发布是唯一的可变 → 不可变转换；纯派生 + 逐项 diff 用例是判据 | 已落地（T34 / T17） |

### 3.1.1 F1 / F2 / F3 / F4 / F5：发布就是内容侧的"更新协议"

- **行为**：宿主改内容（结构、放置、状态、几何、材质），然后 `publish()`（或让引擎在帧首隐式发布）。一次发布把
  新结构、新折叠值、新素材值与灯集一起变成一份新快照；未变的块与新值共享，未变的行不复制。
- **约束**（C18 / C19）：批内不发布；材质以**值**过帧。
- **要求**（R9 / R13 / R17）：一次交换、在飞帧逐项不变、批 = 一次发布。
- **验收**（T3 / T17 / T31 / T34）：`SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture`、
  `FrameExpanderTest.AFrameExpandsTheSamePictureTheSceneWould`、
  `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch`、
  `SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith`、
  `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames`。
- **注意**（§2.4）：字节 revision 只回答"字节换了"，不回答"在哪读"；形状与值的变化走发布（§2.14）。

### 3.1.2 F8 每帧

- **行为**：内容半段（`RenderEngine::buildContentHalf`）按 run order 跑每个声明的 `before_draw`、发布该 pass 的内容、
  把 pass 描述成 `FrameDescription`；引擎半段（`runEngineHalf`）为每个 pass 绑定快照、展开命令、交给 executor 执行并呈现。
- **约束**（C16）：两个半段各归一条线程；一条引擎线程碰设备。
- **要求**（R16）：`frame()` 在内容线程只花 O(passes) 加一次握手；等待必须显式。
- **验收**（T12 / T16）：`FrameRunnerTest.ManyFramesThroughTheEngineThreadStayEquivalent`（64 帧逐项相同）、
  `HeadlessBackendTest.PrepareHandsOverTheSameFrameTheNextFrameRunsAndRunsNothing`。
- **注意**（N15）：画面比内容晚一帧。

### 3.1.3 F9 表面事件与交换链

- **行为**：尺寸是**帧自己的值**（帧首交给 target，只在变化时）；**设备动词（`createSurfaceTarget` /
  `setWindowHandle` / `device->initialize`）是帧间操作**，宿主先 `waitForFrame()`；每帧的呈现结局是
  `PresentOutcome{Presented, Skipped, Lost}` 的值（`RenderEngine::presentRecord()`）；`Lost` 由**引擎**修 ——
  忘掉已告知的尺寸，下一帧重报，目标据此重建交换链（计一次 device wait）；**宿主**只在句柄真的变了/为空时重 attach，
  且**不**因为一张丢掉的图去改变区域状态（隐藏会让修复帧停掉）。
- **约束**（C17）：present 与交换链重建只在引擎线程；UI 不持有交换链句柄，也不写引擎侧任何状态。
- **要求**（R9 / R16）：重建只在帧首、只随尺寸变化；设备拒绝时跳过这一帧而不计错；`deviceWaits` 与跳过原因可见；
  UI 调用不因 present 或交换链重建而阻塞。
- **验收**（T9）：`RenderEngineTest.TheAreasSizeAnnouncedBeforeInitializeReachesTheBackend`、
  `RenderEngineTest.TheFramesPictureOutcomeIsAFactOfTheFrame`。
- **注意**（§6.3）：Wayland"未映射拿不到 extent"的回退仍在 UI 侧决定（`appfw-render-surface.md` F5）；另有 N15。

### 3.1.4 F10 关闭与拆解

- **行为**：`stop()`：投"不要再要帧" → 引擎半段跑完手上这一帧（或丢弃）→ 在**引擎线程**释放 present target /
  会话 / 设备 → 线程退出 → join（有界，超时报错）。
- **约束**：引擎线程**不得**同步等 UI 线程（否则与"UI 不得等 worker"互相等死）。
- **要求**（R10）：`stop()` 之后引擎侧分配的一切已释放（ASan 判据）；未处理的帧可观测。
- **验收**（T10）：`stop()` 无泄漏；重复 `setFrameThreading` 稳定。
- **注意**：析构顺序：引擎线程必须在设备与表之前结束。

### 3.1.5 F11 / F12 / F13

- **F11 并行 select**：候选是派生层的"选择"段（纯值）；阈值以 2000 drawables 为起点（R11）。验收：同一快照同一视图，
  P=1 与 P=N 的命令序列逐元素相等（`SelectTest.SameInputSameList`，未做）。
- **F12 `inline`**：默认；`frame()` 就地跑完两个半段并答 `Ran`。既有用例与无头路径用它，不改一行（R12）。
- **F13 违例可诊断**：本层现在只有一条"可变 → 不可变"的边界（发布），所以判据是**值的等价**：发布的源与直接收集在
  600 帧逐项相同（T34），旧帧读旧值（T17）。漏掉一次发布的表现是"画面停在旧图上"，由 T34 抓。

## 4. 公开接口

### 4.1 内容侧（`vn::graphics`）

| 接口 | 契约 | 备注 |
|---|---|---|
| `Scene::publish() → ScenePublishResult` | 一次发布：`Posted` / `Unchanged` / `InsideBatch`，答案带上那一份 `SceneSnapshotRef` | `Scene.hpp` |
| `ScenePublishResult::snapshot` | 这次调用之后作为答案的那一份（批内 = 批开始时的那份） | 同上 |
| `Scene::beginEdit()` / `endEdit()` | 批的边界；只有最外层释放发布 | 已存在 |
| `Node::id()` / `visible` / 状态 setter | 内容的编辑 API，全部留在内容线程 | 已存在 |

### 4.2 引擎侧（`vn::graphics`）

| 接口 | 契约 | 备注 |
|---|---|---|
| `RenderEngine::bindSnapshot(scene, ref)` | 说"这个引擎从这一份开始画"；**接管**该场景的发布（不再隐式发布） | 新（2026-11-12）；**迁移 API，尚未成为宿主契约**（`W2`） |
| `RenderEngine::frame() → FrameOutcome` | 构建 + 交出（`Submitted`）或就地跑完（`Ran`） | 已存在 |
| `RenderEngine::setFrameThreading(FrameThreading)` | `Inline`（默认）/ `EngineThread`；生命周期决定 | 已存在 |
| `RenderEngine::waitForFrame(n, timeout)` | 唯一显式等待 | 已存在 |
| `RenderEngine::presentRecord()` / `presentedCamera()` | 上一帧的呈现结局 / 那一帧的相机快照（N17） | 已存在 |
| `RenderEngine::prepare()` | 只建不提交的预热（见 `graphics-frame-pipeline.md` F7） | 已存在 |

### 4.3 不变的部分

- 内容的编辑 API（`Scene` / `Node` / `Geometry` / `Material`）不变，全部在内容线程同步调用。
- `DrawListSource`（"这个视图画什么"）不变：`Scene` 与 `SnapshotSource` 是它的两个实现。

## 5. 约束、要求与不变量

### 5.1 约束（C#）

**编号说明**：`C1–C15` 属于镜像 / delta 那一代形状，随 5b-2b 删除而**退役**，编号不复用（代码里对 `C8` / `C12` /
`C14` / `C15` 的旧引用指的是历史）。现行约束如下：

| # | 约束 |
|---|---|
| C16 | 一帧的两个半段各归一条线程：内容半段（钩子 / 发布 / describe）在内容线程，引擎半段（bind / expand / execute / present）在引擎线程；帧值两份轮换，每侧一帧在飞。 |
| C17 | 表面事件只在帧首应用一次；交换链只在引擎线程重建；UI 侧不写任何引擎侧状态。 |
| C18 | 批内版本不推进：`beginEdit()` 之后、最外层 `endEdit()` 之前，发布停在原处 —— 落在批中间的帧**不发布任何东西**，画批次开始时的状态；整批在 `endEdit()` 之后那一帧作为**一次**发布落地。 |
| C19 | 材质以**值**过帧：命令携带 `MaterialDescriptionPtr`（`revision` + 解析后的参数），引擎侧只读值，不读 `Material::resolved()`。值的取点在内容侧，批内不取。 |
| C20 | 会话形状（pass 集合、资源表、后端保留态）**只在帧与帧之间**改：帧在飞时 `applyPlan` / `addPass` / `removePass` 一律拒绝（`sessionChangeAllowed`），并按 episode 报**一条** `PassProtocolViolation`。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R8 | UI 调用不阻塞等 present；唯一会等的是"上一帧的引擎半段"，需要"这一帧上屏了"必须显式 `waitForFrame`。 |
| R9 | 发布是一次原子指针交换，成本 O(变了的块 + 变了的素材值)。 |
| R10 | `stop()` 后引擎侧无泄漏。 |
| R11 | 小场景不并行（阈值以 2000 drawables 为起点）。 |
| R12 | 既有用例与无头路径在 `inline` 下不改一行。 |
| R13 | 在飞帧读旧值：材质 / 几何 / 程序任一编辑都不改变在飞帧的逐项内容。 |
| R15 | 机器人 28~70 关节每帧变化时，引擎侧重算量只与链路成正比，且每帧零分配。 |
| R16 | 线程化之后 `frame()` 只花 O(passes)（构建 + 交出）加一次握手；expand 与 present 不在调用者的关键路径上。 |
| R17 | 一次仿真步 / 一次装配 = **一次发布**，不是一关节一次。 |

### 5.3 不变量（N#）

**编号说明**：`N1–N14` 属于镜像那一代形状，退役；N15–N17 仍现行（代码里引用它们）。

| # | 不变量 | 它杀哪类 bug |
|---|---|---|
| N15 | 线程化之后画面比内容晚一帧（引擎半段跑的是上一帧）；`waitForFrame` 是唯一"等到那一帧"的手段。 | 把"已提交"当"已上屏" |
| N16 | 一帧之内只有一份发布：钩子写的内容对该帧所有 pass 可见。 | 帧内半新半旧 |
| N17 | 相机在内容侧、事件即刻生效，画面是上一帧的相机快照。所以**把指针变成世界点的查询（拾取、锚点）用"用户看到的那一帧"的相机**：`RenderEngine::presentedCamera()` 发布它，经 `SceneView::setPresentedCamera` 到达 manipulator；没有发布值时退回当前相机。 | 低帧率下枢轴/选择偏一帧 |

## 6. 注意事项与陷阱

### 6.1 P1 把已提交当成已上屏

- 会怎么错：`frame()` 返回即认为画面上已经是这一帧；截图/测量读到上一帧。
- 怎么判：`presentRecord().frame` 与 `frameNumber()` 的比较。
- 怎么防：要"这一帧上屏"就 `waitForFrame(n)`（R8 / N15）。

### 6.2 P2 在帧中间改会话形状

- 会怎么错：pass 表与后端保留态在录制中被换掉，命令引用已释放的对象。
- 怎么判：`applyPlan` 答 `SessionBusy`（C20；名字指“引擎里有帧在录”，SDK 里没有 `Session` 类型）。
- 怎么防：形状只在帧与帧之间改（`graphics-frame-pipeline.md` F6）。

### 6.3 P3 在帧中间做设备动词

- 会怎么错：`createSurfaceTarget` / `setWindowHandle` / `initialize` 改的是引擎线程正在记录的会话。
- 怎么判：TSan 或再现场的崩溃。
- 怎么防：宿主先 `waitForFrame()`；尺寸只经 `RenderSurface::resize` 作为值交给帧（§2.6）。

### 6.4 P4 批被当成事务

- 会怎么错：调用方期望批内失败回滚，而批只界定可见性。
- 怎么判：T31 的语义。
- 怎么防：文档写清"批不是事务"（`Scene::beginEdit` 的注释）。

### 6.5 P5 拿活相机查询

- 会怎么错：相机在帧间移动，查询用新相机回答上一帧的画面。
- 怎么判：T20（`graphics-frame-pipeline.md`）。
- 怎么防：查询吃 `presentedCamera()`（N17）。

### 6.6 P6 在 `before_draw` 里请求帧

- 会怎么错：内容半段在一个帧的构建里再要一帧，重入编译。
- 怎么判：帧号不连续 / 崩溃。
- 怎么防：钩子只写内容，不驱动引擎（F8）。

### 6.7 P7 把并行放进引擎半段

- 会怎么错：两条线程同时碰 vsg 的 context。
- 怎么判：TSan。
- 怎么防：并行只在派生层的纯值上（§2.7 / C16）。

### 6.8 P8 `AllocationGate` 与并行段

- 会怎么错：把并行段的分配算进"帧内零分配"的统计窗口，门禁变成假阳性。
- 怎么判：门禁用例在并行落地后开始失败。
- 怎么防：并行段不进 `AllocationGate` 的统计窗口（F11）。

### 6.9 P9 `*collectRenderCommands(...)` 直接放进 range-for

- 会怎么错：临时 `shared_ptr` 在循环结束时析构，而循环体持有它内部的引用 —— 悬垂。
- 怎么判：ASan / 随机崩溃。
- 怎么防：先把答案存进一个具名变量再用。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | 内容编辑变成异步（提交到队列） | 文档编辑器的核心手感是同步；把复杂度推给每个使用者（`G7`） | ADR-2026-11-12 |
| D2 | 给场景图加锁 | 锁会进到每一次读路径，与稳态零分配冲突（`G1` / `G10`） | ADR-2026-11-12 |
| D3 | 每帧 extract / sync | 每帧 O(drawables) 拷贝，把成本留在内容线程（`G10`） | ADR-2026-11-12 |
| D7 | 持久镜像 + delta 通道 | 曾是本文件的主张；五套"副本过期"答案的根源，5b-2b 删除（`G1`） | ADR-2026-11-12 |
| D8 | 只搬 expand、`executeFrame` 留 UI | 半个线程：UI 仍付 present 的等待（`G3`） | ADR-2026-11-12 |
| D9 | 构建完就地跑完 | 那就是 `inline`，没有并行可言 | ADR-2026-11-12 |
| D10 | 流水线深度 > 1 | 多一份在飞帧换来的是内存与延迟，没有实测收益（`G8`） | ADR-2026-11-12 |
| D11 | UI 线程回调 `onPresented` | 引擎线程回头读 UI，两个写者再次相遇（`G2`） | ADR-2026-11-12 |
| D12 | 表面事件通道 | 一帧在飞 + 载值已经能带过引擎半段要的每个值（`G8`） | ADR-2026-11-12 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 | 状态 |
|---|---|---|---|
| T3 | 批 = 一次发布 | `SnapshotSourceTest.AnEditBatchKeepsTheSourceOnTheOldPicture` + `PublicationStoreTest.PublishAnswersPostedUnchangedAndInsideBatch` | 已实现 |
| T9 | 尺寸与呈现结局 | `RenderEngineTest.TheAreasSizeAnnouncedBeforeInitializeReachesTheBackend` / `TheFramesPictureOutcomeIsAFactOfTheFrame` | 已实现 |
| T10 | 关闭相位 | `FrameRunnerTest` 的 stop / restart 用例 + ASan 运行 | 已实现 |
| T12 | 两种运行位置同图 | `FrameRunnerTest.ManyFramesThroughTheEngineThreadStayEquivalent`（64 帧逐项相同） | 已实现 |
| T16 | `frame()` 只花 O(passes) | `FrameRunnerTest` 的 inline / threaded 计数断言 | 已实现 |
| T17 | 旧帧读旧 revision | `SnapshotSourceTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith` | 已实现 |
| T31 | 批内 publish | 见 T3（同一组用例） | 已实现 |
| T34 | 发布源与直接收集逐项相等 | `SnapshotSourceTest.ThePublishedSourceMatchesTheSceneAcrossSixHundredFrames` + `FrameExpanderTest.AFrameExpandsTheSamePictureTheSceneWould` | 已实现 |
| T35 | 绑定即渲染真值 | `FrameExpanderTest.ABoundPublicationIsThePictureTheFrameDraws` | 已实现（2026-11-12） |
| T13 | 无数据竞争 | `scripts/tsan_check.sh` | 收尾跑 |

## 9. 状态与演进

### 9.1 当前状态

- **已落地**：两个世界与发布（F1–F7、F12）、帧的两个半段与引擎线程（`setFrameThreading` / `waitForFrame` /
  `presentRecord` / `presentedCamera`）、表面与交换链归属（F9）、关闭相位（F10）、宿主接管发布（§2.13）。
- **未做**：F11（派生层并行）—— 它是纯函数的自然延伸，没有消费者之前不建（`G8`）。
- **迁移期的形状**：引擎在帧首为**未绑定**的场景隐式发布（§2.13）；5b-2c 删 `addPass` 家族时一并删除，
  那时"发布"只有一个动词、一个调用者（`graphics-frame-pipeline.md` §9.3）。

### 9.2 已知缺口

- **派生层并行**（F11）：只有测量给出"选择段占帧时间 > 预算"的证据才做。
- **公开面没有"这一帧画了哪一份"的回读**：`FrameResult` 的目标形状（`graphics-frame-pipeline.md` F12）会补上
  `snapshot` / `view`；现在的等价物是 `presentRecord().frame` 加宿主自己保存的 `SceneSnapshotRef`。

### 9.3 兼容性 / 迁移

本文件描述的是**现在**，不是目标。目标形状与分步迁移在 `graphics-frame-pipeline.md` §9.3；两者出现分歧时以
那里的状态字段为准（它逐条标了哪一步已落地）。

## 10. 参考

### 10.1 代码

- `src/viz/graphics/sdk/vine/graphics/Scene.hpp` / `src/viz/graphics/src/Scene.cpp`（`publish` / 批 / 发布值）
- `src/viz/graphics/sdk/vine/graphics/SceneSnapshotRef.hpp` / `src/viz/graphics/src/SceneSnapshotRef.cpp`（快照句柄）
- `src/viz/graphics/src/SceneSnapshot.{hpp,cpp}`（快照值、分块、`selectSnapshot`）
- `src/viz/graphics/src/ScenePublisher.{hpp,cpp}`（场景的派生侧：烘焙行、per-view memo、发布值）
- `src/viz/graphics/src/SceneRows.{hpp,cpp}`（`SceneBake` → 快照的唯一门）
- `src/viz/graphics/src/SnapshotSource.{hpp,cpp}`（快照的 `DrawListSource` 面）
- `src/viz/graphics/src/PublicationStore.{hpp,cpp}`（内容侧的发布/绑定与 `SnapshotSource` 表）
- `src/viz/graphics/src/FrameExpander.{hpp,cpp}`（引擎半段：bind / expand）
- `src/viz/graphics/src/FrameRunner.{hpp,cpp}`（引擎线程）
- `src/viz/graphics/src/EngineCompile.cpp`（内容半段：钩子 / 发布 / describe / 引擎半段入口）
- `src/viz/graphics/sdk/vine/graphics/RenderEngine.hpp`（`bindSnapshot` / `setFrameThreading` / `waitForFrame` /
  `presentRecord` / `presentedCamera`）
- `src/viz/graphics/src/DrawListMemo.{hpp,cpp}`（per-view 列表的复用与它的两个原子计数）
- `src/fw/appfw/src/gui/SurfaceWindow.cpp`（宿主的两种运行位置与 attach 时机）

### 10.2 用例

- `tests/test_graphics/PublicationStoreTest.cpp`（T3 / T31 的三态发布）
- `tests/test_graphics/FrameExpanderTest.cpp`（T34 / T35：帧级接缝、绑定即真值）
- `tests/test_graphics/FrameRunnerTest.cpp`（T10 / T12 / T16）
- `tests/test_graphics/SnapshotShadowTest.cpp`（T3 / T17 / T34）
- `tests/test_graphics/HeadlessBackendTest.cpp`（两半段与预热）
- `tests/test_graphics/GraphicsTest.cpp`（memo 复用与 T9 的表面尺寸）

### 10.3 文档与 ADR

- `../decisions/ADR-2026-11-12-adopt-graphics-next.md`（四层 / 三个动词 / 不变量；本文件是其中的线程层）
- `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`（快照接管收集的拆分与先后）
- `../decisions/ADR-2026-11-10-render-world-mirrors-content.md`（**历史**：镜像形状的原始决定，已被上面两份取代）
- `graphics-frame-pipeline.md`（目标形状与 §9.3 分步迁移）
- `vsg-backend.md`（后端实现与同步细节）
- `appfw-render-surface.md`（面积与窗口的半段归属）

### 10.4 外部资料

- Qt Quick 的 `sync()` / Unreal 的 `ENQUEUE_RENDER_COMMAND`（对照见 §2.9；本仓库不引入第三方）。
