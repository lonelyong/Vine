#pragma once

#include "geometry_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

VN_GEOMETRY_NS_BEGIN

/**
 * @brief Vertex attribute arrays of a mesh, in the mesh's own local space.
 *
 * @details
 * - Geometry stores float: a mesh is local-coordinate data that is handed to a renderer or a collision backend, so the
 *   storage type is not a choice a caller makes per mesh.
 */
using Vec2Array   = std::vector<vn::math::Vec2f>;
using Vec3Array   = std::vector<vn::math::Vec3f>;
using Vec4Array   = std::vector<vn::math::Vec4f>;
using UInt32Array = std::vector<std::uint32_t>;

VN_GEOMETRY_NS_END
