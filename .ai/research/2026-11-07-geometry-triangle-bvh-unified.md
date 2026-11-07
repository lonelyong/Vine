# 三角形树的两帧统一 —— 网格与手搭几何都持有 BVH

> **状态**：提案（非权威；落地后并入 `../design/graphics-geometry.md` 与 `../design/geometry-bvh.md`，本文件删除）
> **日期**：2026-11-07
> **范围**：一棵三角形树到底属于谁、以什么身份被多方持有、按手搭几何与网格几何如何统一、拾取如何用它。
> **取代表明**：本文件取代 `2026-11-07-bvh-persistence-and-picking.md` 的 2.5 / 2.6 / F6（那里靠
> `Geometry::sourceShape()` 反向指针 + 四条同构条件在拾取时“借”树；本文件把树变成几何自己按需派生的缓存，
> 采纳只发生在 `geometryFromShape()` 一处）。
> **前置**：`spatial/Bvh.hpp`、`spatial/BvhBuilder.hpp`、`../design/geometry-bvh.md`、
> `../design/graphics-geometry.md`（2.6 缓存键、F10 `isTriangleTopology()`、F32）、
> `ADR-2026-11-07-mesh-triangle-tree-cache.md`

## 1. 概要设计

### 1.1 问题与背景

树已经建在 `models::Mesh` 上（`Mesh::bvh()`）。但拾取路径读的不是网格：

```
Node → Geometry（通道切片） + Primitive（索引段 + 拓扑） → traverseMesh(positions, indices, topology)
```

`models::Mesh` 根本不在这一行里。于是“让拾取用上树”只剩下两个选择：把网格反向指针塞进 `Geometry`，
或者让几何自己拥有树。前者让渲染类型记住模型身份、并且只对手搭几何以外的情形有效；后者要回答一个新问题：

**手搭的几何（`setPositions()` + `addPrimitive()`，没有 `Mesh` 存在）凭什么没有树？**

答案是“凭它今天还没算”。手搭几何和网格几何的三角形流是同一类东西，网格只是**这条流恰好有一个提前算好树的拥有者**。
所以真正要理顺的不是“拾取怎么拿到网格的树”，而是：

- 一棵树索引的是**一条三角形流**；流有两帧（网格帧、绘制帧），两帧的差异必须被说清楚，相同处必须只有一份实现；
- 同一条流可以被多个持有者共享（网格把树交给几何，不复制）；
- 没有树的时候拾取仍然是今天的样子（线性扫描），所以“有树”永远是纯增益。

### 1.2 目标

- 一个树类型、一个构建算法、一套查询、一把钥匙：网格与几何的树在概念上是同一个东西。
- 手搭几何与网格几何都支持树，且**接口相同、用法相同**（`prepareBvh()` 与 `bvh()`）。
- 网格源几何**共享**网格的树，不复制、不重建；从文件恢复的树一路用到拾取。
- 树缺席时结果不变：有树与无树的命中集合逐条相同。
- 三角形序号只有一处定义：拾取报出的 `triangle_index` 与树里的 primitive id 不可能漂移。

### 1.3 非目标

- 不做“几何角色”声明式字段（见 2.6：那个概念已经叫 `Topology`）。
- 不做几何级的单棵大树（一个几何多个 draw 时，每个 draw 是一条流，一棵树）。
- 不做异构加速器（统一到 Bvh，不引入 kd-tree / 网格哈希）。
- 不做多线程拾取（见 2.8：缓存的写入沿用 `local_bounds_` 的单线程约定）。

### 1.4 一句话结论

**树属于“一条三角形流”：网格帧的流是整张网格，绘制帧的流是一个 primitive 的切片 + 拓扑；
两帧共用同一个 `Bvh<double>`、同一套构建与查询、同一把“流钥匙”；几何按需为自己的每个 draw 派生一棵树，
网格源几何直接共享网格那棵，缺席即退回线性扫描。**

## 2. 设计要点

### 2.1 两帧：什么相同、什么不同

