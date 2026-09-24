#include <vine/math/Transform3.hpp>

#include <cmath>

#include <vine/math/Math.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <typename T, typename Order>

TMPL_PREFIX Matrix4x4<T, Order> rotate(const Vector3<T>& axis, T angle)
{
    // Axis-angle → rotation matrix (Rodrigues' formula):
    //   For unit axis k=(x,y,z) and angle θ, decompose v into parallel
    //   and perpendicular parts:  v' = v∥ + cos(θ)·v⊥ + sin(θ)·(k × v)
    //   Matrix form:  R = I + sin(θ)·[k]× + (1-cos(θ))·[k]×²
    //   e.g. R(0,0) = (1-c)x² + c,  R(1,0) = (1-c)xy + sin(θ)z
    //   Zero angle or zero-length axis → identity.
    Matrix4x4<T, Order> m;
    // Zero rotation or degenerate axis → identity.
    if (angle == T(0) || math::isZero(axis.length2(), EPS<T>())) {
        m.makeIdentity();
        return m;
    }

    auto naxis = axis;
    naxis.normalize();

    const auto c  = std::cos(angle); // cos(θ)
    const auto ic = T(1) - c;        // 1 - cos(θ)
    const auto s  = std::sin(angle); // sin(θ)
    const auto x  = naxis.x;
    const auto y  = naxis.y;
    const auto z  = naxis.z;

    // R = I + sin(θ)·[k]× + (1-cos(θ))·[k]×²
    // Column 0 = image of X-axis (1,0,0) under rotation
    m(0, 0) = x * x * ic + c;     // = 1 + (1-c)(x²-1)
    m(1, 0) = x * y * ic + z * s; // = (1-c)xy + sin(θ)z
    m(2, 0) = x * z * ic - y * s; // = (1-c)xz - sin(θ)y
    m(3, 0) = T(0);
    // Column 1 = image of Y-axis (0,1,0) under rotation
    m(0, 1) = x * y * ic - z * s; // = (1-c)xy - sin(θ)z
    m(1, 1) = y * y * ic + c;     // = 1 + (1-c)(y²-1)
    m(2, 1) = y * z * ic + x * s; // = (1-c)yz + sin(θ)x
    m(3, 1) = T(0);
    // Column 2 = image of Z-axis (0,0,1) under rotation
    m(0, 2) = x * z * ic + y * s; // = (1-c)xz + sin(θ)y
    m(1, 2) = y * z * ic - x * s; // = (1-c)yz - sin(θ)x
    m(2, 2) = z * z * ic + c;     // = 1 + (1-c)(z²-1)
    m(3, 2) = T(0);
    // Column 3 — pure rotation, no translation
    m(0, 3) = T(0);
    m(1, 3) = T(0);
    m(2, 3) = T(0);
    m(3, 3) = T(1);

    return m;
}

TMPL_PREFIX Matrix4x4<T, Order> lookAt(const Point3<T>& eye, const Point3<T>& target, const Vector3<T>& up)
{
    Matrix4x4<T, Order> m;
    // Camera backward direction (+Z)
    Vector3<T> f = eye - target;

    // Foward is invalid
    if (math::isZero(f.normalize(), EPS<T>())) {
        return {};
    }

    // Camera right direction (+X)
    Vector3<T> s = up.cross(f);

    // up is parallel to forward
    if (math::isZero(s.length2(), EPS<T>())) {

        // Pick the axis least parallel to f
        Vector3<T> ref;

        const auto ax = std::abs(f.x);
        const auto ay = std::abs(f.y);
        const auto az = std::abs(f.z);

        if (ax <= ay && ax <= az) {
            ref = Vector3<T>(T(1), T(0), T(0));
        }
        else if (ay <= az) {
            ref = Vector3<T>(T(0), T(1), T(0));
        }
        else {
            ref = Vector3<T>(T(0), T(0), T(1));
        }

        s = ref.cross(f);
    }

    s.normalize();

    // True camera up direction (+Y)
    Vector3<T> u = f.cross(s);
    u.normalize();

    m = {};

    // Rotation part
    m(0, 0) = s.x;
    m(0, 1) = s.y;
    m(0, 2) = s.z;

    m(1, 0) = u.x;
    m(1, 1) = u.y;
    m(1, 2) = u.z;

    m(2, 0) = f.x;
    m(2, 1) = f.y;
    m(2, 2) = f.z;

    // Translation part
    m(0, 3) = -s.dot(Vector3<T>(eye.x, eye.y, eye.z));
    m(1, 3) = -u.dot(Vector3<T>(eye.x, eye.y, eye.z));
    m(2, 3) = -f.dot(Vector3<T>(eye.x, eye.y, eye.z));

    return m;
}

