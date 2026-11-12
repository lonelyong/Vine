#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/DrawListSource.hpp>
#include <vine/graphics/RenderCommand.hpp>

#include "DrawListMemo.hpp"
#include "SceneSnapshot.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The `DrawListSource` face of an immutable `SceneSnapshot`.
 *
 * @details
 * - WHAT IT IS. The snapshot half of the one question every draw source answers: "what would this view draw".
 *   It reads only the snapshot value, so it answers on the engine thread while the content keeps editing.
 * - WHY IT EXISTS NOW. 5a runs it beside the mirror as a SHADOW: both answer the same view and the frame compares
 *   their lists. 5b makes it the only source and deletes the mirror (see
 *   `../decisions/ADR-2026-11-12-snapshot-takes-over-collection.md`).
 * - THE MEMO IS THE SAME ONE THE SCENE AND THE MIRROR USE. A list is a pure function of (snapshot, view, mask);
 *   two passes that draw the same view must select once. The snapshot's `revision` is the memo's state key.
 * - THREADS. `setSnapshot` runs on the content thread (inside `FrameWorker::publish`); `collectRenderCommands` and
 *   `beginFrame` run on the engine thread. Only the shared pointer is shared, under one mutex; the snapshot is
 *   immutable and the memo is engine-thread-only.
 */
class SnapshotSource final : public vn::graphics::DrawListSource
{
  public:
    SnapshotSource() = default;

    SnapshotSource(const SnapshotSource&)            = delete;
    SnapshotSource& operator=(const SnapshotSource&) = delete;

  public:
    /**
     * @brief Publishes @p snapshot as the value this source answers from.
     *
     * @details
     * - Content thread. A null snapshot means "nothing published yet": every view answers with an empty list.
     * - The source keeps the share, not a copy; a collection already reading the previous snapshot keeps it alive.
     *
     * @param snapshot The immutable publication, or null to clear.
     */
    void setSnapshot(SceneSnapshotPtr snapshot);

    /** @brief Gets the snapshot this source currently answers from (null before the first publication). */
    [[nodiscard]] SceneSnapshotPtr snapshot() const;

    /**
     * @brief Opens @p frame on the memo (see DrawListMemo::beginFrame).
     *
     * @param frame The frame about to ask for views; 0 means none is open.
     */
    void beginFrame(std::uint64_t frame);

    /** @brief Gets how many lists this source has handed out of its memo. */
    [[nodiscard]] std::uint64_t memoHits() const noexcept;

    /** @brief Gets how many lists this source has collected. */
    [[nodiscard]] std::uint64_t memoStores() const noexcept;

  public:
    /**
     * @brief Collects what @p view draws from the current snapshot (see DrawListSource).
     *
     * @param view       The view to collect for.
     * @param layer_mask Layers this picture draws.
     * @return The commands, in draw order; empty when no snapshot was published.
     */
    [[nodiscard]] std::shared_ptr<const std::vector<vn::graphics::RenderCommand>> collectRenderCommands(
        const vn::graphics::CollectView& view,
        std::uint32_t                    layer_mask) const override;

  private:
    mutable std::mutex snapshot_mutex_;
    SceneSnapshotPtr   snapshot_{};
    mutable DrawListMemo memo_;
    std::uint64_t        frame_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
