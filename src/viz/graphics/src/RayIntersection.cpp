#include <vine/graphics/RayIntersection.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/math/Transform3.hpp>

#include "TriangleStream.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The picker's write half of `Geometry::triangleTestCount()`.
 *
 * @details
 * - A picker that never ran a tree has no way to prove the tree paid off, and a geometry that lets anyone move its
 *   diagnostic can say anything. So the counter has one reader (public) and one writer (this, reached through
 *   friendship), and both live where the work happens.
 * - It is a friend type rather than a public method for the same reason `Mesh::buildBvh()` is not "add a million
 *   primitives": the number is evidence, not an input.
 */
struct GeometryPickStats {
    /**
     * @brief Records @p count exact triangle tests against @p geometry.
     *
     * @param geometry Geometry the tests were run on.
     * @param count    Triangle tests just performed.
     */
    static void record(const Geometry& geometry, std::uint64_t count) noexcept
    {
        geometry.triangle_tests_ += count;
    }
};

namespace
{

/**
 * @brief Computes the normal of a triangle.
 *
 * @param a Triangle vertex A.
 * @param b Triangle vertex B.
 * @param c Triangle vertex C.
 * @return Face normal (unit length).
 */
Vec3d faceNormal(const Vec3d& a, const Vec3d& b, const Vec3d& c)
{
    return (b - a).cross(c - a).normalized();
}

// Forward declaration: defined below but used by makeTriangleHit() and traverseMesh().
Vec3d toWorld(const Mat4d& m, double x, double y, double z);

/**
 * @brief Computes the local AABB enclosing the given positions.
 *
 * @param positions Position array.
 * @return The bounding box (empty for an empty array).
 */
Aabbd boundsOfPositions(std::span<const vn::math::Vec3f> positions)
{
    Aabbd box = Aabbd::empty();
    for (const auto& p : positions) {
        box.expandBy(Vec3d(p.x, p.y, p.z));
    }
    return box;
}

/**
 * @brief Transforms a local-space vertex into world space.
 *
 * Uses a Point3 multiply so the translation part of the matrix is honoured;
 * multiplying a plain Vector3 would drop it.
 *
 * @param m  World matrix of the geometry.
 * @param x  Local x.
 * @param y  Local y.
 * @param z  Local z.
 * @return World-space vertex.
 */
Vec3d toWorld(const Mat4d& m, double x, double y, double z)
{
    const vn::math::Point3d p = m * vn::math::Point3d(x, y, z);
    return Vec3d(p.x, p.y, p.z);
}

/**
 * @brief Which hits a triangle walk reports, and therefore whether it may prune against the closest hit so far.
 *
 * @details
 * - kNearest is the closest-hit search: a hit that cannot become the nearest one is not worth reporting, so the
 *   walk may lower its distance limit as it goes - which is what lets a triangle tree skip subtrees.
 * - kAll is "every triangle hit", the AllHits mode: the SET of hits is the answer, so nothing may be pruned.
 *   The tree still culls by the ray itself; it just never skips by distance.
 */
enum class HitPolicy
{
    kNearest,
    kAll,
};

/**
 * @brief What one walk over a draw's triangles found.
 */
struct MeshWalk {
    bool        any   = false; ///< At least one triangle was hit.
    std::size_t tests = 0;     ///< Exact triangle tests run (see Geometry::triangleTestCount).
};

// Declared before its first use: a template that calls a later template needs the name visible at definition time, and
// relying on the compiler to find it at instantiation instead is not portable.
template <typename OnHit>
MeshWalk traverseMesh(const TriangleStream&                    stream,
                      const vn::geom::spatial::Bvh<double>*    tree,
                      const vn::geom::spatial::Rayd&           ray,
                      const Mat4d&                             world,
                      HitPolicy                                policy,
                      OnHit&&                                  on_hit);

/**
 * @brief Walks every TRIANGLE primitive of @p geometry and reports each hit it produces.
 *
 * A geometry is a set of primitives, and only the triangle ones are surfaces: a mesh drawn with its edge
 * lines and a selected-point set has one triangle primitive and two that cannot be hit. Each hit is tagged
 * with the primitive it came from, so a caller can tell "which part of the asset" from "which geometry".
 *
 * @param geometry  Geometry to intersect.
 * @param ray       The ray, in world space.
 * @param world     The geometry's world matrix.
 * @param policy    Whether only the closest hit matters (permits pruning) or every hit does.
 * @param visit     Called for every hit (receives it by value, already tagged).
 */
template <typename Visit>
void forEachTriangleHit(const Geometry* geometry, const vn::geom::spatial::Rayd& ray, const Mat4d& world, HitPolicy policy, Visit&& visit)
{
    if (geometry == nullptr) {
        return;
    }
    std::size_t tests = 0u;
    for (std::size_t index = 0; index < geometry->primitiveCount(); ++index) {
        const Primitive* primitive = geometry->primitive(index);
        if (primitive == nullptr || !isTriangleTopology(primitive->topology)) {
            continue; // a line or point primitive is not a surface
        }
        // One stream for the whole draw, and the tree prepared over that same stream: the linear walk, the tree's
        // build and the ordinal a tree query reports all take "which triangle is number t" from here.
        const DrawTriangleStream view(*geometry, index);
        if (!view.valid()) {
            continue;
        }
        const TriangleStream&              stream = view.stream();
        const vn::geom::spatial::Bvh<double>* tree = geometry->bvh(index);
        const auto tag = [&](RayIntersectionResult& hit, std::uint32_t instance) {
            hit.primitiveIndex = static_cast<std::uint32_t>(index);
            hit.geometry       = intrusive_ptr<Geometry>(const_cast<Geometry*>(geometry));
            hit.instanceIndex  = instance;
        };
        if (!primitive->hasInstances()) {
            tests += traverseMesh(stream, tree, ray, world, policy, [&](const RayIntersectionResult& hit) {
                RayIntersectionResult tagged = hit;
                tag(tagged, std::numeric_limits<std::uint32_t>::max());
                visit(tagged);
            }).tests;
            continue;
        }
        const std::size_t visible = primitive->visibleInstanceCount();
        for (std::size_t instance = 0; instance < visible; ++instance) {
            const std::optional<Mat4d> matrix = primitive->instanceMatrix(instance);
            if (!matrix.has_value()) {
                continue; // the buffer shrank under the visible count
            }
            tests += traverseMesh(stream, tree, ray, world * *matrix, policy, [&](const RayIntersectionResult& hit) {
                RayIntersectionResult tagged = hit;
                tag(tagged, static_cast<std::uint32_t>(instance));
                visit(tagged);
            }).tests;
        }
    }
    // The witness of the tree cache: what this pick cost, so a caller can see whether prepareBvh() paid off.
    GeometryPickStats::record(*geometry, tests);
}

/**
 * @brief Builds the world-space hit record for a triangle that was hit.
 *
 * @param a               Triangle vertex A (local).
 * @param b               Triangle vertex B (local).
 * @param c               Triangle vertex C (local).
 * @param local_ray       The ray in the mesh's local space (unit direction).
 * @param t               Hit distance along the local ray.
 * @param triangle_index  Triangle index within the mesh.
 * @param ray             The world-space ray.
 * @param world           World matrix of the geometry.
 * @return The populated hit result (hit == true).
 */
RayIntersectionResult makeTriangleHit(const Vec3d&             a,
                                      const Vec3d&             b,
                                      const Vec3d&             c,
                                      const vn::geom::spatial::Rayd& local_ray,
                                      double                   t,
                                      std::size_t              triangle_index,
                                      const vn::geom::spatial::Rayd& ray,
                                      const Mat4d&             world)
{
    RayIntersectionResult   r;
    const vn::math::Point3d local_hit = local_ray.pointAt(t);
    const Vec3d             world_hit = toWorld(world, local_hit.x, local_hit.y, local_hit.z);
    r.hit                             = true;
    r.point                           = world_hit;
    r.normal                          = faceNormal(toWorld(world, a.x, a.y, a.z), toWorld(world, b.x, b.y, b.z), toWorld(world, c.x, c.y, c.z));
    r.distance                        = (world_hit - Vec3d(ray.origin.x, ray.origin.y, ray.origin.z)).length();
    r.triangleIndex                 = triangle_index;
    return r;
}

/**
 * @brief Walks a draw's triangles and calls on_hit for each hit it reports.
 *
 * Two walks, ONE set of ordinals. Without a tree the geometry is rejected when the ray misses its world-space AABB
 * (cheap slab test) and then every triangle is tested. With a tree the tree's own boxes do that rejection and the
 * walk only tests the candidates it hands out - the exact test, the reported ordinal and the hit record are the same
 * code in both cases, so the two answers can only differ in what they cost.
 *
 * Triangles are tested in LOCAL space: the ray is transformed once instead of transforming every vertex into world
 * space, which removes a 4x4 multiply per vertex and naturally honours non-uniform scaling. The ray is normalized on
 * the way in - geometry's Ray keeps its direction unit length - so the t it reports can be replayed with pointAt().
 *
 * A SINGULAR world matrix is rejected here, before the local space is built.
 * Matrix4x4::inverted() cannot report failure — it returns the original matrix
 * unchanged — so an inversion that was not checked silently tested the ray in
 * a space that is not the geometry's at all, which reports a hit for content
 * the ray never crossed (or misses content it did): a wrong answer from a
 * function whose whole contract is the answer. A world transform that
 * collapses a dimension has no volume to intersect, so nothing is picked
 * there — the same visible outcome as a hidden node.
 *
 * @param stream     The draw's triangle stream; its ordinal IS the reported triangleIndex, so a tree built over the
 *                   same stream reports the same number.
 * @param tree       The tree prepared over @p stream, or null to walk every triangle.
 * @param ray        The world-space ray.
 * @param world      World matrix of the geometry.
 * @param policy     Whether only the closest hit matters (then the walk may prune by distance).
 * @param on_hit     Invoked for each reported triangle hit with its hit result.
 * @return Whether anything was hit and how many exact triangle tests it took.
 */
template <typename OnHit>
MeshWalk traverseMesh(const TriangleStream&                    stream,
                      const vn::geom::spatial::Bvh<double>*    tree,
                      const vn::geom::spatial::Rayd&           ray,
                      const Mat4d&                             world,
                      HitPolicy                                policy,
                      OnHit&&                                  on_hit)
{
    MeshWalk walk;
    if (stream.positions().empty()) {
        return walk;
    }
    if (tree == nullptr) {
        const Aabbd local_bounds = boundsOfPositions(stream.positions());
        if (local_bounds.isValid()) {
            vn::geom::spatial::RayHitd world_hit;
            if (!local_bounds.transformed(world).intersectWith(ray, world_hit)) {
                return walk; // the world-space box is the cheap rejection; the mesh itself is tested in local space
            }
        }
    }

    // Copy first: invert() reports failure by returning false and leaving the matrix as it was
    // (see the note above), so the check has to be on THIS copy, not on the caller's matrix.
    Mat4d inv = world;
    if (!inv.invert()) {
        return walk; // singular world transform: no local space to test the ray in
    }
    const Vec3d local_origin = toWorld(inv, ray.origin.x, ray.origin.y, ray.origin.z);
    const Vec3d local_dir    = toWorld(inv, ray.origin.x + ray.direction.x, ray.origin.y + ray.direction.y, ray.origin.z + ray.direction.z) - local_origin;

    // geometry's ray keeps its direction unit length, so a hit parameter is a distance in this local space.
    const vn::geom::spatial::Rayd local_ray(vn::math::Point3d(local_origin.x, local_origin.y, local_origin.z), local_dir);

    const auto vertex = [&](std::uint32_t index) {
        const auto& p = stream.positions()[index];
        return Vec3d(p.x, p.y, p.z);
    };

    // The distance limit a hit must beat to be worth reporting. It stays infinite when every hit matters, which is
    // what keeps the AllHits answer identical to the linear walk's.
    double limit = std::numeric_limits<double>::infinity();

    const auto test = [&](std::size_t triangle) {
        std::array<std::uint32_t, 3> corners{};
        if (!stream.triangleCorners(triangle, corners)) {
            return; // a corner that names no stored vertex is not a triangle of this draw
        }
        const Vec3d                a = vertex(corners[0]);
        const Vec3d                b = vertex(corners[1]);
        const Vec3d                c = vertex(corners[2]);
        vn::geom::spatial::RayHitd local_hit;
        ++walk.tests;
        if (!vn::geom::spatial::intersectTriangle(local_ray,
                                                 vn::math::Point3d(a.x, a.y, a.z),
                                                 vn::math::Point3d(b.x, b.y, b.z),
                                                 vn::math::Point3d(c.x, c.y, c.z),
                                                 local_hit)) {
            return;
        }
        if (local_hit.t > limit) {
            return; // cannot become the closest hit, and only the closest hit matters
        }
        if (policy == HitPolicy::kNearest) {
            limit = local_hit.t;
        }
        walk.any = true;
        on_hit(makeTriangleHit(a, b, c, local_ray, local_hit.t, triangle, ray, world));
    };

    if (tree != nullptr) {
        tree->forEachRay(local_ray, [&](const vn::geom::spatial::Bvh<double>::PrimId id, const Aabbd&, double& max_t) {
            // The tree hands over its own working limit, and a closer hit tightens it - which is what prunes the rest
            // of the tree. A "every hit matters" walk keeps the limit infinite instead.
            limit = policy == HitPolicy::kNearest ? max_t : std::numeric_limits<double>::infinity();
            test(static_cast<std::size_t>(id));
            if (policy == HitPolicy::kNearest) {
                max_t = limit;
            }
            return true;
        });
        return walk;
    }

    const std::size_t count = stream.triangleCount();
    for (std::size_t triangle = 0; triangle < count; ++triangle) {
        test(triangle);
    }
    return walk;
}

/**
 * @brief Recursively keeps the closest hit of a ray over a node subtree.
 *
 * @param ray              The ray.
 * @param node             Node to traverse.
 * @param inherited_layers Layers @p node inherits from its ancestors (kDefaultLayer at the top).
 * @param layer_mask       Layers this query picks (see Node::setLayers).
 * @param best             Closest intersection found so far (in/out).
 */
void intersectNode(const vn::geom::spatial::Rayd& ray, const Node* node, std::uint32_t inherited_layers, std::uint32_t layer_mask, RayIntersectionResult& best)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if ((node->subtreeLayerUnion(inherited_layers) & layer_mask) == 0) {
        return; // no resolved layer in this subtree can match: skip it whole (see Node::subtreeLayerUnion)
    }
    const std::uint32_t layers = node->layers().value_or(inherited_layers);
    if (const auto* geometry = obj_cast<Geometry>(node)) {
        // The content selection, taken at the drawn node like the collection walk (see
        // Scene::collectRenderCommands): a container stands for structure, so it is never pruned.
        if ((layers & layer_mask) == 0) {
            return;
        }
        // Leaf geometry: test against its data placed by the ancestor
        // MatrixTransform chain.
        const Mat4d world = node->worldMatrix();
        forEachTriangleHit(geometry, ray, world, HitPolicy::kNearest, [&](RayIntersectionResult hit) {
            if (hit.distance < best.distance) {
                best = std::move(hit);
            }
        });
        return;
    }
    if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            intersectNode(ray, child.get(), layers, layer_mask, best);
        }
    }
}

