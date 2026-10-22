# 实例化：`InstanceScope` 作用域设计（2026-10-22，历史登记）

> **状态：历史记录，已被取代**。实例化最终收敛为 `Primitive` 级实例流，见
> `graphics-instancing-primitive-2026-10-23.md`。本文保留“为什么作用域方案被否决”的记录：
> 选择单位、射线递归、间接嵌套与包围盒语义都随“允许子 Group”一起变复杂。
>
> 历史关系：本文取代同日的“几何保留通道”方案
> （`graphics-instancing-channel-2026-10-22.md`），随后又被 `Primitive` 级设计取代。
>
> 关联：`.ai/design/graphics-object-model-vs-3d-frameworks.md` §3 缺口 2、
> `.ai/design/graphics-followups-2026-10-19.md` §3.3、上游 vsg 的 `vsg::InstanceNode` /
> `vsg::InstanceDraw` / `vsg::Draw`（`build/_deps/vsg-src`）。

## 0. 结论

| # | 决定 | 理由 |
| --- | --- | --- |
| **D1** | 实例数据挂在 **`InstanceScope` 作用域节点**（`Group` 的子类），不挂在 `Geometry`，也不新建一种 drawable | 实例是“绘制”的事实，不是“网格”的事实；vsg 的 `InstanceNode` 就是作用域形状；不污染万能 `Geometry` |
| **D2** | 载荷 = 每实例 16 float 列主序 mat4；作用域持有一段 `BufferSlice<float>`（本作用域的实例段）+ 可选 `std::optional<size_t> visibleInstances`（默认没有，即整段） | three.js/Godot/Unity/Unreal/Filament 都用矩阵；vsg 用 TRS+color，但我们的着色器 ABI 和现有项都是 mat4 |
| **D3** | 收集期折叠：作用域给子树中每个 `Geometry` 的每条命令带上实例切片 + `first_instance`/`instance_count`（值，不是作用域指针）；一条命令 = 一个 `(geometry, primitive)` 画 N 个实例；多个 part 共享同一实例流 | 与 `StateNode`/`MatrixTransform` 的折叠模型同构；命令是帧值，不借场景对象的寿命；一个作用域下的多个几何共用一次实例上传 |
| **D4** | 后端 `ContentDraw::Draw` 加 `first_instance`；实例流作为 binding 4 追加进 `vertex_binds`；`vsg::Draw`/`DrawIndexed` 本来就接受 `instanceCount`/`firstInstance` | 同一族 `vkCmdDraw*` 的参数差异，不是新命令；不引入 `vsg::InstanceDraw` |
| **D5** | `VINE_INSTANCED` 是唯一开关：`ProgramVariant.instanced`（bit 8）决定程序文本；`ContentPipeline::create` 从 `shaders.defines` 看到它时追加 binding 4 + 属性 12–15，并检查源码的 `#pragma import_defines` 确实列了它 | 文本与顶点布局必须一起变；vsg 只交付 pragma 里列出的 define，漏写会静默丢弃；不新增 `PipelineKey`/`ContentHalves` 字段（variant 已在键里） |
| **D6** | 实例流按**整段缓冲**上传（location 12、16 分量、offset 0、count 0）；`firstInstance`/`instances` 是 draw 参数 | 可见数变化不重传；与 vsg/Godot/Filament 的“容量 vs 绘制数”一致 |
| **D7** | 包围盒 = 子树局部盒经**可见**实例矩阵变换后的并集；拾取逐可见实例测试，返回子 `Geometry` + `primitiveIndex` + `instanceIndex` + 可选选择节点（`InstanceScope` 或 `Geometry`） | 剔除、阴影相机拟合、拾取都不能假装实例不存在；vsg 的 `ComputeBounds` 也按实例展开 |
| **D8** | 第一片不做：嵌套 `InstanceScope`（直接嵌套被 `addChild` 拒绝；更深层由最近作用域胜出，不保证组合）、逐实例材质/颜色/层/排序/剔除、GPU/indirect、自动合批 | 先把“同一网格 N 份、同材质、静态”做对；其余要真触发再加。作用域与 `Geometry` 之间的 `MatrixTransform` 是允许的：实例矩阵在**被绘制节点的局部空间**里乘，子变换先于实例矩阵 |
| **D9** | 与 vsg 的关系：借 `InstanceNode` 的 **作用域** 形状，不直接用它的类 | vsg 的类只支持 TRS+color、child 限制（不能有 transform/culling/嵌套/标准叶子）、record 期指针、无 intersector；我们的命令图是值 + 任意状态/管线 |

## 1. 背景

### 1.1 今天的事实

