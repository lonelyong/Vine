#pragma once
#include "math_global.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "Math.hpp"
#include "Vector2.hpp"

VN_MATH_NS_BEGIN

template <typename T>
class Vector2;

template <typename T>
class Point3;

/**
 * @brief A class representing a vector in 3D space
 * @tparam T Only accepts float double and integers(include boolean)
 */
template <typename T>
class Vector3 {
  public:
    using value_type = T;

  public:
    /**
     * @brief Constructs a zero vector.
     *
     *     Vector3() = (0, 0, 0)
     */
    constexpr Vector3()
      : x(T())
      , y(T())
      , z(T())
    {}

    /**
     * @brief Constructs a 3D vector from a 2D vector and optional z.
     *
     *     Vector3(vec2, zz) = (vec2.x, vec2.y, zz)
     *
     * @param vec2 Source 2D vector.
     * @param zz   Z component (defaults to 0).
     */
    constexpr Vector3(const Vector2<T>& vec2, T zz = 0.)
      : x(vec2.x)
      , y(vec2.y)
      , z(zz)
    {}

    /**
     * @brief Constructs a vector from individual components.
     *
     *     Vector3(xx, yy, zz) = (xx, yy, zz)
     *
     * @param xx X component.
     * @param yy Y component.
     * @param zz Z component.
     */
    constexpr Vector3(T xx, T yy, T zz)
      : x(xx)
      , y(yy)
      , z(zz) {};

  public:
    /**
     * @brief Sets the x and y components from a 2D vector.
     *
     * The z component is left unchanged:
     *
     *     (this.x, this.y) = (vec2.x, vec2.y)
     *
     * @param vec2 Source 2D vector.
     */
    constexpr void set(const Vector2<T>& vec2)
    {
        x = vec2.x;
        y = vec2.y;
    }

    /**
     * @brief Sets all components from a 2D vector and an explicit z value.
     *
     *     (this.x, this.y, this.z) = (vec2.x, vec2.y, zz)
     *
     * @param vec2 Source 2D vector for x and y.
     * @param zz   Z component.
     */
    constexpr void set(const Vector2<T>& vec2, T zz)
    {
        x = vec2.x;
        y = vec2.y;
        z = zz;
    }

    /**
     * @brief Sets all three components from individual values.
     *
     *     (this.x, this.y, this.z) = (xx, yy, zz)
     *
     * @param xx X component.
     * @param yy Y component.
     * @param zz Z component.
     */
    constexpr void set(T xx, T yy, T zz)
    {
        x = xx;
        y = yy;
        z = zz;
    }

    /**
     * @brief Retrieves all components into output parameters.
     *
     *     (xx, yy, zz) = (this.x, this.y, this.z)
     *
     * @param[out] xx Receives the X component.
     * @param[out] yy Receives the Y component.
     * @param[out] zz Receives the Z component.
     */
    constexpr void get(T& xx, T& yy, T& zz) const
    {
        xx = x;
        yy = y;
        zz = z;
    }

    /**
     * @brief Views this vector as a 3D point without copying.
     *
     * Relies on Vector3 and Point3 having identical memory layout,
     * which is guaranteed by their union-based design.
     *
     * @return Const reference to the same memory interpreted as a point.
     */
    [[nodiscard]]
    constexpr const Point3<T>& asPoint() const
    {
        return reinterpret_cast<const Point3<T>&>(*this);
    }

    /**
     * @brief Views this vector as a 2D vector without copying.
     *
     * Returns a reference to the x/y part only, relying on compatible
     * memory layout between Vector3 and Vector2.
     *
     * @return Const reference to x/y part interpreted as Vector2.
     */
    [[nodiscard]]
    constexpr const Vector2<T>& asVector2() const
    {
        return reinterpret_cast<const Vector2<T>&>(*this);
    }

