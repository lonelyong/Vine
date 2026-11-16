# graphics 与两后端的重新设计（第二轮：概念 / 能力语义 / 实现 / 规范）

> **状态**：非权威提案（第二轮）。**未改一行代码**，等批准。
> **日期**：2026-11-16
> **第一轮**：`graphics-2026-11-16-redesign.md`（能力清单、逐项审核、接缝与词汇、十批计划）。
> **本轮范围**：概念语义（视图 / 区 / 目标 / pass 作者形状）、能力设计（哪些动词真的承载能力）、
> 实现（稳态分配、死存储、契约句）、**统一编程规范审查**、以及**与优秀框架的逐概念对照**。
> **不重复**：第一轮的能力清单（A 表）、逐单元审核（B）、横切（C）、结论表（D-01…D-28）与十批计划仍然有效；
> 本文只写**增量、改判与更正**，并在 §3 给出与既有两份审计的对账。
> **判据**：`G1`–`G13` + 第一轮的三元组规则 + 本轮新增的"框架对照"判据（§2.1）。

---

## 1. 结论摘要（先读这一节）

1. **第一轮的身份/接缝结论成立，但概念层的问题还在。** 2026-11-11 的 API 审计列了 32 条（`P1`–`P32`）与
   10 条缺失能力（`M1`–`M10`），其中 4 条**至今还钉在门禁的 baseline 里**（`check_api_style.py` 的
   `PassDesc` / `PassChain` / `RenderDevice` 三处 + appfw 一处）。也就是说：那条审计的**计划停摆了**，
   而它的问题仍然活着。本文把它的状态逐条对账（§3），并把它没做完的部分接过来。
2. **"视图"这个设计名词没有类型**（`E-01`）。设计页说 `View = 相机值 + 视口 + 层掩码`，代码里它散成
   `CameraSnapshot`（7 个字段）+ `CollectView`（5 个字段，其中 3 个与前者重复）+ `Viewport` + 一个
   `layer_mask` 参数。两个"视图值"并存，是 `P14` 五年没合的原因，也是 `DrawDescription` 要带三个字段
   而不是一个的原因。**目标**：一个 `View` 值（Filament / O3DE / bgfx 都是这么做的）。
3. **"表面"一个词指两个东西**（`E-02`）：`RenderSurface` 是**区**（尺寸 + 呈现表），平台表面是
   `createSurfaceTarget` 的 `VkSurfaceKHR` / `HWND`。类型自己的文档、测试名（`ASecondAreaIs…`）、
   动词（`announceAreaSize`）都说 "area"，只有类型名说 "surface"。**目标**：`RenderSurface` → `RenderArea`，
   "surface" 留给平台。
4. **一个 pass 的"落在哪"仍有 3 种声明 + "发布什么"还有 3 种**（`E-03`，旧 `P3`）。再加上四个
   `setRenderTarget` 同名不同义（`RenderDevice` / `RenderPass` / `FrameRecorder` / 测试双），"目标"这个词
   是当前词汇表里最乱的一处。**目标**：`Target` 值（`Window | Explicit | Transient`）+ 一个 `Output` 值，
   并把 target 的四个角色（声明 / 形状 / 计划 / 对象）写成词表。
5. **`setRenderTarget` 在 vk 里是死存储**（`A-01`）：`Device.cpp` 只把指针写进 `Impl::target`，
   **全文件没有一处读**；vsg 则真的 `adoptHostTarget()` 建对象。同一动词、两种语义，其中一种什么也不做，
   而且留下一个可悬垂的裸指针。这是本轮最结实的一条"实现不合理"。
6. **稳态帧仍有分配**（`I-01`）：`RenderPass::describe` / `ScreenPass::describe` 每个 pass 每帧构造
   一个**局部** `std::vector<LightValue>`，`reserve` + `push` + move——每 pass 每帧一次 alloc/free，
   与 `R24` / `N6`（帧内 arena 外零分配）相反。灯属于**场景**，不属于 pass；同一场景的 N 个 pass
   还会各拷一遍。
7. **规范层面：九道门禁全绿，但"法"与"习惯"有三处不一致**（§7）：字段注释是 `///`（545 行）而
   规范写"prefer multi-line"；26 处 section comment 与"禁止 section comment"冲突；94 个 `enum class`
   里 37 个没有底层类型。三条都能用一次**规范修订 + 一次注释清扫**收掉，不需要动行为。
8. **框架对照的结论不是"照抄"**：render graph（UE RDG / O3DE RPI / Frostbite）、bindless
   （D3D12 heap / `VK_EXT_descriptor_buffer`）、`Result<T>` 全面化、多帧在飞——四条都**不借**，
   理由各自是 `G8`（没有消费者）或 `G10`（真实成本算不过来）。借的是**值的形状**：`View`、`Features`/`Limits`、
   pass 的不可变描述、错误带 code + origin（§8）。

---

## 2. 方法

### 2.1 三条判据（在第一轮之上新增一条）

1. **概念唯一**（`G1`）：一个设计名词要么有类型，要么在设计页里被"没有类型，因为……"显式解释。
   "散在三个字段里"不算解释。
2. **能力可证**（`G4` / `G11`）：每个公开动词要么有生产消费者，要么有用例证明它的可观察效果；
   **有动词、无效果**（`A-01`）比"没有这个动词"更糟。
3. **框架对照**（本轮新增）：同一个概念，若一个成熟框架有更简单的形状，**必须**在本文里被引用一次，
   并给出"借 / 不借 + `G#`"的判决。引用不是权威，是**反驳自己的工具**：如果一个形状在四个框架里
   都不存在，我们说它"业界没有"之前先问"我们是不是把两件事塞进了一个词"。

### 2.2 与前两份文档的关系

| 文档 | 状态 | 本轮怎么用 |
|---|---|---|
| `graphics-2026-11-16-redesign.md`（第一轮） | 有效 | A/B/C/D 与十批计划不变；本文 §3 更正它的两个数字；§9 给增量 |
| `graphics-2026-11-11-next-api.md`（P1–P32 / M1–M10） | 调研，未采纳 | §3 逐条对账；仍开的部分并入本文的 `E`/`A`/`S` 编号与批次 |
| `scripts/check_api_style.py` 的 4 条 baseline | 现行门禁 | 它是"旧审计停摆"的**机器证据**；本轮把它们分配到批次（§10） |

### 2.3 本轮用的量化事实（都可用命令复核）

| 事实 | 值 | 命令 |
|---|---|---|
| `RenderPass` 的 setter/adder | **35** | `grep -cE '^\s{4}(void\|bool) (set\|add\|clear\|remove)[A-Z]' RenderPass.hpp` |
| `PassDesc` 里的 `std::function` | 1（`before_draw`） | `grep -n std::function PassDesc.hpp` |
| `PassChain::OverlayLayout` 的签名 | `function<void(RenderPass&, int, int, double)>` | `PassChain.hpp:394` |
| `tag` 的写 / 读 | **8 写 / 0 读** | `grep -rn '\.tag\b' src tests`（另有 appfw `EventBus` 同名，无关） |
| vk `Impl::target` 的写 / 读 | **1 写 / 0 读** | `grep -nE 'impl->target([^s_]\|$)' Device.cpp` |
| `Device::setRenderTarget` 的 vk 测试 | **0** | `grep -rn setRenderTarget tests/test_vk` |
| 每 pass 每帧的灯向量 | 1 次局部 `vector` 构造 | `RenderPass.cpp:242`、`ScreenPass.cpp:82` |
| `enum class` / 带底层类型 | **94 / 57**（37 个没写） | `grep -rhE 'enum class [A-Za-z]' src/viz/graphics src/plugins --include=*.hpp` |
| section comment | **26** | `grep -rE '^\s*//\s*[-=*]{3,}' src/viz/graphics src/plugins` |
| SDK 里 `///` 字段行 / `/**` 块 | **545 / 1627** | 同上两个 grep |
| SDK 里返回 `bool` 的声明 | **198** | `grep -hE '^\s{4}(\[\[nodiscard\]\] )?(virtual )?bool '` |
| `std::span` 的使用 | **156** | `grep -rh std::span src/viz/graphics/sdk src/plugins --include=*.hpp` |
| `reinterpret_cast` | 50，前三个文件占 25 | `grep -rc reinterpret_cast …`（`WindowSystem` 15 / `VsgHostWindow` 5 / `PipelineCacheFile` 5） |
| 债务：public docs / 诊断 code / api-style | 974 / 22 处未编码 / 4 条 baseline | 三个门禁的输出 |

---

## 3. 与既有审计的对账（旧 `P#` / `M#` 的今天）

**先更正第一轮的两处数字**（诚实优先）：

- 第一轮写"20 条 engine 侧报告没有 code"——**实际是 22 处调用、6 个文件**：
  `RenderEngine.cpp` 13、`RenderSurface.cpp` 5、`ChainBuilder.cpp` 1、`DiagnosticsRoute.cpp` 1、
  `SceneView.cpp` 1、appfw `Viewer.cpp` 1。`diagnostic_code_baseline.txt` 是"每文件上限"，
  不是"条数清单"，我第一轮把它读成了条数。
- 第一轮写"欠文档 baseline 976"——**当前 974**（期间有两条被偿还）。本文一律用当前值。

### 3.1 `P1`–`P32`

