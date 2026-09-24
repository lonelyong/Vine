#include <vine/system/Process.hpp>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
// clang-format off
#    include <windows.h>  // Must precede tlhelp32.h.
#    include <tlhelp32.h>
// clang-format on
#elif defined(__linux__)
#    include <filesystem>
#    include <signal.h>
#    include <unistd.h>
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <signal.h>
#    include <unistd.h>
#endif

VN_SYSTEM_NS_BEGIN

int Process::currentProcessId()
{
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

String Process::currentExecutablePath()
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            return {};
        }
        if (len < buffer.size()) {
            return String::fromUtf16(reinterpret_cast<const char16_t*>(buffer.data()), len);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    char     path[4096] = {};
    uint32_t size       = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        return String::fromLocal8Bit(path);
    }
    return {};
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (len < 0) {
            return {};
        }
        if (static_cast<std::size_t>(len) < buffer.size()) {
            return String::fromLocal8Bit(buffer.data(), static_cast<String::size_type>(len));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

bool Process::exists(int pid)
{
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    CloseHandle(handle);
    return true;
#else
    if (::kill(pid, 0) == 0) {
        return true;
    }
    return errno == EPERM; // Exists, but access is denied.
#endif
}

namespace
{

String exeNameFromPath(const String& path)
{
    const std::size_t slash = path.find_last_of(u8"/\\");
    if (slash == String::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

#ifdef _WIN32
bool iequals(const String& a, const String& b)
{
    return a.toLower() == b.toLower();
}
#endif

} // namespace

std::vector<int> Process::findByName(const String& name)
{
    if (name.empty()) {
        return {};
    }
#ifdef _WIN32
    const String     lowered_name = name.toLower();
    std::vector<int> result;
    const HANDLE     snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const String exe_name = String::fromUtf16(reinterpret_cast<const char16_t*>(entry.szExeFile));
            if (iequals(exe_name, lowered_name)) {
                result.push_back(static_cast<int>(entry.th32ProcessID));
            }
        }
        while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
#elif defined(__linux__)
    std::vector<int> result;
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path("/proc"), ec)) {
        const std::string name_str = entry.path().filename().string();
        bool              numeric  = !name_str.empty();
        for (char c : name_str) {
            if (c < '0' || c > '9') {
                numeric = false;
                break;
            }
        }
        if (!numeric) {
            continue;
        }
        const fs::path exe_link = fs::path("/proc") / name_str / "exe";
        const fs::path exe_path = fs::read_symlink(exe_link, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const String exe_name = String::fromLocal8Bit(exe_path.filename().string().c_str());
        if (exe_name == name) {
            result.push_back(std::stoi(name_str));
        }
    }
    return result;
#else
    // macOS and other platforms: process enumeration is not implemented.
    return {};
#endif
}

bool Process::kill(int pid)
{
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    const HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    const BOOL ok = TerminateProcess(handle, 1);
    CloseHandle(handle);
    return ok != FALSE;
#else
    return ::kill(pid, SIGKILL) == 0;
#endif
}

VN_SYSTEM_NS_END