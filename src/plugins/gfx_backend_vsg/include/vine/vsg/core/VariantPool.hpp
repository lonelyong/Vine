#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The compiled variants of one content scope: identity in, variant out, and nothing else.
 *
 * WHY A POOL AND NOT A CACHE NEXT TO EACH PROGRAM. A pipeline is compiled against a whole identity - the
 * program and its revision, the vertex layout, the render-pass compatibility, and whether a depth texture or
 * a shadow map is bound (see PipelineKey). Two drawables that agree on all of it must share ONE pipeline, and
 * one that differs in any of it must not: the pool is the single place that decides which is which, so the
 * answer cannot depend on which code path happened to ask.
 *
 * WHAT MUST NEVER REACH IT. Anything deliverable with a set command (cull, polygon, blend, depth policy,
 * topology) or written into a buffer (matrices, opacities, material bytes). Those live in @ref DynamicState
 * and @ref InstanceSlot, and they cost a command or a write rather than a compile. The two historical
 * failure families in this backend are exactly this mistake - a host state change that recompiled a pipeline,
 * a resize that rebuilt every full-screen slot - which is why the key's allowed inputs are pinned by the key
 * audit and why the churn phases assert that the variant count does not move.
 *
 * WHAT EVICTION MEANS (and does not). Past the capacity the OLDEST entry leaves the map. That stops the pool
 * from handing the variant out again, and a later acquire of the same identity compiles a new one; it does
 * NOT mean the GPU object may be destroyed - the holder still owns it, and only the retirement queue's
 * timeline decides when it dies. Keeping "the lookup left" and "the object may die" apart is what makes this
 * class device-free.
 *
 * AN ENTRY HOLDS ITS KEY. The key carries owned storage (the layout's locations, the compatibility's
 * formats), so a variant stays answerable after the caller's key object is gone - a lookup does not need the
 * caller to keep anything alive.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
V_VSG_NS_BEGIN

namespace core
{

/** @brief Variants indexed by pipeline identity, with a bounded number of them. */
class VariantPool
{
  public:
    /** @brief Upper bound on the pool, matching the program cache the existing implementation retains. */
    static constexpr std::size_t kDefaultCapacity = 65;

    /** @brief What a lookup did. */
    enum class Action : std::uint8_t
    {
        Created,  ///< No variant had this identity: a compile is needed.
        Reused,   ///< A variant with this identity exists: bind it.
    };

    /** @brief The answer of @ref acquire. */
    struct Lookup
    {
        Action        action{Action::Created};  ///< Whether to compile or to reuse.
        std::uint64_t id{0};                    ///< The variant's id (see @ref contains / @ref keyOf).
    };

  public:
    /** @brief Constructs a pool with the given capacity.
     *
     * @param capacity Maximum variants; 0 is clamped to 1 so the pool stays usable.
     */
    explicit VariantPool(std::size_t capacity = kDefaultCapacity);

    VariantPool(const VariantPool&) = delete;
    VariantPool& operator=(const VariantPool&) = delete;

  public:
    /** @brief Gets (or creates) the variant for a pipeline identity.
     *
     * @param key Pipeline identity (identity layer only - see the file note).
     * @return Whether the caller has to compile, and the variant's id.
     */
    [[nodiscard]] Lookup acquire(const PipelineKey& key);

    /** @brief Gets whether the pool still answers a variant.
     *
     * A stale id - one that was evicted - is reported as absent, which is how a caller learns that its
     * variant may leave with the object it holds and has to be built again.
     *
     * @param id Variant id.
     * @return true when the pool has a variant with this id.
     */
    [[nodiscard]] bool contains(std::uint64_t id) const noexcept;

    /** @brief Gets the identity a live variant stands for.
     *
     * @param id Variant id.
     * @return The key, or null when the pool no longer has the variant.
     */
    [[nodiscard]] const PipelineKey* keyOf(std::uint64_t id) const noexcept;

  public:
    /** @brief Gets the number of variants the pool holds. */
    [[nodiscard]] std::size_t variants() const noexcept;

    /** @brief Gets the number of variants compiled (a first lookup for an identity). */
    [[nodiscard]] std::uint64_t created() const noexcept;

    /** @brief Gets the number of lookups an existing variant answered. */
    [[nodiscard]] std::uint64_t reused() const noexcept;

    /** @brief Gets the number of entries that left because the capacity was reached. */
    [[nodiscard]] std::uint64_t evictions() const noexcept;

    /** @brief Drops every variant, keeping the counters (a scope died: a pass set changed, a session ended). */
    void clear() noexcept;

  private:
    struct Entry
    {
        PipelineKey   key;  ///< The identity this variant was compiled for.
        std::uint64_t id;   ///< The id handed out (monotonic, never reused, so stale ids cannot alias).
    };

    std::size_t                                                  capacity_;
    std::uint64_t                                                next_id_{1};
    std::list<Entry>                                             order_;  ///< FIFO order (insertion).
    std::unordered_map<PipelineKey, std::list<Entry>::iterator, PipelineKeyHash> index_;
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator>                by_id_;
    std::uint64_t                                                created_{0};
    std::uint64_t                                                reused_{0};
    std::uint64_t                                                evictions_{0};
};

}  // namespace core

V_VSG_NS_END
