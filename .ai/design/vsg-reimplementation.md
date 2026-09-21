# vsg 后端"从零重新实现"设计（提案）

> 状态：**设计提案 v2（2026-09-21）**，核心层已开始落地（见 §11），**不改动**现有 `gfx_backend_vsg`。
>
> **实施进度（截至 2026-09-22）**：§11 是逐片的实施记录，每片都带自己的证据面。当前已完成的最后一片是
> **M5d（全屏路径的 128B push：与前向 UBO 同一份灯打包的另一种布局，`projparms` 保留位按 SDK 声明留零；
> 顺带修掉"只读 push 的全屏 pass 建不出管线"这个洞）**：`test_vsg` 547 用例 / 87 套件全绿（含真设备像素用例
> ——一趟三个全屏调用、三个视口，各自对照一条像素），强制验证层 + 同步验证下 **0 VUID / 0 SYNC-HAZARD**，
> `core/` 的 include 边界由 `scripts/check_include_hygiene.py` 机器校验（全树 0 findings / 814 文件）。
>
> v2 修订：按一份外部评审（20 条）重钉了 10 个 P0 定义（见 §2.5），改了架构图（§2.1 两个流 +
> §2.2 六个对象 + §2.3 物理边界），并按评审重写了键的拆分（D3）、资源寿命（D4/D5）、
> 以及循环依赖与 normalize 的语义。评审逐条处理见 §6.2。
>
> 定位：本文回答"如果把 `gfx_backend_vsg` 重做一遍会怎么设计"。**功能面与现有实现等价**
> （§4 是逐条对照表），**内部组织刻意不同**（§6 列出差异与代价）。读"今天怎么做"请看
> `src/plugins/gfx_backend_vsg/docs/backend.md` 与 `docs/data-flow.md`；本文只描述"我会怎么做"。
>
> 前置阅读：`src/viz/graphics/sdk/vine/graphics/RenderBackend.hpp`（契约原文）、
> `ShaderAbi.hpp`（着色 ABI）、`.ai/design/vsg-pass-lifecycle.md`（现有生命周期缺陷表）、
> `vsg-target-resize-in-place.md`（原地改尺寸）、`vsg-pipeline-sharing.md`（管线/动态状态归位）、
> `vsg-upstream-alignment.md`（vsg 上游约束）。
>
> 上游依据：`build/_deps/vsg-src`（**仓库实际编译的那份 vsg，1.1.16**；§9.1 的证据来源）、
> `/opt/opensrc/vsgExamples`（上游用法参考，1.1.15——差一小版，只当idiom参考，不作行为性结论依据）。
>
> **引用约定（便于后续修改）**——本文引用分三类，按此定位；带符号的引用优先于行号：
> 1. **本仓设计/记忆**：`.ai/design/*.md`、`.ai/memory/*.md`（相对仓库根，稳定）；
> 2. **本仓实现/脚本**：写路径 + **符号名或脚本名**（如
>    `src/plugins/gfx_backend_vsg/include/vine/vsg/VsgRetireRing.hpp`、`scripts/gfx_lavapipe_check.sh`）；
> 3. **上游源码**：写 `vsg <tag>` + 仓库内路径 + **符号名**（如
>    `vsg 1.1.16 — src/vsg/app/RecordAndSubmitTask.cpp — RecordAndSubmitTask::start()`）。
>    本地路径 `build/_deps/vsg-src` 只是**便利入口**（FetchContent 产物，清了 `build/` 就没了）；
>    **版本号才是指针**。括号里的行号按 2026-09-21 的检出核对，仅供定位，改动后以符号为准。
>
> 每条事实都要写**复核状态**：`已逐行复核` / `来自既有结论` / `来自脚本断言`——三者不能混用。
>
> 验收口径：§7 的不变量清单 + 现有 `vsg_backend_selftest` 的像素相位 + 证据基线逐字节比对。

## 0. 一句话

后端不是"把场景翻成 Vulkan 调用"，而是**一台把帧意图编译成 GPU 状态的编译器**：
API 是声明意图；`FrameDescription` 是这一帧的事实；`CompiledFrame` 是依赖求值的结果；
`ResourceManager` 管资源生命周期事实；`VsgExecutor` 只是把已经决定好的事实机械地变成 GPU 操作。

这条边界是本设计唯一真正的价值所在：**只要 Protocol / Compiler / ResourceManager 不偷偷操作 vsg 对象、
不做 GPU 决策**，`core/` 四件（Protocol / Compiler / ResourceManager / Timeline）即使把 vsg 换成裸 Vulkan
（甚至换 API）也大体可留；反过来它们一开始越界，"从零重做"的收益就会一点点消失。

**No-VUID is necessary but not sufficient**：验证层干净只说明 API 调用合法，不说明画出了任何东西。
每个相位必须同时满足"验证干净 ∧ 计数器符合预期 ∧ 像素符合预期"（§7）。

## 1. 输入：不可协商的边界

### 1.1 契约（`RenderBackend.hpp`）

| 契约点 | 内容 | 设计后果 |
| --- | --- | --- |
| 帧配对 | `beginFrame()` 可获取交换链图像；**只有** `swapBuffers()` 呈现，且是一帧最后一次调用 | 需要一个"帧令牌"（D5）；空帧也必须提交 |
| pass scope | scope 属性（target / order / depthMode / clear）属于该 pass 并在 `endPass()` 丢弃；每次绘制属性（viewport / lights）只服务下一个绘制调用；无 scope 的绘制**必须拒绝并上报** | 协议是一台状态机，不是一个函数（D1） |
| 借用一切 | 相机 / 命令 / 灯 / target / 程序只在调用期间借用；pass 只在 scope 内借用 | 后端必须"抄下来"而非持有（D3 的 data 层） |
| 增量 | `render()` 必须增量对账；稳态帧不重传几何、不重编管线、不增长 | 三层键 + 键审计（D3） |
| 失败语义 | 不抛异常跨接口；`initialize()` 返回 false 时**后端自己**清理半成品（引擎不会调 `shutdown()`） | 会话必须"要么完整要么不存在"（D4） |
| 保留态 | 可以由 `release*` 释放，但**不得要求**宿主必须调它才正确；不得随帧数增长 | 账本 + park 环（D4） |
| 诊断 | 同步 sink、只记录不回调；无论是否装 sink 都要计数 | 单一出口 + 类型化的"每 episode 报一次"（D8） |
| 线程 | 单线程、串行、不可重入；但不得假设 `initialize()`/`shutdown()` 同一线程 | 全设计不加锁，但会话状态必须整体替换而非逐成员改 |
| 借用必须**当场快照** | 引擎自己的 `resolved_inputs_` 是**复用成员**：每帧 `clear()` + `reserve()` + `push_back()`，`setPassInputs(resolved_inputs_)` 传进来后**下一个 pass 就会改**（`RenderEngine.cpp:515-532`）；`RenderCommand` 同理只活到 `render()` 返回 | **帧内一切声明在到达时就复制进 `FrameArena`**，计划里不得出现指向宿主容器的 `span`（P0-1） |
| 循环依赖无人拦截 | `graphics/src` 里**没有**任何环检测（已核：无 cycle/topological 相关代码），`validateWiring()` 只管声明冲突（谁填谁、谁声明谁的输出） | 后端是唯一能看见环的地方 ⇒ 环必须有**显式失败语义 + 计数**，不能静默按当前顺序执行（P0-7） |

### 1.2 平台事实（不是"现有实现的选择"）

| 事实 | 影响 |
| --- | --- |
| 设备地板 `VK_API_VERSION_1_4`（`VsgBackendUtility.hpp`：`kRequiredVulkanVersion`；注释说明这是**策略**，状态层真正依赖的是 core 1.3 的四个动态状态 + EDS2/3） | 启动即拒绝；版本号要连设备号和原因一起上报 |
| 该 vsg 版本**没有** dynamic rendering、也不传 `VkPipelineCache` | 必须走 render pass + load-op 变体；重建设备等于重编全部管线 ⇒ "移动会话"比"重建会话"值钱 |
| render pass 兼容性**不含** load-op 与 `initialLayout`，但**含**附件格式/采样/深度格式 | 同一附件集合可以有多份"变体"（兼容、可换），形状变化无法用变体绕过 |
| vsg 的 `GraphicsPipeline` 按 **viewID** 编译，且复用循环只比 pipeline states、不比 render pass | 每个 pass 一份状态注册表是**硬约束**，不是优化 |
| push constant 保证预算 128B | 光照 + 视图必须挤进一个块（`LightPushBlock`），前向着色只能走 UBO |
| 顶点 ABI：位置 0 / 法线 1 / 颜色 2 / UV **8** / 自定义 ≥3；四个块 288-80-64-80 字节（std140，静态断言钉死） | 通道→binding 映射是固定正典序，缺的通道要喂零值载体 |
| reverse-Z（远平面 0）+ 比较算子反转；世界 CCW 为正面 ⇒ framebuffer 空间 CW | 投影、清屏值、比较算子、正面声明必须是**同一处**的四个侧面 |
| X11(XCB)/Win32 双实现，Wayland 无（vsg 该版本没有） | 句柄要按平台**精确类型**转换；Win32 采用宿主窗口时要撤掉自己的 OLE 注册 |

每条事实的上游出处与复核状态见 §9.1：其中"无 dynamic rendering""无 EDS2/3 入口""不使用 `VkPipelineCache`"
三条已在 `build/_deps/vsg-src` 里逐行复核（grep 零命中 / 只出现在拉进来的 Vulkan 头文件里）；
其余各条来自仓库既有结论与脚本断言（`scripts/check_vsg_upstream_capabilities.py`、`vsg-pipeline-sharing.md` 等）。

### 1.3 被迫同形的地方（我会照做，不"创新"）

1. render pass + load-op 变体（上游没有 dynamic rendering）。
2. 每个 pass（= 每个 viewID）一份状态注册表；上传/几何流按内容共享、状态不共享。
3. 128B push block 承载全屏路径的光照+视图。
4. 深度提升 / 借用 / LOAD 保留三件套（SDK 的 `RenderTarget` 就是这么建模的）。

## 2. 架构

### 2.1 两个流，不是一条链

本系统里跑的是**两条互不相同的流**，把它们画在一条链上是 v1 最大的表达缺陷：
帧数据流每帧清零重建，资源寿命流跨帧存在。

```
                         FRAME FLOW（每帧重建）

  API  ──→ FrameRecorder ──→ FrameDescription ──→ FrameCompiler ──→ CompiledFrame ──→ VsgExecutor
            │ 合法性            （自包含快照）        │ normalize/resolve          （可执行计划）      │
            │ （Protocol）                            │ graph/schedule                              │
            └───────────── 意图 ────────────────────┴──────────── 引用（key/handle） ─────────────┘
                                                                         │
                                                                         ▼
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │ ResourceManager  active ──replace──→ retired ──completed frame──→ destroyed  │
  │  （几何/管线/描述符/目标/世代/退役队列；owning）                              │
  └──────────────────────────────────────────────────────────────────────────────┘
                         LIFETIME FLOW（跨帧）

            Observe（Diagnostics / Counters / RetentionStats / Trace）
            ↑ 横切：被上面每一个环节调用，不是一层，也不参与决策
```

### 2.2 六个核心对象 + 一个横切面

原则：**对象数尽量少，但每个对象只回答一个问题**。评审担心"v1 的 L1–L6 六层最后变成
边界复杂的大状态机"，这个担心是对的；下面每个对象都明写"不做什么"。

| 对象 | 只负责 | **不做** |
| --- | --- | --- |
| `VsgBackend : RenderBackend` | SDK 的 31 个方法**逐字**实现 + 转发；持有 `FrameTimeline` | 不知道管线/渲染图怎么建，不碰 vsg |
| `FrameRecorder`（内含 `Protocol`） | 把调用收成 `FrameDescription`；**只判"这次调用是否合法"** | 不决定 rebuild / resize / borrow / 顺序 / 缓存 |
| `FrameCompiler`（内含 `FrameGraph`） | normalize → resolve target/depth → 建边 → schedule → `CompiledFrame` | 不碰 GPU，不持有资源，不知道 vsg 类型 |
| `ResourceManager`（含 `RetirementQueue`、`Validator`） | 资源的创建/查找/替换/退役；世代号 | 不决定"该不该重建"（那是 Compiler 的输出），不录制 |
| `VsgExecutor` | 把 `CompiledFrame` 机械地变成 vsg 图/提交/呈现 | 不判断 rebuild / resize / reorder / borrow / 选管线；只保留**平台强制**的步骤（取图/重建交换链/变体回切） |
| `Observe`（横切） | 诊断、计数器、retentionStats、trace | 不参与任何决策（这就是它不能算一层的原因） |

`SceneMirror` 退化成 `ResourceManager` 的输入适配（scene 对象 → 资源身份），**不再拥有 GPU 资源**：
所有权只有一处。

### 2.3 物理边界：`core/` 不许 include vsg

"只有 Executor 碰 vsg"如果只写在文档里，最后一定退化成口号。因此目录本身就是边界：

```
backend/
  core/            ← 不许出现任何 vsg::*（含 ref_ptr / RenderGraph / GraphicsPipeline / DescriptorSet）
    Protocol.hpp  FrameDescription.hpp  FrameCompiler.hpp  FrameGraph.hpp
    ResourceManager.hpp  RetirementQueue.hpp  FrameTimeline.hpp
    Keys.hpp      Handles.hpp           Observe.hpp
  api/             ← 可以 include core；这里是唯一的 API 世界（落地时叫 api/，不是 vsg/：
    Session.hpp      插件目录已经叫 vine/vsg/，再套一层 vsg/ 只会让人读成命名空间）
    DeviceProbe.hpp  VsgExecutor.hpp  VsgResource.hpp  VsgDynamicState.hpp
```

规则：`core → vsg` 禁止，`vsg → core` 允许；`core` 里只用自己定义的 opaque handle
（`PipelineHandle{index, generation}` / `ImageHandle` / `TargetHandle` / `BufferHandle`）。
落地手段（**已实现 2026-09-21**）：`scripts/check_include_hygiene.py` 新增 `core_layer_findings()`——
`core/` 目录下禁止 `vsg/*`（渲染 API 自己的头）与除 `vine/vsg/core/*`、`vine/vsg/vsg_global.hpp`
（命名空间/导出宏，编码规范要求它第一个包含）之外的 `vine/vsg/*`（插件自己的上层）。

这一步的回报是可验证的：换 API（裸 Vulkan / D3D12）时，`core/` 理论上可原样复用。

### 2.4 一帧的生命周期

```
beginFrame          → FrameTimeline 铸令牌；取交换链图像；FrameArena 重置（bump，不释放）
  每个启用的 pass（升序）
      beginPass     → Recorder 开 scope；记下"本帧活跃 pass"
      setPassOrder  → 写进 FrameDescription（堆叠位，不是身份）
      setPassInputs → **复制**解析结果进 FrameArena（绝不持有引擎的 span）
      pass->execute  → setRenderTarget / setViewport / announceClear / setDepthMode /
                       setLights / render（或 drawScreenProgram）—— 只写 FrameDescription
      endPass       → 关 scope；未消费的属性丢弃
endFrame            → FrameCompiler：normalize → 目标/深度求解 → FrameGraph → Schedule
                    → ResourceManager：份额快照 → 退役结算 → 废弃清扫
                    → VsgExecutor：按 Schedule 录制（记录顺序 ≠ 到达顺序）
swapBuffers         → 消费令牌；submit + present 各一次；FrameTimeline.commit()（退役推进只在这里之后）
```

**与现有实现的关键差别**：`render()` / `drawScreenProgram()` 的**录制时机推迟到 `endFrame()`**，
且计划里的每一段数据都活在 `FrameArena` 里而不是宿主的容器里（P0-1）。

### 2.5 P0 定义表（实现契约）

评审要求"先钉死定义再开工"。以下 10 条就是开工契约，每条都给出**结论**：

| # | P0 问题 | 结论 |
| --- | --- | --- |
| 1 | `FramePlan` 的 backing storage 归谁 | `FrameArena`（帧内 bump 分配器）**独占**：所有 span 指向 arena，**禁止**指向宿主 API 参数背后的容器；`render()` 收到 `RenderCommand` 就复制所需字段 |
| 2 | Intent → Plan 的 normalize 语义 | 见 D2 的两列表：`FrameDescription` 允许保序/重复/后设覆盖/非法调用/缺省；`CompiledFrame` 必须是"每 pass 一个终态、每 draw 一个终态、借用已快照、默认已解析、合法性已判定" |
| 3 | `Protocol` 的职责上限 | **只答"这次调用是否合法"**，不答"应该怎么办"：rebuild/resize/borrow/顺序/缓存一律不属于它 |
| 4 | 依赖图与退役是否同一个东西 | 不是：`FrameGraph`（帧内、纯数据、求值后即弃）与 `RetirementQueue`（跨帧、按帧时间轴）分开；`ResourceManager` 只是它们的宿主 |
| 5 | 目标形状 / 兼容性 / load-op 变体的关系 | 拆成 `PipelineCompatibility`（格式/采样/子 pass/深度格式）与 `LoadOpVariant`（load/store + initialLayout）；**管线键只含前者**（见 D3） |
| 6 | Schedule 的节点与边 | 节点 = **pass scope**（不是 draw）；边 = 生产者 pass → 消费者 pass（采样边 + 深度借用边）；draw 顺序只属于 pass 内部 |
| 7 | 环的失败语义 | 环 = **声明本身不合法且引擎不查**（已核）⇒ 上报 Error + **跳过整个强连通分量**（其余 pass 照旧、帧照旧提交呈现）+ `invalid_schedules` 计数；自检断言该计数为 0。禁止"按当前顺序偷偷执行" |
| 8 | `revision` 的来源与稳定性 | 只能来自上游资源（`Geometry/Buffer/Texture/ShaderProgram::revision()`）；**后端绝不自行递增**；键是纯身份 |
| 9 | `InstanceSlot` 是 CPU 逻辑还是 GPU 存储 | **CPU 逻辑记录**（矩阵/透明度/材质引用+revision）；GPU 存储属于 arena。两者靠"pack"连接 |
| 10 | Evidence 是不是一层 | 不是：横切 instrument（图见 §2.1）；相位表、计数器、像素断言围绕它组织（D8） |

## 3. 八个设计决定

### D1 · 合法性收在一个类型里，但只判合法性

**做法**：`FrameRecorder` 内部持有 `Protocol`——一个显式的 `{ Idle, InFrame, InPass }` 状态 + 声明字段
（当前 pass、当前 target、该 target 是否已被 `releaseRenderTarget` 宣告可销毁）。所有入口第一步过它，
违规返回 `ProtocolVerdict{ Allow | Drop | Refuse }`；`ReportOnce` 的重臂边界写在状态机里
（"无 scope 绘制"按帧重臂、"已释放 target"按 scope 重臂）。

**硬边界**（评审 P0-3）：Protocol 只答"这次调用是否合法"，**绝不答"应该怎么办"**——
不判 rebuild、不判 resize、不判 borrow、不排顺序、不碰缓存。它一旦开始回答后者，就变成
评审担心的那个大状态机（`Protocol → {scope, target, pipeline, depth, cache, lifetime}`）。

**为什么**：契约里 8 条协议规则都有"报告 / 拒绝 / 丢弃"三选一；分散实现最容易漏的是
"该拒绝的却被静默重定向"（把已释放 target 的绘制降级到窗口）。集中后规则可穷举，
且"新加一个入口"必然经过它。

**判据**：无设备的状态转移表测试：每个 (状态 × 入口) 断言 `{允许 / 丢弃 / 拒绝并上报}`；
`diagnosticCount(PassProtocolViolation)` 逐格钉住。

### D2 · 三段式（收集 → 编译 → 执行），且 normalize 有明确语义

**做法**：`FrameRecorder` 收 `FrameDescription`（自包含快照）→ `FrameCompiler` 产出 `CompiledFrame`
→ `VsgExecutor` 按 Schedule 机械执行。三段之间只有数据，没有回调。

normalize 的两侧契约（评审 P0-2）**必须分别成立**：

| `FrameDescription`（收集侧）**允许** | `CompiledFrame`（编译侧）**必须** |
| --- | --- |
| 保留调用顺序（顺序本身有意义：`setPassOrder` 是堆叠位） | 每 pass 一个终态：后设覆盖先设 |
| 同一属性被设多次 | 每 draw 一个终态（viewport / lights 已绑定到具体绘制） |
| 非法调用（无 scope 绘制 / 嵌套 scope / 已释放 target） | 合法性已判定（非法的在收集侧就被拒绝或丢弃，不进编译） |
| 缺省值**未解析**（没设 viewport = 全目标，没设 clear = 不清） | 缺省值已解析成显式值（全目标矩形、无 clear） |
| 借用的指针（相机 / 命令 / 灯 / target） | 借用已快照（数据在 `FrameArena`，见 §2.5 P0-1） |

**为什么**：如果 `FrameDescription` 直接等于 `CompiledFrame`（v1 的隐含假设），Emitter 里会重新
冒出 `if (!target) / if (!depth) / if (!lights)`，v1 的 L5 就变回第二个 L1——评审这条是对的，
所以两侧契约写在同一张表上。

**判据**：`GraphOrderTest` 式的纯函数测试（环的另一半见 D2.1）；`AllocationGate`（core，进程内读堆用量）：
稳态帧增长 = 0。

#### D2.1 环的失败语义（评审 P0-7，重新定义）

**事实核对**：`graphics/src` 里**没有**环检测（`Group.cpp` 的 cycle 检查是场景图父子关系，不是渲染依赖），
`RenderEngine::validateWiring()` 只管"谁填谁 / 谁声明谁的输出 / 声明冲突"。⇒ **后端是唯一能看见环的地方**。
（现有后端在 `VsgRecordOrder.cpp:161` 记下了"实践上无环；有环就保持当前顺序"——那正是评审担心的
"验证层干净、画面却少了一个 pass"。）

**结论**：环 = 调用方声明本身不合法，按**不变式违规**处理：

1. 上报 `Error`（"依赖环：A ↔ B，无法确定执行顺序"），每 episode 一次；
2. **跳过整个强连通分量**：分量内每个 pass 本帧**不绘制**（不是"换个顺序偷偷画"，而是不画）；
3. 分量外的 pass 照旧执行，帧照旧提交与呈现（契约要求一帧必须以 `swapBuffers()` 收尾）；
4. `invalid_schedules` 计数 +1，自检断言其恒为 0。

**为什么不是"丢弃整帧"**：环是调用方 bug，通常只涉及两个辅助 pass；把主画面一起丢掉是
把局部错误放大成黑屏。**为什么不是"按当前顺序继续"**：那会让消费方采样到尚未写入的附件，
画面错得无从归因。⇒ "跳过 + 计数 + 响亮"，像素上不假装。

### D3 · 三层状态模型 + 键审计 + 兼容性/变体拆分

**做法**：每个绘制项的状态分三层，并规定**上层的东西绝不进下层的键**：

| 层 | 内容 | 允许的变化代价 |
| --- | --- | --- |
| Identity → 管线键 | 程序 + `revision()`、顶点布局、**兼容性键**（下方拆开）、源深度是否可采样、shadow 是否绑定、采样附件数 | 重编管线（唯一允许） |
| Dynamic state | `DepthMode`、cull、front face、polygon、topology、blend enable/equation（9 项 + 逐附件） | 一条 `vkCmdSet*` |
| Data | 视图块、光照块、`VineDrawBlock`（矩阵 + opacity 在 `params.x`）、材质 64B | 就地字节写 / 描述符 re-point |

**目标属性的两分（评审 P0-5）**——v1 含糊的地方在这里钉死：

```
TargetShape
 ├── PipelineCompatibility      ← 进管线键（Vulkan 的 render-pass 兼容性就是这些）
 │    ├── color formats（每个附件）
 │    ├── depth format（或“无”）
 │    ├── sample count
 │    └── subpass structure
 └── LoadOpVariant              ← 不进管线键（兼容性明确不含 load-op / initialLayout）
      ├── color load / store op
      └── depth load / store op
```

三条推论，直接回答"为什么 resize / CLEAR↔LOAD 不重编"：
* **extent 谁都不进**：像素尺寸是运行时资源（图像 / 视图 / 帧缓冲 / 动态视口），不是身份；
* **CLEAR ↔ LOAD 只是换变体**：同一管线在多个兼容变体上都能用（这也是 bootstrap 变体合法的原因）；
* **形状变化才 rebuild**：附件数 / 格式 / 采样 / 子 pass / 深度格式变了，兼容性才真的变了。

因此 v1 那句"尺寸不进 ShadingKey"要改成更精确的措辞：**像素尺寸不得进入 shader/pipeline identity；
只有影响 attachment 兼容性的属性才能进入兼容性键**（评审 P0-5）。

**为什么**：现有缺陷族（pass-lifecycle §1、pipeline-sharing §2）都是**键污染**：把可动态化的东西
放进管线键（StateNode 一改就重编），或把要换的视图留在节点里（resize 要重建整批程序槽）。

**判据**：**键审计测试**——每个 key 声明"允许的输入集合"，测试断言集合未扩大；再加行为断言：
`StateNode` 改 cull/polygon/blend ⇒ `pipelineVariantCount()` 不变；材质改参数 ⇒ 节点重建 0 次、
只发生一次 64B 写；目标改尺寸 ⇒ `offscreen_builds` / `program_slot_builds` 不变。

### D4 · 资源寿命：owning 的 ResourceManager + 退役队列 + 只读校验器

**做法**（评审 P0-4，把 v1 的 `Ledger` 拆成三件事）：

```
ResourceManager            ← owning：几何 / 管线 / 描述符 / 目标，按 key 查找与创建
 ├── GeometryCache / PipelineCache / DescriptorCache / TargetCache
 ├── Generations           ← 每个会话一个 generation；所有 key/handle 携带它
 └── SceneMirror           ← 只是“scene 对象 → 资源身份”的适配（不拥有 GPU 资源）
RetirementQueue            ← 跨帧寿命：active → replaced → retired → destroyed，按帧时间轴
Validator                  ← 只读：检查 ResourceManager + RetirementQueue 的不变量（Debug 每帧）
```

销毁只有两条合法路径：① **整会话替换**（前面一次计数过的 device idle）；② **退役队列**出队。
Debug 校验器每帧断言："没有 handle 指向已退役资源"、"稳态帧退役队列不增长"、"帧路径 device wait = 0"。

**为什么拆**：依赖关系（descriptor → view、graph → pass）是**帧内**的、求值完就丢；资源寿命是**跨帧**的。
v1 让一个 `Ledger` 同时当依赖图 + 垃圾回收 + 世代管理 + 校验器，正是评审说的"God Object"风险。

**判据**：`DeferredReleaseTest` / `GeometrySafetyTest` 式用例；Debug 校验器在全部自检相位下零报告；
`retentionStats().parked_nodes` 帧末回落。

### D5 · FrameTimeline：submitted 与 completed 必须分开

**做法**（评审 P0-5 修正了 v1 的"帧时钟"语义）：`FrameTimeline` 暴露两术数而不是一个：

```cpp
FrameToken begin();                 // 铸令牌
void       submitted(FrameToken);   // swapBuffers 里，提交完成后
uInt64_t   submittedFrame() const;  // 已提交帧数
uInt64_t   completedFrame() const;  // 有完成证据的帧数（见下）
```

**完成证据从哪来（这条是整个机制的关键）**：本后端**没有**独立的时间线信号量查询，安全性的来源是
**命令缓冲槽位的 fence 复用**。已在仓库实际编译的那份 vsg（`build/_deps/vsg-src`，1.1.16）里逐行复核：

* 槽数 = 帧在飞数：`Viewer::assignRecordAndSubmitTaskAndPresentation()` 里的 `uint32_t numBuffers = 3;`
  （`vsg 1.1.16 — src/vsg/app/Viewer.cpp — :501`）→ `RecordAndSubmitTask::RecordAndSubmitTask(Device*, uint32_t numBuffers = 3)`
  （`include/vsg/app/RecordAndSubmitTask.h`）→ 构造体里的 `_fences.resize(numBuffers)`；
* **槽只有在它自己的 fence 被等过之后才会被重新录制**：`RecordAndSubmitTask::start()` 里
  `if (current_fence->hasDependencies()) { current_fence->wait(timeout); current_fence->resetFenceAndDependencies(); }`
  ——即"一槽一个 fence，重用前先等它"，**这就是完成证据**（详见 §9.1）；
* 槽索引每帧轮转：`RecordAndSubmitTask::advance()`，每帧由 `Viewer::advanceToNextFrame()` 调。

⇒

* `kRetireDepth = slots + 1` **是被推导出来的**，不是魔数：槽数来自 `numBuffers`（本 vsg 是 3）
  ⇒ 深度 4（与现有 `VsgRetireRing::kRetireRingDepth` 一致）；
* 退役点写成 `retire_at = submittedFrame() + kRetireDepth`，释放条件是 `completedFrame() >= retire_at`，
  其中 `completedFrame()` 由**槽位回收**推进；
* 槽数**不在公开 API 上**（`Viewer.cpp:501` 是局部硬编码），而 `fence(i)` 越界返回 nullptr
  （`RecordAndSubmitTask.cpp:79-83`）⇒ **启动期探测或断言一次**（探测值与推导值不一致就上报并按探测值走）；
* **推进点必须是帧的最后一步**（现有实现踩过：park 落在该调用之后会早一帧释放，
  于是销毁了已提交命令缓冲仍在引用的对象）；
* 若执行器将来改用"不等待槽位 fence"的提交方式，`completedFrame()` 的证据就消失 ⇒
  **必须退回计数过的 device wait**，以 `retireWaitCount()` 为判据（不得静默假设）。

同一时间轴同时服务：资源退役、pass 退役（本帧未被 `beginPass` 声明的 slot）、bootstrap 状态、
GPU profile 的滞后判定、诊断里的 `frame=N`。

