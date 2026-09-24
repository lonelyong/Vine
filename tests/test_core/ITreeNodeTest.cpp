#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include <vine/IHierarchyNode.hpp>

using vn::IHierarchyNode;

namespace
{

/**
 * @brief Minimal concrete tree node used to exercise IHierarchyNode.
 */
class Node : public IHierarchyNode<Node> {
  public:
    explicit Node(Node* parent)
      : parent_(parent)
    {}

    Node* parent() const noexcept override
    {
        return parent_;
    }

    std::size_t childCount() const noexcept override
    {
        return children_.size();
    }

    Node* childAt(std::size_t index) const override
    {
        return index < children_.size() ? children_[index] : nullptr;
    }

    void addChild(Node* child)
    {
        children_.push_back(child);
    }

  private:
    Node*               parent_;
    std::vector<Node*> children_;
};

} // namespace

TEST(IHierarchyNodeTest, BuildsTreeAndTraverses)
{
    Node root(nullptr);
    Node child1(&root);
    Node child2(&root);
    Node grandchild1(&child1);
    root.addChild(&child1);
    root.addChild(&child2);
    child1.addChild(&grandchild1);

    EXPECT_EQ(root.childCount(), 2u);
    EXPECT_EQ(child1.childCount(), 1u);
    EXPECT_EQ(root.childAt(0), &child1);
    EXPECT_EQ(root.childAt(1), &child2);
    EXPECT_EQ(root.childAt(5), nullptr);
    EXPECT_EQ(child1.parent(), &root);
    EXPECT_EQ(root.parent(), nullptr);
}

TEST(IHierarchyNodeTest, IsAncestorOf)
{
    Node root(nullptr);
    Node child1(&root);
    Node child2(&root);
    Node grandchild1(&child1);
    root.addChild(&child1);
    root.addChild(&child2);
    child1.addChild(&grandchild1);

    // Direct and transitive ancestors.
    EXPECT_TRUE(root.isAncestorOf(&child1));
    EXPECT_TRUE(root.isAncestorOf(&grandchild1));
    EXPECT_TRUE(child1.isAncestorOf(&grandchild1));

    // Siblings and unrelated branches are not ancestors.
    EXPECT_FALSE(child1.isAncestorOf(&child2));
    EXPECT_FALSE(child2.isAncestorOf(&grandchild1));
    EXPECT_FALSE(grandchild1.isAncestorOf(&root));

    // A node is not its own ancestor; null is not a valid query target.
    EXPECT_FALSE(root.isAncestorOf(&root));
    EXPECT_FALSE(root.isAncestorOf(nullptr));
}

TEST(IHierarchyNodeTest, IsDescendantOf)
{
    Node root(nullptr);
    Node child1(&root);
    Node child2(&root);
    Node grandchild1(&child1);
    root.addChild(&child1);
    root.addChild(&child2);
    child1.addChild(&grandchild1);

    // Direct and transitive descendants.
    EXPECT_TRUE(child1.isDescendantOf(&root));
    EXPECT_TRUE(grandchild1.isDescendantOf(&root));
    EXPECT_TRUE(grandchild1.isDescendantOf(&child1));

    // Siblings and unrelated branches are not descendants.
    EXPECT_FALSE(child2.isDescendantOf(&child1));
    EXPECT_FALSE(grandchild1.isDescendantOf(&child2));
    EXPECT_FALSE(root.isDescendantOf(&grandchild1));

    // A node is not its own descendant; null is not a valid query target.
    EXPECT_FALSE(root.isDescendantOf(&root));
    EXPECT_FALSE(root.isDescendantOf(nullptr));

    // Inverse relationship with isAncestorOf.
    EXPECT_TRUE(root.isAncestorOf(&grandchild1));
    EXPECT_TRUE(grandchild1.isDescendantOf(&root));
}
