#include <vine/graphics/RayIntersection.hpp>

#include <algorithm>
#include <utility>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/math/Transform3.hpp>

V_GRAPHICS_NS_BEGIN

namespace
{

/**
 * @brief Ray-triangle intersection using the Möller–Trumbore algorithm.
 *
 * The ray is given by origin + dir * t; dir does not need to be unit length
 * (t is measured along dir). All inputs must live in the same space.
 *
 * @param origin Ray origin.
 * @param dir    Ray direction (not normalised).
 * @param a      Triangle vertex A.
 * @param b      Triangle vertex B.
 * @param c      Triangle vertex C.
 * @param out_t  Receives the hit distance when intersecting.
 * @return true when the ray hits the triangle.
 */
bool intersectTriangle(const Vec3d& origin, const Vec3d& dir,
                       const Vec3d& a, const Vec3d& b, const Vec3d& c,
                       double& out_t)
{
    constexpr double eps = 1e-9;
    const Vec3d edge1 = b - a;
    const Vec3d edge2 = c - a;
    const Vec3d h = dir.cross(edge2);
    const double det = edge1.dot(h);
    if (std::abs(det) < eps) {
        return false;
    }
    const double inv_det = 1.0 / det;
    const Vec3d s = origin - a;
    const double u = s.dot(h) * inv_det;
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    const Vec3d q = s.cross(edge1);
    const double v = dir.dot(q) * inv_det;
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    const double t = edge2.dot(q) * inv_det;
    if (t > eps) {
        out_t = t;
        return true;
    }
    return false;
}

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

// Forward declaration: defined below but used by transformBounds().
Vec3d toWorld(const Mat4d& m, double x, double y, double z);

/**
 * @brief Slab test: whether a ray (origin + dir * t, t >= 0) hits an AABB.
 *
 * @param origin Ray origin.
 * @param dir    Ray direction (need not be unit length).
 * @param box    Axis-aligned box in the same space.
 * @return true when the ray intersects the box.
 */
bool rayIntersectsBox(const Vec3d& origin, const Vec3d& dir, const Aabbd& box)
{
    constexpr double eps = 1e-12;
    double t_near = 0.0;
    double t_far = std::numeric_limits<double>::max();
    const double o[3] = { origin.x, origin.y, origin.z };
    const double d[3] = { dir.x, dir.y, dir.z };
    const double lo[3] = { box.min().x, box.min().y, box.min().z };
    const double hi[3] = { box.max().x, box.max().y, box.max().z };
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < eps) {
            if (o[i] < lo[i] || o[i] > hi[i]) {
                return false;
            }
            continue;
        }
        double t1 = (lo[i] - o[i]) / d[i];
        double t2 = (hi[i] - o[i]) / d[i];
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_near = std::max(t_near, t1);
        t_far = std::min(t_far, t2);
        if (t_near > t_far) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Transforms a local-space AABB by an affine matrix.
 *
 * The result is the world-space AABB enclosing all eight transformed corners.
 *
 * @param local Box in local space.
 * @param m     World matrix of the geometry.
 * @return World-space AABB (empty when the local box is empty).
 */
Aabbd transformBounds(const Aabbd& local, const Mat4d& m)
{
    Aabbd box = Aabbd::empty();
    if (!local.isValid()) {
        return box;
    }
    const double x[2] = { local.min().x, local.max().x };
    const double y[2] = { local.min().y, local.max().y };
    const double z[2] = { local.min().z, local.max().z };
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                box.expandBy(toWorld(m, x[i], y[j], z[k]));
            }
        }
    }
    return box;
}

/**
 * @brief Computes the local AABB enclosing the given positions.
 *
 * @param positions Position array.
 * @return The bounding box (empty for an empty array).
 */
Aabbd boundsOfPositions(const vine::geometry::Vec3fArray& positions)
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
    const vine::math::Point3d p = m * vine::math::Point3d(x, y, z);
    return Vec3d(p.x, p.y, p.z);
}

/**
 * @brief Owned vertex/index data resolved from a buffer-only Geometry.
 *
 * Positions are materialised from the packed location-0 attribute into a
 * typed array the picker can walk; indices (when present) alias the
 * geometry's index buffer. Lifetime spans the caller's picking call only.
 */
struct GeometryMesh {
    vine::geometry::Vec3fArray positions;
    const vine::geometry::UInt32Array* indices = nullptr;

    /** @brief Returns whether position data is available. */
    bool valid() const { return !positions.empty(); }
};

/**
 * @brief Resolves the vertex/index arrays of a buffer-only Geometry.
 *
 * @param geometry Geometry to read.
 * @return GeometryMesh with no positions for an empty geometry.
 */
