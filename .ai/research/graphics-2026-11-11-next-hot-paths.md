# graphics-next：性能、动画、后期与扩展

> **状态**：调研提案的一部分（**非现行设计**；未落地、未决策）
> **日期**：2026-11-11
> **属于**：`graphics-2026-11-11-next.md`（总纲）。这一份补上前面几份说得不够的四条轴：
> **性能/效率、动画、后期处理、扩展成本**。
> **纪律**：每条成本都写"今天多少、目标多少、怎么量"。这一份不重复概念与 API 的审计
> （见 `graphics-2026-11-11-next-api.md`）与骨架（见 `graphics-2026-11-11-next-design.md`）。

---

## 1. 成本模型：今天的实数与预算

### 1.1 实测（`bin/test_graphics --gtest_filter='*MeasureWhatOneCollectionCosts*'`，2026-11-11）

| 场景 | 相机移动（memo 未命中） | 静止（memo 命中） | 摊到每帧 |
|---|---|---|---|
| 200 drawable | **207 µs** | 7 µs | 9 µs |
| 2000 drawable | **2160 µs** | 37 µs | 85 µs |

读法：**今天只有"内容变了或相机动了"才付 2.16 ms**；静止画面近乎免费（memo 命中）。
这也解释了为什么今天的成本模型"看起来还行" —— 它把成本藏在了 memo 的失效判断里。

### 1.2 一帧的预算（60 fps = 16.6 ms，给渲染的按 12 ms 算）

| 段 | 目标（2000 drawable） | 今天 | 依据 |
|---|---|---|---|
| 发布（内容改动） | ≤ 50 µs（改 1 行） | 整表或逐条 | `N2` |
| fold（若每帧） | ≤ 300 µs | 缓存，不付 | 见 §2.2 |
| select + sort（每视图） | ≤ 1.5 ms | **2160 µs** | `N1` 要求 ≤ ×1.2；并行后目标 800 µs |
| record（构建命令） | ≤ 1 ms | 未单独计量 | 需新增计量点 |
| 上传 / 描述新增条目 | 稳态 0 | 稳态 0 ✓ | `N6` |
| 每帧分配 | **0**（arena 外） | 有（vector/shared_ptr） | `N6` |
| **动画**（2000 行全动） | ≤ 150 µs | 每节点 setMatrix ⇒ epoch 变 ⇒ 整表发布 | 见 §4 |

---

## 2. 热路径：每一段的手段与门禁

### 2.1 select：唯一的 O(行) 段

今天 2000 drawable 的 2160 µs 里，绝大部分是"每行一次视锥测试 + 一次排序键计算 + 一次 vector
push_back"。三处可削：

| 手段 | 预期 | 判据 |
|---|---|---|
| **块级早期剔除**（`chain[0]` 变换块包围盒，整块跳过） | 视野外内容多时 2–10× | 新用例：2000 行其中 90% 在视外 |
| **DrawItem 定长 ≤ 64 B**、写进 arena（无 `vector` 增长、无 `shared_ptr`） | 分配 0，缓存友好 | `N6` + `sizeof(DrawItem)` 静态断言 |
| **分块并行**（每块一个任务，输出到各自的 arena 段，按块序合并） | 2–4×（核数上限） | 串并行逐项相等（`R20`）+ 计时 |

**今天做不到并行**的根本原因不是没线程，而是 select 与"当前内容"耦合（要 `ensureRows`、要 memo、
要 `refresh*`）—— 纯函数化之后并行是免费的（`graphics-2026-11-11-next-design.md` §4.3）。

### 2.2 fold：要不要每帧做？

两条路只能选一条，且必须**按实测选**：

| 方案 | 2000 行成本 | 优点 | 缺点 |
|---|---|---|---|
| **A. 折叠结果留在快照**（今天 + `graphics-2026-11-11-next-design.md` 的 hot/cold 分块） | 发布时付，选中不付 | 静止/相机移动时最省 | 动画（每帧改）会变成"每帧发布" |
| **B. 每帧折叠进 arena** | 每帧 ~2000 × 一次矩阵乘（估 100–300 µs） | 动画天然免费（快照不动） | 静止画面也付 |

**结论（本提案的取舍）**：**A 用于"编辑"，B 用于"动画"** —— 见 §4。两条路共存，判据是"这个改动是不是每帧都发生"。