    /**
     * @brief Computes the dot (scalar) product with another vector.
     *
     * The dot product is defined as:
     *
     *     a · b = a.x*b.x + a.y*b.y + a.z*b.z
     *
     * Geometrically, the dot product equals:
     *
     *     a · b = |a| * |b| * cos(theta)
     *
     * where theta is the angle between the two vectors. It measures how
     * much one vector extends in the direction of another:
     * - Positive  → vectors point in roughly the same direction (theta < 90°).
     * - Zero      → vectors are orthogonal (theta = 90°).
     * - Negative  → vectors point in opposite directions (theta > 90°).
     *
     * This operation is enabled only for arithmetic types excluding bool.
     * For integral types, intermediate arithmetic may overflow.
     *
     * @param other The other vector.
     * @return The dot product value.
     */
    [[nodiscard]]
    constexpr T dot(const Vector3<T>& other) const requires(Real<T>)
    {
        return static_cast<T>(x * other.x + y * other.y + z * other.z);
    }

    /**
     * @brief Computes the 3D cross product with another vector.
     *
     * The cross product is defined as:
     *
     *     a × b = (a.y*b.z - a.z*b.y,
     *              a.z*b.x - a.x*b.z,
     *              a.x*b.y - a.y*b.x)
     *
     * The result is perpendicular to both input vectors and follows the
     * right-hand rule.
     *
     * <b>Magnitude — parallelogram area:</b>
     *
     *     |a × b| = |a| · |b| · sin(θ)
     *
     * Geometrically, |a × b| equals the area of the parallelogram spanned
     * by a and b. For orthogonal vectors (θ = 90°), the area is maximal:
     *     |a × b| = |a| · |b|.
     *
     * <b>Lagrange's identity (avoiding the cross product for squared magnitude):</b>
     *
     *     |a × b|² = |a|² · |b|² − (a · b)²
     *
     * When only the squared magnitude is needed (e.g. for length
     * comparisons or parallelism tests), prefer computing it via dot
     * products and squared lengths using the identity above — this
     * avoids constructing the intermediate cross-product vector entirely:
     *
     *     cross_len2 = a.length2() * b.length2() − a.dot(b)²
     *
     * This operation is enabled only for arithmetic types excluding bool.
     * For integral types, intermediate arithmetic may overflow.
     *
     * @param other The other vector.
     * @return The cross product vector.
     */
    [[nodiscard]]
    constexpr Vector3<T> cross(const Vector3<T>& other) const requires(Real<T>)
    {
        return Vector3<T>(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x);
    }

    /**
     * @brief Computes the Euclidean length (magnitude) of the vector.
     *
     * The length is defined as:
     *
     *     |v| = sqrt(v.x² + v.y² + v.z²)
     *
     * This operation is enabled only for arithmetic types excluding bool.
     * For integral types, computation is internally promoted to the
     * corresponding floating-point type to avoid overflow and preserve
     * precision.
     *
     * @return The vector length.
     */
    [[nodiscard]]
    constexpr TypeF<T> length() const requires(Real<T>)
    {
        return safeLength(x, y, z);
    }

    /**
     * @brief Computes the squared Euclidean length of the vector.
     *
     * The squared length is defined as:
     *
     *     |v|² = v.x² + v.y² + v.z²
     *
     * Prefer this over length() when comparing magnitudes, as it avoids the
     * expensive square root computation. For example, to test whether a
     * vector is within a certain radius r, compare length2() with r²
     * directly.
     *
     * This operation is enabled only for arithmetic types excluding bool.
     * For integral types, computation is internally promoted to the
     * corresponding floating-point type to avoid overflow and preserve
     * precision.
     *
     * @return The squared vector length.
     */
    [[nodiscard]]
    constexpr TypeF<T> length2() const requires(Real<T>)
    {
        return safeLengthSquared(x, y, z);
    }

