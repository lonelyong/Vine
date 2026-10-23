# vsg 后端：契约、平台事实与坑

> 单一来源：vsg 后端的实现契约。头文件 `RenderBackend.hpp` / `ShaderAbi.hpp` 与
> `src/plugins/gfx_backend_vsg/include/vine/vsg/**` 是 API 细节的权威。待办见
> `graphics-followups.md`。

## 1. 平台地板（硬约束，不是实现选择）

- Vulkan **1.4**；设备必须有 7 个必需 feature；EDS2 / EDS3 + 3 个 fn-pointer 入口。
- **没有 dynamic rendering**（vsg 记录路径只有 `vkCmdBeginRenderPass`）⇒ render pass + load-op variants。
- vsg 按 viewID 编译管线 ⇒ **每 pass 的状态注册表必须私有**。
- push constant 预算 **128 B**；reverse-Z（far = 0）；世界 CCW vs framebuffer CW ⇒ `frontFace = CLOCKWISE`。
- X11 / Win32；**无 Wayland**。
- 在飞槽数是 vsg `Viewer` 的硬编码 3：启动期逐帧学习（`probeSlotCount` / `SlotTracker`），学会前不 park。
- 退役窗口 = `slots + 1`；`RetirementQueue::setSlots()` 原地更新，不要重新赋值。

## 2. 分层

```
gfx_backend_vsg/
  core/     纯策略、无 vsg 类型：协议、帧环、退役、键、目标计划、观测、证据夹具
  api/      vsg 适配：设备、会话、目标、管线、描述符、录制、读回
  support/  宿主窗口、场景规则、状态映射、工具
```

`core/` 不 include `vsg/*`（`check_include_hygiene.py` 守边界）。

## 3. 一帧

- 会话：自有窗口或采纳宿主窗口；窗口 ⇒ 交换链 ⇒ `beginFrame` / `commitFrame`。
- 记录：`Viewer` 的 `RecordAndSubmitTask`，每槽一个 fence；槽数由 `fence(i)` 越界回答逐帧学习。
- 帧协议（引擎侧）：`beginFrame` → 每 pass（`beginPass` / `setPassOrder` / `setPassInputs` / `execute` /
  `endPass`）→ `endFrame` → present。`removePass` → `releasePass(pass)` 再 `releaseRenderTarget(pass->renderTarget())`。
- 预热：启用、非清屏、绑定内容的 pass 走 `beginPass` / `setPassOrder` / `execute` / `endPass`，不 `setPassInputs`。

## 4. 目标与渲染通道

- `TargetShape` 是目标的兼容形状：附件数 / 颜色格式 / 深度格式 / samples / subpass。
- `planClearValues(shape, policy, bootstrap, depth_preserved)`：bootstrap 清所有附件；attachment 0 = policy 色，
  其余 = 透明黑；深度清 `0.0`（reverse-Z far）；`depth_preserved` 优先级最高。
- 深度可借出 / 借入：借入方不自建深度，用出借方的 view；`initialLayout` = attachment layout，LOAD；
  借出方撤销提升（`borrowers` 计数）。
- `VkSubpassDependency` 的 stage/access mask 是 render-pass 兼容性的一部分；借深度时 dependency 必须按形状
  取超集，只有 loadOp/layout 可以每个 pass 不同。
- 读回：颜色按 attachment，深度支持 D32 / D16（D24S8 拒绝）；深度图需要 `TRANSFER_SRC`；
  读回后 barrier 回 attachment layout。

## 5. 管线、变体、状态

- `PipelineKey` 的身份：program + revision + layout + topology + render-pass 兼容性 + samples + variant。
  只有 `operator==` 决定身份；hash 写在旁边。
- `VariantPool` 按 key 缓存；FIFO 上限 65，id 不复用；`keyOf(id)` 可查。
- 每 pass 的 `StateRegistry` 分开记 `variant_switched` 与 `dynamic_issued` 两种成本。
- 动态状态块槽位必须是 `detail::kDynamicStateSlot`（15），否则会替换 pipeline bind。
- 动态 `vkCmdSetPrimitiveTopology` 只能在 pipeline 的 topology class 内；`Topology` 进 `PipelineKey`。
- 管线状态：reverse-Z ⇒ `VK_COMPARE_OP_GREATER`；front face `CLOCKWISE`；混合恒开（单颜色附件内容），
  `BlendState::enabled` 只选因子。
