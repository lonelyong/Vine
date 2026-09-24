#include <vine/math/Matrix4x4.hpp>

#include <cmath>

#include <vine/math/Math.hpp>
#include <vine/math/Vector3.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T, typename Order>

TMPL_PREFIX bool Matrix4x4<T, Order>::invert()
{
    // Invert via adjugate:  M^(-1) = adj(M) / det(M).
    // Layout-independent via element(row, col).

    // 2x2 minors (building blocks for 3x3 cofactors).
    const T a2323 = element(2, 2) * element(3, 3) - element(3, 2) * element(2, 3);
    const T a1323 = element(1, 2) * element(3, 3) - element(3, 2) * element(1, 3);
    const T a1223 = element(1, 2) * element(2, 3) - element(2, 2) * element(1, 3);
    const T a0323 = element(0, 2) * element(3, 3) - element(3, 2) * element(0, 3);
    const T a0223 = element(0, 2) * element(2, 3) - element(2, 2) * element(0, 3);
    const T a0123 = element(0, 2) * element(1, 3) - element(1, 2) * element(0, 3);
    const T a2313 = element(2, 1) * element(3, 3) - element(3, 1) * element(2, 3);
    const T a1313 = element(1, 1) * element(3, 3) - element(3, 1) * element(1, 3);
    const T a1213 = element(1, 1) * element(2, 3) - element(2, 1) * element(1, 3);
    const T a2312 = element(2, 1) * element(3, 2) - element(3, 1) * element(2, 2);
    const T a1312 = element(1, 1) * element(3, 2) - element(3, 1) * element(1, 2);
    const T a1212 = element(1, 1) * element(2, 2) - element(2, 1) * element(1, 2);
    const T a0313 = element(0, 1) * element(3, 3) - element(3, 1) * element(0, 3);
    const T a0213 = element(0, 1) * element(2, 3) - element(2, 1) * element(0, 3);
    const T a0312 = element(0, 1) * element(3, 2) - element(3, 1) * element(0, 2);
    const T a0212 = element(0, 1) * element(2, 2) - element(2, 1) * element(0, 2);
    const T a0113 = element(0, 1) * element(1, 3) - element(1, 1) * element(0, 3);
    const T a0112 = element(0, 1) * element(1, 2) - element(1, 1) * element(0, 2);

    // Assemble adjugate:  adj(i,j) = (-1)^(i+j) * minor(j,i)
    Matrix4x4 adj;

    adj.element(0, 0) = (element(1, 1) * a2323 - element(2, 1) * a1323 + element(3, 1) * a1223);
    adj.element(1, 0) = -(element(1, 0) * a2323 - element(2, 0) * a1323 + element(3, 0) * a1223);
    adj.element(2, 0) = (element(1, 0) * a2313 - element(2, 0) * a1313 + element(3, 0) * a1213);
    adj.element(3, 0) = -(element(1, 0) * a2312 - element(2, 0) * a1312 + element(3, 0) * a1212);

    adj.element(0, 1) = -(element(0, 1) * a2323 - element(2, 1) * a0323 + element(3, 1) * a0223);
    adj.element(1, 1) = (element(0, 0) * a2323 - element(2, 0) * a0323 + element(3, 0) * a0223);
    adj.element(2, 1) = -(element(0, 0) * a2313 - element(2, 0) * a0313 + element(3, 0) * a0213);
    adj.element(3, 1) = (element(0, 0) * a2312 - element(2, 0) * a0312 + element(3, 0) * a0212);

    adj.element(0, 2) = (element(0, 1) * a1323 - element(1, 1) * a0323 + element(3, 1) * a0123);
    adj.element(1, 2) = -(element(0, 0) * a1323 - element(1, 0) * a0323 + element(3, 0) * a0123);
    adj.element(2, 2) = (element(0, 0) * a1313 - element(1, 0) * a0313 + element(3, 0) * a0113);
    adj.element(3, 2) = -(element(0, 0) * a1312 - element(1, 0) * a0312 + element(3, 0) * a0112);

    adj.element(0, 3) = -(element(0, 1) * a1223 - element(1, 1) * a0223 + element(2, 1) * a0123);
    adj.element(1, 3) = (element(0, 0) * a1223 - element(1, 0) * a0223 + element(2, 0) * a0123);
    adj.element(2, 3) = -(element(0, 0) * a1213 - element(1, 0) * a0213 + element(2, 0) * a0113);
    adj.element(3, 3) = (element(0, 0) * a1212 - element(1, 0) * a0212 + element(2, 0) * a0112);

    const T det = element(0, 0) * adj.element(0, 0) + element(1, 0) * adj.element(0, 1) + element(2, 0) * adj.element(0, 2) + element(3, 0) * adj.element(0, 3);

    if (math::isZero(det, EPS<T>()))
        return false;

    const T inv_det = T(1) / det;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            element(i, j) = adj.element(i, j) * inv_det;
        }
    }
    return true;
}

TMPL_PREFIX bool Matrix4x4<T, Order>::isRigid(T eps) const
{
    if (!isAffine(eps))
        return false;

    const Vector3<T> x(element(0, 0), element(1, 0), element(2, 0));
    const Vector3<T> y(element(0, 1), element(1, 1), element(2, 1));
    const Vector3<T> z(element(0, 2), element(1, 2), element(2, 2));

    const auto len2_x  = x.length2();
    const auto len2_y  = y.length2();
    const auto len2_z  = z.length2();
    const auto eps_len = T(2) * eps + eps * eps;

    if (!math::isEqual(len2_x, T(1), eps_len) || !math::isEqual(len2_y, T(1), eps_len) || !math::isEqual(len2_z, T(1), eps_len))
        return false;

    if (!math::isZero(x.dot(y), eps) || !math::isZero(y.dot(z), eps) || !math::isZero(z.dot(x), eps))
        return false;

    if (!math::isEqual(x.cross(y).dot(z), T(1), eps))
        return false;

    return true;
}

#undef TMPL_PREFIX

template class VN_MATH_API Matrix4x4<float, ColMajor>;
template class VN_MATH_API Matrix4x4<double, ColMajor>;
template class VN_MATH_API Matrix4x4<float, RowMajor>;
template class VN_MATH_API Matrix4x4<double, RowMajor>;

VN_MATH_NS_END
