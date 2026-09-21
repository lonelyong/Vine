# vsg 后端"从零重新实现"设计（提案）

> 状态：**设计提案 v2（2026-09-21）**，核心层已开始落地（见 §11），**不改动**现有 `gfx_backend_vsg`。
>
> **实施进度（M0 第一片，2026-09-21）**：`core/` 的 8 个类型已落地，22 个无设备用例绿
> （`tests/test_vsg/BackendCoreTest.cpp`），`core/` 的 include 边界已由
> `scripts/check_include_hygiene.py` 机器校验（全树 0 findings）。详见 §11。
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
| `api/OffscreenTarget` | 一张图 + 一个 render pass（CLEAR、`initialLayout = UNDEFINED`、`finalLayout = TRANSFER_SRC`）+ framebuffer + RenderGraph（`clearValues` + **`renderArea`**）+ capture 节点（`CopyImageToBuffer` + buffer barrier）→ `probe()` 给出紧凑 RGBA8 的 `core::PixelProbe` |
| `tests/test_vsg/OffscreenTargetTest.cpp` | 3 个真设备用例，**不需要窗口也不需要显示服务器**（lavapipe 头部路径） |

| 规则 | 结论 |
| --- | --- |
| 证据通道 | 读回是命令图的一部分（`capture()` 由调用方挂在 render graph **之后**）：同一命令缓冲、同一队列、顺序有保证；帧提交 + device idle 之后才 `probe()` |
| 为什么不需要 barrier | `initialLayout = UNDEFINED` + 每帧 CLEAR：上一帧内容被丢弃是**有意**的，这正是"读回不用 barrier 舞蹈"的代价与收益（该目标不支持 LOAD 上一帧——相位本来就每帧清） |
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
| ~~M2c-2b-2b-2a~~ | **已完成（2026-09-21）**：无窗口设备缝（`api/Device` + `api/DeviceFeatures`）+ 离屏目标与读回（`api/OffscreenTarget`：CLEAR、TRANSFER_SRC、capture 节点、`probe()`），真设备无窗口用例 |
| ~~M2c-2b-2b-2b~~ | **已完成（2026-09-21）**：整条栈画三角形到离屏目标并断言像素（三重验证齐） |
| ~~M2c-3~~ | **已完成（2026-09-21）**：内容经 `Session` 进帧（内容根 + 设备 + recompile；窗口像素证据） |
| ~~M3a~~ | **已完成（2026-09-21）**：清屏/加载策略（`core/ClearPlan`：bootstrap 全清、额外附件透明黑、保留的深度永不清、反转 Z 远平面 0.0），无设备用例 |
| ~~M3b~~ | **已完成（2026-09-21）**：多附件目标与逐附件读回（`api/OffscreenTarget::TargetLayout`：MRT + 深度、`capture(i)`/`probe(i)`），真设备像素用例（双输出着色器分色） |
| ~~M3c~~ | **已完成（2026-09-21）**：借用深度落到目标层（不建镜像 / LOAD / 提升撤销 + 事实即计划），真设备用例（颜色 + 深度双重证据）；顺带补上设备地板的**扩展**一半与深度读回（`core/DepthProbe`） |
| M3d | 目标：执行器按 `Schedule` 机械落地（`FrameRecorder` / `FrameCompiler` / `VsgExecutor`，见 D2/§2.4） |

M1 起每条相位都要同时给出：像素/计数器断言（`PhaseTable` + `PixelProbe`）、不得移动的计数器
（`expect` 为“不变”的那些）、以及需要时的一段 `AllocationGate` 窗口。

代码落地前的约定：新增 `core/` 文件会被 `v_add_plugin` 的 `GLOB_RECURSE` 自动收进插件，
`tests/test_vsg` 需要显式加源文件（两份 CMakeLists 各一处）。
