#pragma once
#include "graphics_global.hpp"

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
 * - position is in the parent's frame and is transformed by the parent; the automatic modes do NOT inherit
 *   the parent's rotation or scale (a billboard is world-oriented - only where it sits comes from the
 *   parent). rotation is applied AFTER the automatic orientation, so it can spin the billboard in its own
 *   plane. scale and pivot are the same as PositionAttitudeTransform's.
 * - NoRotation is the plain, camera-independent placement: it behaves exactly like
 *   PositionAttitudeTransform and is what worldMatrix(), picking and Scene::boundingBox() see.
 *
 * @note
 * - Screen-space SCALING (OSG's setAutoScaleToScreen) is not implemented: it needs the target's pixel
 *   height, which collection does not receive. The rotation modes above are the billboard feature; a
 *   constant-pixel-size node is a separate slice.
 * - This node is view-dependent by design. Its boundingBox() is conservative (a sphere-bound around
 *   position) so culling and scene framing never depend on which camera collected last.
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

    /** @brief Gets the scale.
     *
     * @return The per-axis scale factors (one by default).
     */
    Vec3d scale() const;

    /** @brief Sets the scale, applied in the billboard's own axes.
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

    /** @brief Gets the view-INDEPENDENT local matrix (the NoRotation placement).
     *
     * @details
     * - What worldMatrix(), picking and the static layout use. In an auto-rotate mode the matrix actually
     *   drawn is the one localToWorldForView() computes for that camera.
     *
     * @return `T(position) * R(rotation) * S(scale) * T(-pivot)`.
     */
    Mat4d matrix() const;

    /** @brief Computes this billboard's world matrix for the camera being drawn.
     *
     * @details
     * - NoRotation returns false (the walk uses the plain local matrix). The auto modes return true with
     *   `T(world_position) * R(auto) * R(rotation) * S(scale) * T(-pivot)`.
     * - The world position is `parent_world * position`; the auto rotation is a world-space rotation, so
     *   parent rotation / scale do not enter it (see the class note).
     *
     * @param parent_world World matrix of this node's parent.
     * @param view         The camera's world-to-view matrix.
     * @param eye          The camera's eye point in WORLD space.
     * @param up           The camera's up direction in WORLD space.
     * @param world        Receives the world matrix when the return value is true.
     * @return true when @p world was filled; false in NoRotation.
     */
    bool localToWorldForView(const Mat4d& parent_world, const Mat4d& view, const Vec3d& eye, const Vec3d& up,
                             Mat4d& world) const override;

    /** @brief Computes a conservative world-space box (the sphere around position covering every orientation).
     *
     * @return World-space AABB that contains the subtree in any automatic orientation.
     */
    Aabbd boundingBox() const override;

  protected:
    /** @brief This node's view-independent local matrix (see matrix()). */
    Mat4d localTransformMatrix() const override;

  private:
    void recompose();

    Point3d        position_{};
    Quatd          rotation_{};
    Vec3d          scale_{ 1.0, 1.0, 1.0 };
    Point3d        pivot_{};
    AutoRotateMode mode_{ AutoRotateMode::NoRotation };
    Mat4d          matrix_;
};

using AutoTransformPtr = intrusive_ptr<AutoTransform>;

VN_GRAPHICS_NS_END
