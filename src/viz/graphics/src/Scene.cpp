#include <vine/graphics/Scene.hpp>

#include "LayerEpoch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/graphics/AutoTransform.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/LightRig.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Transform3.hpp>

VN_GRAPHICS_NS_BEGIN

using vn::math::Vec3d;
using vn::math::Vec4d;

VN_OBJECT_META_IMPL(Scene, vn::Object);

// The collector's internals: the frustum, the per-pass bound cache, the folded inherited state and the baked half of
// a collection. Named `detail` (not an anonymous namespace) because Scene's header forward-declares the bake type -
// nothing outside this translation unit names any of them.
namespace detail
{

/**
 * @brief Returns the process-wide empty command list (one allocation for "nothing to draw").
 *
 * @return The shared empty list.
 */
std::shared_ptr<const std::vector<RenderCommand>> noCommands()
{
    static const auto kNoCommands = std::make_shared<const std::vector<RenderCommand>>();
    return kNoCommands;
}

/**
 * @brief Returns whether two projection*view matrices are identical.
 *
 * Matrix4x4 has no operator==, and the memo only needs "the same view": exact
 * element equality is the right test because the memo caches a pure function of
 * the view, so a camera that happens to produce the same matrices must reuse
 * the entry (its result is identical by construction).
 *
 * @param lhs First matrix to compare.
 * @param rhs Second matrix to compare.
 * @return true when every element matches.
 */
bool sameView(const Mat4d& lhs, const Mat4d& rhs)
{
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            if (lhs(row, col) != rhs(row, col)) {
                return false;
            }
        }
    }
    return true;
}

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
 * @brief Recursively finds a node by name.
 *
 * @param node Root node to search.
 * @param name Name to find.
 * @return Matching node, or null.
 */
NodePtr findNodeRecursive(const Node* node, const String& name)
{
    if (node == nullptr) {
        return NodePtr();
    }
    if (node->name() == name) {
        return NodePtr(const_cast<Node*>(node));
    }
    if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            NodePtr found = findNodeRecursive(child.get(), name);
            if (found != nullptr) {
                return found;
            }
        }
    }
    return NodePtr();
}

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

/// @brief Bumps every bounds stamp in @p node's subtree (a scene-level invalidation).
///
/// One call per scene edit is rare enough that the O(nodes x depth) propagation invalidateBounds()
/// performs per node is not worth a second bump path.
void invalidateBoundsUnder(Node& node)
{
    node.invalidateBounds();
    if (const auto* group = obj_cast<Group>(&node)) {
        for (const auto& child : group->children()) {
            invalidateBoundsUnder(*child);
        }
    }
}

/**
 * @brief Unions the world-space boxes of a subtree's VISIBLE content the @p layer_mask selects.
 *
 * The scene's box answers for what it would DRAW (see Scene::boundingBox), so an invisible node prunes
 * its whole subtree - the same rule the collection walk (collectNodeCommands below) and the ray walks
 * (RayIntersection) apply. The layers fold down (nearest declaration wins) and the mask is tested where
 * a box is contributed (the drawn node), like the collection: a container stands for structure and is
 * never pruned by the mask. Every other node answers through its own boundingBox(), which stays the
 * purely GEOMETRIC query (see Node::boundingBox) - the selection is this query's own fold, not a change
 * to what a node says about itself. A billboard (AutoTransform) is answered by its own conservative box
 * before the Group recursion: its orientation depends on the drawing camera, so a per-view box here
 * would make the scene bound depend on which pass looked last.
 *
 * @param node             Subtree root to measure.
 * @param inherited_layers Layers @p node inherits from its ancestors (kDefaultLayer at the top).
 * @param layer_mask       Layers this query measures (see Node::setLayers).
 * @return World-space union of the selected visible content, empty when nothing in it is selected.
 */
