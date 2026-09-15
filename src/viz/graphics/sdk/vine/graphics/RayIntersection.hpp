#pragma once
#include "graphics_global.hpp"

#include <limits>
#include <vector>

#include <vine/math/Vector3.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Ray.hpp"

V_GRAPHICS_NS_BEGIN

using vine::math::Vec3d;
using vine::math::Vec2d;
using vine::math::Mat4d;

class Geometry;
class Scene;

/**
 * @brief Result of ray-geometry intersection test.
 *
 * Contains detailed information about the intersection point, if any.
 */
struct V_GRAPHICS_API RayIntersectionResult {
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
};

/**
 * @brief Ray-geometry intersection computation.
 *
 * Provides static methods for computing ray-geometry intersections,
 * supporting both single geometry and full scene queries.
 */
class V_GRAPHICS_API RayIntersection {
  public:
    /** @brief Ray query mode controlling how many hits are collected. */
    enum class Mode {
        Nearest,  ///< Only the closest intersection per geometry (fastest).
        AllHits,  ///< Every triangle intersection, sorted near to far.
    };

    /** @brief Tests ray intersection with a single geometry.
     *
     * Returns the closest intersection only.
     *
     * A geometry whose @p world is SINGULAR (a zero scale on any axis, so the
     * transform loses a dimension) has no volume to intersect: it is reported
     * as a miss rather than tested in a space it does not have.
     *
     * @param ray      The ray to test.
     * @param geometry The geometry to intersect with.
     * @param world    World transform of the geometry (must be invertible to be picked).
     * @return Intersection result. hit=false if no intersection.
     */
    static RayIntersectionResult intersect(const Ray& ray, raw_ptr<Geometry> geometry,
                                           const Mat4d& world);

    /** @brief Tests ray intersection with a single geometry for a given mode.
     *
     * Nearest returns at most the closest hit; AllHits returns every triangle
     * hit of the geometry (front and back faces), each tagged with the
     * geometry, in no particular order — sort by distance when ordering is
     * needed.
     *
     * @param ray      The ray to test.
     * @param geometry The geometry to intersect with.
     * @param world    World transform of the geometry.
     * @param mode     Query mode.
     * @return Vector of intersection results (may be empty).
     */
    static std::vector<RayIntersectionResult> intersect(const Ray& ray, raw_ptr<Geometry> geometry,
                                                        const Mat4d& world, Mode mode);

    /** @brief Tests ray intersection with all geometries in a scene.
     *
     * Returns only the closest intersection.
     *
     * @param ray   The ray to test.
     * @param scene The scene containing geometries.
     * @return Intersection result. hit=false if no intersection.
     */
    static RayIntersectionResult intersectScene(const Ray& ray, raw_ptr<Scene> scene);

    /** @brief Tests ray intersection with all geometries in a scene.
     *
     * Results are sorted by distance (near to far). Nearest returns the
     * closest hit of every geometry; AllHits returns every triangle hit in
     * the scene (multiple hits per geometry, including back faces).
     *
     * @param ray   The ray to test.
     * @param scene The scene containing geometries.
     * @param mode  Query mode; defaults to AllHits.
     * @return Vector of intersection results (may be empty).
     */
    static std::vector<RayIntersectionResult> intersectSceneAll(const Ray& ray,
                                                                raw_ptr<Scene> scene,
                                                                Mode mode = Mode::AllHits);
};

V_GRAPHICS_NS_END
