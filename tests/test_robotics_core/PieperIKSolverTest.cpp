#include <gtest/gtest.h>

#include <vine/math/Isometry3.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>
#include <vine/robotics/kinematics/DHTransformConverter.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/PieperIKSolver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <string>

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

// =============================================================================
// Validated robot class and multi-robot round-trips
// =============================================================================

namespace
{

using RobotTable = std::array<std::array<double, 3>, 6>;

/* MDH table of a generic 6-DOF industrial robot with a spherical wrist. */
RobotTable baseRobot()
{
    return { {
        { 0.0, 0.0, 0.0 },       /* 1: base           */
        { -PI_HALF, 0.15, 0.0 }, /* 2: shoulder       */
        { 0.0, 1.0, 0.0 },       /* 3: elbow          */
        { -PI_HALF, 0.0, 0.5 },  /* 4: wrist roll     */
        { PI_HALF, 0.0, 0.0 },   /* 5: wrist pitch    */
        { PI_HALF, 0.0, 0.2 },   /* 6: wrist yaw/tool */
    } };
}

std::vector<DofInfo> makeRobot(const RobotTable& params)
{
    std::vector<DofInfo> dofs;
    for (int i = 0; i < 6; ++i) dofs.push_back(makeRevoluteDof(params[i][0], params[i][1], params[i][2]));
    return dofs;
}

/* Squared angular distance between two joint configurations (2π-periodic). */
double configDistance(const Q& a, const Q& b)
{
    double distance = 0.0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        const double diff = std::abs(normalizeAngle(a[i] - b[i]));
        distance += diff * diff;
    }
    return distance;
}

/*
 * Checks that every solve() result for the target generated from q is a valid inverse-kinematics
 * solution (soundness) and - when expect_complete is set - that q itself is among the solutions
 * (completeness). Completeness intentionally does not hold at a wrist singularity, where two
 * joints become dependent and the commanded configuration is not reproducible.
 */
void checkTarget(const std::vector<DofInfo>& dofs, const PieperIKSolver& solver, const double q[6], bool expect_complete,
                 const std::string& context)
{
    const Isometry3d target = forwardKinematics(dofs, q);

    std::vector<Q> solutions;
    ASSERT_TRUE(solver.solve(target, solutions)) << context;
    ASSERT_GE(solutions.size(), 1u) << context;
    ASSERT_LE(solutions.size(), 8u) << context;

    bool recovered = false;
    for (const Q& sol : solutions) {
        ASSERT_EQ(sol.size(), 6u) << context;

        double q_sol[6];
        for (int i = 0; i < 6; ++i) q_sol[i] = sol[i];
        const Isometry3d T_sol = forwardKinematics(dofs, q_sol);

        const double dx = T_sol.translation.x - target.translation.x;
        const double dy = T_sol.translation.y - target.translation.y;
        const double dz = T_sol.translation.z - target.translation.z;
        EXPECT_LT(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-4) << context;

        const Quatd  q_diff = (T_sol.rotation * target.rotation.conj()).normalized();
        const double ang    = 2.0 * std::acos(std::clamp(std::abs(q_diff.w), 0.0, 1.0));
        EXPECT_LT(ang, 1e-4) << context;

        bool same = true;
        for (int i = 0; i < 6; ++i)
            if (std::abs(normalizeAngle(sol[i] - q[i])) > 1e-5) same = false;
        if (same) recovered = true;
    }
    if (expect_complete) EXPECT_TRUE(recovered) << "generating configuration missing: " << context;
}

/* Calls checkTarget() on random configurations of a robot inside the supported class. */
void expectRoundTrip(const RobotTable& params, int samples, unsigned seed, bool expect_complete = true)
{
    const std::vector<DofInfo> dofs = makeRobot(params);
    PieperIKSolver             solver(dofs);
    ASSERT_TRUE(solver.isValid());

    std::mt19937_64                        rng(seed);
    std::uniform_real_distribution<double> angle(-PI, PI);

    for (int s = 0; s < samples; ++s) {
        double q[6];
        for (int i = 0; i < 6; ++i) q[i] = angle(rng);
        checkTarget(dofs, solver, q, expect_complete, "sample " + std::to_string(s));
    }
}

} // namespace

