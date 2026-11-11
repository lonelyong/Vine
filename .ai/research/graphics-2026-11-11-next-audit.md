# graphics-next：第二轮审计 —— 证据、身份与可证伪性

> **状态**：调研审计（**非现行设计**；未落地、未决策）
> **日期**：2026-11-11
> **审计对象**：`graphics-2026-11-11-next*.md` 六份提案（总纲 / requirements / design / benefits / api / hot-paths）
> **方法**：逐条对照当前代码；重跑 `Measure*` 与测试计数；用反例检查每条“骨架保证”是否真的成立；不重复 API 审计的 `P1–P32`
> **结论**：诊断（两个可变世界、寿命机器）与三条主干（不可变值、单写者、计划是值）成立；
> 但 `A1`（内容引用只带 id）会让“在飞帧不变”退化，`A2–A4`（拾取身份、共享原型写者、原型寿命）在共享/动画场景下不成立，
> `A5–A8` 让收益数字偏乐观。**建议先修文档与补判死条件，再进原型**；本文件不改变提案的“未采纳”状态。

---

## 1. 审计结论摘要

| 判断 | 结论 | 依据 |
|---|---|---|
| 两个可变世界之间的桥是复杂度来源 | **成立** | 桥 2519 行（头 + cpp，口径见 §2）；`ContentStore` 1016 行、寿命 token 约 72–75 处（口径见 §2） |
| 寿命问题比数据竞争更真实 | **成立** | TSan 首轮 14 条警告全是寿命；当前 `ContentStore` 明确为旧 revision 保留行直到帧走完（`ContentStore.cpp` `ensureGeometry` / `ensureMaterial`） |
| 跨层应只有不可变值 | **成立，但 A1 必须先修** | 当前 `RenderCommand` 已经携带 `GeometryDescriptionPtr` / `MaterialDescriptionPtr` 过帧；新设计把它们换成了裸 id，方向反了 |
| 计划应是值 | **成立，但 A11 的接口缺口要补** | 7 个会话方法里没有 M2/R23 的预热查询；多表面与 `Target` 没对齐 |
| 原型实例化（R3/T4）能“只付 10 次矩阵乘” | **不成立（按当前写法）** | `rows(instance) = Placement ∘ PrototypeLocalRows` 仍是每行一次乘法；5000×10 是 5 万次，且内存按实例倍增（A5） |
| `DrawItem ≤ 64 B`（N15） | **不成立** | 按 §3.5 字段静态估算已 >64 B；`sort_key` 还是 32 位 vs 64 位公式（A6） |
| 发布是 O(变更) | **有条件成立** | 块数组是；`prototypes_` / `instances_` / `registry_` 若整体浅拷贝就是 O(原型数 + 实例数 + 素材数)（A7） |
| 块级裁剪用 `chain[0]` | **不成立** | 一个 32 行块可以跨不同父链；`local_box_union` 字段在 `ColdChunk` 里也不存在（A8） |

---

## 2. 证据复核（先统一口径）

| 提案中的数 | 复核结果 | 口径 / 备注 |
|---|---|---|
| 桥接 2519 行 | **2519（提案基线）；2702（当前，原型新增 `selectRows` / `fillOwnFacts` / `refreshRowFacts`）** | 2519 是 8 个文件的 **.hpp + .cpp** 之和：`ContentMirror 326`（170+156）、`DeltaChannel 263`（190+73）、`DrawListMemo 226`（127+99）、`FrameWorker 316`（162+154）、`FrameRunner 265`（142+123）、`SceneRows 121`（69+52）、`EngineFrame 72`（仅头）、`DrawListBake 930`（375+555）。只算 cpp 是 **1212** 行；`K5` 必须与 2519 同一口径，原型新增行不算进基线 |
| `ContentStore.cpp` 1016 行 | **1016** | ✅ |
| 寿命机器 72 处 | **75**（六个 token：`markUsed` / `release_token` / `release_requested` / `retired` / `kept` / `retirement`，grep 含注释）；把 `parked` 算进去是 **100** | “72”不是可复现的计数；建议在文档里写 token 集合与命令，或降级为“约 70 处” |
| `RenderEngine.hpp` 1309 行 | **1309** | ✅ |
| 49 个公开方法 | **50**（`scripts/measure_graphics_claims.py`，含构造/析构；主 public 块 44 + 4 个句柄访问器） | 数量成立；口径写进脚本 |
| `RenderEngine.hpp` 被 47 处 include | **22 个文件**（`scripts/measure_graphics_claims.py`） | 47 无法复现；可能来自不同统计口径或后续重构。工程效率一节已改成脚本可数的“文件数 / 传递 TU 数” |
| SDK 75 个头 | **75**（`sdk/vine/graphics/*.hpp` 顶层）；含 `backend/` 子目录是 **99** | 文档写“75 个扁平 SDK 头”是对的，但要带范围 |
| 126 个公开方法返回 `bool` | **110**（顶层 SDK）/ **152**（含 `backend/`） | 126 的 regex 未给出；建议写命令或删数字 |
| `test_graphics` 548 / `test_vsg` 537 / 30 套 | **548（提案时）/ 559（当前：`SnapshotTest` 5 + `ContentRegistryTest`/`ResidencyTableTest` 6）/ 537 / 30** | `--gtest_list_tests` 与 `ctest -N` 实测 ✅ |
| 2000 drawable 相机移动 2160 µs；200 为 207 µs | **2074–2128 µs / 204 µs**（同机多次重跑） | 同量级 ✅；具体数字随机器 ±5%，阈值应写相对值或标明机器 |
| `RenderCommand` 5 个 `intrusive_ptr` | **5**（geometry / description / material / material_description / program） | ✅；这条是 §2.5“每命令 5 次原子增减”的依据 |

**审计方法上的结论**：六份文档里既有可复现的硬数（行数、测试数、µs），也有未写口径的计数（72、47、126）。
后者在评审里会被当成“拍脑袋”，所以已落地 `scripts/measure_graphics_claims.py`（§11），
否则每次改代码后数字都会漂移，而读者无法判断哪条结论还有效。

---

## 3. 跨文档矛盾（先修这些，不需要新设计）

