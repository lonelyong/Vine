# 几何体与图元（2026-10-22，R3 的第一片）：一个对象多个 draw

> 一句话：**拓扑是 draw 的事实，资产是一个整体**。`Geometry` 只剩"数据"（通道集），
> `Primitive` 才是"一次 draw"（拓扑 + 索引片段 + 材质 + 程序）；收集器一条 primitive 一条命令。

## 0. 形状

```cpp
struct Primitive {
    Topology                        topology{Topology::Triangles};
    BufferSlice<std::uint32_t>      indices{};      // 空 = 非索引绘制
    intrusive_ptr<Material>         material{};     // 空 = 继承最近 StateNode
    intrusive_ptr<ShaderProgram>    program{};      // 空 = 继承（StateNode，然后 pass）
};

class Geometry {                       // 只剩数据
    std::map<uint32_t, AttributeChannel> attributes_;
    std::vector<Primitive>               primitives_;
};
```

| 层 | 回答 | 谁写 |
| --- | --- | --- |
| `Geometry` | 顶点**数据**是什么（位置/法线/UV/自定义通道，可指向 arena 的一段） | 造数据的人 |
| `Primitive` | 这一次 draw 怎么**消费**它（拓扑、索引片段、计数）和怎么**着色**（材质/程序） | 造资产的人 |
| `StateNode` | 这棵子树的**状态**（深度/剔除/混合/多边形模式）——**不再有拓扑** | 内容作者 |
| `RenderPass` | 这一遍的**观看**（program / polygonMode / **topology** override，覆盖内容与数据） | 视图 |

`StateNode::topology` / `clearTopology()` / `RenderState::topology` / `StateOrigin::topology` 的
`Content`/`Default` 两档**删除**；`resolveRenderState(authored, pass, data_topology)` 第三个参数是**数据的
话**，pass 的 `topology` 可以覆盖它（origin 报 `Data` 或 `Pass`）。

## 1. 为什么索引归 primitive

- **更新节奏不同**：边线/选中点集的索引是运行时生成的、随时变；网格索引基本不变。后端共享索引上传是按
  **整段 buffer**（绑定别名整段、切片在 draw 里），放同一 buffer 就会耦合失效。
- **表达能力**：per-primitive 是 geometry-wide 的**超集**——想要"一个 arena 切三段"仍然可以（三个 primitive
  指同一 buffer 的不同切片，共享一次上传）；反过来做不到。
- 顶点**不受影响**：`StreamKey` 仍是 `(buffer, revision, location, components, offset, count)`，所以多图元
  **不会**让顶点多传一次；索引键是 `(buffer, revision)`（`bindKeyOf` 归零切片）。

## 2. 这一片落地了什么

| 位置 | 变化 |
| --- | --- |
| `Topology.hpp`（新） | 枚举从 `StateNode.hpp` 搬出来：它是 primitive 的事实 |
| `Primitive.hpp`（新） | 上面那个结构 + `IndexSpan` 别名 |
| `Geometry` | `addPrimitive` / `setPrimitive` / `clearPrimitives` / `primitive(i)` / `primitiveCount` / `primitives()`；**数据即绘制**：`addBuffer` 装位置通道时若还没有 primitive 就放一个默认的（三角形、非索引）——"设了数据却没画"在结构上不可能 |
| 单图元拼写（过渡） | `setIndices` / `hasIndices` / `indices` / `firstIndex` / `indexCount` / `indicesBuffer` / `material` / `setMaterial` / `program` / `setProgram` 保留为**单 draw 的便捷写法**（读写 primitive 0；`setMaterial/setProgram` 写给所有 primitive）。位置见 `.ai/design/graphics-followups-2026-10-19.md` §3.3 |
| `RenderCommand` | `primitive_index` + `primitive()`；收集器**一条 primitive 一条命令**，材质/程序按 primitive → StateNode 解析 |
| 计划 | `CollectedCommand` / `CompiledCommand` 带 `primitive`；`CollectedCommand` 还带 primitive 的 `topology`，`FrameCompiler` 用它做 `resolveRenderState` 的第三个参数 |
| 拾取 | 只对 `Topology::Triangles` 的 primitive 求交，命中结果带 `primitiveIndex`（顶点/索引仍从几何体读） |
| 后端（已随第二片完成） | `GeometryFacts::primitives` 是**每 draw 一行**（topology + 索引流 + span + 计数），`ContentPass` 用 `CompiledCommand::primitive` 取那一行；0 个 primitive 是 `Unknown` |

