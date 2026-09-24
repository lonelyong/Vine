#pragma once

#include "math_global.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Math.hpp"
#include "Types.hpp"
#include "Vector2.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief A class representing a point in 2D space
 * @tparam T Only accepts float double and integers
 */
template <typename T>
class Point2 {
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct a point at the origin.
     */
    constexpr Point2()
      : x(T())
      , y(T())
    {}

    /**
     * @brief Construct a point from coordinates.
     * @param xx X coordinate.
     * @param yy Y coordinate.
     */
    constexpr Point2(T xx, T yy)
      : x(xx)
      , y(yy)
    {}

  public:
    /**
     * @brief View this point as a 2D vector without copying.
     * @return Const reference to the same memory as a vector.
     */
    [[nodiscard]]
    constexpr const Vector2<T>& asVector() const
    {
        static_assert(sizeof(Point2<T>) == sizeof(Vector2<T>));
        static_assert(std::is_standard_layout_v<Point2<T>>);
        static_assert(std::is_standard_layout_v<Vector2<T>>);
        return reinterpret_cast<const Vector2<T>&>(*this);
    }

    /**
     * @brief Convert this point to a 2D vector.
     * @return Converted vector.
     */
    [[nodiscard]]
    Vector2<T> toVector() const
    {
        return Vector2<T>(x, y);
    }

    /**
     * @brief Compute Euclidean distance to another point.
     * @param pt Target point.
     * @return Distance between two points.
     */
    [[nodiscard]]
    constexpr TypeF<T> distanceTo(const Point2<T>& pt) const
    {
        return std::sqrt((x - pt.x) * (x - pt.x) + (y - pt.y) * (y - pt.y));
    }

    /**
     * @brief Check whether all coordinates are zero.
     * @return True when x and y are exactly zero.
     */
    [[nodiscard]]
    constexpr bool isZero() const
    {
        return x == T() && y == T();
    }

    /**
     * @brief Check whether all coordinates are near zero.
     * @param eps Tolerance used for comparison.
     * @return True when both coordinates are within tolerance of zero.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(Real<T>)
    {
        return math::isZero<T>(x, eps) && math::isZero<T>(y, eps);
    }

    /**
     * @brief Compare with another point using exact equality.
     * @param other Point to compare.
     * @return True when coordinates are equal.
     */
    [[nodiscard]]
    constexpr bool isEqual(const Point2<T>& other) const
    {
        return x == other.x && y == other.y;
    }

    /**
     * @brief Compare with another point using tolerance.
     * @param other Point to compare.
     * @param eps Tolerance used for coordinate comparison.
     * @return True when coordinates are equal within tolerance.
     */
    [[nodiscard]]
    constexpr bool isEqual(const Point2<T>& other, T eps) const requires(Real<T>)
    {
        return math::isEqual<T>(x, other.x, eps) && math::isEqual<T>(y, other.y, eps);
    }

  public:
    /**
     * @brief Equality operator.
     * @param right Right-hand point.
     * @return True when coordinates are equal.
     */
    [[nodiscard]]
    constexpr bool operator==(const Point2<T>& right) const
    {
        return x == right.x && y == right.y;
    }

    /**
     * @brief Inequality operator.
     * @param right Right-hand point.
     * @return True when points are not equal.
     */
    [[nodiscard]]
    constexpr bool operator!=(const Point2<T>& right) const
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
    constexpr Vector2<T> operator-(const Point2<T>& right) const
    {
        return Vector2<T>(arithmeticSub(x, right.x), arithmeticSub(y, right.y));
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
    constexpr Point2<T> operator+(const Vector2<T>& right) const
    {
        return Point2<T>(arithmeticAdd(x, right.x), arithmeticAdd(y, right.y));
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
    constexpr Point2<T>& operator+=(const Vector2<T>& right)
    {
        x = arithmeticAdd(x, right.x);
        y = arithmeticAdd(y, right.y);

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
    constexpr Point2<T>& operator-=(const Vector2<T>& right)
    {
        x = arithmeticSub(x, right.x);
        y = arithmeticSub(y, right.y);

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
    constexpr Point2<T> operator-() const
    {
        return Point2<T>(arithmeticNagate(x), arithmeticNagate(y));
    }

    /**
     * @brief Access coordinate by index.
     * @param index Coordinate index (0 for x, 1 for y).
     * @return Mutable coordinate reference.
     */
    [[nodiscard]]
    constexpr T& operator[](size_t index)
    {
        assert(index < 2);
        return data[index];
    }

    /**
     * @brief Access coordinate by index.
     * @param index Coordinate index (0 for x, 1 for y).
     * @return Const coordinate reference.
     */
    [[nodiscard]]
    constexpr const T& operator[](size_t index) const
    {
        assert(index < 2);
        return data[index];
    }

  public:
    union
    {
        struct {
            T x, y;
        };

        T data[2];
    };
};

using Point2b    = Point2<bool>;
using Point2i8   = Point2<int8_t>;
using Point2ui8  = Point2<uint8_t>;
using Point2i16  = Point2<int16_t>;
using Point2ui16 = Point2<uint16_t>;
using Point2i32  = Point2<int32_t>;
using Point2ui32 = Point2<uint32_t>;
using Point2i64  = Point2<int64_t>;
using Point2ui64 = Point2<uint64_t>;
using Point2i    = Point2i32;
using Point2ui   = Point2ui32;
using Point2f    = Point2<float>;
using Point2d    = Point2<double>;

VN_MATH_NS_END
