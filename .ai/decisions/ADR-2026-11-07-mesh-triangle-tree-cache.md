# ADR-2026-11-07：网格自带三角形身份，并把三角形树当作缓存

> **状态**：已采纳
> **日期**：2026-11-07
> **影响范围**：`src/base/models/sdk/vine/models`（`Mesh.hpp`、`TriangleMesh.hpp`、`QuadMesh.hpp`、`MixedMesh.hpp`）、
> `tests/test_geom_spatial/MeshBvhTest.cpp`、`.ai/design/graphics-geometry.md`、`.ai/design/geometry-bvh.md`

## 背景

`geometry` 现在有能力建一棵静态包围体层次（`Bvh` + `BvhBuilder`，见 `ADR-2026-11-07-bvh-as-structure-plus-builder.md`）。
要把它用在网格上，必须先回答三个问题：

1. **树的 primitive 是什么**？三角形的相交函数 `intersectTriangle` 是查询单位，所以 primitive 必须是三角形。
2. **谁说得清“这个网格有哪些三角形”**？`Mesh` 只持有顶点属性与索引，面拓扑由子类决定（`TriangleMesh` 三项一面、
   `QuadMesh` 每面四角、`MixedMesh` 用 offsets 分面），此前只有 `TriangleMesh` 有 `triangleCount()`。
3. **树放在哪，算内容还是算缓存**？`Mesh` 已经有 `aabb_` 这个先例。

## 决定

1. `Mesh` 声明三角形身份的两个虚函数：`triangleCount()`（基类默认 0）与 `triangleCorners(triangle, corners[3])`
   （基类默认 false，返回三个**顶点索引**）。三个具体网格各自按自己 `toTriangleMesh()` 的同一条规则实现
   （三角三项一面；四边形沿第 1→3 角对角线拆两个；多边形按三角扇）。`TriangleMesh::triangleCount()` 成为其覆写，签名不变。
2. `Mesh` 缓存一棵 `Bvh<double>`：`bvh()` / `hasBvh()` / `setBvh()` / `buildBvh(builder)` / `clearBvh()`，
   与 `aabb()` / `setAabb()` / `computeAabb()` 同一套形状。`buildBvh()` 每个三角形加一个 primitive，
   因此 **prim id == 三角形序号**。
3. 失效规则与 `aabb_` 同源但范围不同：`setPositions()` / `setIndices()` / `MixedMesh::setFaceOffsets()` 丢树，
   法线、贴图坐标与颜色不丢；未构建的树不回答候选，**不**退化成线性扫描。
4. 树不隐式构建：`bvh()` 不会因为一次查询就建树，`buildBvh()` 是显式的一次性动作。

## 理由

- **primitive id 必须等于三角形序号**：这个序号是拾取结果的身份，也是跨 `toTriangleMesh()` 稳定的东西。
  两个序不一致，调用方就得再做一次映射表（`G4`）。
- **三角形身份上移到 `Mesh`，而不是留在 `TriangleMesh`**：`QuadMesh` / `MixedMesh` 是独立消费者（模型查看器、meshio），
  若树只建在 `TriangleMesh` 上，它们必须先把整个网格复制成 `TriangleMesh` 才能查询（`G1`、`G10`）。
- **虚函数而不是调用方自己遍历**：`MixedMesh` 的多边形要按扇展开、`QuadMesh` 要沿固定对角线拆——这段规则每个消费者
  都会重写一遍（`G7`）。两个虚函数把它压在类型自己身上，且基类有合理默认值（不承诺面的网格回答 0）。
- **`double` 而不是 `float`**：树不是几何数据，是查询用的导引结构（`G13`）。查询射线在所有消费者手里都是 `Rayd`；
  把射线窄化成 float 做盒测试，在远离原点处会丢掉小叶子盒所需的精度而**漏命中**。反过来把 float 顶点加宽进 double
  盒是精确的，所以代价只是节点内存，不是精度。
- **它是缓存，不是内容**：它不持有身份、可由三角形重算、可被任何改变三角形的 setter 丢掉——与 `aabb_` 同一条判断
  （`G1`、`G13`）。这也回答了“为什么不放 `Scene`”：场景关心节点与实例，网格自己就说得清它由哪些三角形组成。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 只在 `TriangleMesh` 上缓存树 | 其它两种网格必须先 `toTriangleMesh()` 复制一份才能查询；三角形身份于是分裂成两份（`G1`、`G10`）。 |
| 树用 `float`（与顶点同精度） | 窄化查询射线会在远离原点处漏命中；把顶点加宽是精确的，只有节点内存的代价（`G4`、`G10`）。 |
| 把三角形身份做成 `std::vector<std::array<uint32_t,3>>` 物化表 | 建树本来就要扫一遍，物化只多一份需要与网格同步失效的副本（`G10`、`G13`）。 |
| 树用 `std::unique_ptr` 惰性分配 | `bvh()` 于是要返回指针或 nullable 引用，每个调用点都要判空；`Bvh` 空构造只是一个空 vector（`G4`、`G7`）。 |
| 首次查询时自动建树 | 查询是 `const` 且要求可并发；隐式构建会让第一次查询分配、且在建树期间阻塞其它查询（`G10`、`G11`）。 |
| 树建在 `Scene` / 渲染侧 | 每个实例复用同一份网格数据，树是**局部空间**的属性，放场景侧会按实例重复（`G1`、`G5`）。 |

## 后果

- `Mesh` 多两个虚函数（`triangleCount()` 的 vtable 位置从 `TriangleMesh` 上移）与一棵空 `Bvh` 成员（三个空 vector）。
  vtable 变化对外不可见：`Mesh` 家族一直是运行时多态边界。
- `TriangleMesh::triangleCount()` 语义不变，只是从“仅此类型定义”变成“覆写基类约定”。
- `QuadMesh` / `MixedMesh` 多出 `triangleCount()` / `triangleCorners()`，因此不复制网格也能查询。
- `MixedMesh::triangleCorners()` 用二分找三角形所属的面（面的三角形起点 `offsets[f] - 2f` 单调），不存逐三角形表。
- 拾取侧尚未接上：`graphics::RayIntersection` 仍线性扫三角形，已记入 `../backlog/graphics.md` T24。
- 现行设计记录在 `.ai/design/graphics-geometry.md` 2.8 / 2.9 / F32，树本身在 `.ai/design/geometry-bvh.md`。
