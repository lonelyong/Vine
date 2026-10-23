# Graphics 待办总表（唯一清单）

> **读法**：只列仍开放的**触发条件型**条目。已关闭、已明确不做的条目在 2026-10-23 清理中删除，
> 需要历史时看 git。每条只写“是什么 / 触发条件 / 权威出处”，论证在设计文档里。
>
> 现行设计文档：`graphics-architecture.md`（分层 / 设备 / 引擎 / 链 / 一帧）、`graphics-scene.md`
> （节点 / 状态 / 收集 / 层 / 拾取）、`graphics-geometry.md`（通道 / 图元 / 实例 / revision）、
> `graphics-rendering.md`（目标 / MSAA / 图源 / 颜色 / 着色 / 阴影）、
> `graphics-content-lifecycle.md`（释放 / 缓存）、`vsg-backend.md`（vsg 契约与坑）。

## 1. 现在可做（无触发条件）

空。剩下的每条都缺一个真实消费方、一次真实负载，或被上游卡住。

## 2. 触发条件型

### 2.1 内容与场景图

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| LOD / 空间划分 | 无 LOD 节点、无八叉树 / 可见性集合 | 大场景“每帧收集本身成为瓶颈” | `graphics-scene.md` §3 |
| 选择态（描边 / 框选 / 多选） | 拾取是机制、选择不在框架 | 多处调用点各自重写同一套选择与高亮 | `graphics-scene.md` §7 |
| `ViewContext` | 命令取当前视图靠 `MainWindow::primaryRenderControl()` + `MainWindow::current()` | 一个窗口出现第二个 3D 区，或命令要指向非活动窗口 | `SceneView.hpp` / `RenderSurface.hpp` |
| L2b 分层目标（一次写多层） | L2a 已裁决不做；L2b 需要数组层 + 一次写多层的后端能力 | VR / 单 pass 多视图真正落地，或实测 N-pass 开销成为瓶颈 | `graphics-rendering.md` §1 |
| `Topology` strip / fan | 只有 `Triangles` / `Points` / `Lines` 列表 | 导入器需要 strip/fan，且拾取 / 管线映射一并设计 | `graphics-geometry.md` §7 |
| update traversal | 无逐帧节点更新入口（`FrameContext` / `UpdateContext` 没有消费者） | 动画 / 关节回放等需要每帧更新节点状态 | `graphics-scene.md` §6 |
| 灯随节点 | `Light` 在 `Scene` 上，不是节点 | 需要灯跟着子树走 | `graphics-scene.md` §1 |

### 2.2 后端与资源

| 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| `BlockStorageTest` 偶发 | `TheRegionsAreLaidOutOnceAndDoNotOverlap` 首跑红重跑绿 | 第三次出现 | `vsg-backend.md` §9 |
| V2 | `forget(view)` 依赖 vsg `protected` 池 | 升级 vsg 时与 V1 一起复核 | 本行 |
| V3 | 拿不到 `VkPipelineCache`（pinned 与 upstream master 都把第三实参写死 `VK_NULL_HANDLE`） | 推上游；影响集中在冷启动与换会话 | 本行 |
| V4 | dynamic rendering（`vkCmdBeginRendering`）vsg 记录路径不存在 | 上游支持后合并每 pass 的 begin | 本行 |

### 2.3 平台与验证

| 项 | 内容 | 触发条件 |
|---|---|---|
| Windows 窗口读像素 | `TestHostWindow` 是 XCB 窗口；Windows 侧仍 SKIP | 有 Windows 机器时（`vsg-backend.md` §1） |
| Wayland 回退实测 | 只按 WSI 契约实现，未在真 Wayland 上跑过 | 手上有 Wayland 会话（`vsg-backend.md` §1） |
| H2 可选目视 | “边缩放边点菜单”需真实交互桌面 | 可选（`vsg-backend.md` §9） |

### 2.4 对象模型复核遗留

| # | 内容 | 触发条件 | 出处 |
|---|---|---|---|
| R1 | `backend/` 半仍是 Vulkan 策略：`Keys` 的 `VkFormat` / `ImageLayout`、`TargetPlan` 的 subpass；证据夹具（`PhaseTable` / `AllocationGate` / `PixelProbe` / `DepthProbe`）随 SDK 安装 | 第二个后端（D3D12 / Metal / WebGPU）出现 | `graphics-architecture.md` §1 |
| R5 | 材质是单一平铺 Phong + 一张贴图；无 `MaterialInstance`；几何侧 `PbrMaterial` 到渲染侧只能是近似 | 逐对象参数（高亮 / tint）在第二个调用点出现，或需要真实 PBR | `graphics-rendering.md` §5 |
| R8 | 无异步上传 / 编译入口（契约是单线程串行） | 一次加载超过一帧预算 | 本行 |
| R10 | `OwnedFrame` 是零生产消费者的公开类型（仅自测） | 远程渲染 / 录制真正开工时再决定去留 | 本行 |

## 3. 下一 session 的第一条命令

`git log --oneline -1` 确认在 `dev` 上；只从 §2 取一条，触发条件命中才做。全部未命中就不做。
