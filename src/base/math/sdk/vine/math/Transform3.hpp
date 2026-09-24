#pragma once

#include "math_global.hpp"

#include <cstring>

#include "Isometry3.hpp"
#include "Matrix3x3.hpp"
#include "Matrix4x4.hpp"
#include "Point3.hpp"
#include "Quaternion.hpp"
#include "Vector3.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief Create a translation matrix from vector.
 * @param offset Translation offset.
 * @return Translation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> translate(const Vector3<T>& offset)
{
    Matrix4x4<T, Order> m;
    m.makeIdentity();
    m(0, 3) = offset.x;
    m(1, 3) = offset.y;
    m(2, 3) = offset.z;
    return m;
}

/**
 * @brief Create a translation matrix from components.
 * @param x Translation along X axis.
 * @param y Translation along Y axis.
 * @param z Translation along Z axis.
 * @return Translation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> translate(T x, T y, T z)
{
    Matrix4x4<T, Order> m;
    m.makeIdentity();
    m(0, 3) = x;
    m(1, 3) = y;
    m(2, 3) = z;
    return m;
}

/**
 * @brief Convert a unit quaternion to a 3x3 rotation matrix.
 *
 * The rotation is fundamentally a 3×3 operation.  The 4×4 rotate() overload
 * delegates here and embeds the result into a 4×4 identity frame.
 *
 * @param quat Rotation quaternion (auto-normalized).
 * @return 3×3 rotation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix3x3<T, Order> rotate3x3(const Quaternion<T>& quat)
{
    auto       q      = quat;
    const auto q_len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;

    // Zero quaternion → identity.
    if (q_len2 == T(0))
        return Matrix3x3<T, Order>{ T(1), T(0), T(0), T(0), T(1), T(0), T(0), T(0), T(1) };

    // Normalize if not already unit length.
    if (!math::isEqual(q_len2, T(1), T(1e-12))) {
        const auto inv = T(1) / std::sqrt(q_len2);
        q.x *= inv;
        q.y *= inv;
        q.z *= inv;
        q.w *= inv;
    }

    const auto x = q.x, y = q.y, z = q.z, w = q.w;
    const auto xx = x * x, yy = y * y, zz = z * z;
    const auto xy = x * y, zw = z * w, xz = x * z;
    const auto yw = y * w, yz = y * z, xw = x * w;

    // R = I + 2w·[qᵥ]× + 2[qᵥ]×²
    return Matrix3x3<T, Order>(T(1) - T(2) * (yy + zz),
                               T(2) * (xy - zw),
                               T(2) * (xz + yw),
                               T(2) * (xy + zw),
                               T(1) - T(2) * (xx + zz),
                               T(2) * (yz - xw),
                               T(2) * (xz - yw),
                               T(2) * (yz + xw),
                               T(1) - T(2) * (xx + yy));
}

/**
 * @brief Create a rotation matrix from a unit quaternion.
 * @param quat Rotation quaternion.
 * @return Rotation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> rotate(const Quaternion<T>& quat)
{
    const auto R = rotate3x3(quat);
    Matrix4x4<T, Order> m;
    m(0, 0) = R(0, 0); m(0, 1) = R(0, 1); m(0, 2) = R(0, 2); m(0, 3) = T(0);
    m(1, 0) = R(1, 0); m(1, 1) = R(1, 1); m(1, 2) = R(1, 2); m(1, 3) = T(0);
    m(2, 0) = R(2, 0); m(2, 1) = R(2, 1); m(2, 2) = R(2, 2); m(2, 3) = T(0);
    m(3, 0) = T(0);    m(3, 1) = T(0);    m(3, 2) = T(0);    m(3, 3) = T(1);
    return m;
}

/**
 * @brief Create an axis-angle rotation matrix.
 * @param axis  Rotation axis.
 * @param angle Rotation angle in radians.
 * @return Rotation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> rotate(const Vector3<T>& axis, T angle);

/**
 * @brief Create a rotation matrix from start and end vectors.
 * @param start Source direction vector.
 * @param end   Target direction vector.
 * @return Rotation matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> rotate(const Vector3<T>& from, const Vector3<T>& to)
{
    Quaternion<T> q;
    q.makeRotate(from, to);
    return rotate(q);
}

/**
 * @brief Create a non-uniform scale matrix from vector.
 * @param vec Scale factors for x/y/z.
 * @return Scale matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> scale(const Vector3<T>& vec)
{
    Matrix4x4<T, Order> m;
    m.makeIdentity();
    m(0, 0) = vec.x;
    m(1, 1) = vec.y;
    m(2, 2) = vec.z;
    return m;
}

/**
 * @brief Create a non-uniform scale matrix from components.
 * @param x Scale factor along X axis.
 * @param y Scale factor along Y axis.
 * @param z Scale factor along Z axis.
 * @return Scale matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> scale(T x, T y, T z)
{
    Matrix4x4<T, Order> m;
    m.makeIdentity();
    m(0, 0) = x;
    m(1, 1) = y;
    m(2, 2) = z;
    return m;
}

/**
 * @brief Create a uniform scale matrix.
 * @param factor Uniform scale factor.
 * @return Scale matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> scale(T factor)
{
    Matrix4x4<T, Order> m;
    m.makeIdentity();
    m(0, 0) = factor;
    m(1, 1) = factor;
    m(2, 2) = factor;
    return m;
}

/**
 * @brief Build a look-at view matrix.
 * @param eye Camera position.
 * @param target Camera target point.
 * @param up Up direction reference (does not need to be orthogonal).
 * @note Uses backward-axis convention: backward = eye - target. Produces a
 *       view matrix suitable for right-handed coordinates where the camera
 *       looks along the negative Z direction in view space.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> lookAt(const Point3<T>& eye, const Point3<T>& target, const Vector3<T>& up);

/**
 * @brief Make an orthographic projection matrix.
 * @param left the left clipping plane.
 * @param right the right clipping plane.
 * @param bottom the bottom clipping plane.
 * @param top the top clipping plane.
 * @param z_near the near clipping plane.
 * @param z_far the far clipping plane.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> ortho(double left, double right, double bottom, double top, double z_near, double z_far)
{
    Matrix4x4<T, Order> m;
    auto                tx = -(right + left) / (right - left);
    auto                ty = -(top + bottom) / (top - bottom);
    auto                tz = -(z_far + z_near) / (z_far - z_near);

    // Column 0
    m(0, 0) = T(2) / (right - left);
    m(1, 0) = T(0);
    m(2, 0) = T(0);
    m(3, 0) = T(0);
    // Column 1
    m(0, 1) = T(0);
    m(1, 1) = T(2) / (top - bottom);
    m(2, 1) = T(0);
    m(3, 1) = T(0);
    // Column 2
    m(0, 2) = T(0);
    m(1, 2) = T(0);
    m(2, 2) = T(-2) / (z_far - z_near);
    m(3, 2) = T(0);
    // Column 3 (translation)
    m(0, 3) = T(tx);
    m(1, 3) = T(ty);
    m(2, 3) = T(tz);
    m(3, 3) = T(1);

    return m;
}

/**
 * @brief Make a perspective projection matrix.
 * @param fovy the vertical field of view angle in radians.
 * @param aspect_ratio the viewport aspect ratio (width / height).
 * @param z_near the near clipping plane (positive, > 0).
 * @param z_far the far clipping plane.
 *
 * @note Produces a right-handed projection matrix compatible with column-major
 *       conventions used throughout this class. NDC conventions follow the
 *       framework's existing usage (check consumer code if unsure).
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> perspective(double fovy, double aspect_ratio, double z_near, double z_far)
{
    Matrix4x4<T, Order> m;
    const auto          f = T(1) / std::tan(fovy / 2.0);
    m.makeIdentity();
    m(0, 0) = T(0);
    m(1, 1) = T(0);
    m(2, 2) = T(0);
    m(3, 3) = T(0);

    m(0, 0) = T(f / aspect_ratio);
    m(1, 1) = T(f);
    m(2, 2) = T((z_far + z_near) / (z_near - z_far));
    m(3, 2) = T(-1);
    m(2, 3) = T(2 * z_far * z_near / (z_near - z_far));

    return m;
}

/**
 * @brief Make a reflection matrix across a plane defined by its normal and offset.
 * @param plane_normal Normal vector of the mirror plane.
 * @param plane_offset Offset of the mirror plane from the origin.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> reflect(const Vector3<T>& plane_normal, T plane_offset);

/**
 * @brief Decompose an affine matrix into translation, rotation, and scale.
 *
 * Assumes the matrix factors as M = translate(t) × rotate(r) × scale(s).
 * Scale is extracted as column vector lengths; rotation is recovered from
 * the normalized columns via the trace method.  Reflection is handled by
 * moving the minus sign into s.x so r remains a proper right-handed rotation.
 *
 * Handling by transform type:
 *   Translation               → t = col[3],   r = identity, s = (1,1,1)
 *   Rotation                  → t = (0,0,0),  r = quat,     s = (1,1,1)
 *   Scale (axis-aligned)      → t = (0,0,0),  r = identity, s = diag
 *   Rotation × Scale          → t = (0,0,0),  r = quat,     s = col lengths
 *   Rotation × Reflection     → handedness flip moves minus sign into s.x
 *   TRS (standard)            → all three components extracted
 *   Shear                     → rejected (non-orthogonal normalized columns)
 *   Projection                → rejected (non-affine)
 *   Zero scale on any axis    → rejected (degenerate)
 *
 * @param[in]  m   Input 4×4 affine matrix.
 * @param[out] t   Translation vector (column 3).
 * @param[out] r   Rotation quaternion.
 * @param[out] s   Per-axis scale factors (column vector lengths).
 * @return true if decomposition succeeded, false if the matrix is not
 *         expressible as a valid scale × rotation × translation.
 */
