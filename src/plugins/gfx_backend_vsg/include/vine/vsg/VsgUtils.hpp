#pragma once

#include <vsg/maths/mat4.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include <vine/String.hpp>
#include <vine/math/Matrix4x4.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief Converts a Vine Mat4d to a vsg::dmat4.
 *
 * Both are column-major; the 16-element array is column 0..3.
 *
 * @param m Vine matrix.
 * @return Equivalent vsg matrix.
 */
inline ::vsg::dmat4 toVsg(const vine::math::Mat4d& m)
{
    double v[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            v[col * 4 + row] = m(row, col);
        }
    }
    return ::vsg::dmat4(v);
}

}  // namespace detail

/**
 * @brief Formats an ASCII diagnostic message.
 *
 * Backend diagnostics are built from literals plus the failing data's numbers,
 * so a byte-wise local 8-bit copy is exact; the result is truncated at 512
 * bytes. Having one formatter means a rejection can carry the numbers that
 * caused it (which component count, how many floats, how many vertices)
 * instead of a bare sentence, wherever it is reported from.
 *
 * @param format printf-style ASCII format string.
 * @param ...    Arguments matching @p format.
 * @return The formatted message (empty when formatting failed).
 */
inline vine::String formatDiagnostic(const char8_t* format, ...)
{
    if (format == nullptr) {
        return vine::String();
    }
    // Diagnostics are ASCII (literals + the failing data's numbers), so the
    // byte-wise local 8-bit copy below is exact for them. The message is
    // truncated at 512 bytes rather than allocated to fit: a diagnostic must
    // never itself become a failure mode.
    char    buffer[512];
    va_list args;
    va_start(args, format);
    const int written =
        std::vsnprintf(buffer, sizeof(buffer), reinterpret_cast<const char*>(format), args);
    va_end(args);
    if (written <= 0) {
        return vine::String();
    }
    const std::size_t length =
        std::min<std::size_t>(static_cast<std::size_t>(written), sizeof(buffer) - 1u);
    return vine::String::fromLocal8Bit(buffer, length);
}

V_VSG_NS_END
