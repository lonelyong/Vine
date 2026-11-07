# graphics 几何：通道、图元、实例、revision

> **状态**：现行
> **最后核对**：2026-11-07
> **范围**：`Geometry` 通道集、`Primitive` 一次 draw、实例流、revision 与存储共享。
> **权威来源**：`Geometry.hpp` / `Primitive.hpp` / `Buffer.hpp` / `ShaderAbi.hpp` / `Mesh.hpp`。
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
2.7 **几何数据统一 float**：网格与参数化 primitive 只有一套 float 实现，不设精度模板、不设 `Meshf` / `Meshd` 之类别名；
    索引为 `uint32_t`。几何数据都是局部坐标系下的形状描述，float 覆盖全部现有消费者（机器人 IO、渲染、碰撞），
    double 没有下游需求，而模板会把精度选择推给每个调用点。`Shape` 是运行时多态边界，一个 `Mesh` 就是全部网格的基类。
    满足 `G1` / `G4` / `G8`，并以 double 场景下须在调用方自行转换支付 `G10` 成本（今天无此消费者）。
2.8 **面拓扑由网格类型表达，三角形身份只有一处定义**：`Mesh` 只持有顶点属性与索引存储，不承诺面是三角形、四边形还是
    多边形（那是子类的）；但“这个网格由哪些三角形组成”只由 `Mesh::triangleCount()` / `Mesh::triangleCorners()` 回答，
    每个具体网格按自己 `toTriangleMesh()` 的同一条规则实现。三角形序号因此能跨转换稳定：它既是树的 primitive id，
    也是拾取结果的身份。`MixedMesh` 以 `face_offsets` 描述各面的 corner 范围。满足 `G1` / `G4` / `G6`，并以
    Quad/Mixed 每次查询按规则现算（不建三角形数组）支付 `G10` 成本。

2.9 **三角形树是缓存，不是内容**：`Mesh` 缓存一棵 `Bvh<double>`（`bvh()` / `hasBvh()` / `setBvh()` / `buildBvh()` /
    `clearBvh()`），与 `aabb_` 同一约定——它描述已存的三角形、不持有身份、被任何改变三角形的 setter 丢掉。
    树用 `double` 而顶点是 `float`：树不是几何数据，是查询用的导引结构（`G13`），而查询射线在所有消费者里都是
    `Rayd`；把射线窄化成 float 做盒测试会在远离原点处丢掉小叶子盒所需的精度，反过来把 float 顶点加宽进盒是精确的。
    满足 `G1` / `G13` / `G4`，并以每个网格多一个空 `Bvh` 的存储支付 `G10` 成本。

## 3. 功能点

### 3.1 通道

| # | 功能 | 说明 |
|---|---|---|
| F1 | 规范通道 | positions / normals / texcoords / colour；location 由 `ShaderAbi` 定义。 |
| F2 | 自定义通道 | location 3 起，`addBuffer(location, AttributeChannel)`。 |
| F3 | 共享 buffer | `setPositions` / `setNormals` / `setTexcoords2/3` / `setColors` 接收 `intrusive_ptr<const Buffer<float>>`。 |
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
| F23 | 统一 float | `Mesh` 属性存 `Buffer<float>`，索引为 `Buffer<std::uint32_t>`；没有精度模板，也没有 `Meshf` / `Meshd` 别名。 |
| F24 | 渲染边界 | `geometryFromShape()` 只把 `TriangleMesh` 交给转换器，属性 buffer 原样共享；`QuadMesh` / `MixedMesh` 在这个边界先转成 `TriangleMesh`，非法网格返回空几何而不抛异常。 |
| F25 | 手动公告 | 没人自动 bump：写者完成一次编辑后自己推进 `Shape::revision()`；持有 `Buffer` 并原地写的写者用 `Buffer::setRevision()`；draw 布局由 `Geometry::setRevision()` 负责。 |
| F26 | vsg 别名 | 后端用真 `vsg::vec3Array` 别名持有 Vine buffer 的 `Data`；stride = 数组元素大小，不是 buffer 元素大小。 |
| F27 | 指针身份判据 | 渲染证据对“拷贝 vs 共享”无感；只有 `dataPointer()` 同一性断言能区分。 |

