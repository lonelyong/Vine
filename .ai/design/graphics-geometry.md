# graphics 几何：通道、图元、实例、revision

> 单一来源：`Geometry` / `Primitive` 的数据模型。头文件 `Geometry.hpp` / `Primitive.hpp` /
> `Buffer.hpp` 是 API 细节的权威。待办见 `graphics-followups.md`。

## 1. 两个对象

- **`Geometry`（叶子节点）= 通道集**：位置、法线、贴图坐标、自定义属性；所有 primitive 共享这些流。
- **`Primitive` = 一次 draw**：topology + 索引片段 + material / program + 实例流。一个几何可以有多条 primitive
  （网格 + 边线 + 选中点共享一份顶点，就是三条 draw）。
- `Geometry` 没有子节点、没有自己的变换；放置靠外层变换节点。可见性/透明度归 `Node`。

## 2. 通道

- `AttributeChannel` = `BufferSlice<float>` + `components`（每顶点标量数，1..4）。
- 通道是**段 + 步长**：`offset` 以标量计，`scalarCount == 0` 表示“从 offset 到缓冲末尾”。
  `BufferSlice` 的切法只有一处（越界钳位、0 = 剩余、长度跟随增长）。
- `setPositions` / `setNormals` / `setTexcoords2` / `setTexcoords3` 共享调用方的 buffer，不拷贝；
  分段用 `first_vertex` / `vertex_count`（单位是顶点）。通用入口是 `addBuffer(location, AttributeChannel::slice(...))`。
- 一个 arena buffer 可以装多个几何的顶点：每个几何只读自己的段，显存里只有一份。
- 自定义通道从 location 3 起；规范槽位由 `ShaderAbi.hpp` 的 `attributeLocation()` 定义，不能占用。
- 组件数就是步长：`vertexCount()` / `xyz()` / `stride()` 都按它走；vec4 位置保留 xyz、跳过 w。

## 3. Primitive

```cpp
struct Primitive {
    Topology topology;              // Triangles / Points / Lines（列表）
    BufferSlice<uint32_t> indices;  // 空 = 非索引
    intrusive_ptr<Material> material;   // null = 几何级默认
    intrusive_ptr<ShaderProgram> program; // null = 几何级默认
    BufferSlice<float> instance_transforms; // 16 float/实例，列主序 mat4
    std::optional<std::size_t> visible_instances; // 可见前缀
};
```

- 索引流按 buffer 共享上传；同一 arena 的多个 primitive 可以指向不同段。
- `Geometry::material` / `program` 是**几何级默认**，不是批量写：图元自己的值优先，再否则最近 `StateNode`。
- 拓扑是 pipeline 身份的一部分，不是动态状态；pass 可以把整片内容“当点看”（`PassRenderState::topology` 覆盖）。

## 4. 实例化

- 实例流挂在 `Primitive` 上，是一条 draw 的参数，不是场景图作用域；没有 `InstanceScope`。
- 每实例 16 float 列主序 mat4；`firstInstance` = 切片起点 / 16；`visibleInstanceCount` 只改 draw 参数，不重传。
- 上传键是**整段 buffer**（offset 0 / count 0），按 buffer revision 共享；可见前缀不换 buffer。
- 收集：`Scene` 对每个 primitive 发一条命令；有实例流且可见数为 0 只跳过该 primitive。
- 后端：binding 4 的逐实例顶点流 + `ProgramVariant.instanced`（bit 8）+ `VINE_INSTANCED`；
  实例矩阵在**该 draw 的几何局部空间**里乘。
- 包围盒：每条 primitive 的可见实例矩阵变换局部盒后的并集；拾取逐 primitive、逐可见实例。
- 边界：逐实例材质/颜色/opacity/层/排序/剔除、GPU indirect、自动合批都不做；一条 draw 的实例共享材质/层/排序。
  多个放置 = 多个 `Geometry` 节点（可共享同一实例 buffer）；多个 part = 多个 `Primitive`。

## 5. revision：形状归几何，字节归 buffer

- `Geometry::revision()` 只描述**形状**（喂哪些通道、从哪读）；`setRevision()` / `bumpRevision()` 是唯一推进入口。
- `Buffer::setRevision()` 是**字节**公告；上传缓存只认它。改了字节却只动几何 revision，会继续画旧字节。
- 事实行（layout / 通道集 / counts）在几何 revision 变化时重建；通道/索引/实例流的新身份会自然触发上传。
- 一个编辑批次只公告一次几何 revision；setter 自己不会推进它。

## 6. 包围盒

- `Geometry::localBounds()`：位置通道的局部盒，按（buffer、buffer revision、段、几何 revision）缓存；
  `localBoundsComputationCount()` 是缓存命中见证。
- `Geometry::subtreeBounds(box)`：几何自身坐标系里的盒，按每条 primitive 的可见实例展开。
- `Geometry::boundingBox()`：局部盒乘父链世界矩阵（视图无关的放置）；`Scene::boundingBox` 只算可见/层掩码内容。
- 移动相机不重算子树盒；移动/编辑子树只失效祖先。

## 7. 边界（不是缺陷）

- 场景图是严格树：不做多父 / DAG；重复子树复制节点即可，`Buffer` 仍共享。
- `Topology` 只有 `Triangles` / `Points` / `Lines` 列表；strip/fan 是独立缺口（见 ledger）。
- 一个 `Geometry` 的所有 primitive 共享节点世界矩阵；需要不同整体变换就多个 Geometry 节点。

## 8. 判据

`GeometryTest` / `PrimitiveTest` / `PrimitiveInstancingTest` / `StreamRevisionTest` / `GeometryFactsTest` /
`PrimitiveDrawTest` / `SceneTest.TheDrawResolvesPrimitiveThenGeometryThenStateNode`，
以及设备级 `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`。
