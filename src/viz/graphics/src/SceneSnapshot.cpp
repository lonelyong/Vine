#include "SceneSnapshot.hpp"

#include <algorithm>
#include <utility>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

SceneSnapshotPtr makeSceneSnapshot(const SceneBake&        rows,
                                   std::uint64_t           epoch,
                                   std::uint64_t           boxes,
                                   std::uint64_t           materials,
                                   const SceneSnapshotPtr& previous)
{
    auto snapshot        = std::make_shared<SceneSnapshot>();
    snapshot->row_count  = rows.rows.size();
    snapshot->epoch      = epoch;
    snapshot->boxes      = boxes;
    snapshot->materials  = materials;
    snapshot->identities.reserve(snapshot->row_count);
    snapshot->node_epochs.reserve(snapshot->row_count);

    const bool have_epochs = rows.nodes.size() == rows.rows.size();
    for (std::size_t index = 0; index < snapshot->row_count; ++index) {
        snapshot->identities.push_back(rows.rows[index].identity);
        snapshot->node_epochs.push_back(have_epochs && rows.nodes[index] != nullptr ? rows.nodes[index]->contentEpoch() : 0U);
    }

    const std::size_t chunk_count =
        (snapshot->row_count + SceneSnapshot::kRowsPerChunk - 1U) / SceneSnapshot::kRowsPerChunk;
    snapshot->chunks.reserve(chunk_count);

    // A box or material refresh edits rows in place without moving a node epoch, so those versions disable sharing
    // entirely rather than serving a stale chunk.
    const bool may_share = previous != nullptr && have_epochs && previous->row_count == snapshot->row_count &&
                           previous->boxes == boxes && previous->materials == materials &&
                           previous->chunks.size() == chunk_count;

    for (std::size_t chunk = 0; chunk < chunk_count; ++chunk) {
        const std::size_t first = chunk * SceneSnapshot::kRowsPerChunk;
        const std::size_t last  = std::min(first + SceneSnapshot::kRowsPerChunk, snapshot->row_count);

        bool share = may_share;
        for (std::size_t index = first; share && index < last; ++index) {
            const Row& current = rows.rows[index];
            const Row& prior   = previous->row(index);
            share = current.identity == prior.identity && current.parent == prior.parent && current.end == prior.end &&
                    current.child_count == prior.child_count &&
                    snapshot->node_epochs[index] == previous->node_epochs[index];
        }
        if (share) {
            snapshot->chunks.push_back(previous->chunks[chunk]);
            continue;
        }
        auto copied = std::make_shared<std::vector<Row>>(rows.rows.begin() + static_cast<std::ptrdiff_t>(first),
                                                         rows.rows.begin() + static_cast<std::ptrdiff_t>(last));
        snapshot->chunks.push_back(std::move(copied));
    }

    return snapshot;
}

void selectSnapshot(const SceneSnapshot&        snapshot,
                    const CollectView&          view,
                    std::uint32_t               layer_mask,
                    std::vector<RenderCommand>& out,
                    bool&                       viewport_used)
{
    out.clear();
    viewport_used = false;
    if (snapshot.row_count == 0U) {
        return;
    }
    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection());
    selectRows(static_cast<std::uint32_t>(snapshot.row_count),
               [&snapshot](std::uint32_t index) -> const Row& { return snapshot.row(index); },
               frustum,
               view,
               layer_mask,
               viewport_used,
               out);
    sortCollected(out, view.eye);
}

} // namespace detail

VN_GRAPHICS_NS_END