### 3.6 网格面拓扑

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F28 | 统一三角网格 | `TriangleMesh` 由 `hasIndices()` 区分索引面与顺序三角形。 | 已落地 |
| F29 | 四边形网格 | `QuadMesh` 按固定对角线显式转换为 `TriangleMesh`。 | 已落地 |
| F30 | 混合多边形网格 | `MixedMesh` 以 offsets 分面，索引与 offsets 一起整体给出，并通过显式转换生成 `TriangleMesh`。 | 已落地 |
| F31 | 点云 | `PointCloud`：`setPositions()` 整体替换点集，`normals()` / `colors()` 可选，`computeAabb()` 缓存盒；约束 C8、R7，验收 T10。 | 已落地 |
| F32 | 三角形树缓存 | `Mesh` 缓存 `Bvh<double>`：`buildBvh()` 每个三角形一个 primitive（id 即三角形序号），`bvh()` 供射线查询；任何改变三角形或面拓扑的 setter 丢树。 | 已落地 |

#### 3.6.1 F28 统一三角网格

- **行为**：带索引时按三项一面；无索引时每三个顺序顶点一面；`triangleCount()` / `triangleCorners()` 由 `Mesh` 声明并规定序号（见 2.8），此类型按三项一面实现。
- **约束**（C5）：索引数量与顶点数量均须组成完整三角形，索引必须落在顶点范围内。
- **要求**（R4）：机器人 IO 只接受 `TriangleMesh`；基本体也是 float。
- **验收**（T6）：`MeshShapeTest` 覆盖两种存储形式及完整三角面约束。
- **注意**（N8）：indexed/soup 是同一类型的存储状态，不由 `ShapeType` 区分。

#### 3.6.2 F29 四边形网格

- **行为**：`QuadMesh` 直接继承 `Mesh`；索引或顺序 corner 每四项构成一面；`toTriangleMesh()` 返回带转换索引及顶点属性副本的三角网格。
- **约束**（C6）：corner 数必须为四的倍数；索引不得越界。
- **要求**（R5）：graphics 与 meshio 消费前验证网格，并复用该转换接口。
- **验收**（T7）：`MeshShapeTest.QuadMeshUsesFourCornersAndTriangulates` 验证转换结果。
- **注意**（N9）：转换结果是独立的 `TriangleMesh`，修改结果不回写来源 QuadMesh。

#### 3.6.3 F30 混合多边形网格

- **行为**：`face_offsets` 以 0 起始、以 corner 数结束，逐面给出边界；索引与 offsets 一起由 `setIndices()` / `setFaceOffsets()` 整体给出；`toTriangleMesh()` 按三角扇生成独立三角网格并复制顶点属性及名称。
- **约束**（C7）：offsets 单调不减且每面至少三个 corner；indexed corner 必须落在顶点范围内。
- **要求**（R6）：graphics、meshio 和模型查看器统计复用显式转换；offsets 不合法时转换不得访问其面数据。
- **验收**（T8）：`MeshShapeTest.MixedMeshValidatesOffsetsAndConvertsToTriangleMesh` 与 `MeshShapeTest.MixedMeshWithSeveralFacesConvertsEveryFace`。

**契约**：
- 网格只有一个填充入口：整组 `setPositions()` / `setNormals()` / `setTexcoords()` / `setColors()` / `setIndices()`（Mixed 再加 `setFaceOffsets()`），没有增量追加。
- `Mesh` 的 `span` 是借用；任何增长都会使既有 view 失效。
- 通道持的是 buffer 而不是快照：源 buffer 增长后通道不悬空，长度与地址现取。
- 段的身份进两层缓存 key：共享绑定缓存 = 缓冲地址 + revision + offset + 长度；派生法线缓存另加索引 first/count。
- 推导量（零填充 texcoords、白色 opacity 载体、推导法线）仍自己分配真数组；vec4 位置有意不进别名。

#### 3.6.4 F32 三角形树缓存

- **行为**：`buildBvh(builder)` 为每个三角形加一个 primitive，primitive 的盒是三个角点在 `double` 下的包围盒；建成后
  `bvh().forEachRay(ray, visit)` 给出候选三角形序号，`bvh().forEachOverlap(box, visit)` 给出候选盒相交的三角形。
  `setBvh()` 接收调用方自己配好构建器建出的树。`hasBvh()` 报告树是否已建。
  树建成即以共享指针发布（`sharedBvh()`）且此后不再修改：重建**发布新对象**，旧持有者继续读到一棵仍然有效的树，
  所以另一帧可以零成本共享同一棵（见 §2.9 与 `../research/2026-11-07-geometry-triangle-bvh-unified.md`）。
- **约束**（C9）：primitive id == `triangleCount()` 的三角形序号；`triangleCount()` / `triangleCorners()` 与
  `toTriangleMesh()` 同序；非法网格不建树。
