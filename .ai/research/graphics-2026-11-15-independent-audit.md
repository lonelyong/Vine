# graphics 渲染栈独立审计（2026-11-15）

> **状态**：审计结论（非权威；采纳后并入 `design/` / `backlog/` / ADR，然后删除本文件）
> **范围**：`src/viz/graphics`（SDK + 引擎内部）+ `src/plugins/gfx_backend_vk`（9,552 行 `.cpp`）+
> `src/plugins/gfx_backend_vsg` 与接缝的边界。**不含** appfw UI 层、`test_*` 之外的测试框架、平台层实现。
> **方法**：只读代码审计 + 计数（动词数 / 情节数 / 行数 / 词汇表数）+ 文档句与实现逐条对账。
> **没有做**：没有跑 GPU 用例、没有新基准；性能数字引用既有记录（`../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`）。
> **与既有审查的关系**：`graphics-2026-11-14-vk-industry-review.md` 按"与行业的差距"审（F1–F13，多数已落地）；
> 本文按**先定的判据**审，覆盖到那份没有问的问题（失败分类、诊断结构、接缝分解、证据缺口）。

## 1. 先定标准

> 一条判据只有能说清"什么证据算通过"才算判据；否则它只是口味。
> 七组共 26 条，编号 `C1`–`C26` 稳定引用。仓库级设计约束 `G1`–`G13` 是**上层**判据，这里不重复；
> 本文每条都落在"渲染框架"这个具体面上。

### C1–C4 契约真实性：公开面不许承诺实现做不到的事

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C1 | 每个公开动词的文档回答五件事：做什么 / 所有权与寿命 / 失败形态 / 线程 / 明确不做什么 | 逐动词表，抽查 10 个 | 寿命或线程要靠读实现才知道 |
| C2 | 文档里可判定的词（always / never / one / first / immediate）与实现一致 | 抽查 + 变异 | 类级契约句被自己的某个字段反驳 |
| C3 | 公开面不含实现层类型，接缝只表达领域 | 机器门禁 | SDK 头出现 `Vk*` / `vsg::` |
| C4 | 领域概念有类型，不用裸 int 表达身份 / 单位 / 数量 | 抽查签名 | 无约束的 `int` 承载语义 |

### C5–C8 寿命与所有权：不悬垂、不泄漏、不半主

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C5 | 每份资源一个 owner；每个借用有一句话说明"消费时刻谁保证它活着" | 所有权表 | 借用与拥有混在同一结构里且未逐字段说明 |
| C6 | GPU 对象销毁晚于最后一次可能读，证据**被计数**（栅栏 / 退休），不是假设 | 计数器 + 用例 | "因为同步所以不需要" |
| C7 | 缓存键含 revision ⇒ 条目不可变；没有条目比它的键活得久 | 键定义 | 可变的条目 |
| C8 | 句柄失效可观察（空 / `Lease`），不是悬垂 | 用例 | 裸指针回读 |

### C9–C12 同步：一张表、一个串行化点、一个"完成"

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C9 | 每次 layout 转换只经一张表，没有内联屏障 | 单一实现 | 散落的 `vkCmdPipelineBarrier` |
| C10 | 每个 externally-synchronized 的 Vulkan 对象只有**一处**串行化 owner | 表 | 两处各自加锁 |
| C11 | "完成"是契约词：谁证明、不成立时答案是什么 | 契约 + 用例 | 隐含的时序假设 |
| C12 | 竞争要么按构造不可能，要么有用例能变红（TSan / 契约断言） | TSan 报告 | 推理 |

### C13–C16 成本模型：稳态、有界、有证人

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C13 | 稳态帧在 arena 外零堆分配，并且有断言 | 分配计数断言 | "我们相信" |
| C14 | 分配次数是**资源数**的函数，不是帧数的函数 | 计数器 | 每帧建 / 毁 |
| C15 | 每个优化都有证人（计数器 / 报告），且变异证明证人承重 | 变异电池 | 证人存在但无人钉 |
| C16 | 增长有界：缓存达到稳态，没有东西随帧数增长 | 长跑用例 | 表只增不减 |

