#pragma once

#include "geometry_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/math/Point2.hpp>
#include <vine/math/Vector3.hpp>

VN_GEOMETRY_NS_BEGIN

using Vec3fArray  = std::vector<vn::math::Vec3f>;
using Vec3dArray  = std::vector<vn::math::Vec3d>;
using Vec2fArray  = std::vector<vn::math::Vec2f>;
using Vec2dArray  = std::vector<vn::math::Vec2d>;
using UInt32Array = std::vector<std::uint32_t>;

VN_GEOMETRY_NS_END
