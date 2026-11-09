# geometry 包围体层次（BVH）

> **状态**：现行（结构与构建器已落地，`Bvh<T, Aabb<T>>` 两个精度均导出；`Obb` 体积经实测否决，见 `D2` / §9.2）
> **最后核对**：2026-11-08
> **范围**：`src/base/geom/sdk/vine/geom/spatial` 的 `Bvh.hpp`、`BvhBuilder.hpp`、`BvhTopology.hpp`、`TriangleBvh.hpp`
> 与 `spatial/detail/` 下的构建内核；记录一棵静态包围体层次的归属、结构 / 构建边界、查询契约、失效规则、拓扑存储
> 与“三角形集 → 树”的编号规则。
> 不含场景遍历与拾取身份（见 `graphics-scene.md`）、不含射线–图元相交本身（见 `geometry-ray.md`）。
> **权威来源**：`Bvh.hpp`、`BvhBuilder.hpp`、`BvhTopology.hpp`、`TriangleBvh.hpp`、`detail/VolumeTraits.hpp`、
> `detail/BvhBuildJob.hpp`、`detail/BvhBuilders.hpp`、`tests/test_geom_spatial/Bvh*Test.cpp`、`TriangleBvhTest.cpp`。
> **待办**：`../backlog/geometry.md`（动态插入删除、并行构建等开放项）

## 1. 概要设计

### 1.1 问题与背景

射线–图元相交已经在 `geometry-ray.md` 落地，但一次查询要对全部图元逐个求交：图元数上千后，每帧的拾取与
LiDAR 仿真都会退化成全扫。需要一层加速结构，并且这层结构要能被**任何**“有包围体、能求交”的消费者复用，
而不是长在渲染侧的场景图里。

要解决的问题不止“写一个 BVH”，而是四条当时没有答案的问题：

- **结构放哪**：`brep`（`base`）与 `graphics`（`viz`）都要用；放进任一侧都是回边（`G2`）。
- **构建策略怎么加**：中位数切分、SAH、以后可能的上千种启发式；如果写进树里，每加一种就改一次核心类型（`G9`）。
- **不同体积怎么支持**：`Aabb` 与 `Obb` 的合并、相交、射线测试都不同，但不能因此让树知道具体类型。
- **buffer 怎么复用**：构建一次要几 MB，重建（模型改动）不能每次都从零分配（`G10`）。

### 1.2 目标

- 一个**结构**：持有图元的包围体、一颗扁平节点树、一次查询遍历；不知道图元是什么、不知道树是怎么建出来的。
- 一个**构建器**：以可组合的算法把结构建出来，可配置质量与叶子规模，**不碰**查询、不碰失效规则。
- 体积类型可替换：树只通过 `detail::VolumeTraits<V>` 取得它需要的九个操作。
- 构建期零临时分配增长：节点数组与图元顺序数组复用容量，重建只写不申请。

### 1.3 非目标

- 不做动态插入 / 删除：树是**静态**的，建成后只允许 `update()` 改包围体 + `refit()` 收拢，或显式重建（`D3`）。
- 不做场景图遍历、实例化、世界矩阵与拾取身份：那是 `graphics-*` 的事。
- 不做 GPU 端结构（LBVH、光线包遍历）。
- 不提供“自动选算法”：质量档位只有两档，额外启发式由调用方经 `buildWith()` 注入（`G8`）。

### 1.4 一句话结论

**`Bvh` 是一棵静态包围体层次：它持有图元的包围体、维护三个不变量、提供重叠与射线两种遍历；
构建是独立类型 `BvhBuilder`，通过 `detail::BuildJob` 这唯一接口写树；体积类型由 `VolumeTraits` 适配。**

## 2. 设计要点

### 2.1 两个类型，一条单向边

`Bvh<T, Volume>`（`spatial/Bvh.hpp`）是**结构**：图元包围体数组、图元顺序、节点数组、质量统计、查询遍历、`refit`、
`validate`。它唯一暴露的构建入口是给 `BvhBuilder` 的私有 `friend` 接口。

`BvhBuilder`（`spatial/BvhBuilder.hpp`）是**构建者**：`leafSize` / `maxDepth` / `quality` 三个可链式配置，
`build(bvh)` 按档位分派，`buildWith(bvh, algorithm)` 接受任意外来算法。

边只有一条：`BvhBuilder.hpp` → `Bvh.hpp`。反向依赖由 `scripts/check_geom_layering.py` 规则 6 拒绝
（`spatial/Bvh.hpp` 不得 include `spatial/BvhBuilder.hpp`）。理由（`G1`、`G2`、`G12`）：

- 树能回答的问题只有“有哪些图元、它们的盒子在哪、怎么遍历”，这些问题与“谁把它排成这样”无关；
- 反过来一旦树知道构建器，加一种算法就要改树，且树的两个使用者被迫一起重编；
- 判据是**可执行的**：删掉 `BvhBuilders.hpp` 与 `BvhBuilder.hpp`，`Bvh.hpp` 单独编译必须仍然通过。

### 2.2 构建算法看到的唯一接口：`BuildJob`

`detail::BuildJob<T, Volume>` 是两个头文件之间的**唯一**契约：

| 成员 | 语义 |
|---|---|
| `prim_bounds` | 只读的图元包围体视图 |
| `order` | 可写的图元顺序（算法只做 `swap` / 分区 / `nth_element`） |
| `nodes` | 节点**组合学**数组的引用（`BvhNodeTopology`），算法用 `appendNode()` 追加 |
| `node_bounds` | 与 `nodes` **平行**的节点体积数组，`appendNode()` 让它一起增长 |
| `leaf_size` / `max_depth` / `depth` | 本次构建的规模参数 |
| `rangeBounds` / `writeLeaf` / `appendNode` | 三个原语：算一组的合并包围盒、写叶子、追加内部节点 |

