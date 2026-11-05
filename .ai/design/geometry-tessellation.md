# geometry 解析实体的三角化

> **状态**：现行
> **最后核对**：2026-11-05
> **范围**：把 `Solid<float>`（五个解析体）变成 `TriangleMesh` 的入口、质量参数与结果契约。
> 不含值类型本身与它的解析查询（见 `geometry-solid.md`）、模型的建模契约（见 `geometry-parametric.md`）、B-rep 的三角化（见 `brep-operations.md`）、网格存储与渲染（见 `graphics-geometry.md`）。
> **权威来源**：`src/base/models/sdk/vine/models/SolidTessellation.hpp`、`src/base/models/src/SolidTessellation.cpp`；`tests/test_geom3d/SolidTessellationTest.cpp`。
> **待办**：`../backlog/geometry.md` G5

## 1. 概要设计

### 1.1 问题与背景

`Primitive` 是建模类型：它带身份、名字、revision 与尺寸，却不含任何顶点数据（`geometry-parametric.md` 2.4）。渲染边界 `graphics::geometryFromShape()` 只接受网格，所以解析实体在今天**画不出来**；B-rep 的 `Tessellation` 能把 `BrepShape` 变成网格，但那条路要求 OCCT 后端，而解析实体的几何是纯数学。

三角化需要两样值类型给不了的东西：一次铺网格的遍历，和 float 精度的 `TriangleMesh`。前者可以放在自由函数里，后者决定了入口只对 `Solid<float>` 成立。

### 1.2 目标

- 一个自由函数，把五个解析体变成带位置、法向与索引的 `TriangleMesh`，不依赖 OCCT。
- 结果在实体自己的 canonical 局部空间里、闭合、外向绕序，可直接送渲染或碰撞。
- 细分程度是显式参数，可预测、可测试。

### 1.3 非目标

- 不做自适应细分 / 曲率误差控制：今天没有按误差取网格的消费者（`G8`），固定段数足够且可预测。
- 不做纹理坐标：解析实体没有“选哪个参数化”的答案，需要的调用方可以从位置推导。
- 不做双精度网格：`TriangleMesh` 是 float，`Solidd` 要出网格必须先转成 `Solidf`（9.2）。
- 不做 B-rep 三角化：那是 `brep` 的职责（`brep-operations.md`）。

### 1.4 一句话结论

**一个无状态自由函数在 `Solid<float>` 上按 kind 解析地铺网格：盒是六个四边形，球 / 椭球是纬带加两极扇，圆柱是侧带加两盖，圆锥是侧扇加底盖；质量是调用方给的固定段数。**

## 2. 设计要点

### 2.1 自由函数，不是 `Solid<T>` 的成员

- **是什么**：`tessellateSolid(const Solid<float>&, const TessellationQuality&)` 在 `geometry` 的 `src/` 里实现，用 `std::visit` 分派到五个解析铺法。
- **为什么**：`G4`、`G10` —— 输出 `TriangleMesh` 只有 float，把 `tessellate()` 挂到 `Solid<T>` 上会让 `Solid<double>::tessellate()` 变成一次隐式降精度，等于把一个精度陷阱写进接口；自由函数只声明它要的那一种精度，也把 250 行铺网格代码留在 `.cpp` 里，而不是给每个包含者编译一遍。
- **边界**：算法挂在**几何一侧**（不是模型上），与 `closestPoint()` / `intersectWith()` 同一侧；模型只通过 `shape()` 参与。

### 2.2 质量是固定段数

- **是什么**：`TessellationQuality{ around, along }`：`around` 是旋转面的方位角段数，`along` 是球 / 椭球的纬度段数；盒完全忽略两者。`around` 钳到 ≥3、`along` 钳到 ≥2。
- **为什么**：`G8`、`G4` —— 固定段数没有隐藏的误差判据，调用方知道会得到多少三角形；钳制让“0 段”不是一个退化网格而是一个最小合法形状。
- **边界**：不做自适应、不做屏幕空间误差——需要时由调用方根据尺寸自己算段数。

### 2.3 逐顶点精确法向，顶点因此在必要时复制

- **是什么**：每个顶点带解析法向——盒是面法向，球 / 椭球是 `(x/a², y/b², z/c²)` 归一化，圆柱是侧面径向与盖面轴向，圆锥是斜侧面法向。位置相同但法向不同的地方（盒棱、圆柱侧与盖之间、圆锥顶点按段）各存一个顶点。
- **为什么**：`G4`、`G10` —— 共享一个顶点就只能选一个法向，或者平滑掉硬边；法向是这批网格的主要用途（光照），精确法向比最小顶点集更值钱。索引网格仍然省下“每个三角形一套顶点”。
- **代价**：顶点数不是理论最小；盒是 24 而不是 8，圆柱的盖环与侧环各一份。

