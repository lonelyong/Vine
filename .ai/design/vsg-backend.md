# vsg 后端：契约、平台事实与坑

> **一句话**：Vulkan 1.4 地板、无 dynamic rendering、每 pass 私有状态表、reverse-Z；
> 后端只物化引擎交来的帧值。
>
> **权威来源**：`RenderBackend.hpp` / `ShaderAbi.hpp` /
> `src/plugins/gfx_backend_vsg/include/vine/vsg/**`。**待办**：`graphics-followups.md`。

## 1. 设计要点

1.1 **core / api / support 分层**：`core` 是纯策略、无 vsg 类型；`api` 做 vsg 适配；`support` 放宿主窗口与工具。
1.2 **帧是值**：后端只按 `FrameDescription.passes` 物化，不重排、不报环。
1.3 **每 pass 状态私有**：vsg 按 viewID 编管线，所以状态注册表不能跨 pass 共享。
1.4 **目标形状驱动**：`TargetShape` 决定图像、通道、管线兼容性；bootstrap 清所有附件。
1.5 **流按 buffer 身份共享**：`StreamKey` = buffer + 自身 revision + 段；稳态零重传。
1.6 **反向 Z + CW 正面**：Vulkan NDC z 就是 depth；世界 CCW 到 framebuffer 是 CW。

## 2. 功能点

### 2.1 平台与设备

| # | 功能 | 说明 |
|---|---|---|
| F1 | 设备地板 | Vulkan 1.4；7 个必需 feature；EDS2 / EDS3 + 3 个 fn-pointer 入口。 |
| F2 | 设备探测 | `DeviceProbe` 枚举 physical device，填 `DeviceFacts`；不与策略分叉。 |
| F3 | 会话 | 自有窗口或采纳宿主窗口；窗口 ⇒ 交换链 ⇒ `beginFrame` / `commitFrame`。 |
| F4 | 槽数学习 | `probeSlotCount` / `SlotTracker`；学会前不 park。 |
| F5 | 退役 | `RetirementQueue` 窗口 = `slots + 1`；`setSlots()` 原地更新。 |

### 2.2 目标与通道

| # | 功能 | 说明 |
|---|---|---|
| F6 | 目标计划 | `TargetShape`：附件数 / 颜色格式 / 深度格式 / samples / subpass。 |
| F7 | 清屏 | `planClearValues`：bootstrap 清所有附件；深度清 0.0（reverse-Z far）。 |
| F8 | 深度借出/借入 | 借入方不自建深度；`initialLayout` = attachment layout，LOAD。 |
| F9 | 依赖 | `VkSubpassDependency` 的 stage/access mask 是兼容性的一部分；借深度取超集。 |
| F10 | 读回 | 颜色按 attachment；深度 D32 / D16（D24S8 拒绝）；barrier 回 attachment layout。 |

### 2.3 管线与状态

| # | 功能 | 说明 |
|---|---|---|
| F11 | 管线键 | program + revision + layout + topology + render-pass 兼容性 + samples + variant。 |
| F12 | 变体池 | `VariantPool` 按 key 缓存；FIFO 上限 65；id 不复用。 |
| F13 | 状态注册 | 每 pass 私有；`variant_switched` / `dynamic_issued` 分开计数。 |
| F14 | 动态状态 | 块槽位 `detail::kDynamicStateSlot`（15）；viewport / scissor 动态。 |
| F15 | 管线状态 | reverse-Z GREATER；frontFace CLOCKWISE；blend 恒开（单颜色附件内容）。 |

### 2.4 流、块、描述符

| # | 功能 | 说明 |
|---|---|---|
| F16 | 流共享 | `StreamKey` = buffer + revision + 段；index 归一到整段 buffer。 |
| F17 | 自定义属性 | location 3 起，转发 `vine_Attribute{location}`。 |
| F18 | 块存储 | 一个 mapped buffer：view / draw / material 三区；每帧 ring；稳态零写。 |
| F19 | 描述符 | 一个 set，3 个 dynamic uniform binding（0 view / 1 draw / 2 material）。 |
| F20 | 材质 arena | 每槽 `copies` 份；槽满 `Refused`；扩容只换 material 区。 |