| 事实 | 位置 | 对设计的影响 |
| --- | --- | --- |
| 一条 `RenderCommand` 画一个几何的一个 primitive | `RenderCommand.hpp` / `Scene::collectRenderCommands` | N 份重复内容 = N 个节点 + N 条命令 + N 次 draw |
| `ContentDraw::Draw::instances{1}` 已存在，`record()` 已把它传给 `DrawIndexed`/`Draw` | `ContentDraw.hpp/.cpp` | 后端“提交 N 个实例”的能力已经有了，只差数据和 `firstInstance` |
| `ContentPipeline::VertexBinding::per_instance` 已映射到 `VK_VERTEX_INPUT_RATE_INSTANCE` | `ContentPipeline.cpp` | 实例步进的管线声明已经有了，只差调用者传 `true` |
| `ContentPipeline::create(abi, GeometryFacts, …)` 一律 `per_instance = false`，且只接受 `bindingOfCanonical` 能喂的规范通道 | `ContentPipeline.cpp` / `StreamUploads.cpp` | 实例矩阵必须是规范角色（位置 12–15），不能走自定义通道 |
| `GeometryFacts` 的“每个通道从同一 VERTEX 开始”检查按 `offset / components` 比较 | `GeometryFacts.cpp` | 本设计**不加**实例通道，因此不会触发这个坑；实例流不经过 `GeometryFacts` |
| `Node::subtreeBounds` 用 `typeid(*this) == typeid(Geometry)` 特判叶子，且该函数不是虚函数 | `Node.cpp` / `Node.hpp` | 新作用域节点要回答自己的包围盒，必须先把 `subtreeBounds` 虚化（S0） |
| 后端明确**不支持**自定义顶点通道（`acquireVertex` 拒绝、`ContentPipeline::create` 拒绝） | `StreamUploads.cpp` / `ContentPipeline.cpp` | 实例流只能以规范位置 12–15 进入 |
| `ContentHalves` 的键含 `VertexLayoutKey` + `variant` | `ContentHalves.cpp` | 实例化只要把 `ProgramVariant.instanced` 置位，half 自然分开，不需要新键字段 |
| 上游 vsg：`Draw`/`DrawIndexed` 自带 `instanceCount`/`firstInstance`；`InstanceNode` 是 record 期作用域，提供 `translations/rotations/scales/colors` 数组，child 只能是 `InstanceDraw`/`InstanceDrawIndexed` 叶子 | `build/_deps/vsg-src` | 形状可借鉴；类不能直接用（TRS、record 指针、child 限制、无 intersector） |

### 1.2 备选与裁决

| 方案 | 形状 | 为什么不用 |
| --- | --- | --- |
| **A. 几何保留通道**（已作废） | `Geometry` 多一条实例矩阵通道；`GeometryFacts.instances`；管线布局位 | 没有引擎把实例数据挂在网格资产上；一个 `Geometry` 只能一套实例集；可见数量要改切片（形状变更）；选择/材质共享都绑死；`GeometryFacts` 还要处理起点检查 |
| **B. 新 drawable 类型** | `InstancedGeometry : Node`（组合共享网格）+ 每命令实例流 | 引入了“第二个 drawable 身份”，选择/拾取/材质都要跟着改；共享网格的材质仍共享；vsg 也不是这个形状 |
| **C. 自动合批** | N 个普通节点 → 后端按相同 key 合成一次 `Draw(instances=N)` | 隐式优化；N 条命令、N 个节点仍在；只覆盖不透明且完全同材质的绘制；逐实例数据/大 N 不适用。**保留为未来的可选优化，不作为本设计的模型** |
| **D. `InstanceScope` 作用域**（采用） | 一个作用域节点 + 子树普通 `Geometry`；命令带实例切片/计数；后端每命令实例流 | 与 vsg 同形；不污染 `Geometry`；多个 part 共享实例；显式；选择单位是作用域；大 N 一条命令/part |

vsg 的 `InstanceNode` 不能直接用的原因：

- 只支持 `translations/rotations/scales`（TRS）+ 可选 `colors`，没有 mat4，也没有逐实例自定义数据；
- child 子树不能有 `Transform`、`Culling`、嵌套 `InstanceNode`、标准 `Geometry/VertexDraw/VertexIndexDraw`，只能是 `InstanceDraw` 叶子；
- 它是 **record 期指针**（`commandBuffer.instanceNode`），而我们的帧是值；
- `vsg::Intersector` 没有 `InstanceDraw`/`InstanceNode` 的 visit 分支，vsg 自己的实例化内容不可拾取；`ComputeBounds` 则支持。

### 1.3 行业对照（提炼）

| 引擎 / API | 实例数据挂在哪 | 每实例载荷 | 计数 | 包围盒 / 拾取 |
| --- | --- | --- | --- | --- |
| **vsg** | `InstanceNode` 作用域，child 是 `InstanceDraw` 叶子 | TRS + 可选 color | `firstInstance`/`instanceCount` | `ComputeBounds` 按实例；`Intersector` 不支持 |
| **three.js** | `InstancedMesh` 独立对象（引用共享 geometry/material） | `instanceMatrix` 16 float；可选 `instanceColor` | `count` | `computeBoundingBox/Sphere`；raycast `instanceId` |
| **Godot** | `MultiMesh` Resource + `MultiMeshInstance3D` | 扁平 buffer（3D transform 12 float + 可选 color/custom） | `instance_count` / `visible_instance_count` | `get_aabb`/`custom_aabb`；无拾取 |
| **Unity** | `DrawMeshInstanced` draw 调用；或自动 GPU instancing 的普通 `MeshRenderer` | `Matrix4x4[]` + `MaterialPropertyBlock` | 数组 + `count` | 整组 AABB；无逐实例剔除/排序 |
| **Unreal** | `UInstancedStaticMeshComponent`/HISM 组件 | `PerInstanceSMData` + custom data | `Add/Update/RemoveInstance` | HISM 簇树；`FHitResult.Item` |
| **Filament** | `RenderableManager::Builder::instances()` + `InstanceBuffer` | local transforms（自动最多约 64） | 容量 vs `setInstanceCount` | 一个 AABB；无拾取 |
| **Bevy** | ECS 实体 `Mesh3d` + `Transform`，没有 InstancedMesh 类型 | 每实体 `MeshUniform`，按 `instance_index` 取 | 实体增删 | 自动分批；按实体 |

共同模式：

1. **没有引擎把实例数据挂在网格资产上**；最接近的 Qt3D `QGeometryRenderer::setInstanceCount` + `QAttribute::setDivisor` 也在 renderer/draw 上。
2. 多数引擎用**专用对象/组件**或**draw 调用**；vsg 用**作用域**；Bevy/Unity 自动实例化走**合批**。
3. payload 多数是 mat4（vsg 是 TRS）；逐实例颜色/自定义数据是可选第二流。
4. **容量与绘制数通常分开**（vsg `firstInstance`/`instanceCount`、Godot `visible_instance_count`、Filament `setInstanceCount`、Unity `count`）。
5. 包围盒默认“全部实例一个盒”，例外是 vsg 的精确展开与 Unreal HISM 的簇树。
6. 拾取不一致：three.js/Unreal/Babylon 回实例索引；vsg/Unity/Godot/Filament 没有内置逐实例拾取。