Aabbd visibleWorldBox(const Node& node, std::uint32_t inherited_layers, std::uint32_t layer_mask)
{
    if (!node.isVisible()) {
        return Aabbd::empty();
    }
    if ((node.subtreeLayerUnion(inherited_layers) & layer_mask) == 0) {
        return Aabbd::empty();
    }
    const std::uint32_t layers = node.layers().value_or(inherited_layers);
    if (const auto* billboard = obj_cast<AutoTransform>(&node)) {
        // A billboard's world box depends on the camera that draws it. Its own answer is the conservative
        // sphere-box, which is a superset of every orientation (and of every layer selection) - a scene
        // bound may be too large, never too small.
        return billboard->boundingBox();
    }
    if (const auto* group = obj_cast<Group>(&node)) {
        Aabbd box = Aabbd::empty();
        for (const auto& child : group->children()) {
            const Aabbd child_box = visibleWorldBox(*child, layers, layer_mask);
            if (child_box.isValid()) {
                box.expandBy(child_box);
            }
        }
        return box;
    }
    // The content selection, taken where the box is contributed (the drawn node): a container stands
    // for structure, so the mask never prunes it (see Scene::collectRenderCommands).
    if ((layers & layer_mask) == 0) {
        return Aabbd::empty();
    }
    return node.boundingBox();
}

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
    /// One node's resolved facts. `node` is kept because two questions are still per node and cached on it: the
    /// layer union (`subtreeLayerUnion`) and, for a view-dependent subtree, its per-view placement.
    struct Entry {
        const Node*     node{ nullptr };
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

/**
 * @brief Recursively collects render commands from a node subtree.
 *
 * Container nodes (Group and its subclasses) are descended into; a leaf
 * Geometry emits one render command baked with its world matrix. Nodes fully
 * outside the frustum (and their subtrees) are culled, and so is a Geometry whose
 * resolved layers do not intersect the mask this picture is drawn with (see
 * Node::setLayers - the test is one bitwise AND per command, and it is taken at
 * the DRAWN node: a mask excludes content, not the structure it sits in). The world matrix is
 * accumulated top-down (one product per node), each node's world bound is
 * computed at most once per pass (see BoundsCache), and so is the state — shading,
 * layers, draw order — a subtree inherits (see InheritedState), so the cost is linear in the node
 * count instead of quadratic.
 *
 * @param node       Root node to traverse.
 * @param world      World matrix of @p node (accumulated by the caller).
 * @param frustum    View frustum for culling.
 * @param view       The view being drawn (for view-dependent nodes, see Node::localToWorldForView).
 * @param opacity    Accumulated opacity of the ancestors.
 * @param inherited  State of the ancestors (see InheritedState).
 * @param layer_mask Layers this picture draws (see Scene::collectRenderCommands).
 * @param bounds     Per-pass bound cache.
 * @param viewport_used Receives true when some node's placement also depends on the device-pixel viewport.
 * @param out        Output command list.
 */
void collectNodeCommands(const Node*                 node,
                         const Mat4d&                world,
                         const Frustum&              frustum,
                         const CollectView&          view,
                         float                       opacity,
                         const InheritedState&       inherited,
                         std::uint32_t               layer_mask,
                         BoundsCache&                bounds,
                         bool&                       viewport_used,
                         std::vector<RenderCommand>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    // The whole subtree can be skipped before its matrix, bounds or visibility below are touched: the union
    // is an over-approximation of the resolved layers below, so a zero intersection is a proof that no
    // drawable in it can match (see Node::subtreeLayerUnion). A positive answer only means "look further".
    if ((node->subtreeLayerUnion(inherited.layers) & layer_mask) == 0) {
        return;
    }
    // A view-dependent node (AutoTransform with auto-rotation or screen-space scaling) supplies its own world
    // matrix for THIS view. The collected list is memoised per view, so the answer is stable for as long as the
    // view is; ViewAndViewport additionally keys the memo on the viewport.
    Mat4d                node_world = world;
    Mat4d                view_world;
    const ViewDependence dependence = node->localToWorldForView(world, view, view_world);
    if (dependence != ViewDependence::None) {
        node_world = view_world;
    }
    if (dependence == ViewDependence::ViewAndViewport) {
        viewport_used = true;
    }
    // The layers this node BELONGS to: its own declaration when it has one, otherwise what it inherited. The
    // fold happens here (it is what the children inherit), but the TEST is at the drawable below: a container
    // stands for structure, not for content, so letting a mask prune a subtree at a group would make content
    // that declares a layer UNREACHABLE under a group that declares none.
    const std::uint32_t layers = node->layers().value_or(inherited.layers);
    // Where this subtree's draws sit inside the pass that draws them (see Node::setDrawOrder): folded the same
    // way, and read by the sort that orders the collected list.
    if (frustum.isOutside(bounds.worldBound(node, node_world))) {
        return;
    }
    // Opacity multiplies down the hierarchy: scene x ancestors x node. A leaf
    // Geometry is itself a node, so its own opacity folds here; a material
    // never contributes transparency.
    const float node_opacity = opacity * node->opacity();

    // This node's own declarations apply to its subtree. A StateNode IS a Group, so the merge has to happen
    // before the descent — and "deeper overrides" is what nearest-ancestor means for the single-valued
    // items, which is why a non-null material / program simply replaces what was inherited.
    InheritedState subtree = inherited;
    subtree.layers         = layers;
    subtree.draw_order     = node->drawOrder().value_or(inherited.draw_order);
    if (const auto* state_node = obj_cast<StateNode>(node)) {
        subtree.state.merge(state_node->renderState());
        if (raw_ptr<MaterialBase> material = state_node->material()) {
            subtree.material = material;
        }
        if (raw_ptr<ShaderProgram> program = state_node->program()) {
            subtree.program = program;
        }
    }
    // The instance stream is NOT a subtree fold: each Primitive is one draw, and its own stream travels
    // with that draw (see Primitive.hpp). Nothing above has to be consulted, so the collector stays the
    // same top-down walk it has always been.

    if (const auto* geometry = obj_cast<Geometry>(node)) {
        // The content selection, tested on the DRAWN node (see Node::setLayers): one AND per command, and no
        // early exit above, because what a mask excludes is content - never the structure it sits in.
        if ((layers & layer_mask) == 0) {
            return;
        }
        // ONE COMMAND PER PRIMITIVE: a primitive IS a draw (its own topology, index span, material and
        // program), and a geometry with none draws nothing. The fold is three levels: a primitive's own
        // material / program wins, otherwise the GEOMETRY's default (the leaf's own value), otherwise the
        // nearest StateNode's - so "this leaf is shaded by m" is one geometry-level value and a primitive
        // can still specialize it.
        const float effective = std::clamp(node_opacity, 0.0f, 1.0f);
        for (std::size_t index = 0; index < geometry->primitiveCount(); ++index) {
            const Primitive* primitive = geometry->primitive(index);
            if (primitive == nullptr) {
                continue;
            }
            // A visible prefix of zero is not a draw. The primitive is skipped, not the whole geometry:
            // another primitive of the same data may still be drawn.
            if (primitive->hasInstances() && primitive->visibleInstanceCount() == 0u) {
                continue;
            }
            const raw_ptr<MaterialBase> geometry_material = geometry->material();
            const MaterialBasePtr material = primitive->material != nullptr
                                                 ? primitive->material
                                                 : (geometry_material != nullptr ? MaterialBasePtr(geometry_material) : MaterialBasePtr(subtree.material));
            const raw_ptr<ShaderProgram> geometry_program = geometry->program();
            const ShaderProgramPtr       program = primitive->program != nullptr
                                                       ? primitive->program
                                                       : (geometry_program != nullptr ? ShaderProgramPtr(geometry_program) : ShaderProgramPtr(subtree.program));
            auto& cmd   = out.emplace_back(intrusive_ptr<Geometry>(const_cast<Geometry*>(geometry)), material, node_world, static_cast<std::uint32_t>(index));
            cmd.opacity = effective;
            cmd.draw_order     = subtree.draw_order;
            // Render state folds along the node path: every StateNode from the
            // scene root to this geometry contributes, deeper nodes overriding.
            // What arrives here is the CONTENT's word (every item optional, so
            // "nobody said" and "said the default" stay different facts); the pass
            // drawing it completes the picture, in one place (see StateResolution.hpp's
            // resolveRenderState and RenderPass::stateOverrides).
            cmd.authored_state = subtree.state;
            cmd.program        = program;
            if (primitive->hasInstances()) {
                cmd.instance_transforms = primitive->instance_transforms;
                cmd.first_instance = static_cast<std::uint32_t>(std::min<std::size_t>(primitive->firstInstance(), std::numeric_limits<std::uint32_t>::max()));
                cmd.instance_count =
                    static_cast<std::uint32_t>(std::min<std::size_t>(primitive->visibleInstanceCount(), std::numeric_limits<std::uint32_t>::max()));
            }
        }
        return;
    }
    if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            collectNodeCommands(child.get(),
                                node_world * child->localTransformMatrix(),
                                frustum,
                                view,
                                node_opacity,
                                subtree,
                                layer_mask,
                                bounds,
                                viewport_used,
                                out);
        }
    }
}

