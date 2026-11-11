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
    // A mirror holds VALUES. The node record is the content's own bookkeeping (it is what refreshes a box when bytes
    // announce themselves), and this side must not name a node - so it is dropped here, once, where the handshake
    // happens (see SceneBake::nodes).
    bake_.nodes.clear();
    rebuildIndex();
}

void RenderWorld::clear()
{
    bake_.rows.clear();
    bake_.nodes.clear();
    bake_.seed = SceneBake::Seed{};
    index_.clear();
    rows_recomputed_ = 0;
    refusals_        = 0;
}

bool RenderWorld::contains(RowId identity) const noexcept
{
    return index_.find(identity) != index_.end();
}

bool RenderWorld::place(RowId identity, const Mat4d& local, ViewPlacementPtr placement)
{
    const auto found = index_.find(identity);
    if (found == index_.end()) {
        ++refusals_;
        return false; // not in this world: adopted from another graph, or the row is not visible
    }
    Row& row      = bake_.rows[found->second];
    row.local     = local;
    row.placement = std::move(placement);
    rows_recomputed_ += resolveSubtree(bake_, found->second);
    return true;
}

bool RenderWorld::setState(RowId identity, const StateValues& values)
{
    const auto found = index_.find(identity);
    if (found == index_.end()) {
        ++refusals_;
        return false;
    }
    Row& row           = bake_.rows[found->second];
    row.own_opacity    = values.opacity;
    row.own_layers     = values.layers;
    row.own_draw_order = values.draw_order;
    rows_recomputed_ += resolveSubtree(bake_, found->second);
    return true;
}

bool RenderWorld::setDescription(RowId identity, const SceneBake& block)
{
    const auto found = index_.find(identity);
    if (found == index_.end() || block.rows.size() != 1U) {
        ++refusals_;
        return false;
    }
    const std::uint32_t row_index    = found->second;
    const Row           structure    = bake_.rows[row_index]; // identity / parent / end / child count are the table's
    bake_.rows[row_index]            = block.rows.front();
    Row& row                         = bake_.rows[row_index];
    row.identity                     = structure.identity;
    row.parent                       = structure.parent;
    row.end                          = structure.end;
    row.child_count                  = structure.child_count;
    rows_recomputed_ += resolveSubtree(bake_, row_index);
    return true;
}

bool RenderWorld::attach(RowId parent_identity, std::uint32_t child_index, SceneBake block)
{
    const auto found = index_.find(parent_identity);
    if (found == index_.end()) {
        ++refusals_;
        return false; // not a row of this world: nothing to hang the subtree under
    }
    const std::uint32_t parent = found->second;
    const std::uint32_t count  = static_cast<std::uint32_t>(block.rows.size());
    if (count == 0U) {
        return false; // an invisible subtree states no rows: nothing changed (see buildOwnFacts)
    }

    // WHERE: past the children before @p child_index, in walk order.
    std::uint32_t at          = parent + 1U;
    const std::uint32_t p_end = bake_.rows[parent].end;
    for (std::uint32_t slot = 0U; slot < child_index && at < p_end; ++slot) {
        at = bake_.rows[at].end;
    }

    bake_.rows.insert(bake_.rows.begin() + at, block.rows.begin(), block.rows.end());

    // RENUMBER. The block's indices were relative to a table of its own, and every row after the insertion point - as
    // well as every row whose subtree CONTAINS it - moves by the block's size.
    for (std::uint32_t row = at; row < at + count; ++row) {
        Row& inserted = bake_.rows[row];
        inserted.parent = inserted.parent == SceneBake::kNoParent ? parent : inserted.parent + at;
        inserted.end += at;
    }
    const auto shift = [&](std::uint32_t begin, std::uint32_t finish) {
        for (std::uint32_t row = begin; row < finish; ++row) {
            Row& existing = bake_.rows[row];
            if (existing.parent != SceneBake::kNoParent && existing.parent >= at) {
                existing.parent += count;
            }
            if (existing.end >= at) {
                existing.end += count;
            }
        }
    };
    shift(0U, at);
    shift(at + count, static_cast<std::uint32_t>(bake_.rows.size()));
    ++bake_.rows[parent].child_count;

    rebuildIndex();
    rows_recomputed_ += resolveSubtree(bake_, at);
    return true;
}

bool RenderWorld::detach(RowId identity)
{
    const auto found = index_.find(identity);
    if (found == index_.end() || bake_.rows[found->second].parent == SceneBake::kNoParent) {
        ++refusals_;
        return false; // unknown, or the root: a whole-table change is adopt's job
    }
    const std::uint32_t first  = found->second;
    const std::uint32_t last   = bake_.rows[first].end;
    const std::uint32_t count  = last - first;
    const std::uint32_t parent = bake_.rows[first].parent;

    bake_.rows.erase(bake_.rows.begin() + first, bake_.rows.begin() + last);
    for (Row& row : bake_.rows) {
        if (row.parent != SceneBake::kNoParent && row.parent >= last) {
            row.parent -= count;
        }
        if (row.end >= last) {
            row.end -= count;
        }
    }
    if (bake_.rows[parent].child_count > 0U) {
        --bake_.rows[parent].child_count;
    }

    rebuildIndex();
    // Only BOXES moved above it (a union lost a child), so the ancestors are re-derived and nothing is re-folded.
    refoldChain(bake_, parent);
    return true;
}

void RenderWorld::rebuildIndex()
{
    index_.clear();
    index_.reserve(bake_.rows.size());
    for (std::uint32_t row = 0U; row < bake_.rows.size(); ++row) {
        const RowId identity = bake_.rows[row].identity;
        if (identity != nullptr) {
            index_.emplace(identity, row);
        }
    }
}

void RenderWorld::collect(const CollectView& view, std::uint32_t layer_mask, std::vector<RenderCommand>& out) const
{
    const Frustum frustum       = Frustum::fromViewProjection(view.viewProjection());
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
