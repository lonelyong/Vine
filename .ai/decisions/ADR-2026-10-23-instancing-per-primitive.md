# ADR-2026-10-23：实例化挂在 `Primitive`，不做场景图作用域

- 状态：已采纳
- 日期：2026-10-23
- 相关：`../design/graphics-geometry.md`、`../backlog/graphics.md`

## 背景

“同一几何 N 份”有三种可能形状：实例矩阵作为 `Geometry` 的通道；`InstanceScope` 作用域
节点；或者一条 draw 的参数。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. `Geometry` 通道 | 与顶点流统一 | 一个几何只能一套实例；可见数要改切片；没有引擎把实例挂在网格资产上 |
| B. `InstanceScope` 作用域 | 与 vsg `InstanceNode` 同形；多 part 共享 | 选择单位、射线递归、嵌套、包围盒语义复杂 |
| C. `Primitive` 参数 | 一条 draw 就是一次实例化；与 topology/index/material 同层 | 多个放置 = 多个 Geometry 节点 |

## 决定

选 **C**：`Primitive` 增加 `instance_transforms`（16 float/实例，列主序 mat4）+
`visible_instances`；收集按 primitive 出行；后端 binding 4 + `VN_INSTANCED`；
没有 `InstanceScope`。

## 后果

- 好：选择身份唯一为 `geometry + primitiveIndex + instanceIndex`；一条 draw 的实例共享
  材质/层/排序；可见前缀不重传。
- 代价：逐实例材质/颜色/opacity/层/排序/剔除、GPU indirect、自动合批都不做；多个放置
  要多个 `Geometry` 节点（可共享 buffer）。
- 重开条件：逐实例属性或 GPU indirect 有真实消费者。

## 证据

- `PrimitiveInstancingTest`（单/多 primitive、可见前缀、包围盒、剔除、拾取）。
- 设备级 `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`。
- 早期 `graphics-instancing-channel` / `graphics-instancing-scope` 方案已作废；历史在 git。
