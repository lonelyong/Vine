# Graphics 待办：只列开放项

> **一句话**：只列**触发条件型**条目；没有命中触发条件就不做。已关闭、已明确不做的条目已删除，
> 需要历史时看 git。每条只写“是什么 / 触发条件 / 出处”。
>
> 设计来源：`../design/graphics-architecture.md` · `../design/graphics-scene.md` · `../design/graphics-geometry.md` ·
> `../design/graphics-rendering.md` · `../design/graphics-content-lifecycle.md` · `../design/vsg-backend.md`。

## 1. 现在可做（无触发条件）

材质 PBR 的四步（模型 / 纹理集 / 两条路径的 BRDF / 实例与 alpha 模式）与 F11 的逐材质诊断已全部落地。

- **graphics-next 的接管（已采纳：`../decisions/ADR-2026-11-12-adopt-graphics-next.md`）**：目标形状、
  `C1–C12` / `R1–R39` / `I1–I7` / `N1–N10` 与迁移表在 `../design/graphics-frame-pipeline.md`。
  未做的是 §9.3 的步 4、5b-2c、6（多表面 → pass 声明收口 → 原型/实例），以及 F11/F12 的全序键与 `query`。
  5a、5b-1、5b-2a 与 5b-2b 已落地 2026-11-12：Published 模式绑定 `SnapshotSource`，公开 `Scene::publish()` 三态，
  `PublicationStore` 直接持有 `SnapshotSource`（W3 拆自 `FrameWorker`），旧 `ContentMirror` / `DeltaChannel` / `RenderWorld` / `PublishOutcome`
  与 23 条 LEGACY 用例已删除。
  5a/5b-1/5b-2a/5b-2b/5b-2c 的拆分理由见 `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`；
  A6 的测量口径见 `../research/graphics-2026-11-12-instances.md`。每步一个可回退提交；
  某步击穿它自己的 `K#` ⇒ 回退该步并把结论写回 ADR，**不重开“是否采纳”**。
  5b-2c 的 pass 声明收口不在“现在可做”里：触发条件是 5b-2b 稳定一轮、第二后端要复用内容层，或 host
  需要计划之外的第二套 pass 形状；A6 的触发条件是出现“一处原型多处放置”的真实宿主。未命中就只写设计（`G8`）。

### 1.1 第三轮审计修复（2026-11-12，W1–W9；W9 按 5b-2c 触发条件）

证据、代码位置与完整论证见 `../research/graphics-2026-11-12-audit-3.md`（R3-1…R3-8）。
**顺序即依赖**：先做零语义风险的，再修正确性，公共 API 的形状变化集中在 W9 一个窗口。

| # | 项 | 做什么 | 为什么 / 代价 | 判据 / 前置 |
|---|---|---|---|---|
| W9 | 5b-2c 收口批 | 删 `addPass` 家族、隐式发布、活指针字段、`Scene` 的 `DrawListSource` 继承（W6b）；`FrameRecorder` 三阶段降为后端组件 | 让“计划是唯一形状”从承诺变事实的唯一一步。**W9a / W9b / W9c / W9d 已落地 2026-11-13**；剩下 W9e（W7 结果形状 + `FramePlan`→`FrameTable`，与 `frame()`/`applyPlan` 的返回形状同窗）。W9d 的边界答案：计划是唯一的形状，**作用域形态**（`applyPlan(plan, owner)`，身份 `(owner, name)`）让一个引擎可以有多个贡献者而不互删 | W9e 开工前先定 `frame()` 的花钱/不花钱两态与 `applyPlan` 的三态答案；`K4` 分母 1096、5% 线 55；`K5` 与 2519 同口径；`T7` / `T30` |
| W7 | 结果形状收口 | `frame()` 只答 Ran / Submitted；`applyPlan` 三态 `{applied, problems, refused}`；`SessionBusy` 移出 `PlanProblemCode` | 7 个枚举、121/163 个 `bool`；3 组是同一件事的不同拼法。全是公开签名，必须与 W9 同窗，否则迁移期三套并存。代价：`TheCodeSpellingsAreStable` 等断言改 | `PlanTest` 断言 busy 不是 plan problem |
| W6b | `Scene` 外移第二步 | 删 `Scene` 的 `DrawListSource` 继承；`RenderPass::describe` 不再把活 `Scene` 写进帧值 | 去掉“活 Scene 过界”的最后一条根；与 W9 同窗，避免两套收集形状并存 | `Scene.hpp` 不再 override `collectRenderCommands`；`T34` 绿 |

