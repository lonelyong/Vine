#pragma once
#include "core_global.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

VN_CORE_NS_BEGIN

template <typename T, std::size_t InlineCapacity>
class SmallVector {
    static_assert(InlineCapacity > 0, "SmallVector requires InlineCapacity >= 1");

  public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;
    using iterator        = T*;
    using const_iterator  = const T*;

  public:
    SmallVector()
      : data_(inline_ptr())
      , size_(0)
      , capacity_(InlineCapacity)
    {}

    explicit SmallVector(size_type count)
      : SmallVector()
    {
        resize(count);
    }

    SmallVector(std::initializer_list<T> init)
      : SmallVector()
    {
        reserve(init.size());
        relocate_elements(const_cast<T*>(init.begin()), init.size(), data_);
        size_ = init.size();
    }

    SmallVector(const SmallVector& other)
      : SmallVector()
    {
        reserve(other.size_);
        relocate_elements(const_cast<T*>(other.data()), other.size_, data_);
        size_ = other.size_;
    }

    SmallVector(SmallVector&& other)
      : data_(inline_ptr())
      , size_(0)
      , capacity_(InlineCapacity)
    {
        move_from(std::move(other));
    }

    ~SmallVector()
    {
        destroy_elements();
        if (using_heap())
            deallocate(data_);
    }

