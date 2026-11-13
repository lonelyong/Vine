# Graphics 待办：只列开放项

> **一句话**：只列**触发条件型**条目；没有命中触发条件就不做。已关闭、已明确不做的条目已删除，
> 需要历史时看 git。每条只写“是什么 / 触发条件 / 出处”。
>
> 设计来源：`../design/graphics-architecture.md` · `../design/graphics-scene.md` · `../design/graphics-geometry.md` ·
> `../design/graphics-rendering.md` · `../design/graphics-content-lifecycle.md` · `../design/vsg-backend.md`。

## 1. 现在可做（无触发条件）

| # | 项 | 内容 | 出处 |
|---|---|---|---|
| T35 | 手撸 Vulkan 学习后端（`gfx_backend_vk`，注册名 `"vk"`） | 目标：学习/研究，**不是默认后端**（宿主仍按名字创建 "vsg"）。**M0 已落地（2026-11-13）**：插件 + factory + 设备生命周期 + 诚实的 `capabilities`，`test_vk` 3 例（本机 validation=on、`InitFailed` 诊断 0）、`gfx_backend_vkd.so` 可加载。**M1 已落地（2026-11-13）**：`BarrierPlan` 映射表（`ImageLayout` 当作“用途”，载/存操作一并映射）+ `TargetImages`（RGBA8/D24 图像、视图、render pass、framebuffer）+ 同步提交 + `readColorBuffer`；`test_vk` 10 例（含 `VkOffscreenTest` 的“全像素等于 clear 色”与“未画的 target 答 `NotReady`”），本机 validation 静默。**M2 已落地（2026-11-13）**：`ContentDraw`（`ShaderProgramDescription` → glslang → `VkShaderModule` → `VkPipeline`；`GeometryDescription` 通道 → 顶点/索引 buffer；每 draw 一个 `VineViewBlock`+`VineDrawBlock` UBO；`vkCmdDrawIndexed`）+ `ShaderCompiler`（直接调 glslang，2 例无设备测试）；内容 ABI = set 0 binding 0（view）/ 1（draw）；`test_vk` 14 例（三角形中心 = 片元色、角落 = clear、默认程序路径、validation 静默）。**glslang 来源已定**：provisioning 上移 `cmake/VineGlslang.cmake`、顶层调用（IMPORTED 目标按目录可见），vsg 与 vk 共用，不走 `vsg::ShaderCompiler`。**M3 已落地（2026-11-13）**：`planTarget` 驱动 Rebuild / ResizeInPlace / Repair（resize 只换 image/view/framebuffer，保留 render pass）；pipeline 键 = `RenderPassCompatibility`（resize 不重编，`Device::contentPipelines()` 可观测）；MRT（≤4 颜色附件，每附件 clear / readback）；depth-LOAD 变体（`ClearPolicy::depth == false`）；`StateMapping` 表 + 逐命令 `resolveRenderState`（reverse-Z、CW 前置面、blend / polygon / topology / depth band）；`test_vk` 22 例。**M4 已落地（2026-11-13）**：内容 ABI 对齐引擎（set 0 = material@0 / 五纹理槽 1–5 / lights@6，set 1 = draw@0，相机 = 128B push；`vine_params.glsl` 是唯一权威）；`ContentImages`（按 revision 上传 `Texture`、sampler 值缓存、1x1 白色 stand-in；上传由 `preparePass` 在 render pass 之前记录）；`LightBlock`（view-space、`kMaxLights = 8`）；`recordScreen`（`fullscreenVertexProgram` + 调用方 fragment，source 颜色附件 → set 0 binding 0..N-1）；无 clear pass 由 colour+depth LOAD 变体承载；target 图像加 `SAMPLED` usage；`test_vk` 26 例（材质因子 / 纹理采样 / ambient 灯 / 全屏拷贝 / 半透明叠加）。**M5 已落地（2026-11-13，Handle 模式）**：`WindowTarget`（XCB surface：`XcbSurface` 用 dlopen 的 `xcb_connect`/`xcb_disconnect`，不新增构建依赖；swapchain + D24 + CLEAR/LOAD render pass + 每 image framebuffer；acquire/present 信号量；帧尾 `COLOR_ATTACHMENT → PRESENT_SRC`；resize/out-of-date 重建）；`createSurfaceTarget` 一窗一 target（重复 = `WindowOccupied`）、`DevicePresentation::Handle` 作为创建输入（pre-init target 在 initialize 时 attach）；`executeFrame(frame, present_to)` 画 `target == nullptr` 的 pass 并 present；`capabilities().presentation` 如实；**`Probe` 已落地（2026-11-13）**：`ProbeWindow`（X11 用 dlopen 的 libX11 建窗，Win32 用注册窗口类，macOS 答 `UnsupportedSurface`）+ `initialize()` 在建 instance 前建窗（surface 要在它上面创建）+ `createSurfaceTarget(nullptr)` 取设备自己的 target；`capabilities().presentation` 从创建意图开始回答（None 仍为首帧前 false）；`test_vk` 37 例（Probe 自建窗口 present / 重复 ask = `WindowOccupied` / 窗口重建后再次 present / 拒绝不属于它的窗口）。**已知缺口**：WSLg 的 XGetImage 读不到窗口像素（纯 XCB 填充也读回黑色），像素断言在仪器不可读时跳过。**M6 已落地（2026-11-13）**：`ResidencyLedger` 每类（Streams / Images / Halves，`live` 必答、`bytes` 有账；material block / descriptor set 是逐帧对象，不记行）；`releaseContent` 接上 SDK 动词（Geometry → 其 stream，ShaderProgram → 模块 + 由它编译的 pipeline，Texture → 图像，Material → 认可但无可释放；`Buffer` 重载按 buffer 身份只放它供出的 stream，兄弟 stream 保留）；`prepareFrame`（与 `executeFrame` 同一条解析：program / stream / pipeline / 目标附件，不取图、不录制、不提交、不 present；纹理首帧仍上传，因为 copy 要命令缓冲）；`FrameReport`（本帧 counters + uploads，累计 retention + progress；`uploads` 在首帧前 disengaged，`device_waits` 恒 0——同步设备在帧尾已等待）。`test_vk` 35 例：稳态 0 上传 / 0 编译、prepare 后帧 0 构建且像素不变、四类 release 使对应行 `live`/`bytes` 下降且下一帧重建（几何 2 stream、程序保留 stream 只重编、纹理重传、单 buffer 只重传 1 条）、未测量不声称。**M7 已落地（2026-11-13）**：`BackendParityTest`（`test_vsg`，8 例）把同一个 `FrameDescription` 交给两个后端（都是 `DevicePresentation::None`、同一 64×64 RGBA8+D24 目标、同一份内容）：clear、不透明绘制、深度序、半透明混合、全屏拷贝、纹理采样六类的像素**逐字节相同**，`FrameReport::counters`（passes/draws）与 `PresentOutcome` 相同；"无效果的 pass"（无 clear 无绘制调用）与"entry 不命名 pass"两例各答 0/0 并同样拒绝回读。对拍修掉了三处：① **vk 的投影只折了 Y**（缺 `0.5-0.5z`），深度测试因此保留**更远**的片元（vk 自己的深度用例按错误约定写深度，看不出来）；② **vsg 的 `FrameCounters` 从不复位**，报告逐帧累加（违反 `FrameStats.hpp` 的"每帧"）——复位落在 `FrameRecorder::beginFrame`，`prepareFrame` 恢复计数器（预热不是一个帧）；③ **vsg 无窗口设备的读回不等离屏会话**，回读拿到全 0（有窗口的设备因窗口等待而正确）。`test_vk` 35 例 / `test_vsg` 556 例。**M8 已落地（2026-11-13，引擎自带程序）**：`ProgramVariant`（规则同参考端：材质任一纹理槽 ⇒ `VN_TEXTURED`、几何 location 2 ⇒ `VN_VERTEX_COLOR`、location 8 宽度 ⇒ `VN_TEXCOORD_UV|CUBE`、命令实例流 ⇒ `VN_INSTANCED`）；`compileStage(stage, defines, …)` 用 glslang preamble 注入名字；程序缓存键 =（文本, variant bits）、pipeline 键加 variant；内容集补上引擎 forward 文本无条件声明的阴影 ABI（binding 7 sampler + 8 块，块填 0 = "无阴影到达本 pass"）；实例流按 buffer 地址 + revision 缓存并接上 `releaseContent(Buffer)` 与账本；`test_vk` 48 例（新增 11）、`BackendParityTest` 新增第 9 例：同一个 `unlitForwardProgram()` + 纹理的帧在两后端**逐字节相同**。**下一步**：M0–M8 全部落地；剩余缺口 = WSLg 的 XGetImage 读不到窗口像素（非后端问题）、阴影贴图与延迟路径未做、纹理上传仍发生在首个采样帧的命令缓冲（`prepareFrame` 不提交）。形态、复用清单、里程碑 M0–M8 与成本见评估 §8；**通用化边界（J1–J3 判据 + 现在就通用的清单 + 只留概念项 + 明确不做项 + D1–D5 纪律门）见 §9；Vulkan 暴露到哪一层与可检查的 V1–V4 见 `../research/graphics-2026-11-13-vk-exposure-boundary.md`** | `../research/graphics-2026-11-13-gpu-resource-abstraction.md` §8、§9、`../research/graphics-2026-11-13-vk-exposure-boundary.md`、`../design/graphics-architecture.md` 2.9 |

