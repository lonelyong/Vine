#include <vine/imaging/PixelFormat.hpp>

VN_IMAGING_NS_BEGIN

int channelCount(PixelFormat format) noexcept
{
    switch (format) {
        case PixelFormat::Unknown:
            return 0;

        case PixelFormat::R8Unorm:
        case PixelFormat::R8Srgb:
        case PixelFormat::R16Float:
        case PixelFormat::R32Float:
        case PixelFormat::D16Unorm:
        case PixelFormat::D32Float:
            return 1;

        case PixelFormat::Rg8Unorm:
        case PixelFormat::Rg16Float:
        case PixelFormat::Rg32Float:
        case PixelFormat::D24UnormS8Uint:
            return 2;

        case PixelFormat::Rgb8Unorm:
        case PixelFormat::Rgb8Srgb:
            return 3;

        case PixelFormat::Rgba8Unorm:
        case PixelFormat::Rgba8Srgb:
        case PixelFormat::Bgra8Unorm:
        case PixelFormat::Bgra8Srgb:
        case PixelFormat::Rgba16Float:
        case PixelFormat::Rgba32Float:
            return 4;
    }

    return 0; // Unreachable for every enumerator; defends against an out-of-range cast.
}

std::size_t bytesPerPixel(PixelFormat format) noexcept
{
    switch (format) {
        case PixelFormat::Unknown:
            return 0;

        case PixelFormat::R8Unorm:
        case PixelFormat::R8Srgb:
            return 1;

        case PixelFormat::Rg8Unorm:
        case PixelFormat::R16Float:
        case PixelFormat::D16Unorm:
            return 2;

        case PixelFormat::Rgb8Unorm:
        case PixelFormat::Rgb8Srgb:
            return 3;

        case PixelFormat::Rgba8Unorm:
        case PixelFormat::Rgba8Srgb:
        case PixelFormat::Bgra8Unorm:
        case PixelFormat::Bgra8Srgb:
        case PixelFormat::Rg16Float:
        case PixelFormat::R32Float:
        case PixelFormat::D24UnormS8Uint:
        case PixelFormat::D32Float:
            return 4;

        case PixelFormat::Rgba16Float:
        case PixelFormat::Rg32Float:
            return 8;

        case PixelFormat::Rgba32Float:
            return 16;
    }

    return 0; // Unreachable for every enumerator; defends against an out-of-range cast.
}

bool isDepthFormat(PixelFormat format) noexcept
{
    switch (format) {
        case PixelFormat::D16Unorm:
        case PixelFormat::D24UnormS8Uint:
        case PixelFormat::D32Float:
            return true;

        case PixelFormat::Unknown:
        case PixelFormat::R8Unorm:
        case PixelFormat::R8Srgb:
        case PixelFormat::Rg8Unorm:
        case PixelFormat::Rgb8Unorm:
        case PixelFormat::Rgb8Srgb:
        case PixelFormat::Rgba8Unorm:
        case PixelFormat::Rgba8Srgb:
        case PixelFormat::Bgra8Unorm:
        case PixelFormat::Bgra8Srgb:
        case PixelFormat::R16Float:
        case PixelFormat::Rg16Float:
        case PixelFormat::Rgba16Float:
        case PixelFormat::R32Float:
        case PixelFormat::Rg32Float:
        case PixelFormat::Rgba32Float:
            return false;
    }

    return false; // Unreachable for every enumerator; defends against an out-of-range cast.
}

bool isSrgbFormat(PixelFormat format) noexcept
{
    switch (format) {
        case PixelFormat::R8Srgb:
        case PixelFormat::Rgb8Srgb:
        case PixelFormat::Rgba8Srgb:
        case PixelFormat::Bgra8Srgb:
            return true;

        case PixelFormat::Unknown:
        case PixelFormat::R8Unorm:
        case PixelFormat::Rg8Unorm:
        case PixelFormat::Rgb8Unorm:
        case PixelFormat::Rgba8Unorm:
        case PixelFormat::Bgra8Unorm:
        case PixelFormat::R16Float:
        case PixelFormat::Rg16Float:
        case PixelFormat::Rgba16Float:
        case PixelFormat::R32Float:
        case PixelFormat::Rg32Float:
        case PixelFormat::Rgba32Float:
        case PixelFormat::D16Unorm:
        case PixelFormat::D24UnormS8Uint:
        case PixelFormat::D32Float:
            return false;
    }

    return false; // Unreachable for every enumerator; defends against an out-of-range cast.
}

const char* formatName(PixelFormat format) noexcept
{
    switch (format) {
        case PixelFormat::R8Unorm:
            return "R8Unorm";
        case PixelFormat::R8Srgb:
            return "R8Srgb";
        case PixelFormat::Rg8Unorm:
            return "Rg8Unorm";
        case PixelFormat::Rgb8Unorm:
            return "Rgb8Unorm";
        case PixelFormat::Rgb8Srgb:
            return "Rgb8Srgb";
        case PixelFormat::Rgba8Unorm:
            return "Rgba8Unorm";
        case PixelFormat::Rgba8Srgb:
            return "Rgba8Srgb";
        case PixelFormat::Bgra8Unorm:
            return "Bgra8Unorm";
        case PixelFormat::Bgra8Srgb:
            return "Bgra8Srgb";
        case PixelFormat::R16Float:
            return "R16Float";
        case PixelFormat::Rg16Float:
            return "Rg16Float";
        case PixelFormat::Rgba16Float:
            return "Rgba16Float";
        case PixelFormat::R32Float:
            return "R32Float";
        case PixelFormat::Rg32Float:
            return "Rg32Float";
        case PixelFormat::Rgba32Float:
            return "Rgba32Float";
        case PixelFormat::D16Unorm:
            return "D16Unorm";
        case PixelFormat::D24UnormS8Uint:
            return "D24UnormS8Uint";
        case PixelFormat::D32Float:
            return "D32Float";

        case PixelFormat::Unknown:
            break;
    }

    return "Unknown"; // Also the answer for an out-of-range cast.
}

VN_IMAGING_NS_END
