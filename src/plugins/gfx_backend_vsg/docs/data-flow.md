# gfx_backend_vsg：逐帧数据流与映射（重写版）

> 本文覆盖：**一帧的数据怎么流**（引擎对象 → 事实 → 计划 → 录制 → 执行），每一层点名负责的单元，
> 以及 L0/L1 契约映射与**支持矩阵**（唯一一份）。
> 契约、设计决定与**坑**见 [`.ai/design/vsg-reimplementation.md`](../../../../.ai/design/vsg-reimplementation.md)；
> 运行时行为（服务/拒绝、调用次数）见 [`backend.md`](./backend.md)；文件地图与构建见
> [`../gfx_backend_vsg.md`](../gfx_backend_vsg.md)。
>
> SceneBridge 时代的数据映射叙述已删（全文在 git 历史）；本文只描述重写版。

## 0. 全景：一帧是"两次调用 + 三段"

```
宿主/引擎                 后端（门面：VsgBackend）
beginFrame()      ──▶   Session.beginFrame（取图、推进槽位时钟）+ FrameRecorder 开帧
render / beginPass … ──▶  FrameRecorder：只记"事实"（合法性 + 数据），不碰资源
endFrame()        ──▶  FrameCompiler + FrameGraph：依赖求值、缺省解析、合法性判定 → CompiledFrame
swapBuffers()     ──▶  ContentAssembly（事实表 + 流 + 块）→ ContentPass 录制 → VsgExecutor 提交/呈递
```

三段的分工是**固定的**：**收集**只回答"这次调用合法吗、它记成什么数据"；**编译**是唯一做决定的地方
（每 pass / 每 draw 一个终态）；**执行**只做机械落地（把已经决定好的事实变成 GPU 操作）。

## 1. 收集段（`FrameRecorder`）

* 引擎把 `RenderCommand`（几何 + 材质 + 程序 + 模型矩阵 + `ResolvedRenderState` + opacity）交给
  `render(commands, camera)`；全屏调用走 `drawScreenProgram(source, program, camera)`。
* 录制器**当场快照**借用的一切（相机矩阵、灯、命令、pass 属性），因为引擎的容器是复用成员。
* 它不判资源、不建对象、不选管线；"无 scope / 不在帧里"是协议（`Protocol`）的答案。
* 一帧的"描述"是值：`FrameDescription`（`FrameArena` 上分配），跨帧不合法。

## 2. 编译段（`FrameGraph` + `FrameCompiler`）

* **依赖求值**：pass 之间按声明的输入求序；环 = 不变式违规 ⇒ 跳过该分量 + 报一次 + `invalid_schedules` 计数，
  其余照跑，帧仍以 `swapBuffers()` 收尾。
* **缺省解析**：视口（未公告 = 整目标）、程序（未点名 = 帧默认）、动态层（`resolveDynamicState`：
  显式 StateNode 赢 pass 深度）、清屏（有效 policy + `bootstrap`/`depth_preserved` 两个**执行器推不出来**的事实）。
* **形状与格式**：`TargetPlan`（`None/Repair/ResizeInPlace/Rebuild`）与 `DepthProbe` 的答案从**同一份目标事实**
  解析；计划携带 `RenderPassCompatibility`（附件格式/深度格式/采样/子 pass）——它进管线键。
* 产物 `CompiledFrame`：每 pass 一个终态、每 draw 一个终态、借用已快照、缺省已解析、合法性已判定。

## 3. 内容世界（`ContentAssembly` 及其件）

**事实表**（每帧 ensure，稳态零构建）：

| 件 | 事实 |
| --- | --- |
| `ContentStore` + `ContentFacts` | 三张表（几何按 identity+revision、程序按 identity+revision+variant+kind、材质按 identity）；行序（`rows`/`order`）让查找走二分 |
| `GeometryFacts` | 通道顺序 = 绑定顺序、索引键归一化为整 buffer、起点一致、Malformed 规则 |
| `DrawBlock` / `LightBlock` / `ShadowBlock` / `ViewBlock` | 四个 ABI 块的字节打包（列主序、平移在 12..14、opacity 在 `params.x`；灯是世界→视图；影子块带"谁的 map + 生产者矩阵"） |
| `MaterialImages` | 材质贴图的 GPU 侧：缓存、上传、立方体逐面、`WhiteImage` 走同一条上传路（fallback 是值不是错误） |
| `StreamUploads` + `Streams` | 流身份（切片 + revision）、别名登记（**不持有字节**）、帧命名寿命、容量边界 |
| `BlockStorage` + `BlockDescriptors` | 每帧 ring（视图/draw）+ 材质 arena；descriptor 按 `ProgramAbi` 声明的 `(set, binding)` 建 |

**半片与集合**：`ContentHalves` 编译"一个 pass 会问到的半片"（(program, revision, layout, variant)），
`ContentPipeline` 负责编译与查找（`VariantPool` 是变体池），`ContentDraw` 把解析过的绘制录成命令图
（"两个仅当"：管线只在变体变了时绑、动态状态只在值变了时发）。

