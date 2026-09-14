#pragma once

#include "math_global.hpp"

#include <cstring>

#include "Math.hpp"
#include "Types.hpp"
#include "Vector2.hpp"
#include "Vector3.hpp"

V_MATH_NS_BEGIN

/**
 * @brief 3x3 matrix.
 *
 * Layout (visualized by rows):
 * | m00 m01 m02 |
 * | m10 m11 m12 |
 * | m20 m21 m22 |
 *
 * Matrices operate on column vectors: y = M * x.
 *
 * Geometric transforms are provided as free functions in Transform2.hpp / Transform3.hpp.
 *
 * @tparam T     floating-point type (typically `float` or `double`).
 * @tparam Order storage order: `ColMajor` (default) or `RowMajor`.
 */
template <typename T, typename Order = ColMajor>
class Matrix3x3 {
  public:
    using value_type    = T;
    using storage_order = Order;

    /**
     * @brief Access element at (row, col), adapting to storage order at compile time.
     */
    constexpr T& element(size_t row, size_t col)
    {
        if constexpr (std::is_same_v<Order, ColMajor>) {
            return vecs[col][row];
        }
        else {
            return vecs[row][col];
        }
    }

    constexpr const T& element(size_t row, size_t col) const
    {
        if constexpr (std::is_same_v<Order, ColMajor>) {
            return vecs[col][row];
        }
        else {
            return vecs[row][col];
        }
    }

  public:
    /**
     * @brief Reset this matrix to identity.
     */
    constexpr void makeIdentity() noexcept
    {
        for (int i = 0; i < 9; ++i) data[i] = T(0);
        data[0] = data[4] = data[8] = T(1);
    }

    /**
     * @brief Construct an identity matrix.
     */
    constexpr Matrix3x3() noexcept
      : vecs{}
    {
        makeIdentity();
    }

    /**
     * @brief Construct a matrix from 9 scalar elements.
     *
     * Parameters follow the logical matrix layout:
     * | m00 m01 m02 |
     * | m10 m11 m12 |
     * | m20 m21 m22 |
     *
     * @param _m00 Row 0, col 0. @param _m01 Row 0, col 1. @param _m02 Row 0, col 2.
     * @param _m10 Row 1, col 0. @param _m11 Row 1, col 1. @param _m12 Row 1, col 2.
     * @param _m20 Row 2, col 0. @param _m21 Row 2, col 1. @param _m22 Row 2, col 2.
     */
    constexpr Matrix3x3(T _m00, T _m01, T _m02, T _m10, T _m11, T _m12, T _m20, T _m21, T _m22) noexcept
      : vecs{}
    {
        element(0, 0) = _m00;
        element(0, 1) = _m01;
        element(0, 2) = _m02;
        element(1, 0) = _m10;
        element(1, 1) = _m11;
        element(1, 2) = _m12;
        element(2, 0) = _m20;
        element(2, 1) = _m21;
        element(2, 2) = _m22;
    }

    /**
     * @brief Construct a matrix from a 9-element array.
     *
     * Elements are interpreted according to @p Order:
     * - ColMajor: col0[0..2], col1[0..2], col2[0..2].
     * - RowMajor: row0[0..2], row1[0..2], row2[0..2].
     *
     * @param elements Pointer to 9 T values in the storage order.
     */
    explicit Matrix3x3(const T* elements) noexcept
      : vecs{}
    {
        for (int i = 0; i < 9; ++i) {
            if constexpr (std::is_same_v<Order, ColMajor>) {
                element(i % 3, i / 3) = elements[i];
            }
            else {
                element(i / 3, i % 3) = elements[i];
            }
        }
    }

    /**
     * @brief Construct a matrix from a 9-element array (compile-time size check).
     */
    explicit Matrix3x3(const T (&elements)[9]) noexcept
      : Matrix3x3(static_cast<const T*>(elements))
    {}

