#include "SystemPlatform.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <pwd.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

// Declared by POSIX; written here rather than relied on from a header, because <unistd.h> only
// exposes it when the feature-test macros happen to allow it.
extern char** environ;

VN_SYSTEM_NS_BEGIN

namespace detail
{

namespace
{

String fromLocal(const std::string& text)
{
    return String::fromLocal8Bit(text.c_str());
}

std::string trimmed(std::string text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

/// Reads one line of a sysfs/procfs file, trimmed: the kernel pads fixed-width fields with spaces
/// ("Virtual Disk    " is the real spelling of a sysfs disk model), and a caller should not have to
/// know that.
std::string readLine(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    return trimmed(line);
}

/// True for a whole block device `/sys/block` reports that is not a virtual device and not a volume
/// a mapper assembled out of other devices.
bool isStorageDevice(const std::string& name)
{
    static constexpr const char* kVirtualPrefixes[] = { "loop", "ram", "zram", "fd" };
    for (const char* prefix : kVirtualPrefixes) {
        if (name.rfind(prefix, 0) == 0) {
            return false;
        }
    }
    return name.rfind("dm-", 0) != 0;
}

bool isNumeric(const std::string& text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

void fillOsInfo(OperatingSystemInfo& info)
{
    info.name = u8"Linux";

    struct utsname uts{};
    if (uname(&uts) == 0) {
        // The kernel release ("uname -r"): Linux publishes no distribution version in a portable
        // way, and Hardware/OperatingSystem.hpp states which spelling each platform reports.
        info.version      = fromLocal(uts.release);
        info.architecture = architectureName(uts.machine);
    }
}

void fillCpuInfo(CpuInfo& info)
{
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        info.logicalCores = static_cast<std::uint32_t>(online);
    }

    // Physical cores are the distinct (physical id, core id) pairs: /proc/cpuinfo repeats one block
    // per logical CPU, so counting the blocks would report the thread count.
    std::ifstream                                    cpuinfo("/proc/cpuinfo");
    std::string                                      line;
    std::string                                      physical;
    std::vector<std::pair<std::string, std::string>> core_ids;
    while (std::getline(cpuinfo, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key   = trimmed(line.substr(0, colon));
        const std::string value = trimmed(line.substr(colon + 1));
        if (key == "vendor_id" && info.vendor.empty()) {
            info.vendor = fromLocal(value);
        }
        else if (key == "model name" && info.model.empty()) {
            info.model = fromLocal(value);
        }
        else if (key == "physical id") {
            physical = value;
        }
        else if (key == "core id") {
            core_ids.emplace_back(physical, value);
        }
    }

    std::sort(core_ids.begin(), core_ids.end());
    core_ids.erase(std::unique(core_ids.begin(), core_ids.end()), core_ids.end());
    // Kernels that publish no core id (many ARM and some virtualised x86 ones) leave the list empty;
    // the shared caller then reports the logical count rather than a wrong, smaller number.
    if (!core_ids.empty()) {
        info.physicalCores = static_cast<std::uint32_t>(core_ids.size());
    }
}

void fillMotherboardInfo(MotherboardInfo& info)
{
    info.manufacturer = fromLocal(readLine("/sys/class/dmi/id/board_vendor"));
    info.product      = fromLocal(readLine("/sys/class/dmi/id/board_name"));
    info.version      = fromLocal(readLine("/sys/class/dmi/id/board_version"));
    info.serial       = fromLocal(readLine("/sys/class/dmi/id/board_serial"));
    if (info.manufacturer.empty()) {
        info.manufacturer = fromLocal(readLine("/sys/class/dmi/id/sys_vendor"));
    }
    if (info.product.empty()) {
        info.product = fromLocal(readLine("/sys/class/dmi/id/product_name"));
    }
}

std::vector<DiskInfo> enumerateDisks()
{
    namespace fs = std::filesystem;

    std::vector<DiskInfo> result;
    std::error_code       ec;

    // /sys/block lists WHOLE devices only - a partition lives under /sys/class/block/<dev>/ and never
    // appears here - so there is no byte pattern in a device name to interpret. The previous
    // "skip a name that contains a digit" rule was a partition heuristic that also dropped every
    // nvme0n1 and mmcblk0, i.e. every disk of a modern machine.
    for (const auto& entry : fs::directory_iterator(fs::path("/sys/block"), ec)) {
        const std::string name = entry.path().filename().string();
        if (!isStorageDevice(name)) {
            continue;
        }

        DiskInfo disk;
        disk.name = fromLocal("/dev/" + name);

        // size is a count of 512-byte sectors for every device, whatever its logical block size is.
        unsigned long long sectors = 0;
        if (std::ifstream size_file(entry.path() / "size"); size_file) {
            size_file >> sectors;
        }
        disk.capacity = sectors * 512;

        if (name.rfind("sr", 0) == 0) {
            // An optical drive is a disk the caller may want to see; its size is 0 while it holds no
            // media, which is what DiskKind::CdRom tells the caller.
            disk.kind = DiskKind::CdRom;
        }
        else if (name.rfind("nbd", 0) == 0) {
            // A network block device: the vocabulary has the word, and this is what it means.
            disk.kind = DiskKind::Network;
        }
        else {
            int removable = 0;
            if (std::ifstream removable_file(entry.path() / "removable"); removable_file) {
                removable_file >> removable;
            }
            disk.kind = removable != 0 ? DiskKind::Removable : DiskKind::Fixed;
        }

        disk.model  = fromLocal(readLine(entry.path() / "device" / "model"));
        disk.serial = fromLocal(readLine(entry.path() / "device" / "serial"));
        result.push_back(std::move(disk));
    }
    return result;
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
    // f_bavail, not f_bfree: the header promises the space available to the caller, and the two
    // differ on a volume that reserves blocks for root.
    return static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
}

int currentPid()
{
    return static_cast<int>(getpid());
}

String executablePath()
{
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return String::fromLocal8Bit(buffer.data(), static_cast<String::size_type>(length));
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool pidExists(int pid)
{
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM; // Exists, but belongs to another user.
}

std::vector<int> pidsByName(const String& name)
{
    namespace fs = std::filesystem;

    std::vector<int> result;
    std::error_code  ec;
    for (const auto& entry : fs::directory_iterator(fs::path("/proc"), ec)) {
        const std::string pid_text = entry.path().filename().string();
        if (!isNumeric(pid_text)) {
            continue;
        }
        int        pid    = 0;
        const auto parsed = std::from_chars(pid_text.data(), pid_text.data() + pid_text.size(), pid);
        if (parsed.ec != std::errc{}) {
            continue;
        }

        // /proc/<pid>/exe is a magic link to the executable. A process whose executable was deleted
        // reports "<path> (deleted)", which is not a file name and does not match - Kernel threads
        // have no such link and are skipped by the read error.
        const fs::path exe = fs::read_symlink(fs::path("/proc") / pid_text / "exe", ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (fromLocal(exe.filename().string()) == name) {
            result.push_back(pid);
        }
    }
    return result;
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
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        result.emplace_back(fromLocal(text.substr(0, equals)), fromLocal(text.substr(equals + 1)));
    }
    return result;
}

namespace
{

/// An absolute value of @p name, or an empty string: the XDG specification says a relative value is
/// invalid and must be ignored, and an empty one means "not set".
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

bool readMeminfoKiB(const std::string& line, const char* key, std::uint64_t& out)
{
    const std::size_t key_length = std::strlen(key);
    if (line.compare(0, key_length, key) != 0) {
        return false;
    }
    unsigned long long kib = 0;
    if (std::sscanf(line.c_str() + key_length, " %llu", &kib) != 1) {
        return false;
    }
    out = static_cast<std::uint64_t>(kib) * 1024;
    return true;
}

} // namespace

String temporaryDirectory()
{
    // TMPDIR first: a mount namespace, a systemd unit or a container sets it per process, and the
    // world-writable /tmp is only the fallback.
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
    const String from_variable = absoluteVariable("XDG_DATA_HOME");
    return from_variable.empty() ? below(homeDirectory(), u8"/.local/share") : from_variable;
}

String configDirectory()
{
    const String from_variable = absoluteVariable("XDG_CONFIG_HOME");
    return from_variable.empty() ? below(homeDirectory(), u8"/.config") : from_variable;
}

String cacheDirectory()
{
    const String from_variable = absoluteVariable("XDG_CACHE_HOME");
    return from_variable.empty() ? below(homeDirectory(), u8"/.cache") : from_variable;
}

void fillMemoryInfo(MemoryInfo& info)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        info.pageSize = static_cast<std::uint32_t>(page_size);
    }

    // MemTotal / MemAvailable are the kernel's own answer for what a new allocation can get;
    // sysinfo() is the fallback for kernels that publish no MemAvailable (before 3.14).
    bool          total_known = false;
    std::ifstream meminfo("/proc/meminfo");
    std::string   line;
    while (std::getline(meminfo, line)) {
        if (!total_known) {
            total_known = readMeminfoKiB(line, "MemTotal:", info.totalBytes);
            continue;
        }
        if (readMeminfoKiB(line, "MemAvailable:", info.availableBytes)) {
            break;
        }
    }

    if (total_known && info.availableBytes != 0) {
        return;
    }
    struct sysinfo usage{};
    if (sysinfo(&usage) == 0) {
        if (!total_known) {
            info.totalBytes = static_cast<std::uint64_t>(usage.totalram) * usage.mem_unit;
        }
        if (info.availableBytes == 0) {
            info.availableBytes = static_cast<std::uint64_t>(usage.freeram) * usage.mem_unit;
        }
    }
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
    // getpwuid_r, not getpwuid: the latter answers from a shared static buffer, and several threads
    // may ask at the same time.
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