**为什么**：三条独立的"延迟"各自维护节拍必然错拍。评审的 `submitted`/`completed` 区分是对的：
"提交"不等于"完成"，把两者混为一个计数就是最后一类静默错误的来源。

**判据**：`FrameCommitTest` 式用例；帧路径 `device_waits == 0`；`runPolicyChurnStressPhase`
下像素与计数器同时稳定。

### D6 · 目标三件套 + `planTarget` / `depthPlan` 纯函数

**做法**（沿用 v1 的纯函数，按评审补上命名与 generation）：

```cpp
struct TargetDesc {                   // 逻辑描述（= SDK 的 RenderTarget 投影）
    Extent2D extent; std::span<const Format> color_formats; std::optional<Format> depth_format;
};
struct TargetInstance {               // 当前这一代 GPU 对象
    TargetDesc desc; ImageHandle color[]; ImageHandle depth; uInt64_t generation;
};

enum class TargetAction { None, Repair, ResizeInPlace, Rebuild };
TargetAction planTarget(const TargetInstance&, const TargetWanted&);
//  Rebuild       ⟸ 兼容性键变了（附件数 / 颜色格式 / 深度格式 / 有无深度 / 子 pass）
//  ResizeInPlace ⟸ extent 变：换图像/视图/帧缓冲 + 描述符 re-point + barrier + 借用源视图重指
//  Repair        ⟸ 尺寸未知，或 bootstrap（首次进新 target 必须清一次）
//  None          ⟸ 其余

struct DepthPlan { bool sampleable; bool borrowed; const RenderTarget* source; bool preserve; };
DepthPlan depthPlan(const RenderTarget&, const PassClearSet&);
//  sampleable ⟺ 提升 ∧ 本 target 无“保留深度”的 pass（提升被任一 LOAD pass 撤销）
//  borrowed   ⟹ 策略来自出借方；出借方被释放 ⇒ 恰好重建一次（记忆“死深度源”）
```

**为什么**：三件事（提升 / 借用 / LOAD 保留）都改 `initialLayout`，都与 load-op 变体交互；
分开实现必然出现"某一帧多清一次"或"采样到 attachment layout"。合成一个函数的输出后可表驱动穷举。

**判据**：`TargetBookkeepingTest` 式的表 + `runTargetResizePhase`（`offscreen_resizes` +2、
`offscreen_builds` 不变、`program_slot_builds` 不变、`device_waits` 不变、`parked_nodes` 先升后落）
+ 像素读回确认采样到的是**新**图像。

### D7 · 两类存储：持久 arena（材质）与每帧 ring（视图 / draw）；三类作用域永不混

**做法**（评审 P0-9 修正了 v1 把材质也塞进"每帧 ring"的含糊）：

| 存储 | 内容 | 写入时机 |
| --- | --- | --- |
| **持久 arena** | 材质块（每材质一个 slot，按 in-flight 副本数轮转） | **只在 `revision()` 变化时**写；稳态零流量 |
| **每帧 ring** | 视图块（每 pass）、`VineDrawBlock`（每 draw：矩阵 + opacity） | 每帧写（这些本来就每帧变） |
| 描述符 | 绑定以上两类（dynamic offset） | 视图被替换时**只换 set 元素**（复用 layout 与 sampler），旧 set 进退役队列 |

这样"材质改参数不产生每帧流量"这条稳态性质被保住，同时仍然只有一套描述符布局 + dynamic offset，
材质管理器不再需要"比别的 bridge 活得久"。

其余三条照旧：
* 作用域三分且**永不混**：**会话级**（纹理、网格流）、**进程级**（GLSL 编译阶段、引擎默认程序）、
  **pass 级**（状态注册表、变体池）；
* 废弃判定 = "entry 持有 key 对象 + 按份额计数"（`useCount() <= shares`）；
* 份额在帧开始时快照、帧结束后清扫（现有 P11 缺陷——互相等待——的直接教训）。

**判据**：材质/纹理/网格缓存用例（含 65 个程序的 FIFO 淘汰与"淘汰真的归还共享对象"）；
"材质稳态零字节写"用计数器断言；"两个 pass 画同一几何只上传一次、但各有自己的状态注册表"。

### D8 · 证据是横切的，而且是三重验证

**做法**：

```cpp
struct Phase { const char* name; SetupFn setup; AssertFn assert; CounterDelta expect; };
```

* 相位表：一行 = 装配 → 断言（**像素读回** + 计数器）→ 期望的计数器增量（含"必须不变"的项）；
* **三重验证**（评审 §14，已提升到 §0）：验证层干净 ∧ 计数器符合预期 ∧ 像素符合预期，
  "No-VUID is necessary but not sufficient"；
* 分配门禁：稳态帧堆增长 = 0（`core/AllocationGate`；`scripts/alloc_trace.c` 仍是“内存去哪了”的排查
  工具，它的文件头自己写着“not a gate”，两者职责不混）；
* 诊断带 `frame=N / pass / target / generation / schedule_index`（评审 §18 的 Plan ID），
  "旧世代 / 新世代 / 退役节点"一类问题可以完整追踪；
* 证据脚本逐字节比对 `[selftest]` 行；后端 trace 行不进基线（spdlog 带 file:line、行数随帧数变）。

**为什么**：能力清单与测试清单同源，"新加能力但没相位"会立刻暴露；`CounterDelta` 让
"悄悄多了一次重建"变成显式失败。三重验证是针"验证层干净但什么都没画"这类问题的唯一有效门禁。

**判据**：`scripts/vsg_selftest_evidence.sh` 基线逐字节一致；`scripts/gfx_lavapipe_check.sh` 全绿。

## 4. 能力对照表（同功能证明）

| # | 能力（现有实现已钉住的行为） | 交付者 | 判据 |
| --- | --- | --- | --- |
| 1 | 设备/会话上线、失败自清、幂等、**移动而非重建**、空帧也提交、令牌语义 | `ResourceManager` + 世代号 + `FrameTimeline` + `planTarget`（格式兼容校验） | `DeviceRequirementsTest`、`runHostSurfaceMovePhase`（`window_builds` 不变、移动前后像素相同、两个宿主窗口都还活着） |
| 2 | 表面尺寸权威、就地改尺寸、隐藏/0 尺寸、"无可用尺寸"每 episode 一次 | D6 `planTarget` | `runTargetResizePhase`、`ReadbackRefusal::Empty` |
| 3 | 离屏 target 懒建、MRT、attachment 0 才清、额外附件透明黑、深度清到 reverse-Z 远平面、LOAD/STORE 变体、提升/借用/退役 | D6 `TargetInstance` / `planTarget` / `depthPlan` + D4 退役队列 | `PassClearValuesTest`、`PassRenderPassPlanTest`、`runColorBootstrapPhase`、`runSharedDepthPhase` |
| 4 | pass scope、order 只是堆叠位、release 语义、协议违规、预热（成本归 "pre-frame"）、停用/移除 pass | D1 + D2 | `PassProtocolTest`、`FrameCommitTest`、`runPassProtocolPhase`（`detached_slots` 升后落、`offscreen_builds` 不变） |
| 5 | 内容绘制、增量刷新、稳态（不重传/不重编/不增长）、缓存有界、废弃判定、共享规则 | D3 三层键 + D7 缓存与份额计数 | `SceneBridgeDataRebuildTest`、`runDataRefreshPhase`（build 1 / refresh 1 / 节点数平）、churn 相位 |
| 6 | 全屏/屏幕 pass、PiP 子矩形、deferred 合成、源附件 i→binding i、深度真可采样才绑 | D3 键（draw kind = fullscreen，不另建系统）+ Executor | `runCompositingPixelPhase`、`runDepthSamplingProgramPhase`、`runTargetResizePhase` 的 `program_slot_builds` 不变 |
| 7 | 光照 world→view、1 ambient + 3 directional、默认头光/环境光、丢弃项每 episode 一次 | 编译侧打包（前向 UBO + 全屏 128B push） | `LightsTest`、`LightDropReportTest`、`ForwardShaderSetTest` |
| 8 | HUD / window layer：就是更高 order 的普通 pass + 子视口 + 不清屏（LOAD） | 无需专门代码 | `ContentSlotViewportTest`、HUD 相位、`.ai/bugs/vsg-maximize-black-band.md` 的复现 |
| 9 | readback：RGBA8 紧凑打包、D32 原样 / D16÷65535 / D24S8 诚实拒绝、借用方读到源 | `core/` 里的 `ReadbackRefusal → ReadbackResult` 单表 | `ReadbackTest`、`runPixelReadbackPhase`、`runSharedDepthPixelPhase` |
| 10 | 缓存与所有权：entry 持有 key、份额计数、会话/进程/pass 三类作用域 | D7 | `SceneBridgeCacheOwnershipTest`、`GeometrySafetyTest`（未画 1000 次仍保留） |
| 11 | 诊断：单一出口、9 类、`ReportOnce`、告警不静默降级 | `Observe`（横切）+ D2.1 的环语义 | `DiagnosticsTest`、`ReportOnceTest`、`LightDropReportTest`、`scripts/check_diagnostic_formats.py` |
| 12 | 平台：reverse-Z 与算子反转、正面声明、动态状态 9+ 项（3 个走函数指针）、无 VUID、WSI 细节 | `vsg/` 目录（唯一设备层） | `RenderStateMapperTest`、`DynamicStateTest`、`runHostSurfaceMovePhase`、`check_vsg_upstream_capabilities.py` |
| 13 | 自检与证据：相位表、像素断言、逐字节基线、性能档（构建档案 / GPU profile） | D8 | `scripts/vsg_selftest_evidence.sh`、`runGpuProfilePhase`、`GpuProfileTest` |

## 5. 类型草图

```cpp
// —— 收集侧：自包含快照（所有 span 指向 FrameArena）——
struct FrameDescription {
    FrameToken token;
    std::span<const CollectedPass> passes;   // 保序；终值可能尚未规范化
};
struct CollectedPass {
    PassId pass; int order; TargetHandle target;
    bool rect_announced; Viewport rect;              // 未设 = 全目标（未解析）
    std::optional<ClearPolicy> clear; DepthMode depth;
    std::span<const InputRef> inputs;                // 从引擎的复用 vector 复制而来
    std::span<const LightRef> lights; std::span<const CollectedDraw> draws;
};

// —— 编译侧：可执行计划（缺省已解析、借用已快照、合法性已判）——
struct CompiledFrame {
    FrameToken token;
    std::span<const CompiledPass> passes;            // 已按 Schedule 排序
    std::span<const TargetPlan> targets;
    std::span<const ResourceRequest> resources;
};
struct CompiledPass {
    PassId pass; TargetHandle target;
    RenderPassCompatibility compatibility;           // 管线兼容性身份
    LoadOpVariantKey variant;                        // 可换的 load-op / initialLayout
    Viewport rect; DepthState depth; std::uint32_t schedule_index;
    std::span<const CompiledDraw> draws;
};
struct CompiledDraw {
    DrawKind kind;                                   // Geometry | Fullscreen（不另建系统）
    PipelineHandle pipeline; DescriptorHandle descriptors;
    DynamicState dynamic; InstanceSlot instance;     // 三层：身份 / 动态 / 数据
    std::span<const GeometryBatch> batches;
};

// —— 键（D3）：只放“改了就必须重编/重建”的东西 ——
struct DataKey { const vine::Buffer<float>* buffer; std::uint64_t revision;   // revision 只来自上游
                 std::uint32_t components, offset, count; };
struct VertexLayoutKey { std::uint32_t canonical_mask; std::span<const std::uint32_t> custom_locations; };
struct RenderPassCompatibility { std::span<const Format> color_formats;
                                 std::optional<Format> depth_format; std::uint32_t samples, subpass; };
struct LoadOpVariantKey { LoadOp color_load, depth_load; ImageLayout initial, final; };
struct PipelineKey { const ShaderProgram* program; std::uint64_t revision;
                     VertexLayoutKey vertex_layout; RenderPassCompatibility compatibility;
                     bool depth_sampleable, shadow_bound; std::uint32_t sampled_color_count; };

// —— 三层里的另两层 ——
struct DynamicState { DepthMode depth; CullMode cull; FrontFace front_face;
                      PolygonMode polygon; BlendState blend; Topology topology; };  // vkCmdSet*
struct InstanceSlot { Mat4d matrix; float opacity; MaterialRef material; };          // CPU 逻辑记录

// —— 目标与寿命 ——
struct TargetInstance { TargetDesc desc; ImageHandle color[]; ImageHandle depth; std::uint64_t generation; };
enum class TargetAction { None, Repair, ResizeInPlace, Rebuild };
struct DepthPlan { bool sampleable, borrowed, preserve; const RenderTarget* source; };

class FrameTimeline { public: FrameToken begin(); void submitted(FrameToken);
                             std::uint64_t submittedFrame() const; std::uint64_t completedFrame() const; };
class RetirementQueue { public: void retire(ResourceHandle, FrameToken);
                               void advance(FrameTimeline&);   // 帧的最后一步
                               std::size_t waitCount() const; };
```

四条**结构性规则**（比类型更重要）：

1. **arena 独占**：`FrameDescription` / `CompiledFrame` 里的每个 `span` 都指向本帧 `FrameArena`；
   不得指向宿主 API 参数背后的容器（P0-1）。借用的 `RenderCommand` / light / target 一律复制所需字段；
2. **键里不放会被就地改写的值**：矩阵、透明度、材质字节属于 `InstanceSlot`；
3. **extent 不进身份，兼容性属性才进**（P0-5）：像素尺寸 → 运行时资源；格式/采样/子 pass/深度格式 →
   `RenderPassCompatibility`；load-op / initialLayout → `LoadOpVariantKey`（不进管线键）；
4. **缓存 entry 必须持有它的 key 对象**（`OwnedCache`），否则地址回收后无从察觉。

## 6. 与现有实现的差异（含代价）

### 6.1 内部组织差异

| # | 现有做法 | 我的做法 | 代价 / 收益 |
| --- | --- | --- | --- |
| 1 | 材质管理器跨会话、必须比 bridge 活得久 | 会话内 ring arena + dynamic offset | 一次描述符布局改动；换来生命周期耦合消失、每帧只有字节写 |
| 2 | 协议规则分散在若干 `refuse*` / `reportPassMisuse` 站点 | 一台 `Protocol` 状态机 + 转移表 | 多一层间接；换来规则可穷举、拒绝不能被绕过 |
| 3 | 键隐含在重建判定里（散在各 `needsRebuild`） | 键是显式结构体 + 审计测试 | 加字段要过测试（刻意的摩擦） |
| 4 | 记录顺序在遍历中顺带决定 | `Schedule` 纯函数 + 推迟到 `endFrame()` 录制 | plan 必须在 arena 里复用；换来顺序可测、录制层只做机械落地 |
| 5 | 逐成员维护会话态与清理顺序 | 整会话替换 + 世代号 + 账本校验器 | Debug 多一次 O(slot) 校验；换来"换屏后旧键失效"是自动的 |
| 6 | 稳态靠用例断言 | 稳态还有**分配门禁**（`core/AllocationGate`，进程内读 glibc 堆用量） | 需要纪律：不能随手 `std::vector` |

**不变的部分**（§1.3）：render pass + load-op 变体、每 pass 一份状态注册表、128B push block、
深度提升/借用/LOAD 三件套、顶点与四个块的 ABI、reverse-Z 与正面声明、诊断类别词表。

### 6.2 外部评审逐条处理（20 条）

结论口径：**采纳**（改设计） / **部分**（方向对但有边界要补） / **不采纳**（附理由）。

| # | 评审点 | 结论 | 处理 |
| --- | --- | --- | --- |
| 1 | `FramePlan` 生命周期未定义，span 可能悬空 | **采纳** | 新增 §2.5 P0-1：`FrameArena` 独占全部 backing storage，禁止指向宿主容器 |
| 2 | `FrameIntent → FramePlan` 缺 normalize 定义 | **采纳** | D2 的两列表（允许 vs 必须） |
| 3 | Protocol 不要变成万能状态机 | **采纳** | D1 硬边界：只答合法性；并写进 §2.2 的"不做"列 |
| 4 | Ledger 职责过重，应拆 | **采纳** | D4：`ResourceManager` / `RetirementQueue` / `Validator` 三分 |
| 5 | 兼容性 / load-op 变体 / 形状要拆清 | **采纳** | D3 的 `TargetShape` 两分 + §2.5 P0-5 |
| 6 | "尺寸不进 key"应更精确 | **采纳** | 改为"extent 不进身份，兼容性属性才进"（D3、§5 规则 3） |
| 7 | `revision` 的来源要钉死 | **采纳** | §2.5 P0-8：只能来自上游；后端不得自行递增 |
| 8 | `InstanceSlot` 是 CPU 逻辑还是 GPU 存储 | **采纳（取 CPU 逻辑）** | §2.5 P0-9 + D7 的两类存储；并补上"材质稳态零流量"这条被 v1 弄含糊的性质 |
| 9 | `park` 的 N+1 不能是魔数 | **部分** | 采纳"写成退役点"；但完成证据不是时间线信号量，而是**槽位 fence 复用**（D5 给出了 `kRetireDepth = slots + 1` 的推导）；拿不到证据时必须退回计数过的 device wait |
| 10 | Schedule 的节点应是 pass 而非 draw | **采纳** | §2.5 P0-6；边 = pass → pass，draw 顺序只在 pass 内部 |
| 11 | 环不能"上报 + 跳过"了事 | **采纳（比评审更严）** | D2.1：跳过整个强连通分量 + `invalid_schedules` 计数 + 自检断言为 0；帧仍提交 |
| 12 | 三重验证应提升为核心原则 | **采纳** | 写进 §0 与 D8（"No-VUID is necessary but not sufficient"） |
| 13 | `FrameClock` 命名不如 `FrameTimeline` | **采纳** | D5 改名；并把 `submittedFrame()` / `completedFrame()` 分开 |
| 14 | Evidence 是横切，不是层 | **采纳** | §2.1 的两个流图；`Observe` 不算一层（§2.2） |
| 15 | M7 不应"最后才做证据" | **采纳** | §7 新增 **M0 证据地基**；M7 改为"证据加固" |
| 16 | L1–L4 不得出现 `vsg::*` | **采纳（升为物理边界）** | §2.3：`core/` / `vsg/` 目录 + include 规则 + 扩 `check_include_hygiene.py` |
| 17 | 应有 Plan ID / frame id 便于追踪 | **采纳** | D8：诊断带 `frame / pass / target / generation / schedule_index` |
| 18 | 把"数据流"与"所有权流"分开画 | **采纳** | §2.1 |
| 19 | 核心对象数不宜多，建议 6 个 | **部分** | 采纳"六个 + 一个横切"（§2.2）；但 `VsgBackend` 必须是 SDK 的 **31 个方法逐字实现**，评审草案里的签名（`initialize(createInfo)` / `beginPass(RenderPass&)` / `render(const RenderCommand&)` 等）与真实契约不符，不能照抄 |
| 20 | Executor "越笨越好" | **部分** | 采纳"不做决策"；但平台强制的步骤（取图/重建交换链/变体回切）留在它里面——否则会把"平台事实"藏进上游层 |

## 7. 里程碑与验收

| 里程碑 | 内容 | 验收 |
| --- | --- | --- |
| **M0 证据地基**（已完成 2026-09-21） | 相位表驱动、计数器骨架、像素探针夹具、分配门禁、诊断出口 | 空相位能跑通并逐字节产生基线；后续每个 M 落地时**同时**加相位（不留给 M7） |
| M1 会话 | 设备地板检查、格式、交换链、submit/present、空帧也提交、令牌 | `DeviceRequirementsTest`+`FrameCommitTest` 式用例；`runHostSurfaceMovePhase` 移动不重建 |
| M2 内容 | 三层键、几何别名、通道切片、材质 arena、稳态 | `runDataRefreshPhase` 的 (build 1 / refresh 1 / 节点数平)；`pipelineVariantCount` 在 churn 下不涨 |
| M3 目标 | `planTarget` / `depthPlan`、MRT、清屏规则、bootstrap、borrow | `PassClearValuesTest`、`runTargetResizePhase`、`runSharedDepthPhase` |
| M4 全屏 | 全屏 slot、PiP 子矩形、deferred 合成、深度采样绑定 | `runCompositingPixelPhase`、`runTargetResizePhase` 的 `program_slot_builds` 不变 |
| M5 阴影/深度 | LOAD 变体、提升撤销、shadow 输入解析、light slot 索引 | `runDeferredShadowPixelPhase`、`runForwardShadowPixelPhase`、`runShadowedLitFacePhase` |
| M6 readback | 分类拒绝先于等待、格式诚实、借用读源 | `ReadbackTest`、`runPixelReadbackPhase` |
| M7 证据加固 | 基线冻结、性能档、无 VUID 门禁、诊断字段完整性 | `vsg_selftest_evidence.sh` 逐字节一致；`gfx_lavapipe_check.sh` 全绿 |

**全局不变量（每个里程碑都要成立）**

| 不变量 | 含义 |
| --- | --- |
| 帧路径 `device_waits == 0` | 只有 readback 与会话替换会 idle 设备 |
| 改尺寸不重建 | `offscreen_builds` / `program_slot_builds` 在 resize 相位不变 |
| 稳态帧零分配 | arena 复用；分配门禁 |
| 稳态帧不涨 | `pipelineVariantCount`、`parked_nodes`、`retained` 均回落 |
| 与计数聚合一致 | `counters()` 与 `retentionStats()` 重叠项必须相等（同一份数据两个视图） |
| 声称与画面一致 | 每个相位既有计数器断言也有像素断言：验证层干净但没画出东西 = 失败 |

## 8. 风险与不做的事

**最难的三处**

1. **记录顺序 × in-flight 复用**：退役深度与 render pass 兼容性的组合决定"能不能原地换"，
   而完成证据来自槽位 fence 复用。`FrameTimeline`（D5）是唯一能把它讲清楚的机制；不做的话，
   "移动/重建/原地"会变成三种互不知道的延迟。
2. **提升 / 借用 / LOAD 三件套**：都改 `initialLayout`，都必须与 load-op 变体回切对齐。
   合成 `depthPlan()` 是唯一能穷举的方式。
3. **vsg 按 viewID 编译**：强制"状态按 pass 分家、上传按内容共享"。任何"共享一份注册表"的简化
   都会在第二个 pass 上炸——这是硬约束，不尝试绕过。

**明确不做**（与现有实现一致）：dynamic rendering（上游没有）、compute、光追、遮挡查询、MSAA、
宽线、实例化绘制、Wayland、窗口事件泵、后台线程、`VkPipelineCache` 持久化。

## 9. 上游依据与惯用法映射

本节把设计里对外部世界的假设落到**可核对的上游文件**上。引用的是仓库实际编译的那份 vsg
（`build/_deps/vsg-src`，**1.1.16**）；惯用法参考 `/opt/opensrc/vsgExamples`（1.1.15，差一小版，
只当"上游怎么用"的参考，不作为行为性结论的依据）。

### 9.1 已复核的事实

| 事实 | 上游出处（vsg **1.1.16**，以**符号**为准；行号为 2026-09-21 检出的定位提示） | 影响的设计 |
| --- | --- | --- |
| 帧在飞 = 3 个命令缓冲槽，每槽一条 fence | `Viewer::assignRecordAndSubmitTaskAndPresentation()`（`src/vsg/app/Viewer.cpp`，`:420`；其中 `numBuffers = 3` 在 `:501`）→ `RecordAndSubmitTask::RecordAndSubmitTask(Device*, uint32_t numBuffers = 3)`（`include/vsg/app/RecordAndSubmitTask.h`）+ 构造体内的 `_fences.resize(numBuffers)`（`src/vsg/app/RecordAndSubmitTask.cpp`） | D5 的槽数来源；**退役深度是推导值** |
| 槽只有在它自己的 fence 被等过之后才被重新录制 | `RecordAndSubmitTask::start()`（`src/vsg/app/RecordAndSubmitTask.cpp`，`:116`）：`if (current_fence->hasDependencies()) { current_fence->wait(...); current_fence->resetFenceAndDependencies(); }` | D5 的**完成证据**；"park 满槽数 + 1 即可释放"成立 |
| 槽索引每帧轮转 | `RecordAndSubmitTask::advance()` + `Viewer::advanceToNextFrame()` 里的每帧 `task->advance()`（`src/vsg/app/Viewer.cpp`，`:197-199`） | 退役点 = `submittedFrame() + slots + 1` 的语义 |
| **槽表每帧只填一个** ⇒ 槽数要“逐帧学到不再增长” | `RecordAndSubmitTask::index()` / `fence()`（`src/vsg/app/RecordAndSubmitTask.cpp`）：`_indices[i]` 初值为哨兵 `numBuffers`，`advance()` 每帧填一格；`index()` 越界返 `_indices.size()` ⇒ `fence(i)` 为 nullptr | 落地时被实测推翻——见 §11.3（第一帧答案是 1，绝不能当窗口） |
| 帧起点取图；槽数不在公开 API 上 | `Viewer::acquireNextFrame()`（`src/vsg/app/Viewer.cpp`，`:208`，其中 `:220` 调 `window->acquireNextImage()`）；`RecordAndSubmitTask::fence(size_t)` 越界返回 nullptr（`src/vsg/app/RecordAndSubmitTask.cpp`） | 启动期**探测/断言**槽数，不写死 4（P0-5） |
| 上游不做 dynamic rendering | 在 `src/` + `include/`（排除 `include/vsg/vk/vulkan.h`）grep `vkCmdBeginRendering` / `VK_KHR_dynamic_rendering` **零命中** | §1.2：必须走 render pass + load-op 变体 |
| 上游不碰 EDS2/3 的三个扩展入口 | 同上 grep `vkCmdSetPolygonModeEXT` / `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT` **零命中** | §1.2：三个入口要后端自己取（volk / 函数指针） |
| 上游不使用 `VkPipelineCache` | `VkPipelineCache` 仅出现在 `include/vsg/vk/vulkan.h`（拉进来的头） | §1.2：重建设备 = 重编全部管线 ⇒ "移动会话"比"重建"值钱 |

**复核状态**：上表七条均为"已逐行复核"（grep / 读码，2026-09-21 于 vsg 1.1.16 检出）；
§1.2 其余几条来自仓库既有结论（`vsg-pipeline-sharing.md`、`graphics-*.md`）与脚本断言
（`scripts/check_vsg_upstream_capabilities.py`），**未逐行复核**。

### 9.2 惯用法映射（`/opt/opensrc/vsgExamples`）

给 `VsgExecutor` 一个"上游认可的写法"参照，降低自创装配方式的风险：

| Executor 要做的事 | 上游参考 |
| --- | --- |
| 无宿主窗口会话（headless 路径） | `examples/app/vsgheadless` |
| 颜色读回（readback 相位的写法参照） | `examples/app/vsgscreenshot`、`vsgoffscreenshot` |
| 渲染到离屏 target / target 数组（MRT 语义） | `examples/app/vsgrendertotexture`、`vsgrendertotexturearray` |
| 多视图 / 子 pass（子视口、PiP） | `examples/app/vsgmultiviews`、`vsgsubpass` |
| HUD / overlay 叠层 | `examples/app/vsgoverlay` |
| 窗口 / 平台 / 设备选择 / validation 开关 | `examples/app/vsgwindows`、`vsgdeviceselection`、`vsgvalidate` |
| 命令图与提交结构 | `examples/commands` |
| 动态状态用法（逐 draw 交付的那 9 项） | `examples/state` |
| 计算 / mesh shader / 光追（"明确不做"那一节的对照） | `examples/vk/vsgcompute`、`meshshaders`、`raytracing` |
| 帧节拍与线程 | `examples/threading`、`examples/core` |

## 10. 术语

* **scope**：`beginPass()` 与 `endPass()` 之间；scope 属性属于该 pass，每次绘制属性只服务一个绘制调用。
* **键污染**：把"可以用动态状态或就地数据表达的东西"放进了管线/几何身份键，导致本可零成本的变化
  触发重建或重编。
* **`FrameDescription` / `CompiledFrame`**：前者是收集侧的**自包含快照**（允许保序、重复、缺省未解析），
  后者是编译侧的**可执行计划**（终态、缺省已解析、合法性已判、借用已快照）。
* **`FrameArena`**：帧内 bump 分配器；计划里所有 `span` 的 backing storage，稳态帧零分配。
* **兼容性键 vs load-op 变体**：前者进管线键（格式/采样/子 pass/深度格式），后者不进
  （load/store op + initialLayout）；这正是"CLEAR↔LOAD 不重编"的原因。
* **退役点**：`retire_at = submittedFrame() + kRetireDepth`，其中 `kRetireDepth = 命令缓冲槽数 + 1`；
  释放条件是 `completedFrame() >= retire_at`，完成证据来自槽位 fence 复用（D5）。
* **强连通分量跳过**：声明出的依赖成环时的唯一合法处理——分量内不画、分量外照旧、计数 + 响亮（D2.1）。
* **bootstrap**：全新 target 的颜色图像 `UNDEFINED` 无法 LOAD，所以首次进入必须清一次（用一次性的
  load-op 变体，帧末换回稳态变体）。
* **移动 vs 重建**：宿主平台窗口被销毁重建时，用新句柄重建 surface+swapchain 而保留设备与全部管线
  （移动），无法服务时才走整会话重建。

## 11. 实施记录

### 11.1 M0（2026-09-21）：`core/` 的无设备骨架 + 证据夹具

落地位置就是 §2.3 划的那两个目录：