### C17–C20 失败语义：一类一条路、不留半态、带得动定位

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C17 | 失败分类唯一（可恢复 / 会话结束 / 不支持），各有**唯一**处理路径 | 分类表 | 同一类失败两条路径 |
| C18 | 失败不留下半应用状态；false 从不意味着"部分生效" | 用例 | 半建成的会话 / 半释放的绑定 |
| C19 | 恢复路径有用例（设备丢失 / 表面重建 / 缓存损坏） | 用例 | 只有主路径 |
| C20 | 失败报告带足够定位的数字（请求 vs 支持 / 对象身份 / `VkResult`） | 抽查句子 | "could not be allocated" |

### C21–C24 证据：可被反驳，且不能静默失去覆盖

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C21 | 每条可破的契约句有用例，且变异能变红 | 变异记录 | 守卫存在但无用例 |
| C22 | 两个实现并存时逐字节对拍 | 对拍用例 | 各自测自己 |
| C23 | 门禁机器运行，且"命中 0 个文件"本身算一条 finding | 探针 | `0/0` 报绿 |
| C24 | 环境依赖是 skip + 理由，且 CI 必须有一台真设备 | CI 配置 | 全绿零覆盖 |

### C25–C26 可演进：加法、单一实现

| # | 判据 | 通过的证据形态 | 违反的形态 |
|---|---|---|---|
| C25 | 新格式 / 新 pass 种类 / 新后端是加法，不改核心模型 | **扩展点成本表** | 没有这张表，所以"易于扩展"不可证伪 |
| C26 | 同一概念只有一处实现（没有两份"同一件事"） | grep + 设计文档 | 同一条规则两个实现 |

## 2. 判词（按判据，不按印象）

| 组 | 判词 | 最能说明问题的一条依据 |
|---|---|---|
| C1–C4 契约真实性 | **够用偏强** | 18 个设备动词里 13 个有默认实现，且多数返回类型化答案；但 `FrameDescription` 的类级"全是借用"句被三个 owning 字段反驳（A4） |
| C5–C8 寿命与所有权 | **强** | 键含 revision、`Lease` 可观察、退休被计数；唯一弱点是"因为同步所以立即销毁"（A11） |
| C9–C12 同步 | **强（单会话）/ 未证（多线程）** | 屏障一张表、外部同步点唯一；但 TSan 证据仍缺（A9） |
| C13–C16 成本模型 | **强** | ring / set / 池 / 缓存四件套各有证人 + 变异；缺 per-pass GPU 归因（A5） |
| C17–C20 失败语义 | **弱 → 中（A1 / A8 已落地 2026-11-15）** | 分配失败现在分类在设备一处、带数字、可注入；仍缺的是内存压力策略（A2） |
| C21–C24 证据 | **强（成功路径）/ 弱（失败路径）** | 变异电池只覆盖成功路径的守卫；失败不可注入（A8），无设备即 skip（A13） |
| C25–C26 可演进 | **中** | 概念单一定义做得很好；但扩展点成本没有表（A12），退休规则有两个实现（A11） |

## 3. 发现（按严重度）

### A1（高，C17 / C20）分配失败把"设备丢失"吞成"显存不足"（**已落地 2026-11-15**）

- **证据**：`MemoryAllocator.cpp` 的 `createBlock` 对任何非 `VK_SUCCESS` 一律给
  `MemoryRefusal::AllocationFailed`，`VkResult` 被丢弃；`MemoryRefusal` 只有 `Ok` / `NoType` /
  `AllocationFailed` 三态，而注释自己写着"`VK_ERROR_OUT_OF_DEVICE_MEMORY`、a lost device、a bad handle"
  三类合流。**五个业务分配点**（`TargetImages.cpp` 一处、`ContentDraw.cpp` 一处、`ContentImages.cpp` 三处）
  **都不传 `refusal_out` / `why`**——只有 `ReadbackStaging.cpp` 传，所以理由在其余五处算出来就丢。
