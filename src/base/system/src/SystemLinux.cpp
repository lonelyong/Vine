#include "SystemPlatform.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <signal.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

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
        info.logicalCores = static_cast<unsigned>(online);
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
        info.physicalCores = static_cast<unsigned>(core_ids.size());
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
            // media, which is why DiskInfo::kind exists on the record.
            disk.kind = u8"cdrom";
        }
        else {
            int removable = 0;
            if (std::ifstream removable_file(entry.path() / "removable"); removable_file) {
                removable_file >> removable;
            }
            disk.kind = removable != 0 ? u8"removable" : u8"fixed";
        }

        disk.model  = fromLocal(readLine(entry.path() / "device" / "model"));
        disk.serial = fromLocal(readLine(entry.path() / "device" / "serial"));
        result.push_back(std::move(disk));
    }
    return result;
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

} // namespace detail

VN_SYSTEM_NS_END
