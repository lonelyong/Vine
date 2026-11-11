#include "RenderWorld.hpp"

#include <memory>
#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

void RenderWorld::adopt(SceneBake rows)
{
    clear();
    bake_ = std::move(rows);
    index_.reserve(bake_.rows.size());
    for (std::uint32_t index = 0; index < bake_.rows.size(); ++index) {
        const RowId identity = bake_.rows[index].identity;
        if (identity != nullptr) {
            index_.emplace(identity, index);
        }
    }
}

void RenderWorld::clear()
{
    bake_.rows.clear();
    bake_.fallbacks.clear();
    bake_.seed = SceneBake::Seed{};
    index_.clear();
    rows_recomputed_ = 0;
    refusals_        = 0;
}

bool RenderWorld::contains(RowId identity) const noexcept
{
    return index_.find(identity) != index_.end();
}

bool RenderWorld::place(RowId identity, const Mat4d& local)
{
    const auto found = index_.find(identity);
    if (found == index_.end()) {
        ++refusals_;
        return false; // not in this world: adopted from another graph, or the row is not visible
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
    const Frustum frustum       = Frustum::fromViewProjection(view.viewProjection());
    bool          viewport_used = false;
    selectFromBake(bake_, frustum, view, layer_mask, viewport_used, out);
    sortCollected(out, view.eye);
}

std::uint32_t RenderWorld::refreshBoxes()
{
    return detail::refreshBoxes(bake_);
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

RenderWorldSource::RenderWorldSource(RenderWorld& world) noexcept
  : world_(&world)
{}

std::shared_ptr<const std::vector<RenderCommand>> RenderWorldSource::collectRenderCommands(const vn::graphics::CollectView& view,
                                                                                          std::uint32_t                    layer_mask) const
{
    // The frame asks the same question of either side (see DrawListSource), and the answer is shared because every
    // pass of this frame that draws the view names this one list. Built once per ask, like a Scene's cold path.
    auto commands = std::make_shared<std::vector<RenderCommand>>();
    world_->collect(view, layer_mask, *commands);
    return commands;
}

} // namespace detail

VN_GRAPHICS_NS_END