- **为什么是问题**：设备丢失是仓库自己定的"会话结束"类，`noteDeviceLost` 是唯一分类点——这条路径绕过了它。
  宿主看到的是"目标建不起来"，于是继续在死设备上重试，正是 `vk-backend.md` §2.2 说要避免的形态。
  `G4`：一个失败值必须说它是什么。
- **最佳设计**：分配器**不自己分类**——把 `VkResult` 交回（或 `MemoryRefusal` 加 `DeviceLost` /
  `OutOfMemory` / `TooManyObjects`），由 `Device` 的同一个 `noteDeviceLost` 收口；四个调用点必须消费
  `refusal_out`，并把"请求字节 / type / 已用 / 保留"写进句子。
- **代价**：约 120 行 + 一条注入式用例（A8）。**触发：无**（正确性）。

### A2（高，C16 / C17）没有内存压力策略（**已落地 2026-11-15，以“可行动的拒绝”形式**）

- **证据**：`createBlock` 是"first-fit + 新建块"；`MemoryStats` 有 used / reserved / peak，但**没有
  free / 最大连续 run**；`MemoryRefusal` 没有"碎片"与"预算"两态；没有 eviction、没有 compaction、
  没有"先释放空块再重试"的阶梯。
- **后果**：1.2 GB 模型载入之后（`ADR-2026-11-13-staging-keeps-the-reusable-block.md` 记过这个量级），
  一次 resize 可能因为块内碎片失败，而失败形态与"设备真没有内存"完全一样；宿主无法决定"释放内容"还是"降级"。
- **最佳设计**：`MemoryStats` 加 `free_bytes` / `largest_free_run`（按 type）；失败前走一条**有界**的回收阶梯
  （释放全空块 → 合并相邻空 run（已有）→ 报 `Fragmented` 并带数字）；再加一个可选**软预算**（按 type），
  超预算先回收再失败。
- **代价**：约 150–250 行 + 用例。**触发**：真实宿主出现"显存够但分配失败"，或需要软预算。

### A3（高，C1 / C20）诊断没有 code，也没有结构化字段（**部分落地 2026-11-15**）

- **证据**：`RenderDiagnostic` = `{severity, category, message}`；`DiagnosticCategory` 有 12 个值，按
  "后果"分（`RenderDiagnostic.hpp` 自己写明），所以同一 category 下的不同原因不可区分。而
  `graphics-frame-pipeline.md` 的 `R28` 要求"稳定 code 与结构化字段"。
- **后果**：宿主想"给这个节点挂个角标"必须解析句子；测试只能按 category 断言（粒度粗）；改一句措辞就可能
  让宿主的正则失效。
- **最佳设计**：`RenderDiagnostic` 加 `code`（稳定字符串常量表或枚举）+ 可选结构化尾（`pass` 身份 /
  `content` 身份 / `requested` 与 `supported` 两个整数）；`message` 只给人看；
  `check_diagnostic_formats.py` 升级为"每条报告必须带 code"。
- **代价**：SDK + 两个后端约 200–300 行；门禁 1 条。**触发：无**（`R28` 已经要求）。

### A4（高，C1 / C2）`FrameDescription` 的类级契约句是错的（**已落地 2026-11-15**）

- **证据**：`RenderFrameExecutor.hpp` 写 "BORROWED ARGUMENTS. Every pointer or reference INSIDE a
  FrameDescription (camera, commands, lights, target, program) is borrowed for the duration of executeFrame()
  and never owned"——但 `DrawDescription::commands` 是 `std::shared_ptr<const std::vector<RenderCommand>>`，
  `program` 与 `PassDraw::program_override` 是 `intrusive_ptr`：**三个成员是 owning 的**。