| # | 位置 | 矛盾 | 建议 |
|---|---|---|---|
| X1 | design §3.6 `PassDesc` vs hot-paths §4.2 `Pass` | 同名概念两个类型；`inputs` 一处是 `std::vector<String>`，另一处是 `std::vector<PassId> reads`；`PassId` 是会话内部句柄，宿主在 `submit` 前不可能有 | 统一为 `Pass`；提交的计划里只放**稳定名字**（`inputs: vector<String>`），会话在帧边界解析成 `PassId`。`PassId` 只出现在 `Session` 内部 |
| X2 | design §3.7 `FrameResult` vs api §7 / P11 `FrameReport` | 一个结果两个名字；`FrameResult` 又缺 `presented_view`（见 A2） | 一个值一个名字：`FrameResult`（每帧）或 `FrameReport`（会话累计）二选一；若都要，明确“帧结果”与“累计报告”的职责 |
| X3 | api §7 七个方法 vs M2/R23 预热 | `open/submit/present/wait/report/capabilities/close` 里没有“给定快照答需要什么”的入口 | 加 `prepare(snapshot, plan) → Requirements`，或让 `submit` 返回本次计划将触发的 `builds`；不要只写在 hot-paths 里 |
| X4 | requirements R27 / api M10 vs I6 / design §2 | R27 的判据写“两会话共享设备”；I6 说只有一条引擎线程碰设备；两个会话各自有引擎线程就是两条 | 一个设备 = 一个 `Session` = 多条 `Surface`；`Target` 带 `SurfaceId`，`FrameRequest` 指明表面。不要用“两个 Session 共享设备”做判据 |
| X5 | 总纲 §1.3“不改作者层 API 的形态” vs design §2.5/§3.5 | 设计要删 `StateNode` / `MatrixTransform` / `ScreenPass` 的“种类”地位、把 `Camera` 变值；这些是公开作者 API 的一部分 | 区分“作者层保留这些类作为糖/构建器”与“快照里只有四类事实”；在 §1.3 写明哪些公开类会保留、哪些是内部重组 |
| X6 | 总纲 §1.3“不改后端实现” vs A1/A5 | revisioned 内容引用、按 (id,rev) 驻留、实例化路径都会改 `ContentStore` 的行键与表 | 把“不改后端实现”改成“不改 vsg 的录制/同步实现；后端表键与接口形状允许改” |
| X7 | 总纲 §2.5 / R5“隐藏是结构（不在树里）” vs `Node::setVisible` / `Scene::setVisible` | 当前作者 API 有可见性开关；若按字面删掉就是 API 破坏 | 把“结构”解释为**快照表示**（隐藏子树不产生行），作者层保留 `setVisible`，由发布把它编译成“无行”；不要把“结构”当成“作者必须 remove/insert” |
| X8 | hot-paths §5.1 `CustomPass : public Backend` vs 同节“今天没有消费者，先不做” | 既说 G8 不做，又给出一个类别错误的接口（pass 不是 backend，`CommandBuffer` 也不是核心类型） | 删掉代码块；只留一句“将来若有消费者，是后端的一个可选能力，不是核心 `Pass` 的函数指针” |
| X9 | design §2.7“5000 行插 10 次只付 10 次矩阵乘” vs `rows(instance) = Placement ∘ PrototypeLocalRows` | 见 A5：按物化写法是 5 万次；按共享 local rows 写法才可能接近 10 次 | 先选实例化实现，再写数字；R3/T4 分开计“原型折叠”与“放置乘法” |
| X10 | design §4.5“record 是纯的、可分块并行” vs `Backend::execute` 是唯一接口 | 录制（命令块构建）在 backend 内部；vsg 的录制要碰设备状态，核心层没有纯 record 的入口 | 把 record 定义为后端内部阶段；若确实要纯命令块，另立接口并说明消费者（G8），不要把它写成所有后端的契约 |

---

## 4. 设计缺口（按严重度）

### A1（高）内容引用只带 id，丢掉 revision —— 在飞帧会看到新内容

**位置**：design §3.3 `ColdChunk`、§3.5 `DrawItem`、§2.8 引擎表、`I4`；requirements `R9`。

**反例**：帧 N 的 `DrawItem` 里是 `MaterialId M`；publish N+1 把 M 改到 rev2；帧 N 仍在飞；
引擎表按 handle index 寻址，找到的是 rev2 的行。帧 N 的材质值就变了——这正是 `R9` / `I1` 禁止的。

**为什么今天没有这个问题**：当前 `RenderCommand` 直接携带
`GeometryDescriptionPtr` / `MaterialDescriptionPtr`（`RenderCommand.hpp`），`ContentStore` 明确把旧 revision 的行
留到“可能还在录制的帧走完”（`ContentStore.cpp` 的 `ensureGeometry` / `ensureMaterial` 注释与 `retirement`）。
新设计把“值过帧”换成了“id 过帧”，方向反了；`§4.1` 的 `id→rev` 表只用于 publish 时发现变化，不用于记录
“这一帧用的是哪一版”。

**修法**（任选，建议 A+B 组合）：

- **A. `ContentRef{ Handle id; uint32_t revision; }`**：冷块与 `DrawItem` 的内容引用带 revision；
  引擎表按 `(index, generation, revision)` 存条目，驻留按条目 `last_epoch`。
- **B. 冷块直接存不可变值指针**（`GeometryDescriptionPtr` / 老 `MaterialDescriptionPtr` / `ProgramValuePtr`）；
  `DrawItem` 用 `RowIndex` 间接；record 不需要解析 id；id+rev 仍用于 dump/replay。
- **C. 会话冻结表**：旧快照活着时表保留旧 revision 的行，按 `(id, rev)` 查（今天做法的推广）。

**成本**：A 每个引用 +8~12 B；B 冷块每行 +8 B/引用，但 `DrawItem` 可缩到 24 B；C 表条目变多。
三者都要进 `N15` / `N3` 的计算，不能只写“句柄化”。

**验收**：新增 `T17`：帧 N 在飞时 publish 修改材质/几何/程序；帧 N 的命令逐项不变；帧 N+1 用新值。`K7`。

