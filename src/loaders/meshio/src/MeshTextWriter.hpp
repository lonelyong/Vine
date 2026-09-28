#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include <vine/meshio/meshio_global.hpp>

VN_MESHIO_NS_BEGIN

namespace detail
{

/// Returned by the writers below when the text no longer fits the line being built.
inline constexpr std::size_t kNoRoom = std::numeric_limits<std::size_t>::max();

/**
 * @brief Appends a text fragment to a line.
 *
 * @param out The line being built.
 * @param at Offset to append at, or kNoRoom to keep the line over-full.
 * @param text The fragment to append.
 * @return The offset after the fragment, or kNoRoom when it does not fit.
 */
inline std::size_t appendText(std::span<char> out, std::size_t at, std::string_view text) noexcept
{
    if (at == kNoRoom || text.size() > out.size() - at) {
        return kNoRoom;
    }
    std::memcpy(out.data() + at, text.data(), text.size());
    return at + text.size();
}

/**
 * @brief Appends a float to a line, in the exact shortest form.
 *
 * The text formats in this module spell numbers the way the standard library's shortest round trip does: no locale can
 * turn a decimal point into a comma (the conversions here are locale-independent, unlike the C library's), and the text
 * parses back to the same value.
 *
 * @param out The line being built.
 * @param at Offset to append at, or kNoRoom to keep the line over-full.
 * @param value The value to write.
 * @return The offset after the text, or kNoRoom when it does not fit.
 */
inline std::size_t appendFloat(std::span<char> out, std::size_t at, float value) noexcept
{
    if (at == kNoRoom) {
        return kNoRoom;
    }
    const auto result = std::to_chars(out.data() + at, out.data() + out.size(), value);
    return result.ec == std::errc{} ? static_cast<std::size_t>(result.ptr - out.data()) : kNoRoom;
}

/**
 * @brief Appends an integer to a line.
 *
 * @param out The line being built.
 * @param at Offset to append at, or kNoRoom to keep the line over-full.
 * @param value The value to write.
 * @return The offset after the text, or kNoRoom when it does not fit.
 */
inline std::size_t appendInteger(std::span<char> out, std::size_t at, std::uint64_t value) noexcept
{
    if (at == kNoRoom) {
        return kNoRoom;
    }
    const auto result = std::to_chars(out.data() + at, out.data() + out.size(), value);
    return result.ec == std::errc{} ? static_cast<std::size_t>(result.ptr - out.data()) : kNoRoom;
}

} // namespace detail

VN_MESHIO_NS_END
