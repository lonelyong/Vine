#include <vine/graphics/Group.hpp>

#include <algorithm>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(Group, Node);

Group::Group() = default;

Group::~Group() = default;

void Group::addChild(intrusive_ptr<Node> child)
{
    if (child == nullptr) {
        return;
    }
    // Reject a re-parent that would close a cycle: if @p child is this node or
    // any of its ancestors, adopting it would make the graph non-tree, and
    // every tree traversal (command collection, bounds, picking, findNode)
    // recurses forever on it — a stack overflow rather than a visible error.
    // The graph is also the invariant the per-traversal bounds cache relies on
    // (a node has exactly one world matrix within one traversal).
    for (const Node* ancestor = this; ancestor != nullptr; ancestor = ancestor->parent_) {
        if (ancestor == child.get()) {
            return;
        }
    }
    // Detach from an existing parent (only Groups own children) before
    // re-parenting.
    if (child->parent_ != nullptr) {
        if (auto* old_parent = dynamic_cast<Group*>(child->parent_)) {
            old_parent->removeChild(child.get());
        }
    }
    child->parent_ = this;
    children_.emplace_back(std::move(child));
}

void Group::removeChild(raw_ptr<Node> child)
{
    if (child == nullptr) {
        return;
    }
    auto it = std::find_if(children_.begin(), children_.end(),
                           [child](const NodePtr& ptr) { return ptr.get() == child; });
    if (it != children_.end()) {
        child->parent_ = nullptr;
        children_.erase(it);
    }
}

std::vector<NodePtr> Group::children() const
{
    return children_;
}

const std::vector<NodePtr>& Group::childrenRef() const noexcept
{
    return children_;
}

Aabbd Group::boundingBox() const
{
    Aabbd box = Aabbd::empty();
    for (const auto& child : children_) {
        const Aabbd child_box = child->boundingBox();
        if (child_box.isValid()) {
            box.expandBy(child_box);
        }
    }
    return box;
}

V_GRAPHICS_NS_END
