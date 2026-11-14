# ADR-2026-11-14：预热可以提交一批“只有传输”的工作

- 状态：已采纳
- 日期：2026-11-14
- 相关：`../design/graphics-frame-pipeline.md`（§3.7 F7、§5.1 `C3`、§6.11 `P11`、§7 `D11`）、
  `../decisions/ADR-2026-11-12-adopt-graphics-next.md`、`RenderFrameExecutor::prepareFrame`、`PrepareReport`、
  `tests/test_vk/ResidencyTest.cpp`、`tests/test_vk/PrepareUploadTest.cpp`

## 背景

`prepare()` 存在的理由只有一个可检验的说法（`R23`）：预热之后，同一份声明的帧 `builds` 增量为 0。
但今天的契约把它能做的那一半切掉了——**需要命令缓冲才能建立的那一半**：

- 头文件写的是 “It must not submit, present, or resize the swapchain”；设计侧是 `C3` / `P11`：预热不跑帧、
  不 acquire 交换链图像。
- vk 把这条规则的后果直接写在代码里（`ContentDraw::prepare`）：“NO FALLBACK IMAGE, NO DESCRIPTOR SET,
  NO COMMAND: a warm-up is programs, streams and pipelines only - the 1x1 stand-in and the texture copies need a
  command buffer, which prepare() does not open.”
- 于是纹理上传落在**第一个采样它的帧**里：`Device::prepareFrame` 的 `report.uploads` 只取 `stream_builds`，
  而那一帧的 `FrameUploads.texture_uploads` 是 1；拷贝是 `executeFrame` 在 `ContentDraw::preparePass` 里记录
  进帧自己的命令缓冲的（`Device.cpp` 的 pass 循环第一条语句）。
- vsg 是同一个形状：`MaterialImages::acquire` → `makeImage` 只把字节 stage 进一个 data-backed `vsg::Image`，
  真正的拷贝由帧的传输任务提交（`../design/graphics-content-lifecycle.md` 2.8 / `F16`–`F22`）。

后果不是“少一个数字”，而是 `R23` 对**最贵的那一类**不成立：程序、流、管线都能预热，字节不能；
宿主按 `PrepareReport` 做加载期预算时，纹理字节完全不在账上（`G4`：报告的契约是“这次调用建了什么”，
却默默排除了一整类，而 `FrameUploads` 明明把流与纹理并列）。这与 ADR-2026-11-12 `D11` 否决的那件事不同：
`D11` 否决的是**跑一帧但不提交**（acquire 图像、画进交换链图像、不 present ⇒ 饥饿），而这里的候选物从头到尾
没有帧——没有 acquire、没有 render pass、没有 present。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 保持现状 | 契约不动，零风险 | `R23` 只在程序/流上成立；纹理字节永远是首帧成本且不进任何报告（`G4` / `G7`：复杂度留给宿主猜） |
| B **预热可以提交一个自己的、只含传输的命令缓冲，返回前等它完成** | 上传在加载期落地，帧的上传增量真的为 0；一条规则覆盖全部内容；不新增入口（`G1` / `G8`） | 预热可能阻塞在自己的传输上；契约必须把“不提交帧”与“可以提交传输”写精确，否则两个词混成一个（`G4`） |
| C 新增第二个动词（`flushUploads()` / `uploadNow()`） | 显式、语义单一 | 两个入口回答同一个问题（“把这一帧需要的东西建好”），调用方还得自己知道什么时候调（`G1` / `G7`）；今天没有消费者要求把两件事分开（`G8`） |
| D 后台线程 / 异步队列上传 | 不阻塞宿主 | 违反 `C3`（只有一条引擎线程碰设备）；`prepare` 返回时上传还没落地，`R23` 的判据不可测（`G11`） |
| E 纹理填充完成时就上传 | 不需要新动词 | 引擎线程不知道哪些纹理会进这一帧 ⇒ 会传没人读的内容（`P8`），而且在宿主填纹理的线程上碰设备（`C3`） |

## 决定

选 **B**，并把边界写成可判定的六条：

1. **预热仍然不提交帧**：不 acquire 交换链图像、不开 render pass、不 present、不改 `FrameProgress` 的
   `submitted` / `presented`。`P11` 与 `D11` 的全部论证保持有效。
