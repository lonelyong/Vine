# ADR-2026-11-16：作者形状、视图与目标 —— 值承载声明，行为归对象

- 状态：**已采纳**。2026-11-16 批准（三条行为变更 BC-08 / BC-09 / BC-10 全批，
  `setRenderTarget` 走"变真"方案）；批次顺序见 `../research/graphics-2026-11-16-redesign-r2.md` §10，
  **2026-11-17 全部落地**。
- 日期：2026-11-16
- 影响范围：`PassDesc.hpp`（`target` / `output` / `tag` / `before_draw`）、`PassChain.hpp`（`OverlayLayout` /
  `update()`）、`CollectView.hpp` → `View`、`RenderSurface` → `RenderArea`、`RenderDevice.hpp` <!-- drift-ok -->
  （`setRenderTarget` / `releasePass` / 读回签名）、`Capabilities.hpp`（`Features` + `Limits`）、
  `RenderDiagnostic.hpp`（`origin`）、`RenderPass.hpp`（35 个 setter 的公开面）、
  `ChainBuilder` / demo / 链预设，以及 `.github/copilot-instructions.md` 的四条规范文字。
- 相关：`ADR-2026-11-16-redesign-seam-and-vocabulary.md`（第一轮：接缝与词汇；本文是它的**概念与 API 形状**续篇）、
  `ADR-2026-11-15-device-is-a-pool-sessions-own-frames.md`、`ADR-2026-11-13-one-engine-one-surface.md`。
- 审校与施工图：`../research/graphics-2026-11-16-redesign-r2.md`（`E-01…E-12` / `A-01…A-08` / `I-01…I-08` /
  `S-01…S-12`，框架对照，18 个批次的顺序）。

> **EN**: Round two of the redesign, on concept and API shape: one `View` value (camera, viewport, layer mask) replaces the overlapping `CameraSnapshot` / `CollectView` pair, `RenderSurface` becomes `RenderArea` so "surface" means the platform surface only, a pass target is declared with two values plus a four-role vocabulary instead of seven fields and four identically named verbs, `setRenderTarget` stops being a verb with no effect, and `PassDesc` returns to being a pure value with per-frame updates owned by objects. The adjudication table is `graphics-2026-11-16-redesign-r2.md`; approved 2026-11-16 and landed 2026-11-17.

## 背景

第一轮把**接缝与词汇**重设计了（FrameDriver / FrameDriverId / FrameHead / DiagnosticDraft / 接缝 6 机制），
能力清单 85 条与十批计划都在 `graphics-2026-11-16-redesign.md`。第二轮按用户要求再查四件事：
**抽象是否合理、能力设计是否合理、概念有没有更好的语义、统一编程规范**，并对照成熟框架。

四组事实决定了本轮的决定（都可复核）：

1. **设计名词 `View` 没有类型**：`CameraSnapshot`（7 字段）与 `CollectView`（5 字段，3 个重复）并存，
   层掩码另传；`DrawDescription` 与 `PassDesc` 因此各带三个字段。
2. **"表面"一个词两个东西**：`RenderSurface` 的文档/测试/动词都说 "area"，类型名说 "surface"；
   平台 surface 由 `createSurfaceTarget` / `WindowPresentTarget` 表达。
3. **一个 pass 的目标有 3 种声明、发布有 3 种**，`PassDesc` 里 7 个相关字段；
   `setRenderTarget` 一个名字四层含义（`RenderDevice` / `RenderPass` / `FrameRecorder` / 测试双）。
4. **`setRenderTarget` 在 vk 里是死存储**（1 写 0 读），vsg 真的建；同一动词两种语义，
   其中一种没有效果，并留下一个可悬垂的裸指针。另有 **每 pass 每帧一次 `vector<LightValue>` 分配**
   （`RenderPass.cpp:242` / `ScreenPass.cpp:82`），与"帧内零分配"相反；`PassDesc::tag` 8 写 0 读。

