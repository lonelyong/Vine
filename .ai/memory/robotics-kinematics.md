# Robotics kinematics notes

Location: `src/robotics/core/{sdk,src}/.../kinematics/` (namespace `vn::robotics::kinematics`).
Tests: `tests/test_robotics_core/PieperIKSolverTest.cpp` and friends;
`SerialKinematics::setIKSolverType(IKSolverType::Pieper)` builds the solver from `ik_dofs_` (which
already folds the fixed transforms into the joint origins).

## Robot classes `PieperIKSolver` supports (checked one by one in the constructor; failing any means
`isValid()==false`)

- Exactly 6 joints, and every `DofInfo::origin` is MDH-representable (`tryMdhFromTransform` succeeds).
- Spherical wrist: `a₃ = a₄ = a₅ = 0` **and `d₅ = 0`** (`mdh_[3].a`, `mdh_[4].a`, `mdh_[5].a`,
  `mdh_[4].d`) - only `d₅ = 0` makes the frame4/frame5 origins coincide, which is what lets `solve()`
  treat `tool − d₆·z` as the wrist centre.
- Arm: `|sin α₁| = 1` (tolerance `kEpsSin`), `sin α₂ = 0` (α₂ = 0 **or π**), `a₂ ≠ 0`,
  **`sin α₃ ≠ 0`**, **`d₄ ≠ 0`** (otherwise θ₃ cannot be observed from the wrist-centre position, so the
  robot is really 5-DOF).
- Wrist twist: **`sin α₄ ≠ 0` and `sin α₅ ≠ 0`** (adjacent wrist axes parallel/antiparallel leaves the
  wrist with 2 DOF and makes the closed form ill-posed).

> These four checks were added on 2026-09-27 by scanning 512 discrete matrices: only `α₃/α₄/α₅ = ±90°`
> (and any angle with `sin ≠ 0`, such as ±60°/±120°) plus `d₄ ≠ 0` reproduces the generating solution
> for each case; every other combination is **silently zero solutions or a lost branch**. The old
> `solveWristDegenA4/A5` (the α₄/α₅ ≈ 0/π path) never worked for those degenerate shapes anyway, so it
> was deleted.

At most 8 solutions (2 shoulder × 2 elbow × 2 wrist); unreachable / out-of-limit / FK-rejected branches
(position and rotation ≤ 1e-4) are dropped, so there can be fewer.

## Derivation anchors that must not be changed casually

- Wrist decomposition `R_wrist = Rx(−α₃)·R₀₃ᵀ·R_target = Rz(θ₄)Rx(α₄)Rz(θ₅)Rx(α₅)Rz(θ₆)`;
  `c₅ = (cα₄cα₅ − R_wrist(2,2))/(sα₄sα₅)`.
- The arm is solved in frame1: `p_w_des = T0_fixed⁻¹·(tool − d₆z) = Rz(q₁)·v`, where `v` is the wrist
  centre in frame1. Hence `arm_v = sinα₁·z_des` (**do not subtract d₁ again**: `T0_fixed` already
  contains `Tz(d₁)`).
- `α₂ = π` only needs the "z-direction" components multiplied by `ε = cos α₂`: `wp_z = d₂ + ε(d₃ +
  d₄cα₃)`, `wy ← ε·wy`, and `Kc`'s last term becomes `2ε·d₂(d₄cα₃+d₃)`; the θ₃ equation and `Wy²` are
  invariant in ε (ε²=1).
- `pw_test` inside `solveArm` must live in the **same frame** as `p_w_des`:
  `pw_test = T0_inv·(T0T1T2·p_w4)`.

## Defects fixed on 2026-09-27 (six new cases went red before the fix)

1. `is_valid_` was never set to false (the base default is `!dofs_.empty()`) ⇒ `isValid()` was always
   true, and on a 5-DOF robot `solve()` read `dofs[5]` out of bounds. The constructor now sets false at
   the start and true at the end, and `solve()` adds a `dofs().size()!=6` guard.
