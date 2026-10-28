#pragma once

#include <vine/appfw/gui/Control.hpp>

#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>

VN_APPFW_NS_BEGIN
class Document;
class DocumentManager;
VN_APPFW_NS_END

VN_APPFWGUI_NS_BEGIN

class DocumentView;
class DocumentViewRegistry;
class DockPanelManager;

/**
 * @brief The central client area as "the current document is what the middle of the window shows" - the GUI host's
 *        default behaviour.
 *
 * The framework owns this, not every application: seeing the current document is what a document GUI does, and each
 * host writing its own version means each host getting the same things subtly wrong (which view, when to build it,
 * when to drop it, and what to say when there is nothing to show). What stays with the application is POLICY: whether
 * opening a document also selects it, whether tabs are shown, how many documents may be open - all of which it does
 * with `DocumentManager` and `opened`/`currentChanged`.
 *
 * ONE TRUTH, DERIVED ONCE: `DocumentManager::current()` decides which document is current; this host never keeps a
 * second answer. Everything it shows is derived from it in one place (syncToCurrentDocument()):
 *   - the tab strip selects the current document's tab;
 *   - the central area shows the current document's page, or a message page that says why it cannot (no current
 *     document, the current document is floating in its own window, or its type has no view);
 *   - the current document's view is the one whose activate() is in effect (see DocumentView).
 * Because the central area never shows a document that is not current, the tab highlight, the visible page and the
 * command target (`resolveCommandTarget()`) always agree.
 *
 * EVERY GESTURE IS A REQUEST, NEVER A SECOND OWNER: clicking a tab calls DocumentManager::setCurrent(), activating a
 * floating document window calls the same, and the API call coming the other way (`setCurrent`) moves the tab and the
 * page. That is the whole loop (R7): the manager decides, the host follows.
 *
 * THE CONTRACTS, and each one is a bug somebody writes by hand otherwise:
 *   1. the central slot is filled ONCE. DockPanelManager::setCentralWidget() replaces what is there and the docking
 *      library reparents the widget, which recreates its native window - doing that per document switch is exactly the
 *      `Pending -> Presenting` dance a render control must not be dragged through. The host installs its container the
 *      first time it has something to show and then only switches pages inside it;
 *   2. it does not take the central area before it has to: until the first document is presented, whatever the shell put
 *      there (a demo scene, a welcome page) stays visible and untouched;
 *   3. ONE VIEW PER DOCUMENT INSTANCE, built lazily the first time that document is shown, kept while it is open
 *      (so its camera/selection/scroll survive switching away and back) and destroyed when it closes;
 *   4. a type with no registered view gets a page that SAYS SO, naming the type - never a blank area;
 *   5. switching between two documents of the same type switches pages and nothing else: no rebuild, no re-registration;
 *   6. OWNERSHIP is split on purpose: the host owns the view OBJECTS (destroyed when their document closes) and the
 *      container owns the page WIDGETS. A view handed to the host is therefore taken over with owns=false - a widget
 *      that dies with the container would otherwise run UIElement's self-destruct and delete a view object the host is
 *      still holding;
 *   7. a document that is in its own window is STILL a tab, and while it is current the central area says so instead
 *      of showing another document - "which document" has one answer even when there are several windows.
 *
 * A 3D view presents through the shared render control (see DocumentView), so switching between two 3D documents does
 * not reparent anything either.
 *
 * MULTI-DOCUMENT: every open document gets one tab, in the order it was opened (moving a tab reorders the strip and
 * nothing else). The strip appears only when a second document is open, so a single-document application sees no tab
 * bar at all. The strip's context menu offers float/dock and close - each one a REQUEST through the same manager API.
 * Closing the current document REQUESTS the tab that takes its place (same position, else the one before): a QTabBar
 * cannot render "nothing selected" while tabs exist, and this request is what keeps the highlighted tab and the page
 * under it showing the same document.
 *
 * FLOATING IS NOT DOCKING: setFloating() moves the view's page into a small top-level window this host owns. That
 * window is deliberately NOT a DockingPane, so it has no docking targets at all - it cannot be dropped on an edge, an
 * auto-hide strip or another pane, and its only ways back are the dock button, the tab's context menu, the API, or
 * closing the document. That is the whole rule: a document is shown either as a tab in the central area or as its own
 * window, never as a dock panel (see .ai/design/appfw-document-model.md and
 * ADR-2026-10-28-document-tabs-and-floating.md).
 *
 * THREAD AFFINITY: application (GUI) thread only.
 */
