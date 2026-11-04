# geometry 待办

> **状态**：开放项
> **范围**：`src/base/geometry` 参数化几何的后续能力。现行设计见 `../design/geometry-parametric.md`。

| # | 内容 | 触发条件 | 参考 |
|---|---|---|---|
| G1 | 参数化几何的三角化 | 需要把参数化几何渲染或送进碰撞（`graphics::geometryFromShape` 今天只接网格） | `../design/geometry-parametric.md` 1.3 |
| G2 | 参数化几何的射线求交与距离 | 参数化几何需要被直接拾取或做距离查询（今天拾取走网格） | `../design/geometry-ray.md` 2.2 |
| G3 | 族级统一算法（射线–曲面、点到曲线距离、实体求交） | `Curve` / `Surface` 族与曲面子类已落地（`../design/geometry-kinds.md`），但族级算法还没有消费者；出现时按自由函数 + concept 约束写，不要往标签里加虚函数 | `../design/geometry-kinds.md` 1.3、9 |
| G4 | 在带 OCCT 的机器上跑 `test_brep` | 本机缺 OCCT 头文件，`brep` 模块编译失败，F3 的解析式构造未验证 | `../design/geometry-parametric.md` 9 |
| G5 | 让参数化几何的 B-rep 桥退休 | 参数化几何能自己三角化（G1）且没有消费者再要解析实体 | `../design/geometry-parametric.md` 2.5 |
| G6 | 无界 / 裁剪曲面的**实现**（`CylindricalSurface`、`ConicalSurface`、`ToroidalSurface`、`BezierSurface`、`BSplineSurface`、`RectangularTrimmedSurface`） | 类型与归属已在 `Surface.hpp` 固定（占位、不可构造）；参数与算法等不依赖 OCCT 的裁剪面消费者 | `../design/geometry-kinds.md` 2.3、`../design/geometry-parametric.md` 2.5 |
| G7 | `TriangleSurface` / `QuadSurface`（名字已保留） | 单个三角/四边形需要自己的查询（射线–面求交、点到面距离）时；今天用自由函数 `intersectTriangle(ray, a, b, c, hit)`。引入时**必须带 `Surface` 后缀**（仓库已有 `TriangleMesh` / `QuadMesh`） | `../design/geometry-kinds.md` 2.5 |

## 判据提醒

- 每一项都只写触发条件：没有消费者的能力不进代码（`G8`）。
- 参数化几何是值类型，任何新能力都要回答"为什么不能是自由函数"。
