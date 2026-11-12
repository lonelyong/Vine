# graphics 内容生命周期：释放与缓存

> **状态**：现行
> **最后核对**：2026-11-12
> **范围**：内容对象的宿主所有权、GPU 驻留释放与缓存策略，以及上传暂存的回收。
> **权威来源**：`RenderDevice.hpp` / `RenderEngine.hpp` / `MaterialImages.hpp` /
> `core/MaterialArena.hpp` / `core/Streams.hpp` / `core/BlockStorage.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

本文件定义 graphics 内容生命周期：释放与缓存 的现行设计：先给对象与边界，再给功能、接口、约束与验收。

### 1.2 目标

- 把 §2 的设计要点落成 §3 的功能点、§4 的接口与 §8 的验收。
- 让“现在是什么”与“为什么这么定”分开：理由在 §2/§7，历史看 git。

### 1.3 非目标

- 不在本文件重复头文件里的完整签名；接口只给契约与指针。
- 不在本文件保存已关闭的历史或逐轮审查记录。

### 1.4 一句话结论

> 内容对象归宿主，GPU 驻留归后端；释放走显式入口；缓存不设容量上限，用多少拿多少。

## 2. 设计要点

2.1 **两层寿命**：`Geometry` / `Material` / `ShaderProgram` / `Texture` 是宿主对象；上传、块、描述符、
    编译变体是后端驻留。
2.2 **释放 GPU 驻留，不释放对象**：`releaseContent()` 之后对象仍可用，下一次使用重建驻留。
2.3 **显式管理**：没有 LRU、没有字节上限、没有压力淘汰；释放由宿主说。
2.4 **不 idle 设备**：释放是宿主的一句话（标记），行的回收按 epoch（2.10）；只有破坏路径用计数 device idle。
2.5 **流按 buffer 身份共享，寿命按“谁还声明这些字节”**：`StreamKey` = buffer + revision + 段；稳态零重传。
    条目持有它键名的那个 `Buffer`，地址与条目同寿。驻留寿命**不看时间**，由两个 O(1) 事实决定：
    内容层发布的声明索引（`ContentFacts::declarations`，由 `ContentStore` 在行变更时同步维护的
    地址→声明计数）说“还有没有活内容声明这块 buffer”；条目自己的句柄说“revision 是不是它上传时的那个”。
    没有声明、或 revision 已经走了的条目停车离场（`backend/ResourceTable`）。每帧成本 O(#驻留条目)，
    不随内容规模走（裁剪不删行，所以扫行会把“什么都没画”的帧变成最贵的帧）。
2.6 **按身份做键的表，键还活着时条目必须活着；只有上传仍按地址**：几何 / 材质 / 程序的事实行按内容句柄
    `(id, revision)` 做键（`ContentId.hpp`），编译半边与描述符集按同一个 `ContentRef`（再加 layout / variant /
    kind）做键 —— “同一地址上的另一次描述”这一整类错误对它们不再存在，因为句柄在释放时移动 generation，陈旧引用
    直接答 gone。**仍然按地址做键的只有上传表**（`StreamUploads` 的 `StreamKey` = `Buffer` 地址 + revision + 段，
    `Buffer` 不在内容对象体系里）：它的条目**持有它键名的那个 `Buffer`**，停靠排不上时**保留条目**、下一帧再试，
    绝不提前把地址还给分配器；计划读的实例流也由计划持有（见 ADR-2026-11-09-address-keys-hold-the-object）。
    显式释放用同一个地址命名：`releaseContent(const Buffer<float>& / const Buffer<uint32_t>&)` 按地址把这条流的条目
    停靠，不需要给 `Buffer` 造身份（G1：在这一层，地址就是它唯一的身份）。
2.7 **三个词，三个意思**（读代码前先看这张表，否则“identity”会指三个东西）：
    - **name**：`Node::name()`，只用于查找与调试，**不**是身份。
    - **key（id + revision）**：缓存表的键。`id` 是内容对象的 `Handle`（见 `ContentId.hpp`），`revision` 说的是
      “这一次的值”；仍然按地址做键的只有上传表（见 2.6）。
    - **variant id**：`core::VariantPool` 发的不重复句柄，只用于认“这一份编译结果”，可以跨帧失效。

2.8 **一次上传的中转暂存，寿命是“一次上传”，不是驻留**：`releaseContent()` 回答的是“宿主还想不想为这个对象保留一份 GPU 副本”，
而一次上传用来中转的 host 暂存（vsg 是 `TransferTask` 的 `staging`：整次上传一个 `HOST_VISIBLE` 缓冲，只增不减）在拷贝落地后
**没有第二个读者**——它既不是宿主对象的驻留，也不是可复用的缓存，只是没还给系统的临时内存。为什么：先把它归类，才谈得上谁决定“何时还”。它不能进 `releaseContent` 的口径（那会替宿主决定驻留），
也不该变成容量上限/预算（`G8`：没有消费者）。
边界：**这块暂存不保留**。它是**一个** `vkCmdCopyBuffer` 的源，没有第二个读者；拷贝的 fence 一完成它就是死重。
所以“何时还”不是一道需要谁决定的题：证据（那块的 fence）到了就还，触发点是**帧边界**，宿主不参与、也不需要知道（F16–F21）。
满足 `G1` / `G7` / `G10`：一条规则、零等待；同一件事也不再有第二个名字（`RenderDevice` 上不再有“清一次上传”的动词）。
代价写在 D14：依赖 vsg 的 protected 块状态（派生任务读/清）。

2.9 **“不再被画”的三个开关，后果也不同**：显式 `releaseContent()`（宿主说“我不要驻留了”；对象，或一条流所读的
    `Buffer`）、
   宿主丢掉最后一个引用（对象离树，行与驻留才会被扫尾）、以及**只是这一帧没被画**（隐藏、被视锥裁掉）。
   前两个由**持有量**决定；第三个不改变持有量，所以**什么都不释放**：
   - 表行（`ContentStore` 的 geometry / program / material）只看持有量：对象还在（场景图把着），行就留着，
     所以隐藏/裁剪不会造成建行-丢行的抖动（`ContentStore.cpp` 的 `releaseAbandoned` 以 `useCount() <= 1` 为条件）。
   - 上传（`StreamUploads`）也只看**持有量**，看的是**声明**：内容层发布的声明索引
     （`ContentFacts::declarations`）说“还有活行声明这块 buffer 吗”，条目自己的句柄说“revision 还是不是它
     上传时那个”。隐藏或裁剪不删行 ⇒ 地址仍被声明 ⇒ 上传留着，回来**零重传**；
     对象离树 ⇒ 行过了 epoch 窗口删掉 ⇒ 声明消失 ⇒ 上传随之离场。buffer 换代（`Buffer::edit()`）
     也让旧条目自己失去 revision 那一半，不需要几何说话。
   - 纹理（`MaterialImages`）走**同一条规则**：声明是材质行的 slot（`declaresTexture`）或该帧某个 pass 的导入
     输入（`ImageInput`，没有事实行也照样算），另一半是条目自己的 `Texture::revision()`；离场不 park（描述符集
     自己持有 view / sampler），所以它在 `ContentAssembly::beginFrame` 里和流一起问。
   - 编译半片 / 描述符集是**会话级**的，不随 pass 走：它们只在“表不再回答这个 program（id + revision）”或
     “没有任何 geometry 还问这个 layout”时被停靠（`ContentHalves::answerable` / `ContentSets` 的 sweep）。
   - 材质块（arena 槽）也走**同一条规则**：槽按**身份**保留，每帧问一次声明索引
     （`ContentDeclarations::declaresMaterial`：材质表的 per-identity 行计数）“还有活行吗”；没有就
     **当帧立即放槽**（`MaterialArena::retireUnpinned`）。不是 park——arena 的副本轮转就是那个窗口，
     放掉的是“可以复用这个槽”的权利，不是在飞帧要读的字节。
   为什么这么分：把“对象没了”和“这一帧没画”合成一个开关，就必然要么为裁剪抖动付重建代价，要么把已离树的
   驻留永久留（`G1`：两个问题，两个答案）。流这一步已经从“时间窗猜第三个开关”改成“只问谁还声明”（见 ADR）。

2.10 **内容行的寿命是算出来的，不是停靠的**：每条几何 / 程序 / 材质行记它最后一次被计划命中的帧
    （epoch），帧边界回收满足两条的行：`last_epoch < completedFrame() + 1`，且**不再是条目当前答案**。
    两个后果：
    - 被取代的 revision（条目已给出新行）与已释放条目（宿主说了 release，或没人再持有）的行，过了窗口就离开；
    - **被裁剪 / 隐藏的内容不会丢行**：它仍是条目的当前答案，所以也仍是上传声明的来源（F27 / F28）。
    条目本身在“宿主不要了”与“没有行还答得出”同时成立时离开（`ContentStore` 的 `retireOwner`：标记与丢弃一遍完成）；
    材质 arena 槽不看这个条目，只看**行**：帧首问声明索引，最后一行离开后就放槽。
    `markUsed` / 释放 token / park / `kept` 都不再存在（`ResidencyTable` 的规则落到行上，见 ADR-2026-11-11 第 3 段）。

## 3. 功能点

### 3.1 显式释放

| # | 功能 | 说明 |
|---|---|---|
| F1 | `releaseContent(...)` | 释放 Geometry / Material（asset 或 `MaterialInstance`）/ ShaderProgram / Texture，以及单条流的 `Buffer<float>` / `Buffer<uint32_t>` 的 GPU 驻留（buffer 按地址命名，读它的 geometry 不动）。 |
| F2 | 释放标记 | 释放只是标记；行按 epoch 回收（2.10）。下次使用（`track` / `ensure*`）清掉标记，所以不误删。 |
| F3 | 纹理立即删 | 纹理 cache 立即删条目；行按 epoch 回收，材质的 arena 槽按行（pin）回收。 |
| F4 | 重建 | 下一次使用重建驻留；不是泄漏。 |
| F5 | 弃用扫尾 | 宿主丢掉引用后由 `ContentStore::releaseAbandoned` 标记（帧内一次，录完内容、提交之前），行与条目按 epoch 回收；上传、纹理与材质块由帧自己的声明扫描离场。 |

### 3.2 缓存

| # | 功能 | 说明 |
|---|---|---|
| F6 | 纹理缓存 | `MaterialImages` 条目由**活内容**钉住：材质行的 slot 声明（`ContentDeclarations::declaresTexture`）+ 该帧各 pass 导入的贴图，另加条目自己的 `revision()`；无 LRU / 上限。 |
| F7 | 材质 arena | 每槽 `copies` 份（对应在飞帧）；槽满 `Refused`，不淘汰活材质；槽的寿命是 pin（活材质行声明这个身份），帧首 `retireUnpinned` 放槽。 |
| F8 | 按需扩容 | 记录前按 `ContentStore::materialCount()` 换更大 storage；无硬上限。 |
| F9 | 流共享 | `StreamUploads` 按 `StreamKey` 共享；稳态帧 0 上传；条目由活几何行钉住（`ResourceTable`）。 |
| F10 | 每帧块 | `BlockStorage` 每帧 ring，slab 数 = 在飞帧 + 1；稳态帧不写。 |
| F11 | 对齐 | 块偏移必须是设备 `minUniformBufferOffsetAlignment` 的倍数。 |
| F12 | 预算 | oversized / overflow / material refusal 在消耗预算前拒绝并计数。 |

### 3.3 观测

| # | 功能 | 说明 |
|---|---|---|
| F13 | 计数器 | `writes` / `bytesWritten` / `materialWrites` / `uploads` / `aliases` / `refusals` / `retired`。 |
| F14 | 诊断 episode | 被拒绝的块 / 材质各报一次；缓存压力不再发 `ResourcePressure`。 |
| F15 | 释放后可观测 | 释放 → 下一帧重建；用例断言驻留消失再出现。 |

### 3.4 上传暂存

| # | 功能 | 说明 |
|---|---|---|
| F16 | 不保留 | 一次上传的 host 可见缓冲在拷贝完成后就被释放：`staging` 与 `buffer_data` 置空（vsg 自己“不够大就重建”时处理的状态），下一次上传到那个块时重新分配。 |
| F17 | 证据是那块自己的 fence | `block->fence->status() == VK_SUCCESS` 就是“读过它的拷贝已完成”。不用时间线窗口、不用退役队列、不 `deviceWaitIdle`（见 N8）。 |
| F18 | 触发点是帧边界 | `VsgBackend::executeFrame()` 最开头跑一次 `releaseFinishedUploadStaging()`，逐个会话问自己的上传任务。宿主不调、不需要知道（F18 曾要求宿主在生命周期点调——已撤销，见 ADR-2026-11-10-upload-staging-is-not-retained）。 |
| F19 | 帧边界不许阻塞 | fence 没完成的块留给下一帧；`DataToCopy` 里还有待拷数据的块也跳过（下一次上传会重新分配，早清只白亏一次分配）。 |
| F20 | 每块独立 | `TransferTask` 有 `2 × numBuffers` 个块（early/late × 3），上传按 `frameIndex` 轮转，所以两块都可能压着一个模型（实测 704 MB 车身 → `SharedUsage` 1,228 MB = `2 × 614 MB`）；清也按块清。 |
| F21 | 可观测 | `VsgBackend::uploadStagingBytes()` / `api::OffscreenSession::stagedBytes()` 报当前占用；稳态恒 0（待释放时才是上传那一刻到下一个帧边界之间）。 |
| F22 | 由派生任务回答 | “手里有没有块”由会话自己的上传任务回答：它是 `detail::StagingTask`（派生自 `vsg::TransferTask`，能看能清 `staging` / `buffer_data` / `fence`），会话起来时由 `installStagingTask()` 装好。不用设备级上传计数（那是设备的计量，不是被问的会话的）。 |

### 3.5 “不再被画”的六种情形

| # | 情形 | 行（表） | 上传（stream） | 半片 / 描述符集 | 材质槽 | 触发处 |
|---|---|---|---|---|---|---|
| F23 | **材质离树** | 宿主丢引用后下个帧边界标记；行过了 epoch 窗口才走 | — | 无（set 键不含材质） | 行走后由帧首的 pin 扫描 `BlockStorage::retireUnpinnedMaterials` 放槽 | `ContentStore::releaseAbandoned` / `releaseContent` |
| F24 | **program 离树** | 同上 | — | 半片在 `answerable` 不再成立时 park（它驱动的 vsg 管线随之走；池里的 variant 只按 FIFO 65 淘汰） | — | 同上 |
| F25 | **pass 脱离** | 后端只有身份：`releasePass` 忘掉 id（永不重发） | — | **不释放**（会话级；随 program/layout 消失才走）；输入集一帧后退回 | — | `RenderEngine::removePass` → `device()->releasePass()` + 拥有目标 `releaseRenderTarget()` |
| F26 | **geom 离树** | 宿主丢引用后扫尾标记；行过了 epoch 窗口 ⇒ pin 消失，上传按自己的窗口离场 | — | 见 F24（layout 不再被需要时） | `releaseAbandoned` |
| F27 | **geom 隐藏**（`setVisible(false)`） | **留着**（宿主仍持有） | **留着**：行还在 ⇒ pin 还在；恢复可见零重传 | 留着 | — | 只影响收集，不触发释放 |
| F28 | **geom 被裁剪**（视锥外，逐视图） | 留着 | 同 F27——裁剪是逐视图的，但它不改变“谁声明了这些字节” | 留着 | — | 同上 |

纹理（`MaterialImages`）在“上传”那一列：它的 pin 同样是声明（材质行的 slot）+ 该帧导入的贴图 + 条目自己的
revision，所以隐藏 / 裁剪的材质不丢贴图，材质行过了 epoch 窗口才让贴图离场（F26 的机制，另一张表）。

## 4. 公开接口

### 4.1 释放

| 接口 | 作用 |
|---|---|
| `RenderDevice::releaseContent(raw_ptr<const Object>)` | 释放 GPU 驻留 |
| `RenderEngine::releaseContent(...)` | 引擎转发 |

上传暂存**没有入口**：它在帧边界自己释放（F16–F19）；后端侧的现面是 `VsgBackend::uploadStagingBytes()` 与
`api::OffscreenSession::stagedBytes()`（只读观测）。

### 4.2 缓存与存储

| 接口 | 作用 |
|---|---|
| `MaterialImages::acquire()` / `releaseTexture()` / `beginFrame()` / `released()` / `bytes()` | 纹理 cache（寿命读 `ContentDeclarations::declaresTexture` + 该帧导入的贴图 + 条目自己的 revision） |
| `core::MaterialArena::write()` / `capacityNeed()` / `live()` | 材质块 |
| `BlockStorage::beginFrame()` / `writeView()` / `writeDraw()` / `writeMaterial()` / `retireUnpinnedMaterials()` | 每帧块 |
| `BlockStorage::strides()` / `regions()` / `bytes()` | 布局与观测 |
| `StreamUploads::acquireVertex()` / `acquireIndex()` / `beginFrame()` / `retired()` / `uploads()` | 流上传（条目由 `core::ResourceTable` 持有；寿命读 `ContentFacts::declarations` + 条目自己的 revision） |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 内容对象归宿主，GPU 驻留归后端；释放走显式入口。 |
| C2 | 缓存不设容量上限；宿主用多少拿多少。 |
| C3 | 材质槽满时拒绝，不淘汰活材质。 |
| C4 | 上传暂存不是驻留也不是缓存：回收它不改任何宿主对象、不改 revision，下一次上传自己重新分配。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | `releaseContent()` 释放 GPU 驻留；不替宿主决定寿命。 |
| R2 | 不 idle 设备；释放只是标记，行的回收按 epoch（2.10）。暂存回收更轻：fence 证据（F17），零等待、零 idle、不进退役队列。 |
| R3 | 不引入全局预算/trim/收缩。 |
| R4 | 暂存回收不需要宿主参与：它不是一条策略，而是“用完就放”（F18）。宿主侧不再有调用点，也没有开关。 |

### 5.3 不变量（N#）

| # | 注意项 |
|---|---|
| N1 | 不要为了“省内存”重新引入 LRU / 字节上限；方向已裁决为显式释放。 |
| N2 | 槽立即复用安全：每槽 `copies` 份，新材质写当前 copy。 |
| N3 | 拒绝不淘汰活材质：同帧复用已记录的槽可能画错。 |
| N4 | 释放路径不 idle 设备；破坏路径才用计数 idle。暂存回收就是这类破坏路径（见 R2 的例外）。 |
| N5 | 块存储增长只加 chunk，不搬旧块（旧 span 必须保持有效）。 |
| N6 | 对齐用设备限制，不要硬写 16 / 256。 |
| N7 | 没有计时，也没有“安静几帧才收”：判据是那块自己的 fence（F17），帧边界只是“什么时候去看一眼”。 |
| N8 | 销毁暂存的证据不是时间线窗口，也不是设备空闲：时间线要帧（宿主停下来就永远不到期），`deviceWaitIdle` 停的是整个设备。 |
| N9 | 暂存回收**不做内部同步**：它读写的是帧路径在读的状态（任务的块、块的 `staging` / `buffer_data` / `fence`）。它只在帧边界跑（一帧一次、一个线程）；另一条线程上调不可检。 |
| N10 | 清**只清 `staging`**（连同指向它的 `buffer_data`），块本身、fence、command buffer / pool 都留着：删元素不行（`*(frames[frameIndex])` 直接解引用），换整个任务代价大得多。 |
| N11 | 派生任务读的是 vsg 的 protected 成员（`_earlyDataToCopy` / `_lateDataToCopy` / `TransferBlock::staging`）。判据只认“`staging != nullptr` ⇒ 手里有一块 host 内存”，与“这块内存还在”是同一句话；上游改名 = 编译错（响的），上游改语义才是危险的那种，靠 T8 里对字节数的断言兜住。 |
## 6. 注意事项与陷阱

### 6.1 一事实一家

- 会怎么错：同一份状态被两处保存，改一处另一处过期。
- 怎么判：出现两份可写的值/两个可写的注册表。
- 怎么防：回到 §5 的 N#；只留一个 owner，其余只读。

### 6.2 后端/平台的边界

- 会怎么错：把平台差异写进引擎/应用层，换平台时到处分叉。
- 怎么判：出现“如果 Windows/X11 就……”的分支。
- 怎么防：平台事实留在后端，引擎只消费编译好的帧值。

### 6.3 上传暂存：什么时候还

- 会怎么错（A）：拿“时间线窗口”当证据。窗口是帧的，拷贝的完成是设备的事实：一个 park 要 `2 × slots + 1` 帧才到期，
  宿主停下来就永远不到期（实测：大模型加载后 1.2 GB 宿住数分钟，进程 1% 单核 / 0% GPU）。
- 会怎么错（B）：拿“设备空闲”当证据。`deviceWaitIdle()` 停的是整个设备，而正确性只需要“这一次拷贝完了”。
- 会怎么错（C）：把“还有没有东西可还”做成一个需要宿主回答的问题（显式动词）。产品里最常见的宿主（载入后不动）
  根本不会调，于是要再补一条“再问一次 = 现在”—— 一个功能两套语义。
- 会怎么错（D）：用设备级的“上传了多少次”代替“这个会话手里还有没有暂存”。一个会话成功就会盖住另一个的答案。
- 会怎么错（E）：在 sweep 里加一条“这个 DataToCopy 还有待拷数据就先不清”的便宜判断。那种表可以长期非空而毫无待拷
  （`_transferData()` 提前返回、不清理它），于是 sweep 永久跳过，整份暂存又常驻回去。
- 怎么判：`VsgBackend::uploadStagingBytes()` / `stagedBytes()` 在静下来之后不是 0；`RetentionStats::device_waits` 在
  这条线上涨了。
- 怎么防：只认那块自己的 `fence->status()`（F17），在帧边界扫一遗（F18），并且**不加任何额外条件**（E）。

### 6.4 隐藏 / 裁剪**不**丢上传（离树才丢）

- 会怎么错：把“这一帧没画”当成“对象没了”，于是要么在离树路径上补一套释放（裁剪抖动 ⇒ 每帧重传），
  要么反过来以为隐藏的内容会忘掉上传，再给它加一条时间窗。
- 会怎么判：`VsgBackend::releasedStreams()` 只在**行消失之后**增长；同一片几何“看不见→又看见”后
  `StreamUploads::uploads()` **不变**。行数（`ContentStore` 的活对象）也不变。
- 怎么防：寿命只问一条——“还有活几何行声明这些字节吗？”（pin = buffer 地址 + 它此刻的 revision，
  由 `ContentStore::tablesFor` 发布的几何行枚举）。要提前还驻留就用显式 `releaseContent()`，
  不要再加时间窗 / 容量上限（`G8`：那是替宿主猜）。

### 6.5 换实例流是**描述**变更，几何要自己报 revision

- 会怎么错：宿主给某个 primitive 换了实例切片（`Geometry::setInstanceTransforms`），
  却没接着报 `Geometry::bumpRevision()`。那个 setter 只推进内容纪元（`Node::invalidateBounds`），
  **不动** `revision()`；而后端的几何行只在 revision 移动时重建（`ContentStore.cpp` 的
  `findGeometry(..., contentRefOf(identity, live.shape->revision()))`）。于是计划每帧从 primitive 现读新 buffer、
  事实行声明的仍是旧 buffer，上传条目**每帧建-退**：画面对，代价静默。
- 会怎么判：什么都没改、只是每帧重画的稳态里，`StreamUploads::uploads()` 与 `retired()` **同时**按帧上涨，
  而 `live()` 不涨（这三条一起看才排得掉“真的每帧换内容”）；被替换的那块 buffer 从不出现在声明索引里。
- 怎么防：换切片属于“这份描述变了”，改完立刻 `bumpRevision()`。隐藏/裁剪**不**触发这条
  （行还在 ⇒ 声明还在，见 6.4），所以它只在**真的换了 buffer** 时才有后果——
  “裁剪 100 帧零重传”那条用例钉不住它，钉住它的是 T10（报告前只声明旧流、报告后两代都声明、旧行离场后只剩新流）。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | LRU / 容量上限 / 压力淘汰 | 300 张同序循环下 LRU 与 FIFO 一样差；上限是替宿主做决定。详见 [ADR](../decisions/ADR-2026-10-22-cache-unbounded-explicit-release.md)。 |
| D2 | 材质 FIFO 淘汰 | 淘汰活材质会画错；拒绝只兜底。 |
| D3 | 释放时 idle 设备 | 破坏路径才需要；释放只是标记，行的回收按 epoch。 |
| D4 | 全局预算 / trim / 收缩 | 没有消费者；宿主显式管理。 |
| D5 | 旧帧读回接缝 | F−N 是观测缺口，不是生产竞态；不做。 |
| D6 | 把回收做成显式入口（宿主或用户命令调） | 最常见的宿主（载入完不动）不会调；park 到期又要 `2 × slots + 1` 帧，于是要再补一条“再问一次 = 一次计数 idle”。详见 [ADR](../decisions/ADR-2026-11-10-upload-staging-is-not-retained.md)。 |
| D7 | 用时间线 park（或 `deviceWaitIdle`）当证据 | 证据是那块自己的 fence，比窗口强也比设备空闲便宜：零等待、零 idle。 |
| D8 | 引擎每帧“好心”调一次回收 | 引擎不该有资源政策，而且它看不到“这次拷贝完了没有”。帧边界上由后端自己看 fence 才是同一件事的正确位置。 |
| D9 | 环境变量当开关 | 它等价于“用完就放”，不是可选政策；开关只会多一条要解释的线。 |
| D10 | 给 `FrameCounters` 加每帧上传数，让引擎据此触发 | 没人读的 SDK 字段（`G8`）；每帧上传数作为观测指标进 backlog（T25）。 |
| D11 | 给引擎加一个转发入口 | 引擎已经有 `device()`；多一个只做一跳的入口等于同一件事两个名字（`G4`）。 |
| D12 | 只登记一个“待清理”标记，真正清理留到下一帧 | 登记不等于还了；帧边界就是那个“下一帧”，不需要一个额外状态。 |
| D13 | 清块之前先问一句“这个 DataToCopy 还有待拷数据吗” | 看起来便宜，实际上那种表可以长期非空而无任何待拷，于是 sweep 永久跳过（见 6.3 E）。 |
| D14 | 用设备级上传计数（`uploads()` / `count()`）判断“手里有没有块” | 计数答的是设备，不是被问的会话（见 6.3 D）。改成问会话自己的任务（F22）。 |
| D15 | 换掉整个 `TransferTask` 来释放 | 会把每块的 command pool / command buffer / fence 一起丢掉，还要把三处交接再正确一遍（`vsg-pitfalls.md` P42.1）；清 `staging` 只丢那块缓冲。 |
| D16 | 只清比 `minimumStagingBufferSize` 大的块（留可复用的小块） | 能让“每帧都上传”的宿主少一次分配，但把“用完就放”变成“够大才放”；当前仓库里没这种宿主，触发条件在 backlog。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `ContentStoreTest.*Release*` / `MaterialImagesTest.Release*`。 |
| T2 | `VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt`。 |
| T3 | `HeadlessBackendTest.TheEngineForwardsAContentReleaseToItsDevice`。 |
| T4 | `BlockStorageTest.*` / `MaterialArenaTest.*` / `StreamUploadsTest.*`。 |
| T5 | `UploadScratchTest.TheUploadScratchIsNotRetainedAndTheFramesGiveItBack`：上传后帧不涨 idle；上传的暂存**没人调**就被后续帧清掉（数归零，且只需几帧）；同一内容重画零上传；新流再上传、再由后续帧归零。 |
| T6 | `OffscreenSessionTest.AReplacedTransferTaskLeavesTheSessionSubmitting`：会话起来就装好了可问的任务（`stagedBytes()` 有值且为 0），出帧不涨 idle，之后仍能提交。 |
| T7 | `OffscreenSessionTest.TheUploadScratchIsGivenBackAndALaterUploadStillLands`：机制层（256 MB 工作集回落 + 后续上传逐字节读回；vsg 自建的任务答不出 → 装过之后报 0 → 再上传后报 ≥ 4096 字节）。 |
| T8 | `HeadlessBackendTest`：设备双不再有暂存入口（动词已从 SDK 删除，`RenderDevice` 少一个虚函数）。 |
| T9 | 六种“不再被画”（§2.9 / F23–F28）：`ContentStoreTest.*Release*`（离树与显式释放）、`ContentStoreTest.ACulledGeometryKeepsItsUploadsForAHundredFrames`（裁剪 100 帧零重传）、`ContentStoreTest.AStreamLeavesWithinTheParkingWindowAfterItsGeometryIsDropped`（离树 `slots + 1` 帧内 `live()` 下降）、`StreamUploadsTest.TwoGeometriesOfOneBufferShareOneUploadUntilBothLeave`（共享 buffer 的 pin 合并）、`VsgBackendTest.ReleasingContentDropsItsResidencyAndTheNextFrameRebuildsIt`（释放后重建）、`VsgBackendTest` 的 pass 身份用例（`releasePass` 只忘 id）。隐藏 / 裁剪的“行不变、上传也留着”由 `StreamUploadsTest` 与 `ContentStoreTest` 各钉一半，不合并成一条——那是两件不同的事实。 |
| T10 | 实例流也是**声明**（§2.5 / §6.5）：`ContentStoreTest.AnInstanceStreamIsDeclaredThroughItsPrimitiveAtTheRowsRevision` —— 行带着 primitive 的实例句柄并据此声明它；换了切片但**没**报新描述时只声明旧流；`bumpRevision()` 后活行声明新流、停车中的旧行仍声明旧流；旧行离场后只剩新流。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。F16–F22（上传暂存）已落地：`gfx_backend_vsg` 在每个帧边界扫一遍
“fence 已完成”的块并释放其 `staging`（零等待、零 idle），SDK 侧**没有**对应动词，模型查看器也不需要调任何东西
（`release_render_memory` 命令随之删除）。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

旧名/旧路径只在 §7 已否决选项或 git 历史里出现；新代码按 §4 的接口写。

## 10. 参考

`graphics-architecture.md` · `graphics-geometry.md` · `vsg-backend.md` · `../backlog/graphics.md`