| # | 主题 | 今天的状态 | 判决 / 去处 |
|---|---|---|---|
| P1 | 一个 pass 两种表示（`PassDesc` / 可变 `RenderPass`） | **仍开**：`RenderPass` 有 35 个 setter，demo 仍在用 | **本轮采纳** → `E-04`（一个作者形状 + 只读活句柄） |
| P2 | 值里有回调（`before_draw`） | **仍开**，且**有 2 个生产消费者**（`ChainBuilder` 的 gizmo / FPS） | 旧审计说"没有消费者"，**过时**；本轮改判为 `E-05`（回调归对象，能力保留） |
| P3 | 一个 pass 的目标四种说法 | **仍开且更糟**：`writes` / `writes_image` / null / `output_target` / `output` / `output_name` / `depth_view` | `E-03`（`Target` + `Output` 两个值） |
| P4 | `PassKind` 与投影重复 | **被本轮驳回**：`ScreenPass` 没有相机，只有 `source` + `program`，它是另一种**执行**，不是投影取值 | `E-11`：保留 `PassKind`（旧审计的判据只看了名字） |
| P5 | 顺序概念四套 | 仍开，但分两个作用域（pass 之间 / pass 之内） | 保留，写进词表；不合并（`G8`） |
| P6 | 值里的装饰字段（`tag`） | **仍开**：8 写 0 读 | `E-06`：删 |
| P7 | 内容层产出渲染命令 | **已解决**（快照 + 纯 select，5b-2b / W9a） | 关闭 |
| P8 | 默认程序是环境态 | **已按 ADR-2026-11-15 解决一半**：默认程序归**驱动者**（两个引擎各画各的） | 关闭；"归计划/视图"**驳回**（两个驱动者需要不同默认值是真实的） |
| P9 | 内容可被两个入口改 | **已解决**：`bindPassContent` 写的就是声明的唯一存储 | 关闭 |
| P10 | `RenderEngine` 49 个公开方法 | **仍开**：公开面未动（第一轮选择拆内部机制，不拆公开面） | 第一轮 `D-08`；本轮补"公开面按五组文档化"，不删动词（`G8`） |
| P11 | 观测散成 9 个函数 | **部分解决**：`FrameResult` + `FrameReport` 已存在；计数仍散 | `E-10`：两个值 + 一张表，停止新增 accessor |
| P12 | 七个名字指"画东西的东西" | **部分解决**：三面接缝清楚；"区/表面"仍混 | `E-02` 修掉最后一处（`RenderArea`） |
| P13 | 五个名字指"一次画" | **部分解决**：`EngineFrame` / `FrameValue` 已删；`FrameTable` / `FrameDescription` / `FrameResult` 是三个**角色** | 保留三角色，写词表（`E-10`） |
| P14 | `CollectView` 名为收集实为视图 | **仍开** | `E-01`（`View`） |
| P15 | `ShaderProgram` 是可变异阶段表 | 仍开，但**值半边**（`ShaderProgramDescription`）已经承担帧内语义 | `E-07`：保留，写清两半；**不建 builder**（`G8`：没有消费者） |
| P16 | 所有权词汇混用 | 仍开（规则没写在 SDK 里） | `S-D`（一段文字，零代码） |
| P17 | 0 当"未指定"（`TransientShape` 尺寸） | 仍开 | 并入 `E-03`（`Target` 值用 `optional` 尺寸） |
| P18 | 三层创建、顺序靠注释 | 仍开；但"引擎借用设备"是 ADR-2026-11-15 的决定 | **驳回**（`Session::open` 会把共享设备的借用关系藏起来）；文档化顺序 |
| P19 | 声明里出现后端概念（格式 / device pixels） | 仍开；第一轮已把它列为可选批次 | 第一轮 `D-12` + `E-03`（目标值隔离格式） |
| P20 | appfw `RenderControl` 18 个方法 | 不在本次范围（appfw） | 记为 appfw 的 backlog，不夹带 |
| P21 | 全局着色模式开关 | 仍开，但它是**链**的设置（`PassChain::setShadingMode`）不是设备 | 保留；文档写"链级程序覆盖" |
| P22 | `reportEngineProblem` 是公开缝 | **仍开且确有宿主消费者**（appfw `Viewer.cpp:83`） | `A-06`：给诊断加 `origin`，计数按来源可分开 |
| P23 | 缺少"声明后能问回来"的值 | **已解决**（`frameTable` / `frameResult`） | 关闭 |
| P24 | 生命周期 API 混进内容 API | 仍开（`releaseContent` 在引擎面） | 保留（`G8`：宿主确实要用它），在 `graphics-content-lifecycle.md` 写清它是"驻留提示" |
| P25 | 描述类型后缀 6 种 | 仍开 | `S-A`：只约束**新**类型（`Config` / `Capabilities` / `Snapshot`），不扫旧名 |
| P26 | 大量 `bool` 返回 | 仍开（198 个声明） | `S-E`：不回填存量，新 API 用值表达失败 |
| P27 | 错误表达三套 | 仍开（`ReadbackResult` 仍在 baseline） | `S-E` + `A-03`（读回先试点） |
| P28 | 回调散在 SDK | 仍开（`before_draw` / `OverlayLayout` / sink） | `E-05`：值里零回调；sink 是合法的（会话配置） |
| P29 | `OverlayLayout` 交出可变 pass | 仍开 | `E-05`：布局返回 `Viewport` 值 |
| P30 | 访问器与命令共用 `get` | **已解决**（graphics 0 新增；appfw 的 `getStringAsync` 是命令，baseline 里） | 关闭（规则按"访问器 vs 命令"记进规范） |
| P31 | 值可以有成员，不能有 setter | 现行且基本遵守 | 保留为规则 |
| P32 | 预设 = 返回值的自由函数 | 现行 | 保留为规则 |

### 3.2 `M1`–`M10`

| # | 缺什么 | 今天 | 判决 |
|---|---|---|---|
| M1 | 能力**协商** | `prepare` 已经回答"这一帧要建什么 + 建成了多少" | **改判为已有**：把 `prepare` 命名为协商入口；缺的只是"逐项原因"（`A-07` 可选） |
| M2 | 管线预热查询 | `prepare` 已落地（`T33` / `T35`） | 关闭 |
| M3 | 异步编译显式语义 | 预热 + "建不了就报并跳过" | 关闭（不做异步，`G8`） |
| M4 | 帧节流策略 | 无 | backlog（不是本次） |
| M5 | 计划回读 | `frameTable()` | 关闭 |
| M6 | replay 后端 | 无（`OwnedFrame` 无消费者） | backlog（`T18`） |
| M7 | 查询吃视图值 | `FrameResult.query` | 关闭（`poses` 待 F10） |
| M8 | 内容句柄化 | `ContentId` / `ContentRef` | 关闭 |
| M9 | 单位与颜色空间在类型里 | 颜色有类型（linear `Colorf`）；长度/深度部分有（`DepthRange`） | 部分；不在本次 |
| M10 | 视图多表面 | 一个引擎一个表面（ADR-2026-11-13） | **驳回**（已裁决） |

---

## 4. 概念层再审

格式：**现象 → 证据 → 判据 → 更好的形状 → 框架先例 → 成本 / 能力影响**。
"能力影响"一律写第一轮的 `CAP-xx`；没有能力变化就写"无"。

### E-01 视图没有类型（`G1` / `G4`）

- **现象**：设计页的八个名词里有 `View`（"相机值 + 视口 + 层掩码"），代码里没有 `View` 类型。
  有两份**重叠**的视图值：`CameraSnapshot{ present, view, projection, eye, target, up }` 与
  `CollectView{ projection, view, eye, up, viewport }`；层掩码是另传的一个 `std::uint32_t`。
  于是 `DrawDescription` 带 `camera` + `collect_viewport` + `layer_mask` 三个字段，
  `PassDesc` 带 `camera` + `viewport` + `layer_mask` 三个字段，`selectSnapshot(snapshot, view, layer_mask)`
  把视图与掩码分开传。
- **证据**：`CollectView.hpp`（5 字段，3 个与 `CameraSnapshot` 重复）、`CameraSnapshot.hpp:32`、
  `DrawDescription` 的三个字段、`PassDesc::layer_mask`、`selectSnapshot` 的签名（第一轮 §3.3.4）。
- **判据**：一个设计名词没有类型，就只能靠"每个调用点自己记得带三个字段"来保证一致；两个重叠的值
  更容易漂移（`CameraSnapshot` 有 `target`，`CollectView` 没有；`CollectView` 有 `viewport`，另一个没有）。
- **更好的形状**：

```cpp
struct View {                      // 一个视图值：投影已含形状，视口是设备像素，掩码是选择
    CameraSnapshot camera{};       // present == false ⇒ 这次调用什么也不发（保持今天的语义）
    Viewport       viewport{};     // invalid = 调用方说不了（保持今天的语义）
    std::uint32_t  layer_mask{ kAllLayers };
};
```

  `DrawDescription` 用 `View view;` 替掉三个字段；`PassDesc` **保留**声明输入（拥有式 `Camera` +
  `viewport` + `layer_mask`，因为声明持有的是对象而不是快照），`describe` 时把它们拍成 `View`；
  `CollectView` **删除**，`selectSnapshot(snapshot, view)` 只收一个值；`Scene::collectRenderCommands(view)` 同。
- **框架先例**：Filament `View`（camera + viewport + visible layers + render target，一个对象）；
  O3DE `RPI::View`（camera + viewport + pass tree）；bgfx 的 per-view 状态（`setViewTransform` +
  `setViewRect` + `setViewScissor` + `setViewClear` 都按 ViewId 归属）。三者都把"视图"做成**一个单位**。
- **成本**：中（公开类型 + 全部调用点，机械）；可用一批完成，先让 `View` 与旧字段并存一版（additive），
  再把字段删掉。
- **能力影响**：CAP-08 / CAP-14 / CAP-21 / CAP-22 不变；证据仍是 `SelectTest.*`、`PassDrawTest`、
  `RenderEngineTest`，加一条"同一 View 值喂给 select 与 describe 得到同一画面"的等价用例。

### E-02 `RenderSurface` 是"区"，不是"表面"（`G1` / `G12`）

- **现象**：`RenderSurface` 的类注释第一句是 "One render area"；测试叫
  `RenderSurfaceTest.ASecondAreaIsAConstructionFailureNotASecondObject`；动词叫 `announceAreaSize`；
  尺寸叫 `surfaceWidth/Height`。而**平台表面**（`VkSurfaceKHR` / `HWND` / 交换链）由
  `createSurfaceTarget` / `WindowPresentTarget` 表达。同一个 "surface" 在两个层次上指两个东西。
- **证据**：`RenderSurface.hpp:20`、`RenderSurfaceTest`、`RenderEngine::announceAreaSize`、
  `RenderDevice::createSurfaceTarget`。
- **判据**：`G1`（一个概念一个名字）；`G12`（名字说领域：这个类型是**一块要画的面**，不是平台的 surface）。
- **更好的形状**：`RenderSurface` → **`RenderArea`**；`RenderEngine::createSurface()` → `createArea()`；
  `surfaceWidth/Height()` → `areaWidth/Height()`；`RenderSurfaceTest` → `RenderAreaTest`。
  "surface" 只保留给平台（`createSurfaceTarget`、`WindowPresentTarget`、`WindowSurface`）。
- **框架先例**：Godot 的 `Viewport` 就是这个概念（一块渲染区域，可以是窗口或纹理）——但 `Viewport`
  在本仓已经是"矩形"类型，所以用 **area** 而不是 viewport；Vulkan/D3D12/Metal 的 "surface/swapchain"
  一律指平台可绘制对象，与本类型不是一回事。
- **成本**：中（改名 + 文档 + appfw 的 `surface()` 访问器）；纯机械。
- **能力影响**：CAP-24 / CAP-25 / CAP-43 不变；证据是同一批用例改名后仍绿。

### E-03 一个 pass 的"落在哪"有三种声明，"发布什么"还有三种（`G1` / `G4`）

- **现象**：`PassDesc` 里同时有
  `writes`（显式 `RenderTarget`）、`writes_image` + `writes_shape`（transient，按名字）、空（= 窗口），
  以及 `output_target`（整目标）/ `output`（单图）/ `output_name`（按名字发布）和 `depth_view`（别人的深度）。
  读一条声明要先判断"这三种哪一种是有效的"，再判断"三种发布哪一种是有效的"。
- **证据**：`PassDesc.hpp` 的 `writes*` / `output*` / `depth_view` 段；`RenderEngine::transientTarget(name)`
  是"声明之后再取回"的隐式两阶段（旧 `P3` 已经指出）。
- **判据**：`G1`（一个事实一个字段）；`G4`（"有效组合"没有类型，只有散文）。
- **更好的形状**：两个值，各一个字段：

```cpp
struct Target {                                  // 这个 pass 画进哪里
    enum class Kind : std::uint8_t { Window, Explicit, Transient };
    Kind                  kind{ Kind::Window };
    intrusive_ptr<RenderTarget> target{};        // Explicit
    String                name{};                // Transient：身份
    std::optional<Extent> extent{};              // Transient：unset = 这一帧的区尺寸（替掉 0 的魔法值）
    std::optional<TransientShape> shape{};       // Transient：颜色/深度/采样（今天 TransientShape）
};
struct Output {                                  // 这个 pass 承诺产出什么
    String name{};                               // "" = 不发布
    bool   whole_target{ false };                // 粗承诺
    intrusive_ptr<ImageRef> image{};             // 细承诺
};
```

  `writes` + `writes_image` + `writes_shape` → `Target target`；`output_*` 三个 → `Output output`。
  `depth_view` 留下（它是"输入"，不是"落在哪"）。`Extent` 是一个 2 int 的新值（替掉
  `TransientShape` 里"0 = 这一帧的区尺寸"的魔法值；第二个消费者是区域读回，今天没有，所以它只做字段类型，
  不做机制）。
