#include <gtest/gtest.h>

#include <vine/math/Isometry3.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>
#include <vine/robotics/kinematics/DHTransformConverter.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/PieperIKSolver.hpp>

#include <cmath>

using namespace vn;
using namespace vn::math;
using namespace vn::robotics::kinematics;

// =============================================================================
// Helper: build a DofInfo from MDH parameters (α, a, d)
// =============================================================================
static DofInfo makeRevoluteDof(double alpha, double a, double d)
{
    DofInfo dof;

    const double ca = std::cos(alpha);
    const double sa = std::sin(alpha);

    // Joint axis (z-axis of the MDH frame) in local coordinates
    dof.axis = Vec3d(0.0, -sa, ca);

    // Origin at θ = 0:  Rot_x(α) * Trans_x(a) * Trans_z(d)
    //   translation = [a,  -d·sinα,  d·cosα]
    //   rotation    = Rot_x(α)  →  quaternion = (sin(α/2), 0, 0, cos(α/2))
    dof.origin = Isometry3d(Point3d(a, -d * sa, d * ca), Quatd(std::sin(alpha * 0.5), 0.0, 0.0, std::cos(alpha * 0.5)));

    return dof;
}

// =============================================================================
// Test fixture: 6‑DOF industrial robot with spherical wrist
//
// MDH table:
//   i │ α_{i-1} │ a_{i-1} │ θ_i │ d_i
//  ───┼──────────┼──────────┼─────┼─────
//   0 │    0     │    0     │ θ0  │  0      ← base
//   1 │  −90°    │    0     │ θ1  │  0      ← shoulder
//   2 │    0     │  1.0 m   │ θ2  │  0      ← elbow
//   3 │  −90°    │    0     │ θ3  │  0.5 m  ← wrist roll
//   4 │   90°    │    0     │ θ4  │  0      ← wrist pitch
//   5 │    0     │    0     │ θ5  │  0.2 m  ← wrist yaw  (tool)
// =============================================================================
class PieperIKSolverTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        // α (rad), a (m), d (m)
        const double params[6][3] = {
            { 0.0,      0.0,  0.0 }, // joint 0: base
            { -PI_HALF, 0.15, 0.0 }, // joint 1: shoulder  (a1 > 0 required by solver)
            { 0.0,      1.0,  0.0 }, // joint 2: elbow
            { -PI_HALF, 0.0,  0.5 }, // joint 3: wrist roll
            { PI_HALF,  0.0,  0.0 }, // joint 4: wrist pitch
            { PI_HALF,  0.0,  0.2 }, // joint 5: wrist yaw  (tool)
        };

        for (int i = 0; i < 6; ++i) {
            dofs_.push_back(makeRevoluteDof(params[i][0], params[i][1], params[i][2]));
        }
        solver_ = std::make_unique<PieperIKSolver>(dofs_);
    }

    std::vector<DofInfo>            dofs_;
    std::unique_ptr<PieperIKSolver> solver_;
};

// -----------------------------------------------------------------------------
// Forward kinematics helper  (T_0_6)
// -----------------------------------------------------------------------------
static Isometry3d forwardKinematics(const std::vector<DofInfo>& dofs, const double* q)
{
    Isometry3d T;
    for (int i = 0; i < 6; ++i) {
        auto dh_opt = tryMdhFromTransform(dofs[i].origin);
        if (!dh_opt)
            return Isometry3d{};
        DHParameter dh = *dh_opt;
        T              = T * mdhToTransform(dh, /*dd=*/0.0, /*dtheta=*/q[i]);
    }
    return T;
}

// =============================================================================
// Tests
// =============================================================================