/**
 * @brief Recursively collects the closest hit of each geometry in a subtree.
 *
 * @param ray              The ray.
 * @param node             Node to traverse.
 * @param inherited_layers Layers @p node inherits from its ancestors (kDefaultLayer at the top).
 * @param layer_mask       Layers this query picks (see Node::setLayers).
 * @param out              Output result list (one entry per geometry that was hit).
 */
void collectNodeNearest(const vn::geom::spatial::Rayd& ray, const Node* node, std::uint32_t inherited_layers, std::uint32_t layer_mask, std::vector<RayIntersectionResult>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if ((node->subtreeLayerUnion(inherited_layers) & layer_mask) == 0) {
        return;
    }
    const std::uint32_t layers = node->layers().value_or(inherited_layers);
    if (const auto* geometry = obj_cast<Geometry>(node)) {
        // The content selection, taken at the drawn node like the collection walk (see
        // Scene::collectRenderCommands): a container stands for structure, so it is never pruned.
        if ((layers & layer_mask) == 0) {
            return;
        }
        const Mat4d           world = node->worldMatrix();
        RayIntersectionResult best;
        forEachTriangleHit(geometry, ray, world, HitPolicy::kNearest, [&](RayIntersectionResult hit) {
            if (hit.distance < best.distance) {
                best = std::move(hit);
            }
        });
        if (best.hit) {
            out.push_back(std::move(best));
        }
        return;
    }
    if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            collectNodeNearest(ray, child.get(), layers, layer_mask, out);
        }
    }
}

