# 层与分层渲染：内容掩码 + 目标层（数组 / 立方体面）（2026-10-12 设计定稿，**分两步落地**）

> **状态**：**L1 内容掩码已落地**（2026-10-12，判据与变异见 §5 实测）；**L2 目标层**（L2a 逐层、L2b 一次写完）
> 仍是**设计定稿、未落地**。落点清单见 §4。
>
> **复核后的修订（同日）**：① 名词统一为**层**（layer）—— 输入侧已有的是 `Texture::layerCount()` / `layer(i)`，
> 新名词必须跟它走，`slice` 只出现在引用 Ogre-Next 上游符号的地方；② 目标侧写层用**一个区间**
> （`first_layer` + `layer_count`），不是 `target_slice` / `all_slices` 两个开关；③ 掩码解析沿用收集遍历里已有的
> `InheritedState`（一条规矩一个实现）；④ 命名避开 `setVisible`：对象侧 `setLayers`、pass 侧 `setLayerMask`；
> 掩码的字段名一律 `layer_mask`；⑤ 掩码的读侧只给"本节点的声明"，不做上溯解析（理由见 §2.2）；
> ⑥ 补齐配方入口（§2.4）、退化语义（§2.5）、L2 读侧（§3.3）与落点清单（§4）；
> ⑦ **落地时两处细化**（设计稿没写对，被 §5 的用例逼出来的）：未声明层的节点取 `kDefaultLayer`（bit 0，不是"全 1"），
> 掩码判定落在**可绘制节点**上（不变容器剪枝）—— 两条都在 §2.1，理由与踩法见 §5。
>
> **为什么现在定**：对象模型复核（`graphics-object-model-vs-3d-frameworks.md` §3）把这两条列为缺口，且
> Ogre-Next 提供了成熟形状（见 §7）。两者会**在同一个函数里碰头**（内容收集的记忆化），所以一起设计、分开落地。

## 0. 一句话

**掩码决定"这条 pass 能画哪些对象"，层决定"这次渲染写目标的哪一层"** —— 前者是内容选择，后者是目标形状，
两者都不引入第二个配置面，也不改"一块区只画一幅画"与"一台相机一张画面"。

## 1. 名词消歧：本仓已经有三种"层"，这是第四、第五种

| 已有的 | 它回答的问题 | 别混 |
| --- | --- | --- |
| `PipelineStage` + pass 的 `tag` | **顺序与类型**（阴影 / 几何 / 着色 / Overlay…） | 不是"谁能被画" |
| `StateNode` 的状态继承 | **状态从哪一层来**（材质 / 状态逐项有 owner，见状态解析单点） | 不是对象选择 |
| HUD pass（`tag = "Hud"`、`clear_enabled = false`） | **合成次序**（后画、不洗屏） | 不是掩码 |
| **内容掩码**（本文 L1） | **这条 pass 画哪些对象** | 新 |
| **目标层**（本文 L2） | **写目标的哪一层**（数组层 / 立方体面） | 新 |

## 2. L1：内容掩码（对象声明 + pass 掩码）

### 2.1 语义（两级，不是三级）

- **对象**（`Node` 子树，可继承 / 可覆盖）携带一个 **32 位层集合**；**pass** 携带一个 **32 位掩码**。
- 判定：`可见(isVisible) && (层集合 & 掩码) != 0` 才被收集。默认掩码全 1、未声明层的节点在 `kDefaultLayer` ⇒ 默认行为与今天**逐位相同**。
- **未声明 = 默认层（bit 0），不是"所有层"**：选择性掩码的用处是**排除**没声明的内容（阴影只画 caster），
  而"未声明 = 全 1"会让每个不画的物体都得先声明一次。这条跟 three.js / Unity / Qt3D 的默认层走，
  Ogre-Next（两边都是 0xFFFFFFFF）**刻意不跟**。`kDefaultLayer` 与 `kAllLayers` 两个常量都在 `Node.hpp`（一个 owner）。
- **掩码测试落在可绘制节点上（容器是结构）**：判定写在遍历里"发一条命令"的地方，而不是节点入口 —— 在容器上剪枝
  会让"声明了层的子内容"在"没声明层的组"下**不可达**（第一版就是这样，被 §5 的 C1 抓到）。与行业口径一致：
  Unity 的层在 GameObject（渲染器）上，Qt3D / OSG 的掩码也按可绘制对象判。
- **为什么只两级**：Ogre-Next 是三级（对象 flags / viewport mask / compositor pass mask）。我们没有 viewport 概念，
  "一次绘制声明"就是 pass ⇒ **pass 级掩码是那条 viewport 级掩码的等价物**，不需要再造一层。

