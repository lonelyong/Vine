# 实例化：几何保留通道方案（2026-10-22，**已作废**，历史登记）

> **状态：已作废（历史登记）**。这是实例化的早期方案：把实例矩阵作为 `Geometry` 的一条保留通道
> （位置 12–15，16 float/实例）。用户评审后放弃：没有引擎把实例数据挂在网格资产上，且它把“一个 `Geometry`
> 一套实例集”、可见数量、选择单位都绑死了。
>
> **现行设计见 `graphics-instancing-primitive-2026-10-23.md`（`Primitive` 级实例流）。**
> 本文保留作为“通道方案为什么被否”的记录，不要再按它落地。
>
> 关联：`.ai/design/graphics-object-model-vs-3d-frameworks.md` §3 缺口 2、
> `.ai/design/graphics-followups-2026-10-19.md` §3.3。

## 0. 结论

| # | 决定 | 理由 |
| --- | --- | --- |
| **D1** | 实例数据是 `Geometry` 上的一条**保留通道**：位置 12–15，每实例 16 个 float（列主序 mat4），局部空间 | 几何已经拥有“通道 + 图元”，实例变换是通道集的事实；与“模型上加一个字段”一致 |
| **D2** | 数据路径选 **A：逐实例顶点属性**（`VK_VERTEX_INPUT_RATE_INSTANCE`），不选实例 UBO / storage buffer | 后端钩子已经在位：`Draw::instances` 已接到 `DrawIndexed/Draw`，`VertexBinding::per_instance` 已映射到 `VK_VERTEX_INPUT_RATE_INSTANCE`；不需要新描述符、新块区、新对齐 |
| **D3** | 着色器用 `VINE_INSTANCED` 变体：`pc.modelView` 是“view × 节点世界”，实例矩阵是“实例局部”，顶点阶段乘一次 | 节点变换仍走 push（每 draw 一次），实例矩阵走顶点取（每实例一次），互不挤占 |
| **D4** | 一条 `RenderCommand` 仍对应一条 draw，但 `Draw::instances = N`（不是 N 条命令） | 省的是 CPU 记录 / 提交；GPU 仍然逐实例着色（除非将来做剔除/LOD，见 §5） |
| **D5** | 实例计数进 `GeometryFacts.instances`，实例标志进 `ProgramVariant::instanced`，两者都进管线键 | 顶点布局（有实例通道）和程序文本（`VINE_INSTANCED`）都变了，少一个就会把管线借给错误的对象 |
| **D6** | 包围盒 = 基础局部盒经每条实例矩阵变换后的并集；拾取逐实例测试并回 `instanceIndex` | 机制层要自洽：剔除、阴影相机拟合、射线拾取都不能假装实例不存在 |
| **D7** | 不做逐实例材质 / 颜色 / 层 / draw order / 透明排序 / GPU 剔除 / 自动合批 | 第一片只解决“同一几何 N 份”；其余要真触发再加，见 §5 |
| **D8** | 实例矩阵的**增删 / 计数变化**是形状变更（`Geometry::setRevision`）；只改已有矩阵的值是字节变更（`Buffer::setRevision`）。移动实例导致包围盒变化时也必须 `Geometry::setRevision` | 沿用 B6 的“形状归几何、字节归 buffer”；包围盒缓存还依赖几何公告，见 §4.4 |

## 1. 背景：今天有什么、缺什么