下一项都在 §2，等各自的触发条件。`T8`（派生纯的并发用例）已于 2026-11-13 落地（`SelectTest.SameInputSameList`）。


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
| T8 | `BlockStorageTest` 偶发 | `TheRegionsAreLaidOutOnceAndDoNotOverlap` 首跑红重跑绿；**证据装置已就位**：用例打印设备的 `minUniformBufferOffsetAlignment` 与三段 region 布局，并用 `SCOPED_TRACE` 把它带进任何失败 | 第三次出现（前两次没有诊断） | `tests/test_vsg/BlockStorageTest.cpp` |
| T24 | 场景收集的失效粒度 | 失效键仍是整份内容——直接路径的 per-view memo 按根子树的 `contentEpoch` 清空，帧路径的 `SnapshotSource` 按 `snapshot->revision`：任一编辑让每个视图的列表重算。**已重测（2026-11-13，RelWithDebInfo，本机 ×3）**：`SelectTest.MeasureWhatOneSelectCostsWithAMovingCamera` —— 2000 drawables：纯 select 337–340 µs、生产 miss 435–443 µs；10000：纯 select 2348–3067 µs、生产 miss 3388–3626 µs。10k 低于 4 ms 门、2000 远低于 2 ms 线 ⇒ **不做优化**；但 10k 本身已越过 2 ms，且生产路径比纯 select 多 ~0.9 ms（memo 查找 + 10k 命令的 shared 分配）——宿主真按 1 万日常场景跑时先看这一半。真触发时的候选：盒 stamp（世界盒已在发布时折好，剩下的是块/子树一级的盒）、命令构造瘦身（10k × `RenderCommand` 的 5 个 `intrusive_ptr`） | Release 下 1 万 drawables 移动相机 select > 4 ms，或 60 fps 预算里 select > 2 ms（已重测：10k 3.1 ms < 4 ms、2000 0.34 ms < 2 ms，均未命中；10k 已越过 2 ms 线本身） | `../design/graphics-scene.md`、`../decisions/ADR-2026-11-09-scene-collection-two-halves.md`、`../design/graphics-threading.md` §3.1.6、`tests/test_graphics/SelectTest.cpp` |
| T10 | V3 | 拿不到 `VkPipelineCache`（vsg 1.1.16 只有 Vulkan 类型声明，没有 cache 管理；本后端也不创建） | 推上游；影响冷启动 / 换会话 | `../design/vsg-backend.md` C6 |
| T11 | V4 | dynamic rendering（`vkCmdBeginRendering`）vsg 记录路径不存在（1.1.16 全树无此调用） | 上游支持后合并每 pass 的 begin | 同上 |
| T27 | 驻留的显存交易复评 | pin 模型（ADR-2026-11-10）让驻留 = 活内容所需：离屏但宿主仍持有的内容不再自动让出显存。若某宿主的显存峰值不可接受，需要重新引入一条时间/容量规则（会与“零时间窗”的判据冲突，必须走新 ADR） | 出现“大场景 + 离线模型常驻”进入显存预算的真实宿主 | `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md` §代价 |
| T30 | GPU 资源抽象（统一类型 / 多态总表） | 现在统一的是**规则与账**（`ResidencyLedger` + pin），不是类型。基类或多态总表只在第二个**API 家族**的后端出现时才值得：届时形状是“SDK 资源描述 + 每后端实现”，不是包 vsg 的基类。**评估已做（2026-11-13）**：规则层完备、类型层无跨 API 消费者；首个同族的第二后端（`vk`）消费的是接缝，不改变本条结论；前置是 T15（设备格式码）与 T32（聚合拆分），但两者都只属于跨 API 那一轮。见 `../research/graphics-2026-11-13-gpu-resource-abstraction.md` | 第二个 **API 家族**的后端（D3D12 / Metal / WebGPU）出现；手撸 Vulkan 同族，不算（评估 §8） | `../decisions/ADR-2026-11-10-gpu-residency-pinned-by-content.md` §选项表、`../research/graphics-2026-11-13-gpu-resource-abstraction.md` |