两条节点数组由 `appendNode()` 一起追加，所以“节点数与节点盒数相等”是接口形状保证的，不是一句约定（`C12`）。

算法只拿到“能读盒子、能重排顺序、能追加节点”这三件事，因此它无法破坏不变量，也无法触碰查询状态。
`nodes` 是 `std::vector<Node>&` 而不是预分配窗口：叶子数与切分方式相关，预先按 `2·⌈n/leaf⌉+1` 算容量会在
叶子规模 5、切出 2+3 时越界；容量由 `prepareForBuild()` 清空但保留，追加时自然增长，重建因此不再分配。

### 2.3 `Volume` 契约：traits 适配，不改已有类型

`detail::VolumeTraits<V>` 声明树需要的九个操作：`merged`、`center`、`halfArea`、`overlaps`、`contains`、
`corners`、`isFinite`、`hitByRay`，外加类型别名 `Scalar` / `VolumeType`。`Aabb<T>` 的适配是精确的：
`merged` 走 `expandBy`，`overlaps` 六次比较，`hitByRay` 复用 `detail::RaySlab.hpp` 的 `preparedRayBoxInterval`
（盒查询走同一份实现，见 `geometry-ray.md`）。

`merged` 允许**保守**（包含两者即可，不必等于并集），但不允许**不包含**：包含性是树正确性的唯一前提，`validate()` 会检查。
`halfArea` 而不是体积：SAH 按表面积计费，退化成矩形的盒子体积为 0 但表面积有意义。

### 2.4 节点布局与遍历顺序

节点的**组合学**是 `BvhNodeTopology` 两个字段、8 字节；它的包围体在**平行的** `node_bounds_` 数组里（`Aabb<double>` 下 24 字节）：

- `offset`：内部节点是**右孩子的下标**，叶子是**图元顺序数组中的起始槽位**；
- `count`：叶子为该组图元数，内部节点为 0；
- `node_bounds_[index]`：该子树（叶子：该组图元）的合并包围体——同一棵树里同一位置的另一条数组。

两条数组分开而不是一个 AoS 结构，理由是**存储**：组合学那条同时就是 `BvhTopology::nodes`，所以
`topologyView()` 不需要拷贝、`restoreTopology()` 可以把整组数组移动进来（2.13、2.15）。代价是遍历每个节点
多一次载入（`node_bounds_[i]` 与 `nodes[i]`）；而 `refit()` 只扫体积那条，反而比 AoS 更集中。

**左孩子恒为 `parent + 1`**，右孩子由 `offset` 给出。这条规则让内部节点少存一个下标（省 4 字节/节点），
代价是遍历时必须写 `nodes_[index + 1]` 而不是 `nodes_[node.offset - 1]`——后者是本轮实现里真实踩到的一次 bug（`P1`）。

### 2.5 三个不变量

- **I1 包含性**：任意节点的 `node_bounds_[i]` 包含其子节点的所有角点（叶子：包含组内每个图元包围体的八个角点）。
- **I2 排列**：`shape_.order` 是 `[0, primCount)` 的一个排列，叶子的槽位区间互不重叠且覆盖全区间。
- **I3 节点上界**：`shape_.nodes.size() <= 2 · primCount`。

三者由 `validate()` 全量检查（`O(n)`，一次 `std::vector<bool>` 分配，只在测试与断言里调用，不进入查询路径）。
不变量被破坏时 `validate()` 返回 `false` 而不是抛异常：查询是 `noexcept` 逻辑的一部分，校验是可选的开发期工具。

### 2.6 构建：两级质量

- `BvhBuildQuality::kSah`（默认）：12 桶的 binned SAH。三个轴 × 12 桶双向扫描算代价，取最优划分再 `std::partition`。
  划分退化（全落一侧或代价非有限）时退回中位数切分，保证**任何输入都能建成**。
- `BvhBuildQuality::kFast`：沿最长轴按中心中位数切分（`nth_element`）。

两者都由 `detail::buildTopDown<T, Volume, Split>` 这一个递归模板驱动，差别只在“选哪个切分点”，
递归在 `count <= leaf_size` 或 `depth + 1 >= max_depth` 时写叶子。

递归深度由 `max_depth` 保证（`kBvhMaxDepth = 64`），因此构建不需要显式栈；`83 KB` 级别的递归栈在这个上界下是安全的，
而 `83 KB` 的显式栈需要堆分配，反而引入 `G10` 的成本。

### 2.7 `refit` 属于结构，质量由调用方裁决

图元移动时 `update(id, bounds)` 只改 `prim_bounds_[id]` 并把树标记为“需要 refit”；`refit()` 一次反向扫描
（孩子恒在父亲之后，所以一次 `for (index = size; index-- > 0;)` 就是合法后序）重算所有节点盒。
它不改变拓扑，因此反复 update + refit 会让叶子变松；`qualityReport()` 给出叶子表面积之和与深度，由调用方决定何时重建。

### 2.8 射线查询：一次预处理，最近优先

`forEachRay(ray, visit, max_t)` 先构造 `detail::PreparedRay`（倒数与符号位，节点测试因此没有除法），
再深度优先、**近孩子先入栈**，并用当前 `limit` 剪掉入口距离已超限的分支。

`visit(id, bounds, max_t&)` 可以写回更小的 `max_t`——这就是最近命中查询的剪枝方式：命中图元后把距离写回，
后续节点自动被剪掉。回调返回 `false` 立即终止遍历，返回值不参与语义。

### 2.9 查询接口的选择：回调，而不是返回容器

