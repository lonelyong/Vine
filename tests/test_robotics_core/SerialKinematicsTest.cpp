#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include <vine/math/Isometry3.hpp>
#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/Q.hpp>
#include <vine/robotics/kinematics/SerialKinematics.hpp>
#include <vine/robotics/kinematics/State.hpp>

using namespace vn::math;
using namespace vn::robotics::kinematics;

namespace
{

/**
 * @brief Frame with a configurable number of revolute dofs and bounds.
 */
class TestJoint : public Frame
{
  public:
    explicit TestJoint(std::size_t dof, double lower = -3.14, double upper = 3.14)
    {
        dof_infos_.resize(dof);
        for (auto& d : dof_infos_) {
            d.type  = DofType::RevoluteJoint;
            d.axis  = Vec3d{ 0.0, 0.0, 1.0 };
            d.lower = lower;
            d.upper = upper;
        }
    }

    const std::vector<DofInfo>& dofInfos() const override
    {
        return dof_infos_;
    }

  private:
    std::vector<DofInfo> dof_infos_;
};

/**
 * @brief Compares two isometries within a small tolerance.
 */
void expectNear(const Isometry3d& a, const Isometry3d& b)
{
    EXPECT_LT((a.translation - b.translation).length(), 1e-9);
    EXPECT_EQ(a.rotation, b.rotation);
}

} // namespace

TEST(SerialKinematicsTest, BuildsChainFromFrameTree)
{
    Frame base;
    TestJoint j1(1);
    TestJoint j2(1);
    Frame end;
    base.addChild(&j1);
    j1.addChild(&j2);
    j2.addChild(&end);

    SerialKinematics kin(&base, &end);

    EXPECT_EQ(kin.dof(), 2u);
    ASSERT_EQ(kin.joints().size(), 2u);
    EXPECT_EQ(kin.joints()[0], &j1);
    EXPECT_EQ(kin.joints()[1], &j2);
    ASSERT_EQ(kin.dofs().size(), 2u);
    EXPECT_EQ(kin.dofType(0), DofType::RevoluteJoint);

    ASSERT_EQ(kin.lowerBounds().size(), 2u);
    EXPECT_DOUBLE_EQ(kin.lowerBounds()[0], -3.14);
    EXPECT_DOUBLE_EQ(kin.upperBounds()[0], 3.14);
    ASSERT_EQ(kin.maxVelocityLimits().size(), 2u);
    ASSERT_EQ(kin.jointResolutions().size(), 2u);

    // A 2-dof chain defaults to the iterative (Jacobian) solver.
    EXPECT_EQ(kin.ikSolverType(), IKSolverType::Iterative);
    EXPECT_NE(kin.ikSolver(), nullptr);

    // Disabling IK clears the solver; re-enabling recreates it.
    kin.setIKSolverType(IKSolverType::None);
    EXPECT_EQ(kin.ikSolverType(), IKSolverType::None);
    EXPECT_EQ(kin.ikSolver(), nullptr);
    EXPECT_TRUE(kin.solveIK(Isometry3d{}).empty());
    kin.setIKSolverType(IKSolverType::Iterative);
    EXPECT_NE(kin.ikSolver(), nullptr);
}

TEST(SerialKinematicsTest, GetSetQ)
{
    Frame base;
    TestJoint j1(1);
    TestJoint j2(1);
    base.addChild(&j1);
    j1.addChild(&j2);

    SerialKinematics kin(&base, &j2);

    State state;
    state.setup(&base);

    const Q q{ 0.5, -1.0 };
    kin.setQ(q, state);
    EXPECT_EQ(kin.getQ(state), q);

    // Out-of-bounds values are rejected.
    EXPECT_THROW(kin.setQ(Q{ 5.0, 0.0 }, state), std::invalid_argument);
    // A wrong-size vector is rejected.
    EXPECT_THROW(kin.setQ(Q{ 0.0 }, state), std::runtime_error);
}

TEST(SerialKinematicsTest, InvalidChainThrows)
{
    Frame base;
    Frame end;
    // end is not a descendant of base.
    EXPECT_THROW(SerialKinematics(&base, &end), std::runtime_error);
    EXPECT_THROW(SerialKinematics(nullptr, &end), std::invalid_argument);
    EXPECT_THROW(SerialKinematics(&base, nullptr), std::invalid_argument);
}

TEST(SerialKinematicsTest, FixedChainHasZeroDof)
{
    Frame base;
    Frame end;
    base.addChild(&end);

    SerialKinematics kin(&base, &end);
    EXPECT_EQ(kin.dof(), 0u);
    EXPECT_TRUE(kin.joints().empty());
    // IK is disabled for a 0-dof chain.
    EXPECT_TRUE(kin.solveIK(Isometry3d{}).empty());
}

TEST(SerialKinematicsTest, InterspersedFixedFrameAccumulatesIntoOrigin)
{
    Frame base;
    TestJoint j1(1);
    Frame fixed;
    TestJoint j2(1);
    base.addChild(&j1);
    j1.addChild(&fixed);
    fixed.addChild(&j2);

    // A fixed frame between the two motion joints, with an x-translation.
    Isometry3d fixed_tf;
    fixed_tf.translation = Point3d{ 100.0, 0.0, 0.0 };
    fixed.setFixedTransform(fixed_tf);

    SerialKinematics kin(&base, &j2);

    ASSERT_EQ(kin.dof(), 2u);
    ASSERT_EQ(kin.joints().size(), 2u);
    ASSERT_NE(kin.ikSolver(), nullptr);

    const auto& solver_dofs = kin.ikSolver()->dofs();
    ASSERT_EQ(solver_dofs.size(), 2u);

    // The first joint has no preceding fixed frame: origin unchanged.
    expectNear(solver_dofs[0].origin, Isometry3d{});
    // The second joint's origin accumulates the interspersed fixed transform.
    expectNear(solver_dofs[1].origin, fixed_tf);
    // The axis is unchanged (motion rotates in the origin's local frame).
    EXPECT_EQ(solver_dofs[1].axis, (Vec3d{ 0.0, 0.0, 1.0 }));
}