/**
 * @brief Fills @p bake with one node's subtree, WITHOUT a view: the graph half of a collection.
 *
 * @details
 * - Every fact it records is a function of the graph alone, so the result stays valid until
 *   `Node::announceContentChange` moves the root's epoch. The frustum test, the layer mask and the sort are
 *   deliberately absent - they are the view's half (see collectFromBake).
 * - A node that DECLARES a per-view placement (Node::viewDependence) is not resolved here: the run it starts is
 *   recorded in `bake.fallbacks`, its subtree box is left EMPTY (so nothing prunes it) and everything below it is
 *   marked view-dependent, so the select step hands the whole run to the walk that knows how to place it.
 * - Invisible subtrees produce no entries at all: the walk skipped them before, and an entry that can never be
 *   drawn is memory nobody reads.
 *
 * @param node                Subtree root to bake.
 * @param world               World matrix of @p node itself (the walk's own convention: the caller has already
 *                            accumulated it).
 * @param parent_opacity      Opacity accumulated above @p node.
 * @param inherited           What @p node inherits from above (see InheritedState).
 * @param under_view_dependent Whether an ancestor already declared a per-view placement.
 * @param bake                Receives the entries.
 * @return One past the last entry this subtree produced.
 */
std::uint32_t bakeSubtree(const Node*                 node,
                          const Mat4d&                world,
                          float                       parent_opacity,
                          const InheritedState&       inherited,
                          bool                        under_view_dependent,
                          SceneBake&                  bake)
{
    if (node == nullptr || !node->isVisible()) {
        return static_cast<std::uint32_t>(bake.entries.size());
    }
    // Reserve this node's slot BEFORE descending: the order is the identity the select step reads by, so a node
    // must be followed by its own subtree (that is what `end` reports when the descent comes back).
    const std::uint32_t index = static_cast<std::uint32_t>(bake.entries.size());
    bake.entries.emplace_back();
    const ViewDependence declared       = node->viewDependence();
    const bool           view_dependent = under_view_dependent || declared != ViewDependence::None;
    const Mat4d          node_world     = world;
    const std::uint32_t  layers         = node->layers().value_or(inherited.layers);
    const float          node_opacity   = parent_opacity * node->opacity();

    InheritedState subtree = inherited;
    subtree.layers         = layers;
    subtree.draw_order     = node->drawOrder().value_or(inherited.draw_order);
    if (const auto* state_node = obj_cast<StateNode>(node)) {
        subtree.state.merge(state_node->renderState());
        if (raw_ptr<MaterialBase> material = state_node->material()) {
            subtree.material = material;
        }
        if (raw_ptr<ShaderProgram> program = state_node->program()) {
            subtree.program = program;
        }
    }

    if (!under_view_dependent && declared != ViewDependence::None) {
        // The TOP of a view-dependent run: the select step resolves this whole subtree per view, so the context
        // the walk needs (not this node's own resolution) is what gets stored.
        bake.fallbacks.push_back(SceneBake::Fallback{ index, node, world, inherited, parent_opacity });
    }

    bool           has_geometry = false;
    std::uint32_t  primitive_count = 0;
    if (const auto* geometry = obj_cast<Geometry>(node)) {
        has_geometry    = true;
        primitive_count = static_cast<std::uint32_t>(geometry->primitiveCount());
        SceneBake::Entry& entry = bake.entries[index];
        entry.geometry          = GeometryPtr(const_cast<Geometry*>(geometry));
        entry.first_primitive   = 0U;
        entry.primitive_count   = primitive_count;
        entry.state             = subtree.state;
        entry.material          = subtree.material != nullptr ? MaterialBasePtr(subtree.material) : MaterialBasePtr();
        entry.program           = subtree.program != nullptr ? ShaderProgramPtr(subtree.program) : ShaderProgramPtr();
    }
    else if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            (void)bakeSubtree(child.get(), node_world * child->localTransformMatrix(), node_opacity, subtree, view_dependent, bake);
        }
    }

    // Written AFTER the descent: `bake.entries` may have reallocated, so the slot is addressed by index. The box is
    // the SUBTREE's (the same call the walk culls with, one pass, cached per node by BoundsCache).
    SceneBake::Entry& entry = bake.entries[index];
    entry.node      = node;
    entry.world     = node_world;
    entry.layers    = layers;
    entry.draw_order = static_cast<std::int32_t>(subtree.draw_order);
    entry.opacity    = node_opacity;
    entry.view_dependent = view_dependent;
    entry.end            = static_cast<std::uint32_t>(bake.entries.size());
    (void)has_geometry; // the leaf payload above is what the select step reads; kept for symmetry with the walk
    (void)primitive_count;
    return entry.end;
}