## 2. 契约

### 2.1 SDK：`InstanceScope`

```cpp
class VN_GRAPHICS_API InstanceScope : public Group
{
  public:
    /** @brief Sets the instance segment this scope draws. */
    void setInstanceTransforms(BufferSlice<float> transforms);

    /** @brief Removes every instance transform (the scope is a plain Group again). */
    void clearInstanceTransforms();

    /** @brief Whether the scope currently carries instance transforms. */
    [[nodiscard]] bool hasInstanceTransforms() const noexcept;

    /** @brief Gets the instance segment (16 floats per instance, column-major). */
    [[nodiscard]] const BufferSlice<float>& instanceTransforms() const noexcept;

    /** @brief Gets the segment's start in instances (the draw's firstInstance). */
    [[nodiscard]] std::size_t firstInstance() const noexcept;

    /** @brief Gets how many instances the segment holds (its capacity). */
    [[nodiscard]] std::size_t instanceCount() const noexcept;

    /** @brief Limits how many instances from the segment's start are drawn. */
    void setVisibleInstanceCount(std::size_t count);

    /** @brief Draws every instance the segment holds again. */
    void clearVisibleInstanceCount();

    /** @brief Gets the drawn prefix, clamped to instanceCount(). */
    [[nodiscard]] std::size_t visibleInstanceCount() const noexcept;

    /** @brief Gets one instance's local matrix, relative to the segment. */
    [[nodiscard]] std::optional<Mat4d> instanceMatrix(std::size_t instance) const;

    /** @brief How many transform edits were ignored because they did not cover whole matrices. */
    [[nodiscard]] std::uint64_t ignoredInstanceEdits() const noexcept;

    /** @brief How many direct child scopes were refused. */
    [[nodiscard]] std::uint64_t ignoredScopeChildren() const noexcept;

    /** @brief Adds a child; a direct InstanceScope child is refused and counted. */
    void addChild(intrusive_ptr<Node> child) override;

    /** @brief World box of the subtree over the visible instance prefix. */
    Aabbd boundingBox() const override;

    /** @brief Local-frame box of the subtree over the visible instance prefix. */
    [[nodiscard]] bool subtreeBounds(Aabbd& box) const override;
};
```

**生命周期与语义**

- `InstanceScope` 是 `Group` 的子类，自身没有局部变换；放在 `MatrixTransform` 下即可整体放置。
  实例矩阵在**被绘制节点的局部空间**里乘：直接 `Geometry` 子节点的最终 `world = 作用域世界 × 几何局部 × 实例矩阵`；
  中间有 `MatrixTransform` 时，子变换先于实例矩阵（和顶点着色器 `pc.modelView * instance` 一致）。
- `setInstanceTransforms` 是**形状变更**：记录切片、`announceContentChange()`、`invalidateBounds()`。
  缓冲里的字节改动只调 `Buffer::bumpRevision()`（B6 契约）；切片本身不变时不需要重设。
- `setVisibleInstanceCount` 也是收集事实：命令快照 `first_instance`/`instance_count`，所以它同样
  `announceContentChange()`；可见数只改 draw 参数，不重传实例流。
- `visibleInstanceCount()` 夹紧到 `instanceCount()`；0 = 不画，且不产生命令。
- `instanceMatrix(i)` 的 `i` 是**相对于切片**的 0..instanceCount()-1；拾取结果的 `instanceIndex` 也用这个相对索引。
- 没有变换的空作用域完全透明：收集、包围盒、拾取都把它当普通 `Group`。
- `firstInstance()` = `transforms_.begin() / 16`；后端把它作为 `vkCmdDraw*` 的 `firstInstance`。
- 直接子节点是 `InstanceScope` 时 `addChild` 拒绝并计数；更深层的嵌套不组合，收集/拾取按**最近的作用域**解释。

**子节点约束（第一片）**

作用域下可以是普通 `Group` / `StateNode` / `MatrixTransform` / `Geometry`。实例矩阵在**画到 GPU 的节点局部空间**里乘，
所以作用域与几何之间的变换是合法的，次序固定为“子变换先、实例矩阵后”：

```
world = scope_world × ... × child_local × instance_matrix
```

不允许的是**直接嵌套的作用域**：`InstanceScope::addChild` 拒绝并计数（两个实例集需要组合的叉积流，本层不发明）。
经 `Group` 间接嵌套不会被门禁拦下；收集与拾取按最近的作用域解释，外层实例不参与内层——这是本片的已知边界，
不是支持的特性。

作用域本身可以在 `MatrixTransform` 下；它的世界会进入 `pc.modelView`。

**继承与共享**

- 层、draw order、opacity、材质、程序：作用域是透明的，全部由子树/祖先按现有规则折叠；
  一个子几何的所有实例共享它的材质、层、opacity、draw order。
- 多个子几何共享**同一份实例流**（一次上传，多条 draw）；这是作用域相对“每个几何一套实例”的主要好处。
- 透明实例之间不排序：一个子几何的所有实例在一条 draw 里按实例序混合；逐实例排序需要拆成多个作用域。
- 镜像实例（负行列式）会翻转缠绕方向，和镜像节点变换是同一个已知问题；第一片不检测。

### 2.2 收集期折叠

- `Scene::collectRenderCommands` 的折叠上下文增加 `const InstanceScope* instances{nullptr}`。
- 遇到 `InstanceScope` 且有变换：**先于 `Group` 判断**（它是 `Group` 子类，顺序错了会走普通组的路径），
  记下当前作用域；没有变换的作用域不改变上下文（外层作用域继续生效）。