| | 网格帧 `models::Mesh` | 绘制帧 `graphics::Geometry` + `Primitive` |
|---|---|---|
| 一条流是 | 网格的全部分 → 三角形 | 一个 primitive 的索引段 + 几何的位置段 + 拓扑 |
| 序号含义 | 网格三角形序号（跨 `toTriangleMesh()` 稳定） | 这次 draw 的第 n 个三角形（含 strip / fan） |
| 谁持有 | `Mesh::bvh_` | 几何里每个 draw 一个缓存槽 |
| 何时失效 | setter 改变三角形时丢弃 | 钥匙不符即失效（不自证过期） |
| 持久化 | 是（`.vbtp`，见持久化提案） | 否（可再从网格共享） |
| **相同** | 同一个 `Bvh<double>` 类型、同一个 `BvhBuilder`、同一套 `forEachRay` / `forEachOverlap`、同一把钥匙纪律 | 同左 |

**不同的只有两件事：所有者，和序号的含义。** 这两件不同恰恰是因为它们真的是两条流；其余全部复用。
判据：删掉 `graphics` 的树缓存，网格帧的树仍然自洽；反之亦然——它们是同一概念的两个实例，不是两份实现。

### 2.2 一条流的定义只有一处

今天绘制帧的三角形序号规则写在 `traverseMesh()` 的 `switch (topology)` 里（`Triangles` 三项一面、`Strip` 每项一个、
`Fan` 以第 0 个角为扇心），而序号又是拾取结果的一部分（`makeTriangleHit(..., ordinal)`）。若为建树再写一份枚举，
树里的 id 与拾取报出的 `triangle_index` 就有两个来源，迟早错位。所以先抽出一个只读视图：

```cpp
// graphics 内部（src/TriangleStream.hpp）
struct TriangleStream {
    std::span<const vn::math::Vec3f> positions;  // 几何的位置段
    std::span<const std::uint32_t>   indices;    // 空 = 顺序顶点
    Topology                         topology;

    std::size_t triangleCount() const noexcept;
    bool triangleCorners(std::size_t triangle, std::uint32_t (&corners)[3]) const noexcept;
};
```

**线性扫描、建树、剪枝后的精确交，三者都只经这一对函数取三角形。** 这同时把 `meshOfGeometry()` 今天
“把positions 复制进一个 `Vec3fArray`”的动作去掉（F6）：树必须与被画的字节是同一批字节，顺带省掉每次拾取的一次拷贝。

### 2.3 一把钥匙：`TriangleIndexKey`

`Geometry` 已经有一个按数据键缓存派生值的先例（`LocalBoundsCache`：buffer 指针 + 该 buffer 的 revision +
段 + 几何 revision）。树的缓存用同一套字段，只多出“索引段 + 拓扑”这一半：

```cpp
struct TriangleIndexKey {          // 与 LocalBoundsCache 同源，不另立一套
    const void*   positions;           // 位置 buffer 的数据指针
    std::uint64_t positions_revision;  // 它的 revision（原地写字节的写者自己推进）
    std::size_t   first_vertex;        // 位置段
    std::size_t   vertex_count;        // 已解析（0 表示“到末尾”已展开）
    const void*   indices;             // 索引 buffer 的数据指针；顺序顶点时为空
    std::uint64_t indices_revision;
    std::size_t   first_index;
    std::size_t   index_count;
    Topology      topology;
    std::uint64_t geometry_revision;   // 描述变化（通道被改指）
    constexpr bool operator==(const TriangleIndexKey&) const noexcept = default;
};
```

缓存槽 `{ TriangleIndexKey key; std::shared_ptr<const Bvh<double>> tree; }`，有效条件是 `tree && key == 当前钥匙`。
钥匙不成立就当作没有树——**这条规则让缓存可以随便错、随便旧，代价只有“重建一次”，不会有错答案。**
`geometry_revision` 保留的理由与 `LocalBoundsCache` 相同：通道可能被重新指向另一块地址复用的 buffer。

### 2.4 所有权：建成即不可变，共享而不复制

树一旦建成就只被读（`forEachRay` / `forEachOverlap` 都是 `const` 且零分配），所以用
`std::shared_ptr<const Bvh<double>>` 持有：

- `Mesh::bvh_` 由值改成这个指针；`buildBvh()` 建好一棵、再发布出去，此后不可变；
- `geometryFromMesh()` 把网格那棵**交给**几何的 draw 槽（共享所有权，一次引用计数，不复制、不重建）；
- 从 `.vbtp` 恢复的树先落到网格上，再沿同一条路被几何共享——一份数据，两个持有者。

代价：网格侧重建立即换成新对象，不再复用上一棵树的容量。可以接受：能触发重建的 setter（`setPositions()` /
`setIndices()`）本来就已经把树丢了，容量复用只对“同一批三角形反复重建”有效，而那不是正常路径。
拿它换“发布后不可变、可安全共享”，是本设计最划算的一笔交易。

