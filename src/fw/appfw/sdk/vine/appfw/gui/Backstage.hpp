#pragma once

#include <vine/Events.hpp>

#include "BackstagePlacement.hpp"
#include "Control.hpp"
#include "Gui.hpp"
#include "Icon.hpp"

VN_APPFWGUI_NS_BEGIN

class MainWindow;
class RibbonAction;

/**
 * @brief One page of the Backstage: a navigation entry plus a list of command items.
 *
 * @details
 * - Set the id, title and icon before adding the page to a Backstage; the Backstage reads them
 *   when the page is added.
 * - The page owns neither the page widget after it was added (the Backstage's stack owns it) nor
 *   the items: a RibbonAction stays owned by its creator.
 * - A page body is one thing: either the command items addItem() builds, or one Control installed with
 *   setBody(). The two cannot be mixed.
 *
 * @note Page ids are ASCII and are not localized; an empty id is allowed but cannot be looked up.
 */
class VN_APPFW_API BackstagePage : public Control {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Creates an empty page; set id, title and icon before adding it to a Backstage.
     */
    BackstagePage();
    ~BackstagePage() override;

  public:
    /**
     * @brief Sets the stable id used for lookups and duplicate checks.
     *
     * @param id Unique id within the owning Backstage, or empty for none.
     */
    void setId(const String& id);

    /**
     * @brief Returns the stable id of the page.
     *
     * @return The id, or an empty string when none was set.
     */
    String id() const;

    /**
     * @brief Sets the navigation title of the page.
     *
     * @param title Title shown in the Backstage navigation.
     */
    void setTitle(const String& title);

    /**
     * @brief Returns the navigation title of the page.
     *
     * @return The title.
     */
    String title() const;

    /**
     * @brief Sets the navigation icon of the page (optional).
     *
     * @param icon Icon shown next to the title.
     */
    void setIcon(const Icon& icon);

    /**
     * @brief Returns the navigation icon of the page.
     *
     * @return The icon; a null icon when none was set.
     */
    Icon icon() const;

    /**
     * @brief Sets the semantic role the page is anchored by.
     *
     * @details
     * - The role is what another contributor anchors on (BackstagePlacement::before/after(role)), and it orders
     *   appended role-bearing pages among themselves; the page id stays the identity.
     * - Set it before addPage(), like the id, title and icon.
     *
     * @param role Role of the page, or BackstagePageRole::None for "reachable by id only".
     */
    void setRole(BackstagePageRole role);

    /**
     * @brief Returns the semantic role of the page.
     *
     * @return The role, or BackstagePageRole::None when none was set.
     */
    BackstagePageRole role() const;

    /**
     * @brief Sets the placement policy that keeps the page in the terminal suffix.
     *
     * @details
     * - A Terminal page is always last: no other page is placed after it, and a request that points past it is
     *   clamped before it. The standard catalog marks the Close page this way.
     * - Set it before addPage(), like the id and role.
     *
     * @param policy Policy flags of the page.
     */
    void setPolicy(BackstagePagePolicy policy);

    /**
     * @brief Returns the placement policy of the page.
     *
     * @return The policy flags; BackstagePagePolicy::None by default.
     */
    BackstagePagePolicy policy() const;

    /**
     * @brief Sets who contributes this page (a plugin or host id) so it can be withdrawn as a group.
     *
     * @details
     * - The provider is what Backstage::removePages() matches on: a plugin that unloads or reloads drops its own
     *   pages instead of leaving them pointing at actions it is about to destroy.
     * - It is a label for grouping and diagnostics, not a permission: it does not stop anyone from touching the page.
     * - Set it before addPage(), like the id and role. An empty provider never matches removePages().
     *
     * @param provider Contributor id, or empty for "no provider".
     */
    void setProvider(const String& provider);

    /**
     * @brief Returns the contributor id of the page.
     *
     * @return The provider, or an empty string when none was set.
     */
    String provider() const;

    /**
     * @brief Appends a command item to the page.
     *
     * @details
     * - The item is shown as a flat button; clicking it triggers the item's QAction, so a
     *   configured command runs exactly like a ribbon button.
     * - The item is not owned by the page: it must outlive the page's widget.
     * - One entry per action: adding the same action again is ignored, which keeps the action a
     *   usable key for setItemVisible()/isItemVisible().
     *
     * @param item Item to append; null is ignored.
     */
    void addItem(RibbonAction* item);

