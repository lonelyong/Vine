#include <vine/crash/CrashHandler.hpp>

#include "CrashDetail.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iterator>
#include <string_view>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__has_include)
#    if __has_include(<execinfo.h>)
#        include <execinfo.h>
#        define VN_CRASH_HAS_BACKTRACE 1
#    endif
#endif

VN_CRASH_NS_BEGIN
namespace detail {
namespace {

constexpr std::size_t kPathChars{ ReportImage::kPathChars };
constexpr std::size_t kMapsChars{ 65536 };
constexpr std::size_t kMailtoChars{ 128 };

/**
 * @brief Everything @ref install resolved, in the shape the crash path needs: plain buffers, no allocation, no lookups.
 *
 * @details
 * - Paths are UTF-8 in fixed arrays: the crash path may not allocate, and it may not consult the filesystem beyond the two
 *   files it writes and the handler it starts.
 * - @ref utc_offset_seconds is read once at install, because turning the epoch into a readable local time needs the C
 *   library's time-zone machinery, which takes locks the crashing thread cannot be assumed to survive.
 */
struct Resolved
{
    char        app[Report::kAppChars]{};
    char        context[Report::kMaxContext][Report::kContextChars]{};
    std::size_t context_count{ 0 };
    int         max_frames{ static_cast<int>(Report::kMaxFrames) };
    char        log_path[kPathChars]{};
    char        handler_path[kPathChars]{};
    char        mailto[kMailtoChars]{};
    bool        launch_handler{ false };
    long long   utc_offset_seconds{ 0 };
};

Resolved s_resolved;

/// @note Static, not automatic: the crashing thread may be the one that overflowed its stack.
Report s_report;
char   s_text[32768];
char   s_maps[kMapsChars];

constexpr int kSignals[]{ SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
constexpr std::size_t kSignalCount{ std::size(kSignals) };

struct sigaction        s_previous[kSignalCount]{};
volatile sig_atomic_t   s_handling{ 0 };
bool                    s_armed{ false };

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
    std::size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
    while (count > 0 && count < text.size() && (static_cast<unsigned char>(text[count]) & 0xC0) == 0x80) {
        --count;
    }
    if (count > 0) {
        std::memcpy(out, text.data(), count);
    }
    out[count] = '\0';
}

/**
 * @brief The directory part of @p path, with its trailing `/` kept so it can be concatenated.
 *
 * @param path Absolute path.
 * @return The directory, or an empty view when @p path has no `/`.
 */
std::string_view directoryOf(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash + 1);
}

/**
 * @brief The file name of @p path without its directory and without its last extension.
 *
 * @param path Absolute path.
 * @return The stem, used as the application name when the configuration names none.
 */
std::string_view fileStem(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    std::string_view  name  = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot   = name.rfind('.');
    return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
}

/**
 * @brief Writes @p directory + @p name into a fixed field.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 * @param directory Directory including its trailing `/`.
 * @param name File name inside @p directory.
 * @return true when the whole path fit.
 */
bool joinPath(char* out, std::size_t capacity, std::string_view directory, std::string_view name) noexcept
{
    if (directory.size() + name.size() + 1 > capacity) {
        return false;
    }
    std::memcpy(out, directory.data(), directory.size());
    std::memcpy(out + directory.size(), name.data(), name.size());
    out[directory.size() + name.size()] = '\0';
    return true;
}

/**
 * @brief Reads this process' own executable path.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 * @return true when the path fit and is absolute.
 */
bool readExecutablePath(char* out, std::size_t capacity) noexcept
{
    if (capacity < 2) {
        return false;
    }
    const ssize_t length = ::readlink("/proc/self/exe", out, capacity - 1);
    if (length <= 0 || static_cast<std::size_t>(length) >= capacity - 1) {
        return false;
    }
    out[length] = '\0';
    return out[0] == '/';
}

/**
 * @brief Writes a wall-clock time in the one shape every reader can parse: `yyyy-MM-ddThh:mm:ss`.
 *
 * @details
 * - The date is computed arithmetically from a day count instead of through `localtime`, because the crash path may not call
 *   into the C library's time-zone machinery; the offset from UTC was captured at install time.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 * @param utc_seconds Seconds since the epoch, UTC.
 */
void writeTimestamp(char* out, std::size_t capacity, long long utc_seconds) noexcept
{
    const long long local_seconds = utc_seconds + s_resolved.utc_offset_seconds;

    long long days           = local_seconds / 86400;
    long long second_of_day  = local_seconds % 86400;
    if (second_of_day < 0) {
        second_of_day += 86400;
        --days;
    }

    // Civil date from a day count (days since 1970-01-01 to year/month/day without tables or library calls).
    const long long shifted_days = days + 719468;
    const long long era          = (shifted_days >= 0 ? shifted_days : shifted_days - 146096) / 146097;
    const long long day_of_era   = shifted_days - era * 146097;
    const long long year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const long long year        = year_of_era + era * 400;
    const long long day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const long long month_prime = (5 * day_of_year + 2) / 153;
    const long long day         = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const long long month       = month_prime < 10 ? month_prime + 3 : month_prime - 9;
    const long long civil_year  = year + (month <= 2 ? 1 : 0);

    char       buffer[24]{};
    char*      cursor = buffer;
    const auto put2   = [&cursor](long long value) noexcept {
        *cursor++ = static_cast<char>('0' + (value / 10) % 10);
        *cursor++ = static_cast<char>('0' + value % 10);
    };
    const auto put4 = [&cursor](long long value) noexcept {
        *cursor++ = static_cast<char>('0' + (value / 1000) % 10);
        *cursor++ = static_cast<char>('0' + (value / 100) % 10);
        *cursor++ = static_cast<char>('0' + (value / 10) % 10);
        *cursor++ = static_cast<char>('0' + value % 10);
    };

    put4(civil_year);
    *cursor++ = '-';
    put2(month);
    *cursor++ = '-';
    put2(day);
    *cursor++ = 'T';
    put2(second_of_day / 3600);
    *cursor++ = ':';
    put2((second_of_day / 60) % 60);
    *cursor++ = ':';
    put2(second_of_day % 60);

    copyUtf8(out, capacity, std::string_view{ buffer, static_cast<std::size_t>(cursor - buffer) });
}

/**
 * @brief Difference between this machine's wall clock and UTC, in seconds.
 *
 * @details
 * - Read once at install: the crash path needs the number, not the machinery that produces it.
 *
 * @return Offset to add to an epoch value to obtain local time; 0 when the C library cannot answer.
 */
long long utcOffsetSeconds() noexcept
{
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    if (::localtime_r(&now, &local) == nullptr) {
        return 0;
    }
    std::tm           local_as_utc = local;
    const std::time_t reinterpreted = ::timegm(&local_as_utc);
    return static_cast<long long>(reinterpreted - now);
}

/**
 * @brief Reads a whole file into a buffer.
 *
 * @param path File to read.
 * @param out Destination buffer.
 * @param capacity Size of @p out in bytes.
 * @return Bytes read; less than @p capacity when the file ended (or was cut short).
 */
std::size_t readWholeFile(const char* path, char* out, std::size_t capacity) noexcept
{
    const int file = ::open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return 0;
    }
    std::size_t total = 0;
    while (total < capacity) {
        const ssize_t got = ::read(file, out + total, capacity - total);
        if (got <= 0) {
            break;
        }
        total += static_cast<std::size_t>(got);
    }
    ::close(file);
    return total;
}