- 遇到 `Geometry`：每条 `RenderCommand` 快照 `instance_transforms`（`BufferSlice<float>`）、
  `first_instance`、`instance_count`；`model_matrix` 仍是几何的世界矩阵。命令是值，后端不再读场景对象。
- `RenderCommand` / `CollectedCommand` / `CompiledCommand` 增加：

```cpp
BufferSlice<float> instance_transforms;  ///< 16 scalars per instance, column-major.
std::uint32_t      first_instance{0};    ///< First instance this command reads.
std::uint32_t      instance_count{0};    ///< Instances this command draws (0 = not instanced).
```

- `FrameRecorder` 快照时把 `instance_transforms.values` 记成 `instance_buffer` + 它的 `revision`；
  `FrameCompiler` 原样带着。这样命令的寿命不依赖作用域，也不依赖调用方在收集后是否改可见数。
- 作用域下的**剔除以作用域为单位**：`BoundsCache` 在作用域节点上得到包含可见实例的盒；
  一旦进入作用域，不再用子节点“未实例化”的盒做视锥剔除（否则放在远处的作用域会被原点的子盒整组剔掉）。
- 可见数变化会 `announceContentChange()`，下一帧重新收集；它只改变 `instance_count`，不改变切片/上传。
- 排序不变：`draw_order` 是外层键，作用域不改变排序语义。

### 2.3 后端：实例 draw 就是普通 draw + 两个参数 + 一个绑定

`ContentDraw::Draw`：

```cpp
std::uint32_t instances{1};       // 已有
std::uint32_t first_instance{0};  // 新增
```

实例绑定不单独做字段：`ContentPass` 把它追加进 `vertex_binds`（binding 4）；`ContentDraw::record` 照常遍历。

`ContentPass::record` 的相关流程：

```cpp
// ContentPass reads the command's snapshot, never the scope.
const bool instanced = command.instance_buffer != nullptr;
if (!instanced && command.instance_count > 1U) {
    reportRefused("the command's instances", "an instance count without an instance stream");
    return false;
}
if (instanced) {
    if (command.instance_count == 0U) {
        return true;                    // nothing to draw, not a refusal
    }
    if (command.instance_buffer->size() % kInstanceMatrixComponents != 0U) {
        reportRefused("the command's instances", "its instance buffer does not hold whole matrices");
        return false;
    }
    const std::size_t capacity = command.instance_buffer->size() / kInstanceMatrixComponents;
    if (command.first_instance > capacity ||
        command.instance_count > capacity - command.first_instance) {
        reportRefused("the command's instances", "its visible range is outside the instance buffer");
        return false;
    }
}

// The half is looked up with the variant the command states; an instance stream selects it.
variant.instanced = instanced;

// ... after the geometry's channel binds:
if (instanced) {
    core::StreamKey key;
    key.kind       = core::StreamKind::Vertex;
    key.location   = vn::graphics::attributeLocation(vn::graphics::VertexAttribute::InstanceModel);
    key.components = vn::graphics::kInstanceMatrixComponents;  // 16
    key.buffer     = command.instance_buffer;
    key.revision   = command.instance_revision;
    key.offset     = 0U;
    key.count      = 0U;                                      // the whole buffer
    const auto acquired = scope_.uploads->acquireVertex(key, command.instance_buffer->view());
    if (acquired.bind == nullptr) {
        reportRefused("the command's instances", "its instance stream could not be uploaded");
        return false;
    }
    binds[bound++] = acquired.bind;                           // binding 4
}

record.instances      = instanced ? command.instance_count : 1U;
record.first_instance = instanced ? command.first_instance : 0U;
```

`ContentDraw::record`：

```cpp
if (draw.index != nullptr) {
    commands->addChild(::vsg::DrawIndexed::create(draw.index_count, draw.instances,
                                                  draw.first_index, draw.vertex_offset,
                                                  draw.first_instance));
} else {
    commands->addChild(::vsg::Draw::create(draw.vertex_count, draw.instances, 0U,
                                           draw.first_instance));
}
```

- `firstInstance` 同时偏移逐实例顶点取和 `gl_InstanceIndex`；实例缓冲整段上传，所以 `first`/`visible` 只是 draw 参数。
- `StreamUploads::bindingOfCanonical(12..15) = 4`；`isCanonicalAttributeLocation` 认 12–15；
  `acquireVertex` 的“派生/非规范/空”拒绝不变。
- 实例流不经过 `GeometryFacts`；`ContentStore` 不新增实例事实；`GeometryFacts` 一行不改。
- A3 释放：实例缓冲由作用域的 `BufferSlice` 持有；后端只是 `StreamUploads` 的借用/共享条目，
  `releaseUnseen` 的 grace 扫尾会把它送走；不需要新的 `releaseContent` 类型。

### 2.4 管线与着色器：`VINE_INSTANCED` 是唯一开关

`ProgramVariant`：

- 新增 `bool instanced{false}`；`bits()` 用 `8U`；`defines()` 追加 `VINE_INSTANCED`；
  `describe()` / `operator==` 同步；`variantOf()` 默认 false（实例化是绘制事实，不是材质/几何事实）。
- `ContentPass` 在命令带实例流时置 `variant.instanced = true`，再算 `record.key.variant = variant.bits()`。

`ContentPipeline::create`：

- `instanced = shaders.defines` 里含 `VINE_INSTANCED`；
- 若 `instanced`，先检查 `shaders.vertex` 的 `#pragma import_defines (...)` **确实列了**这个名字。
  vsg 只把源码列出的名字交给编译器；只写 `#ifdef` 不列会静默丢弃。漏列 ⇒ 返回 `nullptr`，half 被拒、draw 被跳过，
  而不是把 N 个实例画在同一个位置；
- 追加：

```cpp
bindings.push_back(VertexBinding{ 4U, 64U, /*per_instance=*/true });
for (std::uint32_t column = 0; column < kInstanceMatrixColumns; ++column) {
    attributes.push_back(VertexAttribute{
        vn::graphics::instanceMatrixLocation(column), 4U, VK_FORMAT_R32G32B32A32_SFLOAT,
        column * 16U });
}
```