- **框架先例**：UE 的 `FRenderTargetBinding`（一个绑定 = 目标 + mip + 数组层）；O3DE `PassAttachment`
  （附件 + scope 名）；wgpu 的 `RenderPassColorAttachment`（view + resolve + load/store）——都是一条
  目标一个值，不是"三种字段任选"。发布（output）在 O3DE 里由 `PassAttachment` 的 scope 表达，
  也是**一个**概念。
- **成本**：中高（公开声明 + 编译 + 全部 preset / 测试）；可先加 `Target`/`Output` 值并保留旧字段一版，
  再删旧字段（与第一轮"并存 → 迁移 → 删旧"一致）。
- **能力影响**：CAP-18 / CAP-23 / CAP-39 / CAP-42 不变；证据：`PlanTest.*`、`RenderEngineTest.*`
  的 transient / output / input 用例（20+ 条）。

### E-04 pass 的两个作者形状（`G1` / `G4`）

- **现象**：`PassDesc` 是值，`RenderPass` 是活节点（`declaration()` 返回它的唯一存储），
  但 `RenderPass` 仍有 **35 个 setter/adder**。demo 与链预设用后者（`pass_main->setRenderTarget(target)`），
  计划用前者（`applyPlan(Plan{desc})`），`setDesc` 在两者间整份拷贝。
- **证据**：`RenderPass.hpp` 的 35 个写入口；`DemoScene.cpp:1245/1264/1336/1402`；
  `EngineCompile.cpp:861`（`existing->pass->setDesc(desc)`）。
- **判据**：`G1`（一条声明两种写法）；`G4`（"哪一种改动能被计划差分看见"没有答案——setter 改动不经过计划）。
- **更好的形状**：**`PassDesc` 是唯一作者形状**；`RenderPass` 的公开面只读
  （`declaration()`、`cost()`、`name()`）。35 个 setter 降为 `detail` 的构建入口，链 / 预设改为
  "产出 `PassDesc` → `PassChain::apply` → `applyPlan`"。需要"只改一个字段"的宿主写
  `PassDesc d = pass->declaration(); d.writes = …; chain->update(d);`（一次显式差分）。
- **框架先例**：Filament 的 `Material::Builder` / `RenderPass::Builder` + 不可变对象；O3DE 的 `Pass`
  由 `PassDescriptor` 一次配置；wgpu 的管线描述不可变。三者都**不允许**"创建完再逐个 set"。
- **成本**：高（demo / presets / 测试）；单独一批，可整批回滚。
- **能力影响**：无能力删除；CAP-18 / CAP-19 / CAP-28 的证据仍是同一批用例。**行为变更**：作者写法变，
  运行时语义不变（进 §9 的行为变更表）。

### E-05 值里的回调（`G4` / `G12`）

- **现象**：`PassDesc::before_draw`（`std::function<void()>`）在值里；`PassChain::OverlayLayout`
  （`std::function<void(RenderPass&, int, int, double)>`）把**可变 pass 的引用**交给回调。
- **证据**：`PassDesc.hpp:200`；`PassChain.hpp:394`；`ChainBuilder.cpp:99/129` 用 `before_draw` 调
  gizmo / FPS 的 `update()`；`DiagnosticSink` 是合法回调（会话配置）。
- **判据**：`G4`（值要可比较/可差分的理由是它是值；回调让"计划是值"只对一半字段成立）；
  `G12`（"每帧更新自己"是**对象**的行为，不是声明的字段）。
- **更好的形状**：
  1. 删除 `PassDesc::before_draw`。每帧更新由**拥有它的对象**提供：加一个 3 行的接口
     `class FrameUpdate { virtual void update() = 0; }`，由 `FpsOverlay` / `AxisGizmo` 实现
     （**两个真实消费者**，`G8` 满足）；`PassChain::update()` 按 overlay 顺序调用它们，
     宿主在 `frame()` 前调 `view->update()`（W9b 已经确立"发布是内容侧动词"）。
     钩子从"pass 声明里的 `std::function`"变成"链的 overlay 表里的一个类型化指针"（按身份可分、可比较、零分配）。
  2. `OverlayLayout` 改成**返回值**：`std::function<Viewport(int width, int height, double pixel_ratio)>`。
     布局不需要写 pass（今天的 lambda 只算矩形），返回值不给可变引用逃逸的机会。
  `LayoutParticipant` 的 `std::function` 步骤**不改**：它在对象上，不在值里（规则允许）。
- **框架先例**：Qt Quick 的 `updatePaintNode`（每帧更新归节点）；Filament 没有"pass 里的回调"
  （每帧更新由 app 在 `Renderer::render` 前对组件做）；Godot 的 `RenderingServer` 只有命令，没有声明内回调。
- **成本**：小（一个 3 行接口 + 2 个 overlay 类 + 链的 update + 2 个生产 lambda 的迁移）。
- **能力影响**：CAP-27 / CAP-67 不变；新增一条用例"overlay 的 update 在帧前跑一次"（今天由
  `before_draw` 隐式保证）。

### E-06 死字段 `tag`（`G8` / `G10`）

- **现象**：`PassDesc::tag` 注释写 "No behaviour."；链预设写了 8 处；**没有一处读**（`frameTable` 读的是 `name`）。
- **证据**：§2.3 的 8 写 / 0 读；`ChainBuilder.cpp:88/120/262/298/313/390/417`。
- **判据**：`G8`（值里的装饰字段要参与拷贝、比较、dump，却不决定行为）；`G10`（每个 pass 的每一份拷贝都多一个 `String`）。
- **更好的形状**：删。要给人读的标签用 `name`（它本来也要人读）。
- **框架先例**：不需要；这是纯粹的减法。
- **成本**：小（8 处赋值 + 1 个字段）。
- **能力影响**：无（`frameTable` 的 dump 用 `name`）。

### E-07 `ShaderProgram` 既是身份又是可变的阶段表（`G1` / `G8`）

- **现象**：`ShaderProgram` 有 `addStage/clearStages/setStage/stageCount`；`intrusive_ptr<const ShaderProgram>`
  又作为帧值与管线键的身份；值半边 `ShaderProgramDescription`（按 revision 缓存）承担帧内语义。
- **证据**：`ShaderProgram.hpp:142/169/174`、`BuiltinShaders.cpp` 的 `addStage`、`ContentCaches` 的程序键。
- **判据**：这不是缺陷，是**两半**（作者对象 / 不可变描述）。要防的是"再给作者对象加一套 builder"——
  没有消费者（`G8`）。
- **更好的形状**：保留；在 `ShaderProgram.hpp` 顶部写清"对象 = 作者身份 + revision；描述 = 帧内值；
  编辑只经 stage 方法且推进 revision"。**不建** `ProgramBuilder`。
- **框架先例**：Filament 的 `Material`（编译产生 variants）与 wgpu 的 `ShaderModule`（不可变）+ pipeline
  描述是分开的对象，但它们也不为"建 module"造 builder——形状是按 API 需要，不是按对称性。
- **成本**：零代码（文档）。
- **能力影响**：CAP-60 不变。

### E-08 `Content*` 前缀泛滥（`G12`）

- **现象**：SDK 与插件里带 `Content` 前缀的类型有 **15+** 个：`ContentId` / `ContentRef` / `ContentRegistry` /
  `ContentDeclarations` / `ContentStore` / `ContentCaches` / `ContentDraw` / `ContentImages` /
  `ContentPipeline` / `ContentHalves` / `ContentSets` / `ContentPush` / `ContentSources` / `ContentFacts` /
  `ContentPlan` / `ContentAssembly` / `ContentPass`。前缀本来区分"内容侧 vs 引擎侧"，现在连
  "描述符集""push 常量""编译半边"都带它——它不再说明任何事。
- **证据**：`ls src/plugins/gfx_backend_vsg/include/vine/backend_vsg/internal/Content*`（13 个）、
  vk 的 `Content*`（7 个）、SDK 的 `ContentId` / `ContentRef` / `ContentRegistry`。
- **判据**：`G12`（名字要说领域）；一个前缀用在所有东西上，等于没有前缀。
- **更好的形状**：一条**命名法**（写进 `graphics-architecture.md` 的词表）：
  - `Content*` 只给**内容模型与身份**：`ContentId` / `ContentRef` / `ContentRegistry` /
    `ContentDeclarations` / `ContentStore`（设备的"身份 + 行"表）。
  - 由内容**编译/缓存出来的 GPU 对象**按它们是什么命名：`ContentHalves` → `ProgramHalves`、
    `ContentSets` → `BindingSets`、`ContentPush` → `PushConstants`、`ContentDraw` → `DrawRecorder`、
    `ContentImages` → `TextureCache`、`ContentSources` → `ScreenSources`、`ContentPipeline` → `PipelineFactory`。
  - **只改新名字 + 三个最坏的**（`ContentHalves` / `ContentSets` / `ContentPush`，因为"content half"
    在英文里没有意义）；`ContentStore` / `ContentCaches` / `ContentDraw` 保留（它们至少说得清）。
- **框架先例**：Filament 用 `Material` / `Texture` / `RenderPrimitive` / `FrameGraph`，没有"content"前缀；
  wgpu 用 `Texture` / `BindGroup` / `RenderPipeline`。前缀在成熟框架里只用于**层**（`Render*`）或
  **用途**（`Bind*`），不用于"这一切都属于某个大概念"。
- **成本**：小–中（三个改名 + 一批 include/测试）。
- **能力影响**：无（纯改名）。

### E-09 目标词的四个角色（`G1` / `G6`）

- **现象**：带 "Target" 的类型有 `RenderTarget`（宿主声明）/ `TargetShape`（兼容性）/ `TargetPlan`（决策）/
  `TargetImages`（vk 的 GPU 对象）/ `TargetClaims`（租约）/ `HostTargets`（vsg 的表）/
  `VsgTargetRegistry`（vsg 的表）/ `OffscreenTarget`（vsg 的对象 + 探针）。其中
  `OffscreenTarget` 是**生产类型**（`HostTargets` / `VsgExecutor` / `HostReadback` 都用它），但名字听起来像测试夹具。
- **证据**：文件名清单；`OffscreenTarget.hpp` 的用户。
- **判据**：`G1`（一个概念一个名字）；`G6`（两后端的同角色类型不对齐）。
- **更好的形状**（词表，不新增类型）：
  | 角色 | 名字 | 谁拥有 |
  |---|---|---|
  | 宿主声明（尺寸/格式/采样/深度策略） | `RenderTarget` | 宿主 |
  | 兼容性键（格式 + 采样 + 子通道等价物） | `TargetShape` | 后端 |
  | "Reuse / Resize / Rebuild" 的决策 | `TargetPlan` | 后端 |
  | 一帧的租约 | `TargetLease`（今天的 `TargetClaims::Lease`） | 设备 |
  | GPU 对象（图像/视图/render pass/framebuffer） | `TargetObjects`（今天 vk `TargetImages`、vsg `OffscreenTarget`） | 设备 |
  | 设备的表 | `TargetTable`（第一轮新增） | 设备 |
- **框架先例**：D3D12 把"描述"（`D3D12_RESOURCE_DESC`）/"视图"（RTV/DSV 描述符）/"对象"（`ID3D12Resource`）
  分开命名；Vulkan 也是 `VkImage` / `VkImageView` / `VkFramebuffer`。没有框架用同一个词带四个后缀。