### 2.4 闭合实体与外向绕序

- **是什么**：五个解析体都是实体，网格是闭合的；三角形从外面看是逆时针（右手法则），因此可以直接背面剔除。
- **为什么**：`G4`、`G12` —— 绕序就是法向的另一个说法；闭合让“内部 / 外部”有定义，也是碰撞与法向校验的前提。
- **边界**：圆锥顶点按段复制，每个顶点取该段的斜法向；顶点处法向本就不唯一，这是唯一的正确处理。

### 2.5 只写位置、法向与索引

- **是什么**：`TriangleMesh` 的位置 / 法向 / 索引三个通道；纹理坐标与颜色不写。
- **为什么**：`G8` —— `Mesh` 的通道全部可选，渲染边界对缺失通道有既定行为；解析实体没有纹理参数化的唯一答案，写一个就是替调用方做决定。

### 2.6 不依赖 OCCT、不缓存

- **是什么**：每次调用构造一个新网格；`graphics` 侧的几何不会被共享或记住。
- **为什么**：`G2`、`G10` —— `geometry` 不 include OCCT；缓存需要失效规则（模型尺寸可被 setter 改、revision 会变），而今天的调用频率不值得先引一套缓存。值类型没有 revision，所以缓存只能由调用方按自己的失效规则做。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 五个解析体的三角化 | `tessellateSolid(Solid<float>, quality)`：按 kind 解析铺网格，返回索引三角网格 | 已落地（T1–T5） |
| F2 | 质量与钳制 | `around ≥ 3`、`along ≥ 2`；盒忽略两者 | 已落地（T5） |
| F3 | 不可用实体 | `isValid()` 为假返回 `nullptr`，不抛异常 | 已落地（T6） |
| F4 | 自适应细分 / 纹理坐标 | 误差驱动的段数与 UV 通道 | 未做（`G8`） |

### 3.2 F1 铺网格

- **行为**：按 kind 分派到盒的六个四边形、球 / 椭球的纬带 + 两极扇、圆柱的侧带 + 两盖扇、圆锥的侧扇 + 底盖扇。
- **约束**（C1–C3）：空间、闭合与法向见 §5。
- **验收**（T1–T4）：每个 kind 的顶点数、三角形数、点在解析面上的判据。

### 3.3 F3 不可用输入

- **行为**：`isValid()` 为假的实体返回空指针；钳制后的最小质量仍产出闭合网格。
- **要求**（R2）。
- **验收**（T5、T6）。

## 4. 公开接口

| 头文件 | 内容 |
|---|---|
| `models/SolidTessellation.hpp` | `TessellationQuality` 与 `tessellateSolid(const Solid<float>&, const TessellationQuality&)` |

完整签名与每个参数的语义见头文件；网格通道的存储与共享见 `graphics-geometry.md`。

## 5. 约束、要求与不变量

- **C1**：结果在实体的 canonical 局部空间里（盒 / 球 / 椭球居中，圆柱 / 圆锥沿 +Z 跨 `[-half_height, +half_height]`）。
- **C2**：网格闭合——每条几何边恰属两个三角形（位置相同即同一条边，即使顶点因法向被复制）。
- **C3**：法向是单位向量，且对凸的解析体指向背离内部一侧；三角形绕序与法向一致。
- **C4**：`around` / `along` 在入口处钳制，非法值不会产生退化网格或越界索引。
- **R1**：不依赖 OCCT，不分配除结果网格以外的持久状态。
- **R2**：不可用实体返回空指针，不抛异常。
- **N1**：盒的三角形数固定 12；球 / 椭球是 `2 · around · (along − 1)`；圆柱是 `4 · around`；圆锥是 `2 · around`。
- **N2**：圆锥顶点按方位段复制，每个顶点带该段中点的斜法向。

## 6. 注意事项与陷阱

