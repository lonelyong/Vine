#pragma once

#include <vine/appfw/appfw_global.hpp>

VN_APPFW_NS_BEGIN

namespace gui {
class MainWindow;
class ConsolePanel;
class RenderControl;
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
    /// Render view placed in the central client area (owned by the dock manager).
    gui::RenderControl* render_control = nullptr;
};

/**
 * @brief Builds the app shell dock layout (left / central / right / bottom).
 *
 * Creates the side panels and the bottom console, registers the console panel
 * with the GuiApplication, and returns the layout result.
 *
 * @param wnd Target main window.
 * @return The built dock layout.
 */
AppShellDock buildAppShellDock(gui::MainWindow* wnd);

VN_APPFW_NS_END