### 2.5 采纳只发生在一处

`geometryFromMesh()` 是全仓库唯一把网格变成几何的函数，而且它只做四件事：整段共享位置 buffer、整段共享索引 buffer
（或顺序顶点）、`topology = Triangles`、一个 primitive。所以那里的“这条 draw 流 == 这条网格流”是**构造出来的**，
不是判断出来的。采纳就写在这一处：树随 key 一起进槽。

反过来，`QuadMesh` / `MixedMesh` 在 `geometryFromShape()` 里先 `toTriangleMesh()`（新 buffer），所以**不采纳**：
它们的新流会在第一次需要时按需建树。这是一次明确的小损失（它们本来可以复用，因为扇序相同），换来的是
“采纳只在一个可证明的地方发生”。

### 2.6 为什么不是 `Role` 枚举

“给几何加 role”这个直觉是对的——几何确实该自己拥有树——但 `Role` 字段是错的形状：

- **角色这个概念已经存在，它叫 `Topology`。** “这个 draw 是不是可拾取的表面”只有唯一判据 `isTriangleTopology()`
  （`graphics-geometry.md` F10 早就这么规定）。再加一个 `GeometryRole::Surface` 就是同一件事的第二个定义（`G1`）。
- **role 是声明的，缓存是可观察的。** `role == Pick` 但树没建，是一致性错误，而且看不出来；`prepareBvh()` 的返回值
  与 `bvh(draw)` 的 `nullptr` 不会说谎（`G11`）。
- **用途会变。** 同一个几何在“选择模式打开”之后才需要树。声明式字段要求每个消费者跟着改它；按需派生什么都不用改。
- **role 会分叉。** 一个几何有多个 draw（网格 + 边线 + 点集），role 只能长在几何上，而树只能长在 draw 上。

所以“几何也持有 bvh”落成：**每个 draw 一个按需派生、带钥匙的缓存**——与 `localBounds()` 完全同一个模式
（`mutable` 槽，`const` 读中首次计算，单线程约定）。记忆一句话：**几何不声明用途，它记住派生过的视图。**

### 2.7 缺席即退回线性扫描

没有树（没 prepare、钥匙不符、不是三角形拓扑、空流）时，拾取走今天的分支：整盒粗筛 + 线性扫三角形。
所以这个特性是**单调**的：结果集合逐条相同，只有代价不同。第一次悬停可能付一次建树，之后就只是查询。

### 2.8 并发与 const

缓存槽是 `mutable` 的，写入发生在 `const` 的拾取调用里——这正是 `local_bounds_` 今天的做法，依据是同一条约定：
几何的派生缓存在集合/查询线程上被单线程访问，引擎线程不碰它们。多线程拾取若真的出现，规则是：
先用 `prepareBvh()` 在单线程里建好，之后读路径只读（“建”与“读”分离）。这条写进陷阱。

### 2.9 准备时机与阈值

- **懒建**是兜底：第一次拾取把树建出来，正确但可能是一次卡顿（SAH 建树是毫秒到几十毫秒量级）。
- **显式准备**是界面该做的：文档加载完、或第一次交互之前，按 `triangleCount()` 阈值（比如 ≥ 512）对每个三角形 draw
  调一次 `prepareBvh()`。阈值是消费者策略，树不知道什么叫“值得”。
- **不准备也不建**：小网格线性扫更快，硬建树是负收益。

## 3. 功能点

| # | 功能 | 章节 |
|---|---|---|
| F1 | 网格侧：共享不可变的树 | 3.1 |
| F2 | 绘制侧：`TriangleStream`（一条流的唯一定义） | 3.2 |
| F3 | 绘制侧：每 draw 的钥匙化树缓存 | 3.3 |
| F4 | 绘制侧：拾取的树路径 | 3.4 |
| F5 | 采纳：网格源几何共享网格的树 | 3.5 |
| F6 | 拾取不再复制 positions | 3.6 |
| F7 | 缺席退回线性扫描 | 3.7 |

### 3.1 F1 网格侧：共享不可变的树

- **行为**：`Mesh::bvh_` 改为 `std::shared_ptr<const Bvh>`；新增 `std::shared_ptr<const Bvh> sharedBvh() const`
  （未建时为空指针）。`bvh()` 仍返回 `const Bvh&`（未建时返回一个空树）。