`forEachOverlap` / `forEachRay` 都是回调 + `const` + 调用栈上的 `std::array<PrimId, kMaxDepth + 2>` 栈，
**零分配**。返回 `std::vector<PrimId>` 会让“最近命中”这种只要一个结果的查询也分配一次数组，
而“收集全部候选”只是回调十行的事（`G7`：复杂度吸收在遍历里，不推给调用方去管理容器）。

### 2.10 buffer 复用

`prepareForBuild()` 清空节点数组与顺序数组但**保留容量**，`reserve(prim_count)` 供调用方按图元数预留。
一次构建后的重建（同一规模的模型改动）不产生新的分配；`clear()` 同理会保留容量。

### 2.11 线程与重入

结构不是线程安全的：`add` / `update` / `build` 都写状态。**建成后并发查询是安全的**（查询全部 `const` 且不分配），
这是刻意的设计点：构建是偶发的、单线程的；查询是频繁的、可以多线程的。

### 2.12 错误契约

- `add()` 在数组已满（`PrimId` 上限）时返回 `kInvalidPrim`，不抛异常；
- 用空结构查询立即返回（`shape_.nodes.empty()` 早退），不崩；
- 非法参数（`leaf_size == 0`、`max_depth == 0`）由 `prepareForBuild()` 拒绝并返回 `false`，`BvhBuilder::build` 于是什么都不做；
- 构建不会因为输入退化（全重合、NaN）而崩溃：退化时退回中位数切分，NaN 由 `isFinite` 可观测。

### 2.13 拓扑可存储、可恢复：盒子不存

树里只有一半是构建不可廉价重算的：**分组与顺序**。每个节点的包围盒是图元位置的函数，所以存储只带一个值
`BvhTopology{nodes, order, depth}`（每节点一个 `BvhNodeTopology{offset, count}`），恢复时用 `detail::refitNodes()`
一次反向扫描按当前图元重算全部盒子。这条决定了四件事（`G13`、`G10`、`G1`）：

- 文件里没有浮点，也不存在“文件里的盒与文件里的顶点各自演化”这种只能靠容差掩盖的不一致；
- 陈旧或来自另一个模型的拓扑只能是**变差的树**，不会是错答案，因此不需要内容指纹；
- 恢复不是 `Bvh::load()`，而是**两个入口、同一份检查**：手里的只有视图时走 `BvhBuilder::buildWith()`
  的又一个算法（`RestoreTopology`），与中位数 / SAH 走同一条 `finishBuild` 收尾；自己持有那个值时走
  `Bvh::restoreTopology(BvhTopology)`，把数组移动进来。两条路都读 `detail::installableHeight()`，
  所以“什么形状能装进这棵树”只有一处定义；
- **形状是一个值**：节点的 `offset` 指向 `order`，`depth` 说的是这组节点的实际高度，三者离开彼此就没有意义，
  所以它们进一个类型而不是三个平行数组（`G1`：一个概念一处定义；`G4`：调用方不可能把甲树的节点配上乙树的顺序）。

`detail::refitNodes()` 因此是“由孩子重算父亲、叶子由图元重算”的唯一实现，`refit()` 与两条恢复路径都调它。

**形状就是树的存储，不是它的转录。** 树的节点组合学直接按 `BvhTopology` 的形状存放（`shape_.nodes` /
`shape_.order` / `shape_.depth`），体积放在平行的 `node_bounds_` 里。三件事因此成立（`G10`、`G1`）：

- `topologyView()` 零拷贝；`.vbtp` 写入直接编码树自己的数组；
- 恢复可以**移动**而不是逐节点抄：解码得到的 `BvhTopology` 交给 `restoreTopology()` 就完事；
- 旧实现里 `BvhNode` 与 `BvhNodeTopology` 是同一约定的两份转录，一次 `topology()` 在 38M 三角形上要复制
  ≈ `1 GB`（`≤2n` 个节点 × 8 B + 113M `order` × 4 B），现在这个拷贝不存在了。

代价与边界：同一约定只有 `BvhNodeTopology` 一份，树与文件都存它；节点体积不进文件（2.13 第一条理由），
所以“树里有什么”与“文件里有什么”差一条平行数组，而不是差一个结构体。

### 2.14 三角形树只有一个编号规则：`TriangleSource` + `registerTriangles()`

`Bvh` 不认识三角形：它收的是包围体。于是“一个三角形一棵 primitive、primitive id == 三角形序号”这条规则必须住在
`Bvh` 之外、`Mesh` 与 `Geometry` 之下的**公共层**，而这条规则需要的输入只有三件事——“有哪些顶点、有多少三角形、
第 `t` 个三角形的三个角点是谁”。三者拼成一个概念（`G1`）：

- `TriangleSource`（`spatial/TriangleBvh.hpp`）是那个概念的 C++20 `concept`：`positions()`、`triangleCount()`、
  `triangleCorners()`。它用 Qt 风格取值器书写，和库里其它值类型一样。
- `registerTriangles(tree, source)` 是**唯一**的编号与盒规则：一三角形一 primitive、顺序即序号、盒子是三角角点的
  `double` 包围盒（宽化精确，因此叶子在小尺寸远离原点时仍然准）。角点解不出或坐标非有限时返回 `false`——
  “没有树”是一个返回值，不是从拾取栈里飞出去的异常。调用方仍可以拒绝登记（例如 `isValid()` 不成立的 mesh），
  但那段循环只有一份。
- `buildTriangleBvh()` / `restoreTriangleBvh()` 把“先登记、再建树”与“先登记、再装存储的形状”合成一次调用，
  于是模型帧与绘制帧各写一行，而不是各写一次循环。