- **成本**：小（`TargetImages` / `OffscreenTarget` 改名 + 头文件搬家已在第一轮第 3 批）。
- **能力影响**：CAP-36 / CAP-39 / CAP-42 / CAP-47 不变。

### E-10 一帧的三个值：`FrameTable` / `FrameDescription` / `FrameResult`（`G6` / `G11`）

- **现象**：`FrameTable`（这一帧的 pass 行 + 上次成本）、`FrameDescription`（交给后端的值）、
  `FrameResult`（被答完那一帧的事实）三个类型都叫 "frame"，角色不同却没有任何后缀区分；
  另外 `frameReport()`（设备/驱动者的成本）与 `frameResult()` 又是两个读法。
- **证据**：`FrameTable.hpp` / `FrameDescription.hpp` / `FrameResult.hpp`；`RenderEngine::frameTable` /
  `frameDescription` / `frameResult` / `frameReport`。
- **判据**：这不是重复（三者寿命不同），是**命名没说角色**（`G12`）。
- **更好的形状**：保留三个类型，写进词表并在名字上体现角色（只改**新**名字，不改公开签名）：
  - `FrameTable` = **调度**（`FrameSchedule` 是更好的名字；`frameTable()` 保留为访问器名，或加
    `frameSchedule()` 并弃用旧名）。
  - `FrameDescription` = **帧值**（不变）。
  - `FrameResult` = **答案**（不变）。
  - `FrameReport` = **成本**（不变）。
  规则：一个 engine 每帧只答三样——**要跑什么**（schedule）、**跑的是什么值**（description）、
  **跑完怎么样**（result + report）。停止新增第四个 accessor（`contentCollectCount` 之类的计数
  应并入 `FrameResult` 或 `FrameReport`，不要各开一个）。
- **框架先例**：UE 的 `FSceneRenderer`（要跑什么）/ `FRDGPass`（图节点）/ `FGPUStats`（成本）是三种对象，
  名字各自说角色；wgpu 的 `CommandEncoder` / `RenderPass` / `Queue::submit` 同理。
- **成本**：小（一个可选别名 + 文档）。
- **能力影响**：CAP-19 / CAP-28 / CAP-74 不变。

### E-11 `PassKind` 与投影重复吗？（旧 `P4` 的改判：**保留**）

- **现象**：旧审计说"`Screen` 只是投影 = 屏幕的视图，不需要一个 kind / 类"。
- **反证**：`ScreenPass` **没有相机**（`ScreenPass.hpp` 的公开面是 `sourceTarget()` + `program()` +
  `setProgram()`），它不收集、不裁剪、不排序，只画一个全屏三角形并采样 source 的附件；
  `RenderPass` 则拥有相机、内容、层掩码、图元。两者不是同一个执行加一个不同的投影参数。
- **判据**：`G1` 的反面——把两种执行合并成"一种 + 参数"，会让编译器 / 后端里出现
  "如果 kind == Screen 就不做 X"的分支（`G7`：把复杂性转给每个读者）。
- **判决**：保留 `PassKind{Scene, Screen}` 与 `ScreenPass`。**但要写清**：`PassKind` 是**执行形状**，
  投影是**视图取值**；加第三种执行（compute）时加一个 kind + 一个节点，不改 `DrawDescription::Kind`
  之外的既有模型（第一轮的扩展点表已经记了成本）。
- **框架先例**：Filament 的 full-screen pass（`ShadowMap` / `DepthOfField`）与 mesh pass 是不同节点；
  UE 的 `FMeshPassProcessor` 与 full-screen pass 也是不同类。
- **成本**：零。
- **能力影响**：CAP-61 / CAP-63 不变。

### E-12 命名后缀与单位（旧 `P25` / `M9` 的收敛）

- **现象**：描述类型 6 种后缀（`Desc` / `Options` / `Settings` / `Config` / `Info` / `Params`）；
  尺寸/坐标的单位（设备像素 vs 逻辑像素）靠注释。
- **判据**：`G6`（一致性）；但**改名 9 个类型**的收益只是好看（`G8`）。
- **判决**：写一条**面向新类型**的规则：创建参数 `XxxConfig`；能力 `Capabilities`；跨层值不带后缀；
  某一时刻的值 `XxxSnapshot`。**不回填存量**。单位：`Viewport` / `TransientShape` 的像素是设备像素
  （写进类型注释与 `View` 的注释）；不引入单位类型（`G8`）。
- **能力影响**：无。

---

## 5. 能力设计再审（哪些动词真的承载能力）

### A-01 `setRenderTarget`：vk 只写不读，vsg 真的建（`G4` / `G11` / `G10`）

- **现象**：同一个纯虚动词，两个实现完全不同：
  - vk：`Device::setRenderTarget` 取体闸、`impl->target = target`，**全文件没有一处读**（`impl->target`
    仅 1 处写；成员声明在 `Device.cpp:686`）。它既不建目标、也不影响帧；还留下一个可悬垂的裸指针
    （宿主销毁 target 后指针仍在，今天因为不读所以无害）。
  - vsg：`VsgBackend::setRenderTarget` 取内容锁、`adoptHostTarget(target, false)`，**真的**建/复用对象。
- **证据**：`Device.cpp:3250-3258`、`VsgBackend.cpp:2141-2168`、`VsgBackendTest.cpp:2261`
  （注释明写 "held and built (no frame is needed to build one)"）、`tests/test_vk` 里 0 个
  `setRenderTarget`。
- **判据**：`G4`（契约说"设备持有它并建它的附件"——vk 不成立）；`G11`（一个动词在两后端一个有效一个无效）；
  `G10`（一个裸指针的寿命没有任何约束）。
- **能力判定**：这是第一轮 A 表漏掉的一条 **½ 能力**：**CAP-86 目标预建（不需要帧）** ——
  vsg 有、vk 没有。证据：`VsgBackendTest` 的 `…held and built…` 用例；vk 无对应。
- **更好的形状**（二选一，**建议前者**）：
  1. **让 vk 也建**：`setRenderTarget` 调 `ensureTarget(desc)`（在体闸内；失败报一次并保持原状），
     于是"预建"成为两后端同一能力，契约句变真。成本：约 30 行 + 一条 vk 用例。
  2. **退役动词**：删 `RenderDevice::setRenderTarget`（纯虚），"预建"由 `prepareFrame` 承担；
     vsg 的 `adoptHostTarget` 改为帧首解析时建。代价：宿主失去"先建目标再组帧"的入口（今天只有测试用它）；
     5 个测试双少一个纯虚动词。**能力会少一条**，需单独批准。
- **框架先例**：wgpu 没有"set target"动词（`Surface::configure` 配尺寸/格式，目标由 render pass 的
  attachment 表达）；D3D12 的 RTV 在命令列表里绑；O3DE 的 attachment 在 pass 里声明。
  ——成熟框架都**不**提供"设备级当前目标"这个概念；它是我们框定的产物。
- **成本**：方案 1 小；方案 2 中（测试重写）。
- **能力影响**：CAP-39 / CAP-47；新增 CAP-86。

### A-02 `releasePass`：契约承诺多于实现（`G4`）

- **现象**：`RenderDevice::releasePass` 的注释说"释放设备为这个 pass 保留的一切（内容视图 / 编译管线 /
  每 pass 场景桥缓存 / 采样槽 / 窗口层）"。今天：vk **没有覆写**（用 SDK 默认 no-op）；vsg 只
  `passes.release(pass)`——忘掉一个 id。引擎在 `removePass` 里同时调 `releasePass` 和
  `releaseRenderTarget`（`RenderEngine.cpp:786-790`），归属其实已经清楚。
- **证据**：`Device.cpp` 无 `releasePass`；`VsgBackend.cpp:2387-2400`；`RenderEngine.cpp:786`；
  `RenderDevice.hpp:332` 的注释。
- **判据**：`G4`（名字与注释说拥有一组 GPU 对象，实际只是"忘掉身份"）；`G1`（"pass 拥有目标"的说法
  在契约里出现，而引擎已经分开释放）。
- **更好的形状**：改契约，不改动词：`releasePass` = **"宿主说这个 pass 不再存在，忘掉一切以它的身份
  为键的状态"**；明写"今天没有后端为 pass 保留 GPU 对象；vsg 忘一个 id，vk 无可忘"。删掉
  "a pass may own a render target (freed by both)" 这句（引擎的 `removePass` 已经在同一处释放目标）。
- **框架先例**：wgpu 的 `RenderPipeline`/`BindGroup` 是显式 `destroy()` 的资源；没有"pass 句柄"这个
  可释放对象（render pass 是命令，不是资源）。我们的 `releasePass` 存在只因为 vsg 需要回收 id。
- **成本**：零代码（注释 + 设计页一句）。
- **能力影响**：无（CAP-19 的 `RemovePassReleases*` 三条用例不变）。

### A-03 读回：三处不对称（`G4` / `G6`）

- **现象**：`readColorBuffer(target, attachment_index, vector<uint8_t>&, why)` 与
  `readDepthBuffer(target, vector<float>&, why)`：深度没有 attachment 索引（深度是隐式的），
  颜色没有格式参数（固定 RGBA8），两者都靠 `bool + 出参`（旧 `P27` 的 baseline），都返回**整个附件**
  （没有区域）。
- **证据**：`RenderDevice.hpp:446/474`；vk 的两处实现；vsg 的 `HostReadbackRefusal` 九种拒绝。
- **判据**：`G4`（同一件事两个签名）；`G6`（"哪张图"在 pass 输入里是 `ImageRef`，在读回里是
  "target + int"，两套词汇）。
- **更好的形状**（渐进）：
  1. 两个动词都收一个 `ImageRef`（它已经能命名"目标 + 附件/深度"），替掉 `attachment` 参数——
     "哪张图"只有一种说法（与 `E-03` 同一批）。
  2. 失败用**一个值**表达：`ReadbackResult` 已经是四态枚举，把它从出参升级为返回的一部分
     （`ReadbackOutcome{ result, bytes }`），或至少在 `@return` 里说清 bool 与 `why` 的关系。
     **不做**全仓 `Result<T>` 替换（`S-E`）。
  3. **区域读回**（`origin` + `extent`）**不加**：今天没有消费者（`G8`）；触发条件是缩略图 /
    ROI 导出。
- **框架先例**：wgpu `copy_texture_to_buffer`（subresource + origin + extent，一个动词）；
  D3D12 `ReadFromSubresource`（子资源 + footprint）；Vulkan `vkCmdCopyImageToBuffer`（region）。
  它们都有区域，因为它们的消费者需要；我们没有，所以只借"一个动词 + 一个图引用"，不借区域。
- **成本**：中（两个签名 + 调用点）。
- **能力影响**：CAP-69 / CAP-70 / CAP-71 不变；证据是同一批读回用例。

### A-04 `Capabilities` = `Features` + `Limits`（`G4` / `G6`）

- **现象**：`Capabilities` 一个结构里同时装：三个行为位（`render_targets` / `presentation` / 一个
  `features` 位集）与四个 limit（`max_anisotropy` / `max_samples` / `max_line_width` /
  `max_color_attachments`），外加 `supports(Feature)` 静态位运算。
- **证据**：`Capabilities.hpp:24-121`。
- **判据**：`G4`（"支持什么"与"最大多少"是两个问题，混在一起让新增一个 limit 会改一个"能力"结构）；
  `G6`（两后端的"能力"不同集，没有地方说哪个后端答假）。
- **更好的形状**：