审计里“现在不做”的三项已对应本文件 T27（pin 显存）/ T24（F11 派生层并行）/ T22 与 T15（第二个后端），不重复列。


## 2. 触发条件型

### 2.1 内容与场景图

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | LOD / 空间划分 | 无 LOD 节点、无八叉树 / 可见性集合 | 大场景“每帧收集本身成为瓶颈” | `../design/graphics-scene.md` §2.3 |
| T2 | 选择态 | 选中集 / 描边 / 框选 / 多选不在框架 | 多处调用点各自重写同一套选择与高亮 | `../design/graphics-scene.md` §2.5 |
| T4 | L2b 分层目标 | 数组层 + 一次写多层；L2a 已裁决不做 | VR / 单 pass 多视图真正落地，或实测 N-pass 开销成为瓶颈 | `../design/graphics-rendering.md` §2.1 |
| T6 | update traversal | 无逐帧节点更新入口（`FrameContext` / `UpdateContext` 没有消费者） | 动画 / 关节回放等需要每帧更新节点状态 | `../design/graphics-scene.md` §2.4 |
| T7 | 节点灯 / 逐 draw 选灯 | `Light` 在 `Scene` 上且是世界坐标：灯不能跟着子树动，也没有“这几盏灯照这个 draw”（灯块按声明顺序取前 8 盏）。两者是同一个模型的缺口 | 出现挂在会动物体上的灯，或一场景灯数远超槽数且掉落影响画面 | `../design/graphics-scene.md` §2.7、`../design/graphics-shader-params.md`、本文件 S7 |
| T23 | 点云渲染桥 | `PointCloud` 已在 geometry，但 `geometryFromShape()` 只接网格：点云要变成 `Topology::Points` 的 `Geometry`（位置/法线/颜色三个通道原样共享，点大小由程序写）才会被画出来 | 出现要显示点云文件（PLY 等）或点云节点的消费者 | `../design/graphics-geometry.md` 3.6 F31 |

