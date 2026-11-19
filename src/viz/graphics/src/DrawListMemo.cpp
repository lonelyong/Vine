#include "DrawListMemo.hpp"

#include <algorithm>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

DrawListMemo::Entry*
DrawListMemo::find(const Mat4d& view_proj, const Vec3d& eye, const Viewport& viewport, std::uint32_t layer_mask, std::uint64_t revision)
{
    // EXACT equality is the right test: the memo caches a pure function of the view, so a camera that happens to
    // produce the same matrices must reuse the entry (its result is identical by construction).
    for (Entry& entry : entries_) {
        if (entry.revision == revision && entry.eye == eye && entry.layer_mask == layer_mask &&
            (!entry.viewport_sensitive || (entry.viewport.width == viewport.width && entry.viewport.height == viewport.height)) &&
            entry.view_proj == view_proj)
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
                                         std::shared_ptr<const std::vector<DrawItem>> commands,
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