**G**：`G4`（语义明确：引用的是“哪一版”）、`G10`（成本）、`G12`（意图在类型里）。

### A2（高）拾取/报告的身份是位置，不是原型；`FrameResult` 缺 View / pose

**位置**：`R18` / `R19`、`I7`、design §3.7、api `P11` / `P23`。

**反例 1**：机械臂原型 5000 行插 10 次；用户点第 3 个实例的末端。pick 返回 `NodeId` —— 10 个实例都是同一个
`NodeId`，选择器无法区分“哪一个”。`R19` 只承诺 `NodeId`。

**反例 2**：动画帧用 `PoseSet` 移动了关节；pick / anchor / fit 吃 `FrameResult` 的 View（`R18`），但
`FrameResult` 没有 `View`，也没有 pose/world —— 它只有 `snapshot_epoch`。今天的 `RenderEngine::presentedCamera()`
至少给了相机快照；新设计把它弄丢了。

**修法**：

- `PickResult{ InstanceId instance; NodeId prototype; uint32_t row; Vec3d point; double depth; }`；
  诊断同时带 instance / prototype（`Diagnostic` 的 `node` 改成两者）。
- `FrameResult` 增加 `View presented_view`，并携带“这一帧的世界值”：要么
  `FrameQuery{SnapshotPtr, PoseSet, View, TargetSize}`（值；pose 矩阵拷贝或共享），要么 `FrameToken`
  由 `Session` 保留一帧（更像句柄，但引入寿命）。
  推荐前者：查询是纯函数，与 `I5` 一致。
- 定义 pose 的 lifetime：`FrameRequest.poses` 的 span 不能悬垂；`FrameResult` 持有拷贝或共享拥有者。

**验收**：`T18` 两个实例的 pick 不同；`T19` pose 后 pick 到 posed 位置；`T20` 相机移动后 pick 用 `presented_view`。`K9`。

**G**：`G1`（一个身份一个定义）、`G4`、`G12`。

### A3（高）共享原型的写者域与发布触发没有定义

**位置**：`R2`（同一 `NodeId` 可跨 Scene / Session）、`C2`（一个 Scene 一个写者）、design §2.7、
`Node.hpp` 的 `announceContentChange`。

**反例**：Node P 同时被 Scene A、B 引用；A 在 thread 1 publish，B 的写者在 thread 2 改 P。publish 读 P 的
placement / children / state，写者写同样的字段 —— 数据竞争。Node 没有 Scene 反向指针（头文件明说），
P 的 epoch 只沿 parent 链上爬；P 是原型、没有 parent，所以 B 改了 P，A 的 root epoch 不变，A 根本不会重新发布。

**修法**：

- 新增 `C2b`：一个原型和所有引用它的 Scene 属于**同一个写者域**（同一线程，或有 happens-before 的交接）；
  跨线程共享不支持（第一版）。
- 发布触发：Scene 在 publish 时比较“自己引用的原型 revision 表”（O(引用的原型数)），发现变化才重建；
  不要扫描整棵树。
- 若必须跨线程共享，原型必须 **immutable**（编辑产生新 `NodeId` / 新 revision，由拥有它的域发布），
  而不是共享可变 Node。

**验收**：`T21` 两个 Scene 共享同一原型，在同一写者域编辑后两边都更新；跨线程用例被断言/诊断拒绝；TSan 0。`K8`。

**G**：`G2`（边界）、`G10`（并发成本）、`G12`。

### A4（高）原型的寿命：`Instance` 只持 `NodeId` 时，原型先死会留下悬垂 id

**位置**：`R2` / `R4`、design §2.6“原型先析构、画面照旧”、§2.7 `Instance`。

**反例**：`Instance{NodeId}` 在树里；Node 的最后一个 owner 析构；下一次 publish 需要读原型的
placement / 子节点 / 载荷来折叠，但对象没了。已发布的 Snapshot 可以照旧（值），但未来发布不行；
`find(NodeId)` 答 gone 不能救折叠。

**修法**（选一）：

- **Instance 持有 `intrusive_ptr<Node>`**：原型至少活到最后一个 Instance 离树；“原型先析构”只适用于
  已发布 Snapshot。推荐，最符合 `R2` / `R4`，且寿命由引用算出（与 `I4` 同构）。
- Scene 在插入时复制原型的值（不是指针）：原型析构后实例仍可发布；代价是“改原型所有实例一起变”不再成立，
  需要显式同步。
- 注册表保留原型值（proto value）直到无引用；Node 对象析构不删除值。

**验收**：`T22` 原型析构后仍有实例：不崩、不悬垂；要么实例保持原型活，要么插入时已复制值并明确语义。

**G**：`G10`、`G11`。

### A5（高/中）原型实例化的矩阵成本与内存被低估

**位置**：design §2.7、requirements `R3`、`T4`、benefits §8。

**原话**：“5000 节点的机械臂插 10 次，只付 10 次矩阵乘 + 行拼接”。

**反例**：`rows(instance) = Placement(chain) ∘ PrototypeLocalRows`。若每个实例要物化 5000 行 world，
则每个实例 5000 次矩阵乘，10 个实例 **50,000 次**，不是 10 次。内存：10 × 5000 ×（`Mat4d` 128 B +
`Aabbd` 48 B + `chain` 128 B）≈ **15 MB**（仅 hot），cold 另算。若共享 prototype local rows，则 world 不能
共享（每个实例不同），除非把 instance transform 留到绘制时。

**三种做法**：

| 方案 | 折叠成本 | 内存 | 代价 |
|---|---|---|---|
| A 物化 | N × rows 次乘法 | N × rows | 实现简单；`R3`/`T4` 要写“原型折叠=1，放置乘法=N×rows” |
| B 共享 local rows + per-instance root transform | 原型一次 + N 次根变换 | O(rows + N) | DrawItem 带实例矩阵/索引；cull/pick 要按实例组合；内部覆盖（单个关节）迫使该子树物化 |
| C chunk-level chain | N × chunks（5000/32×10 ≈ 1570） | N × rows | 介于两者之间，仍不是 10 |

推荐：静态实例用 B；有内部覆盖的实例把被覆盖子树物化。`R3`/`T4` 分开计“prototype folds”和
“placement multiplications”；benefits 的“子树复用”要写内存与覆盖代价。

