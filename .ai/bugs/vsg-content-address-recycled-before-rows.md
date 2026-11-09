# 内容对象在"行还能被读到"时就被释放：换一次着色模式后整条 pass 不再被画

- **状态**：已修复（2026-11-09）。
- **发现日期**：2026-11-09
- **模块**：`src/plugins/gfx_backend_vsg`
- **涉及**：`ContentStore::releaseAbandoned()`、`ContentStore::releaseGeometry/releaseProgram/releaseMaterial`、
  `ContentHalves::Data::Key` / `find` / `answerable`、`ContentFacts::ProgramFacts`、`ContentPass::record()`

## 现象与影响

模型查看器里切到某一档着色模式（实测为"法线"），画面**冻结在切换前的样子**：之后拖鼠标每一帧都只报一条

```
[graphics] the pass' sampled inputs is not drawn: no compiled content half was built for this pass
```

命令本身成功（`CommandManager` 记 `Command succeeded: set_shading_normals`），换档那一帧也正常——从**下一帧**开始，
整条 pass（连清屏一起）被拒绝，所以画面再也不更新。再切到别的档又"好了"。看起来是"有概率切过去没反应"。

## 根因

`ContentStore` 的三张事实表按**对象地址**做键（`ProgramFacts::program` / `GeometryFacts::geometry` /
`MaterialFacts::material`），行删得比对象晚：删除被 `RetirementQueue` 停靠（in-flight 帧可能还命名着这些行）。
`releaseAbandoned()` 却在停靠成功之后**立刻丢掉 store 的那份对象引用**：

```cpp
const bool parked = retirement.retire(timeline, [weak, identity]() { ... eraseProgramOf(identity); });
if (!parked) { ++d->retained; }
it = d->programs.erase(it);      // ← 对象在这里就没了，地址立即可被复用
```

于是地址在"行还在表里"的整个停靠窗口内就可被分配器复用。一次换档正好是"造一个新程序对象 + 丢一个旧程序对象"，
新对象很容易拿到刚释放的同一个地址（同类大小、刚 free）。接下来：

1. 新程序在同一地址 `track()` → 新的 `LiveProgram`（`described = false`）→ `ensureProgram()` 把**新文本**的行
   **追加**到同一个键 `(地址, revision, 变体, kind)` 上；
2. 旧对象的停靠到期 → `eraseProgramOf(地址)` 按**身份**擦除——把新对象刚描述的行一起擦掉；
3. 新对象的 `LiveProgram` 仍记着 `described && described_revision == revision && 变体已建`，`ensureProgram()`
   于是永远早退，**行再也不会被重建**；
4. `halvesFor()` 的 `findProgram()` 永远 miss → 这一 pass 没有任何内容半边 → `ContentPass::record()` 拒绝整条 pass。

同一类问题还有第二处：`ContentHalves` 的半边键只有 `(地址, revision, layout, 变体, 拓扑, 附件数)`。内置预设程序
每次创建都是**同样的 revision**（两个 stage），所以地址被复用时旧半边的键与新程序完全一致——即使行没被误删，
新程序也会被喂上**上一档**编译出来的流水线（画面停在上一档）。

## 修法

1. **停靠拥有对象**（三张表 + 三条显式 release 路径）：把 store 的那份引用移进停靠回调，行何时删、对象何时亡
   由同一个动作决定；停靠排不上（还没有 in-flight 窗口）时**保留表项**、下一帧再试，绝不提前释放地址。
2. **描述代数**：`ProgramFacts::generation` 由 `ContentStore` 每次追加行时递增，`ContentHalves` 的键与
   `answerable()` 都比对它。地址相同、revision 相同的两个不同程序因此不可能共用同一份编译结果。
3. **报出真正的原因**：pass 级拒绝时除了原来那句，再报一行 `describeMissingContentHalf()` 的结论（几何行/材质行/
   程序行还是"半边构建被拒"）——原来那句话把人指向 sampled inputs，而真实原因是四种里的一种。
4. **同一形态的第三处（`StreamUploads`）**：上传条目也按 `(缓冲区地址, revision)` 做键，新缓冲区一律 revision 0，
   条目还要多活 `slots + 1` 帧——地址被复用就是一个旧条目冒充新流。条目现在**持有**它键名的那个
   `vn::Buffer`（`VertexEntry::owner` / `IndexEntry::owner`），地址与条目同寿。
5. **实例流的那一段没有句柄**：`CompiledCommand::instance_buffer` 原本是**借来的裸指针**，`ContentPass` 拿不到
   句柄，也就无法让上传条目保留它。实例流现在由计划**持有**（`intrusive_ptr<const Buffer<float>>`，收集、编译、
   录制三段都传这个句柄），`instance_revision` 随之删除——revision 从持有的缓冲区现读，与
   `ChannelFacts::streamKey()` 同一拼法。一条规则因此统一：**读者持有它读的数据，键表持有它键名的对象**
   （ADR-2026-11-09-address-keys-hold-the-object）。

## 验证

- `ContentStoreTest.AParkedRemovalKeepsTheObjectsAddressReservedUntilItsRowsLeave`：停靠之后对象**不许**析构，
  新分配的对象拿不到同一地址；停靠到期时对象与行一起消失。
- `ContentStoreTest.AReleaseThatCannotBeParkedYetKeepsItsEntryAndIsRetried`：停靠排不上时条目连对象一起留着、
  行仍可答，`setSlots()` 之后同一条目被重试并停靠，停靠到期才析构。
- `ContentHalvesTest.AnotherDescriptionOfOneIdentityIsAnotherHalf`：同一地址、同一 revision、不同 `generation`
  必须编译出**另一份**半边，旧半边被停靠，之后稳定不再重建。
- `StreamUploadsTest.AnEntryHoldsTheBufferItsKeyNames`：条目持有缓冲区（`useCount == 2`），新缓冲区拿不到同一
  地址，条目被淘汰后引用计数回落到 1。
- `FrameCompilerTest.ThePlanHoldsTheInstanceStreamItReads`：计划持有实例流，宿主放掉自己的句柄后字节仍可读。
- `ContentPassTest`（实例化绘制那条）：录制之后 `instance_storage->useCount() > 1`，上传条目确实持有实例流。
- `ContentSweepTest.EverythingTheHostDroppedIsLetGoInOneCallAndItsRowsLeaveAtThePark`：材质对象随停靠多活一个窗口，
  贴图由**下一次** sweep 释放（生命周期变了，断言随之更新）。
- `test_vsg` 全量 + `test_gui` 全量。

## 仍在的同类风险（未在本轮修）

- **程序行构建失败每帧重试**：几何/材质构建失败会记下"尝试过的 revision"（`described_revision = revision`），
  失败不再重试；程序则只记 `described`，某个变体构建失败（例如文本在 `#error` 分支里、或者 ABI 扫不出来）
  会**每帧**重新复制两份着色器文本并 `scanProgramAbi` 一遍，直到 revision 变动。`ShaderProgram` 的任何改动都会
  bump revision，所以这个重试除了浪费没有别的用途——属于成本问题，不是正确性问题，且从外部不可观察（`builds()`
  只数成功的追加），所以没有为它改代码，只记在这里。

## 设计约束

- **G10**：地址是这张缓存的键，"对象活多久"就是正确性的一部分，不是内存优化；停靠窗口内释放地址是把正确性押给
  分配器。
- **G11**：故障现场只有"没有内容半边"这一句，指向的是错的地方；现在失败会说自己缺的是哪张表。
- **G4**：`releaseAbandoned` 的 `retained` 原先只计数、不保留对象，名字说的和做的不一致。