class VN_APPFW_API CentralDocumentHost : public Control {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Builds the host and starts following the document manager.
     *
     * The host subscribes to `opened`/`closed`/`currentChanged` here and syncs once, so documents that were already
     * open when it was built get their tabs and the current one is presented immediately.
     *
     * @param documents Document manager to follow; borrowed, must outlive the host.
     * @param views     View registry to look document types up in; borrowed, must outlive the host.
     * @param docks     Docking layout whose central slot the host installs into; borrowed, must outlive the host.
     */
    CentralDocumentHost(vn::appfw::DocumentManager& documents, DocumentViewRegistry& views, DockPanelManager& docks);
    ~CentralDocumentHost() override;

  public:
    /**
     * @brief Brings the screen in step with the manager's current document: tab selection, central page, active view.
     *
     * @details
     * - The host runs this itself on `currentChanged`, after a page moves to or from a floating window, and when the
     *   open set changes; a host calls it after changing the view registration behind the manager's back (a plugin was
     *   loaded or unloaded, a view was replaced).
     * - It is the ONE place that derives the display from `DocumentManager::current()`, so the three visible answers
     *   (selected tab, central content, active view) cannot drift apart.
     */
    void syncToCurrentDocument();

    /**
     * @brief Returns the current document's view - in the central area or in its own window.
     *
     * @return The view (borrowed; it lives as long as its document), or nullptr when there is no current document or
     *         its type has no view.
     */
    raw_ptr<DocumentView> currentView() const;

    /**
     * @brief Returns what the central area is showing right now.
     *
     * @details
     * - That is the current view's content(), or a message page while there is nothing to put there: no current
     *   document, the current document is floating in its own window, or its type has no view.
     *
     * @return The element in the central area, or nullptr before the host has taken the area over.
     */
    raw_ptr<UIElement> shownContent() const;

    /**
     * @brief Returns the view built for a document, if it has one by now.
     *
     * @param document Document to look up.
     * @return The cached view, or nullptr when that document was never shown (views are built lazily).
     */
    raw_ptr<DocumentView> viewFor(const vn::appfw::Document& document) const;

    /**
     * @brief Floats a document's view into its own window, or docks it back into the tab strip.
     *
     * @details
     * - Floating builds the document's view on demand (the first one it needs), takes its page out of the central
     *   stack and puts it in a top-level window this host owns.
     * - The window is not a docking pane on purpose: it has no drop targets, so the document can only come back here
     *   (the window's dock button, the tab's context menu, this call, or closing the document).
     * - While the floated document is current, the central area shows a message page naming it instead of another
     *   document's page: the central area only ever shows the current document.
     * - Docking back returns the page to the central stack; it becomes the visible page when the document is current.
     * - A document whose type has no view cannot float: there is no page to move, so the call does nothing.
     *
     * @param document Document to move; must be open in this host's manager.
     * @param floating true to float, false to dock back.
     */
    void setFloating(vn::appfw::Document& document, bool floating);

    /**
     * @brief Reports whether a document is shown in its own window.
     *
     * @param document Document to query.
     * @return true when the document is floating, false when it is (or is not) presented in the tab strip.
     */
    [[nodiscard]] bool isFloating(const vn::appfw::Document& document) const;

    /**
     * @brief Number of open documents presented by this host (one tab each, floating included).
     *
     * @return The tab count; 0 when the manager has no documents.
     */
    [[nodiscard]] int documentCount() const;

    /**
     * @brief Gets the document behind a tab.
     *
     * @param index Tab position, 0-based, in open/moved order.
     * @return The document, or nullptr when @p index is out of range.
     */
    [[nodiscard]] vn::appfw::Document* documentAt(int index) const;

    /**
     * @brief Reports whether the host has taken over the central slot.
     *
     * @return true once the host installed its container into the docking layout.
     */
    bool ownsCentralArea() const;

  private:
    void installIntoCentralArea();
    DocumentView* obtainView(vn::appfw::Document& document);
  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