**验收**：`T4` 计数口径明确；`T23` 5000×10 的发布 / select / 内存实测；`N3` 不超。

**G**：`G10`、`G8`（不要为实例化预建复杂缓存）。

### A6（中高）`DrawItem ≤ 64 B` 与 `sort_key` 的宽度自相矛盾

**位置**：design §3.5、§4.4、hot-paths `N15` / `N16`。

按 §3.5 的字段静态估算（未计 `DrawState`）：

| 字段 | 字节 |
|---|---|
| 4 × id（geometry / material / program / node） | 32 |
| primitive + instance_first + instance_count | 12 → 44 |
| `sort_key`（§4.4 公式是 `(order+2^31) << 32 | ...`，需要 64 位；字段却声明 `uint32_t`） | 8 → 52 |
| `depth` | 4 → 56 |
| `world` 指针 | 8 → 64 |
| `opacity` | 4 → 68 |
| `transparent` | 1 → 69 |
| `DrawState` | ≥ 8 → > 64 |

即使把 `sort_key` 压回 32 位，也在 `world` 之后已到 64，再加 opacity/transparent/state 必然超。
`N15` 的静态断言会直接失败；而 64 B 的排序项也比较贵。

**修法**：

- `DrawRef{ uint32_t row; uint32_t instance_first; uint32_t instance_count; uint32_t flags; uint64_t sort_key; }`
  = 24 B；id / state / material 经 row 从冷块取，world 经 row 从 hot 块取；`depth` / `transparent` 编进 key。
- 或排序只排 `(uint64_t key, uint32_t row)` 两个数组，录制时再 gather；`DrawItem` 本身不参与排序。
- 若一定要保留 64 B：删 `world` 指针、`node` id、`depth` 重复字段，改用 32 位 row 索引；`DrawState` 仍需进冷块，
  不是进 `DrawItem`。
- `sort_key` 类型改成 `std::uint64_t`；`N15` 改成实测后的上限，不要先写 64。

**G**：`G4`、`G10`。

### A7（中）publish 的元数据不是 O(变更)

**位置**：design §4.1 第 2 步：“浅拷贝 `块指针数组`、`instances_`、`prototypes_`、`registry_`（都是指针/小结构，O(块数)）”。

`prototypes_` 是每个原型一项，`instances_` 是每个实例一项，`registry_` 是每个被引用素材一项；
如果每个 Node 都是原型（设计允许外部构造），它们就是 O(节点数) / O(实例数) / O(素材数)，不是 O(块数)。
`ValueRegistry` 的值还可能带 `intrusive_ptr`（材质 5 个纹理槽 = 5 次原子增减），整体拷贝会把“一次发布一次”
变成“每个素材一次”。

**修法**：未变化的表用 `shared_ptr<const Table>` 共享 + 小 delta overlay；或持久化 map / 小型不可变哈希；
或诚实地把成本写成“O(原型数 + 实例数 + 被引用素材值数)”，再用 `K1` / `K11` 测量。

**验收**：新增 `N19`：5000 原型、1 次编辑的 publish ≤ 50 µs（或元数据字节 ≤ X）；`K11`。

**G**：`G10`。

### A8（中）块级裁剪的 `chain[0]` 不成立；`local_box_union` 字段不存在

**位置**：design §3.3 `HotChunk.chain`、§4.3 select。

**反例**：一个 32 行的块可以横跨兄弟子树；它们的父链不同。把 `local_box` 的并集用“本块第一行的父链”变换，
再整块剔除，会把可见内容剔掉。`§4.3` 使用的 `c.local_box_union` 在 `ColdChunk` 里也没有声明。

**修法**：

- 在 `HotChunk` 存 `world_bound`（本块 `world_box` 的并集），发布时维护；块剔除直接用它。这样也不需要在
  hot 块里为每行存 `chain`（省 128 B/行）。
- 或规定块边界绝不跨实例/子树边界，并给出证明与测试；但 32 行块很难保证。
- 用 `world_bound` 时注意：动画 pose 路径的 `world_bound` 会过期；posed 子树必须逐帧重算或保守剔除（见 A10）。

**验收**：`T24` 一个块里有两段不同父变换，一段在视外、一段在视内；不得误剔除。

**G**：`G1`（一个裁剪事实一个来源）、`G11`。

### A9（中）`I4` 的“活 Snapshot”不可观测

**位置**：`I4`、design §4.6。

`I4` 说“GPU 资源存活 ⟺ 有活 Snapshot 引用它的句柄”；`§4.6` 却只用“在飞帧 + 当前快照”的 `oldest_live`。
宿主完全可以在 Session 之外持有一个 `SnapshotPtr`（编辑器预计算、动画预排），Session 不知道它活着。
资源被回收后再次提交，正确行为是重新上传（`builds > 0`），但这与 `I4` 的字面意思不符。

**修法**：把 `I4` 改成“资源存活 ⟺ 被 Session 已接受或在飞的 Snapshot 引用”；若宿主需要钉住，加
`Session::pin(snapshot)` / `unpin`，或让 `FrameRequest` 明确承担钉住责任。

**验收**：`T25` 旧 Snapshot 在回收后再提交：重建、无陈旧资源、无 UAF。

**G**：`G4`。

### A10（中高）`PoseSet` 的语义不足以表达关节层次

**位置**：hot-paths §3.2。

`PoseEntry{ InstanceId first; uint32_t count; const Mat4d* matrices; }`：`first` 若是 `InstanceId`，注册表 index
并不按树序连续，`count` 无法表示“接下来哪些实例”；若 `matrices` 是逐行世界矩阵，则父关节的 pose 不会传播到
子节点；而公式 `world(row) = chain × pose(row) × local(row)` 也只改一行。机器人关节恰恰需要“父关节动，整条
子树跟着动”。

**修法**：

- `PoseEntry{ InstanceId instance; Mat4d local; }` 列表；折叠时替换该实例的 local，并重算它的**子树**行
  （O(被 pose 子树的行数)），而不是 O(pose 条目数)。
