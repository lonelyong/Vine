# gfx_backend_vsg：后端运行时说明（重写版）

> 本文覆盖**运行时行为**：SDK 的每个方法落到哪、**服务什么 / 拒绝什么**、一帧的**调用次数**、
> **更新策略**、**生命周期与所有权**、**诊断与验证**。
> 契约、设计决定与**坑**在 [`.ai/design/vsg-reimplementation.md`](../../../../.ai/design/vsg-reimplementation.md)
> （不在这里重复）；文件地图与构建见 [`../gfx_backend_vsg.md`](../gfx_backend_vsg.md)；逐帧数据流与
> L0/L1 映射见 [`data-flow.md`](./data-flow.md)。
>
> 旧 `VsgRenderer`（SceneBridge）时代的运行时叙述已删，全文在 git 历史里；本文只描述重写版。

## 1. 门面：SDK 的方法落到哪

| SDK 方法 | 实际发生 | 宿主看到的 |
| --- | --- | --- |
| `initialize()` | 由公告（宿主句柄、尺寸、默认程序）建会话；失败时**自己清理半成品** | `true/false` + 诊断；`false` 后没有会话 |
| `beginFrame()` | 会话开帧（取交换链图、推进槽位时钟）+ 录制器开一帧 | 反复调用被协议拒绝并上报 |
| `render(commands, camera)` | 逐命令**查三张表**（几何/材质/程序）→ 记进当帧描述；表里没有 ⇒ 拒绝该命令并计数 | `description()` 里有它；计数与诊断说拒绝 |
| `drawScreenProgram(source, program, camera)` | 记一次全屏调用（源 = pass 声明的第一个输入） | 同上 |
| `beginPass/endPass` | scope 属性（target/order/depth/clear）**属于该 pass**，`endPass` 丢弃 | 无 scope 的绘制被拒绝并上报 |
| `setRenderTarget` | 离屏目标描述送进 `HostTargets`（**快照**，稳态零分配） | 目标懒建；拒绝原因分说 |
| `setClearPolicy/setDepthMode/setViewport/setLights/setPassInputs` | 记进当帧描述（"每次绘制属性"只服务下一个绘制调用） | 与计划一致与否在执行段核对 |
| `endFrame()` | 描述定稿 → **编译**（依赖求值、缺省解析、合法性判定） | 计划进 `CompiledFrame`；环会被跳过并计数 |
| `swapBuffers()` | **关帧**：录制命令图 + 提交 + 呈递 | `framesPresented()` +1；丢帧会被点名为诊断 |
| `setWindowHandle/resize` | 采纳宿主表面 / 跟随表面（**停一次设备，计数**） | `deviceWaits()` |
| `supportsRenderTargets()` | `true` | 离屏路径可用 |
| `releasePass/releaseRenderTarget` | 释放该 pass 的会话身份 / 目标对象（停靠后销毁） | 保留态不随帧数增长 |
| `readColorBuffer/readDepthBuffer` | **分类先于任何工作**，再复制回读 | 分类表逐条说原因（见 §2） |
| `shutdown()` | 破坏性路径：**一次计数过的 device idle** → 内容世界 → 会话 | 之后再 `initialize()` 是合法重来 |

## 2. 服务与拒绝

**服务**（每条都有真设备用例，见 `tests/test_vsg/`）：

* **内容绘制**：索引与非索引、点/线/三角（拓扑**类**进管线键），MRT（多颜色附件）、深度附件、
  材质（贴图 / 立方体 / 天空 / 无贴图的白色 fallback）、光照（方向光三槽 + 环境补光）、阴影
  （按"目标认领 + 深度可采样 + 生产者矩阵"三事实解析）、自定义 program（按**文本声明**的 `(set, binding)` 与变体）。
* **全屏调用**：PiP 拷贝、窗口覆盖层、延迟光照（含阴影）、深度采样重建；push 块按程序的声明填。
* **离屏目标**：宿主 `RenderTarget` 的描述是快照、对象**懒建**；`applyTargetPlans` 在**窗口路径问平台**，
  其余目标按计划 `ResizeInPlace` / `Rebuild`；借用深度（借用方不建镜像）。
* **读回**：颜色只打包 **RGBA8**（float 附件按"不可读格式"拒绝，而不是误解字节）；深度按浮点；
  "永远不行"（`Unsupported`/不可读格式）与"还没行"（未捕获，下一帧可重试）是不同答案。
* **窗口**：采纳宿主表面并跟随 resize；窗口的答案问平台（不采纳计划的主张）。

**拒绝**（都是**一次**纪律 + 计数，不是静默）：

* 无 scope 的绘制、没在帧里的调用 → 协议拒绝（每条一次）。
* 命令点名了表里没有的对象 → 拒绝该命令；几何的"通道与布局对不上"是 Malformed（同一条规则在查找里）。
* pass 的输入与计划逐项对不上（项数/每项彩色数）→ **整趟拒绝**并说出是哪一项。
* 计划与资源世界的形状/格式不一致 → 执行段报，且**不画**（按错形状建的管线与宿主要的画面无关）。
* 读回、目标、程序等各自的分类表/诊断都在各自的头文件里写着；门禁把"未知 VUID"当失败（白名单为空）。