- **后果**：同一句契约对同一结构的不同字段含义不同。一个"照文档实现"的后端要么把 `commands` 当借用而 UAF，
  要么以为自己在借 `program` 却实际持有了一份。
- **最佳设计**：这句话改成**逐字段规则**（三个 owning 成员单独点名，说明它们为什么 owning——身份与寿命），
  或更进一步把帧值拆成 `FrameValue`（纯值）+ `FrameHandles`（owning），借用只出现在展开那一刻。
- **代价**：文档 1 处立刻；拆分约 300 行（可选）。**触发**：拆分等第二个"需要跨帧持有帧值"的消费者
  （render graph / 离线渲染）。

### A5（中高，C13 / C15）GPU 计时没有 per-pass 归因（**已落地 2026-11-15**）

- **证据**：`Device.cpp` 只有 `timestamp_pool` 的 query 0 / 1（帧首、帧尾），且每 `kTimestampSampleFrames`
  帧才采样一次；`FrameReport::timing` 只有整帧。
- **后果**："哪个 pass 贵"无法回答，优化只能靠猜。上一轮把整帧设备时间当成宿主工作量的误读，正是这个缺口的
  直接后果（`ADR-2026-11-15-vk-keeps-one-frame-in-flight.md` 的更正）。
- **最佳设计**：每个 pass 一对时间戳，写进**有界**的 query pool（`passes × 2`，按帧槽轮转）；
  `FrameReport` 加 `per_pass` 数组（缺席 = 不报时钟，不是 0）。
- **代价**：约 150 行 + 1 条用例（"各 pass 时间之和 ≤ 整帧 + 噪声"）。

### A6（高，C25 / C26）一个 `Device` 对象六个关注点

- **证据**：9,552 行 `.cpp`；`Impl` 一个结构装设备身份 / 内存池 / 驻留缓存 / 目标 / 窗口 / 帧机器 / 报告；
  **16 个 `ReportOnce` 成员**——这个对象回答 16 个互不相干的条件。
- **最佳设计**：四个内部单元 + 一个装配根（`MemoryPool` / `Residency` / `Targets` / `FrameScratch`），
  `Device` 只做装配与转发。这与 `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` 是同一根因的两侧：
  那份 ADR 解决"谁拥有帧"，这里解决"池自己怎么分"。
- **代价**：约 800–1200 行搬家。**触发**：与 ADR 第二步同一批（避免搬两次）。

### A7（中高，C1 / C26）SDK 的 `RenderDevice` 18 个动词、13 个有默认实现

- **证据**：`RenderDevice.hpp` 18 个动词（另有 1 个析构），其中 **5 个 pure**（`initialize` / `shutdown` /
  `initialized` / `executor` / `setRenderTarget`）、**13 个有默认体**；`RenderFrameExecutor` 已经
  证明"按面拆"是本仓库自己的做法。
- **后果**：接口有五个变化原因（能力 / 目标 / 驻留 / 诊断 / 生命周期），任何新后端都要在一个类上实现五组
  不相干的事；默认实现让"没实现"与"实现了但什么也没做"长得一样。
- **最佳设计**：拆成 `RenderDevice`（身份 + 能力 + 生命周期）、`RenderResourcePool`（驻留 + 释放 + 内存报告）、
  `RenderTargetFactory`（目标 + 表面）、`RenderDiagnostics`（路由 + 计数）；`RenderBackend` 仍是聚合。
- **代价**：约 400 行 + 所有后端与测试的签名。**触发**：**第二个 API 家族的后端**（D3D12 / Metal）——
  那时接缝形状是被迫的；今天拆只是美学（`G8`）。

### A8（中高，C19 / C21）失败路径没有可注入的测试（**已落地 2026-11-15**）

- **证据**：没有用例能让 `vkAllocateMemory` 失败、让 `vkBindImageMemory` 失败、或让设备在分配路径上丢失；
  `memory/graphics.md` 记录的变异只覆盖 ring / set / 池 / 缓存文件这些**成功路径**的守卫。