- 两侧各自用 `static_assert(TriangleSource<T>)` 钉住：`Mesh` 与 `TriangleStream` 都必须是三角形源，否则这两个帧
  的编号可以悄悄分叉（`G6`、`G11`）。

### 2.15 形状的借用形式：`BvhTopologyView`

`BvhTopology` 拥有一组数组，而读一个形状的人（编解码、恢复、写入器）只需要看它。所以形状有两种形态（`G4`）：

- `BvhTopology`：拥有的值——一次解码的结果、树自己持有的那一份、一个可以比较的值、`Bvh::topology()`
  的返回（那是一次拷贝，所以只需要读或编码时用 `topologyView()`）；
- `BvhTopologyView`：三个 `std::span` 加一个高度，**显式**借用（“我借用它”是类型而不是注释）；
  拥有形可直接隐式转换到借用形（更强承诺到更弱承诺，丢掉的只有所有权，同 Qt 的 `QByteArray` → `QByteArrayView`）。

`RestoreTopology` 因此只持有一个 `BvhTopologyView` 成员：它不再需要三条平行跨度，也不再需要用 `@warning` 提醒调用方
“别把拓扑移走”——参数类型已经说了。两个方向因此都不经过中间拷贝：写 `.vbtp` 时编码的是
`Bvh::topologyView()`（树自己的数组）；读入时解码出一个拥有的 `BvhTopology`，再由 `Mesh::restoreBvh(BvhTopology)`
**移动**进树。只有“借用”和“移动”，没有“复制”。

## 3. 功能点

| 编号 | 功能 | 章节 |
|---|---|---|
| F1 | 图元登记与批量登记 | 3.1 |
| F2 | 包围体更新与 `refit` | 3.2 |
| F3 | 构建（两档质量 + 外来算法） | 3.3 |
| F4 | 重叠查询 | 3.4 |
| F5 | 射线查询与最近命中剪枝 | 3.5 |
| F6 | 不变量校验与质量报告 | 3.6 |
| F7 | buffer 复用与释放 | 3.7 |
| F8 | 拓扑视图与恢复（`BvhTopologyView` / `RestoreTopology`） | 3.8 |
| F9 | 三角形集 → 三角形树（`TriangleSource`） | 3.9 |

### 3.1 F1 图元登记

- **行为**：`add(bounds)` 追加一个图元，返回 `PrimId`，并把结构标记为未构建；`addRange(count, bounds_of)` 批量追加，
  一次预留容量。
- **约束**（C1）：登记不建树；登记后必须 `build` 才能查询（此前查询返回空）。
- **要求**（R1）：批量登记一次 `reserve`，逐个 `add` 不应被用来循环一万次。
- **验收**（T1）：`BvhBuildTest` 比对逐个 `add` 与 `addRange` 的 `primCount` 与查询结果一致。

### 3.2 F2 更新与 `refit`

- **行为**：`update(id, bounds)` 改写单个图元的包围体，`refit()` 重建全部节点盒而不动拓扑。
- **约束**（C5）：`refit` 之后叶子盒是**精确**的合并（没有变形的图元时），内部盒是保守合并。
- **注意**（N2）：反复 update 后叶子会变松，树质量退化但结果仍然正确。
- **验收**（T2）：`BvhBuildTest` 平移全部图元后 `refit` + `validate` 通过，查询结果与重建后的树一致。

### 3.3 F3 构建

- **行为**：`BvhBuilder{}.leafSize(8).quality(kFast).build(bvh)`；`buildWith(bvh, algorithm)` 接受满足
  `BvhBuildAlgorithm<T, Volume>` 概念（C++20 `concept`：可调用、以 `BuildJob&` 为参、返回节点数）的外来算法，
  由调用方自己保证它也维护三个不变量。
- **约束**（C2）：构建是幂等的：同一输入同一配置产生**相同**的树（`nth_element` 的等价元素顺序不保证，
  但划分结果保证；测试因此只断言结构与质量，不断言具体下标）。
- **要求**（R2）：任何配置与任何输入（含全部重合、退化、NaN 盒子）都必须产出一棵 `validate()` 通过的树或干脆不建。
- **验收**（T3）：`BvhBuildTest` 覆盖 SAH / 中位数两档、叶子规模 1 / 4 / 64、`max_depth` 截断、
  1000 个随机盒子的 `validate` 与质量对比。

### 3.4 F4 重叠查询

- **行为**：`forEachOverlap(query, visit)` 访问每个**叶子盒与 `query` 相交**的图元。
- **约束**（C3）：结果是**超集**：叶子盒是保守合并，被访问的图元仍可能在精确测试下不相交（`N1`）。
- **验收**（T4）：`BvhQueryTest` 用暴力全扫的结果作为**子集**比对（`expectCoversExactAnswer`）。

### 3.5 F5 射线查询

- **行为**：`forEachRay(ray, visit, max_t)` 访问每个叶子盒被射线在 `[0, max_t]` 内进入的图元；
  `visit` 可写回 `max_t` 收紧，`return false` 立即终止。
- **约束**（C4）：零方向射线（`direction_length2 <= eps²`）不访问任何图元，也不产生 NaN（沿用 `geometry-ray.md` C3）。
- **注意**（N3）：近孩子优先只是**访问顺序**，不保证“第一个回调就是最近命中”——图元本身还要按 `t` 比较。
- **验收**（T5）：`BvhQueryTest` 比对射线查询结果与暴力全扫、`max_t` 剪枝、最近命中只回调一次（写回后剪掉其余）。

### 3.6 F6 校验与质量

