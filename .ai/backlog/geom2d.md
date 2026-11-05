# geom2d 待办

> **状态**：开放项
> **范围**：`src/base/geom2d` 的后续类型与查询。现行设计见 `../design/geometry-planar.md`。

## 1. 现在可做（无触发条件）

空。

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| P1 | 通用平面 `TrimmedCurve` | 任意基曲线 + 参数区间的视图（`shared_ptr` 基曲线）；落地时**删除** `ArcCurve`，不与之并存（`../decisions/ADR-2026-11-05-planar-arc-is-a-value-type.md`） | 出现第一个要裁剪非圆基曲线的消费者（椭圆弧、样条修剪、导入轮廓截断） | `../design/geometry-planar.md` 2.8、D6 |
| P2 | 运行期决定基曲线的偏置 | 把 `OffsetCurve<C>` 包一层类型擦除（或反过来评估给 `Curve<T>` 加三阶导） | 出现“对 `Curve<T>&` 做偏置”的消费者（异构列表、运行期选择的曲线） | `../design/geometry-planar.md` 2.13、`../decisions/ADR-2026-11-05-planar-offset-curve.md` |

## 判据提醒

- 每项的触发条件都必须是一个**可观察的事实**（某个消费者、某个调用点），不是“以后可能有用”（`G8`）。
- 平面类型是值类型，任何新能力都要先回答“为什么不能是自由函数”（`G7`、`G12`）。
- 新增类型时同步回答：`isValid()` 的判据是什么、退化状态答什么、`derivative` 的量纲是什么，三条缺一不可。