- 不新增 `PipelineKey`/`ContentHalves` 字段：`PipelineKey::variant` 已经携带这个 bit，
  `ContentHalves` 的 key 也带 `variant`，实例化/非实例化自然分到不同 half 和不同管线。
  **不变式**：`variant.instanced` ⇔ 管线声明 binding 4；由 `ContentPipeline::create` 一处保证。

`ShaderAbi.hpp`：

- `VertexAttribute::InstanceModel`，`attributeLocation(...) = 12`，占 12–15；
- `kInstanceMatrixColumns = 4`，`kInstanceMatrixComponents = 16`；
- `isCanonicalAttributeLocation(12..15) = true`；`instanceMatrixLocation(column) = 12 + column`；
- 自定义通道的保留说明更新：≥3，且不得是 8 或 12–15。

着色器（`builtin_forward.vert` / `builtin_gbuffer.vert`）：

```glsl
#pragma import_defines (VINE_VERTEX_COLOR, VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE, VINE_INSTANCED)
#ifdef VINE_INSTANCED
layout(location = 12) in mat4 vine_InstanceModel;
#endif
...
mat4 model = pc.modelView;
#ifdef VINE_INSTANCED
model *= vine_InstanceModel;
#endif
vec4 view_pos = model * vec4(vine_Vertex, 1.0);
vine_view_normal = mat3(model) * vine_Normal;
```

- `pc.modelView` = view × 作用域世界；`vine_InstanceModel` = 实例局部；相乘即最终。
- `builtin_skybox` / `builtin_fullscreen` 不 import 该名字；vsg 对源里没列出的 define 是静默丢弃，行为不变。
- 用户程序必须自己 import `VINE_INSTANCED`；否则 `ContentPipeline::create` 拒绝（不会静默叠在原点）。
  这是明确的约定，不是后端能“猜”的能力。

### 2.5 包围盒与剔除

- `Node::subtreeBounds` 是虚函数：`Geometry` 对**精确** `Geometry` 返回 `localBounds()`（子类仍走自己的
  `boundingBox()`，保持旧契约），`Group` 保留缓存 union，`InstanceScope` 覆写。
- `InstanceScope` 的局部盒：

  1. 对每个子节点求它在作用域局部帧里的盒（`child->subtreeBounds`，再乘 `child->localTransformMatrix()`）；
  2. 对 `i = 0..visibleInstanceCount()-1`，`transformBox(child_box, child_local * *instanceMatrix(i))` 并集；
  3. 没有可见实例 ⇒ 空盒（`isValid() == false`），不是“整段容量”。

- `boundingBox()` = `subtreeBounds(local)` 后乘 `worldMatrix()`；`Scene` 的 culling/阴影拟合照常。
- 包围盒覆盖**可见前缀**（`visibleInstanceCount`），和实际画出的实例一致；可见数变化会
  `invalidateBounds()`，所以不会用旧盒剔除。
- 作用域下的自定义叶子（不以局部盒回答 `subtreeBounds`）使 `InstanceScope::subtreeBounds` 返回 `false`；
  `Scene` 回退到世界空间 `boundingBox()`（保守，可能不是实例精确盒）。
- 逐实例剔除不做：作用域整体一个盒。HISM 式簇树是后续能力。
- **剔除单位是作用域**：进入作用域后，子节点不再用“未实例化”的盒做视锥剔除（见 §2.2）。

### 2.6 拾取与选择

`RayIntersectionResult` 增加：

```cpp
/// The node a selection binds to: the Geometry itself, or the InstanceScope around it.
intrusive_ptr<Node> node;

/// The instance in the hit scope, relative to the scope's segment; max() when not instanced.
std::uint32_t instanceIndex{std::numeric_limits<std::uint32_t>::max()};
```

- 遍历时携带当前 `InstanceScope`；**先于 `Group` 判断 `InstanceScope`**（它是 `Group` 子类），
  且只有 `hasInstanceTransforms()` 的作用域才接管；无变换的作用域对拾取透明。
- 遇到 `Geometry` 且作用域非空：
  - 对 `i = 0..visible-1`，`placed = geometry_world * *scope->instanceMatrix(i)`；
  - `forEachTriangleHit(geometry, ray, placed, …)`，每条命中打 `geometry`、`primitiveIndex`、
    `instanceIndex = i`、`node = scope`；`point`/`normal` 是世界坐标。
- 无作用域：`node = geometry`、`instanceIndex = max()`。
- `intersectScene` 取全场景最近命中（跨实例）；`intersectSceneAll(Mode::Nearest)` 仍是每个几何一条
  （返回它最近的实例）；`AllHits` 是实例×三角形级别。
- 只拾取**可见**实例（`visibleInstanceCount`）；隐藏的作用域/子节点按现有可见性规则跳过。
- 选择单位：**命中节点 `node`**。命中作用域时，`ModelRenderView` 高亮其子树里的全部 `Geometry`，
  清除时逐个还原原来的材质；只关心某一实例时，用 `instanceIndex` 做应用层标记。
  **逐实例换材质高亮做不到**（材质是共享的），第一片明确不收这个能力。

### 2.7 键、缓存与生命周期

- 管线键：`ProgramVariant.instanced` 已在 `PipelineKey.variant` / `ContentHalves` 的 key 里；
  不需要新字段。`ContentPipeline::create` 由 `VINE_INSTANCED` 推导 binding 4。
- 流：`StreamKey` = 整段实例缓冲（location 12、components 16、buffer/revision、offset 0、count 0）；
  两个作用域共享同一缓冲时共享一次上传；可见数变化不改 key。
