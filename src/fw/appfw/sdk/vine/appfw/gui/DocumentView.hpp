#pragma once

#include "UIElement.hpp"

VN_APPFW_NS_BEGIN
class Document;
VN_APPFW_NS_END

VN_APPFWGUI_NS_BEGIN

class RenderControl;

/**
 * @brief One document's presentation: what turns a `Document` into something on screen.
 *
 * ONE VIEW PER DOCUMENT INSTANCE, not per document type: the state a view holds (camera, selection, scroll position,
 * zoom) belongs to *this* look at *this* document. Sharing one view between two documents of the same type would either
 * leak the first document's state into the second (a selection index that means nothing there) or throw away where the
 * user was on every switch. The host (CentralDocumentHost) therefore creates a view the first time a document is shown,
 * keeps it while that document is open, and destroys it when the document closes.
 *
 * A view is NOT the central area: the central area is the slot the host installs into, and `content()` is what goes into
 * it. **One document, one view, its own resources** - so `content()` is the view itself in both kinds (see
 * appfw-document-model.md §7/§9): a 2D view is a widget of its own, and a 3D view is a widget that holds its OWN
 * render control (its own surface, session, device and pipelines). What the application shares between documents is the
 * shell around them - dock panels and ribbon menus - not the document area.
 *
 * VIEW STATE IS NOT DOCUMENT STATE: it is never saved, never makes a document dirty, and disappears with the view.
 *
 * THE VIEW IS WHERE "WHICH 3D AREA?" IS ANSWERED: `activeRenderControl()` names the pane a command should act on, so
 * the window derives its primary control from the current view instead of a view publishing itself into a
 * window-wide slot (see `../decisions/ADR-2026-10-24-command-target.md`).
 */
class VN_APPFW_API DocumentView : public UIElement {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Constructs a view for a document, with no widget of its own.
     *
     * This is the constructor for a view that brings its own control inside a widget tree it builds itself (a 3D view
     * puts its render control there - see ModelRenderView in the model_viewer plugin).
     *
     * @param document Document this view presents; must outlive the view (the host destroys the view on `closed`).
     */
    explicit DocumentView(Document& document);
    ~DocumentView() override;

  public:
    /**
     * @brief Returns the document this view presents.
     *
     * @return The document (borrowed; it outlives the view - see the class note).
     */
    Document& document() const noexcept;

    /**
     * @brief Returns what to show in the central area.
     *
     * @return The element the host puts on screen: this view for a 2D view, the shared render control for a 3D one.
     *         Never null.
     */
    virtual UIElement* content() = 0;

    /**
     * @brief Called when this view becomes the one on screen.
     *
     * A 3D view binds the SceneView it holds into the shared render control here; a 2D view normally does nothing.
     */
    virtual void activate() {}

    /**
     * @brief Called when this view stops being the one on screen (another view took its place, or the area went empty).
     *
     * A 3D view detaches what it bound in activate(); a 2D view normally does nothing.
     */
    virtual void deactivate() {}

    /**
     * @brief Returns the render control of the part of this view a command should act on, or null.
     *
     * @details
     * - A view that hosts ONE 3D area returns that area's control. A view that hosts SEVERAL panes returns the one
     *   the user last worked in - **the view owns that decision**, because it is the view that created the panes; the
     *   framework keeps no pane registry and no layout rule.
     * - This is the accessor that turns "which 3D area is current" into a DERIVED question: `MainWindow::
     *   primaryRenderControl()` answers it through the document host's current view, so no view has to publish
     *   itself into a window-wide slot any more.
     * - A 2D view (and a view that draws nothing) returns null - "this view has no 3D area" - which is a legal
     *   answer, not a failure.
     *
     * @return The active pane's render control, or null when this view has none.
     */
    virtual RenderControl* activeRenderControl() { return nullptr; }

  protected:
    /**
     * @brief Constructs a 2D view around its own widget.
     *
     * @param document Document this view presents.
     * @param impl     The widget this view wraps (the view owns it: content() returns this view).
     */
    DocumentView(Document& document, UIObject* impl);

  private:
    Document* document_ = nullptr;
};

VN_APPFWGUI_NS_END
