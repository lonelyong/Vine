# ADR-2026-11-05：平面圆弧是值类型，通用裁剪视图不并存

- 状态：已采纳
- 日期：2026-11-05
- 相关：`../design/geometry-planar.md`、`../design/geometry-kinds.md`

## 背景

“曲线的一段”在两个模块里现存的形状不一样：空间侧是 `TrimmedCurve<T>`——`std::shared_ptr<const Curve<T>>`
基曲线加两个参数界，是一个视图；平面侧是 `ArcCurve`——一个 `CircleCurve` 按值持有，加首末角度。

平面模块落地后，这个问题必须回答：要不要补一个通用的 `TrimmedCurve<T>`，让椭圆弧、样条修剪也能表达？

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A 只保留 `ArcCurve` | 零分配、域规则就地实现并有测试；模块的“值类型不持有内存”例外只剩样条一条 | 只有圆能被裁剪；接口形状与 3D 的 `TrimmedCurve` 不同 |
| B 用 `TrimmedCurve<CircleCurve>` 取代 `ArcCurve` | 与 3D 同形；将来任何基曲线都能裁剪 | 每次构造一次堆分配（类型擦除只能靠 `shared_ptr`）；现有 `ArcCurve` 的公开面被删掉 |
| C 两者并存 | 两种需求都满足 | “圆的一段”有两个类型、两套规则、两份测试——一个概念两个定义 |

## 决定

采用 A。`TrimmedCurve<T>` **不在** `ArcCurve` 旁边增加。当出现非圆基曲线的裁剪需求（椭圆弧、样条修剪）时，
按 B 落地：引入 `TrimmedCurve<T>` 并**删除** `ArcCurve`，不做 C。

判据是 `G1`（一个概念一个定义）与 `G10`（关注真实成本）：
“圆 + 区间”只有一种表达时，通用视图换不到任何新能力，却把一次堆分配和一条新的 R2 例外塞给每一个圆弧；
而 `ArcCurve` 已经把闭合、周期、反向、域外行为四条规则实现并测到了，这些规则在 3D 的裁剪视图里是
“基曲线 + 区间”的推论，在值类型里必须自己写——两种形状承载的是同一组事实，只是存法不同。

`G8` 也支持 A：今天没有任何消费者需要裁剪椭圆或样条，选项 B 的收益是假想的。

## 后果

- 好的：`ArcCurve` 零分配、大小固定，域规则（`isClosed` 要区间盖满圆的域、更短的段答 `period() == 0`）
  在它自己的文档里说清楚，不必让读者去推“基曲线 + 区间”；R2 的例外只有两条样条。
- 坏的 / 代价：平面侧没有 `TrimmedCurve<T>`，所以“把任意曲线裁一段”这件事今天写不出来；两侧同名概念
  的接口形状不同（2D 值类型 vs 3D 视图），移植代码时要认这一点。
- 触发条件（什么时候重开）：出现第一个需要裁剪非圆基曲线的消费者（椭圆弧、样条修剪、导入轮廓的截断）。
  届时按 B 做：新建 `TrimmedCurve<T>`，删掉 `ArcCurve`，并把 `geometry-planar.md` 的 2.8 改写成
  “裁剪是视图”的版本。

## 证据

- `src/base/geom2d/sdk/vine/geom2d/ArcCurve.hpp`：基圆按值持有，`isClosed` / `isPeriodic` / `period` /
  `reversedParameter` 四条规则都在类型上。
- `src/base/geom3d/sdk/vine/geom3d/TrimmedCurve.hpp`：`shared_ptr` 基曲线 + 区间，同一组事实的视图版本。
- `tests/test_geom2d/ArcCurveTest.cpp`：闭合、半圈、反向、区间外求值、退化基圆共 7 个用例。
- `../design/geometry-planar.md` 2.8、D6。
