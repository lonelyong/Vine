#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Node.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Scene-graph node that aggregates child nodes.
 *
 * @details
 * - The OSG/vsg-style container: it adds no state or transform of its own, only children (nested Groups,
 *   MatrixTransforms, AutoTransforms, StateNodes and leaf Geometries), and keeps each child's parent link in
 *   sync. Kinds that
 *   apply something to their subtree (a transform, render state) derive from Group so they stay ordinary
 *   containers.
 * - Its world-space box is the union of its children's boxes (each child already answers in world space, so
 *   enclosing transforms need no special handling here).
 */
class VN_GRAPHICS_API Group : public Node {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs an empty group (no children). */
    Group();
    ~Group();

  public:
    /** @brief Adds a child node.
     *
     * @details
     * - The group keeps a reference and detaches the child from any previous parent, so the graph stays a
     *   tree. An add that would close a cycle (the child is this node or one of its ancestors) is rejected
     *   and the child is left untouched: a cyclic graph makes every tree traversal (command collection,
     *   bounds, picking, find) recurse forever.
     *
     * @param child Child to add (null is ignored).
     */
    void addChild(intrusive_ptr<Node> child);

    /** @brief Removes a child node.
     *
     * @param child Child to remove.
     */
    void removeChild(raw_ptr<Node> child);

    /** @brief Borrows the child list.
     *
     * @details
     * - BORROWED, never a copy: the list is walked once per pass by command collection, bounds and
     *   picking, and copying it there would bump every child's reference count on every frame.
     * - The reference is valid until the group's children change: do not hold it across an
     *   addChild() / removeChild() (a caller that must keep the list copies it explicitly).
     *
     * @return The children, in insertion order.
     */
    const std::vector<NodePtr>& children() const noexcept;

    /** @brief Computes the world-space bounding box of this subtree (the union of the children's; empty when
     * childless).
     *
     * @details
     * - Containers derive their bound from their children: a subclass that wants a different extent must
     *   contribute it through a leaf instead of overriding this (the per-frame bounds pass unions children
     *   directly).
     *
     * @return World-space AABB of this subtree.
     */
    Aabbd boundingBox() const override;

  private:
    std::vector<NodePtr> children_;
};

using GroupPtr = intrusive_ptr<Group>;

VN_GRAPHICS_NS_END