TMPL_PREFIX Matrix4x4<T, Order> reflect(const Vector3<T>& plane_normal, T plane_offset)
{
    // Householder reflection across plane n·p + d = 0:
    //   p' = p - 2(n·p + d)n
    //   Linear part:  H₃ = I - 2nnᵀ  (det = -1, involution: H₃·H₃ = I)
    //   Translation:  t = -2dn
    //   Full 4×4:
    //       | 1-2a²  -2ab   -2ac  -2ad |
    //   H = | -2ab  1-2b²  -2bc  -2bd |
    //       | -2ac  -2bc  1-2c² -2cd |
    //       |   0      0     0     1  |
    Matrix4x4<T, Order> m;
    Vector3<T>          n = plane_normal;
    n.normalize();

    const T a = n.x, b = n.y, c = n.z, d = plane_offset;

    // H₃ = I - 2nnᵀ  (upper-left 3×3, applied column by column)
    // Column 0 = H₃ · (1,0,0)ᵀ = e₀ - 2a·n
    m(0, 0) = T(1) - T(2) * a * a;
    m(1, 0) = T(-2) * a * b;
    m(2, 0) = T(-2) * a * c;
    m(3, 0) = T(0);
    // Column 1 = H₃ · (0,1,0)ᵀ = e₁ - 2b·n
    m(0, 1) = T(-2) * a * b;
    m(1, 1) = T(1) - T(2) * b * b;
    m(2, 1) = T(-2) * b * c;
    m(3, 1) = T(0);
    // Column 2 = H₃ · (0,0,1)ᵀ = e₂ - 2c·n
    m(0, 2) = T(-2) * a * c;
    m(1, 2) = T(-2) * b * c;
    m(2, 2) = T(1) - T(2) * c * c;
    m(3, 2) = T(0);
    // Column 3 = translation: t = -2d·n
    m(0, 3) = T(-2) * a * d;
    m(1, 3) = T(-2) * b * d;
    m(2, 3) = T(-2) * c * d;
    m(3, 3) = T(1);

    return m;
}

