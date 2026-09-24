#pragma once

#include "math_global.hpp"

#include "Point3.hpp"
#include "Quaternion.hpp"
#include "Vector3.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief 3D rigid transformation class that represents translation, rotation.
 * @tparam T Only accepts float and double
 */
template <typename T>
class Isometry3 {
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct an identity transform.
     */
    constexpr Isometry3() noexcept
      : translation()
      , rotation()
    {}

    /**
     * @brief Construct from translation and rotation.
     * @param t Translation offset.
     * @param q Rotation quaternion.
     */
    constexpr Isometry3(const Point3<T>& t, const Quaternion<T>& q) noexcept
      : translation(t)
      , rotation(q)
    {}

  public:
    /**
     * @brief Invert this transformation in place.
     */
    void invert();

    /**
     * @brief Invert this transformation.
     * @return Inverted transformation.
     */
    Isometry3<T> inverted() const;

    /**
     * @brief Pre-multiply a translation: T := T_trans(dt) * T.
     * @param dt Translation offset in world space.
     */
    Isometry3<T>& preTranslate(const Vector3<T>& dt);

    /**
     * @brief Post-multiply a translation: T := T * T_trans(dt).
     * @param dt Translation offset in local space.
     */
    Isometry3<T>& postTranslate(const Vector3<T>& dt);

    /**
     * @brief Pre-multiply a quaternion rotation: T := T_rot(quat) * T.
     * @param quat Rotation quaternion (world-space).
     */
    Isometry3<T>& preRotate(const Quaternion<T>& quat);

    /**
     * @brief Pre-multiply an axis-angle rotation: T := T_rot(axis, angle) * T.
     * @param axis  Rotation axis (world-space), should be normalized.
     * @param angle Rotation angle in radians.
     */
    Isometry3<T>& preRotate(const Vector3<T>& axis, T angle);

    /**
     * @brief Post-multiply a quaternion rotation: T := T * T_rot(quat).
     * @param quat Rotation quaternion (local-space).
     */
    Isometry3<T>& postRotate(const Quaternion<T>& quat);

    /**
     * @brief Post-multiply an axis-angle rotation: T := T * T_rot(axis, angle).
     * @param axis  Rotation axis (local-space), should be normalized.
     * @param angle Rotation angle in radians.
     */
    Isometry3<T>& postRotate(const Vector3<T>& axis, T angle);

    /**
     * @brief Compose two transforms: T₁ * T₂.
     */
    Isometry3<T>  operator*(const Isometry3<T>& right) const;
    Isometry3<T>& operator*=(const Isometry3<T>& right);

    /**
     * @brief Local X axis in parent coordinates.
     */
    [[nodiscard]] Vector3<T> axisX() const
    {
        return rotation * Vector3<T>::unitX();
    }

    /**
     * @brief Local Y axis in parent coordinates.
     */
    [[nodiscard]] Vector3<T> axisY() const
    {
        return rotation * Vector3<T>::unitY();
    }

    /**
     * @brief Local Z axis in parent coordinates.
     */
    [[nodiscard]] Vector3<T> axisZ() const
    {
        return rotation * Vector3<T>::unitZ();
    }

  public:
    Point3<T>     translation;
    Quaternion<T> rotation;
};

template <typename T>
Point3<T> operator*(const Isometry3<T>& t, const Point3<T>& p);

template <typename T>
Vector3<T> operator*(const Isometry3<T>& t, const Vector3<T>& v);

using Isometry3f = Isometry3<float>;
using Isometry3d = Isometry3<double>;

VN_MATH_NS_END
