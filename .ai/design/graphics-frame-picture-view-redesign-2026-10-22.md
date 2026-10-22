# graphics 重设计：帧 / 画面 / 视图（2026-10-22 提案）

> **状态**：提案，**未落地**。它取代 `graphics-skeleton-review-2026-10-20.md` 对 R2 的实现形状，并与
> `graphics-object-model-audit-2026-10-21.md` 的 R2 行、`graphics-followups-2026-10-19.md` §3.9（公开面收口）
> 指向同一件事：**把"一帧的机制"和"一张画面的配方"彻底分开，并把注册的对称操作放回引擎**。
> 按用户 2026-10-22 的要求，这一版**从总体框架出发、允许破坏性改动**，切片只是落地顺序，不是妥协。

## 0. 一句话

今天有四个大对象（`RenderEngine`、`RenderPipeline`、`SceneView`、`RenderSurface`）各自背着别人的一半职责；重切成
**五个各回答一个问题的类型 + 引擎的三个门面**，并把被绕过两次的"调度"收成一个实现。

## 1. 判据

1. **一个事实一个家**：pass 的登记只有引擎有；配方的开关只在配方里；呈递只在视图/表面之间回答一次。
2. **一个类型一职**：`RenderEngine` 不再同时是登记表、发布表、诊断路由与 UI 门面；`RenderPipeline` 不再同时是
   配方与注册句柄集。
3. **注册与注销对称且类型化**：谁注册，谁拿一个 RAII 句柄；注销不需要任何人"记得"。
4. **不新增第二套登记**：任何 `passes()` 视图都必须由引擎的 `Slot` 表回答（不复制指针清单）。
5. **调度只有一个实现**：今天的"引擎按顺序排一遍 + 后端按依赖再排一遍"必须收敛成一跳。
6. 行业锚点：Filament（`Engine` / `Renderer` / `View` / FrameGraph）、Unreal RDG、Unity SRP
   （`ScriptableRenderer` 持有 pass 列表与设置）、Dawn/WebGPU（设备与命令编码分离）。

## 2. 今天的形状与四处混居（证据都在头文件/实现里）

| 混居 | 证据 | 后果 |
|---|---|---|
| **引擎是五合一** | `RenderEngine.hpp` 的公开面 ~30 个方法：设备借用、诊断、生命周期、pass 登记、命名产出/transient、编译与计划、区与尺寸、默认程序 | 读者无法一眼说出"它的职责"；测试只能整机起 |
| **配方 = 配方 + 注册句柄集** | `RenderPipeline::addPass` 是 private、只做"记住 + 转发 `engine->addPass`"；`passes_` 与引擎 `Slot` 表持有同一批句柄 | 同一事实第二份存储（虽仅为一个子集） |
| **配方 = 配方 + 两种具体 overlay** | `gizmo_` / `fps_overlay_` / `gizmo_pass_` / `fps_overlay_pass_` 各一组成员，`resize()` 里两段特判 | 第三种 overlay 必须改类（开闭原则） |
| **呈递知识散在四处** | `RenderSurface::presentingPass()` 问 `pipeline->windowPass()`；`SceneView`/`RenderControl`/demo 各自读它 | 表面知道"配方"，而"哪条 pass 画这块面"其实是视图的事 |
| **调度两跳** | `RenderEngine::compileFrame` 按 stage/registration 排；`backend::FrameGraph` 再按输入边排 | 两套排序、两套帧词汇、两个"编译" |
| **诊断两条公开路由** | `RenderEngine` 与 `RenderDevice` 都暴露 `setDiagnosticSink/diagnosticCount` | 同一路线的两个门面 |

## 3. 目标形状

