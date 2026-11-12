#include "DrawListMemo.hpp"

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

namespace
{

/// @brief Returns whether two view-projections are the SAME view (exact element equality).
///
/// @details
/// - Matrix4x4 has no operator==, and the memo only needs "the same view": exact element equality is the right test
///   because the memo caches a pure function of the view, so a camera that happens to produce the same matrices must
///   reuse the entry (its result is identical by construction).
///
/// @param lhs First matrix to compare.
/// @param rhs Second matrix to compare.
/// @return true when every element matches.
bool sameView(const Mat4d& lhs, const Mat4d& rhs)
{
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (lhs(row, col) != rhs(row, col)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

DrawListMemo::Entry*
DrawListMemo::find(const Mat4d& view_proj, const Vec3d& eye, const Viewport& viewport, std::uint32_t layer_mask, std::uint64_t revision)
{
    for (Entry& entry : entries_) {
        if (entry.revision == revision && entry.eye == eye && entry.layer_mask == layer_mask &&
            (!entry.viewport_sensitive || (entry.viewport.width == viewport.width && entry.viewport.height == viewport.height)) &&
            sameView(entry.view_proj, view_proj))
        {
            ++hits_;
            return &entry;
        }
    }
    return nullptr;
}

DrawListMemo::Entry& DrawListMemo::store(const Mat4d&                     view_proj,
                                         const Vec3d&                     eye,
                                         const Viewport&                  viewport,
                                         std::uint32_t                    layer_mask,
                                         bool                             viewport_sensitive,
                                         std::uint64_t                    revision,
                                         std::shared_ptr<const std::vector<RenderCommand>> commands,
                                         std::uint64_t                    frame)
{
    ++stores_;
    entries_.push_back(Entry{ view_proj, eye, viewport, revision, layer_mask, viewport_sensitive, std::move(commands), frame });
    return entries_.back();
}

void DrawListMemo::beginFrame(std::uint64_t frame)
{
    if (frame == 0U) {
        return; // no frame open: nothing may evict what a caller is still holding
    }
    // ONE FRAME OF GRACE, then the entry goes: a list is kept while some view keeps asking for it, so a view that stops
    // being drawn (a closed pane, a camera that went away) releases its storage instead of owning it forever.
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [frame](const Entry& entry) { return entry.last_used_frame < frame; }),
                   entries_.end());
}

void DrawListMemo::clear()
{
    entries_.clear();
}

std::size_t DrawListMemo::size() const noexcept
{
    return entries_.size();
}

std::uint64_t DrawListMemo::hits() const noexcept
{
    // Relaxed: the counter is evidence a host reads through RenderEngine::contentCollectCount, which may be called
    // from the content's thread while the engine half answers (see the class note). It orders nothing.
    return hits_.load(std::memory_order_relaxed);
}

std::uint64_t DrawListMemo::stores() const noexcept
{
    return stores_.load(std::memory_order_relaxed);
}

} // namespace detail

VN_GRAPHICS_NS_END