    /**
     * @brief Returns the number of command items, hidden ones included.
     *
     * @return The item count.
     */
    int numItems() const;

    /**
     * @brief Returns the command item at the given position.
     *
     * @param index Zero-based index; out of range returns nullptr.
     * @return The item, or nullptr.
     */
    RibbonAction* itemAt(int index) const;

    /**
     * @brief Shows or hides one command item.
     *
     * @details
     * - Hiding is how a Backstage entry is turned off: the item keeps its position, its action and the command behind
     *   it, but its button no longer takes part in the layout. This is different from a disabled command, which stays
     *   listed and greyed out (CommandManager::setCommandEnabled()).
     * - The item must already be on this page (addItem()); an action the page does not hold is ignored.
     * - Indices stay stable: numItems()/itemAt() keep reporting hidden items, so the same action can show it again.
     *
     * @param item    Item to show or hide; null is ignored.
     * @param visible true to show it, false to hide it.
     */
    void setItemVisible(RibbonAction* item, bool visible);

    /**
     * @brief Reports whether an item is currently shown.
     *
     * @param item Item to query.
     * @return true when the item is on this page and visible; false when it is hidden or unknown.
     */
    bool isItemVisible(const RibbonAction* item) const;

    /**
     * @brief Replaces the page's command column with one control.
     *
     * @details
     * - A page body is exactly one thing: the command column addItem() builds (the default), or this control.
     *   Installing a body after items were added is rejected, and addItem() after a body is rejected too; the two
     *   never share a page.
     * - The body widget is reparented into the page and owned by the page from then on: the Control wrapper must
     *   not delete it (it is set to not own its impl) and must outlive the page widget.
     * - The body fills the page area inside the page's standard margins; the page title stays in the Backstage header.
     * - Installing another body hands the previous one back to its owner, like MainWindow::setBackstage() does with
     *   the Backstage it replaces.
     *
     * @param body Control to show as the page body; null is rejected.
     * @return true when the body was installed (also true when @p body is already the installed one).
     */
    bool setBody(Control* body);

    /**
     * @brief Returns the installed page body.
     *
     * @return The body control, or nullptr while the page shows its command column.
     */
    Control* body() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

/**
 * @brief Full-window Backstage opened by the ribbon's application button.
 *
 * @details
 * - The native widget is a top-level tool window that follows the main window's client area:
 *   a child overlay would be covered by the native 3D surface (see the docking flyout bug).
 * - Pages are a flat list in insertion order; the first page becomes the current one.
 * - The Backstage is owned by MainWindow::setBackstage(); the window itself owns the page
 *   widgets after addPage().
 *
 * @note The application button opens either this page or the application menu, never both
 *   (RibbonBar::setBackstage() detaches the menu).
 */
class VN_APPFW_API Backstage : public Control {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Creates a Backstage that follows the given main window.
     *
     * @param window Main window to follow and return to on close; must outlive the Backstage.
     */
    explicit Backstage(MainWindow* window);
    ~Backstage() override;

  public:
    /**
     * @brief Appends a page to the navigation.
     *
     * @details
     * - The page widget is reparented into the Backstage's stack, which owns it from then on.
     * - A duplicate non-empty page id is rejected and logged.
     * - Equivalent to addPage(page, BackstagePlacement::append()).
     *
     * @note Kept for the "just append it" call site; a null page is rejected here instead of throwing, so the
     *   behaviour of existing callers does not change. The placement overload throws, like RibbonBar::addTab().
     *
     * @param page Page to append; null is rejected.
     * @return true when the page was added.
     */
    bool addPage(BackstagePage* page);

    /**
     * @brief Adds a page where the placement asks and resolves the navigation order.
     *
     * @details
     * - The page's id, title, icon, role and policy are read when it is added; set them before the call.
     * - Anchors resolve against pages registered earlier; a missing anchor falls back to the non-terminal block
     *   and reports PlacedByFallback.
     * - A non-terminal page is never placed after a Terminal page: such a request is clamped to the first
     *   terminal page and reports ClampedBeforeTerminal.
     * - A Terminal page itself joins the terminal suffix in registration order; a relative placement on it is
     *   ignored (the status is still Placed).
     * - A duplicate non-empty id is rejected (DuplicateId); adding the same object twice is a no-op (AlreadyAdded).
     * - The current page is kept across the reorder.
     *
     * @param page      Page to add; must not be null.
     * @param placement Where to place it relative to its siblings.
     * @return What happened and the final logical index (-1 when the page was not added).
     */
    BackstagePlaceResult addPage(BackstagePage* page, const BackstagePlacement& placement);

