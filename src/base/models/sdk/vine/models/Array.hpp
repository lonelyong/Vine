#pragma once

#include <vine/models/models_global.hpp>

#include <cstdint>
#include <vector>

#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>

VN_MODELS_NS_BEGIN

/**
 * @brief Scalar and vertex-attribute arrays, in the mesh's own local space.
 *
 * @details
 * - Every alias names its element type: there is no `Vec2Array` that silently means float, so a reader of a
 *   declaration can tell the precision and the signedness from the name alone.
 * - The vector aliases mirror the math aliases (`Vec2f` -> `Vec2fArray`, `Vec3ui32` -> `Vec3ui32Array`), which keeps
 *   one spelling rule for the whole family.
 */
using Int8Array  = std::vector<std::int8_t>;
using Int16Array = std::vector<std::int16_t>;
using Int32Array = std::vector<std::int32_t>;
using Int64Array = std::vector<std::int64_t>;

using UInt8Array  = std::vector<std::uint8_t>;
using UInt16Array = std::vector<std::uint16_t>;
using UInt32Array = std::vector<std::uint32_t>;
using UInt64Array = std::vector<std::uint64_t>;

using FloatArray  = std::vector<float>;
using DoubleArray = std::vector<double>;

using Vec2fArray = std::vector<vn::math::Vec2f>;
using Vec3fArray = std::vector<vn::math::Vec3f>;
using Vec4fArray = std::vector<vn::math::Vec4f>;

using Vec2dArray = std::vector<vn::math::Vec2d>;
using Vec3dArray = std::vector<vn::math::Vec3d>;
using Vec4dArray = std::vector<vn::math::Vec4d>;

using Vec2i8Array = std::vector<vn::math::Vec2i8>;
using Vec3i8Array = std::vector<vn::math::Vec3i8>;
using Vec4i8Array = std::vector<vn::math::Vec4i8>;

using Vec2i16Array = std::vector<vn::math::Vec2i16>;
using Vec3i16Array = std::vector<vn::math::Vec3i16>;
using Vec4i16Array = std::vector<vn::math::Vec4i16>;

using Vec2i32Array = std::vector<vn::math::Vec2i32>;
using Vec3i32Array = std::vector<vn::math::Vec3i32>;
using Vec4i32Array = std::vector<vn::math::Vec4i32>;

using Vec2i64Array = std::vector<vn::math::Vec2i64>;
using Vec3i64Array = std::vector<vn::math::Vec3i64>;
using Vec4i64Array = std::vector<vn::math::Vec4i64>;

using Vec2ui8Array = std::vector<vn::math::Vec2ui8>;
using Vec3ui8Array = std::vector<vn::math::Vec3ui8>;
using Vec4ui8Array = std::vector<vn::math::Vec4ui8>;

using Vec2ui16Array = std::vector<vn::math::Vec2ui16>;
using Vec3ui16Array = std::vector<vn::math::Vec3ui16>;
using Vec4ui16Array = std::vector<vn::math::Vec4ui16>;

using Vec2ui32Array = std::vector<vn::math::Vec2ui32>;
using Vec3ui32Array = std::vector<vn::math::Vec3ui32>;
using Vec4ui32Array = std::vector<vn::math::Vec4ui32>;

using Vec2ui64Array = std::vector<vn::math::Vec2ui64>;
using Vec3ui64Array = std::vector<vn::math::Vec3ui64>;
using Vec4ui64Array = std::vector<vn::math::Vec4ui64>;

VN_MODELS_NS_END