template <typename T, typename Order>
bool decompose(const Matrix4x4<T, Order>& m, Vector3<T>& t, Quaternion<T>& r, Vector3<T>& s);

/**
 * @brief Transform a direction vector (w = 0, translation ignored).
 *        Uses the upper-left 3x3 linear block only.
 * @param m Transform matrix.
 * @param v Input vector.
 * @return Transformed vector.
 */
template <typename T, typename Order>
Vector3<T> operator*(const Matrix4x4<T, Order>& m, const Vector3<T>& v)
{
    return Vector3<T>(m(0, 0) * v.x + m(0, 1) * v.y + m(0, 2) * v.z,
                      m(1, 0) * v.x + m(1, 1) * v.y + m(1, 2) * v.z,
                      m(2, 0) * v.x + m(2, 1) * v.y + m(2, 2) * v.z);
}

/**
 * @brief Transform a point (w = 1, full affine with perspective divide).
 * @param m Transform matrix.
 * @param p Input point.
 * @return Transformed point (perspective-divided if w != 1).
 */
template <typename T, typename Order>
Point3<T> operator*(const Matrix4x4<T, Order>& m, const Point3<T>& p)
{
    const auto x = m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2) * p.z + m(0, 3);
    const auto y = m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2) * p.z + m(1, 3);
    const auto z = m(2, 0) * p.x + m(2, 1) * p.y + m(2, 2) * p.z + m(2, 3);
    const auto w = m(3, 0) * p.x + m(3, 1) * p.y + m(3, 2) * p.z + m(3, 3);

    if (math::isEqual(w, T(1), T(1e-12)))
        return Point3<T>(x, y, z);
    if (math::isZero(w, T(1e-12)))
        return Point3<T>(x, y, z);
    return Point3<T>(x / w, y / w, z / w);
}

