#pragma once

#include <vine/vine_global.hpp>

/**
 * @file async_global.hpp
 * @brief Framework-wide conventions for the vn::async coroutine module.
 *
 * Lifetime contract: a coroutine suspended at a co_await must not be
 * destroyed concurrently by another thread while the awaited object is
 * completing it (between the completion thread swapping out the waiter list
 * and resuming each waiter). Abandoning an await — destroying the awaiting
 * coroutine before completion — is supported: every awaiter unregisters
 * itself on destruction. But concurrent destruction during completion is
 * undefined behavior, exactly as in cppcoro/folly.
 *
 * Coroutine-lambda caveat: a coroutine *lambda* keeps its captures in the
 * closure object, which is a temporary destroyed at the end of the full
 * expression. The coroutine frame only stores the implicit object parameter
 * (a pointer to the closure), so the closure must outlive the coroutine.
 * Immediately invoking a capturing coroutine lambda and resuming the returned
 * task later reads destroyed closure state — undefined behavior (cppreference:
 * "uses (anonymous lambda type)::i after free"). This is user-side UB, not a
 * compiler defect. Prefer a named coroutine function (parameters are copied
 * into the coroutine state), or pass values as coroutine parameters, or keep
 * the lambda object alive for the coroutine's lifetime. Plain lambdas without
 * co_await/co_return/co_yield are unaffected.
 */

#define VN_ASYNC_NS_BEGIN \
    VN_ROOT_NS_BEGIN \
    namespace async \
    {

#define VN_ASYNC_NS_END \
    } \
    VN_ROOT_NS_END
