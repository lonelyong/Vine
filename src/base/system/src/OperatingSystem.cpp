#include <vine/system/OperatingSystem.hpp>

#include <cstring>
#include <string>

#ifdef _WIN32
#    include <windows.h>
#    include <winternl.h>
#else
#    include <sys/utsname.h>
#    include <unistd.h>
#endif

VN_SYSTEM_NS_BEGIN

namespace
{

String fromNumber(unsigned value)
{
    return String::fromLocal8Bit(std::to_string(value).c_str());
}

String joinVersion(unsigned major, unsigned minor, unsigned build)
{
    return fromNumber(major) + u8"." + fromNumber(minor) + u8"." + fromNumber(build);
}

#ifndef _WIN32
String mapMachine(const char* machine)
{
    if (std::strcmp(machine, "x86_64") == 0 || std::strcmp(machine, "amd64") == 0) {
        return u8"x64";
    }
    if (std::strcmp(machine, "aarch64") == 0 || std::strcmp(machine, "arm64") == 0) {
        return u8"arm64";
    }
    if (std::strcmp(machine, "i386") == 0 || std::strcmp(machine, "i686") == 0 || std::strcmp(machine, "x86") == 0) {
        return u8"x86";
    }
    return String::fromLocal8Bit(machine);
}
#endif

OperatingSystemInfo buildInfo()
{
    OperatingSystemInfo info;
#ifdef _WIN32
    info.name = u8"Windows";

    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (fn != nullptr) {
        RTL_OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (fn(&version) == 0) {
            info.version = joinVersion(version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
        }
    }
    if (info.version.empty()) {
        info.version = u8"unknown";
    }

    SYSTEM_INFO system_info{};
    GetNativeSystemInfo(&system_info);
    switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: info.architecture = u8"x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: info.architecture = u8"arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: info.architecture = u8"x86"; break;
    default: info.architecture = u8"unknown"; break;
    }
#elif defined(__APPLE__) || defined(__linux__)
    info.name = u8"macOS";
#    ifdef __linux__
    info.name = u8"Linux";
#    endif
    struct utsname uts{};
    if (uname(&uts) == 0) {
        info.version      = String::fromLocal8Bit(uts.release);
        info.architecture = mapMachine(uts.machine);
    }
#else
    info.name = u8"Unknown";
#endif
    if (info.version.empty()) {
        info.version = u8"unknown";
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