- **后果**：A1 那种缺陷**不可能被现有用例发现**——它只活在失败路径上。
- **最佳设计**：给低层单元一个故障注入点（`MemoryAllocator` 的 `failNext`，或包装 `VolkDeviceTable`），
  两条用例：①"分配失败 → 目标建不起来 → 一次诊断 → 会话仍可用"；②"设备丢失 → 后续帧 `Lost` →
  `shutdown` + `initialize` 恢复"。
- **代价**：约 150 行测试装置 + 2–3 条用例。**触发：无**。

### A9（中，C12）线程契约没有 TSan 证据（**部分落地 2026-11-15**）

- **证据**：backlog `T34` 自认"TSan 树里实跑 `test_vsg` 的子集仍缺"。
- **后果**：本仓库最核心的线程主张（两个世界各一个写者）今天的证据是推理，不是工具。
- **最佳设计**：把 TSan 跑成门禁的一部分（一个目标 + 一个脚本 + 一条记录），并把 ADR 第二步的两会话用例
  一起跑。
- **代价**：环境（clang TSan 运行时）。**触发**：下一次线程改动（ADR 第二步）。

### A10（中，C26 / C1）五套"为什么不行"的词汇表

- **证据**：`ReadbackResult` / `SurfaceTargetResult` / `MemoryRefusal` / `PipelineCacheRefusal` /
  `PlanRefusal` 各定义一套。
- **后果**：宿主与测试要学五遍；同一个概念（不支持 / 还没好 / 参数非法 / 失败 / 没容量）在五处不同拼写。
- **最佳设计**：一张共享的**失败词汇表**（`Unsupported` / `NotReady` / `Invalid` / `Failed` /
  `NoCapacity` / `DeviceLost`），各接口在自己的枚举里引用同一套语义与顺序。
- **代价**：SDK 层约 200 行（多为映射）。**触发**：与 A3 同批（同一个改动面）。

### A11（中，C6 / C26）"毁之前证明没人读"有两个实现，其中一个叫"不需要"

- **证据**：vsg 有 `RetirementQueue` + `FrameTimeline`；vk 的 `releaseRenderTarget` 写"this device
  submits and waits on one frame at a time, so ... the destroy is immediate"。
- **后果**：ADR 第二步（多会话）必然要在 vk 里重新发明 vsg 已有的东西，而两份实现会漂移。
- **最佳设计**：把"退休点"变成接缝的一个值（每个会话的 `submitted` / `completed`），vk 的同步模型是它的
  **退化情形**（`completed == submitted`）——一份规则、两个后端、零漂移。
- **代价**：约 150 行（把 vsg 的机制上移一半）。**触发**：ADR 第二步。

### A12（中，C25）扩展点成本没有表（**已落地 2026-11-15**）

- **证据**：`PassKind` 2 种、`DrawDescription::Kind` 2 种；没有一张"加一个 pass 种类 / 一个格式 /
  一个后端要改哪些文件"的表。
- **后果**："易于扩展"不可证伪；真加的时候才发现要改六处。
- **最佳设计**：在 `vk-backend.md` / `graphics-architecture.md` 加一张**扩展点成本表**（一行一个扩展点、
  一列一个要改的位置），并用一条骨架用例证明它。
- **代价**：文档 + 0 代码。**触发**：下一次加能力时。

### A13（低–中，C24）`test_vk` 无设备即 skip

- **后果**：CI 可能"全绿零覆盖"；`check_vk_exposure.py` 的 E2 只覆盖低层单元。
- **最佳设计**：CI 必须有一台带 Vulkan 的机器（lavapipe 即可），且 skip 要在报告里**显式计数**
  （"0 个设备用例被执行"不是绿）。
- **代价**：CI 配置。**触发**：无。

### A14（低，C2 / C11）`releaseRenderTarget` 的"立即销毁"是未定义，不是契约（**已落地 2026-11-15**）

