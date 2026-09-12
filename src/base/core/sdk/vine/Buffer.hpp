#pragma once
#include "core_global.hpp"

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "intrusive_ptr.hpp"
#include "RefCounted.hpp"

V_CORE_NS_BEGIN

/**
 * @brief A reference-counted, fixed-length run of T.
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
 * LENGTH IS FIXED. There is deliberately no resize: a view or byte pointer handed out earlier must stay
 * valid, so the count is decided once and only the elements may be written. Callers that build a buffer
 * incrementally do it in their own `std::vector` and hand it over (see the vector constructor).
 *
 * @tparam T Element type. Must be trivially copyable for `bytes()` to be meaningful.
 */
template <typename T>
class Buffer : public RefCounted<Buffer<T>> {
  public:
    using value_type = T;
    using size_type  = std::size_t;

  public:
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
     * Writing is allowed — the LENGTH is what is fixed, not the contents — but a caller that did so after
     * handing `bytes()` to a device must announce the change, or the device keeps the old contents.
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

  private:
    std::vector<T> data_;
};

V_CORE_NS_END
