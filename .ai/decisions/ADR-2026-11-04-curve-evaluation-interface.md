# ADR-2026-11-04：曲线族是求值接口，曲面族仍是分类标签

> **状态**：已采纳（第 6 条已被 `ADR-2026-11-04-surface-evaluation-interface.md` 取代）
> **日期**：2026-11-04
> **影响范围**：`src/base/geometry/sdk/vine/geometry/Curve.hpp`、`Continuity.hpp`、`BoundedCurve.hpp`、`TrimmedCurve.hpp`、`LineCurve.hpp`、`CircleCurve.hpp`、`Ray.hpp`、`tests/test_geometry`

## 背景

同日的 `ADR-2026-11-04-geometry-kinds.md` 把 `Curve` / `Surface` 定为**空标签**，理由是 vptr 会破坏值类型的内联与 trivially copyable。半天后出现新的需求形态：算法需要"不认识的曲线也能求值、问参数域与周期"（B-rep 的边、遍历、采样），这需要一个**抽象接口**；而同一时刻没有任何消费者对曲面提出同样要求。

## 决定

1. `Curve<T>` 改为**抽象接口**：虚析构 + 求值与参数域属性（`evaluate` / `firstParameter` / `lastParameter` / `isClosed` / `isPeriodic` / `period`），全部纯虚；导数、光滑度与反向映射在第 7 条补齐。
2. `BoundedCurve<T>` 提供非虚的 `startPoint()` / `endPoint()`（由 `evaluate` + 参数域导出），参数域仍由具体类型回答。
3. `TrimmedCurve<T>` 实现该接口：持 `std::shared_ptr<const Curve<T>>` 基曲线 + 参数区间，`evaluate` 转发；闭合/周期性由基曲线与区间共同判定。
4. `LineCurve<T>` / `CircleCurve<T>` 实现接口（直线：参数域无限、非闭合非周期；圆：`[0, 2π)`、闭合且周期 `2π`）。
5. **`Ray` 退出曲线族**：它不再派生 `Curve`，只保留 `pointAt`，保持 trivially copyable。
6. ~~`Surface` 族维持空标签（`ElementarySurface` / `BoundedSurface`），不付 vptr。~~ 已被 `ADR-2026-11-04-surface-evaluation-interface.md` 取代：曲面后来也有了同一个消费者。
7. 接口的核心属性参照 OCCT 的 `Geom_Curve` 补齐：`derivative(u)` / `secondDerivative(u)`（D1/D2，返回速度与加速度向量）、`continuity()`（新头 `Continuity.hpp` 的阶梯枚举）、`reversedParameter(u)`（反向参数映射），全部纯虚；可变 `reverse()`、`bounds()`、`curveType()`、`D3`/`DN` 不取，理由见已否决选项。

## 理由

- `G4`、`G9`：求值是曲线算法的共同契约，接口把"未知类型也能问"写进类型系统；新增一种曲线只是实现六个函数。
- `G8`、`G10`：曲面没有这种消费者，所以不预先付 vptr；`Ray` 在拾取热路径上按值拷贝，实测内联收益 7.09 → 5.99 ns（命中）、3.96 → 2.71 ns（平行剔除）每三角形，因此明确排除。
- `G1`：接口只有一个名字 `Curve<T>`，不再存在"标签 Curve + 接口 Curve"两个家。
- `G4`、`G9`：十项属性（点、一二阶导数、参数域、闭合/周期/周期值、光滑度、反向映射）是“参数化曲线”的最小完备集，切线/法向、采样、曲率、接缝判定都只依赖它们；名字照 OCCT 的 `Geom_Curve` 词汇取，不自创同义词。
- `G4`：`derivative` 返回**速度向量**而非单位切向，`secondDerivative` 返回加速度而非曲率向量；差异写在接口注释里，不留给调用方猜。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 整个 `Curve` 族保持空标签 | 无法表达"未知曲线也能求值"；调用方只能自己做 `variant` + `visit`，消费点各自复制一份分派（`G7`） |
| 接口用 `double` + `Point3d`（照 CAD 内核的写法） | `LineCurve<T>` / `CircleCurve<T>` 是模板值类型，非模板接口只能让 `double` 版本派生，`float` 版本无法进入族（`G6`） |
| 让 `Ray` 也派生 `Curve<T>` | 热路径按值拷贝，vptr 会把已测到的内联收益退回去，并失去 trivially copyable（`G10`） |
| `TrimmedCurve` 持 `intrusive_ptr` | 需要给 `Curve<T>` 加引用计数（ADL `intrusive_ptr_release`），把接口变成 `RefCounted`；`shared_ptr` 只买所有权，不改接口（`G8`） |
| 照 OCCT 提供可变的 `reverse()` | 曲线不可变、基曲线可被多条裁剪共享，就地反向会互相干扰；反向表达为“构造镜像 + `reversedParameter`”，裁剪只在自己区间内反射（`G4`、`G10`） |
| `bounds()` 进曲线接口 | 只有有限曲线总能回答；直线只能返回无穷盒，把“无界”伪装成盒子（`G4`、`G8`） |
| `curveType()` 枚举与 `D3` / `DN` | `dynamic_cast` 与 vtable 已能回答“我是什么”，再维护枚举就是两个家；三阶以上导数没有消费者（`G1`、`G8`） |
| `transform` / `transformedParameter` 进接口 | 坐标系住在 `Shape` 的节点上，曲线不持有放置；接口里放变换会造出第二套坐标语义（`G1`、`G5`） |

## 后果

- `LineCurve` / `CircleCurve` 不再是 trivially copyable，各自多一个 vptr；两者都是冷类型。
- `Ray` 不再是 `Curve`：需要"把射线当曲线"的调用方要显式包装，且据 `geometry-ray.md` 2.1 两者量纲本就不同。
- 曲线族的占位类型（`EllipseCurve`、`BezierCurve`、`BSplineCurve`、`OffsetCurve` 等）是**抽象**类型：不能构造，等实现十个纯虚函数后才可用。
- 十条纯虚函数让“新增一种曲线”有固定成本；`LineCurve`（常导数、零加速度、`CN`、`-u`）、`CircleCurve`（速度 `radius * (-X sin u + Y cos u)`、加速度 `centre - P(u)`、`CN`、`period - u`）、`TrimmedCurve`（四项转发，反向在自身区间内反射）已实现。
- 现行设计记录在 `.ai/design/geometry-kinds.md` 2.1/2.2/2.3。
