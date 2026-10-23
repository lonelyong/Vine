# graphics 几何：通道、图元、实例、revision

> **一句话**：`Geometry` 是通道集，`Primitive` 是一次 draw；实例流是这条 draw 的参数；
> 形状归几何、字节归 buffer。
>
> **权威来源**：`Geometry.hpp` / `Primitive.hpp` / `Buffer.hpp` / `ShaderAbi.hpp`。
> **待办**：`graphics-followups.md`。

## 1. 设计要点

1.1 **一个对象一个职责**：`Geometry` 只装通道；`Primitive` 只描述一次 draw；材质/程序是几何级默认。
1.2 **数据即绘制**：装位置通道时自动有一个默认三角形 primitive，避免“有数据却不知道该怎么画”。
1.3 **通道 = 段 + 步长**：`BufferSlice<float>` + components；切片规则只有一处（`BufferSlice`）。
1.4 **实例是 draw 的参数**：挂在 `Primitive` 上，不是场景图作用域；没有 `InstanceScope`。
1.5 **两级 revision**：`Geometry::revision()` 描述形状，`Buffer::revision()` 描述字节。
1.6 **包围盒按数据缓存**：局部盒键 = positions buffer + 其 revision + 段 + 几何 revision。

## 2. 功能点

### 2.1 通道

| # | 功能 | 说明 |
|---|---|---|
| F1 | 规范通道 | positions / normals / texcoords / colour；location 由 `ShaderAbi` 定义。 |
| F2 | 自定义通道 | location 3 起，`addBuffer(location, AttributeChannel)`。 |
| F3 | 共享 buffer | `setPositions` / `setNormals` / `setTexcoords2/3` 接收 `intrusive_ptr<const Buffer<float>>`。 |
| F4 | Arena 切片 | `first_vertex` / `vertex_count`；一个 buffer 装多个几何的顶点。 |
| F5 | 视图访问 | `scalars()` / `xyz()` / `vec3View()` / `vertexCount()` / `stride()`。 |

### 2.2 图元

| # | 功能 | 说明 |
|---|---|---|
| F6 | 一次 draw | `Primitive{topology, indices, material, program}`。 |
| F7 | 多 part | 一个 `Geometry` 多条 `Primitive`，共享顶点通道。 |
| F8 | 索引段 | `BufferSlice<uint32_t>`；空 = 非索引；同一 arena 可共享上传。 |
| F9 | 材质/程序默认 | 图元自己的值 > 几何默认 > 最近 `StateNode` > pass。 |
| F10 | 拓扑 | `Triangles` / `Points` / `Lines`；是管线身份，不是动态状态。 |

### 2.3 实例

| # | 功能 | 说明 |
|---|---|---|
| F11 | 实例流 | `BufferSlice<float> instance_transforms`，每实例 16 float 列主序 mat4。 |
| F12 | 可见前缀 | `std::optional<size_t> visible_instances`；只改 draw 参数，不重传。 |
| F13 | 收集 | 每个 primitive 一条命令；可见数为 0 只跳过该 primitive。 |
| F14 | 后端 | binding 4 的逐实例流 + `VINE_INSTANCED`（bit 8）+ `Draw.instances` / `firstInstance`。 |
| F15 | 包围盒 | 每条 primitive 的可见实例矩阵变换局部盒后的并集。 |
| F16 | 拾取 | 逐 primitive、逐可见实例；结果 `geometry + primitiveIndex + instanceIndex`。 |

### 2.4 revision 与包围盒

| # | 功能 | 说明 |
|---|---|---|
| F17 | 几何 revision | `setRevision()` / `bumpRevision()`；只表示形状变化。 |
| F18 | buffer revision | `Buffer::setRevision()`；字节公告，上传缓存只认它。 |
| F19 | 事实行重建 | 几何 revision 变化 ⇒ 重建 layout / 通道集 / counts。 |
| F20 | 局部盒缓存 | `localBounds()` + `localBoundsComputationCount()` 见证。 |
| F21 | 世界盒 | 局部盒乘父链；`subtreeBounds` 在节点自身坐标系。 |

### 2.5 存储与共享（geometry 模块）

| # | 功能 | 说明 |
|---|---|---|
| F22 | `core::Buffer<T>` | `RefCounted`，内部组合 `std::vector<T>`；不是 `Object`，没有 vptr。 |
| F23 | 元素类型钉死 | 属性 = `float`，索引 = `uint32_t`；`Mesh` 存 `Buffer<float>` / `Buffer<uint32_t>`。 |
| F24 | 共享存储 | `geometryFromShape()` 传 `mesh->positionsBuffer()` 等，两侧读同一块分配；`packAttribute()` 给类型化输入。 |
| F25 | 手动公告 | `Buffer` 自己不 bump；写者改完 `setRevision(revision()+1)`；`Mesh::announceChange()`；`Geometry::setRevision()`。 |
| F26 | vsg 别名 | 后端用真 `vsg::vec3Array` 别名持有 Vine buffer 的 `Data`；stride = 数组元素大小，不是 buffer 元素大小。 |
| F27 | 指针身份判据 | 渲染证据对“拷贝 vs 共享”无感；只有 `dataPointer()` 同一性断言能区分。 |

