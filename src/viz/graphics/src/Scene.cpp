#include <vine/graphics/Scene.hpp>

#include "DrawListBake.hpp"
#include "DrawListMemo.hpp"
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
 * its whole subtree - the same rule the draw list's build (bakeSubtree in DrawListBake) and the ray walks
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



} // namespace



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
    // THE MEMO OUTLIVES ONE FRAME. A collected list is a pure function of the graph and the view, so it is reused for as
    // long as NOTHING changed - which the graph announces through the content epoch of THIS SCENE'S ROOT SUBTREE (every
    // mutator bumps itself and every ancestor, see Node::announceContentChange). The scope is deliberate: editing
    // another scene's tree leaves this root's epoch alone, so its lists survive. Before the memo existed an entry lived
    // for one frame only and a static scene paid a full walk per frame (measured: SceneTest.MeasureWhatOneCollectionCosts*).
    // The memo itself is SHARED WITH THE MIRROR (see DrawListMemo): "which list may this view reuse" is one rule, and
    // the two answers to a view must not implement it twice.
    const std::uint64_t epoch = root_->contentEpoch();
    if (content_memo_ != nullptr && epoch != memo_epoch_) {
        // Something in THIS scene's graph changed since the last look: every view's list may be stale, and re-deriving
        // each one is the walk a miss does anyway.
        content_memo_->clear();
    }
    // Recorded even when the memo was just cleared (or does not exist yet): the next call has to compare against THIS
    // look, or it would invalidate a memo it built itself.
    memo_epoch_ = epoch;
    const bool memoising = content_frame_ != 0; // nothing is memoised until a frame is open (a caller drives it)
    if (memoising && content_memo_ == nullptr) {
        content_memo_ = std::make_unique<detail::DrawListMemo>();
    }
    if (memoising) {
        if (detail::DrawListMemo::Entry* hit = content_memo_->find(view_proj, eye, viewport, layer_mask, content_revision_)) {
            ++content_reuse_count_;
            hit->last_used_frame = content_frame_;
            // The SHARED list itself, not a copy of it: the list is immutable once built (what a pass may change is its
            // own view of it, see RenderPass::execute), so handing out one more reference costs a refcount instead of
            // one copy of every command -- the per-pass copy this replaced was N intrusive_ptr increments plus an
            // N x sizeof(RenderCommand) memcpy, on every pass of every frame.
            return hit->commands;
        }
    }
    ++content_collect_count_;

    const detail::SceneBake& rows = ensureRows();

    std::vector<RenderCommand> commands;
    const detail::Frustum      frustum       = detail::Frustum::fromViewProjection(view_proj);
    bool                       viewport_used = false;
    detail::selectFromBake(rows, frustum, view, layer_mask, viewport_used, commands);
    // The view's half ends in one order, and the order is the same rule for every producer of a draw list (the
    // render world sorts its own collection with this very function - see DrawListBake.hpp).
    detail::sortCollected(commands, eye);
    // Into the shared list by MOVE: the walk's result is the only copy that ever exists.
    auto shared = std::make_shared<const std::vector<RenderCommand>>(std::move(commands));
    if (memoising) {
        if (detail::DrawListMemo::Entry* entry = content_memo_->find(view_proj, eye, viewport, layer_mask, content_revision_)) {
            entry->commands           = shared; // the same view asked again after a change
            entry->viewport_sensitive = viewport_used;
            entry->last_used_frame    = content_frame_;
        }
        else {
            (void)content_memo_->store(view_proj, eye, viewport, layer_mask, viewport_used, content_revision_, shared, content_frame_);
        }
    }
    return shared;
}

const detail::SceneBake& Scene::ensureRows() const
{
    // A scene with no rooted root DRAWS NOTHING (see collectRenderCommands), so its rows are empty - and that is a
    // state, not an error: a mirror of such a scene publishes an empty table and the frame stays correct. This is the
    // total contract of this function; a caller (the mirror's publish) has no business repeating the guard.
    if (!hasRootedRoot()) {
        if (content_bake_ == nullptr) {
            content_bake_ = std::make_unique<detail::SceneBake>();
        }
        content_bake_->rows.clear();
        content_bake_->nodes.clear();
        return *content_bake_;
    }

    // THE GRAPH HALF, ONCE PER EPOCH: every node's own facts, then the one fold over them (see DrawListBake.hpp).
    // A moved camera then pays the VIEW half only - the layer prune, the frustum test and the sort - instead of
    // recomputing every matrix and every fold, which is what a collection used to cost per view
    // (measured: SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera). It is a named step because a collection
    // is not the only reader of the rows any more: the engine side's mirror is published from them (see SceneRows).
    const std::uint64_t epoch = root_->contentEpoch();
    if (content_bake_ == nullptr || bake_epoch_ != epoch) {
        content_bake_ = std::make_unique<detail::SceneBake>();
        detail::buildRows(*root_, *content_bake_, opacity_);
        bake_epoch_ = epoch;
        ++content_bake_count_;
    }
    // Byte content announces itself on the buffer, not on the graph, so the boxes that depend on it are re-read
    // here - the ONE step of a collection that still needs this thread (see refreshBoxes). A box that moved is a
    // change the EPOCH cannot see, so it moves the rows' version too: a reader that mirrors these rows compares
    // that version, and would otherwise keep a stale box forever (see SceneRows::version).
    if (detail::refreshBoxes(*content_bake_) > 0U) {
        ++box_generation_;
    }
    // WHAT A MIRROR IS TOLD. The two numbers above are this scene's own (a query wants them current); the pair below is
    // the version a mirror compares, and it only advances OUTSIDE an edit batch (see beginEdit): a frame that lands in
    // the middle of one publishes nothing, so the mirror keeps drawing the state the batch started from, and the whole
    // batch becomes ONE publication in the frame after the outermost endEdit.
    if (edit_depth_ == 0) {
        published_epoch_ = bake_epoch_;
        published_boxes_ = box_generation_;
    }
    return *content_bake_;
}

void Scene::beginEdit()
{
    // A COUNTER, not a flag: a helper that opens its own batch must compose with the caller's (see the declaration).
    ++edit_depth_;
}

void Scene::endEdit()
{
    if (edit_depth_ > 0) {
        --edit_depth_;
    }
    // The version is NOT forced up here: nothing has been collected yet, so `bake_epoch_` may be behind the graph. The
    // next collection catches the pair up (see ensureRows) - and that collection is the one a frame's publication
    // reads, which is exactly where the whole batch has to be visible.
}

void Scene::setContentFrame(std::uint64_t frame)
{
    if (content_frame_ == frame) {
        return; // idempotent: every pass of a frame may announce the same token
    }
    const std::uint64_t previous = content_frame_;
    content_frame_               = frame;
    if (content_memo_ == nullptr) {
        return;
    }
    // ONE FRAME OF GRACE, then the entry goes (see DrawListMemo::beginFrame): this is what replaced "clear everything at
    // every frame boundary" - a list is kept while some view keeps asking for it.
    content_memo_->beginFrame(previous);
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
