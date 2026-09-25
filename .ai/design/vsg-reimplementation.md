# vsg 后端"从零重新实现"：契约、坑与实施记录

> **这份文件有三个用途**：①当前还生效的**契约**与设计规则；②**坑**（血的教训，按主题索引，永久保留）；③压缩的
> **实施记录**（每片只留规则 / 判据 / 数字，逐片叙述已删）。
>
> **2026-09-25 清理**：删掉已经过时的"提案"部分——能力对照表、类型草图、与旧实现的差异与外部评审逐条、里程碑、
> 风险与不做的事、上游依据表，以及逐片的叙述文字。**被删的全文在 git 历史里**：
> `git show 8c223ba:.ai/design/vsg-reimplementation.md`（需要考古时去那里找；§11.x 的编号全部保留，见 §8）。

## 1. 契约与平台事实（不可协商）

### 1.1 `RenderBackend.hpp` 的边界（逐字实现，不发明）

| 契约 | 内容 | 后果 |
| --- | --- | --- |
| 帧配对 | `beginFrame()` 可获取交换链图像；**只有** `swapBuffers()` 呈现，且是一帧最后一次调用 | 需要一个"帧令牌"（D5）；空帧也必须提交 |
| pass scope | scope 属性（target / order / depthMode / clear）属于该 pass 并在 `endPass()` 丢弃；每次绘制属性（viewport / lights）只服务下一个绘制调用；无 scope 的绘制**必须拒绝并上报** | 协议是一台状态机，不是一个函数（D1） |
| 借用一切 | 相机 / 命令 / 灯 / target / 程序只在调用期间借用；pass 只在 scope 内借用 | 后端必须**当场快照**，不能持有（D3 的 data 层） |
| 增量 | `render()` 必须增量对账；稳态帧不重传几何、不重编管线、不增长 | 三层键 + 键审计（D3，§11.16cq 已实测） |
| 失败语义 | 不抛异常跨接口；`initialize()` 返回 false 时**后端自己**清理半成品（引擎不会调 `shutdown()`） | 会话必须"要么完整要么不存在"（D4） |
| 保留态 | 可以由 `release*` 释放，但**不得要求**宿主必须调它才正确；不得随帧数增长 | 账本 + park 环（D4） |
| 线程 | 单线程、串行、不可重入；但不得假设 `initialize()`/`shutdown()` 同一线程 | 全设计不加锁，但会话状态必须整体替换而非逐成员改 |
| 循环依赖 | `graphics/src` 里**没有**环检测（`Group` 的 cycle 检查是场景图父子关系） | 后端是唯一能看见环的地方 ⇒ 环必须有**显式失败语义 + 计数**（P0-7 / D2.1） |

### 1.2 平台事实（不是"实现的选择"）

| 事实 | 影响 |
| --- | --- |
| 这个 vsg 版本**没有** dynamic rendering，也不传 `VkPipelineCache` | 必须走 render pass + load-op 变体；重建设备 = 重编全部管线 ⇒ "移动会话"比"重建会话"值钱 |
| push constant 保证预算 **128B** | 光照 + 视图必须挤进一个块；前向顶点阶段读视图矩阵 ⇒ 灯走 UBO，全屏路径才用 push |
| reverse-Z（远平面 0）+ 比较算子反转；世界 CCW 为正面 ⇒ **framebuffer 空间 CW** | 投影、清屏值、比较算子、正面声明必须是**同一处**的四个侧面 |
| 槽表每帧只填一个（`RecordAndSubmitTask::index()`） | 槽数要"**逐帧学到**不再增长"；学不到之前不许 park |
| 上游不提供 `vkCmdBeginRendering` / EDS2/3 入口 | 见上；能力事实由 `scripts/check_vsg_upstream_capabilities.py` 复核 |
| 上游参考只有 `/opt/opensrc/vsgExamples`（1.1.15，比库（1.1.16）差一小版） | 只当 idiom 参考，**不作行为性结论的依据** |

### 1.3 被迫同形的地方（照抄不创新）

管线布局/描述符按文本声明走；render pass 兼容性按 Vulkan 的规则；WSI 的 acquire/present 配对；槽位 fence 复用。
"看起来更干净"的写法如果改动了这些，就先证明旧写法错了。

## 2. 架构与一帧

### 2.1 两个流，不是一条链

**收集流**（`beginFrame` → `render`/`beginPass`/`endPass`）产出 `FrameDescription`；**执行流**（`endFrame` 编译 →
`swapBuffers` 录制提交）消费 `CompiledFrame`。收集侧只记录"事实"（合法性 + 数据），执行侧只做机械落地；
**决定资源的地方只有编译段**。

### 2.2 六个核心对象 + 一个横切面

`Protocol`（合法性）/ `FrameRecorder` / `FrameCompiler` / `FrameGraph` / `VsgExecutor` / 资源世界（目标、池、缓存）
+ 横切面 = 证据（相位表、计数器、像素）。对象少是刻意的：每个都有一句"它答什么"，边界写在头文件里。

### 2.3 物理边界：`core/` 不许 include vsg

机器校验（`scripts/check_include_hygiene.py` 的 `core_layer_findings()`）：`core/` 用 `vine/graphics/*`（SDK）是允许的，
`vsg::` 一次都不许出现。这条是"哪些规则可以无设备测试"的物理保证。

### 2.4 一帧的生命周期

```
beginFrame()  → 取图（槽位逐帧学到）→ recorder.beginFrame(token)
  render/beginPass/endPass ×N      → 只记录"事实"，不决定资源
endFrame()    → 描述定稿 + compile（graph → 依赖求值 → 缺省解析 → 合法性）
swapBuffers() → 录制命令图 + 提交 + 呈现；**关帧的是这一步**
```
暂停点：`endFrame()` 只离开 pass；`beginFrame` 之外的"暖机"调用（无帧里 render）**不收集**（协议拒绝，
`render()` 静默返回，`description()` 还是上一帧）。环上的 pass 被跳过并计数，其余照跑，帧仍以 `swapBuffers()` 收尾。

### 2.5 P0 定义表（实现契约，钉死不再讨论）

| # | P0 问题 | 结论 |
| --- | --- | --- |
| 1 | 协议边界 | `Protocol` 只答合法性（允许/丢弃/拒绝并上报），不做策略（D1） |
| 2 | Intent → Plan 的 normalize | `FrameDescription` 允许保序/重复/后设覆盖/非法调用/缺省；`CompiledFrame` 必须"每 pass 一个终态、每 draw 一个终态、借用已快照、默认已解析、合法性已判定"（D2） |
| 3 | 环 | 显式失败语义 + 计数（`invalid_schedules`），不静默按顺序执行（D2.1） |
| 4 | 键 | 三层（identity / dynamic / data）+ 键审计（D3、§4） |
| 5 | 寿命 | owning 资源世界 + 退役队列 + 只读校验器（D4） |
| 6 | 时间 | `submitted` 与 `completed` 分开（D5） |
| 7 | 目标 | `planTarget` / `depthPlan` 纯函数 + 加载变体（D6） |
| 8 | 存储 | 持久 arena（材质）与每帧 ring（视图/draw）绝不混（D7） |
| 9 | 证据 | 三重验证：验证层、计数器、像素（D8） |
| 10 | 不改的 | 不改 SDK 公开 API、不引入第三方依赖、不为"更漂亮"偏离平台事实 |

## 3. 设计决定 D1–D8（规则版）

* **D1 合法性收在一个类型里，但只判合法性。** 每个 (状态 × 入口) 有确定答案（允许/丢弃/拒绝+上报）；
  策略（修不修、退不退）留在别处。判据：无设备的状态转移表用例。
* **D2 三段式（收集 → 编译 → 执行），normalize 有明确语义。** 收集侧允许重复/后设覆盖；编译侧必须每 pass/draw 一个
  终态、缺省全部解析（viewport 未公告 = 整目标；程序未点名 = 帧默认；动态层 = `resolveDynamicState`；清屏 = 有效
  policy + `bootstrap`/`depth_preserved` 两个执行器推不出来的事实）。执行器只做机械落地。
* **D2.1 环 = 不变式违规。** 跳过整个环的分量 + 报一次 + 计数；分量外的 pass 照跑；帧照旧提交呈现。
* **D3 三层状态模型 + 键审计。** "改了必须重编/重建"的进 identity；"变了只要一条命令"的进 dynamic；
  "每帧重写字节"的进 data。键审计 = 行为断言（§11.16cq 落地：材质/几何/状态 `created` 不动，program 与
  topology 类各 +1；目标改尺寸 `offscreen_builds` 不动）。
* **D4 资源寿命。** owning 资源世界（几何/管线/描述符/目标按 key 查找与创建）+ 退役队列（`active → replaced →
  retired → destroyed`，按帧时间轴）+ 只读校验器（Debug 每帧）。销毁只有两条合法路径：整会话替换（前置**一次
  计数过的 device idle**）或退役出队。帧路径 `device_waits == 0` 是判据（**破坏性路径才允许计数过的等待**）。
* **D5 FrameTimeline：`submitted` 与 `completed` 分开。** 推进点必须是帧的最后一步；拿不到"完成的证据"
  （槽数还没学到）时**退回计数过的 device wait**，不许猜 N+1。`retireWaitCount()` / `deviceWaits()` 是判据。
* **D6 目标三件套 + 纯函数。** `planTarget`（`None`/`Repair(Bootstrap)/ResizeInPlace/Rebuild`）与 `depthPlan`
  是纯函数；形状/兼容性/加载策略分开；`bootstrap` 与 `Load` 互斥（LOAD 一张 UNDEFINED 镜像是 bug）。
* **D7 两类存储 + 三类作用域绝不混。** 材质 = 持久 arena（增长不搬块、reset 是唯一失效点）；视图/draw = 每帧 ring；
  份额帧初快照、帧末清扫。
* **D8 证据横切、三重验证。** 相位表：一行 = 装配 → 断言（像素读回 + 计数器）→ 期望的计数器增量（含"必须不变"）。
  "测试过了"不是结论：0 VUID / 0 SYNC-HAZARD / hygiene / 相位行 / 应用画面都要过（§5.8）。

## 4. 键与状态：**当前实现的名字**（`core/Keys.hpp`）

| 层 | 当前拼写 | 允许的变化方式 |
| --- | --- | --- |
| identity | `PipelineKey`：`kind`（内容/全屏）、`program`+`revision`、`vertex_layout`、`compatibility`（附件格式/深度格式/采样/子 pass）、`depth_sampleable`、`sampled_color_count`、`sampled_depth_count`、`variant`（define 集合）、`topology`（**类**，见 §5.2） | 变了 = 重编管线（唯一允许重编的地方） |
| 交换半边 | `LoadOpVariantKey`：color/depth 的 load/store 与 initial/final layout | 同一管线在多个兼容变体上都能用（bootstrap 变体合法） |
| dynamic | `DynamicState`：depth、cull、polygon、topology（**同类内**）、blend | 一条 `vkCmdSet*`；**永远不许重编** |
| data | 视图块（288B）、光照块（112B）、`VineDrawBlock`（矩阵 + opacity）、材质块（64B）、push（≤128B） | 就地字节写 / 描述符 re-point |

**"计数是身份不是状态"**：绑定在描述符布局上的东西（采样彩色数、深度数）必须进键；extent / load-op / 布局 /
material 值 / 流字节 / cull 全部**不进**。

**活着的证据计数**（读它们，不要另造）：

| 问题 | 计数 |
| --- | --- |
| 内容表建了多少行 | `ContentStore::builds()` / `geometryEntries()` / `programEntries()` / `materialEntries()` |
| 流上传 / 别名 / 驻留 | `StreamUploads::uploads()` / `aliases()` / `live()` / `objects()`（`live()==objects()` 必须成立） |
| 管线编译 / 复用 / 淘汰 | `VariantPool::created()` / `reused()` / `variants()` / `evictions()` |
| 录制与编译 | `Observe::counters()` 只有 `passes` / `draws` / `invalid_schedules`（其余数字在各自的层，见 observe 的注释） |
| 设备空闲 | `Session::deviceWaits()`（`RetentionQueue` 自己的账） |

## 5. 坑（不可再踩）

### 5.1 协议与帧

* **关一帧的是 `swapBuffers()`**（`endFrame()` 只离开 pass）。忘了它：下一次 `beginFrame` 被拒并自带协议诊断，
  而且 `description()` 还是**旧帧**的描述（症状极具误导性：新帧的计划里是旧命令）。
* **空帧也是帧**：照样编译、录制、呈递（acquire 过的图必须有一趟 render pass 走过，否则 present 一张 UNDEFINED 的图
  = VUID `01430`）。空**pass** 不是 pass。
* **借用必须当场快照**：引擎的 `resolved_inputs_` 是复用成员（下一个 pass 就会改），`RenderCommand` 只活到
  `render()` 返回。后端持有 = 读别人的栈。
* **无 scope 的绘制拒绝并上报**；scope 属性属于 pass，每次绘制属性只服务下一个调用。
* **环没有上游拦截**：不认定它 ⇒ 按当前顺序静默执行（错画面、0 VUID）。判定 + 跳过分量 + 计数，分量外照跑。
* **`beginFrame` 之外的"暖机" render 不收集**（协议拒绝，静默返回）。

### 5.2 键与管线

* **两个最贵的缺陷族**：把可动态化的东西放进键（StateNode 一改就重编），或把要换的视图留在节点里（resize 要重建
  整批程序槽）。审计判据见 §11.16cq。
* **拓扑只能进"类"**：`VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` 只允许同类切换（除非实现报 unrestricted），
  跨类 = 新管线（`PipelineKey::topology` 的理由写在头文件里）。
* **引擎的格式枚举是投影**：`RGBA8` 里 sRGB/线性、RGB/BGR 都是兼容性的一部分；设备格式必须由设备层交上来，
  "未知"**不会**静默并族。漏了它 ⇒ `VUID-...-02684`，而像素**照过**（判据是验证层）。
* **"两个视图"不等于"两份编译"**：vsg 按状态集合复用；哪些东西算兼容必须由**键**回答完整，视图救不了键。
* **管线的第一个新变体要编译**：实测 ≈ +1.6 ms 一次性（Release，lavapipe），之后是复用；"每帧换 program"本身
  不再编译（§11.16cq）。
* **池容量 65（FIFO）**：淘汰后同键再来会重新 `created`——容量与淘汰是身份的一部分（审计）。
* **`shadow_bound` 这类抄来的键位**：接上真事实后按实测删（本后端 ABI 里它是冗余的）。
* **别信"位置"**：块角色按**类型名**认领（material 在 0、lights 在 2）；按 binding 序号给角色会读成别人的块。

### 5.3 朝向、裁剪与深度

* **reverse-Z 三件套**：远平面 **0.0**、比较 `GREATER`、深度清 **0.0**（清 1.0 + GREATER = 每个片元都被丢）。
* **视图块里的 `proj` / `view_proj` 必须折进设备约定**（x/w 不变、y 取反、z 记作 `0.5 - 0.5*z`），
  `view` / `inv_view` **不折**。不折的失败形态：三角形落到裁剪 z = -0.714 ⇒ 只剩清屏色，而**绘制仍被记录、
  0 VUID、0 诊断**。
* **折反看不出来**：z 镜像（近→0）时画面照样过测，只有算术用例能抓 ⇒ 算术用例与像素用例各管一段。
* **front face = CLOCKWISE**（vsg 的投影翻转 Y；照抄直觉的 CCW 会让每个 cull 模式作用在错误的面——静默）。
  混合**对单附件内容绘制恒开**（因子 `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`），`blend.enabled` 只选因子；
  多附件 pass 与全屏 draw **不混合**（`color_attachments > 1 || !draws_content`，见 §11.16bl；像素专项见 §11.16cv）。
* **全屏调用不测深度是它的定义**（正典三角形 z = 0 = reverse-Z 远平面；测了就整块消失）。
* **全屏 push 必须紧贴绘制、放进 `Commands` 节点**（vsg 的 stateCommands 按 slot 记录 ⇒ 命令发出去了，
  shader 读到的是恒等矩阵，画面"看着正常"）。**push 的阶段（顶点/片元）是 ABI 的一半**，发错阶段 0 VUID 静默。
* **引擎背景的判据是"这条通道写没写过"**（G-buffer 的 position 附件 `w = 1`），不是 `dot(pos,pos) < 1e-6`
  （后者在相机贴脸时留一个固定色洞）。
* **顶点数/ABI 声明像素看不见**（被 viewport 裁掉、或画的是同一张图）⇒ 这类主张的判据是算术用例。

### 5.4 块与描述符（ABI）

* **`VineMaterialBlock` 是聚合体**：`{}` 只初始化成员，**不碰尾部填充**（52..63）⇒ 比较用逐成员 `operator==`；
  字节级比较会把"没改过"报成"改过了"。
* **dynamic offset 必须是设备 `minUniformBufferOffsetAlignment` 的倍数**：不合规**不给命令** + 计数
  （不要把验证错误交给驱动）。
* **别把每帧变化的偏移烤进描述符集**：池会把同一句柄发回下一帧，而上一帧的命令缓冲还在用它（VUID `03047`）。
* **替换一张描述符集 = 旧的那张必须进退役窗口**：`BlockDescriptors::repoint` 换的是 set 对象，而池会把**已被释放**
  set 的 `VkDescriptorSet` 句柄发给替代品 —— 新 set 的 `compile` 发生在**下一帧**，那时被换下的帧仍是 pending ⇒
  `vkUpdateDescriptorSets` 打在在用的句柄上 = 又是 `03047`（实测：本片增长路径；M4 变异=只删停放 ⇒ 用例全绿、
  验证层 1 条）。命令缓冲对 vsg 对象（`BindDescriptorSet` 的 `ref_ptr<DescriptorSet>`）的引用**不足以**保住句柄。
* **布局要声明两种绑定**（块 + 采样）；**空隙必须是真布局**（空指针 ⇒ `PipelineLayout` 里出现无效句柄 ⇒ 段错误）。
* **"什么都不声明"就是"什么都不绑"**：程序文本没声明块，就**没有集合可绑**（夹具别无条件绑块集 ⇒ VUID `00360`）。
* **声明了 push 却没填 ⇒ 拒绝那一半**（"黑屏是因为矩阵是零"不算诊断）。
* **全屏集合的每个 binding 都从程序自己的声明来**（写死 binding 5 读到未定义数据）。
* **std140 尺寸按对齐算**（成员字节相加会漏掉补齐：52 → 64）。
* **`#ifdef` 分支的条件决定事实**（不管 define 全算 taken ⇒ 同一个 `(set,binding)` 上出现两种类型 ⇒ Malformed）。
* **"没贴图"的 fallback 是值不是错误**：白图必须与其他图一样**走上传**（"某帧来录一下"的机制一旦没有录制方
  就静默失效 ⇒ 图永远 UNDEFINED + 描述符撒谎 = 每帧一条 `09600`）。清完必须停在描述符声明的布局（否则像素
  看着对、验证层 4 条）。
* **材质贴图的种类必须对得上声明的采样器**；立方体逐面交织错了是**静默**的（字节总数一样、每个拷贝区域合法）。
* **布局/兼容性这类主张，判据是验证层**（§11.16r/ah/ai/al 四次实测：像素全绿、VUID 成串）。

### 5.5 流、几何与别名

* **流身份 = 切片 + revision**，而且 `revision` 取的是 **Geometry 的** revision（`GeometryFacts`）⇒ 一次公告移动
  **所有**流的身份：替换 buffer 对象与就地改写字节**一样贵**（2 上传/帧的实测）。
* **被重填的 buffer 是新流**（不是旧副本）："revision 移出身份 ⇒ 别名到旧字节"的变异已被用例钉住（§11.16cr）。
* **派生通道**（白载体、零 UV、法线）不可共享（字节属于一个几何）。
* **一个几何的所有通道必须从同一个顶点起算**（否则"索引 0"对每个通道指的是不同顶点）。
* **索引键归一化为整 buffer**（切片共享同一份上传）；**别名判据 = 同一个 bind 对象**（指针相等）。
* **别名登记不持有字节**；淘汰 ≠ 释放（读者仍持有 bind）；寿命靠"帧点名"，宽限期 = `slots + 1`；
  容量 512，淘汰**最久没被点名**的不是最早插入的。
* **测试自己踩过**：用栈对象构造 `intrusive_ptr` ⇒ 析构 `free(): invalid pointer`；SDK 对象必须堆上由
  `intrusive_ptr` 拥有。

### 5.6 目标、寿命与设备空闲

* **销毁只有两条路**：整会话替换（前置**一次计数过的 device idle**）或退役出队。管线销毁与在飞提交的竞态已实测
  报过 `VUID-vkDestroyPipeline-pipeline-00765`（修复见 §11.16cr/B7；再出现先查"最近被替换或逐出的管线"）。
* **退役窗口 = `slots + 1`**；槽数**学不到**时 `retire()` 返回 false 而不是猜 ⇒ 调用方退回**计数过的** device wait
  （`deviceWaits()` 就是这条的判据）。
* **丢帧 = "内容不可信"的事实**（`invalidateAttachments()`）：下一份计划答 `Repair(Bootstrap)`；**只有清屏
  （bootstrap）能修这个事实**，LOAD 不能；resize 换集合时事实随旧集合消失（`written = false`）。
* **`built` = "能 LOAD" = `written`**：新目标第一趟必须清（LOAD 一张 UNDEFINED 镜像 = VUID `09600`，只有同步
  验证看得见）。`written` 必须**复位**（否则第二趟被当成 LOAD）。
* **load op 是交换半边，不进键**——但**依赖掩码绝不能随变体变**（变体连自己的 framebuffer 都不兼容）。
* **形状变了才是真的重建**：连**格式**一起核对（"同一数目、同一深度、不同格式"曾放过去 ⇒ `00904`）。
  兼容性主张**像素抓不住**（老教训在新臂上重演）。
* **借用的深度**：借用方不建镜像、用出借方的视图、`borrowed` 恒不可采样；可采样决定收尾布局；
  **捕获屏障必须命名真正的当前布局**（写死 `DepthAttachment` ⇒ `01197`）。
* **读回是一张分类表**，分类**先于任何工作**（拒绝时不看设备、不拷贝）；"永远不行"和"还没行"必须是不同答案；
  颜色只打包 RGBA8（float 附件按 `unreadable-format` 拒绝，而不是误解字节）。
* **计划与资源世界必须对得上**：目标形状、输入项数与每项彩色数**逐项**核对，不等就**整趟拒绝**并说出是哪一项。
* **`written()` 在"录了但没提交"时也为真**（事实是"有人往里录过"），对 bootstrap 判断够用。
* **窗口的答案问平台**：窗口 `facts()` 的 live 采样与换尺寸答案由平台/表面给出，不采纳计划的主张。

### 5.7 WSI 与窗口

* **present 到"还没 viewable"的窗口永远不会到**，读它会 `BadMatch`（`readError()` 8）：构造要等 viewable
  （>2 s 的映射实测过），测试要 settle 后再呈现同一张画面。
* **读窗口读后端日志里那个 id**（渲染区）；名字/Qt 容器会读到永远亮着的 chrome。`xwinresize.py` 改的是**顶层父窗口**
  （resize 一个子窗口会被布局撤销）。
* **呈现是异步的**：commit 后立刻读可能拿到旧后备存储（全黑）⇒ 多跑两帧再读。
* **每帧换命令图会拆掉在飞的 fence/semaphore/命令缓冲**（实测 12 条 VUID）⇒ 稳帧别每帧 `assignFrameGraphs`。
* **丢帧之后会话要自己活下来**：acquire 过而没呈递的图让 WSI 缺一张（forward-progress 警告 + present 没 acquire
  的图）；会话把这张图还回去。
* **演示窗口的启动尺寸不确定**（窗口随日志 dock 的内容长；六次启动六个尺寸，一次门禁假红）⇒ **门禁先把窗口拖到
  固定尺寸再判**（§11.16cs）。
* 起应用要 `exec` 并且**按名字杀**（`pkill -x Vine`）；遗留窗口会盖住测试窗口（`XGetImage` 读的是可见内容）。

### 5.8 证据与门禁

* **"测试过了"不是结论**：套件全绿的同时验证层可以一直报 VUID。门禁读输出里的 `VUID` / `Validation Error` /
  `SYNC-HAZARD` 计数，跳过即失败，`[selftest]` 行不许有 `FAILED`，hygiene 三个脚本、应用阶段三条画面判据都要过。
* **假绿要能被门禁自己抓到**：忘了 `swapBuffers()`、空 pass、伪造的 VUID/skip 行——判据都改成过"不会骗过自己"的形态
  （"前面一张表红、后面一张表干净收尾"曾经骗过旧判据）。
* **相位表**：一行 = 装配 → 断言（像素 + 计数器）→ 期望的计数器增量（含"必须不变"的项）。
* **稳态的判据是"两帧都不分配"**（一帧不分配只是那一帧的事实）。
* **变异反证要先自己验一遍**：两帧身份一致、字段恒零的变异**不会红**（无效变异两种形态都踩过）。
* **应用阶段的画面判据**（三条）：非近黑 ≥30%、预览条最大通道 ≥64、**平背景占比 ≤5%**（第三者是"G-buffer 停止
  记录"的判据）；窗口尺寸由门禁钉住，VUID 白名单**为空**。
* 测试二进制也在验证层管辖内；`VK_LAYER_ENABLES=…SYNCHRONIZATION_VALIDATION_EXT` 可整仓跑。

### 5.9 构建、插件与文档检查

* **插件源列表是 GLOB**：新增 `core/`/`api/` 的 `.cpp` 要**重新 configure**；`tests/test_vsg/CMakeLists.txt` 要显式加
  两处（`SRC_FILE_LIST` + `target_sources`）。
* **demo 宿主是插件**（`plugins/vine/app_shelld.so`）：`ninja -C build Vine` **不重建它**；改完 demo 要
  `ninja -C build app_shell`（门禁跑全量 ninja，所以只有手工改时踩）。
* **`VSG_MAX_DEVICES` 默认 1 是有意的绊线**：任何"同时两个 device"的路径都该当场抛错；别手工改构建树的这个值。
* `vn::String` 是 `std::u8string`（字面量写 `u8"…"`）；`vn::math::Mat4d` 默认构造 = **单位阵**；
  `vsg::Image::vk(deviceID)` 要下标；`vsg::Exception` **不是** `std::exception`（只 catch 后者会漏）。
* **三份 living 文档**（`gfx_backend_vsg.md`、`docs/backend.md`、`docs/data-flow.md`）受 `check_doc_symbols.py` 管：
  文档点名的单元必须存在；`src/` 与 `include/vine/vsg/` 下每个单元都必须被点名；段落标题含"历史登记"可整段免检；
  必须点名不存在的单元时行尾加**裸的** `<!-- drift-ok -->`。

**过程与脚本（旧后端时代总结，仍然适用）**

* **证据门禁前必须整包 build**：只建目标会留下陈旧二进制（改了类布局后单测绿、自检却崩
  `malloc(): … corrupted`）——这种"证据"不可信。
* **脚本批量改代码的四条护栏**：①同一文件的多个切割段必须**合并后一次写回**（否则互相覆盖）；
  ②改写必须**报告未命中项**（静默漏改到编译期才暴露）；③**不要对"脚本刚生成、缩进变过"的文件打补丁**
  （先把增量打在干净源上、再跑合并脚本）；④改门禁脚本后先 `bash -n` 并确认证据行**真的打印**
  （"只看 `RESULT: PASS`"骗过闸门一次）。
* **替换函数要整段**（只换函数头会留下旧函数体）；锚点必须选在两版**真正不同**的行上。
* **拆出新 TU 后第一个 include 是自己的头** —— 被传递包含掩盖的缺口会立刻现形（顺手补全 include）。
* `tests/test_vsg/CMakeLists.txt` 的 `SRC_FILE_LIST` 用 **tab** 缩进（用空格匹配必然失败）。
* **clang 偶发瞬时崩溃**（frontend exit 135）：先重跑同一条命令；重跑干净后**仍走完整判据**，
  不许当"通过"。
* **本机环境**（2026-09-15 实测）：`http(s)_proxy`（`127.0.0.1:7890`）是坏的（TLS 握手中断）⇒ git 推拉用
  `env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY …`；`vine_shader_check.sh` 需要 Linux
  `glslangValidator`（本机没有）⇒ 着色器变体编译由 `test_vsg` 的 `GlslCompileTest` 覆盖。

### 5.10 引擎侧（场景桥与着色约定）

* **声明与绑定都按名字**：任何 `assign*` 对未声明的名字都是静默 no-op ⇒"加了声明才算接上"；写文档/报告前先看
  绑定侧的门控条件。
* **`skyMap` 与 `diffuseMap` 同一行**（引擎自带天空程序的文本自己写着它是 material 的纹理）；材质图的**种类**必须
  对得上声明的采样器。
* **影子块按"目标认领"选**（同一 light 身份 + 深度可采样 + 生产者公布了矩阵，三个事实缺一不可），
  **不是**"第一个深度可采样的输入"（那会把 G-buffer 的深度当太阳的 map——参考实现的实测缺陷）。
* **色彩空间契约（D3）**：引擎内部一律线性；颜色图声明 `*Srgb`、数据图 `*Unorm`；编码只发生在硬件两头
  （采样器解码 / 窗口 surface 编码）；说错两个方向都静默（颜色图 `*Unorm` ⇒ 偏亮，数据图 `*Srgb` ⇒ 偏暗）。
  仓库里第一个实例是 demo 的六张 JPG 天空面（§11.16cp）。
* **`Material::specular()` 的 alpha 是强度**（两条路径都真的乘它；默认 0.5 ⇒ 接线会改画面）。
* **G-buffer 的 albedo 用 16F**（暗色阶在 8 位里会在亮光下露出台阶；`intensity ≤ 1` 时两者一样，§11.16co）。
* **插话（episode）是"调用方的"**：半片的两种句子归半片、灯掉落归 pass/会话、拒绝报告"先说一遍再说一共几条"
  ——`ContentPass` 每帧新建，把状态挂在它上面等于每帧重报。

## 6. 登记与触发器（未做）

| 项 | 内容 | 触发器 |
| --- | --- | --- |
| **B6** | `StreamKey.revision` 取 Geometry 的 revision ⇒ 任何数据公告重传所有通道；逐 `Buffer::revision()` 能省未改通道，但契约更弱（漏报静默） | 多通道大网格宿主报上传带宽（**2026-09-25 已量化，见 §11.16cw**：512²×4 通道+索引一次公告重传 16.98 MiB ≈ +30 ms/帧；触发未兑现） |
| **B7** | 管线销毁与在飞提交的竞态：已按设计规则给破坏路径加计数过的 device idle；此后 20 次单例 + 2 次全量套件 + 两棵树门禁 **0 VUID，未再复现** | 再次出现时先查"最近被替换或逐出的管线"与 teardown 的 `deviceWaits` |
| **B8** | **"跨帧差异"相位：可读回子类已在门禁里，F−N 洞仍在**。新相位 `VsgBackendTest.TheLastFrameOfAMovingSequenceKeepsItsOwnViewBlockValue`（每帧动相机 0.95..0.75、末帧 0.1；片元 `step(0.75,|cam_pos.x|)` 把红通道变成 0/1 两类，判据与色彩空间无关）守住**末帧读到别帧数据 / 绑定冻结·过期**的形状；变异（`recordCommand` 读侧绑定落后一 slab）⇒ 2/2 红。§11.16dd 的受害帧是**最旧在飞帧（F−N）**：它的输出不进 readback（只见最新帧），且 2026-09-25 探针实测（写偏移/slab 轮转/学到槽数=3/帧写节奏）本机框架逐帧节流 ≈ 1 GPU 帧，F−N 总在覆盖写之前收尾；负载压到 4096²×64 遍（≈1G 像素/帧）也造不出重叠 ⇒ **该洞继续由结构性旋转用例承担**（`BlockStorageTest.TheSlabsRotate…`） | 触发不变；真机/换框架再现同类画面缺陷时，先复核"唯一在飞数/布局"与相位边界（§11.16dd 诚实登记） |
| **B9** | **在飞槽数有地板、学到更深就重布局（已收口，2026-09-25，§11.16de）**：① 构造地板——`BlockStorage::create` 把五个 per-frame 形态**上举**到 `core::perFrameCopies(kAssumedInFlightSlots)`（`layoutForInFlight`；旧字面量 `slots{3}` 再也造不出一条存储），选上举而非拒绝：`create` 的 null 是设备/缓冲失败的语言，太浅的存储没有正确的服务方式；② 学到更深数 ⇒ **一次替换**——`growBlockStorageIfNeeded` 每帧把 `Session::slots()` 与预算增长**合并**成一次 `create` + `adoptBlockStorage`（分开做会丢另半请求：替换身的 `growthNeeded` 从零起），Info 记录两个来源。用例 +3：无设备策略（上举/逐形态/不缩/预算不动）、设备建造地板、接缝 `BackendContentAccess::assumeInFlightSlots` 触发重布局 + 次帧不再换。变异 4/4 红（地板移除 / 上举失效 / 接线断 / 谓词过宽[套件崩]） | 真机首次报出 N≥4 时核对 Info 文案与内存代价（接缝走同一条分支，但“真的学到更深”本机未自然观测） |
| **A2 残留** | 共享流的"释放半边"已按新形态改掉（寿命 = 帧点名，`releaseUnseen`）；旧 `reader` 计数与 `release()` 是**旧实现**的缺陷，已随重写退场——此条仅作历史 | — |
| **A3** | SDK 没有内容释放入口（`releaseGeometry/Material/Program/Texture`）：登记的"不改 SDK" | 宿主报告内存压力（**2026-09-25 账本已量化，见 §11.16cz**：12 轮创建+丢弃全清；唯材料槽残留；slot 知识学到前的丢弃会保留整会话） |
| **A6** | `MaterialImages` 淘汰是 FIFO 而非 LRU；`ContentStore` 无容量上界 | 同屏活纹理逼近 256（**2026-09-25 语义已钉+量化，见 §11.16cy**：A/B/X 判别钉 FIFO；200 张循环 = 0 缺失，300 张 = 每轮 300/300 全重建） |
| **A4** | `Material` 是全 SDK 唯一没有 revision 的内容类型（后端每帧逐命令 compare-and-write；实测 ~10 µs/帧量级，比噪声小） | 材质数量大到"每帧 O(命令数) 次块比较"进入剖析前列（**2026-09-25 已量化，见 §11.16cx**：命令地板 ~28 µs/条（Release）；>256 不同材料 = 0 命中、每帧全量重写+驱逐） |
| **首帧叠层** | 一帧没有工具叠层（§11.16cd 当时读作“表面尺寸未知”） | 低（下一次 app shell 视觉工作）——**已查明并收口（2026-09-25，§11.16db）**：预热帧叠层有效；第一个上屏帧画在 Qt 布局**瞬态**尺寸（实测 100×30）上，角落 HUD 盒放不下 ⇒ 旧算式在那算出**负视口**（-2×-2），已夹到 0 并配钉子用例；用户可见性≈零 |
| **会话侧"画面已落地"** | 把窗口读那条链从"测试碰运气"变成"宿主可查的事实"（WSI present fence） | 下一个真宿主接窗口时 |
| **窗口随日志长大** | demo 的窗口启动尺寸不确定（§11.16cs：六次启动六个渲染区，最大 3418×1110；门禁侧已先 resize 到 800×600 再判图；宿主/框架侧未修——默认窗口尺寸是产品决定） | **已收口（2026-09-25，§11.16dc）**：`MainWindow` 显式 `resize(800×600)`（= 最小值 = 文档默认 = 门禁判图尺寸）；本机修前/修后各一批都读到 378×247（今天未复现漂移，改动按构造去掉“首 show 跟随布局 sizeHint”）；钉子用例 + 变异 1/1 红 |
| **B1/B2/B3** | 三张表查找已二分（§11.16bz/ca）；每 pass 堆分配与 `mallinfo2` 的证据强度问题按 §11.16bx/by 的测量结论维持 | — |
| **BlockStorageTest 偶发** | `TheRegionsAreLaidOutOnceAndDoNotOverlap` 首跑红重跑绿两次（环境/顺序类） | 第三次出现时打印失败断言 + 设备 `minUniformBufferAlignment`（**2026-09-25 证据已预置**：用例始终打印并经 `SCOPED_TRACE` 携带 `minUniformBufferOffsetAlignment`+regions+strides+capacity；复现尝试 40× 冷进程未红，见 §11.16da） |

## 7. 术语（只留还在用的）

