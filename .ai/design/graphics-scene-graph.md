# Graphics 场景图设计（Node 派生 / OSG-vsg 风）

> ⚠ 落地后修订（2026-09-15，**收集路径**，逐条见 `graphics-vsg-audit.md`）：
> 1. **局部包围盒有缓存**：`Geometry` 记住"局部数据盒"，键 = positions 缓冲指针 + **该缓冲的 revision** +
>    段（`offset`/`scalarCount`）+ **geometry revision**；世界盒仍每次由 `worldMatrix()` 派生。
>    此前每个叶子每帧重新扫全部顶点，而 `BoundsCache` 求 root 的盒必须求所有叶子的盒 ⇒ **视锥剔除在 CPU 侧
>    一分钱都没省**（每帧 O(全部顶点)）。可观测：`Geometry::localBoundsComputationCount()`。
> 2. **状态折叠改自顶向下**：`collectNodeCommands` 带一个 `InheritedState`（折好的 `RenderState` + 最近祖先的
>    material / program）往下传，O(1) 每节点、零分配；此前每个叶子要 `effectiveMaterial`（上行）、
>    `collectRenderState`（上行 + **一次 vector 分配**）、`effectiveProgram`（再上行）各一次。
>    `StateNode.hpp` 的三个上行版仍是 SDK 的公开拼写，**两者必须一致**（`SceneTest.TheFoldedStateAgreesWithTheUpWalkingHelpers`）。
> 3. **命令表共享**：新增 `Scene::collectRenderCommandsShared()`（`shared_ptr<const vector<RenderCommand>>`，
>    不可变、帧内有效）。pass 路径只用引用，**只有设了 program override 的 pass** 才 fork 一份；
>    `collectRenderCommands()` 保留为"拥有一份副本"的拼写。
> 4. **接线校验声明驱动**：`RenderPass::wiringRevision()` / `RenderEngine::wiringValidationCount()`（见
>    `graphics-render-pipeline.md`）。