    /**
     * @brief Finds a page by its stable id.
     *
     * @param id Page id; an empty id never matches.
     * @return The page, or nullptr when no page has that id.
     */
    BackstagePage* findPage(const String& id) const;

    /**
     * @brief Finds the first page with the given role in logical order.
     *
     * @param role Role to look up; BackstagePageRole::None never matches.
     * @return The first matching page, or nullptr when no page has that role.
     */
    BackstagePage* findPage(BackstagePageRole role) const;

    /**
     * @brief Returns the number of pages.
     *
     * @return The page count.
     */
    int numPages() const;

    /**
     * @brief Returns the page at the given position.
     *
     * @param index Zero-based index; out of range returns nullptr.
     * @return The page, or nullptr.
     */
    BackstagePage* pageAt(int index) const;

    /**
     * @brief Selects a page, updating the navigation and the content stack.
     *
     * @param page Page to select; a page that is not part of this Backstage is ignored.
     */
    void setCurrentPage(BackstagePage* page);

    /**
     * @brief Returns the selected page.
     *
     * @return The current page, or nullptr when no page is selected (none added, or every page is hidden).
     */
    BackstagePage* currentPage() const;

    /**
     * @brief Shows or hides a page in the navigation and the content stack.
     *
     * @details
     * - Hiding takes the page out of the navigation and makes it unselectable; the page object and its items stay, so
     *   findPage()/pageAt()/numPages() keep reporting it. This is how a host turns a Backstage entry off without
     *   destroying the page it may need again.
     * - Hiding the current page selects the next visible page (wrapping around); when none is left the Backstage has
     *   no current page and open() refuses to show an empty Backstage.
     * - Showing a page again puts it back in the navigation; it becomes current when nothing is.
     * - A page that was not added to this Backstage is ignored.
     *
     * @param page    Page to show or hide; null is ignored.
     * @param visible true to show it, false to hide it.
     */
    void setPageVisible(BackstagePage* page, bool visible);

    /**
     * @brief Reports whether a page is currently shown.
     *
     * @param page Page to query.
     * @return true when the page belongs to this Backstage and is visible; false when it is hidden or unknown.
     */
    bool isPageVisible(const BackstagePage* page) const;

    /**
     * @brief Removes every page contributed by one provider.
     *
     * @details
     * - This is how a plugin withdraws its pages on unload or before a reload; the pages it never added are untouched.
     * - The page object stays owned by its creator and its widget goes back to being owned by the page (the
     *   Backstage took that ownership in addPage()), so the same page can be added again later.
     * - When the current page is among them, the page that followed it becomes current (wrapping to the first
     *   visible one); when nothing is left the Backstage has no current page.
     * - An empty provider matches nothing, so pages added without setProvider() cannot be removed this way.
     *
     * @param provider Contributor id to remove; empty is ignored.
     * @return The number of pages removed.
     */
    int removePages(const String& provider);

    /**
     * @brief Shows the Backstage over the main window and activates it.
     *
     * @note Does nothing when no page was added, or when every page is hidden: an empty Backstage is not shown.
     */
    void open();

    /**
     * @brief Hides the Backstage and returns activation to the main window.
     */
    void close();

    /**
     * @brief Whether the Backstage window is currently visible.
     *
     * @return true while open.
     */
    bool isOpen() const;

  public:
    /// Fired after the Backstage became visible.
    Event<Backstage, EventArgs> opened;
    /// Fired after the Backstage was hidden.
    Event<Backstage, EventArgs> closed;

  private:
    struct Impl;

    /**
     * @brief Recomputes the logical page order and mirrors it into the navigation and the content stack.
     *
     * @param keep_current Page to keep selected after the reorder, or nullptr to leave the selection alone.
     */
    void layoutPages(BackstagePage* keep_current);

    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
