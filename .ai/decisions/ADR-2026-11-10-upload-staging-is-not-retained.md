# ADR-2026-11-10：上传暂存不保留 —— 拷贝的 fence 一完成就还，由帧边界自己判定

> **状态**：已取代（2026-11-13，见 [ADR-2026-11-13-staging-keeps-the-reusable-block.md](ADR-2026-11-13-staging-keeps-the-reusable-block.md)：
> 完成就放的证据规则不变，但**可复用下限的块保留**，只还大于下限的一次性大块）
> **日期**：2026-11-10
> **取代**：[ADR-2026-11-10-upload-staging-release-is-explicit.md](ADR-2026-11-10-upload-staging-release-is-explicit.md)
> **影响范围**：`src/viz/graphics/sdk/vine/graphics/RenderDevice.hpp`（删掉动词）、
> `src/plugins/gfx_backend_vsg/{include,src}/internal/{TransferStaging,VsgBackend,SessionContent,OffscreenSession}`、
> `src/plugins/model_viewer/src/{ModelRenderView.hpp,ModelViewerPlugin.cpp,commands/ReleaseMemoryCommand.*}`（删）、
> `tests/test_vsg/{UploadScratchTest.cpp,OffscreenTargetTest.cpp}`、`tests/test_graphics/HeadlessBackendTest.cpp`、
> `.ai/design/{graphics-content-lifecycle,vsg-backend,vsg-pitfalls}.md`

## 背景

vsg 把一次上传的字节先拷进一块 `HOST_VISIBLE|HOST_COHERENT` 的暂存（`TransferTask::transferData()` 里
`if (!staging || staging->size < totalSize) staging = createBufferAndMemory(...)`，只增不减），传完**不还**。
实测：704 MB 的车身模型在进程里留下 `SharedUsage = 1,228 MB` —— 正好 `2 × 614 MB`，因为上传按
`dataToCopy.frameIndex` 轮转，两次上传落在两块上，两块各涨到 614 MB 后一直留着
（`TransferTask` 有 `2 × numBuffers = 6` 个块，每块各自的 `staging` + fence + command buffer + command pool）。

同日的上一份 ADR 把它做成了**显式入口**（`RenderDevice::releaseUploadStaging()`，宿主或一条用户命令触发），
理由是"何时不需要这块内存只有宿主知道"。落地后暴露出两个问题：

1. **产品里最常见的宿主不调**：载入完就静止的查看器永远等不到"下一帧",而自动的"波"规则（等一帧没有上传）
   正好在最该生效的地方不生效；于是 1.2 GB 一直留着，用户只能看到"敲了命令也没反应"（那次是 park 还没到期）。
2. **一个动词里塞了两件事**：park 的退役点在提交之后 `slots + 1` 帧、完成水位又落后 `slots`，所以要么等
   `2 × slots + 1` 帧，要么"再请求一次"付一次计数 `deviceWaitIdle()` 把它排空。

## 决定

1. **暂存不保留。** 一块暂存是**一个** `vkCmdCopyBuffer` 的源，没有第二个读者；传完它就是死重。
2. **证据是那块自己的 fence。** `block->fence->status() == VK_SUCCESS` 就是"读过它的拷贝已完成"，比时间线窗口更强，
   而且**不用等**：时间线要帧、park 要窗口、设备空闲要停设备，fence 只是一个 `vkGetFenceStatus`。
3. **触发点是帧边界，不是宿主。** `VsgBackend::executeFrame()` 在最开头跑一次 `releaseFinishedUploadStaging()`：
   凡是 fence 已完成、且所在的 `DataToCopy` 没有待拷数据的块，把 `staging` 与 `buffer_data` 置空
   —— 正是 vsg 自己"不够大就重建"时处理的状态。宿主不做任何事，也不需要知道。
4. **SDK 去掉 `RenderDevice::releaseUploadStaging()`**，模型查看器的 `release_render_memory` 命令一并删除。
   没有"何时还"这个问题，就不需要一条让宿主回答它的入口（`G8`：没有消费者的扩展点）。
5. **派生 vsg 的任务来读/清那段状态。** `detail::StagingTask`（`vsg::Inherit<vsg::TransferTask, StagingTask>`）
   能看能清 `_earlyDataToCopy` / `_lateDataToCopy` 里每块的 `staging`、`buffer_data`、`fence`；
   会话起来时由 `installStagingTask()` 把 vsg 建的那个任务换掉（那时没有任何在飞的东西，换掉零成本）。
   `gfx_backend_vsg` 本来就是 vsg 适配器，`numBuffers = 3`、`transferQueue` 手工交接、`device->transferTask`
   观察者这些内部约定也一直写在这一层。