- **行为**：`validate()` 全量检查 I1–I3；`qualityReport()` 返回 `{prim_count, leaf_count, leaf_half_area_sum, depth}`。
- **约束**（C6）：`validate()` 会分配（`std::vector<bool>` 标记已用的顺序槽位），不进查询路径。
- **验收**（T6）：`BvhBuilderContractTest` 断言构建后 `validate()` 为真，并断言破坏不变量时能被检出（测试内手动破坏一棵树的节点盒）。

### 3.7 F7 buffer 复用

- **行为**：`reserve(prim_count)` 预留图元容量；`prepareForBuild` 清空但保留节点与顺序容量；`clear()` 清空一切但保留容量。
- **约束**（C7）：重建不产生新分配（容量足够时）。
- **验收**（T7）：`BvhBuilderContractTest` 记录首次构建后的 `capacity`，重建后断言容量未变。

### 3.8 F8 拓扑视图与恢复

- **行为**：树的形状就是它自己的三条数组，所以有两条读法和两条写入方式：
  - `topologyView()` **借用**树自己的数组（零拷贝），`.vbtp` 写入直接用；`topology()` 返回拥有的那份（一次拷贝，
    给需要值或要改的调用方）；
  - 自己拥有形状时 `Bvh::restoreTopology(BvhTopology)` 把数组**移动**进来、重算全部盒子；手里的只是视图时
    `RestoreTopology{shape}` 作为构建算法交给 `BvhBuilder::buildWith()`，按存储的分组重建节点、写入顺序，
    再用 `detail::refitNodes()` 按当前图元重算盒子。
  - 节点深度优先，`offset` 的含义见 2.4；`order` 就是 `primOrder()` 的那份排列，`depth` 是树自己的高度，
    `primitiveCount()` 就是 `order` 的长度。
- **约束**（C8）：恢复只有一份接受检查（`detail::installableHeight()`，两个入口都读它）：图元数与形状都非空 →
  `order` 长度等于图元数 → `order` 是图元的一个排列 → 每个节点的 `offset + count` 不越过图元数 → 每个节点都从
  根可达且实测高度等于声明的 `depth` → 高度不超过 `max_depth`。任一条不满足就不建树，不做部分恢复。
- **要求**（R5）：两条路径恢复出的树都与重建出的树对同一查询逐条相同；盒子只有一份定义——`detail::refitNodes()`
  只写节点体积那条数组，碰不到组合学。
- **验收**（T10）：`BvhBuildTest.ARestoredTopologyAnswersTheSameQueries`、
  `ARestoredTopologyThatDoesNotDescribeTheTreeIsRefused`、`AnOwnedTopologyIsAdoptedByTheTreeItself`、
  `ATopologyViewBorrowsTheTreesOwnArrays`；`TriangleBvhTest.AMovedShapeIsAdoptedInsteadOfCopied`。

### 3.9 F9 三角形集 → 三角形树

- **行为**：`TriangleSource` 是一个 `concept`（`positions()` + `triangleCount()` + `triangleCorners()`）；
  `registerTriangles(tree, source)` 按三角形序号登记一个 primitive 一个；`buildTriangleBvh(tree, source, builder)` 与
  `restoreTriangleBvh(tree, source, shape)` 各自把“登记 + 建树 / 登记 + 恢复”合成一次调用。
- **约束**（C10）：三角形树的图元编号只有一个定义（`registerTriangles()`）：primitive id == 三角形序号，两侧一致。
- **约束**（C11）：角点解不出、坐标非有限、树非空、源没有三角形时返回 `false`，**不抛异常**（`Bvh::add()` 的
  `invalid_argument` 在拾取路径上是不可接受的失败形态）；拒绝时可以已登记了前缀，调用方丢弃那棵树即可。
- **要求**（R6）：`Mesh` 与绘制帧的 `TriangleStream` 都必须静态满足 `TriangleSource`（两侧各一个 `static_assert`）。
- **验收**（T11）：`TriangleBvhTest`：primitive id == 序号、一条调用建树、坏角点与非有限坐标被拒、恢复与重建
  逐条相同、外来形状被拒、非空树不被重编号。

## 4. 公开接口

| 类型 / 文件 | 内容 |
|---|---|
| `spatial/Bvh.hpp` | `Bvh<T, Volume = Aabb>`：`add` / `addRange` / `update` / `reserve` / `clear`、`primCount` / `empty` / `isBuilt`、`forEachOverlap` / `forEachRay`、`refit`、`validate`、`qualityReport`、`primOrder`、`topology` / `topologyView` / `restoreTopology`；`kBvhMaxDepth = 64` |
| `spatial/BvhTopology.hpp` | 纯词汇，**只依赖标准库**：`BvhNodeTopology{offset, count}`（一个节点的组合学，值类型带 `==`，`static_assert` 平凡可拷贝）；`BvhTopology{nodes, order, depth}`（整棵树的形状，`primitiveCount()` / `view()` / `==`）；`BvhTopologyView{nodes, order, depth}`（同一形状的借用形式，`primitiveCount()` / `empty()`，2.15） |
| `spatial/TriangleBvh.hpp` | `TriangleSource`（三角形源的 `concept`）；`registerTriangles` / `buildTriangleBvh` / `restoreTriangleBvh`（视图与移动两个重载，2.14） |
| `spatial/BvhBuilder.hpp` | `BvhBuildQuality{kFast, kSah}`；`BvhBuildAlgorithm`（构建算法的 `concept`）；`BvhBuilder`：`leafSize` / `maxDepth` / `quality`、`build(bvh)`、`buildWith(bvh, algorithm)`；`RestoreTopology{shape}`（把存储的分组当构建算法装进去） |
| `spatial/detail/BvhBuildJob.hpp` | `BuildJob<T, Volume>`（`prim_bounds` / `order` / `nodes` / `node_bounds` + 三个原语）——**内部契约**，不属于公开 API |
| `spatial/detail/BvhBuilders.hpp` | `MedianSplit`、`SahSplit`、`buildTopDown`、`SahBuild`、`MedianBuild`——内部 |
| `spatial/detail/BvhRefit.hpp` | `refitNodes(nodes, node_bounds, order, prim_bounds)`：由图元重算叶子、由孩子重算父亲——内部 |
| `spatial/detail/BvhTopologyCheck.hpp` | `orderIsPermutation`、`nodesFitPrimitives`、`measuredHeight`、`installableHeight`（恢复的唯一接受检查）——内部 |
| `spatial/detail/VolumeTraits.hpp` | `VolumeTraits<V>` 与 `VolumeTraits<Aabb<T>>`——内部 |

