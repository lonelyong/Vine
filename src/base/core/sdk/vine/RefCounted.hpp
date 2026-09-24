#pragma once

#include "core_global.hpp"

#include <atomic>

VN_CORE_NS_BEGIN

/**
 * @brief CRTP mixin providing intrusive strong reference counting.
 *
 * Derive from RefCounted<Derived> to give Derived an in-object atomic
 * reference counter. The counter is managed through the ADL free functions
 * intrusive_ptr_add_ref / intrusive_ptr_release, so any type (no common base
 * class required) can be owned by intrusive_ptr<Derived>. Because the counter
 * lives inside the object, constructing an intrusive_ptr never allocates and
 * never throws.
 *
 * Objects are destroyed by intrusive_ptr_release when the last reference is
 * released; the destructor is protected so a raw delete of the subobject is
 * prevented. Copy and move are deleted: a reference-counted object has
 * identity and must not be copied.
 *
 * @tparam Derived The type deriving from this mixin. It must be complete when
 *         intrusive_ptr_release is instantiated (i.e. when the last
 *         intrusive_ptr is destroyed).
 */
template <typename Derived>
class RefCounted {
  public:
    RefCounted() noexcept                     = default;
    RefCounted(const RefCounted&)            = delete;
    RefCounted& operator=(const RefCounted&) = delete;
    RefCounted(RefCounted&&)                 = delete;
    RefCounted& operator=(RefCounted&&)      = delete;


  public:
    /**
     * @brief Returns the current strong reference count.
     *
     * @return The number of intrusive_ptr instances currently referencing this object.
     */
    unsigned long useCount() const noexcept
    {
        return refs_.load(std::memory_order_relaxed);
    }

  protected:
    ~RefCounted() = default;

    /**
     * @brief Increments the strong reference count.
     */
    void addRef() const noexcept
    {
        refs_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Decrements the strong reference count and destroys the object
     *        when it reaches zero.
     */
    void release() const noexcept
    {
        if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete static_cast<const Derived*>(this);
    }

  private:
    friend void intrusive_ptr_add_ref(const RefCounted* p) noexcept
    {
        p->addRef();
    }

    friend void intrusive_ptr_release(const RefCounted* p) noexcept
    {
        p->release();
    }

  private:
    mutable std::atomic<unsigned long> refs_{ 0 };
};

VN_CORE_NS_END