- **要求**（R8）：只有改变三角形或面拓扑的 setter 丢树（`setPositions()` / `setIndices()` /
  `MixedMesh::setFaceOffsets()`）；法线、贴图坐标与颜色不丢。
- **验收**（T11）：`tests/test_geom_spatial/MeshBvhTest.cpp` 六项：三角形序号、与转换同序、建树与射线剪枝、
  失效规则、非法网格不建、接收外来树。
- **注意**（N15）：候选是超集——叶子盒是三点的包围盒，精确交仍由调用方用 `intersectTriangle()` 做（同 N10
  的三角扇限制）。

## 4. 公开接口

### 4.1 `Geometry`

| 接口 | 作用 |
|---|---|
| `addBuffer()` / `removeBuffer()` / `buffer()` / `bufferCount()` / `bufferLocations()` | 通道 |
| `setPositions()` / `setNormals()` / `setTexcoords2/3()` / `setColors()` / `has*()` / `*Count()` | 规范通道 |
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
| `Shape::revision()` / `setRevision()` / `bumpRevision()` | 形状内容代际：持有者建完或改完后自己推进 |
| `Mesh::vertexCount()` / `hasIndices()` | 顶点数与索引状态，不定义面拓扑 |
| `Mesh::setPositions()` / `setIndices()` / `indices()` | 共用顶点与 corner 索引存储（整体设置） |
| `Mesh::positions()` / `normals()` / `texcoords()` / `positionsBuffer()` 等 | float 视图与存储句柄；数组别名在 `Array.hpp` |
| `Mesh::triangleCount()` / `triangleCorners()` | 三角形身份：个数与三个顶点索引；具体网格各自实现（2.8） |
| `Mesh::bvh()` / `hasBvh()` / `setBvh()` / `buildBvh()` / `clearBvh()` | 三角形树缓存：建、接收、查、丢（F32） |
| `Mesh::sharedBvh()` | 把已发布的不可变树交给另一帧共享（不复制） |
| `TriangleMesh::isValid()` | 顺序或索引三角网格的完整性 |
| `QuadMesh::quadCount()` / `toTriangleMesh()` | 四边形面与显式三角网格转换 |
| `MixedMesh::faceOffsets()` / `faceOffsetsBuffer()` / `setFaceOffsets()` / `toTriangleMesh()` | 可共享的多边形边界与显式三角网格转换 |
| `Model::Entry::mesh` | `Mesh` 句柄；模型导入写入 `TriangleMesh`，绘制前在 `geometryFromShape()` 三角化；同一份网格可被多个节点引用 |
| `geometryFromShape()` | `Shape` → `Geometry`，共享 buffer |
| `packAttribute()` / `packIndices()` | 类型化输入打包 |

### 4.5 形状类型与模块边界

| 接口 | 作用 |
|---|---|
| `Box` / `Sphere` / `Cone` / `Cylinder` / `Ellipsoid` | 参数化基本体，尺寸均为 `float`，局部坐标系下描述；机器人 IO 直接读写它们 |
| `Mesh` / `TriangleMesh` / `QuadMesh` / `MixedMesh` | 网格类型层次；graphics 与 meshio 只接收这一套 float 网格 |
| `PointCloud` | 点云：位置 + 可选法线/颜色，无面拓扑；与网格共用 `Buffer<float>` 通道 |
| `Primitive` / `BrepShape` / `Shape` | 基本体与 B-rep 的共同基类；`revision` 三件套住在 `Shape` |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
|---|---|
| C1 | 形状归 `Geometry`，字节归 `Buffer`；一个通道一种布局。 |
| C2 | 实例流是 `Primitive` 的参数，不是 `Geometry` 的通道。 |
| C3 | revision 只在内容真正变化时推进；可见实例数是前缀。 |
| C4 | 几何数值只用 `float`：网格顶点属性、基本体尺寸、索引（`std::uint32_t`）均无精度变体。 |
| C5 | 三角网格索引为空表示顺序顶点；非空索引必须按三项分组且全部有效。 |
| C6 | Quad face corner 数须为 4 的倍数，索引不得越界。 |
| C7 | Mixed offsets 从 0 开始、单调不减、末项等于 corner 数，且每面至少 3 个 corner。 |
| C8 | 点云通道布局：位置与法线每点 3 个 float、颜色每点 4 个 float，均存在 `Buffer<float>` 里。 |
| C9 | 三角形树只描述已存三角形：primitive id == 三角形序号，任何改变三角形或面拓扑的 setter 都丢树；未构建的树不回答候选。 |

