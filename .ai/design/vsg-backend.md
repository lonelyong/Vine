# vsg 后端：架构、契约、坑

> **一句话**：后端只把引擎编译好的帧值物化成 Vulkan 命令；平台事实（1.4 地板、无 dynamic rendering、
> 每 pass 私有状态表、reverse-Z）不可协商；下面这些坑都踩过，别重踩。
>
> **权威来源**：`RenderBackend.hpp` / `ShaderAbi.hpp` / `src/viz/graphics/sdk/vine/graphics/backend/**` /
> `src/plugins/gfx_backend_vsg/include/vine/vsg/**`。**待办**：`graphics-followups.md`。

## 1. 设计要点

1.1 **三段式**：收集（`FrameRecorder`）→ 编译（`FrameCompiler`）→ 执行（`VsgExecutor`）；
    执行器只做机械落地，不决定排序、重建或缓存。
1.2 **调度在引擎**：`detail::FrameScheduler` 是唯一排序点；后端按 `FrameDescription.passes` 的顺序物化。
1.3 **每 pass 状态私有**：vsg 按 viewID 编管线；状态注册表跨 pass 共享会拿到不兼容 render pass 的 pipeline。
1.4 **目标形状驱动**：`TargetShape` 决定图像、通道、管线兼容性；bootstrap 清所有附件。
1.5 **流按 buffer 身份共享**：`StreamKey` = buffer + 该 buffer 自己的 revision + 段；稳态零重传。
1.6 **反向 Z + CW 正面**：Vulkan NDC z 就是 depth；世界 CCW 到 framebuffer 是 CW。
1.7 **证据三重**：验证层 / 计数器 / 像素；三者缺一都不算“通过”。
1.8 **物理边界**：SDK `graphics/backend/` 不 include vsg；插件 `internal/` 才 include both。

## 2. 后端架构

### 2.1 分层

| 层 | 位置 | 内容 |
|---|---|---|
| SDK 后端契约 | `src/viz/graphics/sdk/vine/graphics/backend/` | `FrameRecorder` / `FrameCompiler` / `Keys` / `TargetPlan` / `ClearPlan` / `VariantPool` / `StateRegistry` / `RetirementQueue` / `FrameTimeline` / `FrameArena` / `FrameRing` / `MaterialArena` / `Observe` / 证据夹具 |
| 插件内部 | `src/plugins/gfx_backend_vsg/src/internal/` | `VsgBackend`（门面）/ `api::Session` / `WindowTarget` / `OffscreenSession` / `HostTargets` / `VsgExecutor` / `ContentStore` / `StreamUploads` / `ContentPipeline` / `BlockStorage` / `BlockDescriptors` / `MaterialImages` |
| 插件支撑 | `src/plugins/gfx_backend_vsg/src/support/` | `VsgHostWindow` / `RenderStateMapper` / `VsgDynamicState` / `VsgSceneRules` / 入口点 |
| 插件外壳 | `src/plugins/gfx_backend_vsg/src/shell/` | 插件注册、`RenderBackendFactory` |

### 2.2 类图

`VsgBackendImpl` 就是 `VsgBackend::Impl`（私有实现）；`Sdk*` 是 SDK `backend/` 里的可移植类型，带
`<<SDK>>` 标记。

```mermaid
classDiagram
    class RenderBackend { <<interface>> }
    class RenderDevice { <<interface>> }
    class RenderFrameExecutor { <<interface>> }
    class PresentTarget { <<interface>> }
    class VsgBackend
    class VsgBackendImpl
    class Device
    class WindowTarget
    class OffscreenSession
    class VsgExecutor
    class ContentStore
    class HostTargets
    class BlockStorage
    class MaterialImages
    class ContentPass
    class StreamUploads
    class ContentPipeline
    class BlockDescriptors
    class VariantPool
    class StateRegistry
    class SdkFrameRecorder { <<SDK>> }
    class SdkFrameCompiler { <<SDK>> }
    class SdkTargetPlan { <<SDK>> }
    class SdkFrameRing { <<SDK>> }
    class SdkMaterialArena { <<SDK>> }

    RenderBackend <|-- VsgBackend
    RenderDevice <|-- VsgBackend
    RenderFrameExecutor <|-- VsgBackend
    PresentTarget <|-- WindowTarget
    VsgBackend --> VsgBackendImpl : owns
    VsgBackendImpl *-- Device
    VsgBackendImpl *-- WindowTarget
    VsgBackendImpl *-- OffscreenSession
    VsgBackendImpl *-- VsgExecutor
    VsgBackendImpl *-- ContentStore
    VsgBackendImpl *-- HostTargets
    VsgBackendImpl *-- BlockStorage
    VsgBackendImpl *-- MaterialImages
    SdkFrameRecorder --> SdkFrameCompiler : CollectedFrame
    VsgExecutor --> SdkFrameCompiler : walks CompiledFrame
    VsgExecutor --> ContentPass
    ContentPass --> ContentStore
    ContentStore --> StreamUploads
    ContentStore --> ContentPipeline
    ContentStore --> BlockDescriptors
    ContentStore --> VariantPool
    ContentStore --> StateRegistry
    ContentPipeline --> VariantPool
    BlockStorage --> SdkFrameRing
    BlockStorage --> SdkMaterialArena
    HostTargets --> OffscreenSession
```

