#pragma once

#include "math_global.hpp"

#include <cassert>
#include <cstddef>

#include "Math.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief A class representing a vector in ND space
 * @tparam T Only accepts float double and integers(include boolean)
 * @tparam N Number of dimensions
 */
template <typename T, size_t N>
class VectorN {
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct a zero vector.
     */
    constexpr VectorN()
      : data{}
    {}

    /**
     * @brief Construct from N values.
     * @param args N components.
     */
    template <typename... Args>
    requires(sizeof...(Args) == N)
    constexpr VectorN(Args... args)
      : data{ static_cast<T>(args)... }
    {}

  public:
    /**
     * @brief Dot product.
     *        only for real types (floating point and integers) not boolean.
     *        for integer types, overflow is possible.
     * @param other Right-hand vector.
     * @return Dot product value.
     */

    [[nodiscard]]
    constexpr T dot(const VectorN<T, N>& other) const requires(Real<T>)
    {
        T sum = T(0);
        for (size_t i = 0; i < N; ++i) {
            sum += data[i] * other.data[i];
        }
        return sum;
    }

    /**
     * @brief Normalize the vector to unit length.
     *        only for floating point types.
     * @return Original vector length before normalization.
     */
    [[nodiscard]]
    constexpr T normalize() requires(FP<T>)
    {
        T len2 = T(0);
        for (size_t i = 0; i < N; ++i) {
            len2 += data[i] * data[i];
        }
        const T len = std::sqrt(len2);
        if (len == T(0)) {
            for (size_t i = 0; i < N; ++i) {
                data[i] = T(0);
            }
        }
        else {
            const T inv = T(1) / len;
            for (size_t i = 0; i < N; ++i) {
                data[i] *= inv;
            }
        }
        return len;
    }