### 2.3 sort：全序键 + 一次 `std::sort`

2000 项 × `log2(2000) ≈ 11` ≈ 22k 次比较；用 96 位打包键（`order|layer|transparent|depth`）可降到
20–50 µs。两段排序（不透明按键、透明按键+深度倒序），不依赖 `std::sort` 的稳定性（今天的隐含依赖）。

### 2.4 record / execute：分别计量

今天 `record` 与后端 `execute` 混在一起（`buildFrameValue` + `executeFrame`），只有一个总耗时。
新设计必须在 `FrameReport` 里**分开**：`fold_micros / select_micros / record_micros / execute_micros /
upload_bytes / builds`。没有分开的数字，就无法判断优化对了没有。

### 2.5 值的三条热路径规则（这是最容易踩的一处）

1. **每帧被复制的东西必须是 POD**：`DrawItem` ≤ 64 B、无 `intrusive_ptr` 成员、无 `std::function`。
   （反例：今天的 `RenderCommand` 有 5 个 `intrusive_ptr`，每命令 5 次原子增减；2000 条 =
   1 万次原子操作/帧。）
2. **共享的大值用 `shared_ptr` 共享，不进帧**：`GeometryDescription`、`MaterialDescription` 只在
   快照里按句柄引用，帧里只有句柄。
3. **快照里的值允许带 `intrusive_ptr`**（材质 5 个纹理槽 = 5 次原子增减，一次发布一次，可忽略），
   但**必须**写清楚"它只在发布路径被复制"。

> 这条规则直接决定 `MaterialParameters` 的去向：从"每命令带的参数块"降级为"快照里按 `MaterialId`
> 引用"，命令里只留 `MaterialId`（`graphics-2026-11-11-next-design.md` §3.5 已如此）。

---

## 3. 动画：一条新的一等路径

### 3.1 今天的问题（不只是性能）

- 每帧 `node->setMatrix(pose)` ⇒ `announceContentChange` 逐级 `++epoch` ⇒ 下一次 collect 重建行、
  memo 失效、镜像重发**整表**（或按 delta 逐条）。
- **没有"每帧更新"的正式入口**（backlog `T6`：`FrameContext`/`UpdateContext` 无消费者），于是每个宿主
  自己决定"什么时候 setMatrix"，成本不可控。
- 一次配置（一个关节）改到一半时上屏是错的（C18 已经用批次解决，但批次是"人肉"的）。

### 3.2 新设计：**pose 是帧的一部分，不是内容编辑**

| | 编辑（`Instance::setMatrix`） | 动画（`FrameRequest::poses`） |
|---|---|---|
| 语义 | 改内容 ⇒ 需发布 | 这一帧的瞬时值 ⇒ **不进快照** |
| 成本 | 一个 hot 块 CoW（~10 KB） | 每行一次矩阵乘（无复制、无发布） |
| 生命周期 | 直到下一次编辑 | 一帧 |
| 批次 | `beginEdit/endEdit`（一次发布） | **一帧的 pose 天然原子**（无需批） |
| 拾取 | 用快照 | 用 `FrameResult` 里那一帧的 pose（`I7`） |

```cpp
struct PoseSet {                                  // 纯值、POD
    std::span<const PoseEntry> entries;            // {InstanceId first, uint32 count, const Mat4d* matrices}
};
struct FrameRequest {
    SnapshotPtr content{};
    PoseSet     poses{};                           // 空 = 纯静态帧
    TargetSize  size{};
};
```

派生时：`world(row) = chain × pose(row, if posed) × local(row)`。快照不动 ⇒ **发布成本 0**，
动画成本 = 每被 pose 覆盖的行一次矩阵乘（2000 行估 ≤ 150 µs，可并行）。

### 3.3 为什么这是"设计"而不是"优化"

1. **两条路径的成本不同，用哪条必须是 API 上的显式选择**（`setMatrix` vs `poses`），
   而不是"看运气"（今天就是看运气）。
2. **一帧一个 pose 是原子的**：机器人半个配置上屏的问题，在动画路径上根本不出现（不需要批）。
3. **相机/拾取一致性免费**：pose 属于帧 ⇒ "用看到的那一帧瞄准"自动成立。
4. **蒙皮/形变是下一个扩展点**：`PoseSet` 可以扩展成"顶点级的姿态函数"，但**只在有真实消费者时**
   （`G8`）—— 今天 robotics 是刚体关节，不需要。