| 事实 | 位置 | 对设计的影响 |
| --- | --- | --- |
| 一条 `RenderCommand` 画一个几何的一个 primitive | `RenderCommand.hpp` / `Scene::collectRenderCommands` | N 份重复内容 = N 个节点 + N 条命令 + N 次 draw |
| `ContentDraw::Draw::instances{1}` 已存在，`record()` 已把它传给 `DrawIndexed::create` / `Draw::create` | `ContentDraw.hpp/.cpp` | 后端“提交 N 个实例”的能力已经有了，只差数据 |
| `ContentPipeline::VertexBinding::per_instance` 已映射到 `VK_VERTEX_INPUT_RATE_INSTANCE` | `ContentPipeline.cpp` | 实例步进的管线声明已经有了，只差调用者传 `true` |
| `ContentPipeline::create(abi, GeometryFacts, …)` 一律 `per_instance = false`，且只接受 `bindingOfCanonical` 能喂的规范通道 | `ContentPipeline.cpp` / `StreamUploads.cpp` | 需要一条**规范的**实例通道，不能走“自定义通道”那条已被拒绝的路 |
| `GeometryFacts` 的“每个通道必须从同一个 VERTEX 开始”检查按 `offset / components` 比较 | `GeometryFacts.cpp` | **陷阱**：实例通道的 `offset / 16` 是“第几个实例”，若直接比较，非 0 起点的实例切片会被判 Malformed（见 §4.1） |
| `Node::subtreeBounds` 用 `typeid(*this) == typeid(Geometry)` 特判叶子 | `Node.cpp` | **不要**新建 `InstancedGeometry` 子类：它会掉进“未知包围盒”的慢路径；扩展现有 `Geometry` 才是小的改动 |
| `builtin_forward.vert` / `builtin_gbuffer.vert` 用 push 的 `pc.projection` + `pc.modelView` | `src/viz/graphics/shaders` | 实例矩阵放顶点属性，节点世界矩阵放 push，二者相乘即可 |
| 后端明确**不支持**自定义顶点通道（`acquireVertex` 拒绝、`ContentPipeline::create` 拒绝） | `StreamUploads.cpp` / `ContentPipeline.cpp` | 实例矩阵必须是**规范角色**（位置 12–15），而不是“用户自定义 location” |
| `ContentHalves` 的键含 `VertexLayoutKey` + `variant` | `ContentHalves.cpp` | 实例/非实例几何自然落到不同 half，不会借错管线 |

**量化的目标**（落地时用测试钉住，不在这里编造）：300 个同一几何的副本，今天 = 300 个节点 + 300 条命令 + 300 次 draw；
实例化后 = 1 个节点 + 1 条命令 + 1 次 draw + 一份 300×64 B 的实例流。GPU 的顶点着色次数不变。

## 2. 数据路径：为什么是逐实例顶点属性

| 方案 | 做法 | 代价 | 判定 |
| --- | --- | --- | --- |
| **A. 逐实例顶点属性（选它）** | 实例矩阵 16 float 放缓冲，四个 `location 12–15`，一条 binding，`inputRate = INSTANCE`，stride 64 | 多 4 个顶点属性、1 个 binding；实例流按缓冲上传一次 | 复用 `per_instance` / `Draw::instances` 两个已存在的钩子；无新描述符 |
| B. 实例 UBO / storage buffer + `gl_InstanceIndex` | 新 binding / 新块区，按 `gl_InstanceIndex` 索引矩阵数组 | 要动规范块集、`BlockStorage` 区域、描述符 range/stride、动态偏移的连续性；与“每 draw 一个 80 B 块”的现有契约冲突 | 不做；收益只是“矩阵不进顶点缓存” |
| C. 把矩阵塞进 `VineDrawBlock` 的数组，draw 内索引 | 复用 draw UBO | 需要一次为 N 个实例预留连续块；帧的 draw 块预算（1024）被实例数吃掉；ring 回绕时未必连续 | 不做；把“实例数”与“帧预算”绑在一起 |

方案 A 的“每个实例 64 B 顶点取”是移动 GPU 上最普通的做法；实例矩阵本来就要从显存读，顶点取和 UBO 取没有本质差别。
A 的另一个好处是**管线键已经能表达它**：`VertexLayoutKey` 的规范位 + `ProgramVariant` 的 `VINE_INSTANCED` 都已在键里。

## 3. 契约

### 3.1 SDK：`Geometry` 的实例通道

**ABI（`ShaderAbi.hpp`）**