| 文件 | 是什么 | 把哪条 P0 变成可测的 |
| --- | --- | --- |
| `include/vine/vsg/core/Protocol.hpp` + `src/core/Protocol.cpp` | scope 状态机（`Allow` / `Drop` / `Refuse` + 每 episode 上报） | P0-3：只答合法性 |
| `include/vine/vsg/core/FrameArena.hpp` + `src/core/FrameArena.cpp` | 分块 bump 分配器（**增长加块、从不搬块**） | P0-1：计划独占存储 |
| `include/vine/vsg/core/FrameTimeline.hpp` + `src/core/FrameTimeline.cpp` | `submitted` / `completed` 两条水位 + 退役点推导 | D5、P0-9 |
| `include/vine/vsg/core/RetirementQueue.hpp` + `src/core/RetirementQueue.cpp` | 按帧时间轴的延迟销毁 + 计数过的 device wait | D4 |
| `include/vine/vsg/core/TargetPlan.hpp` + `src/core/TargetPlan.cpp` | `planTarget()` / `depthPlan()` 纯函数 | P0-5、D6 |
| `include/vine/vsg/core/Keys.hpp` + `src/core/Keys.cpp` | 三层键 + 键审计表 | D3 |
| `include/vine/vsg/core/Observe.hpp` + `src/core/Observe.cpp` | 计数聚合 + 与退役队列的交叉校验 | D8 |
| `include/vine/vsg/core/PhaseTable.hpp` + `src/core/PhaseTable.cpp` | 相位表驱动（复用 `[selftest]` 行格式） | D8 |
| `include/vine/vsg/core/PixelProbe.hpp` + `src/core/PixelProbe.cpp` | 读回图像的断言夹具（非黑占比、子矩形计数、包围盒钳位） | D8：像素断言有了唯一的写法 |
| `include/vine/vsg/core/AllocationGate.hpp` + `src/core/AllocationGate.cpp` | 进程内堆增长门禁（glibc 堆用量，无插桩） | D2/D8：稳态帧零增长成为可失败的门禁 |
| `include/vine/vsg/core/Diagnostics.hpp` + `src/core/Diagnostics.cpp` | 单一诊断路由（分类计数 + sink 转发）+ `ReportOnce` | D8：报告与计数同源 |

用例：`tests/test_vsg/BackendCoreTest.cpp`（22 例）+ `tests/test_vsg/BackendEvidenceTest.cpp`（11 例），
全部零 GPU、零 `vsg::`：协议转移表；目标/深度计划表；时间轴与退役（“完成证据未到不得释放”、
“提交 ≠ 完成”）；arena（“增长不搬块”、reset 是唯一失效点、稳态零增长）；键审计（“extent 不进身份”）；
计数交叉校验；相位表输出格式；像素探针（行主序、子矩形、钳位、“什么都没画”就叫失败）；
诊断（分类计数、无 sink 也计数、episode 只报一次）；分配门禁（**稳态窗口一次也不分配**，
且“故意分配必须被抓到”这条用例保证了门禁能失败）。

**落地时被代码推翻的三处设计措辞**（本文已同步修正）：

1. v2 写“arena 增长时复制已发放的字节，所以只有 reset 会失效”——**不可能**：单一可增长缓冲一旦
   重分配，此前发出的每个 span 都指向已释放内存。用例当场抓到（读出垃圾值，并伴随堆损坏）。
   改为**分块**：增长加块、不搬块，“只有 reset 会失效”才成立。
2. v2 写 `kRetireDepth = slots + 1` 是推导值——落地时补上另一半：槽数**不在公开 API 上**
   （`Viewer::assignRecordAndSubmitTaskAndPresentation()` 里是局部硬编码），只能启动期探测/断言
   （`RecordAndSubmitTask::fence(i)` 越界返回 nullptr），不得写死 4。
3. v2 把分配门禁挂在 `scripts/alloc_trace.c`（LD_PRELOAD）上——两个理由换成进程内实现：
   ①那个文件自己写着 “an investigation instrument, **not a gate**”，而且只插桩 `mmap`（看不到帧内
   那些小分配）；②插桩分配器要在它正在包装的分配器里解析自己的真实函数。glibc 已经能在进程内回答
   “堆是否增长”，门禁只需要这一位。`alloc_trace.c` 保留原职（“内存去哪了”）。

### 11.2 M1a（2026-09-21）：设备地板 + 探测

M1 的第一半，也是新实现**第一次碰图形 API**——所以它正好把 §2.3 的边界试了一遍：

| 文件 | 是什么 | 在哪一侧 |
| --- | --- | --- |
| `include/vine/vsg/core/DeviceRequirements.hpp` + `src/core/DeviceRequirements.cpp` | 地板版本（major/minor 比较，patch 有意忽略）、必需特性表、`missingFeatureCount()`、`satisfiesRequirements()` | `core/`：无 API 头、可无设备穷举 |
| `include/vine/vsg/api/DeviceProbe.hpp` + `src/api/DeviceProbe.cpp` | 开一个即抛的 instance、枚举物理设备、把驱动答案翻成 `DeviceFacts`、关 instance | `api/`：唯一 include `volk.h` 的地方；不建设备、不建窗口 |

用例：`BackendCoreTest.cpp` 里的 `CoreDeviceRequirementsTest`（3 例：地板忽略 patch、七个特性各自有名有数、
缺一个就拒）+ `DeviceProbeTest.cpp`（无 loader / 无设备 ⇒ **SKIP** 而不是失败，“没装 Vulkan”与“装了但没设备”
是两回事）。

**实测（本机 lavapipe，2026-09-21）**：

```
[device_probe] name=llvmpipe (LLVM 21.1.8, 256 bits) api=1.4.335 features=7/7 usable=1
[device_probe] devices=1 usable=1 loader=1.4
```

⇒ 地板（1.4）与七个必需特性在仓库门禁用的软件光栅器上全部满足；探测与策略的判决一致（用例本身就在断言
“探测的 usable == 策略的 satisfiesRequirements”，不允许第二份意见）。

顺带记一个坑：`vine::String` 包的是 `std::u8string`（`value_type = char8_t`）——字面量必须写 `u8"…"`，
而驱动给的 `char[256]`（如 `deviceName`）与 printf/gtest 转发都要走仓库既有的
`reinterpret_cast<const char*>(s.data()), s.size()` 写法。

### 11.3 M1b（2026-09-21）：会话（自有窗口）+ 空帧提交 + 槽数的**逐帧学习**

| 文件 | 是什么 |
| --- | --- |
| `include/vine/vsg/api/Session.hpp` + `src/api/Session.cpp` | 会话：自有窗口 → 设备 → 交换链 → 帧配对（`beginFrame` / `commitFrame`）；API 全部藏在 PImpl 后（头文件里没有 `vsg::`） |
| `include/vine/vsg/core/SlotProbe.hpp` + `src/core/SlotProbe.cpp` | `probeSlotCount()`（问“现在能答多少个槽”）+ `SlotTracker`（逐帧折叠，直到数字不再增长） |

实测（lavapipe + X11，`SessionTest.EmptyFramesAreCommittedAndAParkedObjectWaitsForTheCompletionEvidence`）：
会话建立 → 学会槽数 → 空帧连续提交与呈现 → 在第 S+1 帧 park 一个对象 → 它在 `retire_at + slots` 帧
才被释放（早一帧的断言会红）→ 全程 `deviceWaits() == 0`。

**本轮最重要的发现（推翻了 v2 在 §9.1 的那条推断）**：**槽数不是一次探测就能得到的**。
上游的 `RecordAndSubmitTask::fence(i)` 走的是 `index(relative)` → `_indices[relative]`，而 `_indices` 每个
槽位在第一次 `advance()` 之前持哨兵值（`numBuffers`），**每帧只填一个** ⇒

* 第一帧探测得到 **1**，第二帧 2，直到填满才停在真值（本机 3）；
* “探测到 1 就当成窗口” = 早一帧销毁一个仍被已提交命令缓冲引用的对象（正是这个环要避开的事故）。

所以拆成两个类型：`probeSlotCount()` 只答“现在能答多少”，`SlotTracker` 负责“数字不再增长才算学会”
（两帧连续相等即饱和）。**学会之前不 park**：`RetirementQueue` 的 `retire()` 直接返回 false，调用方必须
退回计数过的 device wait（`parkingAvailable()` 是这条状态的判据）。学会后若与假设值（3）不一致，报一次
Info；一致则一句话也不说。

另一个落地才看清的时序细节：`RecordAndSubmitTask` 是在 `Viewer::assignRecordAndSubmitTaskAndPresentation()`
里建的（需要设备已存在），而它的槽表要等第一帧 `advance()` 才被填 ⇒ 探测的**最早可能时刻**是
“第一帧开帧之后”，不是 `initialize()` 里。

### 11.4 M1c（2026-09-21）：宿主句柄采纳 + 会话移动（不重建）

| 文件 | 是什么 |
| --- | --- |
| `include/vine/vsg/core/SessionMove.hpp` + `src/core/SessionMove.cpp` | 决策表：`MoveAction{Keep, Move, Rebuild}` × `MoveFacts{has_live_session, on_host_window, handle_announced, handle_differs}` → `MoveDecision{action, reason}` |
| `src/api/Session.cpp` | `SessionOptions::native_handle`；`initialize()` 先决策再动手；`moveTo()`；`moves()/rebuilds()/keeps()/generation()` |
| `tests/test_vsg/SessionMoveTest.cpp` | 真实 X11 窗口：同一句柄 → keep；第二个句柄 → move（`moves()==1`、`rebuilds()==0`、`generation()==2`、`deviceWaits()==1`、两个宿主窗口在 `shutdown()` 后都还活着） |

**决策表**（`planSessionMove()`，四条事实彼此独立，所以是表而不是嵌套 if）：

| 事实 | 动作 | 理由 |
| --- | --- | --- |
| 没有活会话 | `Rebuild` | 移动是给已有会话用的；首次建立只是“建在哪个句柄上” |
| 有活会话、不在宿主窗口上 | `Rebuild` | 自建窗口没有可转换的平台句柄；留在旧窗口上等于控件迁移后画面不动 |
| 有活会话、在宿主窗口上、句柄未变 | `Keep` | 同一个句柄：动它就只是白停一次设备（这是每帧重入 `initialize()` 的常态） |
| 有活会话、在宿主窗口上、句柄已变 | `Move` | 平台转换（xcb/Wayland X11 / HWND），设备与管线保留 |

**复用的是设施，不是策略**：`vine/vsg/detail` 里的 `VsgHostWindow`（`hostHandle()` / `moveToHostSurface()` /
“绝不销毁宿主的窗口”）与 `hostHandleFromVoid()` 保留原样，重写版只负责“什么时候让它动”。
移动失败（例如交换链格式与旧句柄不兼容）→ 报 Warning 并退回**重建**：失败路径就是决策表里的 `Rebuild` 分支，
不是第三套逻辑。

两次实测（lavapipe + X11）：

```text
[VsgHostWindow] attached to the host window 0x600000 (320x240, mapped=true)
[VsgHostWindow] moved to the host's new window 0x600001 (320x240); the device and its pipelines were kept
[       OK ] SessionMoveTest.ASecondHostWindowMovesTheSessionAndTheSameOneKeepsIt (43 ms)
```

**落地才看清的两件事（都是计数器/设计措辞导致的）**：

1. `vsg::Viewer` **没有** `device` 成员；停设备要走 `viewer->deviceWaitIdle()`
   （既有实现的 `VsgRetireRing::waitForIdle(viewer)` 用的就是它，见 §9 的引用约定）。
2. “学会槽数”时**不能**重建 `RetirementQueue`（`= RetirementQueue(slots)`），因为队列自己也数着“被计数的
   device idle”；重建会把那些数悄悄清零（测试里表现为 `deviceWaits()==0`，而移动明明发生了）。
   现在改成 `setSlots()` 就地改窗口 ⇒ 计数器与窗口互不干扰。

### 11.5 M2a（2026-09-21）：内容身份与存储策略（core）

M2（内容）落地时按前几个 M 的做法拆成三段：**M2a 身份与存储（纯 core，本节）**、M2b 变体池与每 pass 状态
注册表、M2c 设备侧（上传 / 描述符 re-point / 绘制录制 + 相位）。

| 文件 | 是什么 |
| --- | --- |
| `core/Streams` | `StreamKey`（**切片 + revision 就是身份**）、`GeometryStreams` / `GeometrySnapshot`、纯函数 `planGeometry`（`None` / `Refresh` / `Rebuild`）、`SharedStreams`（几何别名的登记；**不持有字节**） |
| `core/MaterialArena` | 材质块的持久存储：slot × in-flight 副本轮转；revision 未变 ⇒ 零写，变了 ⇒ 每帧恰好一次 64B 写 |
| `core/FrameRing` | 每帧 ring（视图块 / 绘制块）：容量固定，超预算 ⇒ 拒绝 + 计数（**不增长**） |
| `tests/test_vsg/ContentCoreTest.cpp` | 19 个无设备用例（Streams 7、SharedStreams 4、MaterialArena 5、FrameRing 3） |

**`planGeometry` 的判据表**（纯函数，输入只有两个快照）：

| 输入事实 | 动作 | 理由 |
| --- | --- | --- |
| 还没建过（快照为空） | `Rebuild`（允许共享上传） | 每个流都是新的 |
| 通道形状 / 通道集合 / 有无索引流变了 | `Rebuild` | 装配方式变了，不是字节变了 |
| 通道的字节动了（revision 变） | `Refresh`（列出动过的 location） | 逐流重指：重传的粒度就是一条通道 |
| 索引 **buffer** 换了、span 没变 | `Refresh`（`index_refreshed`） | bind 别名整个 buffer，draw 才声明 span |
| 索引 **span**（offset/count）变了 | `Rebuild` | span 写进了装配好的节点里 |
| 流全同、只有 geometry revision 动了 | `Rebuild` + **拒绝共享上传** | 这是“没有流能解释的声明”（应用透过裸指针改了字节），共享上传持有的是**插入当时**的字节，不能替它背书 |

**三条落地时钉住的边界**：

1. `SharedStreams` **不拥有字节**：容量到顶时最老的 entry 只是离开**查找表**（`evictions()` 数它），已经绑上它的
   读者照旧读自己那份 —— “entry 离开 map” 与 “字节可以释放” 是两件事，后者只能由持有 GPU 对象的层决定；
2. 派生通道（白透明度载体 / 零 UV / 算出来的法线）`buffer == nullptr`，**永远不能共享**：它的字节属于某一个几何；
3. `MaterialArena` 的写 **每材质每帧至多一次**：应用一帧内改两次得到的是 2 个 note、1 次 64B 写（GPU 只可能看到
   一个版本），稳态帧的 `writes()` 保持不变 —— 这就是 “材质稳态零流量” 的可断言形式。

### 11.6 M2b（2026-09-21）：管线变体池 + 每 pass 状态注册表（core）

| 文件 | 是什么 |
| --- | --- |
| `core/VariantPool` | 按 `PipelineKey` 索引的变体池：`acquire()` 答 `Created / Reused` + 一个**永不复用**的 id；FIFO 上界（65，与现有程序缓存同值）；`keyOf(id)` 让条目自己持有键对象 |
| `core/StateRegistry` | 每个 pass 一份：解析出「绑哪个变体」与「动态块要不要重发」两笔**分开的**代价（`variant_switched` / `dynamic_issued`） |
| `Keys` 新增 `pipelineKeyFingerprint`（`PipelineKeyHash`） | 键的哈希与 `operator==` 写在**同一处**：只有 `==` 决定是不是同一个变体，哈希只为查找 |
| `tests/test_vsg/VariantCoreTest.cpp` | 11 个无设备用例（池 5、注册表 6） |

**验收对应**（§7 里 M2 那条 “churn 下 `pipelineVariantCount` 不涨”）：

| 行为 | 断言 |
| --- | --- |
| 同一身份两次查找 | `created()==1`、`reused()==1`、同一个 id |
| 动态状态 churn（cull / polygon / topology / depth / blend 各 20 步） | `pool.created()==1`、`variants()==1`、`variant_switches()==1`、`dynamic_issued()==21` —— **动态层只花 set 命令** |
| 程序 / 布局 / 兼容性 / 深可采样 / shadow 任一改变 | `Created`（6 个键 ⇒ 6 个变体，同键再来仍是 `Reused`） |
| 两个 pass 用同一身份 | 各自 `variant_switched==true`（各自要绑），`pool.created()==1`（编译只有一次） |
| 值没变 | `dynamic_issued==false`（不重发）；换变体但值没变也**不**重发 |
| 容量到顶 | 最老的查找离开（`evictions()==1`），持有者仍持有自己的对象；被淘汰的键再 `acquire` ⇒ `Created`（id 不复用 ⇒ 陈旧 id 无法冒充新变体） |

**这条边界是刻意钉死的**：池只接受 `PipelineKey`（身份层）；`DynamicState` 永不进池 —— 测试通过 20 步 churn 后
`created()` 仍为 1 来证明，而不是靠注释保证（这正是 §6.1 第 3 条“键是显式结构体 + 审计测试”的行为半边）。

### 11.7 M2c-1（2026-09-21）：几何上传与共享（api）

M2c 拆成两段：**M2c-1 上传与共享（本节，已完）**、M2c-2 环/材质映射缓冲 + 描述符 re-point + 绘制录制 + 相位。

| 文件 | 是什么 |
| --- | --- |
| `api/StreamUploads` | 对象层：`StreamKey` → 共享 bind（`acquireVertex` / `acquireIndex` / `release`）；决策仍然由 `core::SharedStreams` 做，这里只持有对象并与之同步 |
| `core/Streams` | `Decision` 新增 `evicted`：容量到顶时把“离开 map 的键”告诉调用方 —— 对象层由此可与登记表对齐，而 core 里**没有回调** |
| `tests/test_vsg/StreamUploadsTest.cpp` | 10 个无设备用例 |

**可观测含义**：一个 bind 命令拥有自己的 `BufferInfo`，而 `BufferInfo` 决定“一个设备缓冲 + 一次上传”。所以
“两个 drawable 读同一流”有一个不需要 GPU 的判据 —— **它们拿到同一个 bind 对象**（测试断言指针相等）。
同理：切片不同 / revision 不同 / buffer 不同 ⇒ 不同 bind（那些是不同的字节）。

| 规则 | 结论 |
| --- | --- |
| 可共享 | 四个 canonical 顶点通道（且字节是模型自己的）+ 索引流 |
| 拒绝（loud + 计数） | 派生通道（白透明度载体等，属于某一个几何）、自定义通道（location ≥ 3：它们的 bind 身份是**整个自定义布局**的一条命令） |
| 索引键 | **归一化**为整个 buffer（无 offset/count）：bind 别名整个 buffer，draw 才声明 span ⇒ 同一 index arena 的两个切片共享一次上传；`release` 用同一个归一化 ⇒ 不需要调用方再给 span |
| 淘汰 | `Decision.evicted` ⇒ 对象层同步丢弃该 bind（已经绑上它的读者持有自己的 ref，读到的字节不受影响） |
| 两账一致 | `agreesWithRegistry()`（`live() == objects()`）是断言，不是假设 —— 与 `Observe` 的“同一份数据两个视图必须相等”同一条纪律 |

### 11.8 M2c-2a（2026-09-21）：每帧块存储（api，真设备）

M2c-2 再拆一次：**M2c-2a 块存储（本节，已完）**、M2c-2b 描述符 re-point + 绘制录制 + 相位。理由是两者的证据面不同：
存储的正确性（区域、旋转、稳态零写、拒绝不增长）可以在真设备上直接读回证明，而录制要等管线/描述符齐了才有像素。

| 文件 | 是什么 |
| --- | --- |
| `api/BlockStorage` | **一个**映射缓冲（HOST_VISIBLE｜HOST_COHERENT）装三类块：视图块 / 绘制块（`core::FrameRing` 决定本帧可写哪个 slab）+ 材质块（`core::MaterialArena` 决定 slot 与 in-flight 副本）；`writeView` / `writeDraw` / `writeMaterial` 直接把调用方的块字节 memcpy 进去 |
| `tests/test_vsg/BlockStorageTest.cpp` | 8 个用例，7 个在**真设备**上跑（X11 + lavapipe，否则 SKIP），1 个无设备（空设备 ⇒ 无存储） |

**为什么是映射内存**：这些块在帧在飞时还要被 GPU 读，`vsg::Data` 的 DYNAMIC 语义会在一个值变化时整块转移；
映射内存让帧自己的线程就地写、可见性由 COHERENT 保证、没有 staging 拷贝 —— 这正是“稳态帧零流量”的机制面。

| 规则 | 结论 |
| --- | --- |
| 布局 | 三段区域（视图 / 绘制 / 材质）在一个 buffer 里，区域起点按**设备的** `minUniformBufferOffsetAlignment` 对齐（调用方只给块的**大小**，从不给 offset 或 stride） |
| 旋转 | 视图/绘制各一个 ring：本帧写自己的 slab（3 帧在飞 ⇒ 3 个 slab，第 4 帧回到第一个）；材质按 in-flight 副本轮转 |
| 稳态 | 同一 revision 的材质**零写**（映射里的字节不变，测试直接读回证明）；真编辑每帧恰好一块 |
| 拒绝 | 超过区域 stride 的块 ⇒ `oversized()` 计数、**不消耗**本帧预算；超过 blocks_per_frame ⇒ `overflows()` 计数，buffer 不迁移 |
| 一帧内两次编辑 | 第二次是 `Unchanged`（GPU 只可能看到一个版本），映射里仍是draw 会用到的那个版本 |

真设备读数（lavapipe + X11，`BlockStorageTest`）：8/8 通过；region 不重叠、所有写入落在自己的区域内、读回等于写入。

### 11.9 M2c-2b-1（2026-09-21）：块描述符（api，真设备）

| 文件 | 是什么 |
| --- | --- |
| `api/BlockDescriptors` | 一个 set 布局（3 个 **dynamic uniform** 绑定：0 = 视图块、1 = 绘制块、2 = 材质块）+ 一个 set（每个绑定 `range` = 一块、descriptor 自身 offset = 0）+ `bind()`（按**绑定序**给 3 个 dynamic offset）+ `repoint()`（换存储、留布局） |
| `api/BlockStorage` | 新增 `strides()`：每区域两块之间的距离（含设备对齐补齐），即每个绑定的 `range` |
| `tests/test_vsg/BlockDescriptorsTest.cpp` | 5 个真设备用例（lavapipe + X11，否则 SKIP） |

| 规则 | 结论 |
| --- | --- |
| 一个 set 服务所有 draw | “哪一块”由 dynamic offset 决定；测试断言两次不同 offset 的 `bind()` 拿到**同一个** `descriptorSet` 对象 —— 这就是“编辑是一次写、新帧是一次偏移”的机制面 |
| 对齐即拒绝 | dynamic offset 必须是设备 `minUniformBufferOffsetAlignment` 的倍数（VUID-vkCmdBindDescriptorSets-pDynamicOffsets-01971）：不合规 ⇒ **不给命令** + `refusals()` 计数，而不是把验证错误交给驱动 |
| re-point | 换存储只换 set 元素、保留 layout；旧 set 不在这里死——绑定它的命令持有引用，调用方照常 park |
| 落地抓到的真 bug | `vsg::DescriptorBuffer::create` 的参数序是（bufferInfo、**目标绑定**、数组元素、类型）：把绑定写进数组元素槽会让三个 descriptor 全落在 binding 0 的元素 0/1/2 上（布局每绑定只有 1 个元素 ⇒ 越界），第 2 个用例当场抓住 |

### 11.10 下一步

