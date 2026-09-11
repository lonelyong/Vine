#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <vine/intrusive_ptr.hpp>

#include "vsg_global.hpp"

V_VSG_NS_BEGIN

/**
 * @brief Returns whether a cache-owned key object has no owner left but the cache.
 *
 * A null object counts as released: there is nothing to keep alive, which is
 * how a default resource (a null program or material) behaves. This is the
 * single definition of "the app has let go of it", shared by every entry kind
 * so a cache can never disagree with another about what "abandoned" means.
 *
 * @tparam Object Key object type (held by const reference).
 * @param object  Object the cache owns an entry for.
 * @return true when the app released @p object (or it is null).
 */
template <class Object>
bool keyReleased(const vine::intrusive_ptr<const Object>& object) noexcept
{
    return object == nullptr || object->useCount() <= 1u;
}

/**
 * @brief A retained-cache entry that OWNS the object its cache is keyed by.
 *
 * A retained cache keyed by a raw pointer cannot observe its object being
 * destroyed: the entry outlives it, and a later object allocated at the
 * recycled address is then served the dead entry's retained GPU state — the
 * wrong mesh / the wrong shader / the wrong colours, silently (see the design
 * doc §8.1 and D13). Holding an owning reference per entry makes the address
 * unique for as long as the entry exists, which is what makes a pointer key
 * valid at all.
 *
 * Ownership must not turn into a leak, so the entry also reports when it is
 * ABANDONED (the cache is the only owner left, i.e. the app has released the
 * object): nothing can look such an entry up again, so it may be released
 * immediately by eraseAbandoned() instead of being pinned for a reuse window.
 *
 * Note that "the only owner left" is judged per cache entry: when SEVERAL
 * caches own the same object (a program is owned by its stage cache, its
 * ShaderSet cache and a pipeline template), each entry sees the others' shares
 * and reports abandoned only after the others have let go — the capacity trims
 * are what bound that chain, and eraseAbandoned() releases its tail.
 *
 * @tparam Object  Object the cache is keyed by (held by const reference).
 * @tparam Payload Retained state for that object (e.g. a vsg resource).
 */
template <class Object, class Payload>
class OwnedCacheEntry
{
  public:
    /** @brief Constructs an entry that owns @p object.
     *
     * @param object   Object the cache keys on (may be null: the default entry).
     * @param payload  Retained state built for it.
     * @param sequence Insertion sequence (see InsertionClock), for FIFO trims.
     */
    OwnedCacheEntry(vine::intrusive_ptr<const Object> object, Payload payload,
                    std::uint64_t sequence)
      : object_(std::move(object))
      , payload_(std::move(payload))
      , sequence_(sequence)
    {
    }

    /** @brief Gets the key object (null for a default entry). */
    const Object* key() const noexcept { return object_.get(); }

    /** @brief Returns whether the cache is the only owner left.
     *
     * True means the app has released the object: no later lookup for it is
     * possible, so the entry (and the object) can be freed now.
     *
     * @return true when nothing but this cache still references the object.
     */
    bool abandoned() const noexcept { return object_ != nullptr && keyReleased(object_); }

    /** @brief Gets the insertion sequence (FIFO order for capacity trims). */
    std::uint64_t sequence() const noexcept { return sequence_; }

    /** @brief Gets the retained state. */
    Payload& payload() noexcept { return payload_; }

    /** @brief Gets the retained state. */
    const Payload& payload() const noexcept { return payload_; }

  private:
    vine::intrusive_ptr<const Object> object_;
    Payload                           payload_;
    std::uint64_t                     sequence_ = 0;
};

/**
 * @brief A retained-cache entry that OWNS the TWO objects its cache keys on.
 *
 * The same bargain as OwnedCacheEntry, for a cache whose identity is a PAIR:
 * the L2 pipeline-template cache keys on (user program, material) and compares
 * those addresses to decide whether a cached template may be reused. Both
 * addresses therefore have to stay unique for as long as the entry exists — if
 * an entry only held them as raw pointers, a program or material destroyed
 * elsewhere could be replaced at the same address and the entry's equality
 * check would then report a hit for a DIFFERENT variant, serving its retained
 * pipeline and descriptor bind (the same defect class as D13, on the variant
 * cache).
 *
 * The entry is ABANDONED once the app has released EVERY object it owns: only
 * then can no later lookup match it. An object that is null is not owned (the
 * built-in path has no user program), and an entry that owns nothing at all
 * never expires — it is the default entry.
 *
 * @tparam First   Primary key object (e.g. the user shader program).
 * @tparam Second  Secondary key object (e.g. the bound material).
 * @tparam Payload Retained state for that pair (e.g. reusable bind commands).
 */
