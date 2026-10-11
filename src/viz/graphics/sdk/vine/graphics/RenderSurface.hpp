#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <vector>

#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderPass;
class SceneView;

/**
 * @brief The presentation table of ONE render surface: which passes draw into it, and in which order.
 *
 * A surface is the thing a picture ends up in (the host's native window, a swapchain). Which passes draw into it
 * is a fact about the SURFACE, not about any single pass - and it has exactly one owner, this object: a host
 * declares its presentations here, and this is the answer to "who draws on this surface, first to last".
 *
 * That matters because "who presents" used to be inferred: a view asked the engine whether ANY registered pass
 * carried its camera with a null render target (RenderEngine::hasWindowPass) and stood down when one did - a
 * global, order-sensitive guess with no owner, which silently dropped the SECOND view sharing a camera and could
 * not tell "this pass draws HERE" from "this pass exists". A host that builds its own main pass now says so:
 *
 * @code
 * control->surface()->present(pipeline->windowPass());   // this pass presents this surface
 * @endcode
 *
 * and the view's default window pass is only built when nobody declared anything (see present(SceneView&)):
 *
 * @code
 * if (!control->surface()->hasPresentation()) {
 *     control->surface()->present(*control->view());     // the fallback: my view presents this surface
 * }
 * @endcode
 *
 * The table is ordered by the ENGINE's schedule (presentations()): the first entry is the pass that draws first
 * on this surface. Ties - two presentations at the same pass order - keep declaration order.
 *
 * @note Splitting a surface between several presentations (each with its own rectangle) is the same table read
 * again; the rectangle per presentation belongs to the surface too, and so does the ONE clear a surface gets.
 * Those land on top of this table, not beside it.
 */
class VN_GRAPHICS_API RenderSurface : public RefCounted<RenderSurface> {
  public:
    /** @brief One presentation: the pass that draws this surface, and the view it was declared for.
     *
     * `pass` is the pass the engine will run - the window pass of the entry's view (resolved when the table is
     * read, so a rebuilt pipeline is followed), or the pass a host declared itself. `view` is null for a pass a
     * host declared (nothing about that presentation belongs to a view).
     */
    struct Presentation {
        /// The pass that draws this surface.
        raw_ptr<RenderPass> pass = nullptr;
        /// The view this presentation came from, or null when a host declared the pass.
        raw_ptr<SceneView> view = nullptr;
    };

  public:
    /** @brief Creates the presentation table of one surface rendered through @p engine.
     *
     * @param engine Engine the presentations are registered on, and the one their order is read from (borrowed;
     *               must outlive this surface). Null is accepted and presents nothing: find the engine first.
     */
    explicit RenderSurface(raw_ptr<RenderEngine> engine);

    /** @brief Destroys the table. The presentations are declarations, not owners: the passes stay registered on
     * the engine and the views keep their pipelines.
     */
    ~RenderSurface();

  public:
    /** @brief Declares that @p view draws this surface, and makes sure it has a default window pass.
     *
     * The declaration a host makes when it has no main pass of its own: this surface is presented by the view's
     * default window pipeline, which is built here (SceneView::ensureWindowPass) if it does not exist yet.
     * Declaring the same view twice is one presentation. A view that draws through ANOTHER engine (or none) is
     * refused and reported: its window pass is registered on that engine's frame, so it could never present this
     * surface, and a host that is told nothing would be looking at an empty area.
     *
     * @param view View to present; must be bound to this surface's engine.
     * @return true when this added a presentation; false when @p view already presented this surface, or could
     *         not.
     */
    bool present(SceneView& view);

    /** @brief Declares that @p pass draws this surface.
     *
     * The declaration a host makes when it built its own main pass (an application pipeline presenting this
     * surface): the view's default window pass is not built while this declaration stands, so the host keeps full
     * control. Declaring the same pass twice is one presentation.
     *
     * The pass is NOT adopted: the pipeline that built it owns it (and the engine holds it while registered).
     * This table keeps its own reference so what it reports cannot outlive what it describes.
     *
     * @param pass Pass that presents this surface; null is a no-op.
     * @return true when this added a presentation; false when @p pass already presented this surface.
     */
    bool present(raw_ptr<RenderPass> pass);

    /** @brief Drops the presentation @p view declared.
     *
     * The view keeps its pipeline (that is the view's), and the passes stay registered: what leaves is the
     * statement that this surface is theirs.
     *
     * @param view View whose presentation to drop.
     * @return true when a presentation was dropped.
     */
    bool dismiss(raw_ptr<SceneView> view);

    /** @brief Drops the presentation @p pass declared (see dismiss(SceneView&)).
     *
     * @param pass Pass whose presentation to drop.
     * @return true when a presentation was dropped.
     */
    bool dismiss(raw_ptr<RenderPass> pass);

    /** @brief Whether anything declares this surface.
     *
     * The question the default-path host asks before falling back to its own view (see the class docs).
     *
     * @return true when this surface has at least one presentation.
     */
    [[nodiscard]] bool hasPresentation() const noexcept;

    /** @brief The presentations of this surface, in the order the engine runs them.
     *
     * The answer describes the frame the engine would draw NOW, not what was once declared: a presentation whose
     * pass the engine no longer has (the pipeline that built it was dropped, the host removed the pass) is left
     * out. An entry whose view has no window pass yet is left out for the same reason.
     *
     * @return The presentations, first-drawn first (empty when nothing presents this surface).
     */
    [[nodiscard]] std::vector<Presentation> presentations() const;

  private:
    /** @brief One declared presentation: a view (its window pass is resolved when the table is read) or a pass a
     * host declared itself.
     */
    struct Entry {
        raw_ptr<SceneView>        view = nullptr;
        intrusive_ptr<RenderPass> pass;
    };

    /** @brief The pass an entry presents now: its declared pass, or the window pass of its view's live pipeline.
     *
     * @param entry Entry to resolve.
     * @return The pass, or nullptr when there is none to resolve (no pipeline yet, no window pass).
     */
    raw_ptr<RenderPass> passOf(const Entry& entry) const;

    /// The engine the presentations are registered on and ordered by (borrowed; must outlive this surface).
    raw_ptr<RenderEngine> engine_;
    /// The table, in declaration order (presentations() answers in engine order).
    std::vector<Entry> entries_;
};

VN_GRAPHICS_NS_END
