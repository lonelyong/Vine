# math 模块：底座层与数学词汇

> **状态**：现行
> **最后核对**：2026-11-06
> **范围**：`src/base/math` —— 值类型（向量、点、矩阵、四元数、等距、矩形）、数学常量与谓词、概念与标记、3×3 对称特征分解。**不含任何过程**（求根、积分、样条求值都在 `geom/detail`），不含任何几何词汇或表示约定。
> **权威来源**：`src/base/math/sdk/vine/math/` 下的 18 个头文件；`src/base/math/src/` 的 14 个显式实例化单元；`tests/test_math/`（7 个用例文件，36 例）。决策见 `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`。
> **待办**：无

## 1. 概要设计

### 1.1 问题与背景

`math` 是仓库里**唯一被所有模块依赖**的层：图形要 `Vec3d` / `Mat4d` / `Point3d`，机器人要 `Vec3d` / `Quatd`，几何要 `Point2` / `Rect2`，模型与加载器要 `Vec3f`，核心要 `Vec3f` 当顶点元素类型。

这个模块一度还装着求根、积分、B 样条节点模型与光滑度阶梯。2026-11-06 一天之内它们全部移入几何模块（`geom/detail/` 与 `geom/Continuity.hpp`）：**`math` 现在只有数学词汇，没有一个子目录。** 移动的两条理由不同——节点模型与 `Continuity` 是因为“签名里带着我们自己的数据模型”，求根/积分/`deCasteljau` 是因为“今天唯一的消费者是几何”，后果与反转条件写在 `ADR-2026-11-06-math-keeps-only-vocabulary.md`。

### 1.2 目标

1. 让任何模块都能用同一套值类型，且**只向下依赖**（`G2`）。
2. 让“数学词汇”与“数值过程”分属两层，判据可执行，新名字放错地方能被一眼看出（`G1`）。
3. 让精度（`float` / `double`）与存储顺序（列优先 / 行优先）是类型参数而不是运行时标志，代价为零（`G10`）。

### 1.3 非目标

- 不做通用数值库：不提供线性方程组求解、稀疏矩阵、复数域、优化器、积分器、求根器。后两者在 `geom/detail`。
- 不做几何：曲线、曲面、包围盒、表示的约定都不在这里。
- 不做单位制、不做区间算术、不做任意精度。

### 1.4 结论

**`math` 是底座层，内容只有“数学词汇”：值类型、常量与谓词、概念与标记，外加一个预算内部固定的特征分解。判据是“有没有调用方给的精度目标”——有，那它就不是词汇，归 `geom/detail`。**

## 2. 设计要点

### 2.1 底座层：只依赖 `global` 与 `core`，任何模块都可依赖它

- **是什么**：`vn::Math` 是 SHARED 库（`src/` 有实例化单元 → 定义 `VN_MATH_LIB`，实体标 `VN_MATH_API`），只 `PUBLIC` 链 `vn::Global`。Release 开 LTO；Clang 下额外打开内联优化提示。
- **为什么**：`G2` —— 上层模块（图形、几何、机器人、模型、加载器）都要它，它不能反向依赖任何一个；一条回边就会让“谁都能用”变成“谁都被拖进来”。
- **边界**：`math` 不认识曲线、不认识渲染、不认识 IO，也不认识 CAD 的表示约定。需要曲线速度才能做的换算（几何容差 → 参数容差）因此留在几何侧。

### 2.2 只有词汇，没有过程

| 类别 | 判据 | 内容 |
|---|---|---|
| 数学**词汇** | 给定数就算出数，没有“收敛到某个精度”的说法 | `Vector2/3/4`、`VectorN`、`Point2/3`、`Matrix3x3/4x4`、`Quaternion`、`Isometry2/3`、`Rect2`、`Math.hpp` 的常量与谓词、`Types.hpp` 的概念与标记、`EigenDecomposition.hpp` |

`math` 里没有第二个类别。数值过程（`adaptiveSimpson` / `rootInBracket` / `parameterReaching`）与样条求值（`deCasteljau` / `differences`）在 `geom/detail`，公开入口是 `geom::planar::arcLength` / `geom::spatial::arcLength` 等几何名字。

