#pragma once
#include "graphics_global.hpp"

#include <limits>

#include <vine/math/Isometry3.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Vector3.hpp>

#include "Group.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Isometry3d;
using vn::math::Point3d;
using vn::math::Quatd;
using vn::math::Vec3d;

/**
 * @brief Scene-graph node that turns its subtree to face the screen or the camera (an OSG-style billboard).
 *
 * @details
 * - The automatic counterpart of PositionAttitudeTransform: the subtree is placed at position with scale
 *   and pivot, but its ROTATION is derived from the camera that draws it. The two modes that matter:
 *   - RotateToScreen: the subtree stays parallel to the screen plane (the camera's view rotation is
 *     cancelled), the classic text / sprite / label billboard;
 *   - RotateToCamera: the subtree's local +Z axis points at the eye (a spherical billboard, which turns
 *     more than the screen-aligned one as it moves off-centre).
 * - The camera-dependent matrix is computed during command collection, once per view: the collected list
 *   is already memoised per view, so two cameras get two correct lists and a still view reuses one.
 * - position, rotation, scale and pivot are in the parent's frame, and the parent transform is inherited
 *   (OSG semantics): the automatic rotation is computed from the model-view, so a parent rotation is
 *   cancelled by the local result and a parent scale still reaches the billboard. A uniform parent scale
 *   is recommended; a non-uniform one stretches/shears a billboard. rotation is applied AFTER the
 *   automatic orientation, so it can spin the billboard in its own plane. scale and pivot are the same as
 *   PositionAttitudeTransform's.
 * - NoRotation is the plain, camera-independent placement: it behaves exactly like
 *   PositionAttitudeTransform and is what worldMatrix(), picking and Scene::boundingBox() see.
 * - setAutoScaleToScreen(true) adds OSG's screen-space scaling: the node is scaled uniformly so the same
 *   local geometry keeps a constant pixel size. It needs the device-pixel viewport, which collection
 *   receives in CollectView; without one it keeps the authored scale.
 *
 * @note
 * - This node is view-dependent by design. Its boundingBox() is conservative (a sphere-bound around
 *   position) so culling and scene framing never depend on which camera collected last.
 * - The OSG reference radius 0.48 is kept: a local radius of 0.48 covers one pixel at the current
 *   distance. scale() stays the authored/fallback value; the computed scale exists only in the per-view
 *   matrix (two passes may use different viewports).
 */
class VN_GRAPHICS_API AutoTransform : public Group {
    VN_OBJECT_META_DECL;

  public:
    /** @brief How the automatic rotation is derived from the current camera. */
    enum class AutoRotateMode
    {
        NoRotation,      ///< No automatic rotation: the plain TRS placement (the view-independent matrix).
        RotateToScreen,  ///< Cancel the camera's rotation, so the subtree stays parallel to the screen plane.
        RotateToCamera,  ///< Point the subtree's local +Z at the eye (spherical billboard).
    };

  public:
    /** @brief Constructs a billboard that does not rotate automatically, at the origin with unit scale. */
    AutoTransform();
    ~AutoTransform();

  public:
    /** @brief Gets the position in the parent's frame.
     *
     * @return The translation (zero by default).
     */
    Point3d position() const;

    /** @brief Sets the position in the parent's frame.
     *
     * @param position Where the pivot lands after the transform.
     */
    void setPosition(const Point3d& position);

    /** @brief Gets the rotation applied AFTER the automatic orientation.
     *
     * @return The stored quaternion (identity by default). The matrix path normalizes a non-unit value.
     */
    Quatd rotation() const;

    /** @brief Sets the rotation applied after the automatic orientation.
     *
     * @param rotation Unit quaternion (a non-unit value is normalized by the matrix path).
     */
    void setRotation(const Quatd& rotation);

    /** @brief Gets the authored scale.
     *
     * @details
     * - With autoScaleToScreen() on, this is the FALLBACK used when no viewport is available; the drawn
     *   scale is computed per view and is not written back here (two passes may disagree).
     *
     * @return The per-axis scale factors (one by default).
     */
    Vec3d scale() const;

    /** @brief Sets the authored scale, applied in the billboard's own axes.
     *
     * @param scale Per-axis factors (negative mirrors that axis, zero collapses it).
     */
    void setScale(const Vec3d& scale);

    /** @brief Gets the pivot in the node's own frame.
     *
     * @return The local point that stays at position (zero by default).
     */
    Point3d pivot() const;

    /** @brief Sets the pivot in the node's own frame: the point scale and rotation turn around.
     *
     * @param pivot Local point mapped to position after the transform.
     */
    void setPivot(const Point3d& pivot);

