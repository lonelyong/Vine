#pragma once

#include <vine/Events.hpp>

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
     *
     * @param page Page to append; null is rejected.
     * @return true when the page was added.
     */
    bool addPage(BackstagePage* page);

    /**
     * @brief Finds a page by its stable id.
     *
     * @param id Page id; an empty id never matches.
     * @return The page, or nullptr when no page has that id.
     */
    BackstagePage* findPage(const String& id) const;

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
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
