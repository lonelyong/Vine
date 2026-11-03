#include <vine/geometry/Aabb.hpp>

#include <algorithm>
#include <cstdint>

#include <vine/math/Math.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Vector3.hpp>

VN_GEOMETRY_NS_BEGIN

#define TMPL_PREFIX template <typename T>

TMPL_PREFIX bool Aabb<T>::contains(T x, T y, T z) const
{
    return x >= xmin && x <= xmax && y >= ymin && y <= ymax && z >= zmin && z <= zmax;
}

TMPL_PREFIX bool Aabb<T>::contains(const math::Point3<T>& pt) const
{
    return pt.x >= xmin && pt.x <= xmax && pt.y >= ymin && pt.y <= ymax && pt.z >= zmin && pt.z <= zmax;
}

TMPL_PREFIX void Aabb<T>::expandBy(const math::Point3<T>& pt)
{
    xmin = std::min<T>(xmin, pt.x);
    ymin = std::min<T>(ymin, pt.y);
    zmin = std::min<T>(zmin, pt.z);
    xmax = std::max<T>(xmax, pt.x);
    ymax = std::max<T>(ymax, pt.y);
    zmax = std::max<T>(zmax, pt.z);
}

TMPL_PREFIX void Aabb<T>::expandBy(const Aabb<T>& rect)
{
    xmin = std::min<T>(xmin, rect.xmin);
    ymin = std::min<T>(ymin, rect.ymin);
    zmin = std::min<T>(zmin, rect.zmin);
    xmax = std::max<T>(xmax, rect.xmax);
    ymax = std::max<T>(ymax, rect.ymax);
    zmax = std::max<T>(zmax, rect.zmax);
}

TMPL_PREFIX bool Aabb<T>::intersectWith(const Aabb<T>& rect, Aabb<T>& out) const
{
    auto ix0 = std::max<T>(xmin, rect.xmin);
    auto iy0 = std::max<T>(ymin, rect.ymin);
    auto iz0 = std::max<T>(zmin, rect.zmin);
    auto ix1 = std::min<T>(xmax, rect.xmax);
    auto iy1 = std::min<T>(ymax, rect.ymax);
    auto iz1 = std::min<T>(zmax, rect.zmax);

    out.xmin = std::min<T>(ix0, ix1);
    out.ymin = std::min<T>(iy0, iy1);
    out.zmin = std::min<T>(iz0, iz1);
    out.xmax = std::max<T>(ix0, ix1);
    out.ymax = std::max<T>(iy0, iy1);
    out.zmax = std::max<T>(iz0, iz1);

    return ix0 <= ix1 && iy0 <= iy1 && iz0 <= iz1;
}

TMPL_PREFIX Aabb<T> Aabb<T>::compute(std::span<const math::Vector3<T>> points)
{
    if (points.empty()) {
        return Aabb<T>();
    }
    Aabb<T> box;
    box.xmin = box.xmax = points.front().x;
    box.ymin = box.ymax = points.front().y;
    box.zmin = box.zmax = points.front().z;
    for (const auto& p : points) {
        box.xmin = std::min<T>(box.xmin, p.x);
        box.ymin = std::min<T>(box.ymin, p.y);
        box.zmin = std::min<T>(box.zmin, p.z);
        box.xmax = std::max<T>(box.xmax, p.x);
        box.ymax = std::max<T>(box.ymax, p.y);
        box.zmax = std::max<T>(box.zmax, p.z);
    }
    return box;
}

#undef TMPL_PREFIX

template class VN_GEOMETRY_API Aabb<float>;
template class VN_GEOMETRY_API Aabb<double>;
template class VN_GEOMETRY_API Aabb<int8_t>;
template class VN_GEOMETRY_API Aabb<uint8_t>;
template class VN_GEOMETRY_API Aabb<int16_t>;
template class VN_GEOMETRY_API Aabb<uint16_t>;
template class VN_GEOMETRY_API Aabb<int32_t>;
template class VN_GEOMETRY_API Aabb<uint32_t>;
template class VN_GEOMETRY_API Aabb<int64_t>;
template class VN_GEOMETRY_API Aabb<uint64_t>;

VN_GEOMETRY_NS_END