- 校验：实例存在、矩阵有限、无重复；违规 → 一条稳定 code 的诊断 + 跳过该条，不崩。
- 定义与 `ViewPlacement` 的顺序：先应用 pose 的局部放置，再解析视图相关放置。
- 剔除与拾取用 posed world；`FrameResult` 必须携带 pose（见 A2）。

**验收**：`T26` 父关节 pose 后子节点跟着动；`T27` pose 后 pick 到 posed 位置；`T28` pose 子树的剔除不变形。`K9`。

**G**：`G4`、`G10`。

### A11（中）缺少的接口：预热、多表面、设备选择、计划校验

| 缺口 | 位置 | 修法 |
|---|---|---|
| 预热查询（M2 / R23）不在 7 个会话方法里 | api §7 | 加 `prepare(snapshot, plan) → Requirements`，或让 `submit` 返回本次计划将触发的 `builds`；不要只写在 hot-paths |
| 多表面（R27 / M10）与 `I6` 冲突 | requirements R27、api M10、design §3.6 `Target` | 一个设备 = 一个 Session = 多条 Surface；`Target::Surface{SurfaceId}`；`FrameRequest` 指明表面 |
| 设备/适配器选择 | `SessionConfig` 只有 required/optional caps | 加 `DeviceRequest`（backend 种类、适配器偏好、必选/可选 caps）；`capabilities()` 在 `open` 前有静态探测入口 |
| 计划校验 | design §3.6 / hot-paths §4.2 | 除“前向引用”外还要 DAG 环检测、重复名字、未知输入；FNV 哈希碰撞要存名字并比较，不能只按 64 位哈希匹配 |
| 固定 `kMaxPasses` | design §3.5 `FrameValue` | 超限要有稳定 code 的诊断；或把 `passes` 放 arena，不设固定上限 |
| `FrameResult` / `FrameReport` | X2 | 一个每帧值 + 一个累计报告，职责写清；每帧值必须带 `presented_view` |

**G**：`G4`、`G8`、`G11`。

### A12（中低）几处编译 / 概念错误

- **`Result<void>`**：`template <class T> class Result { static Result ok(T value); ... }` 对 `T = void`
  不能声明 `T value` 形参；`Backend::create` 返回 `Result<void>` 会编译失败。需要 `Result<void>` 特化，
  或 `Result<Unit>` / `std::optional<Diagnostic>`。
- **`publish` 批内返回 `nullptr`**：`SnapshotPtr` 的 null 是 magic failure；宿主可能把它直接传给 `present`。
  改成 `Result<SnapshotPtr>`，或返回“上一份快照”（并明确“未发布”的观测方式）。
- **`CustomPass : public Backend`**：概念错误（一个 pass 不是一整个后端），且 `CommandBuffer` 不是核心类型；
  同节自己说“今天没有消费者，先不做”。删掉代码块，只留一句扩展位置说明（`G8`）。
- **`ContentRegistry::allocate()` 返回无类型 `Handle`**：`Id<Tag>` 可以由任意 `Handle` 构造，
  “不同 kind 编译期不能互传”并不成立；`domain` 只能检测域，不能检测 kind。加 `allocate<Tag>()` 或每种 kind
  一个注册表。
- **`PassId` 出现在宿主计划里**（X1）：宿主在 `submit` 前拿不到会话内部 id；用名字。

**G**：`G1`、`G4`、`G8`、`G12`。

### A13（中）`record` 的纯函数位置不存在

**位置**：design §4.5、hot-paths §2.4 / §4.3。

设计说 `record` 是“构建命令块（纯）”，可分块并行、确定性合并；但 `Backend` 的接口只有
`create / alignPlan / execute / release`，录制发生在 `execute` 内部；vsg 的录制本身要碰设备状态。
核心层没有一个纯 `record` 的入口，`FrameReport` 也无法在不改后端的情况下分出 `record_micros`。

**修法**：把 `record` 定义为**后端内部阶段**（并行与否是后端的事，先做 1）；若确实要纯命令块，另立
`Backend::encode(FrameValue) → CommandList`（但今天没有消费者，按 `G8` 进 backlog）。核心层的 `FrameResult`
只要求后端报告 `record_micros` / `execute_micros`，不要求它们可被核心并行。

**G**：`G2`、`G8`。

---

## 5. 需求与验收的补充（建议编号）

| # | 要求 | 判据 |
|---|---|---|
| R31 | 内容引用必须能回答“哪一版”：`ContentRef{id, revision}` 或不可变值指针；旧快照永远读旧版 | `T17` / `K7` |
| R32 | 拾取/诊断的身份是**位置**：`InstanceId` + `NodeId` + row；同一原型的多实例可区分 | `T18` |
| R33 | 共享原型属于一个写者域；Scene 的 publish 能发现被引用原型的 revision 变化 | `T21` / `K8` |
| R34 | `Instance` 对原型有明确的存活责任；不允许悬垂 `NodeId` 进入下一次发布 | `T22` |
| R35 | `PoseSet` 的语义是“实例 local 覆盖 + 子树传播”；pose 后可剔除、可拾取 | `T26–T28` / `K9` |
| R36 | 预热查询是会话的一等入口（`prepare`），预热后首帧 `builds` 增量为 0 | `T7` 扩展 |
| R37 | 一个设备一个 Session 多条 Surface；`Target` / `FrameRequest` 带 `SurfaceId` | `T29` / `K10` |
| R38 | 计划校验：重复名字、环、未知输入、超 `kMaxPasses` 都有稳定 code | `T30` |
| R39 | `publish` 在批内不返回 null；未发布是显式的 `Result` / 上一份快照 | `T31` |

对应新增验收：`T17` 旧帧隔离、`T18` 多实例拾取、`T19` pose 拾取、`T20` 相机帧拾取、`T21` 共享原型发布、
`T22` 原型寿命、`T23` 实例化成本、`T24` 块剔除、`T25` 回收后旧快照、`T26–T28` pose、`T29` 多表面、
`T30` 计划校验、`T31` 批内 publish。

---

## 6. 判死条件的补充（`K7–K11`）

