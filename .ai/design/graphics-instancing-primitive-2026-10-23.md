# 实例化：`Primitive` 级实例流（2026-10-23）

> **状态：已落地（2026-10-23）**。本文取代同日的 `InstanceScope` 作用域设计
> （`graphics-instancing-scope-2026-10-22.md`，历史记录）。核心变化：实例化不再是场景图里的
> 作用域，而是 **一条 draw（`Primitive`）的参数**。
>
> 关联：`.ai/design/graphics-object-model-vs-3d-frameworks.md` §3 缺口 2、
> `.ai/design/graphics-followups-2026-10-19.md` §3.3、OSG 的 `PrimitiveSet::numInstances`、
> vsg 的 `Geometry` / `DrawIndexed` / `InstanceNode`（`build/_deps/vsg-src`）。

## 0. 结论

| # | 决定 | 理由 |
| --- | --- | --- |
| **P1** | 实例数据挂在 **`Primitive`** 上，不挂在 `Geometry`，也没有 `InstanceScope` 节点 | `Primitive` 已经是“一次 draw”（topology + index span + material/program）；实例化是这条 draw 的参数，不是场景结构 |
| **P2** | 载荷 = 每实例 16 float 列主序 mat4；`BufferSlice<float>` 持缓冲，`std::optional<size_t>` 持可见前缀 | 与 OSG `PrimitiveSet::numInstances` 同层；多个 primitive 可共享同一 buffer 的不同切片；一个 draw 只读自己那段 |
| **P3** | 收集期按 `Geometry` 的每个 `Primitive` 发一条命令，命令快照实例切片 + `first_instance` + `instance_count` | 没有子树折叠、没有作用域指针、没有“最近作用域”规则；一条 primitive 的实例数据属于它自己的命令 |
| **P4** | 后端仍用 `ProgramVariant.instanced`（bit 8）+ binding 4 的整段实例流 + `VINE_INSTANCED` | 已验证的顶点输入路径不变；变的只是“实例流从哪里来” |
| **P5** | 包围盒 = 每条 primitive 的可见实例矩阵变换几何局部盒后的并集；拾取逐 primitive、逐可见实例测试 | 选择结果唯一：`geometry + primitiveIndex + instanceIndex`；没有 `node=scope` 的歧义 |
| **P6** | 多 part = 一个 `Geometry` 的多个 `Primitive`；多个放置 = 多个 `Geometry` 节点（可共享 Buffer） | `Geometry` 本来就是 leaf node，一个 Geometry 只能有一个 parent；共享的是 Buffer，不是节点 |
| **P7** | 不做：逐实例材质/颜色/opacity/层/排序/剔除、GPU indirect、自动合批；`Topology` 仍只表达 list | 这些是后续能力；`Topology` 的 strip/fan 是独立缺口，不在本设计里偷偷扩展 |
| **P8** | 与 OSG/vsg 的关系：借 OSG 的“primitive set 是一次 draw、numInstances 在 draw 上”，不借 vsg 的 `InstanceNode` 作用域；vsg 的 `Geometry` 是“一个索引缓冲 + draw 命令列表”，本设计更接近 OSG 的 per-primitive 索引 | 与当前 `PrimitiveFacts` / `ContentPass` 的 per-primitive 分派一致；不引入新的 drawable 身份 |

## 1. 为什么放弃 `InstanceScope`

`InstanceScope` 允许多个子节点（除直接嵌套 scope），带来三个连锁问题：

1. **选择单位不稳定**：命中记录必须区分“具体几何”和“选择单位”；多 part 时点中一个 part 却选中整组。
2. **射线求交递归**：三条遍历都要携带作用域指针，经过子 Group 继续下钻；间接嵌套后出现“最近作用域赢”。
3. **包围盒语义不一致**：外层 scope 的 `subtreeBounds` 会把内层 scope 的子树再乘一次外层矩阵，而绘制/拾取只用最近作用域。

Primitive 级没有这些问题：一条 primitive 就是一次 draw，实例流、索引段、材质/程序都在同一个粒度上。

## 2. 契约

### 2.1 `Primitive`

`Primitive` 继续是值对象，不引入子类/虚函数：

```cpp
struct Primitive
{
    Topology topology;
    IndexSpan indices;                 // BufferSlice<uint32_t>

    intrusive_ptr<Material> material;
    intrusive_ptr<ShaderProgram> program;

    BufferSlice<float> instance_transforms;   // 16 float/instance, column-major
    std::optional<std::size_t> visible_instances;
};
```

- `hasInstances()` = 至少一个完整 mat4；
- `instanceCount()` = 切片容量（16 个 float 算一个实例）；
- `firstInstance()` = 切片起点 / 16；
- `visibleInstanceCount()` = 可见前缀，夹紧到容量；
- `instanceMatrix(i)` = 第 i 个实例的 `Mat4d`（相对切片）；
- `setInstanceTransforms(slice)` 校验起点和长度都是 16 的倍数；空 slice 清除；
- `setVisibleInstanceCount(n)` 只改 draw 参数，不换缓冲、不重传。

**为什么是值对象而不是 `InstancedPrimitive` 子类**

- topology / indexed / instanced / material / program 是正交维度，继承会产生
  `IndexedInstancedTrianglePrimitive...` 的组合爆炸；
- `Geometry::primitives()` 返回 `std::span<const Primitive>`，后端按 `PrimitiveFacts` 读字段；
  派生类会迫使整条链改成指针/visitor；
- 实例化没有独立行为，只有数据；行为仍在 `ContentPass`。

### 2.2 `Geometry` 便利入口

`Geometry` 保留 per-primitive API，并给默认 primitive 0 的快捷拼写：

