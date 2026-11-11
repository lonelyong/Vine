#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

#include <vine/graphics/graphics_global.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::geom::spatial::Aabbd;
using vn::math::Mat4d;
using vn::math::Vec3d;
using vn::math::Vec4d;

namespace detail
{

/**
 * @brief The bake and the select of ONE collection, shared by every producer of a draw list.
 *
 * @details
 * - WHY IT IS A HEADER. `Scene` builds the table by walking the live graph; the render world builds the same table
 *   from values it was told (see `.ai/design/graphics-threading.md`). Both must run the SAME fold rules, so the
 *   table type and the two passes live in one place instead of two implementations that would drift.
 * - WHO FILLS IT. `bakeSubtree` (a graph walk) here, and any producer that can answer for one node's own facts;
 *   `collectFromBake` is the only reader, and it is the whole VIEW half of a collection.
 * - WHAT IT DOES NOT OWN: the memo, the frames, the scene. Those belong to whoever is asking.
 */

/**
 * @brief View frustum defined by six planes, used for culling.
 *
 * Planes are stored in the order: left, right, bottom, top, near, far.
 * Each plane is a 4-vector (a, b, c, d) satisfying
 * a*x + b*y + c*z + d = 0 in world space.
 */
class Frustum {
  public:
    /** @brief Extracts the frustum from a combined view-projection matrix.
     *
     * @param vp Combined projection * view matrix.
     * @return Frustum with normalized plane equations.
     */
    static Frustum fromViewProjection(const Mat4d& vp)
    {
        Frustum     f;
        const auto  row = [&vp](int r) { return Vec4d(vp.element(r, 0), vp.element(r, 1), vp.element(r, 2), vp.element(r, 3)); };
        const Vec4d r0  = row(0);
        const Vec4d r1  = row(1);
        const Vec4d r2  = row(2);
        const Vec4d r3  = row(3);
        f.planes_[0]    = r3 + r0; // left
        f.planes_[1]    = r3 - r0; // right
        f.planes_[2]    = r3 + r1; // bottom
        f.planes_[3]    = r3 - r1; // top
        f.planes_[4]    = r3 + r2; // near
        f.planes_[5]    = r3 - r2; // far
        for (auto& p : f.planes_) {
            const Vec3d  n(p.x, p.y, p.z);
            const double len = n.length();
            if (len > 1e-12) {
                p.x /= len;
                p.y /= len;
                p.z /= len;
                p.w /= len;
            }
        }
        return f;
    }

    /** @brief Tests whether a box lies fully outside the frustum.
     *
     * Uses the p-vertex test: the box is outside when its corner most in the
     * direction of a plane normal is still behind that plane.
     *
     * @param box World-space axis-aligned bounding box.
     * @return true when the box is fully outside the frustum.
     */
    bool isOutside(const Aabbd& box) const
    {
        for (const auto& p : planes_) {
            const Vec3d n(p.x, p.y, p.z);
            const Vec3d pos{ (n.x >= 0.0) ? box.max().x : box.min().x, (n.y >= 0.0) ? box.max().y : box.min().y, (n.z >= 0.0) ? box.max().z : box.min().z };
            if (n.dot(pos) + p.w < 0.0) {
                return true;
            }
        }
        return false;
    }

  private:
    std::array<Vec4d, 6> planes_{};
};

/**
 * @brief Per-traversal cache of the world-space bounds of the visited nodes.
 *
 * Group::boundingBox() unions its children's boxes, and Geometry::boundingBox()
 * places the data box in world space. Asking every visited node for its bound
 * therefore re-walks the whole subtree below every container — quadratic in the
 * node count for deep / broad assemblies — while the culling decision only
 * needs each node's box once. This map makes one collection pass visit every
 * subtree exactly once.
 *
 * THE SECOND LAYER IS PER NODE AND OUTLIVES THE PASS (see Node::subtreeBounds):
 * a subtree's box in its OWN frame is cached on the node and survives camera
 * moves, so a pass costs one transform per visited node and a culled subtree is
 * closed without descending — invalidated only by announcements
 * (Node::invalidateBounds). This map still earns its keep for the nodes that
 * cannot answer that way (a custom leaf makes its ancestors compute from
 * scratch) and as the per-pass dedup the tree always relied on.
 *
 * The traversal is a tree (Group::addChild re-parents a node, so a node has one
 * parent), which is what makes a node-keyed cache valid: within one pass a node
 * has exactly one world matrix.
 */
class BoundsCache {
  public:
    /** @brief Returns the world-space bound of @p node (computed once). */
    const Aabbd& worldBound(const Node* node, const Mat4d& world)
    {
        const auto cached = bounds_.find(node);
        if (cached != bounds_.end()) {
            return cached->second;
        }
        Aabbd box   = Aabbd::empty();
        Aabbd local = Aabbd::empty();
        if (node->subtreeBounds(local)) {
            // The subtree's box in its own frame, cached per node: a camera move recomputes nothing,
            // and a culled subtree is closed without visiting anything below it. The one edit that can
            // stale it is a change INSIDE the subtree, announced through Node::invalidateBounds().
            box = local.transformed(world);
        }
        else if (const auto* group = obj_cast<Group>(node)) {
            // The subtree could not answer in its own frame (a node below answers only in world space,
            // e.g. a custom leaf): compute from scratch, exactly as collection always did. Children
            // that CAN answer still use their caches through this same entry point — including the
            // exact Geometry fast path (its local data box placed by the accumulated matrix; a Geometry
            // SUBCLASS keeps answering through its own boundingBox(), see Node::subtreeBounds).
            for (const auto& child : group->children()) {
                box.expandBy(worldBound(child.get(), world * child->localTransformMatrix()));
            }
        }
        else {
            // A leaf that is not the engine's own (or is a subclass of one) answers for itself.
            box = node->boundingBox();
        }
        return bounds_.emplace(node, box).first->second;
    }