## 3. 一帧的调用次数

| 频率 | 发生的事 |
| --- | --- |
| 每帧恰好一次 | `beginFrame` / `endFrame`（编译）/ `swapBuffers`（提交+呈递）；一帧一次 present |
| 每 pass 一次 | 绑定该 pass 的管线变体（**只在变体变了**）、动态状态块（**只在值变了**）、采样输入集（一趟 pass 一条） |
| 每次变化一次（稳态 0 次） | 材质 touch 后的**块写**（比较相同就不写）；几何 revision 变化后的流上传；push（按声明）；一次 `vkCmdSet*` |
| 结构性变化才发生 | 目标 resize/rebuild、管线编译（新**变体**——新 program、新拓扑类、新采样计数）、会话移动（停一次设备） |

**稳态应当为 0 的计数**（可断言，判据就是这些数）：`ContentStore::builds()`、
`StreamUploads::uploads()`（同名流走 `aliases()`）、`VariantPool::created()`、`Session::deviceWaits()`（帧路径）、
`RetentionQueue` 的驻留增长、`AllocationGate`（**连续两帧**零分配才算数）。读数见 §6。

## 4. 更新策略

* **材质**：SDK 没有"材质改了"的公告，所以每帧逐命令 **compare-and-write**（64B；相同就不写、不重建行）。
  实测：一次编辑 = 每帧 +1 行、0 上传、0 变体（设计文档 §11.16cq）。
* **几何**：`Geometry::bumpRevision()` 是唯一公告 ⇒ 新 revision = 新行 + **该几何所有流重传**
  （流身份含 revision；替换 buffer 与就地改写一样贵）。增量/缩容见设计文档 §6 的 B6。
* **状态**：`ResolvedRenderState` 走动态状态（cull/polygon/blend/depth/同类 topology），**diff 才发命令**，
  永不重编；跨拓扑**类**才是新管线（理由在 `Keys.hpp`）。
* **程序**：identity（program + revision + variant + 顶点布局 + 兼容性 + 采样计数）；新变体**编译一次**，
  之后复用（实测新 program 第一帧 ≈ +1.6 ms 一次性）。
* **目标**：加载操作是"交换半边"（不进键）；`written()`/`bootstrap` 决定谁清屏；丢帧 ⇒ `Repair(Bootstrap)`。

## 5. 生命周期与所有权

* **帧份额**：`render()` 里点名的对象由内容世界**持一份**（计划/已录制的帧仍然命名它们），回归稳态后
  通过 **compare**（不是时间）判断是否要重新构建行。
* **替换 = 停靠**：被顶替的行/流/变体/目标进出**退役队列**（窗口 `slots + 1` 帧）；槽数还没学会时
  `retire()` 返回 false，调用方退回**计数过的** device wait（不许猜窗口）。
* **销毁只有两条合法路径**（契约）：整会话替换（前置一次计数过的 device idle）或退役出队。
* **宿主的东西归宿主**：宿主表面/句柄由宿主销毁；后端只采纳与跟随（`setWindowHandle`/`resize`）。
* **`VSG_MAX_DEVICES` 默认 1 是有意的绊线**：任何"同时两个 device"的路径都该当场抛错（别改构建树里的值）。

## 6. 诊断与验证

* **诊断通道**（sink）+ `diagnosticCount()`：实体是 `DiagnosticSeverity` × `DiagnosticCategory`；
  **episode** 规则（同一问题只报一次，句柄归调用方）写在各自的作用域里。
* **读数**：`framesPresented()` / `deviceWaits()` / `releasedContentObjects()` / `releasedTextures()` /
  `ContentStore::builds()` / `StreamUploads::uploads()` / `VariantPool::created()`（见设计文档 §4 的表）。
* **门禁**：`bash scripts/vsg_rewrite_gate.sh <build-dir>`——套件（跳过即失败）+ `VUID`/`SYNC-HAZARD` 计数 +
  三个 hygiene 脚本 + 相位行（不许有 `FAILED`）+ 应用阶段三条画面判据。**"测试过了"不是结论**。
* **本地跑设备用例**：需要 lavapipe（`VK_ICD_FILENAMES=…/lvp_icd.json`）与 X11（`DISPLAY`）；没有就 SKIP，
  而门禁把 SKIP 当失败。

## 7. 读数注意（本模块特有）

* **读窗口**要读后端日志里那个窗口 id（渲染区），不是 Qt 容器；呈现是异步的，读之前多跑一帧；
  窗口没到 viewable 之前的呈递永远不到（`readError()` 8 = 被拒，不是黑屏）。
* **测试环境的 `QT_QPA_PLATFORM`**：窗口/像素类用例要用 offscreen（xcb 会在测试自己的窗口上盖 Qt 窗口，
  `XGetImage` 读到遮挡内容 ⇒ 成片假红）；门禁替应用阶段选 xcb。
* **应用窗口的尺寸不是确定的**（demo 的窗口随日志 dock 长）⇒ 门禁在采样前先把它拖到固定尺寸再判。
* 旧文档里的"每帧成本"数字是当时的（案例见设计文档 §11.16cl/cq）；当前数字以门禁与配方用例的输出为准。