* **scope / pass**：一次 `beginPass`…`endPass`；属性属于它，`endPass` 丢弃。
* **半片（half）**：(program, revision, layout, variant) 的一个编译单元；一个 pass 可以对每个半片各绑一次。
* **bootstrap**：目标首次进入必须清一次（镜像 UNDEFINED 不能 LOAD）；由目标自己声明。
* **normalize**：把"允许重复/后设"的收集侧描述变成"每 pass/draw 一个终态"的编译侧计划的那些规则（D2）。
* **键审计**：断言"允许进键的输入集合没有扩大"的行为测试（§11.16cq）。
* **episode**：一段"同一个问题还没被修好"的区间；期间只报一次（插话的句柄归调用方）。
* **park / retirement**：把被替换的对象按帧时间轴延后销毁的队列；`slots + 1` 是它的窗口。

## 8. 实施记录（§11.x：只留规则 / 判据 / 数字，逐片叙述已删）

> 每一节保留：当时的**规则表**、**判据**、**实测数字**、**变异反证**与**登记（留下的口子）**。
> 数字是**当时**的证据（当前数字看门禁）；更早的完整叙述在 git 历史 `8c223ba`。



### 11.1 M0（2026-09-21）：`core/` 的无设备骨架 + 证据夹具
* 用例：`tests/test_vsg/BackendCoreTest.cpp`（22 例）+ `tests/test_vsg/BackendEvidenceTest.cpp`（11 例）， 全部零 GPU、零 `vsg::`：协议转移表；目标/深度计划表；时间轴与退役（“完成证据未到不得释放”、 “提交 ≠ 完成”）；arena（“增长不搬块”、reset 是唯一失效点、稳态零增长）；键审计（“extent 不进身份”）； 计数交叉校验；相位表输出格式；像素探针（行主序、子矩形、钳位、“什么都没画”就叫失败）； 诊断（分类计数、无 sink 也计数、episode 只报一次）；分配门禁（**稳态窗口一次也不分配**， 且“故意分配必须被抓到”这条用例保证了门禁能失败）。
* （`Viewer::assignRecordAndSubmitTaskAndPresentation()` 里是局部硬编码），只能启动期探测/断言 （`RecordAndSubmitTask::fence(i)` 越界返回 nullptr），不得写死 4。

### 11.2 M1a（2026-09-21）：设备地板 + 探测
* 顺带记一个坑：`vn::String` 包的是 `std::u8string`（`value_type = char8_t`）——字面量必须写 `u8"…"`， 而驱动给的 `char[256]`（如 `deviceName`）与 printf/gtest 转发都要走仓库既有的 `reinterpret_cast<const char*>(s.data()), s.size()` 写法。
* ⇒ 地板（1.4）与七个必需特性在仓库门禁用的软件光栅器上全部满足；探测与策略的判决一致（用例本身就在断言 “探测的 usable == 策略的 satisfiesRequirements”，不允许第二份意见）。

### 11.3 M1b（2026-09-21）：会话（自有窗口）+ 空帧提交 + 槽数的**逐帧学习**
* 实测（lavapipe + X11，`SessionTest.EmptyFramesAreCommittedAndAParkedObjectWaitsForTheCompletionEvidence`）： 会话建立 → 学会槽数 → 空帧连续提交与呈现 → 在第 S+1 帧 park 一个对象 → 它在 `retire_at + slots` 帧 才被释放（早一帧的断言会红）→ 全程 `deviceWaits() == 0`。
* 所以拆成两个类型：`probeSlotCount()` 只答“现在能答多少”，`SlotTracker` 负责“数字不再增长才算学会” （两帧连续相等即饱和）。**学会之前不 park**：`RetirementQueue` 的 `retire()` 直接返回 false，调用方必须 退回计数过的 device wait（`parkingAvailable()` 是这条状态的判据）。学会后若与假设值（3）不一致，报一次 Info；一致则一句话也不说。

### 11.4 M1c（2026-09-21）：宿主句柄采纳 + 会话移动（不重建）
* （既有实现的 `VsgRetireRing::waitForIdle(viewer)` 用的就是它，见 §9 的引用约定）。

### 11.5 M2a（2026-09-21）：内容身份与存储策略（core）
* 一个版本），稳态帧的 `writes()` 保持不变 —— 这就是 “材质稳态零流量” 的可断言形式。

### 11.6 M2b（2026-09-21）：管线变体池 + 每 pass 状态注册表（core）
* （本片只有叙述与文件表，已压缩掉）

### 11.7 M2c-1（2026-09-21）：几何上传与共享（api）
* “两个 drawable 读同一流”有一个不需要 GPU 的判据 —— **它们拿到同一个 bind 对象**（测试断言指针相等）。 同理：切片不同 / revision 不同 / buffer 不同 ⇒ 不同 bind（那些是不同的字节）。
* 两账一致 — `agreesWithRegistry()`（`live() == objects()`）是断言，不是假设 —— 与 `Observe` 的“同一份数据两个视图必须相等”同一条纪律

### 11.8 M2c-2a（2026-09-21）：每帧块存储（api，真设备）
* M2c-2 再拆一次：**M2c-2a 块存储（本节，已完）**、M2c-2b 描述符 re-point + 绘制录制 + 相位。理由是两者的证据面不同： 存储的正确性（区域、旋转、稳态零写、拒绝不增长）可以在真设备上直接读回证明，而录制要等管线/描述符齐了才有像素。

### 11.9 M2c-2b-1（2026-09-21）：块描述符（api，真设备）
* 对齐即拒绝 — dynamic offset 必须是设备 `minUniformBufferOffsetAlignment` 的倍数（VUID-vkCmdBindDescriptorSets-pDynamicOffsets-01971）：不合规 ⇒ **不给命令** + `refusals()` 计数，而不是把验证错误交给驱动
* 一个 set 服务所有 draw — “哪一块”由 dynamic offset 决定；测试断言两次不同 offset 的 `bind()` 拿到**同一个** `descriptorSet` 对象 —— 这就是“编辑是一次写、新帧是一次偏移”的机制面

### 11.10 下一步
* （本片只有叙述与文件表，已压缩掉）

### 11.10 M2c-2b-2a（2026-09-21）：内容管线与着色器（api，无设备）
* keep-alive 不是所有者 — 池淘汰身份 ⇒ 层同步丢对象（`agreesWithPool()` 断言）；绑定它的命令持有自己的引用

### 11.11 M2c-2b-2b-1（2026-09-21）：绘制录制（api，无设备）
* 动态块的槽 — `kDynamicStateSlot == 15`：槽是状态栈的**身份**不是优先级，取默认槽 0 会顶掉管线绑定（既有实现从驱动崩溃里学到的那条，这里断言钉住）
* 两条引擎约定 — reverse-Z ⇒ `VK_COMPARE_OP_GREATER`；**front face = CLOCKWISE**（vsg 的投影翻转 Y，声明成直觉的 CCW 会让每个 cull 模式作用在错误的面——静默）；混合**恒开**，`blend.enabled` 只选因子（2026-09-25 注：该约定后来有了限定——单附件内容绘制才恒开，多附件 pass 与全屏 draw 不混合；见 §11.16bl 与 §11.16cv）
* 顺带修掉的偏差 — `ContentPipeline` 的 baked 常量原写成 `LESS_OR_EQUAL` + `COUNTER_CLOCKWISE`（照抄时的直觉值），已按引擎约定改为 `GREATER` + `CLOCKWISE`

### 11.12 M2c-2b-2b-2a（2026-09-21）：离屏目标与读回（api，真设备、无窗口）
* 落地抓到的坑 — vsg 的 `RenderGraph` 默认 `renderArea` 是**零面积**：pass 记录成功却一个像素都没清，读回全 0，看起来像"拷贝坏了"。设上 `renderArea` 后立刻通过——这类"沉默的空 pass"正是像素相位存在的理由

### 11.13 M2c-2b-2b-2b（2026-09-21）：端到端像素证据（整条栈画一个三角形）
* 落地抓到的坑 — 断言助手把 clear 色写成"深蓝 `b > 150`"，而 0.25 蓝量化后是 **64**：像素相位的第一原则是**读回的值说了算**——期望值必须来自同一量化，这次是把 `(0, 0, 64, 255)` 打出来才看清的

### 11.14 M2c-3（2026-09-21）：会话侧内容挂载（内容经 `Session` 进帧）
* 内容必须在会话的设备上 — accessor 给出会话自己的设备（内容建在别的设备上，会话的命令缓冲引用不了）
* **落地抓到的坑（只有像素能报的）** — 同一份内容在离屏目标（**无深度附件**）里画得出，在窗口里**画不出**：窗口的 pass 有深度附件、按 reverse-Z 清到 **0.0**，而深度比较是 `GREATER` ⇒ 三角形若躺在 z = 0 就被深度测试丢弃。离屏相位永远发现不了这个差异——这正是"两个入口都要像素证据"的理由

### 11.15 M3a（2026-09-21）：清屏/加载策略（无设备可判）
* 与 `planTarget` 的分工 — `planTarget` 回答"这个目标要不要重建/换镜像"（`RepairReason::Bootstrap` 就是规则 1 的触发点），`planClearValues` 回答"这一趟 pass 的附件各自怎么开始、怎么结束"
* 反转 Z 的远平面是 `0.0` — `kReverseZFarDepth = 0.0F`。深度清到 `1.0` 配 `GREATER` 比较 ⇒ 每个片元都被丢弃（空画面），而这个常量是唯一能把这个约定写成可断言事实的地方

### 11.16 M3b（2026-09-21）：多附件目标（MRT + 深度）与逐附件读回
* **落地抓到的坑** — MRT 的"第二个附件等于第一个"很难被察觉：只要 `capture(i)` 落到同一块缓冲，`probe(1)` 就会返回附件 0 的像素，画面看起来"MRT 成功了"。所以证据必须是**两个不同的片元输出**（绿 / 红），而不是同一个颜色的两次复制。另外：片元阶段**没写**的附件在 MRT 下内容是 `UNDEFINED`（不是清屏值），所以"不清也能看到清屏色"这件事本身不是证据
* bootstrap 恒开 — 目标的镜像每帧以 `UNDEFINED` 起始、pass 每帧清屏，所以"这趟是 bootstrap"在这个目标上是永远为真的事实；与其让调用方猜，不如由目标自己声明，并在计划里出现 `Load` 时**拒绝创建**（LOAD 一张 `UNDEFINED` 镜像不是策略选择，是 bug）

### 11.16b M3c（2026-09-21）：借用的深度，以及装上验证层后当场抓到的两个真 bug
* （本片只有叙述与文件表，已压缩掉）

### 11.16c M3d-1（2026-09-21）：帧的收集段（`core/FrameRecorder`，无设备）
* **arena 独占（P0-1）** — 计划里没有一个 span 指向宿主容器：`CollectedCommand` 在 `render()` 里复制，`CollectedDraw`/`CollectedPass` 在 `endPass()`/`endFrame()` 复制。用例按引擎自己的模式驱动（一个 vector 清空重填）后断言**第一趟 pass 的记录仍按字节等于它当时拿到的值**
* **借用的东西只抄不留** — 灯→`LightRef` 数字（用例在 endFrame 之后 `reset()` 掉 `Light` 再断言快照）；相机→`CameraSnapshot`（VIEW/PROJ + eye/target/up）；程序→identity+revision；几何→identity+revision。材质只记身份：SDK 的 `Material` **没有** revision 访问器，材质 revision 的来源在 api 层材质管理器（那是它的账）
* 与计数聚合一致 — 被拒的绘制**不**计入 `draws`：用例断言“两次无 scope 绘制 ⇒ `refusalCount()==2` 而 `draws==0`”
* 边界没被偷偷扩大 — 录制器只答“这次调用合法吗、它记录成什么数据”：不判 rebuild / resize / borrow / 顺序 / 缓存。编译器需要的一切都是描述里的**事实**（这条是 M3d-2 的入口条件）

### 11.16d M3d-2（2026-09-21）：帧的编译段（`core/FrameGraph` + `core/FrameCompiler`，无设备）
* 逐 draw 的 compare op **已进动态层（2026-09-25 落地，见 `.ai/memory/graphics.md` M11ag）** — `core::DynamicState` 增加 `compare`（默认 `Less`，距离语义），`resolveDynamicState` 从 `state.depth.compare` 拷贝（pass 早退分支也拷），`makeDynamicStateCommand` 用 `RenderStateMapper::mapCompareOp` 在 Vulkan 边界反转（默认 `Less→GREATER` = 烘死时的逐字同值 ⇒ 无 StateNode 的场景管线与画面不变）；`Keys.hpp` 审计表 `DynamicState` 行补 `+ compare`。原登记的分歧（内容自定比较算子的画面新旧不同）随之关闭。
* 变异反证 ×2（都实测） — ① 不认环（`cyclic = false`）⇒ **5 个用例红**（图 3 + 编译 2），其余 17 绿；② `freshAttachments` 恒 false ⇒ bootstrap 那条红
* **环 = 跳过整个强连通分量** — 三条失败语义各自落地：① 上报 Error（列出分量里的 pass，**每分量一次/每帧**）；② 分量成员不进计划（不是"按当前顺序偷偷画"，是不画）；③ 分量外的 pass 照旧，帧照旧提交呈现；计数器 `invalid_schedules` **按分量 +1**（两个独立环是两个问题），自检断言恒 0
* **0×0 目标不是错误** — 没有任何东西能画进去 ⇒ 它的 pass 不进计划，**也不上报**（这是状态，不是错；目标层的构建失败另有报告）。用例断言 `diagnostics.clean()`
* **缺省全部解析** — 视口（没公告 = 整目标，逐 draw 落定）、程序（命令没有自己的 = 帧的 default，**在编译期替换**）、动态层（`resolveDynamicState`）、清屏（有效 policy + `bootstrap` / `depth_preserved` 两个**执行器推不出来**的事实）
* 用例当场抓到真 bug — 不可服务的 pass（未知目标 / 0×0）只标了 `servable_=0`，**没有** `graph_.exclude()` ⇒ 它仍被排进 schedule，解析阶段用 `kNoTarget` 去索引 target 表（`stl_vector.h` 的越界断言当场炸）。修法：两处都 exclude，并在解析循环里留一条"不可达但把前置条件放在本地"的守卫
* 与计数聚合一致 — `invalid_schedules` 是被跳过**分量**的数；被跳过的 pass 不进 `CompiledFrame::passes`，而 `counters().passes` 记的是收集到的 scope——两个数的差就是本帧丢掉的 pass，相位可断言
* 边界没被扩大 — 编译器不碰任何 API 类型（事实是纯值、计划是纯值），不建资源、不选 GPU 对象；`planTarget` / `depthPlan` 的答案照读，不自造策略

### 11.16e M3d-3a（2026-09-21）：执行段的第一片（`api/VsgExecutor`，真设备、无窗口）
* **不服务的 pass 必须响** — 默认 framebuffer（窗口 pass 未接）、未注册的 target、无附件的 target ⇒ `Warning`/`ContentSkipped` + `skipped()` 计数 + `record()` 返回 false（这是部分帧，调用方要知道）。默认 framebuffer 那句还会点名“这一层还没接”，而不是把内容画到别的 target 上
* **变异反证（端到端）** — 让调度器忽略公告 order（`ready.emplace(0, node)`）⇒ `FrameGraphTest` 的排序用例与 **`ExecutorTest` 的像素用例**同时变红，其余 22 绿。⇒ 那条像素断言测的是“**画面跟着 schedule 走**”，不是“图能提交”
* 清屏值这一半已到位 — M3a 的 load-op 变体有两半：**清屏值**（本节已能逐 pass 变化）与 **LOAD/CLEAR 操作**（仍归 target：镜像每帧 UNDEFINED ⇒ 必须清）。后者要等“带历史的 target”（跨帧保留 / 深度保留的 LOAD 路径）
* M3d-3 按“**能证明什么**”再拆：**M3d-3a 记录顺序 + 一个 pass scope = 一个 render pass**（本节，已完）、 M3d-3b 内容绘制（几何/块/描述符经计划进 render graph，`ContentDraw` 接进来）、M3d-3c 会话侧（窗口 pass、 `present`、帧计数与退役推进）。理由：**记录顺序是整个三段式拆分存在的理由**，而它恰好可以用清屏色证明—— 不需要先有内容路径。
* **记录序列也是证据** — `recorded()` 断言 {2,4,1,3}（调度序），而不只是“四个都录了”：失败时能看出是哪一级动了

### 11.16f M3d-3b-0（2026-09-21）：计划带上“管线身份要的 pass 侧事实”，执行器核对计划与资源世界
* 2. M3d-2 记下的偏差（“不 materialize `RenderPassCompatibility`，因为带 vector”）需要一个**按身份取兼容性的口子**：
* **计划与资源世界必须对得上** — 计划描述的形状与它解析到的 target 不一致 = 上游事实漂了，而**按错形状建的管线与宿主要的画面没有任何关系**。这个不一致只在执行器这一处能被发现，所以在那里报，而不是交给驱动
* （`depth_sampleable`）；它们现在由编译器从**已经查到的同一份 target 事实**解析进 `CompiledPass`，调用方不必再问一次。
* **同一个事实只答一次** — 两项都从编译器已经拿来建 target 表的事实里解析，所以“计划说的”与“执行器要的”不可能给出两个答案

### 11.16g M3d-3b-1（2026-09-21）：内容“按 pass 放置”，状态全部来自计划
* **变异反证** — 把 `graph->addChild(packet.content)` 拿掉 ⇒ 像素用例红，而且失败消息直接给出真相：中心读到 `(64, 128, 191)` = **计划那个 clear 色**——即“背景是计划的清屏，三角形没进去”

### 11.16h M3d-3b-2（2026-09-21）：内容层的三张事实表（`api/ContentFacts`，无设备）
* **几何的 Malformed 是“通道与布局对不上”** — `popcount(canonical_mask) + custom_locations.size() == channels.size()`，且索引流必须是 `Index`。规则放在**查找里**，内容层就绕不过去：声明了没人喂的属性是驱动拒编或未绑定内存读，多出来的流是数据没人消费

### 11.16i M3d-3b-3（2026-09-21）：每绘制 ABI 块的打包（`api/DrawBlock`，无设备）
* **列主序写成 `column * 4 + row`** — 元素 (row, column) 落到平铺数组的哪个位置，是这份 ABI 与着色器之间的契约；从访问器逐元素写而不是复制存储，让“用的是哪种约定”在读过的地方直接可见
* **平移在 12..14** — 同一个事实从字节侧再看一遍（第四列），也是“没有转置”的第二个证据
* **视图块暂时不做** — `VineViewBlock` 的 ABI 已钉（`view` / `inv_view` / `proj` / `view_proj` / `cam_pos` / `frame`），但 `frame`（时间 / 视口尺寸）与 `cam_pos.w` 需要**会话侧**的约定，而那里才是这两个值得出处；在此发明就是给同一个问题第二个答案

### 11.16j M3d-3b-4（2026-09-21）：几何的通道走查（`api/GeometryFacts`，无设备）
* **同一起点的规则** — 一个几何的所有通道必须从**同一个顶点**起算（`offset / components` 一致），否则“索引 0”对每个通道指的是不同顶点，索引就没有单一含义 ⇒ Malformed
* 变异反证 — ① 通道顺序改回“几何自己报的顺序” ⇒ 顺序用例红；② 索引键改成按切片（`count = indexCount()`）⇒ 两条分享/索引用例红。其余用例保持绿

### 11.16k M3d-3b-5（2026-09-21）：程序与材质两张数据源（`api/ContentSources`，无设备）
* 变异反证 — ① 接受任意阶段组合 + 入口点不一致 ⇒ 程序规则用例红；② `findMaterial` 恢复 nullptr 特判 ⇒ 默认材质用例红；其余 7 绿
* **材质的修订由调用方给** — `Material` 没有 revision 访问器（M3d-1 已记）；知道材质何时被改的是材质管理器，所以修订作为参数传入（与几何同一条规则：修订是计划要报的事实，只有它的拥有者能报）
* 一个值得记住的字节级事实 — 先写了“`{}` 会把对象（含填充）清零”的注释，又被诊断打脸：诊断打印出**差异字节正好是 52..63**（= `shininess` 之后的尾部填充），而逐成员 `operator==` 为真。注释与用例都已改成陈述真正的事实（这一条也已记进仓库记忆）

### 11.16l M3d-3b-6（2026-09-21）：把三张表接进内容层（`api/ContentPass`，真设备）
* 测试自己踩的坑（已修） — 用栈对象构造 `intrusive_ptr<Geometry>(&geometry)` ⇒ 析构时 `free(): invalid pointer`。SDK 对象必须堆上由 `intrusive_ptr` 拥有（与计划里的身份一致）
* 变异反证 — ① 每次 acquire 用**移动的身份**（revision + 计数器）⇒ 缓存断言红（`uploads` 3≠2、`aliases` 1≠2）；② 忽略几何 miss ⇒ 进程崩在空指针（那道检查正是它在防的事）。头两次尝试的变异都是无效变异（`+bound` 恒为 0；`+1` 两帧一致），**变异本身也要验证**——这一条已记进仓库记忆
* **逐命令拒绝，不是逐 pass** — 程序/几何/材质任一查不到 ⇒ 该命令不画、按查找到的理由上报（未知 / 修订不同 / 畸形），**其余照画**。一个没法着色的 drawable 是画面上的一个洞，不是丢掉整帧的理由

### 11.16m M3d-3b-7（2026-09-21）：多布局 scope（`api/ContentPass`，真设备）
* 变异反证 — ① 选半片忽略布局 ⇒ 未服务的布局被画（`record` 真、零消息）红；② 忽略程序身份 ⇒ 未编译的程序被画，红；③ 把注册表改成“每个录制器一份”（临时给 `ContentDraw` 加成员）⇒ 第三次绘制跳过绑定（1≠2）**且左网格被蓝管线圈画成 (0,0,255)** —— 计数与像素同时抓到
* 测试自己踩的坑 — 第二帧的 `beginFrame` 被协议拒绝：**关帧的是 `swapBuffers()`**（`endFrame()` 只离开 pass，回 `Idle` 靠 swap）⇒ 症状是“第二帧的计划还是第一帧的三条命令”，靠打印 token 抓到

### 11.16n M3d-3b-8（2026-09-21）：pass 输入的采样绑定（计划携带输入表 + 内容层绑定，真设备）
* **计数是身份不是状态** — 管线编译绑定在一份描述符集布局上，所以"这趟 pass 绑几个采样纹理"必须进键；两个计数 ⇒ 两个布局 ⇒ 两个管线对象（同键再来仍是 `Reused`）。这也是键审计表里"采样附件数"那一项真正落地的位置
* **计划与资源世界必须对得上** — 计划说几项、每项几张，调用方给的图必须逐项相等；不等就**整趟拒绝**（与 M3d-3b-0 执行器核对 target 形状同一条纪律）。拒绝消息分两条：项数不对 / 第 i 项彩色数不对
* **彩色附件按可采样收尾** — 输入的图像必须真的处在 `SHADER_READ_ONLY` 且建了 `SAMPLED` usage，否则描述符在撒谎。旧实现是"彩色附件永远按 SHADER_READ_ONLY 收尾"；重写版原先为了免 barrier 读回改成 TRANSFER_SRC，这一片把彩色改回**采样收尾**（深度仍留在附件布局：它是下一趟 pass 的附件、也是阴影的纹理，另有机制），**代价是读回的拷贝自带一对转移**（拷贝仍是一条命令，只是前后各一个 image barrier）
* 的有序表，**从与 pass 目标同一份 target 事实**里解析（同一个事实只答一次：调用方重新从 target 对象推一遍 就可能与计划给出两个答案）；空输入（本帧无人产出）留在表里、报 0；**非空但事实答不上来的输入上报一次** （此时什么也绑不了，静默按"没有这个输入"着色更糟）——注意 pass 本身照跑：目标查不到是"没地方画"， 输入查不到只是"这一项绑不上"。
* `OffscreenTarget::colorView(i)` 就是答案），内容层核对**每一项的彩色数**与计划一致后才建 set； 不一致 ⇒ **整趟 pass 拒绝**并说出是哪一项动了（输入是 pass 的**一个**事实，不存在"逐命令近似"）； 计数进键（`sampled_color_count`），set 进每一次绘制的状态组。
* **"没人产出"与"后端不认识"是两件事** — 空白输入（引擎解析成 null）留在表里、报 0 张、**不上报**——那一层的问题归引擎（它自己会报）；非空但 target 事实答不上来才由编译器报一次（后端自己的账缺了一页）。两种情形的共同答案都是"这一项绑不了东西"，所以内容层按 0 张核对，照画其余

### 11.16o M3d-3c（2026-09-21）：会话侧（窗口目标、一次 present、视图块，真设备）
* **不折的失败形态（实测）** — 没折的矩阵把测试三角形放在裁剪 z = **-0.714**；窗口深度清 0.0、比较是 `GREATER` ⇒ 每个片元都被拒绝。画面只剩清屏色，而**绘制仍被记录、计划仍正确、0 VUID、0 诊断**——这正是 `SceneBridgePipeline` 文件头为手写 `gl_Position` 记下的陷阱，只是上了一层（矩阵而不是程序）
* **镜像的深度映射画面看不出来** — 变异 M2（z 折反：近→0、远→1）让三角形落在深度 0.143，仍 `GREATER` 过测 ⇒ **像素断言全绿**，只有算术用例红。这就是"算术用例与像素用例各管一段"的实证：画面负责"这条约定能用"，算术负责"这条约定是对的"
* **变异反证（五条已验，一条登记）** — M1 折掉 ⇒ 3 用例红（2 算术 + 像素的 4 条断言）；M2 z 折反 ⇒ 2 算术红、像素**绿**（见上）；M3 y 不折 ⇒ 2 算术 + 朝向像素红；M4 `view` 也折 ⇒ 2 算术红（像素读不到 `view`）；M6 不调 `prepare` ⇒ 清屏像素红且实测露出 vsg 的默认清屏色 **(102,51,51)**（"画面里的清屏色确实是计划的"由此有了反证）。M1–M4 的容差特意收紧到 ±6：0.25 的 sRGB 像（137）与 0.5 的线性像（128）只差 9，容差一松，**没画出来的像素**就能替着色答"cam_pos 到了"。M5（`prepare` 不刷 renderArea）**在本片的夹具里不可观测**：窗口尺寸没变过，创建时的矩形与活的尺寸相同——它的可观测形态要一个**活改尺寸**的窗口用例（登记到下面的口子）
* 后果，得让同一帧里有一趟离屏 pass 与窗口 pass **同键**（同程序/几何/兼容性）；这是 M4 的用例形态 （共享键 + 采样输入正是全屏 pass 的日常）。**已关，且结论是修正**（§11.16r）：同键的后果实测到了 （`VUID-vkCmdDrawIndexed-renderPass-02684`），但稳定视图**不是**解法——vsg 的复用循环只比 pipeline states， 两个视图的状态集合相同时照样共用一份实现；解法是键带上**设备格式**（两个变体）；
* * **第二个渲染通道家族出现时可能需要逐 pass 视图**（现在是"一个视图管整个窗口"）——**已实测给出答案**
* **视图块的裁剪矩阵是设备的，不是 SDK 的** — SDK 的矩阵是 x 右、y 上、z∈[-1,1] 近端 -1；本后端的设备是 reverse-Z + y 向下 NDC（近→1、远→0、世界上=屏幕上）。**宿主程序写 `gl_Position = view_proj * model * pos` 时不该知道任何一条**，所以折叠（x/w 不变、y 取反、z 记作 `0.5 - 0.5*z`）发生在块装配处。`view`/`inv_view` 是视图空间矩阵，**不折**（视图空间的关照计算与裁剪约定无关）
* **一帧一次 present** — `commitFrame()` 里 `recordAndSubmit()` → `present()` → 帧计数 +1、`timeline.submitted(token)`、`retirement.advance(timeline)` 的顺序就是"帧的账"。像素用例在**第一帧**之后立刻断言 `framesPresented()==1`、`submittedFrame()==1`、`deviceWaits()==0`，再空转两帧只为**让呈现落地**（显示服务器的拷贝是异步的，第一帧刚 present 就读到的可能还是窗口的旧后备存储——另一个用例一直呈现三帧就是这个原因）
* * **活改尺寸的窗口用例**：`prepare` 里"renderArea 跟随活的尺寸"那行现在只有代码与理由，没有可观测证据（M5 不可观测的原因）；它需要把宿主窗口在帧间改尺寸、再读新区域的像素。

### 11.16p M4a（2026-09-21）：全屏绘制调用（身份 + 层 + 事实 + 录制，真设备像素）
* **push 块的**内容**还没落地** — 布局声明了 128B（SDK 的 `deferredLightProgram` 就是这么声明的），但光照半边（world→view 的三盏方向光 + 环境光）与深度重建半边（near/far）分别是光照相位与深度采样相位的活。**这一片的程序（拷贝）不读它**；读它的内置程序在这之前看到零值——登记，不假装
* **变异反证（四条已验）** — N1 集合索引不再跟 kind ⇒ 算术用例红 + 设备侧**段错误**（布局里带着空块集）；N3 三个顶点改成六个 ⇒ 算术红、**像素绿**（PiP 的 viewport 把多出来的三角形裁掉，而全屏拷贝的第二枚三角形画的又是同一张画）；N5 宿主顶点阶段获胜 ⇒ 事实用例红；N6 去掉 kind 检查 ⇒ 层与录制两处一起红。N3 的"像素看不见"是这一片的正当结论：**顶点数是 ABI 的声明，算术用例是它的证据**
* **本片留下的口子（登记）**：**计划侧**（`api/ContentPass` 仍在拒绝 `DrawKind::Screen`）——源 = pass 声明的
* 第一个输入（采样集已就位）、push 块的内容、PiP 子矩形经计划上屏、以及窗口合成（M4b）；顺带把 §11.16o 登记的 两条口子（多趟窗口 pass 的"一次清"、窗口/离屏同键变体）一起变成可观测用例。
* 3. **事实**（`api/ContentSources`）：`buildScreenProgramFacts` 把**引擎的正典全屏顶点阶段**与宿主的片元

### 11.16q M4b（2026-09-22）：全屏绘制的计划侧（计划解析它的状态，内容层按 kind 录它，真设备像素）
* （`deferredLightProgram` 就这么声明），填它的两半（world→view 的光照、near/far）分属光照相位与深度采样 相位。**必须真的推**：声明了 push 范围却从不推，着色读到的是**未定义**字节，"那一相位还没落地"得是确定的 零而不是驱动恰好留下的东西。本片证据用的程序（引擎的 screen copy）一个字节都不读它。
* **全屏调用不测深度是它的定义，不是优化** — 正典三角形在 z = 0.0；窗口的深度清 0.0 且比较是 `GREATER` ⇒ 测了就整块消失。变异 P2（改成继承 pass 的策略）在**有深度附件的目标上**让"矩形内是源的颜色"直接变红（实测得到目标清屏色 (0,0,191)）
* **集合的索引是 ABI，绑错一层就崩** — 变异 P1（全屏采样集绑到 set 1）：4 条 VUID（`vkCmdBindDescriptorSets-firstSet-00360`、`vkCmdDraw-None-08600`）+ **段错误**——全屏管线布局只有 set 0，绑到 1 的集合既越界又不被绘制看见
* **push 的"形"有证据、"内容"没有** — 形：设备无关用例钉住节点（片元阶段 + 128 字节）与它排在管线绑定之后；内容：**登记**为光照相位的活（本片的程序不读它）。想用"读到零值"做反证不可靠（未定义的 push 内存常常也读成零），所以不假装有反证。**已关，§11.16w**（M5d）：内容落地（`LightPushBlock` + 每次调用推），并且**正向**断言了"三带各自的颜色 + 全零变异三条带全黑"
* **本片留下的口子（登记）**：**窗口合成**（一趟窗口 pass 里叠一块全屏覆盖层）与 §11.16o 登记的两条口子
* 修订**找（没有几何那一半：这次调用不画几何，所以不需要事实表），miss 的消息区分"没编过这个程序"与"修订 对不上"。采样集**每 kind 一份**：内容在 set 1（块集之后），全屏在 **set 0**（它自己的 ABI），两个 kind 各 自用自己那一层的布局对象去建 set，而"一趟 pass 一次绑定"的"仅当"逻辑是**同一个注册表**说了算。

### 11.16r M4c（2026-09-22）：窗口合成（一趟窗口 pass 里叠全屏覆盖层），顺带把两条登记的口子做成用例
* M4a/M4b 的全屏绘制只在离屏目标上验过；这一片把它放进**窗口自己的 render pass**（vsg 造的，表面格式 + 它自己 的依赖），与一趟离屏 pass 同帧。夹具 = 四趟 pass 一帧：①「picture」离屏（只清屏，红）②「shared」离屏（清屏 + 同一段场景三角形，深度格式与窗口 traits 相同 ⇒ 引擎看来与窗口同形）③窗口场景（**第一趟窗口 pass，拥有那一次 清**）④窗口覆盖层（全屏绘制：采 picture，画在 PiP 子矩形里，自己公告**绿色**清屏——必须不生效）。计数器与像素 各管一段：像素说"叠对了"，计数器说"录对了"（黑窗也能让"没画"看起来像"画对了"）。
* 2. **"窗口/离屏同键变体"这个口子是错的**：真正的答案不是"共享一个变体、让 vsg 按 viewID 分开编译"，而是
* （`vsg 1.1.16 — src/vsg/state/GraphicsPipeline.cpp — GraphicsPipeline::compile()`）：本后端的管线把 viewport/scissor 声明成动态状态、状态集合逐位相同 ⇒ "两个视图"照样复用同一份实现。 实测形态：`VUID-vkCmdDrawIndexed-renderPass-02684`（`B8G8R8A8_SRGB` vs `R8G8B8A8_UNORM`、 `dependencyCount 2 != 1`）——**画面竟然是对的**（未定义行为，不是"能跑就行"的证据）。 修法（本片落地）：**把设备格式放进键的兼容性半边**——`core::TargetShape` 多两个字段 （`device_color_formats` / `device_depth_format`，装载设备自己的格式码，0 = 无该附件/未知）， `core::RenderPassCompatibility` 同样带上，`TargetShape::compatibility()` 一处装配（调用方不再手搓）， 两个目标各自把真实格式报出来（窗口从 surface，离屏从自己建镜像用的那个映射）。这是**引擎词汇表表达不了 的事实由知道它的那一层交出来**，不是把 extent 之类的运行时量塞进身份：格式是 render pass 身份的一部分， Vulkan 的兼容性规则里写着。
* **"两个视图"不等于"两份编译"** — vsg 的复用循环看的是状态集合；两个视图的状态集合相同时它**故意**复用（对同一 render pass 的 load-op 变体来说这正是想要的：`LoadOpVariantKey` 不进键就是为了让它们共享一份编译）。所以"哪些东西算兼容"必须由**键**回答完整，视图救不了键
* **引擎的格式枚举是投影，不是身份** — `RGBA8` 不能作为 render pass 身份的替身：sRGB/线性、BGR/RGB 都是兼容性的一部分。设备格式因此**必须**从设备层交上来（`core/` 不认识这些码，只比较它们）；"没报"与"报了别的"是两个不同的兼容性，未知**不会**静默并族
* **未定义行为可能"看着对"** — 变异 Q1（键丢掉设备格式）复现 VUID 的同时**像素照过**：管线当初是按另一个 render pass 编译的，驱动只是恰好把这次画对了。所以这一条的判据是验证层，不是画面——"画面对了"证明不了兼容性
* **`Session` 中途换帧图不安全** — 试过"settle 帧只挂窗口图"（避开离屏 capture 的重复录制）：`assignFrameGraphs` 会释放**在飞**的命令缓冲 ⇒ `VUID-vkFreeCommandBuffers-pCommandBuffers-00047`。在飞的帧没有公开的等待点（`shutdown()` 才有），所以这条只能整片地做（登记到下面的口子）
* Q3：不跑 settle 帧 — 窗口读到 **(0,0,0)**：呈现是异步的，`commitFrame()` 之后立刻读会拿到旧后备存储（登记在 §11.16o 的既有事实）
* * **`prepare` 的 renderArea 仍无活改尺寸证据**（§11.16o 登记，M5 之前不动）；
* **变异反证 ×4（全部实测）** — A：计划丢掉输入表（`pass.inputs = {}`）⇒ `FrameCompilerTest` 与真设备用例同时红；B：键里计数写回 0 ⇒ 着色器读一个布局里不存在的 set 1，**段错误**（139）；C：注册表不回答"要发采样绑定"（`inputs_issued` 恒 false）⇒ 注册表 6 条断言红 + 真设备用例在"绑一次"那句红、随后段错误；D：彩色收尾改回 `TRANSFER_SRC` ⇒ **像素仍然通过**，但验证层报 6 条（`vkCmdDrawIndexed-imageLayout-00344` + `vkBarrier-oldLayout-01197`）——布局这类主张只有仪器看得见，这条已记进记忆
* （`isColourByte(background[1], 0.25)` 为假），三角形仍是场景色 ⇒ `window_recorded_` 那条规则 （一帧里第一趟窗口 pass 拥有那次清）从"只有理由"变成"有反证"。执行器还断言了它是**按计划顺序**放置四趟 （`recorded() == {1,2,3,4}`，不是公告顺序——本夹具里两者恰好相同，所以这条是"接缝存在"的证据而不是"顺序会 被改"的证据）。
* 3. **顺带纠正 §11.16o 的一条理由**：那里说"窗口内容挂在自己的稳定 `vsg::View` 下 ⇒ vsg 就按窗口的 render pass
* Q1：键里丢掉设备格式（`TargetShape::compatibility()` 不搬那两个字段） — `pipelines()` 从 2 变 1（计数器断言红）+ `VUID-vkCmdDrawIndexed-renderPass-02684` 复现（验证层红，4 条）+ **像素全绿**（未定义行为，见上）
* 都这么做）。多帧在飞时，"读第 N 帧的图"与"第 N+2 帧正在写同一块缓冲"之间的宿主侧顺序没有机制，只有约定；

