#pragma once

#include "Control.hpp"
#include "Gui.hpp"
#include "Icon.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonTab;
class RibbonAction;
class MainWindow;

/**
 * @brief Ribbon bar (wraps SARibbonBar): manages tabs and the application menu.
 *
 * @note This header includes and exposes no Qt types.
 */
class VN_APPFW_API RibbonBar : public Control {
    VN_OBJECT_META_DECL

    friend class MainWindow;

  public:
    RibbonBar(MainWindow* wnd);
    virtual ~RibbonBar();

  public:
    int        numTabs() const;
    RibbonTab* tabAt(int idx) const;
    void       addTab(RibbonTab* tab);
    void       removeTab(RibbonTab* tab);
    int        currentIndex() const;
    void       setCurrentIndex(int idx);

    /// Appends an application menu item (top-left File button).
    void appendApplicationMenu(RibbonAction* mi);
    /// Whether the application button is visible.
    void setApplicationButtonVisible(bool on);
    bool applicationButtonVisible() const;
    /// Application button icon.
    void setApplicationIcon(const Icon& ic);
    Icon applicationIcon() const;
    /// Application button text.
    void   setApplicationText(const String& t);
    String applicationText() const;

  public:
    /// Appends a quick access action (left of the title bar; its QAction is
    /// owned by the quick access bar).
    void addQuickAccessItem(RibbonAction* item);
    /// Inserts a separator in the quick access bar.
    void addQuickAccessSeparator();
    /// Whether the quick access bar is visible.
    void setQuickAccessVisible(bool on);
    bool quickAccessVisible() const;

  public:
    /// Sets the Ribbon global style (row count x loose/compact).
    void        setRibbonStyle(RibbonStyle s);
    RibbonStyle ribbonStyle() const;
    /// Collapses/expands the Ribbon (minimized mode).
    void setMinimumMode(bool on);
    bool minimumMode() const;
    /// Globally shows/hides all panel titles.
    void setPanelTitleVisible(bool on);
    bool panelTitleVisible() const;
    /**
     * @brief Global batch setting: auto-wrap button text.
     *
     * @note Applies only to RibbonButton (SARibbonToolButton); plain controls
     * added via addControl are unaffected. Cascades to all panels/buttons and
     * overrides group-level/button-level wordWrap.
     */
    void setWordWrap(bool on);
    bool wordWrap() const;
    /**
     * @brief Global batch setting: place text to the right of the icon.
     *
     * @note Applies only to RibbonButton (SARibbonToolButton); plain controls
     * added via addControl are unaffected. Cascades to all panels/buttons and
     * overrides group-level/button-level iconRightText.
     */
    void setIconRightText(bool on);
    bool iconRightText() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