> 状态：设计稿 v1（2026-09-03），评审对象。
> 📋 评审（2026-09-03）：正文 §1/§2/§3/§4/§7 为**写作时**设计（沿用 MatrixNode、Drawable、primitive、
> "Scene 只持 root"等**历史表述**），**以顶部 ⚠/📋 落地记录为准**。代码核对要点：
> 1. **Scene 根节点**：正文 §5/§7(3) 提"Scene 只持 root"。⚠ 2026-09-07 **已收敛单 root**：
>    `Scene::addNode/removeNode/nodes()` 已删除，改 **`setRoot(intrusive_ptr<Node>)/root()`**
>    （root 可为空 = 空场景，不渲染）；内容经根 `Group` 组合（`Group::addChild/removeChild`）；
>    `findNode/boundingBox/collectRenderCommands` 均自单根出发。消费者（demo/axis gizmo/拾取/
>    测试）迁到"单恒等根 Group + addChild"模式。此前为多 root（有意过渡，见 ⚠ 落地后修订）。
> 2. **MatrixNode 名**：已定名 **MatrixTransform**（本稿正文沿用历史名，见顶部 ⚠）。
> 3. **program 槽**：正文 §2/§4 挂点已部分落地——SDK `ShaderProgram/ShaderStage`、
>    `Geometry::setProgram`、`StateNode::setProgram`、`effectiveProgram`、`RenderCommand.program`
>    均已实现（graphics-shader.md）；后端按 program 建 ShaderSet 的 SceneBridge 接线进行中。
> 4. 图元计数：`vertexCount` = 纯数据统计，不随 StateNode Topology 变（有测试钉住）。
>    ⚠ 2026-09-04：graphics `Geometry::triangleCount()` 已**移除**——Geometry 顶点数据面不保证是三角网格
>    （开放通道 + Points/Lines 拓扑），三角语义统计不再成立；geometry 模块 `Mesh::triangleCount()` 保留。
>
> ⚠ 落地后修订（2026-09-03，代码已先于文档）：
> - **R1 核心已全部落地**：`Node` 拆为基类（name/visible/opacity/parent + 虚 `boundingBox()`
>   + `worldMatrix()`）；`Group` 承接 `children()` 容器；**变换唯一归属 `MatrixTransform`**
>   （原设计稿写作 MatrixNode，用户定名 MatrixTransform，osg/vsg 对齐；经 `localTransformMatrix()`
>   虚函数参与 `Node::worldMatrix()` 沿父链累积）；`StateNode : Group` 不变。
> - **`Geometry : Node` 叶子化**：并入 material（visible/opacity/name 继承自 Node）；**`Drawable`
>   已删除**（`Drawable.hpp/.cpp` 移除）；`RenderCommand.drawable`(DrawablePtr) → **`RenderCommand.geometry`(GeometryPtr)**。
> - **boundingBox 语义决策**：所有节点在**世界空间**作答——叶子 Geometry = loc0 本地盒 × 祖先
>   MatrixTransform 链（`worldMatrix()`）；Group/MatrixTransform = children 世界盒并集（自洽，
>   无需要求逐层变换盒）。Scene/视锥/Ray 直接测 `boundingBox()`，收集/拾取在叶子处烤
>   `node->worldMatrix()` 到 modelMatrix。
> - **消费者已迁移**：`Scene::collectRenderCommands`/`findNode`、`RayIntersection`（三遍历器）、
>   `AxisGizmo`、`SceneBridge`（命令直接带 `Geometry*`，无需 dynamic_cast）、`VsgRenderer` 的
>   no-cull walker、`app_shell::addBox`、`test_plugin::TestRenderLiveCommand`、`GraphicsTest.cpp`
>   （99 tests 全绿）。测试 helper `makeTriangleNode` 现返回 `MatrixTransform`（子 = Geometry 叶子）。
> - **已全部落地（对账 2026-09-25）**：①程序槽——`StateNode::setProgram` + `RenderPass::setProgramOverride`，
>   重写版后端按程序文本声明的 `(set, binding)` 建管线与集合；②`StateNode` 的 renderState **由后端消费**——
>   `core::resolveDynamicState` 把 `ResolvedRenderState` 折进逐绘制的动态状态，M11u 的状态区制有像素断言；
>   ③点云——`Topology::Points` + 非索引路径已落地，demo 的 `star_cloud` 在画。
>   `Geometry` 的开放 loc buffer 列表已在早前切片落地。
>
> 早期“落地后修订”记录（2026-09-03，先于本次 R1）：
> - `Geometry` 已**纯数据化**：移除 `shape_`/`shape()`；新增 `geometryFromShape()` 转换器与
>   normals/revision 通道（**2026-09-11：`setShape(Shape)` 已删除**——与转换器职责重复、非 Mesh
>   形状（Sphere/BRep）会静默清空几何、且 `attributes_.clear()` 连带清掉自定义 loc 通道；就地
>   重填改用 `setPositions/setNormals/setIndices`）；`SceneBridge`（缓存键=
>   revision、建几何读 buffers）与 `RayIntersection`（meshOfGeometry）已切 buffers；bbox/计数全从
>   buffers（不再借用 Shape 的 Aabb 缓存）。
> - 拓扑已从 Geometry 移出：`PrimitiveType` 移除，改**渲染状态项 `Topology`**（默认 Triangles，见
>   graphics-state.md）——与 vsg/Vulkan（拓扑属管线）一致。
> - 通用 `setBuffer(loc)/Buffer` 容器**仍未定**：现为类型化 positions/normals/indices；任意自定义
>   loc 通道与点云数据型留到"Geometry 变叶子 + 点云"切片。
> - 本文 §2/§3 中关于 Drawable/shape/primitive 的旧描述已被上述实现取代。

> 关联：`graphics-state.md`（StateNode）、`graphics-shader.md`（用户可编程着色）、
> `graphics-design.md`、`vsg-design.md`、`vine-shader.md`（后端 P0）。
>
> **一句话**：把 graphics 场景图改成 **OSG/vsg 式节点组合**——`Node`(抽象基) 派生
> `Group / MatrixNode / StateNode`，`Geometry` 是**叶子 Node**；去掉 `Drawable` 与
> "renderable 内持 Shape"；数据 = loc 绑定的 buffers（loc0=position 约定）；
> 变换只在 MatrixNode；State 只在 StateNode；Scene 只持 root。

## 0. 现状与动机

现状：`Node`（自带 localTransform + 持 drawables）；`Drawable`（纯 renderable，visible/opacity/
material）；`Geometry : Drawable`（包 `vn::geometry::Shape`）。问题：
- 想获得 osg/vsg 那种"节点类型随意组合"的灵活性（状态/变换按子树生效）；
- 想支持任意顶点数据（点云/自定义属性）与用户可编程着色；
- 多一层 `Drawable` + 一次"renderable 包 Shape"造成两个抽象、两处变换归属。

