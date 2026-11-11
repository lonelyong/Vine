#include <vine/graphics/Scene.hpp>

#include "DrawListBake.hpp"
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

    // THE GRAPH HALF, ONCE PER EPOCH: every node's own facts, then the one fold over them (see DrawListBake.hpp).
    // A moved camera then pays the VIEW half only - the layer prune, the frustum test and the sort - instead of
    // recomputing every matrix and every fold, which is what a collection used to cost per view
    // (measured: SceneTest.MeasureWhatOneCollectionCostsWithAMovingCamera).
    if (content_bake_ == nullptr || bake_epoch_ != epoch) {
        content_bake_ = std::make_unique<detail::SceneBake>();
        content_bake_->seed.parent_opacity = opacity_;
        (void)detail::bakeSubtree(*root_, *content_bake_);
        (void)detail::resolveSubtree(*content_bake_, 0U);
        bake_epoch_ = epoch;
        ++content_bake_count_;
    }
    // Byte content announces itself on the buffer, not on the graph, so the boxes that depend on it are re-read
    // here - the ONE step of a collection that still needs this thread (see refreshBoxes).
    (void)detail::refreshBoxes(*content_bake_);

    std::vector<RenderCommand> commands;
    const detail::Frustum      frustum       = detail::Frustum::fromViewProjection(view_proj);
    bool                       viewport_used = false;
    detail::selectFromBake(*content_bake_, frustum, view, layer_mask, viewport_used, commands);
    // The view's half ends in one order, and the order is the same rule for every producer of a draw list (the
    // render world sorts its own collection with this very function - see DrawListBake.hpp).
    detail::sortCollected(commands, eye);
    // Into the shared list by MOVE: the walk's result is the only copy that ever exists.
    auto shared = std::make_shared<const std::vector<RenderCommand>>(std::move(commands));
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
