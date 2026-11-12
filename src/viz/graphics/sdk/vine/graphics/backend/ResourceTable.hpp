#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

#include <vine/graphics/backend/BackendHalfGuard.hpp>
#include <vine/graphics/backend/FrameTimeline.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>
#include <vine/graphics/graphics_global.hpp>

/**
 * @brief One residency table: values built on demand per key, kept while live content pins them, parked when none does.
 *
 * WHY ONE SHAPE FOR EVERY RESIDENCY KIND. A shared stream bind, an uploaded texture, a material block, a compiled
 * half and a descriptor set answer the same three questions - "do I already have the one for this key?", "build it
 * if I do not", "when may it go?" - and differ only in what the key and the value ARE. Keeping one table means the
 * lifetime rule is written once: a value is live exactly while some content names its key, and a value nothing
 * names any more is PARKED rather than destroyed, because a recorded frame may still bind it. The kinds' own
 * files only say what a key is and how a value is built (see `StreamUploads`).
 *
 * WHAT THE TABLE DOES NOT KNOW. It cannot tell whether a key is still wanted: that is content knowledge (which
 * geometry still declares which buffer, which material still names which texture), and it arrives as the
 * @ref retireUnpinned predicate. That predicate is asked about the KEY **and the resident VALUE**, because the
 * value is often the evidence of its own currency: a stream entry holds the buffer it was uploaded from, so
 * "are these still the buffer's current bytes?" is one revision compare on its own handle - no walk of the
 * content tables. This keeps the table free of devices, facts tables and callbacks between them.
 *
 * GENERATION: ONE NUMBER PER BUILT VALUE. A key may be built again while its previous value is still inside the
 * parking window - the old value is then held by the retirement queue, not by this table, and the new value serves
 * lookups. The number is monotonic and never reused, so anything that kept it (a test, a log, a bookkeeper one
 * layer down) can tell that its value is a previous generation instead of mistaking it for the current one. It is
 * the same idea as `ProgramFacts::generation` one level lower: an address plus a revision does not identify a
 * VALUE, a build does.
 *
 * WHAT A CALLER MAY ASSUME. `acquire` returns a pointer that stays valid until that key leaves the table or
 * @ref clear runs (`std::unordered_map` never invalidates references on rehash). A rejected build leaves the table
 * unchanged. `retireUnpinned` never destroys a value it could not park: when the queue has no window yet, the entry
 * is kept and counted in @ref retained - memory, never correctness.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_GRAPHICSBACKEND_NS_BEGIN

/**
 * @brief Keyed residency with build-on-miss, pin-driven retirement and parked destruction (see the file note).
 *
 * @tparam Key   Identity of one resident value; its hash and equality decide what "the same" means.
 * @tparam Value What is resident. Must be COPYABLE, because parking hands it to `RetirementQueue`'s
 *               `std::function` release act.
 * @tparam Hash  Hasher for @p Key.
 */
template <class Key, class Value, class Hash = std::hash<Key>>
class ResourceTable {
  public:
    /** @brief What one @ref acquire did. */
    struct Acquired {
        Value*        value{ nullptr }; // The resident value; null only when the builder refused.
        std::uint64_t generation{ 0 };  // Which build of this key this value is (see the file note).
        bool          built{ false };   // true when this call built the value, false when it was already resident.
    };

  public:
    /**
     * @brief Returns the value already resident for @p key, or builds one with @p build.
     *
     * @details
     * - A HIT TOUCHES NOTHING: @p build is not called, so a steady frame pays only the lookup.
     * - A MISS calls @p build OUTSIDE the table and moves its answer in only when there is one, so a refusal
     *   (a stream that may not be shared, a half that cannot compile) leaves the table exactly as it was.
     * - The returned pointer stays valid until that key leaves (see @ref retireUnpinned) or @ref clear runs.
     *
     * @tparam Build Callable `() -> std::optional<Value>`; an empty answer means "refused".
     * @param key   The value's identity.
     * @param build How to build the value when none is resident.
     * @return The value, its generation, and whether this call built it; a refused build answers a null value.
     */
    template <class Build>
    [[nodiscard]] Acquired acquire(const Key& key, Build&& build)
    {
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            return { &found->second.value, found->second.generation, false };
        }

        std::optional<Value> made = build();
        if (!made.has_value()) {
            return {};
        }