2. **预热可以提交恰好一个自己的命令缓冲，内容只允许是传输**：buffer→image / buffer→buffer 拷贝与配套的
   布局转换；不得含任何 draw、dispatch 或 render pass 实例。
3. **这一批必须是“帧本来会在它的第一个 render pass 之前记录的那些拷贝”，不是第二条路径**：vk 里就是
   `ContentDraw::preparePass`（1×1 白色 stand-in + 每张材质纹理）在预热自己的命令缓冲上跑一遍；
   随后录制那一帧时同一处查到条目已驻留，什么都不再记录。
4. **没有要拷的东西就不提交**：第二次预热同一个帧不产生任何提交（“零工作 ⇒ 零提交”是可观测的）。
5. **报告口径统一**：`PrepareReport::uploads` 数这次调用建起来的**所有**上传（流 + 纹理），与
   `FrameUploads` 的两类同名同义；`PrepareReport::Outcome::Prepared` 的措辞从 “without submitting it”
   改为 “without submitting the frame”。
6. **传输失败不是新的终态**：照 `prepare` 现有的“这个 pass 服务不了”处理——报诊断、计数不增，
   下一帧自己重试；不改 `Outcome`。

## 后果

- 好的：
  - `R23` 对整份内容成立，不再“除了纹理”；加载期预算能看见字节。
  - 一条规则：“预热建的是这一帧本来就要建的东西”，不再按“要不要命令缓冲”分裂成两类。
  - 区分仍可观测且不可混淆：`FrameProgress::submitted` / `presented` 是帧的水位，传输批不动它们——
    “预热提交了传输”与“预热跑了一帧”在报告上是两个不同的答案。
- 坏的 / 代价：
  - `prepare()` 现在可能阻塞在 GPU 上，时长是它自己那批字节的拷贝时间（不是等某一帧）；宿主需要把它算进
    加载期预算。这与设备已有的“帧同步提交”是同一类同步，不引入新的一致性故事。
  - 契约用词必须一直精确：以后任何“允许预热做更多事”的提议都要先回答“它仍然是传输、不是帧吗”，
    否则 `P11` 会被悄悄绕过。
  - 两个后端的实现进度可以不同：`vk` 本次落地；`vsg` 的拷贝在它自己的传输任务里（不在帧的命令缓冲里），
    要单独改，见 `../backlog/graphics.md` `T39`。
- 触发条件（什么时候重开）：
  - 出现“加载期上传本身就是可感卡顿”的真实宿主（那时需要异步入口，属 `T17` 的范畴，必须新 ADR）；
  - 或第二个消费者要求预热严格非阻塞（同样要新 ADR，因为 `C3` 与新线程故事一起变）。

## 证据

- 现状（改前，可读代码验证）：`Device::prepareFrame` 不创建命令缓冲、`report.uploads` 只取 `stream_builds`；
  `ContentDraw::prepare` 的注释明说“纹理拷贝需要命令缓冲，`prepare()` 不开”；`ContentDraw::preparePass`
  是唯一记录这些拷贝的地方，`executeFrame` 在每 pass 之首用帧自己的命令缓冲调用它。
- 改后（`test_vk`）：`VkPrepareUploadTest.APreparedTexturedFrameUploadsNothingWhenItRuns` —— 预热报告
  流上传 2 + 纹理上传 1，随后的 `executeFrame` 报 `stream_uploads == 0`、`texture_uploads == 0`，
  且像素是该纹理的颜色（证明上传在预热里就落了地）。
  `VkPrepareUploadTest.ASecondPrepareSubmitsNothing` —— 第二次预热 `uploads == 0` 且 `FrameProgress`
  的 `submitted` / `presented` 仍是 0。
- 守卫（改前后都成立）：`VkResidencyTest.PrepareBuildsWhatTheFrameNeedsAndSubmitsNothing` 断言预热后
  `progress->submitted == 0` 且 `frameReport().uploads` 仍未接通——这条现在同时钉住“传输批不算帧”。
- 设计侧：`../design/graphics-frame-pipeline.md` §3.7 / §5.1 `C3` / §6.11 `P11` / §7 `D13`。