### 2.3 一帧的生命周期

```
beginFrame()      取交换链图像（槽位逐帧学习）
  render/beginPass/endPass ×N   → FrameRecorder 只记录事实（快照借用参数）
endFrame()        描述定稿
swapBuffers()     FrameCompiler 编译 → VsgExecutor 录制 → 提交 → 呈现（关帧的是这一步）
```

- 收集侧允许重复 / 后设覆盖；编译侧必须每 pass/draw 一个终态、缺省全部解析（viewport / 清屏 / 默认程序）。
- 环（cycle）在引擎调度器处理；后端不再自建依赖图。分量外的 pass 照跑，帧照旧提交。
- 空帧也是帧：照样编译、录制、呈递；空 **pass** 不是 pass。

### 2.4 对象职责

| 对象 | 回答的问题 |
|---|---|
| `VsgBackend` | `RenderBackend` / `RenderDevice` / `RenderFrameExecutor` 的门面；持有 Impl |
| `api::Session` / `WindowTarget` | 窗口、交换链、槽位、present |
| `api::OffscreenSession` | 无窗口的帧源；headless 设备的 device-wide work |
| `HostTargets` | 宿主声明的离屏目标；描述拷贝、对象懒建、resize/借深度的拒绝 |
| `VsgExecutor` | 按 `CompiledFrame` 的顺序把 pass 录进 render graph |
| `ContentStore` | 内容世界：身份、驻留、流、管线、块、材质 |
| `ContentPass` | 一个 pass 的内容录制（命令 → GPU 工作） |
| `SdkFrameRecorder` | 把每次 API 调用变成自包含记录，快照借用参数 |
| `SdkFrameCompiler` | 解析缺省、计划目标/深度、给出执行顺序 |
| `SdkTargetPlan` / `SdkFrameRing` / `SdkMaterialArena` | 目标计划 / 每帧块 / 持久材质块 |

## 3. 功能点

### 3.1 平台与设备

| # | 功能 | 说明 |
|---|---|---|
| F1 | 设备地板 | Vulkan 1.4；7 个必需 feature；EDS2 / EDS3 + 3 个 fn-pointer 入口。 |
| F2 | 设备探测 | `DeviceProbe` + `DeviceRequirements`；探测与策略同一份事实。 |
| F3 | 会话 | 自有窗口或采纳宿主窗口；窗口 ⇒ 交换链 ⇒ `beginFrame` / `commitFrame`。 |
| F4 | 槽数学习 | `probeSlotCount` / `SlotTracker`；学会前不 park。 |
| F5 | 退役 | `RetirementQueue` 窗口 = `slots + 1`；`setSlots()` 原地更新。 |

### 3.2 目标与通道

| # | 功能 | 说明 |
|---|---|---|
| F6 | 目标计划 | `TargetShape`：附件数 / 颜色格式 / 深度格式 / samples / subpass。 |
| F7 | 清屏 | `planClearValues`：bootstrap 清所有附件；深度清 0.0（reverse-Z far）。 |
| F8 | 深度借出/借入 | 借入方不自建深度；`initialLayout` = attachment layout，LOAD。 |
| F9 | 依赖 | `VkSubpassDependency` 的 stage/access mask 是兼容性的一部分；借深度取超集。 |
| F10 | 读回 | 颜色按 attachment；深度 D32 / D16（D24S8 拒绝）；barrier 回 attachment layout。 |

### 3.3 管线与状态