        const std::uint64_t generation = ++generations_;
        const auto          inserted   = entries_.emplace(key, Entry{ std::move(*made), generation });
        ++builds_;
        return { &inserted.first->second.value, generation, true };
    }

    /**
     * @brief Parks every value @p drop selects, and forgets it.
     *
     * @details
     * - The general door @ref retireUnpinned is written in terms of: a caller that knows better than the pin model
     *   which values must go (an explicit release - "the host says these bytes are not wanted") names them here, and
     *   a caller that asks content supplies a predicate derived from the answer. Same rule, one loop.
     * - WHAT LEAVES IS PARKED, NOT DESTROYED: the value is moved into a release act the queue runs once the frames
     *   that could still bind it are past. A caller that needs the value's storage released immediately must call
     *   @ref clear instead - that is the session-teardown path, where nothing is in flight.
     * - NO WINDOW MEANS KEEP: when @p retirement cannot park yet (the session has not learned its in-flight slot
     *   count), the entry is retained and counted in @ref retained, and the next call tries again. A retained entry
     *   is still resident, so it is NOT counted as departed.
     *
     * @tparam Drop Callable `bool(const Key&, const Value&)` - true when that value must leave now.
     * @param drop       Which values leave (asked exactly once per resident entry).
     * @param timeline   Frame clock the parks are dated against.
     * @param retirement Where departed values are parked.
     * @return How many values left the table (parked).
     */
    template <class Drop>
    std::size_t retireIf(Drop&& drop, FrameTimeline& timeline, RetirementQueue& retirement)
    {
        std::size_t retired = 0U;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!drop(it->first, it->second.value)) {
                ++it;
                continue;
            }
            // The value travels INTO the release act: the queue owns it for the window, so what a submitted frame
            // may still bind outlives the lookup that no longer answers for it.
            if (!retirement.retire(timeline, [value = std::move(it->second.value)]() mutable { (void)value; })) {
                ++retained_; // no window to park through: memory is kept, never freed while a frame may name it
                ++it;
                continue;
            }
            it = entries_.erase(it);
            ++retired;
        }
        retired_ += retired;
        return retired;
    }

    /**
     * @brief Parks every value whose key @p is_live rejects, and forgets it.
     *
     * @details
     * - The predicate is asked exactly once per resident entry, KEY AND VALUE together (see the file note), so a
     *   caller may make it two O(1) lookups rather than a scan over anything.
     * - Parking, retention and the return value are @ref retireIf's (this overload only negates the question).
     *
     * @tparam Live Callable `bool(const Key&, const Value&)` - true when content still names that key, at the
     *              state the resident value was built for.
     * @param is_live    Whether a resident entry is still wanted.
     * @param timeline   Frame clock the parks are dated against.
     * @param retirement Where departed values are parked.
     * @return How many values left the table (parked), i.e. how many keys became unpinned.
     */
    template <class Live>
    std::size_t retireUnpinned(Live&& is_live, FrameTimeline& timeline, RetirementQueue& retirement)
    {
        return retireIf([&is_live](const Key& key, const Value& value) { return !is_live(key, value); }, timeline, retirement);
    }

    /** @brief Gets how many values the table holds. */
    [[nodiscard]] std::size_t live() const noexcept
    {
        return entries_.size();
    }

    /**
     * @brief Visits every resident key and value, in an unspecified order.
     *
     * @details
     * - THE READ DOOR, for a caller whose whole table needs work rather than one lookup: repointing every cached
     *   value at a replacement parent (see `ContentSets::repoint`) or adding up a report. It is not a lookup and
     *   does not touch generations.
     * - The key is const: a value's identity is what the table was ordered by, and a visitor that changed it
     *   would corrupt the map. The value is mutable here for the repoint case only.
     *
     * @warning
     * - THE VISITOR MUST NOT ACQUIRE, RETIRE OR CLEAR on this table: each would move `entries_` while it is being
     *   walked, invalidating the iterator. Changes to a value's own body are fine; changes to its identity are not.
     *
     * @tparam Visit Callable `void(const Key&, Value&)` (or `const Value&` for the const overload).
     * @param visit What to do with each resident value.
     */
    template <class Visit>
    void forEach(Visit&& visit)
    {
        for (auto& [key, entry] : entries_) {
            visit(key, entry.value);
        }
    }

    /** @brief Visits every resident key and value through a const table (see the mutable overload).
     *
     * @tparam Visit Callable `void(const Key&, const Value&)`.
     * @param visit What to do with each resident value.
     */
    template <class Visit>
    void forEach(Visit&& visit) const
    {
        for (const auto& [key, entry] : entries_) {
            visit(key, entry.value);
        }
    }

    /** @brief Gets how many values were built in total: a hit raises nothing. */
    [[nodiscard]] std::uint64_t builds() const noexcept
    {
        return builds_;
    }

    /** @brief Gets how many values left the table, parked (cumulative). */
    [[nodiscard]] std::size_t retired() const noexcept
    {
        return retired_;
    }

    /** @brief Gets how many departures could not be parked and were retained instead (cumulative). */
    [[nodiscard]] std::size_t retained() const noexcept
    {
        return retained_;
    }

    /**
     * @brief Forgets every value WITHOUT parking: the session-teardown path, where nothing is in flight.
     */
    void clear()
    {
        entries_.clear();
    }

  private:
    struct Entry {
        Value         value{};        // What is resident.
        std::uint64_t generation{ 0 }; // Which build of the key this is (see the file note).
    };

    std::unordered_map<Key, Entry, Hash> entries_;
    std::uint64_t                        generations_{ 0 }; // The last generation handed out.
    std::uint64_t                        builds_{ 0 };
    std::size_t                          retired_{ 0 };
    std::size_t                          retained_{ 0 };
};

VN_GRAPHICSBACKEND_NS_END
