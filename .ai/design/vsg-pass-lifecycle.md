# VSG 后端 pass 生命周期与槽身份（2026-09-11 落地）

> 关联：`.ai/design/vsg-user-mutation-strategy.md`（可变性策略表）、`vsg-pipeline-sharing.md`（管线/数据·状态共享）、
> `.ai/design/graphics-render-pipeline.md`（engine 纯调度模型）、
> `src/plugins/gfx_backend_vsg/gfx_backend_vsg.md`、`vine-to-vsg-data-flow.md`（缺陷表 D8/D14/D16/D22）。
> 代码：`RenderBackend.hpp`（beginPass/endPass/releasePass）、`RenderEngine.cpp`（调用序）、
> `VsgRenderer.{hpp,cpp}`（SlotKey / reap / erasePassSlots / reassignPass / offscreenBuildCount）、
> `SceneBridge.{hpp,cpp}`（setContentDepthMode / invalidateState）。

## 1. 问题（本次修复的根因）

引擎是纯调度器，后端长期保留每 pass 的 GPU 状态（View + 管线 + 描述符 + 采样槽）。
但两者之间**没有显式的 pass 契约**：

- 保留状态的**键是 `(camera, pass order)`** —— 与 pass 对象无关；
- 引擎→后端的“本 pass 状态”靠 6 个**一次性 pending 变量**（target / viewport / lights /
  depth mode / pass order / presenting），由“下一个 draw 调用”消费；
- 引擎只在 `removePass/clearPasses` 通知后端，`setEnabled` / `setCamera` / `setRenderTarget`
  / `setDepthMode` 等运行期变化**不通知**。

由此产生的缺陷（均已修复并有回归）：禁用 pass 仍绘制（幽灵）、改 camera/RT 残留旧槽永久绘制、
槽的 depth_mode/presenting 首帧冻结、同 (camera, order) 两 pass 后者覆盖前者、
pass 只设状态不绘制时 pending 泄漏到下一个 pass。

另外两处交叉路径缺陷：`shareDepth` × `clearDepth=false` 使离屏目标**每帧重建**；
`releaseRenderTarget` 不清理借深度者 → 悬垂 `VkImage` 塞进 command graph 屏障。

## 2. 契约（新）

```
RenderBackend::beginPass(pass)      // 开 pass 作用域：宣告身份 + 标记本帧活跃 + 复位 pending
    setPassOrder / setRenderTarget / setViewport / setLights / setDepthMode / clear   // 作用于当前 pass
    render(...) | drawScreenTexture(...) | drawScreenProgram(...)                      // 消费并执行
RenderBackend::endPass()            // 关作用域：未消费的 pending 一律丢弃
RenderBackend::releasePass(pass)    // 释放该 pass 的全部保留状态（等待设备空闲后）
```

- **pass 对象就是槽身份**。`SlotKey{pass, id0, id1}`：pass 非空时只用 pass 指针；
  pass 为空（直接驱动后端的调用方，如 `vsg_backend_selftest`）回退到历史键
  （content: camera+order；PiP: source+attachment；program: source），两种身份不混用。
- **每帧活跃集合**：`beginPass` 把 pass 记入 `active_passes`；`submitFrame()` 在提交前
  `reapInactivePassSlots()` —— 本帧未被宣告的 pass 的槽被拆掉（不再绘制）并提交一帧呈现该移除。
  直接驱动（从未 `beginPass`）永不回收。
- **属性变化即重建**：`renderContentSlot` 每帧比对 `depth_mode` / `order` / `presenting`，
  `drawScreenTexture/Program` 比对 `source_target` / `attachment` / 尺寸 / program；
  pass 换 target 时 `reassignPass(pass, 新 target)` 拆掉旧 target 下的槽。
- **深度策略归属**：`ResolvedRenderState` 由场景图 StateNode 折叠而来；`RenderPass::depthMode`
  填充**未显式设置 depth** 的命令（`RenderCommand::depthExplicit == false`）。
  显式 StateNode depth 优先（更细粒度意图）。由此 `TestOnly`/`Disabled` 真正进入管线
  （此前烘焙在 shader set 里的 depth 状态被 `applyRenderStateObjects` 覆盖，等于失效）。
- **深度共享**：借深度目标不使用 depth-LOAD pass（策略来自被借方），故重建谓词不再期望它；
  被借方释放后，借方丢弃借用、以自有深度重建一次（`dead_depth_sources` 记忆）。
- **每帧必须提交**：`beginFrame()` 已 acquire 一张交换链图像，只有 present 才归还；
  因此 `submitFrame()` 不再因“本帧没画东西”而跳过（否则每帧泄漏一张图像，
  触发 `VUID-vkAcquireNextImageKHR-surface-07783`）。

## 3. 生命周期与同步纪律（重要）

任何**销毁**被提交命令缓冲可能引用的对象之前，必须先 `viewer->deviceWaitIdle()`：

- `releasePass` / `erasePassSlots` / `reapInactivePassSlots` / `releaseRenderTarget` /
  `rebuildOffscreenTarget` / **depth-policy 变化触发的 `invalidateState`** 都先等待；
- 固定顺序：**先等设备 → 再摘图/删对象**。反过来做会触发
  `VUID-vkDestroyPipeline-pipeline-00765` / `VUID-vkDestroySampler-sampler-01082`
  （validation 层实测抓到，见 §4 验证）。
- 稳态帧不等待：`reapInactivePassSlots` 先扫描再决定是否等待。

## 4. 验证

- device-free：`test_graphics` 135（含 pass 作用域调用序、releasePass、MockBackend 契约）、
  `test_vsg` 53（含 `ContentDepthModeAppliesAndRebuildsState`：策略进入管线且只重建 state）。
- on-device（lavapipe + Khronos validation，`scripts/gfx_lavapipe_check.sh` → RESULT: PASS）：
  - `vsg_backend_selftest` 新增两个阶段：
    1. **pass 协议**：同 camera 同 order 的两个 pass 各自成槽；运行期 depth 策略变化生效；
       未宣告的 pass 被回收且**不重建**离屏目标；**全部 pass 停用**时其余视图也被回收；
       **重新启用只重挂视图**（不重建、不重传）；`releasePass` 后继续渲染正常。
    2. **深度共享**：借用深度 + `clearDepth=false` 在 N 帧内**只有 2 次构建**（无重建风暴）；
       释放被借方后借方**恰好重建 1 次**（以自有深度）。
  - 断言依据两个诊断计数：`VsgRenderer::offscreenBuildCount()`（离屏构建次数，稳态不增长）与
    `VsgRenderer::detachedSlotCount()`（当前被回收/摘下的槽视图数）。
- 真机（非 lavapipe）仍需复验：视觉正确性与驱动差异（见 §5）。

## 5. 诊断 API（供 harness / 排障）

| API | 语义 | 稳态期望 |
|---|---|---|
| `offscreenBuildCount()` | 成功构建/重建离屏目标的次数 | 不随帧数增长（增长=重建循环） |
| `detachedSlotCount()` | 当前被回收（视图已摘）的槽数 | 全部 pass 在画时为 0；禁用后升高，重启用后回落 |
| `SceneBridge::pipelineVariantCount()` / `variantReuseCount()` / `programStageCompileCount()` | 变体/复用/glslang 编译计数 | 管线数跟状态变体数走，不跟几何数走 |

## 6. 行业规范对照（三维引擎）与本轮未做项

已对齐的通行做法：

- **pass 作用域 + 显式生命周期**（类似 RenderGraph / FrameGraph 的 pass 声明，OSG 的
  RenderStage + Camera，vsg 的 View + RenderGraph）；
- **保留渲染图 + 数据/状态解耦**（几何数据与管线状态分离，改材质不重传网格）；
- **Pipeline State Object 化的状态键**（`ResolvedRenderState` 折成变体键，同状态共享管线）；
- **MRT / G-buffer / 延迟光照 + 全屏 pass ABI**（每附件一个采样槽，push-constant 块）；
- **不透明前→后、透明后→前排序**（`Scene::collectRenderCommands`）；
- **深度共享 / 深度预 pass**（`shareDepth` + depth-LOAD pass）；
- 命名：`owner/scope/index` 槽身份、`retire`/`detach`/`retarget` 动词、`begin/end` 作用域成对；
- C++：`const` 正确性（相机链全 const，去掉 `const_cast`）、`std::optional` 代替 flag+哨兵、
  `std::array` + `static_assert` 钉住 shader ABI、`[[nodiscard]]`/`noexcept`、
  小 concept 头（`DepthMode.hpp`，include-what-you-use）、请求结构体代替 12 参数长表；
- **顶点属性按 `components` 解算**：`AttributeBuffer` 的 components 就是 stride，新增
  `stride()`/`vertexCount()`/`xyz(i)`，`Geometry::localBounds/positionCount/normalCount` 与
  `RayIntersection` 的网格解析全部改用它（此前假设每顶点 3 个 float，vec4 位置通道会算错 AABB
  → 错误剔除 / 错误 `fitToScreen`，且拾取直接失效）。
- **删除 `Geometry::setShape(Shape)`**（公共 API 收窄）：它与 `geometryFromShape()` 职责重复；
  形参用 `intrusive_ptr` 暗示持有却从不保留；传给非 Mesh 形状（`Sphere` 等 `Primitive`、`BrepShape`）
  时会**先清空再什么都不填**（静默丢数据，无返回值无诊断）；且 `attributes_.clear()` 会连带清掉
  自定义 loc 通道。现在 `geometryFromShape()` 是唯一 `Shape → vertex data` 入口（非 Mesh 返回 null
  可判断），就地（重）填用 `setPositions/setNormals/setIndices`。调用点迁移后 `AxisGizmo` /
  `FpsOverlay` / `app_shell::addBox` 反而更短（不再需要中转 mesh 与随之失效的 `computeAabb()`）。

未做（需单独排期；**逐项设计登记见 §9**）：

- **VkPipelineCache 持久化 / PSO 磁盘缓存**（§9.3，登记 D28）：vsg 的 `GraphicsPipeline::compile` 传
  `VK_NULL_HANDLE`，Vine 无法从外部注入；需等 vsg 支持或自行接管管线创建。
- **显式销毁路径的整帧停等**：teardown / resize / release target / depth 变更等仍用
  `vkDeviceWaitIdle`（已保证正确与 validation 干净），代价是整帧停等。业界做法是 N 帧在飞 +
  per-frame fence / 延迟删除队列；本轮的**退役环（§8.2）已把无等待的活路径纳入延迟释放**，
  余下的是把显式路径也改成“延迟释放 + 去掉 wait”。
- **命令列表缓存（跨 pass 复用）**（§9.2，登记 D27）：`Scene::collectRenderCommands` 每个 pass 每帧全树遍历，
  多 pass 场景仍是 O(passes x nodes)；业界会缓存一次遍历结果供多 pass 复用。
  （单次遍历内部的 O(depth^2)/O(n·depth) 与重复 bbox 已修掉，见 §6.1）
- **材质缓存的逐出与身份**（§9.1，登记 D13）：`VsgMaterialManager` 缓存只增不减，且按裸指针索引
  不自持 → 同地址新材质会复用旧 Phong 值 / descriptor（与 §8.1 同类）。
- **后端诊断走日志系统**：`gfx_backend_vsg` 仍用 `fprintf(stderr, ...)`（历史约定 + lavapipe 脚本
  依赖 stderr 文本），而 `app_shell` 等插件已改用 `vine/logging`。收敛需同时改 harness 断言。

### 6.1 本轮已做的遍历/几何性能修复（含实测）

围绕 `Scene::collectRenderCommands` 这条每 pass 每帧的热路径，修掉四处“算法层面”的重复计算
（非微优化，全部语义不变、有回归测试钉住）：

- **`Node::worldMatrix()` O(depth^2) → O(depth)**：原实现 `return p->worldMatrix() * local;`
  对每一层重算整条祖先链；改为自底向上单次左乘折叠。
- **遍历改为自顶向下累积世界矩阵**：`collectNodeCommands` 现在把父矩阵作为参数往下传
  （每节点 1 次矩阵乘），不再每节点调用一次 `worldMatrix()`（每节点 O(depth)）。
  为此 `Node::localTransformMatrix()` 从 protected 提为 public（自定义遍历同样需要它）。
- **包围盒每趟只算一次**（`BoundsCache`）：原实现每个容器都调 `Group::boundingBox()`，
  而它又要 union 子树 → 深度/宽度大时是 O(n·depth)。现在一趟遍历中：叶子调用一次虚函数
  `boundingBox()`，容器直接 union 子节点结果。
  前提是场景图是树（`Group::addChild` 会重新挂父节点），现有 `Geometry`/`Group` 之外的
  自定义 Node 只要按“叶子自己回答、容器 union 子节点”的约定实现即可（`Group.hpp` 已注明）。
- **排序键预计算**：原 `std::stable_sort` 比较器内每次做两次 `modelMatrix * Point3d` 再开方，
  O(n log n) 次矩阵乘；改为排序前算一次平方距离（与开方排序等价）并排指针，保持稳定排序语义。
- **`Group::childrenRef()`**：热路径（遍历 2 处 + 拾取 3 处）不再拷贝 `std::vector<NodePtr>`
  （省一次分配 + 每个子节点一次引用计数增减），`children()` 保留给需要副本的调用方。

实测（同一台机器、同一 debug 构建 `libviGraphicsd.so`、同一顶点数场景，200 次取平均）：

| 场景规模 | 修复前 | 修复后 | 加速 |
| --- | --- | --- | --- |
| 1,080 命令 / 3 层嵌套 | 23.9 ms/次 | 7.3 ms/次 | 3.3x |
| 9,720 命令 / 5 层嵌套 | 375.7 ms/次 | 83.9 ms/次 | 4.5x |

注：库为 `-O0` debug 构建，绝对值无参考意义，只看同一构建下的相对比例；命令发射本身
（含状态折叠、shader program 解析、`RenderCommand` 构造）在 -O0 下仍是主要成本，
在 Release 下会被内联摊销。

## 7. 公共 API 命名提案（未实施，属破坏性变更）

- 已实施（2026-09-11）：删除 `Geometry::setShape(Shape)`，见 §6；`Geometry::buffer(location)` /
  `bufferLocations()` → `attribute(location)` / `attributeLocations()`：
  “buffer”在 3D 引擎里通常指 GPU 缓冲，而这里是**按 shader location 索引的顶点属性**
  （`AttributeBuffer`）；现名易与索引缓冲/顶点缓冲混淆。
- `Geometry::setIndices(std::shared_ptr<UInt32Array>)`：以 `std::shared_ptr` 表达共享与
  `intrusive_ptr` 体系不一致（可选择统一到 `intrusive_ptr` 或文档化为何用 shared_ptr）。
- `RenderEngine::drawScenePass` 为私有但命名像公共 API；`RenderBackend::releaseWindowLayer`
  在 pass 身份化之后只剩 legacy 直连驱动在用，条件成熟时应删除（含其文档与测试断言）。
- `Overlay`：与 engine/backend 已脱钩（无 addOverlay/releaseOverlay），建议删除或明确降级为
  样例代码。


## 8. 状态一致性与资源生命周期（2026-09-11 终检）

本轮以“不变量”方式逐条核对后端保留状态与资源生命周期，修掉三类真实缺陷，并把
设备无关的机制用测试钉住。

### 8.1 保留缓存的身份：键必须“指向活对象”

`SceneBridge` 的保留缓存按**原始指针**索引（`Geometry*` / `ShaderProgram*`），但缓存
无法观测对象析构：条目会比对象活得久，且**地址被复用**时会把死条目的保留状态喂给新对象。
后果是静默错误而非崩溃：

| 缓存 | 复用地址后的错误后果 |
| --- | --- |
| `cache_`（几何节点） | 新几何被判定“未变更”（revision 相同）→ **画的是旧网格** |
| `rejected_`（拒绝记录） | 新几何命中旧拒绝记录的 revision → **合法几何被静默跳过（不显示）** |
| `program_stages_`（SPIR-V） | 新 program 命中旧 revision → **用旧 SPIR-V 编译，着色错误** |

修法（不新增公共 API，语义自洽）：**条目持有它所索引的对象**（`Item::geometry`、
`StageEntry::program`），地址在条目存活期间不可能被复用；同时把 `rejected_` 合并进
`Item`（一次哈希查找、一次扫描，且拒绝记录随条目一起被正确回收）。驱逐策略：

- 几何**离开帧**时先清拒绝记录（保留“修好数据后重新评估”的语义）；
- 条目**唯一持有者只剩缓存自身**（`useCount() == 1`，即 app 已放弃它）→ **立即**回收
  几何与条目，不占满 600 帧复用窗口（这是“持有键”方案的代价补偿：不长期钉住对象）；
- 仍持有（隐藏/剔除/临时离场）→ 沿用 600 帧窗口，重现时零重建（保持原快速路径）。

测试：`GeometrySafetyTest.RetainedCacheOwnsTheGeometryItIsKeyedBy`（子类计数：
app 释放后对象仍存活 → 空帧后立即被回收）。

### 8.2 帧在飞（frames-in-flight）的延迟释放队列

保留节点被**替换**时（数据 revision 变化 → 数据节点重建；材质/状态/程序标识变化 →
状态包装重建；条目驱逐）会丢掉旧节点，而旧节点的 `VkBuffer` / `VkPipeline` /
descriptor set 可能仍被**已提交但未完成**的命令缓冲引用：viewer 有多个命令缓冲槽在飞，
某个槽要等它的 fence 被等待后才重新录制。此时销毁会触发
`VUID-vkDestroyPipeline-00765` / `vkDestroyBuffer-*` 一类错误，严重时会让设备掉线。

修法：`SceneBridge::retireNode(node)` 把被替换的节点停放在**退役环**里，
`advanceRetireRing()` 每提交一帧推进一次并释放环上最老的一格。环深
`kRetireRingDepth = 4`（= 命令缓冲槽数 3 + 1）：节点在第 F 帧停放，第 F+3 帧开始时那个
可能的槽已被重新录制（`start()` 会先等它的 fence），因此**早于** F+3 帧末释放都危险，
深 4 恰好安全。调用点：`VsgRenderer::submitFrame()` 在 `recordAndSubmit()` + `present()`
之后推进；**未提交的帧不得推进**（一次推进对应一次 fence 等待）。

显式销毁路径（槽 teardown / resize / 释放 target / depth 策略变更 / `shutdown()`）本就
先 `deviceWaitIdle`（§3 铁律），`advanceRetireRing` 只覆盖那些**无等待**的活路径。

**2026-09-12 扩展**：渲染器**自己持有**的对象也有了同一条环（`Impl::retireObject()` /
`advanceRetireRing()`，同为 `kRetireRingDepth = 4`、同在 `submitFrame()` 的提交之后推进）：
被换下的 render pass / framebuffer（变体重建、提升撤销级联）、被丢弃的全屏 program 节点
（同帧撤销提升）、以及"某个 pass 本帧不再公告"时被摘下的视图（那条路径根本没销毁任何东西，
等待本来就是多余的）。判定标准仍是 §3：**破坏性销毁**继续显式 `deviceWaitIdle`。

**关键反例（实测，值得记住）**："把槽的 view 停放进环"**不能**替掉破坏性路径的等待 ——
`SceneBridge::clearCache()` 会清空共享对象注册表（`shared_objects_->clear()`），那里的管线 /
采样器**不一定**还有存活节点作为唯一持有者，于是被停放的 view 撤不走它们：把 target 重建 /
槽 teardown 的等待换成停放 view，lavapipe + validation 报出 **12 条**
`VUID-vkDestroyPipeline-00765` / `vkDestroySampler-01082`（`VkPipeline ... in use by VkCommandBuffer`
= 已录制但未重录的命令缓冲）。所以：**非破坏性路径停放（0 等待），破坏性路径（碰 clearCache /
丢图像）保留计数等待**，边界就这么定的。

测试：`GeometrySafetyTest.ReplacedDataNodeIsParkedUntilTheRingAdvances`（用
`vsg::Object::referenceCount()` 断言被替换的数据节点在环推进前仍被持有、推进 4 次后释
放）；设备侧 `vsg_backend_selftest::runInFlightChurnPhase()` 每帧替换数据/材质标识/绘制
集合做 churn 回归。**诚实说明**：软件光栅器同步完成提交，该 phase 无法真正复现“销毁
时仍在飞”，因此它只作为 churn 回归；机制正确性由上述设备无关测试 + 帧槽推理保证。

### 8.3 场景图不变量：拒绝成环

`Group::addChild` 原来只挡“自己作为自己的孩子”，不挡祖先环（`a->addChild(b)` 后
`b->addChild(a)`）→ 图不再是树，而命令收集/包围盒/拾取/查找都是递归遍历：
**栈溢出**（不是可诊断错误）；同时破坏“一趟遍历内一个节点只有一个世界矩阵”这一
包围盒缓存前提。现按祖先链检查并**静默拒绝**（与已有的 null/自身拒绝一致），
`Group.hpp` 文档化。测试：`NodeTest.AddChildRejectsCycles`、`NodeTest.ChildHierarchyReparentsInsteadOfAdoptingTwice`。

### 8.4 复查通过（无需改动）

- 线程模型：插件内**无** `std::thread` / 锁；viewer 不启 DatabasePager 线程池，
  record/submit/present 都在调用线程同步完成 → 状态一致性问题域是单线程的（引擎与
  UI 的跨线程约定仍应在 app 层文档化）。
- `VsgRenderer::~VsgRenderer()` → `shutdown()`：先 `deviceWaitIdle()` + `removeWindow()`
  + `close()`，再整体重建 `Impl`（一次性丢弃 window/viewer/图/槽/管线），最后
  `materialManager.clear()`；不会在飞销毁。
- 破坏性重建（`clearCache()`）的 5 个调用点均在 `waitForIdle()` 之后（槽 teardown、
  resize、释放 target）。
- `variant_cache_` 逐出（清模板）与 `program_shader_sets_` 上限逐出都只丢**模板**，
  已建管线仍被各自保留的状态组持有 → 不会销毁在飞对象。

### 8.5 仍未做（逐项设计登记见 §9）

1. **D13 材质缓存无逐出 + 地址复用风险** → §9.1
2. **跨 pass 命令列表缓存** → §9.2
3. **`VkPipelineCache` 持久化** → §9.3
4. **合并每 pass 的 `beginRenderPass`（dynamic rendering）** → §9.4 🔴 被上游 vsg 阻塞（仅登记）

## 9. 待办设计登记（4 项；2026-09-11 起，逐项实施/登记）

本节只做**设计登记**（问题、根因、方案、验收、风险、依赖），实施排期与优先级在各条目末尾。
四项都属于“非正确性阻塞”但会被真实场景触发的缺口，登记目的是让后续切片有明确的入口与验收口径。
（§9.4 不同：它是**结构性重构方向**，且当前**无实现路径** —— 登记它是因为 §28 的一批机制
本是为它之前的限制而存在，将来若要动，入口与判据都写在那里。）

### 9.1 材质缓存的逐出与身份（D13，§8.1 同源）——**已实施（2026-09-11）**

> 实施记录见 §12；本节保留设计原文（方案 A 已落地：条目自持 + 放弃即释放 + 容量上限，
> 且 `updateMaterial` 成为唯一就地刷新路径，D19 一并收掉）。

**现状（代码事实）**
- `VsgMaterialManager::cache`：`std::map<Material*, ref_ptr<PhongMaterialValue>>`，只在
  `getOrCreate(Material*)` 中插入；`SceneBridge` 建管线时经由 `material_manager_` 取用。
- `updateMaterial / releaseMaterial / find / forEachMaterial / materialCount / clear` 都已实现，
  但除 `shutdown()` 的 `materialManager.clear()` 外**全仓零调用点**。
- `MaterialManager.hpp` 的契约把“释放时机”交给调用方（`releaseMaterial()`）。

**根因**
- 引擎侧没有“材质解绑 / 销毁”事件：`Geometry::setMaterial(nullptr)`、几何销毁、场景清空都不会
  通知后端；`MaterialManager` 只能靠调用方显式 `releaseMaterial()`，而调用方从未调用。
- 后端按**裸指针**索引，且条目不自持材质 → 与 §8.1 同类：材质先于条目销毁、地址被复用后，
  旧 `PhongMaterialValue` + descriptor 会被新材质复用。

**影响**
1. 内存：会话内每个出现过的 `Material*` 留一份 Phong UBO/descriptor（只增不减，最像泄漏的留存）。
2. 正确性（静默）：同地址新材质渲染成旧颜色/旧高光，且无任何诊断。

**方案**
- **A（推荐，与 §8.1 同法，纯后端内改动）**：把缓存值改成自持条目
  `struct Entry { vine::intrusive_ptr<Material> material; ::vsg::ref_ptr<::vsg::PhongMaterialValue> value; }`；
  驱逐策略同样双轨：`useCount() == 1`（仅缓存持有）→ 立即驱逐；离开使用集合后按帧计数兜底，
  再加**容量上限**（建议 256，与 `program_shader_sets_` 的 64/256 量级一致）兜底；
  `releaseMaterial(m)` 语义不变（仍作显式入口，供引擎调用）。
- **B（引擎侧事件）**：`Geometry::setMaterial` / `~Geometry` / `Scene::clear` 时调
  `MaterialManager::releaseMaterial`。语义更直接，但要把 manager 反查到引擎/几何（跨模块耦合、
  且析构期间调用需小心），**故不作为首选**；两种可并存（A 保证后端自洽，B 让资源更早释放）。

**验收**
- 新增 `test_vsg` 用例（对应 §8.1 的 `TrackedGeometry` 写法）：`TrackedMaterial` 在 app 释放后
  立即被驱逐（活体计数归零）；同地址新材质不复用旧条目（旧值 / descriptor 不再共享）。
- churn 场景下 `materialCount()` 稳定（不随帧数增长）；`shutdown()` 后为 0。
- 现有 `VsgMaterialManager` 相关用例与 lavapipe 门禁保持 PASS。

