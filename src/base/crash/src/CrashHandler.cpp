#include <vine/crash/CrashHandler.hpp>

#include "CrashDetail.hpp"

VN_CRASH_NS_BEGIN

bool install(const Config& config)
{
    // A process has one last-chance filter: installing twice replaces the configuration instead of stacking two writers.
    detail::uninstallPlatform();
    return detail::installPlatform(config);
}

void uninstall()
{
    detail::uninstallPlatform();
}

#if !defined(_WIN32) && !defined(__linux__)

// No platform hook yet. The report model and its renderer are portable, so only this pair is missing; Windows reports
// through SEH (CrashCaptureWindows.cpp) and Linux through sigaction (CrashCaptureLinux.cpp).
namespace detail {

bool installPlatform(const Config& config)
{
    static_cast<void>(config);
    return false;
}

void uninstallPlatform()
{
}

} // namespace detail

#endif // !_WIN32 && !__linux__

VN_CRASH_NS_END