### 11.16s M5a（2026-09-22）：离屏目标的多写者（LOAD 变体，真设备像素）
* M4c 登记的口子：`OffscreenTarget` 只建**一张**渲染通道、load op 恒 CLEAR，所以同一目标一帧两趟时第二趟会擦 掉第一趟。计划侧早就能表达（`CompiledPass::bootstrap` = "该目标的第一个写者"、`planClearValues` 的第 1/2/3 条规则），这一片把执行侧接上。三件事：
* `pass.depth_preserved` 交给 `passGraph`，目标由此解析出 `PassClearPlan` 并选中要用的渲染通道变体。执行器 不再自己判断"要不要清"——它只搬事实。
* 3. **"没人写过"是计划的事实，必须由目标报出来**（`api/OffscreenTarget::written()`）：投影出来的新问题——
* **load op 是"交换半边"，不是身份** — Vulkan 的兼容性规则里没有 load/store 与布局，所以一个变体对象的管线在另一个变体里**合法**——这正是"两趟 pass 共用一个变体"能成立的原因；反过来说，**依赖掩码绝不能随变体变**（变异 Q3 实测：4 条验证层报错，含 `VkRenderPassBeginInfo-renderPass-00904`——变体连自己的 framebuffer 都不兼容了）
* **"没人写过"不能 LOAD** — 新目标的第一趟必须清（颜色与深度都是）；报这条事实的只能是目标自己（`written()`），硬写 `built = true` 的症状是 `VUID-vkCmdDraw-None-09600`（"期望 DEPTH_STENCIL_ATTACHMENT_OPTIMAL，当前 UNDEFINED"）——只有同步验证看得见
* **bootstrap 是"第一个写者"的** — 同一目标一帧两趟：第一趟 bootstrap（清），第二趟不是（LOAD）；变异 Q1（执行器写死 bootstrap）⇒ 第二趟清屏 ⇒ 背景与左网格一起消失（像素读回 (0,0,0)、(0,0,0)）
* * 前一版（§11.16r）留下的 `Session` 等待点、capture 宿主读序两条口子不变。
* 一个刚建好但还没画过的目标，其镜像处于 UNDEFINED；`planTarget` 的 `RepairReason::Bootstrap`（"nothing usable yet - the first pass in has to clear"）本来就有这条规则，但事实 得有人报。`written()` 在**建 pass 图时**置位（"有人往里录过东西"），夹具与将来的 target 账用 `TargetFacts::current.built = target->written()`。
* * **`written()` 在"录了但没提交"时也为真**：事实是"有人往里录过东西"，不是"GPU 写过"；对 bootstrap 判断够用

### 11.16t M5b（2026-09-22）：输入表带上了深度半边（"阴影脊柱"，真设备像素）
* M5a 登记的口子：变体键的**深度半边**还只是"附件布局"一种取值，"提升成纹理"没接 上；而 SDK 的输入契约原文 就是"该来源的**每一张颜色附件**（外加**那张深度**，只要它可被采样）"——上一片只实 现了前半句。这一片补后半 句，并且用"一趟 pass 采样另一趟写进**纯深度目标**的值"来证明它：
* ``` DepthAttachment → TRANSFER_SRC → DepthAttachment ``` 的一对屏障，在"深度收尾在 `ShaderReadOnly`"的形状上直接是**谎报** （实测 `VUID-VkImageMemoryBarrier-oldLayout-01197`：镜像实际在 `SHADER_READ_O NLY`，屏障说它是 附件布局），并且把镜像**放回错的布局**，于是消费者采样到全 0（整幅画面黑，而 深度探针却正常——像素与探针 说的是两件事，这条差异就是线索）。改成"从 `depthSteadyLayout()` 出去、回 `depthSteadyLayout()`"，前向 屏障的源作用域补上 `FRAGMENT_SHADER`（采样者已经读过它），后向补上 `EARLY|LATE_FRAGMENT_TESTS | FRAGMENT_SHADER`（下一趟既当附件又当纹理）。
* 格式 = 合法的阴影贴图形状（原来"空形状"一律拒绝； 注意 `Layout` 那个老重载会默默塞一张 RGBA8——夹具里必须显式 `color_formats.c lear()`，这一条实测踩过）。
* 影贴图形状 ⇒ `ShaderReadOnly`，其余 ⇒ `DepthAttachment`）+ `loadOpVariantOf(pla n, color_final, **depth_steady**, depth_final)`：LOAD 必须命名**它保留的像素真正 在的**布局——借用者读的是出借者的布局，而不是自己写完之后的布局 |
* fscreenRenderPass` 里的布局转换提到文件作用域的 `toVkLayout()`（捕获屏障与渲染通道 描述必须用同一个转换）；深度捕获屏障按 `depthSteadyLayout()` 进/出 |
* 采样目标（`D32`、清 0.0）+ ②消费者颜色目标（清蓝）；生产者三角形的片段着色器为 空（`settings.color_attachments = 0`），消费者采样深度并写出灰值。判据：计划的两 条输入事实、深度探针（三角内 0.5 / 外 0.0）、消费者像素（左半 0.5 灰 / 右半黑 / 三角形外保留清屏色） |
* | **"可采样"决定收尾布局，而不是决定"要不要转"** | 纯深度可采样（阴影贴图）⇒
* 1. **计划为输入解析出深度事实**（`core/FrameCompiler::resolveInputs`）：`Compiled
* 条 VUID（`vkCmdDrawIndexed-imageLayout-00344`：着色器访问时镜像布局与描述符不符） + 6 条断言（4 条无设备 + 2 条像素） |
* 例 2 条（计划的输入事实 + 内容层**拒绝整趟**——报价与计划不一致）且进程不再崩溃 （拒绝发生在录制前） |

### 11.16u M5c-1（2026-09-22）：前向光照块（灯到了着色器里，真设备像素）
* M5b 登记的"灯光与 `VineShadowBlock` 还没接"拆成两片，这是**灯光**那半。要解决的问题只有一句：引擎 把灯光宣布给**一次绘制调用**，而这份数据此前没有任何一条路到达着色器。前向路径**不能走 push**—— 128B 的保证预算被视图矩阵占满（引擎的前向顶点阶段就在那里读），所以灯光必须走**UBO**；全屏路径的 那半（128B push 里装灯与深度重建）留给它自己的相位。四件事：
* **方向必须换成视图空间** — 而**轴对齐的相机会让缺失的旋转变不可见**：变异 M1（方向原样留在世界空间）只有算术用例红，真设备像素用例仍然全绿（那台的相机在 +Z 且目不斜视）。这正是"判据要选对"的又一例：约定用算术用例钉，像素管"能用"
* **补光不算灯** — 变异 M2（把补光算进返回值）⇒ 5 条红：4 条算术 + 设备用例的报告断言（"全装得下"⇒ 不报告 ⇒ 计数 1 变 0）
* **块是每次调用的** — 变异 M3（整趟 pass 只打包第一调用的灯）⇒ 设备用例三条像素红：带 1..3 全变成带 0 的颜色 `(128,26,26)`
* **布局与声明必须一致** — 变异 M4（布局里去掉 lights 绑定，着色器仍声明 binding 3）⇒ 无设备用例红（绑定数 3≠4），设备用例**在驱动里段错误**（未定义行为，崩溃前 0 VUID）——"没绑"不是"少画一点"
* **清屏仍是宿主的** — 夹具不把 `setClearPolicy` 交给第二趟以后的 pass，看到的就是黑屏（M5b 已记一次，这一片在四带图里又踩一次：带外的"背景"必须是 pass 自己的清屏色，否则"背景"断言什么都没有
* M1：世界的方向不换视图空间 — 1 条红（`TheDirectionsAreRotatedIntoViewSpace`）；设备用例**全绿**（轴对齐相机看不出）——判据选对的实测
* `shadow_bound` 键位按实测**删掉**（本后端的 ABI 里它是冗余的，见该节）。
* * **引擎自带的前向程序文本与本后端的 set 0 布局不同**（登记为场景桥的债）：SDK 的
* `builtin_forward.frag` 声明 material 0 / diffuse 1 / lights 2（外加 shadow 3/4），draw 块还在 **set 1 binding 0**；而本重写版的 set 0 是 view 0 / draw 1 / material 2 / lights 3。两者都能工作，但**同一个 管线的着色器文本与集合布局必须匹配**——场景桥把那批程序接进来时，要么按 SDK 的绑定建集合，要么 在文本层做转换，不能两套 ABI 混配。
* * 前一版（§11.16t）留下的"灯光槽索引/阴影块""采样集合深度前缀规则""借用者捕获布局"三条口子，
* （rgb + 强度，后宣布的覆盖前一个）+ 最多三盏方向光（按宣布顺序占 0..2 槽），世界坐标方向**乘相机 的三个轴**换成视图空间（视图矩阵的前三行就是 r / u / -f），方向**归一化**（着色器拿它和法线做点 积，未归一化等于把灯调亮），跳过 disabled 与块装不下的类型（Point/Spot 保留但无槽）。块是 ABI （`VineLightsBlock`，112B，`builtin_forward.frag` 声明的那份文本），静态断言钉死。
* （M8c）。理由是这句债的反面：文本是宿主的，后端选不了它的绑定号；重写版自己的 set 0 布局因此只是 “另一种声明”（它的程序照 `VineViewBlock` 这类 L1 名声明即可）。M8a 已把这批文本的声明逐条读成事实。

### 11.16v M5c-2（2026-09-22）：阴影块（一张 map 只缩放它属于的那盏灯，真设备像素）
* M5c 的后半片。M5b 把 map 当**采样输入**绑上了（深度半边），但着色器拿不到"一个片元落在 map 的哪里"—— map 是**那盏灯的相机**栅格化的，消费 pass 用的是自己的相机，所以块里必须带 `light_vp * inverse(view)` 与比较用的标量（`VineShadowBlock`：一个 mat4 + `params` = 开关 / bias / 强度 / 灯槽 号）。四件事：
* 说这张 map 属于哪盏灯、`setProducerViewProjection` 说怎么读它；事实由调用方（会话/执行器）从 SDK 目标 读进来，计划原样复制。参考实现踩过的坑就在这里：**"第一个深度可采样的输入就是太阳的 map"会把 G-buffer 的 深度当成阴影贴图**（整个 deferred 分支"影子是世界坐标的函数"，`.ai/design/graphics-shadow.md` 有记录）。
* （`castShadow`）、类型是方向光、且在灯块的三个槽内**；满足才写矩阵（列主序）与 `params = {1, 该灯自己的 ShadowSettings::bias, 1, 槽号}`，否则整块**零字节**（开关关）。 `api/LightBlock` 新增 `directionalSlotOf(lights, identity)`——"这盏灯落在第几槽"必须和灯块打包**同一次 遍历**（旧实现的注释原话："the same walk collectLight…"），否则 `params.w` 会指向另一盏灯。
* **顺带的键修正（实测推论，已改设计）**：`PipelineKey::shadow_bound` **删掉**。它在 v1 里是从旧实现抄来的
* **只能由目标"认领"** — 变异 M4（去掉"目标说了属于哪盏灯"这个条件，任何"可采样深度 + 有矩阵"都算）⇒ 计划把 G-buffer 的深度当成 map（`pass.shadow.light == NULL`）——这就是参考实现的实测缺陷
* **槽号决定缩哪盏灯** — 变异 M2（`params.w` 恒 0）⇒ 设备用例带 2 从 `(26,128,26)` 变成 `(128,26,26)`：本该被缩放的那盏灯没被动，另一盏被缩没了——"整张 map 缩放所有灯"的经典错误
* **开关是灯的运行期事实** — 变异 M3（忽略 `castShadow`）⇒ 无设备用例 + 设备用例带 1 红（同一盏灯在两趟调用之间**被翻开开关**⇒ 带 1 从全亮变成只剩 ambient）；相机夹具第一版用**另一个 Light 对象**表达"不投影"，M3 只在无设备用例红——那测的是**身份**不是开关，现已改成同一对象翻开关
* **组合必须走世界** — 变异 M1（丢掉 `inverse(view)`，直接用生产者的矩阵）⇒ **只有算术用例红**（设备用例的相机只有平移，而测试选的生产者矩阵不读 z ⇒ 平移不可见）。又是一次"判据选对"：这类约定归算术用例
* **矩阵是列主序** — 变异 M5（按行写）⇒ 算术用例红（测试的矩阵有非对称项 (0,1)=0.25 / (1,0)=-0.125，对角矩阵看不出来）
* **拒绝必须写零** — 变异 M6（拒绝时把 `params.x` 留成 1）⇒ **只有字节级用例红**：设备像素看不出（零矩阵 ⇒ `light_clip.w = 0` ⇒ NaN ⇒ 着色器的范围判断全 false ⇒ 恰好落回"亮"，那是巧合而不是检查）
* **块是每次调用的** — 变异 M7（整趟 pass 只打包第一调用的阴影块）⇒ 设备用例三条像素红（带 1/2/3 全按带 0 的灯与槽算）
* M4：不要求目标认领（任何可采样深度 + 矩阵都算） — 1 条红（计划选错输入：G-buffer 的深度变成 map）
* * **`params.y/z` 的语义只钉了"来源"**：bias 来自投影灯的 `ShadowSettings`（实测值随灯走），强度恒 1；
* * **全屏路径的 128B push 光照/深度重建半边**仍未填（M5c-1 登记过），它读的是同一份灯数据的另一种表示。

### 11.16w M5d（2026-09-22）：全屏路径的 128B push（光照的第二种表示，真设备像素）
* M4b 登记的"push 的形有证据、内容没有"到此关闭。全屏路径**不需要视图矩阵**（顶点阶段是 `gl_VertexIndex` 生成的正典三角形），所以它把整个 128B push 预算花在光照上——同一个场景的两条路径 因此有两种表示（前向 UBO / 全屏 push），值必须一模一样。三件事：
* **两种表示，一份打包** — 变异 P1（把 UBO 的字节直接塞进 push，两个布局混用）⇒ 无设备用例红 + 设备用例带 0 从 `(128,26,26)` 变 `(26,26,26)`：方向/颜色整体错位，太阳的项落进了保留位
* **内容真的有到** — 变异 P2（push 保持全零，即 M4b 的形态）⇒ 三条带全 `(0,0,0)`（未定义 push 内存"常常也读成零"，所以**反向**断言才是有用的方向——本条正是"M4b 的形有证据、内容没有"的实证）
* **按调用推** — 变异 P3（整趟 pass 只推第一调用的灯）⇒ 带 1/2 变成带 0 的颜色 `(128,26,26)`
* **阶段是 ABI 的一半** — 变异 P4（push 发到顶点阶段）⇒ **0 VUID**、带 1/2 仍显示带 0 的颜色：片元阶段从没读到新 push，静默失败——正是"阶段写错看不出"的实测
* 它的 near/far/proj00/proj11 必须**来自相机自己的投影矩阵**（`CameraSnapshot::projection` 的两个对角 + near/far），而不是在这里二次发明一套公式。
* * 前一版（§11.16v）留下的三条口子（`TargetFacts::shadow` 生产侧、`params.y/z` 语义、场景桥 ABI 债）不变。
* 实现**调用** `packLightBlock`（同一个遍历、同一套规则），再把三个字段搬进 push 的布局—— 两次遍历就是"同一个场景两条路径照出不同亮度"的入口。128B 的四个槽位：`ambient` / **`projparms`** / `dirs[3]` / `cols[3]`，静态断言钉死（含三个偏移）。
* "没有输入时的 pipeline layout"，于是 `layoutFor(0,0)` 返回空 ⇒ **任何"只读 push、不声明输入"的全屏 pass 都被拒**（"its pipeline could not be built"——那是一条从未被请求的管线，而不是失败）。修法：屏幕层也建一份 "只有 push range、没有集合"的布局，`layoutFor(0,0)` 返回它；M4a 那条"全屏层没有布局"的断言按此更正。 "全屏程序只读 push"是合法调用（本片的设备用例就是这样），不是边角。

### 11.16x M5e（2026-09-22）：目标生命周期（计划驱动的换尺寸、租约两向拒绝、停车 vs 计数空闲）
* ：`Data` 里新增 `struct Attachments`（颜色附件 + 视图 + 回读缓冲/映射、深度图 像/视图/回读、帧缓冲、`render_graph`），并且**只由参数化函数构建** （`buildAttachments(width, height, out)`）——纯"按给定尺寸造对象"的一步，**不写 本对象任何字段**。`create` 先建渲染通道与其首个变体（形状的），再调它，成功了才 `std::move` 进 `attachments`，**借用计数也改成成功之后才 +1**（原来在 `return nullptr` 之前就加，创建失败会漏一个借用者）。 尺寸之所以是**参数**而不是 `width()/height()`：换尺寸必须**先造好替代品**才能碰 旧的，而造替代品的那一瞬间，目标仍在服务旧尺寸。
* （`RetirementQueue::retire(timeline, …)`），到退役点才释放。闸门关着（调用方从 来没学到在飞槽数）时 `retire()` **返回 false 而不是猜窗口**——那时释放必须退回
* 路径不停设备"这条不变量的判据（§11.17 的 D5）。 实现细节上有一处必须做对：`retire()` 的 `ReleaseFn` 是**按值**收的，拒绝时它直 接把回调丢掉——**回调若按值持有那批对象，就会在 `retire()` 内部、没有 idle 的情 况下把它们析构掉**（而提交过的命令缓冲可能还命名着它们）。所以这里用一个 `shared_ptr` "托管"捕获：队列丢掉回调什么也不会析构，本地还有一份，退到 idle 那 条路才真正释放。
* 者的尺寸**，而帧缓冲附件必须**不小于**帧缓冲本身 （`VUID-VkFramebufferCreateInfo-pAttachments-00861`）⇒ 尺寸不是借用方能挪的， 调用方先换出借方、再按新尺寸重建这个目标。 `refused` 两向都置位，**不替换、不停车**；借用者析构（计数递减）后同一条调用立 刻替换成功。
* 行者用 `bootstrap = !written()` 推"谁清屏"的那个事实（新图像是 UNDEFINED，LOAD 没有意义）；后者是调用方"我编的那一帧命名的图像没了"的判据。
* **租约是唯一的拒绝原因** — 变异"租约不再拒绝"⇒ 租约用例红（`refused` 为假、尺寸被换掉）
* **停车 ≠ 丢引用** — 变异"报告 parked 却直接丢掉这批对象"⇒ `pending()` 与引用计数两条断言红（对象真的没了，而报告说它停着）
* **`written` 必须复位** — 变异"不复位"⇒ 用例红，并且**验证层与像素同时报**（第二趟不宣布清屏 ⇒ 被当作 LOAD 而不是首写者）
* **换代要真的换** — 变异"不 bump generation"⇒ 红；变异"换了对象却不采纳新尺寸"⇒ `width()/height()` 与探针红
* **退到 idle 要计数** — 变异"不计数"⇒ `deviceWaits()` 断言红（这条判据就是计数，不是"跑起来没崩"）
* * 前一版（§11.16w）留下的四条口子（`projparms` 无读者、全屏丢弃报告、`binding 5/6`
* 与 `Repair` 直接返回、一个 GPU 对象都不碰）：`TargetInstance` 由目标自己的事实填 （`desc`、`generation`、`built = render_graph != nullptr`），`wanted` 的**形状就 是目标自己的形状**——格式在 `create` 时定死，换形状是另一个目标而不是"改尺寸"。
* P2：`written` 不复位 — 1 条红（`written()` 断言 + 随后的验证层报错 + 像素不符）
* * **`attachments_invalidated` 没有生产者**：`TargetInstance` 有这个事实、`planTarget`

### 11.16y M6（2026-09-22）：读回（一张分类表、格式诚实、借用方读到源，真设备）
* 格式、深度格式、各自有没有交出过拷贝节点）+ `ReadbackRequest{kind, attachment}` → `ReadbackResult{ok, refusal}`。**优先级是写下来的**（无设备用例逐条钉）：请求存在性 （`UnknownAttachment`）→ 格式能不能读（`UnreadableFormat`，**永久**原因）→ 有没有交出拷贝 （`NotCaptured`，**下一帧可以再试**）。"永远不行"和"还没行"必须是不同答案：方向报错一次，读者就去错的 地方找 bug。
* **永久 vs 暂时** — 变异 P5（把 `NotCaptured` 排在格式之前）⇒ 无设备用例红：一个跑多少帧都读不出来的目标会被告成"再试一次"
* **簿记是"交出录制"** — 变异 P1（忽略 `captured`）⇒ 无设备用例 + `AProbeBeforeAnyCapture…` 红（未捕获的探针会变成"有效"）
* **格式诚实是 core 的事** — 变异 P3（D16 按 4B 读）⇒ 设备用例红（解码把 2 字节数据当 float）；P4（D16 不除 65535）⇒ 无设备 + 设备用例都红（0.5 变成 ~32768）
* **拒绝格式 ≠ 不渲染** — 变异 P2（忽略"可读"检查）⇒ `Buffer::create(0)` ⇒ `VUID-VkBufferCreateInfo-size-00912` + **段错误**（不是静默）
* **借用方读的是共享图像** — 变异 P8（拷贝的 `srcImage` 换成"自己的图像"，借用方为 null）⇒ **段错误**（不是静默）；变异 P7（借用方盗用出借方的捕获标志）⇒ 断言红
* * 前一版（§11.16x）留下的三条口子不变（`resize` 的执行者、`attachments_invalidated` 的生产者、租约的"重建
* 1. **一张表**（`core/Readback.hpp` / `.cpp`）：`ReadbackState`（目标的事实：颜色附件数、被请求附件的颜色
* `captureDepth()` **交出节点**时置位——和 `written` 是同一条约定（记的是**录制**侧，"录了但丢帧"仍是同一个 目标）。**簿记属于附件集**：换尺寸整体换集，新集的缓冲里什么都没有 ⇒ 标志天然为假。没有它，一次"还没跑过 帧"的读回会返回**分配内存里碰巧有的东西**（旧实现的 `probe()` 就是这样），而头文件早就承诺过"没捕获 ⇒ 无效 探针"——这一片把文档兑现了，并且让"为什么"可查（`readbackResult`）。
* **分类先于任何工作** — 读回被拒时**不看设备、不拷贝**：`readbackResult` 是纯事实函数（无设备用例 0 ms 跑完）
* RGBA8）与它的解码/断言，那是新类型，而不是把这张表放宽——放宽只会让"看起来像图片"的东西替真实数据回答。

### 11.16z M7（2026-09-22）：证据加固（重写版的第一张真相位表 + 门禁脚本 + 稳态帧的门）
* （recorder + compiler）在暖机后不再向堆要一字节，arena 也不加块。判据是"两帧都 0"而不是"一帧 0"： 一帧不分配是那一帧的事实，两帧连着不分配才是规则。
* `invalid_schedules` 恰好 +1。 写这条用例时撞到的两件实测（都写进了注释，因为它们都能让相位**假绿**）： ① `beginFrame` 由协议状态机把关，`endFrame` 之后必须 `swapBuffers()` 才走完一帧的契约——忘了它，下一个 `beginFrame` 直接被拒（相位红得很吵，比静默好）； ② **既没有绘制也没有清屏的 pass 根本不是 pass**（plan 会把它丢掉），第一版的环形 pass 因此"确实被跳过了、 可计数是 0"——`passes.empty()` 为真，行看起来通过了，实际什么都没测；把两个 pass 都改成真 pass（各带一次 绘制）之后，这一行才真正钉住 `invalid_schedules`。 行文本本身冻结成基线（`[selftest] <name>` + 收尾 `[selftest] done`，与旧自检同一格式）：**相位名单就是能力 名单**，改名或消失是一次 diff，而不是一条沉默的缺口。
* build → 套件（含真设备用例，**跳过即失败**，除非显式 `VINE_GATE_ALLOW_SKIPS=1`）→ 强制验证层 （`VUID` / `Validation Error` 计数必须为 0）→ 同步验证（`SYNC-HAZARD` 必须为 0，`--quick` 可跳过）→ 三个 hygiene 脚本 → `[selftest]` 行必须以 `[selftest] done` 收尾，最后打一张阶段表。
* **为什么必须是脚本**：套件全绿的同时验证层可以一直在报 VUID——"测试过了"根本不是这个后端要下的结论；而手跑
* 的仪式里，最后一个阶段总是最容易先被跳过。 门禁**自己也能失败**（三条实测，见下表）：伪造的 VUID、伪造的 `[ SKIPPED ]`、没有 `[selftest] done` 的 相位输出，分别让对应的阶段红。"不允许它失败"的门禁等于没有门禁——这条规矩在 M0 的分配门禁上已经立过一次。
* 3. **实测记录**（写相位时用二分窗口量出来的）：plan 路径的存储在**头两帧**里有界地长一次（第二次 `compile`
* **假绿要能被自己抓到** — 实测的两处（忘了 `swapBuffers()`、空 pass 不是 pass）都是"行看起来通过了却什么都没测"的形态，注释里点名
* * 之前的口子不变（`resize` 的执行者、`attachments_invalidated` 的生产者、租约的"重建借用方"、float 颜色读回、
* **"测试过了"不是结论** — 套件与验证层是两件事：脚本读输出里的 `VUID`/`Validation Error`/`SYNC-HAZARD` 计数，才让"0 VUID"成为可失败的断言
* **计数器要真的动** — 第 2/3 行分别把 `draws`、`invalid_schedules` 的增量写成期望；表在 `run` 前后各读一次计数器，行本身不自己断言数字
* P4 把一行相位改名 — 基线断言红：`report.lines != baseline`（"相位名单 = 能力名单"）
* 存在：把设备用例也搬进 `PhaseTable` 需要"相位运行器"那一层（相位 = 装配 + 断言 + 计数器增量 + 像素读回）， 而装配逻辑现在分散在各用例里。本片先落地**格式、基线与门禁**，形态统一留给相位运行器那一片。

### 11.16aa 执行者收口（2026-09-22）：目标自己说事实、丢帧有生产者、执行者循环落成用例
* §11.16x 登记的三条口子里，有两条是"语义已经写好、但没有生产者"：`attachments_invalidated` 没有生产者、 `resize` 没有执行者。这一片把**事实的产出与消费**接成一条可跑的循环，并把"谁报告丢帧"留给会话那条路（见口子）。
* （含设备格式，兼容性那一半照旧在 `shape()` 里）、`generation`、`built`、`attachments_invalidated`。这里 有一条必须说清的语义：**`built` 不是"图像存在"，而是"能 LOAD"** —— 一个刚建好、从没写过的目标如果报 `built = true`，计划就会答 `None`，第一趟 pass 于是去 LOAD 一张 UNDEFINED 的图；所以 `built = written` （M5a 起 `written` 就是"有人往里录过东西"的那个事实）。设计文里 `TargetInstance::built` 的旧措辞 （"Attachments exist for desc"）按这份实现更正为"**loadable**"。 收益立刻可见：**15 处手拼的 facts 换成 `instance()`**（`ContentPassTest` 3、`SampledInputTest` 6、 `ShadowBlockTest` 2、`LightBlockTest` 2、`WindowCompositionTest` 2），而这些用例里有 8 条真设备像素断言 ——如果 `instance()` 与它们原来的手拼说法不一致，那些像素会当场红。一处拼写、一处事实。
* 除**：图像还在、还是合法的 LOAD 对象，变的只是"内容可信吗"这个事实。计划的反应是 `Repair(Bootstrap)`（`planTarget` 第 2 条），于是**下一帧的第一个写者清屏**而不是 LOAD。 谁是"生产者"：**报告丢帧的调用方**（目标不观察提交），也就是会话/执行者的那条路（本片只提供缝，见口子）。
* 语义上必须如此——**只有清屏能把"内容未知"变回"内容已知"**，一次 LOAD 不能声称它修好了什么；所以 `bootstrap = false` 的调用**不许**清标志（用例钉住：忽略计划的调用者清不掉它）。 同一条约定也用在 `resize` 的替换臂上：新图像换掉了被丢的那批，标志随旧集合一起消失（`written = false` 已经让计划说 `ResizeInPlace` ⇒ 首写者清屏）。
* `instance()` 拿 → `FrameCompiler` 给决定与 `pass.bootstrap` → 用**计划给的那个 flag**建图 → 提交 → 读像素。 像素是判据：帧 1（目标从没写过）计划必须答 `Repair(Bootstrap)`，画面是清屏色；丢帧后帧 2 **不宣布清屏** 而计划仍要求 bootstrap ⇒ 画面是**这一帧自己给的颜色**（说明真的清了）；帧 3 不再清（画面停在帧 2 的颜色， "只修一次"由此可判）；最后 `invalidate` + `resize` 后 `attachments_invalidated` 为假、`generation` 前进。
* **`built` = "能 LOAD"** — 变异 P3（改成"图像存在"）⇒ 帧 1 的计划变 `None` ⇒ 首写者不再清屏 ⇒ 断言与像素都红
* **丢帧必须被记录** — 变异 P1（`invalidateAttachments()` 空实现）⇒ 帧 2 的计划变 `None` ⇒ 画面停在红（旧内容）而不是新颜色
* **只有清屏能修** — 变异 P5（任何 pass 都清标志）⇒ "忽略计划的 LOAD 不许修事实"那条断言红；变异 P2（bootstrap 也不清）⇒ 帧 3 又清了一次 ⇒"只修一次"红
* **换集合就换事实** — 变异 P4（resize 保留标志）⇒ `attachments_invalidated` 断言红
* * **租约的"重建借用者"仍没有 API**（§11.16x 口子不变）：调用方现在只能先析构再 `create`。
* 2. **`invalidateAttachments()`**（新）：一份"提交失败 / 设备丢失 ⇒ 里面现在是什么没人知道"的事实。它**不是拆
* 3. **修复这个事实的，恰好是"清屏的那一趟"**：`passGraph(..., bootstrap = true, ...)` 在交出图的同时把标志清掉。
* P1 `invalidateAttachments()` 不记录 — 2 条红（事实断言 + 像素）

### 11.16ab M7 第二半（2026-09-22）：设备侧相位运行器（能力只有一个写法、两个读出口）
* §11.16z 留下的第一张口子就是"设备侧相位还是散用例"。这一片把**设备能力**搬进 `PhaseTable` 的形态，但 不新增第二份断言：**相位体就是用例体**。
* 方）、`resizes_replaced +1`（换尺寸）、`frames +3`（丢帧修复：新帧 / 修复帧 / 稳态帧）。只写"它跑过了" 的行会在能力**什么都没做**时照样通过——计数列就是为此存在的（变异 P2 实测：相位体直接 return ⇒ 行以 "counter expectation not met" 红）。
* 4 行），门禁输出 9 行 `[selftest]`、2 个 `done`。**门禁的判据随之改对**：原来要求"最后一行是 `[selftest] done`"——两张表之后，**前面一张表红、后面一张表干净收尾**就会骗过它（实测：伪造输出 `FAILED` 行 + 随后的干净表，旧判据 PASS）。现在判据是"**没有任何 `FAILED` 行** + 每次开始运行都收尾"， 伪造输入实测红。
* **能力只有一个写法** — 变异 P1（把离屏读回的期望颜色改掉）⇒ **细节用例与相位行同时红**（同一个身体的两个读出口）
* **"跑过了"不是证据** — 变异 P2（共享深度相位体直接 return）⇒ 用例壳的计数器断言红 + 行以 "counter expectation not met" 红
* **两张表都要能被判** — 伪造"非末表 FAILED + 末表干净收尾"的测试输出 ⇒ 门禁红（旧判据会放行）
* 要照抄语义而不是代码：①每个 render pass 一个采样（离屏 pass + 窗口图，窗口图给的是"呈现路径"整体）； ②采样要有**身份**——上游 `RenderGraph::record` 的时间戳是空对象，所以旧实现给每个 pass 的图包一层 `vsg::InstrumentationNode` 并把图自己当对象传；③读结果**不带 `VK_QUERY_RESULT_WAIT_BIT`**，读到的总是 几帧前的数据，用 `age_frames` 说清"这是哪一帧的"，因为"读到 0 帧延迟"意味着阻塞读，而阻塞读会抬高计数过的 device wait。重写版要做的是：执行者按开关包那层 + 名字从编译好的帧里取（目标/顺序/"window"）+ 会话暴露 `age_frames`。这是独立一片（要动执行者与会话，且需要自己的像素/计数器判据）。
* * 之前的口子不变（调用 `invalidateAttachments()` 的提交失败缝、`Rebuild` 臂、租约"重建借用者"、float 颜色读回）。
* `runSharedDepthPhase` / `runTargetResizePhase` / `runLostSubmissionPhase` 各自是一个函数，**既**被细节 用例调用（`TEST(...) { run…Phase(device, counters); }` 这样的薄壳），**也**被相位表的一行调用。一个能力 长出一条相位没跑的断言、或一条相位行没有身体，在这种形态下**不可能**发生——这正是"两处各写一遍"会烂掉 的地方。断言仍是 gtest 的（红的时候给文件与行号），而**计数器**是行真正门住的东西：帧数、建了几个目标、 换了几次尺寸、停车几个、idle 几次——`DevicePhaseCounters` 记的是"这一相位**驱动**了什么"。

### 11.16ac M7 第三半（2026-09-22）：性能档的"身份"半边（每个 pass 都记成可归属的区间）
* M7 的最后一样是性能档（GPU profile）。它有两个半边：**采样有没有身份**（执行侧的包装）与**读数字**（会话侧的 profiler 安装 + 不阻塞的读取）。这一片把第一半做完，并把第二半的确切语义写进下面的口子。
* `RenderGraph` 本身——没有包装节点、没有名字、没有条目。这是可被变异证明的断言（变异"永远包装"⇒ "关着不加东西"那条用例红）。
* pass 上（变异 P4 实测红）。名字里的序号用 `target_index`（计划给的）与 `schedule_index`——**索引式**，等 适配层能给 SDK 句柄起名时再换成人名；这不是损失，因为归属不靠名字。
* **关着不加东西** — 变异"永远包装"（守卫恒假）⇒ "关"用例红（找到 1 个包装节点）
* **开着必须包装并可归属** — 变异"从不包装"（守卫恒真）⇒ "开"用例红（没有包装、没有条目）
* **身份是图，不是名字** — 变异 P5（包装一个空 `Group` 而不是那张图）⇒ 归属断言红 + 像素红（pass 根本没记录）
* **名字带序号** — 变异 P3（名字用 pass 号代替 schedule）⇒ 名字断言红
* **条目属于本帧** — 变异 P4（不清空条目）⇒ 第二帧的条目数断言红
* `VINE_VSG_PROFILE_CPU` / `_GPU` 设定 `vsg::Profiler::Settings` 的两个 level）在会话的 viewer 上安装 `vsg::Profiler`；②读取**不带 `VK_QUERY_RESULT_WAIT_BIT`**（vsg 自己就是这么读的：没就绪的查询下次再读）， 因此读到的总是几帧前的数据 ⇒ 必须给出 `age_frames`（这一帧与数据所属帧的差），并保证**读取本身不抬高 `deviceWaits()`**（这是本后端"帧路径不停设备"的判据，profile 不许例外）；③归属用本片的 `profileOf(interval.object)`，**不需要**新的目标表；④数字只做"非负 + 相位有采样"这类断言，时间不是像素， 不假装能断言具体数值。
* * 之前的口子不变（提交失败缝、`Rebuild` 臂、租约"重建借用者"、float 颜色读回等）。
* 上游 `RenderGraph::record` 自己写的每图时间戳带的是**空对象**，捕获工具拿到的区间无法归属到任何 pass； 包装节点把**图自己**当作对象传给 instrumentation（`InstrumentationNode::traverse(RecordTraversal&)` 传的是 `child.get()`），于是执行器手里"图 → 是哪个 pass"的账本（`ProfileEntry` + `profileOf(graph)`）就是 读数字那一半需要的全部映射——不需要再建第二张目标表。名字（`"target<i>@<schedule>"`、窗口是 `"window@<schedule>"`）只给人看（捕获里、vsg 的报告里）；**读者按图匹配**，这正是旧实现文件里写下的同一条 约定。

