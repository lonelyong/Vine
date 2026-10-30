#pragma once

#include <string>

#include <vine/crash/CrashHandler.hpp>

namespace handler {

/**
 * @brief Turns a report into the human-readable text the window shows and the developer pastes.
 *
 * @details
 * - Every frame is resolved against the images the report names, loading their symbols by path: the handler is a separate,
 *   healthy process, so it may allocate and may touch DbgHelp freely.
 * - A frame whose symbols cannot be found keeps the information the crashing process had: `image+offset`, or the bare
 *   address when the frame falls outside every loaded image. Missing symbols cost detail, never a line.
 *
 * @param report Report parsed from the crashed process, including its image table.
 * @return UTF-8 text: identity, exception, dump path, context lines, then one line per frame.
 */
std::string symbolize(const vn::crash::Report& report);

} // namespace handler
