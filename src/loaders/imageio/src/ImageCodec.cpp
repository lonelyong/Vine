#include <vine/imageio/ImageCodec.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

#include "StbConfig.hpp"

#include <stb_image.h>
#include <stb_image_write.h>

VN_IMAGEIO_NS_BEGIN

namespace
{

using imaging::PixelFormat;

/**
 * @brief Gets how many 8-bit channels a pixel layout maps to, or 0 when a container cannot hold it.
 *
 * One function answers this for BOTH directions on purpose: what a decoder can produce and what an encoder
 * can consume is the same set of layouts, so a layout cannot become readable but unwritable (or the
 * reverse) by accident.
 *
 * WHY TWO CHANNELS IS ABSENT. A file's two channels mean GREY + ALPHA, not red + green (stb's own comment
 * on the two-channel path is "2 pixels = mono + alpha"), and this SDK has no format that names grey +
 * alpha — `Rg8Unorm` means red + green, so returning it would be a lie. The mapping is refused instead.
 *
 * @param format Layout to map.
 * @return 1, 3 or 4 for the layouts a container has a form for, 0 for everything else.
 */
int channelsForContainer(PixelFormat format) noexcept
{
    switch (format) {
        case PixelFormat::R8Unorm:
        case PixelFormat::R8Srgb:
            return 1;

        case PixelFormat::Rgb8Unorm:
        case PixelFormat::Rgb8Srgb:
            return 3;

        case PixelFormat::Rgba8Unorm:
        case PixelFormat::Rgba8Srgb:
        case PixelFormat::Bgra8Unorm:
        case PixelFormat::Bgra8Srgb:
            return 4;

        // Two channels: a container's two channels are grey + alpha, which `Rg8Unorm` (red + green) does
        // not describe. Refusing keeps the caller from being handed luma in the channel it called red.
        case PixelFormat::Rg8Unorm:

        // Float layouts store a channel in 2, 4 or 8 bytes and depth has no meaning outside a render
        // target; a container can hold neither.
        case PixelFormat::Unknown:
        case PixelFormat::R16Float:
        case PixelFormat::Rg16Float:
        case PixelFormat::Rgba16Float:
        case PixelFormat::R32Float:
        case PixelFormat::Rg32Float:
        case PixelFormat::Rgba32Float:
        case PixelFormat::D16Unorm:
        case PixelFormat::D24UnormS8Uint:
        case PixelFormat::D32Float:
            return 0;
    }

    return 0; // Unreachable for every enumerator; defends against an out-of-range cast.
}

/**
 * @brief States whether a layout stores its colour channels in B,G,R,A order.
 *
 * @param format Layout to test.
 * @return true for the BGRA layouts.
 */
bool isBgraLayout(PixelFormat format) noexcept
{
    return format == PixelFormat::Bgra8Unorm || format == PixelFormat::Bgra8Srgb;
}

/**
 * @brief Swaps the red and blue bytes of every RGBA pixel in place.
 *
 * stb always produces and consumes R,G,B,A order whatever the container stored, so a BGRA layout is served
 * by decoding normally and swapping — rather than by asking stb for something it cannot give.
 *
 * A trailing partial pixel (which a well-formed image never has) is left alone.
 *
 * @param pixels Pixel bytes to swizzle.
 */
void swizzleRedBlue(std::span<std::byte> pixels) noexcept
{
    for (std::size_t i = 0; i + 4 <= pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]);
    }
}

/**
 * @brief Appends whatever the encoder hands over to a byte vector.
 *
 * Matches stb's `stbi_write_func`, which is how the encoders write into memory instead of to a path.
 *
 * @param context The `std::vector<std::byte>` receiving the bytes.
 * @param data    Bytes the encoder produced.
 * @param size    How many bytes.
 */
void appendEncodedBytes(void* context, void* data, int size)
{
    if (context == nullptr || data == nullptr || size <= 0) {
        return;
    }

    auto* out = static_cast<std::vector<std::byte>*>(context);
    const auto* first = static_cast<const std::byte*>(data);
    out->insert(out->end(), first, first + size);
}

} // namespace