### 11.17 下一步
* （本表原是里程碑清单：已完成项见各片；**未做项并入 §6 的登记表**；编号保留供旧引用。）

### 11.16ad（M7e）会话侧读数字半边
* 断言：读之前 / 之后的 `deviceWaits()` 必须相等——设备侧等待正是 `RetirementQueue` 存在的理由， 读性能数字不该是它的第一个反例。
* 归属键是**地址**，只许比较、不许解引用（图可能在帧之间被释放——旧实现实测过这条崩溃）；归属映射的另一半在 `VsgExecutor::profileOf(graph)`。时间戳是否可用是**设备事实**（`timestampComputeAndGraphics`），不可用时 回答“开着、但没有可读的”，而不是一个数字。
* 实测（lavapipe）：8 帧后 `readable == true`、`age_frames == 2`、`frame_gpu_ms ≈ 1.86 ms`、1 个带非空键的 样本——样本数是日志里带对象的 GPU 区间数（会话自己不包 pass，所以这是上游/执行层写下的那些），用例只断言 “键非空、毫秒非负”，不断言条数（那是驱动时序的函数）。
* 证据：`SessionTest.TheProfileIsTheEnvironmentsSwitchAndReadingItNeverStopsTheDevice`（关：`profiling()` 假 + 4 帧后不可读 + 空 `passes` + `deviceWaits()==0`；开：8 帧后 `readable` 必须为真、`age_frames < 8`、 逐样本键非空且毫秒非负、读前后 `deviceWaits()` 相等；无时间戳能力则 `GTEST_SKIP`）；变异反证 5/5 红—— (a) `readable` 永不为真、(b) `age_frames` 取成 `frames.size()`、(c) 关着也报可读、(d) 读里加一次 `noteDeviceWait()`、(e) 空键样本放行。门禁：565 用例 / 88 套件、0 VUID、0 SYNC-HAZARD、hygiene 全清、 相位 9 行 / 2 次运行全收尾。

### 11.16ae M8a（2026-09-22）：场景桥的第一半——程序文本的声明成为事实（`api/ProgramAbi`，无设备）
* §11.16u 把“引擎自带的前向程序文本与本后端的 set 0 布局不同”登记为场景桥的债。这一片不动管线，只把债
* 写在宿主给的文本里，而“文本与集合布局必须匹配”不是风格问题（不匹配时 shader 读的是没人写过的地方， 而管线等的是一个 shader 从不点名的绑定）。所以场景桥的接法是**按程序文本自己的声明服务**：布局跟着 声明装配（M8b），每个声明按**角色**取值（M8c）。重写版自己的 set 0 布局因此不是“两套 ABI”里的另一套， 而是**另一种声明**——它的程序照 `VineViewBlock` 这类 L1 名声明，角色就认出来了；这正是 `graphics-shader.md` §11.3 早就写下的“GLSL 块类型名与 L1 名逐字相同”。
* **角色是块的类型名** — 变异 N2（角色按 binding 序号给：0=view、1=draw…）⇒ 4 条红。引擎的 material 在 0、lights 在 2，按位置读会读成 view/draw——这条正是“位置不是角色”的实测
* **条件决定事实** — 变异 N1（不管 defines，所有分支都算 taken）⇒ 4 条红：同一个 `(0,1)` 上出现两种 sampler 种类 ⇒ Malformed
* **`#error` 是拒绝** — 变异 N5（taken 分支里的 `#error` 当作能编译）⇒ “只给 `VINE_DIFFUSE_MAP` 的变体”那条红
* **尺寸要按 std140 算** — 变异 N3（成员字节相加，不做对齐/补齐）⇒ 4 条红（52 → 64 这类补齐没了；`sizeof` 对照是判据）
* **一个 `(set,binding)` 不能是两个东西** — 变异 N4（把同一 `(set,binding)` 的两个声明当成同一个绑定）⇒ 拒绝面那条红
* 属于全屏路径的后续（§11.16v 登记过）。
* 证据：`test_vsg` 全量 **576 用例 / 90 套件全绿**（+11 用例，`ProgramAbiTest` 一套）；门禁一条命令 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 821 文件、 `check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。变异反证
* **变成事实**。理由是那句债的反面：**一个后端选不了 shader 的块住在哪**——`layout(set = …, binding = …)`
* **事实与政策分开**。`scanProgramAbi(vertex, fragment, defines, out)` 只回答文本声明了什么：
* `(set, binding)`、种类（uniform 块 / `sampler2D` / `samplerCube` / 其它 sampler）、哪些阶段读它、 它是不是五个 L1 块之一（**按类型名，不按位置**）、它的 std140 大小（按成员算出来的）、以及 push 范围 （std430，认 `layout(offset = …)`）。它**不**决定哪段字节、哪张图去哪个绑定——那是服务层的政策，全屏 路径与内容路径在同一份事实上做不同的决定。认不出的块类型是 `Foreign` 这一**事实**而不是拒绝：文本合法， 是后端填不了它，拒绝属于绑定那一层。同理，一个没有绑定的 `uniform sampler2D` 不是事实（文本什么都没说， 编译器自己分配）——不许替它编一个槽。
* **变体才是事实的主体**。引擎的程序用 `#ifdef` 门住声明（`VINE_DIFFUSE_MAP`、`VINE_TEXCOORD_CUBE`…），
* 所以“这个程序的绑定”要等 defines 才知道：扫描**按变体回答**，条件只读地求值，文本自己的 `#define` 算数（平面前向的 `VINE_FLAT` 就是这么来的），而 **taken 分支里的 `#error` 让变体 Malformed**——那是驱动 要到很晚才说、而读文本的人现在就能说的事（SDK 的 texcoord 种类就是这么写的：采了槽却没说 kind ⇒ 编译 不过）。扫描的语法是一个小而固定的子集（`#ifdef` / `#ifndef` / `#if defined(X)`（可带 `!`）、同形的 `#elif`、`#else` / `#endif`、`#define` / `#undef`、`#error`、`#pragma import_defines(…)`），**读不出来 的报，不猜**：一个绑定猜错的后果是着色器读没人写过的内存。`import_defines` 也进事实——编译器会把源里 没要过的 define 静默丢掉，那是只有读过文本的人能说出的第二件事。

### 11.16af M8b（2026-09-22）：布局就是声明——块服务到文本自己写的那个 `(set, binding)` 去
* `serveHalf` 用“set 序号 + 形状逐位相同”去认领（认不出就按名拒绝、每个条目只报一次），`recordCommand` 把 每条声明集的 `bind` 放进 `Draw::blocks`（span，逐条录制）；每个声明绑定一个动态偏移、按**形状的顺序**给 （不是按它们被写进代码的顺序）。还在 `serveHalf` 里立了 M8c 之前必须立的一道闸：**声明了 push 却没有填** ⇒ 拒绝这一半（“黑屏是因为矩阵是零”不算诊断，见 §11.16x 的教训）。
* **角色是类型名，不是位置** — 变异 N1（角色按 binding 序号给）⇒ **7 条红**，含新的像素用例（`(0,0)` 上的 material 会被当成 view ⇒ 黑）与 `shape()[0].role == Material` 断言
* **声明写在哪个 set 就是哪个 set** — 变异 N2（所有块都进 set 0）⇒ 无设备用例 `sets.size() == 2` 失败 + 像素用例 SIGSEGV（shader 要的 set 1 布局不存在）
* **每个声明绑定一个动态偏移** — 变异 N3（只写一个偏移、其余复用）⇒ 2 条红（正典五绑定那条 + 新的声明形状 `{0,64}` 那条）
* **空隙必须是真布局** — 变异 N4（空格填成空指针）⇒ 像素/布局用例 SIGSEGV（`PipelineLayout` 里出现无效句柄）——**这条实测还暴露了一处死代码**：`create` 里那段“补空”防御循环从来没被走到过，已删（空隙只在 `describeAbi` 一处填）
* **填不了的拒绝** — 变异 N5（不看 std140 / 不看尺寸）⇒ 拒绝面那条红（超尺寸与非 std140 的层被建了出来）
* **逐声明集绑定** — 变异 N6（第一个块集复用给所有声明集）⇒ 像素用例 SIGSEGV（set 1 上绑了 set 0 的集合）
* **“什么都不声明”就是“什么都不绑”** — 门禁实测：三处**测试夹具**（`ExecutorTest` / `MrtTargetTest` / `ScreenDrawTest`）仍无条件绑一个块集，而它们的程序文本一个块都没声明 ⇒ 布局 0 个 set、绑定无效句柄：`VUID-vkCmdBindDescriptorSets-firstSet-00360` ×6。修的是夹具（没有声明就没有集合可绑），不是给后端加“容忍”
* 支持一起做（§11.16ae 登记过）。
* 证据：`test_vsg` 全量 **580 用例 / 90 套件全绿**（+4 用例：真设备像素 1、真设备描述符 1、无设备 2）；门禁 一条命令 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 822 文件、 `check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。变异反证
* * **变体 define 还没进管线身份**：`import_defines` 已经在事实里，但“哪些 define 生效”与几何/材质的纹理
* * **三张表的生产侧**还是 §11.17 那一行：活的 SDK 对象 + 修订 + 退役（现在的用例都是自己填事实）。
* 其中 N2 / N4 / N6 的失败形态是崩溃（无效句柄进 `PipelineLayout` / 绑错集合），N1 / N3 / N5 是断言。

### 11.16ag M8c-1（2026-09-22）：声明出来的 push 由 pass 自己填（相机矩阵按名字装配）
* **push 必须紧贴绘制、按插入序记** — 变异 N1（把 push 放回 `StateGroup` 的 stateCommands——vsg 按 slot 记录）⇒ 像素用例红：**命令确实发出去了（vsg 里打印得到我的字节），但 shader 读到的是恒等矩阵**，画面“看着正常”。全屏路径（M5d）一直把 push 加进 `Commands` 节点，这就是原因
* **按名字装配** — 变异 N2（按声明顺序：offset 0 给 modelView）⇒ 6 条红（4 条无设备 + 2 条设备）
* **投影要折** — 变异 N3（直接用 SDK 投影）⇒ 4 条红：几何被裁掉（设备 z 出 [0,1]）、画面=清屏色——§11.16o 同一个坑，这次在 push 上
* **`modelView` 的顺序** — 变异 N4（`model * view`）⇒ 4 条红
* **尺寸不对的名字也要拒** — 变异 N5（只看名字不看尺寸）⇒ 4 条红（层会把 `vec4 projection` 当成投影矩阵建出管线）
* * **相机缺失时的零矩阵**只是“没有视图”的值：pass 照画（几何落在原点）。要不要在上层把“无相机的 pass”
* 证据：`test_vsg` 全量 **587 用例 / 91 套件全绿**（+5 用例：`ContentPushTest` 5 条、`ContentPipelineTest` +1、 `ContentPassTest` +1 —— 其中 `ContentPushTest` 是第 91 套）；门禁 一条命令 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 825 文件、 `check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、相位 9 行 / 2 次运行全收尾。变异反证
* 形态最有价值：它在“命令发出去了”的情况下仍然红——判据是像素，不是“我发了命令”。
* §11.16af 让布局跟着声明走，于是“一个声明了 push 的程序”不再是 M8b 那样被 `serveHalf` 一拒了事：范围是 它自己声明的（128B、顶点阶段、`offset = 0`），里面要装什么也是它自己写下的——`projection` 与 `modelView` 两个 `mat4`。而这两份值不是新约定，是**同一对 L1 值的另一个居住地**：本后端把 `VineViewBlock` / `VineDrawBlock` 直接绑成块，而那 128B 的 push 是同一对矩阵的 L2 实现（既有后端的 `VsgPipelineFactory.cpp` 注释写得很清楚：`pc.projection == VineViewBlock.proj`、 `pc.modelView == VineViewBlock.view * VineDrawBlock.model`——同一段话，改写一遍）。所以这一片做的事是 把它**真的填上**：`describeAbi` 在 `create` 就拒“填不了的成员”，pass 在每个绘制命令前把声明的每个范围 按名字填好、按声明的阶段与偏移发给 `PushConstants`。
* * 没有相机 ⇒ 写零（与 `buildViewBlock` 同一个 `present` 事实）： “没有视图”是一个值，不是错误。

### 11.16ah M8c-2a（2026-09-22）：一个声明的集合可以同时装块与采样图（引擎的 set 0）
* `SampledBinding{binding, view, sampler}` 是图的半边（块那半边照旧走 arena + 动态偏移）， `layoutOfShape(shape, sampler_bindings)` 是**这一个集合布局的唯一拼写**，`samplers()` 把图读回来。 层这一边：`declaredSets()`（调用方要建的集合）与 `samplerBindings(set)`（那个集合里必须有哪些图）—— 把 `blockSets()`（只有块）换成它们的理由很直接：引擎的 `skybox` 程序**一个块都没有**，只有一个 `skyMap`， 它的 set 0 照样是“一个要建的集合”。**输入集仍是 pass 自己的**：`declaredSets()` 里排掉 `kInputSet`（除非那个 set 声明了块），因为它的图不是一个调用方能有的东西——它们是 pass 的输入，按声明的 顺序绑（`Data::sampled` 那套照旧）。
* **布局要声明两种绑定** — 变异 M1（`layoutOfShape` 忘掉采样 binding）⇒ 纯套件**绿**、验证层 **2 条 VUID**：布局类主张的判据是验证层（与 §11.16r 同一条经验）
* **描述符要落在声明的 binding** — 变异 M4（图放进 binding 0）⇒ 纯套件红（用例检查 set 的描述符 binding）+ 验证层 2 条
* **“集合对得上”含图那半** — 变异 M3（只比块形状）⇒ 像素用例红：缺图的集合被绑上去
* **混合是合法的** — 变异 M5（恢复“块集里不许有采样器”）⇒ 像素用例红（层直接把程序拒了）
* 证据：`test_vsg` 全量 **589 用例 / 91 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 825 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、 相位 9 行 / 2 次运行全收尾。变异 **M3 / M5 纯套件红**，**M1 / M4 由验证层抓住**（各 2 条 VUID；干净树上 这三条用例的 VUID 基线是 0，实测核对过）。
* §11.16af 让布局跟着声明走，但那时“采样器”仍然只允许住在输入集（set 1）里——那是**重写版自己的**排布的 习惯（一个 pass 采样上一个 pass 的产物），不是文本的规则。引擎的程序不是这样写的：`builtin_forward` 把 material 块与 `diffuseMap` 都放在 **set 0**（binding 0 与 1），`builtin_gbuffer` 同理。这一片把那条 “采样器只能在 set 1”的限制拆掉：**一个集合可以同时声明块（dynamic UBO）与采样图（combined image sampler）**，谁在哪一个 binding 由文本说了算；只要不把两样东西挤在同一个 binding 上（那是矛盾，事实层就 拒），一个集合是什么就是什么。