### 5.2 要求（R#）

| # | 要求 |
|---|---|
| R1 | 实例共享材质/层/排序；逐实例属性等真实需求再加。 |
| R2 | 多个放置 = 多个 `Geometry` 节点（可共享 buffer）。 |
| R3 | 包围盒/拾取身份 = `geometry + primitiveIndex + instanceIndex`。 |
| R4 | 机器人 IO 只接受 `TriangleMesh` 与 float 基本体，不留 double 变体。 |
| R5 | graphics 与 meshio 消费前验证网格，并复用 `toTriangleMesh()` 转换接口。 |
| R6 | 模型查看器统计与导出复用显式转换；offsets 不合法时转换不得访问其面数据。 |
| R7 | 点云的法线/颜色通道只在长度等于 `pointCount()` 时被消费，否则丢弃而不重新索引（与 N11 同一规则）。 |
| R8 | 需要批量射线时先用 `Mesh::buildBvh()` 建一次树再查，不在拾取循环里线性扫三角形。 |

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
| N8 | `TriangleMesh` 的 `ShapeType` 不表达 indexed/soup；使用 `hasIndices()` 判断。 |
| N9 | Quad 与 Mixed 的三角化结果只用于消费端，不改写原网格数据。 |
| N10 | 三角扇不适合凹多边形；需要凹面正确剖分时应先采用能保留正确三角化结果的输入。 |
| N11 | 通道只在“每顶点一项”时被消费：法线/UV/颜色长度与位置不一致时消费端丢弃该通道，`isValid()` 不因此为假。 |
| N12 | `Mesh::aabb()` 是缓存：位置变化（`setPositions()`）清空它，未计算前为空盒。 |
| N13 | geometry 不自行推进任何 revision：`set*` 换的是 buffer 对象（地址变即缓存 key 变），原地写字节的写者用 `Buffer::setRevision()`，形状整体变化由持有者 `Shape::bumpRevision()` 报告；三者不可互替（N2）。 |
| N14 | `PointCloud::isValid()` 只问“有没有点”，`hasVolume()` 恒为 false；`geometryFromShape()` 还不接点云，不要用没有索引的 `Mesh` 冒充它。 |
| N15 | `Mesh::bvh()` 是缓存：`setPositions()` / `setIndices()` / `MixedMesh::setFaceOffsets()` 清空它，法线/UV/颜色不清；未构建的树返回空候选，不退化成全扫。 |
| N16 | 已发布的树不可变：`buildBvh()` 重建时发布新对象，`clearBvh()` 只停止发布、不动已发出的对象，所以持有 `sharedBvh()` 的一方永远读不到半成品。 |

## 6. 注意事项与陷阱

### 6.1 多边形转换

- 会怎么错：直接按 offsets 或索引读 faces，非法输入可能访问越界。
- 怎么判：`isValid()` 为 false，或 offsets 末项与 corner 数不符。
- 怎么防：转换入口先检查有效性；凹多边形不能假定三角扇保持其形状。
### 6.2 三角形树

- 会怎么错：拿没建过的树查询，得到“没有命中”，看起来像射线没打中网格。
  怎么判：`hasBvh()` 为 false。
  怎么防：查询前建一次，并把 `hasBvh()` 当成前置条件。
- 会怎么错：树建成后绕过 `setPositions()` 原地改 positions buffer 的字节，树还在描述旧位置。
  怎么判：命中位置与显示位置错位。
  怎么防：原地写字节后自己调 `clearBvh()`（与 `Buffer::setRevision()` 同一类手工公告，N13）。
- 会怎么错：把候选当成最终命中。
  怎么判：选中了实际上没相交的三角形。
  怎么防：候选只用于跳过，判定仍用 `intersectTriangle()`（N15）。
## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | `InstanceScope` 作用域 | 选择单位、射线递归、嵌套与包围盒语义复杂；改为 `Primitive` 参数。详见 [ADR](../decisions/ADR-2026-10-23-instancing-per-primitive.md)。 |
| D2 | 实例矩阵作为 `Geometry` 保留通道 | 一个几何只能一套实例；可见数量要改切片；没有引擎把实例挂在网格资产上。详见 [ADR](../decisions/ADR-2026-10-23-instancing-per-primitive.md)。 |
| D3 | 多父 / DAG | 渲染路径可 DAG，但公开世界查询与拾取身份不成立；复制节点即可。详见 [ADR](../decisions/ADR-2026-10-23-no-multi-parent-dag.md)。 |
| D4 | 逐实例材质 / 颜色 / 层 / 排序 / 剔除 | 一条 draw 的实例共享这些；等真实需求再加。 |
| D5 | GPU indirect / 自动合批 | 没有消费者；实例化只解决“同一几何 N 份”。 |
| D6 | 把点云做成 `Mesh` 的子类 | `geometryFromShape()` 与导出器会看到一个没有面的网格，名字与行为都对不上（`G1`、`G4`）；点云是独立的 `Shape`（F31）。 |
| D7 | 只在 `TriangleMesh` 上建树 | `QuadMesh` / `MixedMesh` 是独立的消费者（模型查看器、meshio），只能先 `toTriangleMesh()` 复制一份才能查询（`G1`、`G10`）；三角形身份因此上移到 `Mesh`（2.8）。 |
| D8 | 树用 `float` 与顶点同精度 | 查询射线在所有消费者里都是 `Rayd`，窄化成 float 做盒测试会在远离原点处丢掉小叶子盒所需的精度而漏命中（`G4`、`G10`）；把 float 顶点加宽进盒是精确的，所以只有树是 double。 |
| D9 | 每个三角形物化成数值类型再建树 | 建树本来就要扫一遍，物化只会多一份需与网格同步失效的副本；`triangleCorners()` 按规则现算即可（`G10`、`G13`）。 |

## 8. 测试与验收

| # | 判据 |
|---|---|
| T1 | `GeometryTest` / `PrimitiveTest` / `PrimitiveInstancingTest`。 |
| T2 | `StreamRevisionTest`：单通道编辑只重传该流；几何编辑重建事实行。 |
| T3 | `GeometryFactsTest` / `PrimitiveDrawTest`。 |
| T4 | 设备级 `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`。 |
| T5 | `MeshShapeTest` 验证网格存储与运行时类型，包括一份 positions buffer 被两个 `Geometry` 共享。 |
| T6 | 三角网格索引与顺序顶点均验证完整三角面；`MeshShapeTest`。 |
| T7 | Quad 按固定对角线拆为两个三角形；`MeshShapeTest.QuadMeshUsesFourCornersAndTriangulates`。 |
| T8 | Mixed offsets 验证及三角扇输出；`MeshShapeTest.MixedMeshValidatesOffsetsAndConvertsToTriangleMesh`、`MeshShapeTest.MixedMeshWithSeveralFacesConvertsEveryFace`。 |
| T9 | 盒缓存与 revision 契约；`MeshShapeTest.TheCachedBoxFollowsTheVertices`、`MeshShapeTest.TheRevisionIsReportedByTheHolderNotByASetter`、`PkgIOTest.EditedMeshIsRefusedWhenThePackageIsFinallyWritten`。 |
| T10 | 点云：空云不可用、三标量打包与 buffer 同一性、通道独立、位置变化清空盒缓存；`PointCloudTest`。 |
| T11 | 三角形序号与 `toTriangleMesh()` 同序、一三角形一 primitive、射线拾取剪枝、失效规则、非法网格不建、接收外来树；`MeshBvhTest.cpp`（在 `tests/test_geom_spatial`）。 |

## 9. 状态与演进

### 9.1 当前状态

现行设计；功能点与接口以上表为准。

### 9.2 已知缺口 / 待办

见 `../backlog/graphics.md`；开放项按触发条件保留。

### 9.3 兼容性 / 迁移

索引与顺序三角形统一为 `TriangleMesh`，不再提供 `IndexedTriangleMesh` 类型；
网格与基本体不再提供精度模板，`Meshf` / `Boxd` 这类别名已删除，
新代码按 §4 的接口写。旧名只在 git 历史里出现。

2026-11-07：`Mesh` 新增两个虚函数的三角形身份（`triangleCount()` / `triangleCorners()`，基类默认 0 / false）与三角形树
缓存；`TriangleMesh::triangleCount()` 顺带成为其覆写，签名与语义不变（只是原先“仅此类型定义”变成基类声明）。

## 10. 参考

`Mesh.hpp` · `TriangleMesh.hpp` · `QuadMesh.hpp` · `MixedMesh.hpp` · `PointCloud.hpp` · `MeshShapeTest.cpp` · `PointCloudTest.cpp` ·
`MeshBvhTest.cpp` · `geometry-bvh.md` · `graphics-scene.md` · `graphics-rendering.md` · `graphics-architecture.md` · `../backlog/graphics.md`
