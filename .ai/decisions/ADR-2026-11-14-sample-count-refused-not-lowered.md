# ADR-2026-11-14：采样数是**计数**，设备建不了的请求只拒绝一次，不降级

- 状态：已采纳
- 日期：2026-11-14
- 相关：`../design/graphics-rendering.md` §3.2（F6–F10）、`Capabilities::max_samples`、`RenderTarget::setSamples`、
  `../backlog/graphics.md`、`src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`、
  `src/plugins/gfx_backend_vsg/src/internal/HostTargets.cpp`、`src/plugins/gfx_backend_vk/src/internal/Device.cpp`、
  `tests/test_vsg/MsaaTargetTest.cpp`、`tests/test_vk/MsaaTest.cpp`

## 背景

同一个公开契约今天有三份互相矛盾的答案：

- **文档**：`Capabilities::max_samples` 是“一个 target 可以要求的最大**计数**”；`RenderTarget::setSamples`
  说超过它的请求被拒绝（报一次）、**从不**被悄悄降低；`graphics-rendering.md` F10 也写“超限报
  `SamplesUnsupported`”。
- **`vsg`**：行为符合（`HostTargets::State::SamplesUnsupported`，不建对象），但 `capabilities().max_samples`
  与 `setMaxSamples(...)` 收到的是 `framebufferColorSampleCounts & framebufferDepthSampleCounts` 的**位掩码**：
  一台支持 1/2/4/8 的设备答 **15**。宿主读到 15 再 `setSamples(15)` 会通过“`> max_samples`”这条比较，随后
  这个数一边被 `samplesToVk` 向下取整成 2、一边留在 `shape().samples` 里——**同一个 target 的形状与对象
  不是同一个数**。“设备根本不提供的计数”（如 3）更是没人检查。
- **`vk`**（M11，2026-11-14）：`max_samples` 是计数（本机 8，正确），但超限请求被**降级**到最接近的受支持
  计数并报一次；可被采样的深度 + 多采样则直接把 target 降成单采样。两处都与上面那份文档相反。

两份后端、三份文档、两种结果：这是本 ADR 要收口的那件事（`G1` / `G4` / `G6`）。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 保持现状（`vsg` 位掩码 + `vk` 降级） | 不动代码 | 宿主拿到的“上限”不是计数（`G4`：名字与语义不符）；同一份 `setSamples` 在两个后端得到两种结果（`G6`）；`shape().samples` 可以是设备根本没有的数 |
| B **照文档执行：上限是计数，设备建不了的请求拒绝一次** | 值、文档、两个后端一个答案；宿主可以信任 `samples()` 就是画出来的数（`G4`）；拒绝发生在建对象之前，`SamplesUnsupported` 已是现成的形状 | 一个“质量旋钮”级参数会丢画面：宿主写了 16 而设备只到 8，`vk` 从此不画（`vsg` 一直如此）；宿主必须读 `max_samples`（文档本来就要求） |
| C 契约改成“降级并报一次”，`vsg` 跟着改 | 画面总能出来 | 要改参考后端的目标路径（519 例）；更糟：resolved 画面与宿主声明的形状不同，而 `RenderTarget::samples()` 仍答声明值——一个**没有数字会说**的谎（`G4` / `G10`） |
| D 给 `Capabilities` 加“受支持计数的集合” | 宿主能精确选择 | Vulkan 的位集词汇进了一个 API 中立的值（`G13`）；今天没有消费者（`G8`）；“上限 + 计数是 2 的幂”已完整描述 |
| E 向上取整到最近的受支持计数 | 画面至少和请求一样好 | 比请求**更多**的采样是另一份成本，宿主没答应（`G10`）；与“别悄悄改我声明的形状”同罪 |

## 决定

选 **B**，并把“设备建不了的请求”一次说清：

1. **`Capabilities::max_samples` 是一个计数**（`1` = 不能多采样），不是位掩码；**合法计数是 2 的幂**
   （1/2/4/8/16/32/64），所以“上限 + 2 的幂”完整描述了这台设备能建的集合。
