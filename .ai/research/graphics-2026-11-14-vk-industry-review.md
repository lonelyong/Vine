# `gfx_backend_vk` 工业标准审查（2026-11-14，只读 + 两处小修）

> **状态**：审查结论（非权威，落地后并入 `design/` / `backlog/` 并删除本文件）
> **范围**：`src/plugins/gfx_backend_vk/**`（8.3k 行：internal 7.1k / support 0.5k / shell 0.1k）
> **方法**：读代码 + 计数（分配点/同步点/扩展）+ 实测（Release + lavapipe，`Vine --open demo`，
> 基线见 `../memory/graphics.md` 2026-11-14）+ `clang-tidy-22`（`bugprone-*` / `clang-analyzer-*`，结论已入 memory）。
> **相关**：`../decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`、`../backlog/graphics.md` T35/T41/T42。

## 1. 一句话结论

**规范符合性与可诊断性是工业级；资源与性能架构是原型级。** 这不是“能不能画”的问题——它画得对（与 vsg
逐字节相同、validation 0 VUID、72 例设备用例）；问题在**每帧的 CPU/驱动侧成本模型**：它按“每个 draw 一套新对象”
组织，而工业实现按“每帧一次、按槽复用”组织。ADR 的切换门只覆盖了功能与证据，**没有覆盖这一层**，所以“是否牛逼”
的诚实答案是：**正确性 8/10，架构 5/10，证据 9/10**。

## 2. 评分表

| 维度 | 判词 | 依据 |
|---|---|---|
| 规范正确性 | **强** | validation 常开、`test_vk` 72 例 0 VUID；两个后端逐字节对拍 12 例；同步验证可按 `VK_LAYER_ENABLES` 跑 |
| 错误处理 / 可诊断 | **强** | 8 类诊断词汇 + 设备丢失分类 + 每条件一个 `ReportOnce` 情节 + 拒绝时说明“是哪一项没对上” |
| 资源生命周期 | **中** | 显式释放（`releaseContent` / `releaseRenderTarget`）+ 驻留账；但**无子分配**，每资源一次 `vkAllocateMemory` |
| 性能架构 | **弱–中** | 每 draw 每帧建/毁 UBO（F1）、每 draw 每帧分配描述符集（F2）、每帧分配命令缓冲（F3）、无管线缓存（F4）、单帧在飞（F5） |
| 平台 | **中** | X11 实测；Win32 分支从未构建；设备选择刚改为按类型排序（F6） |
| 测试 / 证据 | **强** | 设备用例 + 跨后端对拍 + ASan/TSan 门禁 + 暴露边界机器检查（V1/V2/V4/E1） |
| 代码组织 | **强** | `internal/support/shell` 分层；`core` 不 include vsg/vulkan 的门；barrier 表与 pipeline key 都是单一定义 |

## 3. 做对了什么（超出多数示例工程的项）

1. **错误即值**：`DiagnosticCategory` 的九类语义分明（`ContentSkipped` / `CompileFailed` / `SubmissionFailed` /
   `PassProtocolViolation` / `UnsupportedRequest` …），拒绝时指名“哪个 pass 的哪一项没对上”，而不是一句“失败”。
2. **设备丢失是一等公民**：六个队列点统一 `noteDeviceLost`，报一次、`initialized()` 转 false、后续帧 `Skipped`，
   `initialize()` 发现丢失会话先 `shutdown()` 再重建。
3. **barrier 表驱动**（`BarrierPlan.cpp`）：一个 `ImageLayout` 一行，同时给出 layout + stage + access 三拼写；
   这是这个仓库反复踩坑（“布局对、stage 漏”）之后收出来的形状。
4. **pipeline key 覆盖完整**：program 身份 + variant bits + 设备侧兼容性（格式/采样数/subpass）+ topology +
   全部状态 + 顶点通道。两套管线不会因为“少比了一项”而串用。
5. **目标 plan 与租赁**：`Rebuild / ResizeInPlace / Repair` + 借深度（`shareDepth`，出借方先、借方后）+
   多采样 resolve（`sample0`，1.2 门控）+ 每附件 clear。
6. **预热契约**（ADR-2026-11-14）：`prepareFrame` 可提交**一个只含传输**的批，第二次预热全零——这是“加载期
   把纹理落盘”的正确形状，比“首帧偷偷慢一下”诚实。
7. **暴露边界是机器检查的**：SDK 后端面零 `Vk*`、插件 `shell/` 零 `Vk*`、帧路径不许走逃生门（`check_vk_exposure.py`）。
8. **两个后端对拍**：同一个 `FrameDescription` 在 vsg 与 vk 上逐字节相同（12 例）——这是比任何单后端用例都强的
   正确性证据。

## 4. 与行业标准的差距（按严重度）

### F1（高，**已落地 2026-11-14**）每 draw 每帧创建/销毁 uniform buffer