| # | 功能 | 说明 |
|---|---|---|
| F11 | 管线键 | program + revision + layout + topology + render-pass 兼容性 + samples + variant。 |
| F12 | 变体池 | `VariantPool` 按 key 缓存；FIFO 上限 65；id 不复用。 |
| F13 | 状态注册 | 每 pass 私有；`variant_switched` / `dynamic_issued` 分开计数。 |
| F14 | 动态状态 | 块槽位 `detail::kDynamicStateSlot`（15）；viewport / scissor 动态。 |
| F15 | 管线状态 | reverse-Z GREATER；frontFace CLOCKWISE；blend 恒开（单颜色附件内容）。 |

### 3.4 流、块、描述符

| # | 功能 | 说明 |
|---|---|---|
| F16 | 流共享 | `StreamKey` = buffer + 该 buffer 的 revision + 段；index 归一到整段 buffer。 |
| F17 | 自定义属性 | location 3 起，转发 `vine_Attribute{location}`。 |
| F18 | 块存储 | 一个 mapped buffer：view / draw / material 三区；每帧 ring；稳态零写。 |
| F19 | 描述符 | 一个 set，3 个 dynamic uniform binding（0 view / 1 draw / 2 material）。 |
| F20 | 材质 arena | 每槽 `copies` 份；槽满 `Refused`；扩容只换 material 区。 |

### 3.5 纹理、着色器、证据

| # | 功能 | 说明 |
|---|---|---|
| F21 | 纹理上传 | `MaterialImages` 键 `(Texture*, revision)`；D2 / Cube 视图；白 cube 兜底。 |
| F22 | sampler | 按值缓存；`InputSetCache` 键 `(view, sampler)`。 |
| F23 | 着色器文件 | `builtin_<角色>.<阶段>`；构建期嵌入；`vine_` 前缀。 |
| F24 | 变体文本 | `#pragma import_defines` 决定文本；漏写会静默丢弃。 |
| F25 | 证据 | 相位表 + 计数器 + 像素；`vsg_rewrite_gate.sh` 读 VUID / SYNC-HAZARD / 画面。 |

## 4. 公开接口

### 4.1 后端契约

| 接口 | 作用 |
|---|---|
| `RenderBackend` | 设备 / 帧 / 呈现三面聚合 |
| `RenderBackendFactory` | 创建后端 |
| `RenderDevice` | 设备、目标、内容释放、读回 |
| `RenderFrameExecutor` | 执行一帧 |
| `PresentTarget` / `WindowPresentTarget` | 呈递契约 / 窗口面 |

### 4.2 插件内部关键类型

| 接口 | 作用 |
|---|---|
| `api::Device` / `api::Session` | 无窗口设备 / 自有或宿主窗口会话 |
| `api::OffscreenTarget` | 离屏颜色 + 可选深度 + 读回 |
| `api::ContentPipeline` / `api::ContentDraw` | 管线与一次 draw 的录制 |
| `api::StateCommands` / `api::BlockDescriptors` | 动态状态 / 描述符 |
| `core::TargetPlan` / `core::ClearPlan` / `core::Keys` | 目标计划 / 清屏 / 键 |
| `core::Streams` / `core::MaterialArena` / `core::FrameRing` | 流 / 材质 / 每帧环 |
| `core::RetirementQueue` / `core::SlotProbe` | 退役 / 槽数 |
| `support::VsgHostWindow` / `support::RenderStateMapper` | 宿主窗口 / 状态映射 |

## 5. 键与状态

| 层 | 当前拼写 | 允许的变化方式 |
|---|---|---|
| identity | `PipelineKey`：kind、program + revision、vertex_layout、compatibility（附件/深度格式、samples、subpass）、depth_sampleable、sampled_color_count、sampled_depth_count、variant、topology（类） | 变了 = 重编管线（唯一允许重编的地方） |
| 交换半边 | `LoadOpVariantKey`：color/depth 的 load/store 与 initial/final layout | 同一管线在多个兼容变体上都能用（bootstrap 变体合法） |
| dynamic | `DynamicState`：depth、cull、polygon、topology（同类内）、blend | 一条 `vkCmdSet*`；永远不许重编 |
| data | 视图块 288 B、光照块 112 B、`VineDrawBlock` 80 B、材质块 64 B、push ≤128 B | 就地字节写 / 描述符 re-point |

**“计数是身份不是状态”**：绑在描述符布局上的东西（采样彩色数、深度数）必须进键；extent / load-op / 布局 /
material 值 / 流字节 / cull 全部**不进**。

## 6. 坑（不可再踩）

