#include <gtest/gtest.h>

#include <vine/math/Isometry3.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>
#include <vine/robotics/kinematics/DHTransformConverter.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/JacobianIKSolver.hpp>

#include <cmath>

using namespace vn;
using namespace vn::math;
using namespace vn::robotics::kinematics;

// =============================================================================
// Helper: forward kinematics (same convention as the solver)
// =============================================================================
static Isometry3d forwardKinematics(const std::vector<DofInfo>& dofs, const std::vector<double>& q)
{
    Isometry3d T;
    for (size_t i = 0; i < dofs.size(); ++i) {
        T = T * dofs[i].origin;
        T = T * Isometry3d(Point3d(0, 0, 0), Quatd(q[i], dofs[i].axis));
    }
    return T;
}

// =============================================================================
// Test fixture: 2‑DOF planar arm
//
// Joint 0: revolute about Z, at origin
// Joint 1: revolute about Z, 1.0 m along local X
// =============================================================================
class Jacobian2DOFTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        DofInfo j0, j1;

        j0.origin = Isometry3d();   // identity
        j0.axis   = Vec3d(0, 0, 1); // Z-axis

        j1.origin = Isometry3d(Point3d(1, 0, 0), Quatd(1, 0, 0, 0)); // translate 1m in X
        j1.axis   = Vec3d(0, 0, 1);                                  // Z-axis

        dofs_   = { j0, j1 };
        solver_ = std::make_unique<JacobianIKSolver>(dofs_);
    }

    std::vector<DofInfo>              dofs_;
    std::unique_ptr<JacobianIKSolver> solver_;
};

// =============================================================================
// Test fixture: 3‑DOF planar arm
// =============================================================================
class Jacobian3DOFTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        DofInfo j0, j1, j2;

        j0.origin = Isometry3d();
        j0.axis   = Vec3d(0, 0, 1);

        j1.origin = Isometry3d(Point3d(1, 0, 0), Quatd(1, 0, 0, 0));
        j1.axis   = Vec3d(0, 0, 1);

        j2.origin = Isometry3d(Point3d(0.8, 0, 0), Quatd(1, 0, 0, 0));
        j2.axis   = Vec3d(0, 0, 1);

        dofs_   = { j0, j1, j2 };
        solver_ = std::make_unique<JacobianIKSolver>(dofs_);
    }

    std::vector<DofInfo>              dofs_;
    std::unique_ptr<JacobianIKSolver> solver_;
};

// =============================================================================
// Test fixture: 2‑DOF planar arm with joint limits
// =============================================================================
class JacobianLimitsTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        DofInfo j0, j1;

        j0.origin = Isometry3d();
        j0.axis   = Vec3d(0, 0, 1);
        j0.lower  = -1.0;
        j0.upper  = 1.0;

        j1.origin = Isometry3d(Point3d(1, 0, 0), Quatd(1, 0, 0, 0));
        j1.axis   = Vec3d(0, 0, 1);
        j1.lower  = -1.0;
        j1.upper  = 1.0;

        dofs_   = { j0, j1 };
        solver_ = std::make_unique<JacobianIKSolver>(dofs_);
    }

    std::vector<DofInfo>              dofs_;
    std::unique_ptr<JacobianIKSolver> solver_;
};

// =============================================================================
// 2‑DOF tests
// =============================================================================

TEST_F(Jacobian2DOFTest, IdentityTarget_ReturnsSolutions)
{
    // Zero joint angles → EE at (2, 0, 0)
    Isometry3d target = forwardKinematics(dofs_, { 0.0, 0.0 });

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    // Verify each solution by FK
    for (const auto& q : solutions) {
        Isometry3d T  = forwardKinematics(dofs_, { q[0], q[1] });
        double     dx = T.translation.x - target.translation.x;
        double     dy = T.translation.y - target.translation.y;
        EXPECT_NEAR(dx, 0.0, 1e-4);
        EXPECT_NEAR(dy, 0.0, 1e-4);
    }
}

TEST_F(Jacobian2DOFTest, BentArm_ReturnsSolutions)
{
    // Target: elbow slightly bent
    Isometry3d target = forwardKinematics(dofs_, { 0.5, 1.2 });

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& q : solutions) {
        Isometry3d T  = forwardKinematics(dofs_, { q[0], q[1] });
        double     dx = T.translation.x - target.translation.x;
        double     dy = T.translation.y - target.translation.y;
        EXPECT_NEAR(dx, 0.0, 1e-4);
        EXPECT_NEAR(dy, 0.0, 1e-4);
    }
}

TEST_F(Jacobian2DOFTest, FullyExtended_ReturnsSolutions)
{
    // Maximum reach ≈ 1 m from origin
    Isometry3d target = forwardKinematics(dofs_, { 0.0, 1.5 });

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& q : solutions) {
        Isometry3d T  = forwardKinematics(dofs_, { q[0], q[1] });
        double     dx = T.translation.x - target.translation.x;
        double     dy = T.translation.y - target.translation.y;
        EXPECT_NEAR(dx, 0.0, 1e-4);
        EXPECT_NEAR(dy, 0.0, 1e-4);
    }
}

