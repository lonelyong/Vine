# ADR-2026-11-13：上传暂存保留可复用块，一次性的大块仍然还给设备

> **状态**：已采纳
> **日期**：2026-11-13
> **取代**：[ADR-2026-11-10-upload-staging-is-not-retained.md](ADR-2026-11-10-upload-staging-is-not-retained.md)（只改一条：不再“全部不保留”）
> **影响范围**：`src/plugins/gfx_backend_vsg/{include,src}/internal/TransferStaging.*`、
> `src/viz/graphics/sdk/vine/graphics/FrameStats.hpp`（同批的 `FrameUploads`）、
> `tests/test_vsg/UploadScratchTest.cpp`、`.ai/design/{graphics-content-lifecycle,vsg-backend}.md`

## 背景

ADR-2026-11-10 的结论是“暂存一完成就全放”：一块 staging 是一个 `vkCmdCopyBuffer` 的源，拷贝的 fence 一
完成它就是死重。704 MB 车身的 1,228 MB 常驻因此归零，且零等待、零 idle。

那条规则的**代价**当时就写下了：每帧都上传的宿主会在每帧重新分配一块最小 16 MB 的暂存。仓库里一直没有这样的宿主
（帧内上传只在 revision 变化时发生），所以代价没有兑现。T26 决定把这笔代价先付掉：**保留下限大小、只放大的**。

## 决定

1. `StagingTask::releaseStaging()` 只释放 **`staging->size > minimumStagingBufferSize`** 的块；等于或小于该
   下限的块留着。下限读的是任务自己的 `minimumStagingBufferSize`（vsg 的 `TransferTask`，默认 16 MiB），不是
   硬编码常量。
2. 其余证据规则不变：块自己的 `fence->status()` 仍是唯一条件；没完成的拷贝留给下一帧；零等待、零 idle、零 park。
3. 结果：每帧上传的宿主在稳态**零分配**（保留的块被 vsg 的 `transferData()` 直接复用）；一次性大上传（模型载入）
   仍会把大块还掉——1,228 MB 那类问题不回潮。

## 理由

- `G10`：一个“每帧一次 16 MB 分配”的代价，与“内存常驻”不是同一量级，但它是唯一能靠一行规则消掉的。保留
  可复用块让两条曲线都好看：稳态零分配，峰值不常驻。
- `G4`：条件按**块的大小**而不是“是否还有待拷数据”——后者看着更便宜，但 `_transferData()` 会在没有待拷时提前
  返回而不清 map，条件会永久为真、sweep 冻结（ADR-2026-11-10 的坑，仍在）。
- `G8`：仍然没有为“宿主想保留多少”开任何入口；这是回收规则的一部分，不是策略。

## 否决的选项

| 选项 | 否决理由 |
|---|---|
| 维持“全部释放” | 每帧上传的宿主每帧一次 16 MB 分配；一次一行规则就能消掉，且不影响大块回收。 |
| 保留所有块、不做大小判断 | 大上传（模型载入）的块会常驻 —— 正是 ADR-2026-11-10 要解决的问题。 |
| 用“这个 `DataToCopy` 还有没有待拷数据”作为跳过条件 | `_transferData()` 提前返回时不清 map，条件永真 ⇒ sweep 冻结，整份载入暂存常驻。 |
| 让宿主配置保留量 | 没有消费者（`G8`）；回收规则不应变成另一个旋钮。 |

## 后果

- **内存**：小上传之后 `uploadStagingBytes()` 停在下限的倍数（每个被用过的 frame slot 一块，至多 `slots × 16 MiB`），
  不再归零；大上传之后回落到这个下限。
- **性能**：每帧上传的宿主稳态零分配（原先每帧一次 malloc + map）。一次性载入的行为不变。
- **证据**：`UploadScratchTest.TheUploadScratchIsNotRetainedAndTheFramesGiveItBack` 改为断言“存活的块都是可复用
  尺寸的倍数，且大于下限的一次性块会被还掉”；T26 的用例在同一次提交里。
- **API**：无（`StagingTask` 仍是内部类型；`minimumStagingBufferSize` 仍是 vsg 的）。

## 触发条件（何时再改）

出现“稳态下保留的块仍然占住不可接受的显存/主机内存”的真实宿主：届时按新 ADR 再定是否需要可配置的上限，而不是
改回“全部释放”。
