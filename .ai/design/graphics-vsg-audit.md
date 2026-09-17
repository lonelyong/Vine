# graphics / vsg 后端审查（2026-09-15 轮次）

本轮是**代码走查 + 修复**：对 `src/viz/graphics`（引擎 SDK）与 `src/plugins/gfx_backend_vsg`（vsg 后端）
做了一次通读式审查，逐条修复，并为每条修复补了门禁（单测 / 像素证据 / 变异验证）。

判据：构建 0 error 0 warning；`test_graphics` 260 → **269**；`test_vsg` 276 → **288**；
`scripts/vsg_selftest_evidence.sh` **55 行逐字节不变**（本轮的着色器、相机、收集、视口、池、视图校验改动
全部行为中性）；`scripts/vine_shader_check.sh` 在本环境**无法运行**（只有 Windows 版
`glslangValidator.exe`，无 Linux 二进制）——着色器变体的编译由 `test_vsg` 的 `GlslCompileTest`
（vsg 内嵌 glslang）与 selftest 的 lavapipe 运行覆盖。

## 1. 评查结论：先排除的"看似可疑但正确"

| 检查项 | 结论 |
| --- | --- |
| `Frustum::fromViewProjection` 的 `r3±r2` | 正确。`vine::math::perspective` 是 GL 风格 clip space（z∈[-1,1]），near 面 = `r3+r2` |
| 深度比较算子取反（`Less → VK_COMPARE_OP_GREATER`） | 正确。vsg 的 `perspective/orthographic` 确为 reverse-Z + Y 翻转 |
| `makeScenePipelineStates` 的 `blendEnable=FALSE` / `cullMode=NONE` | 不是缺陷：它只是 set 的默认值，`buildStateGroup` 随后用 `makeRenderStateObjects` 整体替换 |
| `collectRenderCommands` 的 memo 键用矩阵逐元素比较 | 正确（纯函数复用，非指针键是有意为之） |

## 2. 本轮修复（缺陷 → 证据 → 修复）