/**
 * @brief 3×3 linear transform of a vector (no translation).
 * @param m 3×3 matrix.
 * @param v Input vector.
 * @return Transformed vector.
 */
template <typename T>
Vector3<T> operator*(const Matrix3x3<T>& m, const Vector3<T>& v)
{
    return Vector3<T>(m(0, 0) * v.x + m(0, 1) * v.y + m(0, 2) * v.z,
                      m(1, 0) * v.x + m(1, 1) * v.y + m(1, 2) * v.z,
                      m(2, 0) * v.x + m(2, 1) * v.y + m(2, 2) * v.z);
}

/**
 * @brief 3×3 linear transform of a point (no translation).
 * @param m 3×3 matrix.
 * @param p Input point.
 * @return Transformed point.
 */
template <typename T>
Point3<T> operator*(const Matrix3x3<T>& m, const Point3<T>& p)
{
    return Point3<T>(m(0, 0) * p.x + m(0, 1) * p.y + m(0, 2) * p.z,
                     m(1, 0) * p.x + m(1, 1) * p.y + m(1, 2) * p.z,
                     m(2, 0) * p.x + m(2, 1) * p.y + m(2, 2) * p.z);
}

/**
 * @brief Construct a 4x4 rigid transform matrix from an Isometry3 object.
 * @param tf Isometry3 object containing translation and rotation.
 * @return 4x4 rigid transform matrix.
 */
template <typename T, typename Order = ColMajor>
Matrix4x4<T, Order> matrix4x4(const Isometry3<T>& tf)
{
    const auto x = tf.axisX();
    const auto y = tf.axisY();
    const auto z = tf.axisZ();

    Matrix4x4<T, Order> m;
    m(0, 0) = x.x;
    m(0, 1) = y.x;
    m(0, 2) = z.x;
    m(0, 3) = tf.translation.x;
    m(1, 0) = x.y;
    m(1, 1) = y.y;
    m(1, 2) = z.y;
    m(1, 3) = tf.translation.y;
    m(2, 0) = x.z;
    m(2, 1) = y.z;
    m(2, 2) = z.z;
    m(2, 3) = tf.translation.z;
    m(3, 0) = T(0);
    m(3, 1) = T(0);
    m(3, 2) = T(0);
    m(3, 3) = T(1);
    return m;
}

VN_MATH_NS_END
