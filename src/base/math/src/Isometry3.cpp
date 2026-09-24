#include <vine/math/Isometry3.hpp>

#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

VN_MATH_NS_BEGIN

template <typename T>
void Isometry3<T>::invert()
{
    // T = (q, t)  =>  T⁻¹ = (q⁻¹, -q⁻¹ * t)
    rotation    = rotation.conj();
    translation = -(rotation * translation);
}

template <typename T>
Isometry3<T> Isometry3<T>::inverted() const
{
    // T = (q, t)  =>  T⁻¹ = (q⁻¹, -q⁻¹ * t)
    Isometry3<T> inv;
    inv.rotation    = rotation.conj();
    inv.translation = -(inv.rotation * translation);
    return inv;
}

template <typename T>
Isometry3<T>& Isometry3<T>::preTranslate(const Vector3<T>& dt)
{
    // T := T_trans * T = (q, t + dt)
    translation += dt;
    return *this;
}

template <typename T>
Isometry3<T>& Isometry3<T>::postTranslate(const Vector3<T>& dt)
{
    // T := T * T_trans = (q, q * dt + t)
    const auto rotated = rotation * dt;
    translation.x += rotated.x;
    translation.y += rotated.y;
    translation.z += rotated.z;
    return *this;
}

template <typename T>
Isometry3<T>& Isometry3<T>::preRotate(const Quaternion<T>& quat)
{
    // T := T_rot * T = (quat * q, quat * t)
    translation = quat * translation;
    rotation = quat * rotation;
    return *this;
}

template <typename T>
Isometry3<T>& Isometry3<T>::preRotate(const Vector3<T>& axis, T angle)
{
    return preRotate(Quaternion<T>(angle, axis));
}

template <typename T>
Isometry3<T>& Isometry3<T>::postRotate(const Quaternion<T>& quat)
{
    // T := T * T_rot = (q * quat, t)
    rotation = rotation * quat;
    return *this;
}

template <typename T>
Isometry3<T>& Isometry3<T>::postRotate(const Vector3<T>& axis, T angle)
{
    return postRotate(Quaternion<T>(angle, axis));
}

template <typename T>
Isometry3<T> Isometry3<T>::operator*(const Isometry3<T>& right) const
{
    // (q₁, t₁) * (q₂, t₂) = (q₁ * q₂,  q₁ * t₂ + t₁)
    Isometry3<T> result;
    result.rotation    = rotation * right.rotation;
    const auto rotated = rotation * right.translation;
    result.translation = Point3<T>(translation.x + rotated.x,
                                   translation.y + rotated.y,
                                   translation.z + rotated.z);
    return result;
}

template <typename T>
Isometry3<T>& Isometry3<T>::operator*=(const Isometry3<T>& right)
{
    *this = *this * right;
    return *this;
}

template <typename T>
Point3<T> operator*(const Isometry3<T>& t, const Point3<T>& p)
{
    // p' = q * p + t
    const auto rotated = t.rotation * p;
    return Point3<T>(rotated.x + t.translation.x,
                     rotated.y + t.translation.y,
                     rotated.z + t.translation.z);
}

template <typename T>
Vector3<T> operator*(const Isometry3<T>& t, const Vector3<T>& v)
{
    // v' = q * v  (pure rotation, no translation)
    return t.rotation * v;
}

template class VN_MATH_API Isometry3<float>;
template class VN_MATH_API Isometry3<double>;
template VN_MATH_API Point3<float> operator*(const Isometry3<float>&, const Point3<float>&);
template VN_MATH_API Point3<double> operator*(const Isometry3<double>&, const Point3<double>&);
template VN_MATH_API Vector3<float> operator*(const Isometry3<float>&, const Vector3<float>&);
template VN_MATH_API Vector3<double> operator*(const Isometry3<double>&, const Vector3<double>&);

VN_MATH_NS_END