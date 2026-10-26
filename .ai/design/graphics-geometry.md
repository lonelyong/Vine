# graphics 几何：通道、图元、实例、revision

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：`Geometry` 通道集、`Primitive` 一次 draw、实例流、revision 与存储共享。
> **权威来源**：`Geometry.hpp` / `Primitive.hpp` / `Buffer.hpp` / `ShaderAbi.hpp`。
> **待办**：`../backlog/graphics.md`

## 1. 概要设计

### 1.1 问题与背景

本文件定义 graphics 几何：通道、图元、实例、revision 的现行设计：先给对象与边界，再给功能、接口、约束与验收。

### 1.2 目标

- 把 §2 的设计要点落成 §3 的功能点、§4 的接口与 §8 的验收。
- 让“现在是什么”与“为什么这么定”分开：理由在 §2/§7，历史看 git。

### 1.3 非目标

- 不在本文件重复头文件里的完整签名；接口只给契约与指针。
- 不在本文件保存已关闭的历史或逐轮审查记录。

### 1.4 一句话结论

> `Geometry` 是通道集，`Primitive` 是一次 draw；实例流是这条 draw 的参数； 形状归几何、字节归 buffer。

## 2. 设计要点

2.1 **一个对象一个职责**：`Geometry` 只装通道；`Primitive` 只描述一次 draw；材质/程序是几何级默认。
2.2 **数据即绘制**：装位置通道时自动有一个默认三角形 primitive，避免“有数据却不知道该怎么画”。
2.3 **通道 = 段 + 步长**：`BufferSlice<float>` + components；切片规则只有一处（`BufferSlice`）。
2.4 **实例是 draw 的参数**：挂在 `Primitive` 上，不是场景图作用域；没有 `InstanceScope`。
2.5 **两级 revision**：`Geometry::revision()` 描述形状，`Buffer::revision()` 描述字节。
2.6 **包围盒按数据缓存**：局部盒键 = positions buffer + 其 revision + 段 + 几何 revision。

## 3. 功能点

### 3.1 通道

| # | 功能 | 说明 |
|---|---|---|
| F1 | 规范通道 | positions / normals / texcoords / colour；location 由 `ShaderAbi` 定义。 |
| F2 | 自定义通道 | location 3 起，`addBuffer(location, AttributeChannel)`。 |
| F3 | 共享 buffer | `setPositions` / `setNormals` / `setTexcoords2/3` 接收 `intrusive_ptr<const Buffer<float>>`。 |
| F4 | Arena 切片 | `first_vertex` / `vertex_count`；一个 buffer 装多个几何的顶点。 |
| F5 | 视图访问 | `scalars()` / `xyz()` / `vec3View()` / `vertexCount()` / `stride()`。 |

### 3.2 图元

| # | 功能 | 说明 |
|---|---|---|
| F6 | 一次 draw | `Primitive{topology, indices, material, program}`。 |
| F7 | 多 part | 一个 `Geometry` 多条 `Primitive`，共享顶点通道。 |
| F8 | 索引段 | `BufferSlice<uint32_t>`；空 = 非索引；同一 arena 可共享上传。 |
| F9 | 材质/程序默认 | 图元自己的值 > 几何默认 > 最近 `StateNode` > pass。 |
| F10 | 拓扑 | `Triangles` / `TriangleStrip` / `TriangleFan` / `Lines` / `LineStrip` / `Points`；是管线身份，不是动态状态（每种值一条 pipeline，尽管 Vulkan 允许同拓扑类内动态切换）。`isTriangleTopology()` 是“这是表面吗”的唯一判据（拾取用）。 |

### 3.3 实例

| # | 功能 | 说明 |
|---|---|---|
| F11 | 实例流 | `BufferSlice<float> instance_transforms`，每实例 16 float 列主序 mat4。 |
| F12 | 可见前缀 | `std::optional<size_t> visible_instances`；只改 draw 参数，不重传。 |
| F13 | 收集 | 每个 primitive 一条命令；可见数为 0 只跳过该 primitive。 |
| F14 | 后端 | binding 4 的逐实例流 + `VN_INSTANCED`（bit 8）+ `Draw.instances` / `firstInstance`。 |
| F15 | 包围盒 | 每条 primitive 的可见实例矩阵变换局部盒后的并集。 |
| F16 | 拾取 | 逐 primitive、逐可见实例；结果 `geometry + primitiveIndex + instanceIndex`。 |

### 3.4 revision 与包围盒

| # | 功能 | 说明 |
|---|---|---|
| F17 | 几何 revision | `setRevision()` / `bumpRevision()`；只表示形状变化。 |
| F18 | buffer revision | `Buffer::setRevision()`；字节公告，上传缓存只认它。 |
| F19 | 事实行重建 | 几何 revision 变化 ⇒ 重建 layout / 通道集 / counts。 |
| F20 | 局部盒缓存 | `localBounds()` + `localBoundsComputationCount()` 见证。 |
| F21 | 世界盒 | 局部盒乘父链；`subtreeBounds` 在节点自身坐标系。 |

