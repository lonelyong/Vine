# BVH 的持久化与拾取接入 —— 设计提案

> **状态**：提案（非权威；落地后并入 `../design/`，本文件删除）
> **日期**：2026-11-07
> **范围**：`models::Mesh` 已缓存的三角形树如何被 `robotics/io` 存进包并在下次打开时复用。含几何侧需要补的最小
> 接口、文件格式归属、失效与并发规则。
> **已被取代的部分**：拾取侧（原 2.5 / 3.6 / 3.7、D5–D7、T6 / T7）已由
> `2026-11-07-geometry-triangle-bvh-unified.md` 取代——树按 draw 派生、网格源几何在 `geometryFromShape()` 处共享。
> **前置**：`../../src/base/geom/sdk/vine/geom/spatial/Bvh.hpp`、`BvhBuilder.hpp`、`../design/geometry-bvh.md`、
> `../design/graphics-geometry.md` 2.9 / F32、`ADR-2026-11-07-mesh-triangle-tree-cache.md`
> **待办**：`../backlog/graphics.md` T24（拾取接入）、`../backlog/geometry.md`

## 1. 概要设计

### 1.1 问题与背景

`Mesh` 现在缓存一棵静态三角形树（`buildBvh()`），但两件事还没发生：

- **它活不过一次进程**。机器人包（`.vdevpkg` / `.vwspkg` 是 zip：`device.xml` + geoms 的 mesh bin）今天只存网格。
  每次打开都要重新建树；SAH 建树是 `O(n log n)` 的一遍分桶扫描，几十万三角形的零件在打开时肉眼可见。
- **拾取还不用它**。`graphics::RayIntersection` 走 `traverseMesh()`：先用网格整体盒粗筛，再**线性扫全部三角形**。
  一个五十万三角形的模型，每次悬停就是五十万次 Möller–Trumbore。（拾取侧的统一设计见
  `2026-11-07-geometry-triangle-bvh-unified.md`，本文件只管“树能不能活过进程”。）

两件事看起来只是“加个 IO”和“加个分支”，但它们各自撞到同一个结构问题：

- **存的是什么**？若存节点（包围盒 + 下标），那是 `28 B/三角形` 的冗余数据，而且它可以从网格重算——存了就有
  “文件里的盒与文件里的顶点不一致”的可能。若存别的，存什么才既小又不失真？
- **拾取读的是谁的数据**？拾取路径**没有** `Mesh`：它读 `Geometry` 的通道切片与 `Primitive` 的索引切片
  （`meshOfGeometry()`），而树在 `models::Mesh` 上。两者只有在“这个 `Primitive` 画的就是那个网格”时才共享编号。

### 1.2 目标

- 树能随包存盘、随包读回，读回的树与重建的树**查询结果一致**。
- 缓存文件的正确性**不依赖它是否新鲜**：陈旧、错配、损坏的缓存只会让树变差，不会让答案变错。
- 拾取在有树时用树、无树时用今天的线性扫描，两条路径命中结果**完全一致**（可测）。
- 一个概念一个家：树只有一棵，位置在 `models::Mesh`；文件格式归 `robotics/io`；`geometry` 不认识文件。

### 1.3 非目标

- 不做增量维护（改一个三角形不改树）：缓存是全有或全无。
- 不给 OBJ / PLY / STL 这类交换格式加树：那是别人的格式，写进去别人读不懂，读进来也不可信。
- 不在 `graphics` 侧另建一棵 “draw frame” 树：先用网格的树，等实测证明 draw frame 才是瓶颈再说（`G8`）。
- 不接 `robotics/proximity`：FCL 有自己的包围体层次，两棵树并存是它的现状，不在本期。
- 不做跨进程共享内存 / 内存映射。

### 1.4 一句话结论

**存拓扑不存包围盒（`8 B/三角形`，加载时用位置重算，因此陈旧只会变差不会变错）；
持久化就是“另一种构建算法”，走同一条 `BuildJob`；文件是网格 bin 旁边的独立缓存文件；
拾取在“编号同构”可判定的前提下借用网格的那棵树，否则退回线性扫描。**

## 2. 设计要点

### 2.1 存拓扑，不存包围盒

节点数组里真正不可重算的只有**形状**：`(offset, count)` 两个 32 位整数。包围盒是位置的函数，位置在同一个包里
（mesh bin 的 positions 块），所以：

