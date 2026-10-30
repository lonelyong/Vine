#include <string>

// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#pragma comment(lib, "shell32.lib")

#include <vine/crash/CrashHandler.hpp>

#include "CrashWindow.hpp"
#include "Symbolize.hpp"
#include "Text.hpp"

namespace {

constexpr int kUnusableArguments{ 1 };

/// @brief Parsed command line of the handler.
struct Arguments
{
    std::wstring report_path;
    std::wstring dump_path;
    std::wstring app_name;
    std::wstring mailto;
};

/**
 * @brief Reads `--report`, `--dump`, `--app` and `--mailto` out of a command line.
 *
 * @details
 * - Unknown switches are ignored on purpose: the crashed process may be a newer build that passes an argument this handler
 *   does not know, and refusing to show a crash over one unknown word would be the worse failure.
 *
 * @param argc Argument count.
 * @param argv Argument vector, starting with this executable.
 * @return The parsed arguments; @ref Arguments::report_path is empty when it was not given.
 */
Arguments parseArguments(int argc, wchar_t** argv)
{
    Arguments arguments;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring argument = argv[i];
        if (argument == L"--report") {
            arguments.report_path = argv[i + 1];
        }
        else if (argument == L"--dump") {
            arguments.dump_path = argv[i + 1];
        }
        else if (argument == L"--app") {
            arguments.app_name = argv[i + 1];
        }
        else if (argument == L"--mailto") {
            arguments.mailto = argv[i + 1];
        }
    }
    return arguments;
}

} // namespace

/**
 * @brief Entry point of the visual crash handler: read the report, symbolize it, write the readable text, show the window.
 *
 * @details
 * - A windowed process (`wWinMain`, not `wmain`): the handler's whole output is its dialog, and a console window flashing
 *   over the crash would only look like a second failure.
 *
 * @param instance Module handle the system passed; unused.
 * @param previous_instance Handle of the previous instance; unused.
 * @param command_line The raw command line; re-parsed with `CommandLineToArgvW` so paths keep the system's quoting rules.
 * @param show_command How the system suggests the first window should be shown; unused.
 * @return @ref kUnusableArguments when there is no report to show at all, and 0 when a window was shown (whatever the user
 *         then did with it).
 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance, LPWSTR command_line, int show_command)
{
    static_cast<void>(instance);
    static_cast<void>(previous_instance);
    static_cast<void>(command_line);
    static_cast<void>(show_command);

    int             argc = 0;
    wchar_t**       argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const Arguments arguments = argv != nullptr ? parseArguments(argc, argv) : Arguments{};
    if (argv != nullptr) {
        LocalFree(argv);
    }
    if (arguments.report_path.empty()) {
        return kUnusableArguments;
    }

    const std::string raw = handler::readFile(arguments.report_path);

    vn::crash::Report report;
    std::string       text;
    std::wstring      app_name  = arguments.app_name;
    std::wstring      dump_path = arguments.dump_path;

    if (!raw.empty() && vn::crash::parseReport(raw, report)) {
        text = handler::symbolize(report);
        if (app_name.empty()) {
            app_name = handler::toWide(report.app);
        }
        if (dump_path.empty()) {
            dump_path = handler::toWide(report.dump);
        }
    }
    else {
        // An unreadable or unknown report is still worth showing: the raw addresses it does contain are exactly what a
        // developer asked for when this window appeared.
        text = raw;
    }
    if (app_name.empty()) {
        app_name = L"应用";
    }
    if (!handler::fileExists(dump_path)) {
        dump_path.clear();
    }

    // Next to the report, so the two files a user has to hand over stay together.
    handler::writeFile(handler::replaceExtension(arguments.report_path, L".txt"), text);

    handler::showWindow(app_name, arguments.report_path, dump_path, text, arguments.mailto);
    return 0;
}