| 编号 | 缺陷 | 证据 / 复现 | 修复 |
| --- | --- | --- | --- |
| **B1** | **材质 alpha 与文档矛盾，且不参与透明分类**：`Material.hpp` 写"diffuse alpha is ignored"，而 `builtin_forward.frag` 算 `alpha = material.diffuse.a * draw.params.x`，且 content pipeline 的 blending 恒开 ⇒ 一个共享材质会把**所有**使用它的 drawable 变半透明，而引擎按**每 drawable 的 opacity** 排序（`Scene.cpp` 的 `isTransparent`），并且延迟路径把 alpha 丢掉（点亮程序写 `alpha 1`）⇒ 同一份资产在 forward 半透明、在 deferred 不透明 | 代码对照 + `builtin_forward.frag:104` | 前向着色只读 `draw.params.x`：**引擎只有一条透明度通道**（每 drawable 的 opacity），纹理只贡献颜色。门禁：`ShaderAbiTest.TheForwardFragmentsAlphaIsTheDrawablesOpacityAlone`、`ForwardShaderSetTest.TheFragmentStageScalesAlphaByTheDrawBlock`（变异：把 `material.diffuse.a *` 加回去 ⇒ 两条都红）；证据 55 行不变（selftest 的材质 alpha 全是 1.0、纹理 alpha 全是 255） |
| **B2** | **正交相机裁剪/渲染不一致**：`Camera::setProjectionMatrixAsOrtho` 只保留 `top-bottom`，后端 `CameraBridge` 用 height 重建**居中**视锥 ⇒ 离屏/平铺/立体单眼这类**非对称**窗口，CPU 按偏移视锥剔除、GPU 渲染居中视锥 | 代码对照（`RenderPipelineBuilder` 自造的阴影/HUD 相机都是对称窗口，所以一直没暴露） | `Camera` 保留完整窗口（`orthographicLeft/Right/Bottom/Top`），后端按四个边界建 `vsg::Orthographic`；拾取射线同源使用同一窗口。门禁：`CameraTest.TheOrthographicWindowIsKeptWhole`、`CameraTest.TheOrthographicPickingRayTravelsAcrossTheWindow`、`CameraBridgeTest.*`（新套件 4 条） |
| **B3** | **`Camera::projectionMatrix()` 与 GPU 实际投影不是同一个矩阵**（GL 风格 z∈[-1,1] vs vsg reverse-Z + Y 翻转），文档没有任何说明 ⇒ 宿主用它算"世界→屏幕"会上下镜像 | 代码 + 文档缺口 | `Camera.hpp` 类注释写明该矩阵是 **CPU 侧**约定，后端可以（并且确实）用另一套 clip 约定；`projectionMatrix()`/`screenToWorldRay()` 的 Doxygen 同步 |
| **B4** | **退化 up 向量使拾取射线成 NaN**：`screenToWorldRay` 自己 `cross(forward, up)`，当 up 与视线平行时除零（`lookAt` 内部有"挑参考轴"的兜底，射线没有） | 代码对照（`Transform3.cpp` 的 lookAt 兜底分支） | 射线基改为**直接读视图矩阵的行**（right/up/backward）⇒ 与渲染同源，退化情形由 lookAt 兜底决定。门禁：`CameraTest.ThePickingRayIsBuiltFromTheViewMatrixBasis` |
| **B5** | **每帧全量重建接线校验**：`RenderEngine::frame` 每帧 `validateWiring()`，内部为"谁填哪个 target / 谁声明哪张图"建 map/set（O(pass × attachment) 次分配），而注释自己写"这是**声明**的属性，不是帧的属性" | 代码走查 | 改为**声明驱动**：`RenderPass::wiringRevision()`（每个接线相关 setter 自增）+ 宿主 `publish/unpublish` 标记；只在变化时校验，并新增可观测计数 `RenderEngine::wiringValidationCount()`。门禁：`RenderEngineTest.TheWiringIsCheckedWhenADeclarationMovesAndNotPerFrame`（稳态帧计数不变；in-place 改 target / 加删 pass / publish / unpublish 各 +1；in-place 引入的**新**问题仍被报出且只报一次） |
| **B6** | **每个内容槽每帧新建 `vsg::ViewportState`**：稳态帧分配 + 对象身份每帧变（vsg 的状态比较） | `VsgContentSlot.cpp` 的 `updateSlotViewport` | 槽保留**一个** state，矩形不变就早退，变了就**原位**改 `viewports[0]`/`scissors[0]`；`VsgRenderer::resize` 走同一个实现。门禁：`ContentSlotViewportTest.*`（3 条：只建一次且复用、公告的矩形（含 clamp）对每个 pass 都算数、0 尺寸不写状态） |
| **B7** | **局部包围盒每帧全顶点扫描**（backlog P3）：`BoundsCache` 保证"每个叶子只算一次盒"，但父节点的盒需要**所有**子节点的盒 ⇒ 求 root 的盒就必须扫**全场景**顶点，且相机动则 memo 必 miss ⇒ 每帧 O(全部顶点)，视锥剔除在 CPU 侧一分钱不省 | 代码走查（`Scene.cpp` 的 `worldBound` 递归 + `Geometry::localBounds`） | `Geometry` 缓存**局部**盒，键 = positions 缓冲指针 + **该缓冲的 revision** + 段（offset/scalarCount）+ **geometry revision**；世界盒仍每次派生。新增可观测计数 `Geometry::localBoundsComputationCount()`。门禁：`SceneTest.TheLocalDataBoxIsComputedOnceAndRecomputedWhenTheDataChanges`（变异：去掉缓冲 revision 键 ⇒ 红） |
| **B8** | **每个 geometry 每帧 3 次祖先链上行 + 一次堆分配**：`effectiveMaterial` / `collectRenderState`（每次都 `std::vector` 存整条祖先链）/ `effectiveProgram` | 代码走查 | 收集遍历改成**自顶向下折叠**（`InheritedState`：state 逐层 merge、nearest 材质/程序逐层覆盖），O(1) 每节点、零分配；`StateNode.hpp` 的上行版保留为 SDK 拼写。防漂移门禁：`SceneTest.TheFoldedStateAgreesWithTheUpWalkingHelpers`（同一棵含两层 StateNode 的树，fold 结果与 `effectiveRenderState/effectiveMaterial/effectiveProgram` 逐项相等） |
| **B9** | **命令表每 pass 复制一次**：memo 命中时 `return hit->commands`（N 条命令 = 3N 次原子引用计数 + N×sizeof(RenderCommand) memcpy），而 pass 级 program override 是唯一真正需要改写副本的调用者 | `Scene.cpp` + `RenderPass.cpp` | 新增 `Scene::collectRenderCommandsShared()`（返回 `shared_ptr<const vector<RenderCommand>>`，**不可变、帧内有效**）；pass 路径只用引用，**只有设了 program override 的 pass** 才 fork 一份。`collectRenderCommands()` 保留（拥有一份副本的拼写）。门禁：`SceneTest.TheSharedCollectionIsOneListAndTheOwningSpellingCopiesIt` |
| **B10** | **拾取矩阵求逆无守卫**：`Matrix4x4::inverted()` 对奇异矩阵**返回原矩阵**（不报错）⇒ 射线在一个根本不是该几何体的空间里求交，静默给出**假命中** | 变异验证：去掉守卫后 `RayIntersectionTest.ASingularWorldTransformIsSkipped` 报 `Actual: true`（假命中） | 改用 `invert()`（返回 bool）并在失败时跳过该几何体（"没有体积可以相交"），公共文档写明。门禁：上述测试（另含"极小但非零缩放仍能命中"的正对照） |
| **B11** | **每 drawable 槽可被重复归还**：`VsgDrawBlockPool` 的自由表是裸 `vector`，第二次 `release()` 会把同一 index 压两次 ⇒ 之后两次 `reserve()` 把**同一块 mapped 内存**交给两个 drawable，互相覆盖对方的 opacity，且无任何报告 | 代码走查 + 变异（去掉 in-use 检查 ⇒ 测试红） | 槽的"空闲/在用"记账收进 `VsgDrawBlockPool::SlotAllocator`（无设备、可单测）：重复归还会被**拒绝**并计数（`Stats::refused`），且**拒绝发生在写字节之前**（否则会清掉现在拥有该槽的 drawable 的 opacity）。门禁：`DrawBlockPoolSlotsTest.*`（5 条） |
| **B12** | **ABI 承诺了没人兑现的字段**：`VineMaterialBlock` 的 `emissive`/`alphaMask`/`alphaMaskCutoff` 被两个 shader 声明、无人填、无人读；`LightPushBlock::projparms` 在延迟着色里从不被读（该程序采样 G-buffer 的位置附件） | 代码走查 | 三个材质字段**删除**（ABI 80B → 64B，`static_assert` 与 `ShaderAbiTest` 同步）；`projparms` 保留但**在 C++ 与 GLSL 两侧都写明是"给自建深度重建程序预留"**，并说明引擎自己的程序不读它。行为中性：证据 55 行不变 |