### 11.10 M2c-2b-2a（2026-09-21）：内容管线与着色器（api，无设备）

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPipeline` | GLSL 两段（vertex / fragment）→ `vsg::ShaderStage`（进程内 glslang）→ **一身份一个** `vsg::GraphicsPipeline`；布局 = 块集（set 0）+ 128B push；状态表 = 6 个 baked 状态 + 动态声明 |
| `tests/test_vsg/ContentPipelineTest.cpp` | 7 个**无设备**用例（`vsg::GraphicsPipeline` 在 `vsg::Context` 编译之前只是 create-info，GLSL→SPIR-V 不需要设备） |

| 规则 | 结论 |
| --- | --- |
| 一身份一对象 | 同一 `PipelineKey` 第二次 acquire 拿到**同一个** pipeline 指针（池决定，层只建造）；`pool.created()==1` |
| 动态半声明 | 既有实现的 9 项（depth test/write/compare、cull、front face、polygon、topology、blend enable/equation）**加 viewport 与 scissor** |
| 为什么多两项 | 这是重写版对既有实现的**有意改进**：既有实现把 extent 传进了 `makeContentShaderSet` ⇒ resize 会重建内容管线；声明 viewport/scissor 动态后，resize 只是换一条 set 命令，extent 真的不进身份（D3 的“像素尺寸是运行时资源”由此闭环） |
| keep-alive 不是所有者 | 池淘汰身份 ⇒ 层同步丢对象（`agreesWithPool()` 断言）；绑定它的命令持有自己的引用 |
| 说不了就不说 | 着色器编译失败 ⇒ `create()` 返回 null，而不是造一个永远画不出东西的层 |
| 三层键在真对象上闭环 | 20 步动态 churn：`pool.created()==1`、`layer.compiles()==1`、`registry.dynamic_issued()==21`；而 `revision` 变一次 ⇒ 恰好新 pipeline 一个 |

### 11.11 M2c-2b-2b-1（2026-09-21）：绘制录制（api，无设备）

| 文件 | 是什么 |
| --- | --- |
| `api/StateCommands` | 引擎状态 → API 命令的映射：`makeDynamicStateCommand`（复用既有的 `detail::SetDynamicState`，携带三个扩展入口点）、`makeViewportCommand` / `makeScissorCommand` |
| `api/ContentDraw` | 一次解析好的绘制 → `vsg::StateGroup`：`stateCommands` = [管线绑定（**仅换变体时**）、动态块（**仅值变化时**）、块描述符绑定]；children = `Commands`[viewport、scissor、顶点绑定、索引绑定、`DrawIndexed`] |
| `tests/test_vsg/ContentDrawTest.cpp` | 6 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| 两个“仅当” | 同一变体 + 同一状态连续两次绘制 ⇒ 第二次的 `stateCommands` 只剩块描述符绑定（`pipeline_binds==1`、`dynamic_commands==1`）——pass 的命令缓冲与**内容**成正比，而不是与绘制次数 |
| 动态块的槽 | `kDynamicStateSlot == 15`：槽是状态栈的**身份**不是优先级，取默认槽 0 会顶掉管线绑定（既有实现从驱动崩溃里学到的那条，这里断言钉住） |
| 换身份 ≠ 重发动态块 | 换变体要重绑管线，但动态值没变 ⇒ **不**重发（写这条用例时预期写错成 3 条命令，测试当场纠正为 2 条） |
| 两条引擎约定 | reverse-Z ⇒ `VK_COMPARE_OP_GREATER`；**front face = CLOCKWISE**（vsg 的投影翻转 Y，声明成直觉的 CCW 会让每个 cull 模式作用在错误的面——静默）；混合**恒开**，`blend.enabled` 只选因子 |
| 视口 / 裁剪 | 矩形是**命令**（这正是 extent 不进身份的机制）；scissor 的负原点与零面积按 API 要求钳制，而不是交给驱动 |
| 顺带修掉的偏差 | `ContentPipeline` 的 baked 常量原写成 `LESS_OR_EQUAL` + `COUNTER_CLOCKWISE`（照抄时的直觉值），已按引擎约定改为 `GREATER` + `CLOCKWISE` |

### 11.12 M2c-2b-2b-2a（2026-09-21）：离屏目标与读回（api，真设备、无窗口）

| 文件 | 是什么 |
| --- | --- |
| `api/Device` | **无窗口**的设备缝：instance（版本从 1.4 往下试，取 loader 接受的那个）→ 按 `core::DeviceRequirements` 逐设备挑选 → `vsg::Device` + graphics queue family |
| `api/DeviceFeatures` | 设备特性策略的**唯一拼写**（`static_assert(core::kDeviceFeatureCount == 7)`）；`Session` 改为调用它——同一个策略不能有两个拼写，否则"窗口进来的设备"与"无窗口的设备"能力不同 |
| `api/OffscreenTarget` | 一张图 + 一个 render pass（CLEAR、`initialLayout = UNDEFINED`、`finalLayout = TRANSFER_SRC`【**彩色部分已由 §11.16n 改为 SHADER_READ_ONLY**】）+ framebuffer + RenderGraph（`clearValues` + **`renderArea`**）+ capture 节点（`CopyImageToBuffer` + buffer barrier）→ `probe()` 给出紧凑 RGBA8 的 `core::PixelProbe` |
| `tests/test_vsg/OffscreenTargetTest.cpp` | 3 个真设备用例，**不需要窗口也不需要显示服务器**（lavapipe 头部路径） |

| 规则 | 结论 |
| --- | --- |
| 证据通道 | 读回是命令图的一部分（`capture()` 由调用方挂在 render graph **之后**）：同一命令缓冲、同一队列、顺序有保证；帧提交 + device idle 之后才 `probe()` |
| 为什么不需要 barrier | `initialLayout = UNDEFINED` + 每帧 CLEAR：上一帧内容被丢弃是**有意**的，这正是"读回不用 barrier 舞蹈"的代价与收益（该目标不支持 LOAD 上一帧——相位本来就每帧清）。**已修订（M3d-3b-8，§11.16n）**：读回的那一次拷贝此后自带一对 layout 转移（彩色附件改为按 SHADER_READ_ONLY 收尾，因为它是下一趟 pass 的纹理） |
| 落地抓到的坑 | vsg 的 `RenderGraph` 默认 `renderArea` 是**零面积**：pass 记录成功却一个像素都没清，读回全 0，看起来像"拷贝坏了"。设上 `renderArea` 后立刻通过——这类"沉默的空 pass"正是像素相位存在的理由 |
| DeviceProbe 重写 | `probePhysicalDevices()` 从 volk 版改为 vsg `Instance` + `PhysicalDevice::getFeatures<>` 版：**特性读取只剩一处实现**（`describePhysicalDevice`），并消掉新 api 层的 volk 依赖（include 顺序陷阱随之消失）；既有探测用例仍绿 |

### 11.13 M2c-2b-2b-2b（2026-09-21）：端到端像素证据（整条栈画一个三角形）

| 文件 | 是什么 |
| --- | --- |
| `tests/test_vsg/ContentStackTest.cpp` | 2 个真设备用例，**无窗口**：整条重写栈（`StreamUploads` → `BlockStorage` → `BlockDescriptors` → `ContentPipeline` → `ContentDraw` → `OffscreenTarget`）画三角形并读回像素 |

| 规则 | 结论 |
| --- | --- |
| **三重验证齐了** | ① 验证层干净（`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` 强制开启后零 VUID）② 计数器（`uploads==2`、`pool.created()==1`、`compiles==1`、`pipeline_binds==1`、`dynamic_commands==1`、`materialWrites==1`）③ **像素**（中心是片元色、两角是 clear 色） |
| dynamic offset 的端到端证明 | 一帧两个 draw、两个绘制块：左三角与右三角各自落位，中间**仍是 clear 色**——若偏移、描述符或录制任何一处错，画面会是"两个三角重叠 / 只有一个 / 什么都没有"，而这三个都是计数器看不出来的 |
| 动态入口点 | 测试用 `detail::fetchDynamicStateEntryPoints(device->vk(), instance->vk())` 取真实的三个函数指针（会话将来同样取）：不取则多边形模式与混合调用被跳过，画面变成 create-info 的值 |
| 落地抓到的坑 | 断言助手把 clear 色写成"深蓝 `b > 150`"，而 0.25 蓝量化后是 **64**：像素相位的第一原则是**读回的值说了算**——期望值必须来自同一量化，这次是把 `(0, 0, 64, 255)` 打出来才看清的 |
| 里程碑状态 | **内容绘制（M2）到此完整**：身份/计划/别名/材质/ring（M2a）、变体池 + 状态注册表（M2b）、共享上传（M2c-1）、块存储与描述符（M2c-2a/2b-1）、管线与录制（M2c-2b-2a/2b-1）、端到端像素（2b-2b） |

### 11.14 M2c-3（2026-09-21）：会话侧内容挂载（内容经 `Session` 进帧）

| 文件 | 是什么 |
| --- | --- |
| `api/SessionContent.hpp` | api 内部的三件套：`root(session)`（会话每帧渲染的组）、`device(session)`（内容必须建在**会话的设备**上）、`recompile(session)`（把后来挂上的内容编译出来）；`Session` 只加了一行 friend，公开头文件仍无 `vsg::` |
| `src/api/Session.cpp` | `initialize()` 创建内容根并挂到 render graph（**在 compile 之前**）；`shutdown()` 随会话丢弃；三个 accessor 的定义 |
| `tests/test_vsg/SessionContentTest.cpp` | 真设备 + X11 用例：整条栈的内容挂进会话 → 提交 3 帧 → 用 `xcb_get_image` 读**窗口自己的像素**断言 |

| 规则 | 结论 |
| --- | --- |
| 为什么需要 recompile | vsg 编译一次并保留已建对象；后挂的内容没有实现、根本录制不出来。再跑一次 compile 只补新对象（管线实现存在即跳过），不必重建会话、不重编管线 |
| 证据 | 窗口中心 = 片元色、角落 ≠ 片元色（"整窗一个颜色"过不了这一关）；`framesPresented()>=3`、`deviceWaits()==0`、`draws==1 / binds==1 / dynamic==1`；**强制验证层下 0 VUID** |
| 内容必须在会话的设备上 | accessor 给出会话自己的设备（内容建在别的设备上，会话的命令缓冲引用不了） |
| **落地抓到的坑（只有像素能报的）** | 同一份内容在离屏目标（**无深度附件**）里画得出，在窗口里**画不出**：窗口的 pass 有深度附件、按 reverse-Z 清到 **0.0**，而深度比较是 `GREATER` ⇒ 三角形若躺在 z = 0 就被深度测试丢弃。离屏相位永远发现不了这个差异——这正是"两个入口都要像素证据"的理由 |

### 11.15 M3a（2026-09-21）：清屏/加载策略（无设备可判）

| 文件 | 是什么 |
| --- | --- |
| `core/ClearPlan.hpp` | `ClearPolicy`（这一趟 pass 想清什么：`color` / `color_value` / `depth` / `depth_value`）、`AttachmentClear`、`DepthClear`、`PassClearPlan`、常量 `kReverseZFarDepth = 0.0F`，以及纯函数 `planClearValues(shape, policy, bootstrap, depth_preserved)` |
| `src/core/ClearPlan.cpp` | 四条规则的实现（下表）；四个类型的相等比较 |
| `tests/test_vsg/ClearPlanTest.cpp` | 规则表的 6 个用例，无设备 |

| 规则 | 内容 | 为什么是它 |
| --- | --- | --- |
| 1 | `bootstrap` ⇒ **每个**附件都清：附件 0 取 policy 的颜色、**额外附件保持透明黑** `{0,0,0,0}`、深度取 policy 深度 | 新建（或已失效）的镜像初始布局是 `UNDEFINED`，**不能 LOAD**；清屏是唯一合法的第一笔。额外附件涂成 pass 颜色是"看起来很合理"的错误画面——单附件用例永远发现不了 |
| 2 | 非 bootstrap 且没要求清 ⇒ 该附件 `Load` | 默认是"保留"，不是"清掉"：清屏是开销，也是内容的破坏 |
| 3 | `depth_preserved` ⇒ 深度**永不清**，**且压过 bootstrap** | 深度被后一趟 pass 依赖时（延迟渲染的前置 pass），清掉它会让后一趟读到远平面、把每个片元当成可见 |
| 4 | 要求清颜色 ⇒ **所有**颜色附件都清，但**只有附件 0 拿到这一趟的颜色** | 与规则 1 同一件事的不同入口：`LoadOp` 是逐附件的，颜色值是"这一趟的画面" |

| 结论 | 内容 |
| --- | --- |
| 反转 Z 的远平面是 `0.0` | `kReverseZFarDepth = 0.0F`。深度清到 `1.0` 配 `GREATER` 比较 ⇒ 每个片元都被丢弃（空画面），而这个常量是唯一能把这个约定写成可断言事实的地方 |
| 为什么是纯函数 | 三个输入（形状、policy、bootstrap/depth_preserved）决定整张表，表可以无设备钉死；设备侧只负责把 `Load`/`Clear` 翻译成 API 枚举 |
| 与 `planTarget` 的分工 | `planTarget` 回答"这个目标要不要重建/换镜像"（`RepairReason::Bootstrap` 就是规则 1 的触发点），`planClearValues` 回答"这一趟 pass 的附件各自怎么开始、怎么结束" |

### 11.16 M3b（2026-09-21）：多附件目标（MRT + 深度）与逐附件读回

| 文件 | 是什么 |
| --- | --- |
| `api/OffscreenTarget.hpp` | 新增 `TargetLayout`（引擎语汇的形状：`color_formats` 多个 + 可选 `depth_format` + `core::ClearPolicy`）；旧 `Layout` 保留为等价的单附件写法。新增 `capture(i)` / `probe(i)` / `colorAttachmentCount()` / `hasDepth()` |
| `src/api/OffscreenTarget.cpp` | 每附件一张镜像 + 视图；pass 的附件描述与 clear value 全部由 `planClearValues`（bootstrap 恒开）驱动；**清屏值按附件顺序**排布；每个颜色附件各有一块 host-visible 目的缓冲与一个 copy 节点 |
| `tests/test_vsg/MrtTargetTest.cpp` | 真设备用例 2 个：清屏分量（附件 0 = policy 颜色、附件 1 = 透明黑）与双输出着色器（附件 0 绿、附件 1 红） |

| 规则 | 结论 |
| --- | --- |
| 形状用引擎语汇 | `TargetLayout` 收 `RenderTarget::ColorFormat` / `DepthFormat`，转 API 枚举的是 api 层（与 `VsgPipelineFactory` 同一张映射表），core 不需要认识 `VkFormat` |
| bootstrap 恒开 | 目标的镜像每帧以 `UNDEFINED` 起始、pass 每帧清屏，所以"这趟是 bootstrap"在这个目标上是永远为真的事实；与其让调用方猜，不如由目标自己声明，并在计划里出现 `Load` 时**拒绝创建**（LOAD 一张 `UNDEFINED` 镜像不是策略选择，是 bug） |
| 深度附件的依赖 | 有深度时子依赖要覆盖 `EARLY/LATE_FRAGMENT_TESTS`，否则上一帧的深度读取会和这一帧的深度写入重叠 |
| **落地抓到的坑** | MRT 的"第二个附件等于第一个"很难被察觉：只要 `capture(i)` 落到同一块缓冲，`probe(1)` 就会返回附件 0 的像素，画面看起来"MRT 成功了"。所以证据必须是**两个不同的片元输出**（绿 / 红），而不是同一个颜色的两次复制。另外：片元阶段**没写**的附件在 MRT 下内容是 `UNDEFINED`（不是清屏值），所以"不清也能看到清屏色"这件事本身不是证据 |
| 三重验证 | 6 + 2 = 8 个新用例；`test_vsg` 全量 **444 用例 / 70 套件全绿**（含既有 `OffscreenTargetTest` 走旧 `Layout` 入口，证明兼容）；`check_include_hygiene.py` 0 findings / 769 文件 |

### 11.16b M3c（2026-09-21）：借用的深度，以及装上验证层后当场抓到的两个真 bug

| 文件 | 是什么 |
| --- | --- |
| `api/OffscreenTarget` | `create(device, layout, depth_source)`：借用方不建镜像、用出借方的视图、`initialLayout = DEPTH_STENCIL_ATTACHMENT_OPTIMAL`、按 `depth_preserved` LOAD；`depth()` 把事实交给 `core::depthPlan`（`borrowed` 恒 `sampleable=false`；出借方被借用 ⇒ 提升被撤销，借用方析构即恢复）；`captureDepth()` / `depthProbe()` |
| `core/DepthProbe`（新） | 深度读回的数据面：`valid()` / `depthAt()` / `countNear(value, tolerance)`；越界答 0 而不是越界读 |
| `core/DeviceRequirements` | 设备地板补上**扩展**那一半：`DeviceExtension`（EDS2 / EDS3）+ `missingExtensionCount` + `extensionName`；`satisfiesRequirements` 同时要求版本 ∧ 特征 ∧ 扩展 |
| `api/DeviceFeatures` | `requiredDeviceExtensions()`：名字与上面 7 个特征位是**同一条策略的两半**，`static_assert` 钉住数量 |
| `api/Device` / `api/Session` | 两处建设备的地方都启用这两个扩展名；`createDevice` 现在把 `vsg::Instance::create` 的**异常**当成"换下一个版本"而不是直接终止；验证层不可用时**降级并如实报告**（`DeviceResult::validation`） |
| `tests/test_vsg/SharedDepthTest.cpp` | 真设备用例 2 个：事实/提升撤销/拒绝（无深度、格式不匹配）；像素 + 深度双重证据 |

| 规则 | 结论 |
| --- | --- |
| **借用的深度不是 pass 级的细节** | `VkSubpassDependency` 的 stage/access mask **属于 render-pass 兼容性**（验证层点名 `VUID-vkCmdDrawIndexed-renderPass-02684`）：借用方为了读别人的深度而多声明一个 `DEPTH_STENCIL_ATTACHMENT_READ`，就会和出借方**不兼容**——同一个管线、同一个形状、两个不能共用的 pass。所以依赖按**形状**决定（有深度就把 early/late fragment tests 与 depth 读写全写上，取超集），而 loadOp 与 layout 才允许逐 pass 不同 |
| **扩展没启用，动态状态就是非法的** | `VK_DYNAMIC_STATE_POLYGON_MODE_EXT` / `COLOR_BLEND_ENABLE_EXT` / `COLOR_BLEND_EQUATION_EXT` 属于 `VK_EXT_extended_dynamic_state3`（1.4 也没吸收）。之前的设备缝只请求了**特征位**、没有启用**扩展名** ⇒ 管线 create-info 非法、`vkCmdSet...EXT` 通过函数指针调用到未启用的能力上。**只有验证层能看见这一类错误**：像素和计数器都可以全绿 |
| **验证层是仪器，不是地板** | 它是单独的软件包，很多机器没有。缺了要**降级并报告**（`DeviceResult::validation`、测试里打印 warning），不能让"验证干净 ∧ 计数器 ∧ 像素"悄悄变成两重。仓库自己的 lavapipe gate 也是这个态度（warn and proceed） |
| **Vulkan 的 NDC z 就是深度** | 顶点写 `gl_Position.z = z` ⇒ 默认视口下深度就是 `z`，**没有** OpenGL 的 `z*0.5+0.5`。写错的后果极像"被遮挡"：把"远处"三角形写在 z = -0.5 就是落到了远平面之外，**到处**被 `GREATER` 拒绝，画面与"共享深度生效"一模一样。最后是**深度读回**把这件事一句话讲清楚（0.5 / 0.1 / 0.0 三个值），颜色图永远说不清 |
| 深度读回的诚实 | D32 → 原样 float，D16 → ÷65535；**D24S8 拒绝**（组合格式没有普通深度拷贝语义，按 float 读出来的数字"像深度但不是"）。镜像要带 `TRANSFER_SRC`，否则拷贝是验证错误；拷贝前后各一次 image barrier，把 layout 还原成附件布局——不然共享给后一趟 pass 的深度就废了 |
| 证据 | 颜色：出借方红、借用方在被遮挡处**清屏色**（不是绿）、借用方自己的三角形绿。深度：出借方三角形处 `≈0.5`、清屏处 `0.0`、借用方自己的三角形处 `0.1`，且**每个 texel 恰好是这三个值之一**（第四种值就意味着有别的 pass 写过深度）。计数器：`draws=3 / refusals=0`；`test_vsg` 全量 **448 用例 / 72 套件全绿**，全量输出 `Validation Error` 计数 **0**；hygiene 0 findings / 772 文件 |

### 11.16c M3d-1（2026-09-21）：帧的收集段（`core/FrameRecorder`，无设备）

M3d（“执行器按 `Schedule` 机械落地”）按**证据面**拆成三段：**M3d-1 收集（本节，已完）**、
M3d-2 编译（`FrameCompiler`：normalize → `FrameGraph` → `Schedule` → `CompiledFrame`，含环的跳过语义）、
M3d-3 执行（`api/VsgExecutor`：把已经决定好的计划机械地变成 vsg 图 / 提交 / 呈现）。拆法与 M2c 同一把尺子：
收集段的行为能在无设备下逐条钉死（arena 归属、消费规则、三种裁决），而执行段要有像素才谈得上证据。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameRecorder.hpp` | 收集段的类型与入口：`PassId`、`ProgramRef`、`InputRef`、`LightRef`、`CameraSnapshot`、`CollectedCommand`、`DrawKind`、`CollectedDraw`（= **一次绘制调用**）、`CollectedPass`、`FrameDescription`、`FrameRecorder` |
| `src/core/FrameRecorder.cpp` | 规则实现：协议先行、arena 快照、scope 属性 vs 逐绘制消费、三种裁决的落地与上报 |
| `core/Protocol` | 新增 `droppedCount()` 与 `frameOpen()`；`Drop` 的两个出口收敛到一处计数（Drop 是唯一静默的裁决，只有计数能把它从“什么都没发生”里分出来） |
| `tests/test_vsg/FrameRecorderTest.cpp` | 14 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **arena 独占（P0-1）** | 计划里没有一个 span 指向宿主容器：`CollectedCommand` 在 `render()` 里复制，`CollectedDraw`/`CollectedPass` 在 `endPass()`/`endFrame()` 复制。用例按引擎自己的模式驱动（一个 vector 清空重填）后断言**第一趟 pass 的记录仍按字节等于它当时拿到的值** |
| **一次绘制调用，不是一条命令** | 粒度就是调用，因为契约恰在这个粒度上消费公告，视口/深度也只在调用级有意义；相机按调用记一次而不是按命令（1000 条命令不该抄 1000 份矩阵）。与契约同一句话：`render()` 里的 1000 条命令共享这一次调用的全部公告 |
| **逐绘制消费 vs pass 属性** | 视口 / 光照被“下一次绘制调用”消费（SDK 原文：one announcement serves ONE drawing call）；**输入属于 pass**（引擎自己只宣布一次并写明 “every draw call in it sees the same list”）；target / order / clear / depth 属于 scope，`endPass()` 丢弃。§5 草图把 `lights` 画在 pass 级：那是草图与契约的分歧，落地按契约走 |
| **没画东西的 scope 不进计划，除非它宣布了 clear** | 清屏走的是该 pass 自己的 load-op，丢掉一个“只清屏”的 pass 的代价是画面底色没了；只宣布 target/order/viewport 却什么都没画的 scope 丢掉是**对的**（“未消费的 scope 属性在 endPass 丢弃”的可观测形式） |
| **三种裁决各自落地** | `Allow`→记录；`Drop`→不记录、不报（无 scope 的 setter 是合法的惰性），但计数；`Refuse`→不记录、上报一次/每 episode。episode 的边界由 `Protocol` 拥有，录制器不留第二份“报过没有”的标志 |
| **上报消息带数字** | `frame 3: a drawing call arrives with no pass scope open: it is refused and nothing is drawn...`；“死 scope”与“无 scope”是两句不同的话（前者说“这是那个被释放的 target”，后者说“下一个 pass 从空请求开始”） |
| **借用的东西只抄不留** | 灯→`LightRef` 数字（用例在 endFrame 之后 `reset()` 掉 `Light` 再断言快照）；相机→`CameraSnapshot`（VIEW/PROJ + eye/target/up）；程序→identity+revision；几何→identity+revision。材质只记身份：SDK 的 `Material` **没有** revision 访问器，材质 revision 的来源在 api 层材质管理器（那是它的账） |
| **default program 不走状态机** | 它在所有状态下都合法，问协议只会得到一个恒为 `Allow` 的 `CallKind`；录制器记身份+revision，但**不替换**命令里为 null 的 program——“缺省未解析”是收集侧允许的形状，解析是编译器的活（P0-2） |
| **死 scope 不会被新公告救活** | 被释放的 target 让该 scope 余下的生命失效；后续 `setRenderTarget` 按 `Drop` 落地（上报一次），之后的绘制仍被拒。旧实现注释里的“新的 `setRenderTarget` 会清掉标记”是**有意不保留**的行为：协议已经钉死，录制器不绕过裁决——分歧写在这里，而不是埋在实现对注释的偏离里 |

| 变异反证（两条都实测跑过） | 结果 |
| --- | --- |
| 计划别名收集器的 scratch 而不是 arena（把 `endPass` 的 arena 复制换回 `span(open_draws_.data(), size())`） | `TheHostsReusedCommandListIsCopiedAtTheCallNotHeld` 与 `OneViewportAndLightAnnouncementServesOneDrawingCall` 变红，其余 12 绿 |
| 公告不被绘制调用消费（去掉 `render()` 里的三行消费） | `OneViewportAndLightAnnouncementServesOneDrawingCall` 变红，其余 13 绿 |

| 结论 | 内容 |
| --- | --- |
| 计数只有一处 | 进入计划的 pass 与收集到的绘制调用记在 `Observe`（相位门禁读的那一份）；被拒/被丢弃的次数记在 `Protocol`。录制器**不留**自己的账，同一件事两个计数器就是同一件事两个谎言 |
| 与计数聚合一致 | 被拒的绘制**不**计入 `draws`：用例断言“两次无 scope 绘制 ⇒ `refusalCount()==2` 而 `draws==0`” |
| 边界没被偷偷扩大 | 录制器只答“这次调用合法吗、它记录成什么数据”：不判 rebuild / resize / borrow / 顺序 / 缓存。编译器需要的一切都是描述里的**事实**（这条是 M3d-2 的入口条件） |
| 证据 | `test_vsg` 全量 **462 用例 / 73 套件全绿**（+14）；插件目标 `gfx_backend_vsg` 同样编过（GLOB 收进新 core 文件，插件与测试编的是同一份源）；hygiene **0 findings / 775 文件** |

### 11.16d M3d-2（2026-09-21）：帧的编译段（`core/FrameGraph` + `core/FrameCompiler`，无设备）

| 文件 | 是什么 |
| --- | --- |
| `core/FrameGraph.hpp` / `src/core/FrameGraph.cpp` | 依赖图 + **稳定**拓扑序（`(order, 调用序)` 最小者先）+ Tarjan（**迭代版**：pass 数是宿主的数字，不该上 C++ 调用栈）找强连通分量 |
| `core/FrameCompiler.hpp` / `src/core/FrameCompiler.cpp` | normalize（视口/程序/动态层/清屏+bootstrap）→ `planTarget` / `depthPlan` → 建边 → `Schedule` → `CompiledFrame`；输入是描述 + `FrameFacts`（api 层自己的 target 账） |
| `core/Keys` | 新增 `resolveDynamicState`：**深度意图的唯一裁决点**（显式 StateNode 赢、否则跟 pass），其余动态项只有一个来源 |
| `core/Observe` | 新增 `invalid_schedules` |
| `tests/test_vsg/FrameGraphTest.cpp` / `FrameCompilerTest.cpp` | 10 + 12 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **顺序是依赖图的答案，不是调用顺序** | 稳定 = 在所有可跑者里取 `(公告 order, 调用序)` 最小者。公告顺序是画面叠放的决定（契约原文 "ascending pass order"、同序保持注册序），一个忽略它的拓扑排序会静默把画面重新叠一遍 |
| **环 = 跳过整个强连通分量** | 三条失败语义各自落地：① 上报 Error（列出分量里的 pass，**每分量一次/每帧**）；② 分量成员不进计划（不是"按当前顺序偷偷画"，是不画）；③ 分量外的 pass 照旧，帧照旧提交呈现；计数器 `invalid_schedules` **按分量 +1**（两个独立环是两个问题），自检断言恒 0 |
| **只读一个被跳过 pass 的 pass 仍然要跑** | 它的输入本帧没人生产——这正是契约里本来就存在的状态（`setPassInputs` 的 null）。把消费者一起停掉会让一个两 pass 的环吞掉整帧 |
| **边只有两种** | 采样边（输入目标的生产者 = 本帧写入它的 pass）+ 深度借用边（出借目标的生产者）。**自我边不产生**：pass 采样自己画的目标是 feedback 模式（执行器判定，见 M2c 的 source==destination 拒绝），调度器把它当环会静默跳过每一个这样的 pass |
| **bootstrap = 该目标的第一个写者** | 写 = 有绘制或宣布了 clear；且目标的 attachments 本帧新建/换镜像（`Rebuild` / `ResizeInPlace` / `Repair(Bootstrap)`）。后续写者 LOAD——在那里再来一次清屏会把第一个写者画的东西擦掉 |
| **0×0 目标不是错误** | 没有任何东西能画进去 ⇒ 它的 pass 不进计划，**也不上报**（这是状态，不是错；目标层的构建失败另有报告）。用例断言 `diagnostics.clean()` |
| **未知目标不静默** | 后端从没听说过的 target ⇒ 不进计划 + `ContentSkipped`(Error)。用一个空的 compatibility 或未建的 framebuffer 顶过去就是静默跳过 |
| **缺省全部解析** | 视口（没公告 = 整目标，逐 draw 落定）、程序（命令没有自己的 = 帧的 default，**在编译期替换**）、动态层（`resolveDynamicState`）、清屏（有效 policy + `bootstrap` / `depth_preserved` 两个**执行器推不出来**的事实） |

| 有意偏差（写在这里，不埋在实现里） | 内容 |
| --- | --- |
| `CompiledPass::compatibility` 没有 materialize | §5 草图里它是一份 `RenderPassCompatibility`，而该类型带 `std::vector` ⇒ 逐 pass 复制等于每帧每 pass 一次堆分配，而执行器本就按 target 身份持有同一份 shape。计划改为 `target_index` 指向 `CompiledFrame::targets`，兼容性由执行器按身份取。**回报**：整个计划里**零 vector**（§7 的“稳态帧零分配”从 M3d-3 起就成立） |
| 逐 draw 的 compare op 仍未进动态层 | `core::DynamicState` 没有 compare 字段；M2c 的 `ContentPipeline` 按 §11.11 把 `GREATER` 烘进管线（引擎 reverse-Z 约定），而**旧实现**用 `RenderStateMapper::mapCompareOp` 按 `DepthState.compare` 逐 draw 映射。⇒ 内容自定比较算子的画面，新旧后端的答案不同。**登记为待办**（把 compare 加进 `DynamicState` 要动 `Keys.hpp` 的审计表，是独立小批次） |

| 落地抓到的坑 | 内容 |
| --- | --- |
| 用例当场抓到真 bug | 不可服务的 pass（未知目标 / 0×0）只标了 `servable_=0`，**没有** `graph_.exclude()` ⇒ 它仍被排进 schedule，解析阶段用 `kNoTarget` 去索引 target 表（`stl_vector.h` 的越界断言当场炸）。修法：两处都 exclude，并在解析循环里留一条"不可达但把前置条件放在本地"的守卫 |
| 变异反证 ×2（都实测） | ① 不认环（`cyclic = false`）⇒ **5 个用例红**（图 3 + 编译 2），其余 17 绿；② `freshAttachments` 恒 false ⇒ bootstrap 那条红 |

| 结论 | 内容 |
| --- | --- |
| 与计数聚合一致 | `invalid_schedules` 是被跳过**分量**的数；被跳过的 pass 不进 `CompiledFrame::passes`，而 `counters().passes` 记的是收集到的 scope——两个数的差就是本帧丢掉的 pass，相位可断言 |
| 边界没被扩大 | 编译器不碰任何 API 类型（事实是纯值、计划是纯值），不建资源、不选 GPU 对象；`planTarget` / `depthPlan` 的答案照读，不自造策略 |
| 证据 | `test_vsg` 全量 **484 用例 / 75 套件全绿**（+22）；插件目标 `gfx_backend_vsg` 编过；hygiene **0 findings / 781 文件** |

### 11.16e M3d-3a（2026-09-21）：执行段的第一片（`api/VsgExecutor`，真设备、无窗口）

M3d-3 按“**能证明什么**”再拆：**M3d-3a 记录顺序 + 一个 pass scope = 一个 render pass**（本节，已完）、
M3d-3b 内容绘制（几何/块/描述符经计划进 render graph，`ContentDraw` 接进来）、M3d-3c 会话侧（窗口 pass、
`present`、帧计数与退役推进）。理由：**记录顺序是整个三段式拆分存在的理由**，而它恰好可以用清屏色证明——
不需要先有内容路径。

| 文件 | 是什么 |
| --- | --- |
| `api/VsgExecutor.hpp` / `src/api/VsgExecutor.cpp` | 逐 pass 走 `CompiledFrame`：按 `target_index` 解析 target → 取该 pass 的 render graph（带自己的清屏值）→ 按计划顺序挂进 command graph；所有 target 的 capture 节点**最后**加；不服务的 pass 报告而不是画到别处 |
| `api/OffscreenTarget` | 新增 `passGraph(policy)`：在**同一个** render pass + framebuffer 上造一张带本 pass 清屏值的 `RenderGraph`；清屏值填充抽成与构造函数共用的 `fillClearValues`（一处规则，两处使用） |
| `tests/test_vsg/ExecutorTest.cpp` | 2 个真设备用例（lavapipe 无窗口；否则 SKIP） |

| 规则 | 结论 |
| --- | --- |
| **一个 pass scope = 一个 render pass 实例** | vsg 的一 `RenderGraph` 就是一次 begin/end render pass。清屏**值**属于 pass，render pass / framebuffer / load op 属于 target ⇒ 同一 target 上的两个 pass 是两张图，各清各的颜色，后录的那张胜出 |
| **记录顺序 = 计划顺序（像素可查）** | 用例故意让两个 target 的“公告序”与“调度序”相反：A=红(10)/ 绿(0) ⇒ 期望**红**；B=绿(10)/ 红(0) ⇒ 期望**绿**。(A=红, B=绿) 这对组合只有走 schedule 才可能，“按调用序录”只会得到 (绿, 绿) |
| **记录序列也是证据** | `recorded()` 断言 {2,4,1,3}（调度序），而不只是“四个都录了”：失败时能看出是哪一级动了 |
| **capture 最后加** | 探针要读“最后一个写该 target 的 pass”留下的画面；夹在两个 pass 之间的拷贝读到的不是本帧的终态 |
| **不服务的 pass 必须响** | 默认 framebuffer（窗口 pass 未接）、未注册的 target、无附件的 target ⇒ `Warning`/`ContentSkipped` + `skipped()` 计数 + `record()` 返回 false（这是部分帧，调用方要知道）。默认 framebuffer 那句还会点名“这一层还没接”，而不是把内容画到别的 target 上 |

| 落地抓到的 | 内容 |
| --- | --- |
| **变异反证（端到端）** | 让调度器忽略公告 order（`ready.emplace(0, node)`）⇒ `FrameGraphTest` 的排序用例与 **`ExecutorTest` 的像素用例**同时变红，其余 22 绿。⇒ 那条像素断言测的是“**画面跟着 schedule 走**”，不是“图能提交” |
| 工具 | `GTEST_SKIP() << vine::String` 编不过（u8 串进不了 gtest 的消息流）：要 `.as_std_str()` |

| 结论 | 内容 |
| --- | --- |
| 边界 | 执行器只把计划变成 API 对象：不判 rebuild / resize / 顺序 / 缓存，也不挑管线或描述符（那两样在 M3d-3b 接内容时进来，但也仍是“按计划里的身份取对象”） |
| 与旧实现解耦 | 执行器不认识 `SceneBridge`：它按 **identity** 解析 target，与 api 层其余部分（`Session` / `OffscreenTarget`）用同一张表 |
| 清屏值这一半已到位 | M3a 的 load-op 变体有两半：**清屏值**（本节已能逐 pass 变化）与 **LOAD/CLEAR 操作**（仍归 target：镜像每帧 UNDEFINED ⇒ 必须清）。后者要等“带历史的 target”（跨帧保留 / 深度保留的 LOAD 路径） |
| 证据 | `test_vsg` 全量 **486 用例 / 76 套件全绿**（+2，含真设备）；插件目标 `gfx_backend_vsg` 编过；hygiene **0 findings / 784 文件** |