完整签名与注释在头文件；本文件只记录归属、边界与理由。

## 5. 约束、要求与不变量

- **C1**：登记不建树；未构建的树查询返回空。
- **C2**：构建对同一输入与配置产生同构的树；不保证等价元素的相对顺序。
- **C3**：重叠查询结果是超集；精确判定由调用方对图元自己做。
- **C4**：零方向射线不产生访问，不产生 NaN。
- **C5**：`refit` 不改拓扑，只重算盒子；叶子因此可能变松。
- **C6**：`validate()` 允许分配且只用于开发期；查询路径不分配。
- **C7**：容量足够时重建不产生新分配。
- **C8**：恢复只接受自洽的拓扑（见 F8）；视图与拥有值两个入口读同一份检查（`detail::installableHeight()`）；不自洽 = 不建树，不是部分恢复。
- **C9**：`refit()` 与两条恢复路径共用 `detail::refitNodes()`——盒子的定义只有一处，且它写不到组合学。
- **C10**：三角形树的图元编号只有一处定义：`registerTriangles()`（primitive id == 三角形序号），`Mesh` 与 `Geometry` 都走它。
- **C11**：`registerTriangles()` / `buildTriangleBvh()` / `restoreTriangleBvh()` 不抛异常：不可用的输入是 `false`（可已登记前缀，调用方丢弃那棵树）。
- **C12**：`shape_.nodes` 与 `node_bounds_` 恒同长：只有 `appendNode()` 能增长二者，`refitNodes()` 只写后者。
- **I1 / I2 / I3**：见 2.5。
- **R1**：批量登记一次预留；逐条 `add` 只用于少量图元。
- **R2**：任何输入都必须产出 `validate()` 通过的树，或明确不建，不得崩溃。
- **R3**：查询 `const` 且无分配，可并发调用；写入接口不可并发。
- **R4**：`Bvh.hpp` 不依赖 `BvhBuilder.hpp`；`check_geom_layering.py` 规则 6 强制。
- **R5**：两条路径恢复出的树与重建出的树对同一查询逐条相同；恢复不引入第二条构建内核（视图走 `buildWith`，拥有值走 `restoreTopology`，两者都用 `refitNodes()` 长肉）。
- **R6**：`Mesh` 与 `TriangleStream` 各有一个 `static_assert(TriangleSource<...>)`：两侧的三角形契约不许分叉。
- **N1**：查询回调拿到的盒子是叶子的保守合并盒，不是图元的精确几何。
## 6. 注意事项与陷阱

- **P1**：叶子盒是保守合并，查询结果是**超集**。怎么错：把回调里的命中当成最终答案，选中了其实没相交的图元。
  怎么判：用暴力全扫做对照，断言“全扫结果 ⊆ BVH 结果”。怎么防：回调里对图元做精确相交。
- **P2**：左孩子下标是 `parent + 1`，右孩子是 `node.offset`。怎么错：照抄线性 BVH 的 `node.offset - 1` 当左孩子，
  遍历到自己的父亲或越界。怎么判：小规模手算节点数组下标，或 `validate()` 报 I1 失败。怎么防：只用 `index + 1` 与
  `node.offset` 两种写法。
- **P3**：`PrimId` 是 32 位。怎么错：超过 40 亿图元时溢出（实际不可能，但 `add` 会返回 `kInvalidPrim`，调用方要检查）。
- **P4**：SAH 退化成“全在一侧”时不处理会无限递归。怎么错：栈溢出。怎么判：`SahSplit` 返回 0。怎么防：`buildTopDown`
  检测到退化划分立即改走中位数切分，中位数切分至少把区间分成两段。
- **P5**：`leaf_size` 与 `max_depth` 相互作用。怎么错：`max_depth` 太小会让叶子远超 `leaf_size`（树被截断，叶子变成大块）。
  怎么判：`qualityReport().depth` 与 `leaf_count`。怎么防：`kBvhMaxDepth = 64` 远大于正常树的深度，只在极端输入时才触发。
- **P6**：`update` 之后不 `refit` 就查询。怎么错：叶子盒还是旧的，漏掉移动后的图元。怎么判：`isBuilt()` 只反映“建过”，
  不反映“脏”。怎么防：`update` 后必须 `refit()`，或者重建。
- **P7**：浮点 NaN 盒子。怎么错：`merged` 传播 NaN，整条祖先链失效。怎么判：`VolumeTraits::isFinite`。怎么防：登记前检查，
  或接受“这棵树的查询会漏掉该图元”。
- **P8**：把 `BvhNodeTopology::offset` 当成“两个子下标”存或读。怎么错：内部节点的 `offset` 是**右孩子**下标（左孩子恒为
  `parent + 1`），按线性 BVH 的两下标写文件，恢复后的遍历会跳到父亲的兄弟。怎么判：`validate()` 的包含性检查失败，
  或恢复直接返回 0（右孩子必须严格在左孩子之后）。怎么防：只以一个字段的语义（2.4）编解码，文件里也只出现这一个口径。