- **P1 顶点重复不是缺陷**。会怎么错：拿“顶点数应等于理论最小”去校验网格，于是认为盒的 24 个顶点错了。怎么判：按位置去重后只剩 8 个角。怎么防：C2 的闭合判据按**位置**数边，不按索引数。
- **P2 段数不随尺寸自适应**。会怎么错：一个细长的大圆柱用默认段数，侧面出现可见折线。怎么判：把 `around` 加倍后轮廓改善。怎么防：按目标尺寸与段数的比值选 `around`。
- **P3 圆锥顶点的法向不是单一值**。会怎么错：认为顶点处有一个“真正”的法向，于是把按段复制的顶点合并。怎么判：合并后侧面光照出现一圈错误的高光。怎么防：保留每段的顶点（N2）。
- **P4 局部空间不是世界空间**。会怎么错：把网格顶点当世界坐标送碰撞，忽略节点变换。怎么判：形状位置与场景不符。怎么防：C1；世界盒要乘节点变换。
- **P5 双精度实体出不了网格**。会怎么错：从 `Solidd` 直接要网格，于是随手 `static_cast` 量纲。怎么判：`tessellateSolid` 不接受 `Solidd`，编译不过。怎么防：接受一次显式转换（9.2 的缺口），别在调用点上散落 cast。

## 7. 已否决选项

- **D1** 把三角化放进 `graphics`：渲染需要网格，但碰撞、包围盒核对与导出也会要；放进渲染层会让 `base` 的消费者反向依赖 `viz`（`G2`）。
- **D2** 在 `Primitive` 上加虚函数 `tessellate()`：建模类型被拉上算法面，每个子类多一份虚函数，而算法本身与身份 / revision 无关（`G1`、`G8`）。见 `../decisions/ADR-2026-11-05-solid-value-type.md`。
- **D3** 借 OCCT 的 `BRepPrimAPI` + `Tessellation` 出网格：`geometry` 从此依赖后端，无后端构建失去全部能力；解析实体是纯数学，不需要后端（`G2`、`G10`）。
- **D4** 自适应细分：没有按误差取网格的消费者，误差判据本身还需要曲率与屏幕尺度（`G8`）。
- **D5** 把它做成 `Solid<T>` 的成员：`TriangleMesh` 只有 float，成员化的结果是 `Solid<double>` 隐式降精度（`G4`）。见 `geometry-solid.md` D4。

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 盒：六面、canonical 包围盒、法向 | `tests/test_geom3d/SolidTessellationTest.cpp` 的 `aBoxBecomesSixClosedQuads` |
| T2 | 球 / 椭球：点集在解析面上、法向 | 同文件 `aSphereBecomesAClosedBandBetweenItsPoles`、`anEllipsoidUsesItsThreeSemiAxes` |
| T3 | 圆柱：侧带与两盖、法向分类 | 同文件 `aCylinderHasASideBandAndTwoCaps` |
| T4 | 圆锥：斜法向与底盖 | 同文件 `aConeHasASlantedSideAndABase` |
| T5 | 质量钳制与闭合 | 同文件 `theQualityIsClampedInsteadOfTrusted` 与各用例里的 `expectClosed`（每条几何边恰属两个三角形） |
| T6 | 不可用实体 | 同文件 `anUnusableSolidAnswersNull` |
| T7 | 模型经转换出网格 | 同文件 `aModelReachesTheMeshThroughTheConversion` |

## 9. 状态与演进

### 9.1 当前状态

- 五个解析体的三角化已落地，`geometry` 仍不依赖 OCCT；`brep` 的 B-rep 三角化没有变化。
- 入口以 `SolidShapef`（五个 kind 的 variant）为参数；模型侧用 `solidShapeOf(primitive)` 把 `Primitive` 转成它（`geometry-parametric.md` 4）。

### 9.2 已知缺口 / 待办

- 自适应细分与纹理坐标不做（F4，`G8`）。
- 双精度网格不存在：`TriangleMesh` 是 float，`Solidd` 要出网格必须自己构造 `Solidf`（P5）。
- 与 `graphics` 的接缝还没接上：`geometryFromShape()` 目前不调用本入口，接线要在 `graphics` 侧做（`../backlog/geometry.md` 的相关条目）。

### 9.3 兼容性 / 迁移

- 纯增量：一个头文件、一个结构体、一个自由函数和一个 `.cpp`；`Primitive` 与既有入口不变。
- 旧的以模型为参数的入口已删除，替代是 `tessellateSolid(x.shape())`。

## 10. 参考

- `geometry-solid.md`（族接口与 variant、canonical 局部空间）
- `geometry-parametric.md`（建模类型与 canonical 空间的来源）
- `graphics-geometry.md`（网格通道、`geometryFromShape` 的边界）
- `brep-operations.md` 1.3（为什么“实体 → 网格”归 `geometry` 不归 `brep`）
- `../backlog/geometry.md` G5
