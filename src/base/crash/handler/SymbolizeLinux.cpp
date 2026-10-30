#include "Symbolize.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ReportText.hpp"

namespace handler {
namespace {

constexpr const char* kAddr2line{ "addr2line" };

/// @brief Quotes @p text so a POSIX shell passes it through unchanged.
std::string shellQuote(const std::string& text)
{
    std::string quoted = "'";
    for (const char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        }
        else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

/// @brief What running the symbolizer produced.
struct CommandResult
{
    std::string output;        ///< Standard output, collected in full.
    bool        started{ false }; ///< Whether the command could be started at all.
    int         status{ -1 };  ///< Exit status, `-1` when the command did not run.
};

/**
 * @brief Runs @p command through the shell and collects its standard output.
 *
 * @details
 * - A separate process on purpose: reading DWARF is a library's job, and linking a DWARF reader into the handler would tie the
 *   crash report to one build of one library. `addr2line` ships with the toolchain that produced the binary.
 *
 * @param command Command line to run.
 * @return Its output and status.
 */
CommandResult runCommand(const std::string& command)
{
    CommandResult result;
    std::FILE*    pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    result.started = true;

    char buffer[512]{};
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
        result.output += buffer;
    }
    result.status = ::pclose(pipe);
    return result;
}

/// @brief Splits @p text into lines, dropping the line endings.
std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t              pos = 0;
    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        std::string       line    = text.substr(pos, newline == std::string::npos ? std::string::npos : newline - pos);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (newline == std::string::npos) {
            break;
        }
        pos = newline + 1;
    }
    return lines;
}

/// @brief Whether an `addr2line` location line carries a real file and line, rather than its `??:0` placeholder.
bool hasLocation(const std::string& location)
{
    if (location.size() < 2 || location.rfind("??", 0) == 0) {
        return false;
    }
    return location.compare(location.size() - 2, 2, ":0") != 0;
}

} // namespace

std::string symbolize(const vn::crash::Report& report)
{
    std::vector<FrameSymbol> symbols(report.frame_count);
    std::string              diagnostics;

    const std::size_t frame_count = std::min(report.frame_count, vn::crash::Report::kMaxFrames);

    for (std::size_t image_index = 0; image_index < report.image_count && image_index < vn::crash::Report::kMaxImages;
         ++image_index)
    {
        const vn::crash::ReportImage& image = report.images[image_index];
        if (image.path[0] == '\0') {
            continue;
        }

        // One tool run per image: addr2line takes the addresses as arguments, so the frames of one module cost one process.
        std::vector<std::size_t> frames;
        std::string              command = std::string{ kAddr2line } + " -C -f -e " + shellQuote(image.path);
        for (std::size_t i = 0; i < frame_count; ++i) {
            if (report.frames[i].image != static_cast<int>(image_index)) {
                continue;
            }
            char offset[32]{};
            std::snprintf(offset, sizeof offset, " 0x%llx", static_cast<unsigned long long>(report.frames[i].offset));
            command += offset;
            frames.push_back(i);
        }
        if (frames.empty()) {
            continue;
        }

        const CommandResult result = runCommand(command);
        if (!result.started) {
            diagnostics += "  addr2line 无法启动（binutils 是否已安装？）\r\n";
            break;
        }

        const std::vector<std::string> lines = splitLines(result.output);
        if (lines.size() < frames.size() * 2) {
            char buffer[400]{};
            std::snprintf(buffer,
                          sizeof buffer,
                          "  image %zu (%s) 只解析出 %zu/%zu 帧（addr2line 退出码 %d）\r\n",
                          image_index,
                          image.name[0] != '\0' ? image.name : "?",
                          lines.size() / 2,
                          frames.size(),
                          result.status);
            diagnostics += buffer;
        }

        for (std::size_t frame = 0; frame < frames.size() && frame * 2 + 1 < lines.size(); ++frame) {
            FrameSymbol& symbol = symbols[frames[frame]];
            if (lines[frame * 2] != "??") {
                symbol.name = lines[frame * 2];
            }
            if (hasLocation(lines[frame * 2 + 1])) {
                symbol.source = lines[frame * 2 + 1];
            }
        }
    }

    return composeText(report, symbols, diagnostics);
}

} // namespace handler
