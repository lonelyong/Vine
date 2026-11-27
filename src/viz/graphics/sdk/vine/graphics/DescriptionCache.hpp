#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <mutex>

#include <vine/intrusive_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief ONE immutable value per revision of its source, shared by every reader that asks for it.
 *
 * @details
 * - WHAT IT OWNS (`G1` / `G6`). `Geometry::description()`, `MaterialValues::description()` and
 *   `ShaderProgram::description()` are one rule - "the source's current revision, as an immutable value, built once
 *   and handed to whoever asks" - and it used to be spelled three times by hand. Three copies were three chances to
 *   get the concurrency half wrong, and two of them had no lock at all: two engines describing one content object
 *   could tear the cache member or hand out two identities for one revision. The rule has one home now.
 * - CONCURRENT READERS ARE THE POINT. Several threads may describe one object at the same time - two engines
 *   publishing one scene is the documented shape (`graphics-threading.md` 2.16) - and every one of them gets the SAME
 *   value: `get` measures, compares and builds inside one critical section, so a thread that missed the same revision
 *   waits for the builder and then finds what it built instead of building a second value.
 * - THE KEY IS READ INSIDE THE LOCK, which is why `get` takes a callable and not a number. A material instance's
 *   `revision()` resolves its parent's values on the first read, so it WRITES: measuring outside the lock would race
 *   two readers against each other, and measuring inside makes "which revision, and the value for it" one step.
 * - ONE LOCK, NOT A SHARED ONE. Because the measurement belongs to the same critical section as the compare, a
 *   read-only side cannot be offered - and it would not pay: an uncontended `std::mutex` costs 2.8 ns on the
 *   development machine against 13.2 ns for a shared lock, which is what the geometry cache held before.
 * - AN EDIT IS STILL THE OWNING THREAD'S ACT. The cache makes READERS safe; it does not let a host edit content while
 *   another thread describes it (`graphics-threading.md` C22).
 * - WHAT THE VALUE MUST BE. The cached `Description` is immutable after construction and its `revision()` answers the
 *   number it was built from - all three description types are constructor-only values and already do.
 *
 * @note
 * - In `detail` on purpose: it is the mechanism behind three public accessors, not a fourth way for a host to
 *   publish content.
 */
template <typename Description>
class DescriptionCache final
{
  public:
    /** @brief The shared, immutable value this cache hands out. */
    using Ptr = intrusive_ptr<const Description>;

    /**
     * @brief Gets the value for the source's current revision, building it through @p build when the cache is stale.
     *
     * @details
     * - ONE CRITICAL SECTION: @p measure runs, its answer is compared with the cached value's revision, and
     *   @p build runs only when the two differ - so two threads cannot build two values for one revision.
     * - @p build receives the revision the compare used, which is what makes the value's own revision and the cache
     *   key the same number by construction.
     *
     * @warning
     * - @p build runs while the cache is locked: it may read the source's authored content, but it must not call back
     *   into this cache (a re-entrant `get` on the same object would deadlock).
     *
     * @param measure Reads the source's current revision; it runs on every call, so it must be cheap and noexcept.
     * @param build   Builds the value for the revision @p measure reported; it runs only on a miss.
     * @return The cached value for the current revision, shared with every other caller that asked for it.
     */
    template <typename Measure, typename Build>
    [[nodiscard]] Ptr get(Measure&& measure, Build&& build) const
    {
        const std::lock_guard<std::mutex> held(mutex_);
        const std::uint64_t               revision = measure();
        if (cached_ != nullptr && cached_->revision() == revision) {
            return cached_;
        }
        cached_ = Ptr(build(revision));
        return cached_;
    }

    /**
     * @brief Gets the cached value only while it still belongs to the source's current revision; never builds.
     *
     * @details
     * - THE READ-ONLY FACE: a caller that must not write the cache - and must not read content it does not own -
     *   asks this and treats null as "the source moved on since it was described", which is the one publication of
     *   lag every other content fact follows.
     *
     * @param measure Reads the source's current revision, exactly as `get` does.
     * @return The value built for that revision, or null when the cache holds another one.
     */
    template <typename Measure>
    [[nodiscard]] Ptr peek(Measure&& measure) const noexcept
    {
        const std::lock_guard<std::mutex> held(mutex_);
        const std::uint64_t               revision = measure();
        return cached_ != nullptr && cached_->revision() == revision ? cached_ : Ptr{};
    }

  private:
    /// Guards @ref cached_ AND the measurement that decides whether it still answers (see the class note).
    mutable std::mutex mutex_{};
    /// The one value per revision (see get()).
    mutable Ptr cached_{};
};

} // namespace detail

VN_GRAPHICS_NS_END
