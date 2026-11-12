#include "SnapshotSource.hpp"

#include <utility>
#include <vector>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

void SnapshotSource::setSnapshot(SceneSnapshotPtr snapshot)
{
    const std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = std::move(snapshot);
    // The memo belongs to the engine thread, so it is NOT cleared here: a new snapshot has a new revision, every
    // old entry is a miss, and `beginFrame` drops the ones nothing asked for. Clearing here would race a collection.
}

SceneSnapshotPtr SnapshotSource::snapshot() const
{
    const std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

void SnapshotSource::beginFrame(std::uint64_t frame)
{
    // ONE FRAME OF GRACE, exactly like RenderWorldSource and Scene: evict what the PREVIOUS frame asked for, then
    // move to the new token. Evicting with the NEW frame would drop every list that was used last frame.
    memo_.beginFrame(frame_);
    frame_ = frame;
}

std::uint64_t SnapshotSource::memoHits() const noexcept
{
    return memo_.hits();
}

std::uint64_t SnapshotSource::memoStores() const noexcept
{
    return memo_.stores();
}

std::shared_ptr<const std::vector<vn::graphics::RenderCommand>> SnapshotSource::collectRenderCommands(
    const vn::graphics::CollectView& view,
    std::uint32_t                    layer_mask) const
{
    SceneSnapshotPtr snapshot;
    {
        const std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot = snapshot_;
    }
    if (snapshot == nullptr) {
        return std::make_shared<const std::vector<vn::graphics::RenderCommand>>();
    }

    const auto view_proj = view.viewProjection();
    if (DrawListMemo::Entry* entry = memo_.find(view_proj, view.eye, view.viewport, layer_mask, snapshot->revision)) {
        entry->last_used_frame = frame_;
        return entry->commands;
    }

    std::vector<vn::graphics::RenderCommand> commands;
    bool                                    viewport_used = false;
    selectSnapshot(*snapshot, view, layer_mask, commands, viewport_used);
    auto shared = std::make_shared<const std::vector<vn::graphics::RenderCommand>>(std::move(commands));
    (void)memo_.store(view_proj, view.eye, view.viewport, layer_mask, viewport_used, snapshot->revision, shared, frame_);
    return shared;
}

} // namespace detail

VN_GRAPHICS_NS_END