/**
 * @brief Writes the whole buffer to @p path, replacing what was there.
 *
 * @param path Destination file.
 * @param text Bytes to write.
 * @param size Number of bytes in @p text.
 * @return true when every byte was written.
 */
bool writeWholeFile(const char* path, const char* text, std::size_t size) noexcept
{
    if (path[0] == '\0' || size == 0) {
        return false;
    }
    const int file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < size) {
        const ssize_t chunk = ::write(file, text + written, size - written);
        if (chunk <= 0) {
            break;
        }
        written += static_cast<std::size_t>(chunk);
    }
    ::close(file);
    return written == size;
}

/**
 * @brief The instruction the fault came from.
 *
 * @details
 * - `si_addr` is the memory the fault touched, which is the interesting address for a bad pointer but not the place to look
 *   in the code; the program counter in the signal context is what the report calls the exception address, as it is on
 *   Windows.
 *
 * @param context Signal context the kernel handed to the handler; may be null.
 * @return The program counter, or 0 when this architecture is not covered.
 */
std::uintptr_t faultAddress(void* context) noexcept
{
    if (context == nullptr) {
        return 0;
    }
    const auto* machine_context = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
    return static_cast<std::uintptr_t>(machine_context->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<std::uintptr_t>(machine_context->uc_mcontext.pc);
#else
    static_cast<void>(machine_context);
    return 0;
#endif
}

/**
 * @brief Appends one stack entry; its image is resolved later, against the mapping table.
 *
 * @param report Report being assembled.
 * @param address Return address of the frame.
 */
void pushFrame(Report& report, std::uintptr_t address) noexcept
{
    if (report.frame_count >= Report::kMaxFrames) {
        return;
    }
    ReportFrame& frame = report.frames[report.frame_count++];
    frame.address      = address;
    frame.image        = -1;
    frame.offset       = 0;
}

/**
 * @brief Starts the visual handler with the report path, without waiting for it.
 *
 * @details
 * - Forks rather than spawning a shell: `fork` and `execv` are the two process calls the crash path is allowed to make, and
 *   the child only ever execs, so an inherited broken heap cannot matter.
 * - The child is detached (`setsid`) and its standard streams are sent to `/dev/null`: a crash window must not depend on the
 *   terminal that started the application, which may already be gone.
 *
 * @return Never returns in the child; returns at once in the parent.
 */
void launchHandler() noexcept
{
    const pid_t child = ::fork();
    if (child != 0) {
        return;
    }

    ::setsid();
    const int null_device = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_device >= 0) {
        ::dup2(null_device, STDIN_FILENO);
        ::dup2(null_device, STDOUT_FILENO);
        ::dup2(null_device, STDERR_FILENO);
        if (null_device > STDERR_FILENO) {
            ::close(null_device);
        }
    }

    char*       argv[8]{};
    std::size_t count = 0;
    argv[count++]     = s_resolved.handler_path;
    argv[count++]     = const_cast<char*>("--report");
    argv[count++]     = s_resolved.log_path;
    argv[count++]     = const_cast<char*>("--app");
    argv[count++]     = s_resolved.app;
    if (s_resolved.mailto[0] != '\0') {
        argv[count++] = const_cast<char*>("--mailto");
        argv[count++] = s_resolved.mailto;
    }
    argv[count] = nullptr;

    ::execv(s_resolved.handler_path, argv);
    ::_exit(127);
}

