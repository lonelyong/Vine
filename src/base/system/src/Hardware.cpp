#include <vine/system/Hardware.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#    include <windows.h>  // Must precede winioctl.h.
#    include <winioctl.h>
// clang-format on
#    include <cwchar>
#    include <intrin.h>
#elif defined(__linux__)
#    include <filesystem>
#    include <fstream>
#    include <sys/statvfs.h>
#    include <unistd.h>
#elif defined(__APPLE__)
#    include <sys/statvfs.h>
#    include <sys/sysctl.h>
#endif

VN_SYSTEM_NS_BEGIN

namespace
{

#ifdef __linux__
String readTextLine(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    return String::fromLocal8Bit(line.c_str());
}
#endif

#ifdef _WIN32
String readRegSz(HKEY key, const wchar_t* name)
{
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    DWORD                type  = 0;
    DWORD                bytes = size + sizeof(wchar_t);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()));
}
#endif

#ifdef __linux__
std::string trim(const std::string& s)
{
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}
#endif

CpuInfo buildCpuInfo()
{
    CpuInfo info;
#ifdef _WIN32
    int regs[4] = { 0 };
    __cpuid(regs, 0);
    char vendor[13] = {};
    std::memcpy(vendor, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    info.vendor = String::fromLocal8Bit(vendor);

    __cpuid(regs, 0x80000000);
    if (regs[0] >= 0x80000004) {
        char model[49] = {};
        for (int leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
            __cpuid(regs, leaf);
            std::memcpy(model + (leaf - 0x80000002) * 16, regs, 16);
        }
        info.model = String::fromLocal8Bit(model);
    }

    info.logicalCores = static_cast<unsigned>(std::thread::hardware_concurrency());

    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size != 0) {
        std::vector<unsigned char> raw(size);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(raw.data()), &size)) {
            unsigned count  = 0;
            DWORD    offset = 0;
            while (offset < size) {
                const auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(raw.data() + offset);
                ++count;
                offset += entry->Size;
            }
            info.physicalCores = count;
        }
    }
#elif defined(__APPLE__)
    int         logical  = 0;
    int         physical = 0;
    std::size_t len      = sizeof(int);
    if (sysctlbyname("hw.logicalcpu", &logical, &len, nullptr, 0) == 0) {
        info.logicalCores = static_cast<unsigned>(logical);
    }
    len = sizeof(int);
    if (sysctlbyname("hw.physicalcpu", &physical, &len, nullptr, 0) == 0) {
        info.physicalCores = static_cast<unsigned>(physical);
    }
    char brand[256] = {};
    len             = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0) {
        info.model = String::fromLocal8Bit(brand);
    }
    char vendor[64] = {};
    len             = sizeof(vendor);
    if (sysctlbyname("machdep.cpu.vendor", vendor, &len, nullptr, 0) == 0) {
        info.vendor = String::fromLocal8Bit(vendor);
    }
#elif defined(__linux__)
    const long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    info.logicalCores = nprocs > 0 ? static_cast<unsigned>(nprocs) : 1u;

    std::ifstream                                    cpuinfo("/proc/cpuinfo");
    std::string                                      line;
    std::string                                      current_physical;
    std::vector<std::pair<std::string, std::string>> core_ids;
    while (std::getline(cpuinfo, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key   = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "vendor_id" && info.vendor.empty()) {
            info.vendor = String::fromLocal8Bit(value.c_str());
        }
        else if (key == "model name" && info.model.empty()) {
            info.model = String::fromLocal8Bit(value.c_str());
        }
        else if (key == "physical id") {
            current_physical = value;
        }
        else if (key == "core id") {
            core_ids.emplace_back(current_physical, value);
        }
    }
    std::sort(core_ids.begin(), core_ids.end());
    core_ids.erase(std::unique(core_ids.begin(), core_ids.end()), core_ids.end());
    info.physicalCores = static_cast<unsigned>(core_ids.size());