- **证据**：文档写"between frames, on the frame thread"，但没有任何用例在帧在飞时调用它。
- **最佳设计**：写成前置条件（"调用者保证没有在飞帧命名它"），并在 ADR 第二步换成退休点。
- **代价**：文档 1 行；与 A11 同批。

## 4. 做对了什么（核过的，不是客套）

1. **颜色空间是对的**：`WindowTarget.cpp` 选 `VK_FORMAT_*_SRGB` + `SRGB_NONLINEAR`，离屏目标保持线性，
   `ContentImages.cpp` 保留纹理的 sRGB 拼写（采样时解码）。这是多数示例工程会错的地方。
2. **插件 ABI 有握手**：`Plugin.hpp` 的 `PluginAbi::abi_version` 必须是第一个成员，`static_assert` 钉住
   偏移，`VN_APPFW_PLUGIN_ABI_VERSION` 随面变化升版——后端插件走的是同一条门。
3. **引擎线程不空转**：`FrameRunner.cpp` 用条件变量（`wake_` / `done_`），没有 spin。
4. **屏障是一张表**（`BarrierPlan.cpp`）：layout / stage / access 三拼写在一处。
5. **暴露边界是机器检查的**（`check_vk_exposure.py`）：SDK 面零 `Vk*`、shell 零 `Vk*`、帧路径不许走逃生门。
6. **两个后端逐字节对拍**（`BackendParityTest.cpp`）：比任何单后端用例都强。
7. **`ReportOnce` 的 episode 纪律**：引擎 6 个 + vk 设备 16 个"一个条件一句话"的状态，`rearm` 由条件结束
   的那一处调用，不在关机时统一清空。
8. **预热契约**（`ADR-2026-11-14-prepare-may-submit-transfers.md`）：`prepareFrame` 可以提交一个只含传输
   的批，第二次预热全零——"加载期把纹理落盘"的诚实形状。

## 5. 目标形状（按最佳设计，不被现状束缚）

### 5.1 设备：四个单元 + 一个装配根（对应 A6 / A7）

```
RenderDevice（接缝：身份 / 能力 / 生命周期 / 工厂）
├── MemoryPool      分配、回收阶梯、按 type 的统计与软预算     ← A1 / A2
├── Residency       内容缓存（键 = 身份 + revision）、三态、释放
├── Targets         目标与附件、形状缓存、claim                 ← A11 / A14
├── FrameScratch    每会话：命令池 / 栅栏 / 常量环 / 描述符池   ← ADR-2026-11-15
└── Diagnostics     路由、计数、code + payload                 ← A3 / A10
```

判据：每个单元能被单独测试（`C25`），且"变化留在单元内"（`G5`）。`Device` 只剩装配与转发。

### 5.2 诊断：code + payload，message 只给人看（对应 A3 / A10）

`{severity, category, code, payload, message}`，其中 `code` 是稳定常量表、`payload` 是可选的结构化尾。
门禁从"格式检查"升级为"每条报告必须有 code 且 code 在表内"。

### 5.3 内存：分类、预算、可注入（对应 A1 / A2 / A8）

- `VkResult` 不丢：`DeviceLost` 走 `noteDeviceLost`，其余按 `OutOfMemory` / `TooManyObjects` 分类。
- `MemoryStats` 加 `free_bytes` / `largest_free_run`，报告带数字。
- 回收阶梯有界（空块 → 合并 → 失败），可选软预算。
- 故障注入是**测试装置的一部分**，不是产品分支（`#ifdef` 或注入表）。

### 5.4 可观测性：per-pass GPU 归因（对应 A5）

每个 pass 一对时间戳，有界 query pool，按帧槽轮转；`FrameReport` 的 `per_pass` 缺席 ≠ 0。

## 6. 落地顺序与代价