```
RenderEngine                生命周期 / frame() / 设备借用 / 尺寸 / 能力与报告
 ├─ passes()    : PassRegistry    ← 唯一的 pass 登记表（Slot: pass, order, content, role, group）
 ├─ outputs()   : OutputRegistry  ← publish / resolve / unpublish / transientTarget
 └─ diagnostics(): DiagnosticsRoute ← sink / count / clean / report

PassGroup                    ← RAII 注册句柄（move-only；析构 = registry.removeAll(id)）
                                不存指针清单；passes()/pass(role) 都由 registry 回答

ScenePicture                 ← 一张画面的配方：path / mode / overlays / targets / presentingPass / resize
ScenePictureBuilder          ← 选项 → ScenePicture（一次性组装 + 报告）

OverlayContent               ← overlay 的内容契约：scene() / camera() / resize(w,h,ratio) / viewport()
 ├─ AxisGizmo
 └─ FpsOverlay

SceneView                    ← 相机 + 场景 + 操纵器 + 采用的 ScenePicture + 呈递 pass 的回答
RenderSurface                ← 目标 + 尺寸 + 呈现表（present(view) 时问 view）
```

### 3.1 引擎的三个门面（不是三个对象）

`PassRegistry` / `OutputRegistry` / `DiagnosticsRoute` 是**同一个 Impl 的三个视图**（引用成员），拆分的是**公开面**，
不是存储。效果：每个门面能被单独写契约与用例；`PassGroup` 与 `ScenePicture` 只需依赖 `PassRegistry&`，不必认识整个引擎。

### 3.2 `PassRegistry` 与 `PassGroup`

```cpp
class PassRegistry {
  public:
    intrusive_ptr<RenderPass> add(PassDesc desc, StringView role = {});  // role 在组内唯一
    void remove(raw_ptr<RenderPass>);
    std::size_t size() const;
    std::optional<int> orderOf(raw_ptr<const RenderPass>) const;
    raw_ptr<Scene> contentOf(raw_ptr<const RenderPass>) const;
    void bindContent(raw_ptr<RenderPass>, intrusive_ptr<Scene>);
    std::span<const RenderPassPtr> passes() const;   // 引擎存储上的视图，registry 变更即失效
};

class PassGroup {                                    // move-only
  public:
    ~PassGroup();                                    // registry 仍活 ⇒ 注销它的全部 pass
    [[nodiscard]] raw_ptr<RenderPass> pass(StringView role) const;
    [[nodiscard]] std::span<const RenderPassPtr> passes() const;
    [[nodiscard]] bool isValid() const noexcept;     // 引擎已死 ⇒ false，析构是空操作
};
```

**裁定 A1**：`PassGroup` **不存**成员清单；`role` 与 `group` 是 `Slot` 的字段。这消除了今天唯一真实的数据重复。

### 3.3 `ScenePicture`（原 `RenderPipeline`）

```cpp
class ScenePicture : public RefCounted<ScenePicture> {
  public:
    // 画面意味着什么
    [[nodiscard]] ShadingPath path() const;
    [[nodiscard]] ShadingMode mode() const;
    bool setShadingMode(ShadingMode);                        // 表达不了 ⇒ false（同今天）

    // overlay：角色 + 内容，没有第二种形状
    [[nodiscard]] std::span<const OverlaySpec> overlays() const;   // {role, content}
    bool setOverlayVisible(StringView role, bool);                 // 可见性 = 那条 pass 的 enabled
    [[nodiscard]] raw_ptr<OverlayContent> overlay(StringView role) const;

    // 配方自己的目标与呈递
    [[nodiscard]] raw_ptr<RenderTarget> target(StringView role) const; // "gbuffer" / "composite"
    [[nodiscard]] raw_ptr<RenderPass> presentingPass() const;

    [[nodiscard]] bool draws(raw_ptr<const Scene>) const;    // 只对本组 pass 回答
    void resize(int width, int height, double pixel_ratio);  // 目标尺寸 + 每个 overlay 自己的布局

  private:
    PassGroup                                 group_;
    std::vector<OverlaySpec>                  overlays_;
    std::vector<std::pair<String, RenderTargetPtr>> targets_;
    ShadingPath path_; ShadingMode mode_;
};
```

**裁定 A2**：`draws()` 的语义写死为"**这份配方注册的 pass 里**有没有画这个场景"，不是"引擎里有没有"。

### 3.4 `OverlayContent`（开闭原则的修法）

