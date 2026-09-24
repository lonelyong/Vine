#include <vine/graphics/Ray.hpp>

VN_GRAPHICS_NS_BEGIN

double Ray::distanceToPoint(const Vec3d& point) const
{
    const Vec3d to_point = point - origin;
    const double t = to_point.dot(direction);
    if (t <= 0.0) {
        return to_point.length();
    }
    const Vec3d closest = origin + direction * t;
    return (point - closest).length();
}

VN_GRAPHICS_NS_END