| 批次 | 内容 | 量级 | 触发 |
|---|---|---|---|
| 1 | A1 + A8：分配失败分类 + 故障注入 + 两条用例 | ~250 行 | **已落地 2026-11-15**（`MemoryRefusalReport` + sink + `MemoryBlockCreator`，5 条新用例，3 个变异各自被对应用例杀掉） |
| 2 | A3 + A10：诊断 code + 统一失败词汇 + 门禁 | ~350 行 | **A3 部分落地 2026-11-15**：`RenderDiagnostic` 加 `code` / `subject` / `limit`，vk 的内存/会话/目标三类共 16 个 code 已转换并登记，门禁双向检查 + baseline 棘轮；剩余 90 个调用点在 `scripts/diagnostic_code_baseline.txt` 里逐批转。**A10 暂缓**：五个枚举回答的是不同问题，统一它们会丢精度（无消费者，`G8`）。**A2 已落地**（同一提交系列）：`free_bytes` / `largest_free_run` 只在拒绝时算并写进句子；池内无阶梯可加（见 `vk-backend.md` §2.12） |
| 3 | A4 + A14：契约句修正 | 文档 | **已落地 2026-11-15**（逐字段的拥有/借用规则；`releaseRenderTarget` 的前置条件） |
| 4 | A5：per-pass GPU 时间 | ~150 行 | **已落地 2026-11-15**（`FrameTiming::passes` + 每 pass 一条边界 + 可增长的 query 池；两个变异各自被杀） |
| 5 | A6 + A7 + A11：设备四单元 + 接缝四面 + 退休点上移 | ~1400 行 | ADR 第二步 / 第二个 API 家族 |
| 6 | A9 的第一步：`FrameGate`（一帧一次，检查而非假设）+ 两线程用例 | ~200 行 | **已落地 2026-11-15**（`backend/FrameGate.hpp`，vk 与 vsg 同一实现，`PresentOutcome::Refused`） |
| 7 | A9 + A13：TSan 门禁 + 真设备 CI | 环境 | 下一步线程改动 |
| 8 | A12：扩展点成本表 | 文档 | **已落地 2026-11-15**（`graphics-architecture.md` §2.11：五个扩展点，列的是实际文件与处数） |

批次 1–3 是"正确性与诚实"，不依赖任何触发条件；批次 5 是**大改**，与
`ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md` 的第二步合并执行，避免同一片代码搬两次。

## 7. 审计边界（没做的事）

- **只读**：没有跑 GPU 用例、没有新基准；A5 的收益、A2 的碎片率都需要落地后测量。
- **不审 vsg 的逐行实现**：只审它与接缝的边界（A11 是边界问题）。
- **不审 Windows / Wayland**：本机无法验证，`test_vk` 的平台分支不在判据覆盖内。
- **不审 appfw UI 层**与 `docs/guides/` 的用户文档准确性。
- 颜色空间、插件 ABI、线程等待策略是**核过并判为正确**的（§4），不是漏掉。

## 8. 参考

- 代码：`RenderDevice.hpp`、`RenderFrameExecutor.hpp`、`RenderDiagnostic.hpp`、`Diagnostics.hpp`、
  `MemoryAllocator.cpp`、`TargetImages.cpp`、`ContentDraw.cpp`、`ContentImages.cpp`、`Device.cpp`、
  `WindowTarget.cpp`、`FrameRunner.cpp`、`BarrierPlan.cpp`、`PipelineCacheFile.hpp`、`Plan.hpp`、`Plugin.hpp`
- 文档：`../design/graphics-frame-pipeline.md`（`R28`）、`../design/vk-backend.md`（§2.2 / §9.2）、
  `../design/graphics-threading.md`、`../design/vsg-backend.md`
- ADR：`../decisions/ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`、
  `../decisions/ADR-2026-11-15-vk-keeps-one-frame-in-flight.md`、
  `../decisions/ADR-2026-11-14-prepare-may-submit-transfers.md`、
  `../decisions/ADR-2026-11-13-staging-keeps-the-reusable-block.md`
- 前一轮审查：`graphics-2026-11-14-vk-industry-review.md`（按行业差距，F1–F13）