```cpp
class OverlayContent {
  public:
    virtual ~OverlayContent() = default;
    [[nodiscard]] virtual intrusive_ptr<Scene>  scene() const = 0;
    [[nodiscard]] virtual intrusive_ptr<Camera> camera() const = 0;
    virtual void resize(int width, int height, double pixel_ratio) = 0;
    [[nodiscard]] virtual Viewport viewport() const = 0;   // 它的 pass 要画的矩形
};
```

`AxisGizmo` / `FpsOverlay` 实现它；`ScenePicture::resize` 只遍历 `overlays_`，不再有 gizmo/fps 的特判。
第三种 overlay = 一个实现 + builder 里一条注册，**不必改 `ScenePicture`**。

### 3.5 呈递

- `ScenePicture::presentingPass()`：配方声明"哪条 pass 画这张画面"（原 `windowPass`；一区一幅画，不一定有窗口）。
- `SceneView::presentingPass()`：转发给它采用的配方。
- `RenderSurface::present(view)` 与 `presentingPass()`：**问视图**，不再认识配方类型（今天的
  `RenderSurface::presentingPass()` 直接调 `pipeline->windowPass()`）。

### 3.6 调度（本轮最大的一刀）

**裁定 A3**：调度只有引擎一个实现。

1. `RenderEngine::compileFrame` 读登记表：解析命名输入 → 建边（`PassDesc::inputs` / `output` / `depth_view`）→
   **稳定拓扑排序**（tie-break：`PipelineStage` 的 `pipelineStageOrder`、显式 `order`、注册序）→ 环报告并跳过 →
   分配 transient → 产出 `FrameDescription`，其 pass 顺序**就是**执行顺序。
2. 后端只做设备层的事：把计划物化成目标/subpass/barrier/clear，记录命令，报告无法服务的项。
3. `backend::FrameGraph` 退场（其依赖排序与环处理上移），`backend::FrameCompiler` 只剩"目标形状与计划物化"。
4. `PipelineStage` 保留，但身份从"必须走的阶梯"降为"**tie-break 的意图序**"。

收益：两个"编译"、两套帧词汇、两套顺序消失；`FramePlan` 与后端计划不再可能互相矛盾。
风险：这是本轮唯一会改变像素路径的切片，必须单独一片、单独回滚。

## 4. 旧 → 新对照（调用点迁移）

| 今天 | 明天 | 调用点 |
|---|---|---|
| `engine->addPass(pass, order)` / `(pass, content, order)` | `engine.passes().add(PassDesc, role)` → `PassGroup` | builder / demo / tests |
| `engine->removePass/clearPasses/passCount/passOrder/contentOf/bindPassContent` | `engine.passes()` 上的同名动词 | 宿主、`RenderSurface`、用例 |
| `engine->publish/resolve/unpublish/transientTarget*` | `engine.outputs()` | builder / 用例 |
| `engine->setDiagnosticSink/diagnosticCount/diagnosticsClean/reportEngineProblem` | `engine.diagnostics()` | appfw / 用例 |
| `RenderPipeline` | `ScenePicture`（+ `PassGroup`） | `SceneView::setPipeline` → `setPicture` |
| `RenderPipelineBuilder` | `ScenePictureBuilder` | `Viewer` / `RenderControl` / demo |
| `pipeline->windowPass()` | `view->presentingPass()` | `RenderSurface` / `RenderControl` / demo |
| `pipeline->gizmo()/fpsOverlay()` | `picture->overlay(role)` | `Viewer`（只关心存在性）、`model_viewer`（用例） |
| `pipeline->offscreenTarget()/compositeTarget()` | `picture->target("gbuffer"/"composite")` | `Viewer` / demo |
| `RenderDevice::setDiagnosticSink/diagnosticCount`（公开） | 删除，只留 `engine.diagnostics()` | 路由本来就一条 |

## 5. 切片（每片可回滚；S5 单独一片）

