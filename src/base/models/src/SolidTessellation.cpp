#include <vine/models/SolidTessellation.hpp>

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Vector3.hpp>

VN_MODELS_NS_BEGIN

namespace
{

using vn::math::Vec3f;

/**
 * @brief Accumulates positions, normals and triangle indices for one mesh.
 */
struct MeshBuilder {
    std::vector<Vec3f>         positions;
    std::vector<Vec3f>         normals;
    std::vector<std::uint32_t> indices;

    /**
     * @brief Appends one vertex and returns its index.
     */
    [[nodiscard]] std::uint32_t vertex(const Vec3f& position, const Vec3f& normal)
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
     * @brief Moves the accumulated data into a new triangle mesh.
     */
    [[nodiscard]] vn::intrusive_ptr<TriangleMesh> finish()
    {
        auto mesh = vn::make_intrusive<TriangleMesh>();
        mesh->setPositions(std::move(positions));
        mesh->setNormals(std::move(normals));
        mesh->setIndices(std::move(indices));
        (void)mesh->computeAabb();
        return mesh;
    }
};

/**
 * @brief Appends one planar quad with one normal for all four corners.
 */
void appendQuad(MeshBuilder& out, const Vec3f& normal, const Vec3f& p0, const Vec3f& p1, const Vec3f& p2, const Vec3f& p3)
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
void appendBox(MeshBuilder& out, float half_x, float half_y, float half_z)
{
    const float x = half_x;
    const float y = half_y;
    const float z = half_z;

    appendQuad(out, Vec3f(1.0f, 0.0f, 0.0f), Vec3f(x, -y, -z), Vec3f(x, y, -z), Vec3f(x, y, z), Vec3f(x, -y, z));
    appendQuad(out, Vec3f(-1.0f, 0.0f, 0.0f), Vec3f(-x, y, -z), Vec3f(-x, -y, -z), Vec3f(-x, -y, z), Vec3f(-x, y, z));
    appendQuad(out, Vec3f(0.0f, 1.0f, 0.0f), Vec3f(x, y, -z), Vec3f(-x, y, -z), Vec3f(-x, y, z), Vec3f(x, y, z));
    appendQuad(out, Vec3f(0.0f, -1.0f, 0.0f), Vec3f(-x, -y, -z), Vec3f(x, -y, -z), Vec3f(x, -y, z), Vec3f(-x, -y, z));
    appendQuad(out, Vec3f(0.0f, 0.0f, 1.0f), Vec3f(-x, -y, z), Vec3f(x, -y, z), Vec3f(x, y, z), Vec3f(-x, y, z));
    appendQuad(out, Vec3f(0.0f, 0.0f, -1.0f), Vec3f(x, -y, -z), Vec3f(-x, -y, -z), Vec3f(-x, y, -z), Vec3f(x, y, -z));
}

/**
 * @brief Appends a sphere / ellipsoid: latitude bands closed by two pole fans.
 */
void appendEllipsoid(MeshBuilder& out, float radius_x, float radius_y, float radius_z, int around, int along)
{
    const auto north = out.vertex(Vec3f(0.0f, 0.0f, radius_z), Vec3f(0.0f, 0.0f, 1.0f));

    const auto                 ring_count = static_cast<std::size_t>(along - 1);
    std::vector<std::uint32_t> rings(ring_count * static_cast<std::size_t>(around));
    for (int row = 1; row < along; ++row) {
        const float polar     = static_cast<float>(vn::math::PI) * static_cast<float>(row) / static_cast<float>(along);
        const float sin_polar = std::sin(polar);
        const float cos_polar = std::cos(polar);
        for (int column = 0; column < around; ++column) {
            const float azimuth     = static_cast<float>(vn::math::PI_TWO) * static_cast<float>(column) / static_cast<float>(around);
            const float cos_azimuth = std::cos(azimuth);
            const float sin_azimuth = std::sin(azimuth);

            const Vec3f position(radius_x * sin_polar * cos_azimuth, radius_y * sin_polar * sin_azimuth, radius_z * cos_polar);
            const Vec3f normal = Vec3f(position.x / (radius_x * radius_x), position.y / (radius_y * radius_y), position.z / (radius_z * radius_z)).normalized();
            rings[static_cast<std::size_t>(row - 1) * static_cast<std::size_t>(around) + static_cast<std::size_t>(column)] = out.vertex(position, normal);
        }
    }

    const auto south = out.vertex(Vec3f(0.0f, 0.0f, -radius_z), Vec3f(0.0f, 0.0f, -1.0f));

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
void appendCylinder(MeshBuilder& out, float radius, float half_height, int around)
{
    const float half = half_height;

    std::vector<std::uint32_t> lower(static_cast<std::size_t>(around));
    std::vector<std::uint32_t> upper(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const float angle     = static_cast<float>(vn::math::PI_TWO) * static_cast<float>(column) / static_cast<float>(around);
        const float cos_angle = std::cos(angle);
        const float sin_angle = std::sin(angle);
        const Vec3f normal(cos_angle, sin_angle, 0.0f);
        lower[static_cast<std::size_t>(column)] = out.vertex(Vec3f(radius * cos_angle, radius * sin_angle, -half), normal);
        upper[static_cast<std::size_t>(column)] = out.vertex(Vec3f(radius * cos_angle, radius * sin_angle, half), normal);
    }
    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.quad(lower[static_cast<std::size_t>(column)],
                 lower[static_cast<std::size_t>(next)],
                 upper[static_cast<std::size_t>(next)],
                 upper[static_cast<std::size_t>(column)]);
    }

    const auto top_center = out.vertex(Vec3f(0.0f, 0.0f, half), Vec3f(0.0f, 0.0f, 1.0f));
    const auto low_center = out.vertex(Vec3f(0.0f, 0.0f, -half), Vec3f(0.0f, 0.0f, -1.0f));

    // The caps are built with their own rings, because a fan needs its corners next to the centre and a cap normal
    // cannot be shared with the side band's radial one.
    std::vector<std::uint32_t> top_ring(static_cast<std::size_t>(around));
    std::vector<std::uint32_t> low_ring(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const float angle                          = static_cast<float>(vn::math::PI_TWO) * static_cast<float>(column) / static_cast<float>(around);
        const float cos_angle                      = std::cos(angle);
        const float sin_angle                      = std::sin(angle);
        top_ring[static_cast<std::size_t>(column)] = out.vertex(Vec3f(radius * cos_angle, radius * sin_angle, half), Vec3f(0.0f, 0.0f, 1.0f));
        low_ring[static_cast<std::size_t>(column)] = out.vertex(Vec3f(radius * cos_angle, radius * sin_angle, -half), Vec3f(0.0f, 0.0f, -1.0f));
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
void appendCone(MeshBuilder& out, float base_radius, float half_height, int around)
{
    const float half  = half_height;
    const float slope = base_radius / (half_height * 2.0f);

    std::vector<std::uint32_t> rim(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const float angle                     = static_cast<float>(vn::math::PI_TWO) * static_cast<float>(column) / static_cast<float>(around);
        const float cos_angle                 = std::cos(angle);
        const float sin_angle                 = std::sin(angle);
        const Vec3f normal                    = Vec3f(cos_angle, sin_angle, slope).normalized();
        rim[static_cast<std::size_t>(column)] = out.vertex(Vec3f(base_radius * cos_angle, base_radius * sin_angle, -half), normal);
    }

    for (int column = 0; column < around; ++column) {
        const int   next   = (column + 1) % around;
        const float middle = static_cast<float>(vn::math::PI_TWO) * (static_cast<float>(column) + 0.5f) / static_cast<float>(around);
        const Vec3f normal = Vec3f(std::cos(middle), std::sin(middle), slope).normalized();
        const auto  apex   = out.vertex(Vec3f(0.0f, 0.0f, half), normal);
        out.triangle(rim[static_cast<std::size_t>(column)], rim[static_cast<std::size_t>(next)], apex);
    }

    const auto                 base_center = out.vertex(Vec3f(0.0f, 0.0f, -half), Vec3f(0.0f, 0.0f, -1.0f));
    std::vector<std::uint32_t> base_ring(static_cast<std::size_t>(around));
    for (int column = 0; column < around; ++column) {
        const float angle                           = static_cast<float>(vn::math::PI_TWO) * static_cast<float>(column) / static_cast<float>(around);
        const float cos_angle                       = std::cos(angle);
        const float sin_angle                       = std::sin(angle);
        base_ring[static_cast<std::size_t>(column)] = out.vertex(Vec3f(base_radius * cos_angle, base_radius * sin_angle, -half), Vec3f(0.0f, 0.0f, -1.0f));
    }
    for (int column = 0; column < around; ++column) {
        const int next = (column + 1) % around;
        out.triangle(base_center, base_ring[static_cast<std::size_t>(next)], base_ring[static_cast<std::size_t>(column)]);
    }
}

/**
 * @brief Appends the triangles of whichever kind @p solid holds.
 *
 * @details
 * - The dispatch is a `std::visit` over the closed set, so a new kind is a compile error here instead of a silent
 *   fall-through to an empty mesh.
 */
struct TessellateVisitor {
    MeshBuilder& builder;
    int          around;
    int          along;

    void operator()(const vn::geometry::BoxSolidf& box) const
    {
        appendBox(builder, box.half_x, box.half_y, box.half_z);
    }

    void operator()(const vn::geometry::SphereSolidf& sphere) const
    {
        appendEllipsoid(builder, sphere.radius, sphere.radius, sphere.radius, around, along);
    }

    void operator()(const vn::geometry::CylinderSolidf& cylinder) const
    {
        appendCylinder(builder, cylinder.radius, cylinder.half_height, around);
    }

    void operator()(const vn::geometry::ConeSolidf& cone) const
    {
        appendCone(builder, cone.base_radius, cone.half_height, around);
    }

    void operator()(const vn::geometry::EllipsoidSolidf& ellipsoid) const
    {
        appendEllipsoid(builder, ellipsoid.radius_x, ellipsoid.radius_y, ellipsoid.radius_z, around, along);
    }
};

} // namespace

vn::intrusive_ptr<TriangleMesh> tessellateSolid(const vn::geometry::SolidShapef& solid, const TessellationQuality& quality)
{
    if (!solidOf(solid).isValid()) {
        return {};
    }

    const int around = quality.around < 3 ? 3 : quality.around;
    const int along  = quality.along < 2 ? 2 : quality.along;

    MeshBuilder builder;
    std::visit(TessellateVisitor{ builder, around, along }, solid);
    return builder.finish();
}

VN_MODELS_NS_END