6. **不清"还在飞"的块。** fence 没完成的留给下一帧（帧边界不许阻塞）；`DataToCopy` 里还有待拷数据的块也跳过
   （下一次 `transferData()` 会重新分配，早清只是白亏一次分配）。

## 理由

- `G1` / `G7`：一条规则 —— 证据到了就还。显式入口那版把"要不要还"和"现在能不能还"塞进同一个动词，
  还要靠"再问一次"表达"现在"。
- `G10`：成本差是实打实的。显式版在宿主不出帧时只能 `deviceWaitIdle`（整设备停一次）；现在零等待、零 idle，
  `RetentionStats::device_waits` 在这条线上永远不动（用例直接断言）。
- `G8`：没有消费者要"现在"这个语义之后，动词没有存在理由；自动也不需要开关 —— 它等价于"用完就放"，
  不是一条可选政策。
- `G11`：失败看得见。暂存占用有了一个可读的数（`VsgBackend::uploadStagingBytes()` /
  `OffscreenSession::stagedBytes()`），用例直接断言它归零，而不是靠工作集回落的间接推理。

## 否决的选项

| 选项 | 否决理由 |
|---|---|
| 保留显式动词 + park/drain（上一份 ADR） | 最常见的宿主（载入完不动）不会调；而 park 要 `2 × slots + 1` 帧，还得补一条"再问一次 = 现在"。一个功能两套语义。 |
| 每帧都清、不等 fence | 销毁还在被拷贝读的内存。 |
| 只清比 `minimumStagingBufferSize` 大的块（保留 16 MB 可复用块） | 能让"每帧都上传"的宿主少一次分配，但它把"用完就放"变成"够大才放"，多一条判断和一条要解释的线；当前仓库里没有每帧都上传的宿主（见 backlog 的触发条件）。真出现了再加，一行判断。 |
| 直接从 `TransferBlock` 删元素 / 换掉整个任务 | 元素不能删（`*(frames[frameIndex])` 直接解引用），换任务会把 command pool / command buffer / fence 一起丢掉，还得把三处交接再正确一遍。 |
| 不派生，靠"上传完成后换任务" | 那是上一版机制：能还，但要知道"拷贝读完"就只能靠 park（要帧）或 `deviceWaitIdle`（停设备）。 |
| 把回收挂在"上传结束后第一帧没有上传"上 | 与帧边界规则等价但更弱（多等一帧、还要一条计数比较）；帧边界已经是那个地方。 |
| 起线程/定时器轮询 fence | 销毁 Vulkan 对象要在帧线程上；一个定时器只是把竞态换成了锁。 |

## 后果

- **内存**：一次上传的暂存活到"它的 fence 完成后的第一个帧边界"。704 MB 车身的 1,228 MB → **0**
  （进程 ws 的另外两部分 —— 模型自身 host 副本 ~700 MB 与进程基数 ~760 MB —— 不在这条线的范围内）。
- **性能**：每帧多 `≤ 6` 次 `vkGetFenceStatus`（纳秒级）；零 idle、零 park、零等待。
  **代价**：每帧都上传的宿主会在每帧重新分配一块（最小 16 MB）暂存 —— 见 backlog 的触发条件。
- **依赖 vsg 内部**：读/写 `_earlyDataToCopy` / `_lateDataToCopy`、`TransferBlock::staging`、`buffer_data`、`fence`。
  上游改名 = 编译错（响的）；上游改语义则由 `OffscreenSessionTest` 对字节数的断言兜住（`vsg-pitfalls.md` P42.2）。
- **ABI**：`RenderDevice` 少一个虚函数（本轮追加的那个槽位随之消失），`RenderEngine` 不变。
- **行为**：宿主不再需要（也无法）发起回收；`RenderDevice` 的接口回到"声明资源与跑帧"，不再有"清理一次上传"的动词。
- **测试**：`UploadScratchTest.TheUploadScratchIsNotRetainedAndTheFramesGiveItBack`（帧不 idle；上传后的帧让数归零；
  同一内容重画零上传；新流再上传、再由后续帧归零）、`OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands`
  （256 MB 工作集回落 + 4 KB 逐字节读回 + 字节数断言）、`OffscreenSessionTest.AReplacedTransferTaskLeavesTheSessionSubmitting`
  （起步就装好可问的任务、出帧不涨 idle）。
- **文档**：`graphics-content-lifecycle.md` §2.8 / F16–F22 / C4 / R2 / N7–N11 / 6.3 / D6–D13 / T5–T8；
  `vsg-backend.md` F16.2 / T4；`vsg-pitfalls.md` P42.2。

## 后续

- 宿主侧不再有调用点。想观测的宿主读 GPU 计数器的 `SharedUsage`，或者后端自己的 `uploadStagingBytes()`。
- 若将来出现"每帧都上传"的宿主（动态几何、流式点云）且分配进入 profile：加回"只清大于最小值的块"这一条判断。
