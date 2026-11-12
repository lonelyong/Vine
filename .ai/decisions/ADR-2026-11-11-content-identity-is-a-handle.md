# ADR-2026-11-11：内容身份是一个句柄，不是一个地址

- 状态：已采纳
- 日期：2026-11-11
- 相关：`../research/graphics-2026-11-11-next-design.md`（§3.1 身份、§3.3 `ContentRef`）、
  `../research/graphics-2026-11-11-next-audit.md`（A1 / A12 / 步骤 3a–3b）、
  `ADR-2026-11-09-address-keys-hold-the-object.md`（本篇部分取代它）

## 背景

`ADR-2026-11-09` 当时否决了“稳定身份”，选择“按地址做键的缓存必须持有它键名所指的对象”，并写明重开条件：
出现第三处违反该约定的漏洞、出现一张够不到句柄的表、或需要跨会话/跨进程认同一份内容。今天三条里至少两条成立：

- **重放/转储需要稳定键**：next 设计的快照要能被 dump、replay，并按 `(id, revision)` 认出同一份内容；地址做不到。
- **地址复用已经有一层补丁**：`ProgramFacts::generation` 就是“同地址不同身份”的保险，说明“地址即身份”已经不成立，
  只是补在一个表里。
- **后端仍有一张按身份单键的材质表**：`ContentStore` 的材质查找按地址单键、就地替换，于是旧计划命中的是新值 ——
  audit `A1`（`R9`/`I1`）在后端那一层的残余。

同时 next 设计（§3.1/§3.3）已经把公开身份定为 `Handle`（gen24/domain8/index32）+ `ContentRef{id, revision}`，
`ColdChunk` 的每一行都按它命名。设计已拍板，剩下的只是落地顺序。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 保持地址身份，把 revision 显式化、按 epoch 回收 | 改动最小；只碰后端表 | 拿不到“陈旧句柄答 gone”；重放/转储仍需另造一套键；`generation` 那类补丁要留在每个表里（`G6`） |
| B. 身份改为 `Handle` + `ContentRef`，公开类型一起改 | 陈旧身份可判定、地址复用整类消失；重放/转储有稳定键；与已拍板设计一致 | 公开 API/ABI 变更：`RenderCommand` 字段、行身份、镜像键、计划行、后端表都要改；需清对象重建 |
| C. 只在后端表内部换成句柄，公开类型不动 | 表面改动小 | 句柄得从某处来 —— 要么每个调用点把地址映射成句柄（多一层表 + 一次查表，`G7`），要么接受“两层身份”（`G1`） |

## 决定

选 **B**，并分三段落地，每段自身可编译、可测：

1. **公开身份**（已完成）：新增 `sdk/vine/graphics/ContentId.hpp` —— `Handle`、typed `Id<Tag>`、
   `NodeId`/`GeometryId`/`MaterialId`/`ProgramId`/`TextureId`/`InstanceId`、`ContentRef`、`contentRefOf()`、
   `ContentRegistry`（含 `instance()` 进程默认域、24 位回绕退休）、`std::hash` 特化。
   `Node` 构造分配 `NodeId`、析构释放，`Node::id()`；行身份链全面改为 `NodeId`：`Row::identity`、
   `RenderWorld::RowId`、`DeltaChannel::Delta::identity`、镜像 delta、`SceneSnapshot::identities`；
   公开的 `RenderCommand::row`（`const void*`）改为 `RenderCommand::node`（`NodeId`），
   `CollectedCommand`/`CompiledCommand`、`GeometryFacts`、`ContentStore` 的几何行表与 live 表随之按 `NodeId` 键。
   手写命令没有 node 时以 `geometry->id()` 命名 —— 一个**具名转换**，不是第二真相。