## 3. 建议（尚未实施，按收益排序）

这些是架构级改动，**需要先有实测数字**（见 `.ai/memory/graphics-perf-backlog.md` §2）。本轮只把
"能安全落地且行为中性"的部分做完了。

1. **实例化 / 间接绘制**：现在每个 drawable 一条 bind 集 + 一条 `DrawIndexed`。共享机制（`VsgMeshResourceCache`、
   变体缓存）已经在，下一步是把 `(program, material, state, mesh)` 相同的 drawable 合成
   `VK_EXT_multi_draw` / `vkCmdDrawIndexedIndirectCount`，把一帧数千次调用降到数十次。
2. **GPU 剔除 + 遮挡剔除（HiZ）**：CPU 侧现在有局部盒缓存（B7），下一步是层次化包围球 → compute 剔除 →
   间接绘制（与 1 同一套缓冲）。工作单元/机器人场景遮挡关系强，HiZ 收益预计可观（**估算**）。
3. **透明度**：让 `blendEnable` 进入变体身份 + 用 alpha-to-coverage（MSAA）或加权混合 OIT，取代
   "blending 恒开"。当前恒开的不透明内容白付混合带宽、并影响 early-Z。前提是先把"分类"做对
   （B1 已把语义收敛到"opacity 是唯一通道"，扩展时一并把分类、排序键、变体身份三者一起改）。
4. **内存预算**：`VsgRetentionStats` 现在有 `slot_bytes`/`mesh_streams`/`textures`（B 轮补），下一步是
   mesh/纹理缓存的**字节**统计与可配置上限；依据是"同一 mesh 被 k 个实例、同一纹理被 N 个槽采样"的实测。
5. **多视图 / 多线程**：SDK 契约是"一帧一线程、pass 串行"，VR 立体声或大场景需要 per-view 并行录制；
   `shared_objects_` 因 render pass 变体不能跨槽共享（已实测，见 `vsg-pipeline-sharing.md`），
   并行化时按 (pass 变体) 细分共享表。

## 4. 工程教训（本轮踩到并记下的）

- **改了 `libviGraphics` 里的类布局后，只 build 目标会留下陈旧二进制**：`RenderPass` 加了一个私有字段，
  只 build `test_graphics`/`test_vsg` 后跑证据脚本，`vsg_backend_selftest` 仍是旧布局 ⇒ 结尾
  `malloc(): largebin double linked list corrupted`。**证据门禁前必须整包 build**（`Build` 不带目标）。
- **本环境的 `http(s)_proxy` 是坏的**（`127.0.0.1:7890` 会在 TLS 握手中断连），而**直连正常**：
  CMake reconfigure 时 FetchContent 去 `git fetch` spdlog / `git push` 都会报 `GnuTLS, handshake failed`。
  绕过方式：`env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY git push ...`；
  构建目录（`build/CMakeCache.txt`，gitignore）里已设 `FETCHCONTENT_FULLY_DISCONNECTED=ON`（依赖已就位，无需再取）。
- `vine_shader_check.sh` 需要 Linux `glslangValidator`（本环境只有 Windows 版），无法运行；
  着色器变体编译由 `test_vsg` 的 `GlslCompileTest` 覆盖。