- **判据要能拒绝**：签名里有 `tolerance` 不等于它是过程——`Vector3::isEqual(b, eps)` 的 `eps` 是**比较容差**，答案立刻给出；过程指的是“我尽力逼近你给的目标，可能没做到”。
- **为什么不留一层“通用数值”**：三个函数不构成模块（`G8`），而它们的消费者今天只有几何。代价明确：任何非几何模块要用积分或求根，只能依赖 `vn::Geom`、复制一份、或把内核升成独立模块——后者是记账的反转路径（`../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`）。

### 2.3 为什么 `math` 里没有几何词汇，也没有 `detail`

- **几何词汇归几何**：B 样条的节点模型（`knotSpan` / `isClampedKnotVector` / `knotContinuity` / `basisValue`）的签名里带着我们自己的数据模型；`Continuity` 出现在 `Curve<T>` / `Surface<T>` 的纯虚签名里，唯一消费者是几何。它们现在住在 `geom/detail/BSplineBasis.hpp` 与 `geom/Continuity.hpp`（`G1`、`G2`）。判据是**“签名里有没有我们自己的数据模型”**。
- **`math` 没有 `detail`**：`detail` 回答“这是不是公开接口”，而 `math` 里每个头都是公开词汇，既没有“非公开的共享实现”，也没有第二个内容类别需要区分。要建 `math/detail` 的触发条件是“两个以上 `math` 头需要同一段不该外露的实现”，现在没有（`G8`）。
- **子命名空间也一个都不剩**：`numeric` 与 `spline` 随内容一起走了，`math_global.hpp` 只提供根层命名空间宏。

### 2.4 精度策略：模板 + 显式实例化 + 别名

- **是什么**：每个值类型是 `template <typename T>`（浮点型另受 `vn::math::FP` 约束）；`src/*.cpp` 用 `template class VN_MATH_API X<float>;` 的形式显式实例化，`Vector2/3/4`、`Point2/3` 还对整数类型实例化（整数向量是索引/像素坐标的真实需求，`Aabbi` 就是它）。
- **为什么**：`G10` —— 头里定义 + 出 DLL 处显式实例化，让算术能内联、又只有一个符号副本；精度是类型参数意味着没有运行时分支，也不存在“float 版本忘了同步”。
- **边界**：`T` 不是任意类型。整数实例只给向量/点，不给矩阵与四元数（它们的除法与开方在整数上无意义）；`VectorN` 接受浮点与整数（含 `bool`）。
- **别名规则**：`<Noun><dim><scalar>`——`Vec2f` / `Vec3d` / `Vec3ui32` / `Point3d` / `Point2ui16` / `Mat3f` / `Mat4d` / `Quatf` / `Quatd` / `Isometry3d` / `Rect2i` / `Rect2d`。注意 `Vector2` 的别名是 `Vec2*`，**没有** `Vector2d` 这种写法。

### 2.5 存储顺序是类型参数，不是运行时开关

- **是什么**：`Matrix3x3<T, Order>` / `Matrix4x4<T, Order>` 的 `Order` 默认为 `ColMajor`，另一个是 `RowMajor`；`Types.hpp` 里的两个空结构体就是这两个标记。矩阵按**列向量**约定作用于向量（`y = M * x`），平移在第 4 列；几何变换（平移、旋转、缩放）是 `Transform3.hpp` 的**自由函数**，不是成员。
- **为什么**：`G4`、`G10` —— 布局决定 `rawData()` 怎么解释，把它做成运行时标志会让每个乘法都多一次分支；做成类型参数则两种布局都能被内联、且不能悄悄互传（同一个 `T` 的 `ColMajor` 矩阵与 `RowMajor` 矩阵是不同类型）。
- **边界**：`Order` 只影响存储与原始数据构造的解释，不改变数学语义；与第三方库交换数据时必须在调用点确认对方的顺序（`loaders` 侧把 assimp 的列优先矩阵直接对应，不转置）。

### 2.6 `symmetricEigen` 是词汇里唯一的迭代，而它仍算词汇