/**
 * @brief Collects one view's commands from the baked table: the VIEW half of a collection.
 *
 * @details
 * - The same three decisions the walk made - layer union, frustum, and one command per primitive - taken in the
 *   same order, reading placements and folds instead of computing them. A pruned subtree is skipped with `end`, so
 *   the hierarchical pruning the walk had is kept: this is not a linear scan over every instance.
 * - A view-dependent run is handed back to collectNodeCommands with the context the bake recorded, because that is
 *   the only code that knows how to place such a node for the camera at hand.
 *
 * @param bake        The baked table (see bakeSubtree).
 * @param frustum     The view's frustum.
 * @param view        The view being drawn.
 * @param layer_mask  Layers this picture draws.
 * @param bounds      Per-pass bound cache (used by the view-dependent runs).
 * @param viewport_used Receives true when a view-dependent node needs the device-pixel viewport.
 * @param out         Receives the commands, in walk order.
 */
void collectFromBake(const SceneBake&          bake,
                     const Frustum&            frustum,
                     const CollectView&        view,
                     std::uint32_t             layer_mask,
                     BoundsCache&              bounds,
                     bool&                     viewport_used,
                     std::vector<RenderCommand>& out)
{
    std::size_t fallback = 0U;
    for (std::uint32_t index = 0; index < bake.entries.size();) {
        const SceneBake::Entry& entry = bake.entries[index];
        if (entry.view_dependent) {
            // The rare half: one run per top-most view-dependent node, in index order (see bakeSubtree).
            if (fallback < bake.fallbacks.size() && bake.fallbacks[fallback].index == index) {
                const SceneBake::Fallback& run = bake.fallbacks[fallback];
                ++fallback;
                collectNodeCommands(run.node, run.world, frustum, view, run.parent_opacity, run.inherited, layer_mask, bounds, viewport_used, out);
            }
            index = entry.end;
            continue;
        }
        // The layer prune, the same call the walk makes: passing this node's RESOLVED layers in the "inherited"
        // slot is the same answer (resolution is idempotent - see InheritedState), and it covers a leaf's own
        // declaration because subtreeLayerUnion() starts from the node itself.
        if ((entry.node->subtreeLayerUnion(entry.layers) & layer_mask) == 0U) {
            index = entry.end;
            continue;
        }
        // The box is asked HERE, not baked: a buffer edit changes it without moving the node (see the file note on
        // SceneBake). BoundsCache makes it one transform per visited node, and the local box is cached per node.
        if (frustum.isOutside(bounds.worldBound(entry.node, entry.world))) {
            index = entry.end;
            continue;
        }
        if (entry.geometry != nullptr) {
            const Geometry* geometry  = entry.geometry.get();
            const float     effective = std::clamp(entry.opacity, 0.0f, 1.0f);
            for (std::uint32_t offset = 0; offset < entry.primitive_count; ++offset) {
                const std::size_t ordinal   = static_cast<std::size_t>(entry.first_primitive) + offset;
                const Primitive*  primitive = geometry->primitive(ordinal);
                if (primitive == nullptr) {
                    continue;
                }
                // A visible prefix of zero is not a draw; another primitive of the same data may still be.
                if (primitive->hasInstances() && primitive->visibleInstanceCount() == 0u) {
                    continue;
                }
                const raw_ptr<MaterialBase> geometry_material = geometry->material();
                const MaterialBasePtr       material = primitive->material != nullptr
                                                           ? MaterialBasePtr(primitive->material)
                                                           : (geometry_material != nullptr ? MaterialBasePtr(geometry_material) : entry.material);
                const raw_ptr<ShaderProgram> geometry_program = geometry->program();
                const ShaderProgramPtr       program = primitive->program != nullptr
                                                           ? ShaderProgramPtr(primitive->program)
                                                           : (geometry_program != nullptr ? ShaderProgramPtr(geometry_program) : entry.program);
                auto& cmd      = out.emplace_back(entry.geometry, material, entry.world, static_cast<std::uint32_t>(ordinal));
                cmd.opacity    = effective;
                cmd.draw_order = entry.draw_order;
                cmd.authored_state = entry.state;
                cmd.program        = program;
                if (primitive->hasInstances()) {
                    cmd.instance_transforms = primitive->instance_transforms;
                    cmd.first_instance =
                        static_cast<std::uint32_t>(std::min<std::size_t>(primitive->firstInstance(), std::numeric_limits<std::uint32_t>::max()));
                    cmd.instance_count =
                        static_cast<std::uint32_t>(std::min<std::size_t>(primitive->visibleInstanceCount(), std::numeric_limits<std::uint32_t>::max()));
                }
            }
        }
        ++index;
    }
}

} // namespace