    /**
     * @brief Computes the angle between this vector and another.
     *
     * The angle is computed using the numerically stable atan2 formulation:
     *
     *     θ = atan2(|a × b|, a · b)
     *
     * This avoids the precision loss of acos(dot / (|a|·|b|)) when the
     * two vectors are nearly parallel or anti-parallel, where the argument
     * to acos can exceed [-1, 1] due to floating-point rounding.
     *
     * This operation is enabled only for arithmetic types excluding bool.
     *
     * @param other The other vector.
     * @return The angle in radians, in the range [0, π].
     */
    [[nodiscard]]
    constexpr TypeF<T> angleTo(const Vector3<T>& other) const requires(Real<T>)
    {
        /**
         * Compute the angle between two vectors using atan2:
         *
         *   angle = atan2(sin_component, dot_product)
         *
         * Derivation:
         *   cos(angle) = (a · b) / (|a||b|)
         *
         *   sin²(angle) = 1 - cos²(angle)
         *               = 1 - (a · b)² / (|a|²|b|²)
         *               = (|a|²|b|² - (a · b)²) / (|a|²|b|²)
         *
         *   Therefore:
         *     |a||b|sin(angle) = sqrt(|a|²|b|² - (a · b)²)
         *
         *   Since:
         *     |a||b|cos(angle) = a · b
         *
         *   The angle can be computed as:
         *
         *     angle = atan2(
         *         sqrt(|a|²|b|² - (a · b)²),
         *         a · b
         *     )
         *
         * This method is more numerically stable than:
         *
         *     acos((a · b) / (|a||b|))
         *
         * because acos loses precision when vectors are nearly parallel
         * or anti-parallel.
         */

        using ft = TypeF<T>;

        const ft x1 = static_cast<ft>(x);
        const ft y1 = static_cast<ft>(y);
        const ft z1 = static_cast<ft>(z);

        const ft x2 = static_cast<ft>(other.x);
        const ft y2 = static_cast<ft>(other.y);
        const ft z2 = static_cast<ft>(other.z);

        const ft dot     = x1 * x2 + y1 * y2 + z1 * z2;
        const ft len1_sq = x1 * x1 + y1 * y1 + z1 * z1;
        const ft len2_sq = x2 * x2 + y2 * y2 + z2 * z2;

        const ft sin_theta_sq = len1_sq * len2_sq - dot * dot;
        const ft sin_theta    = std::sqrt((std::max)(ft(0), sin_theta_sq));

        return std::atan2(sin_theta, dot);
    }

    /**
     * @brief Normalizes the vector to unit length in-place.
     *
     * Normalization divides each component by the vector's length:
     *
     *     v̂ = v / |v|
     *
     * After normalization, the vector satisfies |v̂| = 1 (within
     * floating-point precision), preserving its original direction.
     *
     * When the vector is exactly zero, it is set to (0, 0, 0) to avoid
     * division by zero, and the returned length is 0.
     *
     * This operation is enabled only for floating-point types.
     *
     * @return The original vector length before normalization.
     */
    constexpr T normalize() requires(FP<T>)
    {
        auto len = length();

        if (len == T(0)) {
            x = T(0);
            y = T(0);
            z = T(0);
        }
        else {
            x /= len;
            y /= len;
            z /= len;
        }

        return len;
    }

    /**
     * @brief Returns a normalized copy of the vector.
     *
     * This is equivalent to calling normalize() on a copy of the vector.
     * The original vector remains unchanged.
     *
     * This operation is enabled only for floating-point types.
     *
     * @return A new vector with unit length in the same direction.
     */
    [[nodiscard]]
    constexpr Vector3<T> normalized() const requires(FP<T>)
    {
        Vector3<T> result = *this;
        result.normalize();
        return result;
    }

    /**
     * @brief Checks whether all components are exactly zero.
     *
     *     isZero() ⇔ x == 0  ∧  y == 0  ∧  z == 0
     *
     * Uses exact equality — for floating-point types, tiny rounding
     * errors may cause a near-zero vector to return false. Use
     * isZero(T eps) for tolerance-based checks.
     *
     * @return True when all components are exactly zero.
     *
     * @see isZero(T eps)
     */
    [[nodiscard]]
    constexpr bool isZero() const
    {
        return x == T() && y == T() && z == T();
    }