目标：Geometry 数据化、状态节点化、着色可编程，全部后端无关，且 vsg 端映射就是现成
`SceneBridge` 模式的推广。

## 1. 目标树形与类型表

```
Node(抽象基)
 ├─ Group(children)
 │    ├─ MatrixNode : Group   子树局部变换（可嵌套、可累积）
 │    └─ StateNode  : Group   子树渲染状态（见 graphics-state.md）
 └─ Geometry : Node           叶子（数据+program+材质+可见/透明度）
```

| 类型 | 职责 | osg 对应 | vsg 对应 |
|---|---|---|---|
| `Node` | 抽象基：name / 子 bounding / 遍历 | osg::Node | vsg::Node |
| `Group` | children 容器 | osg::Group | vsg::Group |
| `MatrixNode` | 子树局部矩阵（沿路径累积为 world） | osg::MatrixTransform | vsg::MatrixTransform |
| `StateNode` | 子树渲染状态（depth/cull/blend…，子覆盖父） | osg 的 StateSet（挂任意 node） | vsg::StateGroup |
| `Geometry`(叶子) | 数据 buffers + primitive + program + material/opacity/visible | osg::Geode 挂 Drawable | vsg::Geometry（叶子） |

## 2. 关键决策

1. **Geometry 是叶子 Node（vsg 先例），不引入 Geode**：Geometry 直接挂在
   `Group/MatrixNode/StateNode` 下，`MatrixNode{ Geometry, Geometry }` 天然成立；比 osg 的
   `Geode + Drawable(非 Node)` 少一层。
2. **去掉 Drawable**：visible/opacity/material 全部并入 Geometry（叶子）。
3. **去掉 "renderable 内持 Shape"**：Shape 保留在 `vn::geometry`（loader/urdf 继续产 Shape），
   新增**转换工具函数** Shape→buffers；Geometry 不持有 Shape。
4. **变换只在 MatrixNode**：Geometry 不带局部/世界变换；world matrix 由 MatrixNode 沿路径累积，
   渲染收集时烤成 RenderCommand.modelMatrix（现状 `collectRenderCommands` 已在烤，语义照旧）。
5. **Scene 只持 root Node**：`setRoot/getRoot`；树形增删由 Group/父节点自身负责；
   `Scene.lights()`、内容绑定等不变。

## 3. Geometry 数据与查询

```cpp
enum class PrimitiveType { Triangles, Points, Lines, /*…*/ };

class Geometry : public Node {
  // 数据（loc 0 = position，唯一定死的最小约定；其余 loc 由 program 解读）
  void setBuffer(uint32_t location, intrusive_ptr<Buffer> data);
  raw_ptr<Buffer> buffer(uint32_t location) const;
  void setIndexBuffer(intrusive_ptr<Buffer> indices);   // 可选
  void setPrimitiveType(PrimitiveType);

  // 着色（null = 引擎默认内置，见 graphics-shader.md）
  void setProgram(intrusive_ptr<ShaderProgram>);

  // 材质 / 可见 / 透明度（并入自 Drawable）
  void setMaterial(intrusive_ptr<Material>);  raw_ptr<Material> material() const;
  void setVisible(bool);  bool isVisible() const;
  void setOpacity(float); float opacity() const;

  Aabbd boundingBox() const;   // 由 loc0 position（±index）计算
};
```
- `Buffer` 载体：优先复用 `vn::geometry` 的数组（Vec3fArray/ColorArray/UInt32Array，loader 已在用），
  自定义通道用泛型 float 数组——避免再发明一套容器。
- **bbox/视锥裁剪/RayIntersection 都改读 loc0 position + index**（所以 loc0 约定不能丢）。

## 4. 归属总表

| 职责 | 归属 |
|---|---|
| 变换 | MatrixNode（叶子不持有） |
| 渲染状态（depth/cull/blend…） | StateNode（子树；叶子不设，见 graphics-state.md） |
| 数据/图元 | Geometry |
| 着色 program | Geometry(null=默认) 或 StateNode(子树)，叶子优先 |
| material / opacity / visibility | Geometry（叶子）；Node/Group 保留子树可见性（现状已有） |
| 光源 | Scene 级（v4a 既定，不变） |

