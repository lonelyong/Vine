#include <vine/system/OperatingSystem.hpp>

#include "SystemPlatform.hpp"

VN_SYSTEM_NS_BEGIN

namespace
{

OperatingSystemInfo buildInfo()
{
    OperatingSystemInfo info;
    detail::fillOsInfo(info);

    // The fields are shown to a user (an "About" panel), so a fact the platform cannot answer reads
    // "unknown" rather than as an empty cell.
    if (info.name.empty()) {
        info.name = u8"Unknown";
    }
    if (info.osVersion.empty()) {
        info.osVersion = u8"unknown";
    }
    if (info.kernelVersion.empty()) {
        info.kernelVersion = u8"unknown";
    }
    if (info.architecture.empty()) {
        info.architecture = u8"unknown";
    }
    return info;
}

} // namespace

const OperatingSystemInfo& OperatingSystem::info()
{
    static const OperatingSystemInfo cached = buildInfo();
    return cached;
}

VN_SYSTEM_NS_END