- **现状**：`ContentDraw::createHostBuffer`（`ContentDraw.cpp:522`）对**每条 draw** 做
  `vkCreateBuffer` + `vkGetBufferMemoryRequirements` + `vkAllocateMemory` + `vkBindBufferMemory` + `vkMapMemory`
  + `vkUnmapMemory`（`ContentDraw.cpp:2292`、`:2463`），帧尾 `vkDestroyBuffer` + `vkFreeMemory`
  （`ContentDraw.cpp:1653`）。演示帧有 **22 条内容 draw + 6 条全屏 draw** ⇒ **每帧 28 次分配 + 28 次释放**，
  60 fps 下约 **1700 次/秒** 的驱动内存操作。
- **行业做法**：一个（或每帧一个）**持久映射**的 ring buffer，按 `minUniformBufferOffsetAlignment` 切片，
  描述符用 `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` + `vkCmdBindDescriptorSets` 的 dynamic offsets。
  逐 draw 只写字节 + 记偏移，**零分配**。本仓库的 vsg 后端就是这么做的（`vine_draw` 走 dynamic offset，
  `VsgDrawBlockPool`）。
- **判词**：这是与工业实现差得最远的一处，也是“同一框架两条实现”里 vsg 已经做到、vk 没做到的一条。
- **代价**：驱动侧锁与堆操作在每帧关键路径上；在真 GPU 上会把 CPU 帧预算吃掉可观一块（lavapipe 上已占稳态
  7 ms 的一部分）。
- **已落地（2026-11-14）**：`ContentDraw::Impl::UniformRing`——一个持久映射缓冲，按 `minUniformBufferOffsetAlignment`
  切片；四个块（material/lights/draw/shadow）写成 `UNIFORM_BUFFER_DYNAMIC`，绑定时给 4 个动态偏移；帧满则**跳过该帧
  的 draw 并在下一帧前扩容**（扩容会销毁命令缓冲已点名的 buffer，所以只在帧间做）。见证：`Device::contentCacheCounters()`
  的 `ring_builds` 稳态恒 1；用例 `VkUniformRingTest.*`。
- **实测（Release + lavapipe，同一 11-pass 场景，各 3 次）**：稳态 median **9.03–9.23 → 8.16–8.30 ms（−10%）**；
  预热不变（预热不画）。

### F2（高，**已落地 2026-11-14**）描述符集每 draw 每帧分配

- **现状**：`makeContentSet` / `makeDrawSet` / `makeScreenSet` 每条 draw 现分配（池每帧 `vkResetDescriptorPool`，
  `ContentDraw.cpp:1871`）；预热报告的 `sets` 恒 0（T42 已登记）。
- **行业做法**：按（program 布局, 资源集合）**缓存声明集**，只在资源变化时重建；或 push descriptor（1.4 可选）。
  vsg 的 `ContentSets` 就是前者（按 program revision + 纹理 revision 键）。
- **判词**：与 F1 同源（“每 draw 一套对象”），两者一起改才划算（一次 bind 两个 set + offsets）。
- **已落地（2026-11-14）**：set 的键改成**它绑定的资源**（5 个纹理视图 + shadow 视图；screen set 同形），
  三个 set 布局全部 `..._DYNAMIC`，池带 `FREE_DESCRIPTOR_SET` 并**按条目释放**（不再每帧 reset）；每 draw 的
  差异只剩动态偏移，所以 draw set 整个 generation 只有一个句柄。见证：`set_builds` 稳态不涨、`sets_live` 不随
  帧增长；变异（缓存恒 miss）⇒ 两个用例同时红。

### F3（中）命令缓冲每帧分配/释放

- **现状**：`Device.cpp:1534`（帧）与 `:2016`（预热批）每帧 `vkAllocateCommandBuffers` + `vkFreeCommandBuffers`。
- **行业做法**：每个帧槽一个持久命令缓冲（`vkResetCommandBuffer` 复用），或每帧从池里 reset。
- **判词**：单帧在飞时收益有限（每帧 2 次驱动调用），但和 F1/F2 一样属于“每帧建对象”的模式。

### F4（中，**会话内已落地 2026-11-14**）没有 `VkPipelineCache`，着色器运行时编译

- **现状**：`vkCreatePipelineCache` 全仓 0 处；GLSL 在运行时经 glslang 编译（`ShaderCompiler.cpp`）。
- **行业做法**：至少建一个内存内 `VkPipelineCache` 传给 `vkCreateGraphicsPipelines`；发布版把它**序列化到磁盘**
  （按驱动/设备版本命名），冷启动从“编译几分钟”降到“读一个 blob”。
- **判词**：本后端已有 program/pipeline 的内存缓存（同一会话不重编），所以**会话内**收益接近 0；
  真正缺的是**跨会话**。对“每次启动都重新编译”的开发者体验是明显差距。
