#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vector>

#include "Node.hpp"

V_GRAPHICS_NS_BEGIN

/**
 * @brief Scene-graph node that aggregates child nodes.
 *
 * Group is the OSG/vsg-style container node: it adds no state or transform of
 * its own, it simply holds child nodes (nested Groups, MatrixTransforms,
 * StateNodes and leaf Geometries) and keeps each child's parent link in sync.
 * Scene-graph kinds that apply something to their subtree (a
 * MatrixTransform's transform, a StateNode's render state) derive from Group
 * so they remain ordinary containers. A Group's world-space bounding box is
 * the union of its children's boxes (each child already answers in world
 * space, so enclosing transforms need no special handling here).
 */
class V_GRAPHICS_API Group : public Node {
    V_OBJECT_META_DECL;

  public:
    Group();
    ~Group();

  public:
    /** @brief Adds a child node.
     *
     * The node keeps a reference to the child and detaches it from any
     * previous parent. Adding a node to itself is ignored.
     *
     * @param child Child to add.
     */
    void addChild(intrusive_ptr<Node> child);

    /** @brief Removes a child node.
     *
     * @param child Child to remove.
     */
    void removeChild(raw_ptr<Node> child);

    /** @brief Gets all child nodes.
     *
     * Returns a copy: use childrenRef() on hot paths (per-frame traversal,
     * picking) to avoid copying the list and bumping every child's reference
     * count.
     */
    std::vector<NodePtr> children() const;

    /** @brief Borrows the child list without copying it.
     *
     * The reference is valid until the group's children change, so it must not
     * be held across an addChild() / removeChild().
     *
     * @return The children, in insertion order.
     */
    const std::vector<NodePtr>& childrenRef() const noexcept;

    /** @brief Computes the world-space bounding box of this subtree.
     *
     * Union of the children's world-space boxes (empty when childless).
     * Containers derive their bound from their children, so a subclass that
     * wants a different extent must contribute it through a leaf instead of
     * overriding this (the per-frame bounds pass unions children directly).
     *
     * @return World-space AABB of this subtree.
     */
    Aabbd boundingBox() const override;

  private:
    std::vector<NodePtr> children_;
};

using GroupPtr = intrusive_ptr<Group>;

V_GRAPHICS_NS_END
