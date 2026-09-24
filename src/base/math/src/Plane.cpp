#include <vine/math/Plane.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T>

TMPL_PREFIX bool Plane<T>::intersectWith(const Line<T>& line, Point3<T>& intersection_pt, T eps) const
{
    // Plane–Line intersection.
    //
    //   Plane:  n·(P − o) = 0
    //   Line:   P = P₀ + t·d
    //
    // Substitute line into plane:
    //   n·(P₀ + t·d − o) = 0
    //   n·(P₀ − o) + n·(t·d) = 0
    //   n·(P₀ − o) + t·(n·d) = 0
    //
    // Solve for t:
    //   t·(n·d) = −n·(P₀ − o)
    //
    //         −n·(P₀ − o)
    //   t = --------------
    //            n·d
    //
    //   |n·d| ≤ eps  →  line ∥ plane, no unique intersection.

    Vector3<T> diff  = line.origin - origin;
    T          denom = normal.dot(line.direction);

    // Check if line is parallel to plane
    if (isZero(denom, eps)) {
        return false;
    }

    T numer         = -normal.dot(diff);
    T t             = numer / denom;
    intersection_pt = line.origin + line.direction * t;

    return true;
}

TMPL_PREFIX
bool Plane<T>::intersectWith(const Plane<T>& other, Line<T>& intersection_line, T eps) const
{
    // Plane–Plane intersection.
    //
    //   n₁·(Pₓ − o₁) = 0   →   n₁·Pₓ = d₁   (d₁ = n₁·o₁)
    //   n₂·(Pₓ − o₂) = 0   →   n₂·Pₓ = d₂   (d₂ = n₂·o₂)
    //
    // Intersection line:
    //   direction = n₁ × n₂  (⟂ both normals)
    //   point:     solve the 2×2 subsystem on the plane where the axis with
    //              the largest |dir| component is fixed to zero.

    Vector3<T> dir = normal.cross(other.normal);

    // Parallel or coincident?  n₁ × n₂ = 0
    if (isZero(dir.length(), eps)) {
        return false;
    }

    // Pick the axis with the largest |dir| component to fix at 0.
    // This keeps the remaining 2×2 submatrix well-conditioned.
    int fix_axis = 0;
    T   abs_x    = std::abs(dir.x);
    T   abs_y    = std::abs(dir.y);
    T   abs_z    = std::abs(dir.z);

    if (abs_y > abs_x && abs_y > abs_z) {
        fix_axis = 1;
    }
    else if (abs_z > abs_x && abs_z > abs_y) {
        fix_axis = 2;
    }

    // Plane constants:  d₁ = n₁·o₁,  d₂ = n₂·o₂
    T d1 = normal.dot(origin.asVector());
    T d2 = other.normal.dot(other.origin.asVector());

    // Solve 2×2 linear system via Cramer's rule:
    //   [ a  b ] [ u ]   [ e ]            e = n₁·o₁
    //                  =            where
    //   [ c  d ] [ v ]   [ f ]            f = n₂·o₂
    //
    //   det = ad − bc
    //        | e  b |
    //        | f  d |    ed − bf
    //   u = --------- = --------
    //           det        det
    //
    //        | a  e |
    //        | c  f |    af − ec
    //   v = --------- = --------
    //           det        det
    auto solve2x2 = [eps](T a, T b, T c, T d, T e, T f, T& u, T& v) -> bool {
        T det = a * d - b * c;
        if (isZero(det, eps)) {
            return false;
        }
        u = (e * d - b * f) / det;
        v = (a * f - c * e) / det;
        return true;
    };

    Point3<T> pt;

    if (fix_axis == 0) {
        // Fix x = 0, solve for (y, z)
        pt.x = T(0);
        if (!solve2x2(normal.y, normal.z, other.normal.y, other.normal.z, d1, d2, pt.y, pt.z)) {
            // Fallback: fix y = 0, solve for (x, z)
            pt.y = T(0);
            solve2x2(normal.x, normal.z, other.normal.x, other.normal.z, d1, d2, pt.x, pt.z);
        }
    }
    else if (fix_axis == 1) {
        // Fix y = 0, solve for (x, z)
        pt.y = T(0);
        if (!solve2x2(normal.x, normal.z, other.normal.x, other.normal.z, d1, d2, pt.x, pt.z)) {
            // Fallback: fix z = 0, solve for (x, y)
            pt.z = T(0);
            solve2x2(normal.x, normal.y, other.normal.x, other.normal.y, d1, d2, pt.x, pt.y);
        }
    }
    else {
        // Fix z = 0, solve for (x, y)
        pt.z = T(0);
        if (!solve2x2(normal.x, normal.y, other.normal.x, other.normal.y, d1, d2, pt.x, pt.y)) {
            // Fallback: fix x = 0, solve for (y, z)
            pt.x = T(0);
            solve2x2(normal.y, normal.z, other.normal.y, other.normal.z, d1, d2, pt.y, pt.z);
        }
    }

    intersection_line.origin    = pt;
    intersection_line.direction = dir;

    return true;
}

template class VN_MATH_API Plane<float>;
template class VN_MATH_API Plane<double>;
VN_MATH_NS_END