TEST(PieperRobotClass, BaseRobotRoundTrip)
{
    expectRoundTrip(baseRobot(), 200, 42u);
}

TEST(PieperRobotClass, Alpha2PiRoundTrip)
{
    // α₂ = π (anti-parallel joint axes 2/3) is part of the supported class.
    RobotTable params = baseRobot();
    params[2][0]      = PI;
    expectRoundTrip(params, 200, 7u);

    // ... also together with non-zero d₂ and d₃.
    params[1][2] = 0.15;
    params[2][2] = 0.2;
    expectRoundTrip(params, 200, 8u);
}

TEST(PieperRobotClass, BaseOffsetRoundTrip)
{
    // A base height (d₁) and base offset (a₀) must not break the decoupling.
    RobotTable params = baseRobot();
    params[0][1]      = 0.2;
    params[0][2]      = 0.3;
    expectRoundTrip(params, 200, 11u);
}

TEST(PieperRobotClass, ArmVariantsRoundTrip)
{
    const struct {
        const char* name;
        int         row;
        int         col;
        double      value;
    } variants[] = {
        { "a1=0", 1, 1, 0.0 },
        { "alpha1=+90", 1, 0, PI_HALF },
        { "alpha3=+90", 3, 0, PI_HALF },
        { "a2=-1.0", 2, 1, -1.0 },
        { "d2=0.1", 1, 2, 0.1 },
        { "d3=0.25", 2, 2, 0.25 },
        { "d6=0", 5, 2, 0.0 },
        { "alpha0=+90", 0, 0, PI_HALF },
        { "alpha4=-90", 4, 0, -PI_HALF },
        { "alpha5=-90", 5, 0, -PI_HALF },
    };

    for (const auto& v : variants) {
        RobotTable params = baseRobot();
        params[v.row][v.col] = v.value;
        SCOPED_TRACE(v.name);
        expectRoundTrip(params, 120, 100u + static_cast<unsigned>(v.row * 10 + v.col));
    }
}

TEST(PieperRobotClass, UnsupportedRobotsAreRejected)
{
    // A solver is only valid if the robot satisfies every documented precondition;
    // otherwise isValid() must report false (and not merely produce no solutions).
    const std::vector<RobotTable> unsupported = {
        // a₃ ≠ 0 → not a spherical wrist
        [] {
            RobotTable p = baseRobot();
            p[3][1]      = 0.1;
            return p;
        }(),
        // d₅ ≠ 0 → the wrist axes do not meet at a single point
        [] {
            RobotTable p = baseRobot();
            p[4][2]      = 0.1;
            return p;
        }(),
        // a₂ = 0 → no elbow link
        [] {
            RobotTable p = baseRobot();
            p[2][1]      = 0.0;
            return p;
        }(),
        // α₂ = 90° → joint axes 2/3 neither parallel nor anti-parallel
        [] {
            RobotTable p = baseRobot();
            p[2][0]      = PI_HALF;
            return p;
        }(),
        // α₁ = 0 → joint axes 1/2 are not perpendicular
        [] {
            RobotTable p = baseRobot();
            p[1][0]      = 0.0;
            return p;
        }(),
        // α₃ = 0 → joint axes 3/4 parallel: θ₃ would be redundant
        [] {
            RobotTable p = baseRobot();
            p[3][0]      = 0.0;
            return p;
        }(),
        // α₃ = π → same redundancy
        [] {
            RobotTable p = baseRobot();
            p[3][0]      = PI;
            return p;
        }(),
        // d₄ = 0 → joint 3 cannot move the wrist centre
        [] {
            RobotTable p = baseRobot();
            p[3][2]      = 0.0;
            return p;
        }(),
        // α₄ = 0 → joint axes of the wrist are parallel: the wrist has only two DOF
        [] {
            RobotTable p = baseRobot();
            p[4][0]      = 0.0;
            return p;
        }(),
        // α₄ = π → same
        [] {
            RobotTable p = baseRobot();
            p[4][0]      = PI;
            return p;
        }(),
        // α₅ = 0 → last two wrist axes parallel
        [] {
            RobotTable p = baseRobot();
            p[5][0]      = 0.0;
            return p;
        }(),
        // α₅ = π → same
        [] {
            RobotTable p = baseRobot();
            p[5][0]      = PI;
            return p;
        }(),
    };

    for (const RobotTable& params : unsupported) {
        const PieperIKSolver solver(makeRobot(params));
        EXPECT_FALSE(solver.isValid());
    }
}

