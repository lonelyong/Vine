#pragma once

#include "math_global.hpp"

#include <cstring>

#include "Types.hpp"
#include "Vector4.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief 4x4 matrix.
 *
 * Layout (visualized by rows):
 * | m00 m01 m02 m03 |
 * | m10 m11 m12 m13 |
 * | m20 m21 m22 m23 |
 * | m30 m31 m32 m33 |
 *
 * Matrices operate on column vectors: y = M * x.
 *
 * Geometric transforms (rotation, translation, scale, projection, etc.)
 * are provided as free functions in Transform3.hpp.
 *
 * @tparam T     floating-point type (typically `float` or `double`).
 * @tparam Order storage order: `ColMajor` (default) or `RowMajor`.
 *               Controls memory layout and raw-data constructor interpretation.
 */
template <typename T, typename Order = ColMajor>
class Matrix4x4 {
  public:
    using value_type    = T;
    using storage_order = Order;

    /**
     * @brief Access element at (row, col), adapting to storage order at compile time.
     *
     * ColMajor: vecs[col][row]
     * RowMajor: vecs[row][col]
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
     * @brief Construct an identity matrix.
     */
    constexpr Matrix4x4() noexcept
      : vecs{}
    {
        makeIdentity();
    }

    /**
     * @brief Construct a matrix from 16 scalar elements.
     *
     * Parameters follow the logical matrix layout:
     * | m00 m01 m02 m03 |
     * | m10 m11 m12 m13 |
     * | m20 m21 m22 m23 |
     * | m30 m31 m32 m33 |
     *
     * The elements are stored according to the storage order @p Order.
     *
     * @param _m00 Row 0, col 0. @param _m01 Row 0, col 1. @param _m02 Row 0, col 2. @param _m03 Row 0, col 3.
     * @param _m10 Row 1, col 0. @param _m11 Row 1, col 1. @param _m12 Row 1, col 2. @param _m13 Row 1, col 3.
     * @param _m20 Row 2, col 0. @param _m21 Row 2, col 1. @param _m22 Row 2, col 2. @param _m23 Row 2, col 3.
     * @param _m30 Row 3, col 0. @param _m31 Row 3, col 1. @param _m32 Row 3, col 2. @param _m33 Row 3, col 3.
     */
    constexpr Matrix4x4(T _m00, T _m01, T _m02, T _m03, T _m10, T _m11, T _m12, T _m13, T _m20, T _m21, T _m22, T _m23, T _m30, T _m31, T _m32, T _m33) noexcept
      : vecs{}
    {
        element(0, 0) = _m00;
        element(0, 1) = _m01;
        element(0, 2) = _m02;
        element(0, 3) = _m03;
        element(1, 0) = _m10;
        element(1, 1) = _m11;
        element(1, 2) = _m12;
        element(1, 3) = _m13;
        element(2, 0) = _m20;
        element(2, 1) = _m21;
        element(2, 2) = _m22;
        element(2, 3) = _m23;
        element(3, 0) = _m30;
        element(3, 1) = _m31;
        element(3, 2) = _m32;
        element(3, 3) = _m33;
    }

    /**
     * @brief Construct a matrix from a 16-element array.
     *
     * The elements are interpreted according to the storage order @p Order:
     * - ColMajor: elements[0..15] are col0[0..3], col1[0..3], col2[0..3], col3[0..3].
     * - RowMajor: elements[0..15] are row0[0..3], row1[0..3], row2[0..3], row3[0..3].
     *
     * @warning The pointer is not validated. Passing nullptr or fewer than
     *          16 elements results in undefined behavior. Prefer the array-
     *          reference overload `Matrix4x4(const T (&)[16])` when the
     *          array size is known at compile time.
     *
     * @param elements Pointer to 16 T values in the storage order.
     */
    explicit Matrix4x4(const T* elements) noexcept
      : vecs{}
    {
        for (int i = 0; i < 16; ++i) {
            if constexpr (std::is_same_v<Order, ColMajor>) {
                // col-major: element (row i%4, col i/4) = elements[i]
                element(i % 4, i / 4) = elements[i];
            }
            else {
                // row-major: element (row i/4, col i%4) = elements[i]
                element(i / 4, i % 4) = elements[i];
            }
        }
    }