TMPL_PREFIX bool decompose(const Matrix4x4<T, Order>& m, Vector3<T>& t, Quaternion<T>& r, Vector3<T>& s)
{
    using namespace math;

    // ---- translation --------------------------------------------------------
    // Works for any affine matrix: translation, TRS, reflection, projection.
    // For pure rotation / pure scale this simply reads (0,0,0).
    t.x = m(0, 3);
    t.y = m(1, 3);
    t.z = m(2, 3);

    // ---- reject non-affine --------------------------------------------------
    // Projection / perspective matrices do not have the [0 0 0 1] bottom row
    // and cannot be decomposed into TRS.
    if (!m.isAffine(EPS<T>()))
        return false;

    // ---- scale --------------------------------------------------------------
    // Column vectors of the 3×3 upper-left block.
    //   Pure rotation:              columns are orthonormal → scale = (1,1,1)
    //   Pure scale:                 columns are axis-aligned → scale = diag
    //   Rotation × Scale (R·S):     column lengths are the true scale factors
    //   Reflection (axis-aligned):  column lengths stay 1, the sign is recovered
    //                               later via the handedness check
    const Vector3<T> cx(m(0, 0), m(1, 0), m(2, 0));
    const Vector3<T> cy(m(0, 1), m(1, 1), m(2, 1));
    const Vector3<T> cz(m(0, 2), m(1, 2), m(2, 2));

    const T col0_len = std::sqrt(cx.length2());
    const T col1_len = std::sqrt(cy.length2());
    const T col2_len = std::sqrt(cz.length2());

    s.x = col0_len;
    s.y = col1_len;
    s.z = col2_len;

    // ---- reject degenerate --------------------------------------------------
    // A zero-length column means the matrix collapses a dimension (e.g. zero
    // scale on an axis).  No meaningful decomposition possible.
    if (math::isZero(col0_len, EPS<T>()) || math::isZero(col1_len, EPS<T>()) || math::isZero(col2_len, EPS<T>()))
        return false;

    // ---- normalize columns → orthonormal R ----------------------------------
    // Dividing each column by its length factors out scale, leaving the
    // rotation part R.  For pure rotation or reflection this is a no-op
    // (lengths are already 1).
    const T inv0 = T(1) / col0_len;
    const T inv1 = T(1) / col1_len;
    const T inv2 = T(1) / col2_len;

    T m00 = cx.x * inv0, m10 = cx.y * inv0, m20 = cx.z * inv0;
    T m01 = cy.x * inv1, m11 = cy.y * inv1, m21 = cy.z * inv1;
    T m02 = cz.x * inv2, m12 = cz.y * inv2, m22 = cz.z * inv2;

    // ---- reject shear -------------------------------------------------------
    // After removing scale, columns must be mutually orthogonal.  If not, the
    // matrix contains shear and cannot be expressed as R·S.
    if (!math::isZero(m00 * m01 + m10 * m11 + m20 * m21, EPS<T>()) || !math::isZero(m00 * m02 + m10 * m12 + m20 * m22, EPS<T>()) ||
        !math::isZero(m01 * m02 + m11 * m12 + m21 * m22, EPS<T>()))
    {
        r = Quaternion<T>(T(0), T(0), T(0), T(1));
        return false;
    }

    // ---- handle reflection --------------------------------------------------
    // A pure reflection (e.g. scale(-1, 1, 1)) yields an orthonormal matrix
    // with determinant −1.  The trace method below assumes a right-handed
    // rotation (det = +1).  We move the minus sign into s.x and flip the
    // first column so that the remaining matrix is a proper rotation.
    //   Reflection:          det < 0 → flip sign,    s contains the minus
    //   Rotation × Reflection:  same handling; handedness check catches it
    if (!math::isEqual(m00 * (m11 * m22 - m21 * m12) - m10 * (m01 * m22 - m21 * m02) + m20 * (m01 * m12 - m11 * m02), T(1), EPS<T>())) {
        m00 = -m00;
        m10 = -m10;
        m20 = -m20;
        s.x = -s.x;
    }

    // ---- rotation: orthonormal matrix → quaternion (trace method) ----------
    //   trace(R) = 3 - 4(x² + y² + z²) = 4w² - 1  →  w = ½√(1 + trace)
    //   Similarly, x = ½√(1 + m00 - m11 - m22), etc.
    //   The largest component is computed first to avoid division by a small
    //   number; the other three are derived from off-diagonal elements.
    const auto trace = m00 + m11 + m22;

    Quaternion<T> quat;
    if (trace > T(0)) {
        // w is the largest component; compute w first.
        const auto s = T(2) * std::sqrt(trace + T(1)); // s = 4w
        quat.w       = T(0.25) * s;                    // w = s/4
        quat.x       = (m21 - m12) / s;                // x = (m21-m12)/(4w)
        quat.y       = (m02 - m20) / s;
        quat.z       = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22) {
        // x is the largest component; compute x first.
        const auto s = T(2) * std::sqrt(T(1) + m00 - m11 - m22); // s = 4x
        quat.w       = (m21 - m12) / s;
        quat.x       = T(0.25) * s;
        quat.y       = (m01 + m10) / s; // from m01+m10 = 4xy
        quat.z       = (m02 + m20) / s;
    }
    else if (m11 > m22) {
        // y is the largest component; compute y first.
        const auto s = T(2) * std::sqrt(T(1) + m11 - m00 - m22); // s = 4y
        quat.w       = (m02 - m20) / s;
        quat.x       = (m01 + m10) / s;
        quat.y       = T(0.25) * s;
        quat.z       = (m12 + m21) / s;
    }
    else {
        // z is the largest component; compute z first.
        const auto s = T(2) * std::sqrt(T(1) + m22 - m00 - m11); // s = 4z
        quat.w       = (m10 - m01) / s;
        quat.x       = (m02 + m20) / s;
        quat.y       = (m12 + m21) / s;
        quat.z       = T(0.25) * s;
    }

    // Step 4: Normalize the quaternion to compensate for any accumulated
    //         floating-point drift in the matrix.
    const auto q_len2 = quat.x * quat.x + quat.y * quat.y + quat.z * quat.z + quat.w * quat.w;
    if (math::isZero(q_len2, EPS<T>())) {
        r = Quaternion<T>(T(0), T(0), T(0), T(1));
        return false;
    }

    const auto inv_len = T(1) / std::sqrt(q_len2);
    quat.x *= inv_len;
    quat.y *= inv_len;
    quat.z *= inv_len;
    quat.w *= inv_len;
    r = quat;
    return true;
}