```cpp
struct Features { std::uint32_t bits; ... };       // 位 + supports()
struct Limits   { std::uint32_t max_samples, max_color_attachments; float max_anisotropy, max_line_width; };
struct Capabilities { Features features; Limits limits; bool presentation; };
```

  并在每个位/limit 的注释里写"哪个后端今天答假、为什么"（`GpuTimings` 只有 vk；内存池统计只有 vk）。
- **框架先例**：WebGPU / wgpu 把 `Features` 与 `Limits` 分成两个结构（`DeviceDescriptor.requiredFeatures`
  / `requiredLimits`）；D3D12 的 `D3D12_FEATURE_DATA_*` 按特性分开查询。这是行业里最一致的形状。
- **成本**：小（additive 结构拆分 + 两个后端的填充；旧的 `capabilities().max_samples` 可以保留一个
  inline 转发一版）。
- **能力影响**：CAP-31 不变。

### A-05 默认程序的"两扇门"：**撤回**（核过，不是缺陷）

- 第一轮与旧 `P8` 都怀疑 `RenderDevice::setDefaultContentProgram` 是第二扇门。核对结果：
  它是 `RenderFrameExecutor::setDefaultContentProgram` 这个**同一个虚函数**，经
  `RenderBackend` 的多重继承从设备对象上可达；`Device` 的实现把它转发给**设备自己的驱动者**，
  `RenderEngine` 用的是 `session_`（它借的那个驱动者）。`MultiSessionTest` 大量使用
  `device->setDefaultContentProgram`（设备自己的驱动者），语义正确。
- **判决**：不是两扇门，是同一个虚函数在聚合对象上的可达性。**撤回**；只在文档里写清
  "设备 = 它自带驱动者的那扇门；引擎用自己借的那扇"。
- **能力影响**：无。

### A-06 诊断没有"来源"（`G4` / `G11`）

- **现象**：`RenderDiagnostic{severity, category, code, subject, limit, message}` 没有"谁报的"。
  引擎的 `diagnosticCount()` 会把**宿主**经 `reportEngineProblem` 注入的文本也算进去
  （appfw `Viewer.cpp:83` 真的这么做），于是"引擎发现的问题数"不再是一个可判定的量。
- **证据**：`RenderDiagnostic.hpp`；`Viewer.cpp:83`；`RenderEngine.hpp` 的 `reportEngineProblem` 注释。
- **判据**：`G4`（计数说它是什么）；`G11`（一个宿主 bug 与一个后端 bug 混在同一个数里）。
- **更好的形状**：加一个 `DiagnosticOrigin { Engine, Device, Host }` 字段（additive），
  或给宿主自己的路由（appfw 已有自己的 `Diagnostics`？）。前者更小，且让 `diagnosticCount(origin)`
  可分开。`reportEngineProblem` 保留（宿主确实需要报告），但它的报告带 `Host`。
- **框架先例**：D3D12 info queue 的消息带 `ID3D12Object*`（谁产生的）；Vulkan 的
  `VK_EXT_debug_utils` 消息带 `pObjects`（对象列表）。"谁报的"是诊断的一等字段。
- **成本**：小（一个字段 + 两处填充 + 计数重载）。
- **能力影响**：CAP-73 / CAP-76 不变；新增一条"宿主注入的问题不计入引擎自己的计数"的用例。

### A-07 `PrepareReport` 的"为什么没建成"（`G4` / `G11`，可选）

- **现象**：`PrepareReport` 报 outcome + passes/draws + halves/sets/uploads/pipelines 的**数量**，
  不报"哪一个身份没建成、为什么"。宿主知道"3 个管线没建"，不知道"哪个程序、什么原因"。
- **判据**：`G11`（失败可定位）；但诊断路由已经会报建失败的原因（`VK` / vsg 的 code）。
- **判决**：**不新增字段**（`G8`：诊断已经有消费者）；在 `PrepareReport` 的注释里指向诊断 code，
  并让预热的失败走 `ContentRefusal`（vk 已有）。
- **能力影响**：无。

### A-08 测试替身的负担（`G4` / `G11`）

- **现象**：`RenderDevice` 有 5 个纯虚动词；`RenderBackend` 的测试替身分别覆写 10 / 8 / 82 / 37 个
  成员（`PlanTest` / `FrameScheduleTest` / `GraphicsTest` / `HeadlessBackendTest`），appfw 的
  `RenderControlTest` 还有一版。`setRenderTarget` 是其中之一。
- **判决**：`A-01` 若走"退役"方案，替身少一个纯虚动词；若走"vk 也建"，替身不变。
  这与第一轮"不拆 `RenderDevice`"（`D-16`）一致：替身的数量不是拆接口的理由（默认实现已经吸收），
  但**纯虚且无生产消费者**的动词是。
- **能力影响**：无。

---

## 6. 实现再审

### I-01 每 pass 每帧一次灯向量分配（`G10` / C13 / N6）

- **现象**：`RenderPass::describe` 与 `ScreenPass::describe` 各自构造一个**局部**
  `std::vector<LightValue>`：`reserve(lights.size())` → `push_back` → `std::move` 进
  `DrawDescription::light_values`。每个 pass 每帧一次 alloc + free；同一场景的 N 个 pass
  各拷一遍同样的灯。这直接违反 `R24` / `N6`（帧内 arena 外零分配）——而"零分配"是本仓库自己
  写在帧管线设计里的要求。
- **证据**：`RenderPass.cpp:242-247`、`ScreenPass.cpp:82-87`；`DrawDescription::light_values`
  是 `std::optional<std::vector<LightValue>>`。
- **判据**：`G10`（分配次数是 pass 数的函数，而灯集是场景的函数）；`G11`（没有计数器能看见它，
  因为 vsg 的分配门在录制路径，不在内容半段）。
- **更好的形状**（两步）：
  1. **最小**：把 scratch 向量变成 `RenderPass` / `ScreenPass` 的成员（`std::vector<LightValue> light_scratch_`），
     每帧 `assign` 而不是构造新向量；容量稳态复用 ⇒ 零分配。`DrawDescription` 仍按值持有
     （`light_values = light_scratch_` 会拷贝，所以要么 `light_values` 改成 `shared_ptr<const LightSet>`，
     要么把成员向量的内容 move 进去再换回——前者更干净）。
  2. **概念上更好**：**灯集是场景发布的一部分**，不是 pass 的字段。给发布加一个不可变的
     `LightSet`（`std::shared_ptr<const LightSet>`，随 `SceneSnapshot` 一起发布，revision 变才重建），
     `DrawDescription` 持有这个 shared 值；N 个 pass 共享一份，每帧零拷贝。
     这是 `commands` 已经用的形状（`shared_ptr<const vector<RenderCommand>>`）。
- **框架先例**：Filament 的灯按场景（`Scene`）管理，per-view 的灯列表在 `View` 里计算一次；
  UE 的 `FScene` 持有灯数组，pass 读场景，不拷灯。没有框架把灯放进 pass 的值里。
- **成本**：第 1 步小（两个成员 + 两个 describe）；第 2 步中（发布值 + ABI 的 light block 不变）。
- **能力影响**：CAP-12 / CAP-62 不变；证据加一条"多 pass 同场景的稳态帧零分配/零灯拷贝"用例
  （需要内容半段的分配计数，见 I-02）。

### I-02 vk 的"帧首零分配"有文档、没证人（`G11` / C13）

- **现象**：`Device::Session::DrawContent` / `PassContent` 的注释写"THE CONTAINERS ARE REUSED, NOT
  REALLOCATED: resize keeps what the previous frame reserved, so a steady frame resolves without
  touching the allocator (G10)"。这是**正确的实现**（按索引写回），但**没有任何测试**证明它；
  vsg 有 `CoreAllocationGateTest`（mallinfo2），vk 没有对应物。
- **证据**：`Device.cpp:505-525` 的注释；`test_vk` 无分配计数用例；`CoreAllocationGateTest` 只在
  `test_vsg`。
- **判据**：`G11`（"保证"要有一条能红的用例）；第一轮 §4.10 已经指出"靠约定不靠类型"。
- **更好的形状**：给 `test_vk` 加一条**测试装置**（不是产品分支）：在帧首前后读 `mallinfo2`
  或重载分配器，断言稳态帧的堆增长为 0（允许首次 warm-up）。或者更便宜：给 `DrawContent` 的
  `resize` 加一个"容量未变"的计数器，用例断言它。两者都只证明"容器不重分配"，不证明全局零分配——
  **说清楚证明了哪一半**。
- **框架先例**：不需要；这是证据工程。
- **成本**：小（一条用例 + 一个测试计数器）。
- **能力影响**：CAP-38 不变（加证据，不改行为）。

### I-03 `Impl::target` 死存储与可悬垂指针（`G10`）

- **现象**：`Device::Impl` 有一个 `RenderTarget* target{ nullptr }`（`Device.cpp:686`），只在
  `setRenderTarget` 里写；全文件没有读。宿主释放 target 后指针悬垂（今天不读，所以不 UB）。
- **证据**：§2.3 的 1 写 0 读。
- **判据**：`G10`（生命周期）；`G8`（一个不决定任何行为的字段）。
- **更好的形状**：`A-01` 的两个方案都会删掉它——方案 1 改成 `ensureTarget` 的调用（不存指针），
  方案 2 删动词。**必须删**，不能留。
- **成本**：随 `A-01`。
- **能力影响**：随 `A-01`。

### I-04 契约句与实现不符（`G4` / `G12`）

- **现象**（三处实证）：
  1. `RenderDevice::releasePass` 说"释放 pass 拥有的内容视图 / 管线 / 采样槽 / 窗口层"；
     vk 无覆写、vsg 只忘 id（`A-02`）。
  2. `RenderDevice::setRenderTarget` 说"设备持有它并建它的附件"；vk 只存指针（`A-01`）。
  3. `FrameDescription` 的类注释说"EPHEMERAL, BORROWED snapshot"，但三个成员是 owning
     （第一轮已判为**已修**：逐字段规则写明；本轮复核仍成立）。
- **判据**：`G4`（契约句可判定）；`G12`（文档说 intent，代码说 actual）。
- **更好的形状**：跟着 `A-01` / `A-02` 改两句；在第 9 批（文档真实性）逐条对账时加一个"公开头里
  的 always/never/one/all 句式"清单。
- **成本**：零代码。
- **能力影响**：无。

### I-05 `tag` 的 8 处赋值（`G8`）

- 已在 `E-06`；实现上是 `ChainBuilder` 的 8 行赋值，删字段时一起删。

### I-06 `OverlayLayout` 的可变逃逸（`G7`）

- 已在 `E-05`；实现上是 `PassChain::addOverlay` 的签名与 `ChainBuilder` 的 lambda。改成返回
  `Viewport` 后，`PassChain` 内部把返回值写进 overlay 的声明——可变引用不再逃逸。

### I-07 宿主诊断污染计数（`G4`）

- 已在 `A-06`；实现上是 `RenderEngine::reportEngineProblem` 的两处宿主调用（appfw `Viewer`、
  以及 `ChainBuilder` / `SceneView` 的内部调用——后者本来就该计入引擎）。加 `origin` 后，
  `diagnosticCount()` 默认只数引擎 + 设备，宿主另有一个重载。

### I-08 两后端对"内容层"的命名与形状漂移（`G6` / `G9`）