- 存 `(offset, count)`：叶子规模 4 时节点数 ≈ `n/2`，合计 `4 B/三角形`；再加图元顺序 `4 B/三角形`，共 **≈ 8 B/三角形**。
- 存包围盒（`double`）：`56 B/节点 × n/2` = **28 B/三角形**，是网格索引数据的 2.3 倍，且全是冗余。
- 加载时用一次反向扫描（孩子恒在父亲之后）把叶子盒按位置重算、内部盒合并出来——这正是 `refit()` 的算法，
  比 SAH 建树省掉分桶/分区/排序，只留一遍 O(n) 的盒子合并。

### 2.2 缓存的正确性不依赖新鲜度

因为**每一根包围盒都在加载后按当前顶点重算**，一棵来自旧模型、甚至来自另一个三角形数相同的模型的拓扑：

- 仍然是合法拓扑（`offset` 在范围内、叶子区间互不重叠且覆盖全部图元、`order` 是排列）→ 查询**结果正确**；
- 只是分组变差 → 遍历访问的节点变多。

于是这个缓存不需要指纹、不需要哈希、不需要版本比对内容：它能造成的唯一损害是“树不够好”，而 `qualityReport()`
就能看见这个损害。**这是本设计最重要的一条**：缓存可以随便丢、随便错，绝不会让拾取结果变错。

### 2.3 持久化 = 另一种构建算法

`BvhBuilder::buildWith(bvh, algorithm)` 已经接受任意外来算法（`algorithm(BuildJob&)`）。持久化不需要给 `Bvh`
加 `save()` / `load()`：

- **写**：`Bvh::topology()` 给出一份 `(offset, count)` 的只读视图，`Bvh::primOrder()`（已有）给出顺序；`robotics/io` 自己编码字节。
- **读**：`detail::RestoreTopology{nodes, order, depth}` 是一个构建算法——它按文件顺序 `appendNode()`、写入 `offset/count`，
  再走一遍反向扫描算盒子；`BvhBuilder::buildWith()` 与 SAH / 中位数切分走同一条收尾路径（`finishBuild`）。

于是“树是怎么建出来的”仍然只有一个概念：**任何建树方式都是给同一个 `BuildJob` 写内容**。若新加 `Bvh::load()`，
`Bvh` 就会多一条能把自己写成不自洽状态的路径（`G1`、`G11`）。

配套的小重构：`refit()` 的反向扫描与 `RestoreTopology` 的重算必须是**同一份实现**，抽成
`detail::refitNodes(nodes, order, bounds)`，`Bvh::refit()` 改为委托。

### 2.4 缓存放独立文件，不放进 mesh bin

`MeshBin` 的 flags 是“加了新块就不能撞到整份文件的陈述”、未知位**拒绝整份文件**、`byteCount()` 由 flags 推出。
把树塞进去的后果：

- 老版本读新包会**整份拒绝**——一个可丢的缓存把网格本身拖下水；
- 缓存不能独立删除（删了就得改 header、改 flags、改大小）；
- 交换/调试时想只带网格也变得不可能。

所以缓存是**网格 bin 旁边的独立文件**：名字由 bin 的路径派生（`geoms/part.vmsh` → `geoms/part.vbtp`），
于是 `device.xml` / 包的清单**一个字都不用改**，缺失即当没有。包里多一个文件、少一个文件都不影响旧读者。

### 2.5 拾取帧：树怎么到拾取路径手里（已由后续提案取代）

**这一节的内容已由 `2026-11-07-geometry-triangle-bvh-unified.md` 取代**：不再需要 `Geometry::sourceShape()` 反向指针和
四条“编号同构”条件的拾取时判定，改为几何按 draw 按需派生自己的树，`geometryFromShape()` 在唯一能证明
“这条 draw 流 == 这条网格流”的地方把网格的树共享进去。本文件只留下文件格式与恢复（F4 / F5 与 2.1–2.4）。

### 2.6 谁在什么时候建树

- **加载**：`robotics/io` 读到 `.vbtp` 就 `Mesh::restoreBvh(...)`；没有就不建（打开一个包不该为“也许要拾取”付建树时间）。
- **保存**：导出时若 `mesh->hasBvh()` 就写；若没有且三角形数过阈值，导出可**显式选择**建一次再写（导出选项，默认关）。
- **拾取**：绘制帧自己不读文件；它要么共享网格帧恢复出来的那棵树（`geometryFromShape()` 那一处），要么按
  `2026-11-07-geometry-triangle-bvh-unified.md` 的规则为自己那条流懒建一棵。
