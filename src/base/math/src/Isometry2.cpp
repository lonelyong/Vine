#include <vine/math/Isometry2.hpp>

#include <cmath>

VN_MATH_NS_BEGIN

template <typename T>
Isometry2<T> Isometry2<T>::inverted() const
{
    // T⁻¹ = (-θ, -R(-θ) * t)
    Isometry2<T> inv;
    inv.angle = -angle;
    const auto c = std::cos(inv.angle);
    const auto s = std::sin(inv.angle);
    inv.translation.x = -(c * translation.x - s * translation.y);
    inv.translation.y = -(s * translation.x + c * translation.y);
    return inv;
}

template <typename T>
void Isometry2<T>::invert()
{
    angle = -angle;
    const auto c  = std::cos(angle);
    const auto s  = std::sin(angle);
    const auto tx = translation.x;
    const auto ty = translation.y;
    translation.x = -(c * tx - s * ty);
    translation.y = -(s * tx + c * ty);
}

template <typename T>
Isometry2<T>& Isometry2<T>::preTranslate(const Vector2<T>& dt)
{
    translation.x += dt.x;
    translation.y += dt.y;
    return *this;
}

template <typename T>
Isometry2<T>& Isometry2<T>::postTranslate(const Vector2<T>& dt)
{
    // T * T_trans = (θ, R(θ) * dt + t)
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    translation.x += c * dt.x - s * dt.y;
    translation.y += s * dt.x + c * dt.y;
    return *this;
}

template <typename T>
Isometry2<T>& Isometry2<T>::preRotate(T a)
{
    // T_rot(a) * T = (a + θ, R(a) * t)
    const auto c = std::cos(a);
    const auto s = std::sin(a);
    const auto tx = translation.x;
    const auto ty = translation.y;
    translation.x = c * tx - s * ty;
    translation.y = s * tx + c * ty;
    angle += a;
    return *this;
}

template <typename T>
Isometry2<T>& Isometry2<T>::postRotate(T a)
{
    // T * T_rot(a) = (θ + a, t)
    angle += a;
    return *this;
}

template <typename T>
Isometry2<T> Isometry2<T>::operator*(const Isometry2<T>& right) const
{
    // (θ₁, t₁) * (θ₂, t₂) = (θ₁ + θ₂,  R(θ₁) * t₂ + t₁)
    Isometry2<T> result;
    result.angle = angle + right.angle;
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    result.translation.x = translation.x + c * right.translation.x - s * right.translation.y;
    result.translation.y = translation.y + s * right.translation.x + c * right.translation.y;
    return result;
}

template <typename T>
Isometry2<T>& Isometry2<T>::operator*=(const Isometry2<T>& right)
{
    *this = *this * right;
    return *this;
}

template <typename T>
Point2<T> operator*(const Isometry2<T>& t, const Point2<T>& p)
{
    // p' = R(θ) * p + t
    const auto c = std::cos(t.angle);
    const auto s = std::sin(t.angle);
    return Point2<T>(c * p.x - s * p.y + t.translation.x,
                     s * p.x + c * p.y + t.translation.y);
}

template <typename T>
Vector2<T> operator*(const Isometry2<T>& t, const Vector2<T>& v)
{
    // v' = R(θ) * v  (pure rotation, no translation)
    const auto c = std::cos(t.angle);
    const auto s = std::sin(t.angle);
    return Vector2<T>(c * v.x - s * v.y,
                      s * v.x + c * v.y);
}

template class VN_MATH_API Isometry2<float>;
template class VN_MATH_API Isometry2<double>;
template VN_MATH_API Point2<float> operator*(const Isometry2<float>&, const Point2<float>&);
template VN_MATH_API Point2<double> operator*(const Isometry2<double>&, const Point2<double>&);
template VN_MATH_API Vector2<float> operator*(const Isometry2<float>&, const Vector2<float>&);
template VN_MATH_API Vector2<double> operator*(const Isometry2<double>&, const Vector2<double>&);

VN_MATH_NS_END