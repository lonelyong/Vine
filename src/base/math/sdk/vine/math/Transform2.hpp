#pragma once

#include "math_global.hpp"

#include "Isometry2.hpp"
#include "Matrix3x3.hpp"
#include "Point2.hpp"
#include "Vector2.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief Create a translation matrix from vector.
 * @param offset Translation offset.
 * @return Translation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> translate(const Vector2<T>& offset)
{
    Matrix3x3<T, Order> m;
    m.makeIdentity();
    m(0, 2) = offset.x;
    m(1, 2) = offset.y;
    return m;
}

/**
 * @brief Create a translation matrix from components.
 * @param x Translation along X axis.
 * @param y Translation along Y axis.
 * @return Translation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> translate(T x, T y)
{
    Matrix3x3<T, Order> m;
    m.makeIdentity();
    m(0, 2) = x;
    m(1, 2) = y;
    return m;
}

/**
 * @brief Create a 2D rotation matrix (CCW).
 * @param angle Rotation angle in radians.
 * @return Rotation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> rotate(T angle)
{
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);

    Matrix3x3<T, Order> m;
    m(0, 0) = c;    m(0, 1) = -s;   m(0, 2) = T(0);
    m(1, 0) = s;    m(1, 1) = c;    m(1, 2) = T(0);
    m(2, 0) = T(0); m(2, 1) = T(0); m(2, 2) = T(1);
    return m;
}

/**
 * @brief Create a 2D rotation matrix from start and end direction vectors.
 * @param from Source direction vector.
 * @param to   Target direction vector.
 * @return Rotation matrix (from towards to).
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> rotate(const Vector2<T>& from, const Vector2<T>& to)
{
    // Rows of the rotation: cos = from·to, sin = fromₓ·to_y − from_y·toₓ
    const auto c = from.dot(to);
    const auto s = from.x * to.y - from.y * to.x;

    Matrix3x3<T, Order> m;
    m(0, 0) = c;    m(0, 1) = -s;   m(0, 2) = T(0);
    m(1, 0) = s;    m(1, 1) = c;    m(1, 2) = T(0);
    m(2, 0) = T(0); m(2, 1) = T(0); m(2, 2) = T(1);
    return m;
}

/**
 * @brief Create a non-uniform scale matrix from vector.
 * @param vec Scale factors for x/y.
 * @return Scale matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> scale(const Vector2<T>& vec)
{
    Matrix3x3<T, Order> m;
    m.makeIdentity();
    m(0, 0) = vec.x;
    m(1, 1) = vec.y;
    return m;
}

/**
 * @brief Create a non-uniform scale matrix from components.
 * @param x Scale factor along X axis.
 * @param y Scale factor along Y axis.
 * @return Scale matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> scale(T x, T y)
{
    Matrix3x3<T, Order> m;
    m.makeIdentity();
    m(0, 0) = x;
    m(1, 1) = y;
    return m;
}

/**
 * @brief Create a uniform scale matrix.
 * @param factor Uniform scale factor.
 * @return Scale matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> scale(T factor)
{
    Matrix3x3<T, Order> m;
    m.makeIdentity();
    m(0, 0) = factor;
    m(1, 1) = factor;
    return m;
}

/**
 * @brief Decompose an affine 2D matrix into translation, rotation, and scale.
 *
 * Assumes M = translate(t) × rotate(θ) × scale(s).
 * Scale is column vector lengths; angle from atan2 after normalization.
 * Reflection is handled by moving the minus sign into s.x.
 *
 * Returns false when: non-affine, degenerate (zero-length column),
 * or sheared (non-orthogonal columns).
 *
 * @param[in]  m Input 3×3 affine matrix.
 * @param[out] t Translation vector (column 2).
 * @param[out] a Rotation angle in radians (CCW).
 * @param[out] s Per-axis scale factors (column vector lengths).
 * @return true if decomposition succeeded.
 */
template <typename T, typename Order>
bool decompose(const Matrix3x3<T, Order>& m, Vector2<T>& t, T& a, Vector2<T>& s)
{
    using namespace math;

    // ---- translation --------------------------------------------------------
    t.x = m(0, 2);
    t.y = m(1, 2);

    // ---- reject non-affine --------------------------------------------------
    if (!m.isAffine(EPS<T>()))
        return false;

    // ---- scale --------------------------------------------------------------
    const Vector2<T> cx(m(0, 0), m(1, 0));
    const Vector2<T> cy(m(0, 1), m(1, 1));

    const T col0_len = std::sqrt(cx.length2());
    const T col1_len = std::sqrt(cy.length2());

    s.x = col0_len;
    s.y = col1_len;

    // ---- reject degenerate --------------------------------------------------
    if (math::isZero(col0_len, EPS<T>()) || math::isZero(col1_len, EPS<T>()))
        return false;

    // ---- normalize columns → orthonormal R ----------------------------------
    const T m00 = cx.x / col0_len, m10 = cx.y / col0_len;
    const T m01 = cy.x / col1_len, m11 = cy.y / col1_len;

    // ---- reject shear -------------------------------------------------------
    if (!math::isZero(m00 * m01 + m10 * m11, EPS<T>())) {
        a = T(0);
        return false;
    }

    // ---- rotation: atan2 of the first column --------------------------------
    // Detect reflection (det < 0) and flip sign into s.x.
    if (!math::isEqual(m00 * m11 - m10 * m01, T(1), EPS<T>())) {
        s.x = -s.x;
        a = std::atan2(-m10, -m00);
    }
    else {
        a = std::atan2(m10, m00);
    }
    return true;
}

/**
 * @brief Transform a direction vector (w = 0, translation ignored).
 * @param m Transform matrix.
 * @param v Input vector.
 * @return Transformed vector.
 */
template <typename T, typename Order>
Vector2<T> operator*(const Matrix3x3<T, Order>& m, const Vector2<T>& v)
{
    return Vector2<T>(m(0, 0) * v.x + m(0, 1) * v.y,
                      m(1, 0) * v.x + m(1, 1) * v.y);
}

/**
 * @brief Transform a point (w = 1, full affine with homogeneous divide).
 * @param m Transform matrix.
 * @param p Input point.
 * @return Transformed point.
 */
template <typename T, typename Order>
Point2<T> operator*(const Matrix3x3<T, Order>& m, const Point2<T>& p)
{
    const auto x = m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2);
    const auto y = m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2);
    const auto w = m(2, 0) * p.x + m(2, 1) * p.y + m(2, 2);

    if (math::isEqual(w, T(1), T(1e-12)))
        return Point2<T>(x, y);
    if (math::isZero(w, T(1e-12)))
        return Point2<T>(x, y);
    return Point2<T>(x / w, y / w);
}

/**
 * @brief Construct a 3x3 rigid transform matrix from an Isometry2 object.
 * @param tf Isometry2 object containing translation and rotation angle.
 * @return 3x3 rigid transform matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> matrix3x3(const Isometry2<T>& tf)
{
    const auto c = std::cos(tf.angle);
    const auto s = std::sin(tf.angle);

    Matrix3x3<T, Order> m;
    m(0, 0) = c;    m(0, 1) = -s;   m(0, 2) = tf.translation.x;
    m(1, 0) = s;    m(1, 1) = c;    m(1, 2) = tf.translation.y;
    m(2, 0) = T(0); m(2, 1) = T(0); m(2, 2) = T(1);
    return m;
}

VN_MATH_NS_END
