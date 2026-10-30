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

#ifndef _WIN32

// No platform hook yet. The report model and its renderer are portable, so only this pair is missing; see the phase table in
// .ai/research/base-crash-2026-10-30-crash-handler-module.md (Linux sigaction is P3).
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

#endif // _WIN32

VN_CRASH_NS_END