### 3.5 存储与共享（geometry 模块）

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

## 4. 公开接口

### 4.1 `Geometry`

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

### 4.2 `Primitive`

| 接口 | 作用 |
|---|---|
| `topology` / `indices` | 拓扑与索引段 |
| `material` / `program` | 这次 draw 的覆盖 |
| `hasInstances()` / `instanceCount()` / `firstInstance()` / `visibleInstanceCount()` | 实例 |
| `setInstanceTransforms()` / `setVisibleInstanceCount()` | 实例编辑 |

### 4.3 `AttributeChannel`

| 接口 | 作用 |
|---|---|
| `packed()` / `shared()` / `slice()` / `fromSlice()` | 构造 |
| `scalars()` / `scalarSlice()` / `floatCount()` / `vertexCount()` / `stride()` | 读取 |
| `xyz()` / `vec3View()` | 顶点级视图 |

### 4.4 存储与共享

| 接口 | 作用 |
|---|---|
| `Buffer<T>::view()` / `size()` / `setRevision()` / `revision()` | 字节存储与公告 |
| `Mesh::positions()` / `normals()` / `texcoords()` / `positionsBuffer()` 等 | 类型化视图与共享句柄 |
| `geometryFromShape()` | `Shape` → `Geometry`，共享 buffer |
| `packAttribute()` / `packIndices()` | 类型化输入打包 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 形状归 `Geometry`，字节归 `Buffer`；一个通道一种布局。 |
| C2 | 实例流是 `Primitive` 的参数，不是 `Geometry` 的通道。 |
| C3 | revision 只在内容真正变化时推进；可见实例数是前缀。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 实例共享材质/层/排序；逐实例属性等真实需求再加。 |
| R2 | 多个放置 = 多个 `Geometry` 节点（可共享 buffer）。 |
| R3 | 包围盒/拾取身份 = `geometry + primitiveIndex + instanceIndex`。 |

### 5.3 不变量（N#）

| # | 注意项 |
|---|---|
| N1 | 改字节必须 `Buffer::setRevision()`；只改几何 revision 会继续画旧字节。 |
| N2 | 一组编辑只公告一次几何 revision；setter 自己不会推进它。 |
| N3 | 实例流上传键是整段 buffer（offset 0 / count 0）；可见前缀不换 buffer。 |
| N4 | 实例矩阵在**该 draw 的几何局部空间**里乘；一个 Geometry 的所有 primitive 共享节点世界矩阵。 |
| N5 | 一个 Geometry 只能有一个父；多个放置用多个 Geometry 节点（可共享 buffer）。 |
| N6 | 自定义通道不能占用规范 location。 |
| N7 | `POINT_LIST` 要求顶点阶段静态写 `gl_PointSize`。 |

## 6. 注意事项与陷阱

### 6.1 一事实一家

- 会怎么错：同一份状态被两处保存，改一处另一处过期。
- 怎么判：出现两份可写的值/两个可写的注册表。
- 怎么防：回到 §5 的 N#；只留一个 owner，其余只读。

### 6.2 后端/平台的边界

- 会怎么错：把平台差异写进引擎/应用层，换平台时到处分叉。
- 怎么判：出现“如果 Windows/X11 就……”的分支。
- 怎么防：平台事实留在后端，引擎只消费编译好的帧值。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | `InstanceScope` 作用域 | 选择单位、射线递归、嵌套与包围盒语义复杂；改为 `Primitive` 参数。详见 [ADR](../decisions/ADR-2026-10-23-instancing-per-primitive.md)。 |
| D2 | 实例矩阵作为 `Geometry` 保留通道 | 一个几何只能一套实例；可见数量要改切片；没有引擎把实例挂在网格资产上。详见 [ADR](../decisions/ADR-2026-10-23-instancing-per-primitive.md)。 |
| D3 | 多父 / DAG | 渲染路径可 DAG，但公开世界查询与拾取身份不成立；复制节点即可。详见 [ADR](../decisions/ADR-2026-10-23-no-multi-parent-dag.md)。 |
| D4 | 逐实例材质 / 颜色 / 层 / 排序 / 剔除 | 一条 draw 的实例共享这些；等真实需求再加。 |
| D5 | GPU indirect / 自动合批 | 没有消费者；实例化只解决“同一几何 N 份”。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `GeometryTest` / `PrimitiveTest` / `PrimitiveInstancingTest`。 |
| T2 | `StreamRevisionTest`：单通道编辑只重传该流；几何编辑重建事实行。 |
| T3 | `GeometryFactsTest` / `PrimitiveDrawTest`。 |
| T4 | 设备级 `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

旧名/旧路径只在 §7 已否决选项或 git 历史里出现；新代码按 §4 的接口写。

## 10. 参考

`graphics-scene.md` · `graphics-rendering.md` · `graphics-architecture.md` · `../backlog/graphics.md`
