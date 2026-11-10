# ADR-2026-11-10：上传暂存的回收是显式入口，由宿主（或用户命令）触发

> **状态**：已取代（同日的 [ADR-2026-11-10-upload-staging-is-not-retained.md](ADR-2026-11-10-upload-staging-is-not-retained.md)
> 把它改成了“传完即还、帧边界自动”：显式入口在“载入后不再出帧”的宿主上没人调，而 park 到期又得靠“再问一次”才落地）
> **日期**：2026-11-10
> **影响范围**：`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`、`RenderEngine.hpp`、
> `src/viz/graphics/src/RenderEngine.cpp`、`src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`（与同名头文件）、
> `tests/test_graphics/HeadlessBackendTest.cpp`、`tests/test_vsg/UploadScratchTest.cpp`、
> `.ai/design/graphics-content-lifecycle.md`、`.ai/design/vsg-backend.md`、`docs/guides/graphics.md`

## 背景

vsg 上传静态数组时，先把整次上传的字节拷进一块 `HOST_VISIBLE|HOST_COHERENT` 的暂存（`TransferTask` 的
`TransferBlock::staging`），然后才 `vkCmdCopyBuffer` 到 device-local。这块暂存只增不减、传完不还，也不经过任何 pool，
所以 `releaseUnusedPools()` 碰不到它：14M 三角形的模型上传一次，进程里就多出 1.2 GB 工作集（GPU 计数器的
`SharedUsage` 上也读得到同一个数字）。

机制已经落地且被测量过：换掉那个 `TransferTask` 就能把暂存还给系统（`detail::replaceTransferTask`，见
`vsg-backend.md` F16.2），代价是下一次上传重新分配 16 MB。这次要定的是**谁在什么时候调它**，而当时它写在
`VsgBackend` 的帧驱动里，且由环境变量 `VN_VSG_KEEP_UPLOAD_SCRATCH` 开关。三个问题：

1. **时机写在一个后端里**。`RenderEngine` 和 SDK 都不知道有这回事；第二个后端（D3D12/WebGPU 也有自己的 upload heap / staging）
   要重新发明一遍，忘了写就永久占着内存，而画面完全正常（`G2`：分层破了；`G5`：同一件事要在每个后端重写）。
2. **宿主没有入口**。环境变量在产码里无法决定，而且有全局语义。
3. **原来的触发条件本身是错的（实测）**。当时挂的是“波”：上传过一帧之后，**第一帧没有新上传**时回收。但宿主按需重绘
   ——模型加载完就静止了，那一帧**不存在**。实测：`Vine.exe` 加载模型后进程 1% 单核 / GPU 0%，1.2 GB 暂存常驻数分钟，
   `device_waits` 一次没涨：功能看着实现了，一次也没跑。

## 决定

1. **机制进 SDK，动词只有一个**：`RenderDevice::releaseUploadStaging()`（默认返回 false，表示“这个后端没有暂存可还”）。
   契约：帧之间调用、返回“这次有没有把暂存换下来”、**不阻塞**（见 5）。
2. **引擎不转发**：没有 `RenderEngine::releaseUploadStaging()`。宿主已经能拿到设备（`RenderEngine::device()`），多一个一跳入口只会把同一件事叫两个名字（`G4`）；
3. **时机归宿主/用户**：SDK **不**提供自动时机，也不提供开关。宿主在该调的地方调：一次载入结束、文档关闭、
   窗口最小化、一条用户命令、一次内存压力信号——这些只有宿主/产品知道。
4. **后端只回答“能不能”**：`VsgBackend` 删掉自己的计时与波状态、删掉环境变量读取，改成这个动词的实现
   （问**该会话自己的上传任务**手里有没有暂存 → 需要时换任务；不再比较设备级上传计数，见 7）。
5. **不阻塞：换下来是当场，销毁等证据。** 换 `TransferTask` 只是一次指针交换，旧任务被交回；它的暂存什么时候可以
   销毁由证据决定——时间线证明没有在飞帧（`submittedFrame() == completedFrame()`）就当场销毁，否则 **park 进退役队列**，
   由“完成覆盖那一槽”的帧释放（与 `releaseContent()` 同一条路）。只有队列无法 park（会话没学会在飞帧数）才退化为一次**计数**
   设备空闲。
6. **再请求一次就是“现在”。** park 的退役点在提交点之后 `slots + 1` 帧，完成水位又落后提交 `slots`，所以它要
   `2 × slots + 1` 帧才到期（vsg 3 槽 = 7 帧）——加载后不再出帧的宿主永远等不到。因此一次后来的调用会：一次计数
   `deviceWaitIdle()`（这就是证据）→ `RetirementQueue::drain()` 把还挂着的 park 全部跑掉 → 返回 true。持续出帧的宿主零代价。
7. **“还有没有东西可还”由每个会话自己的任务回答，不是设备级计数。** 会话起来时装的是 `detail::StagingTask`
   （派生自 `vsg::TransferTask`，读 protected 的 `TransferBlock::staging` 报字节数）。原来的做法比较
   `ContentAssembly::uploads().uploads()` 与 `MaterialImages::count()`：那是**设备**的计量，不是被问的那个会话的——
   多会话时，换成功的那个推进了计数，另一个“拷贝还在队列里、这次没换成”的会话再也不会被告知，它那块暂存要等下一次
   上传才回收（触发窄、会自愈，但漏的量级是一整个模型的暂存）。派生是唯一不 patch vsg 的读法，代价是依赖三个 protected
   名字：上游改名 = 编译错，改语义则由测试直接断言字节数兜住（`vsg-pitfalls.md` P42.2）。

## 理由

