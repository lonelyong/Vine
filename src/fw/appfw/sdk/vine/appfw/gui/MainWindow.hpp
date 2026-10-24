#pragma once

#include <vector>

#include <vine/raw_ptr.hpp>

#include "Gui.hpp"
#include "Window.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonBar;
class StatusBar;
class DockPanelManager;

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

    // "Which 3D area?" is NOT answered here any more: a command reads it from its execution target
    // (gui::resolveCommandTarget() at dispatch, see CommandTarget.hpp), which keeps the answer per execution instead
    // of per window. The window still CREATES the document host that owns the views - it just does not republish what
    // they answer.

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
