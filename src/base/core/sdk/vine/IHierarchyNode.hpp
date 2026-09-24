#pragma once

#include "core_global.hpp"

#include <cstddef>
#include <concepts>

VN_CORE_NS_BEGIN

/**
 * @brief Interface for a node in a tree.
 *
 * The concrete node type T is supplied as the template argument (curiously
 * recurring template pattern), so the interface can return T* instead of a
 * base-class pointer and callers never need to downcast.
 */
template <typename T>
class IHierarchyNode
{
  public:
    /** @brief Default virtual destructor. */
    virtual ~IHierarchyNode() = default;

  public:
    /**
     * @brief Returns the parent node.
     *
     * @return The parent node, or null for a root node.
     */
    virtual T* parent() const noexcept = 0;

    /**
     * @brief Returns the number of child nodes.
     *
     * @return The child count.
     */
    virtual std::size_t childCount() const noexcept = 0;

    /**
     * @brief Returns the child node at the given index.
     *
     * @param index The child index.
     * @return The child node, or null when the index is out of range.
     */
    virtual T* childAt(std::size_t index) const = 0;

    /**
     * @brief Checks whether this node is an ancestor of the given node.
     *
     * Walks up the parent chain of node and compares against this node. A
     * node is not considered its own ancestor.
     *
     * @param node The node to test.
     * @return true when this node is an ancestor of node.
     */
    bool isAncestorOf(const T* node) const noexcept
    {
        if (!node || node == static_cast<const T*>(this)) {
            return false;
        }

        for (const T* p = node->parent(); p; p = p->parent()) {
            if (p == this) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Checks whether this node is a descendant of the given node.
     *
     * Walks up this node's parent chain and compares against node. A node is
     * not considered its own descendant.
     *
     * @param node The node to test.
     * @return true when this node is a descendant of node.
     */
    bool isDescendantOf(const T* node) const noexcept
    {
        if (!node) {
            return false;
        }

        for (const T* p = parent(); p; p = p->parent()) {
            if (p == node) {
                return true;
            }
        }

        return false;
    }
};

/**
 * @brief Concept for types that provide the node interface of
 *        IHierarchyNode<T>.
 */
template <typename T>
concept Hierarchical = requires(const T& t, std::size_t index) {
    { t.parent() } -> std::same_as<T*>;
    { t.childCount() } -> std::convertible_to<std::size_t>;
    { t.childAt(index) } -> std::same_as<T*>;
};

VN_CORE_NS_END
