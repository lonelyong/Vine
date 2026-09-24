#pragma once

#include "Control.hpp"
#include "Gui.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonGroup;

/**
 * @brief Ribbon tab (wraps SARibbonCategory): a container for a set of panels
 * (RibbonGroup).
 *
 * @note This header includes and exposes no Qt types.
 */
class VN_APPFW_API RibbonTab : public Control {
    VN_OBJECT_META_DECL

  public:
    RibbonTab();
    virtual ~RibbonTab();

  public:
    void   setTitle(const String& t);
    String title() const;

  public:
    void addGroup(RibbonGroup* g);
    void removeGroup(RibbonGroup* g);
    /// Number of groups.
    int numGroups() const;
    /// Gets the group by index (out of range returns nullptr).
    RibbonGroup* groupAt(int i) const;

  public:
    /// Sets the layout mode for all panels on this tab (three/two/single row).
    void                  setPanelLayoutMode(RibbonPanelLayoutMode m);
    RibbonPanelLayoutMode panelLayoutMode() const;
    /// Shows/hides the titles of all panels on this tab.
    void setPanelTitleVisible(bool on);
    bool panelTitleVisible() const;
    /// Button spacing for all panels on this tab.
    void setPanelSpacing(int n);
    int  panelSpacing() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
