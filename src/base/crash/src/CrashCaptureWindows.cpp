#include <vine/crash/CrashHandler.hpp>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>

// clang-format off
#ifndef NOMINMAX
#    define NOMINMAX // windows.h's min/max macros would otherwise capture std::min in this file.
#endif
#include <windows.h>  // Must precede dbghelp.h.
#include <dbghelp.h>
// clang-format on

#pragma comment(lib, "dbghelp.lib")

#include "CrashDetail.hpp"

VN_CRASH_NS_BEGIN
namespace detail {
namespace {

/// @brief Everything @ref install resolved, in the shape the crash path needs: plain buffers, no allocation, no lookups.
struct Resolved
{
    char        app[Report::kAppChars]{};
    wchar_t     app_wide[Report::kAppChars]{};
    char        context[Report::kMaxContext][Report::kContextChars]{};
    std::size_t context_count{ 0 };
    int         max_frames{ static_cast<int>(Report::kMaxFrames) };
    wchar_t     log_path[512]{};
    wchar_t     dump_path[512]{};
    wchar_t     handler_path[512]{};
    wchar_t     command_line[2048]{};
    bool        launch_handler{ false };
};

Resolved s_resolved;

/// @note Static, not automatic: the crashing thread may be the one that overflowed its stack, so the crash path must not
///       ask for 40 KB of it.
Report s_report;
char   s_text[32768];

volatile LONG                 s_filter_running{ 0 };
bool                          s_armed{ false };
LPTOP_LEVEL_EXCEPTION_FILTER s_previous{ nullptr };

/**
 * @brief Copies UTF-8 into a fixed field, always terminated and never splitting a sequence.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 * @param text Text to copy, truncated when it does not fit.
 */
void copyUtf8(char* out, std::size_t capacity, std::string_view text) noexcept
{
    if (capacity == 0) {
        return;
    }
    std::size_t count = std::min(text.size(), capacity - 1);
    while (count > 0 && count < text.size() && (static_cast<unsigned char>(text[count]) & 0xC0) == 0x80) {
        --count;
    }
    if (count > 0) {
        std::memcpy(out, text.data(), count);
    }
    out[count] = '\0';
}

/**
 * @brief Converts a wide string to UTF-8 in place of an allocation, for the crash path.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 * @param wide Source string; when it does not fit, @p out is left empty rather than half a path.
 */
void wideToUtf8(char* out, std::size_t capacity, const wchar_t* wide) noexcept
{
    if (capacity == 0) {
        return;
    }
    out[0] = '\0';
    if (wide == nullptr || wide[0] == L'\0') {
        return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(capacity), nullptr, nullptr) <= 0) {
        out[0] = '\0';
    }
}

/**
 * @brief Copies a wide string into a fixed wide buffer.
 *
 * @param out Destination buffer.
 * @param capacity Size of @p out in characters, including the terminator.
 * @param text Source string.
 * @return true when the whole string fitted.
 */
bool copyWide(wchar_t* out, std::size_t capacity, const std::wstring& text) noexcept
{
    if (capacity == 0 || text.size() >= capacity) {
        out[0] = L'\0';
        return false;
    }
    std::memcpy(out, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    return true;
}

/**
 * @brief Converts UTF-8 to UTF-16; called from @ref installPlatform only, where allocating is still allowed.
 *
 * @param text UTF-8 text.
 * @return The UTF-16 text, empty when @p text was empty or invalid.
 */
std::wstring toWideText(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

/**
 * @brief Writes the local wall clock in the one shape every reader can parse: `yyyy-MM-ddThh:mm:ss`.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 */
void writeTimestamp(char* out, std::size_t capacity) noexcept
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    // "yyyy-MM-ddThh:mm:ss" is nineteen characters, and the buffer holds exactly those plus a terminator.
    constexpr std::size_t kTimestampDigits{ 19 };
    char                  buffer[kTimestampDigits + 1]{};
    char*                 cursor = buffer;
    const auto  put2   = [&cursor](WORD value) noexcept {
        *cursor++ = static_cast<char>('0' + (value / 10) % 10);
        *cursor++ = static_cast<char>('0' + value % 10);
    };
    const auto put4 = [&cursor](WORD value) noexcept {
        *cursor++ = static_cast<char>('0' + (value / 1000) % 10);
        *cursor++ = static_cast<char>('0' + (value / 100) % 10);
        *cursor++ = static_cast<char>('0' + (value / 10) % 10);
        *cursor++ = static_cast<char>('0' + value % 10);
    };

    put4(now.wYear);
    *cursor++ = '-';
    put2(now.wMonth);
    *cursor++ = '-';
    put2(now.wDay);
    *cursor++ = 'T';
    put2(now.wHour);
    *cursor++ = ':';
    put2(now.wMinute);
    *cursor++ = ':';
    put2(now.wSecond);

    copyUtf8(out, capacity, std::string_view{ buffer, static_cast<std::size_t>(cursor - buffer) });
}

/**
 * @brief Appends one stack entry, resolving which image it belongs to.
 *
 * @param report Report being assembled.
 * @param address Return address of the frame.
 */
void pushFrame(Report& report, std::uintptr_t address) noexcept
{
    if (report.frame_count >= Report::kMaxFrames) {
        return;
    }

    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address),
                           &module)
        == FALSE)
    {
        module = nullptr;
    }

    int image_index = -1;
    if (module != nullptr) {
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        for (std::size_t i = 0; i < report.image_count; ++i) {
            if (report.images[i].base == base) {
                image_index = static_cast<int>(i);
                break;
            }
        }
        if (image_index < 0 && report.image_count < Report::kMaxImages) {
            ReportImage& image = report.images[report.image_count];
            image.base         = base;

            wchar_t     module_path[512]{};
            const DWORD length = GetModuleFileNameW(module, module_path, static_cast<DWORD>(std::size(module_path)));
            if (length > 0 && length < std::size(module_path)) {
                wideToUtf8(image.path, ReportImage::kPathChars, module_path);
                const wchar_t* name = wcsrchr(module_path, L'\\');
                wideToUtf8(image.name, ReportImage::kNameChars, name != nullptr ? name + 1 : module_path);
            }
            image_index = static_cast<int>(report.image_count++);
        }
    }

    ReportFrame& frame = report.frames[report.frame_count++];
    frame.address      = address;
    frame.image        = image_index;
    frame.offset       = image_index >= 0 ? address - report.images[image_index].base : 0;
}