TEST(PieperRobotClass, TooFewDofsIsRejectedAndSafeToSolve)
{
    std::vector<DofInfo> dofs;
    for (int i = 0; i < 5; ++i) dofs.push_back(makeRevoluteDof(0.0, 0.1, 0.0));

    const PieperIKSolver solver(dofs);

    EXPECT_FALSE(solver.isValid());
    EXPECT_FALSE(solver.dofs().empty());

    Isometry3d target;
    target.translation = Point3d(0.5, 0.0, 0.5);
    target.rotation    = Quatd(0.0, 0.0, 0.0, 1.0);

    std::vector<Q> solutions;
    EXPECT_FALSE(solver.solve(target, solutions));
    EXPECT_TRUE(solutions.empty());
}

TEST(PieperRobotClass, NonMdhOriginIsRejected)
{
    std::vector<DofInfo> dofs;
    for (int i = 0; i < 6; ++i) dofs.push_back(makeRevoluteDof(0.0, 0.0, 0.0));
    dofs[2].origin = Isometry3d(Point3d(0.3, 0.4, 0.5), Quatd(0.3, 0.2, 0.1, 0.9).normalized());

    const PieperIKSolver solver(dofs);
    EXPECT_FALSE(solver.isValid());
}

TEST(PieperRobotClass, SolutionsRespectJointLimits)
{
    const std::vector<DofInfo> base = makeRobot(baseRobot());
    std::vector<DofInfo>       dofs = base;
    for (DofInfo& dof : dofs) {
        dof.lower = -1.0;
        dof.upper = 1.0;
    }

    PieperIKSolver solver(dofs);
    ASSERT_TRUE(solver.isValid());

    std::mt19937_64                        rng(2024u);
    std::uniform_real_distribution<double> angle(-1.0, 1.0);

    for (int s = 0; s < 100; ++s) {
        double q[6];
        for (int i = 0; i < 6; ++i) q[i] = angle(rng);

        const Isometry3d target = forwardKinematics(dofs, q);

        std::vector<Q> solutions;
        ASSERT_TRUE(solver.solve(target, solutions)) << "sample " << s;

        for (const Q& sol : solutions) {
            for (int i = 0; i < 6; ++i) EXPECT_GE(sol[i], -1.0 - 1e-6) << "sample " << s;
            for (int i = 0; i < 6; ++i) EXPECT_LE(sol[i], 1.0 + 1e-6) << "sample " << s;
        }
    }
}

namespace
{

bool containsConfiguration(const std::vector<Q>& solutions, const double q[6], double tolerance = 1e-5)
{
    for (const Q& sol : solutions) {
        bool same = true;
        for (int i = 0; i < 6; ++i)
            if (std::abs(normalizeAngle(sol[i] - q[i])) > tolerance) same = false;
        if (same) return true;
    }
    return false;
}

} // namespace

TEST(PieperRobotClass, JointLimitCheckingCanBeDisabled)
{
    std::vector<DofInfo> dofs = makeRobot(baseRobot());
    for (DofInfo& dof : dofs) {
        dof.lower = -1.0;
        dof.upper = 1.0;
    }

    PieperIKSolver solver(dofs);
    ASSERT_TRUE(solver.isValid());

    const double     q_out[6] = { 2.0, -1.8, 1.5, 0.5, 0.3, -0.6 }; // outside [-1, 1]
    const Isometry3d target   = forwardKinematics(dofs, q_out);

    // With the check enabled the commanded configuration is filtered out.
    std::vector<Q> solutions;
    solver.solve(target, solutions);
    for (const Q& sol : solutions)
        for (int i = 0; i < 6; ++i) {
            EXPECT_GE(sol[i], -1.0 - 1e-6);
            EXPECT_LE(sol[i], 1.0 + 1e-6);
        }
    EXPECT_FALSE(containsConfiguration(solutions, q_out));

    // Disabling it hands the commanded configuration back.
    solver.setCheckJointLimits(false);

    solutions.clear();
    ASSERT_TRUE(solver.solve(target, solutions));
    EXPECT_TRUE(containsConfiguration(solutions, q_out));
}