另外：2026-11-11 的 API 审计（`graphics-2026-11-11-next-api.md`，`P1`–`P32`）的 4 条发现**至今钉在
`check_api_style.py` 的 baseline 里**。本轮把它逐条对账（研究文档 §3），仍开的部分并入本文的批次。

## 决定

**值承载声明，行为归对象；一个概念一个名字；没有效果的动词要么变真、要么删掉。**

### 1. 一个视图值 `View`

`View { CameraSnapshot camera; Viewport viewport; std::uint32_t layer_mask; }` 成为唯一的视图值：
`DrawDescription` 用它替掉 `camera` + `collect_viewport` + `layer_mask`；`PassDesc` 的声明输入在
`describe` 时拍成它；`CollectView` 删除（一版别名）。`selectSnapshot(snapshot, view)` 只收一个值。

先例：Filament `View`、O3DE `RPI::View`、bgfx per-view 状态。判据 `G1` / `G4`。

### 2. `RenderSurface` → `RenderArea`

"surface" 只指平台表面（`createSurfaceTarget` / `WindowPresentTarget`）；引擎侧的"一块要画的面"叫
**area**（`RenderArea` / `createArea` / `areaWidth/Height`）。类自己的文档、测试名、`announceAreaSize`
已经在用 area，这条是**让名字追上语义**。判据 `G1` / `G12`。

### 3. 目标的声明是两个值：`Target` 与 `Output`

- `PassDesc::writes` + `writes_image` + `writes_shape` → **`Target`**（`Window | Explicit | Transient`；
  transient 的尺寸用 `optional<Extent>`，删掉"0 = 未指定"的魔法值）。
- `PassDesc::output_name` + `output_target` + `output` → **`Output`**（名字 + 粗/细承诺）。
- 目标词的**四个角色**写进词表：`RenderTarget`（宿主声明）/ `TargetShape`（兼容性）/ `TargetPlan`（决策）/
  `TargetObjects`（GPU 对象，替 `TargetImages` 与 vsg 的 `OffscreenTarget`）/ `TargetLease`（一帧租约）。
- **`setRenderTarget` 必须变真或消失**（见决定 5）。

先例：UE `FRenderTargetBinding`、O3DE `PassAttachment`、wgpu `RenderPassColorAttachment`。判据 `G1` / `G4`。

### 4. 一个 pass 作者形状：`PassDesc`

`PassDesc` 是唯一的作者形状；`RenderPass` 的公开面收成**只读活句柄**（`declaration()` / 名字 / 成本）。
`RenderPass` 的 35 个 setter 降为 `detail` 构建入口；链 / 预设改为"产出 `PassDesc` → `apply`"。

同一批里：删 `PassDesc::tag`（8 写 0 读）；删 `PassDesc::before_draw`，每帧更新归对象——
overlay / gizmo 的 `update()` 由 `PassChain::update()` / `SceneView::update()` 驱动；
`OverlayLayout` 改成返回 `Viewport` 的纯函数（可变 pass 引用不再逃逸）。

先例：Filament / O3DE / wgpu 的不可变描述 + Builder。判据 `G1` / `G4` / `G8`。

### 5. 没有效果的动词：变真或退役

- `RenderDevice::setRenderTarget`：**首选变真**——两个后端都做"预建/复用对象"（vsg 已经如此；
  vk 调 `ensureTarget`），于是"目标预建（不需要帧）"成为两后端同一能力（新增能力 ID `CAP-86`）。
  备选是退役（能力会少一条，需单独批准）。**无论哪条，`Impl::target` 的死存储必须删。**
- `RenderDevice::releasePass`：保留动词，**收窄契约**为"忘掉以这个 pass 身份为键的状态"；
  删掉"释放 pass 拥有的 GPU 对象 / 与目标一起释放"的句子（引擎的 `removePass` 已经分开释放）。
