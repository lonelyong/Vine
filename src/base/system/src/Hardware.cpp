#include <vine/system/Hardware.hpp>

#include "SystemPlatform.hpp"

VN_SYSTEM_NS_BEGIN

namespace
{

CpuInfo buildCpuInfo()
{
    CpuInfo info;
    detail::fillCpuInfo(info);

    // Whatever the platform could not answer, the CPU that runs this process has at least one core:
    // a count of 0 would be a wrong answer that a caller cannot distinguish from "no information".
    if (info.logicalCores == 0) {
        info.logicalCores = 1;
    }
    if (info.physicalCores == 0) {
        info.physicalCores = info.logicalCores;
    }
    return info;
}

MotherboardInfo buildMotherboardInfo()
{
    MotherboardInfo info;
    detail::fillMotherboardInfo(info);
    return info;
}

} // namespace

const CpuInfo& Hardware::cpu()
{
    // Function-local statics are initialised once under the language's guard variable, so this is
    // the whole of the thread-safety the header promises.
    static const CpuInfo cached = buildCpuInfo();
    return cached;
}

const MotherboardInfo& Hardware::motherboard()
{
    static const MotherboardInfo cached = buildMotherboardInfo();
    return cached;
}

MemoryInfo Hardware::memory()
{
    // Not cached: availableBytes moves with every allocation on the machine, so an answer is a
    // snapshot of this call and not a fact about the process.
    MemoryInfo info;
    detail::fillMemoryInfo(info);
    return info;
}

std::vector<DiskInfo> Hardware::disks()
{
    // Not cached: a volume can be mounted, removed or filled while the process runs, and the header
    // says the call enumerates the current state.
    return detail::enumerateDisks();
}

unsigned long long Hardware::diskTotalSpace(const String& path)
{
    return detail::volumeTotalSpace(path);
}

unsigned long long Hardware::diskFreeSpace(const String& path)
{
    return detail::volumeFreeSpace(path);
}

VN_SYSTEM_NS_END