- `VertexAttribute::InstanceModel`，`attributeLocation(...) = 12`，占 12–15 四个连续位置；
- `kInstanceMatrixColumns = 4`，`kInstanceMatrixComponents = 16`；
- `isCanonicalAttributeLocation()` 对 12–15 返回 true（它们和 0/1/2/8 一样是引擎自己的角色）；
- `instanceMatrixLocation(column)` = 12 + column；
- 自定义通道的保留说明更新为：“≥3，且不得是 8（texcoord）或 12–15（实例矩阵）”。

**`Geometry` 新增 API**

```cpp
void setInstanceTransforms(intrusive_ptr<const Buffer<float>> transforms,
                           std::size_t first_instance = 0, std::size_t instance_count = 0);
void clearInstanceTransforms();
bool hasInstanceTransforms() const;
std::size_t instanceCount() const;
std::optional<Mat4d> instanceMatrix(std::size_t instance) const;
std::uint64_t ignoredAttributeEdits() const;
```

- `setInstanceTransforms` = `addBuffer(instanceMatrixLocation(0), AttributeChannel::slice(std::move(transforms), 16, first_instance, instance_count))`。
  缓冲里每 16 个 float 是一条**列主序 mat4**，与 `VineViewBlock` / `VineDrawBlock` 的矩阵约定一致。
- `AttributeChannel` 的 `components` 文档从“1..4”扩成“普通属性 1..4；实例通道 16”，
  `vertexCount()` 对实例通道就是实例数；`vec3View()` / `xyz()` 等按 3/4 分量解释的访问器对 16 分量返回空/不适用。
- `clearInstanceTransforms` = `removeBuffer(12)`。
- `instanceCount()` = 该通道的 `vertexCount()`（`floatCount() / 16`）；没有通道时为 0。
- `instanceMatrix(i)` 越界或没有实例通道时返回 `std::nullopt`（**不要**返回单位矩阵冒充成功：
  `Matrix4x4::inverted()` 那种“失败返回原值”的写法是仓库明确点过的坑）。
- `addBuffer` 增加保留位守卫：位置 13–15 一律忽略并计数；位置 12 只接受 `components == 16`。
  这样 `setInstanceTransforms` 与 `addBuffer` 仍只有一个事实来源，坏编辑不会静默变成一条 4 分量的“实例通道”。
- **不**另建 `InstancedGeometry` 子类：`Node::subtreeBounds` 对精确的 `Geometry` 有叶子特判，子类会掉进
  “custom leaf 包围盒未知”的分支（见 §1）；扩展现有叶子是更小的改动。

**形状 vs 字节（B6 契约的延伸）**

- **增删通道 / 改实例数**：形状变更 ⇒ 调 `Geometry::setRevision()`（事实表重建）。
- **只改已有矩阵的值**：字节变更 ⇒ 调 `Buffer::setRevision()`（流重传，事实表不重建）。
- **移动实例导致包围盒变化**：除了 `Buffer::setRevision()`，还要 `Geometry::setRevision()`——
  `Geometry::localBounds()` 自己会凭缓冲 revision 重算，但 `Scene`/`Group` 的缓存盒依赖 `invalidateBounds()` 的公告，
  只改 buffer 时上层缓存盒不会动。这是现有位置通道也有的同一契约，不是实例化新造的。

**局部性与限制**

- 实例矩阵是**节点局部**的：最终位置 = `节点世界矩阵 × 实例矩阵`。把节点放平（identity）时，实例矩阵就是世界矩阵。
- 所有实例共享：材质、程序、`opacity`、`draw_order`、层掩码、渲染状态。
- 透明实例之间**不排序**：一条 draw 内按实例序混合。实例化用于不透明（或加色/顺序无关）内容；
  需要逐实例排序时请拆成多个几何。
- 镜像实例（负行列式）会翻转缠绕方向，和“镜像的节点变换”是同一个已知问题；第一片不检测，文档写明。
- 包围盒是所有实例的**并集**：一个很远实例会让整个集合过不了剔除；逐实例剔除属于 §5。

**包围盒**

`Geometry::localBounds()`：

