# graphics 抽象层优化清单（2026-10-19）

> **结论**：本轮只收口“契约文字”，不改公开形态。结构性条目按触发条件登记，等下一次公开面变更或真实负载命中再做。
> 依据：graphics SDK 公开头与 `graphics-redesign.md` / `graphics-layering.md` /
> `graphics-object-model-vs-3d-frameworks.md`。

## 0. 判断口径

优化的目标不是“更像某个引擎”，而是让**概念边界**自洽：

- 一个事实只有一个权威；
- 声明、解析、编译、执行、证据分开；
- 缺席（没人说）与零（说了零）分开；
- 机制在 SDK，策略在应用层。

## 1. 本轮已收口

| # | 问题 | 落点 |
|---|---|---|
| A1 | `FrameDescription` 被称作 value，实际是 borrowed snapshot | `FrameDescription.hpp` 类注释；`RenderEngine::frameDescription()` |
| A2 | 顺序有两个权威：announced order vs dependency schedule | `FrameDescription.hpp`、`RenderFrameExecutor.hpp`、`FramePlan.hpp`、`RenderEngine::framePlan()` |
| B1 | `PassKind` 封闭集 vs `RenderPass::describe()` 逃生口 | `PassDesc.hpp` 的 `PassKind` 注释 |
| C1 | `Pipeline` 与 PSO 的命名碰撞 | `RenderPipeline.hpp` 类注释 |
| C2 | `FramePlan` 与 `FrameDescription` 两个帧值的读者不同 | `FramePlan.hpp` 类注释 |
| D1 | `program` 不在 state 里 | 已由 `StateNode.hpp` 说明；本轮不改 |
| E1 | `ImageRef`（pass 产出）与 `Texture`（导入）是两个图源根 | `Texture.hpp` 与 `ImageRef.hpp` 互指 |
| F1 | 设备像素/逻辑像素的边界 | `Viewport.hpp` 注释（并指向 `SceneView::resize` / `Pipeline::resize`） |
| F2a | 逻辑→设备像素换算分散在三处 | 私有 `PixelGeometry.hpp`（`toDeviceSize`）；`Pipeline` / `AxisGizmo` / `FpsOverlay` 共用（见 §1.2） |
| G1 | `RenderEngine` 角色过载（`publish`/`resolve` + transient） | 拆出私有 `FrameResources`（见 §1.1）；`RenderEngine` 保留 pass/顺序/编译/诊断 |

### 1.1 G1 落地（2026-10-19）

- **新私有类型** `FrameResources`（`FrameResources.hpp` / `FrameResources.cpp`，不导出）：拥有三类“关于图像”的表——宿主 standing binding（`publish`/`unpublish`）、当前帧的 production/publication 表、transient target 表。`RenderEngine` 仍然拥有 pass 注册、顺序、编译、诊断路线和 episode 规则。
- **公开面不变**：`RenderEngine.hpp` 只多了一个前置声明和一个 `std::unique_ptr<detail::FrameResources>`；`publish`/`resolve`/`unpublish`/`transientTargetCount`/`transientTarget` 的签名与行为不变。
- **顺带修掉两个真实的生命周期缺陷**：
  - `clearPasses()` 原来先 `slots_.clear()`，再用 `raw_ptr<RenderPass>` 去 `releasePass` / 读 `renderTarget()`；现在先持有 `intrusive_ptr<RenderPass>`，pass 在释放期间保证存活。
  - `VsgBackend::~VsgBackend()` 原来是 `= default`，`Data` 的成员顺序会先销毁 `DeviceClock` / `RetirementQueue` / device，再销毁仍借用它们的 `OffscreenSession`；ASan 抓到 `OffscreenSession::shutdown -> DeviceClock::removeWindow` 的 heap-use-after-free。析构现在调用幂等的 `shutdown()`，由它决定销毁顺序。
- **判据**：构建 0 error / 0 warning；`test_vsg` 461 通过；`ctest` 24/24；8 条 hygiene 门禁 0 finding。