#endif
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
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        info.manufacturer = readRegSz(key, L"BaseBoardManufacturer");
        info.product      = readRegSz(key, L"BaseBoardProduct");
        info.version      = readRegSz(key, L"BaseBoardVersion");
        info.serial       = readRegSz(key, L"BaseBoardSerialNumber");
        if (info.manufacturer.empty()) {
            info.manufacturer = readRegSz(key, L"SystemManufacturer");
        }
        if (info.product.empty()) {
            info.product = readRegSz(key, L"SystemProductName");
        }
        RegCloseKey(key);
    }
#elif defined(__APPLE__)
    info.manufacturer      = u8"Apple";
    char        model[128] = {};
    std::size_t len        = sizeof(model);
    if (sysctlbyname("hw.model", model, &len, nullptr, 0) == 0) {
        info.product = String::fromLocal8Bit(model);
    }
#elif defined(__linux__)
    info.manufacturer = readTextLine("/sys/class/dmi/id/board_vendor");
    info.product      = readTextLine("/sys/class/dmi/id/board_name");
    info.version      = readTextLine("/sys/class/dmi/id/board_version");
    info.serial       = readTextLine("/sys/class/dmi/id/board_serial");
    if (info.manufacturer.empty()) {
        info.manufacturer = readTextLine("/sys/class/dmi/id/sys_vendor");
    }
    if (info.product.empty()) {
        info.product = readTextLine("/sys/class/dmi/id/product_name");
    }
#endif
    return info;
}

#ifdef _WIN32
void fillDiskIdentity(const wchar_t* drive_root, DiskInfo& disk)
{
    // drive_root is like L"C:\\"; build the device path \\.\C: by stripping the trailing backslash.
    std::wstring drive = drive_root;
    if (!drive.empty() && drive.back() == L'\\') {
        drive.pop_back();
    }
    const std::wstring device_path = L"\\\\.\\" + drive;

    const HANDLE drive_handle = CreateFileW(device_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (drive_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    VOLUME_DISK_EXTENTS extents{};
    DWORD               bytes = 0;
    const BOOL          ok    = DeviceIoControl(drive_handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents, sizeof(extents), &bytes, nullptr);
    CloseHandle(drive_handle);
    if (!ok || extents.NumberOfDiskExtents == 0) {
        return;
    }
    wchar_t physical_path[64] = {};
    swprintf(physical_path, 64, L"\\\\.\\PhysicalDrive%lu", extents.Extents[0].DiskNumber);
    const HANDLE physical_handle = CreateFileW(physical_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (physical_handle == INVALID_HANDLE_VALUE) {
        return;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId        = StorageDeviceProperty;
    query.QueryType         = PropertyStandardQuery;
    BYTE       buffer[4096] = {};
    DWORD      returned     = 0;
    const BOOL ok2          = DeviceIoControl(physical_handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &returned, nullptr);
    CloseHandle(physical_handle);
    if (!ok2) {
        return;
    }
    const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
    if (desc->SerialNumberOffset != 0) {
        disk.serial = String::fromLocal8Bit(reinterpret_cast<const char*>(buffer + desc->SerialNumberOffset));
    }
    if (desc->VendorIdOffset == 0 && desc->ProductIdOffset == 0) {
        return;
    }
    const String vendor  = desc->VendorIdOffset != 0 ? String::fromLocal8Bit(reinterpret_cast<const char*>(buffer + desc->VendorIdOffset)) : String();
    const String product = desc->ProductIdOffset != 0 ? String::fromLocal8Bit(reinterpret_cast<const char*>(buffer + desc->ProductIdOffset)) : String();
    if (product.empty()) {
        disk.model = vendor;
    }
    else {
        disk.model = vendor.empty() ? product : vendor + u8" " + product;
    }
}
#endif

std::vector<DiskInfo> buildDisks()
{
    std::vector<DiskInfo> result;
#ifdef _WIN32
    std::vector<wchar_t> buffer(128);
    DWORD                len = 0;
    do {
        buffer.resize(buffer.size() * 2);
        len = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size()), buffer.data());
    }
    while (len > buffer.size());
    if (len == 0) {
        return result;
    }
    const wchar_t* p = buffer.data();
    while (*p != L'\0') {
        const std::wstring drive(p);
        DiskInfo           d;
        d.name = String::fromUtf16(reinterpret_cast<const char16_t*>(drive.c_str()));
        switch (GetDriveTypeW(drive.c_str())) {
        case DRIVE_FIXED: d.kind = u8"fixed"; break;
        case DRIVE_REMOVABLE: d.kind = u8"removable"; break;
        case DRIVE_REMOTE: d.kind = u8"network"; break;
        case DRIVE_CDROM: d.kind = u8"cdrom"; break;
        default: d.kind = u8"unknown"; break;
        }
        ULARGE_INTEGER total{}, free_avail{}, free_total{};
        if (GetDiskFreeSpaceExW(drive.c_str(), &free_avail, &total, &free_total)) {
            d.capacity = total.QuadPart;
        }
        fillDiskIdentity(drive.c_str(), d);
        result.push_back(std::move(d));
        p += drive.size() + 1;
    }
#elif defined(__linux__)
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path("/sys/class/block"), ec)) {
        const std::string name = entry.path().filename().string();
        bool              skip = name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 || name.rfind("sr", 0) == 0 || name.rfind("fd", 0) == 0;
        for (char c : name) {
            if (c >= '0' && c <= '9') {
                skip = true;
                break;
            }
        }
        if (skip) {
            continue;
        }
        DiskInfo d;
        d.name = String::fromLocal8Bit(("/dev/" + name).c_str());

        std::ifstream      size_file(entry.path() / "size");
        unsigned long long sectors = 0;
        size_file >> sectors;
        d.capacity = sectors * 512;

        std::ifstream removable_file(entry.path() / "removable");
        int           removable = 0;
        removable_file >> removable;
        d.kind = removable != 0 ? u8"removable" : u8"fixed";

        d.model  = readTextLine((entry.path() / "device" / "model").string());
        d.serial = readTextLine((entry.path() / "device" / "serial").string());
        result.push_back(std::move(d));
    }
#else
    // macOS and other platforms: no portable block-device enumeration here.
#endif
    return result;
}

