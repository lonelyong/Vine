#include <vine/vsg/core/PixelProbe.hpp>

#include <algorithm>
#include <utility>

VN_VSG_NS_BEGIN

namespace core
{

bool Rgba8::operator==(const Rgba8& other) const noexcept
{
    return r == other.r && g == other.g && b == other.b && a == other.a;
}

PixelProbe::PixelProbe(int width, int height, std::vector<std::uint8_t> pixels)
  : width_(width)
  , height_(height)
  , pixels_(std::move(pixels))
{
}

bool PixelProbe::valid() const noexcept
{
    if (width_ <= 0 || height_ <= 0)
    {
        return false;
    }
    const auto expected = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 4u;
    return pixels_.size() == expected;
}

int PixelProbe::width() const noexcept
{
    return width_;
}

int PixelProbe::height() const noexcept
{
    return height_;
}

std::size_t PixelProbe::pixelCount() const noexcept
{
    return valid() ? static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) : 0u;
}

Rgba8 PixelProbe::pixel(int x, int y) const noexcept
{
    if (!valid() || x < 0 || y < 0 || x >= width_ || y >= height_)
    {
        return Rgba8{};
    }
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)) * 4u;
    return Rgba8{pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3]};
}

std::size_t PixelProbe::nonBlackPixels() const noexcept
{
    std::size_t count = 0;
    const std::size_t pixels = pixelCount();
    for (std::size_t i = 0; i < pixels; ++i)
    {
        const std::size_t offset = i * 4u;
        if (pixels_[offset] != 0 || pixels_[offset + 1] != 0 || pixels_[offset + 2] != 0)
        {
            ++count;
        }
    }
    return count;
}

double PixelProbe::nonBlackFraction() const noexcept
{
    const std::size_t pixels = pixelCount();
    return pixels == 0 ? 0.0 : static_cast<double>(nonBlackPixels()) / static_cast<double>(pixels);
}

std::size_t PixelProbe::countMatching(const Rgba8& color, const vn::graphics::Viewport& rect) const noexcept
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!clamped(rect, x0, y0, x1, y1))
    {
        return 0;
    }

    std::size_t count = 0;
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (pixel(x, y) == color)
            {
                ++count;
            }
        }
    }
    return count;
}

std::size_t PixelProbe::countDifferingFrom(const Rgba8& color, const vn::graphics::Viewport& rect) const noexcept
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    if (!clamped(rect, x0, y0, x1, y1))
    {
        return 0;
    }

    std::size_t count = 0;
    for (int y = y0; y < y1; ++y)
    {
        for (int x = x0; x < x1; ++x)
        {
            if (!(pixel(x, y) == color))
            {
                ++count;
            }
        }
    }
    return count;
}

bool PixelProbe::wholeImageMatches(const Rgba8& color) const noexcept
{
    const std::size_t pixels = pixelCount();
    for (std::size_t i = 0; i < pixels; ++i)
    {
        const std::size_t offset = i * 4u;
        if (pixels_[offset] != color.r || pixels_[offset + 1] != color.g || pixels_[offset + 2] != color.b ||
            pixels_[offset + 3] != color.a)
        {
            return false;
        }
    }
    return pixels != 0;
}

bool PixelProbe::clamped(const vn::graphics::Viewport& rect, int& x0, int& y0, int& x1, int& y1) const noexcept
{
    if (!valid() || !rect.isValid())
    {
        return false;
    }
    x0 = std::max(0, rect.x);
    y0 = std::max(0, rect.y);
    x1 = std::min(width_, rect.x + rect.width);
    y1 = std::min(height_, rect.y + rect.height);
    return x0 < x1 && y0 < y1;
}

}  // namespace core

const std::vector<std::uint8_t>& core::PixelProbe::pixels() const noexcept
{
    return pixels_;
}

VN_VSG_NS_END