- **界面需要一个“准备拾取”的时机**（模型查看器打开文档后、或第一次悬停前的一次性准备），阈值（比如
  `triangleCount() >= 512`）由消费者定，树本身不掺和策略。这是 `../backlog/graphics.md` T24 的落地方式。

### 2.7 阈值

树本身没有阈值概念，因为它不知道“值不值得”。值得被明文写下来的只有两条经验数：

- 建树成本 ≈ SAH 一遍分桶，`10^5` 三角形量级在毫秒级到几十毫秒；
- 树的收益从“射线只碰到少数叶子”开始，几十个三角形的网格线性扫更快。

所以阈值是**消费者策略**：导出写不写、打开后建不建，都由消费者按 `triangleCount()` 决定；`Mesh` 只提供
`hasBvh()` / `buildBvh()` / `restoreBvh()`。

### 2.8 并发

- 建成后的树只读：`forEachRay()` / `forEachOverlap()` 是 `const` 且零分配，可被多条射线并发查询。
- `restoreBvh()` 与 `buildBvh()` 写状态，只能在“文档已独占”的加载/准备阶段调用。
- 拾取路径不加锁：它只读 `bvh()` 与 `hasBvh()`。

### 2.9 与其它消费者的关系

- `robotics/proximity`（FCL）：不接。FCL 有自己的 BVH，两棵树并存已经是它的现状（`G8`）。
- LiDAR / 批量射线：本设计之后，`Mesh::bvh()` 就是它的入口，无需新概念。
- `meshio`（OBJ / PLY）：只读不建树；写出的交换格式里没有树。

## 3. 功能点

| # | 功能 | 章节 |
|---|---|---|
| F1 | 几何侧：拓扑视图与恢复算法 | 3.1 |
| F2 | 几何侧：`refit` 与恢复共用一次反向扫描 | 3.2 |
| F3 | 网格侧：`Mesh::restoreBvh()` | 3.3 |
| F4 | IO 侧：`.vbtp` 缓存文件读写 | 3.4 |
| F5 | IO 侧：导出策略与"缺失即当没有" | 3.5 |

### 3.1 F1 几何侧：拓扑视图与恢复算法

- **行为**：`Bvh::topology()` 返回 `std::vector<BvhTopology>`（`{offset, count}` 每节点一条）；`RestoreTopology` 是
  一个满足构建算法契约的公开类型，可传给 `BvhBuilder::buildWith()`。
- **约束**（C1）：`RestoreTopology` 必须拒绝（返回空 `BuildJob` 结果）节点数不符、`offset` 越界、`order` 不是排列的输入。
- **验收**（T1）：建树 → `topology()` + `primOrder()` → 恢复进一棵空树 → `validate()` 通过且查询结果逐条相同。

### 3.2 F2 几何侧：一次反向扫描

- **行为**：`detail::refitNodes(nodes, order, bounds)` 是唯一的"由孩子重算父亲、叶子由图元重算"的实现；
  `Bvh::refit()` 与 `RestoreTopology` 都调它。
- **约束**（C2）：只依赖"孩子恒在父亲之后"这一布局不变量。
- **验收**（T2）：恢复出的树与 `buildBvh()` 出来的树在 `qualityReport()` 上可比（拓扑不同、面积不同，都是合法树）。

### 3.3 F3 网格侧：`Mesh::restoreBvh()`

- **行为**：`bool restoreBvh(std::span<const vn::geom::spatial::BvhTopology> nodes, std::span<const PrimId> order, unsigned depth)`：
  按三角形填好每个 primitive 的盒（与 `buildBvh()` 的前半段相同），再把 `RestoreTopology` 交给 `buildWith`。
  失败（非法网格、拓扑不合）时缓存保持空并返回 false。
- **约束**（C3）：一条三角形一个 primitive，`prim id == 三角形序号`（与 `buildBvh()` 同一约定）。
- **验收**（T3）：`MeshBvhTest` 增加"恢复的树与重建的树查询结果一致"一项。

### 3.4 F4 IO 侧：`.vbtp` 缓存文件