TEST_F(Jacobian2DOFTest, UnreachableTarget_ReturnsFalse)
{
    // 10 m away → unreachable
    Isometry3d target(Point3d(10, 0, 0), Quatd(1, 0, 0, 0));

    std::vector<Q> solutions;
    EXPECT_FALSE(solver_->solve(target, solutions));
    EXPECT_TRUE(solutions.empty());
}

// =============================================================================
// 3‑DOF tests
// =============================================================================

TEST_F(Jacobian3DOFTest, RedundantArm_ReturnsSolutions)
{
    Isometry3d target = forwardKinematics(dofs_, { 0.3, 0.8, -0.5 });

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& q : solutions) {
        Isometry3d T  = forwardKinematics(dofs_, { q[0], q[1], q[2] });
        double     dx = T.translation.x - target.translation.x;
        double     dy = T.translation.y - target.translation.y;
        EXPECT_NEAR(dx, 0.0, 1e-4);
        EXPECT_NEAR(dy, 0.0, 1e-4);
    }
}

// =============================================================================
// Joint limits tests
// =============================================================================

TEST_F(JacobianLimitsTest, WithinLimits_ReturnsSolutions)
{
    Isometry3d target = forwardKinematics(dofs_, { 0.5, 0.5 });

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& q : solutions) {
        EXPECT_GE(q[0], dofs_[0].lower - 1e-6);
        EXPECT_LE(q[0], dofs_[0].upper + 1e-6);
        EXPECT_GE(q[1], dofs_[1].lower - 1e-6);
        EXPECT_LE(q[1], dofs_[1].upper + 1e-6);
    }
}

TEST_F(JacobianLimitsTest, BeyondLimits_ReturnsFalse)
{
    // Target requires joint 0 ≈ 1.5 rad, but limit is [-1, 1]
    Isometry3d target = forwardKinematics(dofs_, { 1.5, 0.3 });

    std::vector<Q> solutions;
    EXPECT_FALSE(solver_->solve(target, solutions));
}

// =============================================================================
// Degenerate case: 0 DOF
// =============================================================================

TEST(JacobianZeroTest, ZeroDof_ReturnsFalse)
{
    JacobianIKSolver solver({});
    std::vector<Q>   solutions;
    EXPECT_FALSE(solver.solve(Isometry3d(), solutions));
}

// =============================================================================
// Orientation tracking: 2‑DOF with orientation
// =============================================================================

TEST(JacobianOrientTest, OrientationConverges)
{
    // 2‑DOF plus a fixed orientation — use 1 joint rotating about X
    DofInfo j0;
    j0.origin = Isometry3d();
    j0.axis   = Vec3d(1, 0, 0); // rotate about X

    JacobianIKSolver solver({ j0 });

    // Target: 90° rotation about X
    Isometry3d target(Point3d(0, 0, 0), Quatd(PI_HALF, Vec3d(1, 0, 0)));

    std::vector<Q> solutions;
    ASSERT_TRUE(solver.solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);
    EXPECT_NEAR(solutions[0][0], PI_HALF, 1e-4);
}

// =============================================================================
// solver validity
// =============================================================================

TEST_F(Jacobian2DOFTest, SolverIsValid)
{
    EXPECT_TRUE(solver_->isValid());
}

TEST(JacobianZeroTest, SolverIsNotValid)
{
    JacobianIKSolver solver({});
    EXPECT_FALSE(solver.isValid());
}

// =============================================================================
// Helper: MDH to DofInfo (same as Pieper test)
// =============================================================================
static DofInfo makeRevoluteDof(double alpha, double a, double d)
{
    DofInfo dof;
    double  ca = std::cos(alpha), sa = std::sin(alpha);
    dof.axis   = Vec3d(0.0, -sa, ca);
    dof.origin = Isometry3d(Point3d(a, -d * sa, d * ca), Quatd(std::sin(alpha * 0.5), 0.0, 0.0, std::cos(alpha * 0.5)));
    return dof;
}

// =============================================================================
// 6‑DOF industrial manipulator (same MDH as Pieper test)
// =============================================================================
class Jacobian6DOFTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        const double params[6][3] = {
            { 0.0,      0.0,  0.0 },
            { -PI_HALF, 0.15, 0.0 },
            { 0.0,      1.0,  0.0 },
            { -PI_HALF, 0.0,  0.5 },
            { PI_HALF,  0.0,  0.0 },
            { PI_HALF,  0.0,  0.2 },
        };
        for (int i = 0; i < 6; ++i) dofs_.push_back(makeRevoluteDof(params[i][0], params[i][1], params[i][2]));
        solver_ = std::make_unique<JacobianIKSolver>(dofs_);
    }

    std::vector<DofInfo>              dofs_;
    std::unique_ptr<JacobianIKSolver> solver_;
};

