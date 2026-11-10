# ADR-2026-11-10：渲染世界镜像内容，内容世界留在 UI 线程

- 状态：已采纳（契约层面；实现按 `../design/graphics-threading.md` §9.3 的阶段与 `../backlog/graphics.md` T31 的触发条件推进）
- 日期：2026-11-10
- 相关：`../design/graphics-threading.md`、`../design/graphics-scene.md`、`../design/vsg-backend.md`、
  `../design/appfw-render-surface.md` §9.2、`../backlog/graphics.md` T24/T31

## 背景

今天一帧的每一段都在 Qt 应用线程的一次调用栈里（`SurfaceWindow::renderFrame` → `RenderEngine::frame()`
→ `compileFrame` → `executor().executeFrame`），而应用线程同时是 UI 线程。成本已经量出来：2000 drawables 移动相机
1597 µs/帧（逐命令构造/解析占 71%），后端记录 33 次分配/录制三角形，会话初始化 ~184 ms，vsg 在空记录帧上睡 16 ms。
appfw 已经把应用线程占用当预算（启动门禁 150 ms 项）。

难点是**内容归谁**：后端的事实行每帧从活内容上读（`GeometryFacts.cpp:181`、`ContentStore.cpp:377`），
收集也要走场景图。所以"渲染搬到另一个线程"必然要在两侧之间定一种关系。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| **A. 内容世界留在内容线程（UI），渲染世界镜像它，靠 delta 增量更新** | 内容编辑 API 不变（同步读写自己的文档）；渲染侧零锁；静止场景零成本（只在编辑时付 O(变化部分)）；只有三条值通道 | 需要镜像与 delta 协议；公告必须**穷尽**（漏发 = 镜像悄悄陈旧）；形状编辑要构造一次描述值 |
| B. 内容归引擎线程，宿主只发意图、读快照 | 无需镜像 | 编辑器要同步读写自己的文档（属性面板、gizmo、拾取）就做不到；每个宿主与用例都要改写；Unreal/Qt Quick/Godot/Bevy 都保留作者态场景图 |
| C. 每帧 extract / `sync()`（Qt Quick / Bevy 形状） | 协议最简单（每帧全量拷一份） | 每帧 O(drawables) 的拷贝落在内容线程 —— 正是要拿走的那部分成本 |
| D. 双缓冲世界（编辑期克隆场景，帧边界原子换） | 内容侧可同步读写 | 每次编辑 O(content)；两份场景 = 两份驻留 |
| E. 维持现状（UI 线程全做），只做并行 select | 零迁移；先拿到收集的 71% | UI 线程仍承担上传/记录/呈现；空记录帧的 16 ms 与 184 ms 的会话 init 仍在 UI 上 |
| F. 给内容模型加锁 | 编辑与渲染可并发 | 同一概念两个 owner；锁进每条读路径；与稳态零分配冲突（`G1`/`G10`） |

## 决定

选 **A**，并把边界画在值上：

1. **两个世界，各自一个写者**：内容世界（`Scene`/`Node`/`Geometry`/`Buffer`/`Program`/`Material`/`Texture`/`Camera`）
   只被内容线程写；渲染世界（镜像节点、几何代理、材质块、驻留表、vsg 命令图、交换链）只被引擎线程写。
2. **镜像 = 已经存在的 bake**：世界矩阵、世界盒、折叠状态、顺序（`SceneBake` 今天算的东西）搬到引擎侧，
   输入从"读活节点"换成"应用 delta 后的值"。镜像持有 `intrusive_ptr<Node>` 只为保地址，**不读字段**。
3. **三条通道**：delta（值，payload 必填 ⇒ 漏发编译不过）、handoff（所有权，过带边界队列）、
   snapshot（已发布不可变值 + 帧号）。原设计的"请求"通道**删掉**（拾取/包围盒/相机在内容侧都能自答）。
