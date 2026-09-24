#pragma once

#include "geometry_global.hpp"

#include <cstddef>

#include "Mesh.hpp"

VN_GEOMETRY_NS_BEGIN

/**
 * @brief A non-indexed triangle mesh (triangle soup).
 *
 * Positions are stored as three consecutive vertices per triangle. Normals
 * and texture coordinates, when present, match the position array 1:1.
 */
class VN_GEOMETRY_API TriangleMesh : public Mesh {
    VN_OBJECT_META_DECL;

  public:
    TriangleMesh();

    /**
     * @brief Appends one triangle.
     *
     * @param a First vertex.
     * @param b Second vertex.
     * @param c Third vertex.
     */
    void addTriangle(const vn::math::Vec3f& a, const vn::math::Vec3f& b, const vn::math::Vec3f& c);

    /**
     * @brief Removes all vertices and attributes.
     */
    void clear();

    /**
     * @brief Returns the number of triangles.
     *
     * @return Triangle count.
     */
    [[nodiscard]]
    std::size_t triangleCount() const;

    [[nodiscard]]
    bool isValid() const override;
};

VN_GEOMETRY_NS_END
