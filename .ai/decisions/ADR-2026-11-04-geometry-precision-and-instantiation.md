# ADR-2026-11-04：几何类型的精度是保留的双选项，两种精度都显式实例化

> **状态**：已采纳
> **日期**：2026-11-04
> **影响范围**：`src/base/geometry/sdk/vine/geometry`（全部值类型与曲线曲面族的 `T`）、`src/base/geometry/src/Instantiations.cpp`（新增）、`LineCurve.cpp` / `Obb.cpp` / `Plane.cpp` 的显式实例化、`.ai/design/geometry-primitives.md` 2.8 <!-- drift-ok -->

## 背景

几何值类型全部以 `math::FP T` 参数化（`Aabb<T>`、`Ray<T>`、`Plane<T>`、`Curve<T>`、`Surface<T>` …）。实际消费者（`graphics`、`meshio`、`robotics`、`brep`）只用 `double`，于是出现两个问题：

1. 模板参数看起来是"零消费者的自由度"，一度准备去掉，并把 `float` 视为应该收窄的承诺；
2. 只有 `LineCurve` / `Obb` / `Plane` 因为部分成员定义在 `.cpp` 才被显式实例化，其余纯头文件类型**库里没有任何符号**——每个消费者各自实例化一份，谁也不确定库里到底"提供"了什么。

## 决定

1. **保留 `T`**。几何值类型继续以精度参数化。
2. **两种精度都随库发布**：`float` 与 `double` 各显式实例化一次并导出，`*f` / `*d` 别名都保留。
3. **实例化集中且完整**：纯头文件类型的实例化清单放在 `src/base/geometry/src/Instantiations.cpp`（每个类型两行）；有 `src/` 实现的 `LineCurve` / `Obb` / `Plane` 继续在自己的 `.cpp` 里实例化，因为显式实例化只能实例化该翻译单元看得见的成员定义。 <!-- drift-ok -->
4. **不加 `extern template`**：内联仍发生在调用方，导出符号只是"不能内联时的兜底"。

## 理由

- **精度是行业变量，不是实现细节**（`G9`、`G8`）。`float` 几何在实时/游戏（Unity、PhysX 只有 float，Bullet 用 `btScalar` + `BT_USE_DOUBLE_PRECISION` 切换）、移动/AR-VR（部分 GPU 无 double 硬件）、GPU 与硬件光追（BVH、compute 全是 float32）、点云/LiDAR（PCD/PLY 就是 float32，一亿点是"能否进内存"的差别）里是正解；`double` 在 CAD/CAM、测量、运动规划里是必须。框架阶段不能替未来的客户选边，而参数化标量恰好是"不选边"的最便宜写法——这也是 Eigen / GLM / CGAL / PCL 的共同做法（本仓已依赖 Eigen）。
- **不实例化就不生成代码**：`T` 的本体成本为 0；真正的成本是 API 表面与文档，那由"`*d` / `*f` 别名 + 一份策略文档"解决（`G4`）。
- **两种精度都发布**才让"双精度"成为事实而不是口头承诺（`G4`）：消费者不需要知道成员定义在哪、也不需要自己实例化；缺失时链接失败而不是静默各自生成一份。
- **集中清单**使"库里到底提供了哪些几何类型"变成一份可审查的清单，同时把这一个 TU 变成"每个类型都能实例化"的编译期检查（`G11`）。
- **不加 `extern template`**（`G10`）：拾取循环的实测收益（`geometry-ray.md` 2.7：命中 7.09 → 5.99 ns，平行剔除 3.96 → 2.71 ns）来自把短查询内联进调用方；`dllimport` 会让这条收益消失。导出是兜底，不是替代。

## 已否决选项

| 选项 | 否决理由 |
|---|---|
| 全族固定 `double`、去掉模板 | 等于宣布"这个框架只服务精密几何"，把实时/移动/GPU/点云这些行业挡在门外；而模板未实例化时代价为零（`G9`） |
| 只发布 `double`，删掉 `*f` 别名与 float 实例化 | 把"支持 float"从事实降成"你自己去实例化"：每个消费者各生成一份实现，ABI 与行为都不确定（`G4`） |
| 只实例化有 `.cpp` 的那三个类型（今天的样子） | 纯头文件的类型在库里没有任何符号，"库提供了什么"取决于谁编译过（`G11`） |
| 加 `extern template` 让消费者完全依赖 DLL | 会打死短查询的内联，把实测收益退回去（`G10`） |
| 用一个 `#define` 切换全局精度（Bullet 式） | 一份二进制只能有一种精度，且切换会改变所有已编译目标的布局；两个实例化共存更便宜 |

## 后果

- `vnGeometry` 的符号表包含每个几何类型的 `float` 与 `double` 实例（用 `dumpbin /exports` 可验证，例如 `?$EllipseCurve@M@…` 与 `?$EllipseCurve@N@…`）。
- 含 `std::vector` / `std::shared_ptr` 成员的四个类型（`TrimmedCurve`、`OffsetCurve`、`BezierCurve`、`BSplineCurve`）在导出处会有 `C4251`（STL 成员无 dll-interface）。这是导出 STL 成员的既有局限，已在 `Instantiations.cpp` 里局部屏蔽并注明，不是缺陷。
- 新增一种几何类型的固定成本：`Instantiations.cpp` 里两行（若它有 `.cpp` 实现，则在自己的 `.cpp` 里两行）。
- 精度策略是显式的：解析几何的官方 API 是 `double`，`float` 同样是受支持的实例；跨精度不混算（`Aabb::intersectWith` 的 `requires is_same_v<T, U>` 就是这条的机械表达）。
