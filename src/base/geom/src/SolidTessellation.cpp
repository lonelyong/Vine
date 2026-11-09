#include <vine/geom/spatial/SolidTessellation.hpp>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <vine/math/Math.hpp>
#include <vine/math/Vector3.hpp>

VN_GEOM_SPATIAL_NS_BEGIN

namespace
{

/**
 * @brief Accumulates positions, normals and triangle indices for one tessellation.
 *
 * @tparam T The precision of the solid being tessellated.
 */
template <vn::math::FP T>
struct TriangleBuilder {
    std::vector<vn::math::Vector3<T>> positions;
    std::vector<vn::math::Vector3<T>> normals;
    std::vector<std::uint32_t>        indices;

    /**
     * @brief Appends one vertex and returns its index.
     */
    [[nodiscard]] std::uint32_t vertex(const vn::math::Vector3<T>& position, const vn::math::Vector3<T>& normal)
    {
        const auto index = static_cast<std::uint32_t>(positions.size());
        positions.push_back(position);
        normals.push_back(normal);
        return index;
    }

    /**
     * @brief Appends one counter-clockwise triangle.
     */
    void triangle(std::uint32_t first, std::uint32_t second, std::uint32_t third)
    {
        indices.push_back(first);
        indices.push_back(second);
        indices.push_back(third);
    }

    /**
     * @brief Appends one counter-clockwise quad as two triangles.
     */
    void quad(std::uint32_t first, std::uint32_t second, std::uint32_t third, std::uint32_t fourth)
    {
        triangle(first, second, third);
        triangle(first, third, fourth);
    }

    /**
     * @brief Hands the accumulated arrays to the caller.
     */
    [[nodiscard]] Triangulation<T> finish()
    {
        return Triangulation<T>{ std::move(positions), std::move(normals), std::move(indices) };
    }
};

/**
 * @brief Appends one planar quad with one normal for all four corners.
 */
template <vn::math::FP T>
void appendQuad(TriangleBuilder<T>&       out,
                const vn::math::Vector3<T>& normal,
                const vn::math::Vector3<T>& p0,
                const vn::math::Vector3<T>& p1,
                const vn::math::Vector3<T>& p2,
                const vn::math::Vector3<T>& p3)
{
    const auto a = out.vertex(p0, normal);
    const auto b = out.vertex(p1, normal);
    const auto c = out.vertex(p2, normal);
    const auto d = out.vertex(p3, normal);
    out.quad(a, b, c, d);
}

/**
 * @brief Appends six quads, one per face, each with its own outward normal.
 */
template <vn::math::FP T>
void appendBox(TriangleBuilder<T>& out, T half_x, T half_y, T half_z)
{
    using Vec3 = vn::math::Vector3<T>;

    const T x = half_x;
    const T y = half_y;
    const T z = half_z;

    appendQuad(out, Vec3(1, 0, 0), Vec3(x, -y, -z), Vec3(x, y, -z), Vec3(x, y, z), Vec3(x, -y, z));
    appendQuad(out, Vec3(-1, 0, 0), Vec3(-x, y, -z), Vec3(-x, -y, -z), Vec3(-x, -y, z), Vec3(-x, y, z));
    appendQuad(out, Vec3(0, 1, 0), Vec3(x, y, -z), Vec3(-x, y, -z), Vec3(-x, y, z), Vec3(x, y, z));
    appendQuad(out, Vec3(0, -1, 0), Vec3(-x, -y, -z), Vec3(x, -y, -z), Vec3(x, -y, z), Vec3(-x, -y, z));
    appendQuad(out, Vec3(0, 0, 1), Vec3(-x, -y, z), Vec3(x, -y, z), Vec3(x, y, z), Vec3(-x, y, z));
    appendQuad(out, Vec3(0, 0, -1), Vec3(x, -y, -z), Vec3(-x, -y, -z), Vec3(-x, y, -z), Vec3(x, y, -z));
}

/**
 * @brief Appends a sphere / ellipsoid: latitude bands closed by two pole fans.
 */
template <vn::math::FP T>
void appendEllipsoid(TriangleBuilder<T>& out, T radius_x, T radius_y, T radius_z, int around, int along)
{
    using Vec3 = vn::math::Vector3<T>;

    const auto north = out.vertex(Vec3(0, 0, radius_z), Vec3(0, 0, 1));

    const auto                 ring_count = static_cast<std::size_t>(along - 1);
    std::vector<std::uint32_t> rings(ring_count * static_cast<std::size_t>(around));
    for (int row = 1; row < along; ++row) {
        const T polar     = static_cast<T>(vn::math::PI) * static_cast<T>(row) / static_cast<T>(along);
        const T sin_polar = std::sin(polar);
        const T cos_polar = std::cos(polar);
        for (int column = 0; column < around; ++column) {
            const T azimuth     = static_cast<T>(vn::math::PI_TWO) * static_cast<T>(column) / static_cast<T>(around);
            const T cos_azimuth = std::cos(azimuth);
            const T sin_azimuth = std::sin(azimuth);

            const Vec3 position(radius_x * sin_polar * cos_azimuth, radius_y * sin_polar * sin_azimuth, radius_z * cos_polar);
            const Vec3 normal =
                Vec3(position.x / (radius_x * radius_x), position.y / (radius_y * radius_y), position.z / (radius_z * radius_z)).normalized();
            rings[static_cast<std::size_t>(row - 1) * static_cast<std::size_t>(around) + static_cast<std::size_t>(column)] = out.vertex(position, normal);
        }
    }

    const auto south = out.vertex(Vec3(0, 0, -radius_z), Vec3(0, 0, -1));

    const auto ring = [&](int row, int column) {
        return rings[static_cast<std::size_t>(row) * static_cast<std::size_t>(around) + static_cast<std::size_t>(column)];
    };

    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.triangle(north, ring(0, column), ring(0, next));
    }

