#include <vine/math/Rect2.hpp>

#include <algorithm>
#include <cstdint>

#include <vine/math/Math.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T>

TMPL_PREFIX bool Rect2<T>::contains(T x, T y) const
{
    return x >= xmin && x <= xmax && y >= ymin && y <= ymax;
}

TMPL_PREFIX bool Rect2<T>::contains(const Point2<T>& pt) const
{
    return pt.x >= xmin && pt.x <= xmax && pt.y >= ymin && pt.y <= ymax;
}

TMPL_PREFIX void Rect2<T>::expandBy(const Point2<T>& pt)
{
    xmin = std::min<T>(xmin, pt.x);
    ymin = std::min<T>(ymin, pt.y);
    xmax = std::max<T>(xmax, pt.x);
    ymax = std::max<T>(ymax, pt.y);
}

TMPL_PREFIX void Rect2<T>::expandBy(const Rect2<T>& rect)
{
    xmin = std::min<T>(xmin, rect.xmin);
    ymin = std::min<T>(ymin, rect.ymin);
    xmax = std::max<T>(xmax, rect.xmax);
    ymax = std::max<T>(ymax, rect.ymax);
}

TMPL_PREFIX bool Rect2<T>::intersectWith(const Rect2<T>& rect, Rect2<T>& out) const
{
    auto ix0 = std::max<T>(xmin, rect.xmin);
    auto iy0 = std::max<T>(ymin, rect.ymin);
    auto ix1 = std::min<T>(xmax, rect.xmax);
    auto iy1 = std::min<T>(ymax, rect.ymax);

    out.xmin = std::min<T>(ix0, ix1);
    out.ymin = std::min<T>(iy0, iy1);
    out.xmax = std::max<T>(ix0, ix1);
    out.ymax = std::max<T>(iy0, iy1);

    return ix0 <= ix1 && iy0 <= iy1;
}

#undef TMPL_PREFIX

template class VN_MATH_API Rect2<float>;
template class VN_MATH_API Rect2<double>;
template class VN_MATH_API Rect2<int8_t>;
template class VN_MATH_API Rect2<uint8_t>;
template class VN_MATH_API Rect2<int16_t>;
template class VN_MATH_API Rect2<uint16_t>;
template class VN_MATH_API Rect2<int32_t>;
template class VN_MATH_API Rect2<uint32_t>;
template class VN_MATH_API Rect2<int64_t>;
template class VN_MATH_API Rect2<uint64_t>;

VN_MATH_NS_END
