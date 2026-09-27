# Robotics kinematics 模块要点

位置: `src/robotics/core/{sdk,src}/.../kinematics/`（ns `vn::robotics::kinematics`）。
测试: `tests/test_robotics_core/PieperIKSolverTest.cpp` 等；`SerialKinematics::setIKSolverType(IKSolverType::Pieper)`
以 `ik_dofs_`（已把固定变换累乘进关节 origin）构造求解器。

## PieperIKSolver 支持的机器人类（构造函数逐条校验，不满足即 `isValid()==false`）

- 恰好 6 个关节，且每个 `DofInfo::origin` 都可 MDH 表示（`tryMdhFromTransform` 成功）；
- 球腕：`a₃ = a₄ = a₅ = 0` **且 `d₅ = 0`**（`mdh_[3].a`、`mdh_[4].a`、`mdh_[5].a`、`mdh_[4].d`）——
  `d₅ = 0` 才使 frame4/frame5 原点重合，`solve()` 才能把 `tool − d₆·z` 当腕心；
- 臂：`|sin α₁| = 1`（`kEpsSin` 容差）、`sin α₂ = 0`（α₂ = 0 **或 π**）、`a₂ ≠ 0`、
  **`sin α₃ ≠ 0`**、**`d₄ ≠ 0`**（否则 θ₃ 不可由腕心位置观测 ⇒ 其实是 5-DOF）；
- 腕扭角：**`sin α₄ ≠ 0` 且 `sin α₅ ≠ 0`**（相邻腕关节轴平行/反平行 ⇒ 腕只剩 2 DOF，解析解不适定）。

> 这 4 条是 2026-09-27 用 512 组离散矩阵扫描补出来的：只有 `α₃/α₄/α₅ = ±90°`（以及任何
> `sin ≠ 0` 的角度，如 ±60°/±120°）+ `d₄ ≠ 0` 才能逐个复现生成解；其余组合是**静默零解或漏解**。
> 因为原 `solveWristDegenA4/A5`（α₄/α₅ ≈ 0/π 路径）对这些退化构型本来就从未跑通，已删除。

最多 8 解（2 肩 × 2 肘 × 2 腕）；不可达 / 越限 / FK 复核不过（pos、rot ≤ 1e-4）分支会被丢弃，故可能更少。

## 不可轻易改动的推导锚点

- 腕分解 `R_wrist = Rx(−α₃)·R₀₃ᵀ·R_target = Rz(θ₄)Rx(α₄)Rz(θ₅)Rx(α₅)Rz(θ₆)`；
  `c₅ = (cα₄cα₅ − R_wrist(2,2))/(sα₄sα₅)`。
- 臂在 frame1 里解：`p_w_des = T0_fixed⁻¹·(tool − d₆z) = Rz(q₁)·v`，`v` = 腕心在 frame1 的坐标。
  故 `arm_v = sinα₁·z_des`（**不要再减 d₁**：`T0_fixed` 已含 `Tz(d₁)`）。
- `α₂ = π` 只需把「z 向」分量乘 `ε = cos α₂`：`wp_z = d₂ + ε(d₃ + d₄cα₃)`、`wy ← ε·wy`、
  `Kc` 末项 `2ε·d₂(d₄cα₃+d₃)`；θ₃ 方程与 `Wy²` 对 ε 不变（ε²=1）。
- `solveArm` 里的 `pw_test` 必须与 `p_w_des` **同系**：`pw_test = T0_inv·(T0T1T2·p_w4)`。

## 2026-09-27 修复的缺陷（改动前 6 个新用例会红）

1. `is_valid_` 从不置 false（基类默认 `!dofs_.empty()`）⇒ `isValid()` 恒真；5-DOF 时 `solve()` 还会
   越界读 `dofs[5]`。现在构造开头置 false、末尾才置 true，`solve()` 再加 `dofs().size()!=6` 兜底。
2. `α₂ = π` 此前零解（臂推导按 α₂=0 展开）。
3. 基座非零 `a₀/d₁` 此前零解（`pw_test` 系不一致 + `arm_v` 重复减 `d₁`）。
4. 球腕判定漏 `d₅`，`d₅≠0` 时静默零解。