### 11.16f M3d-3b-0（2026-09-21）：计划带上“管线身份要的 pass 侧事实”，执行器核对计划与资源世界

M3d-3b（内容绘制进 render graph）开工前先把它的**两处前提**钉住：

1. 一个 `PipelineKey` 里有两项**只能从 pass/target 得出**：写了几个颜色附件（`color_attachments`）与目标是否提供可采样深度
   （`depth_sampleable`）；它们现在由编译器从**已经查到的同一份 target 事实**解析进 `CompiledPass`，调用方不必再问一次。
2. M3d-2 记下的偏差（“不 materialize `RenderPassCompatibility`，因为带 vector”）需要一个**按身份取兼容性的口子**：
   `OffscreenTarget::shape()` 返回它建 render pass 用的那份形状（返回副本，无借用），执行器由此命名 key 的兼容性半边。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler` | `CompiledPass::color_attachments` / `depth_sampleable`；按 slot 缓存附件数，避免逐 pass 重算 |
| `api/OffscreenTarget` | `shape()`（返回 `core::TargetShape`） |
| `api/VsgExecutor` | 记录一个 pass 前**核对**计划与 target：附件数或深度可采样不一致 ⇒ `Warning`/`ContentSkipped` + 跳过 + `record()` 返回 false |
| `tests/test_vsg` | 编译器 +1（无设备，含“保留的深度撤销提升”那一面）；执行器 +1（真设备：故意让事实漂移到两个附件） |

| 规则 | 结论 |
| --- | --- |
| **同一个事实只答一次** | 两项都从编译器已经拿来建 target 表的事实里解析，所以“计划说的”与“执行器要的”不可能给出两个答案 |
| **计划与资源世界必须对得上** | 计划描述的形状与它解析到的 target 不一致 = 上游事实漂了，而**按错形状建的管线与宿主要的画面没有任何关系**。这个不一致只在执行器这一处能被发现，所以在那里报，而不是交给驱动 |
| 证据 | `test_vsg` 全量 **488 用例 / 76 套件全绿**（+2）；插件目标编过；hygiene 0 / 784 文件 |

### 11.16g M3d-3b-1（2026-09-21）：内容“按 pass 放置”，状态全部来自计划

内容世界（管线 / 流 / 块 / 材质）属于拥有它的那一层，不属于执行器。所以这一片的接缝是**数据而不是接口**：
调用方把已录制好的内容按 pass 交给执行器（`PassContent{ pass, content }`），执行器只回答一个问题——
**它进的是不是计划说的那个 pass**。这样既不需要在 api 里提前发明一套“内容工厂”抽象，又让“记录顺序 / 清屏值 /
视口由计划决定”这些主张保持可验证。

| 文件 | 是什么 |
| --- | --- |
| `api/VsgExecutor` | `PassContent` + `record(frame, graph, content)`：匹配同一个 `PassId` 的内容作为该 pass render graph 的子节点；**计划里没有的 pass 的内容会被报**（它的 pass 可能因为环被跳过、或 target 不可服务） |
| `tests/test_vsg/ExecutorTest.cpp` | 新增真设备用例：一个 pass + 一个 render command，内容层用 `ContentDraw`／`StreamUploads`／`BlockStorage`／`BlockDescriptors` 录一个三角形，**动态状态与视口都取自 `CompiledPass`** |

| 规则 | 结论 |
| --- | --- |
| **内容只需进对 pass** | 执行器不看内容内部（里面有哪些 draw、绑了什么，是内容层的事），只保证它在计划的那个 pass 里、在 plan 的 clear 之后 |
| **状态来自计划，而不是内容层重新决定** | 用例把 `pass.draws[0].commands[0].dynamic` 与 `pass.viewport` 直接交给 `ContentDraw`——这就是 M3d-2 把 normalize 做在编译段的意义：内容层不再需要知道"这个 pass 的深度策略是什么" |
| **内容静默丢掉是不行的** | 给一个计划里不存在的 pass 的内容 ⇒ `Warning`/`ContentSkipped` + `skipped()` 计数。那一 pass 缺席是有原因的（环被跳过、target 不可服务），而原因已经在别处报过；这里只报“你录的东西没地方放” |
| **变异反证** | 把 `graph->addChild(packet.content)` 拿掉 ⇒ 像素用例红，而且失败消息直接给出真相：中心读到 `(64, 128, 191)` = **计划那个 clear 色**——即“背景是计划的清屏，三角形没进去” |
| 证据 | `test_vsg` 全量 **489 用例 / 76 套件全绿**（+1，真设备）；插件目标编过；hygiene 0 / 784 文件 |

| 本片**没有**做（下一步的前提） | 内容层今天在测试里手录几何。生产侧需要三张事实表（程序身份 → GLSL、几何身份 + revision → 通道流/索引、材质身份 → 块字节），它们现在只存在于旧实现的 `SceneBridge`／`VsgMaterialManager` 里 |

### 11.16h M3d-3b-2（2026-09-21）：内容层的三张事实表（`api/ContentFacts`，无设备）

计划只能按**身份**命名程序/几何/材质（每帧每个命令只复制得起这个，也只应该复制这个）；
“这个身份是哪些顶点流”“这个材质是哪些字节”是**内容**，不是帧意图，所以由拥有它们的层从自己的表里回答。
这一片把这三张表与它们的**查找语义**钉住（数据源还在旧实现的 `SceneBridge`／`VsgMaterialManager` 里，那是下一片）。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentFacts.hpp` / `src/api/ContentFacts.cpp` | `ChannelFacts` / `GeometryFacts` / `ProgramFacts` / `MaterialFacts` + `ContentFacts`（三张表，借用一帧）+ 三个查找 + 两条规则函数 |
| `tests/test_vsg/ContentFactsTest.cpp` | 5 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **身份 + revision 是键** | 三个查找都按这两个值回答。revision 不同**不是**“旧的也能用”：计划描述的是内容已经往前走之后的画面，拿新字节去录就是一张与帧无关的图 |
| **miss 有三种，都是“不能录”但不是同一件事** | `Unknown`（这张表不认识这个身份）/ `Revision`（身份在、修订不同）/ `Malformed`（条目在但不能画）。三者决定相同（跳过 + 报），但消息与排查路径不同——这正是“一次报清”的前提 |
| **几何的 Malformed 是“通道与布局对不上”** | `popcount(canonical_mask) + custom_locations.size() == channels.size()`，且索引流必须是 `Index`。规则放在**查找里**，内容层就绕不过去：声明了没人喂的属性是驱动拒编或未绑定内存读，多出来的流是数据没人消费 |
| **材质的 Malformed 是“块不是 ABI 的大小”** | `sizeof(VineMaterialBlock)`。字节数错了会让着色器读到别的块（与 M2c-2b-1 “对齐即拒绝”同一条纪律） |
| **空表答 Unknown，不猜** | 表里没有就是没有：一个“缺省材质”或“缺省程序”的静默代替品会让内容错得无从归因（与旧实现“没有有效 shader 就不画”同一条口径） |

| 变异反证 | 把三个 revision 检查改成“照样回答”⇒ 三条 revision 用例变红、其余 2 条绿 |
| --- | --- |
| 证据 | `test_vsg` 全量 **494 用例 / 77 套件全绿**（+5）；插件目标编过；hygiene 0 / 787 文件 |

### 11.16i M3d-3b-3（2026-09-21）：每绘制 ABI 块的打包（`api/DrawBlock`，无设备）

内容层把计划变成 GPU 对象时，第一处“算术”就是每绘制的块（`VineDrawBlock`）。三件事会静默错：矩阵的列主序、
平移在平铺数组的 12..14、不透明度在 `params.x`。所以打包只允许有一处，并用一条**非对称矩阵**的用例钉住它——
对称矩阵（单位阵、缩放、大多数测试内容）的转置打包与正确打包逐字节相同，只会在以后表现为“模型转错方向”。

| 文件 | 是什么 |
| --- | --- |
| `api/DrawBlock.hpp` / `src/api/DrawBlock.cpp` | `packDrawBlock(const CompiledCommand&, VineDrawBlock&)`：从访问器逐元素写（不抄内存），把列主序写成**函数自己的注释与用例**；`params = {opacity, 0, 0, 0}` |
| `tests/test_vsg/DrawBlockTest.cpp` | 2 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **列主序写成 `column * 4 + row`** | 元素 (row, column) 落到平铺数组的哪个位置，是这份 ABI 与着色器之间的契约；从访问器逐元素写而不是复制存储，让“用的是哪种约定”在读过的地方直接可见 |
| **平移在 12..14** | 同一个事实从字节侧再看一遍（第四列），也是“没有转置”的第二个证据 |
| **`params.yzw` 是保留槽，打包为零** | “看起来应该有地方放”的值就是每绘制参数悄悄失效的入口 |
| **值真的从计划来** | 第二个用例跑完整条路（recorder → compiler → packer），从平铺数组里读出平移：这是“屏幕上的数来自宿主那个模型矩阵”的声明 |
| **视图块暂时不做** | `VineViewBlock` 的 ABI 已钉（`view` / `inv_view` / `proj` / `view_proj` / `cam_pos` / `frame`），但 `frame`（时间 / 视口尺寸）与 `cam_pos.w` 需要**会话侧**的约定，而那里才是这两个值得出处；在此发明就是给同一个问题第二个答案 |

| 变异反证 | 把索引改成行主序 ⇒ **两条用例都红**（包括专门为它准备的非对称矩阵那条） |
| --- | --- |
| 证据 | `test_vsg` 全量 **496 用例 / 78 套件全绿**（+2）；插件目标编过；hygiene 0 / 790 文件 |

### 11.16j M3d-3b-4（2026-09-21）：几何的通道走查（`api/GeometryFacts`，无设备）

三张表的第一张真实数据源：把 SDK 的 `Geometry`（宿主 authored 的对象）变成 `GeometryFacts`。规则都是
“错了也看不出来”的那种，所以逐条钉：

| 文件 | 是什么 |
| --- | --- |
| `api/GeometryFacts.hpp` / `src/api/GeometryFacts.cpp` | `buildGeometryFacts(geometry, out, storage)`：按固定顺序走 `bufferLocations()`，逐通道建 `StreamKey` + `vsg::floatArray`，再建索引流 |
| `tests/test_vsg/GeometryFactsTest.cpp` | 5 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **通道顺序就是顶点绑定顺序** | canonical 角色（位置 0 / 法线 1 / 颜色 2 / 保留 UV 8）在前，自定义 location 升序在后。管线属性声明从同一份 entry 建，所以“哪个缓冲喂哪个属性”不可能两边不一致 |
| **索引流归一化为整个 buffer** | 几何声明的是一段**切片**，但流的身份是（buffer, revision）——切片随 draw 走（`index_count` / `first_index`）。这就是同一 index arena 上的两个几何只上传一次的原因；按切片建键会把同一段字节按几何数上传 |
| **切片通道上传自己的段** | 通道读共享缓冲的一段，上传的就是那一段；索引相对几何自己的顶点（SDK 自己这么说）⇒ `vertex_offset = 0` |
| **同一起点的规则** | 一个几何的所有通道必须从**同一个顶点**起算（`offset / components` 一致），否则“索引 0”对每个通道指的是不同顶点，索引就没有单一含义 ⇒ Malformed |
| **不能画就是不能画** | 没有任何顶点缓冲 ⇒ `Unknown`；没有位置（唯一必需的属性）或没有索引（本后端的绘制是 indexed）⇒ `Malformed`。**自定义通道（≥ 3 且 ≠ 8）不被拒绝**：照原样描述，能力上限由上传层说出——在这里丢掉它会让宿主 authored 的几何看起来像另一个它没写过的几何 |

| 落地抓到的 | 内容 |
| --- | --- |
| 用例当场抓到真 bug | 建好的 `storage` **没有发布成 `out.channels`**（entry 的 span 全空）⇒ 三条用例当场红。这正是“表是借用的、发布是显式一步”的价值 |
| 变异反证 | ① 通道顺序改回“几何自己报的顺序” ⇒ 顺序用例红；② 索引键改成按切片（`count = indexCount()`）⇒ 两条分享/索引用例红。其余用例保持绿 |
| 证据 | `test_vsg` 全量 **501 用例 / 79 套件全绿**（+5）；插件目标编过；hygiene 0 / 793 文件 |

### 11.16k M3d-3b-5（2026-09-21）：程序与材质两张数据源（`api/ContentSources`，无设备）

| 文件 | 是什么 |
| --- | --- |
| `api/ContentSources.hpp` / `src/api/ContentSources.cpp` | `buildProgramFacts`（`ShaderProgram` → 两段 GLSL + 入口点）与 `buildMaterialFacts`（`Material` → ABI 块字节；`nullptr` → 默认材质） |
| `api/ContentFacts` | `findMaterial` **不再特判 nullptr**：默认材质是一张表里的普通条目，而不是查找失败——决定“拒绝还是回退”留在内容层 |
| `tests/test_vsg/ContentSourcesTest.cpp` | 4 个无设备用例 |

| 规则 | 结论 |
| --- | --- |
| **一个内容管线 = 恰好两段图形阶段 + 一个入口点** | 多余的同种阶段 / compute 阶段 / 空源 / 两段入口点不一致 ⇒ `Malformed`。“取每种的第一个”会编出宿主没写过的程序，而画面上什么也不会说 |
| **没有材质不等于查找失败** | SDK 的材质是可选的，而 ABI 自己的默认值**就是**默认材质；所以 `nullptr` 是一条**身份为空的普通条目**。表里有它就答得上，没有就答 `Unknown`——是否回退由内容层定，不由查找定 |
| **材质的修订由调用方给** | `Material` 没有 revision 访问器（M3d-1 已记）；知道材质何时被改的是材质管理器，所以修订作为参数传入（与几何同一条规则：修订是计划要报的事实，只有它的拥有者能报） |
| **块的成员才是载荷** | `VineMaterialBlock` 是聚合体，`{}` 只初始化**成员**（这是 ABI 的默认材质：灰、只有一个非零 shininess）而**不碰尾部填充**（shininess 在 48..51，块 64 字节）。填充不被任何着色器读；比较要用 ABI 自己的逐成员 `operator==`，字节级比较不是有意义的运算（它会把“没改过”报成“改过了”） |

| 落地抓到的 | 内容 |
| --- | --- |
| 一个值得记住的字节级事实 | 先写了“`{}` 会把对象（含填充）清零”的注释，又被诊断打脸：诊断打印出**差异字节正好是 52..63**（= `shininess` 之后的尾部填充），而逐成员 `operator==` 为真。注释与用例都已改成陈述真正的事实（这一条也已记进仓库记忆） |
| 变异反证 | ① 接受任意阶段组合 + 入口点不一致 ⇒ 程序规则用例红；② `findMaterial` 恢复 nullptr 特判 ⇒ 默认材质用例红；其余 7 绿 |
| 证据 | `test_vsg` 全量 **505 用例 / 80 套件全绿**（+4）；插件目标编过；hygiene 0 / 796 文件 |

### 11.16l M3d-3b-6（2026-09-21）：把三张表接进内容层（`api/ContentPass`，真设备）

内容层就是“身份变成字节”的那一层：几何的通道变成流、程序的阶段变成管线、材质的块字节变成描述符的载荷。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPass.hpp` / `src/api/ContentPass.cpp` | `Scope`（管线/录制器/注册表/块存储/描述符集/上传）+ `record(pass, facts, compatibility, view_block, out)`：逐命令 `findProgram/findGeometry/findMaterial` → 写块（`packDrawBlock` + 材质的 ABI 块）→ `acquireVertex/acquireIndex` → `ContentDraw::Draw` → 挂到组上 |
| `api/ContentFacts` | **纠正**：`findMaterial` 只按身份查（签名去掉 revision 参数）——计划根本报不出材质的修订（SDK 的 `Material` 没有访问器），所以“表里的条目”就是它此刻的字节；条目仍留 revision，因为块存储按它管理 in-flight 副本 |
| `tests/test_vsg/ContentPassTest.cpp` | 1 个真设备用例（含“第二帧复用”与“拒绝”两半） |

| 规则 | 结论 |
| --- | --- |
| **逐命令拒绝，不是逐 pass** | 程序/几何/材质任一查不到 ⇒ 该命令不画、按查找到的理由上报（未知 / 修订不同 / 畸形），**其余照画**。一个没法着色的 drawable 是画面上的一个洞，不是丢掉整帧的理由 |
| **一个 scope 一种顶点布局（已声明的限制，§11.16m 已取代）** | 本层驱动一套 `ContentPipeline` + `ContentDraw`，而管线是按顶点布局编译的；几何布局不同 ⇒ 拒绝并说出原因。“静默按错布局画”比拒绝更糟；多布局 scope（一布局一管线、共享变体池）是下一片 |
| **输入与全屏绘制调用未接** | 计划还没携带 pass 的输入（所以键里的 `sampled_color_count` 为 0），全屏绘制属于 program-slot 路径 ⇒ 两者都**拒绝**而不是近似 |
| **缓存的证据是计数，不是“画出来了”** | 同一内容第二帧：`uploads()` 不涨而 `aliases()` +2（顶点 + 索引两条流）、`materialWrites()` 不涨而 `materialHits()` +1——这正是“身份带修订而不是带字节”的价值 |

| 落地抓到的 | 内容 |
| --- | --- |
| 测试自己踩的坑（已修） | 用栈对象构造 `intrusive_ptr<Geometry>(&geometry)` ⇒ 析构时 `free(): invalid pointer`。SDK 对象必须堆上由 `intrusive_ptr` 拥有（与计划里的身份一致） |
| 变异反证 | ① 每次 acquire 用**移动的身份**（revision + 计数器）⇒ 缓存断言红（`uploads` 3≠2、`aliases` 1≠2）；② 忽略几何 miss ⇒ 进程崩在空指针（那道检查正是它在防的事）。头两次尝试的变异都是无效变异（`+bound` 恒为 0；`+1` 两帧一致），**变异本身也要验证**——这一条已记进仓库记忆 |
| 证据 | `test_vsg` 全量 **506 用例 / 81 套件全绿**（+1，真设备）；插件目标编过；hygiene 0 / 799 文件 |

### 11.16m M3d-3b-7（2026-09-21）：多布局 scope（`api/ContentPass`，真设备）

一片管线层 = 一个程序的阶段 × 一种顶点布局，所以一个 pass 要画两种布局的网格就需要两片。scope 现在是一**组**“编译好的半片”，逐命令按 (program, revision, layout) 选；所有半片共享池与 pass 的注册表。

| 文件 | 是什么 |
| --- | --- |
| `api/ContentPass` | `Scope::Entry`（program / revision / layout / pipelines / draws）+ `Scope::entries`（span）；ctor 不再收布局；逐命令选半片，选不中时按“哪一项没对上”给两条不同消息 |
| `tests/test_vsg/ContentPassTest.cpp` | 新增真设备用例（两半片 + 两次拒绝），并把既有用例迁到新 scope 形状 |

| 规则 | 结论 |
| --- | --- |
| **半片的身份 = (程序, 修订, 布局)** | 管线层按一份阶段文本 + 一种布局建；借别人的阶段画就是画出没人写过的画面，而池会把那份对象记在计划点名的键上（静默）⇒ 三者都得对上 |
| **选不中要说清是哪一项** | 两种 miss 的修法不同（去编译那个程序 / 去编译那种布局）⇒ 消息各说自己那一项；用例用 sink 收消息逐字钉住 |
| **池与注册表共享是结构性的** | `Scope` 只有一个 registry 指针 ⇒ “半片一个注册表”这种错误安排**表达不出来**；共享的语义价值 = 换过半片后回到前一片要**重发管线绑定**，证据 = 左半片 `pipeline_binds()==2`（三次绘制：左、右、左） |

| 落地抓到的 | 内容 |
| --- | --- |
| 变异反证 | ① 选半片忽略布局 ⇒ 未服务的布局被画（`record` 真、零消息）红；② 忽略程序身份 ⇒ 未编译的程序被画，红；③ 把注册表改成“每个录制器一份”（临时给 `ContentDraw` 加成员）⇒ 第三次绘制跳过绑定（1≠2）**且左网格被蓝管线圈画成 (0,0,255)** —— 计数与像素同时抓到 |
| 测试自己踩的坑 | 第二帧的 `beginFrame` 被协议拒绝：**关帧的是 `swapBuffers()`**（`endFrame()` 只离开 pass，回 `Idle` 靠 swap）⇒ 症状是“第二帧的计划还是第一帧的三条命令”，靠打印 token 抓到 |
| 证据 | `test_vsg` 全量 **507 用例 / 81 套件全绿**（+1）；插件目标编过；hygiene 0 / 799 文件 |

### 11.16n M3d-3b-8（2026-09-21）：pass 输入的采样绑定（计划携带输入表 + 内容层绑定，真设备）

§11.16l 留下的第一个缺口：`setPassInputs` 声明的输入到不了着色——计划不携带它们，键里的
`sampled_color_count` 恒为 0，描述符里也就没有采样的纹理。这一片把那条线接起来，分成三件事：

1. **计划携带输入**（`core/FrameCompiler`）：`CompiledPass::inputs` = `CompiledInput{ target, color_attachments }`
   的有序表，**从与 pass 目标同一份 target 事实**里解析（同一个事实只答一次：调用方重新从 target 对象推一遍
   就可能与计划给出两个答案）；空输入（本帧无人产出）留在表里、报 0；**非空但事实答不上来的输入上报一次**
   （此时什么也绑不了，静默按"没有这个输入"着色更糟）——注意 pass 本身照跑：目标查不到是"没地方画"，
   输入查不到只是"这一项绑不上"。
2. **采样集的布局进管线身份**（`api/ContentPipeline`）：键里的 `sampled_color_count` 是**身份**而不是运行时
   状态（管线是按一份描述符集布局编译的），所以布局层按计数建 **set 1**（每项一个 combined image sampler，
   顶点/片元两阶段都可读）**并按计数缓存**；`layoutFor(count)` 给出与之一体的管线布局，`sampledSetLayout(count)`
   给内容层用来建**同一个**布局对象的 set；`inputSampler()` 一层一个采样器（默认线性）。
3. **内容层绑定**（`api/ContentPass`）：调用方按计划顺序逐个输入交出图像（`InputImages`，
   `OffscreenTarget::colorView(i)` 就是答案），内容层核对**每一项的彩色数**与计划一致后才建 set；
   不一致 ⇒ **整趟 pass 拒绝**并说出是哪一项动了（输入是 pass 的**一个**事实，不存在"逐命令近似"）；
   计数进键（`sampled_color_count`），set 进每一次绘制的状态组。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler` | `CompiledInput` + `CompiledPass::inputs`（声明序、解析出彩色数）+ 答不上来的输入上报；文件头把"输入是什么"列进它决定的事 |
| `core/StateRegistry` | 第三个"仅当"：采样集按**集合身份**去重（`resolve(key, state, inputs)` + `inputs_issued/skipped`），因为输入是 pass 的属性 ⇒ 一趟 pass 一条绑定命令，不是一绘制一条 |
| `api/ContentPipeline` | 按 `sampled_color_count` 现建并保留的 set 1 布局 + 配套管线布局（`layoutFor/sampledSetLayout/inputSampler`）；`acquire()` 用键的计数选布局 |
| `api/ContentDraw` | `Draw::inputs`（采样集绑定）+ `input_binds()` 计数；绑定排在块集之后（set 0 → set 1） |
| `api/ContentPass` | `InputImages` + 计划/资源世界核对 + `makeInputSet`（一次 pass 一个 set）+ 键的计数 |
| `api/OffscreenTarget` | 彩色附件改为**按可采样收尾**（`SAMPLED` usage + render pass 的 `finalLayout = SHADER_READ_ONLY`），`capture()` 自带一对 layout 转移，新增 `colorView(i)` |
| `tests/test_vsg/SampledInputTest.cpp` | 1 个真设备用例（像素 + 拒绝两半）；`FrameCompilerTest` +2、`VariantCoreTest` +1、`ContentPipelineTest` +1 全无设备 |

| 规则 | 结论 |
| --- | --- |
| **输入的粒度是 pass，不是绘制** | "pass 声明的输入"在契约里就是 pass 的属性（`setPassInputs` 每次绘制前重申同一份解析结果）⇒ 集合只建一次、只绑一次（`input_binds()==1` 对两个绘制调用），身份与绑定不可能各自漂移 |
| **计数是身份不是状态** | 管线编译绑定在一份描述符集布局上，所以"这趟 pass 绑几个采样纹理"必须进键；两个计数 ⇒ 两个布局 ⇒ 两个管线对象（同键再来仍是 `Reused`）。这也是键审计表里"采样附件数"那一项真正落地的位置 |
| **计划与资源世界必须对得上** | 计划说几项、每项几张，调用方给的图必须逐项相等；不等就**整趟拒绝**（与 M3d-3b-0 执行器核对 target 形状同一条纪律）。拒绝消息分两条：项数不对 / 第 i 项彩色数不对 |
| **彩色附件按可采样收尾** | 输入的图像必须真的处在 `SHADER_READ_ONLY` 且建了 `SAMPLED` usage，否则描述符在撒谎。旧实现是"彩色附件永远按 SHADER_READ_ONLY 收尾"；重写版原先为了免 barrier 读回改成 TRANSFER_SRC，这一片把彩色改回**采样收尾**（深度仍留在附件布局：它是下一趟 pass 的附件、也是阴影的纹理，另有机制），**代价是读回的拷贝自带一对转移**（拷贝仍是一条命令，只是前后各一个 image barrier） |
| **"没人产出"与"后端不认识"是两件事** | 空白输入（引擎解析成 null）留在表里、报 0 张、**不上报**——那一层的问题归引擎（它自己会报）；非空但 target 事实答不上来才由编译器报一次（后端自己的账缺了一页）。两种情形的共同答案都是"这一项绑不了东西"，所以内容层按 0 张核对，照画其余 |

### 11.16o M3d-3c（2026-09-21）：会话侧（窗口目标、一次 present、视图块，真设备）

执行段的第三片：把"窗口"变成执行器认识的目标，把"视图块"的四处约定钉在一处，把会话的帧时钟与
帧图接缝接上。三件事：

1. **窗口是目标，不是特例**（`api/WindowTarget`）：`prepare(clear)` 刷新 renderArea（跟随窗口**活的**
   尺寸）并按计划写清屏值，`graph()` 给出唯一那张 `RenderGraph`，`shape()/facts()` 用与离屏目标同一套
   词回答"你是几张彩色、深度是什么"。`facts()` 的 `target` 是 **nullptr** —— 这就是默认帧缓冲的身份，
   执行器用它把 pass 分派到窗口路径。
2. **一张图、一次清、一个稳定视图**（`api/WindowTarget` + `api/VsgExecutor::recordWindow`）：
   窗口的 framebuffer 是**记录时**按 `window->imageIndex()` 解的，而窗口的 render pass 的 load op 是
   vsg 的（平台定），所以**一帧里只能清一次**：计划顺序里第一个窗口 pass 拥有这次清（它自己的
   `clear` 策略），后面的窗口 pass 叠在它上面（`window_recorded_` 正是这条规则的开关）。
   窗口的内容挂在**一个** `vsg::View` 下，且这视图跨帧稳定：vsg 的 `GraphicsPipeline` 是**按 viewID**
   编译的（`GraphicsPipeline.cpp` 的 `_implementation[viewID]`），没有视图时所有 pass 共用 viewID 0，
   窗口的 pass 就可能复用到"按别人 render pass 编译"的管线；逐帧换视图则会让编译出的管线**泄漏**
   （`View::~View` 只放号，不回收号上的实现）。
3. **视图块**（`api/ViewBlock`）：`buildViewBlock(camera, time, width, height)` 把计划里的相机快照、
   会话的帧时钟与**目标**（不是 pass 子矩形）的尺寸装配成 ABI 块；`api/Session` 新增 `frameSeconds()`
   （会话自己开机的时刻为零点，**一帧内不变**），并给内容层两条帧图接缝（`makeFrameGraph` /
   `assignFrameGraphs`）。

| 文件 | 是什么 |
| --- | --- |
| `api/WindowTarget`（新） | 窗口作为目标：shape/facts/`prepare`/`addContent`/`graph` + `colorAttachmentCount/width/height/depthSampleable`；文件头写明"一张图、一次清、一个稳定视图"三条理由 |
| `api/ViewBlock`（新） | 视图块唯一装配处：列主序、`view_proj = proj * view`、`inv_view`、`cam_pos.w`/`frame.w` 保留为 0、**裁剪矩阵折进设备约定**、`frame = {时间, 目标宽, 目标高, 0}`、无相机 = 零矩阵 + 帧事实仍在 |
| `api/VsgExecutor` | `setWindow(WindowTarget*)` + `recordWindow`（计划/world 形状核对、首次 `prepare` + 挂图、逐包挂内容；不匹配就整趟跳过并上报）；离屏路径移进 `recordOffscreen` |
| `api/Session` | `Impl` 里窗口目标取代临时 render graph；帧时钟（`started_at`/`frame_seconds`）在 `beginFrame()` 采样；`frameSeconds()`；`makeFrameGraph`/`assignFrameGraphs`（后者换掉 viewer 的录/提交任务并紧接着编译） |
| `tests/test_vsg/ViewBlockTest.cpp`（新） | 4 个无设备用例（列主序 + 组合、**裁剪折叠**的算术、帧事实与保留位、无相机） |
| `tests/test_vsg/SessionContentTest.cpp` | +1 个真设备用例：计划驱动的整帧进窗口（清屏像素、视图块进片元着色、一次 present、timeline 推进、零设备等待） |

