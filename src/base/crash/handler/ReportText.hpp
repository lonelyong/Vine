#pragma once

#include <string>
#include <vector>

#include <vine/crash/CrashHandler.hpp>

namespace handler {

/**
 * @brief One stack entry as a symbolizer resolved it.
 *
 * @details
 * - Resolution and layout are separate on purpose: each platform resolves a frame with its own tool (DbgHelp on Windows,
 *   `addr2line` on Linux) and this header's composer turns the results into the one text a user pastes into a bug report.
 * - A frame the platform could not resolve leaves @ref name empty; composition then falls back to `image+0xoffset`, which is
 *   the information the crashing process had and what a rebuilt binary can still be matched against.
 *
 * @note
 * - @ref name is written as part of a single line, so it must not carry a line ending of its own.
 */
struct FrameSymbol
{
    std::string name;   ///< Function name, optionally with a `+0xdisplacement` suffix; empty when unresolved.
    std::string source; ///< `file:line` of the frame; empty when the symbol has no line information.
};

/**
 * @brief Composes the readable report the handler window shows and writes next to the raw report.
 *
 * @details
 * - Identity, exception, dump and context lines first, then one line per frame, then the symbolizer's diagnostics: a reader
 *   must be able to tell "this frame has no symbol" from "no symbols were available at all", which are different problems.
 * - Line endings are CRLF on every platform, like the report file itself: the text is written next to it and pasted into
 *   issue trackers.
 *
 * @param report Report parsed from the crashed process; supplies the header fields and the image table.
 * @param symbols Resolved frames, aligned with `report.frames` by index; shorter than the frame list when nothing was
 *        resolved, longer entries are ignored.
 * @param diagnostics Why some frames are unresolved; appended as its own section when non-empty.
 * @return UTF-8 text, complete for every frame the report holds.
 */
std::string composeText(const vn::crash::Report& report,
                        const std::vector<FrameSymbol>& symbols,
                        const std::string&              diagnostics);

} // namespace handler
