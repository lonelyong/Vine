# vsg 后端：拆机时释放了仍在被提交读取的资源

> **判据**：`test_vsg`（Release）全套运行约 50% 概率 `malloc(): unaligned tcache chunk detected` 或 SIGSEGV，落点漂移
> （`FrameUploadsTest`、`PrepareUploadTest` 各一次）；Debug 全套很少触发；旧提交同样触发。
> **结论**：vsg 后端的**资源寿命与其提交不同步**——两处释放发生在 GPU/驱动线程仍在读之后。已修两处；
> 仍有一处窄路径（prepare-only 拆机）在内存检查器下可见，见 §4。
> **权威来源**：`src/plugins/gfx_backend_vsg/src/internal/VsgBackend.cpp`（拆机顺序）、`OffscreenSession.cpp` 与
> `Session.cpp`（帧图换代）、`include/vine/backend_vsg/internal/GraphLifetime.hpp`（keep-alive）；内存检查器的原始栈在
> 本文 §2。

## 1. 症状

- 全套 Release `test_vsg`：约每两次运行一次崩在 `libc` 的堆检查里（`malloc(): unaligned tcache chunk detected`），
  或 SIGSEGV；崩溃点每次不同（`FrameUploadsTest`、`PrepareUploadTest.ASecondPrepareCopiesNothing`、…）。
- 单个用例重跑几乎总是绿；Debug 全套多数绿。
- 用 `git switch` 回到改动前的提交、重建二进制后**同样**能复现 ⇒ 不是某一轮改动引入的。

## 2. 证据（valgrind，Release 二进制）

`valgrind --error-exitcode=9 --num-callers=16 ./build-release/bin/test_vsg`（窗口套件过滤掉，跑 416 例后
valgrind 自身 SIGSEGV）给出 5 处 `Invalid read`，**全部**是 `libvulkan_lvp.so` 的工作线程读一块已经 `free` 的内存，
而释放栈全在本进程里：

1. **旧帧图被丢**（`FrameUploadsTest`）：读方是 lavapipe 工作线程；释放方是
   `vsg::Buffer::~Buffer ← … ← vsg::RenderGraph::~RenderGraph`，由 `OffscreenSession::assignFrameGraphs()` 里
   `task->commandGraphs = graphs;` 触发 —— 上一帧的图在这一刻失去最后一个引用。
2. **拆机释放材质图**（`PrepareUploadTest.ASecondPrepareCopiesNothing`）：释放方是
   `vsg::ImageView::~ImageView ← MaterialImages::Data::~Data ← VsgBackend::releaseContentWorld() ← shutdown()`。
3. 其余三处是同一情节的其它读取点（同一线程栈、同一批块）。

## 3. 根因

**这个后端把帧推进成流水线**（`core::FrameTimeline` + slot 环 + `FrameProgress`），所以“上一个 `executeFrame`
返回了”不等于“队列空了”；而 Vulkan 的生命周期规则是**仍在被提交引用的内存不得销毁**（对应 VUID 家族：
device memory / buffer / image 销毁时仍在使用）。后端自身在 `VsgBackend` 里**从不**调用 `waitForFences`
（全仓 grep 确认），资源释放完全依赖 slot 回收的“证据”，可**帧图与内容世界不是按这套证据释放的**：

- `assignFrameGraphs()` 直接替换 `commandGraphs`，丢掉上一帧的图（其中节点的析构释放驱动内存）；
- `shutdown()` 直接 `releaseContentWorld()`，而会话可能还有帧在飞。

## 4. 修了什么 / 还剩什么

**已修（两处，均按“证据属于会话”的方式）：**

1. `VsgBackend::shutdown()` 在**任何销毁之前**让每个会话 `waitDeviceIdle()`（offscreen 的公开方法 + 窗口会话的
   `SessionContentAccess::waitDeviceIdle`），并把这次等待计入 `RetirementQueue::noteDeviceWait()`。
2. 新增 `detail::GraphLifetime`（`include/vine/backend_vsg/internal/GraphLifetime.hpp`）：会话把将要替换掉的
   `commandGraphs` 收进一条 keep-alive 列表，按 **`core::FrameTimeline::completedFrame()`**（slot 回收给出的完成
   证据，和内容退休用的是同一套）逐帧丢弃；两个会话（窗口 / 离屏）的 `assignFrameGraphs` 都改成这个形状。

**量出来的效果**：Release 全套连跑 **4/4 全绿**（修前约 50% 崩）。

**仍剩（已定位、未修）**：`PrepareUploadTest` 的 **prepare-only** 路径——它 `prepareFrame()` 之后直接
`shutdown()`，很可能**一帧都没提交**（`submittedFrame() == 0`，因此 `waitDeviceIdle()` 提前返回），而 VSG 的
**viewer 线程/传输任务**可能仍在跑，于是 `MaterialImages` 的图像被销毁时仍有工作线程在读。修法方向（按 VSG 自己的
API）：拆机时先 `Viewer::stopThreading()`（并等传输任务完成）再 `deviceWaitIdle()`；`waitDeviceIdle()` 的
`submittedFrame() == 0` 早退也要按“有没有编译/传输工作在跑”重新判断。触发条件：下一次在内存检查器下复现，或
`test_vsg` 再次出现堆破坏。

## 5. 为什么验证层看不见

validation 只检查 **Vulkan 对象之间的引用**；这里是**宿主内存**（驱动在 `vkAllocateMemory`/`vkMapMemory` 时分配的
簿记块）被宿主提前 `free`、驱动线程继续读——对象句柄关系完全合法。只有内存检查器（或运气）能看见。这也解释了
“零 VUID 但会崩”。

## 6. 教训

- **“一帧返回”不是“队列空了”**：流水线后端的每一条释放路径都要说得出“谁证明它用完了”。本项目已有的证据是
  slot 回收（`FrameTimeline::completedFrame()`），新的资源类别必须接同一套，而不是各写各的。
- **关闭路径要和稳态路径用同一份证据**：`shutdown()` 之前先 idle 是廉价的；稳态靠 keep-alive、拆机靠 idle，
  两者都不需要 sacrifice 流水线。
- **内存检查器要跑“会崩的那棵树”**：ASan（Debug）连跑三次全绿，valgrind 跑 Release 一次就抓到 5 处 —— 这类
  时序相关缺陷先用 ASan 排除越界，再用 valgrind 跑真正会崩的构建。