**风险 / 回滚**
- 自持会延长 CPU 侧生命周期 → 由 `useCount()` 立即驱逐抵消（同 §8.1）。
- 容量上限逐出会丢掉 UBO 复用（下次重建 descriptor）→ 只影响性能，不影响正确性。
- 回滚成本低（改回裸 map 即可），不涉及公共 API。

**依赖 / 优先级**：无外部依赖；🔴（正确性 + 内存，且与 §8.1 同一根因，建议紧随其后做）。

### 9.2 跨 pass 命令列表缓存（D27）

**现状**
- `Scene::collectRenderCommands(camera)` 每调一次就**全树遍历 + 剔除 + 排序**，返回新
  `std::vector<RenderCommand>`；`RenderEngine::frame()` 对每个 pass 各调一次。
- 单次遍历内部已是 O(n)（§6.1：自顶向下矩阵累积、每趟 bbox 只算一次、排序键预计算）。
- 引擎典型场景 3~5 个 pass 共用同一 `(scene, camera)`（主 pass + HUD + PiP + 离屏 + 后处理输入），
  即把同一份结果算 3~5 次；命令列表对同一帧的同一 `(scene, camera)` 是**确定性**的。

**根因**
- `RenderPass` 只持有 `content`（`Scene*`）与 `camera`，没有“本帧已为该组合收集过”的记忆；
  收集结果也没有跨 pass 的存放位置（每 pass 一个临时 vector）。

**方案（设计要点，含必须先答的问题）**
1. **缓存键 = `(scene, camera, frame_seq)`**，`frame_seq` 由 `RenderEngine` 自增（每 `frame()` +1）。
   **不用**脏标记/时间启发式：相机被就地修改（`Camera::setViewMatrixAsLookAt` 不换指针）也必须失效，
   帧序号是唯一安全兜底。
2. **放置位置**：`RenderEngine`（调度器）内的 per-frame 缓存，**不放进 `Scene`**（Scene 不应知道 pass）。
   引擎在 `frame()` 开始时清空缓存，逐个 pass 查询。
3. **返回只读视图**：缓存持有 `std::vector<RenderCommand>`，向 pass 暴露
   `std::span<const RenderCommand>`，避免每 pass 拷贝；`RenderBackend::render` 增加 `std::span` 重载
   （保留现有 `const std::vector&` 重载以免破坏后端实现）。
4. **保守起步（建议第一切片）**：仅当本帧存在**多个 pass 使用同一 `(scene, camera)` 且都不做 per-pass
   追加/覆盖** 时才复用；HUD 叠加类 pass（追加自己的命令）走原路径。全量复用留作后续切片
   （需要“每 pass 附加命令”的合并策略与排序口径）。
5. **语义边界**：`Scene::collectRenderCommands` 保持现有可见性/透明度/排序语义不变；缓存只消除
   重复计算，不改变命令内容与顺序（含 §6.1 的稳定排序语义）。

**验收**
- `RenderEngineTest`：同帧内 N 个 pass 共享 `(scene, camera)` → `collectRenderCommands` **调用计数为 1**；
  帧推进后 +1；`scene` 结构变更（`setRoot`/`addChild`/`removeChild`）后 +1；相机就地修改后 +1。
  （计数用测试桩注入，避免依赖计时。）
- 命令内容等价性测试：缓存路径与非缓存路径产出的命令序列逐项相等（含 `opacity/isTransparent/
  renderState/depthExplicit/program/modelMatrix`）。
- 基准记录：3~5 pass、≥10k 命令场景的 CPU 时间改善（同一 debug 构建对比，方法同 §6.1）。

**风险**
- 键不全 → 画面停留旧帧（相机就地修改、scene 结构未通知）→ 由 `frame_seq` + 结构 revision 双保险，
  并在验收里显式覆盖这两个反例。
- 生命周期：命令持 `intrusive_ptr<Geometry/Material/ShaderProgram>`，缓存会延长一帧内的持有期
  → 仅一帧，且引擎在帧尾释放（与 §8.1 的自持策略一致方向）。

**依赖 / 优先级**：可能需要给 `Scene` 加结构 revision（或让引擎在 pass 注册/场景挂载时失效）；
🟡（性能，非正确性）。

### 9.3 VkPipelineCache 持久化 / PSO 磁盘缓存（D28）

**现状**
- vsg 的 `GraphicsPipeline::compile(...)` 内部以 `VK_NULL_HANDLE` 作为 `VkPipelineCache`
  （已在 `build/_deps/vsg-src/src/vsg/state/GraphicsPipeline.cpp` 核对）→ Vine **无法从外部注入**
  管线缓存；每次进程启动都要重新创建全部 PSO。
- 已有缓解：glslang 阶段的 SPIR-V 结果在 Vine 侧按（program, revision）缓存（L1a）、按（program,
  layout）缓存 ShaderSet（L1b）、按状态变体缓存管线模板（L2），但这些都是**进程内**。
- 现状影响：冷启动/场景切换的 PSO 创建耗时无法跨会话复用（磁盘上没有可复用的 `VkPipelineCache`
  数据）。

**方案（按侵入度排序）**
- **A 等上游**：向 vsg 提 issue/PR，让 `GraphicsPipeline`（或 `Device`）暴露 `VkPipelineCache`
  注入点。Vine 侧零改动，后续只需“建 cache → 传 → 落盘”。**首选**（成本最低，且不与 vsg 版本耦合）。
- **C 折中（先核对可行性）**：若 vsg 允许在 `Options`/`Device` 上挂 cache 句柄（需查新版 API），
  则 Vine 只做“数据载体”：注入 + `vkGetPipelineCacheData` 落盘 + 启动时回读。侵入度小。
- **B 自建管线**：绕过 vsg 的 `GraphicsPipeline`，自己 `vkCreateGraphicsPipelines` + 自己的
  `VkPipelineCache`。需重做 vsg 的 shader 组装/descriptor 兼容/绑定逻辑，与 vsg 版本强耦合，
  **不推荐**（除非上游长期不支持）。

**验收**
- 冷启动 vs 热启动对比：PSO 创建耗时或 `VkPipelineCache` 数据大小 > 0（最小可测口径）；
  可用 `pipelineVariantCount()` + 计时日志作为观察点。
- **失效安全**：cache 文件损坏 / `pipelineCacheUUID` 与驱动或 Vine 版本不匹配时必须**丢弃并重建**
  （绝不因坏 cache 崩溃或产生非法管线）；写入使用临时文件 + 原子重命名。
- 磁盘路径可配置且默认关闭（避免在只读/容器环境写文件失败）。

**风险**
- 跨驱动/跨版本复用非法 → 必须校验 `pipelineCacheUUID`。
- 多线程创建管线需 `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` 时自行加锁（当前后端
  单线程，仍应在文档标注前提）。

**依赖 / 优先级**：依赖 vsg 能力（A/C 路线）；🟢（启动性能，非正确性）。

### 9.4 合并每 pass 的 `beginRenderPass`（dynamic rendering）——**上游卡住，仅登记（2026-09-12）**

> 本条是 §28 的后续方向，此前只在批次小结里被列为"待办①"，**没有写清它被上游挡住** ——
> 本节补齐：现状、能删掉什么、为什么现在动不了、以及将来动手时的第一步。

**现状（代码事实）**
- §28 之后每个 pass 有自己的 render pass 对象 + framebuffer，记录进自己的 `vsg::RenderGraph`
  （`Impl::PassObjects::graph`）。一个目标上 K 个 pass ⇒ 一帧 **K 对**
  `vkCmdBeginRenderPass` / `vkCmdEndRenderPass`（vsg 侧就在
  `src/vsg/app/RenderGraph.cpp:150/170`）。
- 之所以"每 pass 一个对象"：**一个 render pass 只能烧死一种 load-op 组合**，而清屏的 pass 与保留的
  pass 需要不同组合。由此派生出一整类机制：变体工厂
  （`makeColorDepthRenderPass(depth_initial, promote_depth, color_clear)`）、
  "变体必须结构兼容"（子通道依赖逐字段相同，否则 `renderPass-02684`）、
  布局处于过渡态时的一次性变体 + 帧末换回（`render_pass_transient`）、
  以及宿主运行期改清屏策略要重建（D49）。

**合并指什么**：同一个目标的 K 个 pass 只记录 **1 个渲染作用域** —— 用
`vkCmdBeginRendering` / `VkRenderingInfo` 内联 attachments，load/store 变成**逐 pass 的取值**
（`VkRenderingAttachmentInfo::loadOp`），布局转换从 render pass 的 initial/final layout 变成
**显式 barrier**，管线改用 `VkPipelineRenderingCreateInfo`（写格式而非 render pass 对象）。

**能删掉什么 / 代价**
- 删：变体工厂与"兼容"不变量、一次性 transient 变体 + 帧末换回、D49 的"请求变了就重建"、
  每帧 K−1 对 begin/end。
- 付：提升 / 撤销 / seed / 回读这四处的布局记账要**重新实现成 barrier**（语义不变，更微妙）。
- **不白费**：`planPassVariant()` 的**决策**原样复用（喂给 `VkRenderingAttachmentInfo`），
  selftest 全部相位（`clear flip:` / `preserved depth:` / `depth only preserve:` /
  `promotion revoke` / `policy churn:` …）作为行为契约**不动** —— 判据先有，重构才有底。
- 收益性质：桌面 / 离屏路径的 begin/end 开销很小，**主要收益是简化**，不是性能
  （tile-based GPU 与记录开销才会真吃到）。

**为什么现在动不了（实测证据，vsg 1.1.16）**
1. `build/_deps/vsg-src` 全树**没有** `vkCmdBeginRendering` / `VkRenderingInfo` /
   `dynamic_rendering`（`include/`、`src/` 都搜过）：vsg 的记录路径只有
   `RenderGraph` 的 begin/end render pass。
2. **没有任意命令的记录钩子**（`include/vsg/commands/` 里没有 `CustomCommand` 一类），
   所以无法在 vsg 的遍历里插一个 `vkCmdBeginRendering`。
3. `src/vsg/state/GraphicsPipeline.cpp:233` 是 `pipelineInfo.renderPass = *renderPass;`、
   紧接着 `pNext = nullptr` ⇒ 管线**硬绑** render pass 对象，产不出 dynamic-rendering 形态的管线。

⇒ 两条落地路径：**(a) 等上游 vsg 支持**（或合入 PR）；**(b) 渲染记录层自研**
（不再用 vsg 的 `RenderGraph` / `GraphicsPipeline`）。后者是"换后端记录实现"量级，**不是 §28 的小后续**。

**触发条件**（满足其一再开工）
- 上游 vsg 支持 dynamic rendering（或我们用 PR 推上去）；
- 实测记录开销 / tile GPU 的 load-store 成为瓶颈（`offscreenBuildCount` 与帧率对照）。

**第一步（spike，判据用现有相位）**
- 目标：**一个** target、**一个** 作用域用 `vkCmdBeginRendering` 跑通（管线用
  `VkPipelineRenderingCreateInfo`，attachments 的初始布局自己 barrier）。
- 判据：复用 `clear flip:`（运行期清屏策略改变生效）+ `policy churn:`（策略变化帧 0 次设备等待、
  退役环释放、末帧像素/深度正确）+ 门禁 VUID 0；再做反证（去掉某个显式 barrier ⇒ 必红）。
- 只在 spike 绿之后才谈"把 `PassObjects` 换成作用域分组"，并同步删除对应单元测试
  （`PassRenderPassPlanTest` 里只有与 render pass 对象绑定的部分会消失，决策部分留下）。

**风险**：布局转换责任转移到应用层，**错误是静默的**（表现为随机花屏 / 深度错，而非 VUID）⇒
必须靠像素 / 深度断言与反证，而不是只看"无 VUID"。

**依赖 / 优先级**：🔴 **被上游阻塞**（vsg 1.1.16 无能力）；正确性上无缺口，登记以备将来。

## 10. 后端诊断通道（2026-09-11 落地）

**问题**：`RenderBackend` 的 22 个方法是 `void`、仅 5 个返回 `bool`，81% 的接口**无法报告失败**；
后端内部 20 处 `return nullptr/false` 静默返回、34 处 `fprintf(stderr, ...)`（`vine/logging` 用量 0）。
于是"没画出来"对宿主完全不可见 —— 这正是本模块最大的风险面（终检 §8 的 9 个缺陷除 1 个外全是静默类）。

**契约（`vine/graphics/RenderDiagnostic.hpp`，增量、非破坏）**

```cpp
enum class DiagnosticSeverity { Info, Warning, Error };      // Error = 该内容没被画出
enum class DiagnosticCategory { GeometryRejected, ChannelIgnored, ShaderFallback,
                                CompileFailed, TargetBuildFailed, ContentSkipped,
                                InitFailed, Count };
struct RenderDiagnostic { DiagnosticSeverity severity; DiagnosticCategory category; String message; };
using DiagnosticSink = std::function<void(const RenderDiagnostic&)>;
```

- `RenderBackend::setDiagnosticSink(sink)` / `diagnosticSink()` / `diagnosticCount()` /
  `diagnosticCount(category)`；`protected reportDiagnostic(...)` 供后端上报。**默认实现齐全 → 现有后端零改动**。
- 分类按**后果**（宿主需要知道什么）而非原因划分，且是**枚举**（宿主/测试靠 `switch` 匹配，改文案不破坏调用方）；
  severity 与 category 正交。
- `RenderEngine::setDiagnosticSink(...)` 是宿主的稳定入口：引擎**保存** sink 并应用到当前后端，
  以及**之后**才 setBackend 的后端（宿主不必关心后端实例何时创建）。

**vsg 后端接线**

- `VsgRenderer` 是**唯一上报权威**：`reportFailure(sev, cat, msg)` = stderr 追踪 + 后端计数器 + 宿主 sink。
  槽内 `SceneBridge` 的拒绝/降级经由 `installDiagnosticRoute(bridge)`（bridge 的 sink 被设为一个
  回调到 `reportFailure` 的 lambda）→ **bridge 的发现就是后端的发现**（`diagnosticCount()` 诚实；
  bridge 自身不再写 stderr，避免双份追踪）。
- 已接线的失败点：loc0 不可用/分量 stride 非法/索引越界（`GeometryRejected`，Error）、
  loc1/loc2/自定义通道被丢弃（`ChannelIgnored`，Warning，网格仍然绘制）、
  用户 program 编译或装配失败→回退内建（`ShaderFallback`，Warning；**原 D9 全静默**）、
  PiP/全屏 program/Screen pass 的编译与装配失败（`CompileFailed`）、
  离屏 target 建不起来（`TargetBuildFailed`）、pass 内容准备失败/相机桥失败/PiP 反馈环拒绝
  （`ContentSkipped`）、初始化失败（`InitFailed`）。
  自由函数 helper（`makeScreenTextureNode` / `makeFullscreenProgramNode` / `makeCompiledOverlayView`）
  不自行上报，改为**返回失败原因**（`ProgramNodeFailure` / out-param），由调用方（成员函数，
  知道是哪个 pass）上报 —— 与 `unpackXyz` 同一手法。
- 频次纪律沿用既有语义：**按 revision 上报**（几何拒绝、program 编译）而不是每帧刷屏；
  帧内重复出现的内容不会重复上报。

**宿主侧（appfw）**：`RenderControl` 安装一个把诊断写进 `vine/logging` 的 sink
（`logger.error/warn/info("[graphics] {}")`），这样窗口应用（看不到 stderr）也能在日志/控制台看到
"哪块内容没画出来、为什么"。`src/app/src/main.cpp` 已初始化 console + 按日文件 sink。

**验证**

- 设备无关：`test_vsg` 新增 3 例（`DiagnosticsTest.*`：按 revision 只报一次、丢弃通道是 Warning
  且仍绘制、干净帧零诊断 + 清空 sink 后只计数不再投递）；`test_graphics` 新增 1 例
  （引擎把 sink 转发给当前与之后的后端，`diagnosticCount()` 一致）。共 151 + 58 全绿。
- 设备侧：`vsg_backend_selftest::runDiagnosticsPhase()` 在真实 Vulkan 上装 sink、喂入
  "索引越界网格 + 编译不过的 program"，6 帧后断言 **1 条 GeometryRejected + ≥1 条 ShaderFallback**、
  `diagnosticCount() == 收到条数`、消息非空、清空 sink 后仍可继续渲染。
- lavapipe 门禁 RESULT: PASS（0 VUID）。

**仍未做**：`gfx_backend_vsg.md` 里的 `fprintf` 信息性追踪（"target attached"、"released GPU
resources" 等）仍是 stderr —— 它们是**追踪**不是失败，收敛到日志系统需要同时改 harness 的 stderr 断言。

## 11. pass 协议的显式化（2026-09-11 落地）

**问题**：一个 pass 的状态原本散在 7 个 `pending_*` 字段里（`pending_presenting` /
`pending_depth_mode` / `pending_viewport` / `pending_lights` / `pending_pass_order` /
`active_target` / `pending_compile_views`），由 `beginPass` 与 `endPass` 各调一次
`resetPerPassState()` 保持同步，并被 **三个不同入口**（`render()` 与两个 `drawScreen*()`）读取。
后果是"这次调用是什么意思"取决于调用顺序 —— 本会话修掉的 6 个缺陷（槽残留、幽灵绘制、depth 冻结、
同键别名、pending 泄漏、presenting 首次冻结）全部出自这里；而且新加一个字段很容易忘记在 reset 里清。

**改法：请求即状态**

```cpp
struct PassRequest {                     // 一个 pass 的完整请求
    const RenderPass* pass;              // 身份（beginPass 宣布）
    RenderTarget*     target;            // setRenderTarget（nullptr = 窗口）
    int               order;             // setPassOrder（叠放位置）
    DepthMode         depth_mode;        // setDepthMode（显式，与 clear 无关）
    bool              presenting;        // clear() 标记：本 pass 填满 target
    std::optional<Viewport> viewport;    // setViewport —— 每次绘制消费
    std::vector<const Light*> lights;    // setLights   —— 每次绘制消费
    std::size_t draws;                   // 本请求服务过的绘制次数（诊断）
};
PassRequest request;   // 进行中的请求：打开中的 pass 作用域，或直连驱动者的队列
bool pass_open;        // beginPass 打开、endPass 关闭
```

- **作用域属性 vs 每次绘制属性**（这是本次唯一的语义收紧）：
  `target / order / depth_mode / presenting` 描述的是**这个 pass**，因此该作用域内每次绘制都能看到
  它们（一个 pass 画两次时两次都用同样的叠放位置 / 深度策略 / 目标），并在 `endPass()` 被丢弃；
  `viewport / lights` 是**每次绘制**的，由紧随其后的绘制调用 `takeViewport()/takeLights()` 消费。
  旧行为是"全部被第一次 render 消费"（同一个 pass 内第二次 render 会拿到 order=0 / 默认深度 / 窗口目标）
  —— 那是隐式协议的产物，不是设计意图。
- **重置只有一次赋值**：`resetPassRequest()` 就是 `request = PassRequest{};`，
  新字段不可能被忘记清理（`beginPass` 与 `endPass` 都调它）。
- **违反协议会被上报**（新 `DiagnosticCategory::PassProtocolViolation`，Warning）：
  - `beginPass()` 时已有打开的作用域（嵌套）→ 旧作用域的请求被丢弃，这可能让调用方以为生效的
    target/order/depth 实际没生效；
  - `endPass()` 时没有打开的作用域（不配对）→ 本次宣布的请求早已被丢弃。
- **零成本契约逃生口**：直连驱动（设备自检、legacy 键路径）从不调用 beginPass/endPass，此时
  `request` 就是普通请求队列：不丢、等下一次 set* 覆盖，行为与之前一致（`isPassScopeOpen()` 返回 false
  可供宿主/测试断言协议状态，SDK 契约里新增该查询的默认实现）。
- SDK 契约（`RenderBackend::beginPass/endPass/isPassScopeOpen` 文档）已把"作用域包含什么、哪些是
  作用域属性、哪些是每次绘制属性、违反如何上报"写清楚。

**验证**

- `tests/test_vsg/PassProtocolTest.cpp`（新，3 例，**无需设备**：`VsgRenderer` 构造不建窗口/设备，
  作用域调用只碰请求结构；测试目标因此把 `VsgRenderer.cpp`/`CameraBridge.cpp` 一并编入）：
  不配对 endPass 上报一次且后续合法序列不再报；嵌套 beginPass 上报且新 pass 干净（外层请求被丢弃）；
  合法序列（引擎协议 + 直连驱动）**零诊断**；`isPassScopeOpen()` 在作用域开/关时正确。
- 既有 151（test_graphics）+ 61（test_vsg）全绿；lavapipe 门禁 RESULT: PASS（含 pass 协议 phase、
  共享深度、在飞 churn、诊断 phase），说明作用域语义对引擎与自检两条驱动路径都成立。

## 12. 统一后端缓存骨架 + 材质缓存重做（D13 / D19，2026-09-11 落地）

**骨架**（`src/plugins/gfx_backend_vsg/src/OwnedCache.hpp`，120 行、无框架味）：

```cpp
template <class Object, class Payload> class OwnedCacheEntry;   // 自持键对象 + 序号 + 载荷
template <class Map> std::size_t eraseAbandoned(Map&);          // 唯一持有者只剩缓存 -> 立即回收
template <class Map> std::size_t trimToCapacity(Map&, std::size_t); // FIFO 逐出最旧（永不动 null 键）
class InsertionClock;                                            // 每缓存一个单调序号
```

把本轮 §8.1 在几何缓存上手工实现的“**条目自持键对象 + `useCount()==1` 即回收**”规则抽成可复用、
只写一次的不变量，并把两半分开说明：自持解决“地址被复用”，回收解决“自持变泄漏”；而“仍被
app 持有但不绘制”的对象如何处置留给各自策略（几何用 600 帧复用窗，材质不需要窗口——见下）。

**D13 材质缓存（红项）**：`VsgMaterialManager::cache` 改为
`unordered_map<Material*, OwnedCacheEntry<Material, Entry>>`：

- 条目自持 `Material` → 指针键在其存活期内不可能被复用（同地址新材质永远拿到自己的条目，
  不会再被喂旧 `PhongMaterialValue` + descriptor）；
- 新增 `releaseAbandoned()`：app 释放后（`useCount()==1`）**立即**回收条目与材质，由
  `VsgRenderer::submitFrame()` 每提交帧调用一次 —— 这就是 D13 “只增不减/最像泄漏”的解法；
  仍被 app 持有的（隐藏/剔除对象）保留条目，避免无谓重建 descriptor；
- `kMaxEntries = 256` 兜底 + **FIFO 逐出最旧**（活跃场景用的是新条目，稳态不会逐出刚要用到的），
  null 键的默认材质条目永不被逐出；
- `releaseMaterial()` / `clear()` 语义不变（仍是调用方的显式工具）。

**D19 单一刷新路径**：`SceneBridge` 里那段“比较参数 → 写入 → dirty”循环删掉，改为逐材质调
`materialManager.updateMaterial(m)`；**比较与写入的知识移进持有该值的缓存**（`Entry` 记住上次
写入的 `PhongParameters`，相等即不写、不 dirty、不传输）。于是 `updateMaterial` 第一次有了真实
调用点（此前全仓零调用），且稳态零传输的性质由测试钉住。

**测试**（`tests/test_vsg/MaterialManagerTest.cpp`，6 例）：
- 放弃材质在下一次 sweep 被释放（`TrackedMaterial` 活体计数归零），**仍被持有的材质幸存**；
- **地址唯一性**：条目自持 ⇒ 存活条目的地址不可能被回收 ⇒ 新材质不会继承死条目的值（D13 的
  别名性质做成确定性断言）；
- 容量上限：插入 `kMaxEntries + 8` 后 ≤ `kMaxEntries`，最新保留、最旧逐出、默认条目不动；
- `updateMaterial` 只在**变化时**重写（用 `vsg::Data::differentModifiedCount` 断言稳态不 dirty，
  改属性后同一对象跟随之并再次归于安静）；
- 显式 `releaseMaterial()` / `clear()` 语义不变。

验证：test_vsg 61 → **67** 全绿，test_graphics 151 全绿；lavapipe 门禁 RESULT: PASS（真实 app
的材质路径 —— HUD / deferred lighting / 每帧就地刷新 —— 全过）；dist 部署冒烟健康。

**仍未做**：D16 的变体/ShaderSet 缓存仍是“超限即整表清空”（正确：只丢模板，已建管线仍被保留
状态组持有），可后续换成同一骨架的 FIFO 逐出；§8.1 的几何缓存可迁到同一骨架（当前是手工实现
且行为正确，迁移只减 bespoke 代码、不修缺陷）。

## 13. 模块拆分：从 3.6k 行单文件到按职责分层的多 TU（2026-09-11 落地）

**问题**：`VsgRenderer.cpp` 一个文件 3603 行，把四件互不相关的事塞在一起（vsg 对象
工厂、会话生命周期、离屏目标构建、叠加绘制、pass 协议 + 槽），并且共享一个 900 行的
匿名命名空间。后果是真实的：改渲染通道要重编整文件；共享 helper 隐式耦合（谁用谁不
用只能靠读）；一个函数的文档注释长期漂移到另一个类型上（见下）；没人能一眼说出
"改动该往哪儿放"。

**布局**（拆分后；职责单一，每个 TU 只依赖它真正用到的头）：

