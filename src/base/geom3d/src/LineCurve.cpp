#include <vine/geom3d/LineCurve.hpp>

#include <cmath>

#include <vine/math/Math.hpp>

VN_GEOM3D_NS_BEGIN

#define TMPL_PREFIX template <vn::math::FP T>

TMPL_PREFIX LineCurve<T>::Intersection LineCurve<T>::intersectWith(const LineCurve<T>& other, T tol) const
{
    Intersection is;

    // A zero direction leaves a point, not a line: there is no closest-point pair to solve for, and dividing by its
    // length would only produce a NaN.
    if (!isValid() || !other.isValid()) {
        return is;
    }

    const math::Point3<T>&  o1 = origin;
    const math::Vector3<T>& d1 = direction;
    const math::Point3<T>&  o2 = other.origin;
    const math::Vector3<T>& d2 = other.direction;

    // Vector connecting the two origins.
    const math::Vector3<T> r = o2 - o1;
    // Direction cross product — zero when d1 ∥ d2.
    const math::Vector3<T> c = d1.cross(d2);

    // Parallel?
    if (c.length2() < tol * tol) {
        is.is_parallel = true;

        // Collinear?  |r × d1|² = 0  means r is parallel to d1 as well.
        if (r.cross(d1).length2() < tol * tol) {
            is.is_intersected = true;
            is.is_collinear   = true;
        }
        // Parallel but not collinear — no further flags.
        else {}

        // Anchor at L1's origin o1, project it onto L2:
        // (o1 − (o2 + t2·d2)) · d2 = 0  →  t2 = (o1−o2)·d2 / |d2|² = −r·d2 / |d2|²
        const auto t2 = r.dot(d2) / d2.dot(d2);

        is.params.t1       = 0;
        is.params.t2       = t2;
        is.params.pt1      = origin;
        is.params.pt2      = o2 + d2 * t2;
        is.params.distance = (is.params.pt2 - is.params.pt1).length();

        return is;
    }

    // Non-parallel
    // Solve for closest-point parameters
    const double d1d1 = d1.dot(d1);
    const double d2d2 = d2.dot(d2);
    const double d1d2 = d1.dot(d2);
    const double rd1  = r.dot(d1);
    const double rd2  = r.dot(d2);

    // Derivation of |d1 × d2|² via Lagrange's identity:
    //
    //   |d1 × d2|  = |d1|·|d2|·sinθ
    //   |d1 × d2|² = |d1|²·|d2|²·sin²θ
    //              = |d1|²·|d2|²·(1 − cos²θ)
    //
    //   cosθ  = (d1·d2) / (|d1|·|d2|)
    //   cos²θ = (d1·d2)² / (|d1|²·|d2|²)
    //
    //   ∴ |d1 × d2|² = |d1|²·|d2|² − (d1·d2)²
    const double denom = d1d1 * d2d2 - d1d2 * d1d2;

    // f(t1) = o1 + d1 * t1
    // f(t2) = o2 + d2 * t2
    // The closest-point connecting line is ⟂ d₁ and ⟂ d₂, simultaneously:
    //   (1) |d₁|²t₁ − (d₁·d₂)t₂ = r·d₁
    //   (2) (d₁·d₂)t₁ − |d₂|²t₂ = r·d₂
    //
    // Eliminate t₂: (1)×|d₂|² − (2)×(d₁·d₂) → t₁ = ((r·d₁)|d₂|² − (r·d₂)(d₁·d₂)) / denom
    // Eliminate t₁: (1)×(d₁·d₂) − (2)×|d₁|² → t₂ = ((r·d₁)(d₁·d₂) − (r·d₂)|d₁|²) / denom

    is.params.t1 = (rd1 * d2d2 - rd2 * d1d2) / denom;
    is.params.t2 = (rd1 * d1d2 - rd2 * d1d1) / denom;

    is.params.pt1      = o1 + d1 * is.params.t1;
    is.params.pt2      = o2 + d2 * is.params.t2;
    is.params.distance = (is.params.pt1 - is.params.pt2).length();

    // Check if closest points coincide
    if (is.params.distance < tol) {
        is.is_intersected = true;
    }
    // else
    //{
    //     is.params.t1 = 0;
    //     is.params.t2 = 0;
    //     is.params.pt1 = {};
    //     is.params.pt2 = {};
    // }

    return is;
}

template class VN_GEOM3D_API LineCurve<float>;
template class VN_GEOM3D_API LineCurve<double>;

VN_GEOM3D_NS_END
