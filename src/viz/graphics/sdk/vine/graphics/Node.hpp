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
 * @details
 * - The OSG/vsg-style base of the scene hierarchy: Group aggregates children, MatrixTransform places a
 *   subtree with a local matrix, StateNode applies render state, Geometry carries vertex data. Node itself
 *   provides only the shared identity - name, subtree visibility / opacity, the parent link, world-matrix /
 *   bounds queries - and is NOT a container: it holds no transform of its own (attach children through Group,
 *   place a subtree through MatrixTransform). Its boundingBox() answers in world space at any depth, by
 *   walking the parent chain.
 * - WHAT A NODE PASSES DOWN (the fold rules; the full table is in docs/guides/graphics.md):
 *   - visibility is a HARD GATE, not a folded value: an invisible node takes its whole subtree out of the
 *     collection, the ray walks and the scene's box (see isVisible);
 *   - opacity MULTIPLIES: scene x every ancestor x the node's own, clamped to [0, 1] at the drawn leaf
 *     (see opacity);
 *   - layers and drawOrder take the NEAREST declaration: a node that declares its own replaces what it
 *     inherited, and clearing goes back to inheriting (see setLayers / setDrawOrder).
 * - BOUNDS ARE CACHED PER NODE (see .ai/design/graphics-scene-graph.md §10.2): the collection walk keeps each
 *   subtree's box in the node's OWN frame, so a camera move recomputes nothing and a culled subtree costs
 *   one transformed box instead of a walk. The cache lives by ANNOUNCEMENT: every built-in mutation
 *   (MatrixTransform::setMatrix, Group::addChild / removeChild, the geometry setters) calls
 *   invalidateBounds(), and a node whose placement or extent is computed OUTSIDE those paths must call it
 *   too. A node that answers only in world space (an overridden boundingBox()) is left uncached - its
 *   ancestors compute from scratch, which is always sound (see subtreeBounds).
 */
class VN_GRAPHICS_API Node : public Object, public RefCounted<Node> {
    VN_OBJECT_META_DECL;

  public:
    /** @brief Constructs a node: visible, fully opaque, unattached. */
    Node();
    ~Node();

  public:
    /** @brief Gets the node name.
     *
     * @return The name (empty when never set - names exist for lookup and debugging, not for identity).
     */
    String name() const;

    /** @brief Sets the node name.
     *
     * @param name New node name.
     */
    void setName(const String& name);

    /** @brief Gets whether this subtree is visible.
     *
     * @details
     * - This node's OWN flag, not a folded value: a hidden ancestor is not reported here, it is a gate
     *   applied while walking.
     * - The gate is absolute: an invisible node leaves the collection (nothing below draws), the ray walks
     *   (nothing below is pickable) and Scene::boundingBox (nothing below is measured).
     *
     * @return true when this node's own flag is set.
     */
    bool isVisible() const;

    /** @brief Sets whether this subtree is visible: false takes the whole subtree out of the picture.
     *
     * @details
     * - The "temporarily off" toggle (a hide, an LOD swap); "which part of the content this is" is the
     *   layer declaration instead, and the two are independent (see setLayers).
     * - The scene has the same switch at the top of the tree (Scene::setVisible).
     *
     * @param visible false hides this node and every node below it.
     */
    void setVisible(bool visible);

    /** @brief Gets the layers this subtree declares.
     *
     * @details
     * - This node's OWN declaration, not the resolved one: nullopt means "this node says nothing", so its
     *   subtree is in the layer its ancestors declared (kDefaultLayer when nobody did).
     * - Resolution happens once, in the collection walk - which is why there is no up-walking spelling
     *   here; "why was this not drawn" is asked by a pass and answered while collecting (see
     *   Scene::collectRenderCommands and .ai/design/graphics-layers.md).
     *
     * @return The declared layer set, or nullopt when this node declares none.
     */
    [[nodiscard]] std::optional<std::uint32_t> layers() const;

    /** @brief Declares which layers this subtree belongs to.
     *
     * @details
     * - A pass draws the content whose layers intersect its mask (see RenderPass::setLayerMask) - this is
     *   how content is kept out of a pass that draws other layers (a shadow pass drawing only casters; two
     *   passes showing different parts of one scene). Independent of visibility.
     * - It covers the whole subtree and the NEAREST declaration wins, exactly like the state a StateNode
     *   passes down; clearLayers() goes back to inheriting.
     *
     * @param layers Layer set (0 means "in no layer", so no pass draws this subtree - a legal authoring
     *               state, not an error; a node that declares none is in kDefaultLayer).
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
     * @details
     * - WITHIN one pass, and only there: a higher order is drawn LATER - how content that must land ON TOP
     *   (a selection outline, a decal, a highlight overlay) is expressed. It never moves content between
     *   passes: which passes exist and in what order they run is the recipe's (`PassDesc::order` /
     *   `PipelineStage`).
     * - The order is the OUTER sort key of the pass's draw list, so a declared order leapfrogs the default
     *   batches (order 1 is drawn after every draw at order 0, opaque or not); INSIDE one order the usual
     *   picture holds - opaque front-to-back, then transparent back-to-front (see
     *   Scene::collectRenderCommands) - which keeps alpha ordering intact among content that shares it.
     * - It covers the whole subtree and the NEAREST declaration wins, like the layer declaration beside it;
     *   clearDrawOrder() goes back to inheriting.
     *
     * @param order Sort key of this subtree's draws inside one pass (0 is the default; higher = later).
     */
    void setDrawOrder(int order);

