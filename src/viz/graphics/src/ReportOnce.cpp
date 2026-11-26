#include <vine/graphics/ReportOnce.hpp>

VN_GRAPHICS_NS_BEGIN

ReportOnce::ReportOnce(const ReportOnce& other) noexcept
  : reported_(other.reported_.load(std::memory_order_acquire))
{}

ReportOnce& ReportOnce::operator=(const ReportOnce& other) noexcept
{
    // A SELF-COPY IS NOT AN OFF-BY-ONE: load first either way, so `x = x` leaves the episode as it was.
    reported_.store(other.reported_.load(std::memory_order_acquire), std::memory_order_release);
    return *this;
}

bool ReportOnce::shouldReport() noexcept
{
    // THE TEST AND THE SET ARE ONE OPERATION, so the thread that wins the exchange is the one that reports - not "the
    // first to look" and not "both of them" (see the class note).
    return !reported_.exchange(true, std::memory_order_acq_rel);
}

bool ReportOnce::reported() const noexcept
{
    return reported_.load(std::memory_order_acquire);
}

void ReportOnce::rearm() noexcept
{
    reported_.store(false, std::memory_order_release);
}

VN_GRAPHICS_NS_END
