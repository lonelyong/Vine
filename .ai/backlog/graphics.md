# Graphics 待办：只列开放项

> **一句话**：只列**触发条件型**条目；没有命中触发条件就不做。已关闭、已明确不做的条目已删除，
> 需要历史时看 git。每条只写“是什么 / 触发条件 / 出处”。
>
> 设计来源：`../design/graphics-architecture.md` · `../design/graphics-scene.md` · `../design/graphics-geometry.md` ·
> `../design/graphics-rendering.md` · `../design/graphics-content-lifecycle.md` · `../design/vsg-backend.md`。

## 1. 现在可做（无触发条件）

（无：材质 PBR 的四步（模型 / 纹理集 / 两条路径的 BRDF / 实例与 alpha 模式）与 F11 的逐材质诊断已全部落地。）

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
| T24 | 场景收集的失效粒度 | memo 的失效键是整个场景根的 `contentEpoch`，换了视图就要重走。**第一半已落地**（ADR-2026-11-09-scene-collection-two-halves）：图依赖的一半（世界矩阵 + opacity/layers/drawOrder 折叠）按 `contentEpoch` 烘焙，选取只做层并集剪枝 + 视锥 + 排序；世界盒仍每视图问（盒子的键是流的 revision，`Buffer::edit()` 不惊动图）。**Release 复测**：2000 drawables 移动相机 **1597 µs（0.94 µs/命令）**、200 个 141 µs、稳态帧 0 µs；归因：box 变换 + `BoundsCache` 哈希 = 317 µs（20%），每条命令的构造/解析 ~1.3 ms（71%），排序 7 µs。 | **触发条件未命中**（真实成本 1.6 ms/帧，不是 Debug 的 13.8 ms）：Release 下 1 万 drawables 移动相机 > 4 ms，或 60 fps 预算里收集 > 2 ms，才做\"烘焙世界盒 + 盒 stamp\"或命令构造瘦身（前置：buffer 编辑→场景的公告路径要说清） | `../design/graphics-scene.md`、`Scene.cpp` 的 `detail::SceneBake`、ADR 里的两张测量表 |
| T9 | V2 | `forget(view)` 依赖 vsg `protected` 池 | 升级 vsg 时与 V1 一起复核 | 本行 |
| T10 | V3 | 拿不到 `VkPipelineCache`（第三实参写死 `VK_NULL_HANDLE`） | 推上游；影响冷启动 / 换会话 | 本行 |
| T11 | V4 | dynamic rendering（`vkCmdBeginRendering`）vsg 记录路径不存在 | 上游支持后合并每 pass 的 begin | 本行 |

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
| T21 | `Node` 测试见证物收窄 | `boundsRecomputeCount` / `subtreeLayerRecomputeCount` 是测试见证物，现为公开 API 且每节点 16 B；收成 `NodeCacheStats` 单一入口，`contentEpoch` 私有 + `friend Scene` | 下一次改 `Node.hpp` 公开面或布局，或见证物增到 3 个 |
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
