#pragma once

#include <vine/Events.hpp>
#include <vine/String.hpp>

#include "Control.hpp"
#include "Gui.hpp"
#include "Icon.hpp"
#include "RibbonPlacement.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonTab;
class RibbonAction;
class MainWindow;
class Backstage;

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

    /**
     * @brief Adds a tab where the placement asks and resolves the bar order.
     *
     * @details
     * - The tab's id, role and policy are read when it is added; set them before the call.
     * - Anchors resolve against tabs registered earlier; a missing anchor falls back to the
     *   non-terminal block and reports PlacedByFallback.
     * - A non-terminal tab is never placed after a Terminal tab: such a request is clamped to
     *   the first terminal tab and reports ClampedBeforeTerminal.
     * - A Terminal tab itself joins the terminal suffix in registration order; a relative
     *   placement on it is ignored (the status is still Placed).
     * - A duplicate non-empty id is rejected (DuplicateId); adding the same object twice is a
     *   no-op (AlreadyAdded).
     *
     * @param tab       Tab to add; must not be null.
     * @param placement Where to place it relative to its siblings.
     * @return What happened and the final logical index (-1 when the tab was not added).
     */
    RibbonPlaceResult addTab(RibbonTab* tab, const RibbonPlacement& placement);

    /**
     * @brief Finds a tab by its stable id.
     *
     * @param id Tab id to look up; an empty id never matches.
     * @return The tab in logical order, or nullptr when no tab has that id.
     */
    RibbonTab* findTab(const String& id) const;

    /**
     * @brief Finds the first tab with the given role in logical order.
     *
     * @param role Role to look up; RibbonTabRole::None never matches.
     * @return The first matching tab, or nullptr when no tab has that role.
     */
    RibbonTab* findTab(RibbonTabRole role) const;

    /// Appends an application menu item (top-left File button).
    void appendApplicationMenu(RibbonAction* mi);

    /**
     * @brief Removes every item from the application button's menu.
     *
     * @details
     * - Only the menu entries are removed; the RibbonAction objects stay owned by their creator.
     * - The menu itself stays attached to the application button (see setBackstage() for the
     *   menu/backstage choice).
     */
    void clearApplicationMenu();

    /**
     * @brief Makes the application button open a Backstage instead of the application menu.
     *
     * @details
     * - With a Backstage, the button's menu is detached, so the two entry points can never show
     *   at the same time; passing nullptr re-attaches the application menu.
     * - The bar only borrows the Backstage: MainWindow::setBackstage() owns it.
     * - The click itself is still reported through applicationButtonClicked().
     *
     * @param backstage Backstage to open, or nullptr to restore the menu mode.
     */
    void setBackstage(Backstage* backstage);

    /**
     * @brief Returns the Backstage the application button opens.
     *
     * @return The Backstage, or nullptr while the application menu mode is active.
     */
    Backstage* backstage() const;

    /**
     * @brief Fired when the application button is clicked.
     *
     * @details
     * - Fired before the menu or Backstage opens, so a host can add its own response.
     * - The connection is RAII: keep the returned vn::Connection alive to stay subscribed.
     *
     * @return The event to connect to.
     */
    Event<RibbonBar, EventArgs>& applicationButtonClicked();
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

    /**
     * @brief Recomputes the logical tab order and mirrors it into the SARibbon categories.
     *
     * @param keep_current Tab to keep selected after the reorder, or nullptr to leave the
     *                     current selection to SARibbon.
     */
    void layoutTabs(RibbonTab* keep_current);

    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