| 文件 | 职责 | 行数 |
| --- | --- | --- |
| `src/VsgRendererImpl.hpp` | 类定义 + `Persistent` / `Impl`（会话态：窗口、viewer、命令图、目标表、槽、pass 请求）+ 私有 helper 声明 | 2091 |
| ~~`include/vine/vsg/VsgRenderer.hpp`~~ | ~~类声明（公开契约 + 私有嵌套类型）~~ —— **§44 已删除**（并入 `src/VsgRendererImpl.hpp`） | — |
| `src/VsgRenderer.cpp` | 会话生命周期、帧泵、诊断路由、查询访问器 | 901 |
| `src/VsgRendererPasses.cpp` | pass 协议（begin/end/releasePass、退役、重定向）+ 内容槽搭建/绘制 | 519 |
| `src/VsgRendererTargets.cpp` | 离屏目标构建与顺序、目标/槽释放、视图按序摆放 | 680 |
| `src/VsgRendererOverlay.cpp` | PiP 采样 blit、全屏用户程序（含视图编译与光照 push 块填充） | 697 |
| `src/VsgPipelineFactory.{hpp,cpp}` | vsg 对象工厂：格式转换、渲染通道、着色器集、管线状态、叠加/程序节点、灯光节点 | 298 / 598 |
| `src/VsgBackendUtility.{hpp,cpp}` | 图手术（摘子节点）、设备同步、会话策略查询 | 57 / 39 |

**三条规则**（新增代码照此放置）：

1. **"只依赖显式参数的纯工厂"进 `VsgPipelineFactory` 的 `detail` 命名空间**：它不读
   渲染器状态，所以能脱离"哪个 pass 问的"单独推理与复用；工厂只**返回失败原因**
   （enum / out-param），不自己上报（见 §10 的既有约定）。
2. **会话态只放 `VsgRendererImpl.hpp`**：`Impl` 持有所有引用 `vsg::Window` /
   `vsg::Device` 的东西，`shutdown()/initialize()` 整块替换，因此不会漏释放；
   扩展会话态 = 改这一个头（各 TU 自动可见）。
3. **新成员函数按职责进对应 TU**，不需要额外的声明（成员已在类里声明）；跨 TU 的
   自由函数一律进 `detail`，各 TU 用一句 `using namespace detail;` 保持调用点原样。

**顺手修掉的两处**（拆分时必须做的决定，不是重写）：

- 叠加绘制节点的文档注释长期挂在失败枚举（`ProgramNodeFailure`）上方一格：拆分按
  "文档随声明走"搬运，注释回到了它描述的函数；
- `LightPushBlock` 的两条 `static_assert`（尺寸 = 128、对齐 = 16）原本落在共享匿名
  命名空间里，离结构体很远：现在贴回结构体定义（该文件顶部），改字段会立刻在编译期
  报错。

**验收**：所有目标零警告构建；test_vsg 67、test_graphics 151 全绿；`gfx_lavapipe_check.sh`
→ `RESULT: PASS`（该门禁覆盖了全部搬走的代码：离屏 MRT、PiP 采样、全屏程序、pass 协议
与退役路径）；dist 冒烟健康。**行为零变更**：纯搬运（含 `git diff -M` 可核对），
`test_vsg`/`vsg_backend_selftest` 的源列表同步更新（二者直接编译插件源码，不能链接
MODULE）。

**仍未做**：`SceneBridge.cpp` 1532 行仍偏大（`syncSceneForSlot` 281 行），可按同一
思路再切（几何同步 / 程序与材质同步 / 缓存决策三类）。

## 14. 后端契约写进 `RenderBackend.hpp`（2026-09-11）

**为什么**：该接口有 40+ 虚函数，而类级注释只有一句"抽象渲染后端接口"。实现者（与宿主）
真正需要知道的六件事——调用序、借用语义、可保留什么、线程、失败语义、诊断——一条都没写，
只能靠读 vsg 后端反推。现在它们是 `RenderBackend.hpp` 类级注释里的规范性文本，本节只记录
**本后端的具体数字**与落地说明。

**契约要点（正文在头文件）**：

1. **调用序**：`beginFrame` → 每个启用 pass（按 order 升序）`beginPass` → `setPassOrder`
   → 可选逐 pass 状态（`setRenderTarget` / `setViewport` / `setLights` / `setDepthMode`
   / `clear`）→ 绘制（`render` / `drawScreenTexture` / `drawScreenProgram`）→ `endPass`
   → `endFrame` → `swapBuffers`（**唯一的 present 点**，`endFrame` 不得呈现）。
   **首帧之前有 warm-up**：引擎先把每个"启用且非清屏"的 pass 跑一遍 —— 后端因此能在呈现
   任何一帧之前看到完整的 pass 集合，把保留态摆到最终位置（本后端正是据此让"首帧前创建"
   的槽落在正确堆叠序）。`beginFrame` 可能已获取下一张交换链图像，所以被驱动的帧必须以
   `swapBuffers` 收尾。
2. **借用语义**：所有指针/引用参数（camera、commands、lights、target、program）只在该次
   调用期间有效；`beginPass` 的 pass 只在其作用域内有效。宿主可以在调用返回后立刻销毁它们，
   pass 也可以随时被移除（由 `releasePass` / `releaseRenderTarget` 宣告）。后端需要后续
   使用就必须拷贝或上传，**不得保留这类指针**。
3. **可保留什么**：保留 GPU 状态（内容视图、编译好的管线、采样槽、纹理缓存）是预期行为，
   但必须按"被服务对象的寿命"定键、由对应的 `release*` 释放、且**不随帧数增长**（长跑必须
   收敛到稳态）。后端不得让宿主为了正确性而调用 `release*`。
4. **线程**：单线程、串行、不可重入 —— 后端无需加锁，但不得假设跨 `initialize`/`shutdown`
   保持同一线程。**诊断 sink 是在后端调用内部同步回调的**，sink 只能记录并返回，不得回调后端。
5. **失败语义**：失败必须上报（§10），接口内不抛异常；`false` 永远不表示"部分生效"。
   **`initialize()` 返回 false 时后端自己收拾残局** —— 引擎只在 initialize 成功后才调
   `shutdown()`（见 `RenderEngine::shutdown`）。本后端已满足（三条失败路径都先 `shutdown()`
   再 `return false`），现在把它写成要求，避免下一个后端实现者踩。

**保留预算（本后端的数字；契约本身只要求"有界 + 可释放"）**：

| 保留项 | 上限 | 何时释放 |
| --- | --- | --- |
| 在飞几何/数据节点（退役环） | `kRetireRingDepth = 4`（= 命令槽 3 + 1） | 每提交帧推进一格，格子里的旧节点才销毁 |
| 几何缓存条目（未再出现的） | `kAbsentEvictFrames = 600` 帧 | 连续 600 帧未出现即回收 |
| 材质缓存 | `kMaxEntries = 256` 条 | app 放弃即回收（每提交帧 sweep）+ FIFO 兜底 |
| 变体 / ShaderSet 缓存 | 超限整表清空（D16） | 只丢模板；已建管线仍被保留状态组持有 |
| pass / target 的槽与目标 | 无寿命上限 | 显式 `releasePass` / `releaseRenderTarget` |

**验收**：仅注释变更（零代码改动），全量重编通过；test_vsg 67、test_graphics 151 全绿；
`scripts/gfx_lavapipe_check.sh` → `RESULT: PASS`。头文件与本节是"实现者视角"的同一份契约。

## 15. 像素回读与像素断言（D20 的正面修补，2026-09-11）

**问题**：整套门禁的信号一直是"没有 VUID"。而 VUID 只说明 API 调用合法，**不说明画出了任何
东西** —— 这一节的第一版实现就撞上了：像素断言一上线，立刻报告"离屏目标中心像素是清屏色"，
而那次运行是 0 VUID 的。

**1. `RenderBackend::readColorBuffer` 在 vsg 后端落地**（`VsgRendererTargets.cpp`，契约见
`RenderBackend.hpp` 既有文档）：

- 只支持**离屏 RGBA8** 附件：blit 到一张 `VK_IMAGE_TILING_LINEAR` 的宿主可见图 → 按
  `rowPitch` 收成紧凑 RGBA8 返回。float 附件（RGBA16F/32F）需要转换与语义约定，**诚实报
  不支持**（返回 false + `ContentSkipped` 警告），不猜测、不误打包。
- 同步语义：先 `deviceWaitIdle()`（写该目标的帧必须已完成）；源图在渲染通道结束时处于
  `SHADER_READ_ONLY`，因此屏障先转 `TRANSFER_SRC`、拷完再转回 —— 下一帧仍能采样它。
- 一次性提交复用 vsg 公共设施（`CommandPool` + `Fence` + `submitCommandsToQueue`），
  与独立颜色探针（`vsg_probe`）同一条已验证路径。

**踩到的坑（写下来省下一个人半小时）**：第一版最后一道屏障写
`dstStageMask = TRANSFER|FRAGMENT_SHADER` + `dstAccessMask = HOST_READ`，立刻被 validation
抓到 `VUID-vkCmdPipelineBarrier-dstAccessMask-02816`：`HOST_READ` 只能配
`VK_PIPELINE_STAGE_HOST_BIT`。**这正是 validation 在"布局/屏障"这一层无可替代的价值**。

**2. selftest 新增像素阶段**（`runPixelReadbackPhase`）：把世界空间四边形（带法线、红色
diffuse）画进自建离屏目标并断言

| 断言 | 含义 |
| --- | --- |
| 中心像素"明显偏红"（R ≥ 30 且 R > G+15、R > B+15） | 被光照的表面真的被光栅化进了该目标 |
| 角像素 == 清屏色 (10,20,30)（±1） | 清屏真的到达图像，且四边形没有盖满整张图 |
| 两处 alpha == 255 | 不透明写入 |
| RGBA16F 附件 → `readColorBuffer` 返回 false | 不支持的能力诚实上报，不返回似是而非的数据 |

阶段自带诊断 sink 并把收到的东西打到 stderr：内容桥只向 sink 上报，**没有 sink 的拒绝是
完全静默的**（这本身就是"只靠无 VUID 会漏掉什么"的又一层）。

**3. 顺带查明的旧盲点**：selftest 里原有的辅助几何**占 0 像素** —— 用户程序把顶点直接当裁剪
空间坐标用，而 `makeTriangle(x)` / `makeChannelTriangle()` 的顶点 x 全相同（边长零面积）；用
Phong 通路时三角形又与世界视线共面（正对侧看）。也就是说**过去所有"无 VUID"通过，从未证明
过任何一次光栅化**。像素阶段因此改用世界空间四边形。

**4. 设备自述**：后端在**首次 submit** 时打印一行
`[VsgRenderer] device: <name> (Vulkan x.y.z, driver N, type T)`。为什么要等首帧：vsg 的
`Window` 懒创建 device/swapchain，`initialize()` 期间 `getPhysicalDevice()` 返回空
（第一版写在 initialize 里，条件恒假、静默不打印 —— 又一个"静默"教训）。harness 现在把这行
提出来显示成 `[info] Vulkan device: ...`，并把 `[selftest] FAIL` 当硬失败。

**5. 环境事实（D20 的剩余部分）**：本机只有软件光栅化器 —— `VK_ICD_FILENAMES` 强制或默认，
拿到的都是 `llvmpipe (LLVM 20.1.2) / Vulkan 1.4.318`；`/dev/dri` 不存在（`/dev/dxg` 在，但没有
可用的 GPU Vulkan 驱动）。因此"真机 GPU 冒烟"仍未完成，只是现在**任何一次绿灯都能说出自己是在
什么设备上绿的**。门禁摘要里同时给出 ICD 路径与设备名。

**验证**：test_vsg 67、test_graphics 151 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`，
输出含 `[info] Vulkan device: llvmpipe ...`；像素阶段输出
`centre=(46,8,3) corner=(10,20,30) clear=(10,20,30)`（红四边形 vs 清屏色）；
selftest exit 0、0 VUID；该断言已被证明会响（修好之前它以 0 VUID 的状态报出"中心是清屏色"）。

## 16. 归因实验：为什么"带用户程序的绘制对象"什么都没画（2026-09-11）

**背景**：§15 的像素断言第一次失败时，我同时改了两个变量（程序 + 几何），所以只能确认
"没画"，不能确认"为什么"。这一节把它拆成一次单变量实验，并给出结论 —— **结论不是光栅化
缺陷，而是一个静默的约定陷阱**。

**实验设计**：`vsg_backend_selftest` 新增 `runContentVariantProbe`，同一张 256x144 离屏
目标、同一台相机、同一个四边形形状，每次只改一个输入：程序种类（内置 Phong / 常量色用户
程序 / loc3 属性用户程序）、法线有无、loc3 通道有无。每个变体独立目标 + 独立 pass（避免看
到上一个变体的残影），回读后打印**中心像素、角像素、覆盖像素数、诊断条数**。

**结果**（`covered` = 与清屏色 (10,20,30) 不同的像素数）：

| 变体 | 中心像素 | 覆盖 | 诊断 |
| --- | --- | --- | --- |
| 内置 Phong + 法线 | (46,8,3) | 400 | 0 |
| 内置 Phong + loc3 通道 | (46,8,3) | 400 | 0 |
| 用户程序（常量色）+ 法线 | (10,20,30) | **0** | 0 |
| 用户程序（常量色）+ loc3 通道 | (10,20,30) | **0** | 0 |
| 用户程序（读 loc3）+ 法线 + loc3 | (10,20,30) | **0** | 0 |
| 用户程序，写 `gl_Position.z = 0.5` | (255,51,51) | **5916** | 0 |

**结论**：变量不是法线、不是自定义通道、也不是"用户程序这条路"—— 而是**用户程序把顶点
写到了裁剪空间 z = 0**。

本后端是**反 Z（reverse-Z）**：近平面映射到 NDC 深度 1、远平面到 0、深度缓冲清为 0、
比较算子是 `VK_COMPARE_OP_GREATER`（`RenderStateMapper::mapCompareOp` 里早已写清楚）。
于是 `z = 0` 恰好是**远平面**，与清屏后的深度**相等**，严格的 "greater" 测试把每个片元
全部拒绝 —— 绘制对象消失，**没有 VUID、没有任何诊断**。非反 Z 渲染器里 `z = 0` 是近平面
的直觉，在这里正好相反。

**为什么它值一节**：这不是渲染 bug（行为符合规范），而是**约定没有写在用户会看到的地方**：
"自定义程序"的契约在 `assembleProgramShaderSet` 上说了 vsg 的绑定/推常量约定，却没说深度
方向；作者唯一的反馈是"什么都没有"，且一句话都不会打印。这也解释了 §15 里那些"占 0 像素的
辅助几何"：`makeUserProgram` / `makeAttributeProgram` 全都写 `z = 0`，所以**过去所有带用户
程序的相位其实一直在空跑**。

**落地**：
1. **契约补上深度约定**（`assembleProgramShaderSet` 的 Doxygen）：反 Z、近 1 远 0、清 0、
   `COMPARE_OP_GREATER`，并明确"写 z = 0 = 远平面 → 被拒且静默"。
2. **像素断言覆盖用户程序路径**：`runPixelReadbackPhase` 用同一个四边形 + `makeMidDepthProgram()`
   （z = 0.5）断言 `covered ≥ 1000` 且中心像素 == 程序写出的 (255,51,51)±2。
3. **静默失败通道补口**（`SceneBridge::buildStateGroup`）：`assignArray` 的返回值此前被忽略
   （vsg 按名字 + 元素类型匹配，未命中就意味着"着色器读了管线未启用的属性"——静默退化），
   现在对**着色器确实声明的绑定**未命中时上报；管线构建失败（`bindGraphicsPipeline == nullptr`）
   也从"返回一个没用的 StateGroup 照常记录"改成上报 `CompileFailed`。
4. **变体探针保留**为常驻诊断（日志里 `covered=0` vs `covered=5916` 就是这段约定的活证据）。

**验证**：test_vsg 67、test_graphics 151 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`；
selftest 输出新增 `[selftest] pixels: user program covered=5916/36864 centre=(255,51,51),
diagnostics=0`，且变体表在日志中同时给出两种 z 的对照。

## 17. 像素断言铺开到其余路径（2026-09-11）

§15 只断言了"一张离屏目标 + 内置着色 + 用户程序 + float 诚实报不支持"。其余路径此前
仍然只有"无 VUID"，于是"采样错图""忽略子视口""深度测反了""MRT 只写了第一张"这类错误
都能绿灯通过。这一节把它们逐个变成像素断言（全部在 `vsg_backend_selftest` 内，跑在
llvmpipe 门禁里；harness 会把证据打出来并**要求至少 5 行像素断言存在**，防止断言被悄悄
删掉后仍读作通过）。

| 断言组 | 断言内容 | 它挡住的失败模式 |
| --- | --- | --- |
| 基础（§15） | 中心 = 被光照的四边形、角 = 清屏色、alpha=255 | 通道没进目标 / 清屏没生效 / 光栅化没发生 |
| 用户程序（§16） | `covered ≥ 1000` 且中心 == 程序写出的 (255,51,51) | 用户程序路径整条失效（反 Z 陷阱即此类） |
| **PiP blit** | 子矩形中心 = 生产者画的内容（红）、矩形内侧边缘 = 生产者自己的清屏色（绿）、矩形外 = 消费者的清屏色、**变化的像素数恰好等于矩形面积** | 采样错附件 / 采样成常量 / 忽略 `setViewport`（溢出或画错位置） |
| **Deferred 全屏程序** | 整张目标被程序输出 (140,153,166) 填满（覆盖 == 全目标） | 程序没跑 / 源没绑上 / 只覆盖了一部分 |
| **深度顺序** | 近红远蓝两个四边形**按画家算法会画错的顺序**提交：开深度测试时**蓝色像素数必须为 0**、中心必须是红的 | 深度测试失效 / 方向反了（反 Z 映射错）/ 深度写入错 |
| **深度模式权威** | 同一份命令 + `DepthMode::Disabled` → 中心必须变蓝 | pass 的深度策略没能到管线（设备无关测试只能断言状态对象） |
| **MRT** | 附件 0 必须收到几何（红）；附件 1 的报告是诊断 | 主输出丢失 |

**顺带量到的一条规则（此前只在注释里，没有任何东西验证过）**：MRT 目标的**附件 0**
清成 pass 的 `clear()` 颜色，而**附件 ≥ 1 一律清成透明黑** ——
`VsgRendererTargets` 的注释写着"empty regions stay black until a fragment writes them"，
探针第一次跑就量到附件 1 是**整张 (0,0,0)**（`covered=9216/9216` 相对清屏色）。这是**有意
的设计**，所以没有改；但对采样额外附件的消费者（例如 deferred 光照读 G-buffer 的法线）是
可见的：未被几何覆盖的像素拿到的是零法线而不是清屏色。已登记（D31），把选择留给需要它的
人：要么把这条规则写进消费者契约，要么统一清成同一颜色（后者会改变 deferred 在未覆盖
区域的输出，需要先审消费者）。

**验证**：test_vsg 67、test_graphics 151 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`，
输出逐条列出像素证据（基础 / 用户程序 / PiP / deferred / 深度 / MRT），并要求至少 5 行
`[selftest] pixels:` 行存在。

## 18. 深度回读与直接深度断言（顺带抓到两个真缺陷，2026-09-11）

**出发点**：§17 的"深度顺序"是用**颜色**间接推断的（近处红、远处蓝，谁在中心谁赢）。这一
节把它变成**直接测量**：`readDepthBuffer` 落地，断言读出的深度值本身。

**1. `VsgRenderer::readDepthBuffer`**：只读**无歧义**的深度格式 —— `D32_SFLOAT`（texel
就是值）与 `D16_UNORM`（除以 65535）；打包的 `D24_UNORM_S8_UINT` **诚实报不支持**
（需要猜实现把深度放在 32 位里的哪 24 位，不猜）。深度的最终布局随 pass 而定
（depth test/write 目标是 attachment、被提升的是 sampleable），所以拷贝完**转回
`render_pass->attachments.back().finalLayout`**，下一帧照常渲染/采样。

**2. 断言**（`runDepthOrderPixelPhase` 扩展）：同一个四边形放在 4 单位与 6 单位处各渲一次，
直接读深度值断言

```
[selftest] depth: near=0.0249 > far=0.0166 > cleared corner=0.0000 (reverse-Z ordering),
                 Disabled centre=0.0000 (no depth written); packed D24 honestly unsupported
```

- 近的 **大于** 远的（比值 0.0249/0.0166 = 1.5 == 6/4，即反 Z 的 `z ≈ near/d`）——这是反 Z
  方向的**定量**证据；
- 未覆盖的角落是**清成 0 的远平面**；
- `DepthMode::Disabled` 时中心仍是 0 → 深度**写入**侧也真的被关掉（不只是测试侧）；
- 打包 D24 返回 false（诚实上报）。
- **修正一个我自己的错期望**：第一版断言写成"近处应接近 1"。实际算出 0.0249 是**正确**的
  ——相机 near=0.1 / far=1000，反 Z 下 4 单位处的深度就是 ≈ near/d。断言随即改为**自证式**
  （同一个四边形更远 → 更小），不再依赖魔法数字。

**3. 顺带抓到的缺陷 A：离屏目标表按裸指针索引且条目不自持 → 同地址新目标继承死目标的附件**

断言第一次跑就报"构造的第二个 D32 目标读回了**打包 D24**"。根因：`impl->targets` 是
`std::map<RenderTarget*, Target>`，而 `Target` 条目**不持有**那个 `RenderTarget`。前一个相位
的局部目标析构后条目还在，于是**新目标被分配到同一地址**时直接继承旧条目的 GPU 状态 ——
包括深度格式（这就是 D24 的来源）、尺寸与颜色格式。这正是 §8.1/§12 早就为缓存定下的规则
（"保留型缓存的条目必须自持它索引的对象"）在**目标表**上漏掉的一处。

修法照搬既有骨架：`Target::owner`（`intrusive_ptr<RenderTarget>`）+ 所有表访问改走
`Impl::entryFor()`（首次触碰即自持，地址在条目存活期内不可能被复用）；并在 `submitFrame()`
里回收"宿主丢弃但没宣告"的目标（`useCount() <= 1` → 走 `releaseRenderTarget` 的完整拆解）——
与几何/材质缓存的"放弃即回收"同一规则。

**4. 顺带抓到的缺陷 B：深度图没建 `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`**

第一版 `readDepthBuffer` 功能上是对的（数值正确），但**只在开了 validation 的门禁里**报出
`VUID-vkCmdCopyImageToBuffer-srcImage-00186` 与 `VUID-VkImageMemoryBarrier-oldLayout-01212`：
深度图的 usage 只有 `DEPTH_STENCIL_ATTACHMENT | SAMPLED`，连转到 `TRANSFER_SRC_OPTIMAL` 都不
允许。修法：加上 `TRANSFER_SRC`（颜色图早已有，深度图漏了）。**教训**：我本地跑 selftest 时
没开 `VINE_VSG_DEBUG_LAYER=1`，于是"功能对 + 0 VUID"的假象只有在 harness 里才被戳破 —— 本地
验证必须用与门禁相同的环境。

**验证**：test_vsg 67、test_graphics 151 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`
（0 VUID）；harness 现在分别要求像素 ≥4 行、深度 ≥1 行、MRT ≥2 行证据，缺一组即失败。

## 19. SceneBridge 拆分：几何 / 管线 / 同步三个 TU + 内部头（2026-09-11）

**问题**：§13 收尾时登记的最后一块 —— `SceneBridge.cpp` 1571 行，其中 `syncRenderCommands`
一个函数占 **256 行**，匿名命名空间里塞着 22 个 helper（顶点数据打包、法线推导、着色器编译、
ShaderSet 组装、状态组搭建、变体哈希），彼此无共享，却共享同一个 TU：改一处顶点格式要重编
整文件，helper 的调用关系只能靠读。

**布局**（拆分后；与 §13 同一条规则：每个 TU 只依赖它真正用到的头）：

| 文件 | 职责 | 行数 |
| --- | --- | --- |
| `src/SceneBridge.cpp` | 会话态与同步：setter/访问器、诊断路由、退役环、缓存清空/失效、`syncRenderCommands`（逐命令 → 节点替换 + 材质/几何/状态缓存决策） | 512 |
| `src/SceneBridgeGeometry.cpp` | 几何数据：`buildGeometryData` + 顶点数据打包 helper（`makeWhiteColors` / `makeNormals` / `makeIndexedNormals` / `makeTypedVertexData` / `XyzUnpack` / `unpackXyz`） | 473 |
| `src/SceneBridgePipeline.cpp` | 管线侧：`getProgramShaderSet`（编译 + 变体缓存查询）、`buildStateGroup`（属性绑定 + 深度/混合状态）+ 对应 helper（`compileProgramStages` / `assembleProgramShaderSet` / `hashStateVariant` / `boundArraysOf` / `customAttributeName` / `stageFlag` / `formatForComponents` / `sampleVertexData` / `shaderCompiler`） | 620 |
| `src/SceneBridgeInternals.hpp` | 唯一被两个 TU 共享的保留态：`struct SceneBridge::VariantEntry`（变体键 → ShaderSet/管线）。**内部头，不安装** | 44 |

**两条规则**：

1. **匿名命名空间的 helper 只留在它唯一的使用者所在 TU**（拆分时逐个查过调用点：22 个 helper
   里除 `VariantEntry` 外全是单用户），因此每个 TU 的 helper 都还能保持 `static`/匿名可见性，
   不升级成公开接口。
2. **`Item` 留在同步 TU**（它是 `syncRenderCommands` 的保留态），只把两个 TU 都要看的
   `VariantEntry` 提到 `SceneBridgeInternals.hpp` —— 与 §13 的 `VsgRendererImpl.hpp` 同一手法。

**纯搬运**（可核对）：`git show HEAD:…/SceneBridge.cpp` 与新四个文件的**非空行多重集**只差
include / 命名空间 / 新内部头前言；**没有一行代码被改写或丢失**。include 块按仓库顺序重建
（自带头（保留文件 BOM）→ 内部头 → 标准库 → vsg → vine → 同目录引号），并按"符号驱动 + 编译
验证"收窄：同步 TU 净减 1 个头（它确实用到大部分 vsg 类型，试删的 8 个里有 7 个编译不过又加
回来），几何 TU 净减 20、管线 TU 净减 12（各自砍掉一半以上）。判据是**编译通过**，且必须用
插件 / selftest / test_vsg **三份编译命令**同时通过 —— 这三个目标各有自己的 include 上下文，
只验一个会漏（第一版只验一份，`<vsg/io/Options.h>` 的完整类型需求就是那样漏过去的）。

