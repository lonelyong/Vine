#include <vine/robotics/kinematics/PieperIKSolver.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

#include <vine/math/Math.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>
#include <vine/robotics/kinematics/DHTransformConverter.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

namespace
{

/* Tolerances */
constexpr double kEpsAbs   = 1e-8;
constexpr double kEpsGeom  = 1e-10;
constexpr double kPosTol   = 1e-4;
constexpr double kRotTol   = 1e-4;
constexpr double kJointTol = 1e-6;
constexpr double kAngleTol = 1e-8;

} // namespace

PieperIKSolver::PieperIKSolver(const std::vector<DofInfo>& dofs)
  : ClosedFormIKSolver(dofs)
{
    if (dofs.size() != 6)
        return;

    for (int i = 0; i < 6; ++i) {
        auto mdh_opt = tryMdhFromTransform(dofs[i].origin);
        if (!mdh_opt)
            return;
        mdh_[i] = *mdh_opt;
    }

    // ---- Pieper structural preconditions (MDH convention) ----

    // Spherical wrist: axes 4,5,6 intersect → a₃=a₄=a₅=0
    if (std::abs(mdh_[3].a) > kEpsGeom || std::abs(mdh_[4].a) > kEpsGeom || std::abs(mdh_[5].a) > kEpsGeom)
        return;

    // Arm geometry (required by solveArm):
    //   α₁ ≈ ±90°  – joint 1 perpendicular to joint 2
    //   α₂ ≈ 0,π   – joint 2 parallel to joint 3
    //   a₂ ≠ 0     – non-zero link length
    if (std::abs(std::abs(std::sin(mdh_[1].alpha)) - 1.0) > kEpsGeom) // |sin α₁| ≠ 1
        return;
    if (std::abs(std::sin(mdh_[2].alpha)) > kEpsGeom) // sin α₂ ≠ 0
        return;
    if (std::abs(mdh_[2].a) <= kEpsGeom) // a₂ ≈ 0
        return;

    is_valid_ = true;
}

