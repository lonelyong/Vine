#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief THE COLLECTED LISTS of the views in use: one per (view, mask), reused while the rows do not change.
 *
 * @details
 * - WHY ONE UNIT AND NOT TWO. A collected list is a pure function of (content state, view, mask), so the memo that
 *   serves it belongs to the thing that answers a view - and BOTH answers have it: the `Scene` (whose content is the
 *   graph) and a content's mirror (whose content is its rows). Written twice it would drift, and the drift would be
 *   invisible: the picture would be right and one of the two would simply do more work (G1/G5).
 * - THE KEY IS THE VIEW, NOT THE CAMERA. Two passes that state the same camera share an entry by construction, a camera
 *   edited between two passes misses (and gets a fresh collection), and nothing is assumed about the camera's lifetime -
 *   a camera may be a stack object (see RenderPass::setCamera), so an owning key would be wrong and a bare address key
 *   could not notice an edit in place.
 * - THE MASK AND (CONDITIONALLY) THE VIEWPORT ARE PART OF THE KEY. Two passes may draw one camera and still want
 *   different lists (a shadow pass drawing only casters), and a view-dependent node that scales to the device-pixel
 *   rectangle produces a different list per rectangle - so a list built by such a node is only reused for the same
 *   one. A list without such a node is shared across rectangles (see `viewport_sensitive`).
 * - IT IS BOUNDED BY THE VIEWS DRAWN, NOT BY FRAMES: an entry nothing asked for in the previous frame is dropped by
 *   `beginFrame`, so a closed pane or a camera that went away releases its storage instead of owning it forever.
 * - THE `revision` IS THE CALLER'S. It says which state the list was built from (a scene's content revision, a mirror's
 *   applied-delta count); a different revision is a miss. That is the one thing the two callers do not share, and
 *   passing it in keeps the memo from knowing where rows come from.
 *
 * @note Not thread-safe: one owner, one thread (the one that answers its views).
 */
class DrawListMemo {
  public:
    /// One view's collected list.
    struct Entry {
        Mat4d   view_proj;
        Vec3d   eye;
        Viewport viewport;
        /// The caller's state version the list was collected from (see the class note).
        std::uint64_t revision = 0;
        /// Layers this list draws - part of the key, not only a parameter (see the class note).
        std::uint32_t layer_mask = kAllLayers;
        /// True when the list was built by a content that also depends on the device-pixel viewport.
        bool viewport_sensitive = false;
        /// SHARED and immutable once built: every pass of a frame hands the same list around.
        std::shared_ptr<const std::vector<RenderCommand>> commands;
        /// The frame that asked for this list last: an entry nothing asked for in the previous frame is dropped.
        std::uint64_t last_used_frame = 0;
    };

  public:
    DrawListMemo() = default;

  public:
    /**
     * @brief Finds a list this view may reuse at @p revision.
     *
     * @param view_proj    The view-projection (the key's identity).
     * @param eye          The eye point the sort keys were measured from.
     * @param viewport     The device-pixel rectangle the caller draws into.
     * @param layer_mask   Layers the caller draws.
     * @param revision     The caller's state version.
     * @return The entry, or null when the caller has to collect.
     */
    [[nodiscard]] Entry* find(const Mat4d& view_proj, const Vec3d& eye, const Viewport& viewport, std::uint32_t layer_mask, std::uint64_t revision);

    /**
     * @brief Stores (or replaces) the list for a view that just missed.
     *
     * @param view_proj          The view-projection.
     * @param eye                The eye point the sort keys were measured from.
     * @param viewport           The device-pixel rectangle the caller draws into.
     * @param layer_mask         Layers the list draws.
     * @param viewport_sensitive Whether the list depends on the device-pixel rectangle.
     * @param revision           The caller's state version the list was built from.
     * @param commands           The list itself (shared, immutable once built).
     * @param frame              The frame asking for it (see beginFrame).
     * @return The stored entry (valid until the next store).
     */
    Entry& store(const Mat4d&                     view_proj,
                 const Vec3d&                     eye,
                 const Viewport&                  viewport,
                 std::uint32_t                    layer_mask,
                 bool                             viewport_sensitive,
                 std::uint64_t                    revision,
                 std::shared_ptr<const std::vector<RenderCommand>> commands,
                 std::uint64_t                    frame);

    /**
     * @brief Drops every list nothing asked for in the previous frame, and starts a new one.
     *
     * @param frame The frame about to be asked (0 means "no frame is open": nothing is evicted).
     */
    void beginFrame(std::uint64_t frame);

    /** @brief Drops every list: the state they were built from is gone. */
    void clear();

  public:
    /** @brief Gets how many lists this memo holds. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** @brief Gets how many entries this memo has handed out (the per-view reuse witness). */
    [[nodiscard]] std::uint64_t hits() const noexcept;

    /** @brief Gets how many lists have been stored (the per-view walk witness). */
    [[nodiscard]] std::uint64_t stores() const noexcept;

  private:
    std::vector<Entry> entries_;
    std::uint64_t      hits_{ 0 };
    std::uint64_t      stores_{ 0 };
};

} // namespace detail

VN_GRAPHICS_NS_END