### 2.2 后端与资源

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T8 | `BlockStorageTest` 偶发 | `TheRegionsAreLaidOutOnceAndDoNotOverlap` 首跑红重跑绿 | 第三次出现 | 本行 |
| T22 | 内容层提成共享库 | 事实表/半边/键这套记账已与 vsg 无关（`content_seam` 守卫），但仍在插件目录里；提成 SDK 库就能给第二个后端直接复用 | 第二个后端（D3D12/WebGPU/软件）落地，或出现第二个渲染路径 | `../design/vsg-backend.md` 2.8 |
| T24 | 场景收集的失效粒度 | memo 的失效键是整个场景根的 `contentEpoch`，换了视图就要重走。**第一半已落地**（ADR-2026-11-09-scene-collection-two-halves）：图依赖的一半（世界矩阵 + opacity/layers/drawOrder 折叠）按 `contentEpoch` 烘焙，选取只做层并集剪枝 + 视锥 + 排序；世界盒仍每视图问（盒子的键是流的 revision，`Buffer::edit()` 不惊动图）。**Release 复测**：2000 drawables 移动相机 **1597 µs（0.94 µs/命令）**、200 个 141 µs、稳态帧 0 µs；归因：box 变换 + `BoundsCache` 哈希 = 317 µs（20%），每条命令的构造/解析 ~1.3 ms（71%），排序 7 µs。 | **触发条件未命中**（真实成本 1.6 ms/帧，不是 Debug 的 13.8 ms）：Release 下 1 万 drawables 移动相机 > 4 ms，或 60 fps 预算里收集 > 2 ms，才做\"烘焙世界盒 + 盒 stamp\"或命令构造瘦身（前置：buffer 编辑→场景的公告路径要说清） | `../design/graphics-scene.md`、`Scene.cpp` 的 `detail::SceneBake`、ADR 里的两张测量表、`../design/graphics-threading.md` §3.1.6（并行 select 的接缝与验收） |
| T9 | V2 | `forget(view)` 依赖 vsg `protected` 池 | 升级 vsg 时与 V1 一起复核 | 本行 |
| T10 | V3 | 拿不到 `VkPipelineCache`（第三实参写死 `VK_NULL_HANDLE`） | 推上游；影响冷启动 / 换会话 | 本行 |
| T11 | V4 | dynamic rendering（`vkCmdBeginRendering`）vsg 记录路径不存在 | 上游支持后合并每 pass 的 begin | 本行 |
| T25 | 每帧上传数当观测指标 | `FrameCounters` 只有 passes / draws / invalid_schedules；`StreamUploads::uploads()` 与 `MaterialImages::count()` 的每帧差值能回答“这一帧传了多少”，但今天没有消费者 | HUD / 门禁 / 性能面板要显示“这一帧上传了几个流 / 多少字节” | `../design/graphics-content-lifecycle.md` D10 |
| T26 | 每帧上传的宿主的暂存重分配 | 现行规则是“fence 一完成就清”，所以每帧都上传的宿主会在每帧重新分配一块最小 16 MB 的暂存。仓库里目前没有这样的宿主（帧内上传都是 revision 变化才发生） | 出现每帧上传的宿主（动态几何 / 流式点云），且分配进入 profile：加回“只清大于 `minimumStagingBufferSize` 的块”这一条判断（一行） | `../design/graphics-content-lifecycle.md` D16、`../decisions/ADR-2026-11-10-upload-staging-is-not-retained.md` |
| T27 | 驻留的显存交易复评 | pin 模型（ADR-2026-11-10）让驻留 = 活内容所需：离屏但宿主仍持有的内容不再自动让出显存。若某宿主的显存峰值不可接受，需要重新引入一条时间/容量规则（会与“零时间窗”的判据冲突，必须走新 ADR） | 出现“大场景 + 离线模型常驻”进入显存预算的真实宿主 | `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md` §代价 |
| T29 | 驻留账的帧级增量 | `RetentionStats::residency`（ADR 第一步）答的是“现在多少”；T25 要的是“这一帧传了多少 / 释放了多少”，即两次报告之差 | 与 T25 同一触发 | `../design/graphics-content-lifecycle.md` §3.3 |
| T30 | GPU 资源抽象（统一类型 / 多态总表） | 现在统一的是**规则与账**（`ResidencyLedger` + pin），不是类型。基类或多态总表只在第二个后端出现时才值得：届时形状是“SDK 资源描述 + 每后端实现”，不是包 vsg 的基类 | 第二个后端出现（与 T15 同一触发） | `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md` §选项表 |
| T31 | 字节 revision 的跨线程轮询 | `StreamUploads::beginFrame` 在引擎线程比较活 `Buffer` 的 `revision()`，而那是普通计数器；流式写入与引擎帧并发时是未定义行为。修法不是改 atomic（只修一个字段），而是让写入方在发布边界声明一个值（`(buffer, revision)` 声明），驻留按声明判活。帧载荷的程序/材质/灯值已走值通道（W1） | T17 异步上传 / 编译落地，或流式写入真的与引擎线程并发 | `../design/graphics-threading.md` §2.4、R3-1 |

### 2.3 平台与验证
| # | 项 | 内容 | 触发条件 |
|---|---|---|---|
| T12 | Windows 窗口读像素 | `TestHostWindow` 是 XCB 窗口；Windows 侧仍 SKIP | 有 Windows 机器时 |
| T13 | Wayland 回退实测 | 只按 WSI 契约实现，未在真 Wayland 上跑过 | 手上有 Wayland 会话 |
| T14 | H2 可选目视 | “边缩放边点菜单”需真实交互桌面 | 可选 |