    /** @brief Gets the automatic rotation mode.
     *
     * @return The mode (NoRotation by default).
     */
    AutoRotateMode autoRotateMode() const;

    /** @brief Sets the automatic rotation mode.
     *
     * @param mode How the camera determines the rotation (see AutoRotateMode).
     */
    void setAutoRotateMode(AutoRotateMode mode);

    /** @brief Gets whether screen-space scaling is enabled.
     *
     * @return true when the per-view matrix derives a uniform scale from the viewport.
     */
    bool autoScaleToScreen() const;

    /** @brief Enables or disables OSG-style screen-space scaling.
     *
     * @details
     * - A viewport must be supplied by the collection (RenderPass does it; a host calling
     *   Scene::collectRenderCommands directly passes one). Without one the authored scale() is kept.
     * - The computed scale is clamp()ed to [minimumScale(), maximumScale()] and is uniform.
     *
     * @param enabled true to derive the scale from the viewport.
     */
    void setAutoScaleToScreen(bool enabled);

    /** @brief Gets the lower clamp applied to the computed screen-space scale.
     *
     * @return The minimum scale (zero by default).
     */
    double minimumScale() const;

    /** @brief Sets the lower clamp applied to the computed screen-space scale.
     *
     * @param scale Lower bound (a value above maximumScale() also raises the maximum).
     */
    void setMinimumScale(double scale);

    /** @brief Gets the upper clamp applied to the computed screen-space scale.
     *
     * @return The maximum scale (the largest double by default).
     */
    double maximumScale() const;

    /** @brief Sets the upper clamp applied to the computed screen-space scale.
     *
     * @param scale Upper bound (a value below minimumScale() also lowers the minimum).
     */
    void setMaximumScale(double scale);

    /** @brief Gets the view-INDEPENDENT local matrix (the NoRotation placement).
     *
     * @details
     * - What worldMatrix(), picking and the static layout use. In an auto-rotate mode the matrix actually
     *   drawn is the one localToWorldForView() computes for that camera.
     *
     * @return `T(position) * R(rotation) * S(scale) * T(-pivot)`.
     */
    Mat4d matrix() const;

    /** @brief Computes this billboard's world matrix for the view being drawn.
     *
     * @details
     * - NoRotation without auto-scale is view-independent (the walk uses the plain local matrix). A
     *   view-dependent mode fills @p world with
     *   `T(world_position) * R(auto) * R(rotation) * S(scale) * T(-pivot)`.
     * - The world position is `parent_world * position`; the auto rotation is a world-space rotation, so
     *   parent rotation / scale do not enter it (see the class note).
     * - With autoScaleToScreen() the scale is uniform and derived from the viewport; otherwise the authored
     *   scale() is used. An invalid viewport keeps the authored scale.
     *
     * @param parent_world World matrix of this node's parent.
     * @param view         The view being drawn: projection / view / eye / up and the device-pixel viewport.
     * @param world        Receives the world matrix when the return value is not None.
     * @return None when neither automatic mode nor screen-space scaling is on; View for an auto-rotating
     *         billboard without a viewport; ViewAndViewport when screen-space scaling uses the viewport.
     */
    ViewDependence localToWorldForView(const Mat4d& parent_world, const CollectView& view,
                                       Mat4d& world) const override;

    /** @brief Computes a conservative world-space box (the sphere around position covering every orientation).
     *
     * @details
     * - With auto-scale on, the bound uses maximumScale() when it is finite (otherwise the authored scale):
     *   the actual per-view scale is not known without a camera.
     *
     * @return World-space AABB that contains the subtree in any automatic orientation.
     */
    Aabbd boundingBox() const override;

  protected:
    /** @brief This node's view-independent local matrix (see matrix()). */
    Mat4d localTransformMatrix() const override;

  private:
    void   recompose();
    double screenScaleFor(const CollectView& view, const Mat4d& parent_world) const;

    Point3d        position_{};
    Quatd          rotation_{};
    Vec3d          scale_{ 1.0, 1.0, 1.0 };
    Point3d        pivot_{};
    AutoRotateMode mode_{ AutoRotateMode::NoRotation };
    bool           auto_scale_to_screen_{ false };
    double         minimum_scale_{ 0.0 };
    double         maximum_scale_{ std::numeric_limits<double>::max() };
    Mat4d          matrix_;
};

using AutoTransformPtr = intrusive_ptr<AutoTransform>;

VN_GRAPHICS_NS_END