    for (int row = 1; row < along - 1; ++row) {
        for (int column = 0; column < around; ++column) {
            const int next = (column + 1) % around;
            out.quad(ring(row - 1, column), ring(row, column), ring(row, next), ring(row - 1, next));
        }
    }

    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.triangle(ring(along - 2, column), south, ring(along - 2, next));
    }
}

/**
 * @brief Appends a cylinder: a side band plus a cap fan at each end.
 */
template <vn::math::FP T>
void appendCylinder(TriangleBuilder<T>& out, T radius, T half_height, int around)
{
    using Vec3 = vn::math::Vector3<T>;

    const T half = half_height;

    std::vector<std::uint32_t> lower(static_cast<std::size_t>(around));
    std::vector<std::uint32_t> upper(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const T angle     = static_cast<T>(vn::math::PI_TWO) * static_cast<T>(column) / static_cast<T>(around);
        const T cos_angle = std::cos(angle);
        const T sin_angle = std::sin(angle);
        const Vec3 normal(cos_angle, sin_angle, T(0));
        lower[static_cast<std::size_t>(column)] = out.vertex(Vec3(radius * cos_angle, radius * sin_angle, -half), normal);
        upper[static_cast<std::size_t>(column)] = out.vertex(Vec3(radius * cos_angle, radius * sin_angle, half), normal);
    }
    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.quad(lower[static_cast<std::size_t>(column)],
                 lower[static_cast<std::size_t>(next)],
                 upper[static_cast<std::size_t>(next)],
                 upper[static_cast<std::size_t>(column)]);
    }

    const auto top_center = out.vertex(Vec3(0, 0, half), Vec3(0, 0, 1));
    const auto low_center = out.vertex(Vec3(0, 0, -half), Vec3(0, 0, -1));

    // The caps are built with their own rings, because a fan needs its corners next to the centre and a cap normal
    // cannot be shared with the side band's radial one.
    std::vector<std::uint32_t> top_ring(static_cast<std::size_t>(around));
    std::vector<std::uint32_t> low_ring(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const T angle                              = static_cast<T>(vn::math::PI_TWO) * static_cast<T>(column) / static_cast<T>(around);
        const T cos_angle                          = std::cos(angle);
        const T sin_angle                          = std::sin(angle);
        top_ring[static_cast<std::size_t>(column)] = out.vertex(Vec3(radius * cos_angle, radius * sin_angle, half), Vec3(0, 0, 1));
        low_ring[static_cast<std::size_t>(column)] = out.vertex(Vec3(radius * cos_angle, radius * sin_angle, -half), Vec3(0, 0, -1));
    }
    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.triangle(top_center, top_ring[static_cast<std::size_t>(column)], top_ring[static_cast<std::size_t>(next)]);
        out.triangle(low_center, low_ring[static_cast<std::size_t>(next)], low_ring[static_cast<std::size_t>(column)]);
    }
}

/**
 * @brief Appends a cone: a side fan from a rim to an apex plus a base fan.
 */
