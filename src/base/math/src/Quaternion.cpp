#include <vine/math/Quaternion.hpp>

#include <vine/math/Point3.hpp>

VN_MATH_NS_BEGIN

#define TMPL_PREFIX template <FP T>

TMPL_PREFIX void Quaternion<T>::makeRotate(T angle, const Vector3<T>& axis)
{
    // q = axis * sin(angle/2), cos(angle/2)
    if (angle) {
        auto len = axis.length();

        // nan > 0 => false
        if (len > 0) {
            auto len_rcp = T(1) / len;

            auto s    = sin(angle * 0.5);
            auto c    = cos(angle * 0.5);
            auto temp = s * len_rcp;
            x         = axis.x * temp;
            y         = axis.y * temp;
            z         = axis.z * temp;
            w         = c;
            return;
        }
    }

    x = T(0);
    y = T(0);
    z = T(0);
    w = T(1);
}

TMPL_PREFIX void Quaternion<T>::makeRotate(const Vector3<T>& from, const Vector3<T>& to)
{
    /**
     * Create a quaternion representing rotation from vector 'from' to vector 'to'
     *
     * Algorithm:
     *   1. Normalize both vectors
     *   2. Compute rotation axis: axis = from × to
     *   3. Compute rotation angle using: cos(θ) = from · to
     *
     * Efficient construction without explicit angle calculation:
     *   q.w = sqrt(|from|² * |to|²) + (from · to)
     *   q.v = from × to
     *   then normalize
     *
     * This avoids sqrt and trig functions in the common case.
     *
     * Special cases:
     *   - Parallel vectors (same direction): identity quaternion
     *   - Anti-parallel vectors (opposite direction): 180° rotation around any perpendicular axis
     */

    auto from_len2 = from.length2();
    auto to_len2   = to.length2();

    // Handle zero-length vectors
    if (from_len2 == T(0) || to_len2 == T(0)) {
        // Identity quaternion
        x = T(0);
        y = T(0);
        z = T(0);
        w = T(1);
        return;
    }

    auto cross = from.cross(to);
    auto dot   = from.dot(to);

    // Check if vectors are parallel (cross product near zero)
    auto cross_len2 = cross.length2();

    // |from x to|^2 = |from|^2 * |to|^2 * sin^2(theta)
    // So this checks sin^2(theta) < 1e-10, i.e. vectors are nearly parallel
    // (theta close to 0 or pi). The right side scales with vector magnitudes.

    if (cross_len2 < EPS<T>() * from_len2 * to_len2) {
        // Vectors are nearly parallel
        if (dot > T(0)) {
            // Same direction: identity quaternion
            x = T(0);
            y = T(0);
            z = T(0);
            w = T(1);
        }
        else {
            // Opposite direction: 180° rotation
            // Find a perpendicular axis
            Vector3<T> axis;
            if (std::abs(from.x) < std::abs(from.y)) {
                if (std::abs(from.x) < std::abs(from.z)) {
                    // x is smallest, use (1,0,0)
                    axis = Vector3<T>(T(0), from.z, -from.y);
                }
                else {
                    // z is smallest, use (0,0,1)
                    axis = Vector3<T>(from.y, -from.x, T(0));
                }
            }
            else {
                if (std::abs(from.y) < std::abs(from.z)) {
                    // y is smallest, use (0,1,0)
                    axis = Vector3<T>(-from.z, T(0), from.x);
                }
                else {
                    // z is smallest, use (0,0,1)
                    axis = Vector3<T>(from.y, -from.x, T(0));
                }
            }

            auto axis_len = axis.length();
            if (axis_len > T(0)) {
                auto axis_rcp = T(1) / axis_len;
                x             = axis.x * axis_rcp;
                y             = axis.y * axis_rcp;
                z             = axis.z * axis_rcp;
                w             = T(0); // cos(180°/2) = cos(90°) = 0
            }
            else {
                // Fallback to identity
                x = T(0);
                y = T(0);
                z = T(0);
                w = T(1);
            }
        }
        return;
    }

    // General case: construct quaternion efficiently
    // q.w = sqrt(|from|² * |to|²) + (from · to)
    // q.v = from × to
    // then normalize

    w = std::sqrt(from_len2 * to_len2) + dot;
    x = cross.x;
    y = cross.y;
    z = cross.z;

    // Normalize the quaternion
    auto len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > T(0)) {
        auto len_rcp = T(1) / len;
        x *= len_rcp;
        y *= len_rcp;
        z *= len_rcp;
        w *= len_rcp;
    }
    else {
        // Fallback to identity
        x = T(0);
        y = T(0);
        z = T(0);
        w = T(1);
    }
}