    /**
     * @brief Construct a matrix from a 16-element array (compile-time size check).
     *
     * @param elements Array of 16 T values in the storage order.
     */
    explicit Matrix4x4(const T (&elements)[16]) noexcept
      : Matrix4x4(static_cast<const T*>(elements))
    {}

  public:
    /**
     * @brief Reset this matrix to identity.
     */
    constexpr void makeIdentity() noexcept
    {
        for (int i = 0; i < 16; ++i) data[i] = T(0);
        data[0] = data[5] = data[10] = data[15] = T(1);
    }

    /**
     * @brief Left-multiply this matrix: M := left * M.
     *
     * The incoming transform is applied in world space (before the existing
     * transform), equivalent to transforming around a fixed/world axis.
     *
     * @param left The transform to apply on the left side.
     * @return Reference to this matrix.
     */
    constexpr Matrix4x4& preMulti(const Matrix4x4& left)
    {
        auto old = *this;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) {
                T v = T(0);
                for (int k = 0; k < 4; ++k) v += left.element(i, k) * old.element(k, j);
                element(i, j) = v;
            }
        return *this;
    }

    /**
     * @brief Right-multiply this matrix: M := M * right.
     *
     * The incoming transform is applied in local space (after the existing
     * transform), equivalent to transforming around a local/moving axis.
     *
     * @param right The transform to apply on the right side.
     * @return Reference to this matrix.
     */
    constexpr Matrix4x4& postMulti(const Matrix4x4& right)
    {
        auto old = *this;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) {
                T v = T(0);
                for (int k = 0; k < 4; ++k) v += old.element(i, k) * right.element(k, j);
                element(i, j) = v;
            }
        return *this;
    }

    /**
     * @brief Calculate the determinant of this 4x4 matrix.
     *
     * For affine matrices (last row = [0 0 0 1]), this equals the
     * determinant of the upper-left 3x3 submatrix, indicating the
     * linear part's orientation and volume scaling:
     *
     * - det > 0: orientation-preserving (no reflection).
     * - det < 0: orientation-reversing (reflection / mirror flip).
     * - det = 0: singular — the linear part loses dimension and is non-invertible.
     *
     * For projection matrices, the determinant still indicates invertibility,
     * but is generally not interpreted as a direct geometric volume scaling factor.
     *
     * For other matrices, the determinant is still mathematically defined
     * and indicates whether the matrix is invertible, but may not have
     * a direct geometric interpretation.
     *
     * @return Determinant value.
     */
    constexpr T determinant() const
    {
        const auto a2323 = element(2, 2) * element(3, 3) - element(3, 2) * element(2, 3);
        const auto a1323 = element(1, 2) * element(3, 3) - element(3, 2) * element(1, 3);
        const auto a1223 = element(1, 2) * element(2, 3) - element(2, 2) * element(1, 3);
        const auto a0323 = element(0, 2) * element(3, 3) - element(3, 2) * element(0, 3);
        const auto a0223 = element(0, 2) * element(2, 3) - element(2, 2) * element(0, 3);
        const auto a0123 = element(0, 2) * element(1, 3) - element(1, 2) * element(0, 3);

        return (element(0, 0) * (element(1, 1) * a2323 - element(2, 1) * a1323 + element(3, 1) * a1223) -
                element(1, 0) * (element(0, 1) * a2323 - element(2, 1) * a0323 + element(3, 1) * a0223) +
                element(2, 0) * (element(0, 1) * a1323 - element(1, 1) * a0323 + element(3, 1) * a0123) -
                element(3, 0) * (element(0, 1) * a1223 - element(1, 1) * a0223 + element(2, 1) * a0123));
    }

    /**
     * @brief Transpose the matrix.
     */
    constexpr void transpose()
    {
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) std::swap(element(i, j), element(j, i));
    }

    /**
     * @brief Return a transposed copy without modifying the original matrix.
     * @return Transposed matrix.
     */
    constexpr Matrix4x4 transposed() const
    {
        Matrix4x4 m(*this);
        m.transpose();
        return m;
    }

    /**
     * @brief Invert the matrix in place.
     *
     * Uses the adjugate method: M⁻¹ = adj(M) / det(M).
     * Inverse exists only when the determinant is non-zero (det(M) != 0).
     *
     * Typical invertible cases:
     * - Rigid transforms (rotation + translation).
     * - Affine transforms with non-zero scale on all axes.
     * - Basis matrix with linearly independent x/y/z axes.
     *
     * Typical non-invertible cases:
     * - Any axis scale is zero (matrix squashes dimension).
     * - Basis axes are linearly dependent (determinant becomes zero).
     * - Any transform that collapses 3D space into lower dimension.
     *
     * @note For singular matrices (det(M) == 0), the matrix is left unchanged.
     * @return true if inversion succeeded (det != 0), false for singular matrices.
     */
    bool invert();

    /**
     * @brief Return an inverted copy without modifying the original matrix.
     *
     * If the matrix is invertible (det != 0), returns the inverse.
     * If the matrix is singular (det == 0), returns a copy of the original matrix unchanged.
     *
     * @note This method cannot report failure. If you need to detect singular matrices,
     *       either check determinant() beforehand or use invert() which returns a bool.
     *
     * @return Inverted matrix, or a copy of the original matrix if singular.
     */
    constexpr Matrix4x4 inverted() const
    {
        Matrix4x4 m(*this);
        m.invert();
        return m;
    }

    /**
     * @brief Is this matrix an identity matrix.
     * @param eps tolerance for floating-point comparisons.
     */
    constexpr bool isIdentity(T eps = EPS<T>()) const
    {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!math::isEqual(element(i, j), (i == j) ? T(1) : T(0), eps))
                    return false;
        return true;
    }

    /**
     * @brief Is this matrix an affine transformation matrix (last row is [0 0 0 1]).
     *        affine matrix that preserve the parallelism of straight lines, such as translation, scaling, rotation,
     *        shearing, and reflection. non-affine matrix includes projection matrix.
     * @param eps Tolerance for floating-point comparisons.
     */
    constexpr bool isAffine(T eps = EPS<T>()) const
    {
        return math::isZero(element(3, 0), eps) && math::isZero(element(3, 1), eps) && math::isZero(element(3, 2), eps) &&
               math::isEqual(element(3, 3), T(1), eps);
    }

    /**
     * @brief Is this matrix a rigid transformation matrix (only rotation and translation, no scaling or shearing or reflection).
     * @param eps Tolerance for floating-point comparisons.
     */
    bool isRigid(T eps = EPS<T>()) const;

    /**
     * @brief Element-wise approximate equality.
     */
    constexpr bool isEqual(const Matrix4x4& other, T eps = EPS<T>()) const
    {
        for (int i = 0; i < 16; ++i)
            if (!math::isEqual(data[i], other.data[i], eps))
                return false;
        return true;
    }

    /**
     * @brief Check if all elements are zero.
     */
    constexpr bool isZero(T eps = EPS<T>()) const
    {
        for (int i = 0; i < 16; ++i)
            if (!math::isZero(data[i], eps))
                return false;
        return true;
    }

  public:
    /**
     * @brief Read matrix element by row and column.
     * @param row Row index.
     * @param col Column index.
     * @return Element value at (row, col).
     */
    [[nodiscard]]
    T operator()(int row, int col) const
    {
        assert(row < 4);
        assert(col < 4);
        return element(row, col);
    }

    /**
     * @brief Access matrix element by row and column.
     * @param row Row index.
     * @param col Column index.
     * @return Mutable element reference at (row, col).
     */
    [[nodiscard]]
    T& operator()(int row, int col)
    {
        assert(row < 4);
        assert(col < 4);
        return element(row, col);
    }

    /**
     * @brief Matrix multiplication.
     */
    constexpr Matrix4x4 operator*(const Matrix4x4& right) const
    {
        Matrix4x4 m;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                T v = T(0);
                for (int k = 0; k < 4; ++k) v += element(i, k) * right.element(k, j);
                m.element(i, j) = v;
            }
        return m;
    }

    /**
     * @brief Matrix multiplication assignment.
     */
    constexpr Matrix4x4& operator*=(const Matrix4x4& right)
    {
        auto m = *this * right;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) element(i, j) = m.element(i, j);
        return *this;
    }

  public:
    union
    {
        struct {
            Vector4<T> vec0;
            Vector4<T> vec1;
            Vector4<T> vec2;
            Vector4<T> vec3;
        };

        Vector4<T> vecs[4];

        T data[16];
    };
};

using Mat4f = Matrix4x4<float>;
using Mat4d = Matrix4x4<double>;

VN_MATH_NS_END
