#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <optional>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/String.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Rect3.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::math::Mat4d;
using vn::math::Aabbd;

class Group;

/// Every layer: the default on the pass side (a mask that states nothing draws every layer), so a scene whose
/// nodes say nothing about layers is drawn exactly as it was before layers existed.
inline constexpr std::uint32_t kAllLayers = 0xFFFFFFFFu;

/// The layer a node is in when it declares none (see Node::layers): the ordinary picture.
///
/// A DEFAULT BIT rather than "every layer", which is the choice three.js / Unity / Qt3D made and the one this
/// SDK needs: a selective mask exists to LEAVE CONTENT OUT (the shadow pass drawing only casters), and with
/// "undeclared = every layer" every non-caster would have to be declared before it could be excluded. Here,
/// declaring the few items that ARE in a named layer is enough.
inline constexpr std::uint32_t kDefaultLayer = 1u << 0;

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
 *
 * BOUNDS ARE CACHED PER NODE (see .ai/design/graphics-scene-graph.md §10.2): the collection walk keeps
 * each subtree's box in the node's OWN frame, so a camera move recomputes nothing and a culled subtree
 * costs one transformed box instead of a walk. The cache lives by ANNOUNCEMENT: every built-in mutation
 * (MatrixTransform::setMatrix, Group::addChild / removeChild, the geometry setters) calls
 * invalidateBounds(), and a node whose placement or extent is computed OUTSIDE those paths must call it
 * too - that is its explicit invalidation hook. A node that answers only in world space (an overridden
 * boundingBox()) is left uncached and its ancestors compute from scratch, which is always sound (see
 * subtreeBounds).
 */
class VN_GRAPHICS_API Node : public Object, public RefCounted<Node> {
    VN_OBJECT_META_DECL;

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

    /** @brief Gets the layers this subtree declares.
     *
     * This node's OWN declaration, not the resolved one: nullopt means "this node says nothing", so its
     * subtree is in the layer its ancestors declared (kDefaultLayer when nobody did). The resolution
     * happens once, in the collection walk, which is why there is no second (up-walking) spelling here -
     * the question "why was this not drawn" is asked by a PASS, and it is answered while collecting (see
     * Scene::collectRenderCommands and .ai/design/graphics-layers.md).
     *
     * @return The declared layer set, or nullopt when this node declares none.
     */
    [[nodiscard]] std::optional<std::uint32_t> layers() const;

    /** @brief Declares which layers this subtree belongs to.
     *
     * A pass draws the content whose layers intersect its mask (see RenderPass::setLayerMask), so this is
     * how content is kept out of a pass that draws other layers: a shadow pass drawing only casters, two
     * passes showing different parts of one scene. Independent of visibility - setVisible(false) is
     * "temporarily off", this is "which layers it is a part of".
     *
     * The declaration covers the whole subtree and the NEAREST declaration wins, exactly like the shading
     * state a StateNode passes down: a child that declares its own set replaces what it inherited, and
     * clearLayers() puts it back to inheriting.
     *
     * @param layers Layer set (a node that declares none is in kDefaultLayer; 0 means "in no layer", so no
     *               pass draws this subtree - a legal authoring state, not an error).
     */
    void setLayers(std::uint32_t layers);

    /** @brief Withdraws this node's own layer declaration, so its subtree inherits its ancestors' again. */
    void clearLayers();

    /** @brief Gets the draw order this subtree declares.
     *
     * This node's OWN declaration (nullopt when it says nothing, so its subtree inherits - see setDrawOrder).
     *
     * @return The declared order, or nullopt when this node declares none.
     */
    [[nodiscard]] std::optional<int> drawOrder() const;

    /** @brief Declares where this subtree's draws sit in the order of the pass that draws them.
     *
     * WITHIN one pass, and only there: a higher order is drawn LATER, which is how content that must land ON TOP
     * of what is already in the target is expressed - a selection outline, a decal, a highlight overlay. It does
     * not move anything between passes: which passes exist and in what order they run is the recipe's
     * (`PassDesc::order` / `PipelineStage`), and this value has no effect on it.
     *
     * The order is the OUTER sort key of the list a pass draws, so a declared order really does leapfrog the
     * default batches: content at order 1 is drawn after every draw at order 0, opaque or not. INSIDE one order
     * the picture is what it always was - opaque front-to-back first, then transparent back-to-front (see
     * Scene::collectRenderCommands) - which is what keeps a higher order from breaking alpha ordering among the
     * content that shares it.
     *
     * The declaration covers the whole subtree and the NEAREST declaration wins, exactly like the layer
     * declaration beside it: a child that declares its own order replaces what it inherited, and clearDrawOrder()
     * puts it back to inheriting.
     *
     * @param order Sort key of this subtree's draws inside one pass (0 is the default; higher = later).
     */
    void setDrawOrder(int order);

