#pragma once

#include <cstdint>

#include <vine/vsg/core/SlotProbe.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The per-frame ring: the storage of everything that changes every frame, laid out so a steady frame
 * writes in place and never grows.
 *
 * WHAT LIVES HERE. The view block of each pass and the draw block of each draw - matrices, opacities, camera
 * values. These change every frame by definition, so they cannot be cached the way materials are; what they
 * can do is be written into the SAME bytes every frame. That is what makes the steady state measurable:
 * a frame's storage footprint is a constant, its allocations are zero, and `highWater()` is the number a
 * phase gates on instead of hoping.
 *
 * WHY IT CANNOT GROW. Growth is the failure mode this type is built to make impossible: an application that
 * adds drawables mid-session must be told that the storage is full (and rebuild it as a deliberate, one-off
 * act), never have a "convenient" resize silently relocate a buffer a submitted command buffer still names.
 * So a reservation past the frame's budget is INVALID and counted, and the caller decides what to do.
 *
 * WHY THE SLAB ROTATION, AND WHY THERE ARE MORE SLABS THAN FRAMES. Frames in flight read the ring at the same
 * time, and "in flight" reaches FURTHER BACK than the previous frame: the framework proves a frame finished
 * only when the slot that recorded it is recycled, and that fence wait happens inside the submit of a later
 * frame - after that frame has written its own bytes (see core::perFrameCopies). So the slab a frame writes is
 * the one the OLDEST frame still allowed to be in flight is reading, and the ring owns ONE SLAB MORE than
 * there are frames in flight. With exactly as many slabs as frames, that oldest frame shades with the newest
 * frame's matrices: a wrong frame rather than a lost one, and with a moving camera a visibly wrong one.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

namespace core
{

/** @brief Per-frame storage: a fixed ring of blocks, one slab per frame that may still read it. */
class FrameRing
{
  public:
    /** @brief The ring's shape. */
    struct Layout
    {
        std::uint64_t stride{80};            ///< Bytes one reservation holds (a draw block, a view block).
        std::uint64_t alignment{256};        ///< Offset alignment; the effective stride is rounded up to it.
        /// Slabs owned, one MORE than the frames that may be in flight (see the file note, and
        /// core::perFrameCopies for the count and why the extra slab is what makes a frame correct).
        std::uint32_t slabs{perFrameCopies(kAssumedInFlightSlots)};
        std::uint32_t blocks_per_frame{512}; ///< Reservations a single frame may make.
    };

    /** @brief One reservation: where the block is, or that the ring refused. */
    struct Reservation
    {
        bool          valid{false};  ///< false when the frame's budget was exhausted (counted, never grown).
        std::uint64_t offset{0};     ///< Byte offset of the block (meaningful when valid).
    };

  public:
    /** @brief Constructs a ring with the given layout.
     *
     * @param layout Stride, alignment, slot count and per-frame budget; every field is clamped to at least 1.
     */
    explicit FrameRing(const Layout& layout);

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

  public:
    /** @brief Begins the next frame: rotates to the next slab and resets the frame's cursor.
     *
     * The first call begins frame 0, so the caller that calls this at the top of every frame sees the frames
     * numbered from zero.
     */
    void beginFrame() noexcept;

    /** @brief Reserves one block in the current frame's slab.
     *
     * @return The block's offset, or `valid == false` when this frame already made
     *         `blocks_per_frame` reservations (the caller reports it; growing is not an option).
     */
    [[nodiscard]] Reservation reserve() noexcept;

  public:
    /** @brief Gets the bytes one reservation occupies, rounded up to the alignment. */
    [[nodiscard]] std::uint64_t stride() const noexcept;

    /** @brief Gets the offset alignment blocks start at. */
    [[nodiscard]] std::uint64_t alignment() const noexcept;

    /** @brief Gets the bytes one slab occupies (one frame's worth). */
    [[nodiscard]] std::uint64_t slabBytes() const noexcept;

    /** @brief Gets the bytes the whole ring occupies (what the API layer allocates, once). */
    [[nodiscard]] std::uint64_t capacityBytes() const noexcept;

    /** @brief Gets the number of frames begun (0 before the first @ref beginFrame). */
    [[nodiscard]] std::uint64_t frames() const noexcept;

    /** @brief Gets the current slab index. */
    [[nodiscard]] std::uint32_t slot() const noexcept;

    /** @brief Gets the reservations made in the current frame. */
    [[nodiscard]] std::uint32_t reserved() const noexcept;

    /** @brief Gets the most reservations any single frame made. */
    [[nodiscard]] std::uint32_t highWater() const noexcept;

    /** @brief Gets the number of reservations refused because the frame's budget was exhausted. */
    [[nodiscard]] std::uint64_t overflows() const noexcept;

  private:
    std::uint64_t stride_{1};
    std::uint64_t alignment_{1};
    std::uint64_t slab_bytes_{1};
    std::uint32_t slabs_{1};
    std::uint32_t blocks_per_frame_{1};
    bool          started_{false};
    std::uint64_t frames_{0};
    std::uint32_t reserved_{0};
    std::uint32_t high_water_{0};
    std::uint64_t overflows_{0};
};

}  // namespace core

VN_VSG_NS_END