TEST_F(PieperIKSolverTest, ZeroConfig_FK_MatchesExpected)
{
    // At zero configuration the accumulated rotation is identity
    // because  α₀=0, α₁=-90°, α₂=0, α₃=-90°, α₄=90°, α₅=90°
    //    R = Rot_x(-90°)·Rot_x(-90°)·Rot_x(90°)·Rot_x(90°) = I

    const double q[6] = { 0, 0, 0, 0, 0, 0 };
    Isometry3d   T    = forwardKinematics(dofs_, q);

    EXPECT_NEAR(T.rotation.x, 0.0, 1e-8);
    EXPECT_NEAR(T.rotation.y, 0.0, 1e-8);
    EXPECT_NEAR(T.rotation.z, 0.0, 1e-8);
    EXPECT_NEAR(T.rotation.w, 1.0, 1e-8);

    EXPECT_TRUE(std::isfinite(T.translation.x));
    EXPECT_TRUE(std::isfinite(T.translation.y));
    EXPECT_TRUE(std::isfinite(T.translation.z));
}

TEST_F(PieperIKSolverTest, IK_KnownPose_ReturnsSolutions)
{
    // Choose a known joint configuration, compute FK to get target pose,
    // then run IK and verify at least one solution reproduces the target.

    const double q_known[6] = { 0.3, -0.5, 1.2, 0.7, -0.4, 0.6 };
    Isometry3d   targetPose = forwardKinematics(dofs_, q_known);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(targetPose, solutions));
    EXPECT_GE(solutions.size(), 1u);
    EXPECT_LE(solutions.size(), 8u); // max 8 for 6‑R spherical wrist

    // Verify that every returned solution, when used in FK, matches the target
    for (auto& sol : solutions) {
        ASSERT_EQ(sol.size(), 6u);
        double q_sol[6];
        for (size_t i = 0; i < 6; ++i) q_sol[i] = sol[i];

        Isometry3d T_sol = forwardKinematics(dofs_, q_sol);

        // Position error
        double pos_err = std::sqrt((T_sol.translation.x - targetPose.translation.x) * (T_sol.translation.x - targetPose.translation.x) +
                                   (T_sol.translation.y - targetPose.translation.y) * (T_sol.translation.y - targetPose.translation.y) +
                                   (T_sol.translation.z - targetPose.translation.z) * (T_sol.translation.z - targetPose.translation.z));
        EXPECT_LT(pos_err, 1e-4) << "Position error too large for solution";

        // Orientation error: |q_sol * q_target⁻¹| ≈ (0,0,0,1)
        Quatd q_sol_q = T_sol.rotation;
        Quatd q_tgt   = targetPose.rotation;
        Quatd q_diff  = q_sol_q * q_tgt.conj();

        // Angular error = 2·acos(|q_diff.w|)  (clamped to [-1,1])
        double w       = std::clamp(std::abs(q_diff.w), -1.0, 1.0);
        double ang_err = 2.0 * std::acos(w);
        EXPECT_LT(ang_err, 1e-4) << "Orientation error too large for solution";
    }
}

TEST_F(PieperIKSolverTest, IK_StraightUp_ReturnsSolutions)
{
    // End‑effector pointing straight up at a reachable position.
    // p_target = (1.0, 0.0, 0.7), orientation = identity
    Isometry3d target;
    target.translation = Point3d(1.0, 0.0, 0.7);
    target.rotation    = Quatd(0.0, 0.0, 0.0, 1.0);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    EXPECT_GT(solutions.size(), 0u);
}

TEST_F(PieperIKSolverTest, IK_UnreachableTarget_ReturnsFalse)
{
    // A target far beyond the reach of the arm (max reach ≈ a2 + d4 + d6 ≈ 1.7 m)
    Isometry3d target;
    target.translation = Point3d(100.0, 0.0, 0.0);
    target.rotation    = Quatd(0.0, 0.0, 0.0, 1.0);

    std::vector<Q> solutions;
    EXPECT_FALSE(solver_->solve(target, solutions));
    EXPECT_EQ(solutions.size(), 0u);
}

TEST_F(PieperIKSolverTest, IK_OutOfReachZ_ReturnsFalse)
{
    // Target below the base (negative z with large magnitude)
    Isometry3d target;
    target.translation = Point3d(0.0, 0.0, -50.0);
    target.rotation    = Quatd(0.0, 0.0, 0.0, 1.0);

    std::vector<Q> solutions;
    EXPECT_FALSE(solver_->solve(target, solutions));
}
