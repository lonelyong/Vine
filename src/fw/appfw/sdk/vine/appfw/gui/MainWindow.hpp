#pragma once

#include <vector>

#include <vine/raw_ptr.hpp>

#include "Gui.hpp"
#include "Window.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonBar;
class StatusBar;
class DockPanelManager;
class RenderControl;

class VN_APPFW_API MainWindow : public Window {
    VN_OBJECT_META_DECL

    friend class RibbonBar;
    friend class StatusBar;

  public:
    MainWindow();
    ~MainWindow() override;

  public:
    /**
     * @brief Returns the currently shown main window, or nullptr if none.
     *
     * @return The main window instance.
     */
    static MainWindow* current();

  public:
    raw_ptr<RibbonBar>        ribbonBar() const;
    raw_ptr<StatusBar>        statusBar() const;
    raw_ptr<DockPanelManager> dockPanelManager() const;

    /** @brief Returns the 3D area a command should act on, or nullptr when there is none.
     *
     * @details
     * - DERIVED, not published: `central document host -> current view -> DocumentView::renderControl()`.
     *   "Which pane" is the view's answer, because it is the view that created the panes; the window only asks the
     *   view that is on screen, so an off-screen view is never consulted and the answer stays single-valued even
     *   when several areas exist.
     * - NO ACTIVE DOCUMENT, NO TARGET: when the central area is not a document host, no view is shown, or the shown
     *   view has no 3D area, the answer is nullptr. There is no "the shell's own 3D page" fallback: the central slot
     *   is where the document host installs, and every 3D area in an application belongs to a view.
     * - A command that needs this answer treats nullptr as "nothing to act on" and says so - it is the normal state
     *   of a window with no document open, not a failure.
     *
     * @return The active render control of the shown view, or nullptr when no view with a 3D area is on screen.
     */
    RenderControl* primaryRenderControl() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