- **行为**：`<name>.vbtp` 与 `<name>.vmsh` 同目录。头部：magic `VBTP`、`u16` 版本、`u16` flags（保留）、
  `u32` 三角形数、`u32` 节点数、`u32` 叶子规模、`u32` 深度；随后节点数组（每节点 `u32 offset` + `u32 count`）、
  图元顺序（每个 `u32`）。全部小端（文件自己定义字节序，不随机器变）。
- **约束**（C4）：读取时三角形数必须等于网格的三角形数，节点数 ≤ `2 × 三角形数`，`order` 必须是 `[0, n)` 的排列。
- **要求**（R1）：任何一项不成立就**当作文件不存在**（不报错、不影响网格加载）。
- **验收**（T4）：写→读→恢复→查询一致；截断、错 magic、三角形数不符、重复顺序值四种坏文件都被忽略。

### 3.5 F5 IO 侧：导出策略

- **行为**：导出选项决定是否写缓存：`mesh->hasBvh()` 时默认写；否则默认不建；显式选项可要求"构建后写出"。
- **约束**（C5）：不写缓存的包必须能被同一版本读成"无树"，反之亦然（两个方向都不改变网格本身）。
- **验收**（T5）：`PkgIOTest` / `DeviceIOTest` 各有"带缓存 / 不带缓存"两份包都能加载。

### 3.6 F6 / F7 拾取侧（已移到另一份提案）

拾取如何拿到树、如何在几何里按 draw 派生与共享，见 `2026-11-07-geometry-triangle-bvh-unified.md` 的 F1–F7。
本节曾经的 `Geometry::sourceShape()` 反向指针方案已否决（那里 D1）。

## 4. 公开接口

| 侧 | 新增 |
|---|---|
| `geometry` | `Bvh<T, Volume>::topology()` → `std::vector<BvhTopology>`；`struct BvhTopology{ std::uint32_t offset; std::uint32_t count; }`；`vn::geom::spatial::RestoreTopology`（构建算法） |
| `models` | `Mesh::restoreBvh(nodes, order, depth)` |
| `robotics/io` | 导出选项"写出三角形树"；`.vbtp` 编解码（内部）；`DeviceIO` / `WorkcellIO` 透明地读它 |


## 5. 约束、要求与不变量

- **C1**：恢复算法拒绝不自洽的拓扑（节点数、`offset`、`order` 排列）。
- **C2**：反向扫描只有一份实现，`refit()` 与恢复共用。
- **C3**：一棵树的 primitive id 恒等于三角形序号。
- **C4**：缓存文件的每一项在读取时都被校验，任何不符都退化为"没有缓存"。
- **C5**：缓存的存与不存都不改变网格文件本身。
- **R1**：缓存可随时删除，删除只损失速度。
- **R2**：`geometry` 不认识文件格式，`robotics/io` 不认识 `Bvh` 的内部布局。
- **R3**：恢复得到的树与重建的树查询结果一致；建成后只读。
- **I1**：恢复后的树满足 `Bvh` 的三条不变量（包含性、排列、节点上界），由 `validate()` 见证。
- **I2**：缓存陈旧只会让树变差（见 2.2），因此不需要内容指纹。

## 6. 注意事项与陷阱

- **P1**：把包围盒也存进文件。怎么错：文件里的盒与文件里的顶点各自演化，微小的不一致就变成"漏命中"，而且查不出来。
  怎么防：只存拓扑，盒子一律按位置重算（2.1）。
- **P2**：`offset` 的语义是**右孩子下标**，左孩子恒为 `parent + 1`。怎么错：按线性 BVH 的"存两个下标"去写文件，
  恢复后遍历跳到父亲的兄弟。怎么判：`validate()` 的包含性检查失败。怎么防：以 `BvhTopology` 的注释为准，文件里
  只出现这一个口径。
- **P3**：`prim_order_` 忘了存。怎么错：叶子区间指向顺序数组，而顺序数组是恒等排列 → 每个叶子的图元集合全错，
  查询结果**错但看起来正常**。怎么判：`validate()` 的排列检查（叶子的顺序槽位被重复覆盖）。怎么防：缓存的
  "顺序"与"节点"是同一份数据的两半，编解码放在同一个函数里。
- **P4**：把缓存塞进 `MeshBin` 的 flags。怎么错：老读者拒绝整份文件。怎么判：用旧版本打开新包报 `InvalidData`。
  怎么防：独立文件（2.4）。
- **P5**：把恢复出来的树当成“永远最新”。怎么错：包的网格被外部工具改过，树还是旧的。
  怎么判：树仍正确（叶盒按位置重算），只是质量差，`qualityReport()` 能看出。怎么防：不用防（2.2）。