| 片 | 内容 | 判据 |
| --- | --- | --- |
| **S1** | 引擎加三个门面：`passes()` / `outputs()` / `diagnostics()`（旧方法保留为转发，零行为） | 全量构建 0 error；三套件计数不变 |
| **S2** | `PassRegistry::add(desc, role)` + `PassGroup`（Slot 加 `role`/`group`）；`RenderPipeline` 改用 group，删 `passes_` | 新增用例：析构注销全部、不影响别的组；同一事实只有一份（`passes()` 视图来自 registry） |
| **S3** | `OverlayContent` + overlay 值化；`ScenePicture` 的 `resize` 去掉特判；builder 按 role 注册 | 用例里注册**第三种** overlay 且不碰 `ScenePicture`；gizmo/fps 现有用例不变 |
| **S4** | 改名与归属：`RenderPipeline`→`ScenePicture`、builder、`windowPass`→`presentingPass`、`SceneView::setPipeline`→`setPicture`、`RenderSurface` 问视图；删 `RenderDevice` 公开诊断 | 构建 0 error；`Viewer` 的"重建后保留 overlay 可见性"用例绿；三套件计数不变 |
| **S5** | 调度上移：引擎稳定拓扑排序；后端只物化；删 `backend::FrameGraph`；`PipelineStage` 降为 tie-break | 像素门禁逐字节不变（`vsg_rewrite_gate.sh` 的画面判据）；环/依赖用例从 test_vsg 迁到 SDK 并保持语义 |
| **S6** | 文档与记忆同步：layering / 对象模型 / 本文件；`.ai/README.md` 索引 | 6 个静态脚本 0 finding |

**变异判据**：S2 把 `passes()` 改回管道自存的向量 ⇒ "registry 变更后视图随之变化"用例红；S3 把 overlay 开关改成私有
bool ⇒ 可见性用例红；S4 让 `RenderSurface` 继续问配方 ⇒ 表面/视图用例红；S5 把排序退回按 stage 先后 ⇒ 依赖用例红。

## 6. 明确不做

- **不引入 ECS / 组件化场景图**：与既有对象模型裁决（OSG/vsg 谱系）冲突，且没有收益证据。
- **不让引擎认识"配方"**（overlay/mode 变成引擎字段）：那才是真重复。
- **不做多视图共享一条配方**：一区一幅画仍是裁决（`graphics-object-model-vs-3d-frameworks.md` §2.1）。
- **不把 `PassKind` 改成插件注册**：闭集 + `RenderPass` 子类已经是两档扩展点。

## 7. 待拍板（2026-10-22 下午：1 已拍，2/3 未拍）

1. ~~**命名**~~ **已定：`PassChain`**。`ScenePicture` 被否（`Picture` 与 `Texture`/`RenderTarget` 同域，读成"场景图片"）；
   `PassPipeline` 被否（重开 K1 关掉的 PSO 混读，`PipelineKey`/`VkPipeline` 还占着 `Pipeline`）。
   配套：`PassChainBuilder` / `ChainDesc` / `SceneView::setChain`；`PipelineStage`→`PassStage` 并挪进叶子头
   `PassStage.hpp`（`PassDesc.hpp` 不再反向 include 链头）。
2. **S5 同轮还是下一轮**：它是唯一改变像素路径的切片；若同一轮做，"调度只有一处"的新契约一次成立；若下一轮，
   S1–S4 先让类型各就各位，S5 独立验证。
3. **`PassStage` 去留**：保留为 tie-break（推荐），或彻底删除、只允许显式 `order` + 依赖边。

## 8. 与既有文档的关系

- 对象模型的行业对照与已裁决偏离：`graphics-object-model-vs-3d-frameworks.md`。
- 骨架与所有权（SK 轮）：`graphics-skeleton-review-2026-10-20.md`。
- 公开面收口的三条已完成项与 R2/R3 的形状：`graphics-object-model-audit-2026-10-21.md`。
- 后端半是什么、为什么叫 `backend/`：`graphics-layering.md`。
- 本次审计的判据与门禁数字：同目录 `graphics-object-model-audit-2026-10-21.md` §3。

## 9. 落地记录（2026-10-22，本轮做 S2 + S3 + S4 的改名部分）

**已落地**

