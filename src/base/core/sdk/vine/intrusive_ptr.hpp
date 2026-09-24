#pragma once

#include "core_global.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>

VN_CORE_NS_BEGIN

/**
 * @brief Intrusive strong reference pointer (boost::intrusive_ptr style).
 *
 * intrusive_ptr<T> is a single owning pointer managing T through the ADL free functions
 * intrusive_ptr_add_ref / intrusive_ptr_release. T does not need a common base
 * class: either provide those functions yourself, or derive T from
 * RefCounted<T>. There is no separate control block; the counter lives inside
 * T, so construction is allocation-free and non-throwing.
 *
 * @tparam T The referenced type; must be complete at instantiation and expose
 *         intrusive_ptr_add_ref(T*) / intrusive_ptr_release(T*) via ADL.
 */
template <typename T>
class intrusive_ptr {
  private:
    template <typename U>
    friend class intrusive_ptr;

    T* px_{ nullptr };

    void addRef() const
    {
        if (px_)
            intrusive_ptr_add_ref(px_);
    }

    void release() noexcept
    {
        if (px_)
            intrusive_ptr_release(px_);
    }

  public:
    using element_type = T;

    /**
     * @brief Constructs a null pointer.
     */
    intrusive_ptr() noexcept = default;

    /**
     * @brief Constructs a null pointer from nullptr.
     *
     * Allows passing nullptr to functions taking an owning intrusive_ptr
     * parameter (e.g. to clear a slot), mirroring std smart pointers.
     */
    intrusive_ptr(std::nullptr_t) noexcept
      : px_(nullptr)
    {}

    /**
     * @brief Takes ownership of p and adds a reference.
     *
     * @param p Raw pointer to adopt, or nullptr.
     */
    explicit intrusive_ptr(T* p)
      : px_(p)
    {
        addRef();
    }

    /**
     * @brief Adopts p, optionally without adding a reference.
     *
     * @param p Raw pointer to adopt, or nullptr.
     * @param add_ref false to adopt an already-held reference.
     */
    intrusive_ptr(T* p, bool add_ref)
      : px_(p)
    {
        if (add_ref)
            this->addRef();
    }

    /**
     * @brief Copy constructor; increments the reference count.
     */
    intrusive_ptr(const intrusive_ptr& other)
      : px_(other.px_)
    {
        addRef();
    }

    /**
     * @brief Converting constructor from intrusive_ptr<U>.
     *
     * @tparam U Source element type convertible to T.
     */
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    intrusive_ptr(const intrusive_ptr<U>& other)
      : px_(other.px_)
    {
        addRef();
    }

    /**
     * @brief Releases the reference.
     */
    ~intrusive_ptr()
    {
        release();
    }

    /**
     * @brief Copy assignment; releases the old reference and takes a new one.
     *
     * @return *this.
     */
    intrusive_ptr& operator=(const intrusive_ptr& other)
    {
        intrusive_ptr(other).swap(*this);
        return *this;
    }

    /**
     * @brief Converting assignment from intrusive_ptr<U>.
     *
     * @return *this.
     */
    template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    intrusive_ptr& operator=(const intrusive_ptr<U>& other)
    {
        intrusive_ptr(other).swap(*this);
        return *this;
    }

    /**
     * @brief Assigns a raw pointer, taking ownership.
     *
     * @return *this.
     */
    intrusive_ptr& operator=(T* p)
    {
        intrusive_ptr(p).swap(*this);
        return *this;
    }

    /**
     * @brief Releases the reference and becomes null.
     */
    void reset()
    {
        intrusive_ptr().swap(*this);
    }

    /**
     * @brief Replaces the managed pointer with p.
     *
     * @param p Raw pointer to adopt, or nullptr.
     */
    void reset(T* p)
    {
        intrusive_ptr(p).swap(*this);
    }

    /**
     * @brief Swaps with another pointer.
     */
    void swap(intrusive_ptr& other) noexcept
    {
        std::swap(px_, other.px_);
    }

    /**
     * @brief Returns the raw pointer.
     *
     * @return The managed pointer, or nullptr.
     */
    T* get() const noexcept
    {
        return px_;
    }