### 2.4 对象模型遗留

| # | 项 | 内容 | 触发条件 |
|---|---|---|---|
| T15 | `backend/` 的设备无关性 | `backend/` 半仍是 Vulkan 策略：`Keys` 的 `VkFormat` / `ImageLayout`、`TargetPlan` 的 subpass；证据夹具随 SDK 安装。**触发前不再新增 Vulkan 拼写**（`../design/vsg-backend.md` C6） | 第二个后端（D3D12 / Metal / WebGPU）出现 |
| T17 | 异步上传 / 编译入口 | 无异步入口（契约是单线程串行） | 一次加载超过一帧预算 |
| T18 | `OwnedFrame` 的消费者 | `OwnedFrame` 是零生产消费者的公开类型（仅自测） | 远程渲染 / 录制真正开工时再决定去留 |
| T20 | 收集遍历的分派开销 | 已把 `Object` 派生类型的 `dynamic_cast` 换成 `obj_cast`（实测命中 21.7→8.2 ns、未命中 42.2→20.5 ns，Debug / MSVC）。仍可选的是 `contributeState` / `forEachChild` 虚钩子：`Group` / `StateNode` / `Geometry` 的新子类已被现有分派识别，不构成扩展性缺口；只有在替换后 profile 仍指向类型转换时才做。非 `Object` 类型（`CameraManipulator`、第三方 vsg 类型）保留 `dynamic_cast` | 大场景“每帧收集本身成为瓶颈”，且 profile 仍指向类型转换 |
| T22 | `RenderBackend` 聚合拆分 | 拆成 device / executor 两个工厂出口（今天一个聚合同时是 `RenderDevice` 与 `RenderFrameExecutor`） | 第一个 device 需要驱动两个 executor，或第一个工厂只想返回 `RenderDevice` |

### 2.5 着色参数

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| S2 | 名字表与注入 | `ParameterTable::find` / `Material::set(name, value)` / 编译前注入 ABI 片段 | 宿主需要一个自定义 per-draw 参数（今天四个保留分量不够用），或第二个后端出现 | `../design/graphics-shader-params.md` §3.4–3.6 |
| S3 | 用户扩展点 | 具名用户槽 + 保留 set/binding 段 | S2 落地，且出现宿主自定义 shader 的真实用例 | `../design/graphics-shader-params.md` §3.6 |
| S4 | 第二个后端的 `ParameterSink` | cbuffer / `glUniform*` 等“写字节”实现 | 第二个后端出现（与 T15 同一触发） | `../design/graphics-shader-params.md` §3.5 |
| S5 | `VineLightsBlock` 上移 SDK | 灯块的 GLSL 已归 `vine_params.glsl`，C++ 仍在后端 `LightBlock.hpp`；上移让布局与声明同模块 | 第二个后端需要同一份灯布局（与 T15 / S4 同一触发） | `../design/graphics-shader-params.md` §3.3 |
| S6 | 逐灯阴影 / 点光阴影 | 今天只有一个投射灯、一张 2D 深度图：方向光（正交光相机）与聚光（透视光相机）都走它；点光需要六面 cube 深度目标 + 多矩阵阴影 ABI（6 个 `viewToLight` 或一个 cube 采样器），即层化目标能力。`Light::setCastShadow` 在点光上不建 pass 并报告 | 点光投影的真实需求，或第二个投射灯 | `../decisions/ADR-2026-10-24-light-block-typed-lights.md` |
| S7 | 逐对象选灯 | 今天按声明顺序取前 8 盏，超出的按 episode 报掉落；一场灯很多时需要"选哪几盏照这个 draw" | 一次场景的灯数远超 8 且掉落可见地影响画面 | `../decisions/ADR-2026-10-24-light-block-typed-lights.md` |

## 3. 下一 session 的第一条命令

`git log --oneline -1` 确认在 `dev` 上；只从 §2 取一条，触发条件命中才做。全部未命中就不做。