/**
 * @brief The last-chance handler: report, handler window, then die of the signal the process already died of.
 *
 * @details
 * - Nothing here allocates, locks or unwinds, and the second fault through this path returns immediately: the first crash is
 *   the one worth reporting.
 *
 * @param signal Signal number that fired.
 * @param info Signal information; unused beyond documentation.
 * @param context Machine context of the faulting instruction; may be null.
 */
void onFatalSignal(int signal, siginfo_t* info, void* context) noexcept
{
    static_cast<void>(info);

    if (s_handling != 0) {
        return; // A fault inside the report path: let the default action end the process now.
    }
    s_handling = 1;

    Report& report = s_report;
    std::memset(&report, 0, sizeof report);

    copyUtf8(report.app, sizeof report.app, s_resolved.app);
    writeTimestamp(report.time, sizeof report.time, static_cast<long long>(std::time(nullptr)));
    for (std::size_t i = 0; i < s_resolved.context_count; ++i) {
        copyUtf8(report.context[i], Report::kContextChars, s_resolved.context[i]);
    }
    report.context_count = s_resolved.context_count;

    report.code      = static_cast<unsigned long>(signal);
    report.address   = faultAddress(context);
    report.thread_id = static_cast<unsigned long>(::syscall(SYS_gettid));

    if (report.address != 0) {
        pushFrame(report, report.address);
    }

#if defined(VN_CRASH_HAS_BACKTRACE)
    void*            raw[Report::kMaxFrames]{};
    const int        captured = ::backtrace(raw, static_cast<int>(std::size(raw)));
    const std::size_t first   = 2; // The unwinder's own frame and this handler's: the crash starts below them.
    for (int i = static_cast<int>(first); i < captured && report.frame_count < static_cast<std::size_t>(s_resolved.max_frames); ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(raw[i]);
        if (report.frame_count > 0 && report.frames[report.frame_count - 1].address == address) {
            continue; // The signal frame repeats the program counter already pushed above.
        }
        pushFrame(report, address);
    }
#endif

    // The mapping list is read here rather than at install time: plugins loaded after install are exactly the images a crash
    // most often happens in, and /proc/self/maps is generated by the kernel on read, so it needs no lock the crash can hold.
    if (const std::size_t maps_size = readWholeFile("/proc/self/maps", s_maps, sizeof s_maps); maps_size > 0) {
        resolveImageTable(std::string_view{ s_maps, maps_size }, report);
    }

    const std::size_t text_size = renderReport(report, std::span<char>(s_text, std::size(s_text)));
    writeWholeFile(s_resolved.log_path, s_text, text_size);
    if (s_resolved.launch_handler) {
        launchHandler();
    }

    // The disposition was reset to the default when the handler was entered (SA_RESETHAND), so raising the signal again
    // gives the process a real signal death: the shell sees the signal and the kernel still writes a core if it is enabled.
    ::raise(signal);
    sigset_t unblock{};
    ::sigemptyset(&unblock);
    ::sigaddset(&unblock, signal);
    ::sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
    ::_exit(128 + signal);
}

} // namespace