    /**
     * @brief Checks whether all components are near zero within a tolerance.
     *
     * A component is considered zero when its absolute value does not
     * exceed the given epsilon. For unsigned integral types, the check
     * simplifies to `component <= eps`.
     *
     * For floating-point types, NaN and Inf values are never considered
     * zero.
     *
     * This operation is enabled only for arithmetic types excluding bool.
     *
     * @param eps Tolerance used for comparison.
     * @return True when all components are within tolerance of zero.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(Real<T>)
    {
        return math::isZero<T>(x, eps) && math::isZero<T>(y, eps) && math::isZero<T>(z, eps);
    }

    /**
     * @brief Compares with another vector using exact component-wise equality.
     *
     *     isEqual(b) ⇔ this.x == b.x  ∧  this.y == b.y  ∧  this.z == b.z
     *
     * For floating-point types, prefer isEqual(b, eps) with a tolerance
     * to account for rounding errors.
     *
     * @param other Vector to compare.
     * @return True when all corresponding components are exactly equal.
     *
     * @see isEqual(const Vector3<T>&, T)
     */
    [[nodiscard]]
    constexpr bool isEqual(const Vector3<T>& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }

    /**
     * @brief Compares with another vector using a per-component tolerance.
     *
     * Two components are considered equal when their absolute difference
     * does not exceed the given epsilon:
     *
     *     |a.x - b.x| ≤ eps  &&  |a.y - b.y| ≤ eps  &&  |a.z - b.z| ≤ eps
     *
     * For floating-point types, NaN and Inf values are never considered
     * equal.
     *
     * This operation is enabled only for arithmetic types excluding bool.
     *
     * @param other Vector to compare.
     * @param eps Tolerance used for comparison.
     * @return True when all components are equal within tolerance.
     */
    [[nodiscard]]
    constexpr bool isEqual(const Vector3<T>& other, T eps) const requires(Real<T>)
    {
        return math::isEqual<T>(x, other.x, eps) && math::isEqual<T>(y, other.y, eps) && math::isEqual<T>(z, other.z, eps);
    }

  public:
    /**
     * @brief Exact component-wise equality operator.
     *
     *     a == b ⇔ a.x == b.x  ∧  a.y == b.y  ∧  a.z == b.z
     *
     * @param right Right-hand vector.
     * @return True when all corresponding components are equal.
     */
    [[nodiscard]]
    constexpr bool operator==(const Vector3<T>& right) const
    {
        return x == right.x && y == right.y && z == right.z;
    }

    /**
     * @brief Exact component-wise inequality operator.
     *
     *     a != b ⇔ ¬(a == b)
     *
     * @param right Right-hand vector.
     * @return True when any corresponding components differ.
     */
    [[nodiscard]]
    constexpr bool operator!=(const Vector3<T>& right) const
    {
        return !(*this == right);
    }

    /**
     * @brief Component-wise vector addition.
     *
     *     a + b = (a.x + b.x, a.y + b.y, a.z + b.z)
     *
     * - For boolean type: `+` treated as logical OR.
     * - For other types: normal addition.
     *
     * @param right Right-hand vector.
     * @return Sum vector.
     */
    [[nodiscard]]
    constexpr Vector3<T> operator+(const Vector3<T>& right) const
    {
        return Vector3<T>(arithmeticAdd(x, right.x), arithmeticAdd(y, right.y), arithmeticAdd(z, right.z));
    }

    /**
     * @brief Component-wise vector subtraction.
     *
     *     a - b = (a.x - b.x, a.y - b.y, a.z - b.z)
     *
     * - For boolean type: `-` treated as left AND (NOT right).
     * - For other types: normal subtraction.
     *
     * @param right Right-hand vector.
     * @return Difference vector.
     */
    [[nodiscard]]
    constexpr Vector3<T> operator-(const Vector3<T>& right) const
    {
        return Vector3<T>(arithmeticSub(x, right.x), arithmeticSub(y, right.y), arithmeticSub(z, right.z));
    }

