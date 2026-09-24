#pragma once

#include <vine/core_global.hpp>

#include <atomic>

#include <vine/deprecated/RefObject.hpp>

VN_CORE_NS_BEGIN

template <typename T>
class [[deprecated]] SPtr {
  private:
    template <typename TOther>
    friend class SPtr;

  public:
    /**
     * Construct from raw pointer without modifying the strong refcount.
     * This is intended for internal use by `WPtr::lock()` after it
     * atomically incremented the strong count.
     */
    SPtr(T* ptr, bool addRef)
      : ptr_(ptr)
    {
        if (ptr_ && addRef) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb)
                cb->strong_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    SPtr()
      : ptr_(nullptr)
    {}

    SPtr(T* ptr)
      : ptr_(ptr)
    {
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb)
                cb->strong_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    SPtr(const SPtr& other)
      : ptr_(other.ptr_)
    {
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb)
                cb->strong_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    template <typename TOther>
    SPtr(const SPtr<TOther>& other)
      : ptr_(other.ptr_)
    {
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb)
                cb->strong_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ~SPtr()
    {
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb) {
                if (cb->strong_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete ptr_;
            }
            else {
                delete ptr_;
            }
        }
    }

  public:
    T* get() const
    { return ptr_; }

    void set(T* ptr)
    {
        if (ptr == ptr_)
            return;
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb) {
                if (cb->strong_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    delete ptr_;
            }
            else {
                delete ptr_;
            }
        }
        ptr_ = ptr;
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb)
                cb->strong_refs.fetch_add(1, std::memory_order_relaxed);
        }
    }

    T* release()
    {
        auto temp = ptr_;
        if (ptr_) {
            static_assert(std::is_base_of_v<RefObject, T>, "SPtr requires RefObject-based T");
            auto* cb = static_cast<RefObject*>(ptr_)->cb_;
            if (cb) {
                cb->strong_refs.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        ptr_ = nullptr;
        return temp;
    }

    void swap(SPtr& other)
    { std::swap(ptr_, other.ptr_); }

  public:
    bool operator!() const
    { return !ptr_; }

    T* operator->() const
    { return ptr_; }

    T& operator*() const
    { return *ptr_; }

    SPtr& operator=(const SPtr& right)
    {
        set(right.ptr_);
        return *this;
    }

    template <typename TOther>
    SPtr& operator=(const SPtr<TOther>& right)
    {
        set(right.ptr_);
        return *this;
    }

    template <typename TOther>
    SPtr& operator=(TOther* ptr)
    {
        set(ptr);
        return *this;
    }

    bool operator==(const SPtr& right) const
    { return ptr_ == right.ptr_; }

    bool operator==(const T* right) const
    { return ptr_ == right; }

    friend bool operator==(const T* left, const SPtr& right)
    { return left == right.ptr_; }

    bool operator!=(const SPtr& right) const
    { return ptr_ != right.ptr_; }

    bool operator!=(const T* right) const
    { return ptr_ != right; }

    friend bool operator!=(const T* left, const SPtr& right)
    { return left != right.ptr_; }

    bool operator<(const SPtr<T>& right) const
    { return ptr_ < right.ptr_; }

    bool operator<(const T* right) const
    { return ptr_ < right; }

    bool operator>(const SPtr<T>& right) const
    { return ptr_ > right.ptr_; }

    bool operator>(const T* right) const
    { return ptr_ > right; }

    bool hasValue() const
    { return ptr_ != nullptr; }

  private:
    T* ptr_;
};

template <typename T>
class [[deprecated]] WPtr {
  private:
    template <typename TOther>
    friend class WPtr;

  public:
    WPtr()
      : ptr_(nullptr)
      , cb_(nullptr)
    {}

    explicit WPtr(T* ptr)
      : ptr_(ptr)
      , cb_(ptr ? static_cast<RefObject*>(ptr)->cb_ : nullptr)
    {
        if (cb_)
            cb_->weak_refs.fetch_add(1, std::memory_order_relaxed);
    }

    WPtr(const SPtr<T>& rp)
      : ptr_(rp.get())
      , cb_(ptr_ ? static_cast<RefObject*>(ptr_)->cb_ : nullptr)
    {
        if (cb_)
            cb_->weak_refs.fetch_add(1, std::memory_order_relaxed);
    }

    WPtr(const WPtr& other)
      : ptr_(other.ptr_)
      , cb_(other.cb_)
    {
        if (cb_)
            cb_->weak_refs.fetch_add(1, std::memory_order_relaxed);
    }

    ~WPtr()
    {
        if (cb_ && cb_->weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete cb_;
    }

    WPtr& operator=(const WPtr& right)
    {
        if (*this == right)
            return *this;
        if (cb_ && cb_->weak_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete cb_;
        ptr_ = right.ptr_;
        cb_  = right.cb_;
        if (cb_)
            cb_->weak_refs.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }

    SPtr<T> lock() const
    {
        if (!cb_ || !ptr_)
            return SPtr<T>();

        unsigned int s = cb_->strong_refs.load(std::memory_order_acquire);
        while (s != 0) {
            if (cb_->strong_refs.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) { return SPtr<T>(ptr_, false); }
        }
        return SPtr<T>();
    }

  private:
    T*                          ptr_;
    RefObject::PtrControlBlock* cb_;

  public:
    bool operator==(const WPtr& right) const
    { return ptr_ == right.ptr_ && cb_ == right.cb_; }

    bool operator!=(const WPtr& right) const
    { return ptr_ != right.ptr_ || cb_ != right.cb_; }

    bool operator==(const T* right) const
    { return ptr_ == right; }

    bool operator!=(const T* right) const
    { return ptr_ != right; }

    friend bool operator==(const T* left, const WPtr& right)
    { return left == right.ptr_; }

    friend bool operator!=(const T* left, const WPtr& right)
    { return left != right.ptr_; }

    bool hasValue() const
    { return ptr_ != nullptr; }

    T* get() const
    { return ptr_; }
};

template <typename T, typename Y>
inline SPtr<T> static_pointer_cast(const SPtr<Y>& rp)
{ return static_cast<T*>(rp.get()); }

template <typename T, typename Y>
inline SPtr<T> dynamic_pointer_cast(const SPtr<Y>& rp)
{ return dynamic_cast<T*>(rp.get()); }

template <typename T, typename Y>
inline SPtr<T> const_pointer_cast(const SPtr<Y>& rp)
{ return const_cast<T*>(rp.get()); }

VN_CORE_NS_END