  private:
    std::unordered_map<const Node*, Aabbd> bounds_;
};

/**
 * @brief The state a node INHERITS from the nodes above it.
 *
 * The fold is computed once on the way DOWN the traversal, which is where the ancestors already are. The
 * three lookups a leaf needs (the folded render state, the nearest ancestor material, the nearest ancestor
 * program) each used to walk back up to the root — per geometry, per pass, per frame — and one of them
 * (collectRenderState) allocated a vector of the whole ancestor chain to do it. Descending, a StateNode
 * merges its own state into what it inherited and replaces the material / program it carries, which is
 * exactly what the three up-walking helpers compute: the deepest StateNode wins for the single-valued
 * items, and the folded state merges root-to-leaf.
 *
 * The layers a subtree belongs to fold the same way and for the same reason: the nearest declaration wins,
 * and what arrives here is already RESOLVED (kDefaultLayer when nobody declared any), because the mask test
 * needs one answer per node, not a chain to walk.
 *
 * @note The up-walking helpers stay in StateNode.hpp/.cpp — they are the SDK's public spelling of the same
 *       rule, used by hosts — and this is their top-down form. A test pins that the two agree on a nested
 *       tree, because two implementations of one rule are a drift waiting to happen. Layers deliberately
 *       have only this form (see Node::layers): the answer is needed by the walk, and no host asks a node
 *       "which layer are you in" - that question belongs to the pass doing the drawing.
 */
struct InheritedState {
    RenderState            state;                      // Fold of every StateNode above this node (deeper overrides).
    raw_ptr<MaterialBase>  material;                   // Nearest StateNode material above this node, or null.
    raw_ptr<ShaderProgram> program;                    // Nearest StateNode program above this node, or null.
    std::uint32_t          layers     = kDefaultLayer; // Nearest layer declaration above, resolved (see Node::setLayers).
    int                    draw_order = 0;             // Nearest draw-order declaration above (see Node::setDrawOrder).
};

/**
 * @brief THE BAKED HALF OF ONE SCENE: every node's view-INDEPENDENT facts, in walk order.
 *
 * @details
 * - WHY IT EXISTS. A collection answers two questions with one walk: "what does this graph contain, placed how"
 *   (a function of the GRAPH) and "which of it is inside THIS view, in what order" (a function of the CAMERA). The
 *   memo is keyed by the view, so before this table a camera move threw the graph half away and recomputed every
 *   world matrix, every world box and every folded state - measured at 13.8 ms per collection for 2000 drawables
 *   (SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera). The graph half now lives here, keyed by the root
 *   subtree's content epoch, so a moving camera reads it instead of recomputing it.
 * - WHY AN ARRAY AND NOT A MAP. `bakeSubtree` and `collectFromBake` are driven by the same order (children in
 *   order, same pruning of invisible subtrees), so the cursor IS the identity: no node-keyed map, no hashing.
 *   `end` is what keeps that alignment when a view prunes a subtree - the select step jumps straight past it.
 * - WHAT IS NOT HERE. Anything a camera decides: the frustum test, the sort key, the final list. And any node whose
 *   placement may depend on the view (Node::viewDependence) - those subtrees are marked and handed back to the
 *   original walk per view (`fallbacks`), which is the only path that knows how to resolve them.
 * - WHY THE WORLD BOX IS NOT HERE EITHER. A geometry's box is a function of its BUFFER's revision, and a writer
 *   announces that on the buffer (`Buffer::edit()` / `setRevision`) - NOT on the node, so the graph epoch does not
 *   move. Baking the box would cull with a stale one until some unrelated node change, which is why the select step
 *   asks BoundsCache for it instead: the transform is cheap, and the LOCAL box is already cached per node by its
 *   own stream revision (see Geometry::localBounds).
 * - LIFETIME. It holds SHARES of the content it names (geometry, material, program), exactly like the memo does, so
 *   a baked entry can never name a freed object; it is dropped when the graph announces a change (the epoch in
 *   Scene::memo_epoch_) or when the scene invalidates itself (Scene::invalidateContent).
 * - The three folded fields (`state` / `material` / `program`) are read at a LEAF entry only. Keeping them in the
 *   same record as the containers costs memory rather than a second array; if a scene with >1e5 nodes ever shows
 *   up, splitting the leaf payload out is the change to make (see the backlog entry for this table).
 */
struct SceneBake {
    /// The row index meaning "this row has no parent" (the table's first row).
    inline static constexpr std::uint32_t kNoParent = 0xFFFFFFFFU;

