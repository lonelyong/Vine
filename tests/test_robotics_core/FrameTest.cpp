#include <gtest/gtest.h>

#include <stdexcept>

#include <vine/robotics/kinematics/Frame.hpp>

using namespace vn::robotics::kinematics;

namespace
{

/**
 * @brief Exposes the protected Frame(FrameType) constructor for tests.
 */
class TestFrame : public Frame
{
  public:
    explicit TestFrame(FrameType type)
      : Frame(type)
    {}
};

} // namespace

TEST(FrameTest, DefaultFrameIsFixed)
{
    Frame frame;
    EXPECT_EQ(frame.frameType(), FrameType::Fixed);
    EXPECT_TRUE(frame.isRoot());
}

TEST(FrameTest, DofFromType)
{
    Frame fixed;
    TestFrame revolute(FrameType::RevoluteJoint);
    TestFrame prismatic(FrameType::PrismaticJoint);
    TestFrame planar(FrameType::PlanarJoint);

    // The DoF count is derived from the frame type at construction.
    EXPECT_EQ(fixed.dof(), 0u);
    EXPECT_EQ(revolute.dof(), 1u);
    EXPECT_EQ(prismatic.dof(), 1u);
    EXPECT_EQ(planar.dof(), 3u);

    EXPECT_EQ(Frame::dofOfType(FrameType::Fixed), 0u);
    EXPECT_EQ(Frame::dofOfType(FrameType::RevoluteJoint), 1u);
    EXPECT_EQ(Frame::dofOfType(FrameType::PrismaticJoint), 1u);
    EXPECT_EQ(Frame::dofOfType(FrameType::PlanarJoint), 3u);
}

TEST(FrameTest, BuildsTree)
{
    Frame root;
    Frame child;
    Frame grand;

    root.addChild(&child);
    child.addChild(&grand);

    EXPECT_EQ(root.childCount(), 1u);
    EXPECT_EQ(root.childAt(0), &child);
    EXPECT_EQ(root.childAt(5), nullptr);
    EXPECT_EQ(child.parent(), &root);
    EXPECT_EQ(grand.parent(), &child);
    EXPECT_TRUE(root.isRoot());
    EXPECT_FALSE(child.isRoot());
    EXPECT_EQ(root.children().size(), 1u);
    EXPECT_EQ(child.children().size(), 1u);
}

TEST(FrameTest, IsAncestorOfReused)
{
    Frame root;
    Frame child1;
    Frame child2;
    Frame grand;

    root.addChild(&child1);
    root.addChild(&child2);
    child1.addChild(&grand);

    // The inherited IHierarchyNode algorithm is reused by Frame.
    EXPECT_TRUE(root.isAncestorOf(&grand));
    EXPECT_TRUE(root.isAncestorOf(&child1));
    EXPECT_TRUE(child1.isAncestorOf(&grand));
    EXPECT_FALSE(child1.isAncestorOf(&child2));
    EXPECT_FALSE(grand.isAncestorOf(&root));
    EXPECT_FALSE(root.isAncestorOf(&root));
    EXPECT_FALSE(root.isAncestorOf(nullptr));
}

TEST(FrameTest, IsDescendantOfReused)
{
    Frame root;
    Frame child1;
    Frame child2;
    Frame grand;

    root.addChild(&child1);
    root.addChild(&child2);
    child1.addChild(&grand);

    // The inherited IHierarchyNode algorithm is reused by Frame.
    EXPECT_TRUE(child1.isDescendantOf(&root));
    EXPECT_TRUE(grand.isDescendantOf(&root));
    EXPECT_TRUE(grand.isDescendantOf(&child1));
    EXPECT_FALSE(child2.isDescendantOf(&child1));
    EXPECT_FALSE(root.isDescendantOf(&grand));
    EXPECT_FALSE(root.isDescendantOf(&root));
    EXPECT_FALSE(root.isDescendantOf(nullptr));
}

TEST(FrameTest, AddChildValidation)
{
    Frame root;
    Frame child;

    EXPECT_THROW(root.addChild(nullptr), std::invalid_argument);
    EXPECT_THROW(root.addChild(&root), std::invalid_argument);

    root.addChild(&child);
    // Child already has a parent.
    EXPECT_THROW(root.addChild(&child), std::invalid_argument);
}

TEST(FrameTest, RemoveChild)
{
    Frame root;
    Frame child;
    Frame other;

    root.addChild(&child);

    // Not a direct child.
    EXPECT_THROW(root.removeChild(&other), std::invalid_argument);

    root.removeChild(&child);
    EXPECT_EQ(root.childCount(), 0u);
    EXPECT_EQ(child.parent(), nullptr);
    EXPECT_TRUE(child.isRoot());
}