1. 基础盒仍由位置通道扫描/缓存（现有逻辑）；
2. 若有实例通道，`box = union(transformBox(base, *instanceMatrix(i)))`；
3. 缓存键在原有 `(位置缓冲, revision, 段, 几何 revision)` 之外，再加 `(实例缓冲, revision, 段)`；
4. `localBoundsComputationCount()` 只在这两种键任一变化时 +1。

**拾取**

- `RayIntersectionResult` 末尾新增 `std::uint32_t instanceIndex{0}`（非实例几何为 0）。
- `forEachTriangleHit` 先解析一次 mesh，再逐实例用 `world * instanceMatrix(i)` 调用 `traverseMesh`；
  每条命中带 `primitiveIndex` 和 `instanceIndex`。`intersectScene` / `intersectSceneAll` 自动获得该行为。
- 单条实例矩阵奇异时由 `traverseMesh` 的既有奇异检查跳过；几何的世界矩阵奇异仍在入口拒绝。

### 3.2 后端：从事实到 draw

**`GeometryFacts`**

- 新增 `std::uint32_t instances{0}`：有实例通道时 = `channel->vertexCount()`，否则 0。
- `canonicalRoleOf(12..15)` 返回 `InstanceModel`；`layout.canonical_mask` 因此多一位。
- 通道分量不是 16、`floatCount % 16 != 0`（尾部半条矩阵）或实例数溢出 `uint32` 时返回 `FactMiss::Malformed`：
  半条矩阵不是“少画一个实例”，而是数据与步长不自洽；宁可拒画也不要把 1 个实例画成非实例。
- **跳过**实例通道的“同一 VERTEX 起点”检查（§4.1）。
- `channelsMatchLayout()` 的 `popcount(规范位) + 自定义数 == channels.size()` 仍然成立：实例矩阵是**一条**通道，不是四条。

**`ProgramVariant`**

- 新增 `bool instanced{false}`；`bits()` 用 `8U`；`defines()` 增加 `VINE_INSTANCED`；
  `describe()` / `operator==` 同步；`variantOf()` 设 `instanced = geometry.instances > 0`。
- 16 种组合的 `bits()` 仍然互不相同（现有测试从 8 扩到 16）。

**`ContentPipeline::create(abi, GeometryFacts, …)`**

对实例通道单独展开：

- 一条 binding：`binding = StreamUploads::bindingOfCanonical(12) = 4`，`stride = 64`，`per_instance = true`；
- 四个属性：`location 12..15`，同一 binding，格式 `R32G32B32A32_SFLOAT`，偏移 `0/16/32/48`；
- 其他通道逻辑不变；`bindingOfCanonical` 返回 `kNoBinding` 时仍拒绝整条管线。

**`StreamUploads`**

- `isCanonicalLocation()` 改为调用 `vn::graphics::isCanonicalAttributeLocation()`（一处拼写；
  12–15 现在也是规范的）；
- `bindingOfCanonical(12..15) = 4`；
- `acquireVertex` 的“派生 / 非规范 / 空”拒绝不变；实例通道是普通缓冲，照常按 `StreamKey` 上传/共享一次。

**`ContentPass`**

- 循环里的通道数不变（`kMaxChannels = 8`，实例只是多一条）；
- 记录 draw 时设 `record.instances = geometry.entry->instances > 0 ? geometry.entry->instances : 1U`；
- 其余（块、push、索引、状态）不变：`pc.modelView` 仍是每 draw 的节点世界矩阵。

### 3.3 引擎着色器

`builtin_forward.vert` / `builtin_gbuffer.vert`：

```glsl
#pragma import_defines (..., VINE_INSTANCED)
#ifdef VINE_INSTANCED
layout(location = 12) in mat4 vine_InstanceModel;
#endif
...
mat4 model = pc.modelView;
#ifdef VINE_INSTANCED
model = model * vine_InstanceModel;
#endif
vec4 view_pos = model * vec4(vine_Vertex, 1.0);
vine_view_normal = mat3(model) * vine_Normal;
```

