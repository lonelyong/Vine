# ADR-2026-11-10：渲染由引擎线程拥有内容，应用线程只发意图

- 状态：已采纳（契约层面；实现按 `../design/graphics-threading.md` §9.3 的阶段与 `../backlog/graphics.md` 的触发条件推进）
- 日期：2026-11-10
- 相关：`../design/graphics-threading.md`、`../design/appfw-render-surface.md` §9.2、`../design/vsg-backend.md`、`../backlog/graphics.md` T24

## 背景

今天一帧的每一段都在 Qt 应用线程的**一次调用栈**里跑完（`SurfaceWindow::renderFrame` → `RenderEngine::frame()`
→ `compileFrame` → `executor().executeFrame`），而每段的成本已经量出来：2000 drawables 移动相机 1597 µs/帧
（其中逐命令构造/解析 71%），后端记录 33 次分配/录制三角形，会话初始化 ~184 ms，vsg 在空记录帧上还会在调用线程睡 16 ms。
应用线程同时也是 UI 线程，所以渲染与交互互斥；appfw 已经把应用线程占用当预算（启动门禁 150 ms 项）。

问题不是"要不要线程"，而是**内容归谁写**：后端的"事实行"每帧从**活内容**上读（`GeometryFacts.cpp:181`、
`ContentStore.cpp:377`），收集也要走场景图。只要渲染在另一个线程，且它需要读内容，就必须在两者之间定一种关系——
共享（锁）、拷贝（快照）、或移交（一个写者）。`appfw-render-surface.md` §9.2 已经把这题记成"待重新设计"。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| **A. 引擎线程拥有内容，应用线程发意图/读快照/发请求** | 内容模型**零锁**（单写者不变，只是换了线程）；渲染完全离开 UI 线程；GPU 面工作（上传/记录/提交/呈现）与上下文（设备/交换链）同处一线程 | 应用线程不能再同步读/写内容：编辑要变成值，读要变成快照，拾取要两条路径；`start()` 成为所有权移交点，误用在 Release 下静默 |
| B. 应用线程拥有内容，渲染线程吃每帧描述快照 | 应用侧 API 不变 | 事实行今天在 `executeFrame` 里构建，搬走 = 把 71% 的逐命令记账挪到 UI 线程（正好是我们要减的那一半）；事实与**跨帧**驻留表（`ContentStore`/`StreamUploads`/`ContentHalves`/`ContentSets`）耦合，表要在两线程间共享 → 又回到锁 |
| C. 双缓冲世界（编辑期克隆场景，帧边界原子换） | 应用侧可同步读、同步改 | 每次编辑 O(content)：大场景（数十万节点）不可接受；两份场景的驻留也是两份 |
| D. 维持现状（UI 线程全做），只做并行 select | 零迁移成本；先拿到最大的单块收益（71% 在收集里） | UI 线程仍要承担上传/记录/呈现；空记录帧的 16 ms 睡眠仍在 UI 上；会话 init 184 ms 仍在 UI 上 |
| E. 打开 vsg 的 `Viewer::setupThreading()` | 改动小，vsg 自带 | 它并行的是 vsg 的记录任务，而每个任务仍读我们**按 pass 交错写**的表（`ContentHalves`/`ContentSets`/`StreamUploads`/`MaterialArena`）⇒ 不解决分区，只把竞争挪进 vsg 的线程 |

## 决定

选 **A**，并把它的接缝画在**值**上：

1. **一个写者**：内容（`Scene`/`Node`/`Geometry`/`Buffer`/`Program`/`Material`/`Texture`/`Camera`）的写只允许引擎线程；
   `start()` 是所有权移交点（配置期任意线程可改，运行期只有引擎线程）。
2. **四种跨线程动作**：意图（值，帧首 drain）、交接（所有权转移，过带边界的队列）、快照（已发布不可变值，带帧号）、
   请求（需要当前内容才能答，应答回原线程）。**其余一律禁止**，包括"内容 setter 从别的线程调"。
3. **表面归应用线程，交换链归引擎线程**：表面创建/销毁在拥有窗口的线程；acquire/present/重建在引擎线程；
   extent 变化只在帧边界应用，重建期间的帧丢弃且不计错。
4. **并行只在纯计算上**：当前唯一候选是收集的 select 一阶段（bake 之后只读、输出可复原顺序），
   按块并行、按块序合并、与串行逐位相同；小场景不并行。
5. **两种模式一套循环**：`inline`（调用线程即引擎线程，今天的行为，测试与无头默认）与 `threaded`（真正移交），
   共用同一份帧循环；模式是启动参数，不是两套实现。

## 后果

- **好的**：内容模型保持零锁（`G1`/`G10`）；UI 线程不再承担 GPU 面工作；交换链/present 与设备同线程，
  现有"一个线程驱动设备"的契约不用改一个字；`inline` 模式让 537 个 vsg 用例与无头路径零改动；
  并行 select 与线程化**解耦**，可以单独触发、单独验收。
- **坏的 / 代价**：这是宿主可见的语义变化 —— `start()` 之后直接改内容变成违约，编辑要写成意图，
  读取要走快照，拾取要分快路径（快照 + BVH 只读）与准路径（请求）；`RenderControl::state()` 等
  同步语义要重新表述；`GeometryPickStats`、`AllocationGate` 窗口这类"写计数器"的东西在并发下需要按线程计或关掉。
- **误用的失败形态**：Release 下"忘了移交"不报错，只偶发画错/崩 ⇒ 必须有 Debug 断言、TSan 构建与文档 warning（见设计 §6.3）。
- **触发条件（什么时候重开这个决定）**：
  - 出现"应用线程改内容"必须在同一帧内被渲染读到的真实需求（例如编辑器要求 0 帧延迟的 gizmo 跟随）——那会逼出 B 或 C 的一小块；
  - 第二个设备需要与第一个**共享同一份内容**并并发编辑（例如多视口同步拖拽）；
  - `OwnedFrame` 的真实消费者（远程/录制）落地，届时"跨线程帧"的形态以它为准，而不是本 ADR 的请求/快照。

## 证据

- 一帧全在应用线程：`src/fw/appfw/src/gui/SurfaceWindow.cpp:832-861`（`renderFrame` → `engine->frame()`）、
  `src/viz/graphics/src/RenderEngine.cpp:240-313`（compile → execute）——全树没有渲染线程，vsg 的 `setupThreading()` 也无人调用。
- 现有契约只承诺"一个线程"，没承诺"哪个线程"：`RenderDevice.hpp:135-138`、`RenderFrameExecutor.hpp:52-55`。
- 事实行从活内容读：`src/plugins/gfx_backend_vsg/src/internal/GeometryFacts.cpp:181`、`ContentStore.cpp:377`。
- 收集的 bake/select 分离与排序键：`src/viz/graphics/src/Scene.cpp:975-981`、`:992-1000`。
- 成本数字：`.ai/backlog/graphics.md` T24（1597 µs @2000、141 µs @200、71% 逐命令）、
  `vsg-backend.md`（33 分配/录制三角形、12 分配/pass）、`appfw-render-surface.md` §9.2（~184 ms 会话 init）、
  vsg `RecordAndSubmitTask::finish()` 的 16 ms 空记录睡眠。
- 跨线程引用计数没有 happens-before：`src/base/core/sdk/vine/RefCounted.hpp:62-83`（relaxed add / acq_rel release）。
- 并发只读的先例：`.ai/design/geometry-bvh.md:155-158`（BVH 查询 const、非分配、可并发）。
