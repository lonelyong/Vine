#include "RenderWorld.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

void RenderWorld::buildFrom(const Node& root, float opacity)
{
    clear();
    bake_.seed.parent_opacity = opacity;
    (void)bakeSubtree(root, bake_);
    (void)resolveSubtree(bake_, 0U);
    index_.reserve(bake_.rows.size());
    for (std::uint32_t index = 0; index < bake_.rows.size(); ++index) {
        if (const Node* node = bake_.rows[index].node) {
            index_.emplace(node, index);
        }
    }
}

void RenderWorld::clear()
{
    bake_.rows.clear();
    bake_.fallbacks.clear();
    index_.clear();
    rows_recomputed_ = 0;
    refusals_        = 0;
}

bool RenderWorld::place(const Node& node, const Mat4d& local)
{
    const auto found = index_.find(&node);
    if (found == index_.end()) {
        ++refusals_;
        return false; // not in this world: it was built from another graph, or the node is not visible
    }
    Row& row = bake_.rows[found->second];
    if (row.dependence != ViewDependence::None) {
        ++refusals_;
        return false; // its placement is a function of the view: only the walk that knows the camera may answer
    }
    if (row.box_from_node) {
        ++refusals_;
        return false; // its box could only be read from a node, so this world cannot re-place it from values
    }
    row.local = local;
    rows_recomputed_ += resolveSubtree(bake_, found->second);
    return true;
}

void RenderWorld::collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const
{
    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection());
    bool          viewport_used = false;
    selectFromBake(bake_, frustum, view, layer_mask, viewport_used, out);
    sortCollected(out, view.eye);
}

std::size_t RenderWorld::rowCount() const noexcept
{
    return bake_.rows.size();
}

std::uint64_t RenderWorld::rowsRecomputed() const noexcept
{
    return rows_recomputed_;
}

std::uint64_t RenderWorld::refusals() const noexcept
{
    return refusals_;
}

std::uint32_t RenderWorld::refreshBoxes()
{
    return detail::refreshBoxes(bake_);
}

} // namespace detail

VN_GRAPHICS_NS_END
