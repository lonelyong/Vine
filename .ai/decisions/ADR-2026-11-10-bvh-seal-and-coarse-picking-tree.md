# ADR-2026-11-10：BVH 可封存（只留结果）、拾取树粗叶 64、后端流上传零拷贝

> **状态**：已采纳
> **日期**：2026-11-10
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial/Bvh.hpp`、`BvhBuilder.hpp`、
> `src/viz/graphics/sdk/vine/graphics/Geometry.hpp`、`src/viz/graphics/src/Geometry.cpp`、
> `src/plugins/gfx_backend_vsg/src/internal/StreamUploads.cpp`（与同名头文件）、
> `tests/test_geom_spatial/BvhQueryTest.cpp`、`tests/test_graphics/GraphicsTest.cpp`、`tests/test_vsg/StreamUploadsTest.cpp`、
> `.ai/design/geometry-bvh.md`、`.ai/design/graphics-geometry.md`、`.ai/design/vsg-backend.md`

## 背景

模型查看器打开 `车身.stl`（704 MB，二进制 STL，14,086,575 个三角形、42,259,725 个角点）时，进程占用到数个 GB。
逐段算账（`geometry-bvh.md` 2.16 的表格是这次测量留下的结论）：

| 常驻项 | 大小 | 说明 |
|---|---|---|
| 模型（位置 + 法线 + 索引） | 1.18 GB | 42.26M × (12 + 12 + 4) B；三角形汤没有共享顶点，file 的 50 B/三角形 只能这样摊 |
| 拾取树 `Bvh<double>` | 1.36 GB | 图元盒 676 MB（14.09M × 48 B）+ 节点层 632 MB + 顺序 56 MB |
| 后端顶点/索引副本 | 1.18 GB | `StreamUploads` 把模型已有的 float 又复制进 `vsg::floatArray` |

三件事在这次排查中被确认，它们决定了本 ADR：

1. **叶粒度只管节点数**。`BvhBuilder::leafSize` 只影响节点数组；`prim_bounds_` 是 `48 B × 图元数`，与粒度无关。
   把叶子从 4 调到 512，1.36 GB 的极限也只能降到 736 MB。
2. **图元盒在拾取路径上没有被使用**。`RayIntersection` 的回调把第二个参数（图元盒）丢掉；`forEachRay` 只在**叶子层**
   用 `node_bounds_` 剔除。图元盒是构建期数据（binned SAH 要它、叶盒合并要它），不是查询结果。
3. **`update()` / `refit()` 没有任何生产调用方**。刚体变换不需要它（树在局部空间，变换在查询时折算，钥匙里没有
   世界矩阵）；顶点变化走 `Mesh` 的丢树重建与 `Geometry::prepareBvh()` 的新钥匙新树。它只在
   `BvhBuildTest` 的两条用例里出现。

## 决定

1. **`Bvh` 增加封存态**：`seal()` 释放逐图元体积（`vector().swap()`），保留 `shape_` 与 `node_bounds_`；
   `isSealed()` 由事实推出（建成了且图元盒为空），不是标志位。封存后 `add` / `addRange` / `update` / `refit` /
   `reserve` / `restoreTopology` / 构建抛 `std::logic_error`；`primCount()` 改由 `shape_.order.size()` 回答；
   `validate()` 只用一个图元个数（它本来也只用个数）。
2. **绘制帧的拾取树默认叶容量 64**（`graphics::kPickTreeLeafSize`，经 `pickTreeBuilder()` 表达），并把
   `Geometry::prepareBvh(draw, builder)` 的 builder 开放给调用方；`Geometry::prepareBvh()` 建完即 `seal()`。
   `BvhBuilder` 自身的默认（4）不动——那是库的形状，不是引擎的策略。
3. **vsg 后端的上传改为别名**：`StreamUploads::acquireVertex` / `acquireIndex` 用 `VsgBufferView` + 真 vsg 数组
   （`vsg::floatArray` / `vsg::uintArray`）读模型自己的字节，只有“没有 buffer 句柄”或“span 不属于该 buffer”时
   才退回复制。顶点通道别名**段**，索引别名**整个 buffer**。

## 理由

- `G10`（关注真实成本）：图元盒是构建草稿，却被当成长期状态常驻；唯一运行期消费者又不用它。封存把“构造成本”
  与“查询成本”分开，谁要可变谁付 48 B/图元。
- `G8`（保持克制）：`update`/`refit` 保留但在封存之外——不删除一个真实存在（形变网格、形状编辑预览）的消费者
  未来会要的能力，也不让它继续向只读消费者收费。
- `G1` / `G7`：两态的差别只有“还留不留图元盒”，做成两个类型就要复制唯一的最近优先剪枝遍历（`D20`）。
- 粗叶的代价是可量化的：一次点击的精确测试数 = 访问叶数 × 叶容量。14M 三角形的拾取树在叶 64 下，一条射线穿过
  十几到几十个叶子，精确测试在千级、每测 20–50 ns，即亚毫秒；换来节点数少 16 倍、建树时间少一个数量级。
- 零拷贝的机制在本仓库已有先例（`VsgBufferView.hpp` 与 texcoord 绑定），并且它同时解决了“后端比模型活得久”
  的生命周期问题：数组持有视图、视图持有 buffer。

## 否决的选项

| 选项 | 否决理由 |
|---|---|
| 只调叶粒度 | 只砍节点层：1.36 GB → 736 MB，而图元盒（676 MB）一分不动。 |
| 封存做成独立类型 `SealedBvh` / `BvhView` | 两态只差一个存储，独立类型要复制唯一的最近优先剪枝遍历，或把它模板化；两者都比“没有标志位、状态由数据推出”更重（`D20`、`G7`）。 |
| 删除 `update` / `refit` / `qualityReport`（按 `G8`） | 会把一类真实的未来消费者（图元集合不变、盒子逐次变化）逼回“全量重建”；封存已经让它们不再互相收费，删除不带来额外收益。`F2` 里写明了它们今天没有生产调用方。 |
| 把 `BvhBuilder` 的默认叶容量改成 64 | 库只提供形状，不提供策略；已有测试与文档按“库的默认”度量质量。策略写在消费者（`kPickTreeLeafSize`）。 |
| 图元盒改 `Aabb<float>`（24 B） | 无损（盒值本来就是 float 坐标的 min/max），但它是**下一个**杠杆，与封存独立；先做封存可以把两者做成可分别度量的两步。留在 `backlog`。 |
| 不做拾取树（线性扫 14M 三角形） | 一次点击 0.2–0.5 s 级，且失去“大网格拾取”这一能力本身；封存后这棵树已经比模型小一个数量级。 |
| 懒建（第一次点击才建树） | 打开时的建树时间从“可感知的一次卡顿”换成“第一次点击的卡顿”，而内存收益已经被封存吃掉了（叶 64 的节点层只有几十 MB）。 |

## 后果

- **内存**（14.09M 三角形的拾取树）：`1.36 GB → 88 MB`（叶 64 + 封存）。后端另省 1.18 GB（顶点与索引不再复制）。
- **语义**：封存树的查询回调收到**叶盒**而非图元盒（超集，`geometry-bvh.md` `R7` / `P10`）。拾取本就不用该参数，
  所以行为不变；任何新消费者若需要逐图元精确盒，不该封存。
- **ABI**：`Bvh` 的类布局不变（没有新成员），新增的全是头文件内联函数；唯一的破坏面是封存树上六个写接口会抛
  `logic_error`——它们今天只有测试在调。
- **测试**：`BvhQueryTest` 五条封存用例、`GraphicsTest` 两条（封存 + 粗叶可观测、调用方粒度可换）、
  `StreamUploadsTest` 三条（数组别名到模型地址、索引别名整段、条目与 bind 各自的存活语义）。
  `AHandBuiltDrawPreparesItsOwnTree` 与 `APreparedTreeChangesTheCostAndNotTheAnswer` 改为显式传细叶——它们量的是
  剪枝机制，不是引擎的粒度策略。
- **文档**：`geometry-bvh.md` 2.16 / F10 / C13 / R7 / N3 / P10 / D20 / T12；`graphics-geometry.md` F33 / C12 / N18；
  `vsg-backend.md` F16.1。同时修正了 `geometry-bvh.md` 2.4 把 `Aabb<double>` 写成 24 字节的错（它是 48 字节）。

## 后续

- 图元盒 / 节点盒改 `Aabb<float>`：再省一半的盒内存（无损），需要给 `Bvh` 的体积类型一个“存储标量 ≠ 计算标量”
  的表述。见 `../backlog/geometry.md`。
- 盒子量化（相对父节点、16 bit/轴）是更远处的一步，只在有大模型消费者的持续证据时再做。