template <vn::math::FP T>
void appendCone(TriangleBuilder<T>& out, T base_radius, T half_height, int around)
{
    using Vec3 = vn::math::Vector3<T>;

    const T half  = half_height;
    const T slope = base_radius / (half_height * T(2));

    std::vector<std::uint32_t> rim(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const T angle                     = static_cast<T>(vn::math::PI_TWO) * static_cast<T>(column) / static_cast<T>(around);
        const T cos_angle                 = std::cos(angle);
        const T sin_angle                 = std::sin(angle);
        const Vec3 normal                 = Vec3(cos_angle, sin_angle, slope).normalized();
        rim[static_cast<std::size_t>(column)] = out.vertex(Vec3(base_radius * cos_angle, base_radius * sin_angle, -half), normal);
    }

    for (int column = 0; column < around; ++column) {
        const int next   = (column + 1) % around;
        const T   middle = static_cast<T>(vn::math::PI_TWO) * (static_cast<T>(column) + T(0.5)) / static_cast<T>(around);
        const Vec3 normal = Vec3(std::cos(middle), std::sin(middle), slope).normalized();
        const auto apex   = out.vertex(Vec3(0, 0, half), normal);
        out.triangle(rim[static_cast<std::size_t>(column)], rim[static_cast<std::size_t>(next)], apex);
    }

    const auto                 base_center = out.vertex(Vec3(0, 0, -half), Vec3(0, 0, -1));
    std::vector<std::uint32_t> base_ring(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const T angle                               = static_cast<T>(vn::math::PI_TWO) * static_cast<T>(column) / static_cast<T>(around);
        const T cos_angle                           = std::cos(angle);
        const T sin_angle                           = std::sin(angle);
        base_ring[static_cast<std::size_t>(column)] = out.vertex(Vec3(base_radius * cos_angle, base_radius * sin_angle, -half), Vec3(0, 0, -1));
    }
    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.triangle(base_center, base_ring[static_cast<std::size_t>(next)], base_ring[static_cast<std::size_t>(column)]);
    }
}

/**
 * @brief Clamps the quality where a value would stop describing a surface.
 */
[[nodiscard]] TessellationQuality clamped(const TessellationQuality& quality)
{
    return TessellationQuality{ quality.around < 3 ? 3 : quality.around, quality.along < 2 ? 2 : quality.along };
}

} // namespace

template <vn::math::FP T>
Triangulation<T> tessellateSolid(const BoxSolid<T>& box, const TessellationQuality&)
{
    if (!box.isValid()) {
        return {};
    }

    TriangleBuilder<T> builder;
    appendBox(builder, box.half_x, box.half_y, box.half_z);
    return builder.finish();
}

template <vn::math::FP T>
Triangulation<T> tessellateSolid(const SphereSolid<T>& sphere, const TessellationQuality& quality)
{
    if (!sphere.isValid()) {
        return {};
    }

    const TessellationQuality steps = clamped(quality);

    TriangleBuilder<T> builder;
    appendEllipsoid(builder, sphere.radius, sphere.radius, sphere.radius, steps.around, steps.along);
    return builder.finish();
}

template <vn::math::FP T>
Triangulation<T> tessellateSolid(const CylinderSolid<T>& cylinder, const TessellationQuality& quality)
{
    if (!cylinder.isValid()) {
        return {};
    }

    const TessellationQuality steps = clamped(quality);

    TriangleBuilder<T> builder;
    appendCylinder(builder, cylinder.radius, cylinder.half_height, steps.around);
    return builder.finish();
}

template <vn::math::FP T>
Triangulation<T> tessellateSolid(const ConeSolid<T>& cone, const TessellationQuality& quality)
{
    if (!cone.isValid()) {
        return {};
    }

    const TessellationQuality steps = clamped(quality);

    TriangleBuilder<T> builder;
    appendCone(builder, cone.base_radius, cone.half_height, steps.around);
    return builder.finish();
}

template <vn::math::FP T>
Triangulation<T> tessellateSolid(const EllipsoidSolid<T>& ellipsoid, const TessellationQuality& quality)
{
    if (!ellipsoid.isValid()) {
        return {};
    }

    const TessellationQuality steps = clamped(quality);

    TriangleBuilder<T> builder;
    appendEllipsoid(builder, ellipsoid.radius_x, ellipsoid.radius_y, ellipsoid.radius_z, steps.around, steps.along);
    return builder.finish();
}

// Both precisions are instantiated here, the way every other geom value type is: the declarations carry no export
// macro and the instantiations do.
template VN_GEOM_API Triangulationf tessellateSolid(const BoxSolidf&, const TessellationQuality&);
template VN_GEOM_API Triangulationf tessellateSolid(const SphereSolidf&, const TessellationQuality&);
template VN_GEOM_API Triangulationf tessellateSolid(const CylinderSolidf&, const TessellationQuality&);
template VN_GEOM_API Triangulationf tessellateSolid(const ConeSolidf&, const TessellationQuality&);
template VN_GEOM_API Triangulationf tessellateSolid(const EllipsoidSolidf&, const TessellationQuality&);

template VN_GEOM_API Triangulationd tessellateSolid(const BoxSolidd&, const TessellationQuality&);
template VN_GEOM_API Triangulationd tessellateSolid(const SphereSolidd&, const TessellationQuality&);
template VN_GEOM_API Triangulationd tessellateSolid(const CylinderSolidd&, const TessellationQuality&);
template VN_GEOM_API Triangulationd tessellateSolid(const ConeSolidd&, const TessellationQuality&);
template VN_GEOM_API Triangulationd tessellateSolid(const EllipsoidSolidd&, const TessellationQuality&);

VN_GEOM_SPATIAL_NS_END