## 验证配方（改这个模块必须跑）

- **随机 FK→IK 往返**：查「生成解是否回来 + 每个解 FK 是否复现目标 + 解的个数 ≤ 8」。
- **独立数值 IK（阻尼最小二乘、多起点）枚举对照**：只查「至少有解」会漏掉整类失败；解析集合必须与数值集合逐个一致。
  实测 4 解的目标确实只有 4 个可达分支（不是漏解）。
- **合规构型矩阵测试**（`PieperCompliantConfigurations` / `PieperSingularConfigurations`，共 21 个用例）：
  穷举 α₁∈{±90}×α₂∈{0,π}×α₃∈{±90}×α₄∈{±90}×α₅∈{±90}×a₁∈{0,0.15}（64 组）+ 非 90° 扭角 + 逐条链路参数 + 关节钉死在特殊值。
- **奇异性要用弱 oracle**：`θ₅ = 0/π`（腕折叠）与「腕心落在基座 z 轴上（`r2 = 0`，肩奇异）」时**指令不可复现**
  （θ₄/θ₆ 或 θ₁ 不可观测）⇒ 只断言 soundness（每个解 FK 都复现目标），不要断言「生成解在解集里」。
  实测 `SpecialJointConfigurations` 的失败样本**全部** `r2 = 0`。
- 变异反证：把 4 条新校验、`is_valid_`、`pw_test` 同系、`arm_v` 去掉 `d₁` 等逐个撤回，确认对应用例变红
  （已验：撤回新校验 ⇒ `UnsupportedRobotsAreRejected` 红）。

## 迭代接口的旋钮（借鉴 RobWork `invkin`，2026-09-27）

- `IKSolver::setCheckJointLimits(bool)` / `isCheckingJointLimits()`（默认 true）：显式开关。关掉后
  `PieperIKSolver` 不再拒绝越限解、也不再 2π 折算（直接给原始解，仍过 FK 闸门）；`JacobianIKSolver`
  关掉后可在限位外收敛（需同时 `setClampToBounds(false)`）。
- `IterativeIKSolver` 的 `maxError()`（默认 1e-6，**每个启用任务分量**的容差，不是范数）、
  `maxIterations()`（150）、`maxSeeds()`（8）、`isClampToBounds()`（true）、
  `constraintMask()`（全 true）。默认值 = 原硬编码值，行为不变。
- `constraintMask()` 对应 RobWork `Target::enabled[6]`：0-2 = x/y/z，3-5 = 旋转向量分量；关闭的分量在误差
  向量与雅可比列里都置零 ⇒ 支持"只对位置/只对姿态"的 IK。
- `KinematicsBase::ikSolver()` 补了非 const 重载，便于经 workcell 路径调这些旋钮。
- 未搬（有意）：`IKMetaSolver`/`AmbiguityResolver` 分层、via-point 插值、`JacobianSolverType`；
  以及 Craig Case2 / 一般 Case3（`|sinα₁|≠1`）—— 若要扩覆盖面须自写数值，勿照抄 RobWork（其 6 条 P0 见对话记录）。
- **求解器选择定案：只留一种** = `IKSolverType` 枚举 + 内部工厂（`SerialKinematics::setIKSolverType`）。
  **刻意不做**注入口（RobWork 式"调用方持有求解器"）也不做工厂注册表 —— 否则"类型"与"实例"成为两个真相源，
  会出现"设了类型却没生效"。配套：`KinematicsBase::setIKSolverType` 里 `ik_solver_.reset()`，保证
  **实例恒与上报类型一致**（不覆写该虚函数的子类会得到 null，而不是拿到类型不符的旧求解器）；
  调参走 `ikSolver()`（const 只读 / non-const 设参），它不改变"谁来选求解器"这件事。
  钉子：`SerialKinematicsTest.SolverInstanceAlwaysMatchesReportedType`（把 Pieper 分支改成造 Jacobian ⇒ 红）。