- **约束**（C1）：发布后的树不可变；`buildBvh()` / `restoreBvh()` 建新对象而不是改旧对象。
- **验收**（T1）：既有 6 个 `MeshBvhTest` 用例不变地通过；新增"共享的树与网格是同一个对象"一项。

### 3.2 F2 绘制侧：`TriangleStream`

- **行为**：`TriangleStream` 提供 `triangleCount()` / `triangleCorners()`，把今天 `traverseMesh()` 里的
  `switch (topology)` 收进一处；线性扫描与建树都用它。
- **约束**（C2）：`triangleCorners()` 返回的是**顶点索引**（顺序顶点时就是角号），与 `models::Mesh` 同一约定。
- **验收**（T2）：`RayIntersectionTest` 现有 strip / fan / list 用例不变地通过（序号未变）。

### 3.3 F3 绘制侧：每 draw 的钥匙化树缓存

- **行为**：`Geometry::prepareBvh(draw = 0)` 建或重建该 draw 的树并返回是否可用；`Geometry::bvh(draw = 0)` 返回
  `const Bvh*`（未准备或钥匙不符时 `nullptr`）；`Geometry::dropBvh()` 丢掉所有槽。
- **约束**（C3）：槽随 `primitives_` 同长，`addPrimitive` / `setPrimitive` / `clearPrimitives` 保持同步；
  即便失同步，钥匙不符也只会导致重建（C4）。
- **约束**（C9）：**只有三角形拓扑的 draw 会有树**（`isTriangleTopology()`）；线、点、带条之类的 draw
  `prepareBvh()` 直接返回 false，不占一棵树。
- **约束**（C10）：**钥匙完全相同的两个槽共享同一棵树**（建之前先在本几何已有的槽里找相同钥匙，找到就
  递增引用计数而不重建）：同一个索引段被两个材质拆成两条 draw 时不该有两棵一模一样的树。
- **验收**（T3）：手搭几何（`setPositions` + 自定义索引段 + `Triangles`）能建树并用于拾取；
  同一个段的两条 draw 得到同一个指针；`Lines` / `Points` 的 draw 拿不到树。

### 3.4 F4 绘制侧：拾取的树路径

- **行为**：有树时 `forEachRay(local_ray, visit, max_t)` 给候选，精确交仍是同一个 `triangleCorners` +
  `intersectTriangle`；最近命中把 `max_t` 写回以剪枝。无树时走线性循环。
- **约束**（C5）：两条路径的命中集合与 `triangle_index` 逐条相同。
- **验收**（T4）：`RayIntersectionTest` 增加"有树 / 无树结果相同"，并见证三角形测试数显著下降。

### 3.5 F5 采纳：网格源几何共享网格的树

- **行为**：`geometryFromMesh()` 在设置 primitive 时，把 `mesh.sharedBvh()` 与由该几何算出的钥匙一起放进槽 0。
  网格源几何只有一个 draw，所以落地后**不需要任何 prepare 调用**：`bvh(0)` 一上来就是网格那棵。
- **约束**（C6）：只在这一个函数里采纳；`QuadMesh` / `MixedMesh` 的三角化结果不采纳。
- **验收**（T5）：`geometryFromShape()` 产出的几何在网格已建树时 `bvh(0) != nullptr`，且指针等于网格那棵。

### 3.6 F6 拾取不再复制 positions

- **行为**：`meshOfGeometry()` 的 `Vec3fArray` 拷贝被 `TriangleStream` 的只读视图取代。
- **约束**（C7）：视图只在一次拾取内有效（缓冲可能被写者替换）。
- **验收**（T6）：拾取结果不变；`RayIntersectionTest` 的粗筛仍能拒绝整盒不相交的几何。

### 3.7 F7 缺席退回线性扫描

- **行为**：`bvh(draw) == nullptr` 时走既有线性循环。
- **约束**（C8）：树的有无不改变语义。
- **验收**（T7）：关闭 prepare 的整轮 `RayIntersectionTest` 全绿。

## 4. 公开接口

| 侧 | 新增 / 变更 |
|---|---|
| `models` | `Mesh::sharedBvh()`（新）；`bvh_` 内部改为 `std::shared_ptr<const Bvh>`（`bvh()` / `hasBvh()` / `buildBvh()` / `setBvh()` / `clearBvh()` 签名不变） |
| `graphics` | `Geometry::prepareBvh(std::size_t draw = 0)`、`Geometry::bvh(std::size_t draw = 0)`、`Geometry::dropBvh()`；内部 `TriangleStream`、`TriangleIndexKey` |
| `geometry` | 无新增（`Bvh<double>` 原样使用；持久化所需的 `BvhTopology` / `RestoreTopology` 见持久化提案） |
| `RayIntersection` | 内部：按 draw 取树，两条路径共用 `triangleCorners()` |

