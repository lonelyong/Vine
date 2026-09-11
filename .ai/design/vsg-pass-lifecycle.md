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

未做（需单独排期，均已在下方说明理由）：

- **VkPipelineCache 持久化 / PSO 磁盘缓存**：vsg 的 `GraphicsPipeline::compile` 传
  `VK_NULL_HANDLE`，Vine 无法从外部注入；需等 vsg 支持或自行接管管线创建。
- **帧在飞（frames-in-flight）资源回收队列**：当前销毁路径用 `vkDeviceWaitIdle`（已保证正确与
  validation 干净），但代价是整帧停等。业界做法是 N 帧在飞 + per-frame fence / 延迟删除队列。
- **命令列表缓存（跨 pass 复用）**：`Scene::collectRenderCommands` 每个 pass 每帧全树遍历，
  多 pass 场景仍是 O(passes x nodes)；业界会缓存一次遍历结果供多 pass 复用。
  （单次遍历内部的 O(depth^2)/O(n·depth) 与重复 bbox 已在本轮修掉，见下）
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