- **P9**：忘了 `primOrder()`。怎么错：叶子区间指向顺序数组，而顺序数组成了恒等排列——每个叶子的图元集合全错，
  查询结果**错但看起来正常**。怎么判：`validate()` 的排列检查。怎么防：拓扑、顺序与高度是同一个值（`BvhTopology`），
  编解码与恢复都只碰这一个值，没有“只带了一半”的调用形态。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | 把 BVH 放 `graphics` 或 `brep` | 两侧都要用；放任一侧都是回边（`G2`）。放在 `geometry` 与 `Aabb` / `Ray` 同层（`G1`）。 |
| D2 | `Obb` 作为第二个 `Volume` 一起交付 | 实测否决：两两合并没有精确解，PCA 拟合十六个角点在细长零件场景下叶子表面积约为 `Aabb` 树的 **2 倍**，且拟合出的盒子容不下刚拟合的角点（差一个舍入步），包含性要加容差。见 `ADR-2026-11-07-bvh-as-structure-plus-builder.md`。 |
| D3 | 支持动态插入 / 删除 | 消费者（拾取、LiDAR）都是“模型改完重建”，动态平衡树的复杂度没有对应收益（`G8`）。 |
| D4 | 把构建算法写进 `Bvh`（`build(kSah)` 一个枚举切完） | 加一种算法就改核心类型；外来算法无法注入；树的编译单元被迫带上全部构建内核（`G9`、`G5`）。 |
| D5 | 构建器继承 `BvhBuilder` 做多态 | 构建是偶发的一次性动作，虚函数与堆分配换来的是“运行时可换算法”这种没人需要的灵活性（`G8`）。 |
| D6 | `buildWith` 用 `std::function<void(BuildJob&)>` | 每次构建一次类型擦除；模板参数零成本，且构建本来就在头文件里（`G10`）。 |
| D7 | 预分配 `2·⌈n/leaf⌉+1` 的节点窗口 | 叶子数取决于切分结果（`5` 个图元会切成 `2+3` 两个叶子），这个上界会越界；改由 `appendNode()` 自然增长 + 保留容量。 |
| D8 | 内部节点存两个子下标 | 多 4 字节/节点；`parent + 1` 规则同样清晰，代价只是一处下标写法（`G10`）。 |
| D9 | 查询返回 `std::vector<PrimId>` | 最近命中这类单结果查询也要分配；回调 + 可提前终止覆盖全部用法（`G7`）。 |
| D10 | 构建用显式堆栈 | `max_depth` 已给出递归深度上界，显式栈只是把一个上界换成一次堆分配（`G10`）。 |
| D11 | 叶子存图元副本 | 图元（网格、三角形）可能很大；树只存包围体与 id，几何留在原处（`G13`）。 |
| D12 | 把 BVH 放进 `math` | 有拓扑、有构建流程的结构不是数值词汇（`geometry-primitives.md` 2.1、`G1`）。 |
| D13 | 存储/恢复包围盒 | 盒是位置的函数，存了就有“文件里的盒与文件里的顶点各自演化”的不一致（`28 B/三角形`，还要处理字节序与容差）；存拓扑只有约 `8 B/三角形`，恢复时按当前图元重算（2.13，`G10`、`G13`）。 |
| D14 | 给 `Bvh` 加 `save()` / `load()` | 文件格式归 `robotics/io`（`G2`）；而 `load()` 会把字节读成树，绕过恢复路径上唯一的那份检查。恢复只有两个入口：视图走 `RestoreTopology`（构建算法），拥有值走 `restoreTopology()`（移动），两者读同一份 `installableHeight()`。 |
| D15 | 给缓存算内容指纹 | 陈旧缓存只会让树变差（2.13），指纹只能提升质量、不能提升正确性，却要在保存与加载各扫一遍全量数据（`G8`）。 |
| D16 | 一个 `TriangleBvh` 类包住 `Bvh` 与三角形源 | 会挡住已有的共享：`Mesh::sharedBvh()`、`Geometry::adoptBvh()` 与拾取都按 `Bvh<double>` 共享（一个引用计数）。缺的从来不是“又一个树类型”，而是“三角形 → 包围体”的登记规则；规则做成自由函数，树仍是 `Bvh`（`G8`、`G7`）。 |
| D17 | 把登记规则做成 `BvhBuildAlgorithm`（交给 `buildWith`） | 算法接口面向 `BuildJob` 的内部契约，而登记是“数据第一步”不是“决定形状”；`RestoreTopology` 又要求“图元已登记”这个前提，两者不在同一层，混在一起会让两者互相猜测（`G1`、`G4`）。 |
| D18 | `BvhTopology` 作为 `Bvh` 的内部类 | 形状与精度无关，而 `Bvh<float>` / `Bvh<double>` 都显式实例化，嵌套会给同一个布局造两个互不可赋值的类型，`.vbtp` 的编解码器还得为一个 POD 拉进整棵树的头；而且层序是反的——`Bvh.hpp` 依赖形状头，不是相反（`G2`、`G10`）。形状是**可以没有树而存在**的值。 |
| D19 | 形状是树的**转录**（前一版：`detail::BvhNode` + `BvhNodeTopology` 两份） | 同一约定两份结构体，每次 `topology()` 都付整份拷贝（38M 三角形≈`1 GB`），恢复再逐个 `appendNode()` 拄两遍。改为“形状是树的存储”后，视图零拷贝、恢复是移动；代价只是遍历每节点多一次载入，而 `refit()` 只扫体积那条反而更集中（`G1`、`G10`）。 |

## 8. 测试与验收