### 6.1 协议与帧

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P1 | 关帧的是 `swapBuffers()`，`endFrame()` 只离开 pass | 忘了它：下一次 `beginFrame` 被拒，`description()` 还是旧帧 |
| P2 | 空帧也是帧 | 空 **pass** 不是 pass；acquire 过的图必须有一趟 render pass 走过，否则 present UNDEFINED |
| P3 | 借用参数必须当场快照 | 引擎 `resolved_inputs_` 是复用成员；持有 = 读下一个 pass 的数据 |
| P4 | 无 scope 的绘制拒绝并上报 | scope 属性属于 pass，每次绘制属性只服务下一个调用 |
| P5 | `beginFrame` 之外的“暖机” render 不收集 | 协议拒绝，静默返回 |
| P6 | 环由引擎调度器处理 | 后端不再自建依赖图；重新引入 = 第二套排序 |

### 6.2 键与管线

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P7 | 不要把可动态化的东西放进键 | StateNode 一改就重编；键审计（created 不动） |
| P8 | 不要把要换的视图留在节点里 | resize 重建整批程序槽；键审计 |
| P9 | 拓扑只能进“类” | `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` 只允许同类切换；跨类 = 新管线 |
| P10 | 格式枚举是投影 | sRGB/线性、RGB/BGR 都是兼容性的一部分；漏了 ⇒ 02684，像素照过 |
| P11 | 池容量 65（FIFO） | 淘汰后同键再来会重新 `created`；容量与淘汰是身份的一部分 |
| P12 | 块角色按类型名认领 | 按 binding 序号给角色会读成别人的块 |

### 6.3 朝向、裁剪与深度

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P13 | reverse-Z 三件套 | 远平面 0.0、比较 GREATER、深度清 0.0；清 1.0 + GREATER = 全丢 |
| P14 | 视图块只折 `proj` / `view_proj` | x/w 不变、y 取反、z 记作 `0.5 - 0.5*z`；`view` / `inv_view` 不折 |
| P15 | 折反看不出来 | z 镜像时画面照样过测；算术用例 + 像素用例各管一段 |
| P16 | front face = CLOCKWISE | 照抄 CCW 会让每个 cull 模式作用在错误的面（静默） |
| P17 | 单附件内容混合恒开 | `blend.enabled` 只选因子；MRT / 全屏 draw 不混合 |
| P18 | 全屏调用不测深度 | 正典三角形 z = 0 = reverse-Z 远平面；测了就整块消失 |
| P19 | 全屏 push 必须紧贴绘制、放进 `Commands` | 放在 stateCommands 里会读到恒等矩阵，画面“看着正常” |
| P20 | push 的阶段是 ABI 的一半 | 发错阶段 0 VUID 静默 |
| P21 | 背景判据是“这条通道写没写过” | 不是 `dot(pos,pos) < 1e-6`；后者在相机贴脸时留洞 |
| P22 | 顶点数 / ABI 声明像素看不见 | 被 viewport 裁掉或画同一张图；判据是算术用例 |

### 6.4 块与描述符（ABI）

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P23 | `VineMaterialBlock` 是聚合体 | `{}` 不碰尾部填充；用逐成员 `operator==`，不要字节比较 |
| P24 | dynamic offset 必须对齐 | `minUniformBufferOffsetAlignment` 的倍数；不合规不给命令 + 计数 |
| P25 | 别把每帧偏移烤进描述符集 | 池会把同一句柄发回下一帧，上一帧命令还在用（03047） |
| P26 | 替换描述符集 = 旧的必须进退役窗口 | 池会把已释放 set 的句柄发给替代品；`vkUpdateDescriptorSets` 打在 pending 句柄上 |
| P27 | 布局要声明两种绑定（块 + 采样） | 空隙必须是真布局；空指针 ⇒ 无效句柄 ⇒ 段错误 |
| P28 | “什么都不声明”就是“什么都不绑” | 程序没声明块就没有集合可绑；别无条件绑块集（00360） |
| P29 | 声明了 push 却没填 ⇒ 拒绝那一半 | “黑屏是因为矩阵是零”不算诊断 |
| P30 | 全屏集合的每个 binding 从程序声明来 | 写死 binding 5 读到未定义数据 |
| P31 | std140 尺寸按对齐算 | 成员字节相加会漏补齐（52 → 64） |
| P32 | `#ifdef` 分支的条件决定事实 | 不管 define 全算 taken ⇒ 同 `(set,binding)` 两种类型 ⇒ Malformed |
| P33 | 白图 fallback 必须走上传 | 没有录制方 ⇒ 图永远 UNDEFINED + 描述符撒谎 = 每帧 09600 |
| P34 | 材质贴图种类必须对上采样器 | 立方体逐面交织错了是静默的（字节总数一样） |
| P35 | 布局 / 兼容性主张的判据是验证层 | 像素全绿、VUID 成串（r/ah/ai/al 四次实测） |