## 5. 约束、要求与不变量

- **C1**：发布后的树不可变，重建发布新对象。
- **C2**：`triangleCorners()` 返回顶点索引，序号规则与线性扫描同一实现。
- **C3**：缓存槽与 `primitives_` 同长，由 `Geometry` 的增删改方法维护。
- **C4**：钥匙不符 = 没有树（重建，不错答案）。
- **C5**：有树 / 无树命中结果逐条相同。
- **C6**：采纳只发生在 `geometryFromMesh()`。
- **C7**：`TriangleStream` 的视图只在一次调用内有效。
- **C8**：树缺席不改变任何语义。
- **R1**：拾取的读路径零分配（除首次建树）。
- **R2**：`geometry` 不认识 `graphics::Topology`；`graphics` 不反向持有 `Shape`。
- **R3**：树按 draw 各一份；但**钥匙相同就是同一条流**，共用同一个不可变对象（C10）。
- **I1**：树只描述它钥匙所描述的那条流；这一条比“树是最新的”更强，也让陈旧缓存安全。
- **I2**：`triangle_index` / primitive id 是同一个数，来源只有 `TriangleStream` 一处。

## 6. 注意事项与陷阱

- **P1**：`TriangleStream` 与 `traverseMesh` 的 `switch` 各写一份序号规则。怎么错：树里的 id 与报出的
  `triangle_index` 错位一格，拾取命中邻接三角形——**看起来能选中，只是偏了**。怎么判：有树/无树结果比对（T4）。
  怎么防：`switch` 只存在 `TriangleStream` 里。
- **P2**：strip / fan 的 draw 直接套用网格的三角形树。怎么错：两种序号不是同一个编号，命中错位。
  怎么防：树按 draw 建（每 draw 一条流），不存在“借用别的流的树”。
- **P3**：缓存槽与 `primitives_` 失同步（插入/删除后没跟着挪）。怎么错：把一棵树当另一个 draw 的用。
  怎么防：钥匙里的索引段 + 拓扑不同 → 直接重建（C4），而不是只靠“记得同步”。
- **P4**：忘了 `geometry_revision`。怎么错：通道被改指到另一块“地址被复用”的 buffer，指针 + revision 都撞上。
  怎么防：钥匙与 `LocalBoundsCache` 同源（2.3）。
- **P5**：在拾取里第一次建树导致的卡顿被当成“拾取变慢”。怎么判：第一次悬停慢、之后快。
  怎么防：界面在交互前显式 `prepareBvh()`（2.9）。
- **P6**：多线程拾取 + 懒建。怎么错：两个线程同时往同一个槽里写 `shared_ptr`。怎么防：建/读分离（2.8）。
- **P7**：`geometryFromMesh()` 的采纳条件放宽到“三角化的 Quad/Mixed”。怎么错：新流来自新 buffer，
  树的顶点坐标只是“值相等”，一旦转换器将来做法线重算或去重，盒子就不再包住三角形，且**不会报错**。
  怎么防：只在一个可证明的地方采纳（2.5）。

## 7. 已否决选项

| # | 否决 | 理由 |
|---|---|---|
| D1 | `Geometry` 反向持有 `Shape`（`sourceShape()`），拾取时借网格的树 | 需要四条“编号同构”条件在拾取时反复判定，且手搭几何永远没有树；改成几何自己按 draw 派生（2.3、2.5）。 |
| D2 | 给 `Geometry` 加 `Role` / `GeometryRole` 枚举 | 与 `Topology` + `isTriangleTopology()` 重复定义（`G1`）；声明与派生不一致不可观察（`G11`）；用途会在运行时变化。见 2.6。 |
| D3 | 一个几何一棵树 | draw 才是流，索引段与拓扑都是 per-draw 的；合并不可能正确（`G1`）。 |
| D4 | 把网格的树**复制**进几何 | 同一批三角形两棵同样大的树；共享不可变对象没有代价（`G10`）。 |
| D5 | 让树可变、几何持有网格的引用 | 重建会改动持有者看到的对象，共享即刻失效；改成“发布新对象”（C1）。 |
| D6 | 只在拾取时建树、不做显式准备 | 交互线程上的一次 SAH 建树就是一次卡顿；准备时机归消费者（2.9）。 |
| D7 | 手搭几何不支持树（只给网格源几何） | 手搭几何正是“手动装配的大网格”的场景（导出的边线、程序化点面），恰是最需要加速的一类（`G1`、`G8`）。 |
| D8 | 用 `std::map` / 哈希表按 draw 存缓存槽 | 槽与 `primitives_` 同序即可，多一层查找没有收益；失同步由钥匙兜底（3.3）。 |
| D9 | 引入 `PickIndex` / `Accelerator` 抽象基类 | 只有一种加速器；抽象基类会立刻带来虚调用与第二套生命周期（`G8`、`G10`）。 |