### 2.3 平台与验证
| # | 项 | 内容 | 触发条件 |
|---|---|---|---|
| T12 | Windows 窗口读像素 | `TestHostWindow` 是 XCB 窗口；Windows 侧仍 SKIP | 有 Windows 机器时 |
| T13 | Wayland 回退实测 | 只按 WSI 契约实现，未在真 Wayland 上跑过 | 手上有 Wayland 会话 |
| T14 | H2 可选目视 | “边缩放边点菜单”需真实交互桌面 | 可选 |
| T33 | 设备创建偶发（疑 WSL） | `VsgBackendTest.ACubeMapInputReachesAContentProgramAsACubeSampler` 首跑 `initialize()` false（约 10.7 s，日志里窗口尺寸报 `4294967295x4294967295`），重跑绿；同一提交下其余用例全部正常。判断：更像是 WSLg 下窗口/设备创建的偶发，不是回归（当时 `git stash` 前后行为一致，且与本次改动无交集）。**下次在 Windows 桌面机器上复跑同一用例**：绿 → WSL 环境问题，删本条；红 → 按设备创建失败查（`VsgHostWindow` 句柄与 `DeviceProbe` 的选择） | 有 Windows 桌面机器时（与 T12 同一轮） |
| T34 | `test_vsg` 的 TSan 构建 | `tests/test_vsg/AllocationCounter.cpp` 替换全局 `operator new/delete`，与 `libclang_rt.tsan_cxx` 的强定义冲突（链接报 multiple definition）；`scripts/tsan_check.sh` 因此只跑 `test_graphics`。T31 的证据是手动从链接命令里去掉 `AllocationCounter.cpp.o` 重链才拿到的。要正式覆盖：给该文件加 sanitizer 守卫（不 announce `countsAvailable`），把依赖计数的用例在 `!countsAvailable()` 时跳过，再让脚本能跑 `test_vsg` 的无设备子集（`StreamUploadsTest.AnEditFromTheHostThread...` 是新 gate 的第一个消费者） | 已命中（T31 修复落地，2026-11-13） | `scripts/tsan_check.sh`、`tests/test_vsg/AllocationCounter.cpp` |
| T36 | `EachAttachmentClearsToItsOwnColour` 偶发 | 2026-11-13 一天内 2 次（全套 5 次运行中）：`VsgBackendTest.EachAttachmentClearsToItsOwnColour` 红，单独重跑与再跑全套均绿。**已带诊断**：失败那次日志里有 `[sdk-pixels] diagnostic: ... a window this device already serves was asked for again`，即 `SdkPixelFixture` 的窗口被同一设备二次索取——更像设备/窗口创建顺序在 WSLg 下的偶发，不是像素逻辑。**判据**：第三次出现时按这条诊断查 `SdkPixelFixture::up()` 与 `VsgBackendTest` 的 fixture 复用（是否同一进程内先建过别的窗口），并考虑把该用例改成“独占 fixture” | 第三次出现（前两次已有诊断） | `tests/test_vsg/VsgBackendTest.cpp`、`tests/test_vsg/SdkPixelFixture.cpp` |

