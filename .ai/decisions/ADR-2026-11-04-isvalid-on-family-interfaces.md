# ADR-2026-11-04：`isValid()` 放进 `Curve<T>` / `Surface<T>` 接口

> **状态**：已采纳
> **日期**：2026-11-04
> **相关**：`../design/geometry-kinds.md` 2.7 / 2.10 / C12 / §9；`src/base/geometry/sdk/vine/geometry/Curve.hpp`、`Surface.hpp`、`Plane.hpp`；`tests/test_geometry/GeometryKindsTest.cpp`

## 背景

几何族里每个具体类型**早就**有一个 `isValid()`：直线看方向、圆锥曲线看框架、样条看权重与节点、
两种裁剪看基句柄与区间、三个旋转面看轴与半径、两个样条曲面看网与重数。它也是设计文档里写明的唯一判据
（`geometry-kinds.md` C12：「`isValid()` 是唯一的判定入口」）。

问题是这个问题**问不出来**：`isValid()` 不是 `Curve<T>` / `Surface<T>` 的成员，所以拿到
`const Surface<double>&` 的调用方要么先知道具体类型，要么 `dynamic_cast`，要么假定输入可用直接求值。
同一个仓库里的 `Shape` / `BrepShape` / `Primitive` 早就走的是另一条路——`Shape::isValid()` 是虚函数。
即几何族是这条家族惯例里的例外（`G6`）。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 在 `Curve<T>` / `Surface<T>` 上加 `virtual bool isValid() const = 0` | 一个问题一个答案，`const Curve<T>&` 就能问；与 `Shape` / `Primitive` 同一问法；族级算法与契约扫查可在一处守卫 | 接口变宽（曲线 10→11、曲面 13→14 项）；每个实现多一个虚表槽；外部实现者必须实现它（仓库内没有） |
| B. 自由函数 `bool isValid(const Curve<T>&)` + `dynamic_cast` | 不动接口 | 只有认识全部具体类型的自由函数能写，新增一种曲线就要改那个函数——把封闭集合写进了算法（`G9`） |
| C. concept 约束（`requires { t.isValid(); }`） | 编译期检查、零运行期代价 | 只对模板开放，`const Curve<T>&` 这种运行期多态路径仍然问不到；而几何族的主体正是运行期多态（`Primitive::Parameters` 用 variant，几何求值用 vtable） |
| D. 维持现状：各类型自己的 `isValid()`，接口不表 | 不动接口 | 「唯一判据」在一半的调用点不可用，实际会退化成「假定输入可用」，与 C12 的承诺矛盾（`G4`） |

## 决定

采用 A。

- `Curve<T>` / `Surface<T>` 各加一项纯虚 `[[nodiscard]] virtual bool isValid() const = 0;`，与 `isCN()` 相邻，
  注释写明两件事：它是**状态**的答案（与参数无关），以及它为假的查询返回退化值是为了「把错误保持为错误答案而不是未定义行为」。
- 十五个实现原本就有这个方法，只是加 `override`；`Plane` 是唯一没有的，因为平面此前被认为没有无效状态。
  现在它有：零法向没有垂直方向、也没有面内轴，参数化会悄悄退回世界轴——`Plane::isValid()` 因此是
  `normal.length() > EPS`，与圆锥曲线对退化框架的规则一致（P6）。
- `OffsetCurve` 的实现已经存在，`secondDerivative` 仍是纯虚，所以类型依旧抽象（`ADR-2026-11-04-curve-third-derivative.md` 不受影响）。

## 后果

- 好的：`expectCurveInvariants` / `expectSurfaceInvariants` 现在先断言 `isValid()`，族级算法可以在一处守卫；
  「为什么 `isValid()` 在接口上」不再需要解释，因为它就是 `Shape` 的同一问法（`G6`、`G12`）。
- 好的：`Plane` 的零法向从「静默退回世界轴」变成「`isValid()` 为假」，与族内其他类型的退化规则统一。
- 坏的 / 代价：接口变宽，曲面的虚表从 13 槽变 14 槽；外部实现者必须补这一个方法。仓库内唯一的实现者集合是
  本模块自己，`test_geometry` 的 84 例与全仓库构建都在这次改动后通过。
- 触发条件（什么时候重开）：出现**不需要**有效性判据的几何族实现者（例如纯函数式、只由构造保证合法的适配器）
  时，再考虑把它降成非虚的 `bool isValid() const { return true; }` 默认实现。

## 证据

- `src/base/geometry/sdk/vine/geometry/Curve.hpp`、`Surface.hpp`：新增的纯虚函数与 `@details`。
- `src/base/geometry/sdk/vine/geometry/Plane.hpp`：唯一新写的实现，`normal.length() > EPS`。
- `tests/test_geometry/GeometryKindsTest.cpp`：两行 `static_assert`（接口上可问、返回 `bool`）、
  `validityIsAskableThroughTheInterface`（五次经接口提问）、族级扫查里的 `EXPECT_TRUE(curve.isValid())`。
- `src/base/geometry/sdk/vine/geometry/Shape.hpp`：`virtual bool isValid() const;`——同一问法的先例。
