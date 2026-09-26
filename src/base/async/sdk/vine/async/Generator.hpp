#pragma once

#include "async_global.hpp"

#include <coroutine>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>

#include "Concepts.hpp"

VN_ASYNC_NS_BEGIN

/**
 * @brief Single-pass, lazy synchronous generator.
 *
 * A coroutine that produces a sequence of values with co_yield. The body does
 * not run until iteration starts; each co_yield suspends and hands one value
 * to the consumer, and the next increment resumes the body. Usable with
 * range-for, and a C++20 input_range, so ranges algorithms and views accept it.
 * Move-only; not awaitable.
 *
 * @tparam T Produced value type. Must be storable (see StorableValue).
 */
template<StorableValue T>
class Generator
{
  public:
    /**
     * @brief The coroutine's control room, living inside the frame.
     */
    struct promise_type
    {
        [[nodiscard]]
        Generator<T> get_return_object() noexcept
        {
            return Generator<T>{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        /// Do not run the body until iteration starts.
        std::suspend_always initial_suspend() noexcept { return {}; }

        /// Keep the frame alive until the generator is destroyed.
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept { exception_ = std::current_exception(); }

        /// Stores a value and suspends until the consumer pulls the next one.
        std::suspend_always yield_value(T value)
        {
            current_.emplace(std::move(value));
            return {};
        }

        /// The value most recently produced by co_yield.
        [[nodiscard]]
        const T& value() const noexcept
        {
            return *current_;
        }

        std::exception_ptr exception() const noexcept { return exception_; }

      private:
        std::optional<T> current_{};
        std::exception_ptr exception_{};
    };

  public:
    /**
     * @brief Single-pass iterator over the produced values.
     *
     * Satisfies std::input_iterator: iterator_concept is input_iterator_tag,
     * and both increment forms exist, which is what the C++20 iterator
     * protocol requires (a missing postfix increment alone was enough to keep
     * std::ranges algorithms and views from accepting a Generator).
     */
    class iterator
    {
      public:
        using iterator_category = std::input_iterator_tag;
        using iterator_concept  = std::input_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const T*;
        using reference         = const T&;

        iterator() noexcept = default;

        explicit iterator(std::coroutine_handle<promise_type> coro) noexcept : coro_(coro) {}

        bool operator==(const iterator& other) const noexcept
        {
            return coro_ == other.coro_;
        }

        bool operator!=(const iterator& other) const noexcept
        {
            return !(*this == other);
        }

        iterator& operator++()
        {
            coro_.resume();
            if (coro_.done())
            {
                // Read the outcome and invalidate before rethrowing: a throwing
                // increment must leave `it == end()`. Leaving the handle set
                // would make the caller's next increment resume a coroutine
                // parked at final_suspend, which is undefined behaviour.
                std::exception_ptr ex = coro_.promise().exception();
                coro_ = nullptr;
                if (ex)
                {
                    std::rethrow_exception(ex);
                }
            }
            return *this;
        }

        /// Postfix increment; the protocol requires it even for single-pass use.
        void operator++(int) { ++*this; }

        [[nodiscard]]
        reference operator*() const noexcept
        {
            return coro_.promise().value();
        }

        [[nodiscard]]
        pointer operator->() const noexcept
        {
            return std::addressof(coro_.promise().value());
        }

      private:
        std::coroutine_handle<promise_type> coro_{};
    };

  public:
    Generator() noexcept = default;

    explicit Generator(std::coroutine_handle<promise_type> coro) noexcept : coro_(coro) {}

    Generator(Generator&& other) noexcept : coro_(std::exchange(other.coro_, {})) {}

    Generator& operator=(Generator&& other) noexcept
    {
        if (this != &other)
        {
            if (coro_)
            {
                coro_.destroy();
            }
            coro_ = std::exchange(other.coro_, {});
        }
        return *this;
    }

    ~Generator()
    {
        if (coro_)
        {
            coro_.destroy();
        }
    }

    Generator(const Generator&) = delete;
    Generator& operator=(const Generator&) = delete;

    /**
     * @brief Resumes to the first co_yield and returns an iterator.
     *
     * @return Iterator to the first value, or end() for an empty generator.
     */
[[nodiscard]]
    iterator begin()
    {
        if (coro_)
        {
            coro_.resume();
            if (coro_.done())
            {
                // The generator finished before (or without) its first co_yield.
                // This frame is done, so free it here: dropping the handle instead
                // would leak it, and rethrowing before dropping it would leave a
                // handle parked at final_suspend behind.
                std::exception_ptr ex = coro_.promise().exception();
                coro_.destroy();
                coro_ = nullptr;
                if (ex)
                {
                    std::rethrow_exception(ex);
                }
            }
        }
        return iterator{ coro_ };
    }

    /// @return Sentinel iterator.
    [[nodiscard]]
    iterator end() const noexcept
    {
        return iterator{};
    }

  private:
    std::coroutine_handle<promise_type> coro_{};
};

VN_ASYNC_NS_END