- 作用域持有 `BufferSlice`（强引用缓冲）；收集出的命令**快照切片**（`intrusive_ptr` + offset/count），
  所以命令不依赖作用域的寿命；`RayIntersectionResult` 持强引用 `node`/`geometry`，调用方可以安全保存结果。
- `Geometry` / `Primitive` / `GeometryFacts` / `ContentFacts` 的**数据形状**不变；`ContentStore` 只在
  `tablesFor` 里按命令的实例流把 variant 置位（与 `ContentHalves`/`ContentSets` 同一规则）。
- 实例化的命令仍走正常的 pass/通道；透明/opaque 的首批排序不变；一个子几何的 N 个实例是一条 draw。

## 3. 实现时要先处理的坑

### 3.1 `subtreeBounds` 虚化与精确叶子的快路径

新作用域要回答自己的局部盒，所以 `Node::subtreeBounds` 是虚函数：`Group` 保留缓存 union，
`InstanceScope` 覆写，`Geometry` 只对**精确** `Geometry` 返回 `localBounds()`。
保留 `typeid(*this) == typeid(Geometry)` 是有意的：`Geometry` 子类可能覆写 `boundingBox()`（旧契约），
强制它按数据盒回答会改变原有语义；子类继续走世界空间 `boundingBox()` 的 fallback。

### 3.2 作用域与几何之间的变换

**允许**，但次序必须固定：子变换先、实例矩阵后（`world = ... × child_local × instance`）。
着色器拿到的是 `pc.modelView`（已经包含作用域世界与子变换），再乘 `vine_InstanceModel`；
拾取/包围盒用同一个次序，所以三者不可能不一致。
vsg 禁止 child 变换，是因为它的 `InstanceNode` 在 record 期替换 child 的变换；我们的实例矩阵是命令值，不受这个限制。

### 3.3 整段缓冲 vs 切片键

如果实例 `StreamKey` 用切片（offset/count 随 `visible` 变），每次改可见数都会换 key、重传。
正确做法：key 取**整段**（offset 0、count 0），`firstInstance`/`instances` 只在 draw 参数里变；
可见数变化零上传。判据要数上传次数，不只看画面。

### 3.4 可见数：命令快照，而不是后端读 live

`RenderCommand`/`CollectedCommand`/`CompiledCommand` 快照 `instance_transforms` + `first_instance` + `instance_count`；
后端只读命令，不读作用域。这样命令的寿命不依赖场景对象，帧 memo 仍然可用。
代价：`setVisibleInstanceCount` 必须 `announceContentChange()` 让下一帧重新收集；它只改 draw 参数，不重传实例流。
如果将来要做“每帧改可见前缀且不想重新收集”，那需要另一条增量路径，而不是把指针藏进命令。

### 3.5 程序 opt-in 的 pragma 检查

vsg 只交付源码 `#pragma import_defines` 里列出的名字。只写 `#ifdef VINE_INSTANCED` 不列名字，define 会被静默丢弃，
着色器不乘实例矩阵，N 个实例全叠在原点。`ContentPipeline::create` 必须检查 pragma 行，不能只在 `shaders.defines`
里看到名字就加 binding 4。用“解析 pragma 行”的 helper，不要用裸 `find("VINE_INSTANCED")`（会命中注释/`#ifdef`）。

### 3.6 嵌套作用域

直接嵌套由 `InstanceScope::addChild` 拒绝并计数：两个实例集需要组合成叉积流，本层不发明。
经 `Group`/`StateNode` 间接嵌套不会被门口拦住；收集与拾取按**最近的作用域**解释，外层实例不参与内层。
这是明确的边界：需要组合实例时，在应用层预先算出合成矩阵，别再套一层作用域。

### 3.7 `firstInstance` 的语义

`firstInstance` 同时偏移逐实例顶点取和 `gl_InstanceIndex`。作用域的 `firstInstance` = 切片起点/16；
拾取返回的 `instanceIndex` 是**相对切片**的，不是绝对 `gl_InstanceIndex`。文档和用例都要钉住这个相对性。
`first + visible <= capacity` 必须校验；`firstInstance` 落在段外时不能画到别的段。

### 3.8 透明与实例排序

一个子几何的 N 个实例是一条 draw，无法逐实例排序；透明实例会按实例序混合。第一片只承诺不透明（或加色/顺序无关）内容。
需要逐实例排序就拆成多个作用域/多个几何。

### 3.9 镜像实例

负行列式会翻转缠绕方向，和镜像节点变换同一已知问题；第一片不检测。需要镜像副本时用翻转的几何，而不是负缩放实例。

### 3.10 包围盒覆盖可见前缀

`visibleInstanceCount` 是收集事实，盒按它展开；可见数变化会 `invalidateBounds()`，不会用旧盒剔除。
代价是可见数频繁变化时盒要重算；这是“剔除正确性优先”的选择。逐实例剔除仍不做。

### 3.11 浮点与次序

`pc.modelView` 是 `view × 作用域世界`，实例矩阵是局部的；直接相乘，不需要 C 方案里的“参考矩阵求逆”。
实例矩阵是 float，作用域世界是 double → 转换发生在 push 打包处（现有路径）；误差和节点变换同一量级。

### 3.12 多个 part 共享实例流

一个作用域下多个 `Geometry` 各画 N 个实例，实例流只上传一次。材质/程序/拓扑不同不影响共享；
每条命令独立编译各自的 half。不要把“实例作用域”误当成“合批”：不同几何不会合成一条 draw。

### 3.13 单实例作用域

`visibleInstanceCount() == 1` 仍走实例化路径（`VINE_INSTANCED` 管线）。不要为 1 特殊化成普通 draw，
否则同一作用域在可见数变化时会切换程序文本/管线，缓存抖动。

### 3.14 自定义叶子

作用域下的自定义 `Node`（不以局部盒回答 `subtreeBounds`）使 `InstanceScope::subtreeBounds` 返回 `false`；
`Scene` 回退到世界空间 `boundingBox()`（保守）。作用域只承诺引擎自己的叶子（`Geometry`/`Group`/`StateNode`）。