## 5. 收集/遍历（CPU 侧）

- `Scene::collectRenderCommands` 沿 root 遍历：累计 world matrix（MatrixNode）、折叠状态
  （StateNode → 生效状态，交给后端做管线变体键）、收集可见 Geometry 的 RenderCommand
  （数据特征 + material + program 引用 + world matrix + 生效状态摘要）。
- 状态折叠与可见性/opacity 折叠规则见 graphics-state.md；program 解析链见 graphics-shader.md。
- **剔除的成本结构（含实测）见 §9；持久化世界 AABB 的重设计方向见 §10。**

## 6. 波及面与重构分期（建议增量）

影响：`Node`（拆分职责）、删 `Drawable.hpp/.cpp`、`Geometry` 重写、`Scene` 简化、
`RayIntersection`（读 buffers）、`SceneBridge`（MatrixTransform 层级 + per-geometry StateGroup 由
生效状态/数据装配）、GraphicsTest 84 全量适配。

分期：
1. 先加 `StateNode` + `MatrixNode`（保留 Drawable/Shape，双轨试水，测试不破）；
2. `Geometry` 增加 `setBuffer(loc)/setProgram/primitiveType` 槽（Shape 路径仍可用）；
3. 删 Drawable、Shape 转工具函数、collect/拾取改 buffers——一步大重构前先各文件就位。

## 7. 决策记录（2026-09-03）

1. Node 派生为 `Group/MatrixNode/StateNode`，Geometry 为叶子 Node；不引入 Geode（vsg 先例）。
2. 删 Drawable（职责并入 Geometry）；删 renderable 内持 Shape（保留 geometry 模块 + 转换工具）。
3. 变换只在 MatrixNode；Scene 只持 root；loc0=position 为唯一定死的数据约定。
4. 状态归属 StateNode（详见 graphics-state.md）；着色归属 program 槽（详见 graphics-shader.md）。

### material 归属决策（2026-09-03，R1 评审确认）⭐

**material / opacity / visible 归 `Geometry`（叶子）；不放进 StateNode。**

- 分界：StateNode 装**管线状态**（depth/cull/blend/polygon/topology，按 batch 共享、深层覆盖浅层）；
  material 是**单对象外观数据**（diffuse/specular/纹理/透明度，可每帧动画），两者不混。
- 直觉：`MatrixTransform{ Geometry红, Geometry蓝 }` 同子树各自颜色是常态；若 material 变子树状态，
  每色要包一层 StateNode，且把"继承/覆盖折叠"引入到数据侧。
- 拓扑反例：topology 之所以移去 StateNode，是因为它是纯管线属性，与数据面统计无关
  （`vertexCount` 不随拓扑变；`triangleCount` 因数据不保证是三角网格已于 2026-09-04 移除）；material 无此性质。
- 后端现状已匹配：`RenderCommand.geometry/material` 分装，SceneBridge per-geometry 装配材质描述符、
  MaterialManager 按 `Material*` 去重。
- 兜底（先不做，等真实用例）：若需"整棵子树统一材质"，扩展方式 = 叶子 `material()==nullptr` 时沿
  父链取最近 StateNode 的材质覆盖，而非把 material 常态化放进 StateNode。


## 8. 与既有文档关系

- 本稿是 **graphics SDK 场景图重构**设计；`vine-shader.md` 是后端（vsg）自写内置 shader 的 P0
  落地稿，两者互补：本稿定 SDK 形状，vine-shader.md 定后端装配细节。
- `graphics-design.md` 中关于 Node/Drawable 的旧描述，在重构落地后需同步更新（本稿为后续版本）。

## 9. 剔除与界的成本结构（2026-09-17 实测）

视锥剔除**结构正确，但成本不随可见量下降**，原因在"盒"是怎么算出来的：

```cpp
// Scene.cpp:239（每个节点，下降之前）
if (frustum.isOutside(bounds.worldBound(node, world))) return;   // 整棵子树不再下降
```

- 早退省掉的是**下降**（可见性检查、状态折叠、叶子发射命令），**不是盒的计算**；
- `BoundsCache::worldBound`（`Scene.cpp:164-179`）对容器是**它所有后代盒的并集** ⇒ 为了让根/容器能被判成"在外"，它整棵子树的盒**必须先算出来**；
- `BoundsCache` 只在**一次收集内**去重，而收集本身按 `(content_revision, eye, view_proj)` 缓存 ⇒ **相机一动或内容一改就每帧重来一遍**（`Geometry.cpp:315` 的注释也写了这点）；世界盒与相机无关，却跟着相机一起被重算。

