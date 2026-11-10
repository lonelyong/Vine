# graphics 的线程模型

> **状态**：草案（未落地；契约先定，实现按 `../backlog/graphics.md` T31 的触发条件推进）
> **最后核对**：2026-11-10
> **范围**：graphics 模块与宿主之间"哪一段工作在哪个线程"的完整契约 —— 内容世界与渲染世界各归谁、
> 场景图/节点状态/相机怎么更新到渲染侧、表面与交换链归谁、并行计算放在哪、关闭怎么收。
> **不含** appfw 的通用异步（`appfw-startup-phases.md`、`async-design.md`）与 Vulkan 同步细节（`vsg-backend.md`）。
> **权威来源**：`src/viz/graphics/src/Scene.cpp`（bake / select）、`src/viz/graphics/src/Node.cpp`（两个公告钩子）、
> `src/plugins/gfx_backend_vsg/src/internal/ContentStore.cpp`（事实行）、
> `src/plugins/gfx_backend_vsg/include/vine/vsg/internal/SessionContent.hpp`（表面重读与交换链重建）。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

今天一帧的每一段都在 Qt 应用线程的**一次调用栈**里：`SurfaceWindow::renderFrame()` → `RenderEngine::frame()`
→ `compileFrame()`（收集 + 描述）→ `executor().executeFrame()`（上传、记录、提交、呈现）。而应用线程同时是 UI 线程，
所以渲染与交互互斥。实测（Release）：2000 drawables 移动相机 **1597 µs/帧**（0.94 µs/命令，其中逐命令构造/解析 ~1.3 ms = 71%），
后端记录基线 **33 次分配/录制三角形**，会话初始化 ~184 ms，vsg 在"这一帧什么都没录"时还会在调用线程睡 16 ms。
appfw 已经把应用线程占用当预算（启动门禁 150 ms 项）。

难的不是"开线程"，而是**内容归谁写**：后端的"事实行"每帧从**活内容**上读
（`GeometryFacts.cpp:181` 读 primitive 的实例句柄、`ContentStore.cpp:377` 读 `geometry->revision()`），
收集也要走场景图。只要渲染在另一个线程，两边就必须有一种关系：共享（锁）、拷贝（每帧快照）、或移交（一个写者）。

### 1.2 目标

- 一帧的 GPU 面工作（上传、记录、提交、呈现、驻留记账、剔除）跑在**引擎线程**上，应用线程不承担它。
- **内容的编辑 API 不变**：宿主继续在 UI 线程同步读写场景图（这是文档编辑器的核心手感，不能变成异步）。
- 两个世界之间只有**三条通道**（delta / handoff / snapshot），每条都是值或所有权，**没有一处共享可变状态**。
- **漏发**要编译不过，而不是静默画错。
- 渲染侧与内容侧对同一个视图产出**逐位相同**的命令序列（这是镜像正确性的判据，也是并行化的前提）。

### 1.3 非目标

- 不做通用 job system / 任务图（`async` 与 `ThreadPool` 已有）。
- 不给 `Node`/`Geometry`/`Buffer` 加锁（见 §7 D4）。
- 不把内容编辑变成异步（见 §7 D1 —— 这是被否决的第一稿）。
- 不做远程渲染 / 录制（`OwnedFrame` 的消费者，`OwnedFrame.hpp:101-105` 已划出三类场景）。
- 不做按 pass 的并行记录（见 §7 D6）。
- 不为"每帧动画"新增引擎侧 update traversal：更新的写者仍是内容线程（见 §3 F4）。

### 1.4 一句话结论

**两个世界**：内容世界（UI 线程，作者态，API 不变）与渲染世界（引擎线程，渲染态）。
渲染世界是内容的一份**镜像**（世界矩阵、世界盒、折叠状态、顺序 —— 即今天 `SceneBake` 已经在算的东西），
由内容侧的**变更公告**（delta，payload 必填）增量更新；字节内容不靠公告，靠两个**原子 revision** 轮询。
每帧引擎只在镜像上剔除/建计划/记录/提交/呈现；并行只发生在镜像上的纯计算里。

## 2. 设计要点

### 2.1 两个世界，各自一个写者

- **是什么**：**内容世界** = `Scene`/`Node`/`Geometry`/`Buffer`/`Program`/`Material`/`Texture`/`Camera`，只被 UI 线程写；
  **渲染世界** = 镜像节点、几何代理、材质块、驻留表（`ContentStore`/`StreamUploads`/`ContentHalves`/`ContentSets`）、
  vsg 命令图、交换链，只被引擎线程写。任何一侧都不读另一侧的可变字段。
- **为什么**：两个写者共享一个可变图，只能靠锁 —— 而锁会进到每一次读路径，且与"稳态零分配"直接冲突（`G1`/`G10`）。
  各自一个写者 + 值通道，是唯一能让两边都保持零锁的形状。
- **边界**：唯一的跨侧读取是"已发布不可变值"（§2.4）与"镜像节点持有的指针"（只为保地址，不读字段，见 §2.2）。

### 2.2 镜像就是已经存在的 bake，只是换了主人