### 2.2 形态（API）

```cpp
// 对象侧（Node；子树继承，最近的声明赢 —— 与 StateNode 的状态继承同一条规矩）
void  setLayers(std::uint32_t layers);                      // 本子树归哪些层（0 = 不属于任何层）
void  clearLayers();                                        // 撤掉本节点自己的声明：改回继承
[[nodiscard]] std::optional<std::uint32_t> layers() const;  // 本节点"说过什么"（null = 继承）

// pass 侧（RenderPass / PassDesc：一个值，随 pass 声明走）
void setLayerMask(std::uint32_t mask);   // 默认 kAllLayers
[[nodiscard]] std::uint32_t layerMask() const noexcept;
```

- **常量一个 owner**：`inline constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;` 放在 `Node.hpp`（层是对象侧的声明，
  读法也在那里）；需要它的公共头各自**直接 include** `Node.hpp`（hygiene gate 要直接 include，不吃传递 include）。
- **命名跟输入侧走**：`Texture::layerCount()` / `layer(i)` 已经是"层"的说法，所以两侧一律 **layer**（不引入 `slice`）。
- **可见性仍是总闸**：`setVisible(false)` 照旧，与掩码是两件事（"临时关掉" vs "归哪一层"）—— 行业同样两个都有
  （Unity 的 `activeSelf`/`enabled` 与 Layer；Ogre 的 visible 与 visibility flags）。
- **读侧不撒谎，也不上溯**：`layers()` 返回**本节点自己的声明**（`std::optional`），解析只在收集时做一次。
  `Node` **有**父指针（`Node::parent_`），所以"不做上溯解析"是**选择而不是限制**：状态那边之所以还有一套宿主的
  上溯读法（`StateNode` 的解析读），是因为宿主需要问"这台机器现在什么状态"；层没有这个问题 —— 问"为什么不画"
  的是 **pass**，而它的答案在收集时算一次（走 `InheritedState`），再造一个上溯实现就是同一规矩的第二份实现
  （`Scene.cpp` 的 `InheritedState` 注释里已写明这种重复只能靠测试钉住；这里干脆不做第二份）。

### 2.3 收集与记忆化（**本文最重要的坑**）

收集只有一个缝：`Scene::collectRenderCommands(camera)` / `collectRenderCommandsShared(camera)`，
共享那份按 **(content_revision, eye, view_proj)** 记忆化（`Scene.cpp` 的 `ContentMemo`）。

- 掩码作为**默认参数**加进去：`collectRenderCommandsShared(camera, layer_mask = kAllLayers)` ⇒ 既有调用点（测试几十处）
  源码不变。
- **掩码必须进 memo key**：否则"阴影 pass（只画 caster）"与"主 pass（画全部）"会共用同一份命令表 ——
  画面看着对、只是多了/少了几个物体，属于最难查的一类错。这条要**正反两面各一条用例**钉住（§5）。
- **层（L2）不进 key**：层不改变"收集出什么"，只改变"画到哪儿"。
- **判定只有一处**：收集遍历里那一个条件；解析出来的层集合由 `InheritedState` 往下带（与状态继承同一个 fold，
  同一处 `clearLayers()` = 把 `std::optional` 清空）。
- **帧值如实，但进对的地方**：掩码进 **`FramePlan::PassPlan`**（字段 + `describe()` 文本），**不进 `PassDescription`**
  —— `PassDescription` 说的是"这条 pass 发出什么"（目标 / 视口 / 清屏 / 状态 / 绘制，后端消费的形状），
  掩码是**选择的理由**而不是绘制内容，塞进去只会让后端多读一个它不用的字段（见 `FrameDescription.hpp` 的说明）。

### 2.4 配方入口（`PipelineOptions` / `ViewerOptions`）

```cpp
// PipelineOptions（RenderPipeline.hpp）：这条配方画哪些层
std::uint32_t layer_mask{ kAllLayers };
// appfw（ViewerOptions.hpp）：原样转发给 PipelineOptions（Viewer::buildPipeline）
```

- **落点是内容 pass，一处一行**：`RenderPipelineBuilder.cpp` 里每一个走收集的**内容** `PassDesc`（阴影 / 前向主 /
  前向透明 / G-buffer / 延迟透明 / 预览 —— 六处）各加 `desc.layer_mask = content_layers_;`（`build()` 开头从
  `options.layer_mask` 取一份，与 `shadows_built_` 同款"一次 build 一个值"）—— 与既有的 `desc.content = ...`
  同样的重复形态，不是新规矩。