    /**
     * @brief Component-wise scalar multiplication.
     *
     *     v * s = (v.x*s, v.y*s, v.z*s)
     *
     * - For boolean type: `*` treated as logical AND.
     * - For other types: normal multiplication.
     *
     * @param scale Scalar multiplier.
     * @return Scaled vector.
     */
    [[nodiscard]]
    constexpr Vector3<T> operator*(T scale) const
    {
        return Vector3<T>(arithmeticMultiply(x, scale), arithmeticMultiply(y, scale), arithmeticMultiply(z, scale));
    }

    /**
     * @brief Component-wise scalar division.
     *
     *     v / s = (v.x/s, v.y/s, v.z/s)
     *
     * - For boolean type: `/` treated as logical AND.
     * - For other types: normal division.
     *
     * @param scale Scalar divisor.
     * @return Scaled vector.
     */
    [[nodiscard]]
    constexpr Vector3<T> operator/(T scale) const
    {
        return Vector3<T>(arithmeticDivision(x, scale), arithmeticDivision(y, scale), arithmeticDivision(z, scale));
    }

    /**
     * @brief Adds another vector in-place.
     *
     *     this += b  ⇔  (this.x, this.y, this.z) += (b.x, b.y, b.z)
     *
     * - For boolean type: `+` treated as logical OR.
     * - For other types: normal addition.
     *
     * @param right Right-hand vector.
     * @return Reference to this vector after addition.
     */
    constexpr Vector3<T>& operator+=(const Vector3<T>& right)
    {
        x = arithmeticAdd(x, right.x);
        y = arithmeticAdd(y, right.y);
        z = arithmeticAdd(z, right.z);

        return *this;
    }

    /**
     * @brief Subtracts another vector in-place.
     *
     *     this -= b  ⇔  (this.x, this.y, this.z) -= (b.x, b.y, b.z)
     *
     * - For boolean type: `-` treated as left AND (NOT right).
     * - For other types: normal subtraction.
     *
     * @param right Right-hand vector.
     * @return Reference to this vector after subtraction.
     */
    constexpr Vector3<T>& operator-=(const Vector3<T>& right)
    {
        x = arithmeticSub(x, right.x);
        y = arithmeticSub(y, right.y);
        z = arithmeticSub(z, right.z);

        return *this;
    }

    /**
     * @brief Multiplies by a scalar in-place.
     *
     *     this *= s  ⇔  (this.x, this.y, this.z) *= s
     *
     * - For boolean type: `*` treated as logical AND.
     * - For other types: normal multiplication.
     *
     * @param scale Scalar multiplier.
     * @return Reference to this vector after scaling.
     */
    constexpr Vector3<T>& operator*=(T scale)
    {
        x = arithmeticMultiply(x, scale);
        y = arithmeticMultiply(y, scale);
        z = arithmeticMultiply(z, scale);

        return *this;
    }

    /**
     * @brief Divides by a scalar in-place.
     *
     *     this /= s  ⇔  (this.x, this.y, this.z) /= s
     *
     * - For boolean type: `/` treated as logical AND.
     * - For other types: normal division.
     *
     * Division by zero follows the underlying arithmetic type's behavior.
     *
     * @param scale Scalar divisor.
     * @return Reference to this vector after division.
     */
    constexpr Vector3<T>& operator/=(T scale)
    {
        x = arithmeticDivision(x, scale);
        y = arithmeticDivision(y, scale);
        z = arithmeticDivision(z, scale);

        return *this;
    }

    /**
     * @brief Unary negation (additive inverse).
     *
     *     -v = (-v.x, -v.y, -v.z)
     *
     * - For boolean type: `-` treated as logical NOT.
     * - For other types: normal negation.
     *
     * @return A new vector with all components negated.
     */
    [[nodiscard]]
    constexpr Vector3<T> operator-() const
    {
        return Vector3<T>(arithmeticNagate(x), arithmeticNagate(y), arithmeticNagate(z));
    }