GeometryMesh meshOfGeometry(const Geometry* geometry)
{
    GeometryMesh mesh;
    if (geometry != nullptr) {
        if (const auto* position_attr = geometry->buffer(0);
            position_attr != nullptr && !position_attr->empty() &&
            position_attr->components >= 3u) {
            // The declared components are the stride: a vec4 position channel
            // keeps its xyz and skips w, exactly as the backend uploads it.
            const std::size_t count = position_attr->vertexCount();
            mesh.positions.reserve(count);
            for (std::size_t v = 0; v < count; ++v) {
                const std::array<float, 3> p = position_attr->xyz(v);
                mesh.positions.emplace_back(p[0], p[1], p[2]);
            }
        }
        if (geometry->hasIndices()) {
            mesh.indices = geometry->indices();
        }
    }
    return mesh;
}

/**
 * @brief Builds the world-space hit record for a triangle that was hit.
 *
 * @param a               Triangle vertex A (local).
 * @param b               Triangle vertex B (local).
 * @param c               Triangle vertex C (local).
 * @param local_origin    Ray origin in local space.
 * @param local_dir       Ray direction in local space (un-normalised).
 * @param t               Hit distance along the local direction.
 * @param triangle_index  Triangle index within the mesh.
 * @param ray             The world-space ray.
 * @param world           World matrix of the geometry.
 * @return The populated hit result (hit == true).
 */
RayIntersectionResult makeTriangleHit(const Vec3d& a, const Vec3d& b, const Vec3d& c,
                                      const Vec3d& local_origin, const Vec3d& local_dir,
                                      double t, std::size_t triangle_index,
                                      const Ray& ray, const Mat4d& world)
{
    RayIntersectionResult r;
    const Vec3d local_hit = local_origin + local_dir * t;
    const Vec3d world_hit = toWorld(world, local_hit.x, local_hit.y, local_hit.z);
    r.hit = true;
    r.point = world_hit;
    r.normal = faceNormal(toWorld(world, a.x, a.y, a.z),
                          toWorld(world, b.x, b.y, b.z),
                          toWorld(world, c.x, c.y, c.z));
    r.distance = (world_hit - ray.origin).length();
    r.triangleIndex = triangle_index;
    return r;
}

/**
 * @brief Walks every triangle of a mesh and calls on_hit for each hit.
 *
 * The geometry is rejected when the ray misses its world-space AABB (cheap
 * slab test). Surviving meshes are tested in LOCAL space: the ray is
 * transformed once instead of transforming every vertex into world space,
 * which removes a 4x4 multiply per vertex and naturally honours non-uniform
 * scaling. The Möller–Trumbore t is measured along the un-normalised local
 * direction, so the local hit is recovered with the same vector that was
 * tested.
 *
 * @param positions  Vertex positions (local space).
 * @param indices    Triangle index array, or null for consecutive triples.
 * @param ray        The world-space ray.
 * @param world      World matrix of the geometry.
 * @param on_hit     Invoked for each triangle hit with its hit result.
 * @return true when at least one triangle was hit.
 */
template <typename OnHit>
bool traverseMesh(const vine::geometry::Vec3fArray& positions,
                  const vine::geometry::UInt32Array* indices,
                  const Ray& ray, const Mat4d& world, OnHit&& on_hit)
{
    if (positions.empty()) {
        return false;
    }
    const Aabbd local_bounds = boundsOfPositions(positions);
    if (local_bounds.isValid() &&
        !rayIntersectsBox(ray.origin, ray.direction, transformBounds(local_bounds, world))) {
        return false;
    }

    const Mat4d inv = world.inverted();
    const Vec3d local_origin =
        toWorld(inv, ray.origin.x, ray.origin.y, ray.origin.z);
    const Vec3d local_dir =
        toWorld(inv, ray.origin.x + ray.direction.x, ray.origin.y + ray.direction.y,
                ray.origin.z + ray.direction.z) -
        local_origin;

    const std::size_t n = indices != nullptr ? indices->size() : positions.size();
    bool any = false;
    for (std::size_t tri = 0; tri + 2 < n; tri += 3) {
        std::size_t i0 = tri;
        std::size_t i1 = tri + 1;
        std::size_t i2 = tri + 2;
        if (indices != nullptr) {
            i0 = (*indices)[tri];
            i1 = (*indices)[tri + 1];
            i2 = (*indices)[tri + 2];
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) {
                continue;
            }
        }
        const Vec3d a(positions[i0].x, positions[i0].y, positions[i0].z);
        const Vec3d b(positions[i1].x, positions[i1].y, positions[i1].z);
        const Vec3d c(positions[i2].x, positions[i2].y, positions[i2].z);
        double t = 0.0;
        if (!intersectTriangle(local_origin, local_dir, a, b, c, t)) {
            continue;
        }
        any = true;
        on_hit(makeTriangleHit(a, b, c, local_origin, local_dir, t, tri / 3, ray, world));
    }
    return any;
}

/**
 * @brief Recursively keeps the closest hit of a ray over a node subtree.
 *
 * @param ray   The ray.
 * @param node  Node to traverse.
 * @param best  Closest intersection found so far (in/out).
 */