| 规则 | 结论 |
| --- | --- |
| **视图块的裁剪矩阵是设备的，不是 SDK 的** | SDK 的矩阵是 x 右、y 上、z∈[-1,1] 近端 -1；本后端的设备是 reverse-Z + y 向下 NDC（近→1、远→0、世界上=屏幕上）。**宿主程序写 `gl_Position = view_proj * model * pos` 时不该知道任何一条**，所以折叠（x/w 不变、y 取反、z 记作 `0.5 - 0.5*z`）发生在块装配处。`view`/`inv_view` 是视图空间矩阵，**不折**（视图空间的关照计算与裁剪约定无关） |
| **不折的失败形态（实测）** | 没折的矩阵把测试三角形放在裁剪 z = **-0.714**；窗口深度清 0.0、比较是 `GREATER` ⇒ 每个片元都被拒绝。画面只剩清屏色，而**绘制仍被记录、计划仍正确、0 VUID、0 诊断**——这正是 `SceneBridgePipeline` 文件头为手写 `gl_Position` 记下的陷阱，只是上了一层（矩阵而不是程序） |
| **镜像的深度映射画面看不出来** | 变异 M2（z 折反：近→0、远→1）让三角形落在深度 0.143，仍 `GREATER` 过测 ⇒ **像素断言全绿**，只有算术用例红。这就是"算术用例与像素用例各管一段"的实证：画面负责"这条约定能用"，算术负责"这条约定是对的" |
| **时间与尺寸的出处** | 时间来自 `Session::frameSeconds()`（会话开机为零点、一帧内所有 pass 同值）；尺寸是**目标**的（着色重建屏幕空间数据时用的那张图），不是 pass 的子矩形——子矩形是 viewport 命令的事 |
| **一帧一次 present** | `commitFrame()` 里 `recordAndSubmit()` → `present()` → 帧计数 +1、`timeline.submitted(token)`、`retirement.advance(timeline)` 的顺序就是"帧的账"。像素用例在**第一帧**之后立刻断言 `framesPresented()==1`、`submittedFrame()==1`、`deviceWaits()==0`，再空转两帧只为**让呈现落地**（显示服务器的拷贝是异步的，第一帧刚 present 就读到的可能还是窗口的旧后备存储——另一个用例一直呈现三帧就是这个原因） |
| **变异反证（五条已验，一条登记）** | M1 折掉 ⇒ 3 用例红（2 算术 + 像素的 4 条断言）；M2 z 折反 ⇒ 2 算术红、像素**绿**（见上）；M3 y 不折 ⇒ 2 算术 + 朝向像素红；M4 `view` 也折 ⇒ 2 算术红（像素读不到 `view`）；M6 不调 `prepare` ⇒ 清屏像素红且实测露出 vsg 的默认清屏色 **(102,51,51)**（"画面里的清屏色确实是计划的"由此有了反证）。M1–M4 的容差特意收紧到 ±6：0.25 的 sRGB 像（137）与 0.5 的线性像（128）只差 9，容差一松，**没画出来的像素**就能替着色答"cam_pos 到了"。M5（`prepare` 不刷 renderArea）**在本片的夹具里不可观测**：窗口尺寸没变过，创建时的矩形与活的尺寸相同——它的可观测形态要一个**活改尺寸**的窗口用例（登记到下面的口子） |

**本片留下的口子（登记，不假装解决）**：

* **多次窗口 pass 的"一次清"没有像素证据**（**已关，§11.16r**：M4c 的夹具里第④趟窗口 pass 公告了另一种清屏色，
  背景仍是第③趟的颜色、公告的那个色**不出现**）；本片用例只有一趟窗口 pass，`window_recorded_` 那条规则
  要等 M4（全屏/screen pass 会往同一张图上叠第二趟）才有可观测的形态；
* **稳定视图的反证（与离屏 pass 共用管线键）也没落地**：要看到"窗口管线被按别人 render pass 编译"的
  后果，得让同一帧里有一趟离屏 pass 与窗口 pass **同键**（同程序/几何/兼容性）；这是 M4 的用例形态
  （共享键 + 采样输入正是全屏 pass 的日常）。**已关，且结论是修正**（§11.16r）：同键的后果实测到了
  （`VUID-vkCmdDrawIndexed-renderPass-02684`），但稳定视图**不是**解法——vsg 的复用循环只比 pipeline states，
  两个视图的状态集合相同时照样共用一份实现；解法是键带上**设备格式**（两个变体）；
* **逐帧新建 `CommandGraph`/内容节点**：`makeFrameGraph` 现在每帧可以给一张新图，内容节点也每帧重建
  （保留 + 停放的优化留给后面）；
* **采样输入的描述符集逐帧重建**（M3d-3b-8 的既有形态）依旧成立；
* **第二个渲染通道家族出现时可能需要逐 pass 视图**（现在是"一个视图管整个窗口"）——**已实测给出答案**
  （§11.16r）：家族之分靠**键**（设备格式），不靠视图；视图只在需要**逐视图状态覆盖**时才有用，本后端不覆盖
  任何状态，所以"一个视图管整个窗口"留着；
* **活改尺寸的窗口用例**：`prepare` 里"renderArea 跟随活的尺寸"那行现在只有代码与理由，没有可观测证据（M5 不可观测的原因）；它需要把宿主窗口在帧间改尺寸、再读新区域的像素。

### 11.16p M4a（2026-09-21）：全屏绘制调用（身份 + 层 + 事实 + 录制，真设备像素）

**为什么先做这一片。** 引擎只有两种绘制调用：内容绘制（`render()`）与全屏绘制（`drawScreenProgram()`）。
M3d-3b-8 把采样线（"源附件 i → binding i"）建在内容路径上，而**全屏路径是这条线的另一个消费者**：它的
源就是它的采样集。但两者**不是同一套描述符 ABI**：内容层是"块在 set 0、采样输入在 set 1"，而引擎自己
的全屏程序（`BuiltinShaders::screenCopyProgram` / `deferredLightProgram`，随 SDK 一起发布的 GLSL）声明的是
`layout(binding = i) uniform sampler2D`（**没有 set 限定词 ⇒ set 0**）与 `layout(push_constant)`。§1.3 的
"被迫同形"清单早就写了这一条（"128B push block 承载全屏路径的光照+视图"），所以这一片**照做**：

| | 内容 ABI | 全屏 ABI |
| --- | --- | --- |
| set 0 | 四个块（view/draw/material，动态偏移） | 源彩色附件（binding i = 附件 i） |
| set 1 | 采样输入（同一条采样线） | 无 |
| 顶点 | 几何流 + 索引流 | 无：`gl_VertexIndex` 生成的三角形 |
| push | 128B（顶点阶段） | 128B（**片元**阶段：引擎的全屏顶点阶段不声明常量） |

落地四件事：

1. **身份**（`core/Keys`）：`DrawKind` 从计划搬到键的旁边（一处拼写），`PipelineKey.kind` 进
   `operator==`/哈希/**键审计表**——两套 ABI 编译出的管线互不可绑，键不携带它就可能把一份错 ABI 的管线
   发给绘制。
2. **层**（`api/ContentPipeline`）：`createScreen` 建一个全屏层（无块集、无顶点流、set 0 按采样数现建、
   push 是片元阶段的 128B）；`kind()` 自报；`acquire` **拒绝另一种 kind 的键**（计数 `failures`，不进池）；
   烘焙状态用旧实现 overlay 的形状（cull NONE、深度测试/写关闭）——动态命令仍然赢。
3. **事实**（`api/ContentSources`）：`buildScreenProgramFacts` 把**引擎的正典全屏顶点阶段**与宿主的片元
   阶段组成一对（顶点阶段是**读**SDK 的文本，不是抄一份）；宿主自带的顶点阶段**按契约忽略**；无片元阶段
   / 多片元 / compute / 空源 / 入口点不是正典的那个 ⇒ `Malformed`（"程序什么都没写" 才是 `Unknown`）。
4. **录制**（`api/ContentDraw`）：`recordScreen` = 管线 + set 0 采样集 + 动态块 + viewport/scissor +
   `Draw(3, 1, 0, 0)`；计数 `screen_draws()` 与 `input_binds()`（一趟 pass 一次绑定，与内容路径同一条
   "仅当"）。

| 文件 | 是什么 |
| --- | --- |
| `core/Keys`（+.cpp） | `DrawKind` 的家 + `PipelineKey.kind`（相等/哈希/审计表一行） |
| `core/FrameRecorder` | 改用 `Keys.hpp` 的 `DrawKind`（一处拼写） |
| `api/ContentPipeline` | `createScreen` + 按 kind 的 set/布局（set 0 采样集、push 片元 128B）+ `kind()` + 异 kind 拒绝 |
| `api/ContentSources` | `buildScreenProgramFacts`（引擎顶点阶段 + 宿主片元阶段；契约说宿主顶点阶段被忽略） |
| `api/ContentDraw` | `ScreenDraw` + `recordScreen` + `screen_draws()`；`vsg::Draw`（非索引） |
| `tests/test_vsg/ScreenDrawTest.cpp`（新） | 2 个真设备用例：**随 SDK 发布的** `screenCopyProgram(0)` 的拷贝只落在 PiP 矩形内、`screenCopyProgram(1)` 读的是附件 1（由内容 MRT pass 画的绿色）而不是附件 0 |
| `tests/test_vsg/ContentSourcesTest.cpp` | +2（全屏事实的组成与被忽略的宿主顶点阶段；六种拒绝） |
| `tests/test_vsg/ContentPipelineTest.cpp` | +2（set 0 采样集/无块集/异 kind 拒绝；旧 overlay 的烘焙状态） |
| `tests/test_vsg/ContentDrawTest.cpp` | +1（`Draw(3)` + set 0 + 空状态组的第二趟） |

| 规则 | 结论 |
| --- | --- |
| **两套 ABI 是身份，不是风格** | 同一个程序身份 + 不同 kind = 两个变体（同键仍是 `Reused`）。这条不是洁癖：把"全屏层编译的管线"发给内容绘制（或反过来）是**描述符集布局层面的不匹配**，驱动不报、画面也未必看得出 |
| **采样线只用一次** | 全屏路径没有第二套采样机制：`sampledSetLayout`/`inputSampler` 原样复用，差别只有**集合的索引**（kind 决定是 0 还是 1）。M3d-3b-8 的"计划携带输入表"因此一行没改就服务了第二种调用 |
| **全屏三角形是引擎的** | 顶点阶段从 `BuiltinShaders::fullscreenVertexProgram()` **读**出来（与旧实现的工厂同一个做法）；宿主带了顶点阶段也忽略——契约原文如此，而"尊重"它的后果是编译出一个引擎片元阶段没写过的三角形 |
| **push 块的**内容**还没落地** | 布局声明了 128B（SDK 的 `deferredLightProgram` 就是这么声明的），但光照半边（world→view 的三盏方向光 + 环境光）与深度重建半边（near/far）分别是光照相位与深度采样相位的活。**这一片的程序（拷贝）不读它**；读它的内置程序在这之前看到零值——登记，不假装 |
| **门禁抓到的是夹具不是产品** | 设备用例第一次跑出 **12 条 VUID**（`vkCmdSetPolygonModeEXT`/`vkCmdSetColorBlendEnableEXT`/`vkCmdSetColorBlendEquationEXT` 从未调用）：管线声明了这些动态状态，而夹具建 `ContentDraw` 时没取扩展入口点 ⇒ 动态命令静默跳过它们。同一族陷阱在 M3d-3b 的夹具注记里已经写过一次（"没有它们，动态命令跳过 polygon-mode 与 blend 调用"） |
| **变异反证（四条已验）** | N1 集合索引不再跟 kind ⇒ 算术用例红 + 设备侧**段错误**（布局里带着空块集）；N3 三个顶点改成六个 ⇒ 算术红、**像素绿**（PiP 的 viewport 把多出来的三角形裁掉，而全屏拷贝的第二枚三角形画的又是同一张画）；N5 宿主顶点阶段获胜 ⇒ 事实用例红；N6 去掉 kind 检查 ⇒ 层与录制两处一起红。N3 的"像素看不见"是这一片的正当结论：**顶点数是 ABI 的声明，算术用例是它的证据** |

**本片留下的口子（登记）**：**计划侧**（`api/ContentPass` 仍在拒绝 `DrawKind::Screen`）——源 = pass 声明的
第一个输入（采样集已就位）、push 块的内容、PiP 子矩形经计划上屏、以及窗口合成（M4b）；顺带把 §11.16o 登记的
两条口子（多趟窗口 pass 的"一次清"、窗口/离屏同键变体）一起变成可观测用例。

### 11.16q M4b（2026-09-22）：全屏绘制的计划侧（计划解析它的状态，内容层按 kind 录它，真设备像素）

M4a 把全屏调用接到了内容栈；这一片把它接到**计划**上：`api/ContentPass` 不再拒绝 `DrawKind::Screen`，源就是
pass 声明的第一个输入（M3d-3b-8 那条采样线的另一个消费者），PiP 子矩形与"一趟 pass 一条绑定"照旧。三件事：

1. **计划解析全屏调用的状态**（`core/FrameCompiler`）：全屏调用**没有命令**，所以没有"逐命令的状态"可解——
   `CompiledDraw::dynamic` 是它的，取的默认值是屏幕程序不编辑的那些（无剔除、fill、三角形、引擎的混合因子），
   而**深度策略取 `Disabled`**：引擎的正典三角形正好落在 **reverse-Z 的远平面**（z = 0.0 —— 窗口深度清屏的
   那个值），任何深度测试都会把整块覆盖层拒掉。旧实现的 overlay 管线就是为此把深度烘成关闭的，SDK 也把这次
   调用描述成"opaque over it"；继承 pass 的 `TestAndWrite` 会让同一块覆盖层在窗口上消失、在无深度附件的小目
   标上却看着正常。
2. **内容层按 kind 找半片**（`api/ContentPass`）：`Scope::Entry` 多一个 `kind`；全屏半片按**计划里的程序身份 +
   修订**找（没有几何那一半：这次调用不画几何，所以不需要事实表），miss 的消息区分"没编过这个程序"与"修订
   对不上"。采样集**每 kind 一份**：内容在 set 1（块集之后），全屏在 **set 0**（它自己的 ABI），两个 kind 各
   自用自己那一层的布局对象去建 set，而"一趟 pass 一次绑定"的"仅当"逻辑是**同一个注册表**说了算。
3. **push 块**（`api/ContentPass::recordScreenDraw`）：128 字节、**片元阶段**、内容**全零**。布局是 SDK 的
   （`deferredLightProgram` 就这么声明），填它的两半（world→view 的光照、near/far）分属光照相位与深度采样
   相位。**必须真的推**：声明了 push 范围却从不推，着色读到的是**未定义**字节，"那一相位还没落地"得是确定的
   零而不是驱动恰好留下的东西。本片证据用的程序（引擎的 screen copy）一个字节都不读它。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler`（+.hpp） | `CompiledDraw::dynamic`（全屏调用的状态）+ 解析规则（默认值 + `Disabled` 深度）与理由 |
| `api/ContentPass`（+.hpp） | `Scope::Entry::kind`；全屏半片查找（程序 + 修订，两种 miss 消息）；`makeInputSet` 改为按层与集合索引参数化；`recordScreenDraw`（管线 + set 0 + 动态块 + **push** + 矩形 + `Draw(3)`） |
| `api/ContentDraw`（+.hpp） | `ScreenDraw::push`（在管线绑定之后录） |
| `tests/test_vsg/FrameCompilerTest.cpp` | +1 无设备用例：全屏调用的 kind/source/program/矩形进计划，`dynamic` 是 `Disabled` + 屏幕默认值；**同一趟 pass 的内容命令仍拿 pass 的深度** |
| `tests/test_vsg/SampledInputTest.cpp` | +1 真设备用例：计划驱动的合成——pass 1 清源、pass 2 用**随 SDK 发布的** `screenCopyProgram(0)` 在 PiP 矩形里拷贝源；矩形内是源的颜色、外是目标自己的清屏色；`input_binds() == 1` |
| `tests/test_vsg/ContentDrawTest.cpp` | 屏幕用例 +push 节点的形状（片元阶段、128 字节） |

| 规则 | 结论 |
| --- | --- |
| **全屏调用不测深度是它的定义，不是优化** | 正典三角形在 z = 0.0；窗口的深度清 0.0 且比较是 `GREATER` ⇒ 测了就整块消失。变异 P2（改成继承 pass 的策略）在**有深度附件的目标上**让"矩形内是源的颜色"直接变红（实测得到目标清屏色 (0,0,191)） |
| **集合的索引是 ABI，绑错一层就崩** | 变异 P1（全屏采样集绑到 set 1）：4 条 VUID（`vkCmdBindDescriptorSets-firstSet-00360`、`vkCmdDraw-None-08600`）+ **段错误**——全屏管线布局只有 set 0，绑到 1 的集合既越界又不被绘制看见 |
| **一趟 pass、两个 kind、一份输入** | 采样集按 kind 各建一份，但"输入是 pass 的"这条没变：注册表还是同一个，`input_binds()` 仍是每 pass 一次。两种 kind 混在一趟 pass 里在计划与本层都合法（内容命令走自己的三张表，全屏调用走半片） |
| **push 的"形"有证据、"内容"没有** | 形：设备无关用例钉住节点（片元阶段 + 128 字节）与它排在管线绑定之后；内容：**登记**为光照相位的活（本片的程序不读它）。想用"读到零值"做反证不可靠（未定义的 push 内存常常也读成零），所以不假装有反证。**已关，§11.16w**（M5d）：内容落地（`LightPushBlock` + 每次调用推），并且**正向**断言了"三带各自的颜色 + 全零变异三条带全黑" |
| **相机不再被本层使用，但契约仍然要它** | 全屏 ABI 的采样器在 set 0、块一个不绑，所以重写版的全屏路径**不读视图块**；SDK 的 `ScreenPass` 在没有相机时**根本不会问后端**（wiring 期就报），所以"没有视图就画不出来"这条不在这里重造 |

**本片留下的口子（登记）**：**窗口合成**（一趟窗口 pass 里叠一块全屏覆盖层）与 §11.16o 登记的两条口子
（多趟窗口 pass 的"一次清"、窗口/离屏同键变体）——它们要一个"离屏 pass + 两趟窗口 pass"的夹具，与 M4a/b 的
离屏夹具不同源，单独做一片（M4c）。

### 11.16r M4c（2026-09-22）：窗口合成（一趟窗口 pass 里叠全屏覆盖层），顺带把两条登记的口子做成用例

M4a/M4b 的全屏绘制只在离屏目标上验过；这一片把它放进**窗口自己的 render pass**（vsg 造的，表面格式 + 它自己
的依赖），与一趟离屏 pass 同帧。夹具 = 四趟 pass 一帧：①「picture」离屏（只清屏，红）②「shared」离屏（清屏 +
同一段场景三角形，深度格式与窗口 traits 相同 ⇒ 引擎看来与窗口同形）③窗口场景（**第一趟窗口 pass，拥有那一次
清**）④窗口覆盖层（全屏绘制：采 picture，画在 PiP 子矩形里，自己公告**绿色**清屏——必须不生效）。计数器与像素
各管一段：像素说"叠对了"，计数器说"录对了"（黑窗也能让"没画"看起来像"画对了"）。

三条结论，两条来自登记的口子，第三条是这一片**新发现**：

1. **多趟窗口 pass 的"一次清"有了像素证据**：窗口背景是第③趟的清屏色（蓝），第④趟公告的绿色**不出现**
   （`isColourByte(background[1], 0.25)` 为假），三角形仍是场景色 ⇒ `window_recorded_` 那条规则
   （一帧里第一趟窗口 pass 拥有那次清）从"只有理由"变成"有反证"。执行器还断言了它是**按计划顺序**放置四趟
   （`recorded() == {1,2,3,4}`，不是公告顺序——本夹具里两者恰好相同，所以这条是"接缝存在"的证据而不是"顺序会
   被改"的证据）。
2. **"窗口/离屏同键变体"这个口子是错的**：真正的答案不是"共享一个变体、让 vsg 按 viewID 分开编译"，而是
   **两个变体**。理由两条，都是实测：
   * 引擎的兼容性词汇表是**投影**（`RenderTarget::ColorFormat::RGBA8` 同时覆盖 `B8G8R8A8_SRGB` 与
     `R8G8B8A8_UNORM`），而这两个 render pass **不兼容**（通道序 + 色彩空间 + 子 pass 依赖数都不同）；
   * vsg 的 `GraphicsPipeline` 虽然按 viewID 编译，但**复用循环只比 pipeline states，不比 render pass**
     （`vsg 1.1.16 — src/vsg/state/GraphicsPipeline.cpp — GraphicsPipeline::compile()`）：本后端的管线把
     viewport/scissor 声明成动态状态、状态集合逐位相同 ⇒ "两个视图"照样复用同一份实现。
   实测形态：`VUID-vkCmdDrawIndexed-renderPass-02684`（`B8G8R8A8_SRGB` vs `R8G8B8A8_UNORM`、
   `dependencyCount 2 != 1`）——**画面竟然是对的**（未定义行为，不是"能跑就行"的证据）。
   修法（本片落地）：**把设备格式放进键的兼容性半边**——`core::TargetShape` 多两个字段
   （`device_color_formats` / `device_depth_format`，装载设备自己的格式码，0 = 无该附件/未知），
   `core::RenderPassCompatibility` 同样带上，`TargetShape::compatibility()` 一处装配（调用方不再手搓），
   两个目标各自把真实格式报出来（窗口从 surface，离屏从自己建镜像用的那个映射）。这是**引擎词汇表表达不了
   的事实由知道它的那一层交出来**，不是把 extent 之类的运行时量塞进身份：格式是 render pass 身份的一部分，
   Vulkan 的兼容性规则里写着。
3. **顺带纠正 §11.16o 的一条理由**：那里说"窗口内容挂在自己的稳定 `vsg::View` 下 ⇒ vsg 就按窗口的 render pass
   编译窗口的管线"。视图**没有**这个作用（见 2 的第二条：状态相同就复用实现），稳定视图本身还在（记录期
   `viewID` 的出处），但"分族"是**键**的事。视图的真正用途是"逐视图状态覆盖"的挂点，本后端不覆盖任何状态。

**顺带修掉的既有缺陷（本片夹具第一个踩到）**：**capture 的跨帧写-写**。每个目标的 `capture()` 每帧被录一次，
写的是**同一块** host-visible 目的缓冲；会话可以有好几帧在飞，两份拷贝之间没有任何依赖 ⇒ 同步验证报
`SYNC-HAZARD-WRITE-AFTER-WRITE`（4 条）。修法是 capture 命令表开头加一条**声明顺序**的缓冲屏障
（`TRANSFER → TRANSFER`、`TRANSFER_WRITE → TRANSFER_WRITE`）——两份拷贝写的是同样的字节，缺的只是"谁先谁后"
的声明。

| 文件 | 是什么 |
| --- | --- |
| `tests/test_vsg/WindowCompositionTest.cpp`（新） | 四趟 pass 一帧的窗口合成夹具：像素三条（三角形 = 自己的变体、背景 = 第一趟窗口 pass 的清、PiP = 覆盖层采到的那张图）+ 计数器五条 + 执行器顺序；窗口读回按**表面字节序**归一（见下） |
| `tests/test_vsg/TestHostWindow.hpp`（新） | 测试自己的宿主窗口（XCB）：`handle()` / `alive()` / `pixel()`——把 `SessionContentTest` / `SessionMoveTest` 里两份雷同的局部 `HostWindow` 合成一处（-116 行），本片的用例是第三个使用者 |
| `core/Keys.hpp` / `Keys.cpp` | `RenderPassCompatibility` 带 `device_color_formats` / `device_depth_format`（+ `operator==` + 哈希 + 审计表那一行的词）与理由（含实测 VUID） |
| `core/TargetPlan.hpp` / `TargetPlan.cpp` | `TargetShape` 同两个字段 + `compatibility()`（键的兼容性半边就此只有一处装配）；`operator==` 带上它们 |
| `api/WindowTarget.cpp` / `api/OffscreenTarget.cpp` | 各自把真实设备格式报进 shape（窗口 = surface format，离屏 = 自己镜像用的那个）；窗口文件头纠正"稳定视图"那条理由 |
| `api/OffscreenTarget.cpp` | capture 命令表加声明顺序的缓冲屏障（跨帧写-写） |
| `tests/test_vsg/{ContentPassTest,ExecutorTest,SampledInputTest,SessionContentTest,WindowCompositionTest}.cpp` | 五份本地 `compatibilityOf(shape)` 助手删掉，改调 `shape.compatibility()`（一处规则一处装配） |

| 规则 | 结论 |
| --- | --- |
| **"两个视图"不等于"两份编译"** | vsg 的复用循环看的是状态集合；两个视图的状态集合相同时它**故意**复用（对同一 render pass 的 load-op 变体来说这正是想要的：`LoadOpVariantKey` 不进键就是为了让它们共享一份编译）。所以"哪些东西算兼容"必须由**键**回答完整，视图救不了键 |
| **引擎的格式枚举是投影，不是身份** | `RGBA8` 不能作为 render pass 身份的替身：sRGB/线性、BGR/RGB 都是兼容性的一部分。设备格式因此**必须**从设备层交上来（`core/` 不认识这些码，只比较它们）；"没报"与"报了别的"是两个不同的兼容性，未知**不会**静默并族 |
| **未定义行为可能"看着对"** | 变异 Q1（键丢掉设备格式）复现 VUID 的同时**像素照过**：管线当初是按另一个 render pass 编译的，驱动只是恰好把这次画对了。所以这一条的判据是验证层，不是画面——"画面对了"证明不了兼容性 |
| **窗口读回要按表面字节序** | `xcb_get_image` 回来的字节是**服务器/表面**的顺序：本机交换链是 `B8G8R8A8_*` ⇒ `[B,G,R]`。第一版夹具直接在 `[0]` 上比红色，把红蓝读反了——而三角形选的洋红在红蓝上对称，这个错误**看不出来**。夹具现在按 `device_color_formats` 把像素归一成 (r,g,b) 一次，并把颜色选成"红蓝不对称"（背景蓝、覆盖层源红） |
| **记录一帧两次会碰到 probe 的跨帧序** | capture 写的是同一块目的缓冲；会话有好几帧在飞时这就是写-写。修法是声明顺序的屏障（不是加环：probe 的语义正是"读这帧的最后一张图"，而宿主读之前要等设备） |
| **`Session` 中途换帧图不安全** | 试过"settle 帧只挂窗口图"（避开离屏 capture 的重复录制）：`assignFrameGraphs` 会释放**在飞**的命令缓冲 ⇒ `VUID-vkFreeCommandBuffers-pCommandBuffers-00047`。在飞的帧没有公开的等待点（`shutdown()` 才有），所以这条只能整片地做（登记到下面的口子） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| Q1：键里丢掉设备格式（`TargetShape::compatibility()` 不搬那两个字段） | `pipelines()` 从 2 变 1（计数器断言红）+ `VUID-vkCmdDrawIndexed-renderPass-02684` 复现（验证层红，4 条）+ **像素全绿**（未定义行为，见上） |
| Q2：capture 不声明顺序（去掉那条屏障） | 同步验证从 0 变 **4** 条 `SYNC-HAZARD-WRITE-AFTER-WRITE`（写的是同一块缓冲） |
| Q3：不跑 settle 帧 | 窗口读到 **(0,0,0)**：呈现是异步的，`commitFrame()` 之后立刻读会拿到旧后备存储（登记在 §11.16o 的既有事实） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **527 用例 / 85 套件全绿**（+1 用例 / +1 套件） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / **808** 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过；`check_vsg_upstream_capabilities.py` 7 条全部仍成立 |

**本片留下的口子（登记，不假装解决）**：

* **离屏目标的多趟写入**：`OffscreenTarget::passGraph()` 里 bootstrap 恒为 `true`（每趟都清），且离屏内容挂在
  **pass 图**里而不是目标的稳定视图下 ⇒ 同一目标一帧两趟时第二趟会擦掉第一趟、内容只画在自己那趟里。计划侧
  （`CompiledPass::bootstrap` = "该目标的第一个写者"）已经能表达，执行侧还没接上——留给"多写者离屏目标"那片；
  **已关，§11.16s**（M5a）：执行侧接上了——`passGraph` 按计划解析出的 load op 选/建**渲染通道变体**，
  第二趟 LOAD 第一趟写的东西；"内容只画在自己那趟里"正是对的（LOAD 保住了第一趟的画面）。
* **`Session` 在飞的帧没有等待点**：中途 `assignFrameGraphs` 会释放仍在待处理的命令缓冲（VUID 00047）。要用
  "换图"表达"这一帧只画窗口"的用例（本片的 settle 帧本可以更省）得先有那个等待点（或 vsg 侧的回收协议）；
* **capture 的宿主读序**：屏障声明的是**设备侧**的顺序；宿主 `probe()` 前仍要靠 `deviceWaitIdle`（真设备用例
  都这么做）。多帧在飞时，"读第 N 帧的图"与"第 N+2 帧正在写同一块缓冲"之间的宿主侧顺序没有机制，只有约定；
* **`prepare` 的 renderArea 仍无活改尺寸证据**（§11.16o 登记，M5 之前不动）；
* **窗口内容视图仍是"一个管整个窗口"**：与 pass 的对应关系不存在（本后端不覆盖逐视图状态），若将来某个 pass
  需要逐视图状态（例如按视图不同的深度范围），那才是"逐 pass 视图"的时机。


| 有意不做（写在这里，不埋在实现里） | 内容 |
| --- | --- |
| 输入的**深度**半边 | "深度真可采样才绑"与 shadow 的专用绑定（`shadow_bound` + `VineShadowBlock`）一起留给 M5：键里另一个 `depth_sampleable` 字段现在的含义是"pass 自己的目标深度可采样"（M3d-3b-0 的事实 + 执行器核对），在 M5 重钉之前不在这里造第二条语义 |
| 全屏绘制调用（`DrawKind::Screen`） | **已接（§11.16p 内容侧 / §11.16q 计划侧）**：键带 kind、`createScreen` 建全屏层（set 0 = 采样集）、`recordScreen` 录 `Draw(3)`；计划解析其状态（深度 `Disabled`），`api/ContentPass` 按 kind 找半片并把 pass 的输入绑在 set 0 + 推 128B push（内容全零，光照/深度相位各管一半）。采样线没有第二套机制——"源附件 i→binding i"就是 M3d-3b-8 那条线的另一个消费者 |
| 着色器绑定声明的核对（"声明了却供给不了"） | 旧实现靠扫源码 `layout(binding=…)`（`MissingDescriptorBinding`）。这一片不做：驱动/验证层会报，而"谁声明了什么"要动 `ProgramFacts`，留给需要它的那片（M4 的拒绝理由与旧实现逐字对齐时） |

| 落地抓到的 | 内容 |
| --- | --- |
| 顺带修掉的既有偏差（验证层抓的） | 整仓跑验证层时 `MrtTargetTest` 报 `VUID-vkCmdDrawIndexed-firstAttachment-07476`：那两个附件用例的绘制**没告诉录制器**自己写几个附件（默认 1）⇒ 动态混合命令只覆盖附件 0。修法是那一行 `draw.color_attachments = 2U`；修完全套 0 VUID（此前该套件只在无层的情况下跑过，所以这条一直没露） |
| **变异反证 ×4（全部实测）** | A：计划丢掉输入表（`pass.inputs = {}`）⇒ `FrameCompilerTest` 与真设备用例同时红；B：键里计数写回 0 ⇒ 着色器读一个布局里不存在的 set 1，**段错误**（139）；C：注册表不回答"要发采样绑定"（`inputs_issued` 恒 false）⇒ 注册表 6 条断言红 + 真设备用例在"绑一次"那句红、随后段错误；D：彩色收尾改回 `TRANSFER_SRC` ⇒ **像素仍然通过**，但验证层报 6 条（`vkCmdDrawIndexed-imageLayout-00344` + `vkBarrier-oldLayout-01197`）——布局这类主张只有仪器看得见，这条已记进记忆 |
| 工具事实 | 本机 `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` 对**测试二进制**同样有效（不只是 selftest）；`VK_LAYER_ENABLES=…SYNCHRONIZATION_VALIDATION_EXT` 也能整仓跑（512 用例约 3.5 s，完全可做常规门禁） |
| 证据 | `test_vsg` 全量 **512 用例 / 82 套件全绿**（+5）；插件目标与全仓 `ninja` 零 error / 零 warning；强制验证层整仓 **0 VUID**，再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / **800** 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

### 11.16s M5a（2026-09-22）：离屏目标的多写者（LOAD 变体，真设备像素）

M4c 登记的口子：`OffscreenTarget` 只建**一张**渲染通道、load op 恒 CLEAR，所以同一目标一帧两趟时第二趟会擦
掉第一趟。计划侧早就能表达（`CompiledPass::bootstrap` = "该目标的第一个写者"、`planClearValues` 的第 1/2/3
条规则），这一片把执行侧接上。三件事：

1. **计划的三个输入决定 load op**（`api/VsgExecutor::recordOffscreen`）：`pass.clear` / `pass.bootstrap` /
   `pass.depth_preserved` 交给 `passGraph`，目标由此解析出 `PassClearPlan` 并选中要用的渲染通道变体。执行器
   不再自己判断"要不要清"——它只搬事实。
2. **目标按 load op 建渲染通道变体**（`api/OffscreenTarget` + `core::LoadOpVariantKey`）：`LoadOpVariantKey`
   从"粗粒度一份"改成能表达变体的形状（颜色集合一份 + 深度独立一份：load/store + 起始/收尾布局 + `has_depth`），
   `core::loadOpVariantOf(plan, color_final, depth_final)` 是唯一的装配处；目标按该键缓存渲染通道对象，
   framebuffer 与附件在所有变体间共享（`passVariantCount()` 是可读的计数）。
   **关键不变量：所有变体的子 pass 结构与依赖列表逐位相同**——依赖掩码是渲染通道**兼容性**的一部分
   （验证层点名 `VUID-vkCmdDrawIndexed-renderPass-02684` 会比较 `pDependencies`），只有 load/store 与布局可
   以差。因此依赖取"该形状任何一趟都可能需要"的超集，其中颜色的目的域补上了
   `COLOR_ATTACHMENT_READ`（LOAD 的一趟要**读**附件）。
3. **"没人写过"是计划的事实，必须由目标报出来**（`api/OffscreenTarget::written()`）：投影出来的新问题——
   一个刚建好但还没画过的目标，其镜像处于 UNDEFINED；`planTarget` 的
   `RepairReason::Bootstrap`（"nothing usable yet - the first pass in has to clear"）本来就有这条规则，但事实
   得有人报。`written()` 在**建 pass 图时**置位（"有人往里录过东西"），夹具与将来的 target 账用
   `TargetFacts::current.built = target->written()`。

| 文件 | 是什么 |
| --- | --- |
| `core/Keys.hpp` / `Keys.cpp` | `LoadOpVariantKey` 改成变体的名字（颜色集合 load/store + 起始/收尾布局；深度独立一份 + `has_depth`）+ `operator==` + 审计表那一行 |
| `core/ClearPlan.hpp` / `ClearPlan.cpp` | `loadOpVariantOf(plan, color_final, depth_final)`：CLEAR 从 UNDEFINED 起步、LOAD 命名上一趟留下的布局；**清屏值不进键**（值属于 pass 实例） |
| `api/OffscreenTarget`（+.hpp） | 变体表（键 → 渲染通道对象）+ `renderPassFor()`；`makeOffscreenRenderPass` 改为按变体建造（依赖列表取超集，与 load op 无关）；`passGraph(policy, bootstrap, depth_preserved)`；`passVariantCount()`、`written()` |
| `api/VsgExecutor` | `recordOffscreen` 把计划的三个事实交给目标 |
| `tests/test_vsg/ClearPlanTest.cpp` | +1 无设备用例：变体命名"做什么"而不命名"清成什么"（CLEAR ⇒ 起始 UNDEFINED、LOAD ⇒ 起始 = 收尾布局；值不同的两份计划是**同一个**变体；保留的深度走 LOAD；纯色目标深度位为空） |
| `tests/test_vsg/ContentPassTest.cpp` | +1 真设备用例：一帧两趟同一目标（第一趟 bootstrap 清屏 + 左网格，第二趟 LOAD + 右网格），像素三条（背景是第一趟的清、左右两个网格都在）+ 三个计数（`pool.created() == 1`、`pipeline_binds() == 2`、`passVariantCount() == 2`）；夹具的 `built` 改成 `target->written()` |

| 规则 | 结论 |
| --- | --- |
| **load op 是"交换半边"，不是身份** | Vulkan 的兼容性规则里没有 load/store 与布局，所以一个变体对象的管线在另一个变体里**合法**——这正是"两趟 pass 共用一个变体"能成立的原因；反过来说，**依赖掩码绝不能随变体变**（变异 Q3 实测：4 条验证层报错，含 `VkRenderPassBeginInfo-renderPass-00904`——变体连自己的 framebuffer 都不兼容了） |
| **"没人写过"不能 LOAD** | 新目标的第一趟必须清（颜色与深度都是）；报这条事实的只能是目标自己（`written()`），硬写 `built = true` 的症状是 `VUID-vkCmdDraw-None-09600`（"期望 DEPTH_STENCIL_ATTACHMENT_OPTIMAL，当前 UNDEFINED"）——只有同步验证看得见 |
| **值不属于变体** | 清屏值是 pass 实例的事，所以两趟清成不同颜色的 pass 共用一份渲染通道对象、一份管线；变体键里只放"操作 + 布局" |
| **bootstrap 是"第一个写者"的** | 同一目标一帧两趟：第一趟 bootstrap（清），第二趟不是（LOAD）；变异 Q1（执行器写死 bootstrap）⇒ 第二趟清屏 ⇒ 背景与左网格一起消失（像素读回 (0,0,0)、(0,0,0)） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| Q1：执行器忽略计划（`passGraph(clear, /*bootstrap*/ true, false)`，即 M4c 之前的形态） | 新用例红 4 条：`passVariantCount` 2→1、背景 (0,0,0)、左网格 (0,0,0)（第一趟的画面被第二趟擦掉） |
| Q2：`loadOpVariantOf` 忽略 LOAD（恒 Clear） | 无设备用例 3 条红（`color_load`、`color_initial`、`color_initial=ShaderReadOnly` 那句）+ 设备用例红（变体数 1、背景黑） |
| Q3：依赖掩码随变体变（LOAD 时多一个 stage 位、颜色目的域去掉 READ） | 用例本身**全绿**，验证层 4 条：`VkRenderPassBeginInfo-renderPass-00904`（变体与 framebuffer 不兼容）+ `vkCmdDrawIndexed-renderPass-02684`（共享管线跨族）——"依赖是兼容性的一部分"由此有反证 |
| Q4：`written()` 恒 true | 整仓同步验证 0 → **4** 条 `VUID-vkCmdDraw-None-09600`（深度附件期望 DEPTH_STENCIL_ATTACHMENT_OPTIMAL、实际 UNDEFINED） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **529 用例 / 85 套件全绿**（+2 用例） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 808 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`passGraph` 的默认参数是"bootstrap"**（`bootstrap = true, depth_preserved = false`）：手驱路径（`renderGraph()`
  与旧夹具）仍按"清"来建图，因为它的调用方不经过计划；等手驱路径退役再收掉这两个默认值。
* **退役/重用变体**：变体表只增不减（一个目标最多几种 load-op 组合，且随目标一起销毁）；若将来出现"每帧换
  load-op"的形态，这里要按 `RetirementQueue` 处理。
* **`written()` 在"录了但没提交"时也为真**：事实是"有人往里录过东西"，不是"GPU 写过"；对 bootstrap 判断够用
  （下一帧的 pass 会写），但"提交失败后重来"的语义要靠 `attachments_invalidated` 那条路（M6/M7 再说）。
* **深度侧的 promote/borrow 布局仍只走"附件布局"**（M3c 的既有形态）：`depth_final` 目前恒为
  `DepthAttachment`，阴影那片的"提升成纹理"会带来第三种收尾布局——那时变体键的深度半边才真正被用满。
  **已关，§11.16t**（M5b）：`core::depthFinalLayout` 给了第三种收尾布局（纯深度可采样 ⇒ `ShaderReadOnly`），
  LOAD 变体的 `depth_initial` 改成命名 `depth_steady`（借用者命名的就是出借者的布局）。
* 前一版（§11.16r）留下的 `Session` 等待点、capture 宿主读序两条口子不变。

### 11.16t M5b（2026-09-22）：输入表带上了深度半边（"阴影脊柱"，真设备像素）

M5a 登记的口子：变体键的**深度半边**还只是"附件布局"一种取值，"提升成纹理"没接
上；而 SDK 的输入契约原文
就是"该来源的**每一张颜色附件**（外加**那张深度**，只要它可被采样）"——上一片只实
现了前半句。这一片补后半
句，并且用"一趟 pass 采样另一趟写进**纯深度目标**的值"来证明它：

1. **计划为输入解析出深度事实**（`core/FrameCompiler::resolveInputs`）：`Compiled
Input` 加 `depth_sampleable`，
   值与目标自己那份计划**同源**（`core::depthPlan(found->depth).sampleable`）——
"这个深度能不能被采样"
   一帧只有一个答案，采样者与被采样者不会各说各话。
2. **深度-only + 可采样 = 深度收尾在采样布局**（`core::depthFinalLayout(shape, sam
pleable)`）：只有"purely
   深度 + 宿主要求可采样"的形状（阴影贴图的形状）把深度留在 `ShaderReadOnly`——
   它的深度**就是**那幅画，下一趟采样它不需要自己的屏障；任何带颜色附件的目标
   仍留 `DepthAttachment`（下一趟还要拿它做深度测试）。这条规则只依赖形状与宿主要
   求，所以**无设备可钉**（`CoreClearPlanTest` 两条用例）。
   `OffscreenTarget::depthSteadyLayout()` 用同一个函数，并且**借用者问出借者**：
   借来的深度处在出借者留下的布局里，所以借用者的 LOAD 变体声明的是出借者的收尾
   布局。
3. **内容层绑定深度**（`api/ContentPipeline` + `api/ContentPass`）：采样集合的布局
从"颜色数"扩成（颜色数, 深度数）
   的**对**（`SampledKey`），绑定顺序**固定为：输入逐个来，一个输入内部先颜色附件
（按附件序）后深度**——所以
   深度绑定号 = 颜色数（阴影那片的程序里就是 `binding 0`，因为它没有任何颜色输
入）；深度用**NEAREST** 采样器
   （`ContentPipeline::depthSampler()`）：着色器比对的是精确深度，插值出来的值
等于一个谁也没栅格化过的深度。
   `PipelineKey` 因此也加了 `sampled_depth_count`（"这个管线的集合里有几个深度采
样器"是身份，与颜色同理）。
4. **给消费者的报价也校验深度半边**（`api/ContentPass::record`）：调用方与计划在
**两个半边**上都要一致，不一致
   就整趟不画，报文点名是哪一个半边动了（"计划说不可采样/计划说可采样而你没
给"）。
5. **深度捕获的出入布局跟着形状走**（`api/OffscreenTarget::create`）：原来写死
```
DepthAttachment → TRANSFER_SRC → DepthAttachment
```
   的一对屏障，在"深度收尾在 `ShaderReadOnly`"的形状上直接是**谎报**
   （实测 `VUID-VkImageMemoryBarrier-oldLayout-01197`：镜像实际在 `SHADER_READ_O