4. **字节内容不靠 delta**：`Buffer::revision()` / `Geometry::revision()` 成为原子计数，渲染侧继续轮询
   （现行 pin 判据本来就是 O(#驻留条目)、与内容规模无关）。这是唯一允许的跨侧读。
5. **表面归内容线程，交换链归引擎线程**：extent 变化只在帧边界应用，重建期间的帧丢弃且不计错。
6. **并行只在镜像上的纯计算**：收集的选取一阶段分块并行，按块序合并，与串行逐位相同；小场景不并行。
7. **两种模式一套实现**：`inline`（调用线程即引擎线程，今天的行为，测试与无头默认）与 `threaded`，共用帧循环与镜像。

## 后果

- **好的**：宿主侧零语义变化（编辑 API 不动，`inline` 下行为逐位相同）；渲染侧摆脱 UI 线程；静止场景每帧零拷贝；
  镜像的正确性有一个可执行的判据（镜像产出 ≡ 内容侧 `collectRenderCommands` 对同一视图逐位相同），
  并行化也建立在这个判据上；`RenderDevice` 的"一个线程驱动"契约不用改。
- **坏的 / 代价**：多了一份镜像（内存 O(节点数)，与内容本身同阶）；公告必须穷尽，否则镜像陈旧而无任何报错；
  形状编辑要在内容侧构造一次描述值（O(#通道 + #primitive)）；引擎侧要按子树增量重算世界矩阵与盒（新代码）；
  第一次握手是 O(节点数)，大场景加载时一次性付掉。
- **误用的失败形态**：漏发 delta / 读了内容字段 —— 都不崩、只画错，且常在特定编辑序列下才出现（见设计 §6.1、§6.2）。
  因此把"payload 必填"做成编译期约束，并用"镜像 vs 从零重建"的等价性用例当总闸。
- **触发条件（什么时候重开这个决定）**：
  - 需要"内容与渲染在同一帧内互相看见"的真实需求（例如每帧动画的结果必须当帧回读做反应）—— A 允许延迟一帧，届时要么接受延迟，要么为那条数据单独开一条同步通道；
  - 内容线程的编辑成本本身成为瓶颈（大场景里形状编辑的 O(#primitive) 描述构造进入 profile）——那时考虑把描述构造搬到 worker（handoff），不改本决定；
  - `OwnedFrame` 的真实消费者（远程/录制）落地，跨线程帧的形态以它为准。

## 证据

- 现状全在应用线程、无渲染线程：`src/fw/appfw/src/gui/SurfaceWindow.cpp:832-861`、`src/viz/graphics/src/RenderEngine.cpp:240-313`；
  vsg 的 `setupThreading()` 全树无人调用。
- 事实行从活内容读：`GeometryFacts.cpp:181`、`ContentStore.cpp:377`；驻留 live 判据读 `Buffer::revision()`（`StreamUploads.cpp` 的 `beginFrame` 谓词）。
- 镜像的原料已经存在：`Scene.cpp:937-1000`（bake / 逐视图 memo / 排序键）。
- 公告钩子与 52 处调用点：`Node.cpp:14-36`；`Geometry.cpp:398/412`；`StateNode.cpp`。
- 两个 revision 只被比较且单调：`Buffer.hpp:169-181`、`Geometry.hpp:766-770`；存储是非原子 `std::uint64_t`（`Buffer.hpp:368`）。
- 线程契约只承诺"一个线程"，不承诺哪个线程：`RenderDevice.hpp:135-138`、`RenderFrameExecutor.hpp:52-55`。
- 成本数字：`../backlog/graphics.md` T24（1597 µs @2000 / 141 µs @200 / 71% 逐命令）、`vsg-backend.md`（33 分配/录制三角形）、
  `appfw-render-surface.md` §9.2（~184 ms 会话 init）、vsg `RecordAndSubmitTask::finish()` 的 16 ms 空记录睡眠。
- 跨线程引用计数没有 happens-before：`RefCounted.hpp:62-83`（relaxed add / acq_rel release）⇒ handoff 必须过带边界的队列。
- 其它库的对照（据公开架构文档）：Unreal 的 `FScene` + `FPrimitiveSceneProxy` + `ENQUEUE_RENDER_COMMAND`（＝本决定）、
  Qt Quick 的每帧 `sync()`、Godot 的 `CommandQueueMT` + RID、Bevy 的 `Extract`、Filament 的单线程 API + job system。
  它们都保留作者态的场景图，这正是否决选项 B 的直接依据。
