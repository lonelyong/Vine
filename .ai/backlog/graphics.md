# Graphics 待办：只列开放项

> **一句话**：只列**触发条件型**条目；没有命中触发条件就不做。已关闭、已明确不做的条目已删除，
> 需要历史时看 git。每条只写“是什么 / 触发条件 / 出处”。
>
> 设计来源：`../design/graphics-architecture.md` · `../design/graphics-scene.md` · `../design/graphics-geometry.md` ·
> `../design/graphics-rendering.md` · `../design/graphics-content-lifecycle.md` · `../design/vsg-backend.md`。

## 1. 现在可做（无触发条件）

| # | 项 | 内容 | 出处 |
|---|---|---|---|
| S1 | 参数 Schema P0 | 参数清单 + 生成器（C++ 结构体 / GLSL 片段 / 偏移表），生成结果与现行 `ShaderAbi.hpp` 逐字节一致；先证明等价，不加新能力 | `../design/graphics-shader-params.md` §3.2–3.3、§3.8 |

其余条目都缺一个真实消费方、一次真实负载，或被上游卡住。

## 2. 触发条件型

### 2.1 内容与场景图

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | LOD / 空间划分 | 无 LOD 节点、无八叉树 / 可见性集合 | 大场景“每帧收集本身成为瓶颈” | `../design/graphics-scene.md` §2.3 |
| T2 | 选择态 | 选中集 / 描边 / 框选 / 多选不在框架 | 多处调用点各自重写同一套选择与高亮 | `../design/graphics-scene.md` §2.5 |
| T3 | `ViewContext` | 命令取当前视图靠 `MainWindow::primaryRenderControl()` + `MainWindow::current()` | 一个窗口出现第二个 3D 区，或命令要指向非活动窗口 | `SceneView.hpp` / `RenderSurface.hpp` |
| T4 | L2b 分层目标 | 数组层 + 一次写多层；L2a 已裁决不做 | VR / 单 pass 多视图真正落地，或实测 N-pass 开销成为瓶颈 | `../design/graphics-rendering.md` §2.1 |
| T5 | `Topology` strip / fan | 只有 `Triangles` / `Points` / `Lines` 列表 | 导入器需要 strip/fan，且拾取 / 管线映射一并设计 | `../design/graphics-geometry.md` §5 |
| T6 | update traversal | 无逐帧节点更新入口（`FrameContext` / `UpdateContext` 没有消费者） | 动画 / 关节回放等需要每帧更新节点状态 | `../design/graphics-scene.md` §2.4 |
| T7 | 灯随节点 | `Light` 在 `Scene` 上，不是节点 | 需要灯跟着子树走 | `../design/graphics-scene.md` §2.1 |

### 2.2 后端与资源

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T8 | `BlockStorageTest` 偶发 | `TheRegionsAreLaidOutOnceAndDoNotOverlap` 首跑红重跑绿 | 第三次出现 | 本行 |
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
| T15 | R1 | `backend/` 半仍是 Vulkan 策略：`Keys` 的 `VkFormat` / `ImageLayout`、`TargetPlan` 的 subpass；证据夹具随 SDK 安装 | 第二个后端（D3D12 / Metal / WebGPU）出现 |
| T16 | R5 | 材质是单一平铺 Phong + 一张贴图；无 `MaterialInstance`；PBR 只能近似 | 逐对象参数（高亮 / tint）在第二个调用点出现，或需要真实 PBR |
| T17 | R8 | 无异步上传 / 编译入口（契约是单线程串行） | 一次加载超过一帧预算 |
| T18 | R10 | `OwnedFrame` 是零生产消费者的公开类型（仅自测） | 远程渲染 / 录制真正开工时再决定去留 |

### 2.5 着色参数

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| S2 | 名字表与注入 | `ParameterTable::find` / `Material::set(name, value)` / 编译前注入 ABI 片段 | 宿主需要一个自定义 per-draw 参数（今天四个保留分量不够用），或第二个后端出现 | `../design/graphics-shader-params.md` §3.4–3.6 |
| S3 | 用户扩展点 | 具名用户槽 + 保留 set/binding 段 | S2 落地，且出现宿主自定义 shader 的真实用例 | `../design/graphics-shader-params.md` §3.7 |
| S4 | 第二个后端的 `ParameterSink` | cbuffer / `glUniform*` 等“写字节”实现 | 第二个后端出现（与 T15 同一触发） | `../design/graphics-shader-params.md` §3.5 |

## 3. 下一 session 的第一条命令

`git log --oneline -1` 确认在 `dev` 上；只从 §2 取一条，触发条件命中才做。全部未命中就不做。
