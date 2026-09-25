# Graphics 渲染状态设计（StateNode / 状态继承）

> 状态：设计稿 v1（2026-09-03），评审对象。
> ⚠ 落地后修订（2026-09-03）：
> **后端消费已落地（vsg RenderStateMapper）**，此前两个待拍决策已定：
> 1. **深度约定（已决）**：核对 vsg 源码 `maths/transform.h`——其 perspective/orthographic 是
>    **reverse-depth（1→0）**，故 vsg 默认 `VK_COMPARE_OP_GREATER` 正确。SDK `CompareOp` 注释本就
>    是"closer/farther"**距离语义**（非裸 NDC），映射到 reverse-Z 时 Less/Greater 一族**在边界反转**
>    （Less→VK GREATER，LessEqual→GREATER_OR_EQUAL，Greater→LESS，GreaterEqual→LESS_OR_EQUAL，
>    Equal/NotEqual/Always/Never 直映）。默认路径 = vsg GREATER = 现状不变（零回归）。
> 2. **Blend 语义（已决）**：per-vertex-alpha opacity 使引擎**必须常开 alpha blend**，故 `blend.enabled`
>    不作为"是否合色"，而作为"是否用自定义因子"：enabled=true 用 StateNode 的 src/dst；默认
>    (disabled) 用 SrcAlpha/OneMinusSrcAlpha。即 vsg 后端无法通过 StateNode 关掉 alpha blending
>    （若确需"无 blend"，走 depth-only pass 或未来的显式 no-blend 标志）。
> 其余映射：CullMode→cullMode（**frontFace = CLOCKWISE** —— vsg 的投影翻转 Y，写成直觉的 CCW 会让每个 cull
> 模式作用在错误的面；D6 修正，见 `RenderStateMapper.hpp`）；PolygonMode→polygonMode；
> Topology→InputAssemblyState.topology（Triangles/Points/Lines）。
> **对账（2026-09-25）**：winding 已钉——D6 修正 + 像素用例（`VsgBackendTest` 状态区制：Back 剔除下网格仍在）；
> blend 因子由 `ContentDrawTest` 逐项映射断言钉住（每附件的 blendEnable/因子），**像素级专项仍未做**。

> 落地记录（2026-09-03）：
> - SDK 类型已实现——`StateNode`（Group 子类）+ `CompareOp/CullMode/BlendFactor/PolygonMode/
>   **Topology**/DepthState/BlendState/RenderState/ResolvedRenderState` +
>   `collectRenderState/resolveRenderState/effectiveRenderState` 折叠函数（`StateNode.hpp/.cpp`），
>   GraphicsTest StateNodeTest 8 用例全绿；`RenderCommand.renderState` +
>   `Scene::collectRenderCommands` 每叶折叠填入（Scene 集成用例全绿）。
> - **后端消费（本次）**：`src/plugins/gfx_backend_vsg/include/vine/vsg/RenderStateMapper.hpp`
>   （纯、device-free）：`makeRenderStateObjects(resolved) -> {DepthStencil, Rasterization,
>   ColorBlend, InputAssembly}` + `applyRenderStateObjects(config, …)` 替换 pipelineStates 对应项；
>   `SceneBridge::buildGeometry` 按 cmd.renderState 装配管线（替代原 enableBlending 特判）；
>   `SceneBridge::Item` 增 `render_state`，resolved 状态变化触发保留子树重建。
>   测试：`tests/test_vsg/RenderStateMapperTest.cpp` 6 用例（默认=现状、depth 反转、cull/polygon/
>   topology/blend 因子映射）全绿；test_vsg 直跑 8/8。
>   ⚠ 默认 resolved 状态产出的字段值 == 原共享默认管线，故无 StateNode 的场景零视觉变化。
> ⚠ Topology（默认 Triangles）= 渲染状态项；曾误放 Geometry 的 PrimitiveType 已移除。Geometry 数据面
> 计数 `vertexCount` 为**纯数据统计**，不随 Topology 变（有测试钉住）。`triangleCount()` 已于 2026-09-04
> 移除：Geometry 顶点数据不保证是三角网格（geometry 模块 `Mesh::triangleCount()` 保留）。
> 关联：`graphics-scene-graph.md`、`graphics-shader.md`（生效状态参与管线变体键）、
> `graphics-render-pipeline.md`（视图/pass 提供默认状态）。
>
> **一句话**：State = **子树级、可选、可继承覆盖**的渲染状态集合，承载在 `StateNode` 节点上；
> 叶子 Geometry 不直接设状态；生效状态沿 root→leaf 折叠（子覆盖父），作为后端**管线变体键**。
> 与 vsg::StateGroup / OpenGL 状态栈同一模型，后端无关。

## 0. 为什么是 StateNode（而不是"状态并入 Geometry"）

- per-geometry 状态会把"每个几何 = 一个完整管线变体"变成常态，破坏合批；
- vsg 把状态放在祖先 `StateGroup`（沿子树压栈/弹栈）；OpenGL 后端同样有状态机
  （glEnable/glDepthFunc/glCullFace/glBlendFunc + 状态栈）——三者同构；
- `StateNode` 让"一批同状态几何共享一个变体"，且可嵌套组合（如"这整块无深度写"）。

## 1. 状态项集合（全部 optional，未设 = 继承）

| 状态项 | 取值示例 | 说明 |
|---|---|---|
| `DepthTest` | on/off | 与视图默认的关系见 §4 |
| `DepthWrite` | on/off | |
| `DepthCompare` | Less/LEqual/… | |
| `CullMode` | None/Back/Front | 双面/背面剔除 |
| `Blend` | on + src/dst | 透明混合（默认跟 opacity 机制联动） |
| `PolygonMode` | Fill/Line/Point | 线框/点渲染 |

