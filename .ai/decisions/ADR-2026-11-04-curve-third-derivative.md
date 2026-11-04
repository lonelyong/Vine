# ADR-2026-11-04：偏置曲线的二阶导数不实现，也不给 `Curve<T>` 加三阶导数

> **状态**：已采纳
> **日期**：2026-11-04
> **相关**：`../design/geometry-kinds.md` 2.7 / 2.8 / D9；`src/base/geometry/sdk/vine/geometry/Curve.hpp`、`OffsetCurve.hpp`；`tests/test_geometry/GeometryKindsTest.cpp`

## 背景

`BezierCurve` / `BSplineCurve` 的求值落地后，`OffsetCurve` 只剩一个纯虚函数：`secondDerivative()`。按定义
`P(u) = p(u) + d * N(u) / |N(u)|`，其中 `N(u) = reference_direction x p'(u)`，因此

- `P'(u)` 需要 `p''`（接口已有）；
- `P''(u)` 展开后含 `N''(u) = reference_direction x p'''(u)`，也就是**基曲线的三阶导数**。

`Curve<T>` 的第一版就把 `D3` / `DN` 明确排除在接口之外（`ADR-2026-11-04-curve-evaluation-interface.md` 第 7 条、`geometry-kinds.md` D6），理由是当时没有任何消费者。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 给 `Curve<T>` 加 `thirdDerivative()` 纯虚 | 偏置的二阶导数精确、与 OCCT 的 `Geom_Curve::D3` 对齐 | 所有具体曲线（直线、四个圆锥、两种样条、裁剪、偏置自己）都要实现；接口从十项变十一项，每个新增曲线多一份成本，而今天只有一条曲线需要它（`G8`） |
| B. 用 `derivative()` 的中心差分近似 | 不改接口，`OffsetCurve` 立刻可构造 | 精度随步长变化、结果依赖参数化尺度；把一个隐式数值误差藏进一个本应精确的契约（`G4`、`G11`） |
| C. `secondDerivative()` 保持纯虚，`evaluate()` / `derivative()` 照常实现 | 不扩大接口、不引入数值误差；没实现的部分由类型系统如实表达（抽象类型不可构造） | `OffsetCurve` 仍不可构造，只能通过派生类测试（测试里一个只补这一项的小 stub） |

## 决定

采用 C。

`OffsetCurve` 在派生类里重新声明 `secondDerivative(T) const override = 0;`，并在声明处写清阻塞原因（缺三阶导数、
拒绝有限差分）。类型因此保持抽象，和“这一项还没有实现”这件事完全一致。

## 后果

- 好的：接口大小不变，八种具体曲线不必为一个没有消费者的能力付账（`G8`）；偏置的点与一阶导数已经可用，未来补
  三阶导数时只需在 `Curve<T>` 上增加一项、在 `OffsetCurve` 里补一条公式。
- 坏的 / 代价：`OffsetCurve` 不能构造，`evaluate()` / `derivative()` 只能经派生类调用；使用者要多看一眼
  `is_abstract` 的原因。
- 触发条件（什么时候重开）：出现真正需要**偏置曲线二阶导数**的消费者（例如偏置曲线的曲率、G2 拼接检查），
  或第二个需要三阶导数的算法（例如 jerk 连续的运动规划）。届时按 A 扩充接口，并同步更新
  `geometry-kinds.md` 2.7 与 `D6`。

## 证据

- `src/base/geometry/sdk/vine/geometry/OffsetCurve.hpp`：`evaluate()` / `derivative()` 的实现与 `secondDerivative()`
  上的 `= 0` 及说明。
- `tests/test_geometry/GeometryKindsTest.cpp`：`static_assert(std::is_abstract_v<OffsetCurved>)`、
  `OffsetStub` 只补 `secondDerivative()`、`anOffsetCurveInheritsTheBasisDomain` 覆盖域转发与位移几何。
- OCCT `Geom_OffsetCurveUtils.pxx` 的 `CalculateD2` 用到了基曲线的 `D3`，即同一阻塞在参考实现里的形态。
