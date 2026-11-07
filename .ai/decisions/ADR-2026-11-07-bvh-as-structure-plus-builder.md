# ADR-2026-11-07：BVH 拆成「结构 + 构建器」，`Obb` 体积实测否决

> **状态**：已采纳
> **日期**：2026-11-07
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial`（新增 `Bvh.hpp`、`BvhBuilder.hpp`、`detail/`）、
> `src/base/geom/src/Instantiations.cpp`、`scripts/check_geom_layering.py`、`tests/test_geom_spatial`、
> `.ai/design/geometry-bvh.md`

## 背景

`geometry-ray.md` 的 `D5` 曾把“射线–网格与 BVH”列为不做：当时没有性能证据，且担心引入第二种数据组织。
现在消费者出现（拾取与 LiDAR 仿真的批量射线），需要一棵可复用的加速结构。设计过程中定了三件事：

1. 结构放 `geometry`（`Aabb` / `Ray` 同层），不放 `graphics` 或 `brep`——两侧都要用，放任一侧都是回边（`G2`）；
2. 结构与构建算法必须分离：加一种启发式不应修改核心类型（`G9`）；
3. 设想同时支持 `Aabb` 与 `Obb` 两种体积（`VolumeTraits` 适配）。

## 决定

1. `Bvh<T, Volume = Aabb>` 是**结构**：图元包围体、扁平节点数组、图元顺序、两种遍历、`refit`、`validate`、质量统计。
   它不知道图元是什么，也不知道树怎么建出来。
2. `BvhBuilder` 是**构建者**：`leafSize` / `maxDepth` / `quality` 链式配置 + `build()` / `buildWith()`。
   少量状态、无继承、无虚函数。
3. 两者之间只有 `detail::BuildJob<T, Volume>` 一个契约（能读盒子、能重排顺序、能追加节点）。
   `Bvh.hpp` 不得 include `BvhBuilder.hpp`，由 `check_geom_layering.py` 规则 6 强制。
4. `VolumeTraits<V>` 提供树需要的九个操作；本轮只实现 `VolumeTraits<Aabb<T>>`。
   **`Obb` 体积不交付**，理由见下。
5. `geometry-ray.md` 的 `D5` 记为本 ADR 取代：条件（出现批量射线消费者）已满足，加速结构落地。

## 理由

- 结构 / 构建分离的判据是可执行的：删掉 `BvhBuilder.hpp` 与 `BvhBuilders.hpp` 后 `Bvh.hpp` 必须仍能单独编译。
  这个判据可以写成 gate，而“职责清晰”不能（`G1`、`G2`、`G12`）。
- 构建是偶发的、单线程的、可以慢；查询是频繁的、可并发的、必须无分配。两种节奏放进一个类型会让查询路径
  被迫携带构建状态（`G5`、`G10`）。
- traits 适配而不是改 `Aabb` / `Obb`：树的需求清单写在树这一侧，已有类型不为“可能被树用到”增加成员（`G13`）。
- 节点布局 `parent + 1` 作左孩子、`offset` 存右孩子：32 字节/节点（`float` + `Aabb`），内部节点少一个下标（`G10`）。
- 查询用回调而不是返回容器：最近命中只要一个结果，分配一次数组是纯损失（`G7`）。

## `Obb` 体积为什么被否决

按“两个盒子的十六个角点拟合一个 PCA 盒子”的保守合并实现了 `VolumeTraits<Obb<T>>` 并写了测试，实测两件事：

1. **质量更差**：细长零件场景下，`Obb` 树的叶子表面积之和约为 `Aabb` 树的 **2 倍**（22977 vs 11293，同一组 200 个
   细长盒子）。原因是一个拟合出的朝向解释不了两个孩子各自的朝向，每次合并都把盒子撑大。
2. **包含性不严格成立**：拟合盒的角点会落在刚拟合出的盒子外一个舍入步，`validate()` 的 I1 因此失败，要加容差才对。

一个“更慢、更松、还要容差”的体积不值得交付。需要的是**保留两个孩子朝向**的合并（例如双朝向节点或
两盒并集的多盒子表示），那是另一次设计，不在本轮范围内（`G8`）。`VolumeTraits` 的模板参数保留，
`Obb` 的适配连同否决理由记在 `geometry-bvh.md` 的 `D2`。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 把构建算法做成 `Bvh::build(BvhBuildQuality)` 里的一个 switch | 加一种算法改核心类型；外来算法无法注入；`Bvh.hpp` 被迫带上全部构建内核（`G9`、`G5`）。 |
| 构建器多态（`IBvhBuilder` + 虚函数） | 构建是偶发一次性动作；运行时可换算法没有消费者（`G8`）。 |
| `buildWith` 收 `std::function` | 每次构建一次类型擦除；模板参数零成本（`G10`）。 |
| 预分配 `2·⌈n/leaf⌉+1` 的节点窗口 | 叶子数取决于切分结果（5 个图元会切成 2+3），该上界越界。改为 `appendNode()` 增长 + 构建后保留容量。 |
| 查询返回 `std::vector<PrimId>` | 单结果查询也要分配；回调可提前终止（`G7`）。 |
| 构建用堆上的显式栈 | `max_depth = 64` 已给出递归深度上界，显式栈只是把上界换成一次堆分配（`G10`）。 |
| 支持动态插入 / 删除 | 消费者是“模型改完重建”，动态平衡树的复杂度没有对应收益（`G8`）；`update` + `refit` 覆盖移动。 |

## 后果

- `geometry` 新增两个公开头（`spatial/Bvh.hpp`、`spatial/BvhBuilder.hpp`）与四个 `detail` 头；纯新增，无兼容性影响。
- `Instantiations.cpp` 增加 `Bvh<float>` / `Bvh<double>` 两行显式实例化。
- `check_geom_layering.py` 增加规则 6：结构不得依赖构建器。
- `Aabb::rayBoxInterval` 的实现搬进 `detail/RaySlab.hpp`，`Aabb` 改为委托——slab 只有一份实现（`geometry-ray.md` R2）。
- `geometry-ray.md` 的 `D5` 与 §9.2 缺口项改为指向 `geometry-bvh.md`。
- 现行设计记录在 `.ai/design/geometry-bvh.md`。