**程序侧**：`ProgramAbi` 把 program 的**文本**读成事实（set/binding、种类、阶段、std140 尺寸、push 范围），
`ProgramVariant` 把"哪几个 define 生效"算成一个数（进管线键），`ContentPush` 按名字填 push 成员。

## 4. 执行段（`VsgExecutor` + 会话）

* `VsgExecutor` 按计划**顺序**录制每个 pass 的命令图：`WindowTarget`（窗口那趟，含"一次清"）、
  `OffscreenTarget`（按加载变体建 render pass/framebuffer）、`HostTargets`（宿主的离屏目标，懒建、快照描述）。
* 目标自己说事实：`shape()`/`instance()`/`written()`/`passVariantCount()`；计划的答案由
  `applyTargetPlans` 应用（窗口路径**问平台**，其余按计划）。
* `Session`：帧时钟 `FrameTimeline`（`submitted` 与 `completed` 分开）、`RetirementQueue`（停靠窗口 = `slots + 1`）、
  呈递与丢帧的记录（丢帧 ⇒ 目标 `attachments_invalidated` ⇒ 下一帧 `Repair(Bootstrap)`）。
* 提交这一步自己说失败（`SubmissionFailed` + 计数），重试的决定留给宿主/会话。

## 5. 上传路径（谁把字节放上 GPU）

| 数据 | 路径 |
| --- | --- |
| 顶点/索引流 | `StreamUploads::acquireVertex/acquireIndex` → vsg 的 transfer 步（`TRANSFER_BEFORE_RECORD_TRAVERSAL`）——绑定对象自持数据，别名即共享 |
| 材质贴图 / 立方体 / 白图 | `MaterialImages`（同一路径；**1×1 白**也走上传，不是清屏节点） |
| 每帧块（视图/draw/材质） | `BlockStorage`（ring + arena）写字节；descriptor 的动态偏移指到当帧的段 |
| 光照 / 阴影 / push | 每次变化一次：`LightBlock`/`ShadowBlock`/`ContentPush`（push 必须紧贴绘制、放进 `Commands`） |

## 6. L0/L1：名字即绑定

* **一个后端选不了 shader 的块住在哪**：`layout(set = …, binding = …)` 写在引擎给的程序文本里；
  不匹配 = 着色器读没人写过的地方。所以 `ProgramAbi` 只回答"文本声明了什么"，绑定侧照它建集合。
* **块角色按 L1 类型名认领**（`VineViewBlock` / `VineDrawBlock` / `VineMaterialBlock` / `VineLightsBlock` /
  `VineShadowBlock` / push 结构），**不是**按 binding 序号。
* **变体是事实的一部分**：`VINE_DIFFUSE_MAP`（材质有图）、`VINE_VERTEX_COLOR`（几何自己写了颜色通道）、
  `VINE_TEXCOORD_CUBE|UV` 由 `ProgramVariant` 从事实算出，进管线键。
* 引擎自带程序的声明与实测 ABI 逐条钉在 `tests/test_vsg/ProgramAbiTest.cpp`；自定义程序的契约见
  `.ai/design/vsg-custom-shader.md`。

## 7. 支持矩阵（唯一一份）

**支持**（各有真设备用例）：内容绘制（索引/非索引、点/线/三角）、MRT 与深度附件、材质（贴图/立方体/天空/
无图白）、方向光三槽 + 环境补光、阴影（三事实解析）、自定义 program（声明式 ABI + 变体）、全屏调用
（拷贝/PiP/延迟光照/深度采样 + push）、离屏目标（多写者 LOAD、resize/rebuild、借用深度）、读回（颜色 RGBA8 /
深度浮点）、窗口（采纳宿主表面、跟随 resize、丢帧自愈）。

**不支持 / 已登记**（见设计文档 §6 的登记表）：块预算按需增长（B5）、`MaterialImages` 的 LRU 与容量上界（A6）、
SDK 侧内容释放入口（A3）、`Material` revision（A4）、逐 buffer revision 的省带宽（B6）、首帧工具叠层（低）、
会话侧"画面已落地"的事实（等真宿主）。

## 8. 一帧时序与成本

```
beginFrame → [render/beginPass/endPass …] → endFrame(编译) → swapBuffers(内容世界→录制→提交→呈递)
```
每帧成本：**record 半段**（facts/计划/块/流）在 1 drawable 的配方里是 7–19 µs（Release，四类编辑都一样平），
**commit 半段**（录制/提交/呈递，lavapipe）是 0.8–1.0 ms，唯一花毫秒的编辑是**新 program 的第一帧**
（≈ +1.6 ms 一次性）。数字与配方见设计文档 §11.16cl（引擎侧收集）与 §11.16cq（后端四类编辑）。
