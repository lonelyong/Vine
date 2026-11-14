# ADR-2026-11-14：`gfx_backend_vk` 转为生产后端，最终替代 `vsg`

- 状态：已采纳
- 日期：2026-11-14
- 相关：`../backlog/graphics.md` T35、`../research/graphics-2026-11-13-gpu-resource-abstraction.md` §8–§9、
  `../research/graphics-2026-11-13-vk-exposure-boundary.md`、
  `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/WindowSystem.hpp`、
  `src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/Device.hpp`、`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`

## 背景

`gfx_backend_vk` 今天的定位写在三处：backlog T35（“学习/研究，**不是默认后端**”）、插件 `CMakeLists.txt`
（“It is NOT the production backend - nothing selects it implicitly”）与 research §8。功能上它已经走完
M0–M13：离屏与窗口、内容 ABI、阴影、延迟链、MSAA（含深度 resolve）、暴露边界门、预热传输批，`test_vk`
62 例，且与 `vsg` 有 12 例逐字节对拍。它缺的不再是“能不能画”，而是**生产后端要回答的那几件事**：

- `RenderDevice::releaseRenderTarget` 没被重写（`vsg` 有）：宿主丢弃的目标，附件驻留到 `shutdown()`；
- `RenderDevice::readDepthBuffer` 没被重写（`vsg` 有）：同一个读取在两个后端得到两种答案；
- 设备丢失（`VK_ERROR_DEVICE_LOST`）只折成“提交失败”，没有可定位的诊断；
- 平台矩阵只有 Linux/X11 有实测（Win32 分支从未构建，macOS 不支持，Wayland 未测）；
- 没有帧时间/加载时间的测量；没有 pipeline cache；
- `docs/guides/` 没有 vk 页。

同时，用户决定改变目标：**vk 最终替代 `vsg` 成为默认后端**。本 ADR 只裁决“怎么走过去”，不裁决“何时切”。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 维持学习后端定位 | 不动任何东西 | 与目标相反；缺口永远不修（`G8` 的反面：能力有真实消费者却不做） |
| B 立刻把默认后端换成 `vk` | 目标一步到位 | 目标释放与深度回读的缺口会在第一个真实宿主上暴露；平台只有 X11；无性能证据（`G10` / `G11`） |
| C **先生产化，达标后切默认** | 缺口逐条关闭、每条有测试；`vsg` 在达标前仍是默认，回退路径天然存在 | 一段时间要维护两个后端；切换门必须写成可检查的清单，否则会停在“差不多” |
| D 双后端长期并存、都不叫默认 | 没有切换风险 | 宿主必须自己选；两个后端都要付生产化成本（`G5` / `G8`） |

## 决定

选 **C**：把 `gfx_backend_vk` 的目标改为**生产后端**，按下面的规则推进，达标后切换默认。

1. **平台顺序**：Linux/X11 是第一支持目标（今天已有实测）；**Win32 第二**；其它平台（含 macOS）诚实答
   `UnsupportedSurface` / `NoPresentation`，不做静默降级。Wayland 是否支持另开条目，不阻塞 X11 上线。
2. **平台接缝**：窗口系统与 Vulkan 分层——每个设备会话一个 `WindowSystem`（连接、surface、设备自建窗口），
   平台实现按文件分开（今天是 X11：xcb 连接 + Xlib 自建窗口，均 `dlopen`，插件无构建期 X 依赖）。
   加 Win32 = 新增一个实现文件 + 选择点，**不改 `Device` / `WindowTarget`**。这是本 ADR 与本次提交的抽象承诺。
3. **未实现的能力**：允许答 `Unsupported` / `NoPresentation` / `NotReady`，但必须
   - 一次、具名诊断（`DiagnosticCategory::UnsupportedRequest`），
   - 在用户文档里与 `vsg` 的差异并列，
   - **不得**退回“悄悄换个计数 / 换个路径继续画”。
4. **切换门**（全绿才把注册表的默认从 `"vsg"` 换成 `"vk"`）：
   - `releaseRenderTarget` 真正释放附件，`releasePass` 明确 no-op 并写明理由；
   - `readDepthBuffer` 实现，或写进差异表并从“与 vsg 同能力”的宣称里去掉；
   - 设备丢失：提交/呈现的 `VkResult` 分类，`VK_ERROR_DEVICE_LOST` 报一次并让宿主走
     `RenderEngine::shutdown()` + `initialize()` 的重建路径；
   - 质量门：`test_vk` 进 ASan/TSan（T34 的守卫是前置）、validation 在测试里常开且静默、全量 ctest 绿；
   - 平台证据：X11 实测记录（本机）；Win32 需要一台机器，未过之前不支持 Windows 上线；
   - 性能证据：一个真实宿主的帧时间与加载时间基线（含 `prepareFrame` 的预热批）；
   - 文档：`docs/guides/` 下的 vk 用户页与差异表（待写）；`.ai/design/` 的 vk 规范页从 research 转正。
5. **切换后的 `vsg` 去留**：保留至少一个发布周期作为回退路径；删除与否另开 ADR，不在本 ADR 内决定。

## 后果

- 好的：目标从“学习”变成可验收的清单；平台抽象今天落地（`WindowSystem`），Win32 是加法不是改法；
  未实现能力的答案有统一形状（`Unsupported` + 一次诊断），不会出现两后端“同一请求两种结果”（`G6`）。
- 坏的 / 代价：达标前要维护两个后端；切换门的平台与性能项依赖真实的机器与宿主场景，**代码侧不能单独宣告上线**。
- 与既有约束的关系：本 ADR 不改变 C7/C8（SDK ABI 形状与打包面）与 D1（`backend/` 半不因 vk 增长）；
  `support/Handles.hpp` 的逃生门仍是唯一一条 Vulkan 暴露口（V2/V4/E1 门继续跑）。