> 范围克制：先只做这 6 项；视口/裁剪矩形属于 pass 级（现状 setViewport），不进 StateNode。

## 2. StateNode 定义

```cpp
struct DepthState  { bool test=true; bool write=true; DepthCompare compare=LEqual; };
struct CullState    { CullMode mode=Back; };
struct BlendState   { bool enable=false; BlendFactor src; BlendFactor dst; };
struct RasterState  { PolygonMode mode=Fill; };      // 可按需拆
// 每项 std::optional，未设=继承

class StateNode : public Group {
  // 设/清某一项；未设的项在生效时继承祖先
  void setDepthTest(bool on);             void clearDepthTest();
  void setDepthWrite(bool on);            void clearDepthWrite();
  void setCullMode(CullMode);             void clearCullMode();
  void setBlend(const BlendState&);       void clearBlend();
  void setPolygonMode(PolygonMode);       void clearPolygonMode();
  // 子树统一着色（可选，见 graphics-shader.md）
  void setProgram(intrusive_ptr<ShaderProgram>);
};
```
- 成员用 `std::optional` 表达"未设"，语义干净且可直接参与折叠。

## 3. 继承规则

**实现是"每叶独立合成"，不是压栈 / 出栈还原。** `Scene::collectNodeCommands` 在**每个叶子**处调用
`collectRenderState(node)`：它先把叶子→根收集成链，再**根→叶**依次 `merge`，所以越深的节点越优先。
`merge` 是**逐项**的（每项一个 `optional`），因此深层只设了 `depth` 时，外层的 `blend` 仍然生效。

没有持久状态、也没有还原步骤 —— "离开子树"在实现上**不需要任何处理**，因为不在链上的节点自然不参与。
代价是每个叶子都重走一次祖先链（M 个几何体 × 深度 D）。

- 叶子 Geometry 拿到的"生效状态" = 路径上所有 StateNode 覆盖后的结果。
- **叶子 Geometry 不直接设状态**（避免 per-object 变体泛滥）；除非出现真实用例再开白名单。

### 3.1 两条继承规则，按属性的种类选

同一个 `StateNode` 上有**两套**规则，不要混：

| 规则 | 形状 | 适用 |
|---|---|---|
| **逐项折叠**（`collectRenderState` + `merge`） | 每项一个 `optional`，根→叶覆盖，**所有**祖先都参与 | 渲染状态：depth / cullMode / blend / polygonMode / topology |
| **就近胜出、找到即停**（`effectiveProgram` / `effectiveMaterial`） | 单值指针，叶子优先 → 最近的祖先，**首个命中即结束** | program、material |

判据：**单值、指针型、整体替换** ⇒ 后者；**多值、可逐项覆盖** ⇒ 前者。这与属性最终落到哪里是一致的 ——
渲染状态是**管线状态**（进 variant 的状态键），而 program / material 在 variant 键里是**各自的独立入参**。

- **material 可设在 `StateNode` 上**（`setMaterial` / `clearMaterial` / `material()`）：子树共用一份材质，
  叶子用 `Geometry::setMaterial` 覆盖，省掉"每个 Geometry 都设一遍"；都未设 ⇒ `null` ⇒ 引擎默认。
- **`Geometry` 自身没有任何渲染状态字段**（只有 `program()` 与 `material()`）：状态只能来自祖先 StateNode。
- 材质继承**不引入新机制**，也不改变合批 —— 本渲染器**没有合批**：每几何体一条命令、一个 drawable，
  只共享编译好的管线（管线共享省的是重编译，不是重复绘制）。

## 4. 与视图/pass 默认状态的关系

- 视图/pass 提供**基底默认状态**（主视图：depth test/write on、cull none、blend 随 opacity；
  overlay：depth off、blend on）——可视为"视图根上的默认 StateGroup"。
- 用户 `StateNode` 在其下覆盖；无用户 StateNode 时行为 = 现状（零回归）。

## 5. 管线变体与缓存键

- **变体键 = `(生效状态 + program + 数据特征/图元 + material 相关)`**，后端缓存；
  同状态同数据的几何共享同一管线。
- 与现状"blend 常开 + per-vertex alpha 免重编"的关系：opacity 机制保留（leaf opacity 走
  per-vertex alpha），`Blend` 项只在用户显式改 src/dst 时才新增变体。

## 6. 后端映射

| SDK | vsg | OpenGL(假想) | 手写 Vulkan |
|---|---|---|---|
| StateNode | vsg::StateGroup(StateCommands，含按生效状态装配的 GraphicsPipeline) | 遍历时 glEnable/glDepthFunc/glCullFace/glBlendFunc + 状态栈 | 生效状态烘进 pipeline state；变体缓存 |
| 生效状态→管线 | 每几何 StateGroup（现 SceneBridge 模式推广） | 无管线（状态直接下发） | 变体键 |

## 7. 决策记录（2026-09-03）

1. 状态承载在 **StateNode（Group 子类）**，子树级、可继承；叶子不直接设状态。
2. 状态项全部 **optional**，生效 = 沿路径折叠、子覆盖父（vsg/GL 同款）。
3. 视图/pass 提供基底默认 → 无用户 StateNode 时零回归。
4. 生效状态进入**管线变体键**；opacity 仍走 per-vertex alpha，不因加状态而推翻。

## 8. 打开项

- 是否允许叶子 Geometry 白名单式覆盖个别状态（真用例出现再加）；
- StateNode 的 program 槽与 Geometry program 的优先级（暂定叶子优先）；
- 排序/透明（depth-sorted bin）是否算状态（现状由 RenderCommand.isTransparent 驱动，另文处理）。