    /// One node's resolved facts. `node` is kept because two questions are still per node and cached on it: the
    /// layer union (`subtreeLayerUnion`) and, for a view-dependent subtree, its per-view placement.
    struct Entry {
        const Node*     node{ nullptr };
        Mat4d           local{};                              // This node's OWN placement (its parent's frame).
        std::uint32_t   parent{ kNoParent };                  // The row this node hangs from (0-based; kNoParent = the root).
        Mat4d           world{};                              // Resolved placement (meaningful when !view_dependent).
        std::uint32_t   end{ 0 };                             // One past this node's subtree.
        std::uint32_t   layers{ kDefaultLayer };              // Resolved layers at this node (see InheritedState).
        std::int32_t    draw_order{ 0 };                      // Folded draw order.
        float           opacity{ 1.0F };                      // Accumulated opacity INCLUDING this node (unclamped).
        RenderState     state{};                              // Leaf only: the folded authored state.
        MaterialBasePtr material{};                           // Leaf only: nearest StateNode material.
        ShaderProgramPtr program{};                           // Leaf only: nearest StateNode program.
        GeometryPtr     geometry{};                           // Leaf only: null for a container.
        std::uint32_t   first_primitive{ 0 };
        std::uint32_t   primitive_count{ 0 };
        bool            view_dependent{ false };              // This node or an ancestor resolves its placement per view.
    };

    /// The context a view-dependent subtree is handed back to the walk with (see collectFromBake). One per run -
    /// the TOPMOST view-dependent node - so a run is resolved once, by the code that already does it correctly.
    struct Fallback {
        std::uint32_t index{ 0 };        // The entry the run starts at.
        const Node*   node{ nullptr };
        Mat4d         world{};           // The run's TOP node's own world matrix (the walk's own convention).
        InheritedState inherited{};
        float         parent_opacity{ 1.0F };
    };

    std::vector<Entry>    entries;
    std::vector<Fallback> fallbacks; // Ascending by `index` (the bake walks in order).
};

std::uint32_t bakeSubtree(const Node*           node,
                          const Mat4d&          world,
                          float                 parent_opacity,
                          const InheritedState& inherited,
                          bool                  under_view_dependent,
                          SceneBake&            bake,
                          std::uint32_t         parent = SceneBake::kNoParent);

/**
 * @brief Re-resolves ONE row's subtree after that row's own placement changed: the one fold a producer runs on an edit.
 *
 * @details
 * - It walks the subtree of @p index (a row of @p bake) and re-multiplies world matrices top-down; nothing above the
 *   row is touched, which is what makes a joint edit cost the joint's subtree instead of the whole graph.
 * - A view-dependent row (or one below it) is skipped: its placement is resolved per view by the walk that knows how
 *   to place it (see collectFromBake's fallbacks), so a row cannot answer for it.
 * - Folded values are NOT recomputed: this is the placement-only fold. A change to visibility, opacity, layers, draw
 *   order or state re-derives its subtree from its own facts instead (a different fold, deliberately not mixed in).
 *
 * @param bake   The table containing @p index.
 * @param index  The row whose own placement changed.
 * @return How many rows were re-placed (the row itself included; 0 for a view-dependent row).
 */
std::uint32_t refoldSubtree(SceneBake& bake, std::uint32_t index);

/**
 * @brief Reorders @p commands into the draw order a frame draws them in, in place.
 *
 * @details
 * - The order is the CONTENT's declared draw order (see Node::setDrawOrder), then opaque before transparent, then
 *   front-to-back for opaque and back-to-front for transparent. Ties keep the collection order.
 * - It exists as its own step because BOTH producers of a draw list need it and neither may own a copy: the scene's
 *   collection and the render world's (see `.ai/design/graphics-threading.md`).
 *
 * @param commands Commands to reorder (replaced by their sorted order).
 * @param eye      World-space eye point the distances are measured from.
 */
void sortCollected(std::vector<RenderCommand>& commands, const Vec3d& eye);

void collectFromBake(const SceneBake&            bake,
                     const Frustum&              frustum,
                     const CollectView&          view,
                     std::uint32_t               layer_mask,
                     BoundsCache&                bounds,
                     bool&                       viewport_used,
                     std::vector<RenderCommand>& out);

} // namespace detail

VN_GRAPHICS_NS_END