2. **`ContentRef` 成为唯一键**（材质与程序已完成，几何合并未做）：`MaterialBase` 已持自己的 typed id，材质表已由
   地址单键改为 `(MaterialId, revision)`（消掉 `A1` 残余）；`ShaderProgram` 也已持 `ProgramId`，计划与记录里的
   `program` 是 `ContentRef`（`ProgramRef` 类型删除），程序表按 `(ProgramId, revision, variant, kind)` 建行、
   `ProgramFacts::generation` 随之删除；几何行的 `geometry` + `geometry_revision` 合并仍在待办。
3. **寿命交给 epoch**（未做）：`ContentStore` 的 `markUsed` / parking / `kept` 换成 `ResidencyTable`
   （`last_epoch < oldest_live` 即可回收，无时间窗、无 kept 回退）。

**不做 A**：设计已把身份定为句柄，A 会让设计文档与代码长期不一致，且重放/转储仍要另造键。
**不做 C**：地址→句柄的映射表正是 `G7` 说的“把复杂性往下推一层”，并留下两层身份。
**不一次做完**：中间态会在同一个 `ContentStore` 里同时存在两套寿命机制，丢掉已验证的证据。

## 后果

- 好的：陈旧行/陈旧计划能答“gone”，地址复用那一类错画面无法再发生；重放与转储有了稳定键
  （`(id, revision)`）；`ProgramFacts::generation` 这类补丁的身份来源收敛到一个概念（`G1`/`G6`）。
- 坏的 / 代价：公开类型变更（`RenderCommand::row` → `node`、`Row::identity`/`RowId` 变成 `NodeId`、计划里的
  `program` 从 `(地址, revision)` 变成 `ContentRef`），生产者与消费者一起改；`Node` 与 `ShaderProgram` 布局各多
  8 字节，构造/析构各一次注册表加锁（每次内容对象一次，不进帧循环）；
  改了 `Node`/`RenderCommand`/`ShaderProgram` 布局后必须清掉旧对象再编，否则会读到上个布局的对象（本次实测过 `malloc(): invalid size`）。
- 代价（落地时发现）：`ContentRef::revision` 必须是 `std::uint64_t` —— 设计稿写的是 `std::uint32_t`，而 SDK 的每个
  `revision()` 都是 64 位，窄键会在四十亿次编辑后回绕；设计稿片段已同步。
- 触发条件（什么时候重开）：若某张表**够不到** `Handle`（例如只有裸缓冲区、没有内容对象），那张表先按
  `ADR-2026-11-09` 的“持有即身份”处理，并把“够不到句柄”作为重开信号写回本篇；若句柄的 domain 需要跨进程，
  再单独决定序列化格式。

## 证据

- `nodeId::` 分配/释放语义：`test_graphics` 的 `ContentRegistryTest`（5 条）、`ResidencyTableTest`（1 条）。
- 行身份迁移：`test_graphics` 559 条、`test_vsg` 538 条、`ctest` 30/30 全绿。
- 计划/记录携带 `NodeId`：`FrameRecorderTest.EveryFactOfACommandReachesTheRecord`、
  程序身份：`ShaderProgram::id()`（ctor 分配 / dtor 释放）；`ShaderProgram::id()` 的同一文本、同一 revision 的两个
  对象得到两条半边（`ContentHalvesTest.AnotherProgramIdentityIsAnotherHalf`）；程序表按 `(ProgramId, revision)` 建行
  （`ContentStoreTest` 的 program 断言）；被拒绝的程序仍能报出名字（`ContentStoreTest.AProgramThatCannotBeDescribedStillHasItsName`）；
  `ContentSourcesTest` 断言事实行带的是程序的 `ContentRef`。
- `FrameCompilerTest` 的计划断言；手写命令回退到 `geometry->id()`：`GeometryFactsTest`。
- 性能未退化：`SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect` 在 RelWithDebInfo 下 175 µs publish /
  282 µs select（N2 ≤250 µs、K1 ≤500 µs 仍通过）。
- 门禁：`check_ai_docs --strict`、`check_design_docs`、`check_doc_symbols`、`check_include_hygiene`、
  `check_export_annotations`、`check_api_style`、`check_public_docs` 全清。