| # | 判据 | 用例 |
|---|---|---|
| T1 | 逐个 `add` 与 `addRange` 结果一致 | `tests/test_geom_spatial/BvhBuildTest.cpp` |
| T2 | `update` + `refit` 后 `validate` 通过且查询与重建一致 | `BvhBuildTest.cpp` |
| T3 | 两档质量 × 三种叶子规模 × 深度截断 × 随机 1000 盒 | `BvhBuildTest.cpp` |
| T4 | 重叠查询覆盖暴力全扫结果（超集语义） | `BvhQueryTest.cpp` |
| T5 | 射线查询：命中、未命中、`max_t` 剪枝、最近命中只回调一次 | `BvhQueryTest.cpp` |
| T6 | 构建后 `validate()` 为真；破坏 I1 能被检出 | `BvhBuilderContractTest.cpp` |
| T7 | 重建后 `capacity` 不增长 | `BvhBuilderContractTest.cpp` |
| T8 | 空树查询不崩、未构建的树查询返回空 | `BvhBuilderContractTest.cpp` |
| T9 | SAH 树的重叠查询节点访问数不高于中位数树（随机场景） | `BvhBuildTest.cpp`（节点访问计数经 `qualityReport` 的叶子表面积间接断言） |
| T10 | 拓扑 → 恢复 → 查询逐条相同；不自洽的拓扑被拒；两条恢复路径同答 | `BvhBuildTest.ARestoredTopologyAnswersTheSameQueries`、`ARestoredTopologyThatDoesNotDescribeTheTreeIsRefused`、`AnOwnedTopologyIsAdoptedByTheTreeItself`、`ATopologyViewBorrowsTheTreesOwnArrays`、`TriangleBvhTest.AMovedShapeIsAdoptedInsteadOfCopied` |
| T11 | primitive id == 三角形序号；一条调用建树；坏角点与 NaN 被拒；恢复与重建同答；外来形状被拒；非空树不被重编号 | `tests/test_geom_spatial/TriangleBvhTest.cpp` |

现有 28 个 BVH 用例与既有 geometry 用例同批运行：`build/bin/test_geom_spatial`（256 个全绿，2026-11-09）。

## 9. 状态与演进

### 9.1 当前状态

已落地：`spatial/Bvh.hpp`、`spatial/BvhBuilder.hpp`、`spatial/BvhTopology.hpp`、`spatial/TriangleBvh.hpp`、
`spatial/detail/{RaySlab,VolumeTraits,BvhBuildJob,BvhBuilders,BvhRefit,BvhTopologyCheck}.hpp`；
`Aabb::intersectWith()` 与盒实体的射线查询直接调用 `preparedRayBoxInterval()`（`geometry-ray.md` 的唯一 slab 实现）；
`src/base/geom/src/Instantiations.cpp` 导出 `Bvh<float>` / `Bvh<double>`。
拓扑存储与恢复（F8）已落地；消费它的载入侧见 `robotics-io-design.md`（`.vbtp`）。

### 9.2 已知缺口

- `Obb` 体积（`D2`）：需要一个“保留两个孩子朝向”的合并，而不是每次合并拟合一个新盒子；在此之前不提供。
- 并行构建、SIMD 遍历、射线包（packet）遍历：没有消费者（`G8`）。
- 消费者：`Mesh::buildBvh()` / `Mesh::restoreBvh()`（`graphics-geometry.md` 2.9 / F32）、
  `Geometry::prepareBvh()`（F33）、`robotics/io` 的 `.vbtp` 缓存；两者都经 `buildTriangleBvh()` /
  `restoreTriangleBvh()`，因此编号规则只有一份（F9）。
- 缺少“访问节点数”的直接计数器；目前用 `qualityReport()` 的叶子表面积间接比较质量（T9）。
- **遍历的两条数组布局还没有 bench 基线**：仓库里没有 BVH 基准目标，所以“每节点多一次载入”的代价尚未用
  `scripts/callgrind_diff.py` 与 AoS 版本对比过。在建立基准前，`topologyView()` / `restoreTopology()` 的
  零拷贝是**结构可证**的（指针相等的用例钉着），而遍历吞吐只有定性判断。

### 9.3 兼容性与迁移

`Bvh<T>` 的布局变了（节点组合学与节点体积从一条 AoS 数组拆成两条），它是显式实例化的 DLL 类，所以这是一次
**ABI 破坏**：消费者需要重新编译，源码级接口保持（`add` / 查询 / `refit` / `validate` / `primOrder` 不变）。
`topology()` 保留（返回拥有的值）；新增 `topologyView()`、`restoreTopology()`、`Mesh::restoreBvh(BvhTopology)`
与 `restoreTriangleBvh(..., BvhTopology)`。`RestoreTopology` 从 `BvhTopology.hpp` 搬到 `BvhBuilder.hpp`——
它本来就是以“交给 `buildWith`”的方式使用的，包含 `BvhBuilder.hpp` 的调用方不需要改。

## 10. 参考

- 代码：`src/base/geom/sdk/vine/geom/spatial/Bvh.hpp`、`BvhBuilder.hpp`、`BvhTopology.hpp`、`TriangleBvh.hpp`、
  `src/base/geom/sdk/vine/geom/spatial/detail/VolumeTraits.hpp`、`BvhBuildJob.hpp`、`BvhBuilders.hpp`、`RaySlab.hpp`。
- 相关设计：`geometry-ray.md`（射线与 slab 实现）、`geometry-primitives.md`（值类型归属）。
- 决策：`../decisions/ADR-2026-11-07-bvh-as-structure-plus-builder.md`。
- 外部：Wald, *On fast Construction of SAH-based Bounding Volume Hierarchies*（binned SAH）；Aila & Laine,
  *Understanding the Efficiency of Ray Traversal on GPUs*（`parent + 1` 左孩子布局）。