- 读回：两个动词都收 `ImageRef`（与 pass 输入用同一个"哪张图"词汇）；失败仍用 `ReadbackResult`，
  但把它从出参升级为返回的一部分（`S-10` 的试点）。**不加区域读回**（没有消费者，`G8`）。

### 6. 能力结构与诊断来源

- `Capabilities` 拆成 **`Features` + `Limits`**（`Capabilities` 保留为聚合；旧字段一版 inline 转发）。
  先例：WebGPU / wgpu / D3D12。判据 `G4` / `G6`。
- `RenderDiagnostic` 加 **`DiagnosticOrigin { Engine, Device, Host }`**；`diagnosticCount()` 默认只数
  引擎 + 设备，宿主注入的问题按 origin 分开数（新增能力 ID `CAP-87`）。先例：D3D12 info queue 的
  对象指针、Vulkan debug utils 的 objects。判据 `G4` / `G11`。

### 7. 稳态帧不再有灯分配

灯集是**场景**的事实，不是 pass 的字段：第一步把 `describe` 的局部向量换成成员 scratch（零分配），
第二步（可选）把它做成随发布共享的不可变 `LightSet`（`shared_ptr<const LightSet>`，revision 变才重建），
N 个 pass 共享一份。判据 `G10` / `R24` / `N6`。

### 8. 规范的四条文字修订（`S-01` / `S-05` / `S-11` / `S-09`）

1. 结构体/类的**字段**允许一行 `///`（须说清契约）；函数仍用 `/** @brief … */` 分节（现有 545 行字段注释）。
2. 访问器 Qt 风格、**命令用动词**（`getStringAsync` 这类是命令，不是访问器）。
3. 尺寸/坐标用 `int`（平台拼写）、id/计数/位掩码用定宽无符号；不回填。
4. 在 `graphics_global.hpp` 顶部写所有权词汇表：`intrusive_ptr` 共享拥有 / `raw_ptr` 借用 /
   `std::shared_ptr<const T>` 不可变共享值 / `std::span` 非拥有连续数据。

同批清扫：删 26 处 section comment；公开值字段与跨接缝枚举补固定底层类型（约 10 个，不是 37 个全改）。

## 被否决的路

| 选项 | 理由 |
|---|---|
| 引入 render graph（UE RDG / Frostbite FrameGraph） | 图编译、瞬态别名没有消费者（`G8`）；屏障已由后端单表覆盖（`G10` 的账算不过来） |
| bindless / descriptor heap / argument buffer | 没有实测瓶颈；会改内容 ABI（`G8` / `G10`） |
| 全仓换 `Result<T>`（旧 `P27`） | ~200 调用点，收益与风险不成比例（`G8`）；新 API 用值，存量随改动偿还 |
| `Session::open(config)` 一次创建（旧 `P18`） | 会藏起"引擎借用共享设备"（ADR-2026-11-15）；顺序用文档表达 |
| 把 `PassKind` 合并成投影取值（旧 `P4`） | `ScreenPass` 没有相机/内容/层，是另一种**执行**，不是投影参数 |
| 为 `ShaderProgram` 造 `ProgramBuilder`（旧 `P15`） | 没有消费者（`G8`）；两半（对象 / description）已经清楚 |
| 统一 9 个描述类型后缀、把 15 个 `Content*` 全改名（旧 `P25`） | 纯改名，收益只是好看（`G8`）；只约束新类型、只改三个最坏的 |
| 拉取式 error scope（wgpu 式） | 测试已用计数 + sink；没有消费者（`G8`） |
| 直接退役 `setRenderTarget` 而不先做 characterization | 会丢一条能力；先变真或先补用例（`G8` 的反面是"删了才发现有人用"） |
| 维持每 pass 拷灯 | 与 `R24` / `N6` 相反 |

## 后果

**好的**

- `DrawDescription` / `PassDesc` 各少两个字段；视图的三件事实只有一处定义，两个重叠值消失。
- "目标"从 7 个字段 + 4 个同名动词收成 2 个值 + 一张四角色词表；`setRenderTarget` 不再是一个
  "有动词、无效果"的接缝。
