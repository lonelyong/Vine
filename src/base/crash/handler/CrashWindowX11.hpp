#pragma once

#include <string>

namespace handler {

/**
 * @brief Shows the crash window and serves its actions until the user closes it.
 *
 * @details
 * - Same contract as the Windows dialog: the window only displays and exports. It does not re-run the application, does not
 *   modify the report and does not try to explain the crash.
 * - X11 only, with Xft for text: the window is the whole UI of a base-layer process, so it uses the two libraries the
 *   platform already ships instead of a UI toolkit (see ADR-2026-10-30-crash-handler-x11-on-linux.md).
 * - Actions: copy the report, open the folder holding the report, start a mail draft (only when @p mailto is non-empty),
 *   close. The window stays open after an action runs.
 *
 * @warning
 * - Needs a reachable X server; a Wayland-only session without XWayland has none.
 *
 * @param app_name Application name used in the title and the body.
 * @param report_path Path of the raw report file the process wrote.
 * @param dump_path Path of the dump; empty when none was written (Linux writes no dump).
 * @param report_text Human-readable report text the window shows and copies on demand.
 * @param mailto Mail recipient; empty hides the mail action.
 * @return true when a window was opened and closed by the user, false when no window could be created (no display, no Xft
 *         font); the caller then still has the text and the `.txt` file.
 */
bool showWindow(const std::string& app_name,
                const std::string& report_path,
                const std::string& dump_path,
                const std::string& report_text,
                const std::string& mailto);

} // namespace handler
