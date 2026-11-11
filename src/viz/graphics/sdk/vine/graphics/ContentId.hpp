#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A 64-bit content handle: generation:24 | domain:8 | index:32.
 *
 * @details
 * - THE INDEX names the slot a content object occupies, the GENERATION says which occupant, and the DOMAIN keeps
 *   two content domains from answering for each other's handles.
 * - A RELEASED HANDLE ANSWERS GONE, FOREVER: the slot keeps its index and moves its generation, so an old handle
 *   never resolves to the slot's next occupant. That is the whole point of carrying a handle instead of an address:
 *   an address is reused, and a stale one silently names somebody else's bytes.
 * - TRIVIALLY COPYABLE, 8 bytes: a handle crosses snapshots, frame values and plan rows by value, so no owner has to
 *   be shared for a consumer to name what it draws.
 *
 * @note The null handle has generation 0. `isNull()` is the one test for "names nothing".
 */
class Handle {
  public:
    static constexpr std::uint32_t kGenerationMax = 0xFFFFFFU;

  public:
    constexpr Handle() noexcept = default;

    /**
     * @brief Builds a handle from its three parts.
     *
     * @param index      Slot index; the registry answers for it.
     * @param generation Occupant of that slot; 0 builds the null handle.
     * @param domain     The content domain the slot belongs to.
     * @return The handle.
     */
    [[nodiscard]] static constexpr Handle make(std::uint32_t index, std::uint32_t generation, std::uint8_t domain) noexcept
    {
        return Handle{ (static_cast<std::uint64_t>(generation & kGenerationMax) << 40U) |
                       (static_cast<std::uint64_t>(domain) << 32U) | static_cast<std::uint64_t>(index) };
    }

    /** @brief Gets the slot index. */
    [[nodiscard]] constexpr std::uint32_t index() const noexcept { return static_cast<std::uint32_t>(bits_ & 0xFFFFFFFFU); }

    /** @brief Gets the occupant of the slot; 0 means "null handle". */
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept
    {
        return static_cast<std::uint32_t>((bits_ >> 40U) & kGenerationMax);
    }

    /** @brief Gets the content domain the slot belongs to. */
    [[nodiscard]] constexpr std::uint8_t domain() const noexcept { return static_cast<std::uint8_t>((bits_ >> 32U) & 0xFFU); }

    /** @brief Returns whether this handle names nothing (generation 0). */
    [[nodiscard]] constexpr bool isNull() const noexcept { return generation() == 0U; }

    /** @brief Gets the raw bits: the key to hash by, never a value to interpret. */
    [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return bits_; }

    friend constexpr bool operator==(const Handle& lhs, const Handle& rhs) noexcept = default;
    friend constexpr bool operator<(const Handle& lhs, const Handle& rhs) noexcept { return lhs.bits_ < rhs.bits_; }

  private:
    explicit constexpr Handle(std::uint64_t bits) noexcept
      : bits_(bits)
    {}

    std::uint64_t bits_{ 0 };
};

/**
 * @brief A typed content id: the tag makes ids of different kinds different TYPES.
 *
 * @details
 * - The registry hands out `Id<Tag>` only, so a geometry id cannot be passed where a material id is meant - the
 *   compiler refuses it, which is the only place that mistake can still be caught cheaply.
 *
 * @tparam Tag The content kind (see the tags below).
 */
template <class Tag>
class Id {
  public:
    using tag_type = Tag;

  public:
    constexpr Id() noexcept = default;

    /** @brief Wraps a raw handle as an id of this kind.
     *
     * @param handle The handle to wrap.
     */
    explicit constexpr Id(Handle handle) noexcept
      : handle_(handle)
    {}

    /** @brief Gets the handle this id wraps. */
    [[nodiscard]] constexpr Handle handle() const noexcept { return handle_; }

    /** @brief Returns whether this id names nothing. */
    [[nodiscard]] constexpr bool isNull() const noexcept { return handle_.isNull(); }

    friend constexpr bool operator==(const Id& lhs, const Id& rhs) noexcept = default;
    friend constexpr bool operator<(const Id& lhs, const Id& rhs) noexcept { return lhs.handle_ < rhs.handle_; }

  private:
    Handle handle_{};
};

struct NodeTag;
struct GeometryTag;
struct MaterialTag;
struct ProgramTag;
struct TextureTag;
struct InstanceTag;

using NodeId     = Id<NodeTag>;     ///< A placement in the graph (see Node).
using GeometryId = Id<GeometryTag>; ///< A leaf's shape (see Geometry).
using MaterialId = Id<MaterialTag>; ///< A material's values (see MaterialBase).
using ProgramId  = Id<ProgramTag>;  ///< A shader program (see ShaderProgram).
using TextureId  = Id<TextureTag>;  ///< An image a material reads (see Texture).
using InstanceId = Id<InstanceTag>; ///< One instance of an instanced draw.

/**
 * @brief A content reference: `id` says WHICH object, `revision` says WHICH version of its values.
 *
 * @details
 * - THE REVISION IS PART OF THE IDENTITY OF THE VALUE, not bookkeeping. A frame in flight draws a geometry at
 *   revision 3 after the content published revision 4, and a table keyed by the id alone would answer the new
 *   streams for the old frame - a wrong picture with no refusal anywhere.
 * - THE PAIR IS THE STABLE KEY. A dump, a replay and a backend's residency ledger all use `(id, revision)`, so
 *   what a frame recorded is what a later reader can name again.
 * - Two refs are equal when both halves are; ordering is by id first, then revision (a stable map key).
 */
struct ContentRef {
    Handle        id{};
    std::uint64_t revision{ 0 }; // As wide as every SDK `revision()`: a narrowing key would wrap after 4 billion edits.