### 9.1 实测（Release `-O2`；同一个 N、同一个"可见 95 条命令"的四种形状）

| 形状 | N=10 000 | N=100 000 | ns/节点 |
| --- | --- | --- | --- |
| 全部可见（对照：walk + 发射 + 排序都是真活） | 5.61 ms | 72.8 ms | 561 / 728 |
| 99% 是散落的兄弟节点（都在视锥外） | 1.94 ms | **24.7 ms** | 194 / 247 |
| 99% 挂在**一个**视锥外的 Group 下 | 1.82 ms | **25.7 ms** | 182 / 257 |
| 理想持久化代理（只存在那 95 个可见节点） | **0.19 ms**（N=1000） | — | 186 |

Debug 树（`build/`）同比值：被剔节点 ≈3.4 µs、可见 ≈5.2 µs/节点。

**关键读法**：把被剔的 99% 从"散落兄弟"改成"挂在一个视锥外的 Group 下"——也就是把整片下降与逐节点测试**全省掉**——只从 24.7 ms 变成 25.7 ms（落在噪声里）。⇒ 那 25 ms 几乎是**"为了让容器能被判成在外而先算出来的盒"**。

⇒ 单位经济学：**每个"存在但不可见"的节点、每次收集 ≈ 250 ns（Release）/ 3.4 µs（Debug）**。
100k 节点、可见 95 条命令 ⇒ **每帧每相机 24.7 ms 只花在看不见的东西上**（60 fps 下约 1.5 个帧预算）。

### 9.2 测量配方（可复现，**不进 CI**）

临时把 `tests/test_graphics/SceneCollectBench.cpp` 加进该目录 `CMakeLists.txt` 的 `SRC_FILE_LIST`（**tab 缩进**），
`cmake -S . -B build && ninja -C build test_graphics`（Release 用 `build-release`），再
`./build/bin/test_graphics --gtest_filter=SceneCollectBench.*`；跑完删文件并还原 `CMakeLists.txt`。

负载形状：一个 root + N 个 `MatrixTransform{ Geometry(共享同一 positions buffer) }`，每帧换一次 `setContentFrame` token
（等价于"相机在动/内容被改"，即 memo miss）；四种形状 = 全部可见 / 99% 散落兄弟 / 99% 一个在外 Group / 只存在可见的那 1%。
四者报告的都是**真实** `commands.size()`，所以"可见量相同"这件事是被验证过的，不是假设。
不进 CI 的理由：25–500 ms 级、依赖机器，做成门禁会拖慢套件又不可靠。

## 10. 重设计：持久化世界 AABB（**§10.1 / §10.2 均已落地**）

结论：这是**"场景一大就撞墙"的项**（§9 实测上限 ~99%）。前置 **§10.1 与本体 §10.2 已于 2026-09-25 落地**：
契约形如"**公告驱动的版本键 + unknown 回退**"，sound 性由构造保证（自定义节点不公告 ⇒ 其祖先退回从零推，
永远正确）⇒ 不再等触发条件；代价是不可公告的自定义摆放拿不到缓存收益（见 §10.2 的边界）。

### 10.1 前置（零失效风险，可先做）

把两件事分开：**局部盒（数据派生，已按数据身份缓存）** 与 **世界盒（摆放派生）**。
现状 `Geometry::boundingBox()` 把两者混着做（`return transformBox(local_bounds_.box, worldMatrix());`），
而 walk 手上已经有累乘好的 `world` ⇒ 叶子每次收集都白走一遍父链（O(depth)），再加上这个重复本身。

```cpp
Aabbd Node::localBounds() const;                                // 数据派生：按数据身份缓存（local_bounds_ 已有）
transformBox(node->localBounds(), world /* walk 传下来的 */);    // 摆放派生：walk 本来就有
```

- 收益：每次收集的界成本从 **O(节点数 × 深度)** 降到 **O(节点数)**（每节点 8 个角点，不再重算父链）；
- **零失效面**：仍然每次收集从零推 ⇒ 对任何 `Node` 子类都 sound；
- 门禁：一条"两种拼写必须一致"的测试，照 `InheritedState` 那条先例
  （"同一规则的两种拼写必须有测试对齐"）：`boundingBox(worldMatrix()) == transformBox(localBounds(), worldMatrix())`。