- **两条 Screen 灯照 pass 不落**：它们的 content 只用来取灯（全屏程序不遍历场景），盖上掩码是**假话**。
- **Overlay 不落**：gizmo 与帧率那两个 pass 保持默认 —— 那是 UI 不是内容，宿主查 UI 时不该被内容选择一起藏掉。

### 2.5 退化语义（0 不静默，但不是拒绝）

- `(层集合 & 掩码)` 一条公式管两边：掩码 0 或层集合 0 都 = "选不到任何对象"，语义上合法。
- 但**内容 pass 的掩码 0** 几乎总是笔误（声明了要画内容却一个都选不中）⇒ 登记一条**帧计划问题**
  （与其它声明检查同一处、同一 episode 规则：同一句话不重复报），pass 照旧列在计划里、照旧零绘制 ——
  "画了但没东西"与"没画"是两件事（见 `FrameDescription.hpp`）。
- `Node::setLayers(0)` **静默**：那是作者态（内容还没分层），不是一条要画的声明；两者都由同一条公式解释，
  所以不需要第二条规矩。

## 3. L2：目标层（分层渲染）

### 3.1 目标形状

```cpp
// RenderTarget（离屏描述）：
void setLayerCount(int count);                    // 1 = 2D（默认）；> 1 = 2D 数组；立方体 = 6 + 语义标记（后续）
[[nodiscard]] int layerCount() const noexcept;    // 与 Texture::layerCount() 同名同义

// pass 侧：写哪个区间 —— 一个区间，不是两个开关
PassDesc::first_layer { 0 } + PassDesc::layer_count { 1 }
```

- **一个区间而不是两个开关**："整张"就是 `{0, layerCount}`，"某一层"就是 `{i, 1}` —— 同一种表达；L2a 与 L2b 是
  同一个值的不同能力实现，不是两套 API。默认 `{0, 1}` 让"不分层"的目标行为不变。
- 后端按 `layerCount > 1` 建 `2D_ARRAY` 视图；**深度的层数必须与颜色一致**（不一致 ⇒ 拒绝 + 报一次，
  与既有的目标形状检查同一处）。
- 输入侧已有的对应物：`Texture::Kind::{D2, Cube}` 与 `layerCount()` / `layer(i)`（新名词跟它走）。

### 3.2 两次落地的切法（**L2a 先、L2b 后**）

| 步 | 能力 | 为什么先/后 |
| --- | --- | --- |
| **L2a 逐层** | 一条 pass 写**一层**（`{i, 1}`）；N 层 = N 条 pass（或 N 次执行） | 后端只需给每层建一个视图/帧缓冲，CPU 侧循环；**不需要着色器写层号**、不需要几何阶段能力。点光源立方体阴影（6 面）与 CSM（几级）用它就能做 |
| **L2b 一次写完** | 一条 pass 写**多/全部层**（`{0, layerCount}`）：几何/网格阶段写层号，后端 render view 带层数 | 省的是"同一份内容收集 N 次 + 提交 N 次"；要着色器与后端两个能力都到位。触发条件见 §6 |

### 3.3 读侧（**L2 的前一半，落地顺序上要先做**）

- **层的家是图像身份**：`ImageRef` 已经是"目标 + attachment"（见 `ImageRef.hpp`），加层就是加第三个部分 ——
  `bind(target, attachment, layer = 0)` + `layer()`。生产侧（谁写这层）与消费侧（谁采样这层）说的是同一个东西，
  与既有的"一件图像一个身份"一致。
- **裸目标输入没有层**：`PassDesc::input_targets` / `RenderPass::addInput(target)` 那种输入不带层，在分层上下文里
  它只能表示"整层集合"（采样器读所有层 = 需要着色器能力）⇒ 归 L2b。**L2a 的消费者只用 `ImageRef` 输入。**
- **回读**：`RenderDevice::readColorBuffer(target, attachment, layer, out, why)`（层参数默认 0；深度同）。
- **预览 / 拷贝**：`screenCopyProgram(n)` 选的是**绑定槽位**，不是层 —— 读哪一层由声明的输入图像说
  （`ImageRef` 带层），所以预览不需要新参数。
- 结论：**先读侧（`ImageRef` 层 + 回读层），再写侧（目标层 + pass 区间）**；反过来的话做出来的是半成品
  （写进去没人能按层读出来）。

### 3.4 与既有裁决的关系（都不冲突）

