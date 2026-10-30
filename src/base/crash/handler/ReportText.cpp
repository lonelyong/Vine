#include "ReportText.hpp"

#include <cstdio>

namespace handler {
namespace {

/// @brief Formats an address the way the report file does: `0x` followed by sixteen hex digits.
std::string hex(std::uintptr_t value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof buffer, "0x%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

/**
 * @brief Formats one frame as a line, falling back to the report's own image and offset when no symbol was resolved.
 *
 * @param report Report the frame came from.
 * @param index Position of the frame in the report, shown in front of it.
 * @param symbol Resolved names for that frame; may be absent.
 * @return The frame's line, without a line ending.
 */
std::string frameLine(const vn::crash::Report& report, std::size_t index, const FrameSymbol* symbol)
{
    const vn::crash::ReportFrame& frame = report.frames[index];

    std::string name   = symbol != nullptr ? symbol->name : std::string{};
    std::string source = symbol != nullptr ? symbol->source : std::string{};

    if (name.empty()) {
        // No symbols: the image and the offset are still exact, and the offset is what matches a rebuilt binary.
        if (frame.image >= 0 && static_cast<std::size_t>(frame.image) < report.image_count) {
            const vn::crash::ReportImage& image = report.images[frame.image];
            char                          offset[32]{};
            std::snprintf(offset, sizeof offset, "+0x%llx", static_cast<unsigned long long>(frame.offset));
            name = std::string{ image.name[0] != '\0' ? image.name : "?" } + offset;
        }
        else {
            name = "<未知模块>";
        }
    }

    std::string text = "  " + std::to_string(index) + "  " + hex(frame.address) + "  " + name;
    if (!source.empty()) {
        text += "  [" + source + "]";
    }
    return text;
}

} // namespace

std::string composeText(const vn::crash::Report&       report,
                        const std::vector<FrameSymbol>& symbols,
                        const std::string&              diagnostics)
{
    std::string text;
    text += "崩溃报告\r\n";
    text += "========\r\n\r\n";

    if (report.app[0] != '\0') {
        text += "应用: ";
        text += report.app;
        text += "\r\n";
    }
    if (report.time[0] != '\0') {
        text += "时间: ";
        text += report.time;
        text += "\r\n";
    }
    {
        char buffer[160]{};
        std::snprintf(buffer,
                      sizeof buffer,
                      "异常: 0x%08lx 于 %s 线程 %lu\r\n",
                      report.code,
                      hex(report.address).c_str(),
                      report.thread_id);
        text += buffer;
    }
    if (report.dump[0] != '\0') {
        text += "转储: ";
        text += report.dump;
        text += "\r\n";
    }

    if (report.context_count > 0) {
        text += "\r\n上下文:\r\n";
        for (std::size_t i = 0; i < report.context_count && i < vn::crash::Report::kMaxContext; ++i) {
            text += "  ";
            text += report.context[i];
            text += "\r\n";
        }
    }

    text += "\r\n调用栈:\r\n";
    for (std::size_t i = 0; i < report.frame_count && i < vn::crash::Report::kMaxFrames; ++i) {
        text += frameLine(report, i, i < symbols.size() ? &symbols[i] : nullptr);
        text += "\r\n";
    }

    if (!diagnostics.empty()) {
        text += "\r\n符号:\r\n";
        text += "（本段说明为什么有些帧只有模块+偏移而没有函数名）\r\n";
        text += diagnostics;
    }
    return text;
}

} // namespace handler
