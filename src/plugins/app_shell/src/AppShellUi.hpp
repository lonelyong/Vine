#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <vine/async/Task.hpp>

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
 * @brief Builds the app shell dock layout (left / central / right / bottom) and the demo's skeleton.
 *
 * Creates the side panels, the central render view and the bottom console, binds the console panel to the host's visual
 * user I/O, and starts the demo's content load on the pool. It does NOT bring the render session up: that is
 * initAppShellRenderControl(), so that the caller can hand the event loop a turn in between (see AppShellPlugin::load()).
 *
 * THE 3D VIEW'S CONTENT IS NOT IN YET. The demo's skeleton (scenes, camera, passes, pipeline) is built here, while its
 * TWO CUBE MAPS are left for later: reading and decoding twelve 2048^2 faces is 1.8 s of the app's start-up (measured
 * 2026-09-26), it is pure data, and doing it inline would freeze the loop (no repaint, no progress) for that long. It is
 * assembleDemoContentLater(), which the plugin starts from here: the shell's own work stays on the application thread -
 * widgets and graphics objects are not built anywhere else - and the heavy half goes to the pool.
 *
 * @param wnd Target main window.
 * @return The built dock layout (its render control still has to be initialized, its demo still has to get its content).
 */
AppShellDock buildAppShellDock(gui::MainWindow* wnd);

/**
 * @brief Brings the 3D view's render session up: attach, device and pipelines.
 *
 * Must run after buildAppShellDock() (the pipeline is built from the passes the demo's skeleton registered) and adjacent
 * to the main window going up (a session the window system never maps stays unattached - measured on X11).
 *
 * @param wnd  Main window that owns the dock layout.
 * @param dock Layout built by buildAppShellDock().
 */
vn::async::Task<void> initAppShellRenderControl(gui::MainWindow* wnd, const AppShellDock& dock);

VN_APPFW_NS_END
