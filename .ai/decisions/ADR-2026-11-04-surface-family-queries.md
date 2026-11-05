# ADR-2026-11-04：族级曲面查询用数值迭代，不扩 `Surface` 接口

- 状态：已采纳
- 日期：2026-11-04
- 相关：`../design/geometry-surface-queries.md`、`../design/geometry-kinds.md` 2.10、`../design/geometry-intersection.md` C5；`src/base/geometry/sdk/vine/geometry/SurfaceProjection.hpp`、`RaySurfaceIntersection.hpp`、`SurfaceParameterWindow.hpp`；`tests/test_geometry/SurfaceQueryTest.cpp` <!-- drift-ok -->

## 背景

`Surface<T>` 是求值接口：`evaluate`、两个一阶导、参数域、闭合 / 周期、`isValid`。它对“点到曲面的最近点”和“射线 × 曲面”都答不出来，而这两件事正是拿着 `const Surface<T>&` 的调用方最常问的。解析面各有精确入口（`Plane::intersectWith(Ray)`、`signedValue()`），样条面与裁剪面没有。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 给接口加 `inverse(point, u, v)` / `normalAt()` / `bounds()` | 每个曲面自己回答投影，算法薄 | 13 个实现各写一套迭代；接口从“求值”扩成“求值 + 求解”；族级契约扫查要多验三项（`G7`、`G8`） |
| B 逐类型补最近点与射线入口 | 精确、可优化 | 13 × 2 个入口，新曲面再写一遍（`G9` 的反例）；裁剪视图与样条面仍要另做 |
| C 无状态自由函数，接口约束 + 参数窗口 + 网格播种 + 牛顿迭代 | 一套覆盖所有实现；接口不动；解析面继续走自己的入口 | 局部解、有采样代价、相切与奇异点有明确缺口（`G10`） |

## 决定

采用 C：

1. `closestPointOnSurface()` 对 `|S(u,v) - P|²` 做带阻尼与线搜索的高斯-牛顿（只需两个一阶导）。
2. `intersectRayWithSurface()` 在 `(u, v, t)` 上解 `S(u,v) = O + tD`，雅可比 `[dS/du dS/dv -D]`。
3. 窗口由调用方给（`SurfaceParameterWindow<T>`），**无限的一端取曲面自己的界**（默认值 `naturalSurfaceWindow()` 因此就是自然域）；`finiteParameterWindow()` 是唯一解析点，规则与曲线 × 曲面一致。
4. 射线结果内嵌 `RayHit<T>`，法向朝向规则只有一个家；重合（射线在面上）用 `coincident` 标志而不是枚举。
5. 约束写成函数上的 `requires std::is_base_of_v<Surface<T>, S>`，不写成命名概念：`Plane` 经分类标签间接派生，`const Surface<T>&` 参数推导不到 `T`，而概念体里出现受约束模板 `Surface<T>` 会被 MSVC 拒绝（C7605）。
6. 曲线 × 非解析曲面在同一个投影上合成：`intersectCurveWithSurfaceByProjection()` 用 `detail::signedDistanceToSurface()`（距离 + 参数化法向的符号）驱动与解析入口**同一份**扫描。带符号距离留在 `detail`，因为它的极性属于参数化，而求根只需要变号。

## 后果

- 好的：接口维持“只回答求值”；任何现有与将来的 `Surface<T>` 直接被覆盖；仓库不再出现第二套法向约定；曲线 × 任意曲面与射线 × 任意曲面共用同一套投影，而不是各自写一份。
- 坏的 / 代价：结果是**局部**最小；相切漏检；网格分辨不了的折叠会漏命中；每次查询有 `(seed_count+1)²` 次求值的采样代价。全部写在 `geometry-surface-queries.md` §6。
- 触发条件（什么时候重开）：出现热路径消费者（拾取逐帧对样条面求交）或需要全局最近点时，把某个具体类型自己的解析 / 缓存入口加到该类型上，不回扩接口。

## 证据

- `tests/test_geometry/SurfaceQueryTest.cpp`：19 个用例，覆盖平面 / 球 / 圆柱 / 圆环 / 双线性样条面、两种精度、窗口约束与无限端规则、背后命中、重合分类与不可用输入。 <!-- drift-ok -->
- `tests/test_geometry/CurveSurfaceTest.cpp` 的 `CurveSurfaceProjection.*`：7 个用例，覆盖曲线 × 样条面、与解析入口的同一交点、裁剪窗口、曲线在面上、奇异参数线、不可用输入与两种精度。 <!-- drift-ok -->
- `GeometryKindsTest` 的族级扫查未改动：接口没有新增虚函数。