**契约**：
- `Mesh` 的 `span` 是借用；任何增长都会使既有 view 失效。
- 通道持的是 buffer 而不是快照：源 buffer 增长后通道不悬空，长度与地址现取。
- 段的身份进两层缓存 key：共享绑定缓存 = 缓冲地址 + revision + offset + 长度；派生法线缓存另加索引 first/count。
- 推导量（零填充 texcoords、白色 opacity 载体、推导法线）仍自己分配真数组；vec4 位置有意不进别名。

## 3. 公开接口

### 3.1 `Geometry`

| 接口 | 作用 |
|---|---|
| `addBuffer()` / `removeBuffer()` / `buffer()` / `bufferCount()` / `bufferLocations()` | 通道 |
| `setPositions()` / `setNormals()` / `setTexcoords2/3()` / `has*()` / `*Count()` | 规范通道 |
| `addPrimitive()` / `setPrimitive()` / `clearPrimitives()` / `primitive()` / `primitives()` | 图元 |
| `material()` / `setMaterial()` / `program()` / `setProgram()` | 几何级默认 |
| `revision()` / `setRevision()` / `bumpRevision()` | 形状公告 |
| `vertexCount()` / `localBounds()` / `boundingBox()` / `subtreeBounds()` | 数据与盒 |
| `setInstanceTransforms()` / `setVisibleInstanceCount()` / `clear*()` | 默认 primitive 0 的实例便捷 |
| `geometryFromShape()` / `packAttribute()` / `packIndices()` | 桥与打包 |

### 3.2 `Primitive`

| 接口 | 作用 |
|---|---|
| `topology` / `indices` | 拓扑与索引段 |
| `material` / `program` | 这次 draw 的覆盖 |
| `hasInstances()` / `instanceCount()` / `firstInstance()` / `visibleInstanceCount()` | 实例 |
| `setInstanceTransforms()` / `setVisibleInstanceCount()` | 实例编辑 |

### 3.3 `AttributeChannel`

| 接口 | 作用 |
|---|---|
| `packed()` / `shared()` / `slice()` / `fromSlice()` | 构造 |
| `scalars()` / `scalarSlice()` / `floatCount()` / `vertexCount()` / `stride()` | 读取 |
| `xyz()` / `vec3View()` | 顶点级视图 |

### 3.4 存储与共享

| 接口 | 作用 |
|---|---|
| `Buffer<T>::view()` / `size()` / `setRevision()` / `revision()` | 字节存储与公告 |
| `Mesh::positions()` / `normals()` / `texcoords()` / `positionsBuffer()` 等 | 类型化视图与共享句柄 |
| `geometryFromShape()` | `Shape` → `Geometry`，共享 buffer |
| `packAttribute()` / `packIndices()` | 类型化输入打包 |

## 4. 注意项

| # | 注意项 |
|---|---|
| N1 | 改字节必须 `Buffer::setRevision()`；只改几何 revision 会继续画旧字节。 |
| N2 | 一组编辑只公告一次几何 revision；setter 自己不会推进它。 |
| N3 | 实例流上传键是整段 buffer（offset 0 / count 0）；可见前缀不换 buffer。 |
| N4 | 实例矩阵在**该 draw 的几何局部空间**里乘；一个 Geometry 的所有 primitive 共享节点世界矩阵。 |
| N5 | 一个 Geometry 只能有一个父；多个放置用多个 Geometry 节点（可共享 buffer）。 |
| N6 | 自定义通道不能占用规范 location。 |
| N7 | `POINT_LIST` 要求顶点阶段静态写 `gl_PointSize`。 |

## 5. 否决项

| # | 否决 | 理由 |
|---|---|---|
| D1 | `InstanceScope` 作用域 | 选择单位、射线递归、嵌套与包围盒语义复杂；改为 `Primitive` 参数。 |
| D2 | 实例矩阵作为 `Geometry` 保留通道 | 一个几何只能一套实例；可见数量要改切片；没有引擎把实例挂在网格资产上。 |
| D3 | 多父 / DAG | 渲染路径可 DAG，但公开世界查询与拾取身份不成立；复制节点即可。 |
| D4 | 逐实例材质 / 颜色 / 层 / 排序 / 剔除 | 一条 draw 的实例共享这些；等真实需求再加。 |
| D5 | GPU indirect / 自动合批 | 没有消费者；实例化只解决“同一几何 N 份”。 |
| D6 | `Topology` strip / fan | 需要单独扩展枚举与拾取/管线映射；不在本设计里伪支持。 |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | `GeometryTest` / `PrimitiveTest` / `PrimitiveInstancingTest`。 |
| T2 | `StreamRevisionTest`：单通道编辑只重传该流；几何编辑重建事实行。 |
| T3 | `GeometryFactsTest` / `PrimitiveDrawTest`。 |
| T4 | 设备级 `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`。 |

## 7. 相关文档

`graphics-scene.md` · `graphics-rendering.md` · `graphics-architecture.md` · `graphics-followups.md`
