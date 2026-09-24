#pragma once
#include "graphics_global.hpp"

#include <vine/math/Matrix4x4.hpp>

#include "Group.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;

/**
 * @brief Scene-graph node placing its subtree with a local transform.
 *
 * MatrixTransform is the OSG/vsg-style transform node and the ONLY holder of
 * a transform in the scene graph: the matrices of nested MatrixTransforms
 * multiply along the root-to-leaf path (Node::worldMatrix()) to place
 * geometry in world space, and render command collection bakes that chain
 * into each command's world model matrix. As a Group it also aggregates
 * children (typically leaf Geometries or nested transforms), and its
 * world-space bounding box is inherited from Group (children already answer
 * in world space, including this matrix).
 */
class VN_GRAPHICS_API MatrixTransform : public Group {
    VN_OBJECT_META_DECL;

  public:
    MatrixTransform();
    ~MatrixTransform();

  public:
    /** @brief Gets the local transform of this node. */
    Mat4d matrix() const;

    /** @brief Sets the local transform of this node.
     *
     * @param matrix Local-space transform (identity by default).
     */
    void setMatrix(const Mat4d& matrix);

  protected:
    /** @brief This node's local matrix, contributed to Node::worldMatrix(). */
    Mat4d localTransformMatrix() const override;

  private:
    Mat4d matrix_;
};

using MatrixTransformPtr = intrusive_ptr<MatrixTransform>;

VN_GRAPHICS_NS_END