TEST(PieperSeed, SolutionsSortedByDistanceFromSeed)
{
    const std::vector<DofInfo> dofs = makeRobot(baseRobot());
    PieperIKSolver             solver(dofs);
    ASSERT_TRUE(solver.isValid());

    const double   q_known[6] = { 0.3, -0.5, 1.2, 0.7, -0.4, 0.6 };
    const Isometry3d target   = forwardKinematics(dofs, q_known);

    const Q seed{ 0.1, -0.2, 0.0, 0.4, 0.0, 0.0 };

    std::vector<Q> solutions;
    ASSERT_TRUE(solver.solve(target, solutions, seed));
    ASSERT_GT(solutions.size(), 1u);

    const Q nearest = solutions.front();
    for (const Q& sol : solutions) EXPECT_GE(configDistance(sol, seed), configDistance(nearest, seed) - 1e-12);

    for (std::size_t i = 1; i < solutions.size(); ++i)
        EXPECT_LE(configDistance(solutions[i - 1], seed), configDistance(solutions[i], seed) + 1e-12);

    // The seed-ordered overload must return the same set as the unsorted one.
    std::vector<Q> unsorted;
    ASSERT_TRUE(solver.solve(target, unsorted));
    EXPECT_EQ(unsorted.size(), solutions.size());
}

// =============================================================================
// Exhaustive coverage of the supported (compliant) configuration space
// =============================================================================

namespace
{

struct NamedRobot {
    const char* name;
    RobotTable  params;
};

/* Representative robots covering each structural branch of the solver. */
std::vector<NamedRobot> representativeRobots()
{
    std::vector<NamedRobot> robots;
    robots.push_back({ "canonical", baseRobot() });
    {
        RobotTable params = baseRobot();
        params[2][0]      = PI; // alpha2 = pi (anti-parallel joints 2/3)
        robots.push_back({ "alpha2=pi", params });
    }
    {
        RobotTable params = baseRobot();
        params[1][2]      = 0.15; // d2
        params[2][2]      = 0.2;  // d3
        robots.push_back({ "d2,d3", params });
    }
    {
        RobotTable params = baseRobot();
        params[0][1]      = 0.2; // a0
        params[0][2]      = 0.3; // d1
        robots.push_back({ "base-offset", params });
    }
    {
        RobotTable params = baseRobot();
        params[1][0]      = PI_HALF; // alpha1 = +90°
        params[1][1]      = 0.0;     // a1 = 0 (zero shoulder offset)
        robots.push_back({ "alpha1=+90,a1=0", params });
    }
    {
        RobotTable params = baseRobot();
        params[3][0]      = PI / 3.0; // non-canonical but non-parallel twists
        params[4][0]      = -PI / 3.0;
        params[5][0]      = PI / 3.0;
        params[5][2]      = 0.0; // d6 = 0
        robots.push_back({ "60deg-twists", params });
    }
    return robots;
}

std::string jointContext(const char* robot, int joint, double value)
{
    return std::string(robot) + " joint " + std::to_string(joint + 1) + " = " + std::to_string(value);
}

} // namespace

