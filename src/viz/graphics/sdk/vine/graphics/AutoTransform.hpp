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
 * @brief Scene-graph node placing its subtree with position, rotation and scale.
 *
 * @details
 * - The TRS counterpart of MatrixTransform: a host that drives a joint, a part or a camera rig keeps the
 *   three properties apart and changes the one it animates, instead of rebuilding a matrix by hand.
 * - The local matrix is composed as `T * R * S` (scale in the node's own axes, then rotation, then
 *   translation) on every setter, so the render path reads a ready matrix through
 *   localTransformMatrix() and never composes one per frame.
 * - Scale is a vector, so non-uniform scale is legal. A zero component collapses the subtree along that
 *   axis; a negative component mirrors it (and flips the winding of its triangles).
 * - `transform()` / `setTransform()` exchange the translation and rotation as an Isometry3d, which is
 *   what the robotics side already produces. Scale is not part of an isometry, so setTransform() leaves
 *   it alone.
 *
 * @note
 * - The NAME is the host's, not OSG's: OSG's AutoTransform is a billboard that faces the camera. This node
 *   never looks at a camera - "auto" means the matrix follows the properties automatically.
 */
class VN_GRAPHICS_API AutoTransform : public Group {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a node at the origin, with an identity rotation and unit scale. */
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
     * @param position Translation applied after rotation and scale.
     */
    void setPosition(const Point3d& position);

    /** @brief Gets the rotation.
     *
     * @return The stored quaternion (identity by default). The matrix path normalizes a non-unit value.
     */
    Quatd rotation() const;

    /** @brief Sets the rotation, applied after scale and before translation.
     *
     * @param rotation Unit quaternion (a non-unit value is normalized by the matrix path).
     */
    void setRotation(const Quatd& rotation);

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

    /** @brief Gets the translation and rotation as an isometry (scale is not part of it).
     *
     * @return The stored position and rotation; scale is reported separately by scale().
     */
    Isometry3d transform() const;

    /** @brief Sets the translation and rotation from an isometry, leaving scale unchanged.
     *
     * @param transform Pose in the parent's frame (its rotation should be a unit quaternion).
     */
    void setTransform(const Isometry3d& transform);

    /** @brief Gets the composed local matrix, `T * R * S`.
     *
     * @details
     * - A READ-ONLY view of what localTransformMatrix() hands the render path: derived from the three
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
    Quatd   rotation_{};
    Vec3d   scale_{ 1.0, 1.0, 1.0 };
    Mat4d   matrix_;
};

using AutoTransformPtr = intrusive_ptr<AutoTransform>;

VN_GRAPHICS_NS_END