| # | 判死条件 | 怎么量 |
|---|---|---|
| **K7** | 在飞帧的旧内容被新发布改变（材质 / 几何 / 程序任一） | `T17`：两帧在飞时改内容，逐项 diff；任何差异 = 失败 |
| **K8** | 两个 Scene 共享原型时出现数据竞争，或跨线程用例无法被拒绝 | `T21` + `scripts/tsan_check.sh`；任何新报告 = 失败 |
| **K9** | pose 后的拾取/剔除与画面不一致 | `T27` / `T28`：posed 位置与拾取点、剔除结果对齐 |
| **K10** | 多表面必须靠两个 Session / 两条引擎线程才能成立 | `T29`：一个 Session 两个 Surface；否则 `I6` 或 `R27` 必须改 |
| **K11** | 5000 原型、1 次编辑的 publish 超过预算（元数据整体拷贝暴露） | `N19`：≤ 50 µs 或字节上限；超了 = 表结构要改 |

同时修订两条现有判据：

- **K4**：把“`test_vsg` 537 条里 >5% 需要改测试”改成“**语义断言**改变 >5%”；改名/别名迁移导致的纯拼写改动不计，
  否则 B 批改名会误触发判死。
- **K5**：明确“桥 + 寿命层”是 **.hpp + .cpp** 还是仅 cpp；建议与 §2 的 2519 行同口径。当前原型把桥加到 2700 行（新增 select/facts 函数），K5 仍以提案基线 2519 为分母。

---

## 7. 成本与收益的再评估

| 提案主张 | 审计结论 | 说明 |
|---|---|---|
| 桥接 + 版本逻辑 2519 → ≤600 行 | **保留，但统一口径** | 2519 是头+cpp；cpp-only 1212。用 2519 对比 600 是 76% 降幅，用 1212 对比 600 是 50%；两者都值得，但不能混用 |
| 20 条约束 → 7 条不变量 | **保留为“机制”比较，不当作规则总数** | 新文档仍有 `C1–C12` + `R31–R39` + `N#`；真正的收益是镜像/delta/memo/版本这些机制消失，不是规则条数 |
| 发布 = O(变更) | **有条件** | 块数组成立；元数据表若不共享就是 O(原型+实例+素材)（A7） |
| 原型实例化省矩阵乘 | **不成立（按当前写法）** | 5000×10 = 5 万次，不是 10 次；内存也按实例倍增（A5） |
| `DrawItem ≤ 64 B` | **不成立** | 按字段估算已 >64 B；`sort_key` 还少 32 位（A6） |
| 寿命机器 72 → ≤20 | **保留，口径要写** | 75（六 token）/100（含 parked）；新模型加入 revisioned 表后会增加条目数，但 `markUsed` / `parking` / `kept` 的协调消失 |
| 并行 select 免费 | **保留，但不是卖点** | 2160 µs 还没进 16 ms 预算；并行是纯函数化的副产品，不应作为采用理由 |
| “不改作者层 API” | **需要限定** | `StateNode` / `MatrixTransform` / `ScreenPass` / `Camera` 的公开地位会变；应说“保留糖类，改变内部表示” |

**总体判断**：主干仍然值得做，但卖点应从“快照/结构共享的抽象更漂亮”改成：
**“寿命与版本从猜变成算，且旧帧永远读旧值”**。后一句只有在 A1 修好后才能成立。

---

## 8. 修订后的推进顺序（替代原三批）

0. **文档修正（无代码）**：修 X1–X10；把 A1–A4 写进 design / requirements；补 `K7–K11`、`R31–R39`。
1. **测量闸门**：`-hot-paths` §8 第 1 步（`Measure*` 阈值断言）；同时把 §2 的计数写成可复现脚本/命令。
2. **快照 + 纯 select（不带原型实例化）**：在 `detail::` 内用**今天的图语义**（Node 树、`StateNode` 等）
   构造快照；内容引用带 revision / 值指针；跑 `K1–K3` + `K7`。这一步通过才谈下一步。
3. **寿命/注册表**：`ContentRegistry` + `ContentRef`；引擎表按 `(id, rev)`；删 `markUsed` / `parked` / `kept`；
   跑 `K6` / `K8`。
4. **计划值 + 预热 + 多表面**：`Session` 7 方法 + `prepare`；一个设备一个 Session 多条 Surface；
   跑 `T7` / `T29` / `K4`（语义口径）。
5. **原型实例化单独实验**：只有前四步稳定后才做；先定 A5 的实现（B 为主），再测 `T23` / `K11` / `N3`。
   若实例化数字不好，保留“原型 DAG 共享”作为内容侧能力，不进入快照主路径。

理由：A1/A5/A8 都是“结构没定就先做实现会返工”的地方；把它们放在原型之前比事后改快照布局便宜。

---

## 9. 建议修改的文档位置

| 文档 | 位置 | 改什么 |
|---|---|---|
| `graphics-2026-11-11-next.md` | §0 文件表 | 加一行本文件；读法里加“第二轮审计先看 A1–A4” |
| 同上 | §4.5 | 加 `K7–K11`；修订 `K4` / `K5` 口径 |
| 同上 | §3 概念对照 | `DrawItem` / 冷块的内容引用改成 `ContentRef{id,rev}` 或值指针 |
| `graphics-2026-11-11-next-requirements.md` | §2.2 / §2.3 / §2.4 | 加 `R31–R39`；`C2b` 写者域；`T17–T31` |
| `graphics-2026-11-11-next-design.md` | §3.3 / §3.5 | `DrawItem` 加 revision/row 间接；`sort_key` 改 64 位；`HotChunk` 去 `chain`、加 `world_bound` |
| 同上 | §3.7 | `FrameResult` 加 `presented_view` 与 `FrameQuery` / pose |
| 同上 | §4.1 / §4.3 / §4.5 | 元数据表共享；块剔除用 `world_bound`；`record` 改后端内部 |
| 同上 | §5 | 加写者域与原型寿命规则 |
| `graphics-2026-11-11-next-benefits.md` | §1 / §4 / §8 | 按 A5–A7 重算实例化、`DrawItem`、元数据；统一行数口径 |
| 同上 | §10 | 三批改为 §8 的五步 |
| `graphics-2026-11-11-next-api.md` | §7 | 加 `prepare`；多表面；`FrameResult` / `FrameReport` 职责 |
| 同上 | §5.0 / §6 | `Pass` 统一；`M2` 加接口 |
| `graphics-2026-11-11-next-hot-paths.md` | §3.2 / §4.2 / §7 | `PoseSet` 语义；`Pass` 输入用名字；`N15` 改实测值；`N19` 元数据 |
| 同上 | §8 | 第 0 步：先做文档修正与证据脚本 |