- **是什么**：镜像节点 = `(节点身份, 局部变换, 世界矩阵, 世界盒, visible/opacity/layers/drawOrder 折叠值, 子节点顺序)`；
  几何代理 = `(通道切片 + 标量数, primitive 切片（索引/实例）, 材质句柄@revision, program 句柄@revision, 拓扑/计数)`。
  这正是 `detail::SceneBake` + `GeometryFacts` 今天算的两样东西，**内容源与算法都不变，只是搬到引擎侧**。
- **为什么**：不新增概念（`G1`），代价最小 —— 今天的 bake 已经按 `contentEpoch` 缓存、按子树重算；镜像只是把它的
  输入从"读活节点"换成"应用 delta 后的值"（`G7`：复杂性吸收在引擎侧，不推给宿主）。
- **边界**：镜像节点持有 `intrusive_ptr<Node>`，**只为让地址活到停靠窗口结束**（ADR-2026-11-09 的规则），
  **不读它的任何字段**；读字段就等于回到共享可变状态。

### 2.3 三条跨线程通道

| 通道 | 方向 | 装载什么 | 顺序 / 失效 | 违反的形态 |
|---|---|---|---|---|
| **delta**（变更公告） | 内容 → 引擎 | **值**：结构变化（attach/detach/换根）、节点 placement/state、几何描述、相机、表面事件、帧请求 | 队列 FIFO；引擎在**帧首一次 drain**，同帧内按序应用 | 漏发（编译期拦，§2.5）、改了不公告 |
| **handoff**（交接） | worker → 任何一侧 | **所有权**：新构造好的 `Buffer`/`Geometry`/纹理，或整帧（`OwnedFrame`） | 交接后发送方不再触碰；必须过带边界的队列 | 用 `intrusive_ptr` 直接跨界（无 happens-before，`RefCounted.hpp:62-83`） |
| **snapshot**（快照） | 引擎 → 内容/UI | 已发布**不可变值**：`FrameCounters`/`RetentionStats`/`FramePlan`（已有）、镜像校验和、帧号 | 读到的一定是完整的某一版，带帧号 | 把"上一帧的答案"当"现在" |

- **为什么是三条**：它们覆盖"改 / 交 / 读"。原来设计里还有一条"请求"（拾取、包围盒），复核后**删掉**了：
  拾取与包围盒都在内容世界侧（`Camera::screenToWorldRay` + geometry 的 BVH + `Scene::boundingBox`），
  UI 自己就能答，不需要往返（`G8`：没有消费者的通道不做）。
- **边界**：delta 没有返回值；需要"以后完成"的只有"第 N 帧上屏了"（截图的拼法），那是完成句柄，不是查询。

### 2.4 字节内容不靠 delta，靠两个原子 revision

- **是什么**：`Buffer::revision()` 与 `Geometry::revision()` 改成 `std::atomic<std::uint64_t>`（relaxed 读），
  其余一切不变。渲染侧的驻留条目本来就在问"我上传的 revision 还是不是现在这个"（`StreamUploads` 的 live 判据），
  这条轮询**继续用**，只是那次读现在是原子的。
