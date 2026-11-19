#include "SystemPlatform.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

VN_SYSTEM_NS_BEGIN

namespace detail
{

namespace
{

String fromLocal(const char* text)
{
    return String::fromLocal8Bit(text);
}

/// Reads one `sysctl` value into @p out; returns false when the key is absent on this machine.
bool sysctlValue(const char* key, void* out, std::size_t size)
{
    std::size_t length = size;
    return sysctlbyname(key, out, &length, nullptr, 0) == 0;
}

} // namespace

void fillOsInfo(OperatingSystemInfo& info)
{
    info.name = u8"macOS";

    struct utsname uts{};
    if (uname(&uts) == 0) {
        // The Darwin kernel release, which is the version this platform reports; OperatingSystem.hpp
        // states that spelling per platform.
        info.version      = fromLocal(uts.release);
        info.architecture = architectureName(uts.machine);
    }
}

void fillCpuInfo(CpuInfo& info)
{
    int logical = 0;
    if (sysctlValue("hw.logicalcpu", &logical, sizeof(logical)) && logical > 0) {
        info.logicalCores = static_cast<unsigned>(logical);
    }
    int physical = 0;
    if (sysctlValue("hw.physicalcpu", &physical, sizeof(physical)) && physical > 0) {
        info.physicalCores = static_cast<unsigned>(physical);
    }

    char buffer[256] = {};
    if (sysctlValue("machdep.cpu.brand_string", buffer, sizeof(buffer))) {
        info.model = fromLocal(buffer);
    }
    char vendor[64] = {};
    if (sysctlValue("machdep.cpu.vendor", vendor, sizeof(vendor))) {
        info.vendor = fromLocal(vendor);
    }
}

void fillMotherboardInfo(MotherboardInfo& info)
{
    info.manufacturer = u8"Apple";

    char model[128] = {};
    if (sysctlValue("hw.model", model, sizeof(model))) {
        info.product = fromLocal(model);
    }
}

std::vector<DiskInfo> enumerateDisks()
{
    // macOS exposes no sysfs equivalent: listing volumes means IOKit, which this module does not
    // link. Hardware.hpp states the empty answer, so a caller can tell "no implementation" from
    // "an empty machine".
    return {};
}

unsigned long long volumeTotalSpace(const String& path)
{
    struct statvfs vfs{};
    if (statvfs(reinterpret_cast<const char*>(path.c_str()), &vfs) != 0 || vfs.f_frsize == 0) {
        return 0;
    }
    return static_cast<unsigned long long>(vfs.f_blocks) * vfs.f_frsize;
}

unsigned long long volumeFreeSpace(const String& path)
{
    struct statvfs vfs{};
    if (statvfs(reinterpret_cast<const char*>(path.c_str()), &vfs) != 0 || vfs.f_frsize == 0) {
        return 0;
    }
    return static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
}

int currentPid()
{
    return static_cast<int>(getpid());
}

String executablePath()
{
    // _NSGetExecutablePath answers the required size when the buffer is too small, so the call is
    // made twice instead of guessing a maximum path length.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size > 1 ? size : 4096, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    return fromLocal(buffer.data());
}

bool pidExists(int pid)
{
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM; // Exists, but belongs to another user.
}

std::vector<int> pidsByName(const String&)
{
    // KERN_PROC enumeration is not implemented; Process.hpp states the empty answer.
    return {};
}

bool terminatePid(int pid)
{
    return ::kill(pid, SIGKILL) == 0;
}

} // namespace detail

VN_SYSTEM_NS_END