---

## 10. 结论

这次审计没有推翻提案的诊断，也没有把“未采纳”改成“采纳”。它做的是三件事：

1. **把“可证伪”落到具体反例**：旧帧隔离（A1）、多实例拾取（A2）、共享原型（A3/A4）、
   实例化成本（A5）、`DrawItem` 尺寸（A6）、块剔除（A8）——这些都能写成用例，不需要争论审美。
2. **把数字的口径钉住**：2519 是头+cpp；cpp-only 1212；72/75/100 是 token 口径；47 已漂移。
   没有口径的数字不应作为 `K5` 的判据。
3. **把顺序改对**：先修文档和补判死条件，再做测量闸门；快照原型先不带原型实例化；
   实例化是独立实验，不是第一步的一部分。

---

## 11. 落地状态（2026-11-11）

| 项 | 状态 |
|---|---|
| `X1–X10` | 已写入六份文档：`Pass` / `Target{SurfaceId}` / `inputs` 用名字、`FrameResult`、`prepare`、单 Session 多 Surface、作者糖类保留、隐藏=快照表示、`CustomPass` 删除、实例化数字、record 后端阶段 |
| `A1` revision | design §3.3/§3.5/§4.1/§4.6 + `ContentRef{id,rev}`；requirements `R31`/`R9`/`R22`；main `I4`/`D9` |
| `A2` 拾取身份 | design §3.5 `PickResult` + §3.7 `FrameQuery`；requirements `R18`/`R19`/`R32`；`T18–T20` |
| `A3` 写者域 | design §5；requirements `C2b`/`R33`；`T21`/`K8` |
| `A4` 原型寿命 | requirements `R4`/`R34`；design §7 `kPrototypeMissing`；`T22` |
| `A5` 实例化成本 | design §2.7/§4.2 改为 `N × 原型行数` + `Q8`；requirements `R3`/`T4`/`T23`；benefits §8 |
| `A6` DrawItem | design §3.5 改成 24 B / 64 位 `sort_key`；hot-paths `N15` |
| `A7` 元数据 | design §3.4/§4.1 共享表；hot-paths `N19`；`K11` |
| `A8` 块剔除 | design §3.3 `world_bound` / §4.3；requirements `T24` |
| `A9` I4 | main `I4`；design §4.6；requirements `R22`/`T25` |
| `A10` pose | hot-paths §3.2 `PoseEntry`/子树传播；requirements `R35`/`T26–T28` |
| `A11` 接口 | design §3.6/§3.7/§4.7；api §7 八个方法 + `prepare`；requirements `R36–R38`/`T29`/`T30` |
| `A12` 编译/概念 | design `Result<void>` 特化、`publish → Result`、`allocate<Tag>()`；hot-paths 删 `CustomPass` |
| `A13` record | design §4.5 后端内部阶段；hot-paths §2.4 |
| 步骤 1 | `SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera` 已加阈值（`drawables*3+1000` µs，sanitizer 只打印）；新增 `scripts/measure_graphics_claims.py` |
| 步骤 3a（身份/驻留原型） | 内部 `ContentRegistry`（typed `Id<Tag>`、`Handle` 64 位、generation/domain、槽退休）+ `ContentRef{id,rev}` + `ResidencyTable`（按 epoch 的 collectable/reclaim）；`ContentRegistryTest` / `ResidencyTableTest` 6 条通过 |
| 步骤 3b（公开身份） | `ContentId.hpp` 公开 `Handle`/`Id<Tag>`/`ContentRef`/`ContentRegistry`；`Node::id()`；`Row::identity`、`RenderWorld::RowId`、delta、镜像、快照、`RenderCommand::node`、`CollectedCommand`/`CompiledCommand`、`GeometryFacts`、`ContentStore` 几何表全部改为 `NodeId`；`test_graphics` 559 / `test_vsg` 537 / ctest 30/30 |
| 步骤 4（材质身份，关掉 `A1` 的后端残余） | `MaterialBase::id()`（ctor 分配 / dtor 释放）；`MaterialFacts::material` 改为 `ContentRef`（**revision 进键**，独立 `revision` 字段删除）；`CompiledCommand`/`CollectedCommand` 的 `material` 改为 `ContentRef`；`ContentStore` live 表按 `MaterialId`、行表**每 revision 一行** + 旧行停靠；`MaterialArena`/`BlockStorage` 改按 `MaterialId`；`released_materials` 改 `vector<MaterialId>`；`test_vsg` 537 全绿 |
| 步骤 2（chunked + 增量） | 已落地内部 `detail::SceneSnapshot`（32 行 chunk 共享）+ `selectSnapshot` + `SceneRows::snapshot` 缓存；`Scene::ensureRows` 增加**叶子/放置热字段的增量刷新**（结构、状态、几何/材质变化仍回退整表）。`SnapshotTest` 五条通过（纯 select、旧 revision、transform、计时、常驻内存）。2000 行改 1 行：**RelWithDebInfo publish 约 154–203 µs**，select 约 293–307 µs；Debug 为约 2.2 ms / 1.3 ms。shared 62/63 chunk。**K1 / K2 / K3 / N2 全部通过**：K1 ≤500 µs、K2/N3 常驻 **1.03×** 行表、N2 已调整为 ≤250 µs（原 50 µs 需要 dirty notification，见 §11.1） |

六份提案与 audit 仍保持“未采纳”状态。步骤 2 已把“整表复制”换成了 **chunk 共享 + 热字段增量刷新**，
纯 select、旧 revision 隔离、K1、K2 与 N2 都已由 `SnapshotTest` 证明（RelWithDebInfo 203 µs / 293 µs，
常驻 1.03× 行表；N2 已按 §11.1 调整为 ≤250 µs）。步骤 3a（`ContentRegistry` / `ContentRef` / `ResidencyTable`）、
步骤 3b 第一段（公开 `ContentId.hpp` + `Node::id()` + 行身份全面 `NodeId`）与步骤 4（材质身份进键）
已落地并通过 559 / 537 / 30 套测试。