- **是什么**：`symmetricEigen(matrix, eigenvalues, eigenvectors?)` 用循环 Jacobi 求 3×3 实对称矩阵的特征分解：特征值升序、特征向量列对应、每个特征向量的最大分量取正以消除符号任意性、只读上三角；sweep 预算固定，返回 `bool` 表示是否收敛。
- **为什么留在这里**：判据是**谁给精度目标**。这里调用方不给：预算内部固定，相对容差是 `4 * EPS<T>()`。所以它不是“我尽力逼近你给的目标”，而是“我按固定代价算到底并告诉你结果如何”（`G4`）。它**能**答“没达到”（`false`），但那只说明判据的前半句才是决定性的。
- **边界**：不保证特征基的定向（要旋转矩阵时由调用方叉乘重建第三轴）；重特征值时基不唯一。

### 2.7 退化输入给可检查的值，不抛异常

- **是什么**：`isZero` / `isEqual` 显式处理 `NaN` 与 `inf`（`EPS` 为 `inf` 时任何有限值都算“零/相等”，为 `NaN` 时退化为精确比较）；`EPS<T>()` 对 `float` / `double` 有特化，其它 `T` 答 `0`。
- **为什么**：`G11`、`G4` —— `math` 在最底层，抛异常会把“一个谓词”变成控制流；返回 `NaN` 则让错误无声扩散到调用方最后才被发现。`isZero(NaN, eps) == false` 是刻意的：一个 `NaN` 不是零。
- **边界**：`EPS<T>()` 对整数答 0 意味着整数上的“零”是精确比较——这是对的，整数没有舍入。

## 3. 功能点

| 编号 | 功能点 | 内容 | 状态 |
|---|---|---|---|
| F1 | 值类型 | 向量、点、矩阵、四元数、等距、矩形、N 维向量 | 已落地 |
| F2 | 常量与谓词 | π 与自然对数常量、`EPS<T>()`、角度归一化、容差比较、整数安全的乘加与开方 | 已落地 |
| F3 | 概念与标记 | `FP` / `Real` / `Arithmetic` / `Integer` / `Integral` / `TypeF`、`ColMajor` / `RowMajor` | 已落地 |
| F4 | 特征分解 | 3×3 实对称循环 Jacobi | 已落地 |

### 3.1 F1 值类型

- **行为**：`Vector2/3/4`、`Point2/3`、`Matrix3x3/4x4`、`Quaternion`、`Isometry2/3`、`Rect2`、`VectorN` 为 `constexpr` 友好的值类型；`Isometry3` 是平移 + 四元数旋转的刚体变换，`Transform3.hpp` 提供 `translate` / 旋转 / 缩放等返回矩阵的自由函数。
- **约束**（C2、C3）：浮点类型受 `FP` 约束；矩阵的存储顺序是类型参数、列向量约定；几何变换是自由函数而不是成员（`G1`：矩阵不知道“变换”这个用途）。
- **验收**（T1）：`VectorTest`、`Matrix3x3Test`、`Matrix4x4Test`、`QuaternionTest`、`Rect2Test`、`Transform3Test`。

### 3.2 F2 常量与谓词

- **行为**：`PI` / `PI_TWO` / `PI_HALF` / `PI_QUARTER`、`E` / `LOG2E` / `LOG10E` / `LN2` / `LN10`、`RAD_TO_DEG` / `DEG_TO_RAD`、`EPS<T>()`、`isZero` / `isEqual`、`normalizeAngle`（`[-π, π]`）/ `normalizeAnglePositive`（`[0, 2π)`）、`safeMultiply` / `safeLength` / `safeLengthSquared`（整数先提升到 `double`）、`arithmeticAdd/Sub/Multiply/Division/Nagate`（`bool` 按逻辑运算解释）。
- **约束**（C4）：`isZero` / `isEqual` 必须显式处理 `NaN` / `inf`，不得让它们静默通过。
- **验收**（T2）：由 `VectorTest` / `Rect2Test` 的谓词用例与几何侧的 `isValid()` 用例共同覆盖。

### 3.3 F4 特征分解

- **行为**：见 2.6。
- **验收**（T4）：`EigenDecompositionTest`。

