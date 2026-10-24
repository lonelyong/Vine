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

    /** @brief Registers the control the SHELL itself shows when no document view is up.
     *
     * @details
     * - A document view is NOT registered here: the window answers with the shown document's view instead (see
     *   primaryRenderControl), so a view never has to publish itself into a window-wide slot.
     * - This setter is for the area the shell owns (the demo view, a host's own 3D page): it is the FALLBACK, used
     *   when the central widget is not a document host or the current document has no 3D view.
     *
     * @param control The shell's own render control, or nullptr to clear.
     */
    void setPrimaryRenderControl(RenderControl* control);

    /** @brief Gets the 3D area commands should act on: the shown document view's active pane, else the shell's own.
     *
     * @details
     * - DERIVED, not published: the answer is `central document host -> current view -> activeRenderControl()`,
     *   which is where "which pane" belongs (the view created it - see DocumentView::activeRenderControl). Only when
     *   there is no document view (or it has no 3D area) does the explicitly registered fallback answer.
     * - That makes the query single-valued even when several areas exist: a document view with two panes answers its
     *   active one, and a view that is not on screen cannot answer at all.
     *
     * @return The active render control, or nullptr when no 3D area is showing.
     */
    RenderControl* primaryRenderControl() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