### 11.16ai M8c-2b（2026-09-22）：没有贴图的材料采样"白"——fallback 是值，不是错误
* > `VUID-vkCmdDraw-None-09600`（实测：demo 最后剩下的 7 条，同一张图、每一帧）。下面这段保留为当时的决定
* **fallback 必须是白的** — 变异 N1（清成黑）⇒ 纯套件红 2 条（无设备用例查了清的四个通道 + 像素用例变黑）
* **清完必须停在描述符声明的布局** — 变异 N2（第二道屏障留在 `TRANSFER_DST`）⇒ 纯套件绿、验证层 **4 条 VUID**：像素在 lavapipe 上照样"看着对"，判据是验证层（§11.16r 的老经验）
* 证据：`test_vsg` 全量 **591 用例 / 92 套件全绿**；门禁 `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、hygiene 0 / 828 文件、`check_diagnostic_formats.py` 0 / 39、`check_doc_symbols.py` 通过、 相位 9 行 / 2 次运行全收尾。变异 **N1 纯套件红**（2 条）、**N2 由验证层抓住**（4 条 VUID；干净树上这两条 用例的 VUID 基线为 0，实测核对过）。
* `vsg::Image` / `ImageView` / `Sampler` 到 `Context` 编译前都只是 create-info，和 `ContentPipeline` 同一个 理由）。**谁把哪个 binding 指向它**是政策：`diffuseMap` → 材料自己的贴图，没有贴图就是白（本片）；材料 真正带贴图、以及按名字把 pass 的输入图指到声明的 binding（`shadow_map` / G-buffer 采样）是"三张表的生产 侧"和贴图那一并做的活（引擎还没有可用的 GPU 贴图路径——`Material::texture()` 有了，缓存与上传没有）。
* → 材料/白、`skyMap` → 环境（仍拒）、其余名字 → pass 的输入，按声明顺序）；随"三张表的生产侧"一起做， 因为**谁把哪个输入图给哪条声明**是那一层的事实。

### 11.16aj M8c-3（2026-09-22）：名字即来源——`shadow_map` 到达它声明的 binding
* 但**反过来的情形必须说话**：pass 解析出了 map、程序的名字表里没有它 ⇒ 画照画（"一个 drawable 不该因为阴影而消失"）， 并**每个半片报一次**（`UnsupportedRequest`："the pass declared a shadow, but the program shading it declares no `shadow_map` sampler…"）。这是旧实现已经认下的判据（§SceneBridgePipeline 的那条），搬到新层后不再依赖 "set 0 / binding 3"这种写死的坐标——**名字**才是契约。
* 常态）时返回 `{Unchanged, offset 0, 0}`，而 `ContentPass::recordCommand` 会照用那个 offset ⇒ 一个帧里**第二次** 绘制同一材料时绑到 offset 0——那是存储缓冲的第一块（view 区），着色读到的 material 全是 0（实测：(0,0,0)）。 之前没有用例把"第二帧的记录"提交上去过（§11.16p 的第一个用例只记录、不断言第二帧的画面），本片的像素帧里同一 材料被画三次，第二次就现形。修法：HIT 也报出**上一次写留下的那块**（`offsetOf(slot, copy)`——arena 的 slot 与 copy 本来就是给读的一侧准备的），并把"hit 也指名"钉进 `BlockStorageTest`。
* M3 "程序读不到阴影"不上报 — 像素红（上报计数断言）——纯套件抓不到（这就是它必须上报的原因）
* **这一片留下的口子（登记，不假装解决）**：全屏路径的输入集仍是"按计数依次绑定"，引擎的 shadow ABI 给全屏程序
* §11.16ai 给"没有贴图"定了值；这一片给"没有阴影"定值，并把**哪个名字的图从哪来**变成一条可断言的策略。
* **`shadowImageOf` 不问位置，问计划**：它按 `resolveShadow` 的同样三个事实（同一个 light 身份 + 深度可采样 +
* **pass 侧能断言的只有它自己知道的事实**：程序声明了 `shadow_map` 而这条 pass 没解析出 map ⇒ 不拒（白替身兜住），

### 11.16ak M8c-4（2026-09-22）：全屏集合就是程序自己的声明——引擎的带影延迟光照端到端
* M8c-3 把"哪个名字的图从哪来"变成策略；这一片把**全屏程序声明在哪个号上**变成策略。 引擎的 `shadowedDeferredLightProgram` 把 `shadow_map` 声明在 **binding 5**、 `VineShadowBlock` 在 **6**（源的四张颜色占 0..3，源自己的深度本应占 4），而它的 G-buffer 没有可采样深度——"按计数依次绑定"（M5d 起的行为）会把 map 放到 4，着色器 在自己写死的 5 上读到未定义数据。判据一句话：**全屏集合的每个 binding 都能从程序的 声明反查出来**。
* RGBA8 / RGBA16F）+ 两张 depth-only 可采样 map（各自一趟只清屏的 pass，**必须录进 帧**：不录就对着镜像里的旧内容比深度）+ 引擎两个变体共四趟全屏光照：map 清到远平面 ⇒ 阳光到达（64,32,26）、清到 1.0 ⇒ 只剩环境（13,6,26）、无 map 的变体 ⇒ 与前者同值、 给了 map 而程序不读 ⇒ 照画 + 一次上报。写入者的法线附件 alpha = 材料的 shininess/256（引擎的 G-buffer 约定；本夹具的材料 shininess = 0）——这是下一条的探针。
* 写死 `blendEnable = VK_TRUE`（"这个引擎混合恒开"——L2 每顶点 opacity 路径的规则）， 而混合的**使能**是这条命令投递的、bake 的常量不再决定 ⇒ 多附件 pass 的每个附件都被 自己的 alpha 缩放。L2 早就在自己的 G-buffer 上量过同一件事并写下了规则 （`applyOpaqueBlendForAttachments`："法线附件带 shininess/256（≈0.125），混合把存下 的法线缩到 12.5%"），新路径的命令把这个缺陷重新造了一遍。修法：`color_attachments > 1` ⇒ 全部 `blendEnable = VK_FALSE`、因子 ONE/ZERO（与 L2 那条规则逐位一致）；单附件 仍恒开（opacity 路径）。无设备用例 `DynamicStateTest.SeveralColourAttachmentsAreDeliveredUnblended` 钉住两侧， `ContentDrawTest` 里"混合恒开"的旧断言改为单附件规则。判据的强度：本夹具 shininess = 0 ⇒ 混合会把法线**整体抹掉**（透明黑底 × 0），阳光消失——变异 M5 把规则退回 `> 4` 时像素直接回到 (13,6,26)。
* **另一个坑（夹具侧，写进 memory）**：`vn::math::Mat4d` 默认构造 = **单位阵**（不是
* **这一片留下的口子**：材质贴图的 GPU 侧（缓存 + 上传 + 立方体视图）→ 变体的 define 进
* M1 声明的采样器放到"下一个空位"（就是那片已量过的缺陷） — 无设备用例红 + 像素红（按名拒绝 ⇒ 用例的断言）
* M4 块描述符只认动态 UBO（静态布局下没人填） — 像素红（按名拒绝 ⇒ 用例的断言）

### 11.16al M8d-1（2026-09-22）：材质贴图的 GPU 侧——缓存、上传、立方体视图
* **为什么必须在这一层**：引擎的 `Texture` 是**逻辑描述**（种类、尺寸、格式、mip 数、逐面的 `imaging::Image`
* staging 字节链（**mip-major 交错**：vsg 的拷贝区域按"一级里的所有层连续"读，引擎的存储是"一层里所有级连续"， 多层纹理必须在这里交织——错了是**静默**的，字节总数一样、每个拷贝区域都合法，只是每个面采到别人的数据）， 用**一个 texel 宽**的元素类型把它包成 `vsg::Data`（元素宽 = stride，vsg 的步进就是一片 face 的大小）， 数组的**维数 = 层数**（`TransferTask` 对 cube 从 DATA 的 depth 取 arrayLayers），cube 的 `properties.imageViewType = CUBE`（ImageView 的构造从 DATA 读它，事后赋 `view->viewType` 会被覆盖）， `flags |= CUBE_COMPATIBLE`。数据背书的 image 由 viewer 的传输步上传（`RecordAndSubmitTask::submit` 里 `transferData(TRANSFER_BEFORE_RECORD_TRAVERSAL)`）——**测试的手动驱动也走这条**，所以不需要额外的上传节点 （§11.16ai 记的"`requiresDataCopy` 无人消费"指的是那个遗留标志；真正生效的是 `Data::dirty` 那套）。
* * **变异脚本的恢复断言会误伤**：M4 的还原串 `return white();` 在文件里出现 4 次 ⇒ 断言失败、脚本中止、文件留在
* **变异态**（本片的 M4 就是这样，手动还原后才继续）。规则：还原串必须带**唯一上下文**，或者先存原文再整段写回。
* **这一片留下的口子**：变体的 define 进管线身份（`VINE_DIFFUSE_MAP` 与 texcoord kind 取决于几何与材质——

### 11.16am M8d-2（2026-09-22）：程序的文本是好几份程序——变体的 define 进管线身份
* "文本这次真的声明了什么"，`compileStage` 把它交给 vsg 的 `compiler.compile(stage, defines)`。两者读同一份 `out.shaders.defines`，所以布局与模块不可能对不上（M5 变异证明扫描那一侧也吃 define：没有它，带贴图的 程序扫不出 (0,1) 的采样器）；
* **这一片留下的口子**：pass 的**输入集合（set 1）仍按第一个 content half 的层构建**——设计上写在
* 由文本写死；开关取什么值，由 drawable 的**事实**决定。
* **pass 侧的判断**：`recordCommand` 先查材质（变体的输入之一），由**事实**算出
* `variantOf` 于是永远说"没贴图"，无设备用例全绿、只有真设备用例的打印看得见 （`textured.diffuse_map=0`）。事实结构体在函数尾部整体赋值时，任何"提前赋值"都是死代码。
* M5 事实扫描丢 define（`shaders.defines.clear()`） — 引擎逐变体用例红 + 真设备用例红（ABI 里没有采样器）

### 11.16an M8e（2026-09-22）：三张表的生产侧——计划说要什么，修订说何时重建，时间线说何时退役
* 3. **被顶替的修订留在表里**：点过 3 的帧可能还被重录（重建臂），所以 3 必须一直能答——直到**还可能记录它的
* 槽**过去为止（与 GPU 对象同一窗口、同一个 `RetirementQueue`）。做法是**追加**而不是替换：表里短暂地同时 有 3 和 4，3 在停靠到期时连同它的存储一起离场。**材质是例外**：它按身份查（计划不能点材质修订），表必须 回答"现在的材质"，所以编辑**就地替换**那一行，只把旧值（facts + 它的块存储）停靠起来。
* 带 `variant`，`findProgram(…, variant)` 按三者匹配，`recordCommand` 先由材料+几何算变体、再取条目—— 这样命令拿到的 `abi`（含 **push 范围**）就是它自己那份文本声明的。变异 M1/M5 证明：忽略变体时，要么 真设备用例的左半只剩清屏色（贴图命令拿到"没有 push"的 ABI，矩阵从没写过），要么半片条目断言红。
* * **查找必须扫全表**：表里现在可能有被顶替的修订，`findGeometry`/`findProgram` 原先"第一个同身份条目修订
* **这一片留下的口子**：①**半片的生产侧**——表有了，管线层与 `Scope::Entry` 仍由宿主自己建（用例里就是
* 事实（画面错，且是静默的）。所以每个被 track 的对象由 store 持一份份额；`releaseAbandoned()` 丢"只剩 存储自己"的那些，把它们的行按同一窗口停靠。没有停靠窗口（会话没探到在飞槽数）时**留下并计数** （`retained()`）：内存有代价、正确性没有。停靠的闭包持 `weak_ptr<Data>`——store 先死则释放无事发生， 队列先死则行按时离场，两个方向都不悬空。
* "由表建层"那段循环），那才是下一步；②屏幕路径的程序条目**不带变体**（全屏 ABI 是引擎的、今天的全屏程序 也门控不了什么，但用户片元文本若按 define 变脸，这里要补）；③M8d-2 记的输入集合仍按第一个 content half 建。

### 11.16ao M8f（2026-09-22）：半片的生产侧——表与几何产出可录制的半片
* 是 scope 的事，本片的真设备用例仍自己按半片的 `abi` 建它）；也**不**清扫"再也没被画到"的程序留下的半片 （它的键还答得出，就留着——见下面的口子）。
* * **替换文本的搜索必须限定起点**：`s.index(marker)` 从 0 找起，而同一段文本在文件更早的用例里也有
* **这一片留下的口子**：①**逐 drawable 的声明集合**——同一变体、不同贴图的两个 drawable 今天会撞同一套
* 1. **走 pass，做与录制器相同的查找**：几何按计划点名的修订、材质按身份、变体由这两条事实决定（engine 的规则）、
* （`serveHalf` 认领集合只比 set 序号 + 形状 + 采样器绑定号，不比**装的是哪张图**）⇒ 下一步；②"再也没被画到" 的程序留下的半片没有清扫（键还答得出就留着，容量上界只能靠表的退役）；③M8d-2 记的输入集合仍按第一个 content half 建。

### 11.16ap M8g（2026-09-22）：一个 drawable 的图，一个集合
* 没有 drawable 的图 ⇒ 服务任何 drawable）。**这条回落是既有调用的活路**：M8d-1/M8f 的用例建的正是这种 集合（M5 变异把它删掉 ⇒ 那些用例红）。
* **顺手记一个 C++ 坑**：`ImageSource` 是 `BlockDescriptors` 的**嵌套类型**，而它的默认参数写作 `= {}`——嵌套类型
* M2 pass 不要图（demand 恒空） — 新用例红（打标的集合一套都匹配不上 ⇒ 拒绝 ⇒ 清屏色）
* **这一片留下的口子**：①**集合的生产侧**——谁按 (程序变体 × 材质纹理修订) 建集合、并在纹理重填后把旧集合停靠，
* 2. **pass 由 drawable 的事实算它要的那对**：材质的 `texture` 指针 + **实时**读到的 `Texture::revision()`
* 今天还是调用方手写（下一步）；②"图来源"只覆盖 `diffuseMap` 一类**材质图**：`shadow_map`/输入图是 pass 级的， 两次 pass 用不同影子图时集合各建各的（调用方 per-pass ✓），但没有一条规则**强制**它；③M8d-2 记的输入集合仍按 第一个 content half 建。

### 11.16aq M8h（2026-09-22）：声明集合的生产侧
* **这一片留下的口子**：①**帧装配的收口**——表（`ContentStore`）、半片（`ContentHalves`）、集合（`ContentSets`）、

### 11.16ar M8i（2026-09-22）：一帧收成两次调用
* 静默跳过 ⇒ 门禁 12 条 VUID（`07621`/`07627`/`10862`）。这正是 M4a 记过的坑，三个月后在新的调用链上原地复现—— 说明"入口点"这条依赖最好由**知道设备的层**（assembly，而不是每个宿主）满足：现在它的构造签名就把这件事钉死了。
* **这一片留下的口子**：①**提交失败接缝 / 重建臂**——"重录一帧"还不是一条真实路径（它一落地，集合/半片/表的停靠
* 至此每一块都有自己的所有者：`ContentStore` 回答计划点到名的事实、`ContentHalves` 编译 pass 会问的半片、 `ContentSets` 建它们的声明集合、`ContentPass` 录制——而**把它们串起来的那段循环**（走 pass、把上一块的答案交给 下一块、开帧的块预算、每个 pass 一个注册表）每个调用方都要写一遍。`api/ContentAssembly` 就是那段循环：

### 11.16as M8j（2026-09-22）：丢掉的提交，下一帧修一次
* `OffscreenTarget` 的"内容不可信"事实（`attachments_invalidated`）与它的修复路径（计划答 Repair(Bootstrap)、 第一个 bootstrap pass 清标志）早已就位，但**只有测试手调 `invalidateAttachments()`**。这一片补上那道缝： `VsgExecutor::noteLostSubmission(frame)` —— 把"这一帧的提交没发生"变成"它写过的那些离屏目标的内容不可信"： 只标**该帧 pass 真正点到的**离屏目标（没写的目标不动、默认帧缓冲没有我们的附件 ✓），下一份计划的对应 pass `bootstrap == true`（用例断言），第一个 bootstrap pass 清标志（用例断言），第二帧起回到普通计划。 证据：真设备用例（记录一帧 → noteLostSubmission == 1 → 下一帧的计划 bootstrap ✓ → 记录后标志清 ✓ → 另一注册目标始终未被标 ✓）、0 VUID；变异 2/2（不标 / 清成 true ⇒ 红；另一次尝试的变异是空操作，不计）。 门禁 637 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 全清。

### 11.16at M8k（2026-09-22）：提交这一步自己说它失败了
* **不在这层重试**：要不要再来一帧是宿主/会话的决定，而标记让重试变正确——下一份**编译出的**计划答 Repair(Bootstrap)（用例断言钉住）。
* 顺手两件：①`vsg::Exception` **不是** `std::exception`（纯结构体 message + VkResult），只 catch `std::exception` 会让故障直接穿过去—— 变异 M3（只留 std::exception，去掉 vsg 分支与 `catch (...)`）红，且 gtest 打出的是 `Unknown C++ exception thrown in the test body.`（这条教训自己会说话）； ②SDK 的分类表按它自己写明的规则（"Append a new value before Count"）在 `Count` 前追加 `SubmissionFailed`——宿主得能按类别分辨"这一帧没提交"与"后端没起来"。
* 证据：真设备用例（两个目标；帧 1 经 `submit` 提交成功 ⇒ 清屏色真的在像素里（alpha=255）、没有标记；帧 2 的提交在记录步抛 `vsg::Exception` ⇒ `submit` 答 false、**只有它写过的**目标被标、另一目标不动、`SubmissionFailed` 计数 +1；帧 3 的计划 `bootstrap == true`）、0 VUID； 变异 4/4 红（不标 / 答 true / 标到别的目标上 / 不报）+ M3 也红。门禁 638 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 848 文件。
* **这一片留下的口子**：①`Viewer::recordAndSubmit()` 吞掉的最后一层——队列提交返回的 `VkResult`（真·设备丢失正是这样回来的）宿主与会话都看不见；
* M8j 把"提交没发生"变成了目标上的事实，但**说这句话的人还是宿主**：`noteLostSubmission(frame)` 谁来调、调在哪个时刻，是宿主自己的事。这一片把那句话搬进执行器： `VsgExecutor::submit(frame, viewer)` —— **记录 → 提交 → 失败即标记**。任何一个从 `viewer.recordAndSubmit()` 抛出来的异常都意味着这一步没走完： vsg 自己的词表是异常（命令缓冲建不出来就抛 `vsg::Exception`，而 `Viewer::recordAndSubmit` 返回 void、什么都不告诉调用方），而"这一步没走完" ⇒ 该帧写过的离屏目标内容不可信 ⇒ `noteLostSubmission` 标记它们、报一条 `SubmissionFailed`、答 false。宿主从此不需要解释任何失败。

### 11.16au M8l（2026-09-22）：形状变了就是真的重建（Rebuild 臂）
* （那些 pass 是按旧格式建的、`vkCmdBeginRenderPass` 直接点名）、镜像 / 视图 / 回读缓冲 / 帧缓冲 / 图；被保留的是 lease（两向都拒，理由与 `resize` 相同：借用方的帧缓冲点名出借方的镜像）；被**停靠**的是旧附件**和旧渲染通道** （这是 `resize` 不需要、重建必须做的一半）。新附件是刚建的 ⇒ `written=false`、`generation+1`、下一份计划答 `Repair(Bootstrap)`。构建失败（含驱动拒绝帧缓冲时 vsg 抛的 `vsg::Exception`）⇒ 一个字段都不动，目标继续服务旧形状。
* 证据：新设备相位（真设备 + 验证层）——先按旧形状（1 色、8×4）真画一帧，再 `rebuild` 成 **2 色 + D32F、16×12**： 计划答 `Rebuild`、`generation=1`、`written=false`、`passVariantCount()==1`（旧变体随旧 pass 一起走）、 `shape()` / `instance()` 都变成新形状而且 **compatibility 真的不一样**（管线键的判据）、旧图被停靠 （引用计数不变 + `pending()==1` + `deviceWaits()==0`）；第二帧只用新形状的图渲染：附件 0 = 新清屏色、
* 和一个深度；相位行按 `rebuilds_replaced` 计数门禁（门禁相位 9 → 10 行 / 2 次运行）。另加 lease 两向拒绝的真设备用例 （borrower 消失后同一调用重建，且重建后的 lender 能再次出借）+ 次序表的 2 条无设备用例。 变异 5/5 红（次序复原 / 新形状不装 / 事实不重置 / 旧变体留着 / 直接销毁不停靠）+ 一次"不装新 pass"的变异
* `VkRenderPassBeginInfo-renderPass-00904`），像素照过——"兼容性主张像素抓不住"的老教训在新臂上重演。 门禁 639 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 848。
* **这一片留下的口子**：①**谁在每帧应用计划的答案**（`resize` / `rebuild` 今天仍由宿主或测试调用；帧驱动落地时接上，

### 11.16av M8m（2026-09-22）：帧驱动把计划的答案应用到自己持有的目标上
* （计划只带答案）；执行器注册表里有、但这一帧没点名的目标**不动**（相位里的"旁观者"就是这条的判据）。
* **格式变化它看不见**（继续记为口子）。
* 证据：新设备相位（真设备 + 验证层）——同一执行器持有两个目标：帧 1 = 目标的第一个写者（计划答 `Repair(Bootstrap)`， 驱动**不**应用、录制清屏 ⇒ 像素）；帧 2 = 计划答 `ResizeInPlace` ⇒ `applied.resized == 1`、16×12 的像素； 帧 3 = 计划答 `Rebuild`（2 色 + D32F）⇒ `applied.rebuilt == 1`、附件 0 是清屏色、附件 1 透明黑；三帧都经 `executor.submit` 提交；旁观者目标的 `generation`/`width`/`height` 全不动。相位行按 `plan_applied` 门禁（+2）。 另加真设备用例，与既有的"计划与目标不符 ⇒ 拒录"配对：同一份事实（这次说真话、帧点名**第二个**目标）下 `applyTargetPlans` 之后同一帧**录得进去**（`applied.rebuilt == 1`、像素是帧自己的清屏色），计划没点名的目标不动。 变异 4/4 红（Rebuild 不应用 / ResizeInPlace 不应用 / 拿"第一个事实"当答案 / 新形状取 `current` 而不是 `wanted`； 第一次 M4 尝试只改了 extent、在"形状变化"下是空操作，不计）。 门禁 640 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 848、相位 11 行 / 2 次运行。
* **这一片留下的口子**：①`record` 的 shape 核对看不见格式变化；②`Viewer::recordAndSubmit` 吞掉的队列 `VkResult`；

### 11.16aw M8n（2026-09-22）：会话的提交也自己说它失败了
* M8k 让执行器的提交步自己说失败，但**窗口路径的提交在会话里**：`Session::commitFrame()` 调的是 `Viewer::recordAndSubmit()`，而它返回 void、把每个任务的 `VkResult` **丢掉**——队列提交被驱动拒绝（设备丢失 / 内存不够） 会和"这一帧发生了"长得一模一样，宿主连"要不要标这个帧写过的目标"都无从判断。这一片把这一半接上：
* 这个环境里就开始报错（写用例时实测：`vkAcquireNextImageKHR` 的 forward-progress 警告 + 呈现未渲染图像的错误）。 这一层不修 WSI：持续提交失败的会话就是设备没了，宿主要么重建会话（`initialize()`，与移动 surface 同一条路）， 要么结束。用例走的是前者。
* 证据：真设备用例（会自己开窗口）——会话的帧图里放一个"被武装就在记录步抛异常"的节点：`commitFrame()` 答 false、 `SubmissionFailed` +1、`lostFrames()==1`、`framesPresented()` 不动、`submittedFrame()==0`、没有开着的帧；随后 `initialize()` 把会话建回来，一帧正常提交并呈现（`framesPresented()==1`、`submittedFrame()==1`），**0 VUID**。 另加一条无设备时间线用例（`abandoned`：帧结束、水位不动、帧号是提交时钟所以下一个帧接它的号、停靠日期）。 变异：M1 不接 vsg 异常（异常穿出提交）、M2 不结束帧、M3 把丢掉的帧算成提交、M4 不计数、M5 不报告、M6 `abandoned` 也推水位 ——**6/6 红**（各 0 VUID；"帧没结束"那一条的后果就是断言红，不是挂住）；另一次"失败也照样呈现"的变异让**用例挂住** （呈现一张没渲染过的图像，阻塞在 WSI 上），并在阻塞前先吐出 2 条 VUID（`VkPresentInfoKHR-pImageIndices-01430` + `vkAcquireNextImageKHR-surface-07783`）——两种读法都抓得住它，也是"丢帧后 WSI 状态"那条口子的现场。 门禁 642 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849。
* **这一片留下的口子**：①把 M8m/M8n 连起来（窗口路径上"提交失败 ⇒ 标这一帧写过的目标"由帧驱动一次完成）；②`record` 的

### 11.16ax M8o（2026-09-22）：窗口路径上一次调用把"提交失败"变成事实
* 证据：真设备用例（开真窗口）——**两个会话**：会话 A 上经 `commit` 提交一帧（窗口 pass + 离屏 pass）⇒ 答 true、离屏目标 像素 = 清屏色、没有被标；会话 B（丢失的帧会把 swapchain 钉住一张已取未呈的图，所以第二半本来就该跑在重建出来的会话上， 见 §11.16aw）上把记录步做成抛异常 ⇒ `commit` 答 false、`SubmissionFailed` +1、离屏目标被标、`framesPresented` 不动、 `lostFrames()==1`；下一份计划里**写它的那一趟** `bootstrap == true`（窗口那趟不动），把那一趟录进去 ⇒ 标记清掉。 两次运行各 **0 VUID**、0 崩。变异 3/3 红（不标 / 成功也标 / 永远答 true）。
* **写这一片时撞出来的新口子（记在案）**：`assignFrameGraphs` 换的是 viewer 的 record-and-submit task——**上一次提交还在飞**
* 时再 assign（本片用例最初的写法：每帧 assign 一次）会销毁在飞的 fence / semaphore / 命令缓冲：实测 12 条 VUID （`vkDestroyFence/Semaphore/Buffer` 在飞、`vkFreeCommandBuffers` pending）+ 验证层里段错误（不带层时静默通过）。本片用例 因此**每个会话只 assign 一次**；正确做法（assign 前等设备、或复用 task 而不是重建）留给帧驱动收口那片。

### 11.16ay M8p（2026-09-22）：每帧换命令图不再拆掉在飞的机器
* M8o 收尾时撞出来的口子：`SessionContentAccess::assignFrameGraphs` 走的是 viewer 自己的 `assignRecordAndSubmitTaskAndPresentation`，而它**清空并重建** record-and-submit task——task 手里握着在飞提交还点名的 fence / semaphore / 命令缓冲，于是"每帧换图"（这本来就是 `makeFrameGraph` 的用法：一帧一张图）在上一次提交还没回来时 会拆掉它们：实测 12 条 VUID（`vkDestroyFence/Semaphore/Buffer` in use、`vkFreeCommandBuffers` pending）+ 验证层里段错误 （不带层时静默通过）。
* 证据：真设备用例——一帧一张图，**帧帧都在上一帧的提交还在飞时换图**：六个提交（前三个各写一个离屏目标，后三个只写窗口； 后三个是判据的一半：帧 k 会等它进入的那个槽的 fence，所以"后面三帧"正是"前三帧已经做完"的证据）⇒ `framesPresented()==6`、 `lostFrames()==0`、`deviceWaits()==0`、`diagnostics.clean()`、两次运行各 **0 VUID**，且两个离屏目标的像素是**最后换的那张图** 的清屏色（0.4 / 0.6，而不是第一帧的 0.2 / 0.6）。变异 3/3 红：①回到旧行为（viewer 重建 task）⇒ **30 条 VUID**； ②换图不装机（新图不交给 task）⇒ 像素错；③不编译 ⇒ 红。 门禁 644 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、相位 11 行 / 2 次运行。

### 11.16az M8q（2026-09-22）：丢帧的会话自己活下来
* M8n/M8o 的现场留了一个"宿主必须重建会话"的尾巴：丢帧时 acquire 到的那张 swapchain 图像再也不会被呈递，WSI 因此缺一张图， 下一次 acquire 就报 forward-progress 警告、present 报"呈了一张没 acquire 的图"（实测两类 VUID）。这一片把这一层修进会话自己：
* 证据：真设备用例（M8o 的会话 B 扩写）——丢帧（`commit` 答 false、目标被标、`SubmissionFailed` +1）之后**同一个会话**继续驱动： 下一帧（窗口 pass + 离屏 pass）`commit` 答 true，计划里写该目标的那趟 `bootstrap == true`，录进去后标记清掉，随后四个只写窗口的 帧（比槽数多一：帧会等它进入的那个槽的 fence，所以它们是"那一帧已经做完"的证据）⇒ `framesPresented()==5`、`lostFrames()==1`、 `deviceWaits()==1`、`diagnostics.total()` 恰好一条；目标像素 = 这一趟的清屏色。两次运行各 **0 VUID**。 变异 3/3 红：①不重建 ⇒ **10 条 VUID**（acquire 的 forward-progress + present 未 acquire 的图）；②重建但不计数 ⇒ 红； ③每帧都重建 ⇒ 红（本片用例的 `deviceWaits()==0`），且连带给**另外 8 个会话用例**留下失败——"这条路径不该停设备"的判别力 原来就在那儿。门禁 644 用例 / 98 套件、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、相位 11 行 / 2 次运行。

### 11.16ba M8r（2026-09-22）：计划说的形状，连格式一起核对
* `record` 那道"计划与世界必须一致"的检查此前只问两件事：**颜色附件数**与**深度可采样**（§11.16f）。于是同一数目、同一深度、 只有**格式**不同的漂移完全不可见——而格式正是引擎自己的词汇无法分辨的那一半（RGBA8 同时是线性图和 sRGB 面，见 `RenderPassCompatibility` 的实测）。计划本身也**没有携带**形状，连比较的材料都不在。
* 变异 5/5 红：①离屏那半回到只比数目 ⇒ 红（本片用例 + 2）；②窗口那半回到只比数目 ⇒ 红（窗口用例 + 2）；③编译器不抄形状 ⇒
* **本片留下的口子（登记）**：①窗口路径仍没接 `applyTargetPlans`（计划的换尺寸/重建答案对窗口还没有执行者）；②`skyMap` 仍无
* 生产者；③集合与半片停靠窗口各自独立（旧口子不变）；④设备半边在"某一侧没说"时跳过——那是一处**已知的不检查**，不是等价性声明。
* 格式（设备表因此少一项）⇒ 改成"在引擎的词汇里**声明**想要的东西"（设备拼写是目标层对这次请求的回答：不声明是诚实的，半声明 不是）；`SampledInputTest` 的全屏用例把有深度附件的目标说成"只有颜色"（此前这让计划一直答 Rebuild）⇒ 改成目标自己的 `shape()`。 两处都是这条检查存在的理由本身。

### 11.16bb M8s（2026-09-22）：窗口的答案也有执行者——问平台，不采纳计划的主张
* 变异 3/3 红：①把 `refresh()` 的结果取反（把"没变"记成 `rebuilt`）；②`refresh()` 删掉比较、永远答"变了"；③去掉 `action != Rebuild` 的守卫 （对每个窗口答案都跑一次）⇒ 稳定帧的"四计数全 0"红。
* **本片留下的口子（登记）**：①窗口 `facts()` 的 live 采样与 `refresh()` 的**成功臂**今天没有可驱动的触发（宿主换格式被拒、宿主重建走整会话
* Rebuild）——它的可观测形态需要一条"平台真的换了形状"的宿主路径（或一个能重建 surface 的测试缝），那时两者会同时被观测；②`skyMap` 仍无 生产者；③集合与半片停靠窗口各自独立；④设备半边在"某一侧没说"时跳过（M8r 的登记不变）。

### 11.16bc M8t（2026-09-23）：`skyMap` 是 drawable 自己的图——引擎自带天空程序落地
* 忘了 `{}`——`ContentSets` 里"这次 drawable 要哪张图"的局部量与 `ContentPass` 里的 `demanded`。结果：**没贴图的材质会被按上一个 drawable 的纹理归档**（栈上的残留值）。实测形态：天空盒用例里"无图"那条的集合根本没建成（键撞上前一个 drawable 的图），pass 随后 按"材质要的图不在调用方的集合里"拒绘。两处都改成 `{}` 并写明理由。
* 无设备行同步更新：名字表（`skyMap` ⇒ Material）、cube/2D 的 `skyMap` 层建得起来、全屏 cube 声明被拒。变异 5/5 红：①名字表不认 `skyMap`；②fallback 不看声明种类（**6 条 VUID**）；③不一致不检查（**2 条 VUID**）；④`ImageSource` 又未初始化（复现上面的缺陷）； ⑤全屏 cube 声明放行。门禁 **650 用例 / 98 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 849、相位 11 行 / 2 次运行。
* **本片留下的口子（登记）**：①天空"跟随相机/当作无穷远"是宿主的事（demo 的盒子是静态的，内容 push ABI 里没有视图旋转）；②集合与
* 半片停靠窗口各自独立、窗口 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"（皆是旧口子）。
* M8c-3 把 `skyMap` 读成"帧级环境"并在建层处拒掉，理由是"环境图没落地"。这一片纠正这个名字：**`skyMap` 就是 drawable 自己的图**， 证据在引擎自己的文本里——`builtin_skybox.frag` 写着 "The material's texture, at the binding every content program samples it from (set 0 / binding 1, the ABI's `diffuseMap` slot)"，`AppShellDemo` 的天空盒也是把自己的 cube 放进 material 再挂 `skyboxProgram()`。所以 `imageOriginOf("skyMap") == Material`（与 `diffuseMap` 同行），`ImageOrigin::Environment` 这一行、 `describeAbi` 的拒绝、`ContentSets` 的那条分支一起删除：**引擎自带的天空程序从"建层即拒"变成能画**。

### 11.16bd M9a（2026-09-23）：门面立起来——SDK 的方法落到会话与帧驱动上
* 图**——gtest 全绿，验证层却给每条呈递一条 **VUID 01430**（presented image is in `VK_IMAGE_LAYOUT_UNDEFINED`，实测 16 行）。原因不在 acquire：临时打进 `Session::beginFrame/commitFrame` 的打印证明 imageIndex 在 0/1/2 之间轮转、 尺寸公告（640x360 → 320x180）真的生效。**把图像搬出 `UNDEFINED` 的是窗口那棵 render graph**——会话自己那张初始 化期的图里就有它（所以会话自己的空帧路径从来干净），空命令图里什么都没有。修法：**计划为空时不换图**——直接 `commit` 会话自己的那张（`Session::commitFrame`）；有 pass 的帧再照 M8q/M8o 的驱动形状自建图（窗口那趟由 `recordWindow` 把窗口图放进新图里）。这不是"测试口味"，是一条不变量：**acquire 过的图必须有一趟 render pass 走 过它，才轮得到呈递**。
* 变异 4/4 红：①空帧不呈递（不 commit）⇒ 5 行红 + 6 VUID；②又交空图（复现上面的问题）⇒ **16 VUID**（gtest 0 行）； ③"未服务"的报一声不响 ⇒ 3 行红；④`supportsRenderTargets()` 说谎（答 true）⇒ 3 行红。门禁 **652 用例 / 99 套件**、 0 VUID / 0 SYNC-HAZARD、hygiene 0 / 852、相位 11 行 / 2 次运行。
* **本片留下的口子（登记）**：①门面今天的计划永远是空的（画的一半全部拒绝），所以"有 pass 的帧"那条驱动形状要等
* 门面第二片才真被用过；②活着的 `resize` 只报不改（会话 swapchain 的形状只有平台能换采样）；③集合与半片停靠窗口 各自独立、窗口 `facts()` 的 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"（皆是旧口子）。
* * **表面事实**：`setWindowHandle()` 在 `initialize()` 前公告，采纳后 `nativeHandle()` 答的是宿主那个；`resize()` 是

### 11.16be M9b（2026-09-23）：pass 协议接到内容层——宿主经 SDK 真的画出内容
* 变异 **7/7 红**：①帧首不换窗口内容（复现累积缺陷）；②不组装内容（只剩清屏）；③`beginPass` 不开范围（计划 里没有 pass）；④清屏色不翻译；⑤命令对象不追踪（表回答不了 ⇒ 报告 + 拒录）；⑥`releasePass` 不留身份；⑦ warm-up 的调用进了录制器（帧外调用变成一串拒判）。门禁 **655 用例 / 100 套件**、0 VUID / 0 SYNC-HAZARD、 hygiene 0 / 856、相位 11 行 / 2 次运行。
* **本片留下的口子（登记）**：①离屏目标 / pass 输入 / 全屏程序 / 读回仍报"未服务"（下一片）；②门面仍不接
* `MaterialManager` 风格的材质编辑通知——`ContentStore::updateMaterial()` 存在但 SDK 的 `RenderBackend` 面里没有 入口，等**工厂切换**时由宿主侧接线（登记）；③活着的 `resize` 只报不改；④集合与半片停靠窗口各自独立、窗口 `facts()` 的 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"（皆是旧口子）。
* 原样折算（不套传递函数——测试读出窗口时的约定），深度只带标志（SDK 没有深度值可给，给一个就是第二处会搞错 裁剪约定的地方）。
* 且有绘制调用的 pass 产出一个内容包——按**声明的输入**数给等长的空 `InputImages`（内容录制器按声明逐项读， 短一截会读过界；"没人提供"是它自己会报的事实），视图块按该趟第一个绘制调用的相机 + `Session::frameSeconds()` + 窗口的实时尺寸建，兼容性取窗口的形状；离屏 pass 不产包（执行器会报它，内容给了也没人录）。报告之后**内容 替换**由 M9b 的缺陷修复负责（见下）。

### 11.16bf M9c（2026-09-23）：离屏那一半——目标、输入、全屏、重建
* M9b 让宿主经 SDK 画出**窗口**的内容；这一片把**离屏**那一半接上：`supportsRenderTargets()` 转真，宿主的 `RenderTarget` 被持有、被画、被当输入采样，并且**形状变化真的触发重建**。至此门面的 SDK 面只剩读回与"活着的 resize"。
* 建的时候**才说（形状变了就没有"设备的拼写"可说——"不知道"不能读成"没有"，M8r 的规矩）；深度的事实：建成后 promotion 按**目标自己的 `layout()`** 报（重建保留的是它自己那份），借来的深度 `borrowed` + `source`，且
* `shareDepth` 自己保活源对象，这边是配套的另一半：lender 被 `releaseRenderTarget()` 释放时，注册表丢掉它的条目， 但 borrower 还把它撑着，图像不会在飞行中被销毁。lender 必须**先被公告**（SDK 的调用序契约就是"源在同一帧更早 渲染"），所以 lender 的解析发生在**公告的那一刻**，不是事后去找。
* 只报 `DepthSourceMissing`/`BuildFailed` 一次（`Ready` 后 `rearm()`——同一个毛病再犯还报），`NotBuilt` **静默** （宿主先配后画；真需要的 pass 由执行器报"没告诉过我这个目标"）。`endFrame()` 的事实表在窗口之后**逐条目补 行**（建没建都算——计划要能说"这个还没有对象"，而且编译器解析 **pass 的输入**用的就是同一张表）。`setPassInputs()` 把每个非空输入 `observe()`（刷新描述——对象是它被画进去时建的，要新的是**事实**）后整表转交。`drawScreenProgram()` 追踪它的**片元程序**（表要回答它的两份文本）后转交；源是**身份**，哪张图在 `setPassInputs` 那一步说过。 `releaseRenderTarget()` 忘条目 + 注销执行器（那是借来的指针，绝不能悬）+ 让 recorder 丢掉挂起的公告。
* 自己的清屏绿**（不是窗口的清屏蓝），左/右/角落三处都对（外围两字节按**集合**读：这个服务器发的是 BGRA 序， 红在最后一个字节——测试自己的坑，不是后端的）；
* 变异 **7/7 红**：①宿主目标永不建；②离屏目标不进事实表（编译器解析不了输入）；③`setRenderTarget` 丢身份 （**12 条 VUID**——同一帧两次把窗口当目标）；④离屏趟不录内容（**2 条 VUID**：屏上复制一张没写过的图）； ⑤输入不给图；⑥释放不清（用例数一下 `live()`）；⑦形状变了还说旧的设备格式（**2 条 VUID**：管线与 render pass 不再相容）。门禁 **658 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 859、相位 11 行 / 2 次运行。
* **本片留下的口子（登记）**：①**读回**仍是"未服务"（下一片：capture 的录制策略 + 一次被计数的设备等待 + 字节
* 取出）；②**借了深度的目标**（以及有 borrower 的 lender）会被 `OffscreenTarget::rebuild`/`resize` 的租约拒绝—— 宿主的 `shareDepth` 组合要保持同尺寸，形状变了得先释放再重建（写在这里，免得当成"重建坏了"）；③活着的目标上 翻转 `depthPromotion` 不会被应用（目标的 `layout()` 说了算，事实跟着目标走——要变就得释放重建）；④活着的 `resize` 只报不改；⑤集合与半片停靠窗口各自独立、窗口 live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧 没说就跳过"（皆是旧口子）。
* 无设备侧 `HostTargetsTest`：描述是快照（对象改了、事实不动，直到再次公告）、`NotBuilt`、借来的深度不是 borrower 的承诺、shadow 声明原样旅行、释放只答一次。

### 11.16bg M9d（2026-09-23）：宿主的读回——两个来源一张图
* pass 都没有**时，执行器只录了宿主目标的命令——被 acquire 的窗口图像从头到尾没跑过 render pass，停在 `UNDEFINED`，呈递就是 **VUID 01430**（实测 **2 行**，gtest 全绿）。修法两半： `WindowTarget::prepareWithoutClear()`（**只重开渲染区**，清屏值保持原样——它补的是"图像被写过"这一步， 不是画的新一帧）+ 执行器在**没有任何窗口 pass** 的帧尾把窗口图**不加壳**挂进命令图（**没有 pass 可以给 它归因**，注释里写明这是呈递需要的，不是内容）。
* 清屏绿 `(0,64,0,255)`、alpha 255（**离屏读回是 RGBA 序**——窗口面那个 BGRA 是测试服务器的坑，见 M9c）；
* 变异 **6/6 红**：①没画过的目标读起来像能服务（3 行）；②只有离屏的帧不跑窗口图（**2 条 VUID**——用例 抓不住、门禁抓住）；③**两道"没录过"的闸一起拆**（3 行；只拆一道是**绿**的——分类那一道先答，说明两道闸 在**不同来源**上各管一半：帧拷过的附件本来就不需要"录过"，要靠一提交路径上的那道闸拦）；④借来的深度从 borrower 读（3 行）；⑤读回的停不计（3 行）；⑥帧不把自己的目标拷回来（**63 行**：执行器与相位一起红）。 门禁 **659 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次运行。
* **本片留下的口子（登记）**：①活着的 `resize` 只报不改（下一片可选）；②借了深度的目标、以及有 borrower
* 的 lender 会被 `OffscreenTarget::rebuild`/`resize` 的租约拒绝（M9c 的登记，不变）；③一个**帧没拷过**的 附件上的读回会走一次提交路径——真要"零额外提交"的宿主自己保证先画过（这正是 `NotRecorded` 的意义）； ④旧口子（集合/半片独立、live 采样与 `refresh()` 成功臂不可驱动、设备半边"某一侧没说就跳过"）不变。

### 11.16bh M9e（2026-09-23）：活着的 resize——跟随表面
* 尺寸**，所以活着的一条尺寸公告不是命令，而是"跟随"：`SessionContentAccess::followResizedSurface` 让窗口 做它自己的那件事（`window->resize()`：重读所在表面的几何 + 重建交换链），挂在尺寸上的东西**下一帧从窗口 现读**（`WindowTarget::prepare` / `prepareWithoutClear` 每帧写 `renderArea`、目标形状、每趟 pass 的视图 块），所以没有第二份尺寸要同步。重建**停一次设备**（vsg 的 `buildSwapchain()` 先 `vkDeviceWaitIdle` 再销毁 旧交换链——与 M8q 丢帧修复同一笔开销），这次停**被计数**：活着 resize 是宿主的例外，计数器让它保持可见。 公告的数字仍是**下次 `initialize()` 建窗的尺寸**；自开窗口那一半"应用公告"没做（它没有窗口系统事件、也 没有别人能改它——旧实现同一口径；登记）。
* **本片量到的真机理（M1 变异顺带抓到）**：表面变了却不公告（或后端不跟随）时，vsg 的 **Viewer 自己**会在
* acquire 发现 `_extent2D` 与交换链不符（`Window::acquireNextImage` 直接答 `OUT_OF_DATE`），于是在**提交里** 调 `window->resize()` 重建——可那一帧**已经按旧矩形录完了**：`VUID-VkRenderPassBeginInfo-pNext-02852/02853` （render area 128 > framebuffer 96，实测 4 行）+ 段错误。跟随发生在**录制之前**，这正是这一片存在的理由。
* 变异 **5/5 红**：①**不重读表面**（不重建交换链）⇒ **4 条 VUID（02852/02853）+ 段错误**（上面那条机理）； ②重建发生了但**停不计数**（3 行红）；③**活得公告不记**（下次 `initialize()` 回到默认 640×360，3 行红）； ④ `0×0` 闸拆掉（3 行红）；⑤**渲染区冻在第一个尺寸**（4 条 VUID + 段错误，红）。门禁 **660 用例 / 101 套件**、 0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次运行。
* `vkCreateSwapchainKHR` 收到**未初始化的表面能力**（`preTransform` 垃圾位、`imageExtent` 宽 1891005984、 `VkSwapchainCreateInfoKHR-*` 共 10 条 VUID）随后抛异常——那是 `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` 失败的症状；**同一棵树**再跑即绿（`SessionTest` 单独跑 5/5、去掉本片用例全量跑 4/4 绿），而且 `SessionTest` 在二进制里**跑在本片用例之前**，与本片无因果。登记为环境级偶发（X/lavapipe），下一次见到先重跑再查。
* **本片留下的口子（登记）**：①**自开窗口不"应用"公告**（没有窗口系统事件能改它，也没有它的主人：公告仍
* 在下次 `initialize()` 生效——SDK 那句 "owns its surface ⇒ applies the announcement" 在这一半未实现）； ②宿主**不公告**表面变化时不会被跟（vsg 会在提交里补，代价是那一帧错 + VUID/崩）——不能报尺寸的宿主要自己 保证公告；③旧口子（借来的深度租约、活目标翻 `depthPromotion`、集合/半片独立、live 采样与 `refresh()` 成功 臂、设备半边"某一侧没说就跳过"）不变。

### 11.16bi M9f（2026-09-23）：工厂切到门面——"vsg" 就是重写版
* 最后一步：`VsgRenderBackendFactory::create()` 不再造 `VsgRenderer`，而是造 `vn::vsg::VsgBackend`（门面）。 从此**注册名 "vsg" 的含义就是重写版**——插件 load → 注册表 → `create()` 这条生产路径上跑的是这一路 M0–M9e 建起来的东西；被替换的实现仍在树里、仍由自己的测试驱动，但**没有任何名字会创建它**（第二个名字就是"哪个是 vsg"的第二个答案，而这正是重写要消灭的东西）。注册实际有**两条路**：`GfxBackendVsgPlugin::load()` 显式注册 一个 `s_factory`，而 `VsgRenderBackendFactory.cpp` 里的静态 `Registrar` 在模块被载入时就自己注册了同一个名 字（后注册的赢，两者造出的东西一样）——变异 M3 就是把**两处一起**关掉，只关一处处仍然绿。
* **切换逼出来的半片（必须一起交）**：老实现**每帧**刷新它命令到的每个材质（`SceneBridge` 对每个 distinct
* material 调 `VsgMaterialManager::updateMaterial`，而那个方法**自己就是 compare-and-write**——它的测试原话 是 "refreshes in place, and only when something changed"）。重写版是**拉**模型：`ensureMaterial` 只在 `described_revision == revision` 时跳过，而 `revision` 只有 `updateMaterial()` 会推——**而重写版里没有任何人 调它**（§11.16be 登记的就是这一条：SDK 的 `RenderBackend` 面里没有"材质变了"的入口，引擎也不持有 `MaterialManager`）。不补的话，切完工厂**宿主的材质编辑就不进画**（老实现的核心行为之一，静默丢）。所以本片 把两半一起交：
* "没变"报成"变了"，§11.16k 的老坑），只有**不同**才推修订，于是"下一帧 `tablesFor` 换掉那一行、把旧值停 靠"照旧；稳态帧只比不建、**不分配**，首帧之后的每帧开销就是几个浮点比较。块的字段映射收成一处 （`blockOfMaterial`），`buildMaterialFacts` 与比较共用同一份拼写。
* **夹具教训（当场抓到的）**：块的 canonical 绑定是 **View=0 / Draw=1 / Material=2**；把 `VineMaterialBlock`
* **变异 6/6 红**：①名字又造旧实现（`CreateBackendByName` 的 cast 先红）；②`create()` 答空；③**两条注册
* **变异夹具的一课**：插件**独有**的源（工厂、插件入口、旧实现）**不在 `test_vsg` 目标里**，所以
* `ninja -C build test_vsg` 不会重编它们——第一次跑这六个变异**全绿**，就是因为被测的仍是**旧的插件 .so**。 电池必须 `ninja -C build test_vsg gfx_backend_vsg`（插件的 api/core 源同时编进测试目标，所以 M9e 的电池没 踩到这一条）。已写进仓库记忆。
* **本片留下的口子（登记）**：①旧实现仍在树里（测试驱动）——删它是独立一步（要连它的源文件、公共头与测试
* 一起处理）；②引擎侧真的跑起来（真窗口、真场景）没有在本片里做过：证据是"注册表 → 门面 → 宿主窗口一帧"这 条路径，**宿主侧的接线**（怎么把场景/材质编辑接到这个后端）属于引擎那边；③旧口子（自开窗口不"应用"公告、 宿主不公告表面变化不会被跟、借来的深度租约、活目标翻 `depthPromotion`、集合/半片独立、live 采样与 `refresh()` 成功臂、设备半边"某一侧没说就跳过"）不变。
* * 门面 `render()` 对**每条命令的材质**调一次 `ContentStore::updateMaterial()`——"在哪被点名，就在哪被注意
* `BackendContentAccess::store()` 作为测试视图（`builds()` 是"这一帧建了东西"的读数，与上一片的 `windowTarget()` 同一个理由）。

### 11.16bj M10a（2026-09-23）：引擎侧冒烟——真应用跑起来，把剩下的口子钉成清单
* 工厂切完之后没人验证过**真应用**。这一片就是那次冒烟：`build/bin/Vine`（默认 Deferred + 阴影的演示场景）在 lavapipe + X11 上跑起来，用 `scripts/xwin2ppm.py` 读它自己的窗口——**画面是真的**：378×247、100% 非近黑、均值 (118,121,127)，天空底、红色平台、紫盒、坐标轴 gizmo、G-buffer 预览、右侧红箱、HUD 都在。那一次运行同时把后端 在**真实管线**上拒绝的东西摆上了台面：两条当场修掉，两条登记。
* 同一类的"首帧自愈"。登记：要么查明首帧哪一项目标还没就绪，要么像报告那样按情节处理。
* `[VsgHostWindow] attached to the host window 0x…` 给出的窗口 id，`scripts/xwin2ppm.py <id>` 读像素、 `ppm2png.py` 转图）；②`VsgBackendTest.ADepthOnlyTargetIsHeldBuiltAndOfferedAsASampledInput`（真设备： depth-only 目标建成、0 彩色附件、`depthView()` 非空，`HostTargets::facts` 行说 `promotion`、`core::depthPlan` 答 sampleable）；③第一个与离屏两个用例的改写（缺 lender 帧外沉默、帧内报一次，且报文里含 `'composite'` 与 `'gbuffer'`）。变异 **3/3 红**：①depth-only 又被拒（新用例红）；②缺 lender 在帧外也报（离屏用例红）；③报文不再 指名（离屏用例红）。门禁 **664 用例 / 101 套件**、0 VUID / 0 SYNC-HAZARD、hygiene 0 / 861、相位 11 行 / 2 次运行。

### 11.16bk M10b（2026-09-23）：非索引 + 拓扑进管线——`star_cloud` 画出来了
* M10a 登记的第一条：演示的点云没有索引，而重写版把"有索引"当成可绘制的前提 ⇒ 那条命令从不进图（每次录到它报一次 "the command's geometry is not drawn: the content layer was never told about this identity"）。这一片把它补上。 两件事必须同时成立才有那张画，所以它们被钉在同一个用例里。
* 新增 `GeometryFacts::vertex_count` 承载"非索引时画几个顶点"（位置流自己的顶点数——SDK 的 `Geometry::vertexCount()` 口径）；`buildGeometryFacts` 对无索引的几何**描述**而不是拒绝；`channelsMatchLayout` 检查的是**从哪条流组装**： 有索引 ⇒ 必须是 Index 流（kind 就是模式），无索引 ⇒ 顶点数大于 0（载荷在不在是上传层的事，不是这条检查的事）。 录制侧：`ContentDraw::Draw::index` 允许为空、`vertex_count` 决定 `vsg::Draw`（`vkCmdDraw`）还是 `vsg::DrawIndexed`；
* **变异 4/4 红**：①管线恒烤 TRIANGLE_LIST ⇒ 拓扑烤制用例红；②`buildGeometryFacts` 再拒非索引 ⇒ 两个事实用例 +
* **顺带量到、本片不修（登记）**：①应用带验证层跑仍有**先前就存在**的三类 VUID（20 条 `vkUpdateDescriptorSets-None-03047`、
* 点云用例红；③非索引也记成 `DrawIndexed` ⇒ 两个用例红（"是 `vsg::Draw`"与"没有索引绑定"两条断言）；④半片匹配丢掉 拓扑比较 ⇒ 点云用例的拒绝阶段红（三角形半片把 POINTS 命令画了）。`test_vsg` **670 用例 / 101 套件**全绿（+6）， `scripts/vsg_rewrite_gate.sh`：**0 VUID / 0 SYNC-HAZARD**、skipped=0、hygiene 0 / 861、相位 11 行 / 2 次运行。

### 11.16bl M10c（2026-09-23）：默认演示的三处回归——租约死锁、全屏混合、输入集归属
* `composite`，而 `composite` **借** `gbuffer` 的深度（SDK 契约：一张深度图两个目标）。重写版里 `OffscreenTarget::resize` 对**任何**持有/借出深度租约的目标一律拒绝（怕借方的 framebuffer 指到刚被换掉的图上），而计划侧的 `ResizeInPlace` 每帧都在问——于是两个目标互相卡死：日志每帧两行 `apply target=… wanted=378x247 current=160x160`，帧**一直是**建目标时的 160×160，只有窗口那一角在画。修法是三件事同时到位：①`applyTargetPlans` 按**出借方先、借方后**的顺序应用（用事实表的 `depth.borrowed`/`depth.source` 做选择排序，借方里"自己也有应用"的那些跳过）；②`resize` 的拒绝收窄成真正的 API 约束 （借方的 framebuffer 附件不得**大于**出借方当前的图，VUID-VkFramebufferCreateInfo-pAttachments-00861——出借方自己可以 换，借方换了之后必须由调用者在**同一帧**内跟随）；③新增 `OffscreenTarget::repointBorrowedDepth`：只换借方的 framebuffer （**颜色附件不动**，已经规划好的 load-op 因此仍然成立），旧 framebuffer 交退役队列托管，并是一次性的（同一张图不会重复 跟随）。另外"计划没应用"以前是**静默**的——现在 `repo
* blending 是**关**的；重写版给了它们所有 draw 同一份 SRC_ALPHA/ONE_MINUS_SRC_ALPHA 动态状态。附件 1..3 在延迟光照那帧 是**清成透明黑**的（它们只在 gbuffer pass 里被写），叠一次就把目标乘没了 ⇒ 屏幕上只剩第 1 个预览（albedo 恰好不透明）。 修法：`makeDynamicStateCommand(..., draws_content)`，`ContentDraw::recordScreen` 传 false ⇒ 全屏 draw 一律不混合 （判据写进注释：`color_attachments > 1 || !draws_content`）。
* **顺带量到、本片不修（登记）**：①`ContentPass` 是每帧新建的，它的 `ReportOnce` 插话因此每帧重置**（已由 §11.16ci 收掉）**——"pass 声明了影子图但
* 程序没有 `shadow_map` 采样器"这条警告在延迟链上仍会每帧重复（24 行/帧）；②执行者层的**顺序**目前只有对象层用例 + 演示 画面钉着（`applyTargetPlans` 的选择排序本身没有设备相位用例；相位表加一行要动门禁基线，留作下一步）。**（②已由 §11.16ce 收掉。）**

