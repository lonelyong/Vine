# 层与分层渲染：内容掩码 + 数组/切片目标（2026-10-12 设计定稿，**分两步落地**）

> **状态**：**设计定稿，未落地**。落地分两步 —— **L1 内容掩码**（对象 flags + pass mask，含收集与记忆化的坑）、
> **L2 数组/切片目标**（L2a 逐切片、L2b 一次写完）。判据见 §5、§6；与既有裁决的关系见 §4。
>
> **为什么现在定**：对象模型复核（`graphics-object-model-vs-3d-frameworks.md` §3）把这两条列为缺口，且
> Ogre-Next 提供了成熟形状（见 §7）。两者会**在同一个函数里碰头**（内容收集的记忆化），所以一起设计、分开落地。

## 0. 一句话

**掩码决定"这条 pass 能画哪些对象"，切片决定"这次渲染写目标的哪一层"** —— 前者是内容选择，后者是目标形状，
两者都不引入第二个配置面，也不改"一块区只画一幅画"与"一台相机一张画面"。

## 1. 名词消歧：本仓已经有三种"层"，这是第四、第五种

| 已有的 | 它回答的问题 | 别混 |
| --- | --- | --- |
| `PipelineStage` + pass 的 `tag` | **顺序与类型**（阴影 / 几何 / 着色 / Overlay…） | 不是"谁能被画" |
| `StateNode` 的状态继承 | **状态从哪一层来**（材质 / 状态逐项有 owner，见状态解析单点） | 不是对象选择 |
| HUD pass（`tag = "Hud"`、`clear_enabled = false`） | **合成次序**（后画、不洗屏） | 不是掩码 |
| **内容掩码**（本文 L1） | **这条 pass 画哪些对象** | 新 |
| **目标切片**（本文 L2） | **写目标的哪一层**（数组层 / 立方体面） | 新 |

## 2. L1：内容掩码（对象 flags + pass mask）

### 2.1 语义（两级，不是三级）

- **对象**（`Node` 子树，可继承 / 可覆盖）携带一个 **32 位 flag 集合**；**pass** 携带一个 **32 位掩码**。
- 判定：`可见(isVisible) && (flags & mask) != 0` 才被收集。默认两边都是全 1 ⇒ 行为与今天**逐位相同**。
- **为什么只两级**：Ogre-Next 是三级（对象 flags / viewport mask / compositor pass mask）。我们没有 viewport 概念，
  "一次绘制声明"就是 pass ⇒ **pass 级掩码是那条 viewport 级掩码的等价物**，不需要再造一层。

### 2.2 形态（API）

```cpp
// 对象侧（Node；子树继承，最近的声明赢，与 StateNode 的状态继承同一条规矩）
void  setVisibilityFlags(std::uint32_t flags);              // 本子树归哪些层
void  clearVisibilityFlags();                               // 撤掉本节点自己的声明：改回继承
[[nodiscard]] std::optional<std::uint32_t> visibilityFlags() const;  // 本节点"说过什么"（null = 继承）

// pass 侧（RenderPass / PassDesc：一个值，随 pass 声明走）
void setContentMask(std::uint32_t mask);   // 默认 0xFFFFFFFF
[[nodiscard]] std::uint32_t contentMask() const noexcept;
```

- **可见性仍是总闸**：`setVisible(false)` 照旧，与掩码是两件事（"临时关掉" vs "归哪一层"）—— 行业同样两个都有
  （Unity 的 `activeSelf`/`enabled` 与 Layer；Ogre 的 visible 与 visibility flags）。
- **读侧不撒谎**：`visibilityFlags()` 返回**本节点的声明**（`std::optional`），解析只在收集时做一次 ——
  与"一个值一个 owner + 逐项说出处"一致；不做"查询即上溯解析"（`Node` 没有父指针，也不该为它加）。

### 2.3 收集与记忆化（**本文最重要的坑**）

收集只有一个缝：`Scene::collectRenderCommands(camera)` / `collectRenderCommandsShared(camera)`，
共享那份按 **(content_revision, eye, view_proj)** 记忆化（`Scene.cpp` 的 `ContentMemo`）。

- 掩码作为**默认参数**加进去：`collectRenderCommandsShared(camera, mask = 0xFFFFFFFF)` ⇒ 既有调用点（测试几十处）
  源码不变。
- **掩码必须进 memo key**：否则"阴影 pass（只画 caster）"与"主 pass（画全部）"会共用同一份命令表 ——
  画面看着对、只是多了/少了几个物体，属于最难查的一类错。这条要**单独一条用例**钉住（§5）。
- 一条 pass 的掩码也进 `PassDescription`，让帧值（`FrameDescription`）能如实说"这条 pass 画哪些对象"。

## 3. L2：数组/切片目标（分层渲染）

### 3.1 目标形状

```cpp
// RenderTarget（离屏描述）：
void setSlices(int count);      // 1 = 2D（默认）；> 1 = 2D 数组；立方体面 = 6 + 语义标记
[[nodiscard]] int slices() const noexcept;
// 输入侧已经有的对应物：Texture 的 Kind（D2 / Cube）与 layerCount()。
```