/**
 * @brief Writes the whole buffer to @p path, replacing what was there.
 *
 * @param path Destination file.
 * @param text Bytes to write.
 * @param size Number of bytes in @p text.
 */
void writeWholeFile(const wchar_t* path, const char* text, std::size_t size) noexcept
{
    if (path[0] == L'\0' || size == 0) {
        return;
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::size_t written_total = 0;
    while (written_total < size) {
        DWORD       written = 0;
        const DWORD chunk   = static_cast<DWORD>(size - written_total);
        if (WriteFile(file, text + written_total, chunk, &written, nullptr) == FALSE || written == 0) {
            break;
        }
        written_total += written;
    }
    CloseHandle(file);
}

/**
 * @brief Writes a minidump of the current process.
 *
 * @param info Exception pointers the dump should record; may be null.
 * @param path Destination file; a failed dump deletes the partial file.
 * @return true when a dump was written.
 */
bool writeDump(EXCEPTION_POINTERS* info, const wchar_t* path) noexcept
{
    if (path[0] == L'\0') {
        return false;
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId          = GetCurrentThreadId();
    exception.ExceptionPointers = info;
    exception.ClientPointers    = FALSE;

    // MiniDumpNormal keeps the file small (threads, stacks, module list) and is what a developer actually opens in a
    // debugger; the report next to it carries the frames even when this call fails.
    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(),
                                           GetCurrentProcessId(),
                                           file,
                                           MiniDumpNormal,
                                           info != nullptr ? &exception : nullptr,
                                           nullptr,
                                           nullptr);
    CloseHandle(file);
    if (written == FALSE) {
        DeleteFileW(path);
        return false;
    }
    return true;
}

/**
 * @brief Starts the visual handler with the report and dump paths, without waiting for it.
 *
 * @param command_line Command line resolved by @ref installPlatform.
 */
void launchHandler(const wchar_t* command_line) noexcept
{
    wchar_t mutable_line[std::size(s_resolved.command_line)]{};
    if (wcscpy_s(mutable_line, command_line) != 0) {
        return;
    }

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof startup;
    if (CreateProcessW(nullptr, mutable_line, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

/**
 * @brief The last-chance filter: report, dump, handler, exit. Nothing here allocates, locks or unwinds.
 *
 * @param info Exception information the operating system reported.
 * @return Never returns normally; the process exits inside.
 */
LONG WINAPI lastChanceFilter(EXCEPTION_POINTERS* info) noexcept
{
    // A second exception raised inside this function would re-enter it (a broken heap, a stack overflow escalating): the
    // first crash is the one worth reporting, so later entries just let the OS continue its own handling.
    if (InterlockedExchange(&s_filter_running, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    Report& report = s_report;
    std::memset(&report, 0, sizeof report);

    copyUtf8(report.app, sizeof report.app, s_resolved.app);
    writeTimestamp(report.time, sizeof report.time);
    wideToUtf8(report.dump, sizeof report.dump, s_resolved.dump_path);
    for (std::size_t i = 0; i < s_resolved.context_count; ++i) {
        copyUtf8(report.context[i], Report::kContextChars, s_resolved.context[i]);
    }
    report.context_count = s_resolved.context_count;

    if (info != nullptr && info->ExceptionRecord != nullptr) {
        report.code    = info->ExceptionRecord->ExceptionCode;
        report.address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    }
    report.thread_id = GetCurrentThreadId();

    // The faulting instruction is the top of the stack, and RtlCaptureStackBackTrace starts one frame further up (it
    // cannot see the PC the exception was raised at). Skipping one frame drops this filter itself.
    if (report.address != 0) {
        pushFrame(report, report.address);
    }
    void*            raw[Report::kMaxFrames]{};
    const USHORT     captured = RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(raw)), raw, nullptr);
    for (USHORT i = 0; i < captured && report.frame_count < static_cast<std::size_t>(s_resolved.max_frames); ++i) {
        pushFrame(report, reinterpret_cast<std::uintptr_t>(raw[i]));
    }

    // Report first: a dump that cannot be written still leaves the one file that identifies the crash.
    const std::size_t text_size = renderReport(report, std::span<char>(s_text, std::size(s_text)));
    writeWholeFile(s_resolved.log_path, s_text, text_size);
    writeDump(info, s_resolved.dump_path);
    if (s_resolved.launch_handler) {
        launchHandler(s_resolved.command_line);
    }

    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(report.code));
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

bool installPlatform(const Config& config)
{
    std::memset(&s_resolved, 0, sizeof s_resolved);
    s_resolved.max_frames = std::clamp(config.max_frames, 1, static_cast<int>(Report::kMaxFrames));

    wchar_t     exe_path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe_path, static_cast<DWORD>(std::size(exe_path)));
    if (length == 0 || length >= std::size(exe_path)) {
        return false; // A truncated path means we cannot know where our own files belong: better no handler than one that
                      // writes into the wrong directory.
    }
    const std::wstring exe_file{ exe_path, length };
    const std::wstring exe_dir    = exe_file.substr(0, exe_file.find_last_of(L"\\/") + 1);
    const std::wstring exe_name   = exe_file.substr(exe_file.find_last_of(L"\\/") + 1);
    const std::wstring exe_stem   = exe_name.substr(0, exe_name.find_last_of(L'.'));

    // The name reaches a file name, an argument and a window title, so it may not be empty: the executable's own stem is
    // always there, and "app" is the last resort for a process renamed to something without a stem.
    std::wstring app = toWideText(config.app_name.as_std_str());
    if (app.empty()) {
        app = exe_stem;
    }
    if (app.empty()) {
        app = L"app";
    }
    if (!copyWide(s_resolved.app_wide, std::size(s_resolved.app_wide), app)) {
        return false;
    }
    wideToUtf8(s_resolved.app, sizeof s_resolved.app, s_resolved.app_wide);

    const std::wstring log_path     = config.log_file.empty() ? exe_dir + app + L"_crash.log" : config.log_file.native();
    const std::wstring dump_path    = config.dump_file.empty() ? exe_dir + app + L"_crash.dmp" : config.dump_file.native();
    const std::wstring handler_path = config.handler_exe.empty() ? exe_dir + L"CrashHandler.exe" : config.handler_exe.native();

    if (!copyWide(s_resolved.log_path, std::size(s_resolved.log_path), log_path)
        || !copyWide(s_resolved.dump_path, std::size(s_resolved.dump_path), dump_path)
        || !copyWide(s_resolved.handler_path, std::size(s_resolved.handler_path), handler_path))
    {
        return false;
    }

    for (const String& line : config.context) {
        if (s_resolved.context_count >= Report::kMaxContext) {
            break;
        }
        copyUtf8(s_resolved.context[s_resolved.context_count], Report::kContextChars, line.as_std_str());
        ++s_resolved.context_count;
    }

    std::wstring command_line = L"\"" + handler_path + L"\" --report \"" + log_path + L"\" --dump \"" + dump_path
                              + L"\" --app \"" + app + L"\"";
    if (!config.mailto.empty()) {
        command_line += L" --mailto \"" + toWideText(config.mailto.as_std_str()) + L"\"";
    }
    // A command line that does not fit would reach the handler with a truncated argument, i.e. with a wrong path: the
    // report is written either way, so the launch is what gets dropped.
    s_resolved.launch_handler = copyWide(s_resolved.command_line, std::size(s_resolved.command_line), command_line)
                                && GetFileAttributesW(handler_path.c_str()) != INVALID_FILE_ATTRIBUTES;

    s_previous = SetUnhandledExceptionFilter(&lastChanceFilter);
    s_armed    = true;
    return true;
}

void uninstallPlatform()
{
    if (s_armed) {
        SetUnhandledExceptionFilter(s_previous);
        s_armed    = false;
        s_previous = nullptr;
    }
    std::memset(&s_resolved, 0, sizeof s_resolved);
    InterlockedExchange(&s_filter_running, 0);
}

} // namespace detail
VN_CRASH_NS_END
