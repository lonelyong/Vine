#pragma once

/**
 * @brief Parks values until the frames that could still reference them have been submitted.
 *
 * A GPU object whose handle a SUBMITTED command buffer may still name cannot be destroyed the
 * moment it is replaced: that is undefined behaviour, and the validation layer does not necessarily
 * see it (the recording that references the object may be several frames old). The same is true of
 * a per-draw block slot whose offset a frame in flight may still bind, and of a retained node whose
 * subtree a frame in flight may still traverse.
 *
 * All three therefore PARK the value here and release it @ref kDeferredReleaseFrames frame advances
 * later. One clock, one definition: this used to be a bucketed ring for the objects and a separate
 * countdown list for the slots, whose depths had to be kept in step by hand (an assertion was the
 * only thing holding them together).
 *
 * @note An ADVANCE means "one frame has been submitted". A command-buffer slot re-records the frame,
 *       it never reuses a recording, and the viewer waits on a slot's fence before re-recording it —
 *       so a value is safe once kDeferredReleaseFrames advances have happened after it was parked.
 */

#include <vine/vsg/vsg_global.hpp>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

V_VSG_NS_BEGIN

/** @brief Frame advances a parked value waits before it is released.
 *
 * One more than the viewer's command-buffer slot count: the slot that could still reference a parked
 * value has had its fence waited (that wait happens before the slot is re-recorded) once this many
 * frames have been submitted after the park.
 */
inline constexpr std::size_t kDeferredReleaseFrames = 4;

/**
 * @brief A fixed-depth parking queue: park() during a frame, advance() after it is submitted.
 *
 * The values are handed to the advance() callback in the order they were parked, bucket by bucket,
 * and destroyed right after: the callback exists for the parks whose release is not just "drop it"
 * (a slot has to go back to the pool), and a caller that only wants them gone passes nothing.
 *
 * @tparam T Value to park (a ref_ptr, a POD handle, ...).
 */
template <class T>
class VsgDeferredRelease
{
  public:
    /** @brief Parks @p value, to be released kDeferredReleaseFrames advances from now.
     *
     * @param value Value to park.
     */
    void park(T value) { buckets_[head_].push_back(std::move(value)); }

    /** @brief Releases the values parked kDeferredReleaseFrames advances ago.
     *
     * Called once per SUBMITTED frame: the bucket entered now was filled kDeferredReleaseFrames
     * submits ago, so every command buffer that could have referenced one of its values has been
     * re-recorded since.
     *
     * @param release Callback run for each due value before it is destroyed.
     * @return Number of values released.
     */
    template <class Release>
    std::size_t advance(Release&& release)
    {
        head_ = (head_ + 1u) % kDeferredReleaseFrames;
        std::vector<T>& bucket = buckets_[head_];
        const std::size_t count = bucket.size();
        for (T& value : bucket) {
            release(value);
        }
        bucket.clear();
        return count;
    }

    /** @brief Releases the values parked kDeferredReleaseFrames advances ago, without a callback.
     *
     * @return Number of values released.
     */
    std::size_t advance() { return advance([](T&) {}); }

    /** @brief Gets how many values are parked right now.
     *
     * Diagnostic: a queue that never released would grow without bound, so a caller exposes this
     * (VsgRetireRing / VsgDrawBlockPool).
     *
     * @return Number of parked values.
     */
    [[nodiscard]] std::size_t parkedCount() const noexcept
    {
        std::size_t total = 0;
        for (const std::vector<T>& bucket : buckets_) {
            total += bucket.size();
        }
        return total;
    }

  private:
    // One bucket per advance: the values parked during one frame's assembly, released when the
    // queue comes back around to that bucket.
    std::array<std::vector<T>, kDeferredReleaseFrames> buckets_;
    std::size_t head_ = 0;
};

V_VSG_NS_END
