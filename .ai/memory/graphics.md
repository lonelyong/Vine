# graphics / vsg 后端 —— 现役要点

> 只记**现役**：机制、判据、坑，以及“当时为什么这么定”。逐片历史与重写前那一代在 git；
> 设计文档在 `.ai/design/`，**唯一待办清单**是 `.ai/backlog/graphics.md`。
> 更细的后端契约见 `.ai/design/vsg-backend.md`，坑表见 `.ai/design/vsg-pitfalls.md`。

## 1. 分层与一帧

- SDK：`src/viz/graphics/sdk/vine/graphics`；vsg 后端：`src/plugins/gfx_backend_vsg`（`core/` 纯策略 + `api/` vsg 适配 + `support/`）。
- `RenderEngine`：生命周期 / `frame()` / 设备借用 / 尺寸 / 能力与报告；三个门面是 `PassRegistry`、`OutputRegistry`、`DiagnosticsRoute`。
- `PassChain`：一张画面的配方 + 注册归属。`PassChain::build(engine, ChainDesc)` 建链并注册 pass；析构把手里的 pass 交还引擎；`SceneView::setChain` 采纳它。
- 一帧：`beginFrame` → 每个 pass（`beginPass` / `setPassOrder` / `setPassInputs` / `execute` / `endPass`）→ `endFrame` → present。
- 调度只在引擎（`detail::FrameScheduler`：迭代 Tarjan + 堆就绪队列，稳态零分配）；后端按交来的 `FrameDescription.passes` 物化，不再重排、不报环。
- 后端契约：`RenderBackend.hpp` + `ShaderAbi.hpp`；`backend/` 半的类型在 `core/`、`api/`。

## 2. 内容模型

- `Geometry` = 通道集；`Primitive` = 一次 draw（topology + index span + material / program + instance stream）。
- 索引与实例流只写在 `Primitive`；`Geometry::material` / `program` 是**几何级默认**（图元自己的值优先，再否则最近 `StateNode`）。
- 通道 = `BufferSlice<float>` + components。`setPositions` / `setNormals` / `setTexcoords2` / `setTexcoords3` 共享 buffer；arena 用 slice。
- 实例流：每实例 16 个 float（列主序 mat4）；`visible_instances` 是 draw 参数，不重传；`VINE_INSTANCED` 决定 binding 4 与顶点布局。
- revision：**形状归几何**（`Geometry::setRevision` / `bumpRevision`），**字节归 buffer**（`Buffer::setRevision`）。上传缓存只认 buffer revision；几何 revision 只驱动事实行重建。
- 收集：`Scene::collectRenderCommands` 对每个 primitive 发一条命令，快照 world / state / material / program / instance；按 `drawOrder` 后不透明前向、透明后向排序。
- 包围盒：`Geometry::localBounds` 缓存键 = positions buffer + 其 revision + 段 + 几何 revision；`subtreeBounds` 在节点自身坐标系；`Scene::boundingBox` 只算可见且命中层掩码的内容。

## 3. 目标、层、颜色、图源

- `RenderTarget` 是**描述**，后端按描述造图；`RenderTarget::setSamples` 是 MSAA 唯一入口，采样数进 `TargetShape` ⇒ 管线键 ⇒ 通道/图像。颜色与深度各解析一张单采样图（深度 `sample0`，不平均）。
- 层：`Node::setLayers` + `PassDesc::layer_mask`；最近声明获胜；`Node::subtreeLayerUnion` 是 layer epoch 驱动的懒缓存，三条 walk 在可见性之后先判 `(union & mask) == 0`。
- 颜色：`Colorf` 是线性光，`Color` 是 sRGB 编码字节；`toLinear()` / `toColor()` 一对转换；清屏与 pass 颜色用线性值。
- 图源：`ImageInput` / `ImageSource`；produced（`ImageRef`：target + attachment）与 imported（`Texture`）两种生命周期不合并。`MaterialImages` 上传键 `(Texture*, revision)`，sampler 按值缓存。

## 4. vsg 平台事实（硬约束）

- Vulkan 1.4 地板；EDS2 / EDS3 + 3 个 fn-pointer 入口；**没有 dynamic rendering** ⇒ render pass + load-op variants。
- vsg 按 viewID 编管线 ⇒ 每 pass 的状态注册表必须私有。
- push constant 128 B；reverse-Z（far = 0）；世界 CCW vs framebuffer CW ⇒ `frontFace = CLOCKWISE`。
- X11 / Win32；**无 Wayland**。
- 在飞槽数是 vsg `Viewer` 的硬编码 3：启动期逐帧学习（`probeSlotCount` / `SlotTracker`），学会前不 park。
- 退役窗口 = `slots + 1`；`RetirementQueue::setSlots()` 原地更新，不要重新赋值（会清零计数）。

## 5. 坑（不可再踩）

- `vn::String` 包的是 `std::u8string`；字面量写 `u8"..."`，printf/gtest 用 `reinterpret_cast<const char*>(s.data()), s.size()`。
- `PassDesc::camera` 是**拥有式** `intrusive_ptr<Camera>`；传栈相机地址会 double-free。
- `vsg::Viewer` 没有 `device` 成员；停设备用 `viewer->deviceWaitIdle()`。
- `vsg::Instance::create` **抛异常**（不返回 null）；版本回落循环要 catch。
- `vsg::RenderGraph` 默认 `renderArea` 是零面积；必须显式设置，否则 pass 记录了但什么都不清。
- `vsg::DescriptorBuffer::create` 参数顺序是 `(bufferInfoList, DESTINATION BINDING, array element, type)`。
- 动态状态块槽位必须是 `detail::kDynamicStateSlot`（15），否则会替换 pipeline bind。
- `VkSubpassDependency` 的 stage/access mask 是 render-pass 兼容性的一部分；借深度时 dependency 必须按形状取超集。
- 设备扩展必须真正 enable（EDS3 的 polygon mode / color blend）；只开 feature bit 不够，validation layer 才看得见。
- Vulkan NDC z 就是 depth（没有 OpenGL 的 `z*0.5+0.5`）；reverse-Z far = 0.0，清深度不能清 1.0。
- 动态 `vkCmdSetPrimitiveTopology` 只能在 pipeline 的 topology class 内；`Topology` 进 `PipelineKey`。
- `POINT_LIST` 要求顶点阶段静态写 `gl_PointSize`；内置程序写 1.0。
- 透明度：引擎唯一透明通道是 `Node::opacity`；材质不贡献 alpha；blend 恒开，`enabled` 只选因子。
- 设备读回像素是 BGR 字节序；单通道断言用 `isRed()` 等，不要读 `pixel[0]`。
- 改公开签名或改名后必须全量构建；旧插件 `.so` 会 `undefined symbol`。

## 6. 证据与门禁

- `test_graphics`（模型无关）、`test_vsg`（后端，含真实设备用例）、`test_gui`、`test_core`。
- `scripts/vsg_rewrite_gate.sh`：suite 两跑 0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。
- 静态门禁：`check_ai_docs.py`、`check_doc_symbols.py`、`check_include_hygiene.py`、`check_export_annotations.py`、`check_diagnostic_formats.py`、`check_public_docs.py`。
- `scripts/asan_check.sh` / `VINE_ASAN_*`；`skipped == 0` 才算跑过设备电池。