namespace
{

/* Angle utilities */
inline double wrapAngle(double a)
{
    return math::normalizeAngle(a);
}

inline double angleDiff(double a, double b)
{
    double d = std::abs(wrapAngle(a) - wrapAngle(b));
    return (d > math::PI) ? math::PI_TWO - d : d;
}

/* Joint limit handling (with 2π-wrap equivalence) */
inline double snapToLimits(double q_raw, double lower, double upper)
{
    if (!(lower < upper))
        return q_raw;
    for (int k = -1; k <= 1; ++k) {
        double qk = q_raw + k * math::PI_TWO;
        if (qk >= lower - kJointTol && qk <= upper + kJointTol)
            return qk;
    }
    return q_raw;
}

inline bool checkJointLimits(const std::array<double, 6>& q_raw, std::span<const DofInfo> dofs)
{
    for (int i = 0; i < 6; ++i) {
        const auto& lim = dofs[i];
        if (!(lim.lower < lim.upper))
            continue;
        bool ok = false;
        for (int k = -1; k <= 1; ++k) {
            double qk = q_raw[i] + k * math::PI_TWO;
            if (qk >= lim.lower - kJointTol && qk <= lim.upper + kJointTol) {
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

/* Pose error */
struct PoseError {
    double pos;
    double rot;
};

inline PoseError computePoseError(const math::Isometry3d& T, const math::Isometry3d& target)
{
    using namespace math;
    const double dx = T.translation.x - target.translation.x;
    const double dy = T.translation.y - target.translation.y;
    const double dz = T.translation.z - target.translation.z;

    const Quatd  q = (T.rotation * target.rotation.conj()).normalized();
    const double w = std::clamp(std::abs(q.w), 0.0, 1.0);
    return { std::sqrt(dx * dx + dy * dy + dz * dz), 2.0 * std::acos(w) };
}

/* MDH forward kinematics */
math::Isometry3d fkMdh(std::span<const DHParameter> dh, std::span<const double> q)
{
    math::Isometry3d T;
    for (size_t i = 0; i < dh.size(); ++i) T = T * mdhToTransform(dh[i], 0.0, q[i]);
    return T;
}

/* A·cos(x) + B·sin(x) = C */
int solveTrig(double A, double B, double C, double sol[2])
{
    const double norm = std::sqrt(A * A + B * B);
    if (norm < kEpsAbs)
        return 0;
    double rhs = C / norm;
    if (rhs > 1.0 + kEpsAbs || rhs < -1.0 - kEpsAbs)
        return 0;
    rhs              = std::clamp(rhs, -1.0, 1.0);
    const double phi = std::atan2(B, A);
    const double ac  = std::acos(rhs);
    sol[0]           = wrapAngle(phi + ac);
    sol[1]           = wrapAngle(phi - ac);
    return (angleDiff(sol[0], sol[1]) < kAngleTol) ? 1 : 2;
}

// addSolution – limit check → snap → FK verify → dedup → store
void addSolution(std::span<const DHParameter> mdh,
                 const math::Isometry3d&      target,
                 const std::array<double, 6>& q_raw,
                 std::span<const DofInfo>     dofs,
                 std::vector<Q>&              solutions)
{
    using namespace math;

    if (!dofs.empty() && !checkJointLimits(q_raw, dofs))
        return;

    std::array<double, 6> q;
    for (int i = 0; i < 6; ++i) {
        const auto& lim = dofs.empty() ? DofInfo{} : dofs[i];
        q[i]            = snapToLimits(q_raw[i], lim.lower, lim.upper);
    }

    const Isometry3d T06          = fkMdh(mdh, q);
    const auto [pos_err, rot_err] = computePoseError(T06, target);
    if (pos_err > kPosTol || rot_err > kRotTol)
        return;

    for (const auto& existing : solutions) {
        bool same = true;
        for (int i = 0; i < 6; ++i)
            if (angleDiff(existing[i], q[i]) > kJointTol) {
                same = false;
                break;
            }
        if (same)
            return;
    }

    Q sol;
    for (int i = 0; i < 6; ++i) sol.push_back(q[i]);
    solutions.push_back(std::move(sol));
}

/* Wrist decomposition helpers
   R_wrist = Rz(θ₄) · Rx(α₄) · Rz(θ₅) · Rx(α₅) · Rz(θ₆) */
struct WristContext {
    std::span<const DHParameter> mdh;
    const math::Isometry3d&      target;
    std::array<double, 3>        arm_q;
    std::span<const DofInfo>     dofs;
    std::array<double, 6>        seed;
    math::Mat3d                  R; // rotation matrix of R_wrist
    double                       ca4, sa4, ca5, sa5;
};

/* Path A.1: regular non-singular */
void solveWristRegular(WristContext& ctx, double theta5, std::vector<Q>& solutions)
{
    const auto&  R  = ctx.R;
    const double s5 = std::sin(theta5), c5 = std::cos(theta5);
    const double P     = ctx.sa5 * s5;
    const double Qw    = ctx.ca4 * ctx.sa5 * c5 + ctx.sa4 * ctx.ca5;
    const double denom = P * P + Qw * Qw;
    if (denom < kEpsGeom)
        return; // falls through to singular

    // [r₁₃; r₂₃] = [P, Qw; −Qw, P] · [c₄; s₄]
    const double c4     = (P * R(0, 2) - Qw * R(1, 2)) / denom;
    const double s4     = (Qw * R(0, 2) + P * R(1, 2)) / denom;
    const double theta4 = std::atan2(s4, c4);

    // θ₆ from Aᵀ · R_wrist  (A = Rz₄·Rxα₄·Rz₅·Rxα₅)
    const double c4_ = std::cos(theta4), s4_ = std::sin(theta4);
    const double A00 = c4_ * c5 - s4_ * ctx.ca4 * s5;
    const double A10 = s4_ * c5 + c4_ * ctx.ca4 * s5;
    const double A20 = ctx.sa4 * s5;
    const double M01 = -ctx.ca5 * s5;
    const double M11 = ctx.ca4 * ctx.ca5 * c5 - ctx.sa4 * ctx.sa5;
    const double M21 = ctx.sa4 * ctx.ca5 * c5 + ctx.ca4 * ctx.sa5;
    const double A01 = c4_ * M01 - s4_ * M11;
    const double A11 = s4_ * M01 + c4_ * M11;

    const double c6     = A00 * R(0, 0) + A10 * R(1, 0) + A20 * R(2, 0);
    const double s6     = A01 * R(0, 0) + A11 * R(1, 0) + M21 * R(2, 0);
    const double theta6 = std::atan2(s6, c6);

    addSolution(ctx.mdh, ctx.target, { ctx.arm_q[0], ctx.arm_q[1], ctx.arm_q[2], theta4, theta5, theta6 }, ctx.dofs, solutions);
}

/* Path A.2: θ₅ singular (s₅ ≈ 0) */
void solveWristSingular(WristContext& ctx, double theta5, std::vector<Q>& solutions)
{
    const auto&  R      = ctx.R;
    const double theta4 = ctx.seed[3];
    const double c5     = std::cos(theta5);
    const double c4 = std::cos(theta4), s4 = std::sin(theta4);

    // Rz(−θ₄)·R_wrist
    const double Rw11 = c4 * R(0, 0) + s4 * R(1, 0);
    const double Rw21 = -s4 * R(0, 0) + c4 * R(1, 0);

    const double M00 = c5;
    const double M11 = ctx.ca4 * c5 * ctx.ca5 - ctx.sa4 * ctx.sa5;
    double       c6, s6;
    if (std::abs(M00) > kEpsGeom && std::abs(M11) > kEpsGeom) {
        c6 = Rw11 / M00;
        s6 = Rw21 / M11;
    }
    else {
        s6 = Rw21;
        c6 = Rw11;
    }
    const double theta6 = std::atan2(s6, c6);

    addSolution(ctx.mdh, ctx.target, { ctx.arm_q[0], ctx.arm_q[1], ctx.arm_q[2], theta4, theta5, theta6 }, ctx.dofs, solutions);
}

/* Path B.1: α₄ ≈ 0 or π */
void solveWristDegenA4(WristContext& ctx, std::vector<Q>& solutions)
{
    const auto& R      = ctx.R;
    // σ = +1 for α₄≈0, −1 for α₄≈π  (from r₃₃·cα₅ sign)
    const double sigma = (R(2, 2) * ctx.ca5 > 0.0) ? 1.0 : -1.0;

    // θ_sum = θ₄ + σ·θ₅  (observable from col 2)
    double       theta_sum;
    const double sa5_eff = std::sqrt(std::max(0.0, 1.0 - (sigma * R(2, 2)) * (sigma * R(2, 2))));
    if (sa5_eff > kEpsGeom)
        theta_sum = std::atan2(R(0, 2), -R(1, 2));
    else
        theta_sum = std::atan2(R(1, 0), R(0, 0));

    const double theta4 = ctx.seed[3];
    const double theta5 = sigma * (theta_sum - theta4);

    // Strip Rz(θ_sum) → Rx(α₅_eff)·Rz(θ₆)
    const double cs = std::cos(theta_sum), ss = std::sin(theta_sum);
    const double Rp11   = cs * R(0, 0) + ss * R(1, 0);
    const double Rp12   = cs * R(0, 1) + ss * R(1, 1);
    const double theta6 = std::atan2(-Rp12, Rp11);

    addSolution(ctx.mdh, ctx.target, { ctx.arm_q[0], ctx.arm_q[1], ctx.arm_q[2], theta4, theta5, theta6 }, ctx.dofs, solutions);
}

/* Path B.2: α₅ ≈ 0 or π */
void solveWristDegenA5(WristContext& ctx, std::vector<Q>& solutions)
{
    const auto&  R     = ctx.R;
    const double sigma = (R(2, 2) * ctx.ca4 > 0.0) ? 1.0 : -1.0;

    // θ₄ from col 2 (sa₄ ≠ 0 here)
    const double theta4 = std::atan2(R(0, 2), -R(1, 2));

    // Strip Rz(θ₄) → Rx(α₄)·Rz(θ₅₆)
    const double c4 = std::cos(theta4), s4 = std::sin(theta4);
    const double Rp11    = c4 * R(0, 0) + s4 * R(1, 0);
    const double Rp31    = R(2, 0);
    const double theta56 = std::atan2(Rp31 / ctx.sa4, Rp11);

    const double theta6 = ctx.seed[5];
    const double theta5 = theta56 - sigma * theta6;

    addSolution(ctx.mdh, ctx.target, { ctx.arm_q[0], ctx.arm_q[1], ctx.arm_q[2], theta4, theta5, theta6 }, ctx.dofs, solutions);
}

// solveWrist – dispatch
void solveWrist(const DHParameter*           mdh,
                const math::Isometry3d&      target,
                const std::array<double, 3>& arm_q,
                const math::Quatd&           q_03,
                std::span<const DofInfo>     dofs,
                const std::array<double, 6>& seed,
                std::vector<Q>&              solutions)
{
    using namespace math;

    // R_wrist = Rot_x(−α₃) · R₀₃ᵀ · R_target
    Quatd q_36 = q_03.conj() * target.rotation;
    Quatd q_rx_neg_a3(std::sin(-mdh[3].alpha * 0.5), 0.0, 0.0, std::cos(-mdh[3].alpha * 0.5));
    Quatd q_wrist = (q_rx_neg_a3 * q_36).normalized();

    WristContext ctx{
        .mdh    = { mdh, 6 },
        .target = target,
        .arm_q  = arm_q,
        .dofs   = dofs,
        .seed   = seed,
        .R      = rotate3x3(q_wrist),
        .ca4    = std::cos(mdh[4].alpha),
        .sa4    = std::sin(mdh[4].alpha),
        .ca5    = std::cos(mdh[5].alpha),
        .sa5    = std::sin(mdh[5].alpha),
    };

    const double denom5 = ctx.sa4 * ctx.sa5;

    if (std::abs(denom5) > kEpsGeom) {
        // Path A: regular
        double c5               = (ctx.ca4 * ctx.ca5 - ctx.R(2, 2)) / denom5;
        c5                      = std::clamp(c5, -1.0, 1.0);
        const double t5_vals[2] = { std::acos(c5), -std::acos(c5) };
        for (int wb = 0; wb < 2; ++wb) {
            const double s5 = std::sin(t5_vals[wb]);
            const double P  = ctx.sa5 * s5;
            const double Qw = ctx.ca4 * ctx.sa5 * std::cos(t5_vals[wb]) + ctx.sa4 * ctx.ca5;
            if (P * P + Qw * Qw > kEpsGeom)
                solveWristRegular(ctx, t5_vals[wb], solutions);
            else
                solveWristSingular(ctx, t5_vals[wb], solutions);
        }
        return;
    }

    // Path B: α degenerate
    if (std::abs(ctx.sa4) <= kEpsGeom)
        solveWristDegenA4(ctx, solutions);
    else if (std::abs(ctx.sa5) <= kEpsGeom)
        solveWristDegenA5(ctx, solutions);
}

// solveArm – position IK: θ₁, θ₂, θ₃
void solveArm(const DHParameter*           mdh,
              const math::Vec3d&           p_w_des,
              const math::Vec3d&           p_w4,
              const math::Isometry3d&      target,
              std::span<const DofInfo>     dofs,
              const std::array<double, 6>& seed,
              std::vector<Q>&              solutions)
{
    using namespace math;

    const double a1 = mdh[1].a, d1 = mdh[0].d;
    const double a2 = mdh[2].a, d2 = mdh[1].d, d3 = mdh[2].d;
    const double d4  = mdh[3].d;
    const double sa1 = std::sin(mdh[1].alpha);
    const double ca3 = std::cos(mdh[3].alpha), sa3 = std::sin(mdh[3].alpha);

    const double r2_des = p_w_des.x * p_w_des.x + p_w_des.y * p_w_des.y;
    const double z_des  = p_w_des.z;

    // a₁ branch selection (|sin α₁|≈1 guaranteed by constructor)
    const bool a1_nonzero = (std::abs(a1) > kEpsAbs);
    const bool a1_zero    = !a1_nonzero;

    const double sig1  = (sa1 > 0.0) ? 1.0 : -1.0;
    const double arm_v = (z_des - d1) * sig1;

    // Wrist centre components in frame 2 (constant for α₂=0)
    const double wc_x = mdh[3].a;      // a₃
    const double wc_y = -d4 * sa3;     // −d₄·sinα₃
    const double wc_z = d4 * ca3 + d3; // d₄·cosα₃ + d₃
    const double wp_z = wc_z + d2;     // + d₁ (T₁ offset)

    if (a1_nonzero) {
        // ---- |w'|² quadratic in branch ±
        const double p_w4_sq = wc_x * wc_x + d4 * d4;
        const double Kc      = p_w4_sq + d3 * d3 + 2.0 * d3 * d4 * ca3 + a2 * a2 + d2 * d2 + 2.0 * d2 * (d4 * ca3 + d3);
        const double S       = r2_des + arm_v * arm_v - a1 * a1;
        const double abs_a1  = std::abs(a1);

        for (int branch = 0; branch < 2; ++branch) {
            const double sign  = (branch == 0) ? 1.0 : -1.0;
            const double Delta = S - wp_z * wp_z - arm_v * arm_v + a1 * a1;
            if (Delta < -kEpsAbs)
                continue;
            const double sqrtDelta = std::sqrt(std::max(0.0, Delta));
            const double wp_sq     = S + 2.0 * a1 * a1 + sign * 2.0 * abs_a1 * sqrtDelta;
            const double rhs       = (wp_sq - Kc) / (2.0 * a2);

            double    th3_sol[2];
            const int n3 = solveTrig(wc_x, -wc_y, rhs, th3_sol);
            for (int i3 = 0; i3 < n3; ++i3) {
                const double theta3 = th3_sol[i3];
                const double c3 = std::cos(theta3), s3 = std::sin(theta3);
                const double wx     = a2 + wc_x * c3 - wc_y * s3;
                const double wy     = wc_x * s3 + wc_y * c3;
                const double denom2 = wx * wx + wy * wy;
                if (denom2 < kEpsAbs)
                    continue;

                const double U      = (r2_des - a1 * a1 - (wx * wx + wy * wy) - (wp_z * wp_z - arm_v * arm_v)) / (2.0 * a1);
                const double c2     = (wx * U + wy * arm_v) / denom2;
                const double s2     = (wx * arm_v - wy * U) / denom2;
                const double r2     = std::sqrt(c2 * c2 + s2 * s2);
                const double theta2 = (r2 > kEpsAbs) ? std::atan2(s2 / r2, c2 / r2) : 0.0;
                const double vx = a1 + U, vy = -sig1 * wp_z;
                const double theta1 = wrapAngle(std::atan2(p_w_des.y, p_w_des.x) - std::atan2(vy, vx));

                const double     q_arm[3] = { theta1, theta2, theta3 };
                const Isometry3d T03      = fkMdh({ mdh, 3 }, q_arm);
                const Point3d    pw_pt    = T03 * Point3d(p_w4.x, p_w4.y, p_w4.z);
                const Vec3d      pw_test(pw_pt.x, pw_pt.y, pw_pt.z);
                if ((pw_test - p_w_des).length2() > kPosTol * kPosTol)
                    continue;
                solveWrist(mdh, target, { theta1, theta2, theta3 }, T03.rotation, dofs, seed, solutions);
            }
        }
    }
    else {
        // ---- a₁ = 0: explicit ±U
        const double Kc_xy = a2 * a2 + wc_x * wc_x + d4 * d4 * sa3 * sa3;
        const double wx2y2 = r2_des - (wp_z * wp_z - arm_v * arm_v);
        const double rhs   = (wx2y2 - Kc_xy) / (2.0 * a2);

        double    th3_sol[2];
        const int n3 = solveTrig(wc_x, -wc_y, rhs, th3_sol);
        for (int i3 = 0; i3 < n3; ++i3) {
            const double theta3 = th3_sol[i3];
            const double c3 = std::cos(theta3), s3 = std::sin(theta3);
            const double wx     = a2 + wc_x * c3 - wc_y * s3;
            const double wy     = wc_x * s3 + wc_y * c3;
            const double denom2 = wx * wx + wy * wy;
            if (denom2 < kEpsAbs)
                continue;
            const double U2 = denom2 - arm_v * arm_v;
            if (U2 < -kEpsAbs)
                continue;
            const double U_val = std::sqrt(std::max(0.0, U2));
            for (int sign = -1; sign <= 1; sign += 2) {
                const double U      = sign * U_val;
                const double c2     = (wx * U + wy * arm_v) / denom2;
                const double s2     = (wx * arm_v - wy * U) / denom2;
                const double r2     = std::sqrt(c2 * c2 + s2 * s2);
                const double theta2 = (r2 > kEpsAbs) ? std::atan2(s2 / r2, c2 / r2) : 0.0;
                const double vx = U, vy = -sig1 * wp_z;
                const double theta1 = wrapAngle(std::atan2(p_w_des.y, p_w_des.x) - std::atan2(vy, vx));

                const double     q_arm[3] = { theta1, theta2, theta3 };
                const Isometry3d T03      = fkMdh({ mdh, 3 }, q_arm);
                const Point3d    pw_pt    = T03 * Point3d(p_w4.x, p_w4.y, p_w4.z);
                const Vec3d      pw_test(pw_pt.x, pw_pt.y, pw_pt.z);
                if ((pw_test - p_w_des).length2() > kPosTol * kPosTol)
                    continue;
                solveWrist(mdh, target, { theta1, theta2, theta3 }, T03.rotation, dofs, seed, solutions);
            }
        }
    }
}

} // anonymous namespace

/* PieperIKSolver::solve */

bool PieperIKSolver::solve(const math::Isometry3d& target, std::vector<Q>& solutions) const
{
    return solve(target, solutions, {});
}

bool PieperIKSolver::solve(const math::Isometry3d& target, std::vector<Q>& solutions, const Q& seed) const
{
    using namespace math;

    if (!is_valid_)
        return false;

    const auto&        dof_list = dofs();
    const DHParameter* mdh      = mdh_.data();

    const double d4 = mdh[3].d, d6 = mdh[5].d;
    const double ca3 = std::cos(mdh[3].alpha), sa3 = std::sin(mdh[3].alpha);
    const Vec3d  p_w4(mdh[3].a, -d4 * sa3, d4 * ca3);

    const Vec3d p_target(target.translation.x, target.translation.y, target.translation.z);
    const Vec3d z_target  = target.axisZ();
    const Vec3d p_w_world = p_target - z_target * d6;

    const Isometry3d T0_fixed = mdhToTransform(mdh[0], 0.0, 0.0);
    const Isometry3d T0_inv   = T0_fixed.inverted();
    const Point3d    pw_base  = T0_inv * Point3d(p_w_world.x, p_w_world.y, p_w_world.z);
    const Vec3d      p_w_des(pw_base.x, pw_base.y, pw_base.z);

    std::array<double, 6> seed_arr{};
    for (size_t i = 0; i < seed.size() && i < 6; ++i) seed_arr[i] = seed[i];

    solutions.clear();
    solveArm(mdh, p_w_des, p_w4, target, dof_list, seed_arr, solutions);

    // Sort by angular distance from seed
    std::sort(solutions.begin(), solutions.end(), [&seed_arr](const Q& a, const Q& b) {
        double da = 0.0, db = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            da += angleDiff(a[i], seed_arr[i]) * angleDiff(a[i], seed_arr[i]);
            db += angleDiff(b[i], seed_arr[i]) * angleDiff(b[i], seed_arr[i]);
        }
        return da < db;
    });

    return !solutions.empty();
}

VN_ROBOTICS_KINEMATICS_NS_END
