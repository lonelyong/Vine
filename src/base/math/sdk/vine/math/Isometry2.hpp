#pragma once

#include "math_global.hpp"

#include "Point2.hpp"
#include "Vector2.hpp"

VN_MATH_NS_BEGIN

/**
 * @brief 2D rigid transformation (translation + rotation), no scale.
 * @tparam T floating-point type (typically float or double).
 *
 * Rotation is stored as a single angle in radians. A point p is
 * transformed as:
 *
 *   p' = R(θ) * p + t
 *
 * Composition: T₁ * T₂ = (θ₁ + θ₂,  R(θ₁) * t₂ + t₁)
 */
template <typename T>
class Isometry2 {
  public:
    using value_type = T;

  public:
    /**
     * @brief Construct an identity transform.
     */
    constexpr Isometry2() noexcept
      : translation()
      , angle(T(0))
    {}

    /**
     * @brief Construct from translation and rotation angle.
     * @param t Translation offset.
     * @param a Rotation angle in radians (CCW).
     */
    constexpr Isometry2(const Point2<T>& t, T a) noexcept
      : translation(t)
      , angle(a)
    {}

  public:
    /**
     * @brief Invert this transformation.
     * @return Inverted transformation.
     */
    Isometry2<T> inverted() const;

    /**
     * @brief Invert this transformation in place.
     */
    void invert();

    /**
     * @brief Pre-multiply a translation: T := T_trans(dt) * T.
     * @param dt Translation offset in world space.
     */
    Isometry2<T>& preTranslate(const Vector2<T>& dt);

    /**
     * @brief Post-multiply a translation: T := T * T_trans(dt).
     * @param dt Translation offset in local space.
     */
    Isometry2<T>& postTranslate(const Vector2<T>& dt);

    /**
     * @brief Pre-multiply a rotation by angle: T := T_rot(a) * T.
     * @param a Rotation angle in radians (CCW), world space.
     */
    Isometry2<T>& preRotate(T a);

    /**
     * @brief Post-multiply a rotation by angle: T := T * T_rot(a).
     * @param a Rotation angle in radians (CCW), local space.
     */
    Isometry2<T>& postRotate(T a);

    /**
     * @brief Compose two transforms: T₁ * T₂.
     */
    Isometry2<T>  operator*(const Isometry2<T>& right) const;
    Isometry2<T>& operator*=(const Isometry2<T>& right);

  public:
    Point2<T> translation; ///< Translation offset.
    T         angle;       ///< Rotation angle in radians (CCW).
};

template <typename T>
Point2<T> operator*(const Isometry2<T>& t, const Point2<T>& p);

template <typename T>
Vector2<T> operator*(const Isometry2<T>& t, const Vector2<T>& v);

using Isometry2f = Isometry2<float>;
using Isometry2d = Isometry2<double>;

VN_MATH_NS_END