#pragma once

#include <cstdint>
#include <string>

#include <vine/String.hpp>
#include <vine/backend_vk/backend_vk_global.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief Builds a message fragment from a count.
 *
 * @details
 * - A diagnostic that names the numbers a host can act on ("asks for 8, this device offers 4") is worth more than one
 *   that only says "too many", and `vn::String` holds UTF-8 while `std::to_string` answers UTF-8 bytes, so the
 *   conversion is a copy.
 * - It is an internal header because two units build sentences (`Device` for the pool and window, `TargetTable` for a
 *   refused shape) and a second copy is how one of them ends up spelling 8 as "8.0".
 *
 * @param value The number to spell.
 * @return The number as text.
 */
[[nodiscard]] inline vn::String numberText(const std::uint32_t value)
{
    return vn::String::fromUtf8(std::to_string(value));
}

VN_BACKEND_VK_NS_END