    /** @brief Returns whether the ref names nothing (a null id). */
    [[nodiscard]] bool isNull() const noexcept { return id.isNull(); }

    /**
     * @brief Gets the id as one typed kind.
     *
     * @details
     * - A NAMED CONVERSION, not a second identity: the raw handle is what a ref stores, and this says which kind it is
     *   where a typed id is what a consumer needs (an arena's slot key, a table's typed key).
     *
     * @tparam Tag The content kind the id names.
     * @return The typed id.
     */
    template <class Tag>
    [[nodiscard]] Id<Tag> as() const noexcept
    {
        return Id<Tag>(id);
    }

    friend bool operator==(const ContentRef& lhs, const ContentRef& rhs) noexcept = default;
    friend bool operator<(const ContentRef& lhs, const ContentRef& rhs) noexcept
    {
        if (lhs.id.bits() != rhs.id.bits()) {
            return lhs.id.bits() < rhs.id.bits();
        }
        return lhs.revision < rhs.revision;
    }
};

/**
 * @brief Builds a typed ref from an id and the revision its values are at.
 *
 * @param id       The id (its handle is taken).
 * @param revision The revision of the values the ref names.
 * @return The ref.
 */
template <class Tag>
[[nodiscard]] ContentRef contentRefOf(Id<Tag> id, std::uint64_t revision) noexcept
{
    return ContentRef{ id.handle(), revision };
}

/**
 * @brief The content domain's slot allocator: the only place an id is created or released.
 *
 * @details
 * - ALLOCATION IS TYPED. `allocate<Tag>()` answers the id of that kind, so ids of different kinds cannot be
 *   exchanged by construction. Which kind a handle names is a compile-time fact, not a field the runtime checks.
 * - RELEASE MOVES THE GENERATION. A released handle answers `alive() == false` forever, and the slot may be reused
 *   by the next allocation with a new generation. A generation reaching its 24-bit maximum RETIRES the slot instead
 *   of wrapping, so a handle from a previous era can never answer for a new object (ABA).
 * - THE DOMAIN IS AN ARENA, NOT A KIND. A registry constructed with another domain byte answers only for its own
 *   slots, which is what lets two independent content domains (an editor and a preview, two documents, two tests)
 *   coexist without one's ids resolving in the other.
 * - `instance()` IS THE PROCESS DEFAULT (domain 0): the registry the SDK's content objects take their id from. A
 *   host that wants its own arena constructs one and passes ids in explicitly; nothing forces the default.
 *
 * @warning
 * - Allocation and release take the registry's lock; `alive()` takes it for the read. They are called once per
 *   content object lifetime, never per frame - the lock is short and deliberately does not guard anything else.
 * - Do NOT call `release()` for an id whose object is still reachable: a live object with a released id is a handle
 *   that answers gone while its values are still drawn.
 */
class VN_GRAPHICS_API ContentRegistry {
  public:
    /**
     * @brief Creates an empty registry for one domain.
     *
     * @param domain The domain byte every handle of this registry carries (0 is the process default).
     */
    explicit ContentRegistry(std::uint8_t domain = 0) noexcept;

    ContentRegistry(const ContentRegistry&) = delete;
    ContentRegistry& operator=(const ContentRegistry&) = delete;

  public:
    /**
     * @brief Gets the process default registry (domain 0).
     *
     * @return The registry; it lives as long as the process.
     */
    [[nodiscard]] static ContentRegistry& instance() noexcept;

    /**
     * @brief Allocates an id of one kind.
     *
     * @tparam Tag The content kind the id names.
     * @return The id; it is alive until released.
     */
    template <class Tag>
    [[nodiscard]] Id<Tag> allocate()
    {
        return Id<Tag>(allocateHandle());
    }

    /**
     * @brief Releases a handle: the slot becomes reusable with the next generation.
     *
     * @details
     * - A stale handle (already released, another domain, never allocated) is a no-op, so a double release cannot
     *   move a generation twice and orphan the slot's next occupant.
     *
     * @param handle The handle to release.
     */
    void release(Handle handle) noexcept;

    /**
     * @brief Says whether @p handle still names a live slot in this registry's domain.
     *
     * @param handle The handle to test.
     * @return true when the slot is still occupied by that exact generation.
     */
    [[nodiscard]] bool alive(Handle handle) const noexcept;

    /**
     * @brief Gets how many slots this registry has ever had to allocate (its high-water mark).
     *
     * @return The slot count; it grows when a release left no reusable slot.
     */
    [[nodiscard]] std::size_t peakObjects() const noexcept;

  private:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFU;

    struct Slot {
        std::uint32_t generation{ 1 };
        std::uint32_t next_free{ kNone };
    };

    [[nodiscard]] Handle allocateHandle() noexcept;

    mutable std::mutex mutex_;
    std::vector<Slot>  slots_;
    std::uint32_t      free_head_{ kNone };
    std::uint8_t       domain_{ 0 };
};

VN_GRAPHICS_NS_END

namespace std
{

/** @brief Hashes a handle by its bits: the one value an id is (see Handle::bits). */
template <>
struct hash<vn::graphics::Handle> {
    [[nodiscard]] std::size_t operator()(const vn::graphics::Handle& handle) const noexcept
    {
        return std::hash<std::uint64_t>{}(handle.bits());
    }
};

/** @brief Hashes a typed id by the handle it wraps, so a table can be keyed by `NodeId` or `MaterialId`. */
template <class Tag>
struct hash<vn::graphics::Id<Tag>> {
    [[nodiscard]] std::size_t operator()(const vn::graphics::Id<Tag>& id) const noexcept
    {
        return std::hash<vn::graphics::Handle>{}(id.handle());
    }
};

} // namespace std
