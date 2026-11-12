# ADR-2026-11-12：快照接管生产收集拆成 5a 影子切换、5b-1 切换真相与 5b-2 删除

- 状态：已采纳（迁移形状；5a 影子接缝、5b-1 生产切换与 5b-2a 公开入口已落地 2026-11-12，5b-2b 未落地）
- 日期：2026-11-12
- 相关：`../design/graphics-frame-pipeline.md` §9.3；`../research/graphics-2026-11-12-instances.md`；
  `tests/test_graphics/RenderWorldTest.cpp`（镜像 / delta / 烘焙的现役用例）

## 背景

§9.3 的第 5 步是唯一“删掉约 2000 行现役形状”的步骤：生产收集从 `ContentMirror` + `DeltaChannel` +
`DrawListMemo` + `Version` 改走 `detail::SceneSnapshot` + `selectSnapshot`。`K1`（203 µs）、`K2`（1.03×）、
`K3` 只证明**原型的快照与派生够快**，没有证明**在飞帧逐项等价**；`T17` 在 snapshot 侧也还没有覆盖
生产路径的全部形状。把“证明等价”和“删除旧路径”放进同一个提交，失败时无法归因，回退面也过大。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 一步完成：切换 + 删除 | 提交数少 | 等价性与删除两种风险叠加；失败无法归因（`G11`）；回退要恢复 2000 行 |
| B 5a 影子切换，5b-1 切换真相，5b-2 删除 | 5a 只证明等价、不动生产形状；5b-1 只翻源、旧路径仍是影子；5b-2 机械删除，回退面小（`G3` / `G10`） | 迁移期两套路径并存；5a / 5b-1 的 diff 要进 CI |
| C 拆三步：mirror / delta / memo 各一步 | 每步更小 | 中间态是死代码，每个都要独立的寿命 / 版本故事，且没有独立判据（`G8`） |
| D 保留 mirror，只在快照上做新功能 | 不动现役路径 | 两个真相；复杂度没有消失，只是换了名字（`G1` / `G7`） |
| E 只在 debug 里双跑 | 不进生产 | 不能作为门禁；release 行为没被证明，debug-only 路径会腐烂（`G11`） |

## 决定

采用 B。

- **5a = 影子切换**：`selectSnapshot` 与旧 mirror 路径同帧同输入跑，逐项 diff（row 身份、world、
  material / program ref、sort key、instance id）；旧路径仍是渲染真相。不删生产形状、不公开
  `Scene::publish()`、不删 `addPass` 家族（`C11`）。diff 模式必须在 CI 的至少一个构建里常开
  （建议 RelWithDebInfo 的 `test_graphics`），不能只靠手跑。
- **5a 出口**：全量 `test_graphics` / `test_vsg` 绿 + `T17` 旧帧隔离绿 + `T34`（固定场景连续 ≥600 帧逐项 diff 为 0）
  + `K1` / `K2` / `K3` / `N3` / `K7` / `N5` 绿 + 既有语义断言改动为 0。
- **5b-1 = 切换真相（2026-11-12 已落地）**：Published 模式绑定 `snapshotSource()`；旧 rows / delta 路径降为
  可选 LEGACY 影子（`setLegacyShadow` / `legacySource`，默认关闭），`FrameMode::Mirrored` 改名
  `FrameMode::Published`。本步不删生产形状、不公开 `Scene::publish()`、不删 `addPass` 家族（`C11`）。
- **5b-2a = 公开入口 + 源归位（2026-11-12 已落地）**：公开 `Scene::publish()`（三态
  `Posted`/`Unchanged`/`InsideBatch`）；`FrameWorker` 直接持有 `SnapshotSource`，不再经过 `ContentMirror`。
  本步不删 LEGACY 文件/用例，它们仍在树内但已不在生产路径。
- **5b-2b = 删除旧路径**：删 `ContentMirror` / `DeltaChannel` / `RenderWorld` / `RenderWorldSource` /
  `PublishOutcome` / `SceneRows::version/publish/editing`（约 2000 行，含 LEGACY 用例）+ `addPass` 家族 +
  `PassRegistry` 的运行时拒绝。`DrawListMemo` 保留为 `SnapshotSource` 的纯 per-view 缓存；
  `Scene::refreshBoxes` / `refreshMaterials` 保留（快照的输入）。`K4` 的分母在 5b-2b 开始时重取；
  `K5` 前后 `wc -l` 对比；`K7` 必须在新路径上绿。只有 5b-2a 稳定后进 5b-2b。

## 决定补充：A5 必须回答的四件事