### 2.5 纹理与着色器

| # | 功能 | 说明 |
|---|---|---|
| F21 | 纹理上传 | `MaterialImages` 键 `(Texture*, revision)`；D2 / Cube 视图；白 cube 兜底。 |
| F22 | sampler | 按值缓存；`InputSetCache` 键 `(view, sampler)`。 |
| F23 | 着色器文件 | `builtin_<角色>.<阶段>`；构建期嵌入；`vine_` 前缀。 |
| F24 | 变体文本 | `#pragma import_defines` 决定文本；漏写会静默丢弃。 |
| F25 | POINT_LIST | 顶点阶段必须静态写 `gl_PointSize`；内置程序写 1.0。 |

## 3. 公开接口

### 3.1 后端契约

| 接口 | 作用 |
|---|---|
| `RenderBackend` | 设备 / 帧 / 呈现三面聚合 |
| `RenderBackendFactory` | 创建后端 |
| `RenderDevice` | 设备、目标、内容释放、读回 |
| `RenderFrameExecutor` | 执行一帧 |
| `PresentTarget` / `WindowPresentTarget` | 呈递契约 / 窗口面 |

### 3.2 插件内关键类型

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

## 4. 注意项（平台事实与坑）

| # | 注意项 |
|---|---|
| N1 | 没有 dynamic rendering：`vkCmdBeginRendering` 在 vsg 记录路径不存在。 |
| N2 | push constant 128 B；块布局不可超。 |
| N3 | reverse-Z：far = 0.0；清深度不能清 1.0，否则 GREATER 拒绝所有片元。 |
| N4 | 世界 CCW vs framebuffer CW：frontFace 必须 CLOCKWISE。 |
| N5 | 设备扩展必须真正 enable（EDS3 polygon / color blend）；只开 feature bit 不够。 |
| N6 | `vsg::Instance::create` 抛异常；版本回落要 catch。 |
| N7 | `vsg::RenderGraph` 默认 `renderArea` 零面积；必须显式设置。 |
| N8 | `vsg::DescriptorBuffer::create` 参数是 `(bufferInfoList, DESTINATION BINDING, array element, type)`。 |
| N9 | 动态拓扑只能在 pipeline 的 topology class 内；`Topology` 进键。 |
| N10 | 设备读回是 BGR 字节序；单通道断言用 `isRed()` 等。 |
| N11 | `vn::String` 是 `std::u8string`；gtest/printf 用 `reinterpret_cast<const char*>`。 |
| N12 | 改公开签名 / 改名后必须全量构建；旧插件 `.so` 会 undefined symbol。 |

## 5. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | dynamic rendering 合并每 pass begin | vsg 记录路径没有 `vkCmdBeginRendering`；被上游阻塞。 |
| D2 | Wayland | 只按 WSI 契约实现；没有真机验证。 |
| D3 | 跨 pass 共享管线状态注册表 | vsg 按 viewID 编管线；共享会拿到不兼容 render pass 的 pipeline。 |
| D4 | 自建管线绕开 vsg | 会与 vsg 的 state 装配 / 记录路径分叉；收益只在冷启动。 |
| D5 | 超过 3 个方向光 | 灯块 1024 槽，不限；旧限制已消失。 |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | `test_vsg`：设备无关 + 真实设备电池；`skipped == 0` 才算跑过。 |
| T2 | `scripts/vsg_rewrite_gate.sh`：0 VUID / 0 SYNC-HAZARD、相位行闭合、应用画面百分比。 |
| T3 | `check_ai_docs.py --strict` / `check_doc_symbols.py` / `check_include_hygiene.py` /
      `check_export_annotations.py` / `check_diagnostic_formats.py` / `check_public_docs.py`。 |

## 7. 相关文档

`graphics-architecture.md` · `graphics-rendering.md` · `graphics-content-lifecycle.md` ·
`graphics-followups.md`
