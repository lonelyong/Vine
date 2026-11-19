#include "SystemPlatform.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// clang-format off
#include <windows.h>      // Must precede tlhelp32.h, winioctl.h and winternl.h.
#include <tlhelp32.h>
#include <winioctl.h>
#include <winternl.h>     // RTL_OSVERSIONINFOW, which windows.h does not declare.
// clang-format on
#if defined(_M_X64) || defined(_M_IX86)
#    include <intrin.h>
#endif

VN_SYSTEM_NS_BEGIN

namespace detail
{

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

/// Removes the padding the firmware and the registry put around fixed-width strings, so a model
/// reads "Samsung SSD 860" and not "Samsung SSD 860    ".
String trimmed(String text)
{
    while (!text.empty()) {
        const char8_t last = text.back();
        if (last != u8' ' && last != u8'\t' && last != u8'\r' && last != u8'\n') {
            break;
        }
        text.pop_back();
    }
    return text;
}

String readRegSz(HKEY key, const wchar_t* name)
{
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    DWORD                type  = 0;
    DWORD                bytes = size;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS || type != REG_SZ) {
        return {};
    }
    return trimmed(String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data())));
}

String readRegSz(const wchar_t* key_path, const wchar_t* name)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    const String value = readRegSz(key, name);
    RegCloseKey(key);
    return value;
}

/// Returns the one physical disk that backs a volume root ("C:\\"), or -1 when the volume has no
/// single backing disk (it is spanned) or the query is refused.
int physicalDiskNumber(const std::wstring& drive_root)
{
    std::wstring drive = drive_root;
    while (!drive.empty() && drive.back() == L'\\') {
        drive.pop_back();
    }
    const std::wstring device_path = L"\\\\.\\" + drive;

    const HANDLE handle = CreateFileW(device_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    VOLUME_DISK_EXTENTS single{};
    DWORD               bytes = 0;
    BOOL                ok    = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &single, sizeof(single), &bytes, nullptr);
    if (ok) {
        CloseHandle(handle);
        return single.NumberOfDiskExtents == 1 ? static_cast<int>(single.Extents[0].DiskNumber) : -1;
    }
    // More than one extent answers ERROR_MORE_DATA together with the size it needs. A spanned volume
    // has no single model and serial to report, so both the one-extent and the greater-than-one case
    // end in the same "no identity" answer instead of the first disk's.
    if (GetLastError() != ERROR_MORE_DATA || bytes <= sizeof(single)) {
        CloseHandle(handle);
        return -1;
    }
    std::vector<BYTE> raw(bytes);
    ok = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, raw.data(), static_cast<DWORD>(raw.size()), &bytes, nullptr);
    CloseHandle(handle);
    if (!ok) {
        return -1;
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(raw.data());
    return extents->NumberOfDiskExtents == 1 ? static_cast<int>(extents->Extents[0].DiskNumber) : -1;
}

void fillDiskIdentity(int disk_number, DiskInfo& disk)
{
    if (disk_number < 0) {
        return;
    }
    wchar_t physical_path[64] = {};
    std::swprintf(physical_path, 64, L"\\\\.\\PhysicalDrive%lu", static_cast<unsigned long>(disk_number));

    const HANDLE handle = CreateFileW(physical_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;

    std::vector<BYTE> raw(4096);
    DWORD             returned = 0;
    const BOOL        ok =
        DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), raw.data(), static_cast<DWORD>(raw.size()), &returned, nullptr);
    CloseHandle(handle);
    if (!ok || returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return;
    }

    const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(raw.data());
    if (desc->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR) || desc->Size > returned) {
        return; // A descriptor that does not describe itself is not read through.
    }
    // Every offset is relative to the descriptor and is bounded by desc->Size; the string itself may
    // be unterminated, so its length is found inside the same bound.
    const auto field = [&](DWORD offset) -> String {
        if (offset == 0 || offset >= desc->Size) {
            return {};
        }
        DWORD length = 0;
        while (offset + length < desc->Size && raw[offset + length] != 0) {
            ++length;
        }
        return trimmed(String::fromLocal8Bit(reinterpret_cast<const char*>(raw.data() + offset), length));
    };

    disk.serial = field(desc->SerialNumberOffset);

    const String vendor  = field(desc->VendorIdOffset);
    const String product = field(desc->ProductIdOffset);
    if (vendor.empty()) {
        disk.model = product;
    }
    else if (product.empty()) {
        disk.model = vendor;
    }
    else {
        disk.model = vendor + u8" " + product;
    }
}

