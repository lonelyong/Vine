# geometry 待办

> **状态**：开放项
> **范围**：`src/base/geom/sdk/vine/geom/spatial` 参数化几何的后续能力。现行设计见 `../design/geometry-parametric.md`。

| # | 内容 | 触发条件 | 参考 |
|---|---|---|---|
| G3 | 实体求交（族级自由函数） | 出现需要它的消费者；曲面侧（最近点 / 射线 × 曲面）、点到曲线的距离、两曲线最近距离与解析实体的查询已落地，见 `../design/geometry-surface-queries.md`、`../design/geometry-curve-queries.md` 与 `../design/geometry-solid.md`，仍按自由函数 + `requires` 约束写，不给接口加虚函数 | `../design/geometry-kinds.md` 1.3、9 |
| G5 | 让参数化几何的 B-rep 桥退休 | 三角化已落地（`../design/geometry-tessellation.md`）且没有消费者再要解析实体 | `../design/geometry-parametric.md` 2.5 |
| G7 | `TriangleSurface` / `QuadSurface`（名字已保留） | 单个三角/四边形需要自己的查询（射线–面求交、点到面距离）时；今天用自由函数 `intersectTriangle(ray, a, b, c, hit)`。引入时**必须带 `Surface` 后缀**（仓库已有 `TriangleMesh` / `QuadMesh`） | `../design/geometry-kinds.md` 2.5 |
| G8 | 空间曲线的第四阶及偏置的第三阶导 | 偏置的阶数规则是 `min(2, 基曲线能力 − 1)`，要再上一阶就要基曲线答四阶 | 出现需要偏置第三阶导的消费者（曲率变化率、更高阶拼接） | `../design/geometry-kinds.md` 2.8、`../decisions/ADR-2026-11-06-space-third-derivative-and-offset.md` |
| G9 | `BSplineCurve` 求值的 O(p²) 表算法 | 次数 ≥ 8 的样条被放进批量采样循环（现在是无分配的递归基函数，代价随次数增长） | `../design/geometry-kinds.md` 2.8 |
| G10 | 样条曲面求值的临时分配 | 大批量采样或求值进热路径（现在每次调用按控制点数分配临时数组：`deCasteljau` 按值取数组、`differences` 返回新数组） | `../design/geometry-axis-surfaces.md` 2.5、R2 |
| G11 | 相交配对表里缺的组合（射线–直线、直线–`Aabb`/`Obb`、平面–`Aabb`/`Obb`、`Obb`–`Aabb`） | 出现需要其中某个组合的消费者（布尔运算、截面裁剪、新拾取图元）；届时按 `../design/geometry-intersection.md` 的 C1–C4 加，不加统一接口 | `../design/geometry-intersection.md` §3.1、D1 / D2 |
| G12 | 保证最大偏离的曲线离散化（每条弦扫描真实最大距离后再决定细分，OCCT `CPnts_UniformDeflection` 的做法） | 出现要求「真实最大偏离 ≤ 公差」的消费者（显示之外的用途：布尔、公差报告、厚度/间隙检查、CAM）；只要画得好看就用草案里的中点估计版 | `../design/geometry-curve-discretization.md` 2.5、P1、D2 |
| G13 | 曲线离散化按间断点（`C0` knot）强制分段，折角作为折线顶点保留 | 出现要求「折角必须是一个顶点」而不是「折角被逼近到容差内」的消费者（例如按折线做角点检测、或显示上必须出硬角） | `../design/geometry-curve-discretization.md` 2.6、D3 |
| G14 | 曲线离散化的进度与取消后端：接 `vn::progress::ProgressIndicator` / `ProgressHost`（现在只有预留位 `progress(callback)`） | 第一次出现「对高次样条或整条线框离散时界面要能报进度 / 能取消」的调用点；届时先试无依赖回调，若直接接 `vn::progress` 则要记账 `geom` 新增的依赖边 | `../design/geometry-curve-discretization.md` 2.12、9.2 |
| G15 | 盒的存储标量降为 `float`（`Aabb<float>` 存储、`double` 计算）：盒值本来就是 float 坐标的 `min`/`max`，无损；封存后节点盒是剩余的最大一项 | 大模型的拾取/查询树内存再次成为问题时（封存已落地，见 `ADR-2026-11-10-bvh-seal-and-coarse-picking-tree.md`）；要先在 `Bvh` 上写出“存储标量 ≠ 计算标量”的表述 | `../design/geometry-bvh.md` 2.16、9.2 |
## 判据提醒

- 每一项都只写触发条件：没有消费者的能力不进代码（`G8`）。
- 参数化几何是值类型，任何新能力都要回答"为什么不能是自由函数"。