| 片 | 落了什么 | 证据 |
| --- | --- | --- |
| **S2** | `PassGroup`（`vine/graphics/PassGroup.hpp/.cpp`）：引擎仍是唯一注册表，`Slot` 多了 `group` / `role`；`RenderEngine::createPassGroup()` + 私有的 `addPassToGroup/removePassGroup/groupPass(es)`；`RenderPipeline` 删掉 `passes_`，析构不再有 for 循环，两条 HUD pass 按 role 登记、按 role 取回（`gizmo_pass_` / `fps_overlay_pass_` 两个字段随之消失） | `PassGroupTest.AGroupTakesBackExactlyThePassesItRegistered`（引擎先死、外来的 pass 不受影响、role 查回同一个结点）、`AGroupOutlivingItsEngineAnswersNothing`、`ThePipelineFindsItsHudPassesByRoleAndHandsThemAllBack`；现有 HUD 用例（`TheBuildOptionsDecideWhichOverlayPassesExist`、`OverlayVisibilityIsASwitchThatNeverRebuilds`）不改即绿 |
| **S2 的移动语义** | `PassGroup` 的移动**手写**：移动赋先 `release()` 自己那批再接管 —— 默认移动会覆盖 id、把原来那批漏在帧里（这条是被上面的用例当场抓到的） | 同用例最后一句：`group = PassGroup()` 之后 `passCount() == 1`（外来那条还在） |
| **S4（部分）** | `RenderPipeline::windowPass()` → `presentingPass()`（`RenderSurface` / `SceneView` / demo / appfw 文档 / 用例 / 指南同步）；`RenderPipeline` 自己的三参 `addPass` 删除（无人调用，注册只走 group） | 全量构建 0 error；`test_graphics` 406、`test_vsg` 481、`test_gui` 261、`test_core` 165；6 个静态脚本 0 finding；`PassGroupTest.*` 过 ASan |
| **S3（同一轮补上）** | overlay 变成 **值**：`RenderPipeline::addOverlay(role, content, PassDesc, layout)` 是**公开扩展点**（配方的 gizmo / 读数走同一条路），`overlayPass/overlayContent/setOverlayVisible/isOverlayVisible` 按 role 读写，`resize()` 只跑每条 overlay 自己的 layout（不再特判 gizmo / 读数），`gizmo_` / `fps_overlay_` 两个字段删除 —— 类型化访问器成了 `obj_cast<AxisGizmo>(overlayContent(kGizmoRole))` 一行 | `PassGroupTest.AHostRegistersAThirdOverlayWithoutTouchingThePipeline`：宿主注册**第三条** overlay（同一个引擎表、自己的 role、自己的 layout、`resize` 里被调用），不碰 `RenderPipeline` 一行代码；老 HUD 用例不改即绿；`test_graphics` 407 |
| **顺手修的老账** | `ContentDrawTest.TheDynamicMappingFollowsTheEngineConventions` 还停在 B1（混色契约）**之前**的断言（"一个附件总是混"），因插件 `.so` 未重建而一直假绿；插件重建后暴露。已改成交付的契约：不 opt-in ⇒ `blendEnable = VK_FALSE`；opt-in 且默认因子 ⇒ 标准 alpha 对 | `test_vsg` 481 全绿 |

**一条被纠正的措辞**（原 §3.4 / 上一版本 §9 写"需要一个类型擦除的持有者"）：仓库里已经有擦除的**引用**——`vn::Object`
是所有 graphics 对象的公共基类，读侧用 `raw_ptr<Object>` + `obj_cast<T>`（`appfw::DocumentManager` 就是这么存 payload 的），
拥有侧 appfw 用 `std::shared_ptr<const Object>`（`EventBus`）。graphics 的对象是 intrusive 的，所以 `intrusive_ptr<Object>`
确实编不过（计数在 `RefCounted<T>` 基类里，不在 `Object` 上）—— 但 overlay 根本不需要擦除的**拥有者**：内容由注册方持有
（builder 的 layout lambda 捕获 `intrusive_ptr<AxisGizmo>`，`PassDesc::before_draw` 也捕获一份），表里存 `raw_ptr<Object>`
就够。S3 因此不需要新类型擦除容器（§3.4 建议的 `OverlayContent` 接口也随之不必存在：值 + role + layout 已经够开）。

