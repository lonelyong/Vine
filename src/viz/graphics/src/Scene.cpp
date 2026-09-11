#include <vine/graphics/Scene.hpp>

#include <algorithm>
#include <array>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/math/Transform3.hpp>

V_GRAPHICS_NS_BEGIN

using vine::math::Vec3d;
using vine::math::Vec4d;

V_OBJECT_META_IMPL(Scene, vine::Object);

namespace
{

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
        Frustum f;
        const auto row = [&vp](int r) {
            return Vec4d(vp.element(r, 0), vp.element(r, 1), vp.element(r, 2),
                         vp.element(r, 3));
        };
        const Vec4d r0 = row(0);
        const Vec4d r1 = row(1);
        const Vec4d r2 = row(2);
        const Vec4d r3 = row(3);
        f.planes_[0] = r3 + r0;  // left
        f.planes_[1] = r3 - r0;  // right
        f.planes_[2] = r3 + r1;  // bottom
        f.planes_[3] = r3 - r1;  // top
        f.planes_[4] = r3 + r2;  // near
        f.planes_[5] = r3 - r2;  // far
        for (auto& p : f.planes_) {
            const Vec3d n(p.x, p.y, p.z);
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
            const Vec3d pos{ (n.x >= 0.0) ? box.max().x : box.min().x,
                             (n.y >= 0.0) ? box.max().y : box.min().y,
                             (n.z >= 0.0) ? box.max().z : box.min().z };
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
    if (const auto* group = dynamic_cast<const Group*>(node)) {
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
 * @brief Recursively collects render commands from a node subtree.
 *
 * Container nodes (Group and its subclasses) are descended into; a leaf
 * Geometry emits one render command baked with its world matrix (the product
 * of enclosing MatrixTransforms). Nodes fully outside the frustum (and their
 * subtrees) are culled.
 *
 * @param node    Root node to traverse.
 * @param frustum View frustum for culling.
 * @param out     Output command list.
 */
void collectNodeCommands(const Node* node, const Frustum& frustum, float opacity,
                         std::vector<RenderCommand>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if (frustum.isOutside(node->boundingBox())) {
        return;
    }
    // Opacity multiplies down the hierarchy: scene x ancestors x node. A leaf
    // Geometry is itself a node, so its own opacity folds here; a material
    // never contributes transparency.
    const float node_opacity = opacity * node->opacity();

    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        Material* material = geometry->material();
        const float effective = std::clamp(node_opacity, 0.0f, 1.0f);
        auto& cmd = out.emplace_back(
            intrusive_ptr<Geometry>(const_cast<Geometry*>(geometry)),
            intrusive_ptr<Material>(material), node->worldMatrix());
        cmd.opacity = effective;
        cmd.isTransparent = effective < 1.0f - 1e-6f;
        // Render state folds along the node path: every StateNode from the
        // scene root to this geometry contributes, deeper nodes overriding.
        // The fold is computed once so the backend can also tell whether the
        // depth item was explicitly authored (an explicit StateNode depth wins
        // over the pass-level depth policy, see RenderCommand::depthExplicit).
        const RenderState folded = collectRenderState(node);
        cmd.renderState    = resolveRenderState(folded);
        cmd.depthExplicit  = folded.depth.has_value();
        // Shading program resolves leaf-first then ancestor StateNodes.
        cmd.program = effectiveProgram(node);
        return;
    }
    if (const auto* group = dynamic_cast<const Group*>(node)) {
        for (const auto& child : group->children()) {
            collectNodeCommands(child.get(), frustum, node_opacity, out);
        }
    }
}

}  // namespace

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
    visible_ = visible;
}

float Scene::opacity() const
{
    return opacity_;
}

void Scene::setOpacity(float opacity)
{
    opacity_ = opacity;
}

NodePtr Scene::root() const
{
    return root_;
}

void Scene::setRoot(intrusive_ptr<Node> root)
{
    root_ = std::move(root);
}

NodePtr Scene::findNode(const String& name) const
{
    if (root_ != nullptr) {
        return findNodeRecursive(root_.get(), name);
    }
    return NodePtr();
}

void Scene::clear()
{
    root_.reset();
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
    auto it = std::find_if(lights_.begin(), lights_.end(),
                           [light](const LightPtr& ptr) { return ptr.get() == light; });
    if (it != lights_.end()) {
        lights_.erase(it);
    }
}

void Scene::clearLights()
{
    lights_.clear();
}

const std::vector<LightPtr>& Scene::lights() const
{
    return lights_;
}

bool Scene::hasLights() const
{
    return !lights_.empty();
}

Aabbd Scene::boundingBox() const
{
    if (!visible_ || root_ == nullptr || !root_->isVisible()) {
        return Aabbd::empty();
    }
    return root_->boundingBox();
}

std::vector<RenderCommand> Scene::collectRenderCommands(raw_ptr<const Camera> camera) const
{
    std::vector<RenderCommand> commands;
    if (camera == nullptr || !visible_ || root_ == nullptr) {
        return commands;
    }
    const Mat4d view_proj = camera->projectionMatrix() * camera->viewMatrix();
    const Frustum frustum = Frustum::fromViewProjection(view_proj);
    collectNodeCommands(root_.get(), frustum, opacity_, commands);
    // Sort: opaque front-to-back (near first), transparent back-to-front
    // (far first) after the opaque batch. Transparent objects need painter's
    // order for correct alpha blending.
    const Vec3d eye = camera->eye();
    std::stable_sort(commands.begin(), commands.end(),
                     [&eye](const RenderCommand& lhs, const RenderCommand& rhs) {
                         if (lhs.isTransparent != rhs.isTransparent) {
                             return !lhs.isTransparent;
                         }
                         const auto lhs_pos = lhs.modelMatrix * vine::math::Point3d(0.0, 0.0, 0.0);
                         const auto rhs_pos = rhs.modelMatrix * vine::math::Point3d(0.0, 0.0, 0.0);
                         const double lhs_dist = (lhs_pos.asVector() - eye).length();
                         const double rhs_dist = (rhs_pos.asVector() - eye).length();
                         return lhs.isTransparent ? lhs_dist > rhs_dist : lhs_dist < rhs_dist;
                     });
    return commands;
}

V_GRAPHICS_NS_END