    SmallVector& operator=(const SmallVector& other)
    {
        if (this != &other) {
            destroy_elements();
            reserve(other.size_);
            relocate_elements(const_cast<T*>(other.data()), other.size_, data_);
            size_ = other.size_;
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other)
    {
        if (this != &other) {
            destroy_elements();
            if (using_heap())
                deallocate(data_);
            data_     = inline_ptr();
            size_     = 0;
            capacity_ = InlineCapacity;
            move_from(std::move(other));
        }
        return *this;
    }

  public:
    size_type size() const
    {
        return size_;
    }

    size_type capacity() const
    {
        return capacity_;
    }

    bool empty() const
    {
        return size_ == 0;
    }

    reference operator[](size_type i)
    {
        assert(i < size_);
        return data_[i];
    }

    const_reference operator[](size_type i) const
    {
        assert(i < size_);
        return data_[i];
    }

    reference at(size_type i)
    {
        if (i >= size_)
            throw std::out_of_range("SmallVector::at");
        return data_[i];
    }

    const_reference at(size_type i) const
    {
        if (i >= size_)
            throw std::out_of_range("SmallVector::at");
        return data_[i];
    }

    reference front()
    {
        assert(!empty());
        return data_[0];
    }

    const_reference front() const
    {
        assert(!empty());
        return data_[0];
    }

    reference back()
    {
        assert(!empty());
        return data_[size_ - 1];
    }

    const_reference back() const
    {
        assert(!empty());
        return data_[size_ - 1];
    }

    pointer data()
    {
        return data_;
    }

    const_pointer data() const
    {
        return data_;
    }

    iterator begin()
    {
        return data_;
    }

    const_iterator begin() const
    {
        return data_;
    }

    const_iterator cbegin() const
    {
        return data_;
    }

    iterator end()
    {
        return data_ + size_;
    }

    const_iterator end() const
    {
        return data_ + size_;
    }

    const_iterator cend() const
    {
        return data_ + size_;
    }

    void push_back(const T& value)
    {
        emplace_back(value);
    }

    void push_back(T&& value)
    {
        emplace_back(std::move(value));
    }

    template <typename... Args>
    reference emplace_back(Args&&... args)
    {
        ensure_capacity();
        T* ptr = std::construct_at(data_ + size_, std::forward<Args>(args)...);
        ++size_;
        return *ptr;
    }

    void pop_back()
    {
        if (empty()) {
            return;
        }
        --size_;
        std::destroy_at(data_ + size_);
    }

    /**
     * @ brief Reserve capacity.
     * Provides strong exception guarantee when T is nothrow movable or copyable.
     * For move-only types with a throwing move constructor, provides basic guarantee: the container remains valid, but some elements may have been moved-from.
     */
    void reserve(size_type new_capacity)
    {
        if (new_capacity <= capacity_)
            return;

        T* new_data = allocate(new_capacity);

        try {
            relocate_elements(data_, size_, new_data);
        }
        catch (...) {
            deallocate(new_data);
            throw;
        }

        /**
         * Old elements have been moved-from (or copied-from) — destroy them.
         */
        std::destroy_n(data_, size_);

        if (using_heap())
            deallocate(data_);

        data_     = new_data;
        capacity_ = new_capacity;
    }

    void resize(size_type count)
    {
        if (count > size_) {
            reserve(count);
            while (size_ < count) emplace_back();
        }
        else {
            while (size_ > count) pop_back();
        }
    }

    void clear()
    {
        std::destroy_n(data_, size_);
        size_ = 0;
    }

    /**
     * @brief Release heap memory and shrink capacity to inline.
     *
     * When size_ < InlineCapacity, elements are moved back to the inline buffer
     * and the heap allocation is freed.  When size_ >= InlineCapacity and using
     * heap, the allocation is reduced to exactly size_ to release excess memory.
     */
    void shrink_to_fit()
    {
        if (!using_heap())
            return;

        if (size_ < InlineCapacity) {
            // --- move back to inline buffer ---
            T* const inline_data = inline_ptr();
            relocate_elements(data_, size_, inline_data);
            std::destroy_n(data_, size_);
            deallocate(data_);
            data_     = inline_data;
            capacity_ = InlineCapacity;
        }
        else if (size_ < capacity_) {
            // --- shrink heap allocation to exact size ---
            T* new_data = allocate(size_);
            try {
                relocate_elements(data_, size_, new_data);
            }
            catch (...) {
                deallocate(new_data);
                throw;
            }
            std::destroy_n(data_, size_);
            deallocate(data_);
            data_     = new_data;
            capacity_ = size_;
        }
    }

  private:
    T* inline_ptr()
    {
        return reinterpret_cast<T*>(buffer_);
    }

    const T* inline_ptr() const
    {
        return reinterpret_cast<const T*>(buffer_);
    }

    bool using_heap() const
    {
        return data_ != inline_ptr();
    }

    void ensure_capacity()
    {
        if (size_ == capacity_) {
            if (size_ >= std::numeric_limits<size_t>::max() / 2) {
                reserve(std::numeric_limits<size_t>::max());
            }
            else {
                reserve(capacity_ * 2);
            }
        }
    }

    /**
     * @brief Allocate raw uninitialized heap memory, respecting alignment.
     */
    static T* allocate(size_type n)
    {
        if constexpr (alignof(T) > alignof(std::max_align_t)) {
            return static_cast<T*>(::operator new(sizeof(T) * n, std::align_val_t(alignof(T))));
        }
        else {
            return static_cast<T*>(::operator new(sizeof(T) * n));
        }
    }

    /**
     * @brief Deallocate raw heap memory.
     */
    static void deallocate(T* p)
    {
        if constexpr (alignof(T) > alignof(std::max_align_t)) {
            ::operator delete(p, std::align_val_t(alignof(T)));
        }
        else {
            ::operator delete(p);
        }
    }

    /**
     * @brief Relocate elements from [src, src+n) to uninitialized dst.
     *
     * Prefers move construction via std::uninitialized_move_n, falls back to
     * std::uninitialized_copy_n if move is unavailable, and throws if neither
     * is possible.  On exception, previously constructed elements in dst are
     * destroyed and the exception is rethrown — src is untouched.
     */
    static void relocate_elements(T* src, size_type n, T* dst)
    {
        if constexpr (std::is_trivially_copyable_v<T>) {
            memcpy(dst, src, n * sizeof(T));
        }
        else {
            if constexpr (std::is_move_constructible_v<T>) {
                std::uninitialized_move_n(src, n, dst);
            }
            else if constexpr (std::is_copy_constructible_v<T>) {
                std::uninitialized_copy_n(src, n, dst);
            }
            else {
                throw std::runtime_error("SmallVector::relocate_elements: T is neither move nor copy constructible");
            }
        }
    }

#if 0 // Manual implementation kept for reference
    static size_type relocate_elements_manual(T* src, size_type n, T* dst)
    {
        size_type i = 0;
        try {
            for (; i < n; ++i) {
                if constexpr (std::is_move_constructible_v<T>)
                    std::construct_at(dst + i, std::move(src[i]));
                else if constexpr (std::is_copy_constructible_v<T>)
                    std::construct_at(dst + i, src[i]);
                else
                    throw std::runtime_error("SmallVector::relocate_elements: T is neither move nor copy constructible");
            }
        }
        catch (...) {
            std::destroy_n(dst, i);
            i = 0;
            throw;
        }
        return i;
    }
#endif

    void destroy_elements()
    {
        std::destroy_n(data_, size_);
        size_ = 0;

        // while (size_ > 0) {
        //     --size_;
        //     std::destroy_at(data_ + size_);
        // }
    }

    /**
     * Transfer state from a moved-from SmallVector.
     */
    void move_from(SmallVector&& other)
    {
        if (other.using_heap()) {
            /**
             * Steal heap buffer — O(1)
             */
            data_     = other.data_;
            size_     = other.size_;
            capacity_ = other.capacity_;

            other.data_     = other.inline_ptr();
            other.size_     = 0;
            other.capacity_ = InlineCapacity;
        }
        else if (other.size_ > 0) {
            /**
             * Move elements from other's inline buffer
             */
            reserve(other.size_);
            relocate_elements(other.data_, other.size_, data_);
            size_ = other.size_;
            std::destroy_n(other.data_, other.size_);
            other.size_ = 0;
        }
    }

  private:
    alignas(T) std::byte buffer_[sizeof(T) * InlineCapacity];
    T*        data_;
    size_type size_;
    size_type capacity_;
};

VN_CORE_NS_END