### 11.16bm M10d（2026-09-23）：resize 时"几何闪、天空不闪"——借来的深度被借方自己清掉
* 那一帧），修后每档**全 0 差异**；把同一电池灌给**老实现**（临时改工厂并当场还原重建）也是 0 差异 ⇒ 修复把重写版拉回老实现的不动点。 ②默认尺寸与"改预览之前"的差异**只落在 y∈[8,97]**（预览带），画面其余部分逐像素相同。③`test_vsg` **670/670 绿**（`BackendCoreTest` 的深度计划用例与 `SharedDepthTest.ABorrowedDepthIsNeverSampleableAndRevokesTheLendersPromotion` 跟着新口径改，后者写明为什么借方的 pass 必须 preserve）。④`VINE_PIPELINE=forward` 的 showcase 正常；hygiene 0/866、诊断格式 0/39、文档符号 0。
* **登记**：①resize 帧仍会报 `vkCmdDraw-None-09600`（"被采样的图仍是 UNDEFINED"）约 2 条/帧；稳态跑是 20×`03047` + 6×`09600` +
* 6×`Viewport-01770`，与**修前日志逐项相同**（核对过），所以那一类是**先于本片**存在的，不是这次改动带出来的；②上一片登记的 "执行者层顺序还没有设备相位用例"仍未补。**（②已由 §11.16ce 收掉。）**

### 11.16bn M10e（2026-09-23）：自查一遍——五个真缺陷（其中两个是上一片自己带出来的）、一条租约语义修正
* `any_pass_preserves_depth` 从来没被填过（`OffscreenTarget::facts()` 里那句才对）。后果：一个**开着 promotion** 的 lender 一旦有 borrower，计划仍说它的深度"可采样"——同一张图既当纹理又当附件（一个采样读取必须 SHADER_READ_ONLY，另一个必须是附件布局）。 示例的延时链只是因为 `RenderPipelineBuilder` 自己把 promotion 关了才躲过去；宿主不关就会撞上。修法：这一行改从目标自己取 （`entry.target->depth().preserve`，borrower 恒真、lender 在 borrower 数 > 0 时真）。新增真设备用例 `SharedDepthTest.TheHostTargetRowsSayWhoReadsTheDepthTheLenderWrites`（lender 行 + borrower 行的两个 `depthPlan`）；
* **变异反证 4 条断言红**。
* 而宿主释放顺序（注册表按插入序走，lender 先）会让 lender 先死 ⇒ 堆损坏/`double free`（本轮新写的宿主行用例**必然**触发， `test_vsg` 全量直接 abort）。这不是测试问题：宿主的发布/释放顺序可以让它发生（示例只是恰好没走到）。修法：借用计数搬进
* framebuffer 小**：lender 缩小到 borrower 之下时会把 borrower 指向小图 ⇒ VUID 00861 静默发生。修法：拒绝（返回 false，注释写清 这是"合法答案"而不是"没事干"），并在执行者层补**对称**一条：lender 缩到某个 borrower 之下时**拒绝并上报**（`coversBorrowers`）。 注意它比的是 borrower **这一帧之后**的尺寸（它自己有 ResizeInPlace/Rebuild 就会跟着缩）——第一版写成"当前尺寸"， 示例 resize 一缩一长就误拒（实测两条警告 + 配对卡住），已修。
* 698×132 = **0.24%**（只 FPS）；278×363 = **1.14%**（只第 4 预览槽与 FPS）⇒ 修复后与参考实现一致。`test_vsg` **672/672 绿** （新增 2 个用例：宿主的 lender/borrower 深度行、重新指向的最小图拒绝；`ClearPlanTest` 重写为两个方向）。 hygiene 0/866、诊断格式 0/39、文档符号 0。变异反证：①宿主机行那条（4 条断言红）②重新指向哨兵（3 条红）。
* **登记（未修，按严重度）**：①执行者层的**顺序**仍只有对象层用例 + 演示画面钉着（相位表加一行要动门禁基线）**（已由 §11.16ce 收掉）**；
* **1. `HostTargets::facts` 少了一个事实：lender 的"有人读它的深度"**。宿主目标的深度行是 HostTargets 自己拼的，
* 永不碰 lender 对象。用例把"按 lender 序释放"写成显式断言（它就是这个缺陷的回归用例）。

### 11.16bo M10f（2026-09-24）：量一次帧预算，并把"resize 偶发红"钉成确定
* **1. 帧预算（Debug + lavapipe，实测，`VINE_PROBE_TIMING` 临时探针 + `AllocationGate` 临时探针，均已撤）**：
* 用表面能力探针（临时）量到失败现场是 `follow: vsg 128x96 -> 128x96`——**窗口已经 96×64，平台却答旧几何**：宿主在**平台还没应用**时就把新尺寸公告出来，而后端只**读一次**（这是它该做的：`RenderBackend::resize` 的权威顺序写着"表面拥有自己的尺寸"），于是会话继续按旧尺寸出图（画面被拉伸，帧在飞时甚至会全黑）。我试过"没变化就再问一次"的后端补救——实测**再问一次仍然答旧值**，只是白花一次设备停顿，于是**撤掉**（并把"一次读取、不做第二次猜测"的理由写进 `followResizedSurface` 的注释）。 正确的修法在**宿主那一侧**：测试宿主 `TestHostWindow::resize` 现在**轮询到服务器确实报告了新尺寸**（上限 500 ms，超时返回 false 并由用例 `ASSERT_TRUE` 响亮失败），而不是"发完 configure 就假装已经应用"（`xcb_configure_window` 是 unchecked 请求，原来那记 round trip 的回复还被丢掉了）。**证据：全量套件 6/6 绿**（改前 5 次里 2–3 次红）。
* **顺带**：`followResizedSurface` 的注释补上这条契约（宿主公告必须是表面已经有的尺寸），以后再有"resize 后画面不对"的报障，先查这一条。
* **3. 没做的（登记，附理由）**：①resize 帧那约 2 条 `vkCmdDraw-None-09600`（稳态 20×`03047`+6×`09600`+6×`Viewport-01770`，与修前逐项相同）——先于本片存在，且画面正确；②Release 构建：`build-release/` 只配了 selftest/tests，没有 app+插件（要整棵重配重编，不在本轮预算内）；③执行者层的相位用例。**（③已由 §11.16ce 收掉。）**
* 用户要求"继续，优化"。先量，再改——结论是**这一层已经没有可观的 CPU 可优化项**，真正的问题在别处（下面第 2 条）。
* - resize 的大头不在这条路径里：跟随表面会重建交换链，`buildSwapchain()` 要 `vkDeviceWaitIdle`（计数的 `deviceWaits`）⇒ 每次尺寸事件一次设备停顿，**设计如此**（M9e 的结论：不跟随的话 vsg 会在提交里补重建，那会带来 VUID 02852/02853 与段错误）。
* ⇒ **结论**：想再快，只有换构建配置（Release）或改产品行为（比如拖动时不做实时重建），两者都不是这一层能顺手做的；本层的帧预算已经很小。

### 11.16bp M10g（2026-09-24）：给重写版补一道应用级门禁（画面即证据）
* - 两条判据（两次采样都判）：①渲染区域非近黑 ≥30%；②**G-buffer 预览条**（第 2 个槽位按 `fitPreviewRect` 反算矩形）
* 最大通道 ≥64——②是"离屏链到底画没画"的判据，①答不了（只剩天空时窗口照样 84% 非黑）。另计：0 个未知 VUID、 警告 ≤5（"每帧一条警告"就是洪水）、进程必须在跑、窗口行必须出现（否则 FAIL）。
* **变异反证（这道门禁为什么值得存在）**
* 把深度清除整条抑制（`ClearPlan.cpp` 的 own-depth 分支加一个恒假的开关）：`test_vsg` **672 全绿**、hygiene 全绿， 第 7 阶段两条判据**同时红**——`before 378x247: content 1.09%, preview 0; after 698x132: content 1.70%, preview 0`。 即"天空-only"这类回退，只有画面能抓。
* 会把窗口用例饿到读黑 ⇒ **已回退**。结论留给 09600 的真正修法：让"采样描述符声明的布局"在**第一次被命名之前** 就是真的（或在生产者没跑的那一帧不采样），不要用额外提交去补。
* （实测 6 个遗留窗口时全套件 5~9 个用例红，杀干净后 3/3 全绿）。门禁之所以用 `exec` 起应用并 `cleanup_app`， 就是为了自己不留窗口；**任何别的跑法也必须做到**（`( … ) &` 里不加 `exec` 的话，杀的是子壳，应用活着）。
* **未修（登记，附方向；`03047` 已于 M10i 修掉，见 §11.16br；`09600` 已于 M10j 修掉，见 §11.16bs）**
* - 新脚本 `scripts/xwinresize.py`（顶层父窗口 = 宿主容器，理由与用法写在头注释）、`scripts/ppmprobe.py`（读 PPM 某个矩形）。
* 名单外的任何 VUID 仍会让阶段红。方向分别是：屏幕路径改用**动态 uniform 偏移**（内容路径 `api/BlockDescriptors` 已经是这么做的，这正是"一个事实一种拼法"的漏网处）；以及上面 09600 的那条。
* 这是测试宿主的脆弱点，不是后端的画面问题；下一步要么让读窗口有可靠的同步点（会话自己的 presented 计数）， 要么把"整片窗口用例红"当成环境信号报告出来。
* 呈递的帧永远到不了——测试宿主现在先等这个报告，单跑 4/12 红 → 0/12、全量 4 跑全绿；**会话侧** "画面已落地"的事实仍未做，那是 §11.16cf 第 8 条。）**

### 11.16bq M10h（2026-09-24）：Release 构建进同一道门禁——并抓出"两个 device 同时活着"
* 而 `build-release` 用的是 vsg 的默认值 **1**。而这个 1 是**有意的**：插件 `CMakeLists.txt` 的注释写着"a path that did try to create one now throws instead of quietly working"——**它就是用来抓"同时存在两个 device"这类路径的**。 ⇒ Debug 的缓存把这个绊线**关掉了**，一路掩盖了缺陷。（方法教训：**"有意的"缓存项如果只在某个构建树里被手工改过， 另一个树就是唯一的证人**；两棵树都要跑同一道门禁。）
* **登记（仍未修）**：①`09600` 一类（已于 M10j 修掉：§11.16bs——白 fallback 改走上传；`03047` 见 §11.16br）；
* （注释写明理由：宿主自己的引用是宿主的事）。

### 11.16br M10i（2026-09-24）：`03047` 修掉——影子块改走动态偏移，屏幕采样集可复用
* **缺陷**（M10g 的门禁发现、M10h 登记）：稳态每帧约 20 条 `VUID-vkUpdateDescriptorSets-None-03047`——**屏幕路径每帧重建它那份
* 采样集**。原因写在 `api/ContentPipeline::sampledSetLayout` 的注释里：`a full-screen call has ONE block ... the pass bakes that block's offset into the descriptor`——而那个偏移**每帧都变**（帧 arena 每帧为每个 call 写一份块），于是集合必须重建； 框架的池又把同一个 `VkDescriptorSet` 句柄发回给下一帧，而上一帧的命令缓冲还在用它，正撞在这条 VUID 上。
* - 用例 `ContentPipelineTest.TheEnginesShadowedLightingDeclaresItsOwnShadowSlots` 改成断言 `..._DYNAMIC`（注释写明为什么必须是动态的）。

### 11.16bs M10j（2026-09-24）：`09600` 修掉——白 fallback 改走上传，"录制方"这个前提没了
* `0xFF`，`properties.imageViewType = 2D` + `MipmapLayout`，与 `api/MaterialImages` 的白 cube 同一套 拼法），由 viewer 的传输步上传——`RecordAndSubmitTask::submit` 里的 `transferData(TRANSFER_BEFORE_RECORD_TRAVERSAL)`（§11.16al 记过的那条），**在录制之前**完成拷贝并把 图像留在 `SHADER_READ_ONLY_OPTIMAL`。设备依旧不需要：`create()` 只造 create-info，上传是 viewer 的 事。`fill()` 与它的一整套清屏机制随之删掉（`WhiteImageTest` 改成断言"数据在图上、属性正确、四通道全 `0xFF`"）。
* （`vkQueueSubmit` 后把命令缓冲、池、状态的保管交给 `RetirementQueue`，不 `vkWaitForFences`）。想法本身 （FIFO 队列保证它在后续帧之前执行；持有到窗口到位即可，无需等待）没问题，但**建图像的那一刻根本没有可 以提交的东西**：`vsg::Image` / `ImageView` 是 create-info，`vk()` 要等 `vsg::Context` 编译（帧循环里的 compile 步）才有效——在 `OffscreenTarget::buildAttachments` 里录的屏障会指着空句柄。实现过、量过（应用 VUID 一条没少）、随即整体回退；结论写在这里：**要在"提交前"把某张图弄进声明布局，只有两条路——让它带 DATA 走传输步，或让它已经被编译过。**

### 11.16bt M10k（2026-09-24）：老渲染器退场——删掉 22 个 TU、31 个头、30 个用例、自检目标与两个脚本
* 老渲染器那些章（§1 文件地图、§2.4/§3.1/§4.1/§5.1/§5.7 等）标题改标 **（历史登记）**，让 `scripts/check_doc_symbols.py` 的"历史段落不参与漂移检查"这条规则接住它们；`gfx_backend_vsg.md` 的两处 （§11.1 类布局、§14.2 头文件纪律）同样标历史，§10 里那句指向 `VsgTargetBookkeeping.cpp` 的清屏值改指 `core/TargetPlan`；`data-flow.md` 过时提示里的两个单元名用 `<!-- drift-ok -->`（那行**就是**在说被删掉的 东西）。

### 11.16bu M10l（2026-09-24）：收尾四件——图像名字、顺序用例、指名的报告、letterbox 单测
* 同时钉了三层——编译出来的计划顺序、`executor.recorded()` 的顺序、**以及像素**（谁最后跑）。§11.16bo 的登记 已过时，本片只做确认（不新增重复用例）。
* `the target '<名字>' did not follow its description: <原因>`（名字取自注册时那条标签；没有名字才是"a target"）。 新用例 `ExecutorTest.ATargetThatDidNotFollowItsDescriptionIsNamedInTheReport`：出借方+借方，计划让**出借方** 重建（借方还在 ⇒ 拒），把宿主 sink 里的消息读出来断言含 `probe-lender`。

### 11.16bv M11a（2026-09-24）：一次外部审查的落地——三处真缺陷 + 一道失效的门禁（含变异）
* **触发**：对 `src/viz/graphics`（SDK + 引擎）与 `src/plugins/gfx_backend_vsg`（重写版 `api/` + `core/`）做了一次
* 全量走查（读代码 + 跑三条静态门禁，未跑设备）。走查列出的条目分三类：**"实现好了但生产路径上没人调用/键不完整" 的真缺陷**（本条修掉，各有变异反证）、**门禁自己静默失效**（修掉）、**设计取舍**（不改的先写论证，见 §11.16bw）。
* `ContentStore::releaseAbandoned`（`ContentStore.hpp:151`）与 `MaterialImages::releaseAbandoned`（`MaterialImages.hpp:156`） 都实现好了、都各有单测，而**帧驱动里一个调用点都没有**（`ContentStore::clear` 更是全仓零调用）⇒ 宿主丢掉的几何 连着它的**表行、半片、集合与管线**活到会话结束（半片/集合的 sweep 判据是"表还能不能回答这个键"， `ContentHalves.cpp:210` / `ContentSets.cpp:302`，所以表不放手它们也不会放手）。这正是上一轮 P8 修过的缺陷在重写版复发。
* - 调用点唯一：`VsgBackend::swapBuffers()`，**录完之后、提交之前**（提交推进停放队；停放必须在推进之前）。
* **变异 2/2**：图像那一半空转 ⇒ 恰好 `EverythingTheHostDropped…` 红（计数 + `has()` 两条断言）；
* **变异**：把 kind 从比较里去掉 ⇒ 恰好这两条红。
* 两半都失效：① `MAPPED_DIRS` + `units_of()` 只枚举**目录直接子项**，而代码搬进了 `src/api|core`、 `include/vine/vsg/api|core` ⇒ 它只看见顶层那 19 个（实测：树里 143 个，**124 个不在门禁范围内**）； ② 点名检查用前缀白名单（`PLUGIN_UNIT_PREFIXES`），它不认任何新名字 ⇒ 文档里写错/写旧了也不会红。 ⇒ "每个单元都要被文档点到"和"文档点到的单元必须存在"两条都成了空话。
* - 做法：`units_of` 改递归；点名检查改成"这个名字在本仓（`src`/`tests`/`tools`/`cmake`）里必须真的存在"，
* 四个单元**没有任何文档点到**，以及 `backend.md:600` 一句仍在点已删除的 `selftest_datarefresh.cpp` （它所在的 §5.3.2 是旧渲染器的内容，已补 `历史登记` 标记）。修完 **145 单元全绿**。
* - **变异**：把枚举改回只列顶层 ⇒ 立刻红；把 `ContentSweep` 从三份文档里整体改名 ⇒
* 而 switch 后面就有一段静默尾部；`mapBlendFactor` 的尾部把未知因子变成 `VK_BLEND_FACTOR_ONE`（**不是**"不混合"， 而是另一种混合 ⇒ 静默错图）。两处都改成说真话，并把"未知因子"那条登记为需要"在状态被书写的源头（引擎的 `BlendState`）校验"才算真修（见 §11.16bw）。
* **doc symbols 145 单元**（修前 19）；五条变异各自咬住目标。**证据边界**：本机没有窗口系统 ⇒ 窗口类用例
* 64 ⇒ 钳到 16；0.5 ⇒ 1；**2 级 mip 纹理**的采样器 `anisotropyEnable=TRUE` 且倍率 = 上限；单级纹理仍是 FALSE）； 设备用例 `VsgBackendTest` 第 3 步断言"缓存被告诉过"（`maxAnisotropy() > 1`）。

### 11.16bw 审查的其余条目：不改的，先在这里论证（附"真要做时的形状 + 触发条件"）
* > 规则与本仓既有习惯一致：**"有意不做"必须写得出理由和触发器**，否则它就是被忘掉的缺陷。
* **A2** 共享流的释放半边没接线，且 `SharedStreams::acquire` 每次 `++readers`（`Streams.cpp:156`），`release` 生产路径零调用 — **缺陷（已修：§11.16cb 换成“帧命名 + 窗口”的寿命，不可达的 `release` 撤掉）** | `readers` 已经退化成"累计 acquire 次数"，"最后一个读者放手 ⇒ 条目离开"不可达；唯一回收是容量 FIFO（512）。**但**：①它现在的可见后果只在"同一帧里被命名的不同流数 > 512"时才出现（那时每帧会把最老的 ~(N−512) 条挤出去、下一帧重新上传），demo 是 ~10 条量级；②修它要动"条目什么时候可以走"的语义（谁是读者？答案不是命令，而是**保留在半片/集合里的 bind**），而正确形状与 A1 的扫尾同一套：**按"本帧被命名过"设 seen 集 + 停 K 帧未命名才放手**，并把容量 FIFO 降级为硬兜底 | 形状：`StreamUploads::beginFrame()` + 每命令 `noteSeen(key)` + 帧尾 `releaseUnseen(timeline, retirement)`（K = `slots + 1`，与其余停放同窗）。**触发器**：某个负载的"每帧命名流数"越过 512，或有人报"网格多起来之后每帧在重传"
* **A3** SDK 没有内容释放入口（`releaseGeometry/Material/Program/Texture` 都没有） — **设计问题（不改 SDK）** | `RenderBackend` 只给了 `releasePass` / `releaseRenderTarget`，因为**只有这两种对象的生命周期由宿主显式宣布**（SDK 文档如此）；内容对象是引用计数的，宿主放手即 `useCount` 变化 —— 修 1 之后这条链已经闭合（帧级扫尾看得见放手）。加一套"显式 release 内容"的入口等于把引用计数的信息再手写一遍，还多一处必须与 `useCount` 一致的状态 | 不加。**触发器**：出现"宿主必须在同帧内让后端立刻放手"的需求（例如显存压力下的显式驱逐），那时先加**一个**入口（`releaseContent()`？）而不是四个
* **A4** `Material` 是全 SDK 唯一没有 revision 的内容类型；`MaterialManager` 成了死抽象（唯一实现是测试假件） — **两半：一半改设计（已论证），一半登记** | ①`Material` 无 revision ⇒ 后端只能**每帧逐命令** compare-and-write（`VsgBackend.cpp:659`）。这是**有意的兜底**，不是漏接：SDK 的既有规矩是"被共享的对象自己不推断内容变了"，而 `Material` 的 setter 至今没有公告语义 ⇒ 后端不能假设"没人公告 = 没变"。②`MaterialManager`（`MaterialManager.hpp:19-27` 明说"具体后端实现它并自己持有资源缓存"）现在**没有任何生产实现**，那句话是**错的** | ①形状：给 `Material` 加 `revision()/setRevision()/bumpRevision()`（照 `Geometry`/`Texture`/`ShaderProgram`），缓存用"revision 变了才比较"的快路径，**保留** compare-and-write 作为"没公告"的兜底。**触发器**：材质数量大到"每帧 O(命令数) 次块比较"进入剖析的前列（当前 11 趟 × 42 命令 = 数百次 64 B 比较，量级还看不见）。（2026-09-25 已量化：§11.16cx——命令地板 ≈28 µs/条；>256 不同材料时 **0 命中**、每帧全量重写+驱逐；比较本身不是大头，write/evict 才是。另：storage **没有释放路径**，arena 槽位只经驱逐离场。）②形状：删掉 `MaterialManager` + 它的假件与用例，并在 SDK 文档里写明"材质由后端按帧观察"。**触发器**：任何一次"宿主以为管理器在物化材质"
* **A6** `MaterialImages` 淘汰是 FIFO 而非 LRU；`ContentStore` 没有任何容量上界 — **登记** | FIFO 的代价是"批量加载贴图会把正在用的挤掉并重建"，而那一次重建的价钱是**一次上传**（不是错误）；换成 LRU 需要"最近使用帧号"并接进诊断，收益面（同屏活纹理数逼近 256）在 demo 上不成立。`ContentStore` 的上界在修 1 之后由**扫尾**给出（宿主放手即回收），剩下的是"宿主一直持有但从不画"的对象 —— 按本仓既有口径那是**应当保留**（持有者是宿主） | 形状：淘汰键从 `stamp`（插入序）换成"最近被 acquire 的帧号"，`kMaxEntries` 不变。**触发器**：活纹理数接近 256 且观察到"用了很久的贴图被重建"
* **A8**（**已删：§11.16cr**，M11v）`Observe::FrameCounters` 及 `RetentionStats` 共九个字段无人写也无人读：`data_nodes_built`、`streams_refreshed`、`offscreen_builds`、`offscreen_resizes`、`window_builds`、`program_slot_builds`（`Observe.hpp:38-43` 声明并写文档；`PhaseTable.hpp:26` 拿 `offscreen_builds` 举例说"phase 必须能拦住它"）。相位用的是自己的 `DevicePhaseCounters`，所以那些规则永远不会响。| **已删**（连同无调用方的 `planGeometry` 刷新计划） — 要么接线（`ContentStore`/`Streams`/`HostTargets` 都有现成计数点），要么删掉；本配方想要的"Refresh vs Rebuild" 正是 `streams_refreshed`/`data_nodes_built` 能答的 | **触发器**：下一次有人想按"帧里的重建次数"写规则
* **B1** 三张内容表的查找是**线性扫描**，而每命令每帧要跑 5~9 次（`ContentFacts.cpp:50/91/122`；表只增不减） — **缺陷（已修：§11.16bz 量了斜率并落地行序 + 二分；`tablesFor` 自身那两次查找仍登记，见该节第 4 点）** | 复杂度 O(每帧命令数 × 表项数)：1 万 drawable/1 万表项时单帧 ~10⁸ 次指针比较，而 demo 是 ~42 条命令 ⇒ **任何现有门禁都看不见**（所以先做的是修 2：把键补完整，否则索引化会把"两行同键"变成"索引里后写覆盖先写"，把一个静默错图换成另一个）。另一条论证：**现在做没有收益面**，而有真实的回归面（内容路径是 407 条用例里最密的一片） | 形状：每帧在 `tablesFor` 里重建**排序的行号索引**（`vector<uint32_t>` + 每表一个比较器，O(n log n)/帧、`lower_bound` 每次 O(log n)），**不是**给表本身排序（手工构造的表会静默失配）；`ContentFacts` 带一个可空的 `const RowOrder*`，为空时回退到扫描并**在文档里写明这是慢路径**。**触发器**：某个负载的 drawable 数越过 ~2 000，或剖析里 `find*` 家族进入前列
* **B2** 每 pass 每帧的堆分配：`planClearValues` 的 `std::vector<AttachmentClear>`（`ClearPlan.cpp:36-52`，`ClearPlan.hpp:78`）、`ContentPass` 每帧的两个 `vector<ReportOnce>`（`ContentPass.cpp:153`）、`makeInputSet` 的 key `vector`（`ContentPass.cpp:516`）、每 pass 一个 `vsg::RenderGraph`（`OffscreenTarget.cpp:757`） — **缺陷（已收尾：§11.16bx 修第一处，§11.16by 量完其余并改为上限门禁）** | 全部是**小对象**（每 pass 几十~几百字节），M10f 已实测本层稳态帧 ≈2 ms（Debug + lavapipe），而 `AllocationGate` 测的是**堆净增长**、看不见 allocate/free churn（见 B3）⇒ 现在改它无法用证据收尾。`vsg::RenderGraph` 那条更不该省：上一帧的图可能还在飞，复用一个对象就是在改一个已提交命令图里的状态 | 形状：`PassClearPlan::colors` 换成定长 `std::array<AttachmentClear, kMaxColorAttachments>`（附件的上界是设备给的，很小）+ 计数；`ContentPass` 的两个 `ReportOnce` 向量改成复用（resize 而非重新赋值）；`make
* **B3** 分配证据的强度被高估：`AllocationGate` 用 `mallinfo2`（`AllocationGate.cpp:21-30`，`__GLIBC__` 限定）⇒ **Windows 上 unsupported**，用例在 unsupported 时把增长当 0（`BackendEvidenceTest.cpp:363`）；且只测净增长，对 churn 免疫 — **缺陷（已修：§11.16bx 加了计数的一半，相位改以计数为判据）** | 它守的命题（"稳态帧不分配"）在**交付平台上没有量具**：Windows 上那条相位退化成"没测"，而本仓已经宣称 Windows 是一等公民（H1）。修法是换量具而不是改断言 —— 需要**计数式**分配门禁（覆写 `operator new/delete` 计数，或注入计数分配器），这本身要新单元 + 相位 + 变异，属独立一片 | 形状：`test_vsg` 里一个只计数不改行为的全局 `operator new` 钩子 + `AllocationGate::countAllocations()`；相位断言"稳态帧分配次数 == 0"（并保留 heap 增长作为第二判据）。**触发器**：B2 落地之前必须先有它（否则 B2 无法证明干净）
* **B4**（**已测：§11.16cl**，0.53 µs/drawable·次，触发点仅 ~6% 帧预算 ⇒ 不改）`Scene::collectRenderCommandsShared` 每次收集分配 3 个 vector（`Scene.cpp:468/497/512`）并把整表搬 2~3 遍，`commands` 无 `reserve` — **缺陷（登记）** | 相机每动一帧就整份重来，是**引擎侧**（不在本轮前端改动范围内），而它的可见代价取决于命令数与 `sizeof(RenderCommand)`（≈200 B，含 3 个 `intrusive_ptr` 的原子增减）。当前 demo 的收集是 memo 命中或 ~42 条命令，量不出来 | 形状：`keyed` 改成 `vector<pair<double, uint32_t>>`（行号）并就地应用置换；`commands` 按上一帧规模 `reserve`。**触发器**：相机常动的负载 + drawable 数越过 ~2 000，或 B1 之后收集成为下一热点
* **B5**（**前半已收：§11.16ck**；预算不改，理由见该节）拒绝路径逐命令上报（`ContentPass.cpp:882-1116`）+ 每帧重置的 `ReportOnce`；块预算是硬上限（`draws/lights/shadows` 1024/帧、`views` 256/帧，`BlockStorage.hpp:50-56`） — **登记（前者已在 M10c/M10e 登记过）** | 洪水只在"场景里有坏内容"时出现，而那时宿主**需要**知道是哪一条；把逐命令上报压成"每插话一次"会让"这一帧有 300 条画不出来"变成一句话（丢信息）。块预算超限是**拒画**（有报告）而不是错图，且 1024 条/帧远超 demo 量级 | 形状：①按"每 pass 每原因一次"上报（保留第一条的完整身份，后续只计数）；②预算按需增长（插入点 `BlockStorage::beginFrame`）并在诊断里报"本帧预算不够"。**触发器**：大场景宿主报"日志被刷满"或撞到 1024
* **B6**（**登记**，§11.16cq；更正：§11.16cr——不存在的刷新路径已删）`StreamKey.revision` 取的是 **Geometry 的 revision**（`GeometryFacts.cpp:107/178`）⇒ 任何一次几何数据公告都会重传**所有** channel 与 index（本片实测 2 streams/帧；替换 buffer 与就地改写一样贵） — **登记（保持现状）** | 逐 `Buffer::revision()` 能省下未改通道的上传，但把"一次公告"的契约换成"每个 buffer 各自公告"——更弱（手册：漏报是静默的） | **触发器**：多通道大网格宿主报上传带宽（2026-09-25：已量化——见 §11.16cw；触发器仍未兑现）
* **B7**（**已按设计规则处理；未再复现，§11.16cq**）管线销毁与在飞提交的竞态：本配方首次入套件时门禁两阶段各报 **4×VUID-vkDestroyPipeline-pipeline-00765**；修复 = `releaseContentWorld()` 首行的**计数过的 device idle**（`SessionContentAccess::waitDeviceIdle`） — **登记（诚实记录未复现）** | 此后 20 次单例 + 两次全量套件（有/无该 wait 各试过）+ 两棵树门禁都是 0 VUID | **触发器**：再次出现时先查"最近被替换或逐出的管线"（`VariantPool` 逐出是另一条销毁路径）与 teardown 的 `deviceWaits`
* **D2**（**已修：§11.16cj**）`Material::specular()` 的 alpha 文档写"A 是强度"，但**没有任何着色器读它**（`builtin_forward.frag:145`、`builtin_gbuffer.frag:59` 都只读 `.rgb`） — **缺陷（登记：要么接线，要么改文档，二选一）** | 接线会**改画面**（默认 `specular.a = 0.5` ⇒ 高光减半），而"逐像素材质"的通道已经排满（G-buffer 的 spec 附件 alpha 空着，前向可用 `material.specular.a`），于是它是"能接、但要重新调 demo 并重钉像素基线"的一类 | 形状：前向 `spec *= material.specular.a`、G-buffer 把 alpha 写进 spec 附件、延迟侧读出并相乘；两条基线（证据行）随之更新。**触发器**：有人要求"按材质调高光强度"（当前唯一能做到的是改 shininess）

### 11.16bx M11b（2026-09-24）：先把量具做出来，它当场量出并修掉一处真的每帧分配（B3 + B2 的第一半）
* 字节读数**纹丝不动**——这就是字节读数看不见的那类）；`AZeroCountIsOnlyReadWhereSomethingCounts`； 相位 `two steady frames allocate nothing` 改成**以计数为判据**（字节读数降为第二判据），并在读数之前 `ASSERT_TRUE(countsAvailable())`。
* **变异**：把计数去掉（`noteAllocation` 空转）⇒ churn 用例红；把 `FrameGraph::reset` 改回 `assign` ⇒ 相位行红。
* 新用例 `FrameGraphTest.RebuildingAndSchedulingAFrameAsksForNoMemory`（**带两条边的**三 pass 图：reset + 2×addEdge + schedule 的窗口计数必须为 0——边上最容易丢容量，所以判据取带依赖的形状）；相位行 `two steady frames allocate nothing` 现在**真的在判**（变异：`reset` 改回 `assign` ⇒ 该行红）。
* **仍然登记**：B2 表的其余几处（`planClearValues` 的 vector、`ContentPass` 每帧的两个 `ReportOnce` 向量、
* 这两个**同一个数字**可区分——这正是这个文件存在的理由。
* 这条断言一直是空的。** 逐段量（探针，已撤）： `entry → graph.reset` **+4**，`reset → step2` 0，`step2 → schedule` **+23**，`schedule → step4` 0。 两处都在 `core/FrameGraph.cpp`：`findCycles`/`orderAcyclic` 把 Tarjan 的三张表、两个栈、 `std::priority_queue` 的底层容器都当**局部变量**建（一次 6~10 块），`reset` 用 `successors_.assign(n, {})`/`schedule_ = FrameSchedule{}` **丢掉容量**（下一步 `addEdge` 每条边再要一块）。 `Recording` 那一半是干净的（`beginFrame/beginPass/setRenderTarget/render×2/endPass/endFrame/swapBuffers` 全 0）， 所以这 27 块全在计划侧。
* `makeInputSet` 的 key、每 pass 一个 `vsg::RenderGraph`）——它们现在**可以被量了**（把 `AllocationGate` 的计数 窗口挪到那一条路径上即可），这是下一步而不是本轮的事。

### 11.16by M11c（2026-09-24）：B2 的其余几处——量完再决定（改 0 处代码，留 1 道门 + 4 条论证）
* `PassClearPlan::colors` — 2 次/pass | 要免掉它只有两条路：①签名改成"调用方给 scratch"（核心计划函数的入口形状要变，十余处调用与用例跟着动）；②换成定长 `std::array` + 计数（那就要给颜色附件数**定一个上界并加拒绝路径**，而上界是设备给的、本层现在没有这条规矩）。而"计划是 per-attachment 的表"正是 `ClearPlanTest` 断言的东西——改它就是改设计去换 2 次分配 | `void planClearValues(PassClearPlan& out, ...)`，`OffscreenTarget`/`WindowTarget` 各持一个复用的 plan。**触发器**：记录路径的"零分配"成为硬指标（硬实时/嵌入式），或剖析里 per-pass 分配进入前列
* `ContentPass` 的两个 `ReportOnce` 向量 — 4 次/pass（该构造共 7） | **语义上没有错**：`Scope` 的头注写着"episode 的边界由调用方决定——一帧的 scope 就每帧报一次"，而 `ContentAssembly` 每 pass 建一个 scope。要变成 session 级 episode，要么让 episode 行由调用方持有（`Scope` 加可空 span + 空时回退 = 第二条路径），要么把状态放进半片表——但表行是跨 pass、跨帧共享的，一次拒绝就会**永久静音**，那是行为回归 | `Scope` 加一个可空的 `std::span<EntryEpisodes>`（照 `input_sets` 的"空 = 调用方不保留"口径），由 `ContentAssembly` 按 session 持有。**触发器**：宿主报"日志每帧重复同一条拒绝"，或这条路径要进零分配门禁
* `makeInputSet` 的 key `vector` — 与上面同量级 | 同一个形状（per-pass scratch），而 key 就是 cache 查找的键，换成成员 scratch 又要开一个"调用方持有"的口子 | 与上一条合并考虑。**触发器**：同上
* `CoreAllocationGateTest.TheRecordPathsBookkeepingCostsAFewSmallVectorsPerPass`（device-free）： `planClearValues` ≤ 2 次/调用、`ContentPass` 构造 ≤ 8 次/次，且 **1 个 entry 与 4 个 entry 的次数必须相等** （"每 entry 一个 vector"这种形状会被这条断言挡住）。
* **变异 2/2 红**：在 `ContentPass` 构造里塞一个 `std::vector<int>(8)` ⇒ 9 > 8 红；在 `planClearValues` 里塞一个 ⇒ 4 > 2 红。
* **为什么不是"== 0"**：记录路径**按设计**每帧新建命令节点（`vsg::RenderGraph`、每条命令的 bind/draw），在那里断言 0
* 等于在断言 vsg 的行为；能且应当为 0 的是**计划路径**，它由相位行 `two steady frames allocate nothing` 守着（§11.16bx）。

### 11.16bz M11d（2026-09-24）：B1——三张内容表的查找从扫描变成二分（先量斜率，再改）
* **本条取代 §11.16bw 的 B1 行**（那行把 B1 登记为"规模化墙"，触发器是"drawable 数越过 ~2 000"。这次把
* ⇒ §11.16bw 里"1 万 drawable 时单帧 ~10⁸ 次比较"这个推算有了实测支撑。
* - 同一身份+revision 的**第一行**仍然决定答案（`channelsMatchLayout` / `blockFitsAbi` 的 Malformed 判据、
* 未命中的三分类、以及那些"多行同键"的形状（同一几何的两个 revision、一个程序的两种 kind 与三个 variant、 材质原地替换的两行、**畸形行与好行成对且顺序颠倒**——扫描"第一行决定"，段内表序必须让二分也这样答）。
* - **变异 1/1 红**：把 `orderGeometryRows` 的键改成"先 revision 后身份" ⇒ 两条用例同时红（差分 + 单调）。
* **4.（已收：§11.16ca）当时的 B1 另一半，`api/ContentStore` 自己的查找**：本节写下时 `tablesFor` 走帧调用的
* `Data::liveGeometry` / `liveMaterial` 还是线性扫描；§11.16ca 把（rows、每行存储、行序）收进一个 `Table<Row, Storage, MakeOrder>`，两个 live 查找现在走的正是与录制同一个二分（`findGeometry` / `findMaterial`）。本节留下的形状描述只作历史。
* - **结论：不需要"小表扫描 / 大表二分"的阈值规则。** 在 demo 量级（几十行）两者同为 ~3 ns，而 demo 之上
* 每一行都是纯赚。这个"没有阈值"是用数字换来的，所以阈值也就没有存在的理由（少一条分支、少一条规则）。
* - `AnIndexedTableAnswersExactlyWhatAScanWould`：同一批行问两遍（带序 / 不带序），断言**答案逐字相同**——包括
* （这是二分成立的前提，直接断言，而不是"某次查找恰好命中"）。

### 11.16ca M11e（2026-09-24）：B1 的另一半——表自己拥有行序（rows + storage + order 一个主人）
* 所以行序必须**始终当前**——过期的行序不是漏一行，而是按旧同余关系读到**别的行**（甚至越界）。而"始终当前" 意味着**每个移动行的地方**都得刷新它：三个追加点 + 五个删除点，其中两个（revision 上跳、对象被弃）是**走帧期间** 由停放回调触发的。手工维护三条平行向量（rows / storage / order）正是上一节拒绝过的形状。
* **4. 变异 2/2 红**：`Table::eraseIf` 不刷新 ⇒ 上面那条用例在删除那一步红；`Table::append` 不刷新 ⇒ **两条**用例红
* **并且变异抓住了守卫自己的一个漏洞**：`covers()` 最初只检查"前 n 个是 0..n-1"，于是**行序比表长**时它仍然为真——
* 这正是删除变异**第一次没被抓住**的原因；改成先比尺寸才红。教训写在用例注释里：**守卫写错了会静默通过， 而"没抓住变异"是它唯一的报警器**。
* - **性能依赖不变式，而它被断言**：`ContentStoreTest.TheRowOrderCoversEveryRowAfterAppendsAndErasures` 把 store
* 走一遍四种移动行的方式（首次描述 = 追加、材质编辑 = 原地替换、revision 上跳 = 追加 + 停放、停放到期 = 删除）， 每一步都断言三张表的行序是**恰好覆盖**该表的置换。
* （6 处断言：覆盖 + 发布处的"答案就是行序指向的那一行"）。

### 11.16cb M11f（2026-09-24）：A2——共享流的寿命改成“帧命名”，那半个 `release` 撤掉
* （它录进去的节点每帧重建）⇒ `readers` 是“累计 acquire 次数”，`release()` 的“最后一个读者”**永不可达**。 实测：`release()` 在生产代码里**零调用点**（grep 全库，只有声明、定义与用例）。
* 正在用的条目每帧被挤掉 ⇒ 每帧重传（这就是§11.16bw 登记的墙）；② 长会话里被弃的旧网格占着名额，把活的挤出去。
* - **变异 2/2 红**：① `StreamUploads::beginFrame` 不记帧号（接线失效）⇒ **3 条**用例红（含装配级）；

### 11.16cc M11g（2026-09-24）：量具自己坏了——4 对齐的分配变成 `bad_alloc`，设备电池全灭（修好并接回）
* 静态门禁三条全绿 ⇒ 报告一直很好看。**最后一次同时跑了设备与门禁的记录是 M10j（§11.16bs）**；M11a–M11f 六片的证据面都没有设备。教训写进本条：**"passed / N skipped" 里的 N 就是设备电池的缺席**，它必须被当成**没测**， 而不是"跑过了、跳过了几条"。（门禁脚本本来就把"有 SKIP 即失败"写在头上，但只有当人真的去跑门禁时才生效。）
* 对齐 **≤ `alignof(std::max_align_t)` 时直接 `std::malloc`**（malloc 本来就满足一切基本对齐，`posix_memalign` 只服务更大的），其余仍走 `posix_memalign`。**分支故意只放在 POSIX 半**：MSVC 的 `_aligned_malloc` 自己接受小对齐， 而它分配的内存必须由 `_aligned_free` 释放 —— 在 Windows 上返回 `malloc` 内存会被 `releaseAligned` 交给 `_aligned_free`，那是**堆损坏**（注释里写明）。
* ①逐对齐（1/2/4/8/16/32/64/128）断言"服务得到 + 指针真的按它对齐 + 配对的 aligned delete 能释放"； ②再在**只有分配/释放的窗口**里数一次（`allocations() == 8`、字节读数 0）——检查必须在窗口外，因为
* **gtest 的消息自己会分配**（第一版把 `EXPECT_*` 放进窗口，读数就带上 160 B 的测试框架噪声，实测抓到）。
* **7. 变异（2/2，都跑了恢复后的复验）。** 把"小对齐走 malloc"那一支删回去 ⇒ ①新用例**红**（exit 1）；
* 红：`gate.allocations() == 0`、`gate.bytes() == 0` —— 语言允许**省略**对可替换全局分配函数的调用 （`[expr.new]` 的省略规则），`-O2` 正是这么做的 ⇒ §11.16bx 写的这条**正对照**只在 Debug 树里成立。 修法：新 `keepAllocation()`（本文件的匿名命名空间里的 `noinline` 函数：POSIX 用 `asm volatile("" : : "r"(block) : "memory")`，MSVC 用 `__declspec(noinline)` + volatile 存储）把块地址交给 优化器看不穿的代码；注释里写明**为什么 volatile 存储不够**（语言不要求地址值互不相同，栈块也是合法答案）。 变异：**删掉那一行调用 ⇒ Release 红（0/64）、Debug 绿（64/64）**——它正是那棵树里唯一承重的一行；恢复后两棵树都绿。
* - 应用阶段那**唯一一条** warning 就是 §11.16bj 登记的**首帧 "no compiled content half"**
* （实测抓到原文：`the pass' sampled inputs is not drawn: no compiled content half was built for this pass`）， 它仍按"首帧自愈"登记（未做）。
* `VsgBackendPluginTest` 报后端为空）；`ninja -C build-release gfx_backend_vsg` 补上后那三条立刻绿。 这与 M10h 的教训同源（`--target Vine` 不会顺带建插件），所以"两棵树跑门禁"必须**连插件目标一起建**。
* **下一步（本片之后）**：**设备电池必须回到每次收尾里**（跑门禁、或至少带 `VK_ICD_FILENAMES` 跑一次套件并断言
* `skipped == 0`，**连插件目标一起建**）；其余未做项照旧以 §11.16bw 的表与各片"登记"为准（B4/B5、D2–D5、A4/A6、 执行者顺序的设备相位、首帧自愈、`ContentPass` 每帧重置的插话）。
* aligned 分配都转给 `posix_memalign`，而它拒绝小于 `sizeof(void*)` 的对齐 ⇒ 那些请求变成 `std::bad_alloc`； libLLVM 不接这个异常（lavapipe 的 JIT 会给 `allocate_buffer` 传 **4 对齐**的块），于是**任何带设备的运行都会在 第一个设备用例上 abort**。**从 M11b 起，设备电池事实上停止运行**，而 M11a–M11f 的证据行（"383~393 passed / 24 skipped"）恰好是**不含设备用例**的那一半 —— 这条缺陷的形状正是本仓最在意的那一类：**门禁静默失效**。