    /**
     * @brief Dereferences the managed object.
     *
     * @return A reference to the managed object.
     */
    T& operator*() const
    {
        return *px_;
    }

    /**
     * @brief Provides member access to the managed object.
     *
     * @return The raw pointer.
     */
    T* operator->() const noexcept
    {
        return px_;
    }

    /**
     * @brief Checks whether the pointer is non-null.
     */
    explicit operator bool() const noexcept
    {
        return px_ != nullptr;
    }

    /**
     * @brief Checks whether the pointer is non-null.
     *
     * @return true when non-null.
     */
    bool hasValue() const noexcept
    {
        return px_ != nullptr;
    }

    friend bool operator==(const intrusive_ptr& a, const intrusive_ptr& b) noexcept
    {
        return a.px_ == b.px_;
    }

    friend bool operator!=(const intrusive_ptr& a, const intrusive_ptr& b) noexcept
    {
        return a.px_ != b.px_;
    }

    friend bool operator==(const intrusive_ptr& a, T* b) noexcept
    {
        return a.px_ == b;
    }

    friend bool operator==(T* a, const intrusive_ptr& b) noexcept
    {
        return a == b.px_;
    }

    friend bool operator!=(const intrusive_ptr& a, T* b) noexcept
    {
        return a.px_ != b;
    }

    friend bool operator!=(T* a, const intrusive_ptr& b) noexcept
    {
        return a != b.px_;
    }

    friend bool operator==(const intrusive_ptr& a, std::nullptr_t) noexcept
    {
        return a.px_ == nullptr;
    }

    friend bool operator==(std::nullptr_t, const intrusive_ptr& a) noexcept
    {
        return a.px_ == nullptr;
    }

    friend bool operator!=(const intrusive_ptr& a, std::nullptr_t) noexcept
    {
        return a.px_ != nullptr;
    }

    friend bool operator!=(std::nullptr_t, const intrusive_ptr& a) noexcept
    {
        return a.px_ != nullptr;
    }
};

/**
 * @brief Constructs an object owned by an intrusive_ptr in place.
 *
 * Equivalent to intrusive_ptr<T>(new T(std::forward<Args>(args)...)): the
 * object's intrusive counter starts at 0 and the returned pointer takes the
 * first reference (see RefCounted). Mirrors std::make_unique / std::make_shared
 * so callers never write a bare `new`; existing raw pointers are still adopted
 * with intrusive_ptr<T>(p) / reset(p).
 *
 * @tparam T    Referenced type.
 * @tparam Args Constructor argument types.
 * @param args  Forwarded to T's constructor.
 * @return Owning pointer to the new object.
 */
template <typename T, typename... Args>
inline intrusive_ptr<T> make_intrusive(Args&&... args)
{
    return intrusive_ptr<T>(new T(std::forward<Args>(args)...));
}

/**
 * @brief Performs a static pointer cast.
 *
 * @tparam T Target element type.
 * @param r Source pointer.
 * @return A new owning pointer to the same object.
 */
template <typename T, typename U>
inline intrusive_ptr<T> static_pointer_cast(const intrusive_ptr<U>& r)
{
    return intrusive_ptr<T>(static_cast<T*>(r.get()));
}

/**
 * @brief Performs a const pointer cast.
 *
 * @tparam T Target element type.
 * @param r Source pointer.
 * @return A new owning pointer to the same object.
 */
template <typename T, typename U>
inline intrusive_ptr<T> const_pointer_cast(const intrusive_ptr<U>& r)
{
    return intrusive_ptr<T>(const_cast<T*>(r.get()));
}

/**
 * @brief Performs a dynamic pointer cast.
 *
 * @tparam T Target element type (must be polymorphic).
 * @param r Source pointer.
 * @return A new owning pointer, or null when the cast fails.
 */
template <typename T, typename U>
inline intrusive_ptr<T> dynamic_pointer_cast(const intrusive_ptr<U>& r)
{
    return intrusive_ptr<T>(dynamic_cast<T*>(r.get()));
}

/**
 * @brief Alias for intrusive_ptr.
 */
template <typename T>
using IPtr = intrusive_ptr<T>;

VN_CORE_NS_END