**步骤 4：材质的 `A1` 残余已关掉，并顺手改了两个设计细节。**

- `MaterialFacts::material` 从 `const void*` + 独立 `revision` 改成 `ContentRef`，于是材质表的查找键就是
  `(id, revision)`，旧 revision 的行在停靠窗口内继续作答 —— 与几何表同一条规则。`ContentStore::ensureMaterial`
  不再就地替换 + `RetiredMaterial`/`kept`，改成**每 revision 追加一行**、旧行停靠后删；`materialEntries()`
  因此是“每 revision 一行”。
- 计划要能命名那个 revision，所以 `CollectedCommand`/`CompiledCommand::material` 也改成 `ContentRef`；
  `FrameRecorder::snapshotCommands` 的规则是“已发布的值取值的 revision，否则取对象当前 revision”，
  与 `ContentStore::ensureMaterial` 取值的规则一致 —— 两边算同一个数，手写命令不会与表永远错过。
- `ContentRef::revision` 从 `std::uint32_t` 加宽到 `std::uint64_t`：SDK 的每个 `revision()` 都是 64 位，窄键会在
  四十亿次编辑后回绕；设计稿 §3.3 的片段已同步。
- `MaterialArena` / `BlockStorage` / `ContentSweep` / `released_materials` 全部改按 `MaterialId`
  （不是“持有一个地址”的键，而是真正的身份）；测试侧新增 `tests/test_vsg/TestMaterialFacts.hpp`
  （`materialId()`、`materialRef()`、对象指针 → `ContentRef` 的具名转换），避免每个用例自己拼键。

**步骤 3b 已按设计拍板走 B（公开身份就是 `Handle`），第一段已落地。** design §3.1/§3.3 早已把身份定为
`vn::graphics::Handle` + `ContentRef{id, revision}`，所以“A 不改公开身份”与设计冲突，不再作为选项保留。

已落地：

- 新增公开头 `sdk/vine/graphics/ContentId.hpp`：`Handle`（gen24/domain8/index32，释放即换 generation、到顶退休）、
typed `Id<Tag>`、`NodeId`/`GeometryId`/`MaterialId`/`ProgramId`/`TextureId`/`InstanceId`、`ContentRef`、
`contentRefOf()`、`ContentRegistry`（含 `instance()` 进程默认域）以及 `std::hash` 特化；`ResidencyTable` 改用公开
`ContentRef`。
- `Node` 构造时分配 `NodeId`、析构时释放，新增 `Node::id()`：陈旧行拿到的是“gone”，不是地址复用后的别人。
- 行身份全面改为 `NodeId`：`Row::identity`、`RenderWorld::RowId`、`DeltaChannel::Delta::identity`、
`ContentMirror` 的 delta、`SceneSnapshot::identities`。
- 公开命令字段 `RenderCommand::row`（`const void*`）改为 `RenderCommand::node`（`NodeId`）；
`CollectedCommand`/`CompiledCommand`、后端 `GeometryFacts`、`ContentStore` 的几何行表与 live 表随之按 `NodeId`
键（`std::hash<Id<Tag>>`）。
- 手写命令没有 node 时以 `geometry->id()` 命名（`VsgBackend::collectDraws`、`FrameRecorder::snapshotCommands`）：
一个具名转换，不是第二真相。
- 证据：`test_graphics` 559、`test_vsg` 537、ctest 30/30 全绿；`Node`/`RenderCommand` 布局变更后需清掉旧对象再编。

仍未做（顺序即依赖）：

1. **程序身份**：`ShaderProgram::id()` 与 `ProgramRef`（目前仍是 `(地址, revision)`）改成 `ContentRef`，让程序表也按
   `(id, revision, variant, kind)` 键；今天靠 `ProgramFacts::generation` 补地址复用（见 `ADR-2026-11-09`）；
2. 计划行的 `CompiledCommand::geometry` + `geometry_revision` 合并为 `ContentRef geometry`（几何表现在是
   `(NodeId, revision)`，两字段同源）；
3. `ContentStore` 的 `markUsed` / `parking` / `kept` 换成 `ResidencyTable` 的 epoch 回收。

不要在 1–3 之间半迁移 `ContentStore`：当前 `markUsed`/parking 仍是后端已验证的寿命路径，半迁移会同时丢掉旧证据
和新不变量。不要把 Debug 的 2.2 ms 当作最终数字。

### 11.1 N2 复核：203 µs 与 50 µs 的差距

一次 2000 行、改 1 行的 publish 在 RelWithDebInfo 下仍要扫四遍 O(rows)：

1. `Scene::ensureRows` 的 node-epoch/结构校验（找编辑行）；
2. `refreshBoxes`（字节编辑可能没有改 epoch，不能直接相信图）；
3. `refreshMaterials`（材质编辑不走图 epoch）；
4. `makeSceneSnapshot` 的 row/chunk 对比与元数据分配。

这四遍是 N2 与 K1 的差距来源。要把 203 µs 压到 50 µs，需要让内容侧把“哪一行/哪个 box/哪个 material
变了”直接告诉发布（dirty notification）。但那会重新引入 `Node → Scene` 的反向通知或全局订阅，
与 `G2`（依赖单向）和 `G7`（不把复杂度推给调用者）直接冲突；也是本提案刻意删掉的镜像/delta 机器的一类。

因此审计的结论是：**N2 的 50 µs 与“Node 无 Scene 反向指针”是同一张取舍表上的两项**。
2026-11-11 的决策：**N2 调整为 ≤250 µs**，保留 K1=500 µs 作为判死线；不在这一步引入 dirty 通知。
50 µs 作为“若将来有内容侧 dirty 通道再追求”的 backlog 项，而不是当前实现的门。

如果只保留一句话：**“跨层只有不可变值”这句话要落到引用上——引用必须回答“哪一版”；
只带 id 的引用不是不可变值，只是把版本问题推到了引擎表里。**