### 11.16cd M11h（2026-09-24）：空绘制调用不是绘制调用——应用日志的最后一条 warning 消失
* **6. 变异（1/1，含恢复复验）。** 把守卫改回 `if (true || !commands.empty())` ⇒ ①新用例**红**；②应用日志的
* **登记（本片没做）**：①**首帧的 gizmo 画不出来**这件事本身在引擎侧（`AxisGizmo` 表面尺寸未知 ⇒ 空场景）；后端
* 现在照实处理（那一帧没有它的 pass），要"首帧也画"属于引擎的改动（触发条件：有人报首帧缺 gizmo）**（2026-09-25 复核并收口：见 §11.16db——不是"尺寸未知"，预热帧叠层有效；是布局瞬态尺寸（100×30）下盒放不下，
  而旧算式给的是负视口）**；②门禁本次首跑 出现一次**已知的窗口读回偶发**（`VsgBackendTest.AMaterialEditLandsOnTheNextFrameAndASteadyFrameRebuildsNothing`， 重跑即绿，门禁照既有规矩**把首跑失败写进证据行**再裁决）——与本节改动无关，留给出窗口同步点那一条登记； ③其余未做项照旧以 §11.16bw 与各片"登记"为准。
* `ContentAssembly::record` / `ContentHalves::halvesFor` / `ContentPass::record` 的拒绝点插探针，第一帧的七趟 pass 里 第 7 趟是：`colors=1 draws=1 content=1 entries=0`，**而且没有任何"事实缺失"的 skip 输出** ⇒ 不是表查不到，而是那条 draw 本身**没有命令**（探针：`draw kind=0 commands=0`）。再在 `beginPass` 打印 pass 身份：`id=7 name=`（无名， 与 `id=6 name=` 一起正是 `RenderPipelineBuilder::applyOverlays` 加的 gizmo 与 fps 两个叠层 pass）；`AxisGizmo::execute` 在 `surface_w_ <= 0` 时不会设视口，那一帧的场景收集因此是空的。

### 11.16ce M11i（2026-09-24）：租约的顺序有了设备相位——借来的一对一起长大
* **本条收掉 M10c/M10d/M10e/M10f 反复登记的那一条**："`applyTargetPlans` 的选择排序只有对象层用例 + 演示画面钉着，
* `depth.source`）⇒ 执行者无从知道"借方的 framebuffer 指着出借方的深度图"，于是按计划顺序走、借方被拒 （实测 `resized == 1`、借方停在 8×4）。相位现在把租约**按 `OffscreenTarget::depth()` 的报法**原样填进事实 （`has_depth` / `borrowed` / `source` / `promotion` / `any_pass_preserves_depth`）。⇒ **任何自己拼 facts 的调用方 都要照这条**，否则拿到的是"坏顺序"那一支。
* **4. 变异（1/1，含恢复复验）。** 把选择排序的租约查找改成"没有出借方"（`indexOf(lenderOf(...))` ⇒
* **下一步（本片之后）**：仍未做的按 §11.16bw 与各片"登记"（B4/B5、D2–D5、A4/A6、`ContentPass` 每帧重置的插话、
* 出借方的深度**）；计划把**借方的那趟 pass 放在第一位**（走计划顺序 = 走错顺序），而事实里两个目标都要求长到 16×12 ⇒ 两趟都是 `ResizeInPlace`。第二帧应用：
* - **顺序规则靠事实驱动，不靠身份猜测**：第一版相位手工拼 `TargetFacts` 时**没写租约**（`depth.borrowed` /
* 表尾的合计断言（frames / targets_built / resizes_replaced / plan_applied / parked）跟着更新。

### 11.16cf M11j（2026-09-24）：读窗口的偶发红——"窗口还没被服务端报可用就呈递"（测试宿主的同步点）
* **登记来自哪里**：§11.16bp 的最后一条（"全量套件里读窗口的用例在**显示环境被占**时仍会红…
* 窗口按自己那张图**重画**（清屏色）。所以"驱动两帧空计划再读"其实是在**盖掉**画面：读到的黑是 真事实，不是延迟。⇒ 三处 settle 改成**再呈递同一张画面**：`VsgBackendTest` 四例（`settle()` 再 `drive()` 两次）、`SessionContentTest` 与 `WindowCompositionTest`（在循环里重新 `assignFrameGraphs` 同一张 `command_graph` 再提交——分配是"给下一次 `commitFrame` 用"的， 所以必须进循环）。
* **7. 变异与"这次没证成的部分"（诚实记录）。** M3（确定性守卫）：构造里去掉那次等待 ⇒
* **8. 没做到的（登记）。** 真正的同步点是**显示路径**的事实，今天没做：构造的等待只保证"呈递
* **下一步（本片之后）**：仍未做的按 §11.16bw 与各片"登记"（B4/B5、D2–D5、A4/A6、`ContentPass`
* **1. 先把"黑"拆成两种事实（新仪器）。** `TestHostWindow` 现在记下每一次读**自己的 X 错误**
* 画面）。改之前两者在返回值里一模一样（都是 `{0,0,0}`）——这正是它被当成"画面错误"读了好几轮的 原因。设备无关的新用例组 `HostWindowReadTest`（纯 X，不碰设备）把两半都钉住：画上去的白能读回 且 `read-error 0`；**越界的读被拒并如实报 8**（确定性守卫）；有界等待在"已经满足"时不花时间、 "永不满足"时到点也返回（调用方自己的断言才判红）。
* 每帧重置的插话、首帧的 gizmo 空白），加上本片第 8 条（会话侧"画面已落地"的事实）。

### 11.16cg M11k（2026-09-25）：延迟光照的"背景"判据（收掉 D4）——写掩码，不是距离
* **登记来自哪里**：§11.16bw 表的 **D4** 行："延迟光照的背景判据是 `dot(pos,pos) < 1e-6`
* （`builtin_deferred_lighting.frag:34`）⇒ 相机贴住几何时出现固定的 0.06 色洞"，形状当时就写明了： "判据该换成'这条通道写没写过'（G-buffer 的 position 附件 `w = 1` 表示写过，清成透明黑 ⇒ `w == 0` 就是没写过）"，并在末句标注"本机窗口用例跳过，这条链路里最贵的一环（真机画面）恰好是缺的"。
* - 左半（写过的、位置在相机原点）⇒ 必须被**着色**：`albedo(1,0.25,0.25) × ambient 0.5 = (0.5,0.125,0.125)`；
* - 右半（没写过的）⇒ 必须是 0.06 的平背景。
* 修好之前/之后两条都被断言，所以它同时钉"新判据"和"背景没丢"。
* **4. 变异（2/2 红，各带恢复复验）。**
* 画面**量（照门禁自己的法子：从应用日志里取 `attached to the host window 0x…`）。实测：窗口读**看 得见**——天空的渐变消失、出现 **14 440 个像素的平色 `(69,69,69)`**（= sRGB 编码后的 0.06）、 平均色 `(101,112,121) → (84,91,99)`；但**门禁的判据看不见**：`content` 两次都是 87.04%（阈值 30%）。 ⇒ **登记**（**已由 §11.16ch 收掉**）：应用阶段补了一条"视口带里平背景色的占比 ≤ 5%"的判据， 该变异在门禁里**红**（`background 25.64%`）。**本片当时不改**（只修 D4）。
* **下一步（本片之后）**：仍未做的按 §11.16bw 与各片"登记"（B4/B5、D2、D3、D5、A4/A6、`ContentPass`
* 每帧重置的插话、首帧 gizmo 空白、会话侧"画面已落地"的事实），加上本片第 4 条 M3 登记的应用阶段判据。

### 11.16ch M11l（2026-09-25）：应用阶段的**第三条**画面判据——"整片背景灰"（收掉 §11.16cg 第 4 条的登记）
* **登记来自哪里**：§11.16cg 第 4 条的 **M3**：D4 修好之后，契约的另一半（**引擎 G-buffer 写 `w = 1`**）没有任何
* 用例看着——把 `builtin_gbuffer.frag` 的 `w` 改成 0，演示窗口**看得见**（14 440 px 平色 `(69,69,69)`、平均色 (101,112,121)→(84,91,99)），但门禁两条判据都**通过**（`content` 87.04%、`preview` 244）。本片把这条判据补上。
* **1. 先量判据的候选（在两张已有截图上离线算，不动代码）**：健康的画面里平背景色
* **一个像素都没有**（`(69,69,69)±4` = 2 px、(15,15,15)±4 = 1 px），变异画面里 **13 565 px（14.53%）**
* - `scripts/vsg_rewrite_gate.sh` 的应用阶段：多一条判据 `background` = **预览条以下那条"视口带"**里平背景色的占比
* - 头部的"两条像素判据"改成三条（写明为什么第三条存在、以及量到的数字）。
* - **变异（G-buffer 写 `w = 0`）**：应用阶段**红**——
* - **诚实记录（判据的限度）**：resize **之后**那个样本不敏感——698×132 的窗口里四个预览槽盖住了视口的大部分，
* 同一次失败只读出 3.97%（< 5%）。也就是说**敏感的是 resize 前那个样本**，这正是"两个样本都判"的意义； 整窗版 vs 视口带版也量过（同一次失败：整窗 15.47%、视口带 25.64%）⇒ 判据选视口带。
* **4. 顺带记录（新登记的观察）**：Debug 那次门禁的**首跑**出现一次
* `BlockStorageTest.TheRegionsAreLaidOutOnceAndDoNotOverlap`（976 ms）红、**重跑即绿**，门禁按既有规矩把首跑 失败具名写进证据行再裁决（Release 同一次没有）。它此前不在"已知偶发"名单里 ⇒ 登记：**若第二次出现**，按 §11.16cf 的办法（探针 + 单独重跑 10 次）先分"环境/顺序"与"真缺陷"。
* **下一步（本片之后）**：仍未做的按 §11.16bw 与各片"登记"（B4/B5、D2、D3、D5、A4/A6、`ContentPass` 每帧重置的
* 插话、首帧 gizmo 空白、会话侧"画面已落地"的事实），加上本片第 4 条那次首跑红。

### 11.16ci M11m（2026-09-25）：一句话不是说一次——**半分片的插话状态归半片所有**（收掉 §11.16ce/§11.16bw 反复登记的一条）
* - **变异 2/2 红（各带恢复复验）**：M1 记录器忽略调用方状态（`episode = nullptr`）⇒ 设备用例红
* **4. 没做到的（登记）**：`lights_dropped_episode` / `empty_rectangle_episode` 的**管线**只有代码审查，
* 没有自己的用例（两条句子的触发都要真设备 + 一次"灯装不下"/"矩形为空"的计划；本轮预算给了半分片那两条）。
* **触发器**：任何一次"同一条灯/矩形警告每帧重复"的宿主报告，或下次给 `ContentAssembly` 加用例时。
* **（已由 §11.16cn 收掉：两条句子各有跨帧 + 重臂的用例，变异 2/2 红。）**
* 其余按 §11.16bw 与各片"登记"。
* 原来只断言"这条句子报了一次"，现在再录一帧（新记录器、同一半片状态）断言**不再报**，然后 `rearm()` 再录一帧断言**又报**。
* **下一步（本片之后）**：任务列表里的 B（D2 高光 alpha）、C（B5 拒绝路径 + 预算）、D（B4 引擎收集分配，先测），

### 11.16cj M11n（2026-09-25）：高光强度真的接上了（收掉 D2）
* **登记来自哪里**：§11.16bw 表的 **D2** 行：`Material::specular()` 的文档写着"A 是强度"，而**没有任何着色器读
* 它**（`builtin_forward.frag` 与 `builtin_gbuffer.frag` 都只读 `.rgb`）。登记给了两条路："要么接线，要么改 文档"。
* - **变异 2/2 红**：M1 前向去掉强度 ⇒ 读回 **0.5**（消息自带 `0.5 here means the forward program still ignores
* 其余按 §11.16bw 与各片"登记"。
* **下一步（本片之后）**：任务列表里的 C（B5 拒绝路径逐命令上报 + 块预算）、D（B4 引擎侧收集分配，**先测**），

### 11.16ck M11o（2026-09-25）：拒绝报告"先说一遍，再说一共几条"（收掉 B5 的前半；预算那一半不改）
* **登记来自哪里**：§11.16bw 表的 **B5** 行前半：拒绝路径**逐命令**上报（`ContentPass.cpp` 里 ~35 个
* `reportRefused` 调用点），而"场景里有坏内容"时这就是刷屏；登记给的形状是"按'每 pass 每原因一次'上报 （保留第一条的完整身份，后续只计数）"。**后半（块预算 1024/256 的硬上限）不改**——代码里早就写明了理由 （`BlockStorage.hpp` 文件注记："A block larger than its region's stride, and the (blocks_per_frame)th view or draw block of one frame. Both are counted rather than accommodated: **growing the buffer would move bytes a submitted command buffer still names**"）——这是**有意的设计决定**，不是待修的洞；本片把这句话与 B5 的 登记对齐（登记里"按需增长"的方向与它冲突，理由在实现里）。
* - **变异 1/1 红**：`noteRefusal` 每次都返回 true（回到逐命令上报）⇒ 消息变成 **4 条**，用例红；恢复即绿。
* **3. 没做（登记）。** 预算的"按需增长"与实现里的理由冲突，**保持现状**；若将来真撞上 1024 条/帧
* （触发器：宿主报"内容被拒"且原因里出现 `the frame's block budget is full`），要走的不是"悄悄长大"， 而是**双缓冲/新 buffer + retirement**（同 `MaterialArena` 的轮转思路），那是另一片的量级。 **（2026-09-25：已由 §11.16ct 落地——拒写仍按帧成立，增长是帧与帧之间的整体替换。）**
* （`std::vector<RefusalRow>{what, why, count}`）：`reportRefused` 的第一次**照旧完整报**（点名那条命令）， 同一 `what` 的后续**只加计数**；pass 结束时（`record` 里的 RAII 守卫，任何 return 路径都跑）每个 count ≥ 2 的原因**报一行汇总**："N drawing call(s) were not drawn for one reason - <what> is not drawn: <why>（第一条在上面；其余是同一条事实）"。诊断计数跟着消息走（2 条消息 = 2 个计数）。
* 表里答得上几何与材质、**答不上**程序 ⇒ 3 条命令同一个原因被拒（且给一趟一个可服务的半片，否则记录器 会把**整趟**用一句话拒掉——那是它自己的收敛，见那条分支）；断言**恰好 2 条消息**（第一条点名 "the command's program"，第二条说 "3 drawing call(s)…"）且 `ContentSkipped` 计数 == 2。
* **下一步（本片之后）**：任务列表的 D（B4 引擎侧 `Scene::collectRenderCommandsShared`，**先测再改**）。

### 11.16cl M11p（2026-09-25）：B4 的测量——**只测不改**（引擎侧收集的真实代价）
* **登记来自哪里**：§11.16bw 表的 **B4** 行（`Scene::collectRenderCommandsShared` 每次收集分配 3 个 vector、
* 把整表搬 2~3 遍、`commands` 无 `reserve`），它自己写着"**引擎侧（不在本轮前端改动范围内）**"且"当前 demo 的 收集是 memo 命中或 ~42 条命令，**量不出来**"，触发器 = "相机常动的负载 + drawable 数越过 ~2000"。 2026-09-25 决定：**先测、不改引擎**（用户明确要求 `src/viz/graphics` 不随意改动）。
* `tests/test_graphics/GraphicsTest.cpp` 的 `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera`： N 个三角形（网格铺开、全部在视锥内、距离各不同 ⇒ 排序有真活）装在同一个 root 下；打开一个 content frame （`Scene::setContentFrame(1)`，memo 只在帧内有效——**第一次写这个测量时忘了它，结果"静止相机"也全量重收**， 这一步是测量能不能读的关键）；相机每帧移动一点点（memo 键 = revision + eye + view_proj ⇒ 必 miss）收 10 次， 再静止着收 10 次；断言的全是**机器无关**的事实（移动的每次都真的走树：`contentCollectCount()` 每次 +1；静止的 走 memo：`contentCollectReuseCount()` +9；两种视图命令数相同），耗时只打印。 跑法：`./build/bin/test_graphics --gtest_filter='*MeasureWhatOneCollectionCosts*'`（Release 更有意义）。
* **2. 实测数字（本机，lavapipe 无关——这是纯 CPU 的树遍历）。**
* ⇒ 边际成本 ≈ **(1047 − 85) / 1800 ≈ 0.53 µs 每 drawable 每次收集**（Release）； ⇒ demo 量级（~42 命令）外推 ≈ **18 µs/帧 ≈ 0.1% 的 16.6 ms 帧**——这就是"demo 上量不出来"的量化说法； ⇒ 登记触发点（2000 drawable + 相机常动）≈ **1.05 ms/帧 ≈ 6% 的 60 fps 预算**：有代价，但**未达**"必须改"。
* **3. 决定与形状（登记更新）。** **不改**：把 B4 从"缺陷（登记）"改成"**已测，代价 0.53 µs/drawable·次；
* 触发点约 6% 帧预算**"。要改时的形状与登记一致（`commands` 按上一帧规模 `reserve` ⇒ 省掉倍增重分配与 ~400 KB 的搬运；`keyed` 改行号 + 就地置换 ⇒ 省一次 N 条 `RenderCommand` 的移动）——**那是引擎侧的另一片**， 需要单独的批准与它自己的像素/顺序用例。
* 各片"登记"（D3/D5、A4/A6、首帧 gizmo 空白、会话侧"画面已落地"的事实、`lights_dropped`/空矩形两条句子的用例）。
* **1. 测量配方（新加在引擎自己的套件里，只打印、不断言机器相关的数字）。**
* **下一步**：任务列表 1–4 全部收到（A §11.16ci、B §11.16cj、C §11.16ck、D 本节）；仍未做的按 §11.16bw 与

### 11.16cm M11q（2026-09-25）：**收益分析**——剩下的登记各值多少（能测的都测了）
* **为什么要写这一节**：任务列表 1–4 收完之后，剩下的登记（D3/D5、A4/A6、B5 后半、B4 的修、首帧 gizmo、会话侧
* "画面已落地"、两条句子的用例）都带"触发器"，但触发器不等于**收益**。本节把能测的测了，给出每个候选项的 "收益 / 代价"，作为下一片的依据。数字分两类：**本机实测**与**由已有实测推的上界**（标明出处）。
* M11h 空调用 — 应用日志 warning **1 → 0**（第二条画面判据也从此不再被误判）
* M11j + M11l 窗口读 — 单用例 **4/12 红 → 0/12**、三套件组 **2/3 → 3/3**、全量 **4 跑里 3 跑红 → 4 跑全绿**；门禁新增第三条判据能看见"整片背景灰"（变异下 `background 25.64%`，旧判据全过）
* M11o 拒绝账本 — 同一原因 N 条命令：**N 条消息 → 2 条**（实测 3 条 ⇒ 2 条）
* M11p B4 测量 — "要不要修"从猜变成数：**0.53 µs/drawable·次**，demo 量级 ≈0.1% 帧预算
* **D5** albedo 附件 RGBA8 → RGBA16F — ★ **实测**：0.06 的暗色阶在 RGBA8 里只有 **16 级 / 64 步**、相邻步长正好 **1/255** ⇒ 暗端**相对跳变 6.5%**（人眼看得见的条带）；RGBA16F 在同量级的步长 ≈2^-17 ≈ **0.013%**（不可见） | 每张 G-buffer 颜色附件 **+8.3 MB**（1080p：8.29 → 16.6 MB）+ 目标形状进管线键 ⇒ 全部离屏管线**重编一次**（一次性） | **值得做**（唯一的"看得见画质"的项；代价小且一次性）
* **B5 后半**（块预算按需增长） — 1024 draw/帧的上限在**B4 的触发规模（2000 drawable）上会拒掉约一半绘制**（拒画有报告、有计数） | 新 buffer + retirement（照 `MaterialArena` 的轮转形状）；已写明"跨帧换 buffer 会动到已提交命令缓冲命名的字节" | 中：第一个大场景宿主出现时做
* **A4** `Material` revision（**已实测：§11.16cq**——一次材质编辑 = 每帧 +1 行重建、**0 上传、0 变体**；record 半段 7–19 µs/帧（Release, 1 drawable）） — 每帧的上界：demo 规模 11 趟 × 42 命令 ≈ **460 次 64 B 比较 ≈ 10 µs/帧 ≈ 0.06%**（由 M10f 实测的 2 ms/帧内容层推） | 新公开 API + 文档 | **不值得**：数字比噪声还小
* **B4 的修**（reserve + 就地置换） — 省掉倍增重分配与 ~400 KB 搬运 ≈ **1.05 ms 里的 5–8%**（M11p 实测） | 引擎侧改动 + 顺序用例 | 不在本轮（触发点只到 6% 帧预算）
* **A6** LRU / 容量上界 — 只在"同屏活纹理逼近 256"时才有面（demo 不成立） | 淘汰键改造 | 保持登记
* **D3** 色彩空间契约（**已做：§11.16cp**，M11t） — demo 的天空从亮一个 gamma 回到正确亮度（视口带 mean 122.7 → 80.8），门禁判据一行不动 | 文档 + demo 三处声明；起作用的映射早有用例 pin | **已做**：契约写进四处 SDK 注释 + `usage.md` §3.9；③（`acquire` 的 Info）以"引擎分不出颜色/数据"为由不做
* ②**两条句子的用例**（最便宜的守卫补齐）；③**D3 写契约**（便宜），其余按各自触发器。
* **会话侧"画面已落地"** — 把窗口读那条链**从"测试碰运气"变成"宿主可查的事实"**（测试侧已 4/12 → 0/12，剩下的价值在宿主 API） | WSI present fence（中等） | 中的：下一个真宿主接窗口时做

### 11.16cn M11r（2026-09-25）：两条会话句子的守卫（补 §11.16ci 的缺口）
* **登记来自哪里**：§11.16ci 第 4 条自己写着：`lights_dropped_episode` / `empty_rectangle_episode` 两条管线
* **2. 变异 2/2 红（各带恢复复验）。** M1 空矩形忽略调用方句柄（退回每 `ContentPass` 一份）⇒ 第二帧又说一遍；
* **3. 写这个用例踩到的两个坑（都记进仓库记忆）。**
* +8.3 MB/张 + 一次重编）②**D3** 写契约（便宜）③其余按各自触发器。
* **下一步**：按 §11.16cm 的收益排序 —— ①**D5**（唯一看得见的画质收益：暗端相对跳变 6.5% → 0.013%，代价

### 11.16co M11s（2026-09-25）：D5 落地——albedo 附件换半浮点（**先量端到端，再改**）
* **登记来自哪里**：§11.16bw 的 **D5** 行："albedo 附件是 `RGBA8` 而存的是**线性** albedo……收益只在极暗材质上
* **1. 端到端测量（本片的判据，两个光强区制）。** `ContentPassTest.MeasureWhatAHalfFloatAlbedoWouldBuyEndToEnd`：
* （代价：1080p 每张 G-buffer **+8.3 MB**；目标形状进管线键 ⇒ 全部离屏管线**重编一次**，一次性）。 引擎自己的用例 `RenderPipelineBuilderTest.DefaultGbufferTargetIsCanonicalLayout` 现在把**四个附件的格式** 当布局的一部分断言下来（附理由与出处）。**变异 1/1 红**：把 att 0 改回 `RGBA8` ⇒ `colorFormat(0)` 断言红。
* M11l 的 Debug 门禁与本次 Release 门禁各首跑红一次、重跑即绿（门禁照规矩把首跑失败具名写进证据行）。 本次量了：**单独跑 12/12 绿** ⇒ 按 §11.16cf 的判法属**环境/顺序类**。判据不足（门禁只留用例名，不留断言 文本）⇒ 复现时先打印"失败的那条断言 + 设备的 `minUniformBufferOffsetAlignment`"。
* **下一步**：按 §11.16cm 排序，①D3 写色彩空间契约（便宜）②B5 后半（第一个大场景宿主出现时）③其余按触发器；

### §11.16cp D3：色彩空间契约写下来了，仓库里第一个"错法的实例"就是 demo 自己（M11t，2026-09-25）
* **登记来自哪里**：§11.16bw 的 **D3** 行："真正缺的是**把这条写成契约**"。形状①文档、②`Colorf`、
* 数值和传递函数对得上：编码值 0.5 被当成线性 ⇒ 显示 188（`E(0.5)=0.74`），解码后应为 128 ⇒ 暗约 32%， 实测视口带 −34%。
* **4. 变异证明"解码就是这个映射干的"（1/1 红）。** 把 `vkFormatFor` 的 `Rgba8Srgb` 唯一锚点改成
* * 恢复（`cp` 备份回填 + 重编）：`grep -c MUTANT` = 0、单元绿、重测画面与变异前**逐字节相同**。
* **下一步**：D3 收口 ⇒ §11.16cm 的排序里只剩 **B5 后半**（等第一个大场景宿主）与各自触发器的项；
* `BlockStorageTest` 那条偶发仍等第三次出现（复用 §11.16co 的判据）。
* `VsgSceneRulesTest.MapsEveryPixelLayoutToItsVulkanFormat` 断言 `vkFormatFor(Rgba8Srgb) == VK_FORMAT_R8G8B8A8_SRGB` （`tests/test_vsg/SceneRulesTest.cpp:753`），上传路径（`MaterialImages`）用的就是 `vkFormatFor(texture->format())`。
* **6. ③（`MaterialImages::acquire` 的 Info）不做，理由写下。** 引擎**分不出**颜色图和数据图：同一个

### §11.16cq 每帧变化的成本：四类编辑的实测 + 键审计落地（M11u，2026-09-25）
* **问题**：宿主每帧改一样东西——材质值、Geometry 的顶点字节、StateNode 的状态、节点的 program——后端必须
* 对象与就地改写字节一样贵**——因为 `StreamKey.revision` 取的是 **Geometry 的 revision** （`GeometryFacts.cpp:107/178`）：一次公告移动了所有流的身份。**更正（§11.16cr）**：本行当时接着写了"就地 那条路的收益只是节点 Refresh 而不是 Rebuild"——**这是错的**：那条 Refresh 路径（`planGeometry`）没有任何 生产调用方，本片量到的每一次几何编辑都是**重建**（新 revision ⇒ 新行 + 节点重造）。"逐 `Buffer::revision()`" 的登记（**B6**）不受影响：那是一条真实存在的省法（未改通道不必重传），与这条不存在的刷新路径无关。 （触发器：多通道大网格宿主报带宽。） （该机制已删：§11.16cr）
* `offscreen_builds`、`offscreen_resizes`、`window_builds`、`program_slot_builds`（`Observe.hpp:38-43` 声明并 写文档，`PhaseTable.hpp:26` 还拿 `offscreen_builds` 举例说"phase 必须能拦住它"）——全仓库没有一处写、没有 一处读；相位用的是自己的 `DevicePhaseCounters`（`tests/test_vsg/DevicePhases.hpp`）。⇒ 那些规则永远不会响。 要么接线（`ContentStore`/`Streams`/`HostTargets` 都有现成计数点——本配方想要的"Refresh vs Rebuild"正是 `streams_refreshed`/`data_nodes_built` 能答的）要么删掉；触发器：下一次有人想按"帧里的重建次数"写规则。 （该机制已删：§11.16cr）
* **下一步**：排序表只剩 **B5 后半**（等第一个大场景宿主）；新登记 B6/B7/A8 按各自触发器。
* （1024 顶点 = 12 KiB + 5766 索引），一个 pass 一个 drawable。先 6 帧热机，然后 6 个区制各 12 帧，每帧只改 一样东西；时间把一帧切成两半——**record**（`beginFrame…endFrame`：facts/计划/块/流）与 **commit** （`swapBuffers`：记录+提交+呈现）——并在每个区制前后读三组**活的**计数：`ContentStore::builds()`、 `StreamUploads::uploads()`、`VariantPool::created()/reused()`。数字打印出来供下表；断言全是机器无关的。
* 过的字节真的送到了帧里**）；状态区制打开 Back 面剔除后网格仍在（**状态真的到了光栅化器**）。后者抓到过 真错：第一版网格索引是**顺时针**，Back 剔除把整片网格剔没了——这条断言不是装饰。

### §11.16cr 死机制退场：无生产调用方的"几何刷新计划" + 九个永远为零的计数（M11v，2026-09-25）
* 一次公告移动**所有**流的身份（§11.16cq 实测 2 上传/帧，替换 buffer 与就地改写一样贵）。能省带宽的是另一件事 （逐 `Buffer::revision()`，登记 **B6**），与这个刷新路径无关。
* **规则不能丢：换成活路径钉住。** 被删的断言里有一条真规则——"**被重填的 buffer 是新流，不是旧副本**"。
* 它现在由 `CoreSharedStreamsTest.ARefilledSliceIsANewStreamSoItUploadsAgain`（真注册表：同键 ⇒ Alias， revision 变了 ⇒ **Upload**）钉住。**变异 1/1 红**：把 `revision` 从 `StreamKey` 的相等与哈希里拿掉 ⇒ 该用例红 （其余 7 条共享流用例仍绿）；恢复 ⇒ 9/9 绿。

