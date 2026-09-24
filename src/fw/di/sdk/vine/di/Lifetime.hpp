#pragma once
#include "di_global.hpp"

VN_DI_NS_BEGIN

/**
 * @brief Controls how a resolved service instance is created and cached.
 */
enum class Lifetime
{
    /** @brief A single instance is created once and reused for every resolve. */
    Singleton,
    /** @brief A new instance is created on every resolve call. */
    Transient
};

VN_DI_NS_END
