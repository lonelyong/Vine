#pragma once
#include "core_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "intrusive_ptr.hpp"
#include "RefCounted.hpp"

V_CORE_NS_BEGIN

/**
 * @brief A reference-counted run of T that two consumers can share.
 *
 * WHY IT EXISTS. Vector data used to be held twice: a model keeps its vertices as `std::vector<T>` (typed
 * access is what modelling, collision, picking and IO need) while the renderer needs the same bytes at a
 * layout it can hand to a device. Copying between the two views duplicated every vertex in memory, and for
 * `Vec3f`-like element types the copy produced a BYTE-IDENTICAL block — pure overhead, not a conversion.
 * A shared buffer lets both sides describe ONE allocation: the model keeps typed access through `view()`,
 * and the consumer takes `bytes()`.
 *
 * WHY NOT DERIVE FROM std::vector. `std::vector` has no virtual destructor, so deleting a derived object
 * through a vector reference is undefined behaviour, and passing it anywhere that takes `std::vector&`
 * SLICES the reference count away. Holding a vector as a member gets the same storage without either trap.
 *
 * WHY NOT A `std::shared_ptr<std::vector<T>>`. The repo's rule is `intrusive_ptr` for reference-counted
 * Vine objects and `shared_ptr` for plain data. This is the former: it has identity, is not copyable, and
 * is meant to be shared — exactly the shape `RefCounted` describes.
 *
 * GROWTH IS ANNOUNCED, NOT FORBIDDEN. A modelling API appends (a mesh builder adds vertices one at a time),
 * so there is an append API and the length is not fixed. What a consumer must not do is assume the bytes it
 * read are still current: any mutation INVALIDATES byte pointers and views taken before it, and every
 * mutation bumps revision(). A consumer that cached the bytes compares the revision it read against
 * revision() before reusing them — the same rule `Texture` and `ShaderProgram` already follow for "the same
 * object, new contents". Freezing the length instead would have made the model side unexpressible.
 *
 * @tparam T Element type. Must be trivially copyable for `bytes()` to be meaningful.
 */
template <typename T>
class Buffer : public RefCounted<Buffer<T>> {
  public:
    using value_type = T;
    using size_type  = std::size_t;

  public:
    /** @brief Creates an empty buffer. */
    Buffer() = default;

    /**
     * @brief Creates a buffer of @p count value-initialised elements.
     *
     * @param count Number of elements; may be 0.
     */
    explicit Buffer(size_type count)
      : data_(count)
    {
    }

    /**
     * @brief Adopts an existing vector.
     *
     * The vector is moved in, so a caller that builds data in a `std::vector` (a loader reading a file, a
     * mesh builder) hands the storage over rather than copying it.
     *
     * @param values Storage to adopt.
     */
    explicit Buffer(std::vector<T> values)
      : data_(std::move(values))
    {
    }

  public:
    /**
     * @brief Gets the element count.
     *
     * @return Number of elements.
     */
    [[nodiscard]] size_type size() const noexcept
    {
        return data_.size();
    }

    /**
     * @brief States whether the buffer holds no elements.
     *
     * @return true when the count is 0.
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return data_.empty();
    }

    /**
     * @brief Gets writable access to the elements.
     *
     * Writing through this pointer CANNOT be seen by the buffer, so a caller that does it must announce the
     * change with setRevision() — otherwise a consumer that compares revisions keeps reusing what it read.
     * This is the same reason the write path exists at all: the model side writes vertices in place far more
     * often than it rebuilds the array.
     *
     * @return Pointer to the first element, or null when empty.
     */
    [[nodiscard]] T* data() noexcept
    {
        return data_.data();
    }

    /**
     * @brief Gets read-only access to the elements.
     *
     * @return Pointer to the first element, or null when empty.
     */
    [[nodiscard]] const T* data() const noexcept
    {
        return data_.data();
    }

    /**
     * @brief Gets one element.
     *
     * @param index Element index; must be below `size()`.
     * @return The element.
     */
    [[nodiscard]] T& operator[](size_type index) noexcept
    {
        return data_[index];
    }

    /**
     * @brief Gets one element, read-only.
     *
     * @param index Element index; must be below `size()`.
     * @return The element.
     */
    [[nodiscard]] const T& operator[](size_type index) const noexcept
    {
        return data_[index];
    }

    /**
     * @brief Gets a typed view of the elements.
     *
     * This is the TYPED face of the buffer: the model side keeps `Vec3f`-style access without a cast, and
     * the view is a value (`ptr` + `size`), so it costs nothing and needs no lifetime management of its own.
     *
     * @return View over every element.
     */
    [[nodiscard]] std::span<T> view() noexcept
    {
        return std::span<T>(data_.data(), data_.size());
    }

    /**
     * @brief Gets a typed read-only view of the elements.
     *
     * @return View over every element.
     */
    [[nodiscard]] std::span<const T> view() const noexcept
    {
        return std::span<const T>(data_.data(), data_.size());
    }

    /**
     * @brief Gets the byte view a device-side consumer uploads.
     *
     * This is the TYPE-ERASED face of the buffer, and it is the reason a single buffer can feed both sides:
     * the bytes ARE the typed elements, so nothing is converted, copied or repacked — the consumer only has
     * to be told the layout (which element type, how many components, what stride).
     *
     * @return The elements as raw bytes.
     */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "Buffer::bytes() reinterprets the elements; a non-trivial type has no byte view");

        return std::as_bytes(view());
    }

    /**
     * @brief Gets the content revision.
     *
     * Bumped by every mutation, so a consumer that cached bytes can tell "the same buffer, still the same
     * contents" from "the same buffer, new contents". A pointer alone cannot: a cache keyed by the buffer's
     * address would otherwise keep serving the contents it read first.
     *
     * @return Monotonic revision counter (starts at 0).
     */
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return revision_;
    }

    /**
     * @brief Sets the content revision by hand.
     *
     * Needed because the write paths the buffer cannot see — a non-const `data()` pointer, an element
     * reference from `operator[]`, a write from another thread — are exactly the ones the automatic bump
     * misses. A caller that wrote through any of them announces it here. The usual value is
     * `revision() + 1`.
     *
     * The counter is only ever COMPARED, so nothing breaks if it jumps; but lowering it is a real hazard: a
     * consumer holding a cached revision could then treat old bytes as current, or vice versa. Treat it as
     * monotonic unless a caller genuinely needs otherwise.
     *
     * @param revision Revision to report.
     */
    void setRevision(std::uint64_t revision) noexcept
    {
        revision_ = revision;
    }

    /**
     * @brief Reserves room for @p count elements.
     *
     * Does NOT count as a mutation of the contents, but it may move the storage, so views taken earlier are
     * invalidated all the same — take a view after the building is done.
     *
     * @param count Elements to make room for.
     */
    void reserve(size_type count)
    {
        data_.reserve(count);
    }

    /**
     * @brief Appends one element.
     *
     * @param value Element to append (copied).
     */
    void push_back(const T& value)
    {
        data_.push_back(value);
        ++revision_;
    }

    /**
     * @brief Appends every element of @p values.
     *
     * @param values Elements to append.
     */
    void append(std::span<const T> values)
    {
        if (values.empty()) {
            return;
        }

        data_.insert(data_.end(), values.begin(), values.end());
        ++revision_;
    }

    /**
     * @brief Removes every element.
     */
    void clear() noexcept
    {
        data_.clear();
        ++revision_;
    }

  private:
    std::vector<T> data_;
    std::uint64_t  revision_ = 0;
};

V_CORE_NS_END