void intersectNode(const Ray& ray, const Node* node, RayIntersectionResult& best)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        // Leaf geometry: test against its data placed by the ancestor
        // MatrixTransform chain.
        const GeometryMesh mesh = meshOfGeometry(geometry);
        if (!mesh.valid()) {
            return;
        }
        const Mat4d world = node->worldMatrix();
        Geometry* geom = const_cast<Geometry*>(geometry);
        traverseMesh(mesh.positions, mesh.indices, ray, world,
                     [&](const RayIntersectionResult& hit) {
                         if (hit.distance < best.distance) {
                             best = hit;
                             best.geometry = geom;
                         }
                     });
        return;
    }
    if (const auto* group = dynamic_cast<const Group*>(node)) {
        for (const auto& child : group->childrenRef()) {
            intersectNode(ray, child.get(), best);
        }
    }
}

/**
 * @brief Recursively collects the closest hit of each geometry in a subtree.
 *
 * @param ray   The ray.
 * @param node  Node to traverse.
 * @param out   Output result list (one entry per geometry that was hit).
 */
void collectNodeNearest(const Ray& ray, const Node* node,
                        std::vector<RayIntersectionResult>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        const Mat4d world = node->worldMatrix();
        RayIntersectionResult r = RayIntersection::intersect(
            ray, const_cast<Geometry*>(geometry), world);
        if (r.hit) {
            out.push_back(std::move(r));
        }
        return;
    }
    if (const auto* group = dynamic_cast<const Group*>(node)) {
        for (const auto& child : group->childrenRef()) {
            collectNodeNearest(ray, child.get(), out);
        }
    }
}

/**
 * @brief Recursively collects every triangle hit over a node subtree.
 *
 * @param ray   The ray.
 * @param node  Node to traverse.
 * @param out   Output result list (multiple entries per geometry possible).
 */
void collectNodeHits(const Ray& ray, const Node* node,
                     std::vector<RayIntersectionResult>& out)
{
    if (node == nullptr || !node->isVisible()) {
        return;
    }
    if (const auto* geometry = dynamic_cast<const Geometry*>(node)) {
        const GeometryMesh mesh = meshOfGeometry(geometry);
        if (!mesh.valid()) {
            return;
        }
        const Mat4d world = node->worldMatrix();
        Geometry* geom = const_cast<Geometry*>(geometry);
        traverseMesh(mesh.positions, mesh.indices, ray, world,
                     [&](const RayIntersectionResult& hit) {
                         RayIntersectionResult copy = hit;
                         copy.geometry = geom;
                         out.push_back(std::move(copy));
                     });
        return;
    }
    if (const auto* group = dynamic_cast<const Group*>(node)) {
        for (const auto& child : group->childrenRef()) {
            collectNodeHits(ray, child.get(), out);
        }
    }
}

}  // namespace

RayIntersectionResult RayIntersection::intersect(const Ray& ray, raw_ptr<Geometry> geometry,
                                                 const Mat4d& world)
{
    RayIntersectionResult result;
    if (geometry == nullptr) {
        return result;
    }
    const GeometryMesh mesh = meshOfGeometry(geometry);
    if (!mesh.valid()) {
        return result;
    }
    traverseMesh(mesh.positions, mesh.indices, ray, world,
                 [&](const RayIntersectionResult& hit) {
                     if (hit.distance < result.distance) {
                         result = hit;
                     }
                 });
    if (result.hit) {
        result.geometry = geometry;
    }
    return result;
}

std::vector<RayIntersectionResult> RayIntersection::intersect(const Ray& ray,
                                                              raw_ptr<Geometry> geometry,
                                                              const Mat4d& world, Mode mode)
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
    const GeometryMesh mesh = meshOfGeometry(geometry);
    if (!mesh.valid()) {
        return results;
    }
    traverseMesh(mesh.positions, mesh.indices, ray, world,
                 [&](const RayIntersectionResult& hit) {
                     RayIntersectionResult copy = hit;
                     copy.geometry = geometry;
                     results.push_back(std::move(copy));
                 });
    return results;
}

RayIntersectionResult RayIntersection::intersectScene(const Ray& ray, raw_ptr<Scene> scene)
{
    RayIntersectionResult best;
    if (scene == nullptr) {
        return best;
    }
    if (const auto root = scene->root(); root != nullptr) {
        intersectNode(ray, root.get(), best);
    }
    return best;
}

std::vector<RayIntersectionResult> RayIntersection::intersectSceneAll(const Ray& ray,
                                                                      raw_ptr<Scene> scene,
                                                                      Mode mode)
{
    std::vector<RayIntersectionResult> results;
    if (scene == nullptr) {
        return results;
    }
    if (const auto root = scene->root(); root != nullptr) {
        if (mode == Mode::Nearest) {
            collectNodeNearest(ray, root.get(), results);
        } else {
            collectNodeHits(ray, root.get(), results);
        }
    }
    std::sort(results.begin(), results.end(),
              [](const RayIntersectionResult& lhs, const RayIntersectionResult& rhs) {
                  return lhs.distance < rhs.distance;
              });
    return results;
}

V_GRAPHICS_NS_END