## 4. 公开接口

| 头文件 | 公开内容 |
|---|---|
| `Vector2.hpp` / `Vector3.hpp` / `Vector4.hpp` / `VectorN.hpp` | `Vector2/3/4<T>`、`VectorN<T, N>`、`Vec2f` / `Vec3d` / `Vec3ui32` 等别名 |
| `Point2.hpp` / `Point3.hpp` | `Point2/3<T>`、`Point2d` / `Point3f` / `Point2ui16` 等别名 |
| `Matrix3x3.hpp` / `Matrix4x4.hpp` | `Matrix3x3<T, Order>` / `Matrix4x4<T, Order>`、`Mat3f` / `Mat4d` |
| `Quaternion.hpp` / `Isometry2.hpp` / `Isometry3.hpp` | `Quaternion<T>`、`Isometry2/3<T>`、`Quatf` / `Quatd` / `Isometry3d` |
| `Rect2.hpp` / `Transform2.hpp` / `Transform3.hpp` | `Rect2<T>`（`Rect2i/f/d`）与变换构造函数（返回矩阵） |
| `Math.hpp` | 常量、`EPS<T>()`、谓词、整数安全算术、角度归一化 |
| `Types.hpp` | 概念（`FP` / `Real` / `Arithmetic` / …）、`TypeF<T>`、`ColMajor` / `RowMajor` |
| `EigenDecomposition.hpp` | `symmetricEigen()` |
| `math_global.hpp` | `VN_MATH_API`、`VN_MATH_NS_BEGIN` / `VN_MATH_NS_END` |

## 5. 约束、要求与不变量

- **C1**：依赖方向只有“上层 → `math`”。`math` 的公开头只 include 本模块头与标准库，不 include 任何其它 `vine` 模块。
- **C2**：精度是模板参数；浮点类型受 `FP` 约束；整数实例只覆盖向量与点。
- **C3**：矩阵存储顺序是类型参数、默认 `ColMajor`，按列向量约定 `y = M * x`；几何变换是自由函数。
- **C4**：退化/非有限输入给可检查的值（`NaN` 感知的谓词、`bool`），不抛异常、不静默产生 `NaN`。
- **C5**：`math` 只装词汇。带调用方精度目标的函数、以及任何带我们自己的数据模型的函数，都不在这里。
- **C6**：`math_global.hpp` 只提供根层命名空间的宏；模块内没有子命名空间。
- **C7**：公开别名是 `<Noun><dim><scalar>`；`Vector2` 的别名是 `Vec2*`。
- **R1**：值类型默认不分配内存（`VectorN` 是定长数组）。
- **N1**：`EPS<T>()` 只对 `float` / `double` 有特化，其它 `T` 答 `0`。
- **N2**：`normalizeAngle` 落在 `[-π, π]`，`normalizeAnglePositive` 落在 `[0, 2π)`。
- **N3**：`symmetricEigen` 特征值升序、只用上三角、特征向量符号规范化；`false` 表示“未在固定预算内收敛”，返回的基是近似值。

## 6. 注意事项与陷阱

- **P1 谓词对 `NaN` 的答案不是直觉**：`isZero(NaN, eps)` 是 `false`；`eps` 为 `inf` 时任何有限值都算零/相等。调用方要“`NaN` 算作无效”，就用 `isValid()` 这类显式判据，不要指望谓词。
- **P2 别名别猜**：`Vector2` 的浮点别名是 `Vec2f` / `Vec2d`，**没有** `Vector2d`；`Vector3` 同理。写错会在编译期报“未声明的标识符”，但在跨模块 review 里不容易发现。
- **P3 `Point3::length()` 不存在**：两点距离写 `(a - b).length()`，单点没有“长度”这个语义。
- **P4 `Isometry2` / `Isometry3` 的 `T` 只接受 `float` / `double`**：刚体变换用整数没有意义，模板约束在类定义里就挡住了；向量/点则接受整数。
- **P5 需要积分、求根或样条求值时不要来 `math` 找**：它们在 `geom/detail`（第三个非几何消费者出现时才升独立模块，见 `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`）。