- 后端按 `slices > 1` 建 `2D_ARRAY` 视图；`PassDesc` 说"写哪一层 / 写全部层"。
- 深度的层数必须与颜色一致（否则拒绝 + 报一次，与既有的"目标形状"检查同一处）。

### 3.2 两次落地的切法（**L2a 先、L2b 后**）

| 步 | 能力 | 为什么先/后 |
| --- | --- | --- |
| **L2a 逐切片** | 一个 pass 写**一层**（`PassDesc::target_slice`）；N 层 = N 条 pass（或 N 次执行） | 后端只需给每层建一个视图/帧缓冲，CPU 侧循环；**不需要着色器写层号**、不需要几何阶段能力。点光源立方体阴影（6 面）与 CSM（几级）用它就能做 |
| **L2b 一次写完** | 一条 pass 写**全部层**（`all_slices`）：几何/网格阶段写层号，后端 render view 带 `layerCount` | 省的是"同一份内容收集 N 次 + 提交 N 次"；要着色器与后端两个能力都到位。触发条件见 §6 |

### 3.3 与既有裁决的关系（都不冲突）

- **一块区只画一幅画**：分层渲染写的是**离屏目标**的层；区照旧只呈现一条 pass（合成后的那一幅）。VR/双眼最终
  仍要"两条 pass 或一次分层"，而窗口那一半仍是**一幅画**。
- **一台相机一张画面**：逐切片可以让**同一台相机**画进不同层（层不是"形状"），与"相机的形状必须与它画的矩形
  一致"不矛盾 —— 但**不同视角**（双眼、6 面）是**不同相机**，这是 L2 的价值前提。
- **不新增配置面**：掩码是 pass 的一个值 + 节点的一个声明；切片是目标的一个属性 + pass 的一个值。

## 4. 与对象模型复核的关系

- 复核 §3 的缺口 1（同 pass 内优先级）**不在本文**：那是 `Renderable::setRenderQueueSubGroup` 那一类
  （Ogre-Next 的 render queue 子组），归属"排序"，与掩码是两件事；登记在同一张缺口表里，触发条件不变。
- 复核 §3 的缺口 3（LOD / 空间划分）与本文无关。
- 本文落地后，复核 §3 的缺口 2（实例化）仍在，且**与 L2b 同源**（一个几何画 N 份 ↔ 一份内容画 N 层）：
  两者都要求后端支持"一次提交、多个目标/实例"，建议**同一触发条件一起评估**。

## 5. L1 判据（落地时实测）

- 新用例：① 同一场景两条 pass 用不同掩码 ⇒ 命令数不同（且**互不污染**：先跑 A 再跑 B，B 拿到的不是 A 的表）；
  ② 掩码继承与覆盖（父设了、子没设 = 继承；子设了 = 覆盖；`clearVisibilityFlags()` 回到继承）；
  ③ `setVisible(false)` 与掩码各自独立生效；
  ④ pass 掩码进 `PassDescription`（帧值如实）；
  ⑤ **memo key**：同一相机同一帧、两条不同掩码的 pass ⇒ 两次收集（不是复用错表）。
- 变异（各自单独实测红）：① 掩码不进 memo key ⇒ ⑤ 红；② 收集不过滤 ⇒ ① 红；③ 继承不生效（只看本节点）⇒ ② 红。
- 计数：`test_graphics` 只升不降；其余套件不动；五 gate 0 finding。

## 6. L2 判据与触发条件

- **L2a 落地判据**：① `RenderTarget::setSlices(n)` + `PassDesc::target_slice` 被后端如实建成 `2D_ARRAY`（结构用例）；
  ② 真设备用例（`test_vsg`）：两层数组目标、两条 pass 各写一层、读回两层像素不同且各自正确；
  ③ 层数 > 1 而 pass 未指定层 ⇒ 拒绝 + 报一次（不静默画错层）；④ 深度层数与颜色不一致 ⇒ 拒绝 + 报一次。
- **L2b 触发条件**（命中一条再做）：① 点光源立方体阴影或 CSM 的**逐切片**版本已被采用且成为瓶颈（收集/提交次数
  翻 N 倍）；② VR/双眼落地（需要一帧两只眼）；③ 实例化（复核缺口 2）同时被触发 —— 两者共用同一后端能力。

## 7. 行业口径（本次的实际依据）

- **Ogre-Next**（最近的那位）：
  - 三级掩码：`SceneManager::setVisibilityMask` / `Viewport::setVisibilityMask` / `CompositorPassShadowsDef::setVisibilityMask`，
    上游注释写明与 viewport 掩码按位与 —— 对应本文的"对象 flags + pass mask"（我们的 pass 就是它的 compositor pass）。
  - 数组/切片目标是一等能力：`TextureGpu::getNumSlices()`（含 cube array），render pass 描述支持"绑整张 array/cubemap"
    或"绑某个 slice"（上游对"分层渲染只允许 slice = 0"的校验就是这条能力的证据）。
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