std::uint64_t queryVolumeSpace(const String& path, bool free)
{
    if (path.empty()) {
        return 0;
    }
    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows wchar_t is UTF-16");
    const auto wide_path = path.toUtf16();

    ULARGE_INTEGER free_to_caller{}, total{}, free_total{};
    if (GetDiskFreeSpaceExW(reinterpret_cast<const wchar_t*>(wide_path.c_str()), &free_to_caller, &total, &free_total)) {
        return free ? free_to_caller.QuadPart : total.QuadPart;
    }
    // The API wants a directory: retry with the parent, e.g. "C:\\file.txt" -> "C:\\".
    const std::size_t slash = path.find_last_of(u8"/\\");
    if (slash == String::npos) {
        return 0;
    }
    const String parent = slash == 0 ? String(u8"/") : path.substr(0, slash);
    if (parent == path) {
        return 0; // Nothing left to walk up to; the query has no answer.
    }
    return queryVolumeSpace(parent, free);
}

} // namespace

void fillOsInfo(OperatingSystemInfo& info)
{
    info.name = u8"Windows";

    // RtlGetVersion is the version the API contract promises; GetVersionEx lies unless a manifest
    // asks otherwise, so the ntdll entry point is used instead.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll   = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn != nullptr) {
            RTL_OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = static_cast<DWORD>(sizeof(version));
            if (fn(&version) == 0) {
                info.version = joinVersion(version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
            }
        }
    }

    SYSTEM_INFO system_info{};
    GetNativeSystemInfo(&system_info);
    switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: info.architecture = u8"x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: info.architecture = u8"arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: info.architecture = u8"x86"; break;
    case PROCESSOR_ARCHITECTURE_ARM: info.architecture = u8"arm"; break;
    default: break;
    }
}

void fillCpuInfo(CpuInfo& info)
{
    info.logicalCores = static_cast<std::uint32_t>(std::thread::hardware_concurrency());

    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size != 0) {
        std::vector<unsigned char> raw(size);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(raw.data()), &size)) {
            unsigned count  = 0;
            DWORD    offset = 0;
            while (offset < size) {
                const auto* record = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(raw.data() + offset);
                if (record->Size == 0) {
                    break; // A record that does not advance would walk the buffer forever.
                }
                ++count;
                offset += record->Size;
            }
            info.physicalCores = static_cast<std::uint32_t>(count);
        }
    }

#if defined(_M_X64) || defined(_M_IX86)
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
        info.model = trimmed(String::fromLocal8Bit(model));
    }
#else
    // ARM64 has no __cpuid and no vendor leaf: the processor name comes from the registry and the
    // vendor stays empty, as Hardware.hpp states.
    info.model = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
#endif
}

void fillMotherboardInfo(MotherboardInfo& info)
{
    info.manufacturer = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardManufacturer");
    info.product      = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardProduct");
    info.version      = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardVersion");
    info.serial       = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"BaseBoardSerialNumber");
    if (info.manufacturer.empty()) {
        info.manufacturer = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer");
    }
    if (info.product.empty()) {
        info.product = readRegSz(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName");
    }
}

std::vector<DiskInfo> enumerateDisks()
{
    std::vector<DiskInfo> result;

    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required <= 1) {
        return result;
    }
    std::vector<wchar_t> roots(required, L'\0');
    if (GetLogicalDriveStringsW(required, roots.data()) == 0) {
        return result;
    }

    for (const wchar_t* root = roots.data(); *root != L'\0'; root += std::wcslen(root) + 1) {
        const std::wstring drive_root(root);

        DiskInfo disk;
        disk.name = String::fromUtf16(reinterpret_cast<const char16_t*>(drive_root.c_str()));

        switch (GetDriveTypeW(drive_root.c_str())) {
        case DRIVE_FIXED: disk.kind = DiskKind::Fixed; break;
        case DRIVE_REMOVABLE: disk.kind = DiskKind::Removable; break;
        case DRIVE_REMOTE: disk.kind = DiskKind::Network; break;
        case DRIVE_CDROM: disk.kind = DiskKind::CdRom; break;
        default: disk.kind = DiskKind::Unknown; break;
        }

        ULARGE_INTEGER free_to_caller{}, total{}, free_total{};
        if (GetDiskFreeSpaceExW(drive_root.c_str(), &free_to_caller, &total, &free_total)) {
            disk.capacity = total.QuadPart;
        }
        fillDiskIdentity(physicalDiskNumber(drive_root), disk);
        result.push_back(std::move(disk));
    }
    return result;
}

std::uint64_t volumeTotalSpace(const String& path)
{
    return queryVolumeSpace(path, false);
}

std::uint64_t volumeFreeSpace(const String& path)
{
    return queryVolumeSpace(path, true);
}

int currentPid()
{
    return static_cast<int>(GetCurrentProcessId());
}

String executablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool pidExists(int pid)
{
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle != nullptr) {
        CloseHandle(handle);
        return true;
    }
    // Access denied means the process is there and protected. Reporting it as absent would answer a
    // different question than the POSIX half does for EPERM.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