TMPL_PREFIX void Quaternion<T>::getRotate(T& o_angle, Vector3<T>& o_axis) const
{
    // sin^2(θ/2) + cos^2(θ/2) = 1
    // w = cos(θ/2)
    // x^2 + y^2 + z^2 + w^2 = 1
    // x^2 + y^2 + z^2 = sin^2(θ/2)

    // 2 * acos(w)，θ趋向于0时，误差变大
    // 2 * atan2(sin(θ/2), w)更稳定

    // sin(θ/2)
    auto s  = sqrt(x * x + y * y + z * z);
    o_angle = T(2) * atan2(s, w);

    if (s) {
        o_axis.x = x;
        o_axis.y = y;
        o_axis.z = z;
        o_axis /= s;
    }
    else {
        o_axis.x = T(0);
        o_axis.y = T(0);
        o_axis.z = T(0);
    }
}

TMPL_PREFIX Quaternion<T> Quaternion<T>::slerp(const Quaternion<T>& from, const Quaternion<T>& to, T t)
{
    /**
     * Spherical Linear Interpolation (SLERP) between two quaternions
     *
     * Algorithm:
     *   slerp(q1, q2, t) = (sin((1-t)θ) / sinθ) * q1 + (sin(tθ) / sinθ) * q2
     *   where θ = acos(q1 · q2) is the angle between quaternions
     *
     * Properties:
     *   - Produces constant angular velocity interpolation
     *   - Result is always on the unit sphere (if inputs are unit quaternions)
     *   - Always takes the shortest path between rotations
     *
     * Special cases:
     *   - t = 0: returns from
     *   - t = 1: returns to
     *   - θ near 0 (quaternions very close): use linear interpolation to avoid division by small sinθ
     *   - dot < 0: negate one quaternion to take shorter path
     *
     * Performance note:
     *   For many sequential interpolations, consider using normalized lerp (nlerp) as approximation
     */

    Quaternion<T> result;

    // Compute dot product (cosine of angle between quaternions)
    T dot = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;

    // If dot product is negative, negate one quaternion to take the shorter path
    // This is because q and -q represent the same rotation, but we want the shorter arc
    Quaternion<T> to_adjusted = to;
    if (dot < T(0)) {
        to_adjusted.x = -to.x;
        to_adjusted.y = -to.y;
        to_adjusted.z = -to.z;
        to_adjusted.w = -to.w;
        dot           = -dot;
    }

    // Clamp dot product to avoid numerical issues with acos
    if (dot > T(1)) {
        dot = T(1);
    }

    // Threshold for switching to linear interpolation
    // When quaternions are very close, sin(theta) becomes very small
    constexpr T SLERP_THRESHOLD = T(0.9995);

    if (dot > SLERP_THRESHOLD) {
        // Quaternions are very close, use linear interpolation (LERP)
        // to avoid division by near-zero sin(theta)
        result.x = from.x + t * (to_adjusted.x - from.x);
        result.y = from.y + t * (to_adjusted.y - from.y);
        result.z = from.z + t * (to_adjusted.z - from.z);
        result.w = from.w + t * (to_adjusted.w - from.w);

        // Normalize the result
        T len = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
        if (len > T(0)) {
            T len_rcp = T(1) / len;
            result.x *= len_rcp;
            result.y *= len_rcp;
            result.z *= len_rcp;
            result.w *= len_rcp;
        }
        else {
            // Fallback to 'from'
            result = from;
        }
    }
    else {
        // Standard SLERP
        T theta     = std::acos(dot);  // Angle between quaternions
        T sin_theta = std::sin(theta); // sin(theta)

        T weight_from = std::sin((T(1) - t) * theta) / sin_theta;
        T weight_to   = std::sin(t * theta) / sin_theta;

        result.x = weight_from * from.x + weight_to * to_adjusted.x;
        result.y = weight_from * from.y + weight_to * to_adjusted.y;
        result.z = weight_from * from.z + weight_to * to_adjusted.z;
        result.w = weight_from * from.w + weight_to * to_adjusted.w;
    }

    return result;
}