- **P6**：恢复时用文件里的三角形数而非网格的实际三角形数。怎么错：顺序数组长度与实际不符，叶子区间越界。
  怎么防：两者必须相等（C4），不等就当没有缓存。
- **P7**：strip / fan 拓扑借用三角形树。怎么错：网格序号是"三项一面"，strip 的序号是"每项一个三角形"，
  两者不是同一个编号。怎么防：拓扑必须是 `Triangles`（2.5 第 3 条）。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 存包围盒（`28 B/三角形`，文件里带浮点） | 存的是可从位置重算的冗余，还要处理端点精度与字节序；存拓扑只要 `8 B/三角形` 且文件里没有浮点（2.1）。 |
| D2 | 在 `MeshBin` 里加一个 flags 位 | 未知位会被旧读者拒绝整份文件，`byteCount()` 也要跟着变；一个缓存不该有这种权力（2.4）。 |
| D3 | 给 `Bvh` 加 `save()` / `load()` | 文件格式归 `robotics/io`（`R2`）；而且 `load()` 是绕开 `finishBuild` 的第二条建树路径，能把树写成不自洽状态（2.3）。 |
| D4 | 给缓存算内容指纹 | 陈旧缓存只会让树变差（2.2），指纹只能提升质量、不能提升正确性，却要在保存与加载各扫一遍全量数据（`G8`）。 |
| D5 | 在 `graphics` 里另建一棵 draw-frame 树 | 已由 `2026-11-07-geometry-triangle-bvh-unified.md` 细化为“按 draw 派生、网格源共享”；不再存在“只借用网格那棵树”的约束。 |
| D6 | 用 `std::function` 让恢复接口可配置 | 恢复就是构建算法，模板参数已够；再包一层类型擦除只是每次构建一次开销（`G10`）。 |
| D7 | 拾取接口收 `std::function` 加速器 | 每次拾取一次类型擦除；一个 `const Bvh*` 已经覆盖全部用例（`G10`）。 |
| D8 | 让 `geometry` 直接支持文件（哪怕只是 `.vbtp`） | 几何模块不认识文件系统是分层的既有结论（`G2`）。 |
| D9 | 缓存里存三角形顶点副本 | 与网格重复且会失同步；恢复时反正要按位置算盒（`G13`）。 |

## 8. 测试与验收

| # | 判据 | 用例 |
|---|---|---|
| T1 | 建树 → 拓扑 → 恢复 → 查询逐条相同、`validate()` 通过 | `BvhBuildTest`（几何） |
| T2 | 恢复树与构建树都是合法树，`qualityReport()` 可比 | `BvhBuildTest` |
| T3 | `Mesh::restoreBvh()` 与 `buildBvh()` 的查询结果一致 | `MeshBvhTest` |
| T4 | `.vbtp` 写读一致；四种坏文件被忽略 | `tests/test_robotics_io`（新用例） |
| T5 | 带 / 不带缓存的包都能打开 | `PkgIOTest`、`DeviceIOTest` |

## 9. 状态与演进

实施顺序（每一步都能独立提交、独立验收）：

1. **几何**：`BvhTopology` + `Bvh::topology()` + `RestoreTopology` + `detail::refitNodes` 重构；测试 T1 / T2。
2. **网格**：`Mesh::restoreBvh()`；测试 T3。
3. **IO**：`.vbtp` 编解码 + 导出选项 + 加载时透明恢复；测试 T4 / T5。

已知缺口（本期不做）：draw-frame 树、增量维护、跨进程共享、`robotics/proximity` 接入。

## 10. 参考

- 代码：`spatial/Bvh.hpp`、`BvhBuilder.hpp`、`detail/BvhBuilders.hpp`、`models/Mesh.hpp`、
  `robotics/io/src/MeshBin.hpp`、`viz/graphics/src/RayIntersection.cpp`、`viz/graphics/sdk/vine/graphics/Geometry.hpp`
- 设计：`../design/geometry-bvh.md`、`../design/graphics-geometry.md`（2.6 缓存键、2.9 / F32）、
  `../design/geometry-ray.md`、`../design/robotics-io-design.md`
- 决策：`ADR-2026-11-07-bvh-as-structure-plus-builder.md`、`ADR-2026-11-07-mesh-triangle-tree-cache.md`