- **一块区只画一幅画**：分层渲染写的是**离屏目标**的层；区照旧只呈现一条 pass（合成后的那一幅）。VR/双眼最终
  仍要"两条 pass 或一次分层"，而窗口那一半仍是**一幅画**。
- **一台相机一张画面**：逐层可以让**同一台相机**画进不同层（层不是"形状"），与"相机的形状必须与它画的矩形
  一致"不矛盾 —— 但**不同视角**（双眼、6 面）是**不同相机**，这是 L2 的价值前提。
- **不新增配置面**：掩码是 pass 的一个值 + 节点的一个声明；层是目标的一个属性 + pass 的一个值。

## 4. 落点清单与缺口表

**L1 七处**（**已落地**，改动落到哪儿，落地时对着这张表走）：

| # | 落点 | 改什么 |
| --- | --- | --- |
| 1 | `Node.hpp` / `Node.cpp` | `setLayers` / `clearLayers` / `layers` + `layers_`（`std::optional<std::uint32_t>`）+ `kAllLayers` |
| 2 | `Scene.hpp` / `Scene.cpp` | 两个收集函数加默认参数；`ContentMemo` 的 key 加掩码；`InheritedState` 加层集合；遍历里那一行判定 |
| 3 | `PassDesc.hpp` | `layer_mask{ kAllLayers }` |
| 4 | `RenderPass.hpp` / `RenderPass.cpp` | `setLayerMask` / `layerMask`；`describe()` 把掩码交给收集（今天那一行在 `RenderPass.cpp`） |
| 5 | `FramePlan.hpp` + `EngineCompile.cpp`（问题）+ `RenderEngine.cpp`（填行与文本） | `PassPlan::layer_mask` + "掩码 0"的帧计划问题 |
| 6 | `RenderPipeline.hpp` + `RenderPipelineBuilder.cpp` | `PipelineOptions::layer_mask` + 六个内容 pass 的盖章（两条 Screen 灯照 pass 与 overlay 不盖） |
| 7 | `ViewerOptions.hpp` + `Viewer.cpp` | `ViewerOptions::layer_mask` 转发到 `PipelineOptions` |

**L2 六处**：`RenderTarget`（层数）/ `PassDesc` + `RenderPass`（区间）/ `ImageRef`（层）/ `RenderDevice`（回读层）/
后端（`2D_ARRAY` 视图与逐层帧缓冲）/ `RenderPipeline` + appfw（配方要分层时）。

**与对象模型复核（§3 缺口表）的关系**：

- 缺口 1（同 pass 内优先级）**不在本文**：那是 `Renderable::setRenderQueueSubGroup` 那一类（排序），与掩码两件事。
- 缺口 3（LOD / 空间划分）与本文无关。
- 缺口 2（实例化）**与 L2b 同源**（一个几何画 N 份 ↔ 一份内容画 N 层）：两者都要求后端"一次提交、多个目标/实例"，
  建议**同一触发条件一起评估**。

## 5. L1 判据（**已落地，2026-10-12**，实测）

用例（`test_graphics` 的 `ContentLayersTest` 七条 + `test_gui` 一条）：

- **C1 过滤**：同一场景、同一相机、两条不同掩码 ⇒ 命令集合不同，且各自内容正确（`TheMaskDecidesWhichNodesAreCollected`）。
- **C2 记忆化正反两面**：同一帧、同一相机、同一掩码 ⇒ 两次收集拿到同一份表；**不同掩码 ⇒ 不同表**（`TheMaskIsPartOfWhatTheMemoIsKeyedOn`）。
- **C3 继承与覆盖**：父设、子没设 = 继承；子设 = 覆盖且不影响父的其它子树；`clearLayers()` 回到继承（`TheNearestDeclarationCoversItsSubtree`）。
- **C4 与可见性独立**：`setVisible(false)` 在掩码命中时也不画；掩码不命中时 `setVisible(true)` 也不画（`VisibilityAndLayersAreTwoQuestions`）。
- **C5 通过声明落值**：`PassDesc::layer_mask` 经 `RenderEngine::addPass(PassDesc)` 落到 `RenderPass::layerMask()`（`TheDeclarationCarriesTheMaskOntoThePassAndIntoThePlan`）。
- **C6 帧计划如实**：`FramePlan::PassPlan::layer_mask` 说出该 pass 的掩码；`describe()` 只在非默认时打 `layers=0x..`。
- **C7 退化**：掩码 0 的内容 pass ⇒ 零绘制（`render_command_counts` 里一条 0 命令的调用）+ 一条帧计划问题（两帧只报一次）。
- **C8 配方 + appfw**：`PipelineOptions::layer_mask` 盖到内容 pass 上、**不**盖到 overlay（`test_graphics` 的
  `ARecipeStampsItsMaskOnEveryContentPassButNotOnTheOverlays`）；`ViewerOptions::layer_mask` 原样到配方
  （`test_gui` 的 `TheViewerOptionsLayerMaskReachesTheContentPassesAndNotTheHud`）。