- `pc.modelView` = view × 节点世界；`vine_InstanceModel` = 实例局部；相乘即 view × 节点世界 × 实例局部。
- 法线沿用现有的 `mat3(model)`（和节点非均匀缩放同一口径，不在这一片改）。
- `builtin_skybox` / `builtin_fullscreen` 不 import 该名字；vsg 对源里没列出的 define 是**静默丢弃**，
  所以它们的行为不变。
- 用户程序：想画实例化内容，必须自己声明 12–15 并 import `VINE_INSTANCED`。后端的 `ProgramAbi` 只描述
  描述符/推入，不描述顶点 ABI，**无法校验**这一点；不声明的用户程序会把 N 个实例画在同一个位置。
  这是明确的已知锐边，写进文档，不假装能检测。

### 3.4 键、缓存与生命周期

- 管线键：`VertexLayoutKey`（实例位） + `PipelineKey::variant`（`VINE_INSTANCED`）都已存在，不需要新字段。
- `ContentHalves`：实例与非实例几何落到不同 half；同一 half 可服务所有实例数不同的几何（管线不依赖 N）。
- 流：`StreamKey` 的 `location=12`、`components=16`、`buffer/revision/offset/count` 唯一标识一条实例流；
  两个几何切同一 arena 的同一段时共享一次上传。
- 实例缓冲由 `Geometry` 的通道持有，`StreamUploads` 上传后不依赖它；生命周期与现有位置通道一致。
- 不新增 `ResourcePressure` 警告：实例化没有容量上限；编译/布局失败仍走现有“half 被拒”报告。

## 4. 实现时要先处理的坑

### 4.1 “同一 VERTEX 起点”检查会把实例切片判成 Malformed

`buildGeometryFacts` 现有代码：

```cpp
const std::size_t base = channel->offset / channel->components;
if (!base_known) { vertex_base = base; ... }
else if (vertex_base != base) { return FactMiss::Malformed; }
```

实例通道的 `offset / 16` 是**实例序号**，不是顶点序号。一个 `first_instance = 7` 的切片会与位置通道的
`vertex_base = 0` 不等，几何直接被判 Malformed。修法：把 `role == InstanceModel` 的通道排除在这个检查之外
（它不参与顶点组装），并为“`first_instance != 0` 的实例切片”写一条判据——否则这条路径只会在真实使用切片时坏。

### 4.2 自定义通道那条路是**主动拒绝**的

`StreamUploads::acquireVertex` 与 `ContentPipeline::create` 都拒绝非规范通道（自定义通道的 bind 身份是整套
自定义布局，共享会画错）。因此实例矩阵**不能**以“用户自定义 location”的形式接入；它必须是规范角色。
这也是为什么本设计要动 `ShaderAbi.hpp`，而不是只加一个 `addBuffer(12, …)` 的约定。
附带一笔文档债：`Geometry.hpp` 的 `kTexCoordLocation` 注释仍写着“backend forwards custom channels as
`vine_Attribute{location}`”，那是自定义通道删除前的说法，落地这一片时一并改正。

### 4.3 不要用 `InstancedGeometry` 子类

`Node::subtreeBounds` 用 `typeid(*this) == typeid(Geometry)` 把叶子当数据盒；新子类会走“custom leaf 包围盒未知”
的分支，导致场景包容盒退化为“每帧从头算/未知”。扩展现有 `Geometry` 避免了这个问题，也更符合“几何已经拥有通道集”。

### 4.4 只改缓冲 revision 不会移动上层缓存盒

`Geometry::localBounds()` 的缓存认缓冲 revision，但 `Group`/`Scene` 的缓存盒认 `invalidateBounds()` 的 stamp。
实例矩阵移动了包围盒却只调 `Buffer::setRevision()` 时，剔除/阴影拟合会读到旧盒。契约：**移动实例 ⇒ `Geometry::setRevision()`**。
如果将来实测“每次移动都重建事实行”成为瓶颈，再评估一个只清包围盒、不 bump 几何 revision 的入口；不在第一片加。

### 4.5 实例数与缓冲增长