    /**
     * @brief Left-multiply this matrix: M := left * M.
     */
    constexpr Matrix3x3& preMulti(const Matrix3x3& left)
    {
        auto old = *this;
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                T v = T(0);
                for (int k = 0; k < 3; ++k) v += left.element(i, k) * old.element(k, j);
                element(i, j) = v;
            }
        return *this;
    }

    /**
     * @brief Right-multiply this matrix: M := M * right.
     */
    constexpr Matrix3x3& postMulti(const Matrix3x3& right)
    {
        auto old = *this;
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                T v = T(0);
                for (int k = 0; k < 3; ++k) v += old.element(i, k) * right.element(k, j);
                element(i, j) = v;
            }
        return *this;
    }

    /**
     * @brief Calculate the determinant.
     */
    constexpr T determinant() const
    {
        const auto a = element(0, 0), b = element(0, 1), c = element(0, 2);
        const auto d = element(1, 0), e = element(1, 1), f = element(1, 2);
        const auto g = element(2, 0), h = element(2, 1), i = element(2, 2);
        return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    }

    /**
     * @brief Transpose the matrix.
     */
    constexpr void transpose()
    {
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j) std::swap(element(i, j), element(j, i));
    }

    /**
     * @brief Return a transposed copy.
     */
    constexpr Matrix3x3 transposed() const
    {
        Matrix3x3 m(*this);
        m.transpose();
        return m;
    }

    /**
     * @brief Invert the matrix in place.
     */
    bool invert();

    /**
     * @brief Return an inverted copy.
     */
    constexpr Matrix3x3 inverted() const
    {
        Matrix3x3 m(*this);
        m.invert();
        return m;
    }

    /**
     * @brief Is this matrix an identity matrix.
     */
    constexpr bool isIdentity(T eps = EPS<T>()) const
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (!math::isEqual(element(i, j), (i == j) ? T(1) : T(0), eps))
                    return false;
        return true;
    }

    /**
     * @brief Is this matrix affine (last row = [0 0 1]).
     */
    constexpr bool isAffine(T eps = EPS<T>()) const
    {
        return math::isZero(element(2, 0), eps) && math::isZero(element(2, 1), eps) && math::isEqual(element(2, 2), T(1), eps);
    }

    /**
     * @brief Is this matrix rigid (rotation + translation only).
     */
    constexpr bool isRigid(T eps = EPS<T>()) const
    {
        if (!isAffine(eps))
            return false;

        const Vector2<T> x(element(0, 0), element(1, 0));
        const Vector2<T> y(element(0, 1), element(1, 1));

        const T eps_len = T(2) * eps + eps * eps;

        if (!math::isEqual(x.length2(), T(1), eps_len))
            return false;

        if (!math::isEqual(y.length2(), T(1), eps_len))
            return false;

        if (!math::isZero(x.dot(y), eps))
            return false;

        const T det = element(0, 0) * element(1, 1) - element(0, 1) * element(1, 0);

        if (!math::isEqual(det, T(1), eps))
            return false;

        return true;
    }

    /**
     * @brief Element-wise approximate equality.
     */
    constexpr bool isEqual(const Matrix3x3& other, T eps = EPS<T>()) const
    {
        for (int i = 0; i < 9; ++i)
            if (!math::isEqual(data[i], other.data[i], eps))
                return false;
        return true;
    }

    /**
     * @brief Check if all elements are zero.
     */
    constexpr bool isZero(T eps = EPS<T>()) const
    {
        for (int i = 0; i < 9; ++i)
            if (!math::isZero(data[i], eps))
                return false;
        return true;
    }

  public:
    /**
     * @brief Read matrix element by row and column.
     */
    [[nodiscard]]
    T operator()(int row, int col) const
    {
        assert(row < 3);
        assert(col < 3);
        return element(row, col);
    }

    /**
     * @brief Access matrix element by row and column.
     */
    [[nodiscard]]
    T& operator()(int row, int col)
    {
        assert(row < 3);
        assert(col < 3);
        return element(row, col);
    }

    /**
     * @brief Matrix multiplication.
     */
    constexpr Matrix3x3 operator*(const Matrix3x3& right) const
    {
        Matrix3x3 m;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                T v = T(0);
                for (int k = 0; k < 3; ++k) v += element(i, k) * right.element(k, j);
                m.element(i, j) = v;
            }
        return m;
    }

    /**
     * @brief Matrix multiplication assignment.
     */
    constexpr Matrix3x3& operator*=(const Matrix3x3& right)
    {
        *this = *this * right;
        return *this;
    }

  public:
    union
    {
        struct {
            Vector3<T> vec0;
            Vector3<T> vec1;
            Vector3<T> vec2;
        };

        Vector3<T> vecs[3];

        T data[9];
    };
};

using Mat3f = Matrix3x3<float>;
using Mat3d = Matrix3x3<double>;

V_MATH_NS_END