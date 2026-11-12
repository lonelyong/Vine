#include "SceneRows.hpp"

#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

SceneSnapshotPtr SceneRows::snapshot(const Scene& scene)
{
    // Ensuring the rows is what makes the answer CURRENT: the build is keyed by the epoch, and the refresh is what
    // notices a box or material value that moved under it. Inside an edit batch the PUBLISHED pair stays where it
    // was, so this returns the previous cached snapshot (or builds the pre-batch one on first use).
    (void)scene.ensureRows();
    const std::uint64_t epoch  = scene.published_epoch_;
    const std::uint64_t boxes  = scene.published_boxes_;
    const std::uint64_t values = scene.published_values_;

    SceneBake* rows = scene.content_bake_.get();
    if (rows == nullptr) {
        return makeSceneSnapshot(SceneBake{}, epoch, boxes, values, nullptr);
    }
    const SceneSnapshotPtr& cached = rows->snapshot_cache;
    if (cached != nullptr && cached->epoch == epoch && cached->boxes == boxes && cached->values == values) {
        return cached; // nothing moved: the previous publication is still the answer
    }

    SceneSnapshotPtr snapshot = makeSceneSnapshot(*rows, epoch, boxes, values, cached);
    rows->snapshot_cache      = snapshot;
    return snapshot;
}

} // namespace detail

VN_GRAPHICS_NS_END
