# Bug: 节点区间搜索在“最后两跨”上死循环

- **模块**: `src/base/math`（`spline/BSplineBasis.hpp`）
- **状态**: 已修复
- **涉及**: `BSplineBasis.hpp`、`tests/test_math/BSplineBasisTest.cpp`

## 现象

对一段两端夹持、只有一个内部节点的 B 样条（度 1，相异节点 `0 / 1 / 2`，重数 `2 / 1 / 2`），在第二个跨里求值
（例如 `u = 1.5`）会无限循环，进程挂死。写 `geom2d::BSplineCurve` 的测试时第一次暴露。

几何侧此前没有暴露这个缺陷：现有的 3D 样条用例要么是单跨（`knots` 只有两个值，搜索提前返回），要么构造后
`isValid()` 不通过、求值直接返回退化值。也就是说，**带内部节点的样条从未被真正求值过**。

## 根因

`knotSpan()` 的二分搜索把 `high` 初始化成 `last_span`，而搜索实际依赖的不变量是 `U[low] <= u < U[high + 1]`。
当括号只剩一格（`high == low + 1`）时 `mid == low`，而“`u` 在右半”分支执行的是 `low = mid`，中点不再前进，
循环无法退出。

## 修复

`high` 改成 `last_span + 1`，恢复经典 NURBS `FindSpan` 的不变量 `U[low] <= u < U[high]`；理由写进注释，
免得下一次“优化”又把 `+1` 去掉。

## 验证

- `BSplineBasisTest.TheSpanSearchTerminatesOnAOneApartBracket`（度 1、`u = 1.75`）与两个跨搜索用例：
  修前挂死，修后返回正确的跨号。
- `geom2d::BSplineCurve` 的 11 个用例把同一条路径当作正常行为覆盖（`FollowsItsKnots`、`ContinuityComesFromTheMultiplicities`）。
- 全量 ctest 29/29。