NLY`，屏障说它是
   附件布局），并且把镜像**放回错的布局**，于是消费者采样到全 0（整幅画面黑，而
深度探针却正常——像素与探针
   说的是两件事，这条差异就是线索）。改成"从 `depthSteadyLayout()` 出去、回
`depthSteadyLayout()`"，前向
   屏障的源作用域补上 `FRAGMENT_SHADER`（采样者已经读过它），后向补上
   `EARLY|LATE_FRAGMENT_TESTS | FRAGMENT_SHADER`（下一趟既当附件又当纹理）。
6. **纯深度形状被 `create` 接受**：`TargetLayout::color_formats` 为空 + 有深度
格式 = 合法的阴影贴图形状（原来"空形状"一律拒绝；
   注意 `Layout` 那个老重载会默默塞一张 RGBA8——夹具里必须显式 `color_formats.c
lear()`，这一条实测踩过）。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameCompiler.hpp` / `.cpp` | `CompiledInput::depth_sampleable`（输入表的
深度半边）+ `resolveInputs` 用 `core::depthPlan` 解析它 |
| `core/ClearPlan.hpp` / `.cpp` | `depthFinalLayout(shape, depth_sampleable)`（阴
影贴图形状 ⇒ `ShaderReadOnly`，其余 ⇒ `DepthAttachment`）+ `loadOpVariantOf(pla
n, color_final, **depth_steady**, depth_final)`：LOAD 必须命名**它保留的像素真正
在的**布局——借用者读的是出借者的布局，而不是自己写完之后的布局 |
| `core/Keys.hpp` / `.cpp` | `PipelineKey::sampled_depth_count`（+ `operator==`
、哈希、审计表那一行） |
| `api/OffscreenTarget`（+.hpp） | `depthSteadyLayout()`（借用问出借者）；`makeOf
fscreenRenderPass` 里的布局转换提到文件作用域的 `toVkLayout()`（捕获屏障与渲染通道
描述必须用同一个转换）；深度捕获屏障按 `depthSteadyLayout()` 进/出 |
| `api/ContentPipeline`（+.hpp） | `SampledKey{colors, depths}` + 哈希；`sampledSe
tLayout(colors, depths)`、`layoutFor(colors, depths)`；`depthSampler()`（NEAREST
） |
| `api/ContentPass.cpp`（+.hpp） | `sampledDepthCount(pass)`；报价两半边校验（不一致 ⇒ 整趟不画 + 报文点名）；`makeInputSet` 绑定顺序=先颜色后深度、深度用 NEAREST |
| `api/VsgExecutor.cpp` | 帧尾读回循环里 `readback()` 的空安全（一个目标可以只有
深度、没有颜色捕获） |
| `tests/test_vsg/SampledInputTest.cpp` | +1 真设备用例（"阴影脊柱"）：①纯深度可
采样目标（`D32`、清 0.0）+ ②消费者颜色目标（清蓝）；生产者三角形的片段着色器为
空（`settings.color_attachments = 0`），消费者采样深度并写出灰值。判据：计划的两
条输入事实、深度探针（三角内 0.5 / 外 0.0）、消费者像素（左半 0.5 灰 / 右半黑 /
三角形外保留清屏色） |
| `tests/test_vsg/ClearPlanTest.cpp` | +2 无设备用例：`depthFinalLayout` 四种形状
（纯深度可采样 ⇒ `ShaderReadOnly`；纯深度不可采样 / 带颜色 + 可采样 / 无深度 ⇒
`DepthAttachment`），以及阴影贴图形状的变体（CLEAR 起 `Undefined`、LOAD 起 `Shad
erReadOnly`，两者收尾都是 `ShaderReadOnly`，且是**两张**渲染通道对象） |

| 规则 | 结论 |
| --- | --- |
| **"可采样"决定收尾布局，而不是决定"要不要转"** | 纯深度可采样（阴影贴图）⇒
深度收尾 `ShaderReadOnly`，采样者零屏障；带颜色附件的目标⇒ `DepthAttachment`（下
一趟的深度测试要用）。规则是形状的函数（§11.16t-2 的两条无设备用例），所以相位/
目标/捕获三处不会各算各的 |
| **捕获屏障必须命名真正的当前布局** | 写死 `DepthAttachment` 的捕获入口屏障对阴
影贴图是谎报（`VUID-VkImageMemoryBarrier-oldLayout-01197`），而且它还把镜像放回错
布局 ⇒ 采样全 0。**探针绿 / 像素黑**这种撕裂，正是"探针读的是捕获副本、消费者读
的是原图布局"的指纹 |
| **借用的深度：LOAD 命名出借者的布局** | `depth_steady` 是"这趟不清它时它现在在
哪"，借用者是**出借者**的收尾布局；收尾仍是本层决定的 `depth_final`（借用者原样
还回去） |
| **绑定顺序是 ABI** | 输入逐个来，一个输入内部先颜色后深度 ⇒ 深度绑定号 = 颜色
数；阴影片（无颜色输入）里它就是 `binding 0`。`sampled_depth_count` 进 `Pipeline
Key`，因为"集合里有没有深度采样器"是管线身份的 ABI 半边 |
| **深度用 NEAREST** | 比对精确深度：线性插值出来的值是一个谁也没栅格化过的深度 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| Q1：`depthFinalLayout` 恒 `DepthAttachment`（阴影贴图也收在附件布局） | 红：2
条 VUID（`vkCmdDrawIndexed-imageLayout-00344`：着色器访问时镜像布局与描述符不符）
+ 6 条断言（4 条无设备 + 2 条像素） |
| Q2：`makeInputSet` 不绑深度（`if (false && input.depth != nullptr)`） | 红：2
条 VUID（`vkCmdDrawIndexed-None-08114`：集合里的描述符无效）+ 进程段错误（lavapi
pe 读未绑定描述符）——"没绑"不是"少画点东西"，是未定义行为 |
| Q3：`resolveInputs` 把 `depth_sampleable` 恒 false（计划忘了深度半边） | 红：用
例 2 条（计划的输入事实 + 内容层**拒绝整趟**——报价与计划不一致）且进程不再崩溃
（拒绝发生在录制前） |
| Q4（过程记录）：夹具只设 `Layout::clear_color`、没给第二趟 `setClearPolicy` |
红：整幅清成黑（第二条）——补上策略后清屏色才生效；顺带证明"清屏是宿主决策、计划
只决定 bootstrap 何时override" |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **532 用例 / 85 套件全绿**（+3 用例） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验
证仍 **0 SYNC-HAZARD**；hygiene 0 / 808 文件；`check_diagnostic_formats.py` 0 /
39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **灯光与 `VineShadowBlock` 还没接**：这一片证明的是"深度能被采样"，还不是"阴影
算得对"——`shadow_bound` 这个键位、灯槽索引、
  `VineShadowBlock`（旧路径：颜色数 +2 处的块）与 `Light::castShadow` 的挑选逻辑
都留到 M5c。**灯光那半已关，§11.16u**（M5c-1）：前向光照块落地（`shadow_bound` / 阴影块 / 槽号仍是 M5c-2）。
* **采样集合的深度数仍是"每输入 0/1"**：计划里一个输入最多贡献一个深度；若将来
出现"一趟采样两个目标的深度"，`SampledKey` 的
  `depths` 计数已经够用，但绑定顺序的推导（=颜色数）要保持"先颜色后深度"的前缀
规则。
* **深度捕获的 `steady_layout` 对借用者的语义**：借用者捕获的是**共享**深度（出
借者的镜像），布局也按出借者算——如果将来允许"借用者独立提升"，
  这条要再想；目前借用深度恒不可采样（`core::depthPlan`），所以还到不了这里。
* 前一版（§11.16s）留下的 `passGraph` 默认参数、变体退役、`written()` 语义三条口
子不变。

### 11.16u M5c-1（2026-09-22）：前向光照块（灯到了着色器里，真设备像素）

M5b 登记的"灯光与 `VineShadowBlock` 还没接"拆成两片，这是**灯光**那半。要解决的问题只有一句：引擎
把灯光宣布给**一次绘制调用**，而这份数据此前没有任何一条路到达着色器。前向路径**不能走 push**——
128B 的保证预算被视图矩阵占满（引擎的前向顶点阶段就在那里读），所以灯光必须走**UBO**；全屏路径的
那半（128B push 里装灯与深度重建）留给它自己的相位。四件事：

1. **打包是一个规则，不是一个 memcpy**（`api/LightBlock` + `api/LightBlock.cpp`）：一个 ambient 槽
   （rgb + 强度，后宣布的覆盖前一个）+ 最多三盏方向光（按宣布顺序占 0..2 槽），世界坐标方向**乘相机
   的三个轴**换成视图空间（视图矩阵的前三行就是 r / u / -f），方向**归一化**（着色器拿它和法线做点
   积，未归一化等于把灯调亮），跳过 disabled 与块装不下的类型（Point/Spot 保留但无槽）。块是 ABI
   （`VineLightsBlock`，112B，`builtin_forward.frag` 声明的那份文本），静态断言钉死。
2. **"空灯单"仍然可见，但补光不算数**：没有可用的环境光时块里填 0.15 的 ambient
   （否则着色把 albedo 乘成零），但**计数只算宿主宣布的灯**——补光被算进去，调用方就会报一个宿主从
   未宣布过的"丢弃"。返回值 = "块代表了几盏宣布的灯"，调用方拿去和宣布数比。
3. **没有相机 = 空块**：没有视图空间可换算时块保持全零（不是补光）。这是参考实现的行为，而且**看得
   见**：没宣布相机的 pass 是"不亮"，不是"被猜出来的环境光照亮"。
4. **每绘制调用写一块、四个动态偏移绑定**（`api/BlockStorage` 的第 4 个区域 + `api/BlockDescriptors`
   的 `kLightsBinding = 3` + `api/ContentPass` 在每个内容 draw 的命令循环**之外**写块）：契约宣布灯光的
   粒度是"一次调用"（该调用的每条命令共享），所以块也按调用写一次；`BlockDescriptors::Offsets` 从三个
   变四个（view / draw / material / **lights**），布局多一个 `UNIFORM_BUFFER_DYNAMIC` 绑定。
5. **丢弃报告是"每 episode 一次"**（`ContentPass::reportLightsDropped` + `Scope::lights_dropped`）：
   规则照旧实现搬——`announced == 0 || represented >= announced` ⇒ **re-arm**（episode 结束），否则
   只在 `shouldReport()` 为真时报一次；报文分三支，且每支都说清楚**替代物**是什么（没相机 ⇒ 不亮；
   全 unusable ⇒ 补光；部分 ⇒ 缺几盏）。`ReportOnce` 放在 caller 提供的 `Scope` 里：episode 的结束由
   调用方决定（一帧一个 scope ⇒ 每帧一次；一个 session 一个 scope ⇒ 全程一次）。

| 文件 | 是什么 |
| --- | --- |
| `api/LightBlock.hpp` / `.cpp`（新） | `VineLightsBlock`（112B：ambient + `dirs[3]` + `cols[3]`，std140 静态断言）+ `packLightBlock(lights, camera, out)` 的完整规则与"没有相机 ⇒ 空块" |
| `api/BlockStorage`（+.hpp） | 第 4 个区域 `lights{112, 1, 3, 1024}`（每调用一块）+ `writeLights` + `Regions` / `Strides` / `overflows()` 的相应半边 |
| `api/BlockDescriptors`（+.hpp） | `kLightsBinding = 3` + 第 4 个 `UNIFORM_BUFFER_DYNAMIC` 绑定 + `Offsets::lights`（动态偏移数组按**绑定顺序**四个） |
| `api/ContentPass`（+.hpp） | 每个内容 draw 写一块、把 offset 传进 `recordCommand`；`Scope::lights_dropped`（`core::ReportOnce`）+ `reportLightsDropped`（re-arm 规则 + 三支报文，`ChannelIgnored`）；文件注记里"深度半边还没接"的过期句子一并更正（M5b 已接） |
| `tests/test_vsg/LightBlockTest.cpp`（新） | 6 条无设备用例（视图空间换算、**旋转相机**才是判据、disabled/无槽类型跳过、空灯单 = 补光且不计数、四盏方向光只装三盏、无相机 ⇒ 空块）+ 1 条真设备像素用例（**一帧一趟四带**：带 0 灯朝观察者 = ambient + 太阳；带 1 同色反方向 = 只有 ambient；带 2/3 宣布装不下的灯 = 补光，且两条之间**只有一条**报告；带外是 pass 自己的清屏色） |
| `tests/test_vsg/BlockDescriptorsTest.cpp` | 三条断言从"三个绑定"改成"四个"（布局、集合的描述符范围、动态偏移数组按绑定顺序四个） |

| 规则 | 结论 |
| --- | --- |
| **光照块是宿主决策的载体，不是身份** | 与材质块同理：块在描述符集里、按动态偏移选，不进 `PipelineKey`——"灯换了"是写一块新字节，不是编译一份新管线 |
| **方向必须换成视图空间** | 而**轴对齐的相机会让缺失的旋转变不可见**：变异 M1（方向原样留在世界空间）只有算术用例红，真设备像素用例仍然全绿（那台的相机在 +Z 且目不斜视）。这正是"判据要选对"的又一例：约定用算术用例钉，像素管"能用" |
| **补光不算灯** | 变异 M2（把补光算进返回值）⇒ 5 条红：4 条算术 + 设备用例的报告断言（"全装得下"⇒ 不报告 ⇒ 计数 1 变 0） |
| **块是每次调用的** | 变异 M3（整趟 pass 只打包第一调用的灯）⇒ 设备用例三条像素红：带 1..3 全变成带 0 的颜色 `(128,26,26)` |
| **布局与声明必须一致** | 变异 M4（布局里去掉 lights 绑定，着色器仍声明 binding 3）⇒ 无设备用例红（绑定数 3≠4），设备用例**在驱动里段错误**（未定义行为，崩溃前 0 VUID）——"没绑"不是"少画一点" |
| **清屏仍是宿主的** | 夹具不把 `setClearPolicy` 交给第二趟以后的 pass，看到的就是黑屏（M5b 已记一次，这一片在四带图里又踩一次：带外的"背景"必须是 pass 自己的清屏色，否则"背景"断言什么都没有 |
| **报告要有替代物** | 三条报文都说清了替代物（不亮 / 补光 / 缺几盏）：只说"丢弃"会让读者猜是黑屏还是补光 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| M1：世界的方向不换视图空间 | 1 条红（`TheDirectionsAreRotatedIntoViewSpace`）；设备用例**全绿**（轴对齐相机看不出）——判据选对的实测 |
| M2：补光算进"代表了几盏" | 5 条红（4 条算术 + 设备用例的报告计数 1→0） |
| M3：整趟 pass 只打包第一调用的灯 | 设备用例红 4 条（报告计数 + 带 1/2/3 像素全变成带 0 的 `(128,26,26)`） |
| M4：布局去掉 lights 绑定 | 无设备用例红（3≠4）+ 设备用例段错误（崩溃前 0 VUID） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **539 用例 / 86 套件全绿**（+7 用例、+1 套件） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 811 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **阴影块（`VineShadowBlock`）与 `shadow_bound` 键位还没接**（M5c-2）：灯的"槽号"（`params.w` 指向
  块里第几盏方向光）、`Light::castShadow` 的挑选、以及把 map 与块绑到内容 ABI 里，都是那一半的事。
  **已关，§11.16v**（M5c-2）：阴影块落地（`packShadowBlock` + `kShadowBinding = 4` + 槽号），而
  `shadow_bound` 键位按实测**删掉**（本后端的 ABI 里它是冗余的，见该节）。
* **全屏路径的 128B push 内容仍全零**：`deferredLightProgram` 读的 ambient/sun/projparms 是光照相位
  的另一半（全屏不需要矩阵，所以它走 push），本片只做了前向 UBO 那半。
