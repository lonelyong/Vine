#pragma once

#include <vine/raw_ptr.hpp>
#include <vine/String.hpp>
#include <vine/appfw/appfw_global.hpp>

#include <memory>
#include <vector>

#include "Gui.hpp"

V_APPFWGUI_NS_BEGIN

class DockPanel;
class MainWindow;
class UIElement;

/**
 * @brief Manages the lifecycle and organization of DockPanel instances.
 *
 * Owns an internal DockingPaneManager and provides factory methods to create
 * dock panels, tracks all managed panels, and supports lookup by id or title.
 */
class V_APPFW_API DockPanelManager final {

  public:
    DockPanelManager();
    ~DockPanelManager();

    /** Set the target window and initialize the docking manager.
     *  Must be called once before any docking operation. */
    void setWindow(UIElement* wnd);

    /** Set the central working-area widget (replaces the default placeholder) */
    void setCentralWidget(UIElement* widget);

    /** Get the root widget to embed in a window's central area */
    raw_ptr<UIElement> root() const;

    /** Create and dock a DockPanel with title and content at the given area. */
    DockPanel* createDockPanel(const String& title, DockAreas area);

    /** Create and dock a DockPanel with title, content widget, and area. */
    DockPanel* createDockPanel(const String& title, UIElement* content, DockAreas area);

    /** Add and dock a DockPanel at the given area (area is always required). */
    void addDockPanel(DockPanel* panel, DockAreas area);

    /**
     * @brief Removes a dock panel and deletes its wrapper.
     *
     * The pane is closed first, so DockPanel::onClosing() may veto: when it does,
     * nothing is removed and the panel stays valid. Otherwise the library takes over
     * the container (and the content inside it), the wrapper is deleted, and every
     * pointer returned earlier by panels()/findById()/findByTitle() is invalid.
     *
     * @param panel Panel to remove; nullptr is ignored.
     */
    void removeDockPanel(DockPanel* panel);

    /** Find a DockPanel by its unique id */
    DockPanel* findById(const String& id) const;

    /** Find a DockPanel by its display title */
    DockPanel* findByTitle(const String& title) const;

    /** Get the number of managed dock panels */
    int count() const;

    /** Get all managed dock panels */
    std::vector<DockPanel*> panels() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFWGUI_NS_END