    /**
     * @brief Check whether all components are zero.
     * @return True when x/y/z/w are exactly zero.
     */
    [[nodiscard]]
    constexpr bool isZero() const
    {
        for (size_t i = 0; i < N; ++i) {
            if (data[i] != T(0)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Check whether all components are near zero.
     * @param eps Tolerance used for comparison.
     * @return True when all components are within tolerance of zero.
     */
    [[nodiscard]]
    constexpr bool isZero(T eps) const requires(Real<T>)
    {
        for (size_t i = 0; i < N; ++i) {
            if (!math::isZero(data[i], eps)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Compare with another vector using exact equality.
     * @param other Vector to compare.
     * @return True when components are equal.
     */
    [[nodiscard]]
    constexpr bool isEqual(const VectorN<T, N>& other) const
    {
        for (size_t i = 0; i < N; ++i) {
            if (data[i] != other.data[i]) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Compare with another vector using tolerance.
     * @param other Vector to compare.
     * @param eps Tolerance used for comparison.
     * @return True when components are equal within tolerance.
     */
    [[nodiscard]]
    constexpr bool isEqual(const VectorN<T, N>& other, T eps) const requires(Real<T>)
    {
        for (size_t i = 0; i < N; ++i) {
            if (!math::isEqual(data[i], other.data[i], eps)) {
                return false;
            }
        }
        return true;
    }

  public:
    /**
     * @brief Equality operator.
     * @param right Right-hand vector.
     * @return True when vectors are equal.
     */
    [[nodiscard]]
    constexpr bool operator==(const VectorN<T, N>& right) const
    {
        for (size_t i = 0; i < N; ++i) {
            if (data[i] != right.data[i]) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Inequality operator.
     * @param right Right-hand vector.
     * @return True when vectors are not equal.
     */
    [[nodiscard]]
    constexpr bool operator!=(const VectorN<T, N>& right) const
    {
        return !(*this == right);
    }

    /**
     * @brief Vector addition.
     *
     * - For boolean type: `+` treated as logical OR.
     * - For other types: normal addition.
     *
     * @param right Right-hand vector.
     * @return Sum vector.
     */
    [[nodiscard]]
    constexpr VectorN<T, N> operator+(const VectorN<T, N>& right) const
    {
        VectorN<T, N> v;
        for (size_t i = 0; i < N; ++i) {
            v.data[i] = arithmeticAdd(data[i], right.data[i]);
        }
        return v;
    }

    /**
     * @brief Vector subtraction.
     *
     * - For boolean type: `-` treated as left AND (NOT right).
     * - For other types: normal subtraction.
     *
     * @param right Right-hand vector.
     * @return Difference vector.
     */
    [[nodiscard]]
    constexpr VectorN<T, N> operator-(const VectorN<T, N>& right) const
    {
        VectorN<T, N> v;
        for (size_t i = 0; i < N; ++i) {
            v.data[i] = arithmeticSub(data[i], right.data[i]);
        }
        return v;
    }

    /**
     * @brief Scale this vector.
     *
     * - For boolean type: `*` treated as logical AND.
     * - For other types: normal multiplication.
     *
     * @param scale Scalar multiplier.
     * @return Scaled vector.
     */
    [[nodiscard]]
    constexpr VectorN<T, N> operator*(T scale) const
    {
        VectorN<T, N> v;
        for (size_t i = 0; i < N; ++i) {
            v.data[i] = arithmeticMultiply(data[i], scale);
        }
        return v;
    }

    /**
     * @brief Divide this vector by a scalar.
     *
     * - For boolean type: `/` treated as logical AND.
     * - For other types: normal division.
     *
     * @param scale Scalar divisor.
     * @return Scaled vector.
     */
    [[nodiscard]]
    constexpr VectorN<T, N> operator/(T scale) const
    {
        VectorN<T, N> v;
        for (size_t i = 0; i < N; ++i) {
            v.data[i] = arithmeticDivision(data[i], scale);
        }
        return v;
    }

    /**
     * @brief Add another vector in-place.
     *
     * - For boolean type: `+` treated as logical OR.
     * - For other types: normal addition.
     *
     * @param right Right-hand vector.
     * @return Reference to this vector.
     */
    constexpr VectorN<T, N>& operator+=(const VectorN<T, N>& right)
    {
        for (size_t i = 0; i < N; ++i) {
            data[i] = arithmeticAdd(data[i], right.data[i]);
        }
        return *this;
    }

    /**
     * @brief Subtract another vector in-place.
     *
     * - For boolean type: `-` treated as left AND (NOT right).
     * - For other types: normal subtraction.
     *
     * @param right Right-hand vector.
     * @return Reference to this vector.
     */
    constexpr VectorN<T, N>& operator-=(const VectorN<T, N>& right)
    {
        for (size_t i = 0; i < N; ++i) {
            data[i] = arithmeticSub(data[i], right.data[i]);
        }
        return *this;
    }

    /**
     * @brief Multiply by scalar in-place.
     *
     * - For boolean type: `*` treated as logical AND.
     * - For other types: normal multiplication.
     *
     * @param scale Scalar multiplier.
     * @return Reference to this vector.
     */
    constexpr VectorN<T, N>& operator*=(T scale)
    {
        for (size_t i = 0; i < N; ++i) {
            data[i] = arithmeticMultiply(data[i], scale);
        }
        return *this;
    }

    /**
     * @brief Divide by scalar in-place.
     *
     * - For boolean type: `/` treated as logical AND.
     * - For other types: normal division.
     *
     * @param scale Scalar divisor.
     * @return Reference to this vector.
     */
    constexpr VectorN<T, N>& operator/=(T scale)
    {
        for (size_t i = 0; i < N; ++i) {
            data[i] = arithmeticDivision(data[i], scale);
        }
        return *this;
    }

    /**
     * @brief Unary negation.
     *
     * - For boolean type: `-` treated as logical NOT.
     * - For other types: normal negation.
     *
     * @return Vector with negated components.
     */
    [[nodiscard]]
    constexpr VectorN<T, N> operator-() const
    {
        VectorN<T, N> v;
        for (size_t i = 0; i < N; ++i) {
            v.data[i] = arithmeticNagate(data[i]);
        }
        return v;
    }

    /**
     * @brief Dot product.
     * @param other Right-hand vector.
     * @return Dot product value.
     */
    [[nodiscard]]
    constexpr T operator*(const VectorN<T, N>& other) const requires(Real<T>)
    {
        return dot(other);
    }

    /**
     * @brief Access component by index.
     * @param index Component index in [0, N-1].
     * @return Mutable component reference.
     */
    [[nodiscard]]
    constexpr T& operator[](size_t index)
    {
        assert(index < N);
        return data[index];
    }

    /**
     * @brief Access component by index.
     * @param index Component index in [0, N-1].
     * @return Const component reference.
     */
    [[nodiscard]]
    constexpr const T& operator[](size_t index) const
    {
        assert(index < N);
        return data[index];
    }

  public:
    T data[N];
};

VN_MATH_NS_END
