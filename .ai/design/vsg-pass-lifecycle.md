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

## 9. 待办设计登记（3 项；2026-09-11 记录，尚未实施）

本节只做**设计登记**（问题、根因、方案、验收、风险、依赖），实施排期与优先级在各条目末尾。
三项都属于“非正确性阻塞”但会被真实场景触发的缺口，登记目的是让后续切片有明确的入口与验收口径。

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
| `include/vine/vsg/VsgRenderer.hpp` | 类声明（公开契约 + 私有嵌套类型） | 632 |
| `src/VsgRendererImpl.hpp` | `Persistent` / `Impl`（会话态：窗口、viewer、命令图、目标表、槽、pass 请求） | 319 |
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