### 1.2 F2a 落地：逻辑→设备像素换算（2026-10-19）

- **新私有头** `PixelGeometry.hpp`：`LogicalSize` / `DeviceSize` / `PixelRatio` / `toDeviceSize`；换算规则只有一处（截断，与宿主自己的缩放一致），`EngineCompile.cpp` 的 `kShapeTolerance` 说明改为指向它。
- **落点**：`Pipeline::resize`（离屏目标）、`AxisGizmo::onSurfaceResized`、`FpsOverlay::onSurfaceResized`——三处原来各自 `static_cast<int>(logical * ratio)`，现在都调用 `toDeviceSize`。
- **公开面不变**：`Viewport`、`SceneView::resize`、`Pipeline::resize` 的签名不动；这次只把“内部换算”收成一个有名字的类型边界。
- **未做**：色彩空间（linear/sRGB）与数值域（0–255 / 0–1）仍在注释里，按 F2b 的触发条件。
- **判据**：构建 0 error / 0 warning；`ctest` 24/24；8 条 hygiene 门禁 0 finding。

## 2. 待办（带触发条件）

| # | 优化项 | 抽象问题 | 动作 | 触发条件 |
|---|---|---|---|---|
| H1 | 能力模型偏“懒惰” | `Capabilities` 只放预检事实，其余靠调用结果与诊断 | 第二个后端或 MSAA/compute 立项时加 `Feature` 枚举 | 第二个图形家族 / 新的预检问题 |
| F2b | 色彩空间与数值域未进类型 | linear/sRGB、0–255/0–1 靠注释 | 强类型 `LinearColor` / `SrgbColor` / `UnitScalar` | **本轮不做（用户决定 2026-10-19）** |
| L1 | 帧 IR 没有 owned 形式 | 无法离线/序列化/跨线程 | `OwnedFrame` 或 `FrameDescription` 的 owned 兄弟 | 离线/远程渲染或帧录制 |
| I1 | `RenderTarget::shareDepth` 把调度边藏进描述 | 资源图的边表达成字段 | 独立 `DepthView` / `ImageView` 关系 | 第三个共享 depth 的消费者或排序缺陷 |
| J1 | `SceneView` 门面角色偏多 | 相机/内容/操纵器/管线/区/输入汇于一身 | 维持门面；必要时拆 `InputRouter` / `LayoutParticipant` | 多视图/输入路由重新进入 SDK |
| B2 | `RenderPass::execute` 仍是虚函数 | 值路径与逃生口并存 | 保持现状；主路写死为 `describe()` | 已具备，仅登记 |

> E2 已于 2026-10-19 落地（`114e0465`），其收尾项（全屏 ABI imported 输入已落地；按 attachment 绑定、revision 失效、sampler state、Cube 仍登记）不在本表，见
> `graphics-followups-2026-10-19.md` §2；P1 的开工顺序与 F2b 的“不做”决定也在那份任务列表里。
> K1 已于 2026-10-19 落地（`Pipeline` → `RenderPipeline`，见任务列表 §1.1）。

## 3. 明确不做

- 不改 `RenderTarget` 的名字（S10a 已裁决：它是描述，不是资源）。
- 不现在拆 `RenderBackend` 聚合（三张契约已各自成头，聚合是过渡）。
- 不引入通用 `std::variant` / `std::any` 的帧图节点（会破坏 `PassKind` 的可读性）。
- 不为“将来可能”加能力标志（触发条件命中再加）。

## 4. 判据

- 契约文字批：全量构建 0 error / 0 warning；8 条 hygiene 门禁 0 finding；不跑测试（comment-only 口径）。
- G1 批：构建 0 error / 0 warning；`test_vsg` 461 通过；`ctest` **24/24**；8 条 hygiene 门禁 0 finding。
- F2a 批：构建 0 error / 0 warning；`ctest` **24/24**；8 条 hygiene 门禁 0 finding。
