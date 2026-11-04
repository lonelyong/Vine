# ADR-2026-11-04：曲面也是求值接口

> **状态**：已采纳
> **日期**：2026-11-04
> **影响范围**：`src/base/geometry/sdk/vine/geometry/Surface.hpp`、`ElementarySurface.hpp`、`BoundedSurface.hpp`、`Plane.hpp`、`SphericalSurface.hpp`、六个占位曲面头、`tests/test_geometry`

## 背景

`ADR-2026-11-04-curve-evaluation-interface.md` 第 6 条把曲面族留成空标签，理由是"没有消费者要求未知曲面也能求值"。半天后同一个需求在曲面侧出现：面遍历、采样与求交的前置都需要"求值 + 两个方向的参数域与周期"，而曲线接口已经证明这组契约的形状。

## 决定

1. `Surface` 改为**模板接口** `Surface<T>`：虚析构 + 十三项纯虚 —— 求值 `evaluate(u, v)`、两个一阶导 `derivativeU/V`、四个参数界 `first/lastUParameter` / `first/lastVParameter`、两个方向的闭合与周期 `isUClosed/isVClosed/isUPeriodic/isVPeriodic/uPeriod/vPeriod`、`continuity()`；外加非虚 `isCN(n)` 与 `kDimension`。
2. `ElementarySurface<T>` / `BoundedSurface<T>` 降为分类标签：不加状态、不加虚函数。
3. `Plane<T>` 实现接口：两个面内轴由 `normal` 每次推导（世界轴里与法向最不平行的那个的投影），不存第三个向量 —— 平面在射线求交里被复制。
4. `SphericalSurface<T>` 实现接口：`u` 是方位角（`[0, 2π]`，闭合且周期），`v` 是极角（`[0, π]`，两端是极点，不闭合不周期）。
5. 六个占位曲面模板化为 `XxxSurface<T>` 并继承标签，仍不可构造。
6. 接口**不**放 `normalAt()` / `bounds()` / 二阶导数 / 反向与变换族。

## 理由

- `G4`、`G9`：十三项是"参数化曲面"的最小完备集，与 `Curve<T>` 一一对应（一个参数变两个）；新增一种曲面只是实现十三项。
- `G6`：`Curve<T>` 的命名与分组被逐字照搬（`first/last` → `first/lastU`），学一套就够。
- `G1`：接口里只放"必然能回答且只有一个答案"的事实；法向的朝向、包围盒的无界性、曲率的阶数都需要约定，留在具体类型。
- `G10`：`Plane` / `SphericalSurface` 因为接口只多一个 vptr，不多一个成员。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 曲面继续保持空标签 | 标签答不了"未知曲面求值 + 两个方向的参数域与周期"，面遍历/采样只能各自写一套分派（`G7`） |
| 把四个参数界打包成一次调用（OCCT 的 `Bounds(U1,U2,V1,V2)`） | 打包是 C++98 的 out 参数风格；四个虚函数与 `Curve<T>` 的 `first/last` 完全对齐（`G6`） |
| 接口里放 `normalAt(u, v)` | 它是 `dU × dV` 归一化的一行，而且必须选朝向约定；放进接口等于把约定当契约（`G4`） |
| `Plane` 存面内框架向量（像 `ConicCurve`） | 平面在射线求交里被复制且大小有断言；推导比存储便宜（`G10`） |
| 保留非模板 `Surface` 标签、另立一套曲面接口 | 两个 `Surface` 就是两个家，`is_base_of` 会分出两棵树（`G1`） |

## 后果

- **破坏性**：`Surface` / `ElementarySurface` / `BoundedSurface` 都是模板了，必须写 `<T>`；仓库内唯一消费点是 `tests/test_geometry`。
- `Plane` / `SphericalSurface` 各多一个 vptr，不再是 trivially copyable；`sizeof` 断言补上 `sizeof(void*)`。
- 曲面与曲线从此同一个形状，现行设计记录在 `.ai/design/geometry-kinds.md` 2.1 / 2.2 / 2.10。