/**
 * @brief Recursively collects every triangle hit over a node subtree.
 *
 * @param ray              The ray.
 * @param node             Node to traverse.
 * @param inherited_layers Layers @p node inherits from its ancestors (kDefaultLayer at the top).
 * @param layer_mask       Layers this query picks (see Node::setLayers).
 * @param out              Output result list (multiple entries per geometry possible).
 */
void collectNodeHits(const vn::geom::spatial::Rayd& ray, const Node* node, std::uint32_t inherited_layers, std::uint32_t layer_mask, std::vector<RayIntersectionResult>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if ((node->subtreeLayerUnion(inherited_layers) & layer_mask) == 0) {
        return;
    }
    const std::uint32_t layers = node->layers().value_or(inherited_layers);
    if (const auto* geometry = obj_cast<Geometry>(node)) {
        // The content selection, taken at the drawn node like the collection walk (see
        // Scene::collectRenderCommands): a container stands for structure, so it is never pruned.
        if ((layers & layer_mask) == 0) {
            return;
        }
        const Mat4d world = node->worldMatrix();
        forEachTriangleHit(geometry, ray, world, HitPolicy::kAll, [&](RayIntersectionResult hit) {
            RayIntersectionResult copy = std::move(hit);
            out.push_back(std::move(copy));
        });
        return;
    }
    if (const auto* group = obj_cast<Group>(node)) {
        for (const auto& child : group->children()) {
            collectNodeHits(ray, child.get(), layers, layer_mask, out);
        }
    }
}

} // namespace