2. `α₂ = π` previously gave zero solutions (the arm derivation was expanded for α₂=0).
3. A non-zero base `a₀/d₁` previously gave zero solutions (the `pw_test` frame disagreed and `arm_v`
   subtracted `d₁` twice).
4. The spherical-wrist check missed `d₅`, so `d₅≠0` gave silent zero solutions.

## Verification recipe (must run when this module changes)

- **Random FK→IK round trip**: check "the generating solution comes back + every solution's FK
  reproduces the target + the count is ≤ 8".
- **Independent numerical IK (damped least squares, multiple starts) enumerated for comparison**: only
  checking "at least one solution" would miss a whole class of failures; the analytic set must match the
  numerical set one for one. Measured: a target with 4 solutions really has only 4 reachable branches (no
  branch is lost).
- **Compliant-configuration matrix** (`PieperCompliantConfigurations` / `PieperSingularConfigurations`,
  21 cases in total): enumerate α₁∈{±90}×α₂∈{0,π}×α₃∈{±90}×α₄∈{±90}×α₅∈{±90}×a₁∈{0,0.15} (64
  combinations) plus non-90° twists, per-link parameters, and joints pinned at special values.
- **Singularities need a weak oracle**: when `θ₅ = 0/π` (folded wrist) or the wrist centre lies on the
  base z axis (`r2 = 0`, shoulder singularity) the command **cannot** be reproduced (θ₄/θ₆ or θ₁ are
  unobservable) ⇒ assert soundness only (every solution's FK reproduces the target), never "the
  generating solution is in the set". Measured: **all** failing samples of `SpecialJointConfigurations`
  had `r2 = 0`.
- Mutation counter-check: revert the four new checks, `is_valid_`, the `pw_test` frame, `arm_v`'s `d₁`
  removal one at a time and confirm the matching case goes red (verified: reverting the new checks turns
  `UnsupportedRobotsAreRejected` red).

## Iterative-interface knobs (borrowed from RobWork's `invkin`, 2026-09-27)

- `IKSolver::setCheckJointLimits(bool)` / `isCheckingJointLimits()` (default true): an explicit switch.
  With it off, `PieperIKSolver` no longer rejects out-of-limit solutions and no longer wraps by 2π (it
  returns the raw solution, still passing the FK gate); with it off, `JacobianIKSolver` can converge
  outside the limits (which also needs `setClampToBounds(false)`).
- `IterativeIKSolver`'s `maxError()` (default 1e-6, the tolerance of **each enabled task component**, not
  a norm), `maxIterations()` (150), `maxSeeds()` (8), `isClampToBounds()` (true), `constraintMask()`
  (all true). The defaults equal the old hard-coded values, so behaviour is unchanged.
- `constraintMask()` mirrors RobWork's `Target::enabled[6]`: 0-2 = x/y/z, 3-5 = rotation-vector
  components; a disabled component is zeroed in both the error vector and the Jacobian columns ⇒ IK "on
  position only" or "on orientation only" is supported.
- `KinematicsBase::ikSolver()` gained a non-const overload so these knobs can be turned through the
  workcell path.
- Not ported (deliberately): the `IKMetaSolver` / `AmbiguityResolver` layering, via-point interpolation
  and `JacobianSolverType`; nor Craig case 2 / general case 3 (`|sinα₁|≠1`) - expanding the coverage
  means writing our own numerics, not copying RobWork (its six P0s are in the conversation record).
- **Solver selection is settled: exactly one** = the `IKSolverType` enum + an internal factory
  (`SerialKinematics::setIKSolverType`). **Deliberately not done**: an injection point (RobWork-style
  "the caller holds the solver") and a factory registry - both would make "type" and "instance" two
  sources of truth, producing "the type was set but did not take effect". The companion rule:
  `KinematicsBase::setIKSolverType` resets `ik_solver_`, so **the instance always matches the reported
  type** (a subclass not overriding that virtual gets null rather than a stale solver of the wrong type);
  tuning goes through `ikSolver()` (const to read, non-const to set), which does not change "who selects
  the solver". The pin: `SerialKinematicsTest.SolverInstanceAlwaysMatchesReportedType` (make the Pieper
  branch build a Jacobian ⇒ red).