- `setInstanceTransforms(index, slice)` / `setInstanceTransforms(slice)`；
- `setVisibleInstanceCount(index, n)` / `setVisibleInstanceCount(n)`；
- `clearVisibleInstanceCount(index)` / `clearVisibleInstanceCount()`；
- `clearInstanceTransforms(index)` / `clearInstanceTransforms()`。

这些方法只修改对应 `Primitive` 并走 `setPrimitive`（因此会 `invalidateBounds()` / 公告内容变化）；
它们不拥有第二份实例状态。

### 2.3 收集期

`Scene::collectNodeCommands` 保持普通树遍历：

- 遇到 `Geometry`：对每个 `Primitive` 发一条命令；
- `primitive->hasInstances() && visibleInstanceCount() == 0` ⇒ 跳过该 primitive（不是整颗 Geometry）；
- 否则把 `primitive->instance_transforms` / `firstInstance()` / `visibleInstanceCount()` 快照进命令；
- 没有实例流 ⇒ 普通命令，`instance_count = 0`。

`RenderCommand` / `CollectedCommand` / `CompiledCommand` 继续带：

```cpp
BufferSlice<float> instance_transforms;  // scene command only
const vn::Buffer<float>* instance_buffer; // recorder/compiler: borrowed identity
std::uint64_t instance_revision;
std::uint32_t first_instance;
std::uint32_t instance_count;
```

没有 `instance_scope` 字段，也没有折叠上下文。

### 2.4 后端

`ContentPass::recordCommand` 不变：读命令的 `instance_buffer` / `instance_count` /
`first_instance`，拒绝不合法范围；`ContentPipeline` 仍由 `VINE_INSTANCED` 决定 binding 4
和属性 12–15。实例流上传 key 必须是**整段 buffer**（offset 0/count 0），可见前缀只改 draw 参数。

### 2.5 包围盒与剔除

`Geometry::subtreeBounds` 对每条 `Primitive`：

- 没有实例流：并上几何局部盒（一个放置）；
- 有实例流：对每个可见实例，并上 `transformBox(data_box, instanceMatrix(i))`。

`boundingBox()` 再把局部盒乘 `worldMatrix()`。`Scene` 不需要“作用域是剔除单位”的特殊分支；
每个 Geometry 的盒已经包含自己的实例放置。

### 2.6 拾取与选择

`RayIntersectionResult`：

```cpp
intrusive_ptr<Geometry> geometry;
std::uint32_t primitiveIndex;
std::uint32_t instanceIndex;  // max() when the primitive has no stream
```

`forEachTriangleHit` 对 triangle primitive 循环可见实例：

```text
world_instance = geometry_world * primitive->instanceMatrix(instance)
```

命中即可唯一回答“哪个几何、哪个 primitive、哪个实例”。`ModelRenderView` 直接按
`hit.geometry` 高亮，不再遍历作用域子树；逐实例高亮仍做不到（材质共享）。

### 2.7 生命周期与编辑

- `Primitive` 的 `BufferSlice` 持缓冲引用；命令快照切片，所以命令不依赖 Primitive 的寿命。
- 改字节后 `Buffer::bumpRevision()`；替换切片/可见数走 `Geometry::setInstanceTransforms` /
  `setVisibleInstanceCount`，它们会 `setPrimitive` 并 `invalidateBounds()`。即使切片身份不变，重新
  `setInstanceTransforms(same slice)` 也算一次编辑（缓冲里的字节可能已移动），会显式失效。
- 只原地 bump buffer revision、既不调用 setter 也不 `Geometry::bumpRevision()` 时，包围盒/剔除缓存
  不会自动失效；需要精确剔除时调用 `Geometry::bumpRevision()` / `invalidateBounds()`（与 mesh 字节编辑
  的现有契约一致）。

## 3. 边界

- 一个 `Geometry` 的所有 primitive 共享同一个节点世界矩阵；需要不同整体变换就多个 Geometry 节点。
- 多个 Geometry 节点可以共享同一个实例 buffer；`StreamKey` 按 buffer/revision 共享上传。
- 一个 primitive 有多个实例，材质/层/opacity/排序在一条 draw 内必须相同。
- `Topology` 仍是 `Triangles` / `Points` / `Lines` 的列表形式；strip/fan 需要单独扩展枚举和
  拾取/管线映射，不在本设计里伪支持。
- 没有 `InstanceScope`，所以没有嵌套作用域、作用域级选择或作用域级剔除。

## 4. 落地记录

- SDK：`Primitive` 增加实例流字段/方法；`Geometry` 增加 per-primitive 便利方法；
  `RenderCommand`/`FrameRecorder`/`FrameCompiler` 字段保持命令快照；删除 `InstanceScope.hpp/.cpp`。
- 收集：`Scene` 逐 primitive 快照实例流；`Geometry::subtreeBounds` 按可见实例展开。
- 拾取：`RayIntersection` 逐 primitive/逐可见实例；`RayIntersectionResult` 去掉 `node`，
  只留 `geometry + primitiveIndex + instanceIndex`；`ModelRenderView` 按 geometry 选择。
- 后端：binding 4 / `VINE_INSTANCED` / `first_instance` 路径复用；设备用例
  `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`。
- 测试：`PrimitiveInstancingTest` 覆盖单/多 primitive、可见前缀、包围盒、剔除、拾取；
  `test_graphics` 448/448、`test_vsg` 503/503、`test_gui` 260/260（1 skip：无显示）。
- 文档：本文件取代 `graphics-instancing-scope-2026-10-22.md`；
  `.ai/README.md`、`graphics-followups-2026-10-19.md`、对象模型复核与 `docs/guides/graphics.md` 指向本设计。