**验收**：所有目标零警告构建；test_vsg 67、test_graphics 151 全绿；`gfx_lavapipe_check.sh`
→ `RESULT: PASS`（0 VUID，六组像素/深度/MRT 证据齐全）。两个直接编译插件源码的目标
（`test_vsg`、`vsg_backend_selftest`）的源列表已同步 —— MODULE 目标不能链接，漏一个就是链接
期或静默缺失。

**仍未做**：`syncRenderCommands` 256 行仍是单函数最长（内部可按"缓存决策 / 节点替换"再切）；
D16（变体缓存超限整表清空）、D27（跨 pass 命令缓存）、D28（`VkPipelineCache` 被 vsg 传
`VK_NULL_HANDLE` 挡住）与真机 GPU 冒烟仍待在册事项里。

## 20. 缓存收口：一套骨架、四种缓存（D16 / D34，2026-09-11）

**出发点**：§12 给材质缓存定了骨架（`OwnedCache.hpp`：条目自持键对象 + `abandoned()` +
FIFO `trimToCapacity`），但 `SceneBridge` 的四个缓存各自一套：几何缓存手写自持与手写放弃
判据；`program_stages_` 手写自持但**只增不减**；`program_shader_sets_` 与 `variant_cache_`
用裸键且**超限整表清空**。三种"放弃 / 修剪"语义并存，谁属于哪种只能读代码。

**骨架的两条规则**（现在只有一个定义）：

1. **自持**：条目按被服务对象的地址定键，就必须持有它 —— 否则对象销毁后同地址新对象会通过
   相等性检查（D13 的原始形态）。`abandoned()` = "除本缓存外无人引用"（`keyReleased()` 是
   唯一的定义处）。持有**一对键**的条目（`OwnedPairCacheEntry`，program + material）要求两个
   键都被释放才算放弃；一个键都不持有的条目永不放弃（默认资源）。
2. **FIFO 修剪**：`trimToCapacity(map, n)` 逐最旧插入序（`InsertionClock`），跳过 null 键
   默认条目；`eraseAbandoned(map)` 回收"app 已放手"的条目。**两半都要**：自持解决地址复用，
   回收解决自持变泄漏。

**四种缓存的最终形态**：

| 缓存 | 键 | 自持 | 容量 | 放弃 |
| --- | --- | --- | --- | --- |
| `cache_`（几何 → 保留节点） | 几何地址 | 是 | **无上限**（600 帧复用窗是它的策略，不是容量） | `abandoned()` → 立即逐出，否则等窗 |
| `program_stages_`（program → SPIR-V） | program 地址 | 是 | 64（FIFO） | `eraseAbandoned` |
| `program_shader_sets_`（(program, layout) → ShaderSet） | 内容哈希 | 是 | 64（FIFO） | `eraseAbandoned` |
| `variant_cache_`（(program, material, state, layout) → 可复用绑定命令） | 内容哈希 | **两个键都持** | 256（FIFO） | `eraseAbandoned`（两个键都放手） |

**修掉的两个真缺陷**：

- **D34（身份靠地址、但不持地址）**：`Item::material` / `Item::program` 是裸指针，
  `program_shader_sets_` / `variant_cache_` 的条目也只存裸键。对象被 app 释放并销毁后，同地址
  新对象（同 revision、同变量身份）会**通过相等性检查** → 复用死对象的管线、descriptor、材质
  颜色，静默错色 / 错 shader。修法：`Item` 自持它比较的 program 与 material；两个哈希键缓存
  分别用 `OwnedCacheEntry` / `OwnedPairCacheEntry` 自持键对象。
- **D16（超限整表清空）**：`program_shader_sets_.size() > 64` 就把两个 program 缓存清空 ——
  连当前场景正在绘制的程序一起丢，随后逐个重编译。改为与几何 / 材质同一套 FIFO：最旧的先走，
  最新（正在画）的留下。

**"放弃"在多缓存共享一个对象时的含义**（写进骨架文档，勿再误读）：一个 program 可能被阶段
缓存、ShaderSet 缓存与某个变体模板同时持有，每个条目看到的 `useCount()` 都包含别人的份额，
所以 `abandoned()` 只有在**其它缓存也放手**之后才成立 —— `eraseAbandoned()` 回收的是这条链的
**尾部**，链能有多长由 FIFO 上限决定（不会无限增长，但不是"app 一放手就立刻回收"）。几何缓存
不受此影响（几何只有这一个缓存持它，判据与 §8.1 一致）。

**落地细节**：`OwnedCache.hpp` 从 `src/` 挪到插件的 `include/vine/vsg/`（`cache_` 等成员类型
要出现在 `SceneBridge.hpp` 里；该 include 目录只在构建树内可见，不进 SDK）；四个缓存类型起名
（`GeometryCacheEntry` / `ProgramStagesEntry` / `ProgramShaderSetEntry` / `VariantCacheEntry`）；
插入改 `insert_or_assign`（新条目不再可默认构造）；`trimToCapacity` 跳过 null 键默认条目的逻辑
用 `if constexpr (std::is_pointer_v<key_type>)` 收窄（哈希键缓存没有这种条目）；每帧
`releaseAbandonedCaches()` 在 `syncRenderCommands` 尾部（几何缓存的回收仍在它的缺席循环里，
因为它还要算复用窗）。

**验收**：新增 `tests/test_vsg/SceneBridgeCacheOwnershipTest.cpp` 四个测试 —— 桥持有它比较的
program / material（地址不被复用）；FIFO 替掉整表清空（70 个程序后复用最新 10 个**不重编译**、
最旧的必然重编）；**变体模板在 program 的阶段 / ShaderSet 条目已被 FIFO 逐出后仍持有该
program**（旧实现在这里会释放 → 地址可被复用）。test_vsg 71（67 + 4）、test_graphics 151、
`gfx_lavapipe_check.sh` → `RESULT: PASS`（0 VUID，六组证据齐全）。

**仍未做**：`shared_objects_`（vsg 内容去重表）仍是"只增 + 槽释放时清空"，没有容量上界；
D27（跨 pass 命令缓存）、D28（`VkPipelineCache`）与真机 GPU 冒烟仍待在册。

## 21. 深度 LOAD 与共享深度的语义断言（2026-09-11）

**出发点**：pass 协议里有两条语义此前**只被"无 VUID + 不崩"覆盖**，没有任何回读断言：
`clearDepth=false`（深度的 depth-LOAD pass，深度跨帧保留）与 `RenderTarget::shareDepth`
（借用者的深度测试真的对着出借方的深度）。§5 那个"借深度 + clearDepth=false 每帧重建"的
缺陷、以及共享深度的生命周期（§5 的 tombstone）都已被测到，但**语义**从没被量过。

**1. `runDepthLoadPixelPhase` —— 深度真的从上一帧 LOAD 出来了**

先弄清模型：`clearDepth` 是**目标**的 pass 属性（`t.clear_seen && !t.clear_depth` 决定
LOAD），所以"同一目标上两个 pass 一个要求 CLEAR、一个要求 LOAD"是自相矛盾的用法（见下面
"踩到的坑"）。支持的用法是**一个 pass + 一个目标**，深度**跨帧**保留：

- 阶段 1：只画近四边形（z=1，反 Z 深度 ≈0.0249）。首帧走"播种用 depth-CLEAR pass"
  （LOAD pass 不能从 UNDEFINED 开始），之后走稳态 depth-LOAD pass；无论哪条路径，缓冲最终
  持有近面的深度。
- 阶段 2：内容换成远四边形（z=-1，≈0.0166）盖住同一批像素。深度被 LOAD 出来时，远片元
  **必须输掉测试**：不允许出现蓝色像素、中心必须仍是该 pass 自己的清屏色、深度值必须
  **一字不变**。若被误清屏，远四边形就会赢（蓝像素 + 深度变小到 0.0166）。

证据行：`[selftest] depth load: far quad over the same pixels left 0 blue pixel(s), centre
stayed the LOAD pass' clear colour, depth unchanged at 0.0249 (loaded from the previous
frame, not cleared); 1 build(s) over 4 frames` —— 顺带断言 **LOAD 目标只构建一次**（首帧播种
pass 与稳态 LOAD pass 共用一个目标条目，否则又是 §5 那种每帧重建）。

**踩到的坑（写下来避免重犯）**：第一版把场景写成"pass A（clearDepth=true）画近 + pass B
（clearDepth=false）画远、同一个目标"。它**必然失败**，而且失败得很有教育意义：目标只有
一个 depth load op，"最后一次 clear 请求"决定它 → 远 pass 的 `clearDepth=false` 把目标切到
LOAD，但 A 的清屏请求与它互斥，实测结果是 B 执行时深度仍被清 → 远四边形赢（256 蓝像素、
深度从 0.0249 变 0.0166）。**这不是后端缺陷，是我把断言建在了模型外的用法上**；也正因为
断言足够具体（像素 + 深度值都报出来），一眼就能看出是"深度被清了"。

**2. `runSharedDepthPixelPhase` —— 借用来的深度真的被试过**

出借方（`depthPromotion(false)` + D32）先画近四边形写自己的深度（order 0 < 1），借用者
（`shareDepth`）只清颜色（`clearDepth=false`）后绘制自己的颜色：

- **拒绝**：借用者只画远四边形 → 必须在出借方的深度上输掉 → 借用目标**一个像素都不变**。
  （深度没共享或被清掉时它会通过并涂蓝。）
- **接受**：借用者再画一个更近的四边形（z=1.6，3.4 单位）→ 必须赢 → 中心变绿。少了这一半，
  "全部被拒绝"也能通过上一半的断言，等于什么都没证明。
- 顺带断言**出借方自己的深度可读且非清屏值**（`readDepthBuffer(lender)`；共享深度不应让
  出借方的图像变得不可读）。

证据行：`[selftest] shared depth pixels: behind the borrowed depth 0 pixel(s) drawn, in
front it covered the centre (5,41,10); lender depth 0.0293`。

**判据力（做过反证）**：把出借方改成 `DepthMode::Disabled`（借出来的深度就只剩清屏值），
该阶段立刻报 `256 pixel(s) of the borrowing target changed (256 blue)`；深度 LOAD 阶段的那
次"模型外用法"失败同样报出 256 蓝像素 + 深度变化。两条断言都能被破坏性改动点红，不是
"永远绿"的装饰。

**harness**：`require_evidence` 现在要求 `pixels:` ≥4、`depth:` ≥1、**`depth load:` ≥1**、
**`shared depth pixels:` ≥1**、`MRT ` ≥2 —— 断言被删掉而阶段照跑必须读作失败。

**验收**：test_vsg 71、test_graphics 151 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`
（0 VUID），新增两行证据随门禁打印。

**仍未做**：真机 GPU 冒烟（本机只有 llvmpipe/lavapipe）；"同一目标上多 pass 的深度策略"
仍是**每目标一个 load op** —— 多 pass 想各自声明 CLEAR/LOAD 需要把 render pass 从目标粒度
下沉到 pass 粒度（登记为未做项，不是缺陷：当前模型里"最后一次 clear 请求"就是该目标的
策略）。

## 22. 同目标多 pass 的深度策略：从"最后一次请求赢"到每 pass 自清（D35，2026-09-11）

> **已被 §28 第 3/4 步取代（2026-09-11）**：render pass 下沉到 pass 粒度后，每个 pass 自带一对
> 附件 load-op，本节描述的 `depth_policy_mixed`（sticky）+ `ClearAttachments` 回退 + "颜色清屏
> 跟随最后一次请求"整体删除；本节保留为设计推导的历史记录，**不要照它实现新功能**。

**§21 结尾登记的那个模型边界，是缺陷，不是限制。** §21 只证明了"同一目标上两个 pass 一个
要 CLEAR、一个要 LOAD"会失败（远 pass 的深度仍被清），当时归因为"模型外用法"。把它当用法
问题放过去是不对的：**不透明 pass 每帧清深度 + 半透明 pass 保留深度**是真实多 pass 管线的
标准写法，而旧实现里 `clearDepth` 是**目标**属性 → 目标只烧一个 depth load-op → **最后一次
clear 请求赢**。于是第二个 pass 的 `clearDepth=false` 会把第一个 pass 的每帧清深度**吞掉**：

- 目标恒为 LOAD → 上一帧的深度留在缓冲里；
- 移开的物体/换掉的内容**仍然遮挡**（ghosting），下次绘制被"死几何"的深度拒绝，静默错画。

**修法**（保持"一个目标一个 render pass"，把清除下沉到 pass）：

1. `clearDepth` 变成**双重身份**：`PassRequest::clear_depth` 是 pass 作用域属性（和
   `presenting` 同一机制，`beginPass`/`endPass` 自动清），`Target::clear_depth` 仍记最后请求。
2. **冲突检测**：`clear()` 里若同一目标出现与上次**不同**的 `clearDepth` → 置
   `Target::depth_policy_mixed`（sticky，避免在两种策略间来回重建图）。
3. `Target::wantsDepthLoad()`（唯一判据，`render()` 的重建谓词与 `buildOffscreenTarget` 共用）：
   `clear_seen && (!clear_depth || depth_policy_mixed) && depth_source == nullptr` ——
   混合目标改用 depth-LOAD pass。
4. **要清的人自己清**：混合目标里"请求了 clear"的 pass 在自己的 view 里、自己的绘制之前插一条
   `vsg::ClearAttachments`（深度面，值为 `Target::depth_clear_value`，与 render pass 本来会写的
   值同一个来源）。它放在**独立于 bridge root 的 group** 里 —— bridge 会按命令流重建 root 的
   子节点，注入 root 就会被抹掉。位置在 view 内 → 就在 render pass 实例里 → `vkCmdClearAttachments`
   合法性满足（验证层 0 VUID 已证）。
5. 常量与顺序：`depth_clear_value` 由离屏构建时记录（colour 目标 0.0 = 反 Z 远平面 ✓，深度专用
   目标 1.0），清除命令写同一个值；同目标内 view 按 order 升序 → 不透明 pass 先清先画、半透明
   pass 后测 ✓。
6. **已知残留（1 帧收敛）**：冲突是在**第二个请求到达时**才被发现的，所以"第一次混用"的那一帧
   仍按旧策略执行（本例是半透明 pass 那帧清掉了深度）；从下一帧起两种请求都被满足。粘性标志
   保证只发生一次，不震荡。域外借用深度的目标不受影响（`depth_source != nullptr` 时既不用 LOAD
   pass 也不自清 —— 它借的是别人的深度，策略归出借方，H4 不变）。

**验证**：`runMixedDepthPolicyPhase` —— 不透明 pass（order 0，`clearDepth=true`）画近面遮挡，
半透明 pass（order 1，`clearDepth=false`）画远面：
- 阶段 1（遮挡物在）：远面**必须被拒绝**（0 蓝像素）→ 证明保留的深度确实还在被测试；
- 阶段 2（遮挡物移除）：远面**必须出现**（256 蓝像素）→ 证明不透明 pass 的每帧清深度没被吞掉；
- 目标构建**恰好 2 次**（初次 + 切到 LOAD pass），阶段 2 增加 0 次 → 不是每帧重建。

**判据力（反证）**：把自清那一行改成 `false`（等价于修前行为）→ 立刻报
`the far quad is still invisible after the occluder was removed … the dead occluder still occludes
(ghosting)`。harness 追加 `mixed depth:` ≥1 行证据要求。

**验收**：test_vsg 71、test_graphics 151 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`（0 VUID，
三行新证据随门禁打印）；dist 冒烟 25s 存活。

## 23. 深度借用的可用性校验（D36，2026-09-11）

**出发点**：`RenderTarget::shareDepth` 是把**源目标的深度图像**当作本目标的深度附件用，因此只有在
那张图"原样可用"时才能成立。以前只检查了一件事（"源在本帧里还没渲染过"），另两种不可用情形**完全
静默**。都先用 selftest 探针量到，再修：

| 不可用情形 | 之前的实际行为（探针量到的） |
| --- | --- |
| **尺寸不同**（半分辨率 composite 借全分辨率深度） | 0 条诊断；`vkCreateFramebuffer` 建出**非法帧缓冲**（`VUID-VkFramebufferCreateInfo-pAttachments-00880`：所有附件必须与帧缓冲同尺寸），之后是未定义渲染（llvmpipe 上"看起来正常"，所以只有验证层能戳破） |
| **源把深度提升成了可采样纹理**（`setDepthPromotion(true)`） | 0 条诊断；源的深度留在 `SHADER_READ_ONLY_OPTIMAL`，而 render pass 的附件声明是 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` → **每帧** `VUID-VkImageMemoryBarrier-oldLayout-01197`（借深度用的屏障是按"附件布局"写的），并且**什么也没画**（读回是清屏色） |

**修法**（`buildOffscreenTarget` 里一次三向校验，分成**两类**命运）：

1. **源本帧有没有深度图**（瞬时）：不记墓碑 —— 本帧用自己的深度渲染，**源一出现就重试借**
  （`render()` 的重建谓词新增一项：请求了借、没烧进去、源现在有深度图 → 重建一次）；
  **每段瞬时问题只报一次**（`depth_borrow_pending_reported`，借成功即重新武装）。
2. **尺寸必须一致 / 源不能把深度提升成可采样**（持久，是 setup 的性质）：报一次（`unusable_depth_source`
  墓碑）+ 回落自有深度，直到宿主把借用指向另一个源。

3. 新增构建期标志 `Target::depth_sampleable`，在**选用 sampleable pass 且 `depthPromotion()` 为真**时置位 ——
   记构建期事实，而不是事后读 `depthPromotion()`（那可能已经变了）。

**一个自己踩的坑（写下来）**：第一版把**三类**原因都当成持久，于是 app 的预热阶段
（光照 pass 早于 gbuffer 建好 → "源还没有深度图"）把这个借永久关掉 —— 表面还能跑，但
composite 再也不会借用 gbuffer 的深度（半透明内容就测不到不透明深度）。像素级断言
（`runDepthBorrowValidationPhase` 的瞬时用例："借未生效时远四边形可见 / 借生效后必须被拒绝"）
当场把这次的错误行为变成红。**结论：瞬时与持久必须分开，且要有"重试"路径。**

**契约（写进注释与记忆）**：借深度要求 ① 源与借方的**尺寸相同**、② 源**不**把深度提升为可采样
（`setDepthPromotion(false)`，内置 deferred 管线正是这么做的）；③ 源必须先于借方在本帧建好
（否则本帧用自己的深度 + 下一帧重试）。

**验证**：`runDepthBorrowValidationPhase` —— 三个用例各驱动多帧：
- 半分辨率借方（256×144 源 → 128×72 借方）；
- 借"深度已提升"的源（同尺寸）；
- **瞬时**：借方的 pass 排在源的 pass 之前（首帧源还没建好）。
前两个断言：**恰好 1 条**诊断（消息里含 `shared-depth`）+ 借方**像素正确**（近四边形必须赢、
不得出现远四边形的蓝 → 证明回落的自有深度真的在测）。瞬时用例断言：**恰好 1 条**诊断 +
**借在源出现后被重试并生效**（借生效后远四边形必须被借来的近深度拒绝 → 不得有蓝）。
门禁 0 VUID（提升用例的 01197 全部消失）。harness 追加 `depth borrow:` ≥1 行证据要求。证据行：

```
[selftest] depth borrow: mismatched extent (1 report(s)) and depth-promoted source (1 report(s)) both
fell back to the target's own depth and still drew the near quad; a borrower built before its source
(1 report(s)) retried and used the borrowed depth
```

**判据力（反证）**：把瞬时分支也改成写墓碑（即上一版的行为）→ 瞬时用例立刻报
`256 pixel(s) of the late-source borrower are still blue after 5 frames — the borrow was refused
once and never retried`。

**验收**：test_vsg 71、test_graphics 156 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`（0 VUID）；
dist 冒烟存活。

## 24. `DepthMode::TestOnly` 的语义断言（2026-09-11）

**出发点**：两条内置半透明 pass（forward 与 deferred 链的 `forward_transparent`）都用
`DepthMode::TestOnly`，但 selftest 从来只是**驱动**它（pass 协议阶段跑一下、共享深度阶段用一下），
没有量过它两半语义中的任何一半：**测试开着**、**写入关着**。

**场景**（`runDepthTestOnlyPixelPhase`，一个 D32 目标）：

1. 阶段 1：不透明 pass（`TestAndWrite` + `clear(colour, true)`）画近面（红）→ 读回深度作为**基准**
   （不假设魔法值）。
2. 阶段 2：加一个**不调用 clear** 的半透明 pass（`TestOnly`，与内置管线同形），按顺序画
   近（z=1.6，绿）→ 中（z=1.2，蓝，仍在不透明面**之前**）→ 远（z=-1，灰，在不透明面**之后**）。

**断言**（两条，各自封死一种错误）：

- **中心必须被"中间那个"（蓝）赢**：写深度 → 更近的绿色（先画）会遮挡它；不测试 → 最后画的灰色
  （在不透明面之后）会盖住它。一条断言同时覆盖两半。
- **深度读回必须仍是不透明 pass 的值**：半透明片元不得写深度（直接读深度，而不是从颜色反推）。

**判据力（反证，实测三色各一次）**：`TestOnly` → 中心 `(5,10,46)`（蓝，符合预期）；改成
`TestAndWrite` → `(5,41,10)`（绿，被更近的先画片元遮挡）→ 报红；改成 `Disabled` → `(43,43,43)`
（灰，最后画的穿透了）→ 报红。证据行：

```
[selftest] depth testonly: middle translucent quad won the centre (5,10,46), depth still the opaque
pass' 0.0249 (tested, not written)
```

harness 追加 `depth testonly:` ≥1 行证据要求。

**验收**：test_vsg 71、test_graphics 157 全绿；`gfx_lavapipe_check.sh` → `RESULT: PASS`（0 VUID）。

## 25. 深度借用的两个隐性缺陷：源重建后帧缓冲"冻结" + 依赖图缺边（D38，2026-09-11）

**背景**：`RenderTarget::shareDepth(src)` 让本目标的帧缓冲**直接用**源的深度图像
当深度附件（延迟渲染里 composite 复用 gbuffer 的深度：不透明几何写、前向内容测
试）。§23 只保证了**构建那一刻**的可用性（源有没有深度图 / 同尺寸 / 未被提升成
可采样）。这一节补齐"之后"。

### 25.1 缺陷 A：借用方只在自己重建时重新校验，看不见"源重建"

`buildOffscreenTarget` 把源的 `depth_view` 烧进帧缓冲附件。源的**同尺寸重建**在
真实 app 里是常规事件：

* 混合深度策略收敛（§22：第二次 `clear()` 请求与第一次不同 → `depth_policy_mixed`
  → `wantsDepthLoad()` 翻面 → 目标重建）；
* 尺寸变化（`Pipeline::resize`）；
* 其它触发重建的改动。

重建会**换掉源的深度图像**。`render()` 的重建谓词只看借用方自己的尺寸与
`depth_load`（外加 §23 的 `borrow_pending`：借**从未成功**才重试），所以借用方不
重建、帧缓冲继续指旧图 —— 旧图**没人再写**，借来的深度**静默冻结**（画面里"遮挡
关系停在那一刻"），旧图还被帧缓冲握着，白白留着显存。无 VUID、无诊断。

### 25.2 缺陷 B：`reconcileOffscreenOrder` 的依赖边不含"深度借用"

`reconcileOffscreenOrder()` 用稳定 Kahn 拓扑排序把离屏 graph 排成"消费者在生产者
之后"，但**边只来自采样属性**（PiP 的 `screen_slots`、全屏 program 的
`program_slots` 的 `source_target`）。深度借用**不是采样**，没有边，于是顺序完全
来自"`buildOffscreenTarget` 末尾把 graph 追加到 `command_graph->children` 末尾"这
个副作用。源在借用方之后重建 → 源被追加到末尾 → 借用方的 render graph 排在**源
之前** → 整帧用上一帧的深度（1 帧滞后，静默；布局相同，无 VUID）。

两个缺陷同源（"源被重建"），且都只在**同尺寸重建**下才可达：不同尺寸会被 §23 的
校验拦下（回落 + 报告）。

### 25.3 修法

1. `Target::depth_source_view`：记住**烧进帧缓冲的那张源视图**。
2. `render()` 增加 `borrow_stale`：`depth_source != nullptr`、未被墓碑
   （`unusable_depth_source`，防诊断/重建循环）且源的当前 `depth_view` ≠ 记录值时
   重建 → 重跑 §23 的校验：要么重新烧上新图，要么按持久故障回落 + 报告。
3. `reconcileOffscreenOrder()` 把 `target.depth_source` 也作为一条依赖边
   `add_source()`，源必然排在借用方之前，barrier 插在两者之间。

### 25.4 判据（selftest `runDepthShareOrderPhase`）

借用方用 `DepthMode::TestOnly`（**只测不写**，否则它自己的深度写会污染共享图像、
让下一次比较相等而被 `GREATER` 拒掉）；场景安排使"源被同尺寸重建"可控：

| 帧 | 源 | 借用方 |
|---|---|---|
| 0–1 | 清深度 + 画远面（z=-1 → 0.0166） | 探针 z=0（0.02），在远面**之前** → 应画上 |
| 2 | 第二个 pass 出现（`clearDepth=false`）→ **混合** → **同尺寸重建**（换深度图） | 仍是旧图 → 探针画上（对照组） |
| 3– | 再画近面（z=1 → 0.0249），在探针之前 | 探针必须**帧帧被拒** |

实测模式 `AAA---`（A = 探针被画）：第 2 帧是"借用确实生效"的对照组（否则后面
的"被拒"什么也证明不了），第 3 帧起帧帧被拒。

**反证（判据力）**：

