#include "RenderWorld.hpp"

#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

void RenderWorld::buildFrom(const Node& root, float opacity)
{
    clear();
    (void)bakeSubtree(&root, root.worldMatrix(), opacity, InheritedState{}, false, bake_);
    index_.reserve(bake_.entries.size());
    for (std::uint32_t index = 0; index < bake_.entries.size(); ++index) {
        if (const Node* node = bake_.entries[index].node) {
            index_.emplace(node, index);
        }
    }
}

void RenderWorld::clear()
{
    bake_.entries.clear();
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
        return false; // not in this world: the caller built it from another graph, or the node is not visible
    }
    SceneBake::Entry& row = bake_.entries[found->second];
    if (row.view_dependent) {
        ++refusals_;
        return false; // its placement is a function of the view: only the walk that knows the camera may answer
    }
    row.local = local;
    rows_recomputed_ += refoldSubtree(bake_, found->second);
    return true;
}

void RenderWorld::collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const
{
    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection());
    // Per collection, like the content side's: the boxes are asked per view because a buffer's revision can move
    // without the graph doing so (see SceneBake's note on why the world box is not baked).
    BoundsCache bounds;
    bool        viewport_used = false;
    collectFromBake(bake_, frustum, view, layer_mask, bounds, viewport_used, out);
    sortCollected(out, view.eye);
}

std::size_t RenderWorld::rowCount() const noexcept
{
    return bake_.entries.size();
}

std::uint64_t RenderWorld::rowsRecomputed() const noexcept
{
    return rows_recomputed_;
}

std::uint64_t RenderWorld::refusals() const noexcept
{
    return refusals_;
}

} // namespace detail

VN_GRAPHICS_NS_END