## 7. 已否决选项

- **D1** 保留一层通用数值（`math/numeric` 或独立 `vn::Numeric` 模块）：`G8`、`G5`——今天唯一的消费者是几何，为三个函数开一层是空壳；独立模块的触发条件写在 ADR 里（第二个消费者出现），到那时再拆比现在预建便宜。
- **D2** 把 `spline/` 升成独立模块 `vn::Nurbs`：`G8`——只有一个“表示约定”家族时它是空壳；且该家族已随 `geom/detail` 落到几何侧。
- **D3** 让 `math` 继续当几何词汇与表示约定的家：`G1`、`G2`——`math` 会被迫用一篇文档为名字辩护（`ADR-2026-11-06-math-is-the-base-layer.md` 当时就是这么做的），而它的消费者只有几何。已推翻，见 `../decisions/ADR-2026-11-06-spline-model-moves-to-geom.md` 与 `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`。
- **D4** 往 `math` 里加 `detail` 子树：`G8`——没有“非公开的共享实现”可放，见 2.3。
- **D5** 按类型族再分子目录（`vector/`、`matrix/`、`transform/`）：`G8`、`G4`——根层 18 个头按名字已经能找，再分一层会让 `Point3` / `Vector3` / `Matrix3x3` 这类必须一起用的类型散在三个路径里。

## 8. 测试与验收

| 编号 | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | 值类型的算术、比较、变换语义 | `test_math`：`VectorTest`、`Matrix3x3Test`、`Matrix4x4Test`、`QuaternionTest`、`Rect2Test`、`Transform3Test` |
| T2 | 常量与谓词的边界（`NaN` / `inf` / 方向） | 同上，加几何侧 `isValid()` 用例 |
| T4 | 特征值升序、特征向量正交与符号规范化、重特征值 | `test_math`：`EigenDecompositionTest` |
| T5 | 全量 | `build\bin\Debug\test_math.exe --gtest_brief=1`（7 套 36 例） |

积分、求根与样条求值的用例随实现迁到了 `test_geom_spatial`（`QuadratureTest`、`RootFindingTest`、`BSplineBasisTest`）。

## 9. 状态与演进

- **现状**：F1–F4 全部落地；`test_math` 36 例全绿。依赖它的模块：图形、几何、机器人、模型、加载器、核心（`Vec3f` 作为顶点元素类型）。
- **2026-11-06 的三次收缩**：① `Continuity` 与 B 样条节点模型移入 `geom`；② 求根 / 积分 / Bernstein 求值移入 `geom/detail`，`math` 至此只剩词汇；③ 删掉只有 BOM 的占位头 `EulerAngles.hpp`（零内容、零引用）。 <!-- drift-ok -->
- **已知偏差**：根层的数学常量沿用 `PI` / `PI_TWO` / `EPSF` 这类全大写名，与仓库“常量用 `k` 前缀”的约定不一致（历史遗留，未改名：它们出现在大量调用点，改名收益低于风险）。新增常量按 `k` 规则写。
- **待办**：无。若将来出现非几何的求根/积分/Bernstein 消费者，按 `../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md` 的反转路径升独立模块。

## 10. 参考

- 代码：`src/base/math/sdk/vine/math/`（18 个头，无子目录）、`src/base/math/src/`（14 个实例化单元）、`tests/test_math/`（7 个用例文件，36 例）。
- 决策：`../decisions/ADR-2026-11-06-math-keeps-only-vocabulary.md`、`../decisions/ADR-2026-11-06-spline-model-moves-to-geom.md`、`../decisions/ADR-2026-11-06-math-is-the-base-layer.md`（已取代）、`../decisions/ADR-2026-11-06-shared-numeric-kernels-in-math.md`（已取代）。
- 相关设计：`geometry-module.md`（几何侧的目录、命名与共享子树）、`geometry-intersection.md` 2.7（求根器为何在几何侧）、`geometry-primitives.md` 2.7（`symmetricEigen` 为何不进几何）。
- 陷阱复盘：`../bugs/spline-knotspan-hang.md`（`knotSpan` 死循环，现已随头文件移入几何）。
