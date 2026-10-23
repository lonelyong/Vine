# ADR-2026-10-23：不引入多父（DAG）

- 状态：已采纳
- 日期：2026-10-23
- 相关：`../design/graphics-scene.md`、`../design/graphics-geometry.md`、`../backlog/graphics.md`

## 背景

R7 的 DAG 半边：`Group::addChild` 目前自动从旧父脱离，场景图是严格树。问题是要不要在
`Node` 上允许多个父，从而共享整棵子树（而不是共享几何 buffer）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 全 DAG（OSG/vsg 式） | 任意共享；编辑一次所有放置生效 | `worldMatrix()` / `boundingBox()` / 拾取身份失去唯一答案；需要 `NodePath`；缓存/失效/生命周期全改 |
| B. 显式引用节点（USD 式） | 保留普通节点的单父语义；放置身份显式 | 新类型 + 四条 walk 的展开；holder 失效链 |
| C. 严格树 + 实例化 | 零风险；同一几何 N 份已覆盖 | 重复子树要复制节点；编辑不联动 |

## 决定

选 **C**：`Group::addChild` 继续“自动脱离旧父 + 拒绝成环”。同一几何 N 份用
`Primitive` 实例流；同一子树 N 份复制节点（`Buffer` 仍共享）。将来若出现“同一子树 N 份且
编辑一次全生效”的真实需求，按 B 重新设计，不放开 `Group`。

## 后果

- 好：`parent()` / `worldMatrix()` / `boundingBox()` / 拾取结果保持唯一；包围盒缓存与
  内容 epoch 继续按单父链成立。
- 代价：重复子树没有“编辑一次全生效”；复制节点。
- 重开条件：真实的共享子树编辑需求（不是“同一几何多放置”）。

## 证据

- OSG/vsg 的 DAG 都配 `NodePath`（拾取/选择身份）；vendored vsg 1.1.16 的 `Node` 甚至
  不存父指针，路径只活在遍历里。
- 代码里的单父假设：`Node::worldMatrix()`、`invalidateBounds()`、`BoundsCache`、
  `RayIntersectionResult`。
- 记录：`../backlog/graphics.md` T1–T2；R7 在 2026-10-23 关闭。
