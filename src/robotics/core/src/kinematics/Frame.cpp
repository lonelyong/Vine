#include <vine/robotics/kinematics/Frame.hpp>

#include <algorithm>
#include <stdexcept>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

namespace
{

/**
 * @brief Returns the root frame of the tree that frame belongs to.
 */
raw_ptr<const Frame> rootOf(raw_ptr<const Frame> frame)
{
    raw_ptr<const Frame> root = frame;
    while (root->parent()) {
        root = root->parent();
    }
    return root;
}

} // namespace

Frame::Frame() = default;

Frame::Frame(FrameType type)
  : type_(type)
  , dof_(dofOfType(type))
{}

std::size_t Frame::dofOfType(FrameType type) noexcept
{
    switch (type) {
        case FrameType::PlanarJoint: return 3;
        case FrameType::Fixed: return 0;
        case FrameType::RevoluteJoint:
        case FrameType::PrismaticJoint: return 1;
    }
    return 0;
}

void Frame::addChild(raw_ptr<Frame> child)
{
    if (!child) {
        throw std::invalid_argument("Frame::addChild, child is null");
    }
    if (child == this) {
        throw std::invalid_argument("Frame::addChild, cannot add self as child");
    }
    if (child->parent_) {
        throw std::invalid_argument("Frame::addChild, child already has a parent");
    }
    child->parent_ = this;
    children_.push_back(child);
}

void Frame::removeChild(raw_ptr<Frame> child)
{
    if (!child) {
        throw std::invalid_argument("Frame::removeChild, child is null");
    }
    if (child == this) {
        throw std::invalid_argument("Frame::removeChild, cannot remove self");
    }
    const auto it = std::find(children_.begin(), children_.end(), child);
    if (it == children_.end()) {
        throw std::invalid_argument("Frame::removeChild, child is not a direct child");
    }
    children_.erase(it);
    child->parent_ = nullptr;
}

math::Isometry3d Frame::frameInWorld(raw_ptr<const Frame> frame, const State& state)
{
    if (!frame) {
        throw std::invalid_argument("Frame::frameInWorld, frame is null.");
    }
    math::Isometry3d tf = frame->transform(state);
    for (raw_ptr<const Frame> cur = frame->parent(); cur; cur = cur->parent()) {
        tf = cur->transform(state) * tf;
    }
    return tf;
}

math::Isometry3d Frame::frameInFrame(raw_ptr<const Frame> from, raw_ptr<const Frame> to, const State& state)
{
    if (!from || !to) {
        throw std::invalid_argument("Frame::frameInFrame, from or to is null.");
    }
    if (from == to) {
        return math::Isometry3d{};
    }
    if (rootOf(from) != rootOf(to)) {
        throw std::invalid_argument("Frame::frameInFrame, frames are not in the same world.");
    }
    return frameInWorld(to, state).inverted() * frameInWorld(from, state);
}

VN_ROBOTICS_KINEMATICS_NS_END