#include <vine/graphics/Scene.hpp>

#include "LayerEpoch.hpp"
#include "ScenePublisher.hpp"

#include <algorithm>
#include <memory>
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

VN_OBJECT_META_IMPL(Scene, vn::Object);

// The collector's internals: the frustum, the per-pass bound cache, the folded inherited state and the baked half of
// a collection. Named `detail` (not an anonymous namespace) because Scene's header forward-declares the bake type -
// nothing outside this translation unit names any of them.
namespace detail
{

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



Scene::Scene()
  : publisher_(std::make_unique<detail::ScenePublisher>(*this))
{}

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
    // THE QUESTION BELONGS TO THE DERIVED SIDE (see detail::ScenePublisher): this scene's baked rows, its memo key
    // and its edit batch are what the answer depends on, and none of them is authored content.
    return publisher_->collectRenderCommands(view, layer_mask);
}

ScenePublishResult Scene::publish()
{
    return publisher_->publish();
}

void Scene::beginEdit()
{
    publisher_->beginEdit();
}

void Scene::endEdit()
{
    publisher_->endEdit();
}

void Scene::setContentFrame(std::uint64_t frame)
{
    publisher_->setContentFrame(frame);
}

void Scene::invalidateContent()
{
    publisher_->invalidateContent();
}

std::uint64_t Scene::contentCollectCount() const noexcept
{
    return publisher_->collectCount();
}

std::uint64_t Scene::contentCollectReuseCount() const noexcept
{
    return publisher_->reuseCount();
}

std::uint64_t Scene::contentBakeCount() const noexcept
{
    return publisher_->bakeCount();
}

VN_GRAPHICS_NS_END
