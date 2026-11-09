# ADR-2026-11-09：场景收集拆成"烘焙"与"选取"两半

- 状态：已采纳（第一半落地，第二半待测量）
- 日期：2026-11-09
- 相关：`../design/graphics-scene.md`、`../backlog/graphics.md` T24、`../../src/viz/graphics/src/Scene.cpp`

## 背景

`Scene::collectRenderCommands` 用一次遍历回答两件事：**图**（谁被画、世界矩阵、opacity/layers/drawOrder/state 的
折叠）和**视图**（视锥剔除、排序）。memo 的键是视图，所以相机一动，图那一半也被丢掉重算——实测（Debug，
`SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera`）2000 drawables 移动相机 **13797 µs/帧**，而 memo 命中
1 µs。环绕/缩放是查看器的常态交互，所以这不是"以后优化"。

## 选项

| 选项 | 好处 | 代价 / 违反 |
|---|---|---|
| A 保持现状 | 零改动 | 每个视图付一次全量重算 |
| B 脏子树局部重走 | 听起来最省 | 结果不具子树局部性（全局排序、透明度分组、距离键、跨子树折叠）——`G1`：缓存粒度必须与结果的依赖同构，否决 |
| C **烘焙（图半）+ 选取（视图半）** | 按依赖边界切；复用现成的 `contentEpoch` 公告；公开 API 与计数器语义不变 | 多一份 O(节点) 的旁表 |
| D 给对象发全局稳定 id | 与"地址做键"那一族问题一起解决 | 与本问题无关（`PipelineKey`/`StreamKey` 的键并不是本次瓶颈），且已在 ADR-2026-11-09-address-keys-hold-the-object 中否决 |

## 决定

选 **C**，落在 `Scene` 内部（公开面只增一个诊断计数）：

- **烘焙**（键 = `root_->contentEpoch()`）：按遍历序存每个节点的**世界矩阵**与三个折叠（`opacity` / `layers` /
  `draw_order`），叶子再存解析后的 state/material/program 与 primitive 跨度。数组下标就是身份（两次遍历同一段
  代码驱动），`end` 让"剪掉一整棵子树"仍然一次跳过。
- **选取**（键 = 视图）：在旁表上迭代，做层并集剪枝、视锥测试、push 命令、排序。计数 `contentCollectCount`
  语义不变（"这个视图收集了一次"），所以既有用例的数字全部照旧成立。
- **视图依赖的子树不烘焙**：新增 `Node::viewDependence()`（`AutoTransform` 按自身开关声明最宽的那一档），
  这类 run 在选取时交给原遍历 `collectNodeCommands` 处理——不重写 AutoTransform 的定位逻辑。

## 两个实测教训（写下来，避免下次重犯）

1. **世界盒不能烘焙**。第一版把每个节点的世界盒也存进旁表，两条成本见证用例立刻红：
   `CollectCommandsAsksEachLeafBoundOnce`、`TheLocalDataBoxIsComputedOnceAndRecomputedWhenTheDataChanges`。原因不是
   用例过时，而是**盒子的键是流的 revision，不是节点的 epoch**：`Buffer::edit()` 不会惊动图，烘焙的盒子会一直
   用到某个无关的节点改动为止。修正：选取时仍经 `BoundsCache` 问盒子（每节点一次变换，局部盒已按流 revision
   缓存在节点上）。**能烘焙的只有"只由图决定"的事实。**
2. **收益没有预期大**：Debug 下 2000 drawables 移动相机 **13797 → 11113 µs（−19%）**，200 个 1267 → 1040 µs
   （−18%）。说明 13.8 ms 的大头不在矩阵乘和折叠，而在每视图的 `BoundsCache` 哈希表、每条命令的构造
   （3 个 `intrusive_ptr` 拷贝 + 128 B 矩阵 + `push_back`）与排序。`G10`：不能把"结构变好了"说成"性能解决了"。
3. **Release 复测（2026-11-10，决定性）**：同一个配方在 Release 下是 **2000 drawables 1597 µs**（0.94 µs/命令）、
   200 drawables **141 µs**、稳态帧（memo 命中）**0 µs**。即：**13.8 ms 是 Debug 放大 8.6 倍的结果，真实交互成本是 1.6 ms/帧**。
   归因（Release，2000）：box 变换 204 µs + 哈希往返 113 µs + 命令拷贝/排序/排序键 226 µs = 可解释 **543 µs（29%）**，
   select 体内每命令记账 **~1339 µs（71%）**——比例与 Debug 一致，所以"大头在每条命令的记账"不是 Debug 假象。

## 后果

- 好：视图不再做矩阵乘与折叠；两个 pass 差 `layer_mask` 时也共享同一份烘焙（同一帧里 shadow 与 main 以前各走
  一遍全树）。
- 好：`viewDependence()` 让"位置是否依赖相机"成为**可声明**的属性，取代"调一次看看"的协议（`G4`）。
- 代价：O(节点) 的旁表（2000 节点约 0.5 MB，图变才重建；叶子的 state/material/program 与容器同记录，若出现
  >1e5 节点的场景再拆 SoA）。
- 待办（T24 的下一步）：把每视图的 `BoundsCache` 哈希表换掉——**烘焙世界盒 + 一个"盒 stamp"**（stamp 含几何的
  数据 revision），比哈希表便宜且仍能在 buffer 编辑后失效。**Release 数字出来后这一步不做了**：它能省的是
  204 + 113 µs（2 成的 1.6 ms），代价是新增一条"buffer 编辑 → 场景"公告；触发条件改为"Release 下 1 万 drawables
  移动相机 > 4 ms"或"60 fps 预算下收集 > 2 ms"，写进 backlog T24。

## 证据

- `test_graphics` 全量 **496 条全绿**（含 44 条 `SceneTest`：顺序、剔除、opacity、layers、state 折叠、实例化、
  AutoTransform、memo 语义、两条成本见证）。
- 两条可重复配方：`SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera`（移动/静止/稳态帧）与
  `SceneTest.MeasureWhereACollectionSpendsItsTime`（按成分归因，含三个原语的直接计时）。Debug 与 Release 两种构建跑。
- `Scene::contentBakeCount()`：新计数，钉"一图一烘焙、每视图一收集"的稳态。