/**
 * @brief One frame's collected command lists, keyed by the camera's view.
 *
 * Scoped to a content frame (Scene::setContentFrame): the entries are dropped
 * at the next frame boundary, so the memo cannot grow with time and cannot
 * serve content from an earlier frame.
 *
 * The key is the camera's VIEW (its projection*view product and eye point), not
 * its address: the collected list is a pure function of that view, so two
 * cameras with the same view share the entry by construction, a camera edited
 * between two passes misses (and gets a fresh walk), and — unlike a pointer key
 * — nothing is assumed about the camera's lifetime. Cameras are borrowed as
 * raw pointers by design (a stack camera is legal, see RenderPass::setCamera),
 * so an owning key would be wrong, and a bare address key could not notice a
 * camera being edited in place.
 */
struct Scene::ContentMemo {
    /// One view's collected list.
    struct Entry {
        Mat4d view_proj;
        Vec3d eye;
        // The device-pixel rectangle is part of the KEY: the same camera drawn into two PiP rectangles
        // produces different screen-space scales for view-dependent nodes (AutoTransform).
        Viewport      viewport;
        std::uint64_t revision   = 0;
        // The mask is part of the KEY, not only a parameter: two passes may draw the same view through the
        // same camera and still want DIFFERENT lists (a shadow pass drawing only casters, a main pass drawing
        // everything), so serving one's list for the other's mask would swap a few objects in and out of the
        // picture - correct-looking and wrong.
        std::uint32_t layer_mask = kAllLayers;
        // True when the list was built by a node that also depends on the device-pixel viewport
        // (AutoTransform screen-space scaling). A list without such a node is shared across rectangles.
        bool viewport_sensitive  = false;
        // SHARED and immutable once built (see collectRenderCommands): the passes of a frame hand the same list
        // around, and it is reused across frames for as long as the graph does not change.
        std::shared_ptr<const std::vector<RenderCommand>> commands;
        // The content frame that asked for this list last (see setContentFrame): an entry nothing asked for in the
        // previous frame is dropped, which is what keeps the memo bounded to the views actually drawn.
        std::uint64_t last_used_frame = 0;
    };

