#pragma once
#include "graphics_global.hpp"

#include <vine/RefCounted.hpp>
#include <vine/geom/spatial/Aabb.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>

#include "CollectView.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::geom::spatial::Aabbd;
using vn::math::Mat4d;

/**
 * @brief What a node's per-view placement depends on.
 *
 * @details
 * - None: the node is placed by localTransformMatrix() alone (the common case).
 * - View: projection / view / eye / up matter, but the device-pixel viewport does not (a billboard that only
 *   rotates). A collected list can be shared by two passes that look the same but fill different rectangles.
 * - ViewAndViewport: the viewport matters too (screen-space scaling), so the collected-list memo must key on
 *   the device-pixel rectangle as well.
 */
enum class ViewDependence
{
    None,            // View-independent placement.
    View,            // Depends on the camera, but not on the device-pixel viewport.
    ViewAndViewport, // Depends on the camera and on the device-pixel viewport.
};

/**
 * @brief A node's per-view placement as an immutable VALUE: one pure function of (parent placement, view).
 *
 * @details
 * - WHY A VALUE. A draw list is built from rows so a side that owns no content can draw it (see
 *   `.ai/design/graphics-threading.md`). A node whose placement depends on the camera was the one thing that could
 *   not be expressed that way - the row had to hand its whole run back to a walk that could still read the node. With
 *   the placement described as a value, the row carries it and the select resolves it from the view it was given,
 *   like every other row.
 * - WHAT IT OWNS. Only numbers: the mode, the authored placement, the scale limits. No node, no camera, no content -
 *   so two threads may hold the same placement.
 * - THE TWO BOXES. A caller that culls without a view needs a bound that covers EVERY view
 *   (`conservativeParentBox`), which is why a rotation is bounded by a cube around its pivot rather than by the
 *   unrotated box. `isBounded()` says whether that bound exists at all: an auto-scaled billboard with no finite
 *   maximum has no view-independent size, and a caller must then skip the cull instead of trusting a box.
 * - THE FRAME OF EACH ANSWER. `localMatrix` maps the node's OWN frame into its parent's; `conservativeParentBox`
 *   takes a box in the node's own frame and answers in the PARENT frame. That is the frame the caller folds in
 *   (a child's box is unioned into its parent's with exactly that frame change), and it is what lets a rotated
 *   placement be bounded by a box that no longer shares the node's orientation.
 *
 * @note Not copyable: it has identity (it is shared), and it never changes.
 */
class VN_GRAPHICS_API ViewPlacement : public RefCounted<ViewPlacement> {
  public:
    ViewPlacement() = default;
    virtual ~ViewPlacement();

    ViewPlacement(const ViewPlacement&)            = delete;
    ViewPlacement& operator=(const ViewPlacement&) = delete;

    /**
     * @brief Gets the WIDEST dependence this placement can have (the declaration a bake may trust without a view).
     *
     * @return None only for a placement that is never view-dependent; otherwise the strongest case it can produce.
     */
    [[nodiscard]] virtual ViewDependence declaredDependence() const noexcept = 0;

    /**
     * @brief Gets the dependence for ONE view.
     *
     * @param view View the placement is resolved for.
     * @return None when this view needs no per-view placement at all (the caller keeps the view-independent
     *         matrix); otherwise the strongest input it read.
     */
    [[nodiscard]] virtual ViewDependence dependence(const CollectView& view) const noexcept = 0;

    /**
     * @brief Resolves this placement for one view: the node's LOCAL matrix (the parent is applied by the caller).
     *
     * @param parent_world The placement the row inherits (some modes cancel a parent rotation or scale).
     * @param view         The view to resolve for.
     * @return The local matrix to compose with @p parent_world.
     */
    [[nodiscard]] virtual Mat4d localMatrix(const Mat4d& parent_world, const CollectView& view) const = 0;

    /**
     * @brief Bounds the subtree for EVERY view, in the PARENT frame.
     *
     * @details
     * - A cull that runs without a view (the fold, an ancestor's box) cannot use the box the placement produces for
     *   one camera, so the placement answers the smallest box it can prove contains every orientation and scale it
     *   may choose. A rotation therefore becomes a cube around its pivot.
     * - It reads the OWN-frame box and answers in the parent frame, because a rotation does not stay axis-aligned in
     *   the node's own frame: stating the bound in the frame the caller folds into keeps one convention for every
     *   child, per-view or not.
     *
     * @param own_box The subtree's box in the node's own frame (the box `localMatrix` consumes).
     * @return A box in the PARENT frame containing the subtree for every view (empty for an empty @p own_box).
     */
    [[nodiscard]] virtual Aabbd conservativeParentBox(const Aabbd& own_box) const = 0;

    /**
     * @brief Gets whether @ref conservativeParentBox is meaningful for every view.
     *
     * @return false when the placement can grow without a finite bound (screen scaling with no finite maximum), in
     *         which case a caller must not cull the row by its box.
     */
    [[nodiscard]] virtual bool isBounded() const noexcept = 0;
};

using ViewPlacementPtr = intrusive_ptr<const ViewPlacement>;

/**
 * @brief Resolves one placement for one view: the world matrix a row draws with, and what deciding it costs.
 *
 * @details
 * - THE ONE RULE a collector applies to a per-view row, here rather than inline so every caller - the select, a
 *   render world, a test - resolves a billboard the same way.
 * - @p world is written only when the answer is not None: a view that needs nothing beyond the node's own local matrix
 *   leaves the caller's view-independent placement alone.
 *
 * @param placement    The value taken from the node (Node::viewPlacement).
 * @param parent_world The placement the row inherits (some modes cancel a parent rotation or scale).
 * @param view         The view being drawn.
 * @param world        Receives the node's world matrix when the return value is not None.
 * @return None when this view needs no per-view placement; otherwise the strongest input it read.
 */
[[nodiscard]] VN_GRAPHICS_API ViewDependence localToWorldForView(const ViewPlacement& placement,
                                                                const Mat4d&         parent_world,
                                                                const CollectView&   view,
                                                                Mat4d&               world);

VN_GRAPHICS_NS_END
