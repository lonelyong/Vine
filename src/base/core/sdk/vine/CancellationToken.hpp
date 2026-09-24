#pragma once

#include "core_global.hpp"

#include <stop_token>

VN_CORE_NS_BEGIN

/**
 * @brief Token that can be polled to observe a cancellation request.
 *
 * Cancellable async operations accept an optional token and throw
 * TaskCancelledException when cancellation is requested before the operation
 * completed.
 */
using CancellationToken = std::stop_token;

/**
 * @brief Source that owns the cancellation state and can request cancellation.
 */
using CancellationSource = std::stop_source;

VN_CORE_NS_END
