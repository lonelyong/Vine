#include "SystemPlatform.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/vm_statistics.h>
#include <pwd.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/types.h>
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
        info.logicalCores = static_cast<std::uint32_t>(logical);
    }
    int physical = 0;
    if (sysctlValue("hw.physicalcpu", &physical, sizeof(physical)) && physical > 0) {
        info.physicalCores = static_cast<std::uint32_t>(physical);
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

std::uint64_t volumeTotalSpace(const String& path)
{
    struct statvfs vfs{};
    if (statvfs(reinterpret_cast<const char*>(path.c_str()), &vfs) != 0 || vfs.f_frsize == 0) {
        return 0;
    }
    return static_cast<unsigned long long>(vfs.f_blocks) * vfs.f_frsize;
}

std::uint64_t volumeFreeSpace(const String& path)
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

bool environmentVariable(const String& name, String& value)
{
    if (name.empty()) {
        return false;
    }
    const char* found = std::getenv(reinterpret_cast<const char*>(name.c_str()));
    if (found == nullptr) {
        return false;
    }
    value = fromLocal(found);
    return true;
}

std::vector<std::pair<String, String>> environmentVariables()
{
    std::vector<std::pair<String, String>> result;

    // _NSGetEnviron(), not ::environ: a dylib's view of the global array is not guaranteed.
    char** entries = *_NSGetEnviron();
    for (char** entry = entries; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        result.emplace_back(fromLocal(text.substr(0, equals).c_str()), fromLocal(text.substr(equals + 1).c_str()));
    }
    return result;
}

namespace
{

String absoluteVariable(const char* name)
{
    const char* value = std::getenv(name);
    if (value == nullptr || *value != '/') {
        return {};
    }
    return fromLocal(value);
}

String below(const String& root, const char8_t* leaf)
{
    return root.empty() ? String() : root + leaf;
}

} // namespace

String temporaryDirectory()
{
    // macOS gives every user a private temporary directory; /tmp is only the fallback.
    const String from_variable = absoluteVariable("TMPDIR");
    return from_variable.empty() ? String(u8"/tmp") : from_variable;
}

String homeDirectory()
{
    const char* value = std::getenv("HOME");
    return value != nullptr ? fromLocal(value) : String();
}

String dataDirectory()
{
    return below(homeDirectory(), u8"/Library/Application Support");
}

String configDirectory()
{
    return below(homeDirectory(), u8"/Library/Preferences");
}

String cacheDirectory()
{
    return below(homeDirectory(), u8"/Library/Caches");
}

void fillMemoryInfo(MemoryInfo& info)
{
    std::uint64_t total = 0;
    if (sysctlValue("hw.memsize", &total, sizeof(total))) {
        info.totalBytes = total;
    }

    const mach_port_t host = mach_host_self();

    vm_size_t page_size = 0;
    if (host_page_size(host, &page_size) == KERN_SUCCESS) {
        info.pageSize = static_cast<std::uint32_t>(page_size);
    }

    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS && page_size != 0) {
        // Free + inactive + speculative is what a new allocation can claim without paging.
        info.availableBytes = (static_cast<std::uint64_t>(statistics.free_count) + statistics.inactive_count + statistics.speculative_count) * page_size;
    }

    mach_port_deallocate(mach_task_self(), host);
}

String hostName()
{
    char name[256] = {};
    if (::gethostname(name, sizeof(name)) != 0) {
        return {};
    }
    name[sizeof(name) - 1] = '\0'; // The call does not terminate a name it had to truncate.
    return fromLocal(name);
}

String userName()
{
    // getpwuid_r, not getpwuid: the latter answers from a shared static buffer.
    long buffer_size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buffer_size <= 0) {
        buffer_size = 16384;
    }
    std::vector<char> storage(static_cast<std::size_t>(buffer_size));
    struct passwd     entry{};
    struct passwd*    found = nullptr;
    if (::getpwuid_r(::getuid(), &entry, storage.data(), storage.size(), &found) != 0 || found == nullptr) {
        return {};
    }
    return fromLocal(found->pw_name);
}

} // namespace detail

VN_SYSTEM_NS_END