std::vector<int> pidsByName(const String& name)
{
    std::vector<int> result;
    const String     lowered = name.toLower();

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const String exe_name = String::fromUtf16(reinterpret_cast<const char16_t*>(entry.szExeFile));
            // The platform compares file names case-insensitively, which is what the header says.
            if (exe_name.toLower() == lowered) {
                result.push_back(static_cast<int>(entry.th32ProcessID));
            }
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool terminatePid(int pid)
{
    const HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    const BOOL ok = TerminateProcess(handle, 1);
    CloseHandle(handle);
    return ok != FALSE;
}

namespace
{

/// Reads a variable whose name is ASCII (the environment names this module asks for are).
String variable(const char* name)
{
    String value;
    environmentVariable(String::fromLocal8Bit(name), value);
    return value;
}

} // namespace

bool environmentVariable(const String& name, String& value)
{
    if (name.empty()) {
        return false;
    }
    const auto  wide_name = name.toUtf16();
    const auto* wide_text = reinterpret_cast<const wchar_t*>(wide_name.c_str());

    // The first call asks for the size. A variable that exists and is empty needs one character,
    // and an absent variable reports 0, so existence and emptiness are decided here and not by the
    // second call, which stores 0 characters for an empty value.
    const DWORD required = GetEnvironmentVariableW(wide_text, nullptr, 0);
    if (required == 0) {
        return false;
    }
    if (required == 1) {
        value.clear();
        return true;
    }
    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD          copied = GetEnvironmentVariableW(wide_text, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) {
        return false;
    }
    value = String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), copied);
    return true;
}

std::vector<std::pair<String, String>> environmentVariables()
{
    std::vector<std::pair<String, String>> result;

    const LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return result;
    }
    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        // "=C:=C:\dir" is a per-drive working directory, not a name/value pair a caller can use.
        if (*entry == L'=') {
            continue;
        }
        const wchar_t* equals = std::wcschr(entry, L'=');
        if (equals == nullptr) {
            continue;
        }
        result.emplace_back(String::fromUtf16(reinterpret_cast<const char16_t*>(entry), static_cast<std::size_t>(equals - entry)),
                            String::fromUtf16(reinterpret_cast<const char16_t*>(equals + 1)));
    }
    FreeEnvironmentStringsW(block);
    return result;
}

String temporaryDirectory()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        // Too small a buffer reports the size it needs, including the terminating null.
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), length);
        }
        buffer.resize(length + 1);
    }
}

String homeDirectory()
{
    const String profile = variable("USERPROFILE");
    if (!profile.empty()) {
        return profile;
    }
    // A profile that is served from a network share reports the drive and the path separately.
    return variable("HOMEDRIVE") + variable("HOMEPATH");
}

String dataDirectory()
{
    const String local = variable("LOCALAPPDATA");
    if (!local.empty()) {
        return local;
    }
    const String home = homeDirectory();
    return home.empty() ? String() : home + u8"\\AppData\\Local";
}

String configDirectory()
{
    const String roaming = variable("APPDATA");
    if (!roaming.empty()) {
        return roaming;
    }
    const String home = homeDirectory();
    return home.empty() ? String() : home + u8"\\AppData\\Roaming";
}

String cacheDirectory()
{
    // Windows has one per-user local root; it does not separate "cache" from "data".
    return dataDirectory();
}

void fillMemoryInfo(MemoryInfo& info)
{
    MEMORYSTATUSEX status{};
    status.dwLength = static_cast<DWORD>(sizeof(status));
    if (GlobalMemoryStatusEx(&status)) {
        info.totalBytes     = status.ullTotalPhys;
        info.availableBytes = status.ullAvailPhys;
    }

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    info.pageSize = system_info.dwPageSize;
}

String hostName()
{
    constexpr DWORD kCapacity         = 256;
    wchar_t         buffer[kCapacity] = {};
    DWORD           size              = kCapacity;

    // The DNS name first: the legacy NetBIOS name is capped at 15 characters.
    if (!GetComputerNameExW(ComputerNameDnsHostname, buffer, &size)) {
        size = kCapacity;
        if (!GetComputerNameW(buffer, &size)) {
            return {};
        }
    }
    return String::fromUtf16(reinterpret_cast<const char16_t*>(buffer), size);
}

String userName()
{
    DWORD required = 0;
    GetUserNameW(nullptr, &required); // Fails with ERROR_INSUFFICIENT_BUFFER and reports the size.
    if (required == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(required, L'\0');
    DWORD                copied = required;
    if (!GetUserNameW(buffer.data(), &copied) || copied == 0) {
        return {};
    }
    // The report counts the terminating null.
    return String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), copied - 1);
}

} // namespace detail

VN_SYSTEM_NS_END