变异（各自**单独**实测必须红，实测结果）：

| # | 变异 | 实测红在哪 |
| --- | --- | --- |
| M1 | 掩码不进 memo key | C2 红（不同掩码拿到同一份表），C1 仍绿 |
| M2 | 掩码收下但不过滤（`false &&`） | C1 / C2 / C3 / C4 / C7 红，C5 / C6 绿 |
| M3 | 继承不生效（`value_or(kDefaultLayer)`） | C1 / C3 / C4 红 |
| M4 | 掩码不进 `PassPlan` | C5 / C6 / C7 红 |
| M5 | 掩码顺手盖到 overlay 上 | C8（两条）红 |

计数（实测）：全量 Release **0 error / 0 warning**；`test_graphics` **368**（361 + 7，只升）；
`test_gui` **258**（257 + 1）；`test_vsg` **429** = 426 + 3 skip、`test_appfw` 32 / `test_meshio` 34 / `test_iobase` 94 不动；
五 gate 0 finding，`check_ai_docs --strict` 仍 **13**。

## 6. L2 判据与触发条件

- **L2a 落地判据**：① `RenderTarget::setLayerCount(n)` + pass 的区间被后端如实建成 `2D_ARRAY`（结构用例）；
  ② 真设备用例（`test_vsg`）：两层数组目标、两条 pass 各写一层、按层读回两张图不同且各自正确（这条同时钉住读侧）；
  ③ 层数 > 1 而 pass 的区间超出范围 ⇒ 拒绝 + 报一次（不静默画错层）；④ 深度层数与颜色不一致 ⇒ 拒绝 + 报一次。
- **L2b 触发条件**（命中一条再做）：① 点光源立方体阴影或 CSM 的**逐层**版本已被采用且成为瓶颈（收集/提交次数
  翻 N 倍）；② VR/双眼落地（一帧两只眼）；③ 实例化（复核缺口 2）同时被触发 —— 两者共用同一后端能力。

## 7. 行业口径（本次的实际依据）

- **Ogre-Next**（最近的那位）：
  - 三级掩码：`SceneManager::setVisibilityMask` / `Viewport::setVisibilityMask` / `CompositorPassShadowsDef::setVisibilityMask`，
    上游注释写明与 viewport 掩码按位与 —— 对应本文的"对象声明 + pass 掩码"（我们的 pass 就是它的 compositor pass）。
  - 数组/层目标是一等能力：`TextureGpu::getNumSlices()`（含 cube array），render pass 描述支持"绑整张 array / cubemap"
    或"绑某个 slice"（上游对"分层渲染只允许 slice = 0"的校验就是这条能力的证据；上游用 slice 一词，本文统一叫 layer）。
  - 视图是"目标 + 归一化矩形"：`Viewport::setDimensions(TextureGpu*, relativeVp, scissors, mipLevel)` ——
    **这条我们刻意不跟**（一区一幅画，见 `docs/guides/render-views.md` §6）。
  - 排序优先级：`Renderable::setRenderQueueSubGroup`（复核缺口 1 的行业形状）。
  - 材质取向相反：v2 把状态从图上拿走（`Item::setDatablock` + Hlms 生成 shader），我们保留沿图继承
    （`StateNode`）与状态解析单点 —— **只借机制，不借取向**。
- Unity（Layer + `Camera.cullingMask`）、Qt3D（`QLayer` / `QLayerFilter`）、OSG（`NodeMask` + `Camera` cull mask）：
  都是"对象带层、消费者带掩码"这一条，没有别的花样。

## 8. 不做的话，今天的替代

| 需求 | 今天的替代 | 代价 |
| --- | --- | --- |
| 某条 pass 只画一部分对象 | 把对象拆到两个 `Scene`，或整棵子树 `setVisible(false)` 再恢复 | 拆场景 = 内容重复与同步；`setVisible` = 两处互相影响（多视角下必错） |
| 立方体面 / 多级阴影 | 多个 2D 目标 + 多次 pass + 自己合成 | N 份显存与 N 次收集；点光源阴影在今天的阴影实现里根本没有入口 |