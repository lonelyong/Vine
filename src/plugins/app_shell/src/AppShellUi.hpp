#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <vine/async/Task.hpp>

VN_APPFW_NS_BEGIN

namespace gui {
class MainWindow;
class ConsolePanel;
}

/**
 * @brief Builds the app shell Ribbon tabs and their command buttons.
 *
 * Each button is bound to a registered command name; adding an entry is a
 * single addCommandButton() call. Tabs and groups are created in a fixed order.
 *
 * @param wnd Target main window; its RibbonBar receives the tabs.
 */
void buildAppShellRibbon(gui::MainWindow* wnd);

/**
 * @brief Result of building the app shell dock layout.
 */
struct AppShellDock {
    /// Console panel created as the bottom dock (owned by the dock manager).
    gui::ConsolePanel* console_panel = nullptr;
};

/**
 * @brief Builds the app shell dock layout (left / right / bottom, plus the console).
 *
 * Creates the side panels and the bottom console and binds the console panel to the host's visual user I/O.
 *
 * THE CENTRAL AREA IS NOT THE SHELL'S ANY MORE (2026-09-27): the shell used to put a render control there and every
 * content plugin hung its scene on it. A 3D content is a DOCUMENT now, and a document brings its own view and its own
 * render surface (one document, one view, its own resources - see appfw-document-model.md §7/§9); the shell leaves the
 * central slot empty and the document host takes it over when a document is shown.
 *
 * @param wnd Target main window.
 * @return The built dock layout.
 */
AppShellDock buildAppShellDock(gui::MainWindow* wnd);

/**
 * @brief Opens the document named by the command line, if one was asked for.
 *
 * `Vine --open <type-id>` starts with that document shown. It goes through the SAME path the user's own command goes
 * through (DocumentManager::create) rather than through anything shell-specific, so a type that has no create factory
 * simply is not openable this way - and it must run after every plugin has loaded, because that is when the types are
 * registered. AppShellPlugin::postLoad() is exactly that point.
 *
 * @param wnd Main window whose document host shows it.
 */
void openDocumentFromCommandLine(gui::MainWindow* wnd);

VN_APPFW_NS_END
