#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include <vine/graphics/ContentId.hpp>
#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

namespace detail
{

/**
 * @brief The engine-side residency ledger keyed by `(id, revision)`, aged by snapshot epoch.
 *
 * @details
 * - THE RULE IS ONE COMPARISON: an entry is collectable when `last_epoch < oldest_live`, where `oldest_live` is
 *   the smallest snapshot epoch still referenced by the session (or the current snapshot when none is in flight).
 *   No time window, no `markUsed` flag and no `kept` fallback.
 * - A TOUCH IS A SNAPSHOT REFERENCE. The frame/session side calls `touch()` for every ref a snapshot names at the
 *   epoch of that snapshot; `touch()` keeps the newest epoch so a later frame extends the entry's life.
 * - THIS TABLE IS A PROTOTYPE POLICY, NOT THE BACKEND'S TABLE YET. It proves the enumerable collectable set and
 *   the revision key before `ContentStore` is migrated; it owns no GPU resource and is engine-thread-only.
 */
class ResidencyTable {
  public:
    ResidencyTable()  = default;
    ~ResidencyTable() = default;

    ResidencyTable(const ResidencyTable&)            = delete;
    ResidencyTable& operator=(const ResidencyTable&) = delete;

    /**
     * @brief Records that @p ref is referenced by a snapshot at @p epoch.
     *
     * @param ref   The versioned content identity.
     * @param epoch The snapshot epoch that references it.
     * @return false when @p ref has a null id, true otherwise.
     */
    bool touch(ContentRef ref, std::uint64_t epoch) noexcept;

    /** @brief Says whether the exact (id, revision) pair is resident. */
    [[nodiscard]] bool contains(ContentRef ref) const noexcept;

    /** @brief The number of live entries. */
    [[nodiscard]] std::size_t liveCount() const noexcept;

    /**
     * @brief Lists the entries collectable when @p oldest_live is the oldest live snapshot.
     *
     * @param oldest_live The smallest epoch still referenced by the session.
     * @return The collectable refs, in key order.
     */
    [[nodiscard]] std::vector<ContentRef> collectable(std::uint64_t oldest_live) const;

    /**
     * @brief Removes every entry with `last_epoch < oldest_live`.
     *
     * @param oldest_live The smallest epoch still referenced by the session.
     * @return How many entries were removed.
     */
    std::size_t reclaim(std::uint64_t oldest_live);

    /** @brief Forgets every entry (a new session or a test reset). */
    void clear() noexcept;

  private:
    std::map<ContentRef, std::uint64_t> last_epoch_;
};

} // namespace detail

VN_GRAPHICS_NS_END