## 4. 明确不做（第一片）

| 不做 | 原因 / 将来的触发 |
| --- | --- |
| 逐实例材质 / 颜色 / opacity / 层 / draw order | 都是“命令/子树”的事实；逐实例化要第二个实例流或实例块。真出现“同一网格不同颜色”的批量需求时再评 |
| 逐实例透明排序 | 一条 draw 内无法重排；需要排序就拆作用域/几何 |
| 逐实例剔除 / GPU culling | 作用域一个并集盒；触发：实例数大到“画不可见实例”成为主要成本 |
| 自动合批（N 个普通节点合成一次 draw） | C 方案；隐式、N 条命令仍在、只覆盖完全同键的绘制。未来可作为独立优化，不进本设计 |
| 直接嵌套作用域 | 两个实例集需要组合叉积；`addChild` 拒绝并计数。间接嵌套按最近作用域解释，见 §3.6 |
| 作用域与几何之间的变换 | **已支持**；次序固定为子变换先、实例矩阵后（§3.2） |
| 镜像实例检测 | 与镜像节点同一已知问题；且没有逐实例翻转正面的状态 |
| 粒子/GPU 间接路径 | 单节点、十万级、逐实例数据、GPU 生成，属于独立的 emitter + storage buffer + indirect 设计 |
| LOD / 空间划分 | object-model 缺口 3，独立触发 |
| 分层目标（L2b） | 实例化不需要它；只是共享“一次提交许多份”的评估触发 |

## 5. 落地切片与判据

### S0：`subtreeBounds` 虚化（已落地）

改动：`Node.hpp` / `Node.cpp` / `Group` / `Geometry` / `Scene`；`Geometry` 的**精确**叶子快路径保留
（子类仍走自己的 `boundingBox()`），`Group` 保留缓存 union，`InstanceScope` 覆写。

判据：现有包围盒/剔除用例全绿；`SceneTest.CollectCommandsAsksEachLeafBoundOnce` 仍数到每个叶子一次。

### S1：`InstanceScope` SDK + 收集折叠（已落地）

改动：`InstanceScope.hpp/.cpp`；`RenderCommand`/`CollectedCommand`/`CompiledCommand` 增加实例切片 +
`first_instance`/`instance_count`；`Scene::collectRenderCommands` 折叠。

判据（已钉住）：

- `InstanceScopeTest.FoldsAnInstanceStreamOntoEveryDrawBelowIt`：两条实例 ⇒ 一条命令、
  `instance_count == 2`、`instance_transforms.size() == 32`、`instanceMatrix(1)` 的平移列正确；
  作用域盒覆盖 x=0/2 两个放置（0..3）。
- `InstanceScopeTest.TheVisiblePrefixIsTheDrawRangeAndCanChangeWithoutANewSlice`：切片从第 1 个实例开始；
  `visible=0` 不产生命令；清掉可见数后命令 `first_instance==1`、`instance_count==1`。
- `InstanceScopeTest.WithoutTransformsItIsTransparentAndNestedOrMalformedStreamsAreRefused`：无变换作用域不产生
  实例字段；非整矩阵切片被计数拒绝；直接嵌套计数拒绝。

### S2：后端实例 draw + 着色器（已落地）

改动：`ShaderAbi`（`InstanceModel`/12–15/16 分量/`instanceMatrixLocation`）；
`ProgramVariant.instanced`（bit 8）；`ContentPipeline::create`（从 `VINE_INSTANCED` 推导 binding 4 + 属性，
pragma 检查）；`StreamUploads`（12–15 → binding 4）；`ContentDraw::Draw.first_instance`；
`ContentPass`（命令切片→绑定/计数）；`ContentDraw::record`（`firstInstance`）；
`builtin_forward.vert`/`builtin_gbuffer.vert`；`vine_shader_check.sh` 的 `VINE_INSTANCED` 组合。

判据（已钉住）：

- `ShaderAbiTest`：12–15 是规范位置，11/16 不是；`instanceMatrixLocation(0..3) == 12..15`。
- `ProgramVariantTest`：`instanced` 是第 4 个开关（bit 8），16 个组合的 key 互异；`defines()` 含
  `VINE_INSTANCED`。
- `ContentPipelineTest.AnInstanceVariantDeclaresThePerInstanceStreamTheShaderImports`：binding 4、stride 64、
  `VK_VERTEX_INPUT_RATE_INSTANCE`、属性 12–15 偏移 0/16/32/48。
- `ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`（lavapipe）：两个放置由**一条** draw 画出，
  探针 (19,32)/(45,32) 都是三角形；上传 = 3（位置、索引、整段实例流）。

### S3：包围盒 + 拾取 + 选择（已落地）

改动：`InstanceScope::subtreeBounds`；`RayIntersectionResult`（`node`/`instanceIndex`）；
`RayIntersection` 的逐可见实例循环；`ModelRenderView` 绑定到 `hit.node`，并高亮其子树全部 `Geometry`。

判据（已钉住）：

- `InstanceScopeTest.PickingReportsTheScopeAndTheInstanceThatWasHit`：射线打第二个放置 ⇒ `node == scope`、
  `geometry == leaf`、`instanceIndex == 1`、世界点 x≈2.25；
- `InstanceScopeTest.WithoutTransforms...`：无变换作用域的命中 `node == geometry`；
- `RayIntersectionTest.SceneQuery`：非实例几何 `node == geometry`、`instanceIndex == max()`；
- `ModelViewerTest.AClickSelectsTheGeometryUnderTheCursorAndHighlightsIt` 与
  `AClickOnAnotherPartMovesTheHighlightAndBlankClickRestoresIt`：单几何/多部件选择与还原仍绿。

### S4：文档与账本（已落地）

