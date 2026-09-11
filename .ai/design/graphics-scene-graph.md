# Graphics 场景图设计（Node 派生 / OSG-vsg 风）

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
> - 遗留：program 槽（graphics-shader.md）、StateNode 的 renderState 后端消费、点云数据型自定义通道——
>   均留待后续切片；`Geometry` 的开放 loc buffer 列表已在早前切片落地。
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
material）；`Geometry : Drawable`（包 `vine::geometry::Shape`）。问题：
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
3. **去掉 "renderable 内持 Shape"**：Shape 保留在 `vine::geometry`（loader/urdf 继续产 Shape），
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
- `Buffer` 载体：优先复用 `vine::geometry` 的数组（Vec3fArray/ColorArray/UInt32Array，loader 已在用），
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