- **现象**：第一轮已把"帧首计划"统一为 `FrameHead`/`DrawPlan`。本轮再确认：vk 的 `DrawContent`
  **按调用复用容器**（好），vsg 的 `ContentPlan::Call` **固定数组**（也好）；两者的稳态分配语义相同，
  但**没有一个共同的名字**说这件事（"解析出的每调用值"）。第一轮起的名字 `FrameHead` 应同时覆盖两者，
  并在两处注释里写同一条"容器复用 / 固定容量"的规则。
- **判决**：随第一轮第 2 批；本轮只补一条**跨后端用例**：同一帧（同一内容、同一 pass 数）在两后端各跑
  N 帧，断言第二帧起到第 N 帧的"构建/分配"计数为 0（vk 用 1-2 的计数器，vsg 用 `builds/uploads`）。
- **成本**：小（一条对拍用例 + 各自计数器）。
- **能力影响**：CAP-38 / CAP-64。

---

## 7. 统一编程规范审查

> 判据不是"我觉得"，是**规范条款 + 机器门禁 + 计数**。三样都对不上时才是一条发现。

### 7.1 九道门禁的今天

| 门禁 | 结果 | 数字 |
|---|---|---|
| `check_api_style.py` | **PASS**（有 baseline） | 0 new / **4 baseline** / 84 header |
| `check_include_hygiene.py` | PASS | 0 finding / 1329 files |
| `check_export_annotations.py` | PASS | 48/48 后端导出实体有标注 |
| `check_public_docs.py` | PASS（有 baseline） | **974 baseline** / 0 new / 0 stale / 107 files |
| `check_diagnostic_formats.py` | PASS（有棘轮） | 100 code 在用 / 100 登记 / **22 处未编码**（6 文件） |
| `check_ai_docs.py --strict` | PASS | 0 finding / 215 note |
| `check_design_docs.py` | PASS | 0 error / 56 design doc |
| `check_doc_symbols.py` | PASS | 3 living doc / 106 unit |
| `check_vk_exposure.py`（ctest） | PASS | SDK 零 `Vk*`；shell 零 `Vk*`；帧路径不走逃生门 |

**结论**：机器能查的都绿；剩下的问题在**机器没有表达的规则**与**法/习惯不一致**。

### S-01 字段注释：`///` 是主流，规范却写"prefer multi-line"（`G6`）

- **现象**：SDK 公共头里 `///` 单行字段注释 **545 行**，`/** … */` 块 **1627 个**。值类型的字段
  （`ClearPolicy` / `DrawDescription` / `PassDesc` / `CameraSnapshot` / `Ramp`…）几乎全是 `///` 一行。
- **规范原文**："Prefer multi-line comments; do not condense them into a single line." +
  "Public interfaces must use Doxygen-style comments."
- **判据**：`///` 是合法 Doxygen，但违反"prefer multi-line"；545 行与规范冲突，说明**规范缺一条字段例外**。
- **建议（规范修订，不改代码）**：在 `.github/copilot-instructions.md` 的 Comments 段加一条：
  > 结构体/类的**字段**允许一行 `///`，前提是这一行说清契约（不写"the width"这种复述）；
  > 函数（含构造函数）仍用 `/** @brief … */` 的分节格式。
  理由：字段没有参数/返回值/失败模式可以分节；把 545 行展开成 5 行块是纯噪声（`G10`）。
- **成本**：零代码。

### S-02 Section comment：26 处与"禁止 section comment"冲突（`G6` / `G12`）

- **现象**：`// ---- Host bindings: standing, across frames ----` 这类分组注释 **26 处**，
  分布在 `FrameResources.hpp`（3）、`DrawListBake.hpp`（3）、`CameraManipulator.hpp`（2）、
  `OrbitCameraManipulator.hpp`（3）、`ChainBuilder.cpp`、`SceneSnapshot.hpp` 等。
- **规范原文**："Do not create section comments." + "The block names … are labels used ONLY to describe
  what each block holds. They are NOT comments and MUST NOT be inserted into the code."
- **判据**：`G6`（同一件事两种做法）。这些注释**不是**访问控制块标签（那是规范明确禁止的），
  而是"一个类型里的成员分组"——规范没有区分这两件事。
- **建议（二选一，推荐 A）**：
  - **A. 清扫**：删 26 处，用空行 + 每个成员的 `///` 契约表达分组。机械、可回滚、零行为。
  - **B. 修订规范**：允许"一个类型 ≥ 20 个成员时用一行 `// ---- 组名 ----` 分组"，但**仍然禁止**
    访问块标签。理由：26 处里有 8 处确实让 900 行的 `RenderPass` / 680 行的 `PassChain` 可读。
  - 我推荐 **A**：规范的本意是"注释要说契约，不要当目录"；目录可以由成员自己的文档承担。
    如果清扫后确实难读，再按 B 补一条**窄**例外。
- **成本**：小（26 行删除，comment-only 提交）。

### S-03 `enum class` 的底层类型：94 个里 37 个没写（`G6` / `G10`）

- **现象**：`PassKind : std::uint8_t`（写了）、`DrawDescription::Kind : std::uint8_t`（写了）、
  `DevicePresentation`（没写）、`ReadbackResult`（没写）、`PassStage`（没写）、`ShadingMode`（没写）……
  37/94 没写底层类型。
- **判据**：`G10`（进公共值 / 跨 ABI 的枚举，底层类型是布局的一部分；不写就由实现选）；
  `G6`（同一层的两个枚举两种写法）。
- **建议（规范 + 小改）**：规则："**出现在公开值类型字段里、或跨接缝传递的 `enum class` 必须写
  固定底层类型**（`std::uint8_t` / `std::uint32_t`）；只在函数内部用的枚举可以省略。"
  按此规则只需要改公开值字段里那部分（约 10 个），不是 37 个全改。
  **注意：给公开值字段里的枚举补底层类型会改变布局（ABI）**——它必须随一个**已经破 ABI** 的批次
  （B3 接缝搬家或 B16 作者形状）做，连同插件重建与全量测试；**不随 B10a 的注释清扫**。
- **成本**：小。

### S-04 一个名字四种含义：`setRenderTarget`（`G1`）

- **现象**：`RenderDevice::setRenderTarget(RenderTarget*)`（设备级公告）、
  `RenderPass::setRenderTarget(intrusive_ptr<RenderTarget>)`（声明里写目标）、
  `FrameRecorder::setRenderTarget(const void*)`（后端记录的当前目标，类型擦除）、
  以及测试双的覆写。四层同名，语义不同；其中 recorder 那个连类型都丢了（`const void*`）。
- **判据**：`G1` / `G4`（调用点看不出是哪一层）。
- **建议**：随 `E-03` / `A-01`：设备级那个改名或退役；`RenderPass` 的写入口随 `E-04`（作者形状）
  或改名 `setTargetDeclaration`；`FrameRecorder` 的随第 3 批搬进 vsg（不再出现在 SDK 接缝）。
  **不**保留两个同名的公开写入口。
- **成本**：随 `E-03` / `E-04`。

### S-05 `get` 前缀：graphics 已经零新增（旧 `P30` 关闭）

- **现象**：`check_api_style.py` 的规则 2（`sdk/` 下 `get[A-Z]\w*` 零容忍）通过；唯一 baseline 是
  appfw `VisualUserIO::getStringAsync`——它是**命令**（"向用户要一个值"），不是访问器。
- **判决**：graphics 关闭；规范补一句"访问器 Qt 风格、命令用动词（`ask*Async`）"（旧 `P30` 的结论）。
- **成本**：零。

### S-06 描述类型后缀（`G6`，旧 `P25`）

- 见 `E-12`：**只约束新类型**，不回填 9 个旧名。

### S-07 公开文档债：graphics 欠 49 条（`G4` / `G11`）

- **现象**：`public_docs_baseline.txt` 974 条里 **49 条在 `src/viz/graphics`**
  （`RenderCommand` / `RenderPass` / `Scene` 等），其余大头是 appfw 456、core 165、math 92、async 87。
- **判据**：`G4`（公开契约要写清参数与返回）；`G11`（宿主读头文件就能用）。
- **建议**：第一轮已经定了"新写/改写的公开声明必须完整，baseline 只降不升"。本轮加一条**批次门禁**：
  任何触碰 `sdk/vine/graphics/**.hpp` 的批次，结束时该文件的 baseline 行必须消失（填掉或证明不需要）。
  不是全仓扫债（`G8`：49 条里很多是值字段/重载），只**随改动偿还**。
- **成本**：随批次。

### S-08 注释分节：`@warning` 的使用（`G4`）

- **现象**：抽查 `RenderDevice` / `RenderFrameExecutor` / `ContentCaches` 的 `@warning` 都有内容
  （线程、重入、非递归锁），不是样板；`PassDesc` 的字段注释没有 `@warning`，但它没有危险。
- **判决**：符合规范；**不**把 `@warning` 变成必填（规范说"section appears only when it carries content"）。
- **成本**：零。

### S-09 所有权词汇规则没有写在 SDK 里（`G4` / `G10`，旧 `P16`）

- **现象**：`intrusive_ptr`（共享拥有）、`raw_ptr`（调用期借用）、`std::shared_ptr<const T>`
  （不可变共享值）、`std::span`（非拥有连续数据）四种拼写并存且**规则只存在于文档与习惯**。
  例子：`RenderCommand::material` 是 `intrusive_ptr`，`Geometry::material()` 返回 `raw_ptr`。
- **判据**：`G4`（借用与拥有不能靠读实现才知道）。
- **建议**：在 `graphics_global.hpp` 顶部写一段 8 行的**所有权词汇表**（不是新类型）：
  | 拼写 | 含义 | 语义 |
  |---|---|---|
  | `intrusive_ptr<T>` | 共享拥有 | 值/对象持有它命名的东西 |
  | `raw_ptr<T>` | 借用 | 只在调用期间有效；`@return` 必须说"borrowed" |
  | `std::shared_ptr<const T>` | 不可变共享值 | 跨线程传值（快照、命令列表） |
  | `std::span<const T>` | 非拥有连续数据 | 调用期有效 |
  并在 `check_api_style.py` 加一条可选规则：`@return` 为指针/引用的公开函数，文档必须出现
  "borrowed" 或 "owned"（否则 baseline）。**先只写规则，不加门禁**（避免一次 100+ 条新 finding）。
- **成本**：零代码 + 一条未来门禁。

### S-10 `bool` 与 `Result`：不回填存量（`G8` / `G4`，旧 `P26` / `P27`）

- **现象**：SDK 里 198 个返回 `bool` 的声明；`bool + XxxResult*` 的组合在 `RenderDevice` 有两处。
- **判据**：`G4`（bool 同时表示成功/有无/找到）。但全仓换成 `Result<T>` 是 ~200 个调用点的迁移，
  收益与风险不成比例（`G8` / `G10`）。
- **建议**：**不回填**。规则：新增的、会失败且有原因的操作返回**值**（`ReadbackOutcome`、
  `SurfaceTargetOutcome` 之类），或至少 `bool` + `enum` 出参；存量按 `S-07` 的"随改动偿还"。
  `A-03` 的读回是第一个试点。
- **成本**：0（存量）/ 中（试点）。

### S-11 `int` 与固定宽度（`G6`）

- **现象**：公开值里的尺寸/坐标字段是 `int`（27 个），id/计数是 `std::uint32_t`。规范没有说。
- **建议**：写一条约定：**尺寸、坐标、矩形用 `int`（平台 API 的拼写）；id、计数、位掩码用
  `std::uint32_t` / `std::uint64_t`**。不回填。
- **成本**：零。

### S-12 类型转换与 raw 分配：核过，判为正确