### 3.4 今天 robotics 的用法怎么落

`app_shell` / `test_robotics_*` 的关节是刚体：一次仿真步 = 一组 `(InstanceId, Mat4d)` ⇒ 一个 `PoseSet`
⇒ 一次 `present`。于是 `T6`（update traversal）消失了 —— 更新不再是"遍历场景图"，而是"提交一个值"。

---

## 4. 后期处理：计划即依赖图

### 4.1 今天的三处别扭

| 现象 | 后果 |
|---|---|
| 目标有四种说法（`writes` / `writes_image` / shape / null=窗口）+ `transientTarget(name)` 取回 | 后处理链要"按名字取回刚建的 target"，且跨尺寸变化时指针失效 |
| 输入按**名字**（`addInputName`）或按**对象**（`addInput(Texture)` 等） | 两条路径；值化后无法差分 |
| `PassKind::Screen` + `ScreenPass` 是"种类" | 屏幕 pass 只是 `View::projection = Screen` |

### 4.2 新设计：pass 之间的依赖是**句柄**，生命周期由计划推导

```cpp
struct Pass {
    String              name{};       // 差分键
    View                view{};       // 含投影种类（Perspective | Ortho | Screen）
    Target              target{};     // Window | Transient{name,shape} | External{Handle}
    std::vector<PassId> reads{};      // 前向引用 = 计划错误（编译期校验）
    ClearPolicy         clear{};
    DrawState           state{};
    std::int32_t        order{ 0 };
};
```

| 由计划推导的东西 | 规则 |
|---|---|
| transient 的**创建时机** | 第一个写它的 pass 之前 |
| transient 的**释放时机** | 最后一个读它的 pass 之后（帧边界统一回收） |
| **ping-pong** | 两遍后期 ⇒ 声明两个 transient（`hdr_a`/`hdr_b`），差分保证复用不重建 |
| **顺序** | `order` 拓扑有效（读在前写在后）；违反 ⇒ 一条 `DiagnosticCode`，该 pass 跳过 |
| **格式/采样** | `shape` 里声明（`RGBA16F` + `samples`）；能力不足 ⇒ 协商失败（`M1`） |
| **色彩空间** | `shape.color_space`；HDR→LDR 的那一步是**一个状态值**，不是隐式约定 |

**收益**：后处理链从"宿主记得按什么顺序建什么 target、再取回句柄"变成"声明我要什么"——
差分保证稳态零重建，这正是今天 `transientTarget` 那套想做的事。

### 4.3 成本

| 项 | 今天 | 目标 |
|---|---|---|
| 每 pass 的额外 draw | 全屏三角形（1 draw） | 同 |
| transient 重建 | 尺寸变化/名字变化才重建 | 计划差分后，只有 shape 变才重建 |
| 后期链 5 遍 @1080p | 未单独计量 | record ≤ 100 µs；execute 由 GPU 决定 |

---

## 5. 扩展：加一个值 vs 加一处代码

| 想加 | 扩展方式 | 要改的文件数 | 备注 |
|---|---|---|---|
| 新节点行为（朝向、跟随） | 一个**放置值** | 1 值 + 1 消费点 | 折叠/派生不改 |
| 新着色模型 | 一个**材质 ABI 版本 + 参数块** | 1 值 + backend ABI | 现有 `ProgramAbi` 机制沿用 |
| 新几何布局 | 一个**布局描述值** | 1 值 + backend | publish 时校验 |
| 新 pass（阴影/HUD/后期） | 一个**计划取值** | 0 代码（宿主声明） | 见 §4 |
| 新查询（框选、吸附） | 一个**纯函数** | 1 函数 | 吃快照 + 视图 |
| 新后端 | 实现 `Backend` | 1 模块 | ①②③ 层不动 |
| **任意自定义 draw**（逃逸口） | `CustomPass`（见 §5.1） | 1 模块 | **明确不受"值"安全网保护** |

### 5.1 受控逃逸口：`CustomPass`

如果宿主真需要"我自己记一条命令"（第三方渲染、调试绘制、嵌入式 viewport 渲染）：