**已落地（2026-09-25）**：

- `Geometry::localBounds()` 公开（数据派生的局部盒，缓存语义原样搬过去）；`Geometry::boundingBox()` 变成
  `transformBox(localBounds(), worldMatrix())`；`transformBox` 从 `Geometry.cpp` 的匿名命名空间提到
  `Node.hpp`/`Node.cpp`（一个拼写给 walk 与节点共用，带 Doxygen）。
- `Scene` 的 `BoundsCache` 叶子分支：**恰好是 `Geometry` 类**时走 `transformBox(localBounds(), world)`
  （walk 的 `world` 对每个访问节点就是 `node->worldMatrix()`）；**`Geometry` 的子类保持走虚函数
  `boundingBox()`**——先例是测试里的 `CountingGeometry`（覆写它并被要求只被问一次），子类扩展行为不静默改变。
- 门禁：`NodeTest.TheLocalBoxPlacedByTheWorldMatrixIsTheWorldBox` 钉"两种拼写相等"（带旋转/平移/非均匀
  缩放的嵌套链，并钉局部盒不随摆放变化）；`test_graphics` 两棵树 **277/277**；vsg 门禁两棵树
  `cases=445 vuid=0 hazard=0`、应用画面行**逐字不变**。
- 实测（临时基准，形状照 §9.2，跑完已删）：100k 节点"平铺 ~1% 可见"收集 **~38.7 → ~34.0 ms/帧**（−~12%）；
  深链 8 层（225k 节点）**~38.0 → ~36.8 ms/帧**（−~3%）；全可见形状不变（发射主导）。
  **§9 的 ~250 ns/节点大头仍在**——那部分正是 §10.2 的持久化要吃的。

### 10.2 持久化本体（2026-09-25 落地）：每节点持久**子树盒（自身坐标系）** + 公告驱动版本键

**契约决定**：持久保存的不是"世界盒"，而是**子树盒在该节点自身坐标系里的值**——它与相机无关，也与
**本节点自身的摆放**无关（`setMatrix` 只改本节点→祖先这条前缀的合成，子树形状没变）⇒ 失效只需**上行**。

三条规则：

1. **版本键上行**：`Node::invalidateBounds()` 从本节点沿 `parent_` 链把每个节点的 `bounds_stamp_` 加一。
   内置变更路径全部挂钩：`MatrixTransform::setMatrix`、`Group::addChild/removeChild`、
   `Geometry::addBuffer/removeBuffer/setRevision/bumpRevision`、`Scene::invalidateContent`（另起一层：
   内容帧号 + 全体 stamp 加一，作为"懒得逐条公告"的逃生口）。
2. **unknown 回退（sound 由构造保证）**：`bool Node::subtreeBounds(Aabbd&)` 只对**恰好是 `Geometry`** 的叶子
   回答 `localBounds()`（§10.1 的数据身份缓存）；恰好是 `Group` 类（含**子类**——容器语义由结构决定）时
   查缓存/重算；**任何其他子类**（自定义摆放/自定义盒）一律返回 false ⇒ 使用方（`Scene` 的 walk）对该节点
   **整棵子树每次从零推**。漏失效因此不可能发生：没公告的节点根本不会被缓存回答。
3. **显式公告口**：自定义节点想在祖先处吃到缓存，就自己调 `Node::invalidateBounds()`（SDK 文档已写）。
   缓存命中后 `boundsRecomputeCount()` 可观测量供门禁/诊断用（每次子树并集重算 +1）。

**实现形状**（`Node.hpp` / `Node.cpp`）：

```cpp
void invalidateBounds() noexcept;                 // 上行 bump（自己 + 祖先）
[[nodiscard]] bool subtreeBounds(Aabbd& box) const;   // 子树盒，在 *this 的坐标系里；false = unknown
[[nodiscard]] std::uint64_t boundsRecomputeCount() const noexcept;
```

- 容器命中 `cached_known_ && cached_stamp_ == bounds_stamp_` 直接返回；否则重算
  `∪ transformBox(child->subtreeBounds(child_box), child->localTransformMatrix())`；**任一子节点 unknown
  ⇒ 本节点也不留缓存**（`cached_known_ = false`，不把半算的并集留下）。
