#include <vine/vsg/core/DepthProbe.hpp>

#include <cmath>

VN_VSG_NS_BEGIN

namespace core
{

DepthProbe::DepthProbe(int width, int height, std::vector<float> values)
    : width_(width), height_(height), values_(std::move(values))
{
    if (width_ < 0 || height_ < 0 || values_.size() != static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_))
    {
        width_  = 0;
        height_ = 0;
        values_.clear();
    }
}

bool DepthProbe::valid() const noexcept
{
    return width_ > 0 && height_ > 0 && !values_.empty();
}

int DepthProbe::width() const noexcept
{
    return width_;
}

int DepthProbe::height() const noexcept
{
    return height_;
}

float DepthProbe::depthAt(int x, int y) const noexcept
{
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
    {
        return 0.0F;
    }
    return values_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

std::size_t DepthProbe::countNear(float value, float tolerance) const noexcept
{
    std::size_t count = 0;
    for (const float stored : values_)
    {
        if (std::fabs(stored - value) <= tolerance)
        {
            ++count;
        }
    }
    return count;
}

}  // namespace core

const std::vector<float>& core::DepthProbe::values() const noexcept
{
    return values_;
}

VN_VSG_NS_END
