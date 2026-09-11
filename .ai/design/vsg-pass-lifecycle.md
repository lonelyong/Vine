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
