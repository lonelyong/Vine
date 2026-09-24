#include <vine/math/Matrix3x3.hpp>

#include <vine/math/Math.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T, typename Order>

TMPL_PREFIX bool Matrix3x3<T, Order>::invert()
{
    const auto a = element(0, 0), b = element(0, 1), c = element(0, 2);
    const auto d = element(1, 0), e = element(1, 1), f = element(1, 2);
    const auto g = element(2, 0), h = element(2, 1), i = element(2, 2);

    const T det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (math::isZero(det, EPS<T>()))
        return false;

    const T   inv_det = T(1) / det;
    Matrix3x3 adj;
    adj.element(0, 0) = (e * i - f * h);
    adj.element(1, 0) = -(b * i - c * h);
    adj.element(2, 0) = (b * f - c * e);
    adj.element(0, 1) = -(d * i - f * g);
    adj.element(1, 1) = (a * i - c * g);
    adj.element(2, 1) = -(a * f - c * d);
    adj.element(0, 2) = (d * h - e * g);
    adj.element(1, 2) = -(a * h - b * g);
    adj.element(2, 2) = (a * e - b * d);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            element(i, j) = adj.element(i, j) * inv_det;
        }
    }

    return true;
}

#undef TMPL_PREFIX

template class VN_MATH_API Matrix3x3<float, ColMajor>;
template class VN_MATH_API Matrix3x3<double, ColMajor>;
template class VN_MATH_API Matrix3x3<float, RowMajor>;
template class VN_MATH_API Matrix3x3<double, RowMajor>;

VN_MATH_NS_END