    /// Lists collected for the views in use (one per distinct view), reused while the graph does not change.
    std::vector<Entry> entries;
};

Scene::Scene() = default;

Scene::~Scene() = default;

String Scene::name() const
{
    return name_;
}

void Scene::setName(const String& name)
{
    name_ = name;
}

bool Scene::isVisible() const
{
    return visible_;
}

void Scene::setVisible(bool visible)
{
    if (visible_ == visible) {
        return; // no content change: keep the frame's memo usable
    }
    visible_ = visible;
    invalidateContent();
}

float Scene::opacity() const
{
    return opacity_;
}

void Scene::setOpacity(float opacity)
{
    if (opacity_ == opacity) {
        return; // no content change: keep the frame's memo usable
    }
    opacity_ = opacity;
    invalidateContent();
}

NodePtr Scene::root() const
{
    return root_;
}

void Scene::setRoot(intrusive_ptr<Node> root)
{
    if (root_ == root) {
        return; // no content change: keep the frame's memo usable
    }
    // A root must be a ROOT (see the class note): a node that is still a child of a Group is detached first,
    // exactly like Group::addChild detaches from the old parent. Without this, "set this as my root" would
    // silently render the subtree at another tree's transform. Only Groups own children, so the cast is the
    // same one Group::addChild uses to reach an old parent.
    if (root != nullptr && root->parent() != nullptr) {
        if (auto* old_parent = obj_cast<Group>(root->parent())) {
            old_parent->removeChild(root.get());
        }
    }
    root_ = std::move(root);
    detail::announceLayerChange();
    invalidateContent();
}

bool Scene::hasRootedRoot() const
{
    return root_ != nullptr && root_->parent() == nullptr;
}

NodePtr Scene::findNode(const String& name) const
{
    if (root_ != nullptr) {
        return detail::findNodeRecursive(root_.get(), name);
    }
    return NodePtr();
}

void Scene::clear()
{
    if (root_ == nullptr) {
        return;
    }
    root_.reset();
    detail::announceLayerChange();
    invalidateContent();
}

void Scene::addLight(intrusive_ptr<Light> light)
{
    if (light == nullptr) {
        return;
    }
    lights_.emplace_back(std::move(light));
}

void Scene::removeLight(raw_ptr<Light> light)
{
    if (light == nullptr) {
        return;
    }
    auto it = std::find_if(lights_.begin(), lights_.end(), [light](const LightPtr& ptr) { return ptr.get() == light; });
    if (it != lights_.end()) {
        lights_.erase(it);
    }
}

void Scene::clearLights()
{
    lights_.clear();
}

void Scene::addLighting(const LightRig& rig)
{
    rig.addTo(*this);
}

void Scene::setLighting(const LightRig& rig)
{
    clearLights();
    rig.addTo(*this);
}

const std::vector<LightPtr>& Scene::lights() const
{
    return lights_;
}

bool Scene::hasLights() const
{
    return !lights_.empty();
}

Aabbd Scene::boundingBox(std::uint32_t layer_mask) const
{
    if (!visible_ || !hasRootedRoot()) {
        return Aabbd::empty();
    }
    return detail::visibleWorldBox(*root_, kDefaultLayer, layer_mask);
}

