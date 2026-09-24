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

    /** @brief Registers the central render view control.
     *
     * The app shell sets the single 3D view it hosts here so other plugins
     * (e.g. tests, editors) can reach the render engine/scene without
     * depending on app shell internals.
     *
     * @param control Render control, or nullptr to clear.
     */
    void setPrimaryRenderControl(RenderControl* control);

    /** @brief Gets the registered central render view control.
     *
     * @return The render control, or nullptr when none was registered.
     */
    RenderControl* primaryRenderControl() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
