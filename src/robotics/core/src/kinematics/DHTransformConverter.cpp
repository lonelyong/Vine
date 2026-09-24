
#include <vine/robotics/kinematics/DHTransformConverter.hpp>

#include <cmath>
#include <optional>

#include <vine/math/Math.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Transform3.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

/* ========================================================================= */
/*  Forward transforms                                                       */
/* ========================================================================= */

VN_ROBOTICS_CORE_API math::Isometry3d mdhToTransform(const DHParameter& mdh, double dd, double dtheta)
{
    using namespace math;

    const double ca    = std::cos(mdh.alpha);
    const double sa    = std::sin(mdh.alpha);
    const double theta = mdh.theta + dtheta;
    const double d     = mdh.d + dd;
    const double ct    = std::cos(theta);
    const double st    = std::sin(theta);

    /* R = Rot_x(α) · Rot_z(θ)
     *
     *   R = | ct      -st       0    |
     *       | ca·st    ca·ct   -sa   |
     *       | sa·st    sa·ct    ca   |
     */
    const double m00 = ct;
    const double m01 = -st;
    const double m02 = 0.0;
    const double m10 = ca * st;
    const double m11 = ca * ct;
    const double m12 = -sa;
    const double m20 = sa * st;
    const double m21 = sa * ct;
    const double m22 = ca;

    double       qx, qy, qz, qw;
    const double tr = m00 + m11 + m22;

    if (tr > 0.0) {
        double s = std::sqrt(tr + 1.0) * 2.0;
        qw       = 0.25 * s;
        qx       = (m21 - m12) / s;
        qy       = (m02 - m20) / s;
        qz       = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        qw       = (m21 - m12) / s;
        qx       = 0.25 * s;
        qy       = (m01 + m10) / s;
        qz       = (m02 + m20) / s;
    }
    else if (m11 > m22) {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        qw       = (m02 - m20) / s;
        qx       = (m01 + m10) / s;
        qy       = 0.25 * s;
        qz       = (m12 + m21) / s;
    }
    else {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        qw       = (m10 - m01) / s;
        qx       = (m02 + m20) / s;
        qy       = (m12 + m21) / s;
        qz       = 0.25 * s;
    }

    /* t = Rot_x(α) · [a, 0, d]ᵀ  =  [a, -d·sinα, d·cosα]ᵀ */
    const Vec3d t(mdh.a, -d * sa, d * ca);

    return Isometry3d(Point3d(t.x, t.y, t.z), Quatd(qx, qy, qz, qw));
}

VN_ROBOTICS_CORE_API math::Isometry3d sdhToTransform(const DHParameter& sdh, double dd, double dtheta)
{
    using namespace math;

    const double ca    = std::cos(sdh.alpha);
    const double sa    = std::sin(sdh.alpha);
    const double theta = sdh.theta + dtheta;
    const double d     = sdh.d + dd;
    const double ct    = std::cos(theta);
    const double st    = std::sin(theta);

    /* R = Rot_z(θ) · Rot_x(α)
     *
     *   R = | ct   -st·ca    st·sa |
     *       | st    ct·ca   -ct·sa |
     *       | 0     sa        ca   |
     */
    const double m00 = ct;
    const double m01 = -st * ca;
    const double m02 = st * sa;
    const double m10 = st;
    const double m11 = ct * ca;
    const double m12 = -ct * sa;
    const double m20 = 0.0;
    const double m21 = sa;
    const double m22 = ca;

    double       qx, qy, qz, qw;
    const double tr = m00 + m11 + m22;

    if (tr > 0.0) {
        double s = std::sqrt(tr + 1.0) * 2.0;
        qw       = 0.25 * s;
        qx       = (m21 - m12) / s;
        qy       = (m02 - m20) / s;
        qz       = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        qw       = (m21 - m12) / s;
        qx       = 0.25 * s;
        qy       = (m01 + m10) / s;
        qz       = (m02 + m20) / s;
    }
    else if (m11 > m22) {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        qw       = (m02 - m20) / s;
        qx       = (m01 + m10) / s;
        qy       = 0.25 * s;
        qz       = (m12 + m21) / s;
    }
    else {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        qw       = (m10 - m01) / s;
        qx       = (m02 + m20) / s;
        qy       = (m12 + m21) / s;
        qz       = 0.25 * s;
    }

    /* t = Rot_z(θ) · [a, 0, d]ᵀ  =  [a·cosθ, a·sinθ, d]ᵀ */
    const Vec3d t(sdh.a * ct, sdh.a * st, d);

    return Isometry3d(Point3d(t.x, t.y, t.z), Quatd(qx, qy, qz, qw));
}