- `PassDesc` 回到纯值：可比较、可差分、可过线程；每帧更新归对象，宿主看得见自己调了什么。
- 稳态帧少 `passes` 次 alloc/free + `passes` 次灯拷贝。
- `Capabilities` / `Diagnostics` 与 WebGPU / D3D12 的形状对齐；宿主能分开"引擎的问题"与"自己的问题"。
- 2026-11-11 API 审计的遗留（`PassDesc` 回调、`OverlayLayout`、`ReadbackResult`、`tag`）有了归宿，
  `check_api_style.py` 的 baseline 可以在相应批次后清零。

**代价 / 风险（如实）**

- 决定 4（作者形状）爆炸半径最大：demo、链预设、`PassChainTest` / `PlanTest` / `RenderEngineTest`
  的声明写法都要改；必须单独一批、可整批回滚。
- 三条**行为变更**需单独批准：`before_draw` 不再隐式调用（BC-08）、vk 预建（BC-09）、
  宿主报告不再计入引擎计数（BC-10）。
- 改名（`RenderArea` / `View` / `TargetObjects`）是源码级破坏；仓库外的宿主需要一版迁移说明。
- 第一步的灯 scratch 只是"不再分配"，N 个 pass 仍各拷一份；第二步（共享 `LightSet`）才有零拷贝，
  但那要动 `DrawDescription` 的公开字段——可以只做第一步就停。

**触发条件（什么时候重开被否决的项）**

1. 出现 ROI/缩略图消费者 ⇒ 读回加区域（`A-03`）。
2. 实测 draw 数让描述符集绑定成为瓶颈 ⇒ 重新评估 bindless（`G10` 的账要重算）。
3. 出现跨帧持有 `FrameDescription` 的消费者 ⇒ 再把帧值拆成 `FrameValue` / `FrameHandles`。
4. 第三个后端（非 Vulkan 家族）⇒ 第一轮第 8 批（`DeviceFormat` / `RenderPassKey`）与本文的
   `Target` 值一起作为接缝形状的入口。

## 实施状态

- **批准（2026-11-16）**：与接缝 ADR 一同批准。决定 5 的 `setRenderTarget` 选**变真**（vk 预建，新增 CAP-86），
  不退役；决定 8 的四条规范文字与 26 处 section comment 清扫在第一批（规范清扫）落地。
- **第二轮审校**：完成（`../research/graphics-2026-11-16-redesign-r2.md`）。
- **迁移**：**完成（2026-11-17）**。合并后的 18 个批次位置见研究文档 §10；批次进度以 git 的提交正文为档案
  （每批列出能力 ID 三元组），本文只记状态。本批形状的落地提交：B12 `efc01336`（`View`）、B13 `5b22c289`
  （`RenderArea`）、B14 `001b2b80`（`Target` / `Output` / `releasePass`）、B15 `955f7a94`（`Features` /
  `Limits`）、B16a–c `5e31e89b` + `82b4ec3f` + `49645332`，以及 S8 的 B17 `7b83a509`（`Content*` 三个改名）与
  B9（文档真实性 + 用户文档按能力重组）。
- **B16（2026-11-17，E-04 / E-05 / E-06 + BC-08）已落地**：`PassDesc` 是唯一作者形状，`RenderPass` 公开面只读
  （写门是 `detail::PassNode`）；`before_draw` 由 `FrameUpdate` 对象接口取代（`PassChain::update()` /
  `SceneView::update()`），`OverlayLayout` 返回 `Viewport`；`tag` 删除。**命名偏离**：研究文档写的
  `chain->update(d)` 实现为 `PassChain::restate(desc)`——`update()` 已是"每帧更新 overlay"的动词，同名两义会
  让读者猜参数含义（`G4`）。
- 本 ADR 已于最后一批（S8）落地时改为“已采纳”；没有批次放弃。
