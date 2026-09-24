#pragma once

#include "async_global.hpp"

#include <concepts>
#include <coroutine>
#include <type_traits>
#include <utility>

VN_ASYNC_NS_BEGIN

namespace detail {

/**
 * @brief Resolves the awaiter of an awaitable, exactly as co_await does.
 *
 * co_await tries, in order: a member operator co_await, a non-member operator
 * co_await, and finally the expression itself when it already is an awaiter.
 * Mirroring that lookup (as cppcoro's get_awaiter does) is what lets the
 * Awaitable concept accept the module's own awaitables: Task, SharedTask and
 * AsyncEvent are awaitable through operator co_await, not through await_ready.
 *
 * @tparam T Deduced type of the expression being awaited.
 * @param value Expression to resolve; not consumed.
 * @return The awaiter to drive, or value itself when it has no operator co_await.
 */
template<typename T>
decltype(auto) getAwaiter(T&& value)
{
    if constexpr (requires { std::forward<T>(value).operator co_await(); })
    {
        return std::forward<T>(value).operator co_await();
    }
    else if constexpr (requires { operator co_await(std::forward<T>(value)); })
    {
        return operator co_await(std::forward<T>(value));
    }
    else
    {
        return std::forward<T>(value);
    }
}

/**
 * @brief Type of the expression co_await would drive for T.
 */
template<typename T>
using AwaiterType = decltype(detail::getAwaiter(std::declval<T>()));

/**
 * @brief Whether T converts to bool the way co_await requires await_ready to.
 *
 * The language contextually converts await_ready's result to bool, which also
 * accepts an explicit conversion (e.g. an explicit operator bool), so this is
 * checked as a direct-initialisation rather than as implicit convertibility.
 */
template<typename T>
concept ContextuallyConvertibleToBool = requires(T&& value) { static_cast<bool>(std::forward<T>(value)); };

/**
 * @brief Whether R is the type of a std::coroutine_handle specialization.
 */
template<typename R>
inline constexpr bool s_isCoroutineHandle = false;

template<typename Promise>
inline constexpr bool s_isCoroutineHandle<std::coroutine_handle<Promise>> = true;

/**
 * @brief Whether R is one of the three await_suspend return forms.
 *
 * The language accepts exactly void, bool and a coroutine handle (the handle
 * form drives symmetric transfer). A reference to a handle, or any other type
 * merely convertible to one, is rejected by the compiler, so it must be
 * rejected here too: the point of the check is that a constrained API says no
 * for the same reasons co_await does.
 */
template<typename R>
concept SuspendResult = std::is_void_v<R>
    || std::is_same_v<std::remove_cv_t<R>, bool>
    || s_isCoroutineHandle<std::remove_cv_t<R>>;

/**
 * @brief Whether A fulfils the awaiter protocol co_await relies on.
 *
 * await_ready must convert contextually to bool, await_suspend must be callable
 * with a coroutine handle and return one of the three legal forms, and
 * await_resume must exist. The return type is part of the check because an
 * illegal form only fails once a coroutine is compiled, not when the awaiter is
 * handed to a constrained generic API.
 */
template<typename A>
concept Awaiter = requires(A& a, std::coroutine_handle<> h) {
    { a.await_ready() } -> ContextuallyConvertibleToBool;
    a.await_suspend(h);
    a.await_resume();
} && SuspendResult<decltype(std::declval<A&>().await_suspend(std::coroutine_handle<>{}))>;

} // namespace detail

/**
 * @brief Constrains a type that can be awaited with co_await.
 *
 * A type satisfies this concept when co_await would accept it: either it is an
 * awaiter itself, or it exposes operator co_await (member or non-member) whose
 * result is one. Task, SharedTask, AsyncEvent, every awaiter in this module and
 * plain types with an operator co_await therefore all satisfy it, while a
 * non-awaitable type and an awaiter with an illegal await_suspend return type
 * do not.
 *
 * Use it to constrain custom awaitables or types accepted by generic APIs.
 */
template<typename T>
concept Awaitable = detail::Awaiter<detail::AwaiterType<T&&>>;

/**
 * @brief Constrains a type the module can carry as a result value.
 *
 * Task, SharedTask and Generator cache their result in a std::optional, which
 * cannot hold a reference, an array or a function type (and std::destructible
 * is false for void, which the result-less forms do need). Requiring this at
 * the template's own boundary turns what would be an error deep inside
 * <optional> ("union member has reference type") into a constraint the caller
 * can read. Use a pointer or a std::reference_wrapper to return a reference
 * target.
 */
template<typename T>
concept StorableValue = std::is_void_v<T>
    || (std::destructible<T> && !std::is_reference_v<T> && !std::is_array_v<T>
        && !std::is_function_v<T>);

/**
 * @brief Constrains a type that can decide where a coroutine resumes.
 *
 * A Schedulable type exposes a callable schedule() returning an awaitable
 * object: a bare awaiter, or anything awaitable such as a Task. Awaiting that
 * object suspends the coroutine and later resumes it in the execution context
 * chosen by the type.
 */
template<typename T>
concept Schedulable = requires(T& s) {
    { s.schedule() } -> Awaitable;
};

VN_ASYNC_NS_END