TEST(PieperCompliantConfigurations, CanonicalTwistCombinations)
{
    // Every combination of the canonical twists of a 6-DOF robot with a spherical wrist:
    //   alpha1 = ±90°        (joint 1 ⟂ joint 2)
    //   alpha2 ∈ {0, π}      (joints 2 and 3 parallel)
    //   alpha3 = ±90°        (joints 3 and 4 not parallel, joint 3 moves the wrist centre)
    //   alpha4, alpha5 = ±90° (consecutive wrist axes neither parallel nor anti-parallel)
    //   a1 ∈ {0, 0.15}       (zero and non-zero shoulder offset)
    const double k90   = PI_HALF;
    int          count = 0;

    for (double alpha1 : { -k90, k90 })
        for (double alpha2 : { 0.0, PI })
            for (double alpha3 : { -k90, k90 })
                for (double alpha4 : { -k90, k90 })
                    for (double alpha5 : { -k90, k90 })
                        for (double a1 : { 0.0, 0.15 }) {
                            RobotTable params = baseRobot();
                            params[1][0]      = alpha1;
                            params[1][1]      = a1;
                            params[2][0]      = alpha2;
                            params[3][0]      = alpha3;
                            params[4][0]      = alpha4;
                            params[5][0]      = alpha5;

                            SCOPED_TRACE("alpha1=" + std::to_string(alpha1) + " alpha2=" + std::to_string(alpha2) +
                                         " alpha3=" + std::to_string(alpha3) + " alpha4=" + std::to_string(alpha4) +
                                         " alpha5=" + std::to_string(alpha5) + " a1=" + std::to_string(a1));
                            expectRoundTrip(params, 40, 101u + static_cast<unsigned>(count));
                            count++;
                        }

    EXPECT_EQ(count, 64);
}

TEST(PieperCompliantConfigurations, NonCanonicalTwists)
{
    // The class only requires the twists to be non-parallel, not exactly ±90°.
    const double k60   = PI / 3.0;
    const double k120  = 2.0 * PI / 3.0;
    int          count = 0;

    for (double alpha3 : { -k120, -k60, k60, k120 })
        for (double alpha4 : { -k120, -k60, k60, k120 })
            for (double alpha5 : { -k120, -k60, k60, k120 }) {
                RobotTable params = baseRobot();
                params[3][0]      = alpha3;
                params[4][0]      = alpha4;
                params[5][0]      = alpha5;

                SCOPED_TRACE("alpha3=" + std::to_string(alpha3) + " alpha4=" + std::to_string(alpha4) +
                             " alpha5=" + std::to_string(alpha5));
                expectRoundTrip(params, 20, 211u + static_cast<unsigned>(count));
                count++;
            }

    EXPECT_EQ(count, 64);
}

TEST(PieperCompliantConfigurations, LinkParameterVariations)
{
    // Each link parameter may be zero or non-zero independently.
    const struct {
        const char* name;
        int         row;
        int         col;
        double      value;
    } variations[] = {
        { "a0=0.2", 0, 1, 0.2 },
        { "d1=0.3", 0, 2, 0.3 },
        { "alpha0=+90", 0, 0, PI_HALF },
        { "alpha0=180", 0, 0, PI },
        { "a1=0", 1, 1, 0.0 },
        { "a2=-1.0", 2, 1, -1.0 },
        { "d2=0.15", 1, 2, 0.15 },
        { "d3=0.2", 2, 2, 0.2 },
        { "d4=0.1", 3, 2, 0.1 },
        { "d4=1.0", 3, 2, 1.0 },
        { "d6=0", 5, 2, 0.0 },
        { "d6=1.0", 5, 2, 1.0 },
    };

    int index = 0;
    for (const auto& variation : variations) {
        RobotTable params                    = baseRobot();
        params[variation.row][variation.col] = variation.value;
        SCOPED_TRACE(variation.name);
        expectRoundTrip(params, 60, 313u + static_cast<unsigned>(index));
        index++;
    }

    // ... and all of them at once.
    RobotTable combined = baseRobot();
    combined[0][0]      = PI_HALF;
    combined[0][1]      = 0.2;
    combined[0][2]      = 0.3;
    combined[1][0]      = PI_HALF;
    combined[1][1]      = 0.0;
    combined[1][2]      = 0.15;
    combined[2][0]      = PI;
    combined[2][1]      = -1.0;
    combined[2][2]      = 0.2;
    combined[3][2]      = 0.1;
    combined[5][2]      = 0.0;
    SCOPED_TRACE("combined");
    expectRoundTrip(combined, 100, 411u);
}

