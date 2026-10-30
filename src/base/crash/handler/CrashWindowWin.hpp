#pragma once

#include <string>

namespace handler {

/**
 * @brief Shows the crash window and serves its four actions until the user closes it.
 *
 * @details
 * - The window only displays and exports: it does not re-run the application, does not modify the report and does not try to
 *   explain the crash. What it shows is the report the dead process left behind.
 * - Actions: copy the report, open the folder holding the dump, start a mail draft (only when @p mailto is non-empty), close.
 * - The dialog is closed and reopened after an action, because TaskDialog has no "and then show a confirmation" state; it
 *   costs one window redraw and keeps every action independent.
 *
 * @param app_name Application name used in the title and the body.
 * @param report_path Path of the raw report file the process wrote.
 * @param dump_path Path of the dump; empty when none was written.
 * @param report_text Human-readable report text the window shows expanded and copies on demand.
 * @param mailto Mail recipient; empty hides the mail action.
 */
void showWindow(const std::wstring& app_name,
                const std::wstring& report_path,
                const std::wstring& dump_path,
                const std::string&  report_text,
                const std::wstring& mailto);

} // namespace handler