- 恰好 `Node`（无子、无数据）⇒ 空盒、known；叶子（`Geometry`）不算容器并集，直接数据盒。
- 内存：容器 ≈ 48 B 盒 + 8 B stamp + flags；叶子只多 stamp。100k 容器 ≈ 5.7 MB。
- `Scene::worldBound` 的顺序：`subtreeBounds` 快路径 → 否则容器从零推（与旧实现同形）→ 否则 `boundingBox()`。
  walk 的 `world` 累乘仍照旧，所以"快路径命中"与"从零推"两种拼写产出的世界盒逐位一致（§10.1 的对齐测试仍然成立）。

**风险与边界（诚实陈述）**：

- 漏失效的后果是**剔错 ⇒ 物体凭空消失**（不是慢一帧），属"结构门禁全绿也照样漏"的一类（V5 先例）⇒
  所以设了 5 条防火墙测试 + 4 条**变异电池**（每条把一类公告改回静默 ⇒ 对应测试必须红；恢复后全绿）。
- `Scene` 的 `setContentFrame` token **不能**当失效依据：`RenderEngine` 每渲染帧公告一个新 token ⇒ 拿它做键
  等于永远 miss（这也是 §9 里"每帧重算"的来源）。
- **unknown 子树会让其所有祖先退回从零推**（sound 但零收益）——自定义 placements 想拿收益必须公告，
  这是契约有意留下的唯一代价。

**门禁（两棵树 282/282；变异电池全部会红）**：

- `SceneTest.CachedSubtreeBoxesSurviveCameraMovesAndFollowAnnouncements`（相机移动缓存存活 + `boundsRecomputeCount` 不涨；
  一次 `setMatrix` 后恰好涨一次）
- `SceneTest.ASubtreeMovedIntoViewIsNotHiddenByAStaleBox`（移入视锥的子树不被陈旧盒藏掉——"物体消失"形态）
- `SceneTest.AChildAddedLaterExtendsItsParentsBox`
- `SceneTest.AnEditedGeometryAnnouncesItsBoundsThroughItsSetters`
- `SceneTest.ACustomPlacementAnnouncesThroughInvalidateBounds`（自定义 `localTransformMatrix()` 的节点；
  公告后必须被发现）
- 变异：`setMatrix` 静默 ⇒ 红；`addChild` 静默 ⇒ 红；`addBuffer+bumpRevision` 同时静默 ⇒ 红；
  `invalidateBounds()` 空实现 ⇒ 红。

**实测**（临时基准，§9.2 配方外加 §9 头号形状；Release `-O2`；"缓存关"= 命中判断禁用后的对照，跑完已删）：

| 形状 | 缓存关（对照） | 缓存开 | 倍率 |
| --- | --- | --- | --- |
| 全可见 200k 节点 | 129.0 ms | 112.9 ms | 0.88×（盒每节点只算一次，对照反而重复） |
| 平铺 ~1% 可见 200k 节点 | 39.9 ms | 16.8 ms | 2.4× |
| 深链 8 层 ~1% 可见 225k 节点 | 50.2 ms | 4.7 ms | 10.7× |
| **1k 可见 + 99k 挂一个视锥外 Group**（§9 头号） | 31.8 ms | **0.55 ms** | **57×** |

⇒ §9 的 ~250 ns/节点被压到"每个**被剔根**一次盒测试"；顶部形状正是 §9 说"下降全省掉只值 4%"的那个——现在
它的 99.9% 时间花在可见的 1k 个节点上。

### 10.3 顺序（已完成）

1. **§10.1 前置（2026-09-25 完成）**：局部盒与世界盒拆开，walk 用自己已算好的矩阵——世界盒要有自己的
   身份，才有资格被持久化。
2. **§10.2 本体（2026-09-25 完成）**：每节点持久子树盒（自身坐标系）+ `bounds_stamp_` 版本键 +
   `Node::invalidateBounds()` 显式公告口；"漏失效"由 5 条防火墙测试 + 4 条变异电池钉住（见 §10.2）。
3. 余量：unknown 子树（自定义节点未公告）会让其祖先退回从零推；若真实负载出现"自定义摆放 + 大子树"的组合，
   再做按类型的公告适配（或要求这类节点显式 opt-in），不动当前契约。