bool installPlatform(const Config& config)
{
    std::memset(&s_resolved, 0, sizeof s_resolved);
    s_resolved.max_frames = std::clamp(config.max_frames, 1, static_cast<int>(Report::kMaxFrames));

    char executable[kPathChars]{};
    if (!readExecutablePath(executable, sizeof executable)) {
        return false; // Without knowing where we live, a report would be written into the wrong directory.
    }

    const std::string_view executable_path{ executable };
    const std::string_view directory = directoryOf(executable_path);
    const std::string_view stem      = fileStem(executable_path);
    const std::string_view app       = config.app_name.empty() ? stem : std::string_view{ config.app_name.as_std_str() };

    copyUtf8(s_resolved.app, sizeof s_resolved.app, app);
    s_resolved.utc_offset_seconds = utcOffsetSeconds();

    for (const String& line : config.context) {
        if (s_resolved.context_count >= Report::kMaxContext) {
            break;
        }
        copyUtf8(s_resolved.context[s_resolved.context_count], Report::kContextChars, line.as_std_str());
        ++s_resolved.context_count;
    }

    if (!config.log_file.empty()) {
        copyUtf8(s_resolved.log_path, sizeof s_resolved.log_path, config.log_file.native());
    }
    else {
        const std::string log_name = std::string{ app } + "_crash.log";
        if (!joinPath(s_resolved.log_path, sizeof s_resolved.log_path, directory, log_name)) {
            return false;
        }
    }

    if (!config.handler_exe.empty()) {
        copyUtf8(s_resolved.handler_path, sizeof s_resolved.handler_path, config.handler_exe.native());
    }
    else if (!joinPath(s_resolved.handler_path, sizeof s_resolved.handler_path, directory, "CrashHandler")) {
        return false;
    }

    if (!config.mailto.empty()) {
        copyUtf8(s_resolved.mailto, sizeof s_resolved.mailto, config.mailto.as_std_str());
    }

    s_resolved.launch_handler = s_resolved.handler_path[0] != '\0' && ::access(s_resolved.handler_path, X_OK) == 0;

    struct sigaction action{};
    action.sa_sigaction = &onFatalSignal;
    action.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    ::sigemptyset(&action.sa_mask);

    std::size_t armed = 0;
    for (; armed < kSignalCount; ++armed) {
        if (::sigaction(kSignals[armed], &action, &s_previous[armed]) != 0) {
            break;
        }
    }
    if (armed < kSignalCount) {
        // Half an install is worse than none: the signals already taken are handed back before the failure is reported.
        while (armed > 0) {
            --armed;
            ::sigaction(kSignals[armed], &s_previous[armed], nullptr);
        }
        return false;
    }

    s_armed = true;
    return true;
}

void uninstallPlatform()
{
    if (s_armed) {
        for (std::size_t i = 0; i < kSignalCount; ++i) {
            ::sigaction(kSignals[i], &s_previous[i], nullptr);
        }
        s_armed = false;
    }
    std::memset(&s_previous, 0, sizeof s_previous);
    std::memset(&s_resolved, 0, sizeof s_resolved);
    s_handling = 0;
}

} // namespace detail
VN_CRASH_NS_END
