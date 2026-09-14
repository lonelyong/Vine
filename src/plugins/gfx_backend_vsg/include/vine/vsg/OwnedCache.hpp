#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <utility>

#include <vine/intrusive_ptr.hpp>

#include "vsg_global.hpp"

V_VSG_NS_BEGIN

/**
 * @brief How many retained entries hold each key object, counted across the caches
 * that sweep together.
 *
 * "The app has let go of this object" cannot be read off the object's reference count
alone: when
 * several caches own the same object (the material manager and a bridge's variant
template own one
 * Material; a bridge's stage cache, its ShaderSet cache and its variant templates o
wn one
 * ShaderProgram), each entry sees the OTHERS' shares and, judging by "no owner left b
ut me",
 * waits for the others to let go first — for ever, since they wait for it in turn. 
That is the P11
 * defect: a material the app dropped stayed alive with its variant template until a
 capacity trim
 * happened to push one of the two out.
 *
 * The count is what breaks the tie: an object is released when the only references l
eft to it are
 * the RETAINED ENTRIES that hold it, i.e. `useCount() <= shares`. The number of sha
res is data
 * dependent (a material bound by two slots with two programs is held by three entri
es), so it is
 * counted, never assumed: collectOwnedShares() walks every cache that sweeps together
, and the
 * sweep then judges by that number.
 *
 * Counted entries are exactly the ones an entry's `abandoned()` is evaluated for, so
 over-counting
 * would release too early and under-counting only keeps an entry a sweep longer — the
 collection
 * walks the same caches as the sweep it feeds.
 *
 * The counts are rebuilt every frame (an object's shares change as slots come and go),
 * so the
 * rebuild reuses the table's nodes instead of clearing it: a frame that finds the same
 * objects
 * inserts into keys that are already there, which keeps the pass to hashing and makes
it
 * allocation-free in steady state — the cost this adds to a frame must stay below the
 per-frame
 * sweeps it feeds. Keys whose count returns to zero are kept (the map only GROWS past
 * twice the number of keys seen in a frame when the frame really uses that many), and
 * `of()` answers 0 for them, so a stale key is never mistaken for a held object.
 */
class OwnedShareCounts
{
  public:
    /** @brief Counts one retained share of @p object.
     *
     * @param object Key object held by a retained entry (null is ignored: a null key
 is not owned).
     */
    void add(const void* object)
    {
        if (object == nullptr) {
            return;
        }
        const auto [it, inserted] = shares_.try_emplace(object, 0u);
        if (inserted) {
            touched_.push_back(object);
        }
        ++it->second;
    }

    /** @brief Drops every count, so the collection can be filled again for the next fr
ame.
     *
     * Zeroes the keys this frame touched instead of destroying the table: the next fram
e's
     * collection then reuses those nodes (see the class notes). A table that has grown
 well past
     * what a frame uses — the keys of objects that have since died — is dropped whole,
 so a long
     * session with material churn does not keep one node per object it has ever seen.
     */
    void clear()
    {
        if (shares_.size() > touched_.size() * 4u + 64u) {
            shares_.clear();
            touched_.clear();
            return;
        }
        for (const void* object : touched_) {
            shares_.at(object) = 0u;
        }
        touched_.clear();
    }

    /** @brief Gets the number of retained shares counted for @p object.
     *
     * @param object Key object to look up (null counts as none).
     * @return Retained shares holding @p object, 0 when no entry holds it.
     */
    [[nodiscard]] std::uint32_t of(const void* object) const
    {
        const auto it = shares_.find(object);
        return it == shares_.end() ? 0u : it->second;
    }

    /** @brief Gets how many keys the counts remember.
     *
     * The node count `clear()` deliberately keeps across frames (see the class notes), s
o a test can
     * tell a rebuild that reuses the table from one that re-allocates it every frame.
     *
     * @return Number of remembered keys.
     */
    [[nodiscard]] std::size_t trackedCount() const noexcept { return shares_.size(); }

  private:
    std::unordered_map<const void*, std::uint32_t> shares_;
    std::vector<const void*>                       touched_;
};

/**
 * @brief Returns whether a cache-owned key object has no owner left but the retain
ed entries.
 *
 * A null object counts as released: there is nothing to keep alive, which is how a de
fault resource
 * (a null program or material) behaves. This is the single definition of "the app has
 let go of it",
 * shared by every entry kind so a cache can never disagree with another about what 
"abandoned"
 * means.
 *
 * @tparam Object Key object type (held by const reference).
 * @param object  Object the cache owns an entry for.
 * @param shares  Retained shares counted for the objects of this sweep (see OwnedSh
areCounts).
 * @return true when the app released @p object (or it is null).
 */
template <class Object>
bool keyReleased(const vine::intrusive_ptr<const Object>& object, const OwnedShareCounts& shares) noexcept
{
    // `useCount()` counts references: the app's own references, plus one share per retained
    // entry. Equal to the retained shares means every remaining reference IS a retained entry,
    // i.e. nothing outside the caches holds the object any more.
    return object == nullptr || static_cast<std::uint32_t>(object->useCount()) <= shares.of(object.get());
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
 * immediately by eraseAbandoned() — being kept only while the app wants it, not by a timer.
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

    /** @brief Returns whether the app released the object this entry owns.
     *
     * True means no later lookup for it is possible, so the entry (and the
     * object) can be freed now.
     *
     * @param shares Retained shares this sweep counted for that object.
     * @return true when nothing but retained entries still reference the object.
     */
    bool abandoned(const OwnedShareCounts& shares) const noexcept
    {
        return object_ != nullptr && keyReleased(object_, shares);
    }

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
     * @param shares Retained shares this sweep counted for those objects.
     * @return true when nothing but retained entries still reference any of them.
     */
    bool abandoned(const OwnedShareCounts& shares) const noexcept
    {
        return (first_ != nullptr || second_ != nullptr) && keyReleased(first_, shares) &&
               keyReleased(second_, shares);
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
 * @brief Erases every entry whose objects the app has released.
 *
 * The prompt half of the ownership bargain: an abandoned entry can never be
 * looked up again, so holding it would only pin the object and its GPU state.
 * The other half — objects the app still holds but does not draw — is the
 * caller's policy (a capacity trim, or no bound at all), because rebuilding
 * their state may be expensive.
 *
 * @tparam Map  Map from object pointer to OwnedCacheEntry.
 * @param cache  Cache to sweep.
 * @param shares Retained shares counted for the objects the sweep judges (see
 *               OwnedShareCounts; it must have been collected from every cache that holds
 *               any of them, or two caches wait for each other).
 * @return Number of erased entries.
 */
template <class Map>
std::size_t eraseAbandoned(Map& cache, const OwnedShareCounts& shares)
{
    std::size_t erased = 0;
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->second.abandoned(shares)) {
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
