#pragma once

#include <vector>

#include <vine/raw_ptr.hpp>

#include "Gui.hpp"
#include "Window.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonBar;
class StatusBar;
class DockPanelManager;
class Backstage;

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

    /**
     * @brief Installs the Backstage the application button opens and takes ownership of it.
     *
     * @details
     * - A previous Backstage is destroyed after the ribbon stops pointing at it.
     * - While a Backstage is installed, the application button's menu is detached
     *   (see RibbonBar::setBackstage()); nullptr restores the menu mode.
     *
     * @param backstage Backstage to own, or nullptr to remove the current one.
     */
    void setBackstage(Backstage* backstage);

    /**
     * @brief Returns the installed Backstage.
     *
     * @return The owned Backstage, or nullptr when none is installed.
     */
    Backstage* backstage() const;

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