#undef TMPL_PREFIX

template VN_MATH_API bool decompose(const Matrix4x4<float, ColMajor>&, Vector3<float>&, Quaternion<float>&, Vector3<float>&);
template VN_MATH_API bool decompose(const Matrix4x4<double, ColMajor>&, Vector3<double>&, Quaternion<double>&, Vector3<double>&);
template VN_MATH_API bool decompose(const Matrix4x4<float, RowMajor>&, Vector3<float>&, Quaternion<float>&, Vector3<float>&);
template VN_MATH_API bool decompose(const Matrix4x4<double, RowMajor>&, Vector3<double>&, Quaternion<double>&, Vector3<double>&);

template VN_MATH_API Matrix4x4<float, ColMajor>  rotate(const Vector3<float>&, float);
template VN_MATH_API Matrix4x4<double, ColMajor> rotate(const Vector3<double>&, double);
template VN_MATH_API Matrix4x4<float, RowMajor>  rotate(const Vector3<float>&, float);
template VN_MATH_API Matrix4x4<double, RowMajor> rotate(const Vector3<double>&, double);

template VN_MATH_API Matrix4x4<float, ColMajor>  lookAt(const Point3<float>&, const Point3<float>&, const Vector3<float>&);
template VN_MATH_API Matrix4x4<double, ColMajor> lookAt(const Point3<double>&, const Point3<double>&, const Vector3<double>&);
template VN_MATH_API Matrix4x4<float, RowMajor>  lookAt(const Point3<float>&, const Point3<float>&, const Vector3<float>&);
template VN_MATH_API Matrix4x4<double, RowMajor> lookAt(const Point3<double>&, const Point3<double>&, const Vector3<double>&);

template VN_MATH_API Matrix4x4<float, ColMajor>  reflect(const Vector3<float>&, float);
template VN_MATH_API Matrix4x4<double, ColMajor> reflect(const Vector3<double>&, double);
template VN_MATH_API Matrix4x4<float, RowMajor>  reflect(const Vector3<float>&, float);
template VN_MATH_API Matrix4x4<double, RowMajor> reflect(const Vector3<double>&, double);

VN_MATH_NS_END