## 3. 后端按图元出行（第二片，已完成）

`GeometryFacts::primitives` 是每 draw 一行：

```cpp
struct PrimitiveFacts {
    vn::graphics::Topology    topology;
    std::optional<IndexFacts> indices;      // 空 = 非索引 draw（一个不同的 API 命令）
    std::uint32_t             index_count;
    std::uint32_t             first_index;
};
```

- `buildGeometryFacts(geometry, out, channels, primitives)` 按几何体的顺序建行（计划用 `primitive` 索引进
  这张表）；索引流仍**归一化到整段 buffer**（绑定别名整段、span 在 draw 里）⇒ 同一 arena 上的多个 primitive
  **共享一次上传**。
- `ContentPass` 用 `command.primitive` 取自己那一行：索引流、span、顶点数都读它；越界的 primitive 一次报告
  并跳过那条命令（几何体在计划编译后被改过）。
- `channelsMatchLayout` 对**每条** primitive 判"能否组装"（有索引流、或几何体有顶点）。
- 于是：一份顶点 + 三角形/线/点三条 primitive ⇒ **3 次 draw、2~3 条管线（按 class）、顶点只上传一次**；
  两份索引各在各的 buffer ⇒ 改哪份传哪份；同 arena ⇒ 一次上传、整段失效（宿主可选）。

## 4. 判据（本片）

- `test_graphics` 414 / `test_vsg` 480 / ctest 24/24 全绿；网关**每阶段干净**（0 VUID、0 hazard、app 画面
  判据通过）——单图元路径逐像素与改造前一致。
- `GeometryTest.CountsStayDataStatsUnderPointsTopology`（改写）：拓扑写在 primitive 上，顶点计数不变。
- `StateNodeTest.TopologyIsNotStateAtAll`（改写）：`StateNode` 不再有拓扑；数据层与 pass 覆盖各报自己的 origin。
- `ContentPassTest.AnUnindexedPointCloudDrawsThroughThePointsPipeline`（改写）：点云靠 primitive 声明拓扑。
- `RayIntersectionTest.*`：只打三角形 primitive；`hit.primitiveIndex` 指出命中的那一份。
- `GeometryFactsTest.AMultiPrimitiveGeometryDescribesOneRowPerDraw`：一个几何体三条 primitive（三角形/线/点）
  ⇒ 三行、拓扑按顺序、span 各归各的、同一 arena 上的两条共享流身份（一次上传）、非索引那条用几何体顶点数。

## 5. 端到端证据（同日夜，设备级）

上面每一条都是"按构造"或"设备无关"的。真正关系到宿主的句子——**一份顶点、三条图元、每条流只传一次**——
需要一个画面和一本流账，落在 `tests/test_vsg/PrimitiveDrawTest.cpp`：

- **配方**：几何体 10 个顶点、一个 12 个索引的 arena；三条 primitive = 三角形（左三分之一，红）/ 线（中间一列，
  绿）/ 点（右三分之一四颗，蓝）。三段不重叠，所以每种颜色只可能来自它那一条 draw。`StateNode` 的材质故意是
  白色：图元材质若在链路上被丢掉，画面会全白，三段计数一起归零。
- **判据**：像素 `red=1334 / green=58 / blue=4`（面 > 线 > 点，且各自有下界）；`FrameReport::counters`
  `passes=1, draws=1`（一次**绘制调用**，三条图元是这次调用里的三条命令——这正是 `draws` 的语义）；
  `StreamUploads` 首帧 `uploads=3 / aliases=6 / live=3`（位置 + 法线 + arena），第二帧 `uploads` 不变、
  `aliases` 再涨 9（九次取流全命中）。
- **它抓到的 bug**：`Topology::Points` 配内置前向程序，4 条 `VUID-vkCmdDrawIndexed-primitiveTopology-10748`
  ——POINT_LIST 要求顶点阶段**静态写 `gl_PointSize`**，否则管线与每次 draw 都是无效用法。修在源头：
  `builtin_forward.vert` / `builtin_gbuffer.vert` 写 `gl_PointSize = 1.0`（Vulkan 的默认值，三角形无影响），
  `Topology.hpp` 与指南 §3.21 把"点是数据与程序的共同契约"写进文档。自建程序若忘了写，Vulkan 在**建管线**时
  就报 08773，后端不必反射 SPIR-V。
- 判据：`test_vsg` 482（+1）、`test_graphics` 414、ctest 24/24、网关每阶段干净（0 VUID、0 hazard）。
