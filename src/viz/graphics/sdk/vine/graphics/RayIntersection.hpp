#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <vine/math/Vector3.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Node.hpp"
#include "Ray.hpp"

VN_GRAPHICS_NS_BEGIN

using vn::math::Vec3d;
using vn::math::Vec2d;
using vn::math::Mat4d;

class Geometry;
class Scene;

/**
 * @brief Result of ray-geometry intersection test.
 *
 * @details
 * - Everything a consumer needs from one hit: whether it happened, where (point / normal / distance),
 *   which geometry, and which triangle (max() when not applicable).
 */
struct VN_GRAPHICS_API RayIntersectionResult {
    /** Whether the ray hit the geometry. */
    bool hit = false;

    /** Intersection point position (world space). */
    Vec3d point;

    /** Surface normal at intersection point. */
    Vec3d normal;

    /** Distance from ray origin to intersection point. */
    double distance = std::numeric_limits<double>::max();

    /** Intersected geometry. */
    intrusive_ptr<Geometry> geometry;

    /** Triangle index (if applicable, otherwise max). */
    std::size_t triangleIndex = std::numeric_limits<std::size_t>::max();

    /** Which of the geometry's primitives was hit (see Geometry::primitive). A geometry is picked through its
     *  TRIANGLE primitives only - a line or point primitive is not a surface - so this names the part that
     *  was actually intersected. */
    std::uint32_t primitiveIndex = 0;
};

/**
 * @brief Ray-geometry intersection computation.
 *
 * @details
 * - Static queries against one geometry or a whole scene.
 */
class VN_GRAPHICS_API RayIntersection {
  public:
    /** @brief Ray query mode controlling how many hits are collected. */
    enum class Mode {
        Nearest,  ///< Only the closest intersection per geometry (fastest).
        AllHits,  ///< Every triangle intersection, sorted near to far.
    };

    /** @brief Tests ray intersection with a single geometry (closest hit only).
     *
     * @details
     * - A geometry whose @p world is SINGULAR (a zero scale on any axis, so the transform loses a
     *   dimension) has no volume to intersect: it is reported as a miss rather than tested in a space it
     *   does not have.
     *
     * @param ray      The ray to test.
     * @param geometry The geometry to intersect with.
     * @param world    World transform of the geometry (must be invertible to be picked).
     * @return Intersection result; hit=false when there is no intersection.
     */
    static RayIntersectionResult intersect(const Ray& ray, raw_ptr<Geometry> geometry,
                                           const Mat4d& world);

    /** @brief Tests ray intersection with a single geometry for a given mode.
     *
     * @details
     * - Nearest returns at most the closest hit; AllHits returns every triangle hit of the geometry
     *   (front and back faces), each tagged with the geometry, in no particular order - sort by distance
     *   when ordering is needed.
     *
     * @param ray      The ray to test.
     * @param geometry The geometry to intersect with.
     * @param world    World transform of the geometry.
     * @param mode     Query mode.
     * @return Vector of intersection results (may be empty).
     */
    static std::vector<RayIntersectionResult> intersect(const Ray& ray, raw_ptr<Geometry> geometry,
                                                        const Mat4d& world, Mode mode);

    /** @brief Tests ray intersection with all geometries in a scene (closest hit only).
     *
     * @details
     * - The @p layer_mask selects like the collection does (see Scene::collectRenderCommands): a visible
     *   geometry whose resolved layers do not intersect the mask is left out of the query; a node that
     *   re-declares its layers is picked under its own declaration (containers are structure - the test is
     *   taken at the drawn geometry); kAllLayers (the default) picks everything visible, exactly as before.
     *
     * @param ray        The ray to test.
     * @param scene      The scene containing geometries.
     * @param layer_mask Layers this query picks (see Node::setLayers).
     * @return Intersection result; hit=false when there is no intersection.
     */
    static RayIntersectionResult intersectScene(const Ray& ray, raw_ptr<Scene> scene,
                                                std::uint32_t layer_mask = kAllLayers);

    /** @brief Tests ray intersection with all geometries in a scene, for a given mode.
     *
     * @details
     * - Results are sorted by distance (near to far). Nearest returns the closest hit of every geometry;
     *   AllHits returns every triangle hit in the scene (multiple hits per geometry, including back faces).
     * - The @p layer_mask selects like the collection does (see Scene::collectRenderCommands): a visible
     *   geometry whose resolved layers do not intersect the mask is left out; a re-declaration counts;
     *   kAllLayers (the default) picks everything visible.
     *
     * @param ray        The ray to test.
     * @param scene      The scene containing geometries.
     * @param mode       Query mode; defaults to AllHits.
     * @param layer_mask Layers this query picks (see Node::setLayers).
     * @return Vector of intersection results (may be empty).
     */
    static std::vector<RayIntersectionResult> intersectSceneAll(const Ray& ray, raw_ptr<Scene> scene,
                                                                Mode mode = Mode::AllHits,
                                                                std::uint32_t layer_mask = kAllLayers);
};

VN_GRAPHICS_NS_END
