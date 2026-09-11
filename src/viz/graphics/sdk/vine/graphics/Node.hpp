#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Rect3.hpp>

V_GRAPHICS_NS_BEGIN

using vine::math::Mat4d;
using vine::math::Aabbd;

class Group;

/**
 * @brief Base class of every scene-graph node.
 *
 * Node is the OSG/vsg-style base of the scene hierarchy. Concrete kinds
 * derive from it: Group aggregates children, MatrixTransform places a
 * subtree with a local matrix, StateNode applies render state to a subtree,
 * and Geometry is a leaf carrying vertex data. Node itself provides only the
 * shared identity: the node name, a subtree-level visibility/opacity
 * multiplier, the parent link, and world-matrix / bounding-box queries. It is
 * NOT a container and holds no transform of its own: attach children through
 * Group and place a subtree through MatrixTransform. The world-space
 * boundingBox() of a subtree is computed by walking the parent chain, so any
 * node answers in world space regardless of depth.
 */
class V_GRAPHICS_API Node : public Object, public RefCounted<Node> {
    V_OBJECT_META_DECL;

  public:
    Node();
    ~Node();

  public:
    /** @brief Gets the node name. */
    String name() const;

    /** @brief Sets the node name. */
    void setName(const String& name);

    /** @brief Returns whether this subtree is visible. */
    bool isVisible() const;

    /** @brief Sets whether this subtree is visible. */
    void setVisible(bool visible);

    /** @brief Gets the subtree opacity multiplier in [0, 1]. */
    float opacity() const;

    /** @brief Sets the subtree opacity multiplier in [0, 1]. */
    void setOpacity(float opacity);

    /** @brief Gets the parent node (null for root nodes). */
    raw_ptr<Node> parent() const;

    /** @brief Computes the world-space bounding box of this subtree.
     *
     * Every node answers in world space: leaf Geometry boxes are the bound
     * of their vertex data transformed by the enclosing MatrixTransforms;
     * Group/MatrixTransform boxes union their children's boxes (which are
     * already world-space). Base Node (never used as a renderable or
     * container) returns an empty box.
     *
     * @return World-space AABB of this subtree.
     */
    virtual Aabbd boundingBox() const;

    /** @brief Gets the accumulated world matrix of this node.
     *
     * The product, from the scene root downwards, of the local matrices of
     * every enclosing MatrixTransform (identity for every non-transform
     * node). For a leaf Geometry this places its local vertex data in world
     * space; for a MatrixTransform it also includes its own matrix.
     *
     * This walks the ancestor chain, so a traversal should prefer
     * accumulating from the root with localTransformMatrix() (one product per
     * node) over calling this once per node (one product per ancestor).
     *
     * @return World-space transform.
     */
    Mat4d worldMatrix() const;

    /** @brief Gets this node's own local matrix contribution.
     *
     * Identity for every node that is not a MatrixTransform. Public so a
     * traversal can accumulate world matrices top-down:
     * `child_world = parent_world * child.localTransformMatrix()`.
     *
     * @return This node's local matrix (identity unless a MatrixTransform).
     */
    virtual Mat4d localTransformMatrix() const;

  private:
    String name_;
    bool visible_ = true;
    float opacity_ = 1.0f;
    raw_ptr<Node> parent_ = nullptr;

    friend class Group;
};

using NodePtr = intrusive_ptr<Node>;

V_GRAPHICS_NS_END
