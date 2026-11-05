# geometry 待办

> **状态**：开放项
> **范围**：`src/base/geometry` 参数化几何的后续能力。现行设计见 `../design/geometry-parametric.md`。

| # | 内容 | 触发条件 | 参考 |
|---|---|---|---|
| G3 | 实体求交（族级自由函数）与曲线 × 曲线的最近距离 | 出现需要其中任一项的消费者；曲面侧（最近点 / 射线 × 曲面）、点到曲线的距离与解析实体的查询已落地，见 `../design/geometry-surface-queries.md`、`../design/geometry-curve-queries.md` 与 `../design/geometry-solid.md`，仍按自由函数 + `requires` 约束写，不给接口加虚函数 | `../design/geometry-kinds.md` 1.3、9 |
| G5 | 让参数化几何的 B-rep 桥退休 | 三角化已落地（`../design/geometry-tessellation.md`）且没有消费者再要解析实体 | `../design/geometry-parametric.md` 2.5 |
| G7 | `TriangleSurface` / `QuadSurface`（名字已保留） | 单个三角/四边形需要自己的查询（射线–面求交、点到面距离）时；今天用自由函数 `intersectTriangle(ray, a, b, c, hit)`。引入时**必须带 `Surface` 后缀**（仓库已有 `TriangleMesh` / `QuadMesh`） | `../design/geometry-kinds.md` 2.5 |
| G8 | `OffsetCurve::secondDerivative()`（点与一阶导数已实现） | 出现需要偏置曲线二阶导数的消费者（偏置曲率、G2 拼接）；届时给 `Curve<T>` 加 `thirdDerivative()` 并让八种具体曲线实现它 | `../design/geometry-kinds.md` 2.8、D9、`../decisions/ADR-2026-11-04-curve-third-derivative.md` |
| G9 | `BSplineCurve` 求值的 O(p²) 表算法 | 次数 ≥ 8 的样条被放进批量采样循环（现在是无分配的递归基函数，代价随次数增长） | `../design/geometry-kinds.md` 2.8 |
| G10 | 样条曲面求值的临时分配 | 大批量采样或求值进热路径（现在每次调用按控制点数分配临时数组：`deCasteljau` 按值取数组、`differences` 返回新数组） | `../design/geometry-axis-surfaces.md` 2.5、R2 |
| G11 | 相交配对表里缺的组合（射线–直线、直线–`Aabb`/`Obb`、平面–`Aabb`/`Obb`、`Obb`–`Aabb`） | 出现需要其中某个组合的消费者（布尔运算、截面裁剪、新拾取图元）；届时按 `../design/geometry-intersection.md` 的 C1–C4 加，不加统一接口 | `../design/geometry-intersection.md` §3.1、D1 / D2 |
## 判据提醒

- 每一项都只写触发条件：没有消费者的能力不进代码（`G8`）。
- 参数化几何是值类型，任何新能力都要回答"为什么不能是自由函数"。
