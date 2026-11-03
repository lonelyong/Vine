#include <vine/geometry/Ray.hpp>

#include <cmath>

VN_GEOMETRY_NS_BEGIN

#define TMPL_PREFIX template <vn::math::FP T>

TMPL_PREFIX Ray<T>::Ray()
  : origin(T(0), T(0), T(0))
  , direction(T(0), T(0), T(0))
{}

TMPL_PREFIX Ray<T>::Ray(const Point& origin_point, const Vector& direction_vector)
  : origin(origin_point)
  , direction(direction_vector.length() > math::EPS<T>() ? direction_vector.normalized() : Vector(T(0), T(0), T(0)))
{}

TMPL_PREFIX typename Ray<T>::Point Ray<T>::pointAt(T t) const
{
    return origin + direction * t;
}

TMPL_PREFIX T Ray<T>::distanceToPoint(const Point& point) const
{
    // The ray stops at its origin, so a point behind it measures to the origin rather than to the infinite line.
    const Vector to_point = point - origin;
    const T      t        = to_point.dot(direction);
    if (t <= T(0)) {
        return to_point.length();
    }
    return (point - pointAt(t)).length();
}

TMPL_PREFIX bool intersectTriangle(
    const Ray<T>& ray, const vn::math::Point3<T>& a, const vn::math::Point3<T>& b, const vn::math::Point3<T>& c, RayHit<T>& hit, T eps)
{
    // Möller-Trumbore. The determinant is zero for a ray parallel to the triangle's plane and for a degenerate
    // triangle, which is the single place both cases are rejected.
    const vn::math::Vector3<T> edge_a = b - a;
    const vn::math::Vector3<T> edge_b = c - a;
    const vn::math::Vector3<T> pvec   = ray.direction.cross(edge_b);
    const T                    det    = edge_a.dot(pvec);

    // Compared against the two edge lengths, so the tolerance is an ANGLE rather than an area: a small triangle is
    // still hit, while a triangle the ray runs into edge-on is not.
    if (std::abs(det) <= eps * edge_a.length() * edge_b.length()) {
        return false;
    }

    const T                    inv_det = T(1) / det;
    const vn::math::Vector3<T> tvec    = ray.origin - a;
    const T                    u       = tvec.dot(pvec) * inv_det;
    if (u < T(0) || u > T(1)) {
        return false;
    }

    const vn::math::Vector3<T> qvec = tvec.cross(edge_a);
    const T                    v    = ray.direction.dot(qvec) * inv_det;
    if (v < T(0) || u + v > T(1)) {
        return false;
    }

    const T t = edge_b.dot(qvec) * inv_det;
    if (t < T(0)) {
        return false;
    }

    vn::math::Vector3<T> normal = edge_a.cross(edge_b).normalized();
    if (normal.dot(ray.direction) > T(0)) {
        normal = normal * T(-1);
    }

    hit.t      = t;
    hit.point  = ray.pointAt(t);
    hit.normal = normal;
    return true;
}

#undef TMPL_PREFIX

template class VN_GEOMETRY_API Ray<float>;
template class VN_GEOMETRY_API Ray<double>;

template VN_GEOMETRY_API bool intersectTriangle<float>(
    const Ray<float>&, const vn::math::Point3<float>&, const vn::math::Point3<float>&, const vn::math::Point3<float>&, RayHit<float>&, float);
template VN_GEOMETRY_API bool intersectTriangle<double>(const Ray<double>&,
                                                        const vn::math::Point3<double>&,
                                                        const vn::math::Point3<double>&,
                                                        const vn::math::Point3<double>&,
                                                        RayHit<double>&,
                                                        double);

VN_GEOMETRY_NS_END
