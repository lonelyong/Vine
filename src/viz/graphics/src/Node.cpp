#include <vine/graphics/Node.hpp>

#include <typeinfo>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(Node, vn::Object);

Aabbd transformBox(const Aabbd& local, const Mat4d& world)
{
    Aabbd result = Aabbd::empty();
    if (!local.isValid()) {
        return result;
    }
    const auto mn = local.min();
    const auto mx = local.max();
    const vn::math::Point3d corners[8] = {
        mn,
        vn::math::Point3d(mx.x, mn.y, mn.z),
        vn::math::Point3d(mn.x, mx.y, mn.z),
        vn::math::Point3d(mx.x, mx.y, mn.z),
        vn::math::Point3d(mn.x, mn.y, mx.z),
        vn::math::Point3d(mx.x, mn.y, mx.z),
        vn::math::Point3d(mn.x, mx.y, mx.z),
        mx,
    };
    for (const auto& c : corners) {
        const auto p = world * c;
        result.expandBy(vn::math::Vec3d(p.x, p.y, p.z));
    }
    return result;
}

void Node::invalidateBounds() noexcept
{
    // This node's box and every ancestor's union may now be stale. Only UP: a subtree box lives in the
    // node's own frame (see subtreeBounds), so nothing below this node depended on what changed here.
    for (Node* node = this; node != nullptr; node = node->parent_) {
        ++node->bounds_stamp_;
    }
}

bool Node::subtreeBounds(Aabbd& box) const
{
    // A leaf's box is its DATA box, and localBounds() re-checks its own data key on every ask (cheap:
    // a few compares against the cached scan), so a leaf needs no stamp and answers fresh.
    if (typeid(*this) == typeid(Geometry)) {
        box = static_cast<const Geometry*>(this)->localBounds();
        return true;
    }
    if (const auto* group = dynamic_cast<const Group*>(this)) {
        if (cached_known_ && cached_stamp_ == bounds_stamp_) {
            box = cached_bounds_;
            return true;
        }
        ++bounds_recomputes_;
        Aabbd subtree  = Aabbd::empty();
        bool  complete = true;
        for (const auto& child : group->childrenRef()) {
            Aabbd child_box = Aabbd::empty();
            if (!child->subtreeBounds(child_box)) {
                complete = false;
                break;
            }
            subtree.expandBy(transformBox(child_box, child->localTransformMatrix()));
        }
        if (!complete) {
            // Some node below answers only in world space (a custom leaf): this frame's box is UNKNOWN.
            // The cache stays invalid (no stamp is stored) so every collect takes the from-scratch path -
            // correct, and the documented price of a node the engine cannot see inside.
            cached_known_ = false;
            return false;
        }
        cached_bounds_ = subtree;
        cached_stamp_  = bounds_stamp_;
        cached_known_  = true;
        box            = cached_bounds_;
        return true;
    }
    // An exact plain Node has no content and no children: it answers empty. Any OTHER class answers
    // through its own boundingBox() only, which lives in world space and cannot be expressed in this
    // frame - unknown, so the walk falls back (and stays sound for any subclass).
    if (typeid(*this) == typeid(Node)) {
        box = Aabbd::empty();
        return true;
    }
    return false;
}

std::uint64_t Node::boundsRecomputeCount() const noexcept
{
    return bounds_recomputes_;
}

Node::Node() = default;

Node::~Node() = default;

String Node::name() const
{
    return name_;
}

void Node::setName(const String& name)
{
    name_ = name;
}

bool Node::isVisible() const
{
    return visible_;
}

void Node::setVisible(bool visible)
{
    visible_ = visible;
}

float Node::opacity() const
{
    return opacity_;
}

void Node::setOpacity(float opacity)
{
    opacity_ = opacity;
}

raw_ptr<Node> Node::parent() const
{
    return parent_;
}

Aabbd Node::boundingBox() const
{
    // Base Node is neither a container nor a leaf renderable, so it has no
    // extent of its own; concrete kinds (Group/Geometry) override this.
    return Aabbd::empty();
}

Mat4d Node::worldMatrix() const
{
    // Fold the local matrices from the top of the chain down to this node.
    // Non-transform nodes contribute the identity, so only enclosing
    // MatrixTransforms (and this node itself when it is one) affect the
    // result.
    //
    // Walked bottom-up with a left-multiply per level: the previous recursive
    // form recomputed the whole ancestor chain at every level, i.e. O(depth^2)
    // matrix products for a chain of depth d, on a call every leaf makes once
    // per pass. This is O(depth) and produces the identical matrix.
    Mat4d       world = localTransformMatrix();
    const Node* p     = parent_;
    while (p != nullptr) {
        world = p->localTransformMatrix() * world;
        p     = p->parent_;
    }
    return world;
}

Mat4d Node::localTransformMatrix() const
{
    return Mat4d();
}

VN_GRAPHICS_NS_END