* **引擎自带的前向程序文本与本后端的 set 0 布局不同**（登记为场景桥的债）：SDK 的
  `builtin_forward.frag` 声明 material 0 / diffuse 1 / lights 2（外加 shadow 3/4），draw 块还在 **set 1
  binding 0**；而本重写版的 set 0 是 view 0 / draw 1 / material 2 / lights 3。两者都能工作，但**同一个
  管线的着色器文本与集合布局必须匹配**——场景桥把那批程序接进来时，要么按 SDK 的绑定建集合，要么
  在文本层做转换，不能两套 ABI 混配。
* 前一版（§11.16t）留下的"灯光槽索引/阴影块""采样集合深度前缀规则""借用者捕获布局"三条口子，
  除前两条外不变。

### 11.16v M5c-2（2026-09-22）：阴影块（一张 map 只缩放它属于的那盏灯，真设备像素）

M5c 的后半片。M5b 把 map 当**采样输入**绑上了（深度半边），但着色器拿不到"一个片元落在 map 的哪里"——
map 是**那盏灯的相机**栅格化的，消费 pass 用的是自己的相机，所以块里必须带
`light_vp * inverse(view)` 与比较用的标量（`VineShadowBlock`：一个 mat4 + `params` = 开关 / bias / 强度 / 灯槽
号）。四件事：

1. **map 的"身份"是目标说出来的**（`core::ShadowFacts` + `TargetFacts::shadow`）：`RenderTarget::shadowOf`
   说这张 map 属于哪盏灯、`setProducerViewProjection` 说怎么读它；事实由调用方（会话/执行器）从 SDK 目标
   读进来，计划原样复制。参考实现踩过的坑就在这里：**"第一个深度可采样的输入就是太阳的 map"会把 G-buffer 的
   深度当成阴影贴图**（整个 deferred 分支"影子是世界坐标的函数"，`.ai/design/graphics-shadow.md` 有记录）。
2. **计划解析"这趟 pass 的 map"**（`core/FrameCompiler::resolveShadow`）：输入里**第一个**
   "目标表明它属于某盏灯 + 深度可采样 + 生产者公布了矩阵"的输入；三个事实缺一个 ⇒ `light == nullptr`
   ⇒ 着色开关保持关闭（不假装有一张能读的 map）。
3. **打包与开关**（`api/ShadowBlock`）：按身份找到 map 的那盏灯，要求它**这趟调用宣布了、启用了、还在投影
   （`castShadow`）、类型是方向光、且在灯块的三个槽内**；满足才写矩阵（列主序）与
   `params = {1, 该灯自己的 ShadowSettings::bias, 1, 槽号}`，否则整块**零字节**（开关关）。
   `api/LightBlock` 新增 `directionalSlotOf(lights, identity)`——"这盏灯落在第几槽"必须和灯块打包**同一次
   遍历**（旧实现的注释原话："the same walk collectLight…"），否则 `params.w` 会指向另一盏灯。
4. **每绘制调用写一块、第 5 个动态偏移绑定**（`BlockStorage` 第 5 区域 + `BlockDescriptors::kShadowBinding = 4`）：
   块的内容与调用有关（相机 + 灯单），所以粒度仍是"一次调用"；开关关的调用也**照写**（全零），因为同一个
   着色器文本要同时服务有影/无影两种 pass——**绝不能留下未绑定的块**。

**顺带的键修正（实测推论，已改设计）**：`PipelineKey::shadow_bound` **删掉**。它在 v1 里是从旧实现抄来的
（旧路径的前向 shader set 有"插入阴影 ABI"的变体 ⇒ 集布局不同 ⇒ 管线不同），而在本后端的 ABI 里：
map 走**采样输入**（`sampled_depth_count` 已经是键的一半）、阴影块**恒在块集里**（shader 可以少声明，集合布局
不变）、有影/无影是**运行期开关**（同一个文本）——留着一个"改了 GPU 对象却不变"的键字段，正是键审计表要防止
的事：它只会让本该共用一份管线的两个 pass 分家。§D3 的那一行按此更正。

| 文件 | 是什么 |
| --- | --- |
| `core/FrameRecorder.hpp` / `.cpp` | `LightRef` += `identity`（`Light*`，阴影靠它认领）/ `cast_shadow` / `shadow_bias`（`ShadowSettings::bias`）；`snapshotLights` 逐项填 |
| `core/FrameCompiler.hpp` / `.cpp` | `ShadowFacts`（谁的 map + 是否公布矩阵 + 生产者矩阵）+ `TargetFacts::shadow` + `CompiledInput::shadow` + `CompiledPass::shadow`；`resolveShadow`（三个事实、先来先用） |
| `core/Keys.hpp` / `.cpp` | **删** `PipelineKey::shadow_bound`（等值 / 哈希 / 审计表行同步） |
| `api/ShadowBlock.hpp` / `.cpp`（新） | `packShadowBlock(shadow, draw, out)`：身份匹配 + 启用/投影/类型/槽检查 + `light_vp * inverse(view)`（列主序）+ `params`；不可用 ⇒ 全零 + false |
| `api/LightBlock`（+.hpp/.cpp） | `directionalSlotOf(lights, identity)`（与打包同一次遍历） |
| `api/BlockStorage` / `api/BlockDescriptors` | 第 5 区域 `shadows{80,1,3,1024}` + `writeShadows`；`kShadowBinding = 4` + 5 个动态偏移 |
| `api/ContentPass`（+.hpp） | 每绘制调用打包/写阴影块（开关关也写）+ `shadow_offset` 传入 `recordCommand` |
| `tests/test_vsg/ShadowBlockTest.cpp`（新） | 4 条无设备用例（视图→灯 clip 的组合**用两张矩阵语义验证**且相机非恒等、七种缺一事实的拒绝 + 块必须全零、第四盏方向光的槽号、`directionalSlotOf` 与打包**互相钉住**）+ 1 条真设备像素用例（**一帧四带**，见下） |
| `tests/test_vsg/FrameCompilerTest.cpp` | +1 无设备用例：**四输入**（G-buffer 深度带矩阵且排第一 / 深度不可采样的 map / 没公布矩阵的 map / 可用的 map 排最后）⇒ 计划必须选最后一个；每条被跳过的输入都要说得出原因 |
| `tests/test_vsg/BlockDescriptorsTest.cpp` / `VariantCoreTest.cpp` | 绑定数 4→5（含范围与动态偏移）；把 `shadow_bound` 的开关换成 `sampled_depth_count` |

| 规则 | 结论 |
| --- | --- |
| **只能由目标"认领"** | 变异 M4（去掉"目标说了属于哪盏灯"这个条件，任何"可采样深度 + 有矩阵"都算）⇒ 计划把 G-buffer 的深度当成 map（`pass.shadow.light == NULL`）——这就是参考实现的实测缺陷 |
| **槽号决定缩哪盏灯** | 变异 M2（`params.w` 恒 0）⇒ 设备用例带 2 从 `(26,128,26)` 变成 `(128,26,26)`：本该被缩放的那盏灯没被动，另一盏被缩没了——"整张 map 缩放所有灯"的经典错误 |
| **开关是灯的运行期事实** | 变异 M3（忽略 `castShadow`）⇒ 无设备用例 + 设备用例带 1 红（同一盏灯在两趟调用之间**被翻开开关**⇒ 带 1 从全亮变成只剩 ambient）；相机夹具第一版用**另一个 Light 对象**表达"不投影"，M3 只在无设备用例红——那测的是**身份**不是开关，现已改成同一对象翻开关 |
| **组合必须走世界** | 变异 M1（丢掉 `inverse(view)`，直接用生产者的矩阵）⇒ **只有算术用例红**（设备用例的相机只有平移，而测试选的生产者矩阵不读 z ⇒ 平移不可见）。又是一次"判据选对"：这类约定归算术用例 |
| **矩阵是列主序** | 变异 M5（按行写）⇒ 算术用例红（测试的矩阵有非对称项 (0,1)=0.25 / (1,0)=-0.125，对角矩阵看不出来） |
| **拒绝必须写零** | 变异 M6（拒绝时把 `params.x` 留成 1）⇒ **只有字节级用例红**：设备像素看不出（零矩阵 ⇒ `light_clip.w = 0` ⇒ NaN ⇒ 着色器的范围判断全 false ⇒ 恰好落回"亮"，那是巧合而不是检查） |
| **块是每次调用的** | 变异 M7（整趟 pass 只打包第一调用的阴影块）⇒ 设备用例三条像素红（带 1/2/3 全按带 0 的灯与槽算） |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| M1：组合丢掉 `inverse(view)` | 1 条红（算术用例；设备用例绿——轴对齐/纯平移相机看不出） |
| M2：`params.w` 恒 0 | 设备用例红（带 2 的颜色变成"另一盏灯被缩放"） |
| M3：忽略 `castShadow` | 2 条红（无设备 + 设备带 1） |
| M4：不要求目标认领（任何可采样深度 + 矩阵都算） | 1 条红（计划选错输入：G-buffer 的深度变成 map） |
| M5：矩阵按行写 | 1 条红（算术用例） |
| M6：拒绝时留下开关 1 | 1 条红（字节级用例；像素因 NaN 恰好正确） |
| M7：阴影块按 pass 打包 | 设备用例 3 条红（带 1/2/3 全用带 0 的灯） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **545 用例 / 87 套件全绿**（+6 用例、+1 套件） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 814 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`TargetFacts::shadow` 的生产侧还没接**：本片的夹具从 SDK 目标自己读（`shadowOf()` /
  `producerViewProjection()`）再填进事实表；会话/执行器那层要把它变成常规动作（和 `depth` 事实同一处）。
* **`params.y/z` 的语义只钉了"来源"**：bias 来自投影灯的 `ShadowSettings`（实测值随灯走），强度恒 1；
  `ShadowSettings::filter`（PCF）是 SDK 自己标了 RESERVED 的，本片不假装支持。
* **全屏路径的 128B push 光照/深度重建半边**仍未填（M5c-1 登记过），它读的是同一份灯数据的另一种表示。
* **场景桥的 ABI 债仍在**（§11.16u 登记）：SDK 自带前向程序的绑定与本后端 set 0 布局不同，接进来时要统一。
* 前一版（§11.16u）留下的 `TargetFacts::shadow` 生产侧、`params.y/z` 语义两条口子中，前一条已在上方登记。

### 11.16w M5d（2026-09-22）：全屏路径的 128B push（光照的第二种表示，真设备像素）

M4b 登记的"push 的形有证据、内容没有"到此关闭。全屏路径**不需要视图矩阵**（顶点阶段是
`gl_VertexIndex` 生成的正典三角形），所以它把整个 128B push 预算花在光照上——同一个场景的两条路径
因此有两种表示（前向 UBO / 全屏 push），值必须一模一样。三件事：

1. **push 是同一份打包的另一种布局**（`api/LightBlock` 的 `LightPushBlock` + `packLightPushBlock`）：
   实现**调用** `packLightBlock`（同一个遍历、同一套规则），再把三个字段搬进 push 的布局——
   两次遍历就是"同一个场景两条路径照出不同亮度"的入口。128B 的四个槽位：`ambient` / **`projparms`** /
   `dirs[3]` / `cols[3]`，静态断言钉死（含三个偏移）。
2. **`projparms` 是保留位，且保持全零**：它是"从深度缓冲重建视图位置"的程序要的
   near / far / proj[0][0] / proj[1][1]，而**随 SDK 发布的程序都不读它**（引擎自己的光照程序采样
   G-buffer 的视图位置附件，`builtin_deferred_lighting.frag` 在声明处就写明"this program does not
   read it"）。参考实现为透视相机填了它、没人读——"承诺了效果却没有效果"正是本项目一直在点名的
   失败家族；这一片**不发明读者**，把保留位留成可见的保留位。
3. **按"每次调用"推**（`api/ContentPass::recordScreenDraw`）：`packLightPushBlock(draw.lights, draw.camera, …)`
   的 `draw` 是**这一趟全屏调用**（相机与灯都是它宣布的），空灯单 ⇒ 补光（同前向路径的规则），
   丢弃**不上报**（丢报是内容路径的，参考实现亦然）。

**顺带修掉一个真洞（本片夹具当场撞到）**：`ContentPipeline::createScreen` 从不建
"没有输入时的 pipeline layout"，于是 `layoutFor(0,0)` 返回空 ⇒ **任何"只读 push、不声明输入"的全屏 pass
都被拒**（"its pipeline could not be built"——那是一条从未被请求的管线，而不是失败）。修法：屏幕层也建一份
"只有 push range、没有集合"的布局，`layoutFor(0,0)` 返回它；M4a 那条"全屏层没有布局"的断言按此更正。
"全屏程序只读 push"是合法调用（本片的设备用例就是这样），不是边角。

| 文件 | 是什么 |
| --- | --- |
| `api/LightBlock.hpp` / `.cpp` | `LightPushBlock`（128B：ambient / **projparms 保留** / dirs[3] / cols[3]，静态断言 + 偏移）+ `packLightPushBlock`（**复用** `packLightBlock` 的遍历，只换布局） |
| `api/ContentPass.cpp` | `recordScreenDraw` 的 push 从"128 个零字节"改成打包后的灯；`kFullscreenPushBytes = sizeof(LightPushBlock)`（+ 静态断言 128） |
| `api/ContentPipeline.cpp` | `createScreen` 建"push-only"布局（无集合）；`layoutFor(0,0)` 不再返回空 |
| `tests/test_vsg/LightBlockTest.cpp` | +2 用例：无设备（push 与 UBO 逐字段一致、`projparms` 全零、三个偏移、无相机 ⇒ 空 push、空灯单 ⇒ 补光）+ 真设备像素（**一趟三个全屏调用、三个视口**：带 0 灯朝观察者 = 0.5/0.1/0.1；带 1 同灯反向 = 只有 ambient；带 2 不宣布 = 补光 0.15；且 `ChannelIgnored` 计数为 0——全屏路径不上报丢弃） |
| `tests/test_vsg/ContentPipelineTest.cpp` | M4a 那条"全屏层 `layoutFor(0,0)` 为空"的断言更正为"返回 push-only 布局、集合为空" |

| 规则 | 结论 |
| --- | --- |
| **两种表示，一份打包** | 变异 P1（把 UBO 的字节直接塞进 push，两个布局混用）⇒ 无设备用例红 + 设备用例带 0 从 `(128,26,26)` 变 `(26,26,26)`：方向/颜色整体错位，太阳的项落进了保留位 |
| **内容真的有到** | 变异 P2（push 保持全零，即 M4b 的形态）⇒ 三条带全 `(0,0,0)`（未定义 push 内存"常常也读成零"，所以**反向**断言才是有用的方向——本条正是"M4b 的形有证据、内容没有"的实证） |
| **按调用推** | 变异 P3（整趟 pass 只推第一调用的灯）⇒ 带 1/2 变成带 0 的颜色 `(128,26,26)` |
| **阶段是 ABI 的一半** | 变异 P4（push 发到顶点阶段）⇒ **0 VUID**、带 1/2 仍显示带 0 的颜色：片元阶段从没读到新 push，静默失败——正是"阶段写错看不出"的实测 |

| 变异反证（全部实测） | 结果 |
| --- | --- |
| P1：push 用 UBO 的布局 | 2 条红（无设备 + 设备带 0 错位） |
| P2：push 全零 | 1 条红（三条带全黑） |
| P3：push 按 pass 推 | 1 条红（带 1/2 用带 0 的灯） |
| P4：push 发到顶点阶段 | 1 条红（带 1/2 仍是带 0 的颜色；**0 VUID**，静默） |

| 证据 | 结论 |
| --- | --- |
| 套件 | `test_vsg` 全量 **547 用例 / 87 套件全绿**（+2 用例） |
| 门禁 | 插件目标与全仓 `ninja` 零 error；强制验证层整仓 **0 VUID**；再加同步验证仍 **0 SYNC-HAZARD**；hygiene 0 / 814 文件；`check_diagnostic_formats.py` 0 / 39；`check_doc_symbols.py` 通过 |

**本片留下的口子（登记，不假装解决）**：

* **`projparms` 没有读者**：保留位按 SDK 的声明保持零；将来若出现"从深度重建视图位置"的程序，
  它的 near/far/proj00/proj11 必须**来自相机自己的投影矩阵**（`CameraSnapshot::projection` 的两个对角 +
  near/far），而不是在这里二次发明一套公式。
* **全屏路径的丢弃报告**：按参考实现，全屏调用不报（只有内容路径报）；若将来全屏侧也要报，
  `Scope::lights_dropped` 是现成的 episode 状态，但语义要先钉（"谁宣布的灯"在两个路径里是同一条契约）。
* **`shadowedDeferredLightProgram` 的 map/块（binding 5/6）仍未接**：那属于场景桥的 ABI 债（§11.16u），
  本片只填了 push。
* 前一版（§11.16v）留下的三条口子（`TargetFacts::shadow` 生产侧、`params.y/z` 语义、场景桥 ABI 债）不变。

### 11.17 下一步

| 项 | 内容 |
| --- | --- |
| ~~M0~~ | **已完成（2026-09-21）**：core 骨架 + 证据夹具 + 分配门禁 + 诊断出口 |
| ~~M1a~~ | **已完成（2026-09-21）**：设备地板策略（core）+ 物理设备探测（api） |
| ~~M1b~~ | **已完成（2026-09-21）**：自有窗口会话、空帧提交与呈现、槽数逐帧学习、按证据退役 |
| ~~M1c~~ | **已完成（2026-09-21）**：宿主句柄采纳、会话移动（`Keep` / `Move` / 失败退回 `Rebuild`）、句柄代次 |
| ~~M2a~~ | **已完成（2026-09-21）**：流身份与编辑计划（`planGeometry`）、几何别名登记、材质 arena、每帧 ring |
| ~~M2b~~ | **已完成（2026-09-21）**：变体池（按身份、有界、id 不复用）+ 每 pass 状态注册表（动态层只花 set 命令） |
| ~~M2c-1~~ | **已完成（2026-09-21）**：共享上传（`api/StreamUploads`：canonical 通道 + 索引键归一化；派生/自定义通道响亮拒绝） |
| ~~M2c-2a~~ | **已完成（2026-09-21）**：每帧块存储（`api/BlockStorage`：一缓冲三区域、slab 旋转、材质稳态零写、拒绝不增长），真设备用例 |
| ~~M2c-2b-1~~ | **已完成（2026-09-21）**：块描述符（`api/BlockDescriptors`：一个 set + 三个 dynamic offset、对齐即拒绝、repoint 留布局），真设备用例 |
| ~~M2c-2b-2a~~ | **已完成（2026-09-21）**：内容管线与着色器（一身份一对象、动态半声明含 viewport/scissor、编译失败即拒绝），无设备用例 |
| ~~M2c-2b-2b-1~~ | **已完成（2026-09-21）**：绘制录制（`api/ContentDraw` + `api/StateCommands`：两个“仅当”、动态槽、引擎约定），无设备用例 |
| ~~M2c-2b-2b-2a~~ | **已完成（2026-09-21）**：无窗口设备缝（`api/Device` + `api/DeviceFeatures`）+ 离屏目标与读回（`api/OffscreenTarget`：CLEAR、TRANSFER_SRC【彩色收尾见 §11.16n】、capture 节点、`probe()`），真设备无窗口用例 |
| ~~M2c-2b-2b-2b~~ | **已完成（2026-09-21）**：整条栈画三角形到离屏目标并断言像素（三重验证齐） |
| ~~M2c-3~~ | **已完成（2026-09-21）**：内容经 `Session` 进帧（内容根 + 设备 + recompile；窗口像素证据） |
| ~~M3a~~ | **已完成（2026-09-21）**：清屏/加载策略（`core/ClearPlan`：bootstrap 全清、额外附件透明黑、保留的深度永不清、反转 Z 远平面 0.0），无设备用例 |
| ~~M3b~~ | **已完成（2026-09-21）**：多附件目标与逐附件读回（`api/OffscreenTarget::TargetLayout`：MRT + 深度、`capture(i)`/`probe(i)`），真设备像素用例（双输出着色器分色） |
| ~~M3c~~ | **已完成（2026-09-21）**：借用深度落到目标层（不建镜像 / LOAD / 提升撤销 + 事实即计划），真设备用例（颜色 + 深度双重证据）；顺带补上设备地板的**扩展**一半与深度读回（`core/DepthProbe`） |
| ~~M3d-1~~ | **已完成（2026-09-21）**：帧的收集段（`core/FrameRecorder`：协议先行、arena 独占快照、scope 属性 vs 逐绘制消费、三种裁决 + 计数），14 个无设备用例 |
| ~~M3d-2~~ | **已完成（2026-09-21）**：帧的编译段（`core/FrameGraph`：稳定拓扑序 + 环的 SCC 跳过；`core/FrameCompiler`：normalize + target/depth 求解 + `CompiledFrame`），10 + 12 个无设备用例 |
| ~~M3d-3a~~ | **已完成（2026-09-21）**：执行段第一片（`api/VsgExecutor`：一个 pass scope = 一个 render pass，逐 pass 清屏值，记录顺序 = 计划顺序有像素证据），真设备用例 2 个 |
| ~~M3d-3b-0~~ | **已完成（2026-09-21）**：计划带 pass 侧管线事实（`color_attachments` / `depth_sampleable`）+ `OffscreenTarget::shape()` + 执行器核对计划与 target（§11.16f） |
| ~~M3d-3b-1~~ | **已完成（2026-09-21）**：内容按 pass 放置（`PassContent` 接缝 + 非法内容上报），内容层用 `ContentDraw` 等录的三角形经计划上屏，真设备像素证据（§11.16g） |
| ~~M3d-3b-2~~ | **已完成（2026-09-21）**：三张事实表的定义与查找语义（`api/ContentFacts`：身份 + revision 是键、三种 miss、通道↔布局与块↔ABI 两条规则），无设备用例（§11.16h） |
| ~~M3d-3b-3~~ | **已完成（2026-09-21）**：每绘制 ABI 块的打包（`api/DrawBlock`：列主序 + 平移在 12..14 + `params.x` = opacity），非对称矩阵用例 + 变异反证（§11.16i） |
| ~~M3d-3b-4~~ | **已完成（2026-09-21）**：几何的通道走查（`api/GeometryFacts`：通道顺序 = 绑定顺序、索引键归一化为整 buffer、切片上传自己的段、起点必须一致），无设备用例（§11.16j） |
| ~~M3d-3b-5~~ | **已完成（2026-09-21）**：程序与材质两张数据源（`api/ContentSources`：两段 + 一个入口点才算内容管线；无材质 = 身份为空的默认条目；块的成员才是载荷），无设备用例（§11.16k） |
| ~~M3d-3b-6~~ | **已完成（2026-09-21）**：内容层（`api/ContentPass`：逐命令按表录取、查不到就拒绝并上报；块 + 流 + 绘制；`findMaterial` 改为只按身份查），真设备用例（像素 + 缓存计数）（§11.16l） |
| ~~M3d-3b-7~~ | **已完成（2026-09-21）**：多布局 scope（`api/ContentPass`：半片 = 程序 × 修订 × 布局，池与注册表共享，选不中报“是哪一项”），真设备用例（双色像素 + 绑定计数 + 两条拒绝消息）（§11.16m） |
| ~~M3d-3b-8~~ | **已完成（2026-09-21）**：pass 输入的采样绑定（`core/FrameCompiler` 的输入表 + `api/ContentPipeline` 的采样集布局 + `api/ContentPass` 的绑定 + `api/OffscreenTarget` 的“彩色附件按可采样收尾”），真设备像素用例 + 四条变异反证（§11.16n） |
| ~~M3d-3c~~ | **已完成（2026-09-21）**：执行段第三片（`api/WindowTarget` 把窗口变成执行器认识的目标：一张图、一次清、一个稳定视图；`api/ViewBlock` 定下视图块的四处约定并把 SDK 裁剪空间折进设备约定；`api/Session` 的帧时钟 + `frameSeconds()` + 帧图接缝），真设备像素用例（计划清屏 + 视图块进着色 + 一次 present）+ 五条变异反证（一条登记为不可观测）（§11.16o） |
| ~~M4a~~ | **已完成（2026-09-21）**：全屏绘制调用的内容侧（键的 `kind` + `api/ContentPipeline::createScreen`（set 0 = 采样集、push 片元 128B）+ `api/ContentSources::buildScreenProgramFacts`（引擎顶点阶段 + 宿主片元阶段）+ `api/ContentDraw::recordScreen`（`Draw(3)`）），真设备像素用例（随 SDK 发布的 screen copy 的 PiP 拷贝 + binding i = 附件 i）+ 四条变异反证（§11.16p） |
| ~~M4b~~ | **已完成（2026-09-22）**：全屏绘制的计划侧（`CompiledDraw::dynamic` + 全屏调用的深度策略 = `Disabled`（正典三角形在 reverse-Z 远平面）；`api/ContentPass` 按 `kind` 找半片、采样集在 set 0、push 128B 片元（内容全零）；真设备像素用例（计划驱动的 PiP 拷贝）+ 两条变异反证（§11.16q） |
| ~~M4c~~ | **已完成（2026-09-22）**：窗口合成（`tests/test_vsg/WindowCompositionTest.cpp`：四趟 pass 一帧——离屏清屏 / 同形离屏场景 / 窗口场景（拥有那一次清）/ 窗口全屏覆盖层，真设备像素三条 + 计数器五条 + 执行器按计划顺序放置）；顺带把 §11.16o 的两条口子做成用例，其中"窗口/离屏同键变体"被证明是错的 ⇒ **设备格式进键的兼容性半边**（`core::TargetShape` / `core::RenderPassCompatibility` + `TargetShape::compatibility()` + 两个目标各自上报），并纠正 §11.16o 里"稳定视图分族"那条理由；修掉 capture 的跨帧写-写（声明顺序的屏障）+ 三条变异反证 + 测试宿主窗口去重（§11.16r） |
| ~~M5a~~ | **已完成（2026-09-22）**：多写者离屏目标（LOAD 变体）——`LoadOpVariantKey` 改成变体的名字 + `core::loadOpVariantOf` 装配；`OffscreenTarget` 按变体建/缓存渲染通道（依赖列表逐位相同、framebuffer 共享）+ `passVariantCount()` + `written()`；执行器把计划的 `clear`/`bootstrap`/`depth_preserved` 交下去；真设备像素用例（一帧两趟：清 + LOAD，两个网格都在）+ 四条变异反证（§11.16s） |
| ~~M5b~~ | **已完成（2026-09-22）**：输入表带上深度半边（"阴影脊柱"）——`CompiledInput::depth_sampleable` 由 `core::depthPlan` 解析（采样者与被采样者同一个答案）；`core::depthFinalLayout` 给"纯深度 + 可采样"第三种收尾布局（`ShaderReadOnly`），`loadOpVariantOf` 的深度起始布局改成 `depth_steady`（借用者命名的是出借者的布局）；`PipelineKey::sampled_depth_count` + `SampledKey{colors, depths}` + NEAREST 深度采样器；深度捕获屏障按 `depthSteadyLayout()` 进/出（原来写死的布局是 `VUID-VkImageMemoryBarrier-oldLayout-01197` 的谎报，还会让采样读到全 0）；真设备用例（探针 0.5/0.0 + 像素灰/黑/清屏色）+ 无设备用例两条 + 四条变异反证（§11.16t） |
| ~~M5c-1~~ | **已完成（2026-09-22）**：前向光照块——`api/LightBlock`（`VineLightsBlock` 112B + 世界→视图换算 + 归一化 + 三槽规则 + 补光不计数 + 无相机 ⇒ 空块）；`BlockStorage` 第 4 区域 + `writeLights`；`BlockDescriptors::kLightsBinding = 3` + 四个动态偏移；`ContentPass` 每绘制调用写一块 + "每 episode 一次"的丢弃报告（`ReportOnce`）；6 条无设备用例 + 1 条真设备四带像素用例（带 0 朝向观察者的太阳 / 带 1 反向 / 带 2-3 装不下的灯 = 补光 + 一条报告）+ 四条变异反证（其中 M1 实测"轴对齐相机看不出缺旋转"⇒ 判据是算术用例）（§11.16u） |
| ~~M5c-2~~ | **已完成（2026-09-22）**：阴影块——`ShadowFacts`（谁的 map + 生产者矩阵）进 `TargetFacts`/`CompiledInput`/`CompiledPass`，`FrameCompiler::resolveShadow` 三个事实先来先用；`LightRef` += 身份/投影开关/bias；`api/ShadowBlock::packShadowBlock`（身份匹配、启用/投影/类型/槽检查、`light_vp * inverse(view)` 列主序、`params` 四元组）；`BlockStorage` 第 5 区域 + `kShadowBinding = 4`；`directionalSlotOf`（与灯块打包同一次遍历）；**按实测删除 `PipelineKey::shadow_bound`**（本后端 ABI 里 map 走采样输入、块恒在块集、开关是运行期值 ⇒ 它只会白拆管线）；4 条无设备 + 1 条真设备四带像素用例 + 7 条变异反证（§11.16v） |
| ~~M5d~~ | **已完成（2026-09-22）**：全屏路径的 128B push——`LightPushBlock`（128B，`projparms` 保留为零）+ `packLightPushBlock`（复用 `packLightBlock` 的遍历、只换布局）；`recordScreenDraw` 按每次调用推；`createScreen` 建 push-only 布局（"只读 push"的全屏 pass 不再被拒）；2 条用例（无设备 + 真设备三视口像素）+ 4 条变异反证（其中"push 全零"与"发错阶段"分别是内容缺失与静默失败的实证）（§11.16w） |

M1 起每条相位都要同时给出：像素/计数器断言（`PhaseTable` + `PixelProbe`）、不得移动的计数器
（`expect` 为“不变”的那些）、以及需要时的一段 `AllocationGate` 窗口。

代码落地前的约定：新增 `core/` 文件会被 `v_add_plugin` 的 `GLOB_RECURSE` 自动收进插件，
`tests/test_vsg` 需要显式加源文件（两份 CMakeLists 各一处）。
