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
 * @brief Scene-graph node placing its subtree with position, attitude (rotation) and scale.
 *
 * @details
 * - The TRS node of the scene graph, named as in OSG: a host that drives a joint, a part or a camera rig
 *   keeps the three properties apart and changes the one it animates, instead of rebuilding a matrix by
 *   hand.
 * - The local matrix is composed as `T(position) * R(attitude) * S(scale) * T(-pivot)` on every setter,
 *   so the render path reads a ready matrix through localTransformMatrix() and never composes one per
 *   frame. The pivot is the local point that stays at position (zero by default).
 * - Scale is a vector, so non-uniform scale is legal. A zero component collapses the subtree along that
 *   axis; a negative component mirrors it (and flips the winding of its triangles).
 * - `transform()` / `setTransform()` exchange the translation and rotation as an Isometry3d, which is
 *   what the robotics side already produces. Scale is not part of an isometry, so setTransform() leaves
 *   it alone.
 *
 * @note
 * - This node is NOT camera-aware: it is the plain TRS placement. For a node that turns to face the
 *   screen, see AutoTransform (the OSG-style billboard).
 */
class VN_GRAPHICS_API PositionAttitudeTransform : public Group {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a node at the origin, with an identity attitude and unit scale. */
    PositionAttitudeTransform();
    ~PositionAttitudeTransform();

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

    /** @brief Gets the attitude (rotation).
     *
     * @return The stored quaternion (identity by default). The matrix path normalizes a non-unit value.
     */
    Quatd attitude() const;

    /** @brief Sets the attitude, applied after scale and before translation.
     *
     * @param attitude Unit quaternion (a non-unit value is normalized by the matrix path).
     */
    void setAttitude(const Quatd& attitude);

    /** @brief Gets the scale.
     *
     * @return The per-axis scale factors (one by default).
     */
    Vec3d scale() const;

    /** @brief Sets the scale, applied in the node's own axes before rotation.
     *
     * @param scale Per-axis factors (negative mirrors that axis, zero collapses it).
     */
    void setScale(const Vec3d& scale);

    /** @brief Gets the pivot in the node's own frame.
     *
     * @return The local point that stays at position (zero by default).
     */
    Point3d pivot() const;

    /** @brief Sets the pivot in the node's own frame: the point scale and attitude turn around.
     *
     * @param pivot Local point mapped to position after the transform.
     */
    void setPivot(const Point3d& pivot);

    /** @brief Gets the translation and rotation as an isometry (scale and pivot are not part of it).
     *
     * @return The stored position and attitude; scale is reported separately by scale().
     */
    Isometry3d transform() const;

    /** @brief Sets the translation and rotation from an isometry, leaving scale and pivot unchanged.
     *
     * @param transform Pose in the parent's frame (its rotation should be a unit quaternion).
     */
    void setTransform(const Isometry3d& transform);

    /** @brief Gets the composed local matrix, `T(position) * R(attitude) * S(scale) * T(-pivot)`.
     *
     * @details
     * - A READ-ONLY view of what localTransformMatrix() hands the render path: derived from the four
     *   properties, never set. It is the same spelling MatrixTransform::matrix() uses, so a caller that
     *   asks "where does this subtree land" does not need to know which transform kind it holds.
     *
     * @return The composed local matrix.
     */
    Mat4d matrix() const;

  protected:
    /** @brief This node's composed local matrix, contributed to Node::worldMatrix(). */
    Mat4d localTransformMatrix() const override;

  private:
    void recompose();

    Point3d position_{};
    Quatd   attitude_{};
    Vec3d   scale_{ 1.0, 1.0, 1.0 };
    Point3d pivot_{};
    Mat4d   matrix_;
};

using PositionAttitudeTransformPtr = intrusive_ptr<PositionAttitudeTransform>;

VN_GRAPHICS_NS_END
