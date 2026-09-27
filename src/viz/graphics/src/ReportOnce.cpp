#include <vine/graphics/ReportOnce.hpp>

VN_GRAPHICS_NS_BEGIN

bool ReportOnce::shouldReport() noexcept
{
    if (reported_)
    {
        return false;
    }
    reported_ = true;
    return true;
}

bool ReportOnce::reported() const noexcept
{
    return reported_;
}

void ReportOnce::rearm() noexcept
{
    reported_ = false;
}

VN_GRAPHICS_NS_END