- **已落地（会话内）**：`ContentDraw` 建一个 `VkPipelineCache` 并传给两处 `vkCreateGraphicsPipelines`，
  `destroy()` 销毁它。**仍缺**跨会话：`vkGetPipelineCacheData` 落盘 + `pInitialData` 装回（backlog **T44**）。

### F5（中）单帧在飞：每帧 `submit` + `fence wait`

- **现状**：`executeFrame` 提交后立即 `vkWaitForFences`（`Device.cpp:1782`、`:2211`、`:2343`），CPU 与 GPU
  **完全串行**；一个 `completion` fence 服务所有提交。
- **行业做法**：2–3 个帧槽，每槽自己的 fence/命令缓冲/ring 切片；只在“需要读回”时等那一帧。
- **判词**：**这是 SDK 契约的后果，不是疏忽**——`readColorBuffer` 承诺“画完就能读”，vsg 也因同样的契约在
  无窗口设备上等离屏会话。改成多帧在飞要动 `RenderFrameExecutor` 的语义（谁保证“这一帧已完成”），属于框架级决定，
  不能在插件里偷偷做。**登记为已知代价**，不要当成待修 bug。
- **建议**：写入 `design/` 的 vk 规范页“已知代价”一节；真要改走新 ADR。

### F6（中，已修一半）物理设备选择

- **现状（修前）**：取第一个“有 graphics 队列（+ present）”的设备；笔记本上常是先列 iGPU ⇒ 默默用慢的那个，
  且**没有任何日志**说用了谁。
- **已修**：按 `VkPhysicalDeviceType` 排序（discrete 4 / integrated 3 / virtual 2 / cpu 1 / other 0），
  同分保留先出现者（确定性）。`Device.cpp` 的 `deviceTypeRank`。
- **仍缺**：**选中的设备没有一行证据**（名称/类型/API 版本）。行业实现都会打印。它属于宿主/日志层
  （本插件目前不 include logging），建议随 `docs/guides/` 的 vk 页一起定“设备行”的归属。

### F7（中）无子分配（每个 image/buffer 一次 `vkAllocateMemory`）

- **现状**：`TargetImages.cpp:48`（图像）、`ContentImages.cpp`（纹理与暂存）、`ReadbackStaging.cpp`（读回）
  各自 `vkAllocateMemory`。
- **行业做法**：VMA 或自建 allocator（按 memory type 分块、子分配、对齐、释放表）。
- **判词**：`maxMemoryAllocationCount` 的规范下限是 **4096**（本机 lavapipe 更大）；目标数 × 附件数 + 纹理数
  在“大场景 + 多目标”下会逼近这个上限，而失败形态是 `vkAllocateMemory` 返回 `VK_ERROR_OUT_OF_DEVICE_MEMORY`
  ——**看起来像显存不足，其实是分配次数用尽**。
- **建议**：backlog **T45**（触发：目标/纹理数量到千级，或第一次出现“显存够却分配失败”）。

### F8（低–中，已修）`vkAcquireNextImageKHR` 的无限超时

- **现状（修前）**：`UINT64_MAX`。窗口卡住 / 驱动异常时 **永久挂起，没有任何诊断**。
- **已修**：1 s 有限超时，超时按“这次没拿到图”处理（跳过该帧、按情节报一次）。

### F9（低）legacy barrier，未用 synchronization2

- **现状**：`vkCmdPipelineBarrier` + 1.0 stage mask（`BarrierPlan.cpp`），API 1.2。
- **行业做法**：1.3 的 `vkCmdPipelineBarrier2` + `VkPipelineStageFlags2`（语义更窄、更不容易写错）。
- **判词**：当前表是**单一定义**且被同步验证覆盖过，正确性有保证；这是现代化项，不是缺陷。
  真要做，先把 API 版本提到 1.3（本机 lavapipe 支持），否则要加扩展。

### F10（低）render pass 变体手工组合

- **现状**：`TargetImages` / `WindowTarget` 各维护 fresh × load 变体（颜色/深度两维），窗口另有 CLEAR/LOAD。
- **行业做法**：1.3 的 dynamic rendering 把“每 pass 的 load/store”变成命令参数，变体组合消失。
- **判词**：1.2 下这是正确形状；升级到 1.3 时优先删掉这一整层。

### F11（低）队列族选择

- **现状**：只找“第一个支持 graphics（+present）”的族；传输走 graphics 队列。
- **行业做法**：优先 graphics+compute+transfer 三合一的族，其次分开；大上传用专用 transfer 队列 + 队列族所有权转移。
- **判词**：同步提交模型下收益很小；**明确不做**（G8），除非上传成为瓶颈。

### F12（低，**已落地 2026-11-14**）没有 GPU 侧计时

