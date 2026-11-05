# ADR-2026-11-05：平面曲线契约独立于空间曲线契约

- 状态：已采纳
- 日期：2026-11-05
- 相关：`../design/geometry-planar.md`、`../design/geometry-kinds.md`

## 背景

`geom3d::Curve<T>` 已经是十一项求值接口（`geometry-kinds.md` 2.7）：`evaluate` / `derivative` /
`secondDerivative`、参数域两端、`isClosed` / `isPeriodic` / `period`、`continuity`、`isValid`、
`reversedParameter`。`geom2d` 落地时需要回答一个结构问题：这十一条事实在平面里是**再声明一遍**，
还是让 2D 类型与 3D 共用一份接口？

共用有两种常见写法：把点类型提成模板参数（`Curve<PointT>`），或让 `geom2d::Curve<T>` 继承
`geom3d::Curve<T>` 并把平面看作 `z = 0` 的子空间。前者是“同一份契约、两种维度”，后者是“同一种对象、
两种维度”。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 独立声明 `geom2d::Curve<T>`（十一项同名同义，`Point2` / `Vector2`） | 两个概念各有定义与归属；2D 类型不带 z、不带法向；3D 代码不可能误收平面曲线 | 契约文字有两份，一侧新增事实时另一侧可能漂移 |
| B 一份 `Curve<PointT>` 泛型契约，两模块共用 | 契约只有一份 | 3D 每个消费点都要模板化；2D 类型被迫满足只有 3D 才需要的约束（例如将来的法向、框架）；接口出现“2D 不可用”的成员 |
| C `geom2d::Curve<T>` 继承 `geom3d::Curve<T>`，平面嵌入 `z = 0` | 复用实现与算法签名 | 平面曲线被赋予一个恒等法向与一条恒为零的维度；3D 算法会把平面曲线当空间曲线消费；每次求值多一维 |
| D 在 3D 侧用 variant / 适配器包装平面曲线 | 不新增契约 | 每次求值多一层调用与分支；平面曲线仍要自己存点，包装不减少任何代码 |

## 决定

采用 A：`geom2d::Curve<T>` 是**第二份契约**，与 3D 契约逐项同义但互不继承。

判据是 `G1`（概念清晰、抽象自然）与 `G5`（内聚而低耦合）：
“平面曲线”和“空间曲线”的边界不是同一个类型族的两个特化，而是两个各有消费方、各有几何事实的概念。
共用契约只能靠“给 2D 类型加一个它没有的字段”（C）或“让 3D 代码为维度付模板代价”（B）实现，
两种做法都把复杂度转移给了不需要它的一侧（`G7`）。

共享的部分收在词汇层，而不是类型层：`math::Continuity`、`math::FP`、以及将来的求根设施在 `math` 里只有一份定义，
`geom2d_global.hpp` 只做名字重导出。维度无关的事实因此天然一致，维度相关的事实留在各自的类型上。

## 后果

- 好的：`geom3d` 不知道 `geom2d` 存在，两个模块可以各自演进；平面类型的大小与 `Point2 + 标量` 齐平，没有 vptr 之外的多态成本；
  3D 的族级算法（最近点、求交、三角化）在编译期就拒绝平面曲线。
- 坏的 / 代价：契约文字有两份；一侧新增一项事实时，另一侧不会自动获得（`G6` 的显式让步）。
  今天的十一项在两侧同名同义，没有已知分歧；`Curve.hpp` 的文档注释把这条写成了“同一份契约、少一个维度”，
  以便改动时两边一起读。
- 触发条件（什么时候重开）：出现一个必须同时对 2D 与 3D 曲线编写的算法，且它只依赖 `evaluate` / `derivative` /
  参数域 / `isValid`。那时正确做法是先把这个算法的依赖抽成独立于两个接口的自由函数模板（与
  `closestPointOnCurve()` 同一形态），而不是合并接口。

## 证据

- `src/base/geom2d/sdk/vine/geom2d/Curve.hpp` 与 `src/base/geom3d/sdk/vine/geom3d/Curve.hpp`：十一项签名逐项对应，只把 `Point3` / `Vector3` 换成 `Point2` / `Vector2`。
- `src/base/geom2d/sdk/vine/geom2d/geom2d_global.hpp`：`Continuity` 由 `math` 重导出，不是第二份枚举。
- `src/base/geom2d/sdk/vine/geom2d/CircleCurve.hpp`：平面圆是 `center` + `radius`，没有 3D 的 `ConicCurve` 框架。
- `tests/test_geom2d/CurveBasicsTest.cpp`：`AnswersThroughTheBaseInterface` 通过 `Curve<double>&` 与 `unique_ptr<const Curve<double>>` 验证多态路径可用，且不需要 3D。
