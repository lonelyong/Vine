#include <vine/imaging/Image.hpp>

#include <stdexcept>

V_IMAGING_NS_BEGIN

namespace
{

/**
 * @brief Clamps a mip index into the levels an image actually has.
 *
 * @param mip       Requested mip level.
 * @param mip_count Number of levels the image has.
 * @return A level index inside `[0, mip_count - 1]`.
 */
std::size_t clampedMip(int mip, int mip_count) noexcept
{
    if (mip <= 0) {
        return 0;
    }

    const int last = mip_count - 1;
    return (mip >= last) ? static_cast<std::size_t>(last) : static_cast<std::size_t>(mip);
}

/**
 * @brief Gets the byte size of one mip level.
 *
 * Rows are packed with no padding, so a level is simply its pixel count times the pixel size.
 *
 * @param width  Width of the level in pixels.
 * @param height Height of the level in pixels.
 * @param format Byte layout of one pixel.
 * @return The level's size in bytes.
 */
std::size_t levelByteSize(int width, int height, PixelFormat format) noexcept
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytesPerPixel(format);
}

} // namespace

V_OBJECT_META_IMPL(Image, vine::Object);

int Image::mipCapacity(int width, int height) noexcept
{
    if (width <= 0 || height <= 0) {
        return 0;
    }

    int levels = 1;
    int extent = (width > height) ? width : height;
    while (extent > 1) {
        extent >>= 1;
        ++levels;
    }

    return levels;
}

Image::Image(int width, int height, PixelFormat format, int mip_count)
  : width_(width)
  , height_(height)
  , format_(format)
  , mip_count_(mip_count)
{
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("Image: width and height must be positive");
    }

    if (bytesPerPixel(format) == 0) {
        throw std::invalid_argument("Image: not a pixel layout (PixelFormat::Unknown or out of range)");
    }

    if (mip_count < 1 || mip_count > mipCapacity(width, height)) {
        throw std::invalid_argument("Image: mip count must lie in [1, mipCapacity(width, height)]");
    }

    mip_offsets_.reserve(static_cast<std::size_t>(mip_count) + 1);

    std::size_t offset = 0;
    for (int mip = 0; mip < mip_count_; ++mip) {
        mip_offsets_.push_back(offset);
        offset += levelByteSize(mipWidth(mip), mipHeight(mip), format_);
    }

    // Sentinel: end of the last level, so a level's size is a difference of two offsets.
    mip_offsets_.push_back(offset);

    // Value-initialised, so every pixel starts as zero bytes.
    pixels_.resize(offset);
}

int Image::width() const noexcept
{
    return width_;
}

int Image::height() const noexcept
{
    return height_;
}

PixelFormat Image::format() const noexcept
{
    return format_;
}

int Image::mipCount() const noexcept
{
    return mip_count_;
}

int Image::mipWidth(int mip) const noexcept
{
    const int level  = static_cast<int>(clampedMip(mip, mip_count_));
    const int extent = width_ >> level;
    return (extent > 0) ? extent : 1;
}

int Image::mipHeight(int mip) const noexcept
{
    const int level  = static_cast<int>(clampedMip(mip, mip_count_));
    const int extent = height_ >> level;
    return (extent > 0) ? extent : 1;
}

std::size_t Image::mipByteSize(int mip) const noexcept
{
    const std::size_t level = clampedMip(mip, mip_count_);
    return mip_offsets_[level + 1] - mip_offsets_[level];
}

std::span<std::byte> Image::mipData(int mip) noexcept
{
    const std::size_t level = clampedMip(mip, mip_count_);
    const std::size_t begin = mip_offsets_[level];
    const std::size_t end   = mip_offsets_[level + 1];
    return std::span<std::byte>(pixels_.data() + begin, end - begin);
}

std::span<const std::byte> Image::mipData(int mip) const noexcept
{
    const std::size_t level = clampedMip(mip, mip_count_);
    const std::size_t begin = mip_offsets_[level];
    const std::size_t end   = mip_offsets_[level + 1];
    return std::span<const std::byte>(pixels_.data() + begin, end - begin);
}

std::size_t Image::totalByteSize() const noexcept
{
    return mip_offsets_.back();
}

V_IMAGING_NS_END