#ifdef _WIN32
unsigned long long queryVolumeSpace(const String& path, bool free)
{
    const auto     wide_path = path.toUtf16();
    ULARGE_INTEGER total{}, free_avail{}, free_total{};
    if (GetDiskFreeSpaceExW(reinterpret_cast<const wchar_t*>(wide_path.data()), &free_avail, &total, &free_total)) {
        return free ? free_avail.QuadPart : total.QuadPart;
    }
    // GetDiskFreeSpaceExW expects a directory; retry with the parent directory.
    const std::size_t slash = path.find_last_of(u8"/\\");
    if (slash != String::npos && slash > 0) {
        return queryVolumeSpace(path.substr(0, slash), free);
    }
    return 0;
}
#endif

} // namespace

const CpuInfo& Hardware::cpu()
{
    static const CpuInfo cached = buildCpuInfo();
    return cached;
}

const MotherboardInfo& Hardware::motherboard()
{
    static const MotherboardInfo cached = buildMotherboardInfo();
    return cached;
}

std::vector<DiskInfo> Hardware::disks()
{
    return buildDisks();
}

unsigned long long Hardware::diskTotalSpace(const String& path)
{
#ifdef _WIN32
    return queryVolumeSpace(path, false);
#else
    struct statvfs vfs{};
    if (statvfs(reinterpret_cast<const char*>(path.c_str()), &vfs) == 0) {
        return static_cast<unsigned long long>(vfs.f_blocks) * vfs.f_frsize;
    }
    return 0;
#endif
}

unsigned long long Hardware::diskFreeSpace(const String& path)
{
#ifdef _WIN32
    return queryVolumeSpace(path, true);
#else
    struct statvfs vfs{};
    if (statvfs(reinterpret_cast<const char*>(path.c_str()), &vfs) == 0) {
        return static_cast<unsigned long long>(vfs.f_bavail) * vfs.f_frsize;
    }
    return 0;
#endif
}

VN_SYSTEM_NS_END