TEST_F(Jacobian6DOFTest, ZeroConfig_ReturnsSolutions)
{
    std::vector<double> q0     = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    Isometry3d          target = forwardKinematics(dofs_, q0);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& sol : solutions) {
        Isometry3d T  = forwardKinematics(dofs_, { sol[0], sol[1], sol[2], sol[3], sol[4], sol[5] });
        double     dx = T.translation.x - target.translation.x;
        double     dy = T.translation.y - target.translation.y;
        double     dz = T.translation.z - target.translation.z;

        // Orientation error
        Quatd qe = target.rotation * T.rotation.conj();
        qe.normalize();
        if (qe.w < 0.0) {
            qe.x = -qe.x;
            qe.y = -qe.y;
            qe.z = -qe.z;
            qe.w = -qe.w;
        }
        double angle = 2.0 * std::acos(std::clamp(std::abs(qe.w), 0.0, 1.0));

        EXPECT_NEAR(dx, 0.0, 1e-3);
        EXPECT_NEAR(dy, 0.0, 1e-3);
        EXPECT_NEAR(dz, 0.0, 1e-3);
        EXPECT_NEAR(angle, 0.0, 1e-3);
    }
}

TEST_F(Jacobian6DOFTest, BentConfig_ReturnsSolutions)
{
    std::vector<double> q_target = { 0.3, -0.5, 1.2, -0.8, 0.4, 1.0 };
    Isometry3d          target   = forwardKinematics(dofs_, q_target);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& sol : solutions) {
        Isometry3d T     = forwardKinematics(dofs_, { sol[0], sol[1], sol[2], sol[3], sol[4], sol[5] });
        Vec3d      p_err = target.translation.asVector() - T.translation.asVector();

        Quatd qe = target.rotation * T.rotation.conj();
        qe.normalize();
        if (qe.w < 0.0) {
            qe.x = -qe.x;
            qe.y = -qe.y;
            qe.z = -qe.z;
            qe.w = -qe.w;
        }
        double a_err = 2.0 * std::acos(std::clamp(std::abs(qe.w), 0.0, 1.0));

        EXPECT_NEAR(p_err.length(), 0.0, 1e-3);
        EXPECT_NEAR(a_err, 0.0, 1e-3);
    }
}

TEST_F(Jacobian6DOFTest, UnreachableTarget_ReturnsFalse)
{
    Isometry3d target(Point3d(100, 0, 0), Quatd(1, 0, 0, 0));

    std::vector<Q> solutions;
    EXPECT_FALSE(solver_->solve(target, solutions));
}

TEST_F(Jacobian6DOFTest, SolverIsValid)
{
    EXPECT_TRUE(solver_->isValid());
}

TEST_F(Jacobian6DOFTest, ReachUp_ReturnsSolutions)
{
    // Configuration similar to Pieper "straight up" — elbow up, wrist neutral
    std::vector<double> q_up   = { 0.0, -PI_HALF * 0.3, PI_HALF * 0.6, PI_HALF * 0.5, 0.0, 0.0 };
    Isometry3d          target = forwardKinematics(dofs_, q_up);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& sol : solutions) {
        Isometry3d T     = forwardKinematics(dofs_, { sol[0], sol[1], sol[2], sol[3], sol[4], sol[5] });
        Vec3d      p_err = target.translation.asVector() - T.translation.asVector();

        Quatd qe = target.rotation * T.rotation.conj();
        qe.normalize();
        if (qe.w < 0.0) {
            qe.x = -qe.x;
            qe.y = -qe.y;
            qe.z = -qe.z;
            qe.w = -qe.w;
        }
        double a_err = 2.0 * std::acos(std::clamp(std::abs(qe.w), 0.0, 1.0));

        EXPECT_NEAR(p_err.length(), 0.0, 1e-3);
        EXPECT_NEAR(a_err, 0.0, 1e-3);
    }
}

TEST_F(Jacobian6DOFTest, LargeJointAngles_ReturnsSolutions)
{
    std::vector<double> q_big  = { -2.0, 1.5, -2.5, 1.0, -1.0, 0.5 };
    Isometry3d          target = forwardKinematics(dofs_, q_big);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver_->solve(target, solutions));
    ASSERT_GE(solutions.size(), 1u);

    for (const auto& sol : solutions) {
        Isometry3d T     = forwardKinematics(dofs_, { sol[0], sol[1], sol[2], sol[3], sol[4], sol[5] });
        Vec3d      p_err = target.translation.asVector() - T.translation.asVector();

        Quatd qe = target.rotation * T.rotation.conj();
        qe.normalize();
        if (qe.w < 0.0) {
            qe.x = -qe.x;
            qe.y = -qe.y;
            qe.z = -qe.z;
            qe.w = -qe.w;
        }
        double a_err = 2.0 * std::acos(std::clamp(std::abs(qe.w), 0.0, 1.0));

        EXPECT_NEAR(p_err.length(), 0.0, 1e-3);
        EXPECT_NEAR(a_err, 0.0, 1e-3);
    }
}