RayIntersectionResult RayIntersection::intersect(const vn::geom::spatial::Rayd& ray, raw_ptr<Geometry> geometry, const Mat4d& world)
{
    RayIntersectionResult result;
    if (geometry == nullptr) {
        return result;
    }
    forEachTriangleHit(geometry, ray, world, HitPolicy::kNearest, [&](RayIntersectionResult hit) {
        if (hit.distance < result.distance) {
            result = std::move(hit);
        }
    });
    if (result.hit) {
        result.geometry = geometry;
    }
    return result;
}

std::vector<RayIntersectionResult> RayIntersection::intersect(const vn::geom::spatial::Rayd& ray, raw_ptr<Geometry> geometry, const Mat4d& world, Mode mode)
{
    std::vector<RayIntersectionResult> results;
    if (geometry == nullptr) {
        return results;
    }
    if (mode == Mode::Nearest) {
        RayIntersectionResult r = intersect(ray, geometry, world);
        if (r.hit) {
            results.push_back(std::move(r));
        }
        return results;
    }
    forEachTriangleHit(geometry, ray, world, HitPolicy::kAll, [&](RayIntersectionResult hit) {
        RayIntersectionResult copy = std::move(hit);
        copy.geometry              = geometry;
        results.push_back(std::move(copy));
    });
    return results;
}

RayIntersectionResult RayIntersection::intersectScene(const vn::geom::spatial::Rayd& ray, raw_ptr<Scene> scene, std::uint32_t layer_mask)
{
    RayIntersectionResult best;
    if (scene == nullptr) {
        return best;
    }
    if (const auto root = scene->root(); root != nullptr) {
        intersectNode(ray, root.get(), kDefaultLayer, layer_mask, best);
    }
    return best;
}

std::vector<RayIntersectionResult> RayIntersection::intersectSceneAll(const vn::geom::spatial::Rayd& ray, raw_ptr<Scene> scene, Mode mode, std::uint32_t layer_mask)
{
    std::vector<RayIntersectionResult> results;
    if (scene == nullptr) {
        return results;
    }
    if (const auto root = scene->root(); root != nullptr) {
        if (mode == Mode::Nearest) {
            collectNodeNearest(ray, root.get(), kDefaultLayer, layer_mask, results);
        }
        else {
            collectNodeHits(ray, root.get(), kDefaultLayer, layer_mask, results);
        }
    }
    std::sort(results.begin(), results.end(), [](const RayIntersectionResult& lhs, const RayIntersectionResult& rhs) { return lhs.distance < rhs.distance; });
    return results;
}

VN_GRAPHICS_NS_END