- **为什么**：字节可以在任何线程写（加载 worker、流式写入），把每次 `Buffer::edit()` 变成一条 delta 会让
  "编辑"与"帧"强耦合，且 delta 队列要承受每帧上千条；而轮询的代价已经是 O(#驻留条目)、与内容规模无关（现行设计）。
  两个计数器本来就只被**比较**、单调（`Buffer.hpp:169-181`、`Geometry.hpp:766-770`），加原子不改变语义。
- **边界**：这是**唯一**允许的跨侧读；只读这两个计数器，不读它们指向的任何东西。描述变化（通道集合、切片、primitive）
  仍然必须走 delta —— 轮询只能回答"字节换了"，不能回答"在哪读"。

### 2.5 公告的 payload 必填，漏发编译不过

- **是什么**：`Node::invalidateBounds()` / `Node::announceContentChange()`（`Node.cpp:14-36`）是**唯一**的公告入口，
  今天有 52 处调用点（`AutoTransform`/`Geometry`/`Group`/`MatrixTransform`/`Node`/`PositionAttitudeTransform`/`Scene`/`StateNode`）。
  设计上把它们改成**带 payload 的公告**：`announcePlacement(...)`（placement/state 值）与 `announceDescription(...)`
  （几何描述值），payload 是必填参数。
- **为什么**：漏发 delta 的失败形态是"镜像悄悄陈旧" —— 不报错、画错或漏画，而且只在某些编辑序列下出现（`G11`）。
  把 payload 做成必填参数，任何"改了但没给值"的公告**编译不过**；这是这个设计里唯一能把这类缺陷挡在编译期的地方。
- **边界**：新写的 mutator 如果根本不调钩子，编译期拦不住 ⇒ 第二条网是 §8 的等价性用例（镜像 vs 从零重建）
  与 Debug 下的校验和，见 §6.1。

### 2.6 表面归内容侧（UI），交换链归引擎侧

- **是什么**：表面的创建/销毁在拥有原生窗口的线程（Qt 的句柄与 display 属于 Qt）；`acquire / present / 交换链重建`
  跟着设备走，在引擎线程（`Session` 已有"surface 变了就重读并重建"的路径，`SessionContent.hpp:76`、`Session.cpp:699-712`）。
  UI 把窗口事件变成 delta：`surfaceChanged{handle}` / `extentChanged{w,h}` / `visibility{exposed}`。
- **为什么**：present 必须发生在拥有设备的线程（`RenderDevice.hpp:135-138` 只承诺"一个线程驱动"，没承诺永远是同一线程）；
  而平台的窗口状态只属于创建它的线程（`G2`：依赖方向决定谁调谁）。
- **边界**：extent 变化**只在帧边界应用**；应用前那一帧丢弃且**不算错**；重建会 wait device（计数进 `device_waits`，既有语义）。
  UI 永不阻塞在交换链上；"这一帧上屏了"是完成通知（§3 F9）。

### 2.7 并行只在镜像上的纯计算

- **是什么**：唯一候选仍是收集的**选取**一阶段：镜像按行序切成块，每块独立做层剪枝 + 视锥 + 命令构造 + 排序键，
  按块序合并；排序键每命令预算（`Scene.cpp:992-1000`）+ `std::stable_sort`，所以与串行**逐位相同**。
  镜像在引擎侧，所以并行段只碰引擎自己的数据（比在内容侧并行更干净）。
- **为什么**：这是唯一"输入只读 + 输出顺序可复原"的地方（`G7`/`G8`）。
- **边界**：小场景不并行（200 drawables 141 µs，与投递/合并开销同量级）；`AllocationGate` 的窗口状态非原子
  （`AllocationGate.hpp:27-31`、`.cpp:103-158`）⇒ 并行段不进统计窗口或按线程分桶。

### 2.8 空闲时引擎线程等待，UI 不排帧

- **是什么**：连续渲染不再由 UI 线程的 `QTimer` 驱动；引擎线程在"下一个截止时刻 / delta 到达 / 完成通知"上阻塞等待，
  `requestFrame()` 变成"投一个唤醒"，一帧之内的多次请求合并。
- **为什么**：空闲零 CPU 是既有性质；轮询会让空闲查看器占满一个核（`G10`）。
- **边界**：`RetirementQueue` / `FrameTimeline` 的时间语义不变，仍按帧计。

### 2.9 与其它库的对照：为什么是"持久镜像 + delta"，而不是"每帧 extract"

| 库 | 作者态在哪 | 渲染侧的形态 | 与本设计的关系 |
|---|---|---|---|
| Unreal | 游戏线程（`UWorld`/actor） | 渲染线程的 `FScene` + `FPrimitiveSceneProxy`，变更经 `ENQUEUE_RENDER_COMMAND` 投递 | **本设计的形状**（内容留作者线程，渲染侧持久镜像 + 命令） |
| Qt Quick | GUI 线程拥有 `QSGNode` | 渲染线程每帧 `sync()` 重建自己那份 | 同为两世界，但每帧同步 ⇒ 每帧 O(n)（本设计用 delta 避开） |
| Godot | 主线程节点 | `RenderingServer` 经 `CommandQueueMT` + RID（稳定句柄） | 同为命令队列；RID ≈ 本设计的镜像 id |
| Bevy | 主 world | 每帧 `Extract` 进 render world | 同为两世界，但每帧 extract ⇒ 每帧 O(n) |
| Filament | 单线程 API + job system 并行剔除 | 同线程 | 对应本设计的"帧内并行"（§2.7） |
| three.js / bgfx | 单线程 | 同线程 | 基线：本设计的收益从这里来 |

- **结论**：主流的"作者态在 UI、渲染在别处"引擎（Unreal / Godot / Qt Quick / Bevy）**都保留了作者态的场景图**，
  没有一个是把编辑期的场景搬到渲染线程的 —— 这正是第一稿被否决的原因（§7 D1）。
- **在本仓库为什么选持久镜像而不是每帧 extract**：extract/`sync()` 的形状是每帧 O(drawables) 的拷贝，
  而我们的目标恰恰是把**每帧**的成本从 UI 线程拿走；持久镜像只在**编辑**时付 O(变化的部分)，
  静止场景零成本（`G10`）。
- 依据是这些库的公开架构文档与我们自己的测量，不是逐行读它们的源码；落地前不引用它们的内部细节。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 两个世界与启动握手 | 内容世界由宿主建好 → 引擎侧建一次**全图镜像** → 之后只有增量 | 草案 |
| F2 | delta 协议 | 值 + 必填 payload；帧首一次 drain；同帧按序 | 草案 |
| F3 | 场景图结构更新 | attach / detach / 重父化 / 换根；离树在停靠窗口后释放 | 草案 |
| F4 | 节点状态更新 | placement（局部变换）/ visible / opacity / layers / drawOrder；子树世界矩阵增量重算 | 草案 |
| F5 | 几何描述更新 | 通道集合与切片、primitive（索引/实例）、material/program 句柄@revision | 草案 |
| F6 | 相机更新 | 相机在内容侧；变化发一个相机 delta；`fitToScreen` 留在内容侧 | 草案 |
| F7 | 字节内容与流式 | `Buffer::edit()` 之后只靠原子 revision 轮询；不产生 delta | 草案 |
| F8 | 每帧 | 镜像上剔除 → 建计划 → 记录 → 提交 → 呈现 → 推进时间线 → 发布快照 | 草案 |
| F9 | 表面事件与交换链 | UI 发事件 delta；引擎在帧边界重建；完成通知带帧号 | 草案 |
| F10 | 关闭与拆解 | 停帧 → detach 全图 → 引擎侧释放 → join | 草案 |
| F11 | 并行 select | 镜像分块，按块序合并，与串行逐位相同 | 草案 |
| F12 | `inline` 模式 | 不建线程，"引擎线程"就是调用线程；既有用例与无头路径用它 | 草案 |
| F13 | 违例可诊断与等价判据 | 编译期（payload 必填）+ 运行期（镜像 vs 从零重建的等价比对） | 草案 |

### 3.1.1 F1 两个世界与启动握手

- **行为**：宿主照今天的方式建内容（`view()->scene()`、`Geometry`、材质、链），然后 `start()`：引擎线程建镜像
  （一次全图遍历，等价于今天的第一次 bake），之后进入帧循环；`stop()` 后再写内容才需要重新握手。
- **约束**（C1）：握手之后内容侧**不再**整图遍历给渲染侧看；镜像只靠 delta 前进。
- **要求**（R1）：握手是**一次** O(节点数) 的拷贝，不随帧重复；`inline` 模式下握手是空操作。
- **验收**（T1）：握手后静止场景 N 帧，引擎侧零 delta、零重算。
- **注意**（N1）：握手期间内容侧不得并发改图（握手在同一线程上跑完才启动引擎线程）。

### 3.1.2 F2 delta 协议

- **行为**：`post(Delta)` 接受值：`attach{parent,child}`、`detach{child}`、`reparent{child,parent,index}`、`setRoot{node}`、
  `placement{node, local_matrix}`、`state{node, visible, opacity, layers, draw_order}`、`description{geometry, ContentDescription}`、
  `camera{CameraValues}`、`surfaceChanged{handle}`、`extentChanged{w,h}`、`visibility{exposed}`、`frame{}`、`terminate{}`。
- **约束**（C2）：每个 delta 都是可拷贝的值 + 不持有"还会变的内容"（描述用不可变值，句柄用 `intrusive_ptr` 只保命）。
- **要求**（R2）：MPSC、生产者不阻塞、帧首一次 drain 拿完当时全部；合并是允许的（同一节点多条 placement，后者胜）。
- **验收**（T2）：N 线程并发投递 M 条，总数精确、每线程 FIFO。
- **注意**（N2）：delta 的顺序就是应用的顺序；一个 delta 内的多个字段原子生效。

### 3.1.3 F3 场景图结构更新

- **行为**：`attach` 把一个子树接入镜像（子树描述随 delta 一起给，或由内容侧先建好再 handoff）；`detach` 让镜像
  摘掉子树并把节点**停靠**（`RetirementQueue`，`slots + 1` 帧）后才释放；`reparent` = detach + attach（世界矩阵重算）。
- **约束**（C3）：镜像持有 `intrusive_ptr<Node>` 只为保地址（ADR-2026-11-09）；子树摘除必须停靠，因为已录制的帧可能还命名它。
- **要求**（R3）：`detach` 之后 N 帧内引擎侧该子树的驻留按现行 pin 规则离场（不新增释放路径）。
- **验收**（T3）：attach/detach/reparent 循环若干次后，镜像节点数与内容侧一致，无泄漏（ASan）。
- **注意**（N3）：重父化会重算整棵子树的**世界矩阵**（局部不变），与内容侧 `invalidateBounds` 的传播方向一致。

### 3.1.4 F4 节点状态更新

- **行为**：placement 变化 ⇒ 镜像里该节点子树的世界矩阵重算（局部矩阵 × 父世界矩阵），并沿祖先重算世界盒；
  visible/opacity/layers/drawOrder 变化 ⇒ 只重折该节点子树的折叠值（不重算盒）。
- **约束**（C4）：重算范围 = 变化的子树 + 其祖先，不做整图重算（今天的 `bakeSubtree` 是整图，镜像版要按子树）。
- **要求**（R4）：每帧动画（自转、拖拽 gizmo）就是每帧一条 placement delta；引擎侧**没有** update traversal。
- **验收**（T4）：拖拽一个节点 100 帧，镜像世界矩阵与内容侧一致（逐元素相等），引擎侧重算量只与被拖子树成正比。
- **注意**（N4）：dragging 的手感来自"delta 在帧首被应用"，不会被额外推后一帧。

### 3.1.5 F5 几何描述更新

- **行为**：通道集合/切片、primitive（索引与实例切片）、material/program 句柄变化 ⇒ 内容侧构造一个不可变
  `ContentDescription`（值）随 delta 送出；镜像据此重建几何代理，旧代理停靠后释放。
- **约束**（C5）：`ContentDescription` 只在**形状编辑**时构造，代价 O(#通道 + #primitive)，与帧无关。
- **要求**（R5）：`setMaterial()`/`setProgram()` 这类"只换句柄"的编辑也走同一条 delta（值小）。
- **验收**（T5）：换通道/换切片/换 primitive 后，渲染侧事实行与从零重建的行逐字段相同。
- **注意**（N5）：描述与字节是两件事 —— 换 buffer 是描述变化（delta），改 buffer 里的字节是 revision 变化（轮询，F7）。

### 3.1.6 F6 相机更新

- **行为**：`Camera` 留在内容侧；UI 的 orbit/pan/zoom 照常同步改它，随后发一个 `camera{CameraValues}`
  （视图矩阵、投影矩阵、eye、up、视口），引擎侧把它存进镜像的相机槽位供本帧剔除/投影使用。
- **约束**（C6）：相机值必须与内容侧"这一帧交给渲染的那个相机"一致（同一份值，不在两处各算一次）。
- **要求**（R6）：`fitToScreen()` 留在内容侧（`Scene::boundingBox()` 是内容侧查询），它改完相机后自然发同一条 delta。
- **验收**（T6）：连续 orbit 时镜像相机与内容侧相机逐元素相等；没有 delta 的帧沿用上一帧的值。
- **注意**（N6）：相机不是"快照回读"的必需品 —— UI 读自己的相机对象即可，不需要引擎回传。

### 3.1.7 F7 字节内容与流式

- **行为**：`Buffer::edit()`（或 worker 建好新 buffer 后 handoff）改变字节与 revision；镜像的驻留条目在每帧的
  live 判据里比较**原子 revision**，不等就当旧条目离场、下帧重传。
- **约束**（C7）：字节变化**不产生 delta**；只有"换 buffer / 换切片"才产生。
- **要求**（R7）：流式写入（点云/蒙皮）走"两个 buffer 轮换 + `setPositions`"，或 `edit()` + revision，二者都不新增机制。
- **验收**（T7）：`ContentStoreTest.AStreamWhoseBufferMovedLeavesWithoutTheGeometryChanging` 同族用例在镜像路径上重现。
- **注意**（N7）：原子 revision 只回答"字节换了"，不回答"在哪读"。

### 3.1.8 F8 每帧

- **行为**：drain delta → 应用 → 镜像剔除（每 pass 一个视图）→ 建 `FramePlan` → 记录 → 提交 → 呈现 →
  推进 `FrameTimeline`/`RetirementQueue` → 发布快照（`FrameCounters`/`RetentionStats`/`FramePlan`/帧号）。
- **约束**（C8）：一帧一次 drain；一帧之内不再看队列。
- **要求**（R8）：UI 调用任何入口都不阻塞等待帧结束。
- **验收**（T8）：帧在引擎线程完成（线程 id 断言）；静止场景 CPU ≈ 0。
- **注意**（N8）：`record_micros`（`FramePlan.hpp:77-85`）继续是"这一帧各 pass 花了多少"的观测点，且现在归属引擎线程。

### 3.1.9 F9 表面事件与交换链

- **行为**：UI 发 `surfaceChanged`/`extentChanged`/`visibility`；引擎在帧首应用，必要时重建交换链；present 结果以
  完成通知（`{frame_id}`）回到 UI；需要"这一帧真的上屏了"的调用方等这个通知（不阻塞帧）。
- **约束**（C9）：present 只在引擎线程；UI 不持有交换链句柄。
- **要求**（R9）：重建期间的帧丢弃且不计错；重建后第一帧恢复；`device_waits` 可见。
- **验收**（T9）：连续 resize / 重建窗口后画面正确、无崩溃、UI 无阻塞。
- **注意**（N10）：Wayland"未映射拿不到 extent"的回退仍在 UI 侧决定（`appfw-render-surface.md` F5）。

### 3.1.10 F10 关闭与拆解

- **行为**：`stop()`：投 `terminate` → 引擎自停帧 → detach 全图（停靠条目释放）→ 释放设备/会话/表 → 线程退出 → join（有界，超时报错）。
- **约束**（C10）：引擎线程**不得**同步等 UI 线程（否则与"UI 不得等 worker"互相等死）。
- **要求**（R10）：`stop()` 之后引擎侧分配的一切已释放（ASan 判据）；pending delta 丢弃且可观测。
- **验收**（T10）：`stop()` 无泄漏；重复 `start()`/`stop()` 稳定。
- **注意**（N11）：析构顺序：引擎线程必须在设备与表之前结束。

### 3.1.11 F11 并行 select

- **行为**：镜像行按阈值切成 P 块，块内局部命令 vector + 局部盒缓存，按块序拼接；排序键单线程预算或分块预算 + 稳定归并。
- **约束**（C11）：P=0/1 时与串行**逐位相同**；P 变化不改变输出。
- **要求**（R11）：小场景不并行（阈值以 2000 drawables / 141 µs 为起点）。
- **验收**（T11）：同一镜像同一视图，P=1 与 P=N 的命令序列逐元素相等；`record_micros` 下降。
- **注意**（N12）：并行段不进 `AllocationGate` 窗口（§2.7）。

### 3.1.12 F12 `inline` 模式

- **行为**：不建线程；`post(delta)` 就地应用（或退化为直接调用），`frame()` 与今天完全一致。
- **约束**（C12）：`inline` 下线程断言恒真；`threaded` 下内容写入口不设断言（内容侧本来就是写者），
  但**镜像侧**入口断言引擎线程。
- **要求**（R12）：既有 537 个 vsg 用例与无头路径用 `inline`，不改一行。
- **验收**（T12）：同一套用例两模式行为一致（`inline` 全跑；`threaded` 跑子集）。
- **注意**（N13）：模式是启动参数，两条路径共用同一份帧循环与镜像。

### 3.1.13 F13 违例可诊断与等价判据

- **行为**：（a）编译期 —— 公告 payload 必填（§2.5）；（b）运行期 —— Debug 下引擎发布镜像校验和，
  内容侧在静默时刻（无帧在飞）算同一份校验和并比对；（c）用例 —— §8 的"镜像 vs 从零重建"逐元素比对。
- **约束**（C13）：校验和不进 Release 稳态路径（Debug 或显式开关）。
- **要求**（R13）：新 mutator 漏调钩子时，至少被（c）抓到；漏给 payload 时被（a）挡在编译期。
- **验收**（T13）：故意删掉一处公告，等价性用例失败。
- **注意**（N14）：等价性是本设计的总闸：镜像错了，画面就错了，而它不会崩溃。

## 4. 公开接口

> 本章是**草案**：名字与形状是这一版的提议，落地时以头文件为准。

### 4.1 内容侧（`vn::graphics`，草案）

| 接口 | 契约 | 备注 |
|---|---|---|
| `announcePlacement(...)` / `announceDescription(...)` | 公告的唯一入口，payload 必填 | 取代 `invalidateBounds()`/`announceContentChange()` 的无参形态 |
| `Buffer::revision()` / `Geometry::revision()` | 原子、单调、只被比较 | 唯一允许的跨侧读 |
| `Scene::collectRenderCommands(view)` | 仍是内容侧的纯查询（宿主/工具/等价性用例用） | 与镜像的产出必须逐位相同 |
| `Scene::boundingBox()` | 内容侧查询 | `fitToScreen` 用它，不往返引擎 |

### 4.2 引擎侧（appfw/gui 草案）

| 接口 | 契约 | 备注 |
|---|---|---|
| `RenderWorker::start(mode)` | `inline` / `threaded`；握手点 | 之后镜像只靠 delta 前进 |
| `post(Delta)` | 任意线程、不阻塞、帧首一次 drain | 值 + 保命句柄 |
| `handoff(Owned…)` | 所有权转移，过带边界队列 | 新内容/整帧 |
| `snapshot()` | 已发布不可变值 + 帧号 | 统计/计划/校验和 |
| `onPresented(fn)` | 完成通知 | 截图/离线拼法 |
| `stop()` | 有界 join，超时报错 | 引擎自拆 |

### 4.3 不变的部分

| 接口 | 契约 | 备注 |
|---|---|---|
| 内容编辑 API（`Scene`/`Node`/`Geometry`/`Material`/`Camera`） | **不变**，仍在 UI 线程同步调用 | 这是本设计相对第一稿的关键差别 |
| `RenderDevice` / `RenderFrameExecutor` | 一个线程驱动，不并发、不重入 | `RenderDevice.hpp:135-138` |
| `OwnedFrame` | 跨线程帧的唯一形态 | `OwnedFrame.hpp` |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 握手之后内容侧不再整图给渲染侧看；镜像只靠 delta 前进。 |
| C2 | delta 是值 + 保命句柄，不携带"还会变的内容"。 |
| C3 | 镜像保地址、不读字段；子树摘除必须停靠。 |
| C4 | 重算范围 = 变化子树 + 祖先。 |
| C5 | `ContentDescription` 只在形状编辑时构造。 |
| C6 | 相机值只算一次，两侧用同一份。 |
| C7 | 字节变化不产生 delta。 |
| C8 | 一帧一次 drain。 |
| C9 | present 只在引擎线程。 |
| C10 | 引擎线程不得同步等 UI 线程。 |
| C11 | 并行度 0/1 与串行逐位相同。 |
| C12 | `inline` 模式线程断言恒真；镜像侧入口断言引擎线程。 |
| C13 | 校验和不进 Release 稳态路径。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 握手一次 O(n)，不随帧重复。 |
| R2 | delta 队列 MPSC、生产者不阻塞、drain 拿完当时全部。 |
| R3 | `detach` 后按现行 pin 规则离场，不新增释放路径。 |
| R4 | 每帧动画 = 每帧一条 placement delta，引擎侧无 update traversal。 |
| R5 | 只换句柄的编辑也走同一条描述 delta。 |
| R6 | `fitToScreen` 留在内容侧。 |
| R7 | 流式写法不新增机制（双 buffer 轮换 或 `edit()`+revision）。 |
| R8 | UI 调用不阻塞等帧。 |
| R9 | 重建期间丢帧不计错。 |
| R10 | `stop()` 后引擎侧无泄漏。 |
| R11 | 小场景不并行。 |
| R12 | 既有用例与无头路径在 `inline` 下不改一行。 |
| R13 | 漏调钩子至少被等价性用例抓到。 |

### 5.3 不变量（N#）

| # | 不变量 |
|---|---|
| N1 | 内容世界只被内容线程写；渲染世界只被引擎线程写。 |
| N2 | 渲染世界不读内容对象的字段（两个原子 revision 除外）。 |
| N3 | 镜像的节点集与内容侧一致（除停靠窗口内的已摘除子树）。 |
| N4 | 一次 `present` 对应一次 `acquire`，两者同线程。 |
| N5 | 镜像产出与内容侧 `collectRenderCommands` 对同一视图逐位相同。 |
| N6 | 设备与镜像的重建/销毁只在引擎线程。 |

## 6. 注意事项与陷阱

### 6.1 漏发 delta：画面悄悄错，程序不崩

- **会怎么错**：新增一个 mutator 改了节点状态却忘了公告；镜像永远停在旧值。
- **怎么判**：等价性用例（镜像 vs 从零重建）失败；Debug 校验和不一致；症状是"某个操作之后画面不变"。
- **怎么防**：payload 必填（编译期）+ 等价性用例（运行期/CI）+ Debug 校验和（现场）。

### 6.2 "读了字段"是隐形的越界

- **会怎么错**：镜像/后端为了省事直接读 `node->worldMatrix()` 或 `geometry->primitives()`，编译期完全合法。
- **怎么判**：TSan 报 race；或偶发读到半更新的值。
- **怎么防**：镜像节点类型不暴露内容指针的读接口（只有 `identity()` 与 `intrusive_ptr` 保命）；review 规则 N2；
  两个例外（原子 revision）写在类型上而不是纪律上。

### 6.3 extract 的诱惑：每帧同步一次最省事

- **会怎么错**：为了快速出画面，先做每帧"内容 → 镜像"的全量拷贝（Qt Quick/Bevy 的形状），然后忘记它。
- **怎么判**：静止场景每帧仍有 O(drawables) 的 UI 线程时间（与今天的 1597 µs 同量级 ⇒ 目标没达成）。
- **怎么防**：§2.9 的对照与 §8 的 T8（静止场景 UI 线程零成本）当判据。

### 6.4 停靠与 detach 的先后

- **会怎么错**：detach 立刻释放镜像节点/驻留条目，而已提交的帧还在引用它们。
- **怎么判**：ASan / 偶发崩在提交路径。
- **怎么防**：detach 走 `RetirementQueue`（`slots + 1` 帧），与驻留表的停车窗口同一套。

### 6.5 描述与字节混为一谈

- **会怎么错**：换 buffer 只调了 `bumpRevision()`（以为"变了"），或改字节却发了描述 delta。
- **怎么判**：前者画面停在旧 buffer（键没变、字节也没变，什么都没发生）；后者每帧重建代理。
- **怎么防**：C5/C7；`Geometry.hpp` 已有的那两条注释（`setInstanceTransforms`、`revision()`）就是同一条规则的两个面。

### 6.6 相机两份、差一帧

- **会怎么错**：镜像自己从相机句柄算视图矩阵，而内容侧也算一次 —— 两次结果差一帧或差一次 setter。
- **怎么判**：拖拽/环绕时画面与拾取射线错开。
- **怎么防**：C6 —— 相机值由内容侧算一次，delta 里带值；镜像不再算。

### 6.7 原子 revision 被当成"内容最新"的通用回答

- **会怎么错**：拿 revision 判断"这个几何要不要重建描述"。
- **怎么判**：换了通道集合但 revision 没动（或动了但描述没重建）⇒ 画错布局。
- **怎么防**：revision 只回答字节（C7）；描述一律走 delta。

### 6.8 `AllocationGate` 与并行段

- **会怎么错**：并行 select 进统计窗口，窗口计数被多线程交错更新（状态非原子）。
- **怎么判**：门禁数字抖动/负数。
- **怎么防**：并行段不进窗口或按线程分桶。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | **内容归引擎线程**（宿主只发意图、读快照） | 第一稿的选择，复核后否决：编辑器要同步读写自己的文档（属性面板、gizmo、拾取），异步化是宿主可见的语义损失；且 Unreal/Qt Quick/Godot/Bevy 都保留作者态场景图。 | `../decisions/ADR-2026-11-10-render-world-mirrors-content.md` |
| D2 | 每帧 extract / `sync()`（Qt Quick / Bevy 形状） | 每帧 O(drawables) 的拷贝落在**内容线程**，正是要拿走的那部分成本。 | 同上 |
| D3 | 双缓冲世界（编辑期克隆场景后原子换） | 每次编辑 O(content)；两份场景 = 两份驻留。 | 同上 |
| D4 | 给 `Node`/`Geometry`/`Buffer` 加锁 | 同一概念两个 owner；锁进每条读路径；与稳态零分配冲突（`G1`/`G10`）。 | 同上 |
| D5 | 每帧起线程 / 每帧 join | 破稳态零分配与 `AllocationGate` 门禁；调度延迟大于收益。 | 同上 |
| D6 | 按 pass 并行记录 | 记录读的 `ContentStore`/`ContentHalves`/`ContentSets`/`StreamUploads`/`MaterialArena` 按 pass 交错写；分区或加锁都与 D4 同罪。 | 同上 |
| D7 | 打开 vsg 的 `Viewer::setupThreading()` | 它并行的是 vsg 的记录任务，而每个任务仍读我们按 pass 交错写的表 —— 不解决分区。 | 同上 |
| D8 | 把整帧序列化成描述再交给后端 | 序列化 O(content)；`FrameDescription` 已是借用快照，多一层没有消费者（`G8`）。 | 同上 |
| D9 | 相机/包围盒/拾取的"请求-应答"通道 | 复核后删掉：这三样在内容侧都能自答（`boundingBox`、BVH、相机对象本身），没有消费者（`G8`）。 | 同上 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 握手一次；静止 N 帧零 delta、零重算 | `RenderWorldTest.HandshakeCopiesOnceAndThenIdles` |
| T2 | delta 不丢不重、每线程 FIFO | `DeltaQueueTest.*` |
| T3 | attach/detach/reparent 后镜像节点集与内容一致、无泄漏 | `RenderWorldTest.StructureFollowsTheGraph` + ASan |
| T4 | 拖拽 100 帧后世界矩阵逐元素相等；重算量只与子树成正比 | `RenderWorldTest.ATransformEditRecomputesOnlyItsSubtree` |
| T5 | 换通道/切片/primitive 后代理与从零重建逐字段相同 | `RenderWorldTest.ADescriptionEditRebuildsExactly` |
| T6 | 相机值两侧一致；无 delta 的帧沿用旧值 | `RenderWorldTest.TheCameraIsOneValue` |
| T7 | 字节变化只靠 revision 离场/重传 | `StreamUploadsTest.AStreamWhoseBufferMovedLeaves…`（镜像路径） |
| T8 | 帧在引擎线程；静止场景 UI 线程零成本 | 线程 id 断言 + `scripts/callgrind_diff.py` |
| T9 | 连续 resize / 重建窗口后画面正确、UI 无阻塞 | `RenderControlTest` 系列 + 新增协议用例 |
| T10 | `stop()` 无泄漏；`start()/stop()` 可重复 | ASan 构建 + `test_appfw` |
| T11 | 并行 select 与串行逐位相同、`record_micros` 下降 | `RenderWorldTest.CollectIsIdenticalAtEveryParallelism` |
| T12 | `inline` 与 `threaded` 行为一致 | 同一子集两模式各跑一次 |
| T13 | 删掉一处公告，等价性用例失败 | 变异验证（手工 + 记录在案） |
| T14 | 无数据竞争 | 新增 `-fsanitize=thread` 构建树跑 `test_vsg` 与 `test_appfw` |

## 9. 状态与演进

### 9.1 当前状态

草案。只定契约，未改代码。今天的行为由 `inline` 模式描述。

### 9.2 已知缺口 / 待办

- 落地阶段与触发条件见 `../backlog/graphics.md` T31；并行 select 由 T24 单独触发。
- 多文档 = 多控件 = 多会话（各自的引擎线程与设备），不做"多窗口共享一份内容"的并发编辑。
- `OwnedFrame` 的真实消费者（远程/录制）仍未出现（T18）。

### 9.3 兼容性 / 迁移

- **P1（无触发条件即可做）**：`inline`/`threaded` 两模式骨架 + delta 队列 + 公告 payload 必填 + 等价性用例。
  这一步不改任何宿主代码：`inline` 下行为与今天逐位相同，等价性用例先把镜像钉住。
- **P2**：`threaded` 下的结构/状态/描述/相机 delta 与镜像增量重算。
- **P3**：表面事件与交换链协议，以及 `RenderControl` 的 `state()` 在异步下的重新表述。
- **P4**：并行 select（可与 P2/P3 解耦）。
- 与 `appfw-render-surface.md` §9.2（"会话 init 仍在应用线程；搬到池上需要重新设计……暂不做"）的关系：
  本文是那个设计。落地后线程契约由本文与 ADR 接管，该文继续管 attach 的时机/次序/可见性。

## 10. 参考

### 10.1 代码

- 两个公告钩子与 52 处调用点：`src/viz/graphics/src/Node.cpp:14-36`、`Node.hpp:324`、`Geometry.cpp:398/412`、`StateNode.cpp`。
- bake / select / 排序键：`src/viz/graphics/src/Scene.cpp:937-1000`。
- 事实行读活内容：`src/plugins/gfx_backend_vsg/src/internal/GeometryFacts.cpp:181`、`ContentStore.cpp:377`。
- 驻留的 live 判据：`src/plugins/gfx_backend_vsg/src/internal/StreamUploads.cpp`（`beginFrame` 的谓词）。
- 表面与交换链：`SessionContent.hpp:76`、`Session.cpp:699-712`。
- revision 与写入口：`src/base/core/sdk/vine/Buffer.hpp:169-181, 368`。
- 引用计数没有 happens-before：`src/base/core/sdk/vine/RefCounted.hpp:62-83`。
- 线程契约：`RenderDevice.hpp:135-138`、`RenderFrameExecutor.hpp:52-55`。
- 宿主席位：`src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp`、`src/fw/appfw/src/gui/SurfaceWindow.cpp:832-861`。

### 10.2 文档与 ADR

- `../decisions/ADR-2026-11-10-render-world-mirrors-content.md`（本文的决策、否决与库对照的依据）。
- `graphics-scene.md`（收集与 bake）、`graphics-geometry.md`（通道/revision/拾取树）、`vsg-backend.md`（后端记账成本）、
  `graphics-content-lifecycle.md`（驻留 pin）、`appfw-render-surface.md`（attach 与可见性）、`async-design.md`（协程）。

### 10.3 外部资料

- Unreal Engine：`FScene` / `FPrimitiveSceneProxy` / `ENQUEUE_RENDER_COMMAND` 的公开架构描述。
- Qt Quick：scene graph 的 GUI 线程与渲染线程 + 每帧 sync。
- Godot：`RenderingServer` + `CommandQueueMT` + RID。
- Bevy：render world 的 `Extract` 阶段。
- Filament：单线程 API + job system。
- Vulkan 规范：host access 外部同步（`VkQueue` 与 WSI 的线程规则）。