std::shared_ptr<const std::vector<RenderCommand>>
Scene::collectRenderCommands(raw_ptr<const Camera> camera, std::uint32_t layer_mask, const Viewport* viewport) const
{
    if (camera == nullptr) {
        // The empty answer, shared rather than built: a caller with no camera asks for nothing.
        return detail::noCommands();
    }
    const Viewport    draw_viewport = viewport != nullptr ? *viewport : Viewport{};
    const CollectView view{ camera->projectionMatrix(), camera->viewMatrix(), camera->eye(), camera->up(), draw_viewport };
    return collectRenderCommands(view, layer_mask);
}

std::shared_ptr<const std::vector<RenderCommand>> Scene::collectRenderCommands(const CollectView& view, std::uint32_t layer_mask) const
{
    // The empty answer, shared rather than built: an empty scene asks for nothing, and paying an allocation
    // for "nothing" would make the cheap case the expensive one. A root that is no longer a ROOT is the same
    // answer, and the check runs BEFORE the memo lookup on purpose: the memo must not hand back a list built
    // while the node was still this scene's root (see setRoot).
    if (!visible_ || !hasRootedRoot()) {
        return detail::noCommands();
    }
    // The shape is part of the view: the projection encodes it (the camera states it, see Camera), so the memo
    // below (which matches on the view-projection) cannot answer for one shape with another's walk.
    const Mat4d    view_proj  = view.viewProjection();
    const Vec3d    eye        = view.eye;
    const Viewport viewport   = view.viewport;
    // THE MEMO OUTLIVES ONE FRAME. A collected list is a pure function of the graph and the view, so it is reused
    // for as long as NOTHING changed - which the graph announces through the content epoch of THIS SCENE'S ROOT
    // SUBTREE (every mutator bumps itself and every ancestor, see Node::announceContentChange). The scope is
    // deliberate: editing another scene's tree leaves this root's epoch alone, so its lists survive. Before the
    // memo existed an entry lived for one frame only and a static scene paid a full walk per frame (measured:
    // SceneTest.MeasureWhatOneCollectionCosts*).
    const std::uint64_t epoch = root_->contentEpoch();
    if (content_memo_ != nullptr && epoch != memo_epoch_) {
        // Something in THIS scene's graph changed since the last look: every view's list may be stale, and
        // re-deriving each one is the walk a miss does anyway.
        content_memo_->entries.clear();
    }
    // Recorded even when the memo was just cleared (or does not exist yet): the next call has to compare against
    // THIS look, or it would invalidate a memo it built itself.
    memo_epoch_           = epoch;
    const auto memo_entry = [this, &view_proj, &eye, &viewport, layer_mask]() -> ContentMemo::Entry* {
        if (content_frame_ == 0 || content_memo_ == nullptr) {
            return nullptr; // nothing is memoised until a frame is open (a caller that drives it keeps walking)
        }
        for (auto& entry : content_memo_->entries) {
            if (entry.revision == content_revision_ && entry.eye == eye && entry.layer_mask == layer_mask &&
                (!entry.viewport_sensitive || (entry.viewport.width == viewport.width && entry.viewport.height == viewport.height)) &&
                detail::sameView(entry.view_proj, view_proj))
            {
                entry.last_used_frame = content_frame_;
                return &entry;
            }
        }
        return nullptr;
    };
    if (const ContentMemo::Entry* hit = memo_entry()) {
        ++content_reuse_count_;
        // The SHARED list itself, not a copy of it: the list is immutable once built (what a pass may
        // change is its own view of it, see RenderPass::execute), so handing out one more reference costs
        // a refcount instead of one copy of every command -- the per-pass copy this replaced was N
        // intrusive_ptr increments plus an N x sizeof(RenderCommand) memcpy, on every pass of every frame.
        return hit->commands;
    }
    ++content_collect_count_;

    // THE GRAPH HALF, ONCE PER EPOCH: every node's placement, world box and folded state, in walk order (see
    // ContentBake). A moved camera then pays the VIEW half only - the layer prune, the frustum test and the sort -
    // instead of recomputing every matrix and every fold, which is what a collection used to cost per view
    // (measured: SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera).
    if (content_bake_ == nullptr || bake_epoch_ != epoch) {
        content_bake_ = std::make_unique<detail::SceneBake>();
        (void)detail::bakeSubtree(root_.get(), root_->worldMatrix(), opacity_, detail::InheritedState{}, false, *content_bake_);
        bake_epoch_ = epoch;
        ++content_bake_count_;
    }

    std::vector<RenderCommand>    commands;
    const detail::Frustum        frustum       = detail::Frustum::fromViewProjection(view_proj);
    detail::BoundsCache           bounds;
    bool                          viewport_used = false;
    detail::collectFromBake(*content_bake_, frustum, view, layer_mask, bounds, viewport_used, commands);
    // Where the sort puts things, in one sentence: the draw order the CONTENT declared is the outer key (see
    // Node::setDrawOrder - higher is drawn later), and inside one order: opaque front-to-back (near first),
    // transparent back-to-front (far first) after the opaque batch. Transparent objects need painter's
    // order for correct alpha blending.
    //
    // The sort key is computed once per command instead of inside the
    // comparator: a comparator runs O(n log n) times, and the original form
    // therefore re-did two matrix-vector products per comparison. Squared
    // distance orders identically to distance (sqrt is monotonic on [0, inf))
    // and skips the roots. Sorting pointers keeps std::stable_sort's stable
    // semantics over the collection order, so ties still favour the earlier
    // command.
    std::vector<std::pair<double, RenderCommand*>> keyed;
    keyed.reserve(commands.size());
    for (RenderCommand& cmd : commands) {
        const auto origin = cmd.model_matrix * vn::math::Point3d(0.0, 0.0, 0.0);
        keyed.emplace_back((origin.asVector() - eye).length2(), &cmd);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const std::pair<double, RenderCommand*>& lhs, const std::pair<double, RenderCommand*>& rhs) {
        // The draw order DECLARED on the content is the outer key (see Node::setDrawOrder): a
        // higher order is drawn later, which is how content that must land on top of the rest
        // - an outline, a decal, a highlight - is expressed without its own pass. Equal orders
        // fall through to the ordering the collection has always produced, so the default
        // (everybody at 0) is bit for bit the picture it was.
        if (lhs.second->draw_order != rhs.second->draw_order) {
            return lhs.second->draw_order < rhs.second->draw_order;
        }
        if (lhs.second->isTransparent() != rhs.second->isTransparent()) {
            return !lhs.second->isTransparent();
        }
        return lhs.second->isTransparent() ? lhs.first > rhs.first : lhs.first < rhs.first;
    });
    std::vector<RenderCommand> ordered;
    ordered.reserve(keyed.size());
    for (auto& [distance, cmd] : keyed) {
        (void)distance;
        ordered.push_back(std::move(*cmd));
    }
    // Into the shared list by MOVE: the walk's result is the only copy that ever exists.
    auto shared = std::make_shared<const std::vector<RenderCommand>>(std::move(ordered));
    if (content_frame_ != 0) {
        if (content_memo_ == nullptr) {
            content_memo_ = std::make_unique<ContentMemo>();
        }
        if (ContentMemo::Entry* entry = memo_entry()) {
            entry->commands           = shared; // the same view asked again after a change
            entry->viewport_sensitive = viewport_used;
        }
        else {
            content_memo_->entries.push_back(
                ContentMemo::Entry{ view_proj, eye, viewport, content_revision_, layer_mask, viewport_used, shared, content_frame_ });
        }
    }
    return shared;
}