### 6.5 流、几何与别名

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P36 | 流身份 = buffer + **该 buffer 的 revision** + 段 | 几何 revision 只重建事实行；改字节必须 `Buffer::setRevision` |
| P37 | 被重填的 buffer 是新流 | 旧别名不能指向新字节；revision 移出身份 = 静默旧数据 |
| P38 | 派生通道不可共享 | 白载体、零 UV、法线字节属于一个几何 |
| P39 | 一个几何的所有通道从同一个顶点起算 | 否则“索引 0”对每个通道指不同顶点 |
| P40 | 索引键归一化为整 buffer | 切片共享同一份上传；别名判据 = 同一个 bind 对象 |
| P41 | 别名登记不持有字节 | 淘汰 ≠ 释放；寿命靠帧点名，宽限 = slots + 1；容量 512，淘汰最久未点名 |
| P42 | 用栈对象构造 `intrusive_ptr` 会 invalid free | SDK 对象必须堆上由 `intrusive_ptr` 拥有 |

### 6.6 目标、寿命与设备空闲

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P43 | 销毁只有两条路 | 整会话替换（前置一次计数 device idle）或退役出队；否则 00765 |
| P44 | 退役窗口 = slots + 1 | 槽数学不到 ⇒ `retire()` 返回 false，退回计数 device wait |
| P45 | 丢帧 = 内容不可信 | 下一份计划答 `Repair(Bootstrap)`；只有清屏能修，LOAD 不能 |
| P46 | `built` = 能 LOAD = `written` | 新目标第一趟必须清；`written` 必须复位 |
| P47 | load op 是交换半边，不进键 | 但依赖掩码绝不能随变体变（变体连 framebuffer 都不兼容） |
| P48 | 形状变了才是真重建 | 连格式一起核对；兼容性主张像素抓不住（00904） |
| P49 | 借用深度：不建镜像、用出借方视图 | `borrowed` 恒不可采样；捕获屏障必须命名真正当前布局（01197） |
| P50 | 读回是一张分类表 | 分类先于任何工作；“永远不行”和“还没行”是不同答案；颜色只打包 RGBA8 |
| P51 | 计划与资源世界逐项核对 | 目标形状、输入项数与每项彩色数；不等就整趟拒绝并说出哪项 |
| P52 | `written()` 在“录了但没提交”时也为真 | 事实是“有人往里录过”；对 bootstrap 判断够用 |
| P53 | 窗口答案问平台 | live 采样与换尺寸由平台/表面给出，不采纳计划的主张 |

### 6.7 WSI 与窗口

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P54 | present 到“还没 viewable”的窗口永远不会到 | 读它 `BadMatch`（readError 8）；构造等 viewable，测试 settle 后再呈现 |
| P55 | 读窗口读后端日志里那个 id | 名字 / Qt 容器会读到永远亮着的 chrome；`xwinresize.py` 改顶层父窗口 |
| P56 | 呈现是异步的 | commit 后立刻读可能拿到旧后备存储（全黑）⇒ 多跑两帧 |
| P57 | 每帧换命令图会拆掉在飞 fence/semaphore/命令缓冲 | 实测 12 条 VUID；稳帧别每帧 `assignFrameGraphs` |
| P58 | 丢帧之后会话要自己活下来 | acquire 过没呈递的图让 WSI 缺一张；会话把图还回去 |
| P59 | 启动尺寸不确定 | 窗口随日志 dock 长；门禁先把窗口拖到固定尺寸再判 |
| P60 | 起应用要 `exec` 且按名字杀 | `pkill -x Vine`；遗留窗口会盖住测试窗口 |