- `G1` / `G6`：暂存回收与 `releaseContent()` 用**同一条规则**——显式入口，谁的对象谁决定寿命。为它单独发明一套自动政策，
  等于给“释放”这件事开第二个先例。
- `G2` / `G5`：后端是唯一知道“这次调用时还有没有东西可还”的一方，没有第三处可以偏离；引擎不参与，资源问题就只有一个名字。
- `G8`：自动政策今天没有**表达过要它**的消费者——宿主自己写一行就能得到同样效果，而默认开会让每个上传帧白付一次设备空闲。
- `G4`：动词返回 bool，语义是“这次还了没有”（与 `releaseContent()` 同口径）；没有开关需要理解。
- `G10`：不阻塞调用方。UI 线程上的一次 `deviceWaitIdle` 会把界面卡住，而正确性只需要“销毁前有证据”——证据用时间线/park 就能免费取得，只有学不到在飞帧数时才付一次计数 idle。

## 否决的选项

| 选项 | 否决理由 |
|---|---|
| 政策留在后端的帧驱动里 | 引擎与 SDK 都看不见它；第二个后端要重写一遍（`G2`/`G5`）。环境变量开关也没法从产码里决定。 |
| “波”：等一帧没有新上传再回收 | 实测没有那一帧（宿主按需重绘，1% 单核 / 0% GPU）；在最该回收的时刻永远不跑。 |
| 引擎每帧自动调一次（默认开 + 可关） | 宿主在帧循环旁写一行就有同样效果；默认开让每个上传帧白付一次设备空闲，而宿主没有表达过要付（`G8`）。 |
| 给引擎加一个转发入口 | 宿主已经有 `engine->device()`；多一个只做一跳的入口，等于给同一件事起两个名字（`G4`）。 |
| 让引擎读每帧上传计数来决定 | 只把“要不要调”从一次计数比较变成一次计数比较，却多一个没人读的 SDK 字段（`G8`）。每帧上传数作为**观测**指标进 backlog T25。 |
| 后端在 `executeFrame()` 内部自己回收 | 后端的帧路径不该有资源政策；它也看不见宿主是否还需要那块内存。 |
| 另一个线程/定时器异步回收 | 销毁暂存要动 Vulkan 对象与命令缓冲，帧线程之外做要么加锁要么竞态；一次 idle 已经足够。 |
| 请求式：只登记“待清理”，真正清理留给下一帧 | “下一帧”可能永不发生（宿主按需重绘）：登记了不等于还了，而这正是本功能最早的失败方式。实际采用的是**交换当场 + park 等证据**：既不阻塞，也不把“还没还”藏在状态里。 |
| 环境变量作为入口或开关 | 产品里无法从代码或用户操作决定，且有全局语义。环境变量不再被引擎/后端读取。 |
| 用设备级上传计数判断“还有没有东西可还” | 计数答的是设备，不是被问的会话（见决定 7）：一个会话换成功就把另一个的答案盖成“没有”，漏掉的是它那一整个模型的暂存。 |
| 给每个会话存一份计数快照，代替问任务 | 同样不漏，但答的是“设备上的上传动过没有”而不是“我手里有没有块”：会白换一次（新建一个没人需要的任务），也拿不到字节数。问任务是一句真话，快照是近似。 |

## 后果

- **内存**：宿主调用一次就回落（14M 三角形实测：这份 1.2 GB 不再常驻）。**不调就继续占着**——这是显式入口的代价，
  也是这次决定的边界（`graphics-content-lifecycle.md` F18 / 6.3）。
- **性能**：请求本身不阻塞（一次指针交换 + 一次“任务手里有没有块”的读）；销毁要么当场（无在飞帧）、要么在下一帧的退役队列里完成。
  只有学不到在飞帧数的会话会付一次计数 idle，以及“再请求一次”时那一次用来排空 park 的计数 idle。
- **ABI**：`RenderDevice` 多一个**非纯虚**函数（追加在接口虚函数列表的**末尾**，既有槽位编号不变），`RenderEngine` 不变。
  插件与 SDK 必须同一次构建（本仓库一直如此）；安装树里旧的 `gfx_backend_vsg.dll` 要重编，否则虚表对不上。
- **行为**：不实现该动词的后端，`releaseUploadStaging()` 返回 false；宿主不需要区分“后端没有暂存”和“这次没东西可还”。
- **测试**：`UploadScratchTest`（帧不 idle；显式调一次 → true 且不涨（park）；再调 → true 且恰好 +1（`drain()`）；再调 → false 不涨；
  新上传后再调 → true；连续 10 帧 → park 免费排空）、
  `HeadlessBackendTest.TheUploadStagingIsReleasedThroughTheDeviceTheEngineBorrows`（引擎不转发，经 `engine->device()` 调），
  机制层是 `OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands`（含 `stagedBytes()` 的字节数断言）
  与 `AReplacedTransferTaskLeavesTheSessionSubmitting`（起来就装好可问的任务，没上传就问 → false 且不涨 idle）。
- **文档**：`graphics-content-lifecycle.md` 2.8 / F16–F22 / C4 / R4 / N7–N11 / 6.3 / D6–D15 / T5–T8；
  `vsg-backend.md` F16.2 改成“后端实现动词、时机在宿主、判据问任务”；`vsg-pitfalls.md` P42.2；`docs/guides/graphics.md` 删掉环境变量行。

## 后续

- 宿主侧的调用点（这些是产品决定，不在本 ADR 里定死）：模型载入完成、关闭文档、窗口最小化，以及一条用户命令
  （"释放内存"）。
- 每帧上传数作为观测指标（`FrameCounters::uploads`）：等 HUD 或门禁真的要读它时再做，见 `../backlog/graphics.md` T25。
