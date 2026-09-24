#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include <vine/robotics/kinematics/DofInfo.hpp>
#include <vine/robotics/kinematics/Frame.hpp>
#include <vine/robotics/kinematics/QState.hpp>

using namespace vn::robotics::kinematics;

namespace
{

/**
 * @brief Frame with a fixed number of dof, used to build test trees.
 */
class TestJointFrame : public Frame {
  public:
    explicit TestJointFrame(std::size_t dof)
      : dof_infos_(dof)
    {}

    const std::vector<DofInfo>& dofInfos() const override
    {
        return dof_infos_;
    }

  private:
    std::vector<DofInfo> dof_infos_;
};

/**
 * @brief Builds a tree: root -> j1 -> j2, j1 -> j3.
 *
 * j1 has 1 dof, j2 has 1 dof, j3 has 3 dof; the root is fixed (0 dof).
 */
void buildTree(Frame& root, TestJointFrame& j1, TestJointFrame& j2, TestJointFrame& j3)
{
    root.addChild(&j1);
    j1.addChild(&j2);
    j1.addChild(&j3);
}

} // namespace

TEST(QStateTest, SetupCountsJointsAndDof)
{
    Frame          root;
    TestJointFrame j1(1);
    TestJointFrame j2(1);
    TestJointFrame j3(3);
    buildTree(root, j1, j2, j3);

    QState state;
    state.setup(&root);

    EXPECT_EQ(state.dofCount(), 5u);   // 1 + 1 + 3
    EXPECT_EQ(state.jointCount(), 3u); // fixed root not registered

    // All joint values default to zero.
    EXPECT_EQ(state.getQ(&j1), (Q{ 0.0 }));
    EXPECT_EQ(state.getQ(&j2), (Q{ 0.0 }));
    EXPECT_EQ(state.getQ(&j3), (Q{ 0.0, 0.0, 0.0 }));

    // A fixed (unregistered) frame has no joint values.
    EXPECT_TRUE(state.getQ(&root).empty());
}

TEST(QStateTest, SetAndGetQ)
{
    Frame          root;
    TestJointFrame j1(1);
    TestJointFrame j2(1);
    TestJointFrame j3(3);
    buildTree(root, j1, j2, j3);

    QState state;
    state.setup(&root);

    state.setQ(&j1, Q{ 0.5 });
    state.setQ(&j3, Q{ 1.0, 2.0, 3.0 });

    EXPECT_EQ(state.getQ(&j1), (Q{ 0.5 }));
    EXPECT_EQ(state.getQ(&j2), (Q{ 0.0 }));
    EXPECT_EQ(state.getQ(&j3), (Q{ 1.0, 2.0, 3.0 }));

    // Wrong size throws.
    EXPECT_THROW(state.setQ(&j3, Q{ 1.0, 2.0 }), std::invalid_argument);

    // Non-registered frame throws.
    TestJointFrame other(1);
    EXPECT_THROW(state.setQ(&other, Q{ 0.1 }), std::invalid_argument);
}

TEST(QStateTest, CopyState)
{
    Frame          root;
    TestJointFrame j1(1);
    TestJointFrame j2(2);
    root.addChild(&j1);
    root.addChild(&j2);

    QState a;
    a.setup(&root);
    a.setQ(&j1, Q{ 0.5 });
    a.setQ(&j2, Q{ 1.0, 2.0 });

    // Default copy ctor and assignment preserve the values.
    QState b(a);
    EXPECT_EQ(b.getQ(&j1), (Q{ 0.5 }));
    EXPECT_EQ(b.getQ(&j2), (Q{ 1.0, 2.0 }));

    QState c;
    c = a;
    EXPECT_EQ(c.getQ(&j1), (Q{ 0.5 }));

    // Mutating the copy does not affect the original.
    b.setQ(&j1, Q{ 9.0 });
    EXPECT_EQ(a.getQ(&j1), (Q{ 0.5 }));
}

TEST(QStateTest, CopyFrom)
{
    Frame          root;
    TestJointFrame j1(1);
    TestJointFrame j2(1);
    root.addChild(&j1);
    j1.addChild(&j2);

    QState a;
    a.setup(&root);
    a.setQ(&j1, Q{ 0.5 });
    a.setQ(&j2, Q{ 0.7 });

    // A state of the same tree copies the shared joint values.
    QState b;
    b.setup(&root);
    b.copyFrom(a);
    EXPECT_EQ(b.getQ(&j1), (Q{ 0.5 }));
    EXPECT_EQ(b.getQ(&j2), (Q{ 0.7 }));
}