- 设备扩展必须真正 enable（EDS3 的 polygon mode / color blend）；只开 feature bit 不够。

## 6. 流、块、描述符

- `StreamKey` = buffer + **该 buffer 自己的 revision** + 段；共享上传键是整段 buffer（index 归一化到整段）。
- 自定义属性从 location 3 起，转发为 `vine_Attribute{location}`；规范槽位由 `ShaderAbi` 定义。
- `BlockStorage`：一个 mapped buffer（HOST_VISIBLE|HOST_COHERENT）含 view / draw / material 三区；
  对齐用设备的 `minUniformBufferOffsetAlignment`；每帧 ring，slab 数 = 在飞帧 + 1；稳态零写。
- `BlockDescriptors`：一个 set，三个 dynamic uniform binding（0 view / 1 draw / 2 material），
  offset 按 binding 顺序；misaligned offset 拒绝并计数，不交给驱动。
- 材质 arena 每槽 `copies` 份；槽满 `Refused`，不淘汰活材质；扩容只换 material 区，不动 view/draw。

## 7. 纹理上传

- `Texture` 是逻辑描述（kind / size / format / mip / 每层源图）；后端按描述建 `VkImage`。
- `MaterialImages` 上传键 `(Texture*, revision)`；`setImage` 后下一帧重传；sampler 状态按值缓存。
- 上传按 kind 建视图：`D2` = `VK_IMAGE_VIEW_TYPE_2D`，`Cube` = `TYPE_CUBE`；不完整 cube ⇒ 白 cube + 一条 Warning。
- 程序侧在对应 binding 声明 `sampler2D` / `samplerCube`；sampler 不带维度。

## 8. 着色器文件与 ABI

- 文件命名 `builtin_<角色>.<阶段>`（如 `builtin_forward.vert` / `builtin_forward.frag`）；
  `vine_` 前缀是 GLSL 侧的命名约定。
- 构建期嵌入（`VineEmbedShaders.cmake`）；改 shader 重新配置/构建。
- `ShaderAbi.hpp` 是 C++ 与 GLSL 的唯一契约：顶点 location、块布局、push constant 128 B。
- `#pragma import_defines` 决定变体文本；漏写会静默丢弃，文本与顶点布局必须一起变。
- `POINT_LIST` 要求顶点阶段静态写 `gl_PointSize`；内置程序写 1.0。

## 9. 坑

- `vn::String` 是 `std::u8string`；字面量 `u8"..."`，printf/gtest 用 `reinterpret_cast<const char*>(s.data()), s.size()`。
- `PassDesc::camera` 是拥有式 `intrusive_ptr<Camera>`；传栈相机地址会 double-free。
- `vsg::Viewer` 没有 `device` 成员；用 `viewer->deviceWaitIdle()`。
- `vsg::Instance::create` 抛异常（不返回 null）；版本回落循环要 catch。
- `vsg::RenderGraph` 默认 `renderArea` 零面积；必须显式设置，否则 pass 记录了但什么都不清。
- `vsg::DescriptorBuffer::create` 参数顺序是 `(bufferInfoList, DESTINATION BINDING, array element, type)`。
- Vulkan NDC z 就是 depth（没有 OpenGL 的 `z*0.5+0.5`）；reverse-Z far = 0.0，清深度不能清 1.0。
- 设备读回像素是 BGR 字节序；单通道断言用 `isRed()` 等，不要读 `pixel[0]`。
- 改公开签名或改名后必须全量构建；旧插件 `.so` 会 `undefined symbol`。

## 10. 证据与门禁

- `test_vsg`：设备无关 + 真实设备电池；`skipped == 0` 才算跑过设备电池。
- `scripts/vsg_rewrite_gate.sh`：suite 两跑 0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比
  （含软件光栅器上的应用画面检查）。
- 静态门禁：`check_ai_docs.py` / `check_doc_symbols.py` / `check_include_hygiene.py` /
  `check_export_annotations.py` / `check_diagnostic_formats.py` / `check_public_docs.py`。
