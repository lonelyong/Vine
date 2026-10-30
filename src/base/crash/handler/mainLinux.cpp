#include <cstdio>
#include <cstdlib>
#include <string>

#include <vine/crash/CrashHandler.hpp>

#include "CrashWindowX11.hpp"
#include "Symbolize.hpp"
#include "Text.hpp"

namespace {

constexpr int kUnusableArguments{ 1 };

/// @brief Parsed command line of the handler.
struct Arguments
{
    std::string report_path;
    std::string dump_path;
    std::string app_name;
    std::string mailto;
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
Arguments parseArguments(int argc, char** argv)
{
    Arguments arguments;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view argument{ argv[i] };
        if (argument == "--report") {
            arguments.report_path = argv[i + 1];
        }
        else if (argument == "--dump") {
            arguments.dump_path = argv[i + 1];
        }
        else if (argument == "--app") {
            arguments.app_name = argv[i + 1];
        }
        else if (argument == "--mailto") {
            arguments.mailto = argv[i + 1];
        }
    }
    return arguments;
}

/**
 * @brief Whether a window can be attempted at all.
 *
 * @details
 * - A crash on a machine that is only reachable over ssh, or inside a service, has no display; the report text is printed
 *   instead, because losing the window must not lose the crash.
 *
 * @return true when an X display is configured.
 */
bool hasDisplay() noexcept
{
    const char* display = std::getenv("DISPLAY");
    return display != nullptr && display[0] != '\0';
}

} // namespace

/**
 * @brief Entry point of the visual crash handler: read the report, symbolize it, write the readable text, show the window.
 *
 * @details
 * - Runs as an ordinary process: the crashed process starts it with `--report`, so the handler inherits the environment
 *   (including the display) but none of the crashed process' state.
 * - The `.txt` is written before the window opens: a machine without a display still gets the file, and the text is printed
 *   to standard output instead of being shown.
 *
 * @param argc Argument count.
 * @param argv Argument vector, starting with this executable.
 * @return @ref kUnusableArguments when there is no report to show at all, and 0 when the report was presented.
 */
int main(int argc, char** argv)
{
    const Arguments arguments = parseArguments(argc, argv);
    if (arguments.report_path.empty()) {
        return kUnusableArguments;
    }

    const std::string raw = handler::readFile(arguments.report_path);

    vn::crash::Report report;
    std::string       text;
    std::string       app_name  = arguments.app_name;
    std::string       dump_path = arguments.dump_path;

    if (!raw.empty() && vn::crash::parseReport(raw, report)) {
        text = handler::symbolize(report);
        if (app_name.empty()) {
            app_name = report.app;
        }
        if (dump_path.empty()) {
            dump_path = report.dump;
        }
    }
    else {
        // An unreadable or unknown report is still worth showing: the raw addresses it does contain are exactly what a
        // developer asked for when this window appeared.
        text = raw;
    }
    if (app_name.empty()) {
        app_name = "应用";
    }
    if (!handler::fileExists(dump_path)) {
        dump_path.clear();
    }

    // Next to the report, so the two files a user has to hand over stay together.
    handler::writeFile(handler::replaceExtension(arguments.report_path, ".txt"), text);

    if (hasDisplay() && handler::showWindow(app_name, arguments.report_path, dump_path, text, arguments.mailto)) {
        return 0;
    }

    // No display, no Xft font, or the window could not be created: the text is still the answer, so print it.
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}