`GeometryFacts.instances` 是事实行里的快照。缓冲从空长出来、或实例数变化，都是**形状**变更 ⇒ `Geometry::setRevision()`。
只改已有矩阵的值不需要（`ChannelFacts::streamKey()` 读 buffer 的 live revision）。这条要在 `setInstanceTransforms` 文档里写明。

## 5. 明确不做（第一片）

| 不做 | 原因 / 将来的触发 |
| --- | --- |
| 逐实例材质 / 颜色 / opacity / 层 / draw order | 这些都是“命令”的事实；逐实例化要新 ABI（第二个实例通道或实例块）。真出现“同一网格不同颜色”的批量需求时再评 |
| 逐实例透明排序 | 一条 draw 内无法重排；需要排序就拆成多个几何 |
| 逐实例剔除 / GPU culling | 包围盒是并集；远处实例会让整组通过。触发：实例数大到“画不可见实例”成为主要成本 |
| 自动合批（把多个 `Geometry` 合成一份实例流） | 需要判断数据相同、材质相同、生命周期一致；第一片只接受宿主显式给出的实例矩阵 |
| LOD / 空间划分 | object-model 缺口 3，独立触发 |
| 镜像实例检测 | 与镜像节点变换同一已知问题；检测需要逐矩阵行列式，且没有“逐实例翻转正面”的可用状态 |
| 分层目标（L2b） | 实例化不需要它；两者只是共享“一次提交许多份”的评估触发，不共享实现 |

## 6. 落地切片与判据

### I1：ABI + 后端贯通（最小可画）

改动：`ShaderAbi.hpp`（角色/位置/谓词）、`GeometryFacts`（`instances`/角色/Malformed/跳过起点检查）、
`ProgramVariant`（`instanced`）、`ContentPipeline`（binding 4 + 四属性）、`StreamUploads`（谓词 + binding 4）、
`ContentPass`（`record.instances`）、两个内置顶点着色器、`vine_shader_check.sh` 的 `VINE_INSTANCED`。

判据（结构）：

- `ShaderAbiTest`：12–15 是规范位置，3/7/11/16 不是；`kInstanceMatrixComponents == 16`。
- `ProgramVariantTest`：`variantOf()` 对带实例通道的几何置 `instanced`；`defines()` 含 `VINE_INSTANCED`；
  16 种组合 `bits()` 互异。
- `ContentFactsTest`：实例通道 ⇒ `instances == N`、`layout` 多一位、`channelsMatchLayout` 为真、
  `first_instance != 0` 的切片**不**被判 Malformed。
- `ContentPipelineTest`：binding 4 的 `stride=64`、`inputRate=INSTANCE`；属性 12–15 的偏移 0/16/32/48；
  4 分量的位置 12 被拒。
- `StreamUploadsTest`：`bindingOfCanonical(12..15) == 4`；实例切片上传一次、同键共享。
- `scripts/vine_shader_check.sh` 全绿（16 种 define 组合 × 两种 texcoord kind）。

### I2：SDK 形状 + 包围盒 + 拾取

改动：`Geometry` 的 API/守卫/计数/缓存键/并集/`instanceMatrix`、`RayIntersectionResult::instanceIndex` +
`forEachTriangleHit` 逐实例循环。

判据：

- `GeometryTest`（或新 `GeometryInstanceTest`）：设 3 条矩阵 ⇒ `instanceCount()==3`；`clear` 归零；
  13–15 的 `addBuffer` 被忽略并计数；12 的 4 分量被忽略；`instanceMatrix(i)` 的平移列正确、越界为 `nullopt`。
- 包围盒：单位方片 + 实例 x=0/10/20 ⇒ 盒覆盖 0..21；无实例回到基础盒；
  改实例缓冲 revision 后 `localBoundsComputationCount()` +1；只改位置不改实例时也 +1（键覆盖两者）。
- `RayIntersectionTest`：两个实例，射线打 x≈10 ⇒ `instanceIndex==1`；`intersectScene` 同样。
- 场景缓存：在 `Group` 下移动实例并 `Geometry::setRevision()` ⇒ 组盒更新；只改 buffer 时文档写明不更新（用一个用例钉住
  “必须公告”的契约，而不是让测试依赖旧盒）。