### 2.4 对象模型遗留

| # | 项 | 内容 | 触发条件 |
|---|---|---|---|
| T15 | `backend/` 的设备无关性 | `backend/` 半仍是 Vulkan 策略：`Keys` 的 `VkFormat` / `ImageLayout`、`TargetPlan` 的 subpass；证据夹具随 SDK 安装。**触发前不再新增 Vulkan 拼写**（`../design/vsg-backend.md` C6） | 第二个 **API 家族**的后端（D3D12 / Metal / WebGPU）出现；手撸 Vulkan 同族，不触发 |
| T17 | 异步上传 / 编译入口 | 无异步入口（契约是单线程串行） | 一次加载超过一帧预算 |
| T18 | `OwnedFrame` 的消费者 | `OwnedFrame` 是零生产消费者的公开类型（仅自测） | 远程渲染 / 录制真正开工时再决定去留 |
| T20 | 收集遍历的分派开销 | 已把 `Object` 派生类型的 `dynamic_cast` 换成 `obj_cast`（实测命中 21.7→8.2 ns、未命中 42.2→20.5 ns，Debug / MSVC）。仍可选的是 `contributeState` / `forEachChild` 虚钩子：`Group` / `StateNode` / `Geometry` 的新子类已被现有分派识别，不构成扩展性缺口；只有在替换后 profile 仍指向类型转换时才做。非 `Object` 类型（`CameraManipulator`、第三方 vsg 类型）保留 `dynamic_cast` | 大场景“每帧收集本身成为瓶颈”，且 profile 仍指向类型转换 |
| T32 | `RenderBackend` 聚合拆分 | 拆成 device / executor 两个工厂出口（今天一个聚合同时是 `RenderDevice` 与 `RenderFrameExecutor`，`RenderBackend.hpp` 仍多重继承） | 第一个 device 需要驱动两个 executor，或第一个工厂只想返回 `RenderDevice`；`vk` 直接实现聚合，不触发 |

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