### §11.16cs 应用窗口的启动尺寸不是确定的：门禁先把尺寸钉住（M11v 附带，2026-09-25）
* 六个渲染区：`378x247 / 1037x509 / 491x319 / 378x406 / 558x247 /`（门禁那次）`3418x1110`。即：这不是后端缺陷， 而是"宿主窗口起始尺寸由 Qt 布局竞态决定"，而门禁的第一条判据（`content ≥30%`）默认它接近全窗。
* **修复的判据**：修复前后各测一组——修复前 6 次启动 4 次读出不同尺寸；修复后 **4/4 都是 378x247**；两次门禁的
* **登记（不修，属宿主/框架）**：demo 的窗口会随日志 dock 的内容长大（一次启动可以长到 3418×1110）——"窗口尺寸
* 跟着日志文字走"是宿主侧该收的口子（框架的默认窗口尺寸是产品决定）。**触发器**：下一次 app shell / 框架的窗口 布局工作；复现数字即本节那六个渲染区。

### §11.16ct M11w（2026-09-25）：块预算按需增长——跨帧换新 buffer + 停放旧 storage 与旧 set（收掉 B5 后半）

* **登记来自哪里**：§6 的 B5 后半 / §11.16ck（前半已收，当时判"预算的按需增长是另一片"）。本片按登记里的形状落地：
  拒写仍**按帧**成立，增长发生在**帧与帧之间**（新 buffer + retirement）。
* **规则（新的契约）**
  * `BlockStorage` 记住每帧**试了**多少块（每个 ring region 各计），撞预算时把"最坏一帧的尝试数"存进
    `growthNeeded()`（max over frames，**不重置**：请求靠"换成新 storage"兑现，替换件从零开始）。
  * 调用方（`VsgBackend::growBlockStorageIfNeeded`，在 `beginFrame()` 里）用 `grownLayout()` 算新预算：
    `max(need, 2 x budget)`（doubling 让增长是**稀有事件**；need 更大时一次到位）。只长 `need` 点名的 region。
  * 替换 = **新 buffer** + `ContentAssembly::repoint`（新 storage 的 buffer 绑进每个缓存 set）+ 旧 storage 与
    旧 set 都进**退役队列**；无停放窗口时"留着"（`kept_storage` / `kept_replaced`），绝不早放。
  * 增长本身报一条 `Info/ContentSkipped`（预算耗尽 ⇒ 存储长大 + 各 region 数字）：每 pass 的拒绝警告说的是
    "这一帧丢了内容"，这条说的是"为什么以后不再丢"。
* **关键实现事实（§5.4 也记了）**：`repoint` 换 set 时**必须停放旧 set**。vsg 的池会把**已被释放** set 的
  `VkDescriptorSet` 句柄发给新 set，而新 set 的 `compile` 发生在**下一帧**——那时被换下的帧仍是 pending ⇒
  `vkUpdateDescriptorSets` 打在在用的句柄上 = `03047`（实测：本片第一版，验证层 2 条；用例当时全绿）。
  命令缓冲对 vsg 对象（`BindDescriptorSet` 的 `ref_ptr<DescriptorSet>`）的引用**不足以**保住句柄。
* **判据**（三个新用例，全部带真设备；两端都有像素）：
  * `BlockStorageTest.AFramePastItsBudgetIsRefusedButAsksForAReplacement`：拒写 + `overflows` + buffer 不动；
    请求 = 最坏一帧的尝试（3 块）；跨帧不退；`grownLayout` 给 2→4；替换件三条都装下且无请求。
  * `BlockStorageTest.TheGrowthPolicyDoublesWhileItCoversAndTakesABiggerNeed`（**设备无关**）：doubling、
    大 need 一次到位、未点名的 region 不动。
  * `VsgBackendTest.AFrameThatRanOutOfBudgetGrowsTheStorageBeforeTheNextFrame`：小预算（draws=1）→ 帧 1：第二条
    draw 被拒（1 条诊断 + 像素=第一条的颜色）；帧 2 的 `beginFrame()` 换 storage（指针变、容量涨、布局 1→2）→
    像素=第二条的颜色（两条 draw 都进了新 buffer，且 set 真的被 repoint）+ 第 2 条诊断=增长；`deviceWaits()==0`。
  * 测试接缝：`BackendContentAccess::storage()` / `useBlockStorage(backend, layout)`（走与增长**同一条** adopt 路径，
    不用真的画 1025 条）。
* **数字**：`test_vsg` 437 → **439**；两棵树门禁 `cases=439 failed=0 vuid=0 hazard=0 skipped=0`、hygiene 0/793、
  应用行与历史逐字相同（`87.04%/244`）。
* **变异 4/4 红**：M1 `growBlockStorageIfNeeded` 恒早退、M2 adopt 不 repoint、M3 策略恒不长大、M4 **不停放被换下的
  set**（M4 用例全绿、验证层 1 条 —— 正是"只有门禁看得见"的那一类）。
* **口子（登记）**：①撞预算的那一帧仍丢内容（有报告 + 计数；帧内不搬字节是有意的）；②`ContentSets::kept_replaced`
  是"无停放窗口"的兜底（随增长事件增长，不随帧）；③直连 `ContentAssembly` 的调用方要自己调 `repoint`。

### §11.16cu M11x（2026-09-25）：B5 的触发规模做成配方——一个 drawing call 装 2000 条命令（一次增长收口）

* **登记来自哪里**：§11.16cm 的收益表把"1024 draw/帧的上限在 2000 drawable 规模上会拒掉约一半绘制"列为 B5 后半的触发面；
  §11.16ct 落地了机制，本片把**触发规模本身**放进常驻套件。
* **配方**（`VsgBackendTest.TheDocumentedScaleGrowsTheDrawBudgetOnceAndThenServesEveryCommand`）：一个 pass、一个
  program、**一次 `render()` 带 2000 条命令**（= 文档点名的规模），默认预算（views 256 / draws·lights·shadows 各
  1024）。
* **实测（lavapipe，Debug + 验证层）**：帧 1 的 2000 个 draw block **976 被拒**（首条点名 `the command's draw
  block` + 一条 "976 drawing call(s)…" 汇总）；同一 drawing call 的 **1 个 light 块与 1 个 shadow 块**装得下——
  块的口径是"每 call 一块（灯/影）+ 每命令一块（draw）"，所以撞顶只落在 draw region。帧 2 的 `beginFrame()`
  替换存储（draws 1024 → 2048，doubling 已覆盖 2000）后**同一内容全部装下**（`overflows()==0`、无新请求）；
  帧 3 稳态（不替换、不报）。诊断全书 = **3 条**（2 次拒绝 + 1 次增长）。
* **数字（只打印）**：record ≈ **2–5 ms/帧**、commit ≈ **84–179 ms/帧**（首帧含管道编译；其余是软件光栅器画
  2000 个重叠三角形）。
* **结论**：跨过文档规模是**一次**增长事件，而不是"每个 region 一帧"——增长半径按"本帧真正试过的 region"兑现，
  正常场景的压力只在 draw 块上；若三个 region 都被压满，仍会逐帧逐个兑现（区域隔离由策略用例与 `grow_*` 断言守着）。
* **方法与教训**：第一版配方按"每条命令是一个 drawing call"预测（三帧、每帧长一个 region），实测全相反——
  **量出来的才是口径**：一次 `render()` = 一个 drawing call（灯/影每 call 一块），命令只是它的 draw 块。
* **判据**：套件 439 → **440**；两棵树门禁 `cases=440 failed=0 vuid=0 hazard=0`、应用行与历史逐字相同；本片
  **无源改动**（只加配方与记录）。

### §11.16cv M11y（2026-09-25）：单附件内容绘制的混合因子读回像素（收掉图形侧对账的尾项）

* **登记来自哪里**：图形侧登记对账（commit `aa1176f`）把 `graphics-state.md` 的旧声明“blend 因子的真机视觉
  验证未做”改成“映射单测钉住、像素专项仍无”——本片把这条尾项收掉。
* **用例**（`VsgBackendTest.TheSingleAttachmentContentDrawBlendsBySrcAlphaAndThePixelsSaySo`）：一个 pass、
  一个内容绘制，fragment 写 `(1,0,0,0.5)`，清屏 **蓝**。混合按 `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA` 生效 ⇒
  中心像素两个外通道各 **0.5**（读回接受 0.5 的 linear/sRGB 两种拼法，但不接受 1.0 的）；角落仍是清屏蓝。
  两个外通道都是 0.5 ⇒ **与字节序无关**。
* **判据为何钉的是因子而不是“有没有混合”**：把 `StateCommands.cpp` 的 src 因子改成 `ONE`，或把单附件
  `blendEnable` 关掉，同一探测点会读到 **1.0 红 / 0.0 蓝**（变异 M1/M2，都实测红）。
* **判据**：套件 440 → **441**；两棵树门禁 `cases=441 failed=0 vuid=0 hazard=0`、应用行与历史逐字相同；
  本片**无源改动**（只加用例与记录）。

### §11.16cw M11z（2026-09-25）：B6 的触发面先量出来——多通道大网格一次公告的字节账（只加配方，不实现）

* **登记来自哪里**：§6 的 B6 行把“逐 `Buffer::revision()`”挂在“多通道大网格宿主报上传带宽”上；
  实现前先把“宿主会报多大的带宽”量成曲线。
* **代码事实**（重新核实过）：`src/api/GeometryFacts.cpp` 里每个通道与索引流的 `key.revision` 都取
  `geometry.revision()` ⇒ 一次 `bumpRevision()` 动**所有**流的身份，下一帧整网格重传。
* **配方**（`VsgBackendTest.MeasureWhatAMultiChannelMeshCostsWhenOneChannelChanges`）：512² 网格
  （262144 顶点）× 四个正典通道（位置/法线/色/UV）+ 整索引 buffer；绘制只取索引切片前 6 个
  （两大三角形盖屏）⇒ 光栅成本≈ 0，而索引**流**仍是整 buffer（键归一化到整 buffer）。三个区制：稳态 /
  只改位置 / 四通道全改。
* **实测**（lavapipe；Debug 与 Release 同量级，此处用 Release）：一次公告重传 **5 流 = 16.98 MiB**，
  commit **33.6 ms/帧** vs 稳态 3.2 ms ⇒ **+30.4 ms/帧**；只改一格与全改**同价**（上传数都 = 5/帧，
  `created` 都 = 0）。标定：256² ⇒ 4.24 MiB ⇒ +11..15 ms/帧 ⇒ **按字节近线性**；60 fps 下 512² 规模
  ≈ **1 GB/s** 的名目带宽（只改位置时其中约 82% 是未改通道的字节）。record 半段始终 µs 级。
* **判断**：成本真实且随规模线性 ⇒ B6 真动手时，同场景可省掉约 80% 的上传字节。**但触发器未按字面兑现**
  （没有宿主来报），所以本片只量化，不动契约；真要改，先用“逐 Buffer revision + 漏报如何被抓住”回答
  契约变弱一项。
* **判据**：套件 441 → **442**；两棵树门禁 `cases=442 vuid=0 hazard=0`、应用行与历史逐字相同；
  本片**无源改动**（只加配方与记录）。

### §11.16cx M11aa（2026-09-25）：命令成本地板与材料 arena 的 256 槽边界（A4 触发面量化，只加配方）

* **登记来自哪里**：§6 的 A4 行——“材质数量大到每帧 O(命令数) 次块比较进入剖析前列”。
* **配方**（`VsgBackendTest.MeasureWhatCommandsCostWhenTheirDrawingLandsNothing`）：三个**完全相同的顶点**
  （零面积三角形 ⇒ 不落任何片元），剩下的就是逐命令地板（draw 块、材料 note、绑定、draw call）。
  五个区制：1 条命令 / 2000 条同材质 / 200 个不同材料 / 300 个（超 256 边界）/ 2000 个。
* **实测**（lavapipe；Release 为主，Debug 同量级）：**命令地板 ≈ 28.0 µs/条**
  （(57.6 ms − 1.56 ms)/1999；Debug ≈ 38.7 µs）——2000 条**不落一像素**的命令本身 = **56 ms/帧**。
  2000 个不同材料相对同材质再 **+5.4 µs/条**。
* **材料 arena 边界**（默认 256 槽 FIFO，`BlockStorage::Layout` 的 `materials{64,3,256}`）：200 个材料 ⇒
  每帧 200 命中、0 写、0 驱逐；**300 个 ⇒ 每帧 0 命中、300 写、300 驱逐**；2000 个 ⇒ 0 命中、2000 写、2000 驱逐
  ——超过容量后场景**永远没有稳态帧**（FIFO 级联让每个材料在轮到自己之前就被驱逐）。
* **顺带量到的事实**：storage **没有任何释放路径**（`MaterialArena::release` 无调用者）——槽位只经驱逐
  离场；这也是 200 个活材料读出 201 槽的原因（上一区制的一个材料仍驻留）。
* **判据**：套件 442 → **443**；两棵树门禁 `cases=443 vuid=0 hazard=0`、应用行与历史逐字相同；
  变异（arena 容量 256→400）⇒ 配方红，恢复绿；本片**无源改动**（只加配方与记录）。

### §11.16cy M11ab（2026-09-25）：纹理缓存的 FIFO 语义钉住 + 超界重建成本（A6 触发面量化）

* **登记来自哪里**：§6 的 A6 行——“`MaterialImages` 淘汰是 FIFO 而非 LRU；… | 同屏活纹理逼近 256”。
* **配方**（`VsgBackendTest.TheTextureCacheDropsTheOldestInsertionAndRebuildsWhatItDropped`；不画任何东西，只用缓存自己的
  词汇 `has()/count()`）：
  * ①**A/B/X 判别**：A、B、再 254 张填充（满 256）⇒ **重新 acquire A（命中）** ⇒ 再加 X ⇒ **A 被驱逐、B 仍在**
    ——命中不刷新时间戳（FIFO）；LRU 化（M1 变异）⇒ 红。
  * ②**200 张（64×64）循环**：第一轮 200 缺失，第二轮 **0 缺失**（0.10 ms/轮）。
  * ③**300 张循环**：第一轮尾部 256 存活（`has([0])==false`、`[44]/[299]` 真），第二轮 **300/300 全缺失**
    （3.91 ms/轮 vs 匹配时的 0.10 ms）⇒ **超界后每轮把每张纹理都重建**（~12.7 µs/张，lavapipe）。
* **变异**：M1（命中刷新时间戳）与 M2（`kMaxEntries` 256→400）都红；本片无源改动。
* **备注**：本配方钉的是**缓存层**契约；一帧内是否真的发生这些 acquire（描述符集缓存复用时可能不再 acquire）不在本配方
  里——“同屏活纹理逼近 256”的宿主侧影响仍待真场景触发。
* **判据**：套件 443 → **444**；两棵树门禁 `cases=444 vuid=0 hazard=0`、应用行与历史逐字相同。

### §11.16cz M11ac（2026-09-25）：重复创建+丢弃的账本（A3 触发面量化）

* **登记来自哪里**：§6 的 A3 行——“SDK 没有内容释放入口…| 宿主报告内存压力”。宿主唯一的“释放”=丢掉自己的
  引用；本片把“丢掉之后各层留下什么”做成账本。
* **配方**（`VsgBackendTest.MeasureWhatRepeatedCreateAndDropLeavesBehind`）：每轮新建 geometry+material+program+
  texture，画一帧（零面积三角形），然后全部丢引用；结算 16 帧后读账本（lavapipe，连跑三次全同）。
* **实测**：
  * 每轮结束：表行 **g0 m0 p0**、纹理 **images 0**、流 **streams 0**——全部回到基线；
  * 计数器：`releasedContentObjects` **+3/轮**、`releasedTextures` **+1/轮**、`releasedStreams` **+2/轮**；
  * **唯一残留**：材料 arena 槽 **12/12**（每轮一个，只经驱逐离场，§11.16cx）。
* **顺带发现（保留式设计，不是缺陷）**：在会话**学到 in-flight slot 数之前**被丢弃的内容无法停放
  （`RetirementQueue::retire` 拒绝猜），行按设计保留整会话（`retained()` 3 条；`ContentStore::retained` 注释已写
  “costs memory, never correctness”）。配方用“16 个空帧 + 1 个热身轮”吸收该窗口；测量轮 `retained` 零增长。
* **顺手修的源码小疵**：`ContentSweep.cpp` 里 “The store first, and it parks…” 注释重复了一遍（注释级去重，零行为）。
* **变异**：M1（跳过纹理释放）与 M2（跳过内容释放）都红。
* **判据**：套件 444 → **445**；两棵树门禁 `cases=445 vuid=0 hazard=0`、应用行与历史逐字相同。

### §11.16da M11ad（2026-09-25）：BlockStorageTest 偶发的证据预置（只改测试）

* **登记来自哪里**：§6 的“BlockStorageTest 偶发”行——同一用例首跑红/重跑绿出现过两次，登记的动作是
  “第三次出现时打印失败断言 + 设备 minUniformBufferAlignment”。
* **预置**（`TheRegionsAreLaidOutOnceAndDoNotOverlap`）：无论成败都打印一行
  `[storage] minUniformBufferOffsetAlignment=… regions… strides… capacity…`，并用 `SCOPED_TRACE` 携带同一份
  证据进任何失败——下次红了直接能在日志/失败消息里重建布局算式。（`VkPhysicalDeviceLimits` 里并没有
  `minUniformBufferAlignment` 这个字段；影响布局的是 `minUniformBufferOffsetAlignment`，探针印的是它。）
* **复现尝试**：40 次全新进程单跑 ⇒ **0 红**（本环境本次未复现）。
* **判据**：套件仍 **445**（无新用例）；两棵树门禁 `cases=445 vuid=0 hazard=0`、应用行逐字不变。

### §11.16db M11ae（2026-09-25）：首帧的叠层是什么、不是什么——探针把“尺寸未知”改写成“瞬态尺寸放不下”

* **登记来自哪里**：§6 的“首帧 gizmo 空白 | 一帧没有工具叠层 | 低（下一次 app shell 视觉工作）”，
  以及 §11.16cd（M11h）当时的读法“`AxisGizmo::execute` 在 `surface_w_ <= 0` 时不会设视口，那一帧的场景收集
  因此是空的”。
* **探针（临时，跑完即撤）**：在 `AxisGizmo::onSurfaceResized` 与 `execute` 各打前 8 次调用（尺寸 + 视口），
  与 `[RenderControl] surface … -> …` 状态行对齐时间戳。实测（本机，xcb）：
  * `Pending -> Attached`（表面 160×160）→ `onSurfaceResized#1 160×160` → `execute#1 vp=16,48 96×96`
    ⇒ **预热帧的叠层是好的**（“尺寸未知”不是今天的形态）；
  * `Startup frame going away` 之后：`onSurfaceResized#2 100×30`（Qt 布局**瞬态**）→
    `execute#2 vp=16,16 -2×-2` ⇒ 第一个**上屏**帧（`Attached -> Presenting` 紧随其后）画在 100×30 的瞬态窗口里，
    角落 HUD 盒放不下，旧算式 `dev_h - 2*margin_px_` 在那里是**负数**；
  * 随后 `onSurfaceResized#3 378×247` → `execute#3…6 vp=16,135 96×96`（settle 帧起一切正常）。
  * 旁证：应用日志里那条 `[graphics] a drawing call was not recorded: its rectangle is empty` 的 Info
    （`ContentPass.cpp:416` 的空矩形守卫）就是这帧的负矩形触发的——每会话一条。
* ⇒ **结论改写**：不是“表面尺寸未知”，而是“**布局瞬态尺寸下 HUD 盒放不下**”；用户可见性≈零
  （100×30 的一帧本来就无画可看，settle 帧即正常）。**真正的缺陷是那道负视口**：把“放不下”拼成了
  `-2×-2`，交给后端的空矩形守卫兜底。
* **修**（`AxisGizmo::onSurfaceResized`）：`side = fitted > 0 ? min(size_px_, fitted) : 0`——放不下就老实给
  **零面积**矩形（下游同样跳过绘制，但那是一个说出来的决定，不是垃圾入参）。`FpsOverlay` 的同类边界**保持原样**
  （放不下时保留上一矩形，后端会把视口夹进目标、被裁剪；差别只在瞬态一帧里可见，未统一，此句即登记）。
* **门禁**：`AxisGizmoTest.ASurfaceTooSmallForTheBoxNeverYieldsANegativeViewport`（100×30 ⇒ 0×0；100×128
  ⇒ 96×96 不误伤）。**变异 1/1 红**：把旧算式放回 ⇒ 该用例红；恢复 ⇒ `AxisGizmoTest.*` 4/4 绿。
* **判据**：`test_graphics` 两棵树 **282 → 283**；两棵树门禁 `cases=445 vuid=0 hazard=0`、应用行逐字不变。

### §11.16dc M11af（2026-09-25）：应用窗口的启动尺寸从“由布局竞态决定”改成“说出来的默认”

* **登记来自哪里**：§6 的“窗口随日志长大 | 宿主/框架侧未修——默认窗口尺寸是产品决定”，
  以及 §11.16cs 的记录（六次启动六个渲染区：378×247 / 1037×509 / 491×319 / 378×406 / 558×247 / 一次门禁 3418×1110；
  门禁因此**自己**先把窗口 resize 到 800×600 再判图）。
* **修**：`MainWindow` 构造里在 `setMinimumSize(800,600)` 旁加 `resize(QSize(800,600))`——
  **800×600 = 文档默认 = 最小值 = 门禁判图尺寸**；置上 `WA_Resized` 后，首 show 不再走“按布局 sizeHint 定尺寸”
  那条路。用户之后的拖动/缩放不受影响。
* **实测（诚实记录）**：修前后各跑一批，渲染区**都是 378×247**（修后 4 次、修前对照 6 次；另加 2 次
  “强制宽控制台”对照组）——**今天的本机没能复现漂移**（顶层链读数为 渲染区 378×247 ← 容器 800×600 ←
  顶层 864×664，两配置一致）。⇒ 改动按**构造**去掉机制，而不是靠复现；§11.16cs 那组测量仍是漂移存在的记录。
* **门禁**：`MainWindowTest.TheOpeningSizeIsStatedInsteadOfInheritedFromTheLayout`（构造后
  `minimumSize==size==800×600` 且 `WA_Resized` 已置）。**变异 1/1 红**：删掉 `resize` 行 ⇒
  size 回 Qt 默认 640×480、属性未置 ⇒ 该用例红（两平台都验过）；恢复 ⇒ 绿。
* **顺带修掉一条既有红（两棵树纪律的又一例）**：在 `build-release` 跑 `test_gui` 全量时
  `PluginLifecycleTest.HandwrittenRegistrationCanDisableForAllUsers` 红——断言写死了 Debug 后缀
  `test_plugind`（`808bbd2` 起），而 Release 的库叫 `test_plugin.so`，即**这条在 Release 树一直是红的**。
  改成“注册路径必须点名沙箱里那份拷贝的文件名”（期望值从拷贝自身推出），两棵树单跑 + 全量都绿。
* **判据**：`test_gui` 两棵树 **207 → 208**（新增 1 例）全绿、`test_graphics` 两棵树 283 全绿；
  两棵树门禁 `cases=445 vuid=0 hazard=0`、应用行**逐字不变**（门禁本来就把判图尺寸说成 800×600，
  改了默认之后那一步从“补偿”变成“确认”）。

### §11.16dd（2026-09-25）：每帧块存储的 slab 少一个——“最老的那一帧”才是本轮写入的读者

* **登记来自哪里**：用户报的运行时症状——**默认 demo**（Deferred + 阴影）启动后**上下拖动鼠标**（相机每帧在变），
  地板上多出一些**闪烁的**阴影，停止拖动立刻消失，而“本来该有的那道阴影”一直正常。
* **为什么只有阴影看得见（形状分析）**：延迟路径里片元的视图位置来自 G-buffer 附件，几何变换走 **push**
  （每帧由 CPU 写进命令缓冲 ⇒ 永远是当帧）；着色器真正读的**每帧 ring 数据**只有三样：阴影块、灯块、材质块。
  后两者在默认 demo 里要么不走这条路（灯在延迟路径走 128B push）、要么字节不变（材质静态），于是唯一
  “每帧都变 + 判据是硬的（一次比较）”的就是 `VineShadowBlock`。⇒ “某帧读到别的帧的块”在画面上的表现
  **只**会是阴影错位/多出，其余部分看着正常——这正是症状的形状。
* **根因（可证，不靠猜）**：`FrameRing` 的 slab 数被写成“在飞帧数”（3）。但在飞**比“上一帧”更深**：框架只在
  **回收槽**时等到该帧的 fence，而那次等待发生在**后一帧的 submit 之内**（`RecordAndSubmitTask::submit → start()`），
  即**本帧字节已经写进 ring 之后**（Vine 的块在 `swapBuffers()` 的 `recordContent` 里写，提交仍在它后面）。
  ⇒ 本帧写的 slab 正是**最老的那帧**（F − slots）在读的 slab，`slots` 个 slab **恰好差一个**。
* **规则（一处命名）**：`core::perFrameCopies(frames_in_flight) = frames_in_flight + 1`（`SlotProbe.hpp`，理由写进
  它的 doc）；`FrameRing::Layout::slots` **改名 `slabs`**（名字修正：它从来不是“在飞帧数”），默认
  `perFrameCopies(kAssumedInFlightSlots)`；`MaterialArena::Layout::copies` 同规则；
  `BlockStorage::Layout::kAssumedSlabs` 一处算出四个 ring + arena 的默认。
* **门禁（三条，本机当场跑过）**：① 无设备 `BlockStorageTest.TheDefaultsOwnOneSlabMoreThanTheFramesInFlight`
  （默认布局 = 在飞 + 1，五个 region 逐一断言）；② 无设备 `CoreMaterialArenaTest.TheCopyRotationSpansTheInFlightFrames`
  （一次完整旋转是 `copies` 帧，第一份 copy 在第 `copies` 帧才回来）；③ X11 设备
  `BlockStorageTest.TheSlabsRotateSoASteadyFrameWritesWhereTheFramesInFlightDoNot`（三帧在飞 ⇒ 第 3 帧不得落回
  slab 0——**这一条原来断言的正是错的模型**：“after three frames the first slab is free again”）。
  **变异**：把 `perFrameCopies` 改成不加一 ⇒ ①② 当场红（Windows 上就能证）。`test_vsg` 全量 **400 通过 /
  25 跳过**（无 X ⇒ 设备与窗口读数用例照旧跳过，判法见 §11.16cc）。
* **诚实登记**：① 本机（Windows/Release）**没能**跑设备相位，“用户看到的症状确由这条引起”是**由推导与门禁
  支持的最强候选**，不是实测复现（复现需要能真拖鼠标的 GPU 窗口）；② 在飞帧数一旦学到 ≠3，存储仍按**假设**
  布局（会话会报一条 Info，见 `Session::probeSlots`），那时需要“按学到的数重新布局存储”（走
  `adoptBlockStorage` 同一条路径）。**触发器**：真机报出 N≠3，且画面仍有跨帧串扰。


### §11.16de（2026-09-25）：在飞槽数的地板与“学到更深就重布局”——B9 的双半

* **登记来自哪里**：§11.16dd 的诚实登记②——“在飞帧数一旦学到 ≠3，存储仍按**假设**布局”。翻成可查的缺陷面就是 B9：
  ① `BlockStorage::create` 对 layout 的槽数**零校验**（旧字面量 `slots{3}` 能静默造出“最老在飞帧被本帧覆盖”的存储）；
  ② 会话学到更深数时只发一条 Info，存储不重布局。
* **① 地板（上举，不拒绝）**：`BlockStorage::layoutForInFlight(layout, n)` 把五个 per-frame 形态逐一上举到
  `core::perFrameCopies(n)`（**只上举、从不收缩**——按更浅数缩掉的环形只是“下一次数被学到之前”的安全，且页已付钱）；
  `create` 以 `kAssumedInFlightSlots` 应用它，`layout()` 自此回报“造出来时的形状”（已在地板之上）。判断面是
  `hasSlabsForInFlight(layout, n)`：五个形态逐一查，不只看 views。选“上举”而非“拒绝”：拒绝没有报错渠道
  （`create` 只答 null，那是设备/缓冲失败的语言），而太浅的存储**没有正确的服务方式**——不是更小的预算，是撕裂的画面。
* **② 学到更深 ⇒ 一次替换**：`VsgBackend::growBlockStorageIfNeeded` 每帧读“被声明的在飞数”（生产路径 =
  `Session::slots()`），形态不够就把它与预算增长**合并**成一次 `create` + `adoptBlockStorage`（新 buffer、旧存储经会话
  窗口停靠、一条 Info）。**必须合并**：替换存储身上的 `growthNeeded` 从零开始，分两步做会把另半边的请求悄悄丢掉。
  Info 文案三个变体（只有槽 / 只有预算 / 两者同时），预算变体的前缀照旧含 “grew”（既有增长用例在盯）。
* **门禁（四条，本机跑过）**：① 无设备 `BlockStorageTest.AShallowLayoutIsRaisedToTheInFlightFloorAndNeverShrunk`
  （上举五个形态、逐形态处理混合布局、不收缩、预算/步长不动）；② 设备
  `BlockStorageTest.AStorageBuiltFromAShallowLayoutOwnsTheInFlightFloor`（create 后 `layout()` 五个形态 = 地板；
  五帧写四个不同 slab 再回卷）；③ 设备接缝
  `VsgBackendTest.ACountDeeperThanTheRingsServeReplacesTheStorageBeforeTheNextFrame`（`assumeInFlightSlots` 声明 4 ⇒
  下一帧的 beginFrame 换存储：五形态 = `perFrameCopies(4)`=5、容量变大、恰一条 Info、第二帧不再换）；④ 原增长用例照旧
  （纯预算替换的文案仍含 “grew”）。`test_vsg` 448→**451**，门禁 **cases=451 / failed=0 / vuid=0 / hazard=0**，
  应用阶段判图逐字不变。
* **变异 4/4（各红，恢复后基线 451 全绿）**：M1 地板移除（`create` 直接用请求）⇒ 地板设备用例红；M2 上举失效（identity）
  ⇒ 策略、地板、接缝三用例红（批量运行里 `VsgBackendPluginTest.TheRegisteredBackendComesUpOnTheHostsSurfaceAndDraws`
  曾读回一帧黑——**单跑绿**，判为既知“同批窗口读回”环境模式，与 M2 语义无关：插件路径的默认布局本就 ≥ 地板）；
  M3 接线断（`slabs_short` 恒 false）⇒ 接缝用例红；M4 谓词过宽（只看 `learned != 0`）⇒ 4 用例红 + 套件 SIGSEGV
  （逐帧替换 + 停靠无止境——“次帧不再换”的判别力就在接缝用例的第二帧断言里）。
* **诚实登记**：本机框架学到的在飞数恰是假设值 3，“真 N≥4”仍**没有在真机自然观测到**——接缝声明的数与真实学到的数走
  **同一条分支**（同一次 `hasSlabsForInFlight` 判定、同一次替换、同一条诊断），但“某台机器上真的学到 4”这件事本身不在
  本机证据里。**触发器**：真机首次报出 N≠3 时，核对 Info 文案（应出现 “learned its in-flight count (N)”）与替换后的
  内存代价。

### §11.16df（2026-09-25）：ASan 全量 test_vsg 第一次真的跑起来——一处构建地雷、一处真 UAF、一处测试卫生

* **这根线怎么开的**：B9 收尾想跑脚本里那条严格泄漏判据（`VINE_ASAN_TARGET=test_vsg VINE_ASAN_FILTER='*' VINE_ASAN_LEAKS=1 scripts/asan_check.sh`），
  ASan 树的插件链接先炸：`libvolkd.a(volk.c.o): relocation R_X86_64_PC32 ... can not be used when making a
  shared object; recompile with -fPIC`（ld.bfd）。查证：volk 目标在**所有树**都没有 `-fPIC`（三棵树的对象里 PC32 对
  volk 符号 728/4309/741 处），Debug/Release 能链上是**历史对象**的运气（Debug 的归档还是 9 月 19 日建的，Sep-19 归档的
  PIC 性无法回溯解释——能说的是现在的对象在任何树都非 PIC）。修复 = `third_party/volk/CMakeLists.txt` 给目标
  `POSITION_INDEPENDENT_CODE ON`（静态归档进共享库永远要它）；三棵树重链验证（ASan 树此前根本链不出插件）。
* **环境坑（同一根线）**：ASan 树重生成会跑 FetchContent 的 spdlog 更新步骤 ⇒ **要带 `env -u http_proxy …` 跑**（本仓
  git 走直连，代理环境变量会掐握手）。
* **第一处真缺陷（UAF）**：套件首个 ASan 错——`ContentStoreTest.AnObjectNobodyElseHoldsIsReleasedAndItsRowsLeaveAtThePark`
  里 `tablesFor` 读已释放的 `Geometry`（`Geometry::revision()`，READ of size 8）。机制：用例契约是"**停靠的行在停放
  窗口内仍要应答**"（它把命名已释放地址的旧计划喂回 `tablesFor` 是故意的），而 `tablesFor` 在**查表之前**就读
  `geometry->revision()`；同文件另外三处（`ensureGeometry`/`ensureMaterial`/`ensureProgram`）都是**先查后读**——
  "行在即持 share（`LiveGeometry::object`），持 share 才准读对象"本就是该文件自己的不变式，`tablesFor` 是唯一例外。
  修复：成员检查移进 `liveGeometry(对象指针)`，无行答 `nullptr` 且**不读**。证据：修前 ASan 红（free 栈 = `LiveGeometry`
  析构释放最后一个 share）；修后全量 **0 个 ASan 错**。
* **一处测试卫生（严格泄漏判据的主体）**：修完 UAF 全量跑到尾，LeakSanitizer 报 **685,560 B / 395 处**——主体是
  **22 个设备用例各自 `xcb_connect` 后从不 `xcb_disconnect`**（"Direct leak of 21176 byte(s)" ×22 = 连接缓冲）。
  修复：`TestXConnection` RAII（`tests/test_vsg/TestHostWindow.hpp`；声明在连接之后、窗口之前——C++ 逆序销毁 ⇒ 窗口先走、
  连接后关）+ 22 处插入（`HostWindowReadTest` 的 Fixture 本来就会断，跳过）。**泄漏 685,560/395 → 6,048/108**。
  另：`CoreAllocationGateTest.ADeliberateAllocationIsCaught` 在 ASan 下必红且**应当**红——它的量具是 `mallinfo2`
  （glibc 堆），ASan 把分配器整体替换 ⇒ 种子分配对量具不可见（`grew == 0`）。处理 = ASan 下 `GTEST_SKIP`（判词写进
  用例：门禁的牙由非 ASan 跑证明），不是改断言。
* **剩下的 6 KB 与它的归属（登记待决）**：6,048 B / 108 处（≈56 B/处）的栈**穿过插件模块且符号错乱**（`<unknown
  module>`、把 `calloc` 归给 `_Rb_tree::end()` 之类的胡话）——实测插件与主程序**各自静态链了一份 ASan 运行时**
  （两边符号表里都有 `__asan_init`），插件侧分配在退出报告里走错账本。可见的命名帧是 vsg 的一次性缓存
  （`ResourceRequirements`/`Array2D::create`）与 `OffscreenTarget::buildAttachments` 两处。**收口二选一留作独立单元**：
  全树 `-shared-libasan`，或逐条归属后按 `asan_leaks.supp` 的规矩写**带理由的**豁免。`scripts/asan_check.sh` 头部已把
  这两条边界与 `vsg_backend_selftest`（已随老渲染器删除，头部引用陈旧）写清。
* **门禁**：两棵树 `test_vsg` **451/451**；两棵树门禁 `cases=451 failed=0 vuid=0 hazard=0`、应用阶段判图逐字不变
  （before 378x247: content 87.04% / after 698x132: content 85.14%）；include 卫生 0/798、诊断格式 0/7、文档符号 3/145。
* **提交**：`28f1332`（volk PIC + UAF 修复）、`6dc23d1`（22 处连接 RAII + ASan 跳过 + 脚本边界）。
