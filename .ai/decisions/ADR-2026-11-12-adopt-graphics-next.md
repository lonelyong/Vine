# ADR-2026-11-12：采纳 graphics-next 作为下一版帧管线设计

- 状态：已采纳
- 日期：2026-11-12
- 相关：`../design/graphics-frame-pipeline.md`（目标形状的规范文档）、`../design/graphics-threading.md`
  （迁移期前的线程与两个世界）、`../design/vsg-backend.md`、`../backlog/graphics.md`、
  `../research/graphics-2026-11-11-next*.md`（提案与审计原文）、`scripts/measure_graphics_claims.py`

## 背景

今天"跨层的那份副本什么时候过期"有五套答案（地址 + 版本、镜像 + delta、memo + epoch、`markUsed` 的
引用计数、驻留的停车窗口），桥与寿命层合计 2519 行。两次真实事故（镜像在帧内出生没有帧令牌、
`refreshMaterials` 只改 publish 会滞后）都在这一层；`ContentStore` 的寿命层在 2026-11 又出过
"地址在行被读出前被回收"的 UAF（`.ai/bugs/vsg-content-address-recycled-before-rows.md`）。

`graphics-2026-11-11-next` 及其六份展开文档给出了另一套形状（四层、8 个名词、3 个动词、7 条不变量），
并把"什么证据会否决它"先写成 `K1–K11`。两轮审计之后，提案的**前四步已落地**（文档修正、`Measure*`
阈值、分块首写时复制快照、公开身份 `ContentId.hpp` + `ContentRef` + 按行 epoch 回收），实测
`K1 = 203 µs ≤ 500`、`K2 = 1.03×`、`K3` 通过、`K7` 由 `(id, revision)` 键保证。

此前它在仓库里的状态是"调研提案 / 未采纳"，触发器写成"`K1–K11` 任一被触发"。**这等于把"要不要做"
交给测量，而测量的前提是先把提案做出来。** 负责人 2026-11-12 明确：graphics-next 就是下一版的设计，
不是候选方案。故本 ADR 采纳它。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 保持现状，提案只作原型 | 零风险；已落地的身份/寿命层继续可用 | 五套过期答案与镜像/delta/memo 继续在轨；`K4`/`K5`（等价性、代码量）永远无法评估（`G1`/`G7`） |
| B **采纳整提案，按 `graphics-frame-pipeline.md` §9.3 分步替换** | 一条规则、一个动词；误用不可表达；每步可独立回退 | 迁移期两套形状并存；公开 API 有形状变化（计划、发布、会话）；热路径最后一步（生产收集改走快照）风险最高 |
| C 大爆炸重写后再合入 | 一次性干净 | 无法按步判死；违反 `C12` 与仓库"每步一个可回退提交"的节奏 |
| D 只采纳已落地的身份/寿命层，其余留在提案 | 不碰公开 API | 保留了需要被替换的镜像/delta/memo；"发布 = 一次指针交换"不成立，`R9` 永远只是部分 |

## 决定

选 **B**，并同时改掉三件事：

1. **目标形状进入 `.ai/design/`**：新增 `graphics-frame-pipeline.md`（状态：草案），承载四层、8 个名词、
   3 个动词、`C1–C12` / `R1–R39` / `I1–I7` / `N1–N10` / `P#` / `D#` / `T#`。它是"下一版是什么"的规范；
   `graphics-threading.md` / `graphics-scene.md` 仍是"现在是什么"，两者冲突处以**状态字段**区分。
2. **`K1–K11` 改变身份**：从"否决提案"变成**每一步的验收阈值与回退条件**。某一步实测击穿它，回退该步
   并把结论写进本 ADR（不重开"是否采纳"这个问题）。
3. **backlog `T32` 从触发条件型移出**：改成"现在可做"的具体步骤表，由 `graphics-frame-pipeline.md` §9.3
   维护次序。

调研稿不立即删除：`-design` 的类型草案与七个算法、`-api` 的 `P1–P32`/`M1–M10`、`-hot-paths` 的 pose 与
后期依赖图细节，按步吸收进 design/ADR 后，**每吸收完一份就删一份**（`.github/skills/design-docs` §1）。

## 后果

- 好的：一条生命周期链（`publish → derive → execute`）取代五套过期规则；陈旧的误用变成不可表达
  （会话形状、陈旧句柄、活相机查询）；每一步都有已写好的阈值，判死不再靠争论。
- 坏的 / 代价：
  - **迁移期两套并存**：生产收集在最后一步之前仍走 `RenderWorld` + `drawListBake`，`selectSnapshot`
    只有测试消费者。这段时间"设计是新的、实现是旧的"是**已知状态**，不是遗漏（§9.1 逐项标状态）。
  - **显存**：pin 模型的代价（ADR-2026-11-10 §代价）在 `I4` 下继续成立——宿主自持快照不钉住资源，
    但活快照所需的驻留全部保留。
  - **公开 API 形状**：计划变成值、`publish` 返回 `Result`、`prepare` 成为一等入口、`addPass` 家族消失。
    这些都在 `graphics-frame-pipeline.md` §4 里逐条列出，并按 `C11`（迁移期只增不改）分步落地。
- 触发条件（什么时候重开本 ADR）：某一步击穿它自己的 `K#` 且无法在不违反 `C#` 的前提下修复；或
  第二个后端出现使 `C3`/`C8` 的前提变化。

## 证据

- 提案与两轮审计：`../research/graphics-2026-11-11-next.md`（§4.5 `K1–K11`）、
  `graphics-2026-11-11-next-audit.md`（§8 五步、§11 落地表）、
  `graphics-2026-11-11-next-requirements.md`（`C1–C12` / `R1–R39` / `N1–N10` / `T1–T31`）。
- 已落地的实测：`SnapshotTest.MeasureWhatOneEditCostsToPublishAndSelect`（RelWithDebInfo 203 µs / 293 µs，
  常驻 1.03× 行表）；`scripts/measure_graphics_claims.py`；`test_graphics` 559 / `test_vsg` 540 / ctest 30/30。
- 已落地的机制：`sdk/vine/graphics/ContentId.hpp`（`Handle` / `Id<Tag>` / `ContentRef` / `ContentRegistry`）、
  `Node::id()`、`ContentStore` 的行级 epoch 回收、`detail::SceneSnapshot` + `selectSnapshot`（原型）。