2. **请求 = 声明的形状**：`RenderTarget::setSamples(n)` 说 n 就是 n。设备建不了 n（超上限，或 n 不是 2 的幂）
   ⇒ 这个 target **不被建**，用具名诊断报**一次**，`readColorBuffer` 答 `NotReady`，**绝不**以另一个计数
   画出画面。契约里“never silently lowered”保持原样：`vk` 向它靠拢，不是反过来。
3. **深度与多采样的组合按“能否 resolve”说**：多采样 target 的**颜色**总是 resolve；它的**深度**在需要交给
   shader（`setDepthPromoted`）或交给另一个 pass（`shareDepth`）时必须是单采样画面——能 resolve 深度的后端
   resolve 出来（`vsg` 的 `depthResolveMode = sample0`），不能的（`vk` 今天）**拒绝**这个 target 并报一次。
   **借来的深度 + 多采样**任何后端都拒绝（一个 framebuffer 的附件共享一个计数）。
4. **一次、一个类别**：两个后端都用 `DiagnosticCategory::UnsupportedRequest`，消息里带请求值与上限
   （“asked for 16 samples per pixel, above this device's limit of 8”），episode 由后端自己保证不刷屏。
5. **不新增 `Capabilities` 字段**（否决 D），也不改 `RenderTarget::setSamples` 的签名。

## 后果

- 好的：`max_samples` 终于是一个能直接交给 `setSamples` 的数；两个后端对同一个请求给同一个答案（拒绝）；
  `shape().samples` 与对象的计数不再可能不同；`vk` 的两处“悄悄降级”消失；设计文档 F8 / F10 与代码说同一件事。
- 坏的 / 代价：
  - 宿主写了超过设备能力的数就没有画面（一次警告 + `NotReady`）。这是**有意**的：`Capabilities` 就是为此
    存在的入口，而“我声明 16、你画 4”在这套形状里没有任何数字能表达。真出现“想自动降级”的宿主，那是
    一个新的协商入口，要新 ADR。
  - `vk` 的“多采样 + 可被采样的深度”从“能画（降成单采样）”变成“不画”：`vk` 不会 resolve 深度。真正的修法
    是给 `vk` 补深度 resolve（backlog `T40`），那时这条组合自然放行，两个后端再一次一致。
  - `vsg` 的 `capabilities().max_samples` 从 15 变成 8：读位掩码的宿主（今天没有）会看到变化，读计数的宿主
    从“可能崩”变成对。
- 触发条件（什么时候重开）：
  - 出现一个必须“尽量多采样”的真实宿主（那时是协商入口，不是降级默认）；
  - 或第三个 API 家族后端出现，使 `max_samples` 这个形状不再成立（跨 API 那一轮，见 `T30`）。

## 证据

- 改前（可读代码验证）：`VsgBackend::capabilities()` / `setMaxSamples(...)` 直接传
  `std::min(framebufferColorSampleCounts, framebufferDepthSampleCounts)`；`HostTargets::build` 只比较
  `description.samples > max_samples`；`Device::ensureTarget` 调 `clampSampleCount` 并有两处降级分支。
- 改后（测试）：
  - `tests/test_vsg/MsaaTargetTest.cpp`：超上限（`setMaxSamples(4)` + `setSamples(8)`）与“不是计数”
    （`setSamples(3)`）都答 `HostTargets::State::SamplesUnsupported`；
  - `tests/test_vsg/BackendParityTest.cpp`：`TheSampleLimitsAgreeBetweenTheBackends` —— 两个后端在同一台机器上
    答同一个计数，且它是 2 的幂；
  - `tests/test_vk/MsaaTest.cpp`：超上限被**拒绝**（`draws` 0、`readColorBuffer` 失败、报一次且只一次）、
    3 个采样被拒绝、可被采样深度 + 多采样被拒绝（`T40` 的现状）。
- 设计侧：`../design/graphics-rendering.md` §3.2 F8 / F10；`Capabilities.hpp` / `RenderTarget.hpp` 的契约文字。