TEST(PieperSingularConfigurations, PinnedJointsRoundTrip)
{
    // Pinning individual joints at special values must not lose the commanded configuration.
    // Joint 5 is covered separately because it can fold the wrist.
    const int    joints[] = { 0, 1, 2, 3, 5 };
    const double values[] = { -PI_HALF, 0.0, PI_HALF, PI };

    for (const NamedRobot& robot : representativeRobots()) {
        const std::vector<DofInfo> dofs = makeRobot(robot.params);
        PieperIKSolver             solver(dofs);
        ASSERT_TRUE(solver.isValid()) << robot.name;

        std::mt19937_64                        rng(517u);
        std::uniform_real_distribution<double> angle(-PI, PI);

        for (int joint : joints)
            for (double value : values)
                for (int repetition = 0; repetition < 6; ++repetition) {
                    double q[6];
                    for (int i = 0; i < 6; ++i) q[i] = angle(rng);
                    q[joint] = value;
                    checkTarget(dofs, solver, q, true, jointContext(robot.name, joint, value));
                }
    }
}

TEST(PieperSingularConfigurations, WristFoldedStaysSound)
{
    // theta_5 = 0 (and pi when the wrist twists are equal) folds the wrist: only the sum or
    // difference of theta_4 and theta_6 is observable, so the commanded configuration is not
    // reproducible. Every solution returned for such a target must still reach the target.
    for (const NamedRobot& robot : representativeRobots()) {
        const std::vector<DofInfo> dofs = makeRobot(robot.params);
        PieperIKSolver             solver(dofs);
        ASSERT_TRUE(solver.isValid()) << robot.name;

        std::mt19937_64                        rng(619u);
        std::uniform_real_distribution<double> angle(-PI, PI);

        for (double theta5 : { 0.0, PI })
            for (int repetition = 0; repetition < 10; ++repetition) {
                double q[6];
                for (int i = 0; i < 6; ++i) q[i] = angle(rng);
                q[4] = theta5;
                checkTarget(dofs, solver, q, false, jointContext(robot.name, 4, theta5));
            }
    }
}

TEST(PieperSingularConfigurations, NearFoldedWristRoundTrip)
{
    // A slightly open wrist is ill-conditioned but not singular, so the commanded
    // configuration must still be recovered.
    for (const NamedRobot& robot : representativeRobots()) {
        const std::vector<DofInfo> dofs = makeRobot(robot.params);
        PieperIKSolver             solver(dofs);
        ASSERT_TRUE(solver.isValid()) << robot.name;

        std::mt19937_64                        rng(827u);
        std::uniform_real_distribution<double> angle(-PI, PI);

        for (double theta5 : { -1e-3, 1e-3 })
            for (int repetition = 0; repetition < 6; ++repetition) {
                double q[6];
                for (int i = 0; i < 6; ++i) q[i] = angle(rng);
                q[4] = theta5;
                checkTarget(dofs, solver, q, true, jointContext(robot.name, 4, theta5));
            }
    }
}

TEST(PieperSingularConfigurations, SpecialJointConfigurations)
{
    // All joints pinned at once to special values, with the wrist held away from its singularity.
    const double values[] = { -PI_HALF, 0.0, PI_HALF, PI };

    for (const NamedRobot& robot : representativeRobots()) {
        const std::vector<DofInfo> dofs = makeRobot(robot.params);
        PieperIKSolver             solver(dofs);
        ASSERT_TRUE(solver.isValid()) << robot.name;

        for (double q1 : values)
            for (double q2 : values)
                for (double q3 : values)
                    for (double q6 : values) {
                        const double     qv[6]  = { q1, q2, q3, 0.0, PI_HALF, q6 };
                        const Isometry3d target = forwardKinematics(dofs, qv);
                        const Vec3d      tool_z = target.axisZ();
                        const double     px     = target.translation.x - tool_z.x * robot.params[5][2];
                        const double     py     = target.translation.y - tool_z.y * robot.params[5][2];

                        // When the wrist centre sits on the base axis, joint 1 becomes
                        // unobservable (shoulder singularity) and the command is not
                        // reproducible; soundness alone is asserted there.
                        const bool singular = (px * px + py * py) < 1e-12;

                        std::string context = std::string(robot.name) + " q=";
                        for (double value : qv) context += std::to_string(value) + " ";
                        context += std::string("(singular=") + (singular ? "yes)" : "no)");
                        checkTarget(dofs, solver, qv, !singular, context);
                    }
    }
}