- `.ai/README.md` 索引本文；`graphics-instancing-channel-2026-10-22.md` 标为历史；
- `graphics-followups-2026-10-19.md` §3.3 行指向本文（状态：已落地）；
- object-model 复核 §3 缺口 2 的“模型上加一个字段”改为“InstanceScope 作用域 + 命令级实例流”。

## 6. 变异清单（落地后必须逐条变红）

| 变异 | 预期症状 |
| --- | --- |
| 收集器不写 `instance_transforms`/计数 | 作用域内容画 1 个实例（普通的那个） |
| `ContentPass` 不追加实例绑定 | binding 4 未定义，验证层报错或像素错 |
| `record.instances` 写成 1 | 只画 1 个实例 |
| 去掉 `draw.first_instance` | 全部从第 0 个实例开始 |
| `variant.instanced` 不置位 | 非实例化 shader，全部叠在原点 |
| `ContentPipeline` 不因 define 加 binding 4 | 管线少一个顶点绑定/属性，验证层报错 |
| pragma 检查去掉 | 不 import 的用户程序静默叠在原点（测试要能拒绝） |
| `StreamKey` 用切片而非整段 | 改可见数产生新上传（上传计数红） |
| 作用域盒不做实例并集 | 阴影/剔除用例红（没有该用例就说明没钉住） |
| 进入作用域后仍按子节点未实例化盒剔除 | 放在远处的实例整组消失 |
| 拾取不循环实例 | 射线打第二个实例 miss/命中错误 |
| 直接嵌套作用域不拒绝 | 内层实例集覆盖外层，索引/盒语义不确定 |

## 7. 风险与待定

- **作用域下的变换已支持**：次序固定为子变换先、实例矩阵后；不要再引入第二种解释。
- **命令快照 vs 作用域寿命**：命令快照切片/计数，寿命不依赖场景；如果将来要做“每帧改可见前缀且不重新收集”，
  需要独立的增量路径。
- **选择单位是命中 `node`**：作用域命中时高亮子树全部 `Geometry`；逐实例高亮仍属应用层。这是与 C（自动合批）
  最大的取舍：C 保持每节点选择，D 让选择单位变清楚。
- **粒子/大 N**：作用域能画大 N，但没有逐实例数据/GPU 生成；粒子是独立的 emitter + storage buffer + indirect 路径。
- **vsg 上游**：`InstanceNode`/`InstanceDraw` 的语义我们只借“作用域”形状；不依赖它的类，避免 TRS/child 限制/record 指针。
- **A3 释放**：实例缓冲不是 `releaseContent` 的受管类型；作用域/缓冲的生命周期由场景持有，`StreamUploads` 的 grace 扫尾负责 GPU 侧。

## 8. 与既有文档/账本的关系

- `.ai/design/graphics-object-model-vs-3d-frameworks.md` §3 缺口 2：实例化的落地记录指向本文；
  原“模型上加一个字段”的说法改为“InstanceScope 作用域 + 命令级实例流”。
- `.ai/design/graphics-followups-2026-10-19.md` §3.3：实例化行指向本文（状态：已落地）。
- `.ai/design/graphics-instancing-channel-2026-10-22.md`：早期通道方案，**已作废**，只作历史记录。
- 上游 vsg：`build/_deps/vsg-src/include/vsg/nodes/InstanceNode.h`、`InstanceDraw.h`、`InstanceDrawIndexed.h`、
  `src/vsg/nodes/InstanceDraw.cpp`、`src/vsg/app/RecordTraversal.cpp`、`src/vsg/utils/ComputeBounds.cpp`；
  本设计只借作用域形状，不复用这些类。
- `.ai/design/graphics-layers.md` §6 的“实例化被触发”只是 L2b 的复核信号；实例化本身不依赖分层目标。

## 9. 落地记录

**已落地（2026-10-22）**。

- SDK：`InstanceScope.hpp/.cpp`、`ShaderAbi.hpp`（`InstanceModel`/12–15）、`RenderCommand.hpp`、
  `Node`/`Group`/`Geometry`/`Scene` 的 bounds 钩子、`RayIntersection` 的 `node`/`instanceIndex`。
- 后端：`ProgramVariant.instanced`（bit 8）、`ContentPipeline` 的 binding 4 + 属性 12–15 + pragma 检查、
  `StreamUploads` 的 12–15 → binding 4、`ContentDraw::Draw.first_instance`、`ContentPass` 的实例流/计数、
  `ContentHalves`/`ContentSets`/`ContentStore` 的 variant 一致化。
- 着色器：`builtin_forward.vert` / `builtin_gbuffer.vert` import `VINE_INSTANCED` 并乘实例矩阵；
  `scripts/vine_shader_check.sh` 覆盖该 define。
- 选择：`ModelRenderView` 绑到 `hit.node`，高亮子树全部 `Geometry`，清除时逐个还原材质。
- 设备证据：`ContentPassTest.AnInstanceStreamDrawsEveryPlacementAndUploadsItOnce`（lavapipe）画两个放置，
  一条 draw、3 次上传；`test_graphics` 446/446、`test_vsg` 503/503、`test_gui` 260/260（1 skip：无显示）。
- 门禁：`scripts/vsg_rewrite_gate.sh build` 全绿（503 cases、0 VUID、0 sync hazard、各静态检查 0 finding）；
  `scripts/vine_shader_check.sh` 全绿（含 `VINE_INSTANCED` 组合与嵌入副本一致）。
- 偏差：作用域与几何之间的 `MatrixTransform` **允许**（原设计第一片禁止）；包围盒按**可见前缀**而非容量；
  `subtreeBounds` 对精确 `Geometry` 保留 `typeid` 快路径，子类仍走 `boundingBox()` fallback。
- 尚未变异验证：§6 的变异清单是落地后要逐条跑红的清单，本次只跑了正向用例，未逐个注入变异。
