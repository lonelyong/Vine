#pragma once

#include "async_global.hpp"

#include <concepts>
#include <coroutine>

V_ASYNC_NS_BEGIN

/**
 * @brief Constrains a type to the minimal awaiter protocol.
 *
 * A type satisfying this concept can be awaited with co_await. Use it to
 * constrain custom awaiters or types accepted by generic APIs. await_ready must
 * yield something convertible to bool; await_suspend's return type is left to
 * the awaiter, because void, bool and std::coroutine_handle<> are all valid
 * protocol forms.
 */
template<typename T>
concept Awaitable = requires(T&& a) {
    { a.await_ready() } -> std::convertible_to<bool>;
    a.await_suspend(std::coroutine_handle<>{});
    a.await_resume();
};

/**
 * @brief Constrains a type that can decide where a coroutine resumes.
 *
 * A Schedulable type exposes a callable schedule() returning an awaitable
 * object. Awaiting that object suspends the coroutine and later resumes it in
 * the execution context chosen by the type.
 */
template<typename T>
concept Schedulable = requires(T& s) {
    { s.schedule() } -> Awaitable;
};

V_ASYNC_NS_END