TMPL_PREFIX Quaternion<T> Quaternion<T>::operator*(const Quaternion& right) const
{
    /**
     * Hamilton quaternion multiplication: q1 * q2
     *
     * Quaternion representation: q = xi + yj + zk + w
     * where i, j, k are imaginary units satisfying:
     *   i² = j² = k² = ijk = -1
     *   ij = k,  jk = i,  ki = j
     *   ji = -k, kj = -i, ik = -j  (non-commutative)
     *
     * Alternative form: q = (v, w) where v = (x, y, z) is the vector part
     *
     * Multiplication formula:
     *   q1 * q2 = (w1*w2 - v1·v2,  w1*v2 + w2*v1 + v1×v2)
     *
     * Expanded component-wise:
     *   w = w1*w2 - x1*x2 - y1*y2 - z1*z2    (scalar part: w1*w2 - dot product)
     *   x = w1*x2 + x1*w2 + y1*z2 - z1*y2    (vector part: cross product + scalar terms)
     *   y = w1*y2 - x1*z2 + y1*w2 + z1*x2
     *   z = w1*z2 + x1*y2 - y1*x2 + z1*w2
     *
     * Physical meaning: Composing rotations
     *   If q1 and q2 represent rotations, q1*q2 applies rotation q2 first, then q1
     *   (Note: order matters due to non-commutativity)
     *
     * Verification examples:
     *   i * j = k,  j * k = i,  k * i = j
     *   i * i = -1, j * j = -1, k * k = -1
     */
    Quaternion<T> q;
    q.w = w * right.w - x * right.x - y * right.y - z * right.z;
    q.x = w * right.x + x * right.w + y * right.z - z * right.y;
    q.y = w * right.y - x * right.z + y * right.w + z * right.x;
    q.z = w * right.z + x * right.y - y * right.x + z * right.w;
    return q;
}

TMPL_PREFIX Quaternion<T>& Quaternion<T>::operator*=(const Quaternion& right)
{
    auto w2 = w * right.w - x * right.x - y * right.y - z * right.z;
    auto x2 = w * right.x + x * right.w + y * right.z - z * right.y;
    auto y2 = w * right.y - x * right.z + y * right.w + z * right.x;
    auto z2 = w * right.z + x * right.y - y * right.x + z * right.w;

    w = w2;
    x = x2;
    y = y2;
    z = z2;

    return *this;
}

TMPL_PREFIX Quaternion<T> Quaternion<T>::operator/(const Quaternion& right) const
{
    auto q = (*this * right.inverted());
    return q;
}

TMPL_PREFIX Quaternion<T>& Quaternion<T>::operator/=(const Quaternion& right)
{
    *this = *this * right.inverted();
    return *this;
}

#undef TMPL_PREFIX

// ---------------------------------------------------------------------------
// Quaternion-vector rotation (global operator)
// ---------------------------------------------------------------------------

template <FP T>
Vector3<T> operator*(const Quaternion<T>& left, const Vector3<T>& right)
{
    // Rotate vector v by unit quaternion q: v' = q * v * q⁻¹
    // Efficient formula: v' = v + 2 * cross(q_v, cross(q_v, v) + q_w * v)
    //                     = v + q_w * t + cross(q_v, t)   where t = 2 * cross(q_v, v)
    const auto qv = Vector3<T>(left.x, left.y, left.z);
    const auto t  = qv.cross(right) * T(2);
    return right + t * left.w + qv.cross(t);
}

template <FP T>
Point3<T> operator*(const Quaternion<T>& left, const Point3<T>& right)
{
    const auto rotated = left * right.asVector();
    return Point3<T>(rotated.x, rotated.y, rotated.z);
}

template class VN_MATH_API Quaternion<float>;
template class VN_MATH_API Quaternion<double>;

template VN_MATH_API Vector3<float> operator*(const Quaternion<float>&, const Vector3<float>&);
template VN_MATH_API Vector3<double> operator*(const Quaternion<double>&, const Vector3<double>&);
template VN_MATH_API Point3<float> operator*(const Quaternion<float>&, const Point3<float>&);
template VN_MATH_API Point3<double> operator*(const Quaternion<double>&, const Point3<double>&);

VN_MATH_NS_END
