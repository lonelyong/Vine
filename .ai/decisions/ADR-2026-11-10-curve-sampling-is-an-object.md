# ADR-2026-11-10：曲线离散化做成对象，而不是三个自由函数

> **状态**：已采纳
> **日期**：2026-11-10
> **影响范围**：`src/base/geom/sdk/vine/geom/spatial/CurveSampling.hpp`、`src/base/geom/sdk/vine/geom/planar/CurveSampling.hpp`、
> `tests/test_geom_spatial/CurveSamplingTest.cpp`、`tests/test_geom_planar/CurveSamplingTest.cpp`

## 背景

曲线离散化第一次落地。仓库对"算法"的默认形态是**自由函数 + `requires` 约束**（`closestPointOnCurve()`、`arcLength()`、
`parameterAtLength()`、`tessellateSolid()`），对"封闭集合"的默认形态是**重载 + 编译期分派**——`ADR-2026-11-09-solid-closed-set-without-variant.md`
甚至专门删掉过一个变体类型，并否决了「虚 `kind()` / 虚 `tessellate()`」。

设计评审时方案由自由函数改成了对象形态，理由是三条具体需求：

1. 派生属性要有地方挂（实际的偏离、逐段弧长、切线）。返回一个值结构就只能预先算好所有属性，或每次改签名；
2. 一条线框有几十段，每段一条曲线。窗口与参数应当只设一次，循环里只换曲线；
3. 进度与取消只能有一个位置，否则每个入口都要多一个回调参数，而真正需要它的只有一个。

这三条与"仓库默认"相反，因此记档。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 三个自由函数 + `CurvePolyline<T>` 返回值（仓库默认） | 零状态、无 vptr、具体曲线的 `evaluate` 可内联；实现最小 | 循环里重复传窗口与参数；派生属性无处可放（要么改签名，要么再算一遍）；进度 / 取消只能污染每个入口的签名 |
| B. `CurveSampler<T>` 基类 + 三个子类，setter 配置、`compute()` 无参（**采纳**） | 配置一次跑很多条曲线；属性挂在对象上，新增属性不改已有签名；进度钩子只有一个位置 | 两条继承树（`float` / `double`）；每个采样点一次虚 `evaluate()`；`curve()` 只存引用（短命契约）；结果复位规则必须显式写死 |
| C. 有基类但构造即算（OCCT 的 `Initialize` 在构造里跑） | 调用处最短 | 构造期不能虚分派（纯虚崩溃，或跑成基类版本）；曲线与窗口是配置，只能塞进构造参数，其余参数无处安放 |
| D. 基类 + 一条虚 `sample(curve)`，曲线每次传入 | 对象无状态 | 每条规则的参数只能进构造函数，与 C 同病；"先 set 再 compute"的复用消失 |

## 决定

采用 **B**，并配三条契约把它的代价关住：

- **C7**：`curve()` 只存引用，sampler 必须比曲线短命，不延长寿命；
- **C8**：任何 setter 与任何失败的 `compute()` 都清空结果——`polyline()` 要么属于最后绑定的那条曲线，要么是空，从不是上一条曲线的点；
- setter 返回 `void` 而不是 `*this`：链式返回的类型会随基类 / 子类而变，"哪个 setter 能接在谁后面"就成了一条隐式约定（写测试时踩了两次）。配置写成语句，任何顺序都成立。

规则本身（参数等分 / 弧长等距 / 弦高偏离）仍是封闭集合：新增一条规则是一个新的子类，不改基类、不改内核。算法只有一份，住在
`geom/detail/CurveSubdivision.hpp`（`ADR` 不记账这一点，`geometry-module.md` 2.7 记了"为什么它留在头里"的实测）。

## 后果

- **好的**：`curve(c); compute();` 是主用法，线框循环只需设一次配置；结果与派生属性都在对象上；`C8` 把"读到上一条曲线的点"这类错误从"看起来成功"变成"读到空"；运行时切换规则只需 `std::unique_ptr<CurveSampler<T>>`。
- **坏的 / 代价**：`CurveSampler<float>` 与 `<double>` 是两条无关继承树；`computeImpl()` 只能收 `const Curve<T>&`，放弃了"具体类型的求值可内联"（`Curve.hpp` 的 `@warning` 正是为热路径保留这一点），每个采样点多一次虚调用。采样不是逐帧路径，接受（`G10`）。
- **触发条件（什么时候重开）**：
  - 需要**不物化数组**的流式取点（显示列表、碰撞扫描边取边用）——那是迭代器 / 回调，另行设计，不是把现有入口改成类；
  - 需要**外部实现替换整套规则**——那是上层（插件）的注册表，不在 `geom` 里加虚接口；
  - 同一组配置需要在超过一个对象生命周期里保存——届时是值结构（`TessellationQuality` 的先例），不是 setter 类。

## 证据

- 测试：`tests/test_geom_spatial/CurveSamplingTest.cpp`（10 例，含 setter 复位、跨曲线复用、预算拒绝）、
  `tests/test_geom_planar/CurveSamplingTest.cpp`（4 例）。
- 设计：`../design/geometry-curve-discretization.md` 2.1 / 2.2 / 2.10 / 2.11 与 §7 `D1`、`D6`、`D9`、`D10`。
- 交叉：`ADR-2026-11-09-solid-closed-set-without-variant.md` 否决的是「把虚函数挂到 `Solid<T>` 族接口上」，不是
  「不许有算法对象树」；本 ADR 没有推翻它，采纳的是另一侧的那一半。
