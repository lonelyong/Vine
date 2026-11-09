# ADR-2026-11-09：形状就是树的存储，不是它的转录

> **状态**：已采纳
> **日期**：2026-11-09
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial`（`Bvh.hpp`、`BvhTopology.hpp`、`BvhBuilder.hpp`、
> `TriangleBvh.hpp`、`detail/{BvhBuildJob,BvhBuilders,BvhRefit,BvhTopologyCheck}.hpp`）、
> `src/base/models/sdk/vine/models/Mesh.hpp`、`src/robotics/io/src/BvhCache.hpp`、
> `tests/test_geom_spatial`、`.ai/design/geometry-bvh.md`

## 背景

评审中问了两个问题：`BvhTopology` 为什么要独立成文件、能不能做成 `Bvh` 的内部类；以及有没有比现状
更好的结构。评估后确认独立是对的（层序、精度无关、编解码器包含成本），但现状里有一处真实的浪费：

- 树的节点是 `detail::BvhNode<T, Volume>{bounds, offset, count}`（AoS，32 B）；
- 存进文件的节点是 `BvhNodeTopology{offset, count}`（8 B）；
- 两者是**同一个约定的两份转录**，注释里只能写「两个数字的含义是 `BvhNode` 的」。

于是每一次 `topology()` 都要整份复制：38M 三角形 ≈ `≤2n` 个节点 × 8 B = 608 MB，加上 113M 个
`order` × 4 B = 452 MB，**一次约 1 GB**；`.vbtp` 写入调一次，恢复路径再把节点逐个 `appendNode()`
抄进构建任务，同一条链上三份拷贝。

这与刚修掉的焊接 OOM（`perf(meshio)!: no vertex weld`）是同一类问题：把文件里已有的字节再复制进另一条
表示。

## 决定

1. **`BvhTopology.hpp` 只留词汇**：`BvhNodeTopology`、`BvhTopology`、`BvhTopologyView`、`kBvhMaxDepth`，
   只依赖标准库。`RestoreTopology`（构建算法）搬到 `BvhBuilder.hpp`，`refitNodes` 搬到
   `detail/BvhRefit.hpp`，恢复检查搬到 `detail/BvhTopologyCheck.hpp`。
2. **形状成为树的存储**：`Bvh` 持有 `BvhTopology shape_`（`nodes` + `order` + `depth`）与平行的
   `std::vector<VolumeType> node_bounds_`。`detail::BvhNode` 删除，`BvhNodeTopology` 就是节点。
3. **两个恢复入口，一份检查**：只有视图时走 `BvhBuilder::buildWith(tree, RestoreTopology{shape})`
   （构建算法，与 SAH / 中位数同一个门）；自己拥有值时走 `Bvh::restoreTopology(BvhTopology)`
   （**移动**数组进来）。两者都读 `detail::installableHeight()`，都用 `detail::refitNodes()` 长肉。
4. **保存侧零拷贝**：`Bvh::topologyView()` 借用树自己的数组，`.vbtp` 直接编码它；`topology()` 保留为
   「要一个拥有的值」的入口。

## 理由

- **G10，先算成本**：`1 GB`/次的拷贝消失。这不是「更优雅」，是内存与时间上的实测量级。
- **G1 / G6，一个约定一处定义**：节点只有 `BvhNodeTopology` 一份，树与文件存同一个布局，`validate()`、
  遍历、编解码读的是同一组数字。
- **G4，把「不改拓扑」变成签名事实**：`refitNodes()` 只接收节点体积那条数组作为可写参数，写不到组合学；
  `C5` 从注释变成编译期保证。
- **层序不逆转**：`Bvh.hpp` 依赖形状头，形状头不认识树。把它做成内部类会让 `.vbtp` 的编解码器为了一个 POD
  拉进整棵树与构建内核，并且给 `Bvh<float>` / `Bvh<double>` 造出两个布局相同、互不可赋值的形状类型。
- **G2，形状头变成叶子**：`.vbtp` 编解码器不再传递包含构建内核。

## 否决的替代方案

| 方案 | 否决理由 |
|---|---|
| `BvhTopology` 作为 `Bvh` 的内部类 | 精度无关的值被模板化；层序反转（`Bvh.hpp` 依赖形状头）；编解码器被迫包含 `Bvh.hpp` + 构建内核（`G2`、`G10`）。 |
| 保持转录，只把 `topology()` 改成返回视图 | 无法实现：AoS 节点按 32 B 步长，凑不出连续的 `BvhNodeTopology` 数组。 |
| 让 `BvhTopologyView` 直接指向 `BvhNode<T, Volume>` | 视图重新依赖精度，文件格式与树的精度绑死，`Bvh<float>`/`Bvh<double>` 的缓存不再可互换。 |
| 给 `offset` / `count` 加 `isLeaf()` / `rightChild()` 把约定塞进类型 | `BvhNodeTopology` 特意保持 plain value（平凡可拷贝、编解码直接读写），推翻一条已写明的设计点只换来少读一行注释。 |
| 保留 `detail::BvhNode` 作为「热路径专用节点」 | 两份约定同源，等于把 `1 GB` 拷贝固化下来；遍历多阅读者一次的代价远小于它。 |

## 后果

- 遍历每节点两次载入（`shape_.nodes[i]` + `node_bounds_[i]`），`refit()` 只扫体积数组。查询路径的形态变化
  必须用 `scripts/callgrind_diff.py` 对比，不能用推理代替（第一次落地时记录在 `geometry-bvh.md` 2.4）。
- `Bvh<T>` 的布局变化：它是显式实例化的 DLL 类，**ABI 破坏**，消费者全部重编。
- `Mesh::restoreBvh` 新增 `BvhTopology` 按值重载；`restoreTriangleBvh` 同样两个重载。旧的视图入口保留。
- 头文件计数：`spatial/detail/` 5 → 7（新增 `BvhRefit.hpp`、`BvhTopologyCheck.hpp`）。
- `VolumeT` 别名统一改名为 `VolumeType`（三处：`Bvh`、`BuildJob`、`VolumeTraits<Aabb<T>>`），
  一个概念一个拼写。

## 验证

- `test_geom_spatial` 256 例全绿（新增 `BvhBuildTest.AnOwnedTopologyIsAdoptedByTheTreeItself`、
  `ATopologyViewBorrowsTheTreesOwnArrays`、`TriangleBvhTest.AMovedShapeIsAdoptedInsteadOfCopied`）。
- 全量 `ctest` 28/29（`test_vsg` 的 strip/fan 用例是本机既有失败，与本次无关）。
- `check_geom_layering.py`、`check_include_hygiene.py`、`check_export_annotations.py`、
  `check_public_docs.py`、`check_doc_symbols.py`、`check_ai_docs.py --strict`、
  `check_design_docs.py` 全部 0 finding。