### I3：设备像素与对照

改动：`VsgBackendTest` 新用例；需要先 `ninja -C build` / `ninja -C build-release` 构建插件。

判据：

- `OneCommandDrawsEveryInstanceAtItsOwnPlace`：一个 1×1 方片、300 条平移矩阵、宽视口；回读像素在 300 个预期列上命中，
  间隔是背景；`CompiledFrame` 的命令数 = 1（每个 primitive 一条）。
- `AnInstancedGeometryAndThreeHundredNodesPaintTheSamePicture`：300 个节点 vs 1 个实例化几何，同相机/材质；
  两张图逐像素（或允许极小容差）一致；实例化这边的 draw 记录数 1，节点那边 300。
- `InstancedAndNonInstancedContentOfOneProgramGetTheirOwnPipelines`：同一程序画一份普通几何 + 一份实例化几何；
  两者都正确；如果键合并（例如去掉 `instanced` 位或布局位），验证层应报“管线顶点输入与着色器不匹配”或像素错。
- 现有 498/441 用例全绿，非实例内容不增加管线编译（`ContentStore` 的 variant 表不出现 `VINE_INSTANCED`）。

### I4：文档与账本

- `.ai/README.md` 索引本文；
- `graphics-followups-2026-10-19.md` §3.3 的“实例化 / 批处理”行指向本文并标“设计定稿，未落地”；
- 落地记录见 `graphics-instancing-primitive-2026-10-23.md`（实际文件、判据数字、偏差）。

## 7. 变异清单（落地后必须逐条变红）

| 变异 | 预期症状 |
| --- | --- |
| 去掉 `record.instances` | 设备用例只画 1 个实例（或 0），像素对照红 |
| `per_instance` 改回 `false` | 所有实例读第 0 条矩阵，全部叠在一处 |
| 着色器去掉 `VINE_INSTANCED` 分支 / define | 同上（全部在节点原点） |
| `ProgramVariant::bits()` 去掉 `instanced` 位 | 实例/非实例共用程序文本或管线，像素/验证层红 |
| `VertexLayoutKey` 不反映实例位（若实现成“只靠 variant”） | 非实例管线被借给实例 draw，顶点输入不匹配 |
| `GeometryFacts` 不跳过实例通道的起点检查 | `first_instance != 0` 的切片被判 Malformed（I1 的用例先红） |
| 包围盒不做实例并集 | 阴影相机拟合/剔除用例红（若没有该用例，这条变异是绿的 ⇒ 说明用例没钉住，需补） |
| 拾取不循环实例 | 射线打第二个实例时 miss/命中错误 |
| `bindingOfCanonical(12..15)` 只映射 12 | 13–15 的属性取不到数据，验证层/像素红 |

## 8. 风险与待批准项

**风险**

- 用户程序不 import `VINE_INSTANCED` 时**静默**把所有实例画在原点；后端无法从 `ProgramAbi` 检测（§3.3）。
- 镜像实例的缠绕方向；第一片不检测（§3.1）。
- 透明实例的排序；第一片不排序（§3.1）。
- 移动实例却只公告 buffer revision ⇒ 上层包围盒陈旧（§4.4）。
- 实例缓冲增长/实例数变化却只公告 buffer revision ⇒ 事实行里的 `instances` 陈旧（§4.5）。

**待批准**

1. 实例矩阵放在 `Geometry`（本设计）而不是 `RenderCommand`：前者让管线键继续由几何的事实决定；
   后者能做“同一几何的实例子集”，但会引入两份真相。只有出现“按命令取实例子集”的真实需求才改。
2. `RayIntersectionResult::instanceIndex` 用 0 表示非实例；如果更希望用 `max()` 区分“没有实例”，请在落地前定。
3. `setInstanceTransforms` 是否自动 `setRevision()`：本设计**不自动**（与 `setPositions` 等既有 setter 一致），
   文档要求宿主公告；若实测“每次移动都重建事实行”成为瓶颈，再评估“只清包围盒”的入口。