### 6.8 证据、门禁与构建

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P61 | “测试过了”不是结论 | 套件全绿同时验证层一直报 VUID；门禁数 VUID / SYNC-HAZARD，跳过即失败 |
| P62 | 假绿要能被门禁自己抓到 | 忘 `swapBuffers`、空 pass、伪造 VUID/skip 行；判据要“不会骗过自己” |
| P63 | 相位表 | 一行 = 装配 → 断言（像素 + 计数器）→ 期望的计数器增量（含不变项） |
| P64 | 稳态判据是“两帧都不分配” | 一帧不分配只是那一帧的事实 |
| P65 | 变异反证要先自己验一遍 | 两帧身份一致、字段恒零的变异不会红 |
| P66 | 应用阶段三条画面判据 | 非近黑 ≥30%、预览最大通道 ≥64、平背景 ≤5%；VUID 白名单为空 |
| P67 | 插件源列表是 GLOB | 新增 `.cpp` 要重新 configure；`tests/test_vsg/CMakeLists.txt` 两处显式加 |
| P68 | demo 宿主是插件 | `ninja -C build Vine` 不重建它；改完 demo 要 `ninja -C build demo_plugin` |
| P69 | `VSG_MAX_DEVICES` = 4 | `<1` / `>4` 在 1.1.16 是坏的；第 5 块面要由我们拒（暂无守卫） |
| P70 | `vn::String` 是 `std::u8string` | 字面量 `u8"..."`；`Mat4d` 默认构造 = 单位阵；`vsg::Image::vk(deviceID)` 要下标；`vsg::Exception` 不是 `std::exception` |
| P71 | 三份 living 文档受 `check_doc_symbols.py` 管 | 点名的单元必须存在；段落标题含“历史登记”可整段免检 |
| P72 | 证据门禁前必须整包 build | 只建目标会留下陈旧二进制；“证据”不可信 |
| P73 | 脚本批量改代码的四条护栏 | 合并切割段后一次写回；报告未命中项；别对刚生成的文件打补丁；改门禁先 `bash -n` |
| P74 | `SRC_FILE_LIST` 用 tab 缩进 | 空格匹配必然失败 |
| P75 | clang 偶发瞬时崩溃（exit 135） | 先重跑；干净后仍走完整判据，不许当“通过” |

### 6.9 引擎侧（场景桥与着色约定）

| # | 坑 | 症状 / 判据 |
|---|---|---|
| P76 | 声明与绑定都按名字 | 未声明的 `assign*` 是静默 no-op；“加了声明才算接上” |
| P77 | `skyMap` 与 `diffuseMap` 同一行 | 引擎自带天空程序把 material 纹理当天空；种类对不上采样器 |
| P78 | 影子块按“目标认领”选 | 同一 light 身份 + 深度可采样 + 生产者公布矩阵；不是“第一个深度可采样输入” |
| P79 | 色彩空间契约 | 引擎内部线性；颜色图 `*Srgb`、数据图 `*Unorm`；编码只在硬件两头 |
| P80 | `Material::specular()` 的 alpha 是强度 | 两条路径都真的乘它；默认 0.5 ⇒ 接线会改画面 |
| P81 | G-buffer albedo 用 16F | 8 位暗色阶在亮光下露台阶；`intensity ≤ 1` 时两者一样 |
| P82 | episode 是调用方的 | 半片 / 灯掉落 / 拒绝报告各归各；`ContentPass` 每帧新建，状态挂它会每帧重报 |

## 7. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | dynamic rendering 合并每 pass begin | vsg 记录路径没有 `vkCmdBeginRendering`；被上游阻塞。 |
| D2 | 跨 pass 共享管线状态注册表 | vsg 按 viewID 编管线；共享会拿到不兼容 render pass 的 pipeline。 |
| D3 | 自建管线绕开 vsg | 会与 vsg 的 state 装配 / 记录路径分叉；收益只在冷启动。 |
| D4 | Wayland | 只按 WSI 契约实现；没有真机验证。 |
| D5 | 后端自己再排一次 pass | 调度上移引擎；两套排序 = 两套帧词汇。 |
| D6 | 超过 3 个方向光限制 | 灯块 1024 槽，不限；旧限制已消失。 |

## 8. 判据

| # | 判据 |
|---|---|
| T1 | `test_vsg`：设备无关 + 真实设备电池；`skipped == 0` 才算跑过。 |
| T2 | `scripts/vsg_rewrite_gate.sh`：0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。 |
| T3 | `check_ai_docs.py --strict` / `check_doc_symbols.py` / `check_include_hygiene.py` / `check_export_annotations.py` / `check_diagnostic_formats.py` / `check_public_docs.py`。 |

## 9. 相关文档

`graphics-architecture.md` · `graphics-rendering.md` · `graphics-content-lifecycle.md` · `graphics-followups.md`
