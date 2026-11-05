# geom2d 待办

> **状态**：开放项
> **范围**：`src/base/geom2d` 的后续类型与查询。现行设计见 `../design/geometry-planar.md`。

## 1. 现在可做（无触发条件）

空。

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| P1 | `HyperbolaCurve` / `ParabolaCurve` | 平面开放曲线：求值与导数，域按各自参数化 | 有消费者需要开放的非直线平面曲线（轮廓延长、锥线求交的输入） | `../design/geometry-planar.md` 1.3 |
| P2 | 点到平面曲线的最近点 / 距离 | 与 3D 的 `closestPointOnCurve()` 同一骨架：平方距离驻点求根 + 窗口端点比较，写成自由函数 | 有消费者要做平面投影或拾取距离 | `../design/geometry-planar.md` F7 |
| P3 | 平面曲线求交、平面包围盒 | 求交结果形态沿用 3D 的配对表判据；包围盒用 `math::Rect2` 或新的平面盒，只给有界类型 | 有消费者需要其中任一项（截面、布尔、视图裁剪框） | `../design/geometry-planar.md` F7、9.2 |
| P4 | 平面样条 / 偏置曲线 | `BezierCurve` / `BSplineCurve` 与偏置：引入时在 R2（值类型不持有动态内存）上写明例外 | 有消费者需要自由曲线（2D 草图、导入的平面轮廓） | `../design/geometry-planar.md` 1.3、R2 |
| P5 | `ConicCurve` 层（平面） | 圆锥曲线共享的“起始轴方向 + `continuity() == CN`”；今天只有椭圆一根轴，圆由平面定死 | 双曲线或抛物线至少一个落地，出现第二个消费者 | `../design/geometry-planar.md` 2.4 |
| P6 | 通用平面 `TrimmedCurve` | 任意基曲线 + 参数区间的视图；`ArcCurve` 只是“圆 + 区间”的值类型版本 | 出现非圆基曲线需要裁剪（椭圆弧、样条修剪） | `../design/geometry-planar.md` 2.8 |

## 判据提醒

- 每项的触发条件都必须是一个**可观察的事实**（某个消费者、某个调用点），不是“以后可能有用”（`G8`）。
- 平面类型是值类型，任何新能力都要先回答“为什么不能是自由函数”（`G7`、`G12`）。
- 新增类型时同步回答：`isValid()` 的判据是什么、退化状态答什么、`derivative` 的量纲是什么，三条缺一不可。