- **`reinterpret_cast` 50 处**：`WindowSystem.cpp` 15（XCB/HWND 句柄）、`VsgHostWindow.hpp` 5
  （宿主句柄）、`PipelineCacheFile.cpp` 5（字节 blob），其余 1–2 处散布在 vsg 适配层。
  **集中在平台/文件接缝**，帧路径没有。符合"逃生门只在一处"。
- **raw `new`**：全部是 `intrusive_ptr<T>(new T(...))` / `unique_ptr<T>(new T(...))` 的采纳写法
  （10 处），没有裸 owning 指针；PImpl 用 `new Impl` 是规范允许的形状。
- **`using namespace`**：头文件 0 处。
- **`std::span`**：156 处，非拥有连续数据已经用类型表达。
- **判决**：这三项不需要改；写进"核过"的清单，避免下一轮重复审。

### 7.13 规范修订汇总（6 条）

| # | 修订 | 类型 | 归属 |
|---|---|---|---|
| 1 | 字段允许一行 `///`（S-01） | 规范文字 | 批次 10 |
| 2 | 删 26 处 section comment（S-02，方案 A） | 注释清扫 | 批次 10 |
| 3 | 公开值字段 / 跨接缝枚举写固定底层类型（S-03） | 小改（**ABI**） | 随 B3 或 B16（不随 10a） |
| 4 | 所有权词汇表（S-09） | 规范文字 | 批次 10 |
| 5 | 访问器 vs 命令的命名规则（S-05） | 规范文字 | 批次 10 |
| 6 | 尺寸 `int` / id 与计数定宽（S-11） | 规范文字 | 批次 10 |

---

## 8. 框架对照（逐概念，借 / 不借）

> 引用的是**公开文档里的形状**，不是源码逐行；每行给"它怎么做 → 我们的判决"。

| 概念 | 框架怎么做 | 我们 | 判决 |
|---|---|---|---|
| **视图** | Filament `View` = camera + viewport + visible layers + render target；O3DE `RPI::View`；bgfx per-view 状态 | 两个重叠值 + 散字段 | **借**（`E-01`） |
| **渲染区域** | Godot `Viewport`（窗口或纹理的一块渲染区域）；Vulkan/D3D12 的 "surface/swapchain" 一律是平台对象 | `RenderSurface` 是区，名字像平台表面 | **借名字**（`E-02`） |
| **pass 作者** | Filament `Material::Builder` / `RenderPass::Builder` + 不可变对象；O3DE `PassDescriptor` 一次配置；wgpu 管线描述不可变 | `PassDesc` 值 + 35 个 setter 的活对象 | **借**（`E-04`，分批） |
| **目标绑定** | UE `FRenderTargetBinding`；O3DE `PassAttachment`；wgpu `RenderPassColorAttachment` | 3 种目标字段 + 3 种发布字段 | **借**（`E-03`） |
| **render graph** | UE RDG（pass 参数 + 图编译 + 瞬态别名 + 自动屏障）；Frostbite FrameGraph；O3DE FrameGraph | 引擎做**平面调度** + transient + 借用深度；屏障/别名归后端 | **不借**：图编译与别名没有今天的消费者（`G8`），屏障已由后端的单表覆盖（`G10` 账算不过来） |
| **设备/队列/表面** | wgpu `Device`/`Queue` 与 `Surface` 分离且 `Send+Sync`；D3D12 `Device`/`CommandQueue`/`Fence`；Metal `MTLCommandQueue`/`CAMetalLayer` | `RenderDevice` + 每驱动帧机器 + 窗口目标 | **已同形**（ADR-2026-11-15）；只差 `RenderArea` 的名字（`E-02`） |
| **能力** | WebGPU / wgpu `Features` + `Limits`；D3D12 按特性查询 | 一个结构混装 | **借**（`A-04`） |
| **错误/诊断** | D3D12 info queue（消息 + 对象 + 严重度）；Vulkan debug utils（消息 + 对象 + ID）；wgpu error scope（拉取式） | 推送 sink + code + subject + limit + episode | **已强于多数框架**（code + 一次情节一条）；只缺 origin（`A-06`）。**拉取式 scope 不借**（`G8`：测试已用计数与 sink） |
| **资源寿命** | wgpu 内部引用 + 用户引用；D3D12 residency manager；Vulkan `VK_EXT_device_memory_report` | 三态 + park（vk）/ pin + 声明索引（vsg） | **已同形**（ADR-2026-11-10/15）；保留两机制（`G1` 的边界清楚） |
| **bindless / 描述符堆** | D3D12 descriptor heap；`VK_EXT_descriptor_buffer`；Metal argument buffer | 每 draw 的 set + dynamic offsets；缓存命中稳态零建 | **不借**：没有实测瓶颈（`G8`），且会改内容 ABI（`G10`） |
| **命令记录** | D3D12 command list/allocator；wgpu encoder；Metal command buffer | 每驱动命令池/缓冲 + 帧首解析值 + 无锁录制 | **已同形**（S6c/S8c）；缺的是"三相位"的契约名（第一轮已提） |
| **多帧在飞** | 多数框架有 2–3 帧在飞 | 一帧在飞（ADR-2026-11-15 实测设备占 ~90%，收益 ≤10%） | **不借**（已裁决） |
| **异步编译** | D3D12 PSO library / `VK_EXT_pipeline_creation_cache_control` / wgpu pipeline cache | 会话内 + 跨会话文件缓存 + `prepare` | **不借**额外机制；`prepare` 已覆盖预算（`G8`） |

**这张表要说明的一件事**：我们**不是**缺功能，而是有三处"形状"落后于成熟框架（视图、目标声明、
能力结构），以及一处"命名"落后（区 vs 表面）。其余要么已经同形，要么**明确不借**并有 `G#` 支撑。

---

## 9. 目标设计的增量（在第一轮之上）

第一轮的概念表、层次、类型清单、公开接口草案仍然有效；本节只写**变化**。

### 9.1 概念表增补

| 概念 | 定义 | 拥有者 | 边界 |
|---|---|---|---|
| **View** | 一次绘制用的视图：相机值 + 视口 + 层掩码 | 值（派生层 / 声明） | 不含帧号/时间（那属于 pass 或 update） |
| **RenderArea** | 一块要画的面：尺寸 + 呈现表 + 它绑定的窗口目标 | 引擎（`createArea`） | 不是平台 surface；一区一幅画 |
| **Target**（声明） | 一个 pass 画进哪里：`Window` / `Explicit` / `Transient` | 声明值 | 不建对象；不承诺发布 |
| **Output**（声明） | 一个 pass 承诺产出什么：名字 + 粗/细承诺 | 声明值 | 与"画进哪里"分开 |
| **LightSet**（可选） | 一次发布里的灯，按值共享、revision 变才重建 | 内容侧（随快照发布） | 不是 pass 的字段；不排序 |
| **DiagnosticOrigin** | 一条报告是谁报的：`Engine` / `Device` / `Host` | 诊断值字段 | 不改变 code 的稳定性 |

**旧 → 新**：

| 旧 | 处置 | 新 |
|---|---|---|
| `CollectView` + `CameraSnapshot` + `Viewport` + `layer_mask` | 合并 | `View`（`CameraSnapshot` 保留为它的字段类型） |
| `RenderSurface` / `createSurface` / `surfaceWidth` | 改名 | `RenderArea` / `createArea` / `areaWidth` |
| `PassDesc::writes` / `writes_image` / `writes_shape` | 合并 | `PassDesc::target`（`Target`） |
| `PassDesc::output_name` / `output_target` / `output` | 合并 | `PassDesc::output`（`Output`） |
| `PassDesc::tag` | 删除 | —（用 `name`） |
| `PassDesc::before_draw` | 搬家 | overlay / gizmo 的 `update()`，由 `PassChain::update()` / `SceneView::update()` 驱动 |
| `PassChain::OverlayLayout`（`void(RenderPass&,…)`） | 重塑 | `Viewport(int w, int h, double ratio)` |
| `TargetImages`（vk）/ `OffscreenTarget`（vsg） | 改名 | `TargetObjects` |
| `TargetClaims::Lease` | 改名 | `TargetLease`（类型不变） |
| `ContentHalves` / `ContentSets` / `ContentPush` | 改名 | `ProgramHalves` / `BindingSets` / `PushConstants` |
| `Capabilities` 的 limit 字段 | 拆出 | `Limits`（`Capabilities` 保留聚合） |
| `RenderDiagnostic` | 加字段 | `origin`（`DiagnosticOrigin`） |

### 9.2 公开接口增量

**新增（additive）**

```cpp
struct View { CameraSnapshot camera; Viewport viewport; std::uint32_t layer_mask; };

struct Features { std::uint32_t bits; [[nodiscard]] bool supports(Feature) const noexcept; };
struct Limits   { std::uint32_t max_samples, max_color_attachments; float max_anisotropy, max_line_width; };
struct Capabilities { Features features; Limits limits; bool presentation; /* old fields kept inline for one version */ };

enum class DiagnosticOrigin : std::uint8_t { Engine, Device, Host };
// RenderDiagnostic 增加 origin 字段（默认 Engine）

struct Target { enum class Kind; Kind kind; intrusive_ptr<RenderTarget> target; String name; std::optional<Extent> extent; std::optional<TransientShape> shape; };
struct Output { String name; bool whole_target; intrusive_ptr<ImageRef> image; };
// Extent 是一个新的小值（两个消费者：transient 形状、将来的读回区域）：
struct Extent { int width; int height; };  // 物理像素；两个字段一起出现
```

**改名 / 删除（分批，old 与 new 并存一版）**

| 接口 | 变化 | 兼容策略 |
|---|---|---|
| `RenderSurface` / `RenderEngine::createSurface()` / `surfaceWidth/Height()` | → `RenderArea` / `createArea()` / `areaWidth/Height()` | 一版 `[[deprecated]]` 别名 + 转发 |
| `CollectView` | → `View` | 一版 `using CollectView = View;`（字段名相同则源码兼容） |
| `PassDesc::writes*` / `output_*` | → `target` / `output` | 一版并存：构造 `PassDesc` 时两者取其一，`validatePlan` 报"两个都写" |
| `PassDesc::tag` | 删除 | 无（从未被读） |
| `PassDesc::before_draw` | 删除 + `PassChain::update()` | 一版警告：有 hook 且宿主没调 `update()` 时报一次 |
| `OverlayLayout` | 签名改 | 一版重载 |
| `RenderDevice::setRenderTarget` | vk 实现预建（方案 1）或退役（方案 2） | 方案 1 无源码破坏；方案 2 一版 `[[deprecated]]` |
| `readColorBuffer` / `readDepthBuffer` | 收 `ImageRef`（替 `int attachment`） | 一版重载 |

### 9.3 能力覆盖（新增与受影响的行）