- **现状**：`vkCmdWriteTimestamp` / query pool 全仓 0 处；HUD 的帧时间来自 CPU 墙钟（`FpsOverlay`）。
- **行业做法**：每帧两个时间戳（帧首/帧尾）+ 环形 query pool，读回得到 GPU 时间；与 CPU 时间分开显示。
- **判词**：性能证据目前只能回答“宿主调用花了多久”，不能回答“GPU 花了多久”。做 F1/F2 的收益评估时
  会**需要**它——建议先于 F1 落地，否则改动只能靠 lavapipe 的墙钟判优劣。
- **已落地**：SDK 的 `FrameTiming`（`FrameStats.hpp`，`FrameReport::timing` 可选，缺席 ≠ 0）+ vk 的
  `VK_QUERY_TYPE_TIMESTAMP` 一对查询（`executeFrame` 里写、栅栏之后读，`timestampComputeAndGraphics` 为假时整个
  部件缺席）；demo 的 `bench last frame` 在有样本时印 **gpu 毫秒**。用例
  `VkDeviceClockTest.AFrameReportsItsDeviceTime`（首帧有值、未采样帧缺席、第 31 帧再有值、关机后缺席）。
- **量出来的代价**：lavapipe 上每帧写读时间戳 **~0.8 ms**（关 median 8.30 / 开 9.01–9.20，各 3 次）⇒ 后端**每 30 帧
  采一次**，未采样的帧答缺席。gpu 值本身：**7.15–7.74 ms**（CPU median 8.2–9.0，同一场景）。

### F13（信息）运行时 glslang

- **现状**：GLSL → SPIR-V 在进程内完成（与 vsg 共用 `cmake/VineGlslang.cmake`）。
- **判词**：对“引擎文本可变、宿主可塞程序”的设计是合理的；发布形态若要更快，应缓存 SPIR-V（文本哈希 → blob）。
  与 F4 的磁盘管线缓存是同一类工作。

## 5. 与 vsg 的对照（同一框架，两条实现）

| 能力 | vsg | vk | 说明 |
|---|---|---|---|
| 每 draw uniform | 动态偏移 + 池化大缓冲 | **每 draw 一个 buffer** | F1 |
| 描述符集 | 声明集缓存（program rev + 纹理 rev） | **每 draw 每帧分配** | F2 / T42 |
| 管线缓存 | 无（T10，vsg 上游限制） | 无 | F4，两边同缺 |
| 帧在飞 | vsg 自己的任务/栅栏（可多帧） | **单帧 + 每帧等栅栏** | F5，契约后果 |
| GPU 计时 | 无 | 无 | F12，两边同缺 |
| 设备选择 | 由 vsg 负责 | 按类型排序（已修） | F6 |
| 错误词汇/诊断 | 同一套 SDK 词汇 | 同一套 | 一致 |

**结论**：vk 的“正确性骨架”与 vsg 同代（同一套 SDK 契约、同一套诊断、同一套门禁），差距集中在**每帧对象生命周期**
这一层——而这一层恰好是 vsg 在重写期已经解决过的（`VsgDrawBlockPool` / `ContentSets`）。

## 6. 建议的下一步（按性价比排序）

1. **T46 GPU 计时**（小）：没有它，F1/F2 的收益无法在真 GPU 上判定。
2. **T43 + T42 同批**（中）：持久 ring + dynamic offsets + 声明集缓存。一次改两件同源的事，并让预热报告的
   `sets`/`uploads` 第一次有意义。
3. **F3 命令缓冲复用**（小）：随 T43 一起做，形状一致。
4. **T44 管线缓存**（小，磁盘部分中）：先内存内，发布形态再加磁盘 blob。
5. **文档**：`docs/guides/` 的 vk 页 + 差异表；`.ai/design/` 的 vk 规范页（把 §4 的 F1–F13 变成“已知代价/缺口”表）。
6. **不做**：F5（多帧在飞，动契约，需 ADR）、F11（专用传输队列）、F9/F10（等 API 1.3 一起做）。

## 7. 参考

- Khronos **Vulkan Best Practices** 与 validation 的 `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT`（建议入门禁：
  它对“每帧分配”“未使用 dynamic offsets”“无管线缓存”这类模式会给出警告）。
- Vulkan 规范：`VkPhysicalDeviceLimits::maxMemoryAllocationCount`（下限 4096）、`vkAcquireNextImageKHR` 的超时语义、
  `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` 的偏移对齐要求。
- 社区一手实践：Vulkan Memory Allocator（子分配模型）、vkguide / Sascha Willems 示例（每帧 ring + 帧在飞 + 描述符缓存）。
- 本仓库：`../design/vsg-backend.md`（同一套问题的 vsg 解法）、`../memory/graphics.md`（2026-11-14 基线）、
  `../backlog/graphics.md` T35/T41/T42。