void Scene::setContentFrame(std::uint64_t frame){
    if (content_frame_ == frame) {
        return; // idempotent: every pass of a frame may announce the same token
    }
    const std::uint64_t previous = content_frame_;
    content_frame_               = frame;
    if (content_memo_ == nullptr) {
        return;
    }
    // ONE FRAME OF GRACE, then the entry goes: a list is kept while some view keeps asking for it, so a view that
    // stops being drawn (a closed pane, a camera that went away) releases its storage instead of owning it forever.
    // This is what replaced "clear everything at every frame boundary" (see the memo note in collectRenderCommands).
    auto& entries = content_memo_->entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [previous](const ContentMemo::Entry& entry) { return entry.last_used_frame < previous; }),
                  entries.end());
}

void Scene::invalidateContent()
{
    ++content_revision_;
    if (root_ != nullptr) {
        // Scene-level facts (a swapped root, scene visibility / opacity, an edit no setter could see) are announced
        // on the root's own subtree epoch: the memo then drops its entries at the next collect, and the per-entry
        // revision below is still the key that says which scene version a list was built for.
        root_->announceContentChange();
        // The bounds caches are per node and keyed by ANNOUNCEMENTS, and the built-in setters already
        // announce - so what this reaches is the edit they could not see: data written through a buffer, a
        // custom node's own state. Bumping every stamp makes the next collect recompute every box once (and
        // re-cache), which is what "picked up at once" has to mean for the culling cache too.
        detail::invalidateBoundsUnder(*root_);
    }
}

std::uint64_t Scene::contentCollectCount() const noexcept
{
    return content_collect_count_;
}

std::uint64_t Scene::contentCollectReuseCount() const noexcept
{
    return content_reuse_count_;
}

std::uint64_t Scene::contentBakeCount() const noexcept
{
    return content_bake_count_;
}

VN_GRAPHICS_NS_END
