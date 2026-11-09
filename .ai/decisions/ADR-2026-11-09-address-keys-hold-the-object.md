# ADR-2026-11-09：按地址做键的缓存，必须持有它键名所指的对象

- 状态：已采纳
- 日期：2026-11-09
- 相关：`../design/graphics-content-lifecycle.md`（2.5 / 2.6）、`../bugs/vsg-content-address-recycled-before-rows.md`、
  backlog `graphics.md` T19（本条决定落地时已关闭，条目已删除，证据留在下面）

## 背景

后端的每一张缓存表都用宿主对象的**地址**做键：事实表（`ContentStore` 的几何/程序/材质行）、编译半边
（`ContentHalves::Data::Key`）、流上传（`StreamKey` 的 `buffer`、`DataKey` 的 `buffer`）、管线变体
（`PipelineKey` 的 `program`）。理由是计划里只有指针（`CompiledCommand` 是 `const void*`），转换到地址是零成本的。

但条目比对象活得久：事实行停靠到 in-flight 帧结束，流条目多活 `slots + 1` 帧，管线变体活到被挤出或会话结束。
一次换档正好"造一个新程序 + 丢一个旧程序"，同尺寸的新对象很容易拿到刚释放的地址，而内置预设每次创建的
revision 也相同——于是旧条目冒充新身份。实测过三种后果：

1. `ContentStore` 在停靠成功后就丢掉对象引用 → 新程序在同一地址追加的行被旧身份的停靠擦除 → `LiveProgram.described`
   仍在 → 行永不重建 → 整条 pass（含清屏）永久被拒。
2. `ContentHalves` 的键只有 (地址, revision, …) → 地址复用后新程序被喂上一档编译出的流水线（画面停在上一档）。
3. `StreamUploads` 的条目只有键、不持有缓冲区 → 新缓冲区（revision 0）被喂上另一个几何写进去的字节
   （历史上表现为 `PrimitiveDrawTest` 的 strip/fan 像素用例时好时坏，见 backlog T19）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. **持有即身份**：条目（或其停靠）持有它键名所指的对象，地址与条目同寿 | 局部、零新概念、与既有的 park/停靠机制同向；键仍是指针，转换免费 | 是一条**约定**：新表作者必须记得持有，忘了就是静默错画面 |
| B. **稳定身份**：给可缓存对象发永不重复的 id（`Object`/`RefCounted` 上各一个计数器），所有键改用 id | 不可能悬空；`generation` 等补丁可以删掉 | 要改 `StreamKey`/`DataKey`/`PipelineKey`/`ProgramRef`/`CompiledCommand` 等公开键词汇与计划，30+ 文件；`Buffer` 不在 `Object` 体系里，还要再加一个身份来源 |
| C. **给每个键补一个 generation**：谁复用地址就给谁加一个计数器 | 改动小 | 一个概念要写四遍（事实行、半边键、流键、管线键），是把 A 的约定换成四处补丁；`G6` 明确反对同一事实两种拼法 |

## 决定

选 **A**，并把"约定"变成可检查的规则，而不是靠记性：

- **数据是被读的，所以读者持有它**：计划持有的实例流从裸指针改成 `intrusive_ptr<const Buffer<float>>`
  （`CollectedCommand` / `CompiledCommand`）。它同时钉住地址——流条目的键正是这个地址。
- **键要能查多久，就持有多久**：事实表的停靠回调持有对象、流条目持有缓冲区（`StreamUploads::VertexEntry::owner` /
  `IndexEntry::owner`）；停靠排不上时**保留条目**、下一帧再试，绝不提前把地址还给分配器。
- **同地址不等于同身份**：事实行带 `ProgramFacts::generation`，半边键也带它——A 成立时它永远不该不同，所以它是
  "不把正确性押在另一个模块的纪律上"的保险（`G10`），不是第二套身份。
- 规则写进 `.ai/design/graphics-content-lifecycle.md` 2.6，每个按地址做键的表都有对应用例（见证据）。

**不做 B**：目前没有任何消费者需要跨对象的全局 id；A 的三处违反都已修好并被用例钉住；把键词汇整体换成 id 是
30+ 文件的重构，换来的是"忘写持有也不会错"这一层保险，而"忘写"现在会被同一套用例抓住。
**不做 C**：它正是 B 的碎片版本，会把"一个身份概念"变成四份计数器。

## 后果

- 好：键词汇保持不变（指针仍是身份载体），计划/事实/流三层的生命周期只有一条规则；实例流这一处不再需要
  "计划只借不持"的例外。
- 代价：任何一个按地址做键的新表都必须持有对象——这是纪律，靠设计文档 2.6 + 每个表一条用例维持。
- 代价：`CompiledCommand::instance_revision` 删除（revision 从持有的缓冲区现读，与 `ChannelFacts::streamKey()`
  同一拼法），计划不再快照一个可以廉价现取的数。
- 重开条件（满足任一条就改做 B）：出现**第三处**违反该约定的漏洞；出现一张**够不到句柄**的表（本次的实例流正是
  差点如此——它靠给计划加持有才够到）；或需要跨会话/跨进程认同一份内容。

## 证据

- `FrameCompilerTest.ThePlanHoldsTheInstanceStreamItReads`：计划持有实例流，宿主放掉自己的句柄后字节仍可读。
- `ContentPassTest`（实例化绘制那条）：录制之后 `instance_storage->useCount() > 1`，条目确实持有缓冲区。
- `StreamUploadsTest.AnEntryHoldsTheBufferItsKeyNames`、`ContentStoreTest.AParkedRemovalKeepsTheObjectsAddressReservedUntilItsRowsLeave`、
  `ContentStoreTest.AReleaseThatCannotBeParkedYetKeepsItsEntryAndIsRetried`、
  `ContentHalvesTest.AnotherDescriptionOfOneIdentityIsAnotherHalf`。
- `PrimitiveDrawTest.AStripAndAFanCoverTheSameQuadAListDoes`（原 backlog T19，已关闭）在流条目开始持有缓冲区后
  连跑 10 次全绿。
- `test_vsg` 全量、`test_gui` / `test_graphics` 全量。