/* ========================================================================= */
/*  Representability checks                                                  */
/* ========================================================================= */

VN_ROBOTICS_CORE_API bool isMdhRepresentable(const math::Isometry3d& tf, double tolerance)
{
    const auto R = rotate3x3(tf.rotation);

    /* MDH:  R = Rot_x(α)·Rot_z(θ)  ⇒  R(0,2) = 0 */
    return std::abs(R(0, 2)) <= tolerance;
}

VN_ROBOTICS_CORE_API bool isSdhRepresentable(const math::Isometry3d& tf, double tolerance)
{
    const auto R = rotate3x3(tf.rotation);

    /* SDH:  R = Rot_z(θ)·Rot_x(α)  ⇒  R(2,0) = 0 */
    return std::abs(R(2, 0)) <= tolerance;
}

/* ========================================================================= */
/*  Inverse extraction                                                       */
/* ========================================================================= */

VN_ROBOTICS_CORE_API std::optional<DHParameter> tryMdhFromTransform(const math::Isometry3d& tf, double tolerance)
{
    using namespace math;

    const auto& q = tf.rotation;
    if (std::abs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0) > tolerance)
        return std::nullopt;

    const auto R = rotate3x3(tf.rotation);

    if (std::abs(R(0, 2)) > tolerance) /* R(0,2) must be 0 for MDH */
        return std::nullopt;

    DHParameter p;

    /* α = atan2(-R₂₃, R₃₃)   ——  column 2:  [0, -sinα, cosα]ᵀ */
    p.alpha = std::atan2(-R(1, 2), R(2, 2));

    /* θ = atan2(-R₀₁, R₀₀)   ——  row 0:    [cosθ, -sinθ, 0]    */
    p.theta = std::atan2(-R(0, 1), R(0, 0));

    /* Translation:  t = [a, -d·sinα, d·cosα]ᵀ
     *   →  a = t.x
     *   →  d = t.z·cosα - t.y·sinα */
    const double ca = std::cos(p.alpha);
    const double sa = std::sin(p.alpha);

    p.a = tf.translation.x;
    p.d = tf.translation.z * ca - tf.translation.y * sa;

    return p;
}

VN_ROBOTICS_CORE_API std::optional<DHParameter> trySdhFromTransform(const math::Isometry3d& tf, double tolerance)
{
    using namespace math;

    const auto& q = tf.rotation;
    if (std::abs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0) > tolerance)
        return std::nullopt;

    const auto R = rotate3x3(tf.rotation);

    if (std::abs(R(2, 0)) > tolerance) /* R(2,0) must be 0 for SDH */
        return std::nullopt;

    DHParameter p;

    /* α = atan2(R₃₂, R₃₃)   ——  row 2:  [0, sinα, cosα] */
    p.alpha = std::atan2(R(2, 1), R(2, 2));

    /* θ = atan2(R₁₀, R₀₀)   ——  col 0:  [cosθ, sinθ, 0]ᵀ */
    p.theta = std::atan2(R(1, 0), R(0, 0));

    /* Translation:  t = [a·cosθ, a·sinθ, d]ᵀ
     *   →  d = t.z
     *   →  a = t.x·cosθ + t.y·sinθ */
    const double ct = std::cos(p.theta);
    const double st = std::sin(p.theta);

    p.d = tf.translation.z;
    p.a = tf.translation.x * ct + tf.translation.y * st;

    return p;
}

VN_ROBOTICS_KINEMATICS_NS_END