```cpp
class CustomPass : public Backend {                  // 或 Backend 的一个可选能力
    // 只在引擎线程调用；只能读 FrameValue 的值；不得触碰内容对象
    virtual void record(const FrameValue& frame, CommandBuffer& out) = 0;
};
```

**边界必须写明**（否则等于没有骨架）：
- 只允许读 `FrameValue`（值）—— 不能读 `Scene` / `Material`（否则竞争回来）；
- 只在引擎线程调用；
- 它产生的命令**不参与**排序/透明/裁剪（自己负责）；
- 不在"确定性、dump、replay"的保证内。

`G8` 的说法：这个逃逸口**今天没有消费者**（`app_shell` / robotics / test 都不需要），所以**先不做**，
但**留出接口位置**（`Pass` 里不出现任何函数指针即可，将来是额外的一个 pass 种类/能力）。

---

## 6. 工程效率（被低估的一维）

| 项 | 今天 | 目标 | 手段 |
|---|---|---|---|
| 改一个 SDK 头的重编译 | 几乎全库（`RenderEngine.hpp` 被 47 处 include） | 只重编依赖它的 TU | 值类型各放一个**独立小头**（`View`、`Pass`、`Frame` 各一个），会话头只持指针 |
| 新增一个 pass 类型的编译影响 | 触到 `PassRegistry` / `RenderPass` / 引擎编译 | 0（宿主声明） | 计划是值 |
| `test_graphics` 单次运行 | 0.25 s ✓ | 保持 | 纯函数化后更快 |
| `test_vsg` 单次运行 | 12.5 s | 保持 | |
| 门禁总时长 | 八个脚本 < 20 s | 保持（`check_api_style.py` 0.1 s） | |
| 头文件爆炸 | 75 个扁平的 SDK 头 | 5 组 × 4–6 个 | 见 API 审计 §3 |

---

## 7. 新增要求与验收（接 `graphics-2026-11-11-next-requirements.md`）

| # | 要求 | 阈值 | 怎么测 |
|---|---|---|---|
| N11 | **动画 2000 行**的派生增量 | ≤ 150 µs | 新用例 `MeasureWhatAPoseCosts`（2000 行全 pose） |
| N12 | pose 不改变快照 | `snapshot_epoch` 不变 | 用例断言 |
| N13 | 后期 5 遍 @1080p 的 record 成本 | ≤ 100 µs | `MeasureWhatAPostChainCosts` |
| N14 | transient 稳态零重建 | `builds` 增量 0 | 连续 10 帧（含 resize 后） |
| N15 | `DrawItem` 大小 | ≤ 64 B（静态断言） | 编译期 |
| N16 | 每命令的原子操作 | 0（无 `intrusive_ptr` 拷贝） | 计时 + 代码审查 |
| N17 | 块的早期剔除有效 | 视外 90% 时 select ≤ 400 µs | 新用例 |
| N18 | 扩展一次只改一处 | 新节点行为 ≤ 3 文件 | 参照实现（一个放置值） |

**建议的门禁**（与现有八个脚本并列）：`Measure*` 用例改成**阈值断言**而不是打印 ——
今天它们是"打印给人看"，于是回归无声。做法：把基线数字写成 `kBudgetMicros` 常量，超了就让用例失败。

---

## 8. 优先级（在所有已提议的批次之上）

| 顺位 | 事 | 为什么先做 |
|---|---|---|
| 1 | **`Measure*` 用例加阈值** | 没有这个，后面所有优化都不可证；成本半天 |
| 2 | `FrameReport` 分四段计量（fold/select/record/execute） | 优化方向由数字决定 |
| 3 | DrawItem ≤ 64 B + arena（去 `intrusive_ptr` 拷贝） | 一项改动同时改善 select/record/分配 |
| 4 | 块级早期剔除 | 大场景收益最大、风险最小 |
| 5 | pose 路径（动画） | 消掉 T6，且让"每帧更新"不再经过发布 |
| 6 | 计划即依赖图（后期） | 已有形状，收益是稳态零重建 |
| 7 | 并行 select / record | 等 1–4 做完再谈；数字会告诉我们还要不要 |

**顺序的理由**：1–4 都是"不改架构就能做"的（今天也能做），而它们会给出**是否值得做 5 之后**的证据。
反过来先做架构再优化，就是拿 2519 行去赌一个没量过的收益。
