#pragma once

#include "math_global.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include "Math.hpp"
#include "Point2.hpp"
#include "Vector3.hpp"

VN_MATH_NS_BEGIN

template <typename T>
class Point2;

/**
 * @brief A class representing a point in 3D space
 * @tparam T Only accepts float double and integers
 */
template <typename T>
class Point3 {
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct a point at the origin.
     */
    constexpr Point3()
    {
        x = T();
        y = T();
        z = T();
    }

    /**
     * @brief Construct a 3D point from a 2D point and z value.
     * @param pt2 Source 2D point.
     * @param zz Z coordinate.
     */
    constexpr Point3(const Point2<T>& pt2, T zz)
      : x(pt2.x)
      , y(pt2.y)
      , z(zz)
    {}

    /**
     * @brief Construct a point from coordinates.
     * @param xx X coordinate.
     * @param yy Y coordinate.
     * @param zz Z coordinate.
     */
    constexpr Point3(T xx, T yy, T zz)
      : x(xx)
      , y(yy)
      , z(zz)
    {}

  public:
    /**
     * @brief View this point as a 2D point without copying.
     * @return Const reference to the x/y part.
     */
    [[nodiscard]]
    constexpr const Point2<T>& asPoint2() const
    {
        return reinterpret_cast<const Point2<T>&>(*this);
    }

    /**
     * @brief View this point as a 3D vector without copying.
     * @return Const reference to the same memory as a vector.
     */
    [[nodiscard]]
    constexpr const Vector3<T>& asVector() const
    {
        return reinterpret_cast<const Vector3<T>&>(*this);
    }

    /**
     * @brief Convert this point to a 3D vector.
     * @return Converted vector.
     */
    [[nodiscard]]
    Vector3<T> toVector() const
    {
        return Vector3<T>(x, y, z);
    }

    /**
     * @brief Check whether all coordinates are zero.
     * @return True when x, y and z are exactly zero.
     */
    [[nodiscard]]
    constexpr bool isZero() const
    {
        return x == T() && y == T() && z == T();
    }

    /**
     * @brief Check whether all coordinates are near zero.
     * @param eps Tolerance used for comparison.
     * @return True when all coordinates are within tolerance of zero.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(Real<T>)
    {
        return math::isZero<T>(x, eps) && math::isZero<T>(y, eps) && math::isZero<T>(z, eps);
    }

    /**
     * @brief Compare with another point using exact equality.
     * @param other Point to compare.
     * @return True when coordinates are equal.
     */
    [[nodiscard]]
    constexpr bool isEqual(const Point3<T>& other) const
    {
        return *this == other;
    }

    /**
     * @brief Compare with another point using tolerance.
     * @param other Point to compare.
     * @param eps Tolerance used for coordinate comparison.
     * @return True when coordinates are equal within tolerance.
     */
    [[nodiscard]]
    constexpr bool isEqual(const Point3<T>& other, T eps) const requires(Real<T>)
    {
        return math::isEqual<T>(x, other.x, eps) && math::isEqual<T>(y, other.y, eps) && math::isEqual<T>(z, other.z, eps);
    }

  public:
    /**
     * @brief Equality operator.
     * @param right Right-hand point.
     * @return True when coordinates are equal.
     */
    [[nodiscard]]
    constexpr bool operator==(const Point3<T>& right) const
    {
        return x == right.x && y == right.y && z == right.z;
    }

    /**
     * @brief Inequality operator.
     * @param right Right-hand point.
     * @return True when points are not equal.
     */
    [[nodiscard]]
    constexpr bool operator!=(const Point3<T>& right) const
    {
        return !(*this == right);
    }

    /**
     * @brief Subtract two points to get a displacement vector.
     *
     * - For boolean type: `-` treated as left AND (NOT right).
     * - For other types: normal subtraction.
     *
     * @param right Right-hand point.
     * @return Vector from right to this point.
     */
    [[nodiscard]]
    constexpr Vector3<T> operator-(const Point3<T>& right) const
    {
        return Vector3<T>(arithmeticSub(x, right.x), arithmeticSub(y, right.y), arithmeticSub(z, right.z));
    }

    /**
     * @brief Translate this point by a vector.
     *
     * - For boolean type: `+` treated as logical OR.
     * - For other types: normal addition.
     *
     * @param right Translation vector.
     * @return Translated point.
     */
    [[nodiscard]]
    constexpr Point3<T> operator+(const Vector3<T>& right) const
    {
        return Point3<T>(arithmeticAdd(x, right.x), arithmeticAdd(y, right.y), arithmeticAdd(z, right.z));
    }

    /**
     * @brief Translate this point in-place.
     *
     * - For boolean type: `+` treated as logical OR.
     * - For other types: normal addition.
     *
     * @param right Translation vector.
     * @return Reference to this point.
     */
    constexpr Point3<T>& operator+=(const Vector3<T>& right)
    {
        x = arithmeticAdd(x, right.x);
        y = arithmeticAdd(y, right.y);
        z = arithmeticAdd(z, right.z);

        return *this;
    }

    /**
     * @brief Translate this point in-place by the inverse vector.
     *
     * - For boolean type: `-` treated as left AND (NOT right).
     * - For other types: normal subtraction.
     *
     * @param right Translation vector.
     * @return Reference to this point.
     */
    constexpr Point3<T>& operator-=(const Vector3<T>& right)
    {
        x = arithmeticSub(x, right.x);
        y = arithmeticSub(y, right.y);
        z = arithmeticSub(z, right.z);

        return *this;
    }

    /**
     * @brief Unary negation of point coordinates.
     *
     * - For boolean type: `-` treated as logical NOT.
     * - For other types: normal negation.
     *
     * @return Point with negated coordinates.
     */
    [[nodiscard]]
    constexpr Point3<T> operator-() const
    {
        return Point3<T>(arithmeticNagate(x), arithmeticNagate(y), arithmeticNagate(z));
    }

    /**
     * @brief Access coordinate by index.
     * @param index Coordinate index (0 for x, 1 for y, 2 for z).
     * @return Mutable coordinate reference.
     */
    [[nodiscard]]
    constexpr T& operator[](size_t index)
    {
        assert(index < 3);
        return data[index];
    }

    /**
     * @brief Access coordinate by index.
     * @param index Coordinate index (0 for x, 1 for y, 2 for z).
     * @return Const coordinate reference.
     */
    [[nodiscard]]
    constexpr const T& operator[](size_t index) const
    {
        assert(index < 3);
        return data[index];
    }

  public:
    union
    {
        struct {
            T x, y, z;
        };

        T data[3];
    };
};

using Point3b    = Point3<bool>;
using Point3i8   = Point3<int8_t>;
using Point3ui8  = Point3<uint8_t>;
using Point3i16  = Point3<int16_t>;
using Point3ui16 = Point3<uint16_t>;
using Point3i32  = Point3<int32_t>;
using Point3ui32 = Point3<uint32_t>;
using Point3i64  = Point3<int64_t>;
using Point3ui64 = Point3<uint64_t>;
using Point3i    = Point3i32;
using Point3ui   = Point3ui32;
using Point3f    = Point3<float>;
using Point3d    = Point3<double>;
VN_MATH_NS_END