    /** @brief Withdraws this node's own draw-order declaration, so its subtree inherits again. */
    void clearDrawOrder();

    /** @brief Gets this node's OWN opacity multiplier in [0, 1].
     *
     * @details
     * - Not the folded value: a drawable's effective opacity is scene.opacity() x every ancestor's
     *   opacity() x its own, clamped to [0, 1] at the drawn leaf (see RenderCommand::opacity).
     * - The engine's only transparency channel: built-in forward shading writes it as the fragment
     *   alpha, and a material does not contribute (a shared material's alpha would make every drawable
     *   using it translucent).
     *
     * @return This node's own multiplier.
     */
    float opacity() const;

    /** @brief Sets this node's own opacity multiplier; it multiplies down the subtree (see opacity()).
     *
     * @param opacity Multiplier in [0, 1].
     */
    void setOpacity(float opacity);

    /** @brief Gets the parent node.
     *
     * @return The parent, or null for a root node (a child does not own its parent).
     */
    raw_ptr<Node> parent() const;

    /** @brief Computes the world-space bounding box of this subtree.
     *
     * @details
     * - Every node answers in world space: a Geometry's box is the bound of its vertex data transformed by
     *   the enclosing MatrixTransforms; Group / MatrixTransform union their children's boxes (which are
     *   already world-space); base Node (never a renderable or container) answers empty.
     * - Visibility is NOT folded here - a hidden node still answers (the pure geometry query).
     *   Scene::boundingBox is the one that answers for the VISIBLE content: it prunes hidden subtrees at
     *   any depth.
     *
     * @return World-space AABB of this subtree.
     */
    virtual Aabbd boundingBox() const;

    /** @brief Gets the accumulated world matrix of this node.
     *
     * @details
     * - The product, from the scene root downwards, of every enclosing MatrixTransform's local matrix
     *   (identity for every other node); for a MatrixTransform it includes its own. For a leaf Geometry it
     *   places the local vertex data in world space.
     * - This walks the ancestor chain: a traversal should accumulate top-down with localTransformMatrix()
     *   (one product per node) instead of calling this once per node (one product per ancestor).
     *
     * @return World-space transform.
     */
    Mat4d worldMatrix() const;

    /** @brief Gets this node's own local matrix contribution (identity unless a MatrixTransform).
     *
     * @details
     * - Public so a traversal can accumulate world matrices top-down:
     *   `child_world = parent_world * child.localTransformMatrix()`.
     *
     * @return This node's local matrix.
     */
    virtual Mat4d localTransformMatrix() const;

    /** @brief Announces that this subtree's bound may have changed.
     *
     * @details
     * - The collection caches every subtree's bound in the node's own frame, and a cached box is served
     *   until something announces a change: this bumps the node's stamp AND every ancestor's (a box unions
     *   its children's). A change ABOVE cannot stale a box below - a subtree box lives in the node's own
     *   frame (see subtreeBounds).
     * - Every built-in mutation announces through it; code the engine cannot see that changes a local
     *   matrix or an extent must call it, exactly like a raw data writer announces through
     *   Geometry::bumpRevision().
     */
    void invalidateBounds() noexcept;

    /** @brief Gets this subtree's box in THIS node's own frame, served from the per-node cache.
     *
     * @details
     * - "Own frame" is the frame this node's worldMatrix() maps to world: a container's box is its
     *   children's boxes transformed by each child's localTransformMatrix(), so a change above cannot
     *   stale the cache - only a change inside the subtree can, which invalidateBounds() announces.
     *
     * @param box Receives the box when one is known (possibly genuinely empty).
     * @return true when the box is known; false when the subtree cannot be answered in this frame - what a
     *         node that only overrides boundingBox() (world space) yields, and what a plain Node (no
     *         content, no children) avoids by answering empty. A false answer tells the walk to compute
     *         from scratch, which stays sound for any node class.
     */
    [[nodiscard]] bool subtreeBounds(Aabbd& box) const;

    /** @brief Gets how many times this subtree's box was RECOMPUTED from its children.
     *
     * @details
     * - The witness of the cache above, the way Geometry::localBoundsComputationCount() witnesses its data
     *   box: a steady scene (a moving camera included - world boxes do not depend on it) recomputes
     *   nothing after the first collect, and a count that keeps rising while nothing announces a change
     *   means a stamp is too eager.
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
 * @details
 * - The one spelling of "place an axis-aligned box": all eight corners are transformed and the result is
 *   re-boxed. A node's own `boundingBox()` uses it, and so does the collection walk with the matrix IT
 *   accumulated - the two must agree (a test pins `boundingBox() == transformBox(localBounds(),
 *   worldMatrix())` on a nested chain), which is why this helper lives here rather than once per caller.
 *
 * @param local Box in the node's own frame.
 * @param world World transform to place it with.
 * @return World-space AABB (empty when @p local is empty).
 */
[[nodiscard]] VN_GRAPHICS_API Aabbd transformBox(const Aabbd& local, const Mat4d& world);

VN_GRAPHICS_NS_END