**2026-10-22 下午（S1 落地）**：命名按 `PassChain` 落地（`PassChainBuilder` / `ChainDesc` / `SceneView::setChain`），
`PassGroup` 删除 —— 链自己就是注册范围（`Slot.owner` + `PassChain::~PassChain` 的 `removePassesOwnedBy(this)`，
存储仍只有引擎那张槽位表）；`PassStage` 独立成头，`PassDesc` 不再反向 include 链头。§3.2 的 `PassRegistry` /
`PassGroup` 形状是提案，现行的 `PassRegistry` 门面仍属 F1。 <!-- drift-ok -->

**明确推迟（带触发条件）**

- **S1 引擎三个门面**（`passes()` / `outputs()` / `diagnostics()`）：转发层没有行为收益，等"第二个门面消费者"出现
  （后端也要查表、或诊断要按来源过滤）再做。
- **S4 的改名与归属**（`ScenePicture` / `ScenePictureBuilder` / `setPicture` / `RenderSurface` 问视图）：纯改名，
  等 R2 的下一件（`RenderPipeline` 真拆成"注册 + 画面"）一起做，避免两次全库改名。
- ~~**S5 调度上移**~~ **已落地（2026-10-22 晚，见 §10）**：本来登记为"唯一改变像素路径的一片，与本轮不动画面的判据冲突，单独一轮"；实际单开一片，`vsg_rewrite_gate.sh` 的画面判据（内容/预览/背景三项）与基线一致。

## 10. 落地记录（2026-10-22 晚：S5 / F3 调度上移）

| 片 | 落了什么 | 证据 |
| --- | --- | --- |
| **S5** | 调度进引擎：派生层 `detail::FrameScheduler`（迭代 Tarjan + 堆就绪队列，成员工作区稳态零分配）；`EngineCompile.cpp` 的 `scheduleRows()` 建行（pass/content/order）、建边（`input_targets`、ImageRef 输入、借用深度 `RenderTarget::sharedDepth()`）、排序、按 `frame_order_` 解析与发布；`PassStage` 只做同优先级 tie-break，注册位置只作历史记录 | `FrameScheduleTest`（无依赖保持宣告序 / 先宣告的消费者仍排在生产者之后 / 环整块跳过且只报一次） |
| **后端退场** | `backend::FrameGraph`（`FrameGraph.hpp/.cpp` + `FrameGraphTest` + `FrameCompiler::graph()`）删除；`FrameCompiler::compile()` 按交来的 `description.passes` 物化，不再重排、不检环、`frame_.cycles` 恒 0；`targets_` 仍按宣告序建表（计划的应用顺序与执行顺序是两件事） | `FrameCompilerTest` 两条改为"保持交来的顺序"；`ExecutorTest` 的像素用例断言最后一条交来的 pass 赢；`SampledInputTest` 改生产者先宣告 |
| **借用深度的执行前提** | 引擎为借用深度加边，所以真实帧里 lender 总在 borrower 之前；`DevicePhaseTest` 的 leased 相位是直连编译器的，改为"计划顺序 borrower-first（未提交）+ 设备证据用 lender-first 提交"——borrower 先跑会 `LOAD` 一张未写过的深度（`VUID-vkCmdDraw-None-09600`） | `DevicePhaseTest` 0 VUID；`ExecutorTest.runLeasedTargetOrderPhase` 断言的 `targets[0]`（plan 序）与像素都不变 |
| **门禁** | `vsg_rewrite_gate.sh`：suite 两跑 0 VUID / 0 SYNC-HAZARD、相位行全闭合；`app` 阶段内容 87.04%→85.14%、预览 244、背景 0%（与基线记录一致）。`boot occupancy` 327–437 ms 超 150 ms 预算，**基线同机同法测得 311 ms**，登记为环境项而非本片回归 | `test_graphics` 412 / `test_vsg` 470（−11 条 `FrameGraphTest`）/ `test_gui` 261 / `test_core` 165；6 个静态脚本 0 finding |