* 停掉 `borrow_stale` → `AAAAAA`，第 3 帧起一直画上（旧图冻结）→ 断言红
  （诊断文案按模式区分"只差 1 帧 = 记录顺序"与"一直不拒 = 旧图"）。
* 停掉 25.3 的依赖边 → **仍绿**：借用方的 `borrow_stale` 重建会把它的 graph 重新
  追加到末尾，等价地修好了当帧顺序。该边在当前实现下**不可单独观测**，作为"顺序
  函数必须知道这条依赖"的不变量保留（记录在案，避免后人误以为它是死代码）。

证据行：

```
[selftest] depth share order: borrower followed the source's own frame and image
(AAA--- over 6 frames; A = probe drawn)
```

harness 追加 `depth share order:` ≥1 行证据要求（这次编辑一开始把两行 shell 粘
成一行导致该 grep 与 `require_evidence` 都没执行、闸门仍绿 —— 已修正；教训：改
闸门脚本后必须 `bash -n` + 确认证据行真的打印出来，而不是只看 `RESULT: PASS`）。

**验收**：test_vsg 71、test_graphics 157 全绿；`gfx_lavapipe_check.sh` → `RESULT:
PASS`（0 VUID，含新证据行）；app 冒烟（`dist` 换新插件）：exit 124、设备行在手、
**全程只有 1 条预热借用警告**（`composite` 借 `gbuffer`，随后静默成功）、离屏
目标只构建 3 次（无重建循环），无 `unresolved` 警告。

## 26. 目标描述在构建后改变必须重建：构建指纹（D39，2026-09-11）

`buildOffscreenTarget` 把目标的**描述**烧进它创建的东西：附件数量/格式 → 图像 +
render pass + framebuffer；深度格式 → 深度图 + render pass；深度提升标志 →
render pass 的 finalLayout + `depth_sampleable`。而 `render()` 的重建谓词只逐项列了
**尺寸 / 深度策略 / 借用待办 / 借用过期**，于是**没被列到的属性一律静默失效**：

* **中途 `attachColor()`**：帧缓冲仍是旧的附件数 → 新附件**永远不存在**；
  `readColorBuffer(t, 1)` 报 "attachment 1 is out of range" —— 这句听着像"不支持"的
  诊断，实际是后端在承认自己把请求丢了。
* **中途 `setDepthPromotion(true)`**：**一次重建都不会发生**（反证实测：变更帧重建
  0 次），于是 (a) 深度**从未**被提升成可采样；(b) 更要命的是 §23 的借用校验读的是
  **构建时烧下的** `depth_sampleable`，所以它继续认为这个深度"可借" → 借用方**继续
  把一个源 pass 已按采样布局收尾的深度当附件挂上** —— 正是 §23 花力气拦下的那种错
  误用法，从后门静默回来了。

**修法**：`Target::BuildKey`（颜色附件数 + 各附件格式 + 是否有深度 + 深度格式 +
提升标志）在构建末尾记录、在重建的重置块里清空，重建谓词比较整把钥匙而不是逐项列举
—— 以后再加"烧进构建"的属性时，漏掉列举也不会静默失效。尺寸与深度策略仍是**单独
的项**：`width`/`height` 是借用校验要读的"已建尺寸"，且 `releaseRenderTarget` 靠清
零它逼出重建；`depth_load` 来自引擎**本帧**的 pass 请求，不是目标描述。

**判据**（selftest `runTargetDescriptionChangePhase`，两段）：

1. **中途加颜色附件**：第 1 帧前 `readColorBuffer(t, 1)` 必须失败（对照组：那一刻
   确实只有 1 个附件）→ 第 2 帧 `attachColor()` → 之后附件 1 必须**读得回来**、且是
   契约的**透明黑**（没有管线写它：MRT 契约 D31）；重建次数**恰好 1 次**。
2. **中途打开深度提升**（源 `setDepthPromotion(false)` → 借用生效，借用方远面必须被
   源的近深度拒绝 = 对照组）→ 第 3 帧 `setDepthPromotion(true)` → 源重建（提升 +
   finalLayout）**且借用方重建**（重跑校验 → 拒绝 + 报一次 + 回落自有深度）→ 之后
   借用方的远面**必须又能画出来**（它现在用自己的、每帧清的深度）；变更帧重建**恰好
   2 次**（源 + 借用方），此后不再增长。

**判据力（反证）**：把构建指纹项从谓词里停掉 → 4 条断言同时报红，且证据精确：
`never existed`（附件）、`kept borrowing it`（提升没进构建）、`not reported`
（0 条诊断）、`0 time(s)`（变更帧重建 0 次 = 提升完全不可见）。

证据行：

```
[selftest] target description: a colour attachment added and a depth promotion
turned on after the first frame both took effect, each with the rebuild confined
to its change frame; attachment 1 read back transparent black and the promoted
source's borrow was refused (1 report)
```

harness 追加 `target description:` ≥1 行证据要求。

**验收**：test_vsg 71、test_graphics 157 全绿；`gfx_lavapipe_check.sh` → `RESULT:
PASS`（0 VUID，含新证据行）；app 冒烟：exit 124、离屏只构建 3 次（无重建循环）、
1 条预热借用警告、0 VUID。

## 27. 共享对象表（`shared_objects_`）的保留必须跟着缓存驱逐走（D40，2026-09-11）

`SceneBridge` 每个槽持有一个 `vsg::SharedObjects` 去重表：`config->copyTo(stateGroup,
shared_objects_)` 把**管线 / 布局 / 描述符集**登记进去，内容相同的变体共用同一个对象
（"64 个相同变体塌成 1 个 pipeline"就是它存在的意义）。问题是**登记即持有**：变体条目
被 FIFO 逐出、或 App 释放了 program/material 让条目变成 abandoned 之后，表**仍然**抓着
那个 pipeline —— 三个缓存的上限因此形同虚设，表只增不减，直到槽 teardown 才 `clear()`。

**修法**：`releaseAbandonedCaches()` 在**有驱逐的那一帧**调 `shared_objects_->prune()`
—— vsg 的 `prune()` 恰好就是本项目自己的规则（`referenceCount() == 1`，即"除了表没人
要了"，见 `OwnedCache.hpp` 的 `keyReleased`），所以**在用的变体**通过各自缓存的 bind
命令继续持有 pipeline 而被保留，被逐出的才真的释放。单调不减的 `shared_prune_count_`
作为诊断（`sharedPruneCount()`）。

**触发面**要覆盖两条驱逐路径：abandoned 清扫（`eraseAbandoned` 的返回值）**和**插入点
的 FIFO 裁剪（`trimToCapacity`；它不能等到帧末 —— 那正是"缓存有界"的来源），后者通过
`noteEviction()` 记账，由 `releaseAbandonedCaches()` 统一决定是否 prune。代价：只有在
"确实驱逐过"的帧才走一遍表（表大小受在用变体数 + 当次驱逐量约束）⇒ 摊销到每次驱逐
O(1)，而不是每帧 O(表)。

**判据**（test_vsg `SharedObjectsTableIsPrunedOnEvictionFramesOnly`）：
65 个程序（> 每程序缓存上限 64）⇒ 插入点裁剪 ⇒ **那一帧必须 prune**（`sharedPruneCount()
>= 1`）；同时 64 个相同变体必须仍塌成 **1 个 pipeline**（prune 不能破坏共享）；再画一个
**必定命中缓存**的程序 ⇒ 无驱逐 ⇒ **不得再 prune**（摊销的反面）。反证：停掉 prune →
第一条断言红（`0 vs 1`）。

**未单独断言的部分（诚实记录）**：`prune()` 真的把某个 pipeline 交还给 allocator 这一
步由 vsg 的引用计数规则保证（"除了表没人要" ⇒ 删），而"重建一个被逐出的变体会重新计入
`pipelineVariantCount()`"这个端到端现象依赖**条目所有者何时放手**（几何条目经 retire
环释放），单个 bridge sync 内无法确定性地钉住 ⇒ 不再写脆弱的断言，改为在断言里写清
"这里钉的是触发面与摊销，释放由 vsg 规则保证"。

**验收**：test_vsg **72**（71 + 本条）、test_graphics 157 全绿；`gfx_lavapipe_check.sh`
→ `RESULT: PASS`（0 VUID）；app 冒烟 exit 124、离屏 3 次构建、0 VUID。

## 28. 计划：把 render pass 从**目标粒度**下沉到 **pass 粒度**（**进行中**，分阶段）

> **"每 pass 一个 render pass 对象"不是终点（2026-09-12 补记）**：这条路线存在的理由只有一个 ——
> 一个 render pass 对象只能烧死一种 load-op 组合。若将来能走 **dynamic rendering**
> （`vkCmdBeginRendering`：attachments 内联、load-op 变成逐 pass 取值），同一个目标上的 K 个 pass
> 可以合并成 **1 个渲染作用域**，本节这批机制（变体工厂、"变体必须兼容"的子通道依赖约束、
> 一次性 transient 变体 + 帧末换回、D49 的"请求变了就重建"）**大部分都可以删掉**，剩下的只是
> 把布局记账改写成显式 barrier。当前**走不通**（vsg 1.1.16 无 dynamic rendering、无自定义记录钩子、
> 管线硬绑 render pass 对象）—— 证据、触发条件与第一步 spike 的判据见 **§9.4**。

> **实施进度（2026-09-11）**
> - ✅ **第 1 步：render-pass 工厂支持 per-pass 颜色 load-op。** `makeSampleableRenderPass` /
>   `makeDepthLoadRenderPass` 新增 `color_clear`（含 LOAD 时的
>   `color.initialLayout = SHADER_READ_ONLY_OPTIMAL` 与外部依赖
>   `srcAccessMask = COLOR_ATTACHMENT_WRITE`），默认 `true` ⇒ 现有调用零行为变更。
>   验收：`test_vsg` 77 / `test_graphics` 157 / `gfx_lavapipe_check.sh` RESULT: PASS。
>   **诚实说明**：新路径（`color_clear=false`）要等第 3 步接入每 pass render pass 后才被设备门禁
>   覆盖，当前仅为编译期就绪的工厂能力。
> - ✅ **第 2 步：`Target::PassObjects` 数据模型 + 纯策略函数。** 新增
>   `Target::PassObjects`（render_pass / render_pass_transient / framebuffer / graph /
>   load_depth / color_clear / clear_color / seeded）与目标级 `attachments_built` /
>   `depth_seeded` / `any_load_pass`（**先加不删**，旧字段仍权威）；并把 §28 的三条不变量
>   （颜色 LOAD、LOAD↔提升互斥、未定义图像需 seed、借深度恒 LOAD 不提升）抽成**设备无关**的
>   `detail::planPassRenderPass()`。验收：`test_vsg` 77 → **83**（新增 `PassRenderPassPlanTest` ×6）、
>   `test_graphics` 157、gate PASS。
> - ✅ **第 2 步（续）：命令图排序核心抽成纯函数。** `detail::stableTopologicalOrder(node_count, edges)`
>   （Kahn、按 index 稳定、环不丢）现在驱动 `reconcileOffscreenOrder`（采样边 + 深度借用边），
>   新增 `GraphOrderTest` ×6；gate 的 `depth share order` 相位确认行为不变。这样第 3/4 步把
>   “每目标一张 graph”改成“每 pass 一张 graph”时，排序内核已有测试兜底。
> - ⏭ 第 3 步（按 TU 分批接入每 pass render pass/graph）见下。
>
> **第 3 步尝试记录（2026-09-11）**：本步是**原子性**的——`buildOffscreenTarget` 的
> ~180 行 render-pass/framebuffer/graph 构建必须整体改成每 pass 懒建（`ensurePassObjects`），
> 同时 `reconcileOffscreenOrder` / `placeViewByOrder` / release / retire / readback /
> `clear()` / `submitFrame` 全部要换输入，四个 TU 必须同时改完才能编译。实测无法在不破坏
> 当前全绿基线的前提下一次完成，**已回滚到上一版绿色快照**（工作区与 `test_vsg` 89 / gate PASS
> 完全一致）。下一步应作为**单独一次专注改动**执行，回滚点见 `/tmp/vsg_backup/`（临时）。
>
> **第 3 步完成（2026-09-11）**：落地为“每 pass 一张 render pass + framebuffer + RenderGraph，
> 图像按目标共享”。全部相位通过、零警告、`test_vsg` 89 / `test_graphics` 157、门禁
> `RESULT: PASS`（0 VUID）；`mixed depth` 相位报告 **1** 次目标构建（§28-7 判据达成），
> `ClearAttachments` 回退与 `makeDepthClearCommand` 一并删除。
>
> 三次踩坑的结论（按代价排序）：
> 1. **seed 交换必须在 `recordAndSubmit()` 之后。** `PassObjects::seeded` 的意思是“本帧仍
>    录制 CLEAR（seed）变体”。若在录制前就换成稳态 LOAD 变体，pass 的第一帧就会用
>    `initialLayout = DEPTH_STENCIL_ATTACHMENT_OPTIMAL` 去 LOAD 一张仍是 `UNDEFINED` 的深度图
>    ⇒ `VUID-vkCmdDraw-None-09600`（层报“expects … ATTACHMENT--instead, current layout is
>    VK_IMAGE_LAYOUT_UNDEFINED”，并按 DEPTH / STENCIL 两个 aspect 各报一条）。**这一条就是此前
>    两次尝试全部 14 条 VUID 的唯一根因**，与借用顺序无关。
> 2. **每个目标的 pass graph 必须按各 pass 的 `setPassOrder` 记录。** 视图散到各自的 render
>    pass 后，“在同一 render pass 内按 order 排序视图”（`placeViewByOrder` 原先的职责）必须
>    上移为“在命令图里按 order 排列 pass graph”。若取 `std::map<SlotKey, PassObjects>` 的迭代序
>    （即指针序），目标的第二个 pass 会记录在第一个之前，它的颜色清屏直接抹掉第一个 pass 的
>    绘制 —— 症状是混合相位阶段 2 报“远平面四边形仍不可见”、TestOnly 相位报出不透明的中心色。
>    做法：`PassObjects::order`（`passGraph` 里取 `request.order`），变化时重新 reconcile，
>    并按它 `stable_sort`（用当前子序做稳定种子）。
> 3. **`Target::depth_sampleable` 必须在附件构建期就记下**
>    （`has_depth && !borrowed && target->depthPromotion()`），不能等某个 pass 创建时才记：
>    借用该深度的消费者在**同一帧**就要校验，那时源的任何 pass 都还不存在。
>
> 另外两条行为约定：从未调用 `clear()` 的目标其 pass 仍每帧清颜色与深度（`clear_seen` 判据），
> 否则只作为采样目的地的目标会从“每帧清屏”变成“保留”，改变既有语义；
> `incrementalCompileViews()` 必须用 **pass 自己的 framebuffer** 建编译上下文（搜索结果需另存
> `SlotKey`，结构化绑定在循环外已出作用域）。

§22 的残留（首帧仍按旧策略收敛 1 帧、混合目标失去 render-pass 免费清屏而要付一条
`ClearAttachments`、`depth_policy_mixed` sticky、**颜色清屏同样是"最后一次请求赢"**）
都源自同一个设计决定：**一个目标只有一个 render pass**，而 Vulkan 把 depth/colour 的
load-op 烧在 render pass 里。要根治就得"每个 pass 一个 render pass（+ framebuffer +
RenderGraph），图像按目标共享"。已勘察的改动面与**必须同时成立的不变量**如下（下一片
按此执行，避免半成品）：

1. `Target::PassObjects`（按 `SlotKey` 索引，`std::map` 地址稳定）：`render_pass`（语义
   变体）+ `render_pass_transient`（只在本帧需要：图像 UNDEFINED 的 CLEAR seed，或图像还在
   提升留下的 SHADER_READ_ONLY）+ `framebuffer`
   + `graph` + `load_depth` + `seeded` + `order` + 该 pass 的 clear 颜色；`Target::graph`
   只留给窗口目标（swapchain graph），"目标已建"改判 `framebuffer != nullptr`。