## 8. 测试与验收

| # | 判据 | 用例 |
|---|---|---|
| T1 | 网格共享树与网格是同一对象；既有六项不变 | `MeshBvhTest` |
| T2 | strip / fan / list 的序号规则不变 | `RayIntersectionTest`（既有用例） |
| T3 | 手搭几何可建树并用于拾取 | `tests/test_graphics`（新用例） |
| T4 | 有树 / 无树命中集合与 `triangle_index` 逐条相同；三角形测试数显著下降 | `RayIntersectionTest`（新用例） |
| T5 | 网格源几何的 `bvh(0)` 与网格同一指针 | `tests/test_graphics`（新用例） |
| T6 | 拾取不再分配 positions 副本，结果不变 | 同上（配 `localBounds` 风格的计数见证或直接结果断言） |
| T7 | 不准备树的整轮拾取用例全绿 | `RayIntersectionTest` |

## 9. 实施顺序

1. **网格**：`bvh_` 改共享指针 + `sharedBvh()`；T1（`MeshBvhTest` 不改断言）。
   **2026-11-07 已落地**：`Mesh::bvh_` 为 `std::shared_ptr<const Bvh>`；`buildBvh()` 建局部对象、完成才发布，
   `clearBvh()` / 各 setter 只 `reset()`（不动已发出的对象），`bvh()` 在未发布时返回空树；`MeshBvhTest` 增至 7 项。
2. **绘制侧序号**：`TriangleStream` 抽出，`traverseMesh` 改用它；T2（纯重构，行为不变）。
   **2026-11-07 已落地**：`src/TriangleStream.hpp`（`cornerCount` / `triangleCount` / `triangleCorners`，条带奇偶翻转
   直接体现在返回的绕序里）；`traverseMesh(stream, ray, world, on_hit)` 只剩一个按序号走的循环，
   `bound_positions` 改收 `span`；顺手补了 `traverseMesh` 的前置声明（原先靠 MSVC 宽松的两阶段名字查找才编过）；
   `RayIntersectionTest.AStripAndAFanAreSurfacesToo` 增加 `triangleIndex` 断言（条带两个三角形分别是 1 与 0），
   把“第 n 个三角形”钉在一处。
3. **绘制侧缓存**：`TriangleIndexKey` + `prepareBvh` / `bvh` / `dropBvh` + 拾取树路径；T3 / T4 / T7。
4. **采纳**：`geometryFromMesh()` 共享网格的树 + 去掉 positions 拷贝；T5 / T6。
5. 之后接持久化（`.vbtp`，见 `2026-11-07-bvh-persistence-and-picking.md`），加载即得树，几何零构建。

已知缺口：`QuadMesh` / `MixedMesh` 的三角化结果不复用源树的拓扑；多线程拾取；draw-frame 与 mesh-frame
序号不同源时的跨帧复用（不打算做）。

## 10. 参考

- 代码：`spatial/Bvh.hpp`、`models/Mesh.hpp`、`viz/graphics/sdk/vine/graphics/Geometry.hpp`、`Primitive.hpp`、
  `viz/graphics/src/Geometry.cpp`（`geometryFromMesh`）、`viz/graphics/src/RayIntersection.cpp`
- 设计：`../design/geometry-bvh.md`、`../design/graphics-geometry.md`（2.6、F10、F32）、`../design/geometry-ray.md`
- 决策：`ADR-2026-11-07-mesh-triangle-tree-cache.md`、`ADR-2026-11-07-bvh-as-structure-plus-builder.md`
- 相关提案：`2026-11-07-bvh-persistence-and-picking.md`（持久化与文件格式）