    /** @brief Withdraws this node's own draw-order declaration, so its subtree inherits again. */
    void clearDrawOrder();

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
     * Visibility is NOT folded here: a hidden node still answers with its subtree's box - this is the
     * pure geometry query. Scene::boundingBox is the one that answers for the VISIBLE content: it prunes
     * hidden subtrees at any depth.
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

    /** @brief Announces that this subtree's bound may have changed.
     *
     * The scene's collection caches every subtree's bound in the node's own frame, and a cached box is
     * served until something announces a change: this call bumps the node's bounds stamp AND every
     * ancestor's (an ancestor's box is a union of its children's, so a change here reaches up; a change
     * ABOVE cannot stale a box below, because a subtree box lives in the node's own frame - see
     * subtreeBounds). Every built-in mutation announces through it; a node whose local matrix or extent
     * is computed by code the engine cannot see must call this after changing, exactly like a raw data
     * writer announces through Geometry::bumpRevision().
     */
    void invalidateBounds() noexcept;

    /** @brief Gets this subtree's box in THIS node's own frame, served from the per-node cache.
     *
     * "Own frame" is the frame this node's worldMatrix() maps to world: a container's box is its
     * children's boxes transformed by each child's localTransformMatrix(), so a change above the node
     * cannot stale the cache - only a change inside the subtree can, which invalidateBounds() announces.
     *
     * @param box Receives the box when one is known (possibly genuinely empty).
     * @return true when the box is known; false when the subtree cannot be answered in this frame - what
     *         a node that only overrides boundingBox() (world space) yields, and what a plain Node (no
     *         content, no children) avoids by answering empty. A false answer tells the walk to compute
     *         from scratch, which stays sound for any node class.
     */
    [[nodiscard]] bool subtreeBounds(Aabbd& box) const;

    /** @brief Gets how many times this subtree's box was RECOMPUTED from its children.
     *
     * The witness of the cache above, the way Geometry::localBoundsComputationCount() witnesses its data
     * box: a steady scene (a moving camera included - world boxes do not depend on it) recomputes
     * nothing after the first collect, and a count that keeps rising while nothing announces a change
     * means a stamp is too eager.
     *
     * @return Number of recomputes since construction.
     */
    [[nodiscard]] std::uint64_t boundsRecomputeCount() const noexcept;

  private:
    String name_;
    bool visible_ = true;
    /// This node's own layer declaration, or nullopt when it says nothing and inherits (see setLayers).
    std::optional<std::uint32_t> layers_;
    /// This node's own draw order, or nullopt when it says nothing and inherits (see setDrawOrder).
    std::optional<int> draw_order_;
    float opacity_ = 1.0f;
    raw_ptr<Node> parent_ = nullptr;

    // The bounds cache (see invalidateBounds / subtreeBounds). Mutable for the same reason
    // Geometry::local_bounds_ is (see its note): collection is single-threaded by contract, and the
    // cache is a pure function of the stamps.
    std::uint64_t         bounds_stamp_{0};      ///< Bumped by invalidateBounds() on this node and every ancestor.
    mutable Aabbd         cached_bounds_;        ///< Subtree box in this node's frame, valid at cached_stamp_.
    mutable std::uint64_t cached_stamp_{0};      ///< bounds_stamp_ when the box was computed.
    mutable bool          cached_known_{false};  ///< False until a recompute succeeded (or after one failed).
    mutable std::uint64_t bounds_recomputes_{0}; ///< Recomputation witness (see boundsRecomputeCount).

    friend class Group;
};

using NodePtr = intrusive_ptr<Node>;

/**
 * @brief Places a local-space AABB with @p world into a world-space AABB.
 *
 * The one spelling of "place an axis-aligned box": all eight corners are transformed and the result is
 * re-boxed. A node's own `boundingBox()` uses it, and so does the scene's collection walk with the matrix
 * IT accumulated - the two must agree (a test pins `boundingBox() == transformBox(localBounds(),
 * worldMatrix())` on a nested chain), which is why this helper lives here rather than once per caller.
 *
 * @param local Box in the node's own frame.
 * @param world World transform to place it with.
 * @return World-space AABB (empty when @p local is empty).
 */
[[nodiscard]] VN_GRAPHICS_API Aabbd transformBox(const Aabbd& local, const Mat4d& world);

VN_GRAPHICS_NS_END