1. **在飞帧读什么**（`G3` / `I4`）：5a / 5b-1 / 5b-2 的答案是同一份不可变 Snapshot 值；旧帧持有旧快照的引用，
   新发布只换指针。`K7` 的判据是 `T17` 在 snapshot 侧逐项 diff（材质 / 几何 / 程序）为 0；
   没有这条用例就不写“可删”。
2. **谁 publish**（`G2`）：5a / 5b-1 仍由 `EngineCompile` 在帧内隐式发布；5b-2a 公开
   `Scene::publish()` 返回三态结果；5b-2b 让 `EngineCompile` 不再自动调用 `worker_->publish`，帧内隐式发布
   消失。公开面只在 5b-2a 变。
3. **`DrawListSource` 的去向**（`G5`）：保留这个**一个问题**的接口；`SnapshotSource` 在 5b-1 就是生产实现，
   5b-2 删掉 `ContentMirror` / `RenderWorldSource` 后它仍是同一个接口。`FrameDescription::content` /
   `expandPass` 的契约不变，等价性测试仍能问两个实现。不把它改成公开的新名词（`G8`：今天的消费者就是
   `expandPass`）。
4. **删除面与基线**（`K5`）：`wc -l` 记下 `ContentMirror` / `DeltaChannel` / `RenderWorld` 的现数
   （与 2519 同口径：`.hpp` + `.cpp`），5b-2b 前后各一次；5a / 5b-1 / 5b-2a 只增不删，`K5` 只在 5b-2b 判。

## 后果

- 好的：等价性、翻源、公开入口与删除分开，失败可归因（`G11`）；5a 的语义断言改动必须为 0，`K4` 的分子不会因为
  “顺手重构”而膨胀；5b-2b 是机械删除，回退面小（`G10`）。
- 坏的 / 代价：迁移期两套路径并存，5a / 5b-1 的 diff 增加 CI 时间；5b-2b 之前代码量上升，`K5` 只在 5b-2b 收口时判。
- 触发条件（什么时候重开）：
  - 5a / 5b-1 的 diff 无法到 0 ⇒ 停，回到设计层重审快照模型，不进入 5b-2b；
  - diff 的 CI 成本不可接受 ⇒ 至少保留一个 gating 场景在 CI，其余移到 nightly；
  - 出现 mirror / delta 的新一类 in-flight / 版本缺陷、第二后端要复用内容层、或收集进入性能预算 ⇒
    5b-2b 的代码接管提前（见 backlog）。

## 证据

- **5a + 5b-1（2026-11-12）**：`SnapshotSource`（快照 → `DrawListSource`，按 `SceneSnapshot::revision` 记忆化）、
  `DrawListDiff`（字段级比较）、`ContentMirror::snapshotSource()` / `legacySource()` /
  `FrameWorker::setLegacyShadow()`。5b-1 的生产绑定是 `snapshotSource()`；LEGACY 影子默认关闭。
  `T34`：`SnapshotShadowTest.TheSnapshotSourceMatchesTheMirrorAcrossSixHundredFrames`（600 帧，transform / opacity /
  material / placement 编辑）+ `FrameWorkerTest.TheSnapshotShadowComparesEverySceneDrawAndFindsNoDifference`；
  `SnapshotShadowTest.AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith` 补齐 `T17` 的 snapshot 侧（材质 / 几何 /
  程序三种 revision）；`SnapshotShadowTest.ADifferentSnapshotIsReportedAsADifference` 证明比较不是恒真。
  `test_graphics` 577 / `test_vsg` 542 / `ctest` 30/30 / TSan 0 警告。
- **5b-2a（2026-11-12）**：公开 `Scene::publish()` 三态；`FrameWorker` 直接持有 `SnapshotSource`，
  `T31` 由 `FrameWorkerTest.PublishAnswersPostedUnchangedAndInsideBatch` 钉住；`test_graphics` 577 /
  `test_vsg` 542 / `ctest` 30/30 / TSan 0。
- 5b-1 后分母：`test_graphics` 577 + `test_vsg` 542 = 1119；5b-2b 开始时重取。
- 绑定删除面的用例初判 17 条（`ContentMirrorTest` 11 + `DeltaChannelTest` 5 + `DrawListBakeTest` 1），
  占 1.5%；即使把 `FrameWorkerTest` 的 4 条发布路径用例算上也只有 1.9%，5% 线不会成为 5b-2 的约束。
- 相关判据：`T17` / `K1` / `K2` / `K3` / `K7` / `N5`；`K5` 的 2519 行基线见
  `../research/graphics-2026-11-11-next-audit.md` §2。