vn::io::Result<intrusive_ptr<imaging::Image>> decodeImage(std::span<const std::byte> bytes, PixelFormat format)
{
    const int channels = channelsForContainer(format);
    if (channels == 0) {
        // Not a broken file: this module simply has no way to store the layout, so the caller asked for
        // something no container has a form for.
        return vn::io::IoError::Unsupported;
    }
    if (bytes.empty()) {
        return vn::io::IoError::InvalidData; // an empty buffer holds no image
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        // stb takes the length as an int; refusing here keeps a huge buffer from wrapping into a
        // silently truncated decode.
        return vn::io::IoError::CapacityExceeded;
    }

    int width           = 0;
    int height          = 0;
    int source_channels = 0;
    stbi_uc* const decoded = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                                   static_cast<int>(bytes.size()), &width, &height, &source_channels, channels);

    if (decoded == nullptr) {
        return vn::io::IoError::InvalidData; // the bytes are not an image this build can decode
    }

    // stb hands over a heap buffer; free it on every path out of here.
    const auto free_decoded = [](stbi_uc* p) noexcept {
        if (p != nullptr) {
            stbi_image_free(p);
        }
    };
    const std::unique_ptr<stbi_uc, decltype(free_decoded)> owned(decoded, free_decoded);

    auto image = intrusive_ptr<imaging::Image>(new imaging::Image(width, height, format));

    std::span<std::byte> destination = image->mipData(0);
    std::memcpy(destination.data(), decoded, destination.size());

    // The request was for B,G,R,A but stb produced R,G,B,A.
    if (isBgraLayout(format)) {
        swizzleRedBlue(destination);
    }

    return image;
}

vn::io::Result<intrusive_ptr<imaging::Image>> loadImage(const std::filesystem::path& path, PixelFormat format)
{
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec) || ec) {
        return vn::io::IoError::NotFound; // there is no file to decode
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return vn::io::IoError::IoFailure; // the file is there but this process may not read it
    }

    const std::streamoff size = input.tellg();
    if (size <= 0) {
        return vn::io::IoError::InvalidData; // an empty file holds no image
    }

    input.seekg(0);
    if (!input) {
        return vn::io::IoError::IoFailure;
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        return vn::io::IoError::IoFailure; // a short read is not content to decode
    }

    return decodeImage(bytes, format);
}

vn::io::Result<std::vector<std::byte>> encodeImage(const imaging::Image& image, ImageFileFormat format)
{
    if (!canWrite(format)) {
        return vn::io::IoError::Unsupported;
    }

    const int channels = channelsForContainer(image.format());
    if (channels == 0) {
        return vn::io::IoError::Unsupported; // this module has no way to store the layout
    }

    const std::span<const std::byte> base = image.mipData(0);
    if (base.empty()) {
        return vn::io::IoError::InvalidData; // an image with no pixels states nothing
    }

    // The encoders read RGBA order, so a BGRA image is converted into a scratch copy rather than having
    // the caller's pixels mutated underneath them.
    std::vector<std::byte> scratch;
    const std::byte*       pixels = base.data();
    if (isBgraLayout(image.format())) {
        scratch.assign(base.begin(), base.end());
        swizzleRedBlue(scratch);
        pixels = scratch.data();
    }

    const int width  = image.width();
    const int height = image.height();

    std::vector<std::byte> encoded;
    int                    written = 0;

    switch (format) {
        case ImageFileFormat::Png:
            // PNG is the only one of the three that takes a row stride; rows here are packed, so it is
            // exactly one row.
            written = stbi_write_png_to_func(appendEncodedBytes, &encoded, width, height, channels, pixels, width * channels);
            break;

        case ImageFileFormat::Bmp:
            written = stbi_write_bmp_to_func(appendEncodedBytes, &encoded, width, height, channels, pixels);
            break;

        case ImageFileFormat::Tga:
            written = stbi_write_tga_to_func(appendEncodedBytes, &encoded, width, height, channels, pixels);
            break;

        case ImageFileFormat::Jpeg:
        case ImageFileFormat::Unknown:
            // Unreachable: canWrite() refused them above.
            break;
    }

    if (written == 0 || encoded.empty()) {
        return vn::io::IoError::IoFailure; // the encoder ran but produced nothing
    }

    return encoded;
}

vn::io::IoError saveImage(const std::filesystem::path& path, const imaging::Image& image, ImageFileFormat format)
{
    const vn::io::Result<std::vector<std::byte>> encoded = encodeImage(image, format);
    if (!encoded.ok()) {
        return encoded.error();
    }

    const std::vector<std::byte>& bytes = encoded.value();

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return vn::io::IoError::IoFailure;
    }

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output ? vn::io::IoError::Ok : vn::io::IoError::IoFailure;
}

VN_IMAGEIO_NS_END