template <class First, class Second, class Payload>
class OwnedPairCacheEntry
{
  public:
    /** @brief Constructs an entry that owns both key objects.
     *
     * @param first    Primary key object (may be null: built-in path).
     * @param second   Secondary key object (may be null: no material).
     * @param payload  Retained state built for the pair.
     * @param sequence Insertion sequence, for FIFO trims.
     */
    OwnedPairCacheEntry(vine::intrusive_ptr<const First> first, vine::intrusive_ptr<const Second> second,
                        Payload payload, std::uint64_t sequence)
      : first_(std::move(first))
      , second_(std::move(second))
      , payload_(std::move(payload))
      , sequence_(sequence)
    {
    }

    /** @brief Gets the primary key object (null for the built-in path). */
    const First* firstKey() const noexcept { return first_.get(); }

    /** @brief Gets the secondary key object (null when no material is bound). */
    const Second* secondKey() const noexcept { return second_.get(); }

    /** @brief Returns whether the app released every object this entry owns.
     *
     * @return true when nothing but this cache still references any of them.
     */
    bool abandoned() const noexcept
    {
        return (first_ != nullptr || second_ != nullptr) && keyReleased(first_) && keyReleased(second_);
    }

    /** @brief Gets the insertion sequence (FIFO order for capacity trims). */
    std::uint64_t sequence() const noexcept { return sequence_; }

    /** @brief Gets the retained state. */
    Payload& payload() noexcept { return payload_; }

    /** @brief Gets the retained state. */
    const Payload& payload() const noexcept { return payload_; }

  private:
    vine::intrusive_ptr<const First>  first_;
    vine::intrusive_ptr<const Second> second_;
    Payload                           payload_;
    std::uint64_t                     sequence_ = 0;
};

/**
 * @brief Erases every entry whose object the app has released.
 *
 * The prompt half of the ownership bargain: an abandoned entry can never be
 * looked up again, so holding it would only pin the object and its GPU state.
 * The other half — objects the app still holds but does not draw — is the
 * caller's policy (a reuse window, a frame-count sweep), because rebuilding
 * their state may be expensive.
 *
 * @tparam Map  Map from object pointer to OwnedCacheEntry.
 * @param cache Cache to sweep.
 * @return Number of erased entries.
 */
template <class Map>
std::size_t eraseAbandoned(Map& cache)
{
    std::size_t erased = 0;
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->second.abandoned()) {
            it = cache.erase(it);
            ++erased;
        }
        else {
            ++it;
        }
    }
    return erased;
}

/**
 * @brief Erases the oldest entries until the cache holds at most @p max_entries.
 *
 * Bounds growth when a host never releases its objects explicitly (the common
 * case: the engine has no material-lifecycle event, see D13). Evicting the
 * OLDEST insertions is deliberate: the newest entries are the ones a live scene
 * is using, so a steady workload never evicts what it is about to ask for
 * again. A null-keyed entry is never evicted — it is the default resource
 * every caller falls back to, and it costs one entry.
 *
 * @tparam Map  Map from object pointer to OwnedCacheEntry.
 * @param cache       Cache to trim.
 * @param max_entries Upper bound on the number of entries to keep.
 * @return Number of erased entries.
 */
template <class Map>
std::size_t trimToCapacity(Map& cache, std::size_t max_entries)
{
    if (cache.size() <= max_entries) {
        return 0u;
    }
    std::size_t remaining = cache.size() - max_entries;
    std::size_t erased    = 0;
    while (remaining > 0u) {
        auto oldest = cache.end();
        for (auto it = cache.begin(); it != cache.end(); ++it) {
            // A pointer-keyed cache may hold a null-keyed default entry that
            // every caller falls back to; it is never evicted. A cache keyed by
            // a content hash has no such entry, so the rule does not apply.
            if constexpr (std::is_pointer_v<typename Map::key_type>) {
                if (it->first == nullptr) {
                    continue; // the default entry stays
                }
            }
            if (oldest == cache.end() || it->second.sequence() < oldest->second.sequence()) {
                oldest = it;
            }
        }
        if (oldest == cache.end()) {
            break; // only the default entry is left
        }
        cache.erase(oldest);
        --remaining;
        ++erased;
    }
    return erased;
}

/**
 * @brief Monotonic insertion sequence, so capacity trims are FIFO.
 *
 * One per cache: the sequence only has to order that cache's insertions.
 */
class InsertionClock
{
  public:
    /** @brief Returns the next sequence value. */
    std::uint64_t tick() noexcept { return ++sequence_; }

  private:
    std::uint64_t sequence_ = 0;
};

V_VSG_NS_END
