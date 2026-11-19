#include "SystemPlatform.hpp"

#include <thread>
#include <vector>

VN_SYSTEM_NS_BEGIN

namespace detail
{

// Every platform the module does not implement lands here, so the public API still has a defined
// answer for each query instead of a function whose only return statement was compiled out. These
// answers are the ones Hardware.hpp / OperatingSystem.hpp / Process.hpp document as "empty" or
// "unknown"; nothing here guesses.

void fillOsInfo(OperatingSystemInfo& info)
{
    info.name = u8"Unknown";
}

void fillCpuInfo(CpuInfo& info)
{
    // Core counts are portable, so they are still answered.
    info.logicalCores = static_cast<unsigned>(std::thread::hardware_concurrency());
}

void fillMotherboardInfo(MotherboardInfo&)
{}

std::vector<DiskInfo> enumerateDisks()
{
    return {};
}

unsigned long long volumeTotalSpace(const String&)
{
    return 0;
}

unsigned long long volumeFreeSpace(const String&)
{
    return 0;
}

int currentPid()
{
    return 0;
}

String executablePath()
{
    return {};
}

bool pidExists(int)
{
    return false;
}

std::vector<int> pidsByName(const String&)
{
    return {};
}

bool terminatePid(int)
{
    return false;
}

} // namespace detail

VN_SYSTEM_NS_END