| 能力 ID | 旧证据 | 新承载 | 新证据 |
|---|---|---|---|
| **CAP-86**（新）目标预建（不需要帧） | vsg：`VsgBackendTest` 的 "held and built" 系列；vk：无 | `setRenderTarget` 两后端都 `ensureTarget`（方案 1） | 两后端各一条：`setRenderTarget` 后首帧 `builds == 0`、`readColorBuffer` 不再 `NotReady` |
| **CAP-87**（新）诊断来源可分辨 | 无（`diagnosticCount` 混计） | `RenderDiagnostic::origin` | 新用例：宿主经 `reportEngineProblem` 报一条，`diagnosticCount()` 不变，`diagnosticCount(Host)` +1 |
| CAP-08 / CAP-14 / CAP-21 / CAP-22 | `SelectTest.*`、`PassDrawTest`、`RenderEngineTest.*` | `View` 值 | 同用例 + `AVewValueDrivesSelectAndDescribeTheSameWay`（一个 `View` 喂两处，逐项相同） |
| CAP-18 / CAP-23 / CAP-39 / CAP-42 | `PlanTest.*`、`RenderEngineTest.*` 的 transient/output 用例 | `Target` + `Output` | 同用例 + `ATargetValueAndAnOutputValueCannotBothBeStated`（校验一条） |
| CAP-24 / CAP-25 / CAP-43 | `RenderSurfaceTest.*`、`OneDeviceServesSeveralAreas…` | `RenderArea` | 同用例（改名） |
| CAP-31 | `CapabilitiesTest.*`、`VkMsaaTest.TheDeviceReportsTheSampleCountItCanBuild` | `Features` + `Limits` | 同用例 + `TheLimitsAreOneValueAndTheFeaturesAnother` |
| CAP-69 / CAP-70 / CAP-71 | `VkOffscreenTest.*`、`VkContentTargetTest.*` | `ImageRef` 读回 | 同用例（重载期）+ `AReadbackNamesItsImageLikeAPassInputDoes` |
| CAP-12 / CAP-62 | `LightTest`、`VkShadowTest.*` | `LightSet`（可选） | 同用例 + `AMultiPassFrameCopiesTheLightsOnce` |
| CAP-38 | `VkFrameHeadTest.*` | 加分配证人 | 新用例：稳态帧堆增长 0 |
| CAP-73 / CAP-76 | `VkContentRefusalTest.*`、`OneDeviceReportsIntoEveryEngine…` | `origin` + `DiagnosticDraft` | 同用例 + CAP-87 的用例 |

### 9.4 行为变更表（第二轮增量，逐条批准）

| # | 旧 → 新 | 为什么 | 可观察 | 批准 |
|---|---|---|---|---|
| BC-08 | `PassDesc::before_draw` 不再被链自动调用；宿主 / 链改调 `update()` | 值里零回调（`G4`） | **是**：依赖隐式钩子的宿主会少一次更新 | 单独批准；一版警告 |
| BC-09 | `setRenderTarget` 在 vk 上**真的建**目标（方案 1） | 契约变真（`G4`），补上 CAP-86 | 是：`setRenderTarget` 后首帧不再建；失败会在调用点报一次 | 单独批准 |
| BC-10 | `diagnosticCount()` 默认不含宿主注入的报告 | 计数说它是什么（`A-06`） | 是：宿主报告不再抬高引擎计数 | 单独批准 |
| BC-11 | `Capabilities` 的 limit 字段移到 `Limits` | 形状（`A-04`） | 源码级；旧字段保留一版 inline 转发 | additive，记此 |
| BC-12 | `RenderSurface` → `RenderArea` 等改名 | `G1` | 源码级 | 词汇，记此 |
| BC-13 | `CollectView` → `View`、`PassDesc` 的 target/output 字段 | `G1` | 源码级；运行时等价（等价用例） | 记此 |
| BC-14 | `tag` 删除 | 死字段 | 源码级 | 记此 |
| BC-15 | `OverlayLayout` 返回 `Viewport` | 值里零可变逃逸 | 源码级；布局结果不变 | 记此 |
| BC-16 | `readColorBuffer/readDepthBuffer` 收 `ImageRef` | 一处词汇 | 源码级（重载一版） | 记此 |
| BC-17 | 灯集从 pass 字段变成场景发布值（可选第 2 步） | `G10` | 否（同画面、少分配） | 记此 |

**明确不改**：两个后端的借深度语义、内存池/管线缓存/GPU 计时的差异、`PassKind` 的两种执行、
`Result<T>` 全面替换、`Session::open` 式创建、多表面。

### 9.5 成本预期

| 项 | 预期 | 怎么测 |
|---|---|---|
| 每帧灯向量分配 | pass 数 × 1 → **0**（第 1 步）/ 0 拷贝（第 2 步） | 内容半段分配计数器（新增测试装置）；`FrameReport` 不加字段 |
| 帧时间 | 不变（值变小，字段少两个，属于噪声） | `Vine --open demo` 的 `VN_BENCH_*`、`SceneTest.Measure…` |
| vk 帧首 | 不变（容器复用已有），只多一条**证人** | `VkFrameHeadTest` + 新分配用例 |
| `setRenderTarget`（方案 1） | 宿主显式预建 ⇒ 首帧少一次构建；稳态不变 | 两后端 `builds` 计数 |
| 公开面 | +5 类型、−1 字段（`tag`）、−1 回调字段、4 个改名、2 个签名（读回） | 编译期 |
| 编译时间 | 改名批次会全量重编一次；稳态不变 | `cmake --build` 墙钟 |

### 9.6 第二轮新增的否决项

| # | 选项 | 理由 |
|---|---|---|
| D-14 | 引入 render graph（UE RDG 式） | 图编译 / 瞬态别名没有消费者（`G8`）；屏障已由后端单表覆盖（`G10`） |
| D-15 | bindless / descriptor heap | 没有实测瓶颈；会改内容 ABI（`G8` / `G10`） |
| D-16 | 全仓换 `Result<T>` | ~200 调用点，收益与风险不成比例（`G8`）；新 API 用值、存量随改动偿还（`S-10`） |
| D-17 | `Session::open(config)` 一次创建（旧 `P18`） | 会藏起"引擎借用共享设备"的关系（ADR-2026-11-15）；顺序用文档表达 |
| D-18 | 把 `PassKind` 合并成"投影取值"（旧 `P4`） | `ScreenPass` 没有相机/内容/层，是另一种执行（`E-11`） |
| D-19 | 为 `ShaderProgram` 造 `ProgramBuilder`（旧 `P15`） | 没有消费者（`G8`）；两半已经清楚 |
| D-20 | 统一 9 个描述类型的后缀（旧 `P25`） | 纯改名，收益只是好看（`G8`）；只约束新类型 |
| D-21 | 把 15 个 `Content*` 全改名 | churn；只改三个最坏的（`E-08`） |
| D-22 | 直接退役 `setRenderTarget` 而不先做 characterization | 会丢 CAP-86；先做方案 1 或先补用例再决定（`G8` 的反面是"删了才发现有人用"） |
| D-23 | 拉取式 error scope（wgpu 式） | 测试已用计数 + sink；没有消费者（`G8`） |
| D-24 | 在多 pass 帧里继续按 pass 拷灯（维持现状） | 与 `R24`/`N6` 相反（`I-01`），且灯属于场景 |

---

## 10. 合并后的批次顺序（第一轮 0–9 + 第二轮 10–17）

> 原则不变：每批一个可回滚提交、能力覆盖 100%、文档与代码同批、公开 API 改动单独成批。
> 下表的"证据"列只写**本批新增/替换**的那部分；其余能力仍由既有用例承载。

| 顺序 | 批 | 主题 | 关键证据 / 判据 | 风险 |
|---|---|---|---|---|
| 1 | 0 | characterization（第一轮标红两条 + `CAP-86`） | 新用例先绿 | 无 |
| 2 | 1 | 词汇与身份（`FrameDriverId` / `WindowSession` / `FrameDriver`） | 改名后同用例 | 低 |
| 3 | **10** | **规范清扫**：删 26 处 section comment；四条规范文字（S-01/02/05/09/11）；枚举底层类型属 ABI，拆到 10b 随 B3/B16 | 九道门禁 + 无行为 | 低 |
| 4 | 2 | 一帧三相位 / `FrameHead` / 帧体闸 / `FrameReport.locking` | `VkFrameHeadTest`、两后端 gate 用例 | 中（线程） |
| 5 | 3 | 接缝归位（18 头 + 18 实现搬进 vsg） | `content_seam`、`vk_exposure`、两套全绿 | 中 |
| 6 | 4 | 诊断：`DiagnosticDraft` + 22 处 code + **origin**（A-06） | baseline 清零、CAP-87 用例 | 低 |
| 7 | 5 | 目标归一：`TargetTable` + `TargetObjects` 改名（E-09） | `TargetClaim*`、`VkResidency*` | 中 |
| 8 | **11** | 灯不再每 pass 分配（I-01）+ vk 帧首分配证人（I-02） | 新分配用例 + 既有像素用例 | 低 |
| 9 | **12** | `View` 值（E-01） | 等价用例 + `SelectTest`/`PassDrawTest` | 中 |
| 10 | **13** | `RenderArea` 改名（E-02） | 同用例改名 | 低 |
| 11 | **14** | `Target`/`Output` 声明值（E-03）+ `setRenderTarget` 预建（A-01，方案 1）+ `releasePass` 契约（A-02） | CAP-86 用例 + 计划校验用例 | 中 |
| 12 | 6 | 引擎组合根（第一轮 D-08） | 公开面零变化 + 头文件行数 | 中 |
| 13 | 7 | 一份选择（第一轮 D-05） | `T34` 600 帧 + 性能用例 | 中高 |
| 14 | **15** | `Features`/`Limits`（A-04）+ 读回 `ImageRef`（A-03） | CAP-31 / CAP-69..71 用例 | 低 |
| 15 | 8 | 接缝去 Vulkan 拼写（可选） | 编译期 + 对拍 | 低 |
| 16 | **16** | **一个 pass 作者形状**（E-04）+ 回调归对象（E-05）+ 删 `tag`（E-06） | 全部 `PlanTest`/`PassChainTest`/demo；BC-08 | 高 |
| 17 | **17** | `Content*` 三个改名（E-08） | 编译 + 全绿 | 低 |
| 18 | 9 | 文档真实性 + 用户文档按能力重组（第一轮 D-18/D-23）+ 本文的所有"旧→新"进 `.ai/design` | `check_ai_docs --strict`、`check_doc_symbols` | 低 |

**为什么这样排**：

- **10 放在最前**（第 3 位）：纯注释/规范，零风险，先做能把"法 vs 习惯"的噪声清掉，后面的 diff 更干净。
- **11 在 12 之前**：灯分配是 C13 的硬违约，独立于 `View`；先修它，`View` 批次就不再夹带分配修复。
- **12/13/14 在 6/7 之前**：它们是**值形状**（`View` / `Target` / `Area`），越早定下来，后面的引擎与选择批次
  不用改两次。
- **16 最后（除文档）**：作者形状的爆炸半径最大（demo / presets / 全部声明测试），且它依赖 14 的
  `Target`/`Output`。它单独一批、可整批回滚。
- **9 收口文档**：第一轮已定；本轮的所有"旧→新"表在那一批进 `.ai/design` 与 `docs/guides`。

**每批开始前的文件清单**与**能力 ID 三元组**按第一轮 §7 的格式写进提交正文；这里不重复。

---

## 11. 停点

- 本文是**第二轮阶段一 + 阶段二**的产出：概念/能力/实现/规范四处增量 + 框架对照 + 合并后的批次。
- **未改代码**；第一轮的十批计划被本表**重排**为 18 个位置（0–9 + 10–17），但每一批的验收标准不变。
- 需要**单独批准**的三条行为变更：**BC-08**（钩子不再隐式调用）、**BC-09**（vk 预建）、
  **BC-10**（宿主报告不再计入引擎计数）。其余是源码级改名/新增，运行时等价。
- 批准后从**第 10 批（规范清扫）**或**第 0 批（characterization）**开始——两者互不依赖，
  建议 0 先做（铁律：任何一刻不允许某能力没有证据），10 紧随。