2. **LOAD / 提升互斥**：借用别的目标深度的 pass 一律用 LOAD（策略归出借方）；一旦某目标
   出现 LOAD pass，该目标**所有** pass 都不得提升深度（否则上一帧末 pass 留下
   `SHADER_READ_ONLY`，下一帧 LOAD pass 声明 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` 非法）。
   先建的提升变体在这条规则被触及时要重建（不动图像）。
3. **种子**：`seeded` 是**目标级**（"这张深度图至少被定义过一次"）——UNDEFINED 图像不能
   LOAD，所以本帧第一个 LOAD pass 若目标从未清过，就用 CLEAR 变体录一帧（今天
   `depth_ready` 的等价物）。
4. **顺序**：`reconcileOffscreenOrder` 的目标级拓扑不变，改为"每个目标按 pass order 输出
   它的一组 graph"；深度共享 barrier 插在**源的最后一张 graph 之后**、借用方组之前。
5. **回读**：`readDepthBuffer` 不能再读 `Target::render_pass` 的 finalLayout（可能多个），
   改成跟踪"最后一帧最后记录的那个 pass 的 finalLayout"。
6. **清屏**：颜色清屏进各自 pass 的 `clearValues`（**颜色**也随之下沉，这是额外收益）；
   窗口目标仍只有一个 vsg 拥有的 swapchain pass ⇒ 窗口仍"最后一次清屏请求赢"，写进契约。
7. selftest 必须按新语义重写 `runMixedDepthPolicyPhase`：**不再注入 ClearAttachments**、
   混合目标**首帧即正确**、目标构建数从 2 降到 **1**（残留消失的可观测判据）。§22 保留为
   历史，实施后在此标注 superseded。

**补充不变量（实现勘察发现，2026-09-11）**：

- **颜色 load-op 也必须下沉**：不清屏的 pass（HUD / 透明）其 render pass 必须
  `LOAD_OP_LOAD` color，否则它会清掉同目标更早各 pass 的画面。工厂已支持该组合（第 1 步），
  颜色 LOAD 的 `initialLayout` 必须是 `SHADER_READ_ONLY_OPTIMAL`（上游 pass 的 finalLayout），
  且外部依赖要补 `srcAccessMask = COLOR_ATTACHMENT_WRITE`。
- **“LOAD 与提升互斥”会触发重建级联**：pass 是懒创建的，可能先建了“提升版 CLEAR pass”，
  之后才出现 LOAD pass ⇒ 必须**重建**该 pass 的 render pass + framebuffer + graph，并把
  已挂的 View **重新挂回新 graph**，同时刷新 `reconcileOffscreenOrder` 的依赖边。这是本项
  最容易出隐蔽 bug 的一条（计划原文只写了“不得提升”，没写已有 pass 怎么处理）。
  **同一时刻还要看槽**：撤销提升发生在一个正在组帧的帧里，而已建好的全屏 program 槽若**绑定了**
  该目标的深度，它的描述符名的是"提升后"的布局 —— 必须在本帧里丢弃（否则帧内就过期，每帧一条
  `imageLayout-00344`），见 §30 的 D47 条。
- **`Target::graph` / `render_pass` / `framebuffer` 等字段在 5 个 TU 中被直接引用 98 次**
  （`VsgRenderer.cpp` 22 / `VsgRendererTargets.cpp` 41 / `VsgRendererPasses.cpp` 17 /
  `VsgRendererOverlay.cpp` 10 / `VsgRendererImpl.hpp` 8），迁移要按 TU 分批、每批 gate 绿。

## 29. 交班状态与下一步清单（2026-09-11 收工记录）

**交班状态**：本次提交（D41–D45 + §28 第 1/2 步），工作区干净。基线：
`test_vsg` **89**、`test_graphics` **157**、`gfx_lavapipe_check.sh` → `RESULT: PASS`
（第二轮评审补 `LightGroupTest` ×5 与 D41–D45，见 `vine-to-vsg-data-flow.md` §13.8）
（0 VUID，证据行含 `depth load:` / `program hotspot:` / `shared depth pixels:` / `mixed depth:` /
`depth borrow:` / `depth testonly:` / `depth share order:` / `target description:`）；
app 冒烟 `timeout 25 ./dist/bin/Vine` exit 124、离屏 3 次构建、0 VUID。复验命令：

```
ninja -C build && ./build/bin/test_vsg && ./build/bin/test_graphics
timeout 900 bash scripts/gfx_lavapipe_check.sh
cp -f build/lib/*.so* dist/lib/ && cp -f build/plugins/vine/*.so dist/plugins/vine/ \
  && cp -f build/bin/Vine dist/bin/Vine && timeout 25 ./dist/bin/Vine   # 需退出码 124
```

（改 SDK 公共类布局才需要整份 `dist` 刷新；只改插件时 `cp` 插件 `.so` 即可。）

### 验证纪律（2026-09-11 事故换来的，别跳）

* **A/B 实验前必须确认构建成功。** 一次 clang 前端崩溃（exit 135，并行编译下偶发）让
  `ninja` 失败，而随后的自检跑的还是**上一个二进制**，于是"把缺陷放回去看判据是否咬住"
  得到的结论完全相反——差点让我把一条有效的防回归判据当成无效的删掉。看到构建失败就
  停下，不要拿旧产物继续推理。
* **推测性的"行为对齐"改动一旦被证伪，必须当场回滚。** §28 第 3 步加过一条
  `want_color_clear = !t.clear_seen || request.presenting`，当时已发现它解释不了正在追的
  VUID（真正根因是 seed 交换时机），却给了它一段"保持既有语义"的注释留了下来，结果它
  在 pass 粒度下抹掉了同目标上后一个 pass 的内容，默认 demo 丢失不透明模型（D46）。
* **门禁只能证明"无验证错误 + 已写下的像素判据"，证明不了"语义仍然对"。** 语义变化必须
  由**该语义自己的判据**覆盖（这正是 §28 第 4 步补"每 pass 各自清屏"断言的理由）。

### 下一步按此顺序做（每条都带判据，别跳步）

1. **render pass 下沉到 pass 粒度**（§28 的完整计划，最高价值）：
   * 提交 1（结构 + 语义）：`Target::PassObjects`（按 `SlotKey`）承载 render pass 双变体 /
     framebuffer / graph / `load_depth` / `seeded` / `order` / 本 pass 清屏色；`Target::graph`
     只留给窗口；"目标已建"改判 `framebuffer != nullptr`；`render()` 谓词去掉 `depth_load`
     项（改由每个 pass 对象自己选变体，无需重建）；删除 `depth_policy_mixed` /
     `ContentSlot::clears_depth` / `depth_clear_group` 与 `VsgRenderer::clear` 里的混合判定。
     **必须同时成立**的 6 条不变量见 §28（LOAD/提升互斥最重要）。验收：现有 72 + 157 全绿，
     §28-7 的 selftest 改写后 `runMixedDepthPolicyPhase` 报告目标构建数 **1**、证据行不再提
     "注入清屏"；gate PASS；app 冒烟 124。
   * 提交 2（判据 + 文档）：重写混合策略相位并按新语义补"颜色清屏按下沉"的断言（两 pass 各自
     清不同颜色 → 各自成立），§22 标注 superseded，登记表补一行。
   * 风险点（先想清再动手）：LOAD pass 与"提升"不能共存；`seeded` 必须是目标级；深度共享
     barrier 插在**源最后一张 graph 之后**。
2. **`SceneBridge::syncRenderCommands` 拆分**（256 行 → "缓存决策 / 节点替换 / 收尾"三个函数，
   纯搬运）：判据是 72 + 157 全绿 + gate PASS，无新行为。
   **已部分落地（2026-09-11）**：收尾两段抽出为 `evictAbsentItems`（37 行）与
   `publishRetainedChildren`（44 行），`syncRenderCommands` 降到 **191 行**（纯搬运，gate PASS）。
   命令循环体（~150 行）仍需抽出为 `reconcileCommand`；它整段位于 `for` 内（8 空格缩进），
   抽取必须整体重排缩进，属高风险纯格式改动，留待单独一次提交（无行为收益）。
3. **信息性 stderr 迁 `vine/logging`**：harness 与 `scripts/gfx_lavapipe_check.sh` 有多处
   stderr 断言（`[VsgRenderer] device:` / `EXPERIMENTAL off-screen target` / `has no depth
   image yet`），必须**同一次提交**里同步改，否则闸门会红。
   **已完成（2026-09-11）**：插件库自身的 10 处信息性跟踪改走 `V_LOGI` / `V_LOGW` /
   `V_LOGE`（`vi::Logging` 已加入插件 / `vsg_backend_selftest` / `test_vsg` 三个目标），
   **消息原文保持不变**、级别取 Info ⇒ 默认级别即可见，所以
   `scripts/gfx_lavapipe_check.sh` 的 `[VsgRenderer] device:` 断言**无需改动**（已复跑确认
   PASS）。两条有意不迁：`reportFailure` 的 stderr 半边（它自身就是“validation harness 会读
   的内建跟踪”，真正的诊断已走宿主 sink）与 `[MRT-DIAG]`（env 门控的开发诊断）。
4. **D28 `VkPipelineCache` 持久化**：仍被上游阻塞（vsg `GraphicsPipeline::compile` 传
   `VK_NULL_HANDLE`），只有等 vsg 暴露注入点才可做；不要自建管线。
5. **可选：D40 的"释放半条"端到端断言**：需要设备级（真实 VkPipeline 被 `prune()` 回收后
   重建变体重新计数）。单元层面解决不了（依赖 retire 环的放手时机），已记录在 §27。
6. **真机 GPU 像素冒烟**：本机只有 llvmpipe/lavapipe（无 `/dev/dri`），仍待有 GPU 的机器。

## 30. 第三轮审查（D47–D51，2026-09-11）

审查范围：`gfx_backend_vsg`（pass / 目标 / 槽生命周期）+ `graphics`（前端引擎与 `SceneView`）。
本轮**先证明再动手**：D47 / D48 / D50 做了反证或单元判据；D49 / D51 在第二批改动里
落地（同一批，因为两者都改深度策略），并补齐设备级相位、门禁证据行与逐条反证。

### D47（已修）全屏 program 的深度绑定读的是"描述"，不是"实际可采样性"

`drawScreenProgram` 用 `source->depthPromotion()`（宿主写在**目标描述**里的请求）决定是否把源深度
作为 `gbuffer_depth` 采样纹理绑定，而**实际**可采样性是 `Target::depth_sampleable`：一旦该目标上出现
保留型（LOAD depth）pass，§28 的"LOAD 与提升互斥"就撤销提升（深度留在附件布局），但 program 仍按
`SHADER_READ_ONLY_OPTIMAL` 声明描述符 ⇒ 布局不符（每帧 VUID）。**只有这里**还在读描述 ——
`readDepthBuffer` 与 `buildOffscreenTarget` 的借用校验读的都是实际值，这种不对称就是它漏网的原因。
修法：改用 `src.depth_sampleable`，并把 `ProgramSlot::source_depth_sampleable` 纳入重建身份（策略变化
→ 节点重建）；`VsgRenderer.hpp` 的契约同步写明。
**同帧残留（已修，判据先落地）**：若"采样深度"的 program pass 在同一帧**先于**撤销提升的保留型 pass
创建，则本帧节点已按"可采样"建好（描述符声明 `SHADER_READ_ONLY_OPTIMAL`），而撤销发生在同一帧更晚
⇒ 该帧记录的是**过期描述符**（`VUID-vkCmdDraw-imageLayout-00344`，每一条 draw 一条），宿主无从知道。
判据：`runDepthSamplingProgramPhase` 第 3 段 —— P(order 0) 清屏（提升）→ 采样深度的 program
(order 2，**先建**) → Q(order 1) 保留型（**后到**，但 order 在前 ⇒ P 已跑，Q 无需过渡变体，把问题
**孤立到槽**而不是 pass 自身布局）。修复前实测：**1 条 VUID + 2 条 FAIL**（无丢弃报告；目标被写入
采到的 `(6,6,6)`）；修复后 VUID 0、两条判据转绿。
修法：撤销提升的级联里**丢弃**那些真的**绑定了**该深度的 program 槽（`removeGraphChild` + 删槽 +
一条 `dropped for this frame` 报告）。宿主下一次 `drawScreenProgram` 会按新的可采样性重建：不需要
深度的 program 正常重建并继续绘制，需要深度的走既有 `MissingDescriptorBinding` 拒绝路径（D47 的
另一半语义）。
判"真的绑定"用 `programSamplesDepth()`（纯函数，ABI：深度绑定号 = 颜色附件数；`ProgramSamplingTest`
4 条单测钉住"跟随颜色数 / 只看 fragment 段 / 无声明即无绑定"）。**纯颜色 program 不受影响** ——
它的管线布局里根本没有深度采样器，所以本帧照常绘制、下一帧也不需要重建：丢弃只针对"有东西可丢"的槽。
级联里既有的 `waitForIdle()` 同时兜住了这一步（删槽会释放视图与管线对象，上一帧的命令缓冲可能仍在
飞行）；延后释放（retire 环）仍是**独立改动**，与本条无关。

### D48（已修）深度清屏值被颜色清屏值覆写（`VkClearValue` 是 union）

`passGraph` 的"已建 pass 只更新清屏值"路径写 `graph->clearValues[0].color`，但 `VkClearValue` 是
**union**：**depth-only** 目标（`attachDepth`、无 `attachColor`）的 `clearValues[0]` 是**深度项**
⇒ 清屏颜色一变就把深度清屏值改成颜色的 float 位型（shadow-map 路径）。修法：仅当目标有颜色附件时
才写该项。

### D49（已修）pass 的清屏策略在运行期改变会被静默忽略

`PassObjects` 的 load-op 在 graph 创建时按当时的请求烧死，早退路径只同步颜色**值**、不同步
**load-op**。于是宿主运行期改 `RenderPass::setClearEnabled` / `setShouldClearDepth`（或 direct driver
改 `clear(..., clearDepth)`）**不生效**：原来 LOAD 的仍 LOAD，原来 CLEAR 的仍 CLEAR。
修法（与 D51 同批）：把"每 pass 变体"的决策全部提到早退路径之前（纯策略函数 `planPassRenderPass()`，
不碰设备），并把重建判据改成**宿主请求**（`PassObjects::want_color_clear` / `want_depth_clear`）；
请求变了就重建该 pass 的 render pass / framebuffer（**保留同一个 `RenderGraph` 对象** ⇒ 内容 View
不重编），换之前 `waitForIdle`。

实现时踩到并修好的三个坑，全部写进代码注释，避免后人再犯：

1. **渲染通道兼容性包含子通道依赖**。运行期互换 render pass 的前提是"两个变体兼容"。规范豁免的只有
   initial/final layout、load/store op、attachment reference 的 layout、两个 resolve flag ——
   **子通道依赖必须逐字段相同**，否则 `VUID-vkCmdDrawIndexed-renderPass-02684`
   （`pDependencies[0].srcAccessMask is incompatible ... VkAccessFlags2(0) !=
   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT`）。两个工厂原先各按自己的 load-op 算
   `ext_to_sub.srcAccessMask`（CLEAR 变体 0、LOAD 变体含深度写），现在统一成与 load-op 无关的常量。
2. **保留型（LOAD depth）pass 必须声明深度图像"真实所在"的布局**。它不只是 UNDEFINED（未定义，要先
   CLEAR seed）或 ATTACHMENT（上一 pass 留在附件布局），还可能是 **SHADER_READ_ONLY**（上一帧的提升型
   pass 把它留在可采样布局）。第三种情况若声明 ATTACHMENT ⇒ `VUID-vkCmdDraw-None-09600`。
   因此 `makeDepthLoadRenderPass()` 的 `initial_clear` 升级为 `VkImageLayout depth_initial`：三种变体
   只在 load-op 与 initial layout 上不同 ⇒ 互相兼容，可随时互换。该帧记**一次性变体**
   （initial = SHADER_READ_ONLY，帧末 `submitFrame()` 换回常驻变体），与既有 seed 机制同一套路
   （字段更名为 `render_pass_transient` / `transient`）。
3. **重建判据不能比 load-op，要比请求**。同一帧里一个 pass 会被构建两次（`setupContentSlot()` 与
   `render()` 各一次），而颜色 bootstrap（新目标的颜色图还是 UNDEFINED，第一个 pass 必须清一次）会让
   第二次构建算出不同的 load-op ⇒ 旧判据在同一帧里就把变体重建成"对 UNDEFINED 图像做 LOAD"（09600）。
   现在比较 `want_color_clear` / `want_depth_clear`。颜色 bootstrap 也改成**一次性变体**，否则
   bootstrap 会变成"这个 pass 永远清颜色"，每帧擦掉同目标早先 pass 画的东西 —— 正是 §28 要根除的模型。
   提升被撤销后本帧残存的 SHADER_READ_ONLY 布局由同一机制消费（`depth_read_only`：提升仍有效
   + 本帧还没有别的 pass 先跑过；判据用 `passes_active_this_frame` 与实际 record 顺序）。

判据：selftest 新相位 `runClearPolicyFlipPhase`（证据行 `[selftest] clear flip:`）—— 同一个 pass 先在
`clear(clear, false)` 下画近 quad、再在 `clear(clear, true)` 下画远 quad；断言深度由 0.0249 变回 0.0166
且远 quad 出现（中心 B=46）。反证：把早退判据强行改成恒真（§28 之前的"永不重建"）→ 该相位 FAIL
（"the clear-policy change was ignored"）。

### D50（已修，前端）`SceneView::setScene` 只换引用，两个消费者仍指向旧场景

`setScene` 原实现只有 `scene_ = std::move(scene)`：
* 默认 window pass 的内容是**绑在 pass 上**的（`RenderEngine::addPass(pass, content, order)`），换成
  新场景后 viewer 仍画旧场景；
* 惰性创建的默认 `OrbitCameraManipulator` 持旧场景的 **raw_ptr**（拾取 / `fitToScreen`）⇒ 宿主的最后
  一个引用消失后就是**悬垂指针**。
修法：`setScene` 内 `engine_->bindPassContent(window, scene_)` + 对 `OrbitCameraManipulator` 调
`setScene()`（用 `dynamic_cast`，**不新增成员**以避免 SDK 类布局 / ABI 变更）。
判据：`GraphicsTest.SceneViewTest.SetSceneReachesDefaultWindowPassAndSceneAwareManipulator`
（帧内 `Scene::contentCollectCount()`：替换后的场景走 1 遍、被替换的 0 遍；manipulator 的 `scene()`
也换过去）；反证：stash 掉 `SceneView.cpp` 的改动 → 断言红（实测被替换的场景走 1 遍）。

### D51（已修）depth-only 目标两处叠加缺陷

1. **深度清屏值取的是近平面**：`t.depth_clear_value = has_color ? 0.0f : 1.0f`，而本后端是
   **reverse-Z**（`VK_COMPARE_OP_GREATER`，近 = 1、远 = 0）⇒ 清到 1.0 后 `fragment(depth) > 1.0` 恒假，
   **任何几何体都过不了深度测试**。改为恒为 `0.0f`（远平面），与 window 清屏一致。
2. **shader set 假设至少一个颜色附件**：`buildShaderSet()` 在 `color_count == 0` 时仍用默认（1 个颜色
   附件）的 `ColorBlendState`，与 0 颜色附件的 render pass 不匹配 ⇒
   `VUID-VkGraphicsPipelineCreateInfo-renderPass-06055`（管线创建失败）。改为按 `color_count` 显式构造
   blend 附件列表（0 个即空列表）。
3. D48 的颜色清屏覆写（`VkClearValue` 是 union）在 depth-only 目标上是同一个坑，早退路径已加
   `has_color` 守卫。

判据：selftest 新相位 `runDepthOnlyTargetPhase`（证据行 `[selftest] depth only:`）—— 96x54 的 depth-only
D32 目标，每帧换清屏颜色、`DepthMode::TestAndWrite` 画一个 z=1 的 quad；回读深度断言 quad 写入
~0.0249、未触碰角落 ~0.0000（远平面），且清屏颜色变化不会覆写深度清屏值。反证：把清屏值改回
`has_color ? 0.0f : 1.0f` → 该相位报 2 条 FAIL（"the depth-only target centre holds 1.0000 ..."、
"... untouched corner holds 1.0000, not the far plane"）。

### D51 补（depth-only 目标也要遵守深度策略）

第一轮修完 depth-only 目标"能画"之后，它仍然在两处与其它目标不一致：

1. **`clearDepth=false` 被静默忽略**：`passGraph` 的 `!has_color` 分支恒用 `makeDepthOnlyRenderPass(...)`
   （CLEAR），plan 判成 LOAD 也不管 —— 与 D49 同一类错误。修法：该工厂也接收
   `VkImageLayout depth_initial`（UNDEFINED → CLEAR；其余 → LOAD），`passGraph` 传入
   `depth_load ? depth_initial : UNDEFINED`。
2. **谎报不可采样**：`if (depth_load) t.depth_sampleable = false;` 对 depth-only 目标是错的 ——
   它的深度**永远**收在 `SHADER_READ_ONLY`（这正是这个 render pass 的契约），而
   `readDepthBuffer()` 正是拿 `depth_sampleable` 当"图像当前布局"用 ⇒ 回读 barrier 会拿
   ATTACHMENT 当 oldLayout，而图像实际在 SHADER_READ_ONLY（反证实测：8 行 VUID）。修法：
   撤销提升的两处都加 `has_color` 守卫；depth-only 的"稳态布局"由一个新的
   `steady_depth_layout`（depth-only ⇒ SHADER_READ_ONLY，否则 ATTACHMENT）表达，级联重建、
   常驻变体、一次性变体都读它。

判据：selftest 新相位 `runDepthOnlyPreservePhase`（证据行 `[selftest] depth only preserve:`）—— 同一个
pass 先以 `clearDepth=false` 画近 quad、再以 `clearDepth=false` 画远 quad（必须输）、最后以
`clearDepth=true` 画远 quad（必须赢）；三次回读都走 `readDepthBuffer`（顺带把布局断言钉住）。
反证：① 改回"永远清屏" → 相位报 FAIL（"a depth-only pass that always clears ignores the request"）；
② 改回无守卫地撤销提升 → 8 行 VUID（回读 barrier 的 oldLayout 不符）。

### D49 补（颜色 bootstrap 的一次性语义有判据了）

D49 的第三坑（同一帧内一个 pass 会被 build 两次 ⇒ 重建判据不能比 load-op）当时只是**推断**：
"颜色 bootstrap 必须是一次性变体，否则这个 pass 会永远清颜色"。现在有了相位与两半反证：
`runColorBootstrapPhase`（证据行 `[selftest] color bootstrap:`）在**一个 colour-only 目标**上放两个
都不请求清屏的 pass：前几帧第一个 pass 铺满、第二个画小方块（bootstrap 清一次把 UNDEFINED 图像定义
出来）；**然后第一个 pass 停止出图**（空命令，但仍被 announce、仍记录它自己的 render pass），
它的填充必须还在 —— 没有任何 pass 请求过清颜色。

反证（两半都实测）：
* 把 `bootstrap_color_clear` 从一次性变体里拿掉（等价于"bootstrap 变永久"）⇒ 相位报 FAIL
  （`only 100 of 5184 pixel(s) ... (0 blue fill, 100 red quad) — a pass that never asked to clear kept
  clearing and wiped what the pass before it drew`），另有一条 FAIL 汇总；
* 反过来让首帧直接记录"稳态变体"（不清 UNDEFINED 颜色图）⇒ **6 行 VUID**
  （`VUID-vkCmdDraw-None-09600`：对 UNDEFINED 图像做 LOAD）。

### 顺序假设：实测结论（记录，不改代码）

`depth_read_only`（提升被撤销那一帧，LOAD pass 要吃下 SHADER_READ_ONLY）与提升级联都基于一个假设：
**一个目标的 pass 按它们记录的 order 依次 build**。最可疑的是"host 把两个 pass 的 `render()` 调用
顺序反过来（但 order 不变）"这一帧。做法：把 `runPreservedDepthNotSampleablePhase` 里两个 pass 的
调用顺序对调（仍在 `setPassOrder(-10)` / `(-5)`）实测 —— **VUID 0 / FAIL 0，相位自己的判据依旧成立**。
原因：该帧被构建的第一个 pass 是**保留型**的，它走的是 **seed** 分支（CLEAR 定义新图 + 帧末换回常驻
变体），而提升型 pass 在 `any_load_pass` 变真后就**不再提升**，两者都收在附件布局；记录顺序仍是
order（-10 先）。既然测不出反例，本轮**不做重构**（不做"撤销提升也延后一帧"的改法），而是把这个反序
场景**永久留在该相位里**（它自己的断言 + 门禁的 VUID 扫描一起看住它），并在相位注释与 §28 的假设处
写明"决策基于 build 时刻的目标状态"。

**补记（2026-09-12，同帧残留修复时重新评估）**：D47 的同帧残留**确实**是这一族问题（program 槽的
描述符比 pass 布局早一步），但最终**没有**走"撤销延后一帧"（那条路要同时改 `depth_still_promoted` 的
判据与 install 时机，并把 `depth_sampleable` 的语义从"已撤销"改成"本帧仍可采样"，牵动 D47 诊断、
借用校验与 `readDepthBuffer` 的 barrier 推导 —— 收益只是让槽多活一帧，代价是把整个提升时序改成两段式）。
采用的做法是**只对槽动手**：撤销级联里丢弃真正绑定了该深度的槽（见 D47 一条）。
且即便延后一帧，槽在"被撤销的那一帧"之后仍然可能过期（撤销 pass 自己就把深度交回附件布局），
所以丢弃是**无论如何都要做**的那一步。

### 代码整理（2026-09-12）：变体决策抽成纯函数 + 工厂合并

本轮的三个坑（子通道依赖必须逐字相同、LOAD 必须命名真实布局、同一帧双建不能比 load-op）
全部发生在 `passGraph` 的 9 个布尔量之间，而只有 `planPassRenderPass()` 那一半有单测。整理：

* **决策收进纯函数**：新增 `PassVariant`（记录哪几个 load-op/layout + 是否一次性变体 +
  常驻变体的参数）与 `planPassVariant()`（包住 `planPassRenderPass()`，补上颜色 bootstrap /
  深度 seed / 提升撤销 这三个一次性决定）以及 `passVariantIsStale()`；`passGraph` 只剩
  "取值 → 建对象 → 换图"。
* **三个 render pass 工厂合并为两个**：`makeSampleableRenderPass` + `makeDepthLoadRenderPass`
  → `makeColorDepthRenderPass(depth_initial, promote_depth, color_clear)`（行为等价：LOAD 分支原本
  就强制 `promote=false`，而 plan 对 LOAD pass 也一直是 false），删掉 92 行重复体；两个变体的
  依赖块抽成 `makeColorDepthDependencies()` —— 本轮 `renderPass-02684` 的根因是"两个工厂算的
  依赖不一致"，现在"兼容"是结构性的，不再靠约定。
* **单测 89 → 96**：`test_vsg/PassRenderPassPlanTest.cpp` 新增 7 例，销住 bootstrap 一次性 /
  seed / 撤销 / depth-only 布局制度 / 借用深度 / stale 只看请求。
  **测试当场拓出一个真简化**：`depth_initial` 原先对 CLEAR pass 也返回稳态布局，调用方只好
  自己写 `depth_load ? depth_initial : UNDEFINED`；改成"CLEAR 一律 UNDEFINED"后调用方那层特例
  分类消失了。
* **有意不做**：没有为三种变体引入 Strategy/Factory 类层次 —— 变体是值语义 + 一次设备调用，策略已
  由纯函数表达；项目规则也要求避免不必要的抽象。

### 代码整理（2026-09-12，第二批）：selftest 驱动的 RAII 作用域

自测的每个相位都在手写同一串调用（`beginFrame` → `beginPass` → order → target → depth mode →
clear → lights → draw → `endPass` → `endFrame` + `swapBuffers`）。改成两个 RAII 助手：
`FrameScope`（头尾一对）与 `PassScope`（一个 pass 的全部前置状态 + `endPass()`），再加
`readDepthOrFail()`。**先只改本轮新增的 6 个相位**，旧相位不动，让 diff 可审。

两个细节值得记住：
* **一个 pass 要占一个块**：`PassScope` 若与下一个 pass 共用一个作用域，前者的 `endPass()` 会拖到
  帧末才跑，pass 协议就错位了（backend 会把嵌套/未配对报成 `PassProtocolViolation`）⇒ 变换后每个
  pass 是 `{ PassScope …; draw; }`。
* **变换本身要能验证**：第一版脚本有个 off-by-one，把 draw 调用**删掉了**（编译得过、行为不对）。
  修正后加了断言：变换前后 `renderer.render(` / `renderer.drawScreenProgram(` 的行数必须相等。
  这是“机械重构也要有判据”的一个现成例子。

成效：`main.cpp` 4485 → 4420 行（14 个 pass 块 + 8 个帧循环），后续新相位可直接用；旧相位仍用
长形式，留作增量迁移。

第三批（同日）：按同一套机制把**剩余全部相位**也过了一遍：又 43 个 pass 块 + 25 个帧循环，
`main.cpp` 最终 4485 → **4228** 行；原生 `beginPass`/`endPass` 只剩两个助手自身的实现。

第三个坑（比前两个更难看出来）：**帧对只在它是所在块的最后一条语句时才能折叠**。有四个相位
**在循环内**回读（回读必须在 submit 之后），第一次尝试照样折叠了它，`runDepthShareOrderPhase`
当时就炸了：12 行 VUID + "the borrower tested the PREVIOUS frame's depth" —— 因为回读现在跑在
提交之前。识别方式：`swapBuffers()` 之后的第一个非空行若**缩进变浅**（块结束）才折叠；否则保留
显式帧对。

**机械重构的等价性判据**（以后都这么用）：重构前后各跑一次独立 selftest，`diff` 两份输出中的
**全部 `[selftest]` 行**（像素计数、build 计数、深度值、诊断都在内）。最终结果：逐字节相同 +
VUID 0 / FAIL 0 + 门禁 PASS。draw-count 断言拦住了"draw 被删"，而**证据行 diff 拦住了
"draw 被挪到错误的时刻"** —— 两者都不多余。

### 本轮验证与门禁

* `ninja -C build` 零警告；`test_vsg` **100**（+7：变体决策单测；+4：`ProgramSamplingTest` 的
  深度绑定判定）、`test_graphics` **158**（含 D50 的 +1）。
* 独立设备跑（lavapipe + Khronos validation，`VINE_VSG_DEBUG_LAYER=1`）：`VUID-` 计数 **0**、
  `[selftest] FAIL` 计数 **0**。
* `scripts/gfx_lavapipe_check.sh` → `RESULT: PASS`（0 VUID）；新证据行写进 `require_evidence`：
  `depth only:` / `depth only preserve:` / `clear flip:` / `preserved depth:` / `depth sample:` /
  `color bootstrap:` / `dropped for this frame` / `policy churn:`（既有证据行全部不变；`dropped for this
  frame` 是唯一的非 `[selftest]` 行 —— 它是渲染器 warning，证明同帧撤销真的丢了槽）。
* A/B（判据必须会咬，实测）：
  * D51a 反证（清屏值改回近平面）→ `depth only:` 相位 FAIL（2 条）；
  * D51 补反证（depth-only 永远清屏）→ `depth only preserve:` 相位 FAIL（2 条）；
  * D51 补反证（无守卫地撤销提升）→ 8 行 VUID（回读 barrier oldLayout）；
  * D49 反证（早退判据恒真）→ `clear flip:` 相位 FAIL（2 条）；
  * D47 诊断反证（不报 ChannelIgnored）→ `preserved depth:` 相位 FAIL（2 条）；
  * **D47 绑定反证（绑描述而非实际可采样性）→ `depth sample:` 相位 FAIL（4 条：未被拒、无解释、
    目标被写成了采到的 （6,6,6））+ 6 行 VUID**；
  * **D47 同帧残留反证（先加判据、后修）→ `depth sample:` 第 3 段 FAIL（2 条：无丢弃报告、
    目标被写入 `(6,6,6)`）+ 1 行 `VUID-vkCmdDraw-imageLayout-00344`**；修复后同一次运行转绿；
  * **策略变化不停设备的反证（第九批）**：①把变体交换 / 提升级联改回 `waitForIdle()` →
    `policy churn:` 报 **29 次设备等待** + 退役环零释放（2 条 FAIL）；②单把"不再公告的 pass 的
    视图摘除"的等待改回 → **7 次**（每个未公告帧一次，与相位里 toggle pass 的频率一致）；
  * 颜色 bootstrap 反证①（拿掉一次性变体，等价于"bootstrap 变永久"）→ `color bootstrap:` 相位 FAIL
    （`0 blue fill, 100 red quad` = 填充被抹掉）；
  * 颜色 bootstrap 反证②（首帧不定义颜色图，直接记稳态 LOAD 变体）→ 6 行 VUID；
  * 反序渲染实测（两个 pass 的 `render()` 调用顺序对调，order 不变）→ VUID 0 / FAIL 0
    （结论已写在上面的"顺序假设"一节，该反序场景永久留相位）。
  * D50 反证（stash `SceneView.cpp`）→ `GraphicsTest` 断言红。
* **D47 判据空白已闭合**：新增相位 `runDepthSamplingProgramPhase`（证据行 `[selftest] depth sample:`）
  是自测里唯一真的**采样纹理**的 program ——
  （1）提升型源的深度收在 SHADER_READ_ONLY，按 ABI（binding 1 = 源深度）的 program 必须能构建、
  并把采到的深度写回颜色（断言中心是小数值灰度：既不是源颜色、也不是 0）；
  （2）同一个 program 跑到"深度被保留型 pass 撤销提升"的源上必须被**拒绝**；
  （3）**同帧撤销**（program 先建、撤销后到）时该槽必须被丢弃、目标保留自己的清屏色。
  为此后端新增了一条硬约束（D47 据此才可观测）：**fragment 声明的描述符绑定若本 pass 提供不了，
  就在建节点时拒掉并上报**（`ProgramNodeFailure::MissingDescriptorBinding`）。vsg 1.1.16 没有
  shader 反射，所以绑定是**从源码扫**出来的（`declaredBindings()`：`layout(...)` 限定符里
  的 `binding` / `set`，set 缺省 0）；不拒的话管线缺少该 bind，错的是**每帧一条 VUID**、且宿主
  一无所知。
* **策略变化帧不再停设备（第九批，2026-09-12）**：变体重建、提升撤销级联、被丢弃的 program 节点、
  以及"某个 pass 本帧不再公告"的视图摘除，过去都在帧装配期 `waitForIdle()`。现在它们把被换下的
  render pass / framebuffer / 节点**停放**进渲染器自己的退役环（`Impl::retireObject()` /
  `advanceRetireRing()`，环深与推进点和 §8.2 的节点环一致）。判定标准还是 §3：**破坏性销毁**
  （槽 teardown、target 重建、`bridge.clearCache()`、depth 模式变更的状态重建）继续显式等待 ——
  它们释放的对象是图像 / 视图级别的，逐个停放不划算。
  判据：新增 `runPolicyChurnStressPhase`（证据行 `[selftest] policy churn:`）连续 15 帧翻颜色清屏、
  深度清屏与"pass 是否公告"，断言 **`deviceWaitCount()` 增量 0**、退役环**确实释放**（
  `retiredObjectCount()` 增量 > 0）、**target 构建数恰好 2**（一次一个，策略变化只能重建变体）、
  以及末帧的像素与深度值仍符合末帧策略。
  反证（实测）：把变体交换 / 级联改回等待 → 同 15 帧 **29 次设备等待** + 环零释放（2 条 FAIL）；
  单独把"视图摘除"的等待改回 → **7 次**（每个未公告帧一次）。
  为让等待可数，所有刻意的等待都走 `Impl::waitForIdle()`（计数）而不是自由函数。
  仍未覆盖：**depth 模式变更**（`SceneBridge::invalidateState()` 会丢状态包装）与 `clearCache()`
  系列 —— 要覆盖它们得先把桥接侧的节点也停放（`retireNode` 已在，缺的是"清缓存=停放而非销毁"），
  留作独立改动；相位特意把 depth 模式固定住，正是为了让这条边界可见（第一版相位翻了 depth 模式，
  于是每帧都撞上这处等待 —— 这本身就是一次实测确认）。

* **桥接侧结果与边界（第十批，2026-09-12）**：`SceneBridge::invalidateState()` **本来就**把状态包装
  停放（`retireNode`），所以 depth 模式变更那一处调用方等待是多余的 ⇒ 去掉（反证：改回 → 相位报
  **14 次**等待 / 15 帧，因为该相位每帧翻一次 depth 模式）。现在相位同时翻 colour clear / depth clear /
  **depth 模式** / pass 是否公告，四种都是 0 等待。
  但同一批尝试的\"把 target 重建与槽 teardown 的等待也换成停放 view\"**被实测否决**：那两条路径会
  `clearCache()`，清空共享对象注册表，那里的管线 / 采样器没有存活节点兜底 ⇒ **12 条**
  `vkDestroyPipeline-00765` / `vkDestroySampler-01082`。于是把它们改回计数等待，并在注释里写明原因
  （§8.2 的\"关键反例\"）。
  相位同时加了**第二段**：每帧翻 target 的附件形态（depth promotion，属于 build key）⇒ 每帧重建，
  断言\"重建次数 = 帧数-1、等待次数 = 重建次数\" ⇒ 破坏性路径每帧**恰好一次** teardown 等待，不会更多。
  顺带量到一条语义不对称：**pass 请求**（清屏策略）在**当帧**生效，而**target 描述**变更在**下一帧
  start 时**被采纳 —— 相位的计数按实测写成 `frames - 1`，并把这条写进相位注释（描述变更在帧内做出、
  下一帧 start 后可见；帧前做出则当帧可见）。
  新增/保留的诊断：`deviceWaitCount()`（破坏性等待计数）与 `retiredObjectCount()`（环释放计数，证明
  停放真的会释放）。

## 31. 结构整理：槽访问器 + `passGraph` / `buildOffscreenTarget` 拆分（2026-09-12）

本轮只改**结构**（行为契约不动），判据是**机械重构的等价性**：重构前后各跑一次独立 selftest，
`diff` 两份输出的**全部 `[selftest]` 行**必须逐字节相同（本批实测：相同），外加 `test_vsg` 100 /
`test_graphics` 158 / VUID 0 / FAIL 0 / 门禁 `RESULT: PASS`。

**改了什么（按收益排序）**

1. **三张槽表的遍历收敛成一个访问器。** `Target` 持有三张槽表（content / screen / program），
   它们的生命周期一样、只是"保留什么"不同，于是同一段双/三循环在 4 处重复
   （`retireInactivePassSlots`、`erasePassSlotsFromTarget`、`releaseRenderTarget` 的采样槽清理、
   `detachedSlotCount`）。新增 `Target::forEachSlot` / `visitSlot(key)` / `hasSlot(key)` /
   `eraseSlot(kind, key)` 与 `SlotKind`，四处各变成**一个**循环；"只有 content 槽拥有 bridge"
   这类差异由 `if constexpr (requires { slot.bridge; })` 就地表达（一侧），不再由三块近乎相同的
   代码隐式表达。同时 `Impl::slotGraph()` 取代了三份"这个槽的记录图在哪"的 lambda（其中
   `retireInactivePassSlots` 原先还有一次**只为判断"要不要动手"**的完整预扫描 + 一次真正的扫描，
   现在合成一次遍历，返回值 `any` 决定要不要 `reconcileOffscreenOrder()` + 日志 ⇒ 语义不变）。
2. **`passGraph` 387 → 271 行**，抽出四个命名单元（都在 `Impl`）：
   `passAttachments()`（设备 + 附件格式 + has_color/has_depth/borrowed，含 `PassAttachments` 结构）、
   `makePassObjects()`（一个 load-op 组合 → render pass + framebuffer）、
   `depthStillPromoted()`（"深度是否还停在提升后的布局"这一判断，原先是一个 20 行 lambda）、
   `revokeDepthPromotion()`（撤销级联：重建每个非当前 pass 的非提升变体并停放旧对象）。
   同帧"丢弃绑定深度的 program 槽"单独成为 `VsgRenderer::dropDepthSamplingProgramSlots()`。
3. **`buildOffscreenTarget` 317 → 257 行**：借用校验（三种不可用情形 + 一次/持久上报策略，
   ~90 行）抽成 `VsgRenderer::resolveDepthBorrow()`，于是该函数读作
   "校验借用 → 分配附件 → 发布"，不再在中间夹一段领域策略。
4. 删掉两处**因重构而失效**的重复：`releaseRenderTarget` 里局部 `graph_of_slot` lambda
   （已被 `Impl::slotGraph` 取代）、`VsgBackendUtility` 的自由 `waitForIdle`
   （第十批之后无调用者）。

**踩到的结构约束（值得记住）**

- `VsgRenderer` 的公开头只**前置声明** `struct Impl;`，所以内部 helper **不能**在公开头里写
  `Impl::Target&` 形参（会报 incomplete type）。结论：需要 `Target` 的 helper 一律做成
  **`Impl` 的成员**（声明在 `VsgRendererImpl.hpp`、定义在对应 TU），或做成 `VsgRenderer` 的
  私有方法且**形参不出现 `Impl` 类型**（如 `resolveDepthBorrow(RenderTarget&, u32, u32)`）。
  另外 `Impl` 内**声明位置**要晚于 `Target` 的定义（否则又是 incomplete type）。

**下一批的结构候选（未做，避免和本次混在一起）**

- `VsgRendererOverlay.cpp` 的 `drawScreenTexture`（226 行）与 `drawScreenProgram`（247 行）
  共享同一套骨架：dest 解析 → 尺寸/矩形钳位 → 旧槽失效判定 → 建视图 → 按 order 摆放。
  抽出这套骨架能同时缩短两个函数，但两者的槽类型与失效条件不同，需要先写清"共同骨架"的契约。
- `reconcileOffscreenOrder`（209 行）：排序 + 依赖边重建 + 命令图重挂三件事混在一起。

## 32. 结构整理续：两个 overlay 函数的共享骨架 + 记录计划三段（2026-09-12）

§31 末尾列的三个候选里，前两个在本节做掉（同样只改结构，判据还是**机械重构等价性**：
前后各跑一次独立 selftest，`diff` 全部 `[selftest]` 行**逐字节相同** + VUID 0 / FAIL 0 +
`test_vsg` 100 / `test_graphics` 158 + 门禁 `RESULT: PASS`）。

**1. `drawScreenTexture` 226 → 194 行，`drawScreenProgram` 247 → 222 行**

两者的头部（约 55 行 × 2）是**逐字重复**的：取 viewport → 源目标校验 → 目的目标解析
（`source == destination` 反馈环拒绝 / 无可用颜色附件拒绝 / 离屏目标按尺寸重建）→ surf 尺寸 →
`retargetPass` → `passGraph(dest, key)`。而且**报错措辞除前缀外完全相同**。

修法：新增 `VsgRenderer::OverlayDestination{target, graph, surf_w, surf_h}` 与
`resolveOverlayDestination(source, key, what)`，两个函数各只剩自己的**差异**：源校验
（PiP 的 attachment 钳位 / program 的深度提升报告）、key 的构造（`sampledTarget(source, att)` /
`sampledTarget(source)`）、矩形策略（PiP 自动贴右下 / program 只钳位）、节点工厂与日志。
消息用 `formatDiagnostic(u8"%s: ...", what)` 保持**逐字节不变**（`what` 就是原来的两个前缀），
所以相位里对消息的匹配不受影响。

收益不只是行数：**"overlay 画到哪"的规则现在只有一份** —— 反馈环、无附件、离屏重建这三条
一致性约束不会再在两个函数之间漂移。

**2. `reconcileOffscreenOrder` 215 → 21 行**

原函数把三件事串在一起：收集"本帧要记录哪些 pass 图"（并跳过已 retired 的 pass）、把目标按
采样 / 借用依赖排成合法记录序、把命令图 children 重写成该序。现在是一个值 + 三个命名阶段，
都挂在 `Impl` 上：

- `RecordPlan`：`window_graph` / `graphs_of`（每个目标的 pass 图，已按该目标自己的 pass order
  稳定排序）/ `present`（**当前**记录序，作为并列时的稳定种子）/ `order`（依赖合法序）；
- `fillRecordPlan(plan)`：收集（含"retired 的 pass 不记录"这条语义，以及"同一目标内按显式
  order 记录"的理由）；
- `orderRecordPlan(plan)`：采样边 + **深度借用边**（借用方本帧 LOAD 源写的深度）+ 纯函数
  `stableTopologicalOrder`；
- `applyRecordPlan(plan)`：按序重挂 children，并在"别人借用其深度的目标"的**最后一张图之后**
  插入借用方的 depth-share barrier，窗口图永远最后。

`reconcileOffscreenOrder()` 只剩守卫 + 三步调用（21 行），阶段之间的数据流被显式化成 `RecordPlan`。

**过程教训（大函数重构）**：只替换函数**头部**会留下旧函数体（第一版编辑就是这样，编译期立刻暴露）。
拆函数要**整段替换**，且 oldString 的锚点必须选在**两版真正不同的行**上（本例里新旧的
`pass_records` lambda 注释几乎相同，只有折行与 `Impl::Target&` / `Target&` 不同 ⇒ 只能靠这些
差异行定位）。

**仍剩下的结构候选**

- `vsg_selftest/main.cpp` 4603 行：相位可按主题拆成多个 TU + 一个共享 helper 头（`FrameScope` /
  `PassScope` / `PixelImage` / `readTarget` / `readDepthOrFail` / 若干程序构造）。**它的判据是
  现成的**（输出逐字节相同），但工作量在测试侧，收益也主要在测试侧可读性。
- 两个 overlay 函数的**尾部**（`makeCompiledOverlayView` 失败处理 → 记 `camera` / `view` /
  `ready` → `placeViewByOrder` → `reconcileOffscreenOrder` → 日志）结构一致，可用一个
  槽类型模板化的 helper 收敛（两者的槽类型不同，故需要 template member 或重载）。

## 33. 结构整理三：回读路径去重 + 设备等待一律可数（2026-09-12）

本节同样只改结构；判据仍是**机械重构等价性**（前后各跑一次独立 selftest，`diff` 全部
`[selftest]` 行**逐字节相同** + VUID 0 / FAIL 0 + `test_vsg` 100 / `test_graphics` 158 +
门禁 `RESULT: PASS`），本次还多了一条**不变量**：全后端不再有"未计入诊断"的直接
`vkDeviceWaitIdle`。

**1. 两个回读函数的公共前奏与一次性提交**

`readColorBuffer`（134 → 122 行）与 `readDepthBuffer`（123 → 113 行）各自重复了同一套东西：
会话 / 目标可用性判断、"同步前先等设备"、宿主可见内存的分配、"录制一次提交并等它完成"。
抽出三个 `Impl` 单元：

- `readbackTarget(target)`：会话 + `attachments_built` + 尺寸的公共前奏，**故意不等设备** ——
  两个调用方都先做格式检查（并报明原因）再付等待的代价；
- `hostVisibleMemory(device, requirements)`：把"回读落地内存必须是
  `HOST_VISIBLE | HOST_COHERENT`"这条要求写在一处（colour 落在 LINEAR 图像、depth 落在
  staging buffer，但内存要求相同）；
- `submitOneShot(commands)`：队列选择 + fence + **具名的超时常量**
  `kReadbackTimeoutNs`（原先两处各写 `100000000000`，且都没解释这个数字的含义：超时意味着
  "GPU 根本没做完"，不是"很慢"）。

**2. 设备等待：全部可数（诊断一致性）**

第十批引入 `deviceWaitCount()` 时的说法是"每一次刻意的设备级等待都被计数"，但实测有三处漏网：
`shutdown()`、`readColorBuffer`、`readDepthBuffer` 直接调 `viewer->deviceWaitIdle()`。
现在它们都走 `Impl::waitForIdle()`（计数 + 同一条注释语义），于是
**`grep "deviceWaitIdle"` 在插件里只剩 `Impl::waitForIdle()` 的函数体**，而
`policy churn:` 相位的"0 次设备等待"断言覆盖的正是全部等待入口 —— 诊断与代码重新一致。

**仍剩下的结构候选**（与 §32 相同，未做）

- `vsg_selftest/main.cpp`（约 4600 行）：按主题拆 TU + 一个共享 helper 头（`FrameScope` /
  `PassScope` / `PixelImage` / `readTarget` / `readDepthOrFail` / 程序构造）。它的判据是现成的
  （输出逐字节相同），但把数千行在文件间搬运需要**整段重写**，当前工具链下代价高、收益主要在
  测试侧可读性 ⇒ 记录在案，等专门一轮做。
- 两个 overlay 函数尾部（建视图 → 记 camera/view/ready → `placeViewByOrder` → reconcile → 日志）
  可用槽类型模板化的 helper 收敛。

## 34. 结构整理四：两个 overlay drawable 工厂共享"配方"（2026-09-12）

本轮仍是纯结构改动。判据同前（机械重构等价性：`[selftest]` 行逐字节相同 + VUID 0 / FAIL 0 +
`test_vsg` 100 / `test_graphics` 158 + 门禁 PASS）。

**发现**：`makeScreenTextureNode`（PiP 屏幕三角形）与 `makeFullscreenProgramNode`（用户全屏程序）
在设备眼里是**同一件东西**：全屏三角形 + 深度测试/写入关闭 + 不混合 + overlay 的视口 + set 0 上绑
采样纹理 + 运行时编译的 shader 模块。两人的"配方"里有三段逐字重复：

1. prologue：`ShaderCompiler` 可用性 → 两个 stage 创建 → 编译失败 → `ShaderSet{vs,fs}` +
   `defaultGraphicsPipelineStates = makeOverlayPipelineStates(extent)`（失败码 `NoCompiler` /
   `CompileFailed` 完全一致）；
2. `GraphicsPipelineConfigurator` 之后：`init()` → `copyTo(StateGroup, SharedObjects{})` →
   push constant（仅 program 有）→ `Draw(3,1,0,0)`；
3. 因此"两次 overlay 绘制长什么样"曾经写在两处，任何一处改动（例如将来给 overlay 加混合或
   换深度状态）都要记得改另一处。

**修法**：抽出 `makeOverlayShaderSet(vs_src, fs_src, fs_entry, extent, failure)` 与
`makeOverlayStateGroup(config, push_data)`（`push_data` 为 null 即 PiP 那种不带 push constant 的
drawable），两个工厂只剩自己的**差异**：描述符绑定与纹理（program 是 N 个颜色 + 可选深度，PiP 是
一个屏幕纹理）、采样器（深度用 nearest）、push constant 范围。
`makeScreenTextureNode` 73 → **51** 行，`makeFullscreenProgramNode` 182 → **159** 行。

**诚实说明（收益性质）**：本批**没有**减少总行数（新 helper 的 Doxygen 比省下的代码更长，
TU 758 → 780）。它的收益是**单点定义**："overlay drawable 的配方"现在只有一份，
两个 pass 之间不会漂移 —— 与 §32 的 `resolveOverlayDestination` 同理。

**未做**：`programSamplesDepth` / `declaredBindings` 仍只被全屏 program 使用（PiP 不需要 ABI 校验），
不强行合并；`VsgPipelineFactory.hpp` 的 467 行里真正的声明只占一部分（其余是 Doxygen），
暂不拆分。

## 35. 重构残留检查：守卫要跟着使用者走（2026-09-12）

§33 把"一次性提交"收进 `Impl::submitOneShot()` 之后，我在两个回读函数里留下了**没有编译错、
但已经是死代码**的东西 —— 这一节把这次检查本身记下来，因为它是重构后**必做**的一步：

**发现（都在 `VsgRendererTargets.cpp`）**

1. `readColorBuffer` 里 `const auto queue_family = physical->getQueueFamily(...)` —— 提交搬走之后
   没人用了（`submitOneShot` 自己算）；`readDepthBuffer` 里 `physical` 也只剩"判空"这一处使用。
2. 更值得记的是**守卫的位置**：两个函数原本用 `if (device == nullptr || physical == nullptr) return
   false;` 挡住"没有设备就去提交"。提交搬进 `submitOneShot()` 后，这个判空就与**真正解引用设备的地方**
   分家了 —— 留在调用方是"守卫着别人的参数"，搬到 `submitOneShot` 才是守卫自己的使用。

**修法**

- `Impl::submitOneShot()` 从 `void` 改为 **`bool`**（`[[nodiscard]]`）：它自己判
  `window` / `getDevice()` / `getPhysicalDevice()`，不可用就返回 false 并让调用方 `return false`；
  两个调用方从此不再重复这段判空 —— 于是 `readColorBuffer` 只需 `device`（建图 / 查格式属性用）
  与 `source`，`readDepthBuffer` 只需 `device`（建 staging buffer 用）。
- 顺带修回一处**我在同一批里改错的东西**：抽取时误删了 colour 路径的
  `VkImageSubresource sub_resource{...}` 声明（编译立刻报 `undeclared identifier`）—— 记下来的原因
  不是"手滑"，而是**同类风险**：把"提交"这类语句从中间拿走时，紧邻它的局部声明容易被一起带走，
  而编译器只在真的用到时才报。

**判据**：仍是机械重构等价性（`[selftest]` 行逐字节相同 + VUID 0 / FAIL 0 + `test_vsg` 100 /
`test_graphics` 158 + 门禁 PASS）；另外 `-Wunused-result` 在这次改动前后分别报出"忽略返回值"，
正好证明两个调用点都消费了 `submitOneShot` 的结果。

**结论（写进流程）**：每个"抽走一段逻辑"的批次结束后，都要专门查三类残留 ——
①死局部量 / 死参数；②守卫与被守卫的使用是否还在一起；③被搬走的语句**紧邻**的声明是否被带走。

## 36. 结构整理五：overlay 的"安装/摆放"两步 + 一个死参数（2026-09-12）

判据同前（机械重构等价性：`[selftest]` 行逐字节相同 + VUID 0 / FAIL 0 + `test_vsg` 100 /
`test_graphics` 158 + 门禁 PASS）。

**发现**：两个 overlay 函数在"节点造好之后"的部分也几乎逐字重复 —— 建视图 → 记入槽
（`camera` / `view` / `ready`）→ 按显式 order 摆放 → 若目的是离屏目标则 `reconcileOffscreenOrder()`，
以及"槽是被 retire 过的 → 重新挂上"的分支（`slot.detached = false` + 同样的摆放 + reconcile）。
另外 `makeCompiledOverlayView()` 的 `what` 形参**从无使用者**（当初用于日志，日志搬走后成了死参数 ——
与 §35 同一类残留）。

**修法**

- `placeOverlayView(dest, view, order)`：摆放 + "离屏目标就要 reconcile"这条规则写一处
  （3 个调用点：两个安装分支 + 两个 re-attach 分支）。
- `template <class Slot> installOverlayView(dest, slot, content, x, y, w, h, front, what)`：
  建视图（对目的目标的 render pass 编译）→ 失败则报一次并返回 false（调用方丢自己的槽，
  下一帧重试）→ 成功则记入槽并按 order 摆放。**模板**而非重载：screen / program 两种槽
  只有 `camera` / `view` / `order` / `ready` 四个共同字段被用到；声明放在私有区（形参里没有
  任何 `Impl` 类型，所以公开头能写），定义在本 TU（两个实例化点都在这里）。
- 删掉 `makeCompiledOverlayView()` 的死参数 `what`。

**行数**：`drawScreenTexture` 194 → **169**、`drawScreenProgram` 222 → **199**（相对 §32 之前的
226 / 247 分别少了 57 / 48 行），TU 724 → 705。更重要的仍是**单点定义**：编译失败即丢槽、
"摆放即可能重排命令图"这两条规则各自只有一处。

**验收**：`ninja` 零警告；独立 selftest `[selftest]` 行逐字节相同、VUID 0 / FAIL 0；
`test_vsg` 100 / `test_graphics` 158；`scripts/gfx_lavapipe_check.sh` → `RESULT: PASS`。

**overlay 这两个函数还剩什么（有意保留）**：源校验（PiP 的 attachment 钳位 / program 的深度提升
报告）、key 构造、矩形策略（PiP 自动贴右下 / program 只钳位）、节点工厂与 `V_LOGI` 文案 ——
这些正是两者的**差异**，再合并只会把差异藏进参数。

## 37. 结构整理六：内容槽每帧路径的阶段化（2026-09-12）

判据同前（`[selftest]` 行逐字节相同 + VUID 0 / FAIL 0 + `test_vsg` 100 / `test_graphics` 158 +
门禁 PASS）。

**对象**：`renderContentSlot`（内容槽的每帧热路径）223 行，六个阶段串在一个函数里：守卫 + 槽查找
（必要时建槽）→ `passGraph` → detached 重挂 → 运行时改变的 pass 属性重放（深度策略 / order /
presenting 派生出的默认光）→ 视口维持 → 相机 + 灯光 → 命令流同步 + 增量编译入队 + TEMP 诊断。

**做法（全部是文件内 helper，不动公开头）**

- `updateSlotViewport(camera, presenting, viewport, surf_w, surf_h)`：presenting 填满目标，其余带
  pass 的子视口（没有子视口 = 填满）—— 原先是 if/else-if/else 三分支共 14 行；
- `seedSlotLight(light_group, want_headlight, presenting)`：presenting 角色翻转时重置默认光
  （窗口 presenting 给 vsg headlight，其余给 ambient fill —— 方向光会把坐标轴 gizmo 从斜角照黑）；
- `beginLightsDroppedEpisode(announced, attached, reported)`："宣告的灯全被丢掉"是**场景的属性**
  而非帧的属性 ⇒ 每段只报一次、一旦有可用灯（或本帧没宣告灯）立刻重新武装；函数只回答"现在要不要报"，
  真正的 `reportFailure` 留在调用方（它才有 sink）—— 这是 §31 那条"helper 不能持有 renderer 状态"
  的自然延伸；
- `logContentSlotDiagnostics(target, depth_mode, order, commands, created, root_children, variants)`：
  把 env 门控的 TEMP 诊断（`VINE_VSG_DIAG_MRT`）从热路径里搬出去。**它是有文档的工具**
  （`vsg-user-mutation-strategy.md` / `vsg-pass-lifecycle.md` 都提到），所以**不删**，只搬。
  注意：它不能收 `ContentSlotRequest`（那是 `Impl` 的嵌套类型，文件内自由函数里不可命名）⇒ 传字段。

**结果**：`renderContentSlot` 223 → **120** 行（TU 547 → 550，含新 helper 的 Doxygen），主流程现在读作
"守卫 → 建槽/取图 → 重挂 → 重放属性 → 视口 → 相机/灯 → 同步命令 + 入队编译"七步，每步一句。

**验收**：`ninja` 零警告；独立 selftest `[selftest]` 行逐字节相同、VUID 0 / FAIL 0；`test_vsg` 100 /
`test_graphics` 158；门禁 `RESULT: PASS`。

**仍未做**（记录）：`SceneBridge.cpp` 的 `syncRenderCommands`（191 行）、`SceneBridgePipeline.cpp` 的
`buildStateGroup`（216 行）、`SceneBridgeGeometry.cpp` 的 `buildGeometryData`（225 行）、
`VsgRenderer.cpp` 的 `submitFrame`（131 行）都是同量级对象，但涉及缓存身份 / 顶点打包等更微妙的契约
（改动风险高于收益），以及 `vsg_selftest/main.cpp`（~4600 行）拆 TU —— 均留待专门一轮。

## 38. 结构整理七：目标 teardown 只写一处（2026-09-12）

判据同前（`[selftest]` 行逐字节相同 + VUID 0 / FAIL 0 + `test_vsg` 100 / `test_graphics` 158 +
门禁 PASS）。

**对象（重复的"破坏性拆解"）**：`buildOffscreenTarget`（重建分支）272 行与 `releaseRenderTarget`
（释放分支）266 行各自写了一遍**同一件破坏性拆解**：把该目标每个 pass 的 render graph 从命令图摘除 →
**等待设备** → 逐个 content slot `bridge.clearCache()` 并把它排队的编译 view 摘出队列。两处的注释甚至
互相指认（"see buildOffscreenTarget"）。

**做法**

- `Impl::unhookTargetPasses(Target&)`：上述三件事合成一个具名单位，两处各调一次。
  **为什么等待必须在这里**（§3 的分类）写在这一处：`clearCache()` 释放的是桥的共享对象注册表，其上
  的管线 / 采样器不一定还有存活节点作为唯一持有者 —— 第十批实测"停放 view"会报
  `vkDestroyPipeline-00765` / `vkDestroySampler-01082`，所以这里是计数等待，不是停放；非破坏性路径
  （换 render pass、丢 program 槽）仍旧 `Impl::retireObject()` 停放。原先指向 `buildOffscreenTarget`
  的注释改指这里，读者不再需要去别处找"为什么等"。
- `buildOffscreenTarget` 里**第四份**"槽的 view 记在哪个图"退化写法（局部 lambda `slot_graph`：pass 图
  查不到就退回目标图）与局部 `forget_view`（只有 program 槽分支用）删除，消费者槽的摘除改走既有的
  `Impl::detachSlotView()`（§31 的单点；顺带消掉一处死操作：只有 content 槽会排队编译，program 槽
  那次 `erase` 从来没有删除过任何东西）。等价性说明：目标级 `graph` 只在窗口目标被赋值
  （`VsgRenderer.cpp` 的 `entryFor(nullptr).graph = renderGraph`），离屏目标恒为空，故 `slotGraph()`
  的"查不到即 null"与原 lambda 的"退回目标图"在两种情况下结果一致。
- 深度共享 barrier 的构造抽成 `Impl::makeDepthShareBarrier(source)`：把"合并 depth/stencil 格式必须
  两个 aspect 都覆盖"（`VUID-VkImageMemoryBarrier-image-03320`）这条规则从 30 行内联块变成一处。
  同时把字段 `Target::depth_share_barrier` 从 `ref_ptr<Node>` 收紧为 `ref_ptr<PipelineBarrier>`
  （内部头新增 `<vsg/commands/PipelineBarrier.h>`；`Command` 本身就是 `Node`，所以命令图 children 处
  的隐式转换不受影响）—— 一个 barrier 不该在类型上被藏成"某个节点"。
- 删掉**死声明** `Impl::parkTargetObjects`：第十批把"停放"方案否决后函数体已删，声明与它那段声称
  "停放让这两条路径不用等设备"的注释却留着，与实测结论正好相反（§35 同类缺陷：重构残留）。

**行数**：`buildOffscreenTarget` 272 → **234**、`releaseRenderTarget` 266 → **253**；
`VsgRendererTargets.cpp` 1460 → **1423**，`VsgRendererPasses.cpp` 550 → **571**（新增
`Impl::unhookTargetPasses` 定义）。

**这次改动正好落在有断言的地方**：`policy churn:` 相位第二段每帧翻 target 附件形态 ⇒ 每帧都走重建的
teardown 路径，并断言"重建数 = frames-1、等待数 = 重建数"。等价性不是靠眼看：等待次数变了这一行就会变。

**顺带修掉一处外来残留**（缺陷，非本批次引入）：`tests/test_vsg/ProgramSamplingTest.cpp` 第 46 行出现
`}-10/2=`（编译错误），全仓扫描只有这一处，已修回 `}`；随后**全量重建**（8/8 步）再判定，避免用旧
二进制得出"测试通过"的假结论。

## 39. 可维护性整理：一条规则一处（2026-09-12）

判据同前（`[selftest]` 45 行逐字节相同 + VUID 0 / FAIL 0 + `test_vsg` 100 / `test_graphics` 158 +
门禁 PASS）。这一批的取舍标准从"函数太长"换成"同一条规则写在几处"。

**① 桥的哈希键（`SceneBridgePipeline.cpp`）**：`hashCombine` 的混合式（`0x9e3779b97f4a7c15` + 移位）在这
一个文件里抄了三份（L1 程序 stage 集 / L1b 每布局 ShaderSet / L2 变体模板），顶点布局哈希（种子
`0x517cc1b727220a95` + 遍历 `extra_channels`）抄了两份。现在是一个 `hashCombine()`、一个
`vertexLayoutHash()`、两个具名种子。**为什么值得合**：三份拷贝不同步不会报错 —— 它只是让缓存不再命中，
于是每帧重编译 / 重造管线，而没有任何诊断。`vertexLayoutHash` 是**模板**：`VertexChannel` 是
`SceneBridge` 的私有嵌套类型，文件内自由函数不能命名它，所以把通道区间当不透明参数收进来（与
`forEachSlot` 的 `requires` 手法同理）。同批把 MRT 那两条规则各自命名：`colourAttachmentCount(shader_set)`
（"一帧写几个颜色附件"取自槽 shader set 的混合状态）与 `applyOpaqueBlendForAttachments(states, n)`
（G-buffer 必须**不混合**写入：法线附件的 alpha 是 shininess/256≈0.125，混合会把存下来的法线缩到 12.5%）。
`buildStateGroup` 216 → **179** 行。

**② 顶点通道形状检查（`SceneBridgeGeometry.cpp`）**：同一套"1..4 分量 / 整除 / 顶点数相符"检查写了两遍 ——
一遍在调用处逐条 `report`、一遍在 `makeTypedVertexData` 内部当守卫（第二个永远为真）。现在
`channelShape()` 判定一次，`ignoredChannelMessage()` 一处出消息，`makeTypedVertexData()` 只声明前置条件
（`@pre channelShape(...) == Ok`）。`buildGeometryData` 225 → **202** 行。

**③ 缺陷（本批实测发现并修掉）：`formatDiagnostic` 的参数只为一条分支排序。** loc1 法线被拒时的报告用
`unpack == NotXyzStride ? u8"...%u..." : u8"...%zu...%u..."` 选格式，却按第一条分支的顺序传了
`(components, size, components)` ⇒ **第二条分支打印的是"holds <components> floats, not divisible by its
components=<size> stride"**（数字互换、`%zu` 读 32 位值）。它编译、运行、验证层干净，只是消息在撒谎。
修法：两条分支各自一个 `ignoredNormalChannelMessage()` 调用，各传自己的数。**系统性排查**：写了一个
`formatDiagnostic` / `V_LOG*` 的参数计数检查器（三元格式**按分支**核对，否则这类"顺序错"根本看不见，
因为它不缺参数），对插件 14 个文件扫描 **0 命中** ⇒ 这一类缺陷只剩这一处。检查器已入库：
`scripts/check_diagnostic_formats.py`（默认扫插件源码，有怀疑则退出码 1，可挂到门禁）。

**④ 会话初始化（`VsgRenderer.cpp`）**：`initialize()` 里 38 行窗口 traits 构造（默认尺寸、验证层开关、
两个可选设备特性、平台相关的宿主窗口句柄转换）搬成文件内 `makeWindowTraits(host_handle)`，"哪个设置为什么
要"跟着设置走；`initialize` 137 → **104** 行，读作"清旧会话 → 建窗口 → 三套 depth 策略 shader set →
viewer/命令图 → 首次编译"。

**有意不做**：`makeNormals` 与 `makeIndexedNormals` 里"退化（零长）法线保持零"的写法没合并 —— 前者用
倒数乘（`1/sqrt`）+ 乘法、后者用 `vsg::normalize` 的除法，合并必然改掉最后一位比特，而像素值在证据行里
⇒ 这是行为改动，不值得为省两行承担。`initialize()` 的 `try {` 块体没有内缩（排版问题，历史上就有），
纯空白改动会淹没 diff，同样留给专门一轮。

**行数汇总**：`buildStateGroup` 216→179、`buildGeometryData` 225→202、`initialize` 137→104；
三个 TU 分别 641→700 / 473→539 / 965→981（新增的 Doxygen 比省下的代码长，收益是单点定义与那处缺陷）。

## 40. `passGraph` 拆分（2026-09-12）

**先分析**：`passGraph` 是全插件最长的函数（270 行），先按"职责"数了一遍 —— ①窗口/未构建守卫 ②取附件集
③算本 pass 的清屏请求 ④算深度是否仍在提升布局 ⑤`planPassVariant()` 定变体 ⑥稳态快速路径（顺序变更 +
`passVariantIsStale` + 清屏值更新）⑦设备守卫 ⑧清屏颜色取值 ⑨撤销提升 + 丢弃采样该深度的 program 槽
⑩materialise 变体（transient 时两次 `makePassObjects`）+ 停放被换下的对象 ⑪新建 RenderGraph + 清屏值布局
⑫写 renderPass/framebuffer/清屏值 ⑬填 `PassObjects` 并入库 ⑭目标级不变量 ⑮命令图插入 + 重排 ⇒ **9 个
职责**；270 行里 **约 110 行是解释性注释**（决策理由，保留）。结论：**能拆**，且拆法有现成范式 ——
本文件已经用 `planPassVariant()`（纯决策）/ `passVariantIsStale()`（纯判据）把"决定"从"施工"里分出来。

**本批做掉三步（每步单独验证）**：

1. `Impl::makePassGraph(t, has_depth, clear_color)`：一个 pass 一个 RenderGraph（§28），**清屏值按 framebuffer
   的附件顺序**——颜色附件在前、深度最后；attachment 0 放本 pass 自己的清屏色，额外的 MRT 附件保持透明黑，
   深度项放目标的深度清屏值（每个目标的 reverse-Z 远平面）。这条"清屏值布局"规则原先内联 28 行。
2. `Impl::reuseSteadyPass(objects, want_color_clear, want_depth_clear, has_color, clear_color)`：稳态帧的
   全部开销（`passVariantIsStale` 判据 + 清屏值更新），返回 null 表示"清屏策略变了，要重建"。**"clear value 0
   是颜色项只在目标真有颜色附件时"**这条推理（深度目标的那一项是深度值，`VkClearValue` 是 union）随之成为
   该函数的文档。
3. `Impl::publishPass(t, key, objects, has_color)`：填好的 `PassObjects` 入库 + 目标级不变量
   （`color_seeded` / `depth_seeded` / `any_load_pass` / `depth_sampleable` 的撤销提升规则）。

**踩坑（本批第一次编译就暴露的约束）**：`reconcileOffscreenOrder()` 是 `VsgRenderer` 的成员（命令图只有它
有），而 `Impl` 是嵌套类型、没有外层的 `this` ⇒ **两个 `Impl` helper 不能调用它**。于是"顺序变更 → 重排"和
"新 pass 图入图 → 重排"留在 `passGraph`（`VsgRenderer` 层的动作），`Impl` helper 只做本地记账；两处注释
写明分工。

**结果**：`passGraph` 270 → **210** 行，读作"守卫 → 计划 → 稳态复用或重建 → 发布"；
`VsgRendererTargets.cpp` 1423 → 1425（三个 helper 的 Doxygen 抵掉省下的代码）。

**仍剩下的（下一步候选，本次未做）**：把"决定"整体打成一个 `Impl::PassPlan` 值（`planPass(t, key, target)`
返回 {附件集, 变体, 已记录对象指针, has_color/has_depth, 两个清屏请求, color_clear, depth_load}），并把那两段
决策理由（load-op 策略、提升状态）搬到它的文档里 —— 预计 `passGraph` 再降到 ~135 行，"决定"与"施工"彻底
分家。**风险高于本批**：`current` 指针（指向 `t.passes` 内）与 `planPassVariant` 读取的目标级标志
（`any_load_pass` / `depth_seeded` / `color_seeded`）都必须在"发布"改写它们之前取值，顺序错会静默改变
一个 pass 的 load-op。留待单独一轮。

### 40b. 决定与施工分家（同批续做，2026-09-12）

按上面那条候选做完：新增 `Impl::PassPlan`（值）+ `Impl::planPass(t, key, target) const`（**纯决策、无副作用**，
把两段决策理由搬进它的文档），`passGraph` 只剩"取材 → 施工"。**取值顺序是这一步的全部风险**，所以把
`planPass` 的文档写成契约：`current` 指向目标的 pass 表 ⇒ 必须在 `publishPass` 之前取；`planPassVariant` 读的
`any_load_pass` / `depth_seeded` / `color_seeded` 正是"发布新对象"会改的 ⇒ 计划必须先于撤销提升与发布完成。
`passGraph` 里 `has_color`/`has_depth` 两个中间变量删掉，统一读 `plan.*`（一份真相）。

**踩坑**：`PassObjects` 是 `Target` 的嵌套类型 ⇒ `Impl::PassPlan` 里必须写 `const Target::PassObjects*`（第一次
编译即报 `unknown type name 'PassObjects'`）。另一个约束沿用 §40：`reconcileOffscreenOrder()` 是 `VsgRenderer`
成员，`Impl` 调不到，所以"顺序变更 / 新图入图 → 重排"仍在 `passGraph`。

**结果**：`passGraph` **270 → 164** 行（两批合计 −39%），TU 1425 → 1410；`planPass` 58 行、`makePassGraph` 30、
`reuseSteadyPass` 18、`publishPass` 22 —— 每一步都是一条能单独读、单独说的规则。验收同前（`[selftest]` 45 行
逐字节相同 + VUID 0 / FAIL 0 + test_vsg 100 / test_graphics 158 + 门禁 PASS）。

## 41. `buildOffscreenTarget` 拆分（2026-09-12）

对象 202 行。这里的范式不是"计划/施工"（它的"决定"是 `resolveDepthBorrow()`，早就抽成**会报告**的纯函数），
而是**按"一个 builder 拥有什么"命名**：

1. `Impl::resetTargetAttachments(t)` —— 重建的第二半（第一半是 `unhookTargetPasses`）：**列清一次 build 拥有
   的全部东西** —— 三张槽表、颜色 / 深度图像与视图、`passes`、`attachments_built` / `depth_seeded` /
   `any_load_pass` / `color_seeded` / `depth_sampleable` / `depth_borrow_pending_reported`、借用的源与视图与
   barrier、`graph`、三套 depth 策略 shader set、宽高、`build_key`。**为什么要写成一个函数**：这份清单就是
   build 的所有权边界 —— `Target` 以后加一个字段而这里忘了重置，会以"残留图像 / 残留 built 标志 / 残留借用源"
   的形式活过重建，没有任何断言会报（第九批的 `depth_borrow_pending_reported` 就是靠改这里加进去的）。
2. `Impl::dropConsumersSampling(target)` —— "重建的一方把旧视图换掉了，而消费者的过期检查只看源**尺寸**"这
   条规则：同尺寸重建时消费者仍握着旧视图，会一直采样一张没人再画的图。消费者靠**槽属性** `source_target`
   找（槽的 key 是拥有它的 pass，不是它采样的目标），摘除一律走 `detachSlotView()`。
3. `Impl::createTargetAttachments(t, device, target, w, h, depth_src)` —— 每个附件的图像 + 视图，外加深度
   （自有，或借用源的图像）。把两条只在设备上才发作的规则写成文档：**usage 标志**（颜色附件同时要
   `SAMPLED`/`TRANSFER_SRC`；深度没有 `TRANSFER_SRC` 连 `TRANSFER_SRC_OPTIMAL` 都到不了，
   `VUID-VkImageMemoryBarrier-oldLayout-01212`）与 **必须走 `createImageView()`**（否则句柄是
   `VK_NULL_HANDLE`，从它建出的 framebuffer 带脏句柄，只在 `vkCmdBeginRenderPass` 里崩）。

**结果**：`buildOffscreenTarget` 202 → **89** 行，读作"守卫 → 拆旧（两半）→ 尺寸 → 算宽度/颜色数/深度 → 借用
判定 → 建附件 → 目标级事实 → 丢消费者 → 借用 barrier → build key / 日志 / 计数"。TU 1410 → 1399。

**踩坑（本批两次编译错）**：①`targets` 表以**非 const** `RenderTarget*` 为键 ⇒ `createTargetAttachments` 的
`depth_src` 形参不能收 `const RenderTarget*`（`std::map::find` 报 "would lose const qualifier"）；② 中途给调用点
误加了一句 `if (impl->targets.empty()) return;` —— 它会**提前返回整个函数**，把借用 barrier / build key / 日志
全跳过（`targets` 也不可能为空：调用者的目标已在表里）。编译不会报这种错，是自查时发现的：**"加一个守卫"
之前先想清楚 `return` 会不会跳过后续步骤**。

## 42. `submitFrame` 拆成帧协议（2026-09-12）

对象 130 行，内容是**六步顺序**，每一步的理由都写在它自己那一大段注释里 —— 也就是"顺序是内容，而内容被注释
淹了"。拆成五个具名步骤（都是 `VsgRenderer` 私有成员、无参数，声明在公开头的私有区，与该区既有 helper 的
风格一致）：

| 步骤 | 行数 | 保证 |
|---|---|---|
| `releaseAbandonedTargets()` | 14 | 宿主没打招呼就丢掉的目标（表持有所有权 ⇒ 再也查不到它）在这里被正常释放 |
| `reportSessionDevice()` | 17 | 首次提交时把"跑在哪个驱动上"记到日志（门禁通过只在这个前提下有意义） |
| `compilePendingViews()` | 19 | 本帧槽同步排队的视图先编译（D22 增量 + `VINE_VSG_DISABLE_INCREMENTAL_COMPILE` A/B 逃逸 + 失败回落全图编译） |
| `settleTransientPassVariants()` | 25 | 记录了一次性变体的 pass **提交之后**才换回稳态变体（提前换会让它的第一帧 LOAD 一张还没转换的图像） |
| `releaseParkedObjects()` | 20 | 提交完成 ⇒ `kRetireRingDepth` 帧前停放的对象（每个内容槽的桥环 + 渲染器自己的环）可以释放 |

`submitFrame` 于是变成**协议本身**：130 → **35** 行，读下来是
"丢被抛弃的目标 → 记设备 → [未初始化就返回] → 退掉本帧没跑的 pass → 编译 → 提交 + 呈现 → 结算一次性变体 →
释放停放对象 → 释放被丢弃材质的资源"，每一步一句，顺序一眼可见。

**行数账**：函数体 130 → 35，但 `VsgRenderer.cpp` 981 → 960、`VsgRenderer.hpp` +55 —— 五段理由从函数体内
搬进了声明处的 Doxygen（该文件私有区的既有 helper 都是这种写法）。**收益是 `submitFrame` 不再被理由淹没，
而不是总行数**。

**踩坑（值得记，非代码问题）**：第一次编译时 clang **崩了**（frontend exit 135，栈顶报在
`VsgRenderer.hpp:620` 一个注释里的 "annotation token"）。按插入位置逐行看过注释内容没有任何可疑构造，
直接重跑同一条命令即通过 ⇒ **编译器的一次瞬时崩溃**（同机并行编译的内存压力），不是代码问题。教训：
`Stack dump` 形式的失败先重跑一次再排查，但**不能就此当作通过** —— 重跑干净后仍然走完整判据
（证据逐字节相同 + VUID 0 / FAIL 0 + 两套单测 + 门禁）。

### 42b. "单调用点 helper"审计（同批，由提问引出，2026-09-12）

**问题**（用户提出）：`submitFrame` 拆出来的方法还有别处调用吗？将来可能吗？会不会增加复杂度？

**审计（实测）**：五个方法各 **1 个调用点**，全部在 `submitFrame()` 内（对照：既有的
`retireInactivePassSlots` 同样 1 个）。本批**没有产生死代码**。但它们不是一类：

1. **有真实复用潜力的一个** —— `compilePendingViews()`：它处理的是一个**被别处也碰**的队列
   （`renderContentSlot` 往里 push、detach/teardown 往里 erase）；将来若出现"需要提前编译"的入口
   （同步预热、readback 前要管线已建），或出现第二条提交路径，它就该被共享。
2. **"帧已提交"这个事件的（原为两个）** —— 变体结算 + 停放释放：任何未来的提交者**都必须**调它们，
   复用形态是"契约共享"而非可选。
3. **契约上就该单调用点的两个** —— `reportSessionDevice()`（每会话一次，函数内已有标志守卫 ⇒ 多调是
   no-op）、`releaseAbandonedTargets()`（规则挂在**帧边界**上，而帧边界只有一处；多调只是空扫）。

**风险与实证**：单调用点 helper 的真实代价是**静默腐烂**。本仓库有先例：`Impl::parkTargetObjects` ——
方案被实测否决、函数体删除，而声明与那段"停放可以让这两条路径不必等设备"的注释又活了好几批（与实测结论
相反），直到 §38 才清掉。

**处理：不改结构，改可发现性** —— 给五个方法各写一条**前置 / 幂等契约**：顺序类写 `@pre`（编译必须早于
本帧的记录；结算必须晚于 `recordAndSubmit()` + `present()`），幂等类写明可重复调用（"多调是 no-op"）。
比在文档里写"只被调用一次"更稳 —— 事实型断言本身会腐烂，而前置条件是调用者真正需要的东西。

**并按建议合并"提交之后"那一对**：`settleTransientPassVariants()` + `releaseParkedObjects()` →
**`settleSubmittedFrame()`**。理由是两步**由同一事件触发**（帧已呈现）：合并后无法只做一半（早结算会毁掉
正在记录的那一帧；一帧推两次环会早一帧释放）。名字 5 → 4。`submitFrame` **33** 行，读作
"丢被抛弃的目标 → 记设备 → [未初始化返回] → 退掉本帧没跑的 pass → 编译 → 提交 + 呈现 → 结算已提交的帧 →
释放被丢弃材质的资源"。

**反向观察（同样重要）**：长得像但**保证不同**的规则不该合并。`releaseRenderTarget` 也有"丢弃采样某目标的
槽"的循环，但它**每个槽要一次计数等待**（破坏性），而重建路径**不能等**（§38 实测结论）—— 合并就是把两种
保证藏进参数。这种"看起来能复用、实际必须分开"的地方，比单调用点 helper 更值得警惕。

## 43. `render()` 只取一处（2026-09-12）

先说**判断**：115 行里绝大多数是"取状态 → 判定 → 交给槽"，真正值得抽的只有**一处** —— 两段深度借用判定。
其余（`ContentSlotRequest` 的 12 行结构体填充、`retargetPass` 前后）都是机械搬运，抽了只是把行数挪个地方。

**做法**

1. `Impl::borrowNeedsRebuild(t, target_key) const`：两条**互不相同**的"借用失效"判据合成一个纯函数，理由全部
   写进它的文档 ——
   * **PENDING**：请求的借用还没能兑现（构建时源还没有深度图像），因此已烘入的借用与请求的不同，等到源有
     图像就重试；而**永久不可用**的源被记成 `unusable_depth_source`，所以只在"仅仅在等"时重试（一个禁用 /
     从未构建的产出方每帧只花一次查表，而不是重建循环）；
   * **STALE**：已兑现的借用绑的是源的深度 **view**，而源被重建（尺寸变化，或"同一判据为它自己监视的
     深度策略变化"）会换掉深度图像 ⇒ 借用方的 framebuffer 会继续测那张没人再写的旧图像，**借来的深度静默
     冻结**而旧图像一直活着。用"源当前 view vs 本目标烘入时记下的 view"检测它，重建时会重新跑一遍借用校验。
2. **删掉重复的注释**：`render()` 里那段 20 行的 scope 属性说明，其逐条内容在各自的 setter
   （`setPassOrder` / `setDepthMode` / `setRenderTarget`）**已经写过一遍** ⇒ 只留"READ 而不是 consumed、整
   个 scope 看到同一组值、含义见各自的 setter"，并把 render() 特有的那句（每个目标共用一条槽路径、GPU
   附件在这里确保）压缩进去：20 → 8 行。

**结果**：`render()` 115 → **81** 行，读作"守卫 → 取每绘制调用状态 → 取 scope 属性 → 目标可用性 → 重定向
旧槽 → 借用/重建判定 → 画槽"。TU 960 → 926。

**踩坑**：又是 `targets` 表的键类型 —— 判据里 `wanted_source` 不能声明成 `const RenderTarget*`（`map::find`
报 lose const qualifier），必须用普通指针（SDK 的 `depthSource() const` 本身就返回普通指针）。

## 44. 去掉 `VsgRenderer` 的 PImpl（2026-09-12）

**问题**（用户提出）：这个插件不是 SDK、不会被直接引用，PImpl 还有必要吗？

**先看事实**：`vine/vsg/VsgRenderer.hpp` 的消费者只有插件自己的 6 个 TU + `vsg_selftest/main.cpp` +
`tests/test_vsg/PassProtocolTest.cpp`；插件是 `v_add_plugin`（MODULE DLL），宿主通过
**`vine::graphics::RenderBackend` 这个 SDK 接口**拿渲染器 —— `VsgRenderer.hpp` 不在任何部署边界上。
于是 PImpl 的常规理由逐条落空：

| 理由 | 结论 |
| --- | --- |
| 外部消费者的 ABI 稳定 | 不成立（没有外部消费者；同仓构建） |
| 编译防火墙 | **半破**：公开头本来就 include 了 `<vsg/app/Viewer.h>` 并在私有签名里写 `::vsg::ref_ptr` ⇒ vsg 早就漏进了"公开"面 |
| 隐藏 1118 行会话态 | 成立，但代价是下面那条规矩 |

**代价（都在本会话真实付过）**：需要状态类型的 helper **不能**声明在公开头 ⇒ 必须有"`Impl` 成员 vs
`VsgRenderer` 私有成员（签名里不出现 `Impl` 类型）"这条规矩（逼出 §37 把请求拆成 7 个字段、§36 用模板而非
重载、§40b 里 `PassPlan` 必须写 `const Target::PassObjects*`）；204 处 `impl->` 的间接；§42 的实现理由不得
不写进"公开"头的私有区。

**做法（一次性、机械）**：类定义并入 `src/VsgRendererImpl.hpp`（公开头删除），状态改为**按值**成员 ——
`Persistent` 与 `Impl` **保留拆分**（这是生命周期语义：前者跨会话，后者是一个窗口会话、`shutdown()` 整体
替换），只是不再经过指针。改写：217 处 `impl->` → `impl.`、17 处 `persistent->` → `persistent.`、构造改
`= default`、会话重置 `impl = std::make_unique<Impl>()` → `impl = Impl{};`、8 个 include 点改指内部头。

**它暴露出来的东西（值得单独记）**：`unique_ptr` 的 `operator->` **在 const 方法里也返回非 const 指针** ⇒
去掉之后 `detachedSlotCount() const` 这类只读访问器立刻编译失败：它们原来在**非 const** 地走槽表。补一个
`const` 的 `forEachSlot` 重载后，这些计数器才真正是 const 的。也就是说 PImpl 一直**掩盖着 const 正确性**：
一个声明为 `const` 的查询方法本可以改会话态。

**过程教训（本批真实踩到）**：**不要对"脚本刚生成的、缩进层级变过的文件"打补丁** —— 第一次尝试之后我用手写
替换加 `const` 重载，锚点按的是旧缩进，结果**静默吃掉了相邻的 `visitSlot` 声明**（编译器报 300 个错，最早的
一个指向 `struct Target`）。正确顺序是：**先把预期增量打在合并前的干净源上，再跑合并脚本**（缩进由脚本统一
处理）。已回滚重做，一次通过。

**验收**：`[selftest]` 45 行逐字节相同、VUID 0 / FAIL 0、`test_vsg` 100 / `test_graphics` 158、门禁 PASS、
`check_diagnostic_formats.py` 0 命中。改动 11 个文件 +2171/−2183（两个头文件合成一个，访问点全改）。









