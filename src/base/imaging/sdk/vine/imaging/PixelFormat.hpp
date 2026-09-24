#pragma once

#include "imaging_global.hpp"

#include <cstddef>
#include <cstdint>

VN_IMAGING_NS_BEGIN

/**
 * @brief How the colour or depth channels of ONE pixel are laid out in memory.
 *
 * The format carries the BYTE layout and nothing else: it says which bytes are which channel, not what the
 * image is for.
 * The same pixels can be uploaded as a sampled texture, copied into a render target, or read back out of
 * one, and none of those uses changes the layout.
 *
 * The questions a consumer actually has to answer — how many channels, how many bytes per pixel, is this
 * depth, is this sRGB — are answered by the free functions below instead of by the caller switching on the
 * enumerators.
 * Adding a format then means teaching one place, not every call site.
 *
 * `Unknown` is the only value that is not a format.
 * It exists so that a default-constructed or out-of-range format is detectably wrong instead of silently
 * aliasing the first real format.
 */
enum class PixelFormat : std::uint8_t {
    Unknown = 0, ///< Not a format: the value a default-constructed PixelFormat has.
    R8Unorm,
    R8Srgb,
    Rg8Unorm,
    Rgb8Unorm,
    Rgb8Srgb,
    Rgba8Unorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,
    R16Float,
    Rg16Float,
    Rgba16Float,
    R32Float,
    Rg32Float,
    Rgba32Float,
    D16Unorm,
    D24UnormS8Uint,
    D32Float,
};

/**
 * @brief Gets how many channels one pixel of a format stores.
 *
 * A packed depth+stencil format counts both channels, so `D24UnormS8Uint` reports 2.
 *
 * @param format Format to query.
 * @return The channel count, or 0 for `PixelFormat::Unknown` and any out-of-range value.
 */
VN_IMAGING_API int channelCount(PixelFormat format) noexcept;

/**
 * @brief Gets the size of one pixel of a format, in bytes.
 *
 * A packed depth+stencil format reports the size of the whole packed texel, so `D24UnormS8Uint` reports 4.
 *
 * @param format Format to query.
 * @return The pixel size in bytes, or 0 for `PixelFormat::Unknown` and any out-of-range value.
 */
VN_IMAGING_API std::size_t bytesPerPixel(PixelFormat format) noexcept;

/**
 * @brief States whether a format stores depth instead of colour.
 *
 * A depth image may not be sampled as colour, so a consumer that only accepts colour has to ask.
 *
 * @param format Format to query.
 * @return true for the `D*` formats, false otherwise.
 */
VN_IMAGING_API bool isDepthFormat(PixelFormat format) noexcept;

/**
 * @brief States whether a format's colour channels are sRGB-encoded.
 *
 * An sRGB format is copied byte for byte like any other; the encoding only matters to whoever interprets
 * the values, which is why it is a property of the format rather than a separate flag.
 *
 * @param format Format to query.
 * @return true for the `*Srgb` formats, false otherwise.
 */
VN_IMAGING_API bool isSrgbFormat(PixelFormat format) noexcept;

/**
 * @brief Gets a stable, human-readable name for a format, for diagnostics.
 *
 * The returned string is a static literal: it is never null, never allocated, and never changes for the
 * lifetime of the process, so a caller may keep the pointer.
 *
 * @param format Format to name.
 * @return The format's name, or `"Unknown"` for `PixelFormat::Unknown` and any out-of-range value.
 */
VN_IMAGING_API const char* formatName(PixelFormat format) noexcept;

VN_IMAGING_NS_END
