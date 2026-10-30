#pragma once

#include "crash_global.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vine/String.hpp>

VN_CRASH_NS_BEGIN

/**
 * @brief One image (EXE or DLL) the crashed process had loaded.
 *
 * @details
 * - The out-of-process handler loads this image's symbols by @ref path, so a truncated path costs symbols, not the frame: the
 *   frame still reports `name+0xoffset`.
 * - Both strings are UTF-8 and truncated to the array size, never split mid-code-unit.
 */
struct ReportImage
{
    static constexpr std::size_t kNameChars{ 48 };
    static constexpr std::size_t kPathChars{ 256 };

    std::uintptr_t base{ 0 };
    char           name[kNameChars]{};
    char           path[kPathChars]{};
};

/**
 * @brief One stack entry of the crashing thread.
 *
 * @details
 * - @ref address is what the frame returned to; @ref offset is that address minus the image base, which is what survives a
 *   rebuilt binary and what the handler reports when symbols are missing.
 * - @ref image is an index into @ref Report::images, or -1 when the address falls outside every loaded image.
 */
struct ReportFrame
{
    std::uintptr_t address{ 0 };
    std::uintptr_t offset{ 0 };
    int            image{ -1 };
};

/**
 * @brief What a process can say about its own crash without symbols, locks or the heap.
 *
 * @details
 * - Fixed capacities on purpose: the crashing process fills this from static storage, so filling it never allocates and
 *   cannot fail. A deep stack is truncated, not refused.
 * - The struct is exactly the payload of the report file (see @ref renderReport); the out-of-process handler parses that
 *   file back into this shape, so one definition serves both directions.
 * - Trivially copyable, so the crash path may clear it with a `memset`-shaped assignment.
 *
 * @warning
 * - Never grow this struct per-crash with heap members: it is written by a process whose heap and locks are suspect.
 */
struct Report
{
    static constexpr int         kVersion{ 1 };
    static constexpr const char* kMagic{ "vn-crash-report" };

    static constexpr std::size_t kMaxImages{ 24 };
    static constexpr std::size_t kMaxFrames{ 64 };
    static constexpr std::size_t kMaxContext{ 8 };
    static constexpr std::size_t kAppChars{ 48 };
    static constexpr std::size_t kTimeChars{ 24 };
    static constexpr std::size_t kContextChars{ 200 };

    char        app[kAppChars]{};
    char        time[kTimeChars]{}; ///< "2026-10-30T14:32:07", local time
    char        dump[ReportImage::kPathChars]{}; ///< where the dump was asked to go; empty = none requested
    char        context[kMaxContext][kContextChars]{}; ///< header lines the app chose to identify itself
    std::size_t context_count{ 0 };

    unsigned long  code{ 0 }; ///< the operating system's exception code (Windows: an NTSTATUS such as 0xC0000005)
    std::uintptr_t address{ 0 };
    unsigned long  thread_id{ 0 };

    ReportImage images[kMaxImages]{};
    std::size_t image_count{ 0 };

    ReportFrame frames[kMaxFrames]{};
    std::size_t frame_count{ 0 };
};

/**
 * @brief Install-time configuration; read once by @ref install and never again.
 *
 * @details
 * - Every empty path means "the module's default next to the executing file", so an application that wants the standard
 *   layout passes only @ref app_name and @ref context and touches no filesystem API of its own.
 * - The defaults are `<exe dir>/<app_name>_crash.log`, `<exe dir>/<app_name>_crash.dmp` and `<exe dir>/vnCrashHandler.exe`.
 *
 * @note
 * - A cross-process child is only launched when the handler file exists at install time; the check is done there so the
 *   crash path never tests for a file.
 */
struct Config
{
    String                app_name; ///< Which application is reporting; names the report, the dump and the window title.
    std::filesystem::path log_file; ///< empty = `<exe dir>/<app_name>_crash.log`
    std::vector<String>   context; ///< header lines written before the stack, e.g. "Vine 1.0.0 (Debug)"
    int                   max_frames{ static_cast<int>(Report::kMaxFrames) }; ///< truncated to [1, Report::kMaxFrames]
    std::filesystem::path dump_file; ///< empty = `<exe dir>/<app_name>_crash.dmp`
    std::filesystem::path handler_exe; ///< empty = `<exe dir>/vnCrashHandler.exe`
    String                mailto; ///< Recipient for the handler window's mail action; empty hides that button.
};

/**
 * @brief Renders a report as report-file text, into a caller-provided buffer.
 *
 * @details
 * - Pure and allocation-free, so the crashing process can call it and a unit test can pin the format without crashing.
 * - Writing stops at the first line that does not fit in @p out, so the text is a prefix of the full report: no line is ever
 *   cut in half, and no frame is reported without the image table it refers to.
 * - Line endings are CRLF on every platform: the file is read by editors and pasted into issue trackers.
 *
 * @param report Report to render; its counters are trusted, so keep them within the arrays.
 * @param out Buffer receiving the text; the render never writes past its end.
 * @return Number of bytes written, always `<= out.size()`; zero when even the magic line does not fit.
 */
VN_CRASH_API std::size_t renderReport(const Report& report, std::span<char> out) noexcept;

/**
 * @brief Renders a report as report-file text.
 *
 * @details
 * - Convenience overload for callers that are not on a crash path (tests, tools); it grows the string as needed.
 *
 * @param report Report to render.
 * @return The report text; complete, because the buffer is sized from a first measuring pass.
 */
VN_CRASH_API std::string renderReport(const Report& report);

/**
 * @brief Parses report-file text back into a report.
 *
 * @details
 * - Unknown keys are ignored and missing keys keep their default, so a newer writer only loses fields an older reader does
 *   not know instead of failing the whole report.
 * - The magic line and its version are the one hard requirement: text that is not a report is rejected rather than
 *   misread.
 *
 * @param text Text produced by @ref renderReport; may be truncated, in which case the parsed tail is short.
 * @param out Receives the parsed report; untouched semantics are the defaults of @ref Report.
 * @return true when @p text starts with a report this version understands.
 */
VN_CRASH_API bool parseReport(std::string_view text, Report& out) noexcept;

/**
 * @brief Installs the process-wide crash handler: on an unhandled exception the process writes a report, writes a dump,
 * launches the visual handler and exits.
 *
 * @details
 * - Resolves @p config's empty paths, snapshots what the crash path will need (paths as wide characters, context lines,
 *   frame limit) into static storage, and arms the OS hook. Everything that allocates happens here, while the process is
 *   healthy.
 * - Only the *last-chance* filter is hooked, never a first-chance handler: an exception that a `catch` or a debugger
 *   handles is not a crash.
 * - The report is written before the dump, so a dump that cannot be written still leaves a readable report behind.
 *
 * @warning
 * - Installs process-wide state: a host that owns the fate of the process (a test runner, a plugin host) must call
 *   @ref uninstall before exiting, or its own report and exit code are replaced by ours.
 *
 * @param config Install-time configuration; copied, so the caller may destroy it immediately.
 * @return true when a handler was armed; false on a platform this version does not support yet.
 */
VN_CRASH_API bool install(const Config& config);

/**
 * @brief Removes the crash handler installed by @ref install.
 *
 * @details
 * - Restores the previous last-chance filter and clears the snapshot, so a later @ref install starts from a clean state.
 * - Safe to call when nothing is installed.
 */
VN_CRASH_API void uninstall();

VN_CRASH_NS_END