    /**
     * @brief Dot product operator (convenience alias for dot()).
     *
     *     a * b = a.x*b.x + a.y*b.y + a.z*b.z
     *
     * This operation is enabled only for arithmetic types excluding bool.
     *
     * @param other The other vector.
     * @return The dot product value.
     * @see dot()
     */
    [[nodiscard]]
    constexpr T operator*(const Vector3<T>& other) const requires(Real<T>)
    {
        return dot(other);
    }

    /**
     * @brief Cross product operator (convenience alias for cross()).
     *
     *     a ^ b = (a.y*b.z - a.z*b.y,
     *              a.z*b.x - a.x*b.z,
     *              a.x*b.y - a.y*b.x)
     *
     * This operation is enabled only for arithmetic types excluding bool.
     *
     * @param other The other vector.
     * @return The cross product vector.
     * @see cross()
     */
    [[nodiscard]]
    constexpr Vector3<T> operator^(const Vector3<T>& other) const requires(Real<T>)
    {
        return cross(other);
    }

    /**
     * @brief Accesses a component by index (mutable).
     *
     *     v[0] == v.x,   v[1] == v.y,   v[2] == v.z
     *
     * Index must be in the range [0, 2]. Out-of-bounds access is
     * undefined behavior.
     *
     * @param index Component index: 0 for x, 1 for y, 2 for z.
     * @return Mutable reference to the requested component.
     */
    [[nodiscard]]
    constexpr T& operator[](size_t index)
    {
        assert(index < 3);
        return data[index];
    }

    /**
     * @brief Accesses a component by index (read-only).
     *
     *     v[0] == v.x,   v[1] == v.y,   v[2] == v.z
     *
     * Index must be in the range [0, 2]. Out-of-bounds access is
     * undefined behavior.
     *
     * @param index Component index: 0 for x, 1 for y, 2 for z.
     * @return Const reference to the requested component.
     */
    [[nodiscard]]
    constexpr const T& operator[](size_t index) const
    {
        assert(index < 3);
        return data[index];
    }

  public:
    /**
     * @brief Unit vector along the X axis.
     *
     *     unitX = (1, 0, 0)
     *
     * @return Unit vector along X.
     */
    [[nodiscard]]
    static constexpr Vector3<T> unitX()
    {
        return Vector3<T>(T(1), T(0), T(0));
    }

    /**
     * @brief Unit vector along the Y axis.
     *
     *     unitY = (0, 1, 0)
     *
     * @return Unit vector along Y.
     */
    [[nodiscard]]
    static constexpr Vector3<T> unitY()
    {
        return Vector3<T>(T(0), T(1), T(0));
    }

    /**
     * @brief Unit vector along the Z axis.
     *
     *     unitZ = (0, 0, 1)
     *
     * @return Unit vector along Z.
     */
    [[nodiscard]]
    static constexpr Vector3<T> unitZ()
    {
        return Vector3<T>(T(0), T(0), T(1));
    }

    /**
     * @brief Zero vector.
     *
     *     zero = (0, 0, 0)
     *
     * @return Zero vector.
     */
    [[nodiscard]]
    static constexpr Vector3<T> zero()
    {
        return Vector3<T>(T(0), T(0), T(0));
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

using Vec3b    = Vector3<bool>;
using Vec3i8   = Vector3<int8_t>;
using Vec3ui8  = Vector3<uint8_t>;
using Vec3i16  = Vector3<int16_t>;
using Vec3ui16 = Vector3<uint16_t>;
using Vec3i32  = Vector3<int32_t>;
using Vec3ui32 = Vector3<uint32_t>;
using Vec3i64  = Vector3<int64_t>;
using Vec3ui64 = Vector3<uint64_t>;
using Vec3i    = Vec3i32;
using Vec3ui   = Vec3ui32;
using Vec3f    = Vector3<float>;
using Vec3d    = Vector3<double>;

VN_MATH_NS_END
