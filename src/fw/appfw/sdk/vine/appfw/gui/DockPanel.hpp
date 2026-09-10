#pragma once

#include <vine/raw_ptr.hpp>

#include "Control.hpp"
#include "Gui.hpp"

V_APPFWGUI_NS_BEGIN

class DockPanelManager; // forward-declare for friend access

class V_APPFW_API DockPanel : public Control {
    V_OBJECT_META_DECL

    friend class DockPanelManager;

  public:
    DockPanel();
    virtual ~DockPanel();

    void         setFeatures(DockFeatures f);
    DockFeatures features() const;

    void   setTitle(const String& t);
    String title() const;

    void   setId(const String& i);
    String id() const;

    /**
     * @brief Replaces the pane content.
     *
     * @param c Content to show, or nullptr to keep the current one.
     *
     * @note Ownership: the pane owns the widget it shows. The previous content's
     *       impl is deleted here (that is what the docking library expects when the
     *       client is swapped), so a caller that keeps the old UIElement must not own
     *       its impl. The pane's own destrutor takes the current content with it.
     */
    void       setContent(UIElement* c);
    raw_ptr<UIElement> content() const;

    // State queries
    bool      isFloating() const;
    bool      isPinned() const;
    bool      isCollapsed() const;
    bool      isTabbed() const;
    DockAreas dockArea() const;

    /**
     * @brief Floats or docks the pane.
     *
     * Floating detaches the pane from the dock tree (the space goes back to the
     * remaining panes) and keeps its on-screen position; docking back returns it to
     * the area it was in when it was floated.
     *
     * @param floating true to float, false to dock back.
     */
    void setFloating(bool floating);
    /**
     * @brief Turns the pane into an auto-hide strip button (real pin).
     */
    void pin();
    /**
     * @brief Restores a pinned pane into the dock tree.
     */
    void unpin();
    /**
     * @brief Hides the pane and gives its space back (real collapse).
     */
    void collapse();
    /**
     * @brief Docks a collapsed pane back into its remembered area.
     */
    void restore();

  protected:
    /// Override to intercept close. Return false to veto.
    virtual bool onClosing();

  private:
    void attach(UIObject* container); // only DockPanelManager may call

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
