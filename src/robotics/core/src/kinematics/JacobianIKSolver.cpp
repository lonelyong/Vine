#include <vine/robotics/kinematics/JacobianIKSolver.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <span>

#include <vine/math/Point3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Vector3.hpp>

VN_ROBOTICS_KINEMATICS_NS_BEGIN

namespace
{

constexpr double kPosTol        = 1e-6;   // position convergence tolerance  (m)
constexpr double kRotTol        = 1e-6;   // orientation convergence tolerance (rad)
constexpr double kPosWeight     = 1.0;    // task weight: position error
constexpr double kRotWeight     = 1.0;    // task weight: orientation error
constexpr int    kMaxIter       = 150;    // max iterations per seed
constexpr int    kMaxSeeds      = 8;      // random restart seeds
constexpr double kMinLambda     = 1e-4;   // min DLS damping
constexpr double kInitLambda    = 0.1;    // initial DLS damping
constexpr double kMaxAngularStep = 0.5;   // max revolute step per iteration  (rad)
constexpr double kMaxLinearStep  = 0.1;   // max prismatic step per iteration  (m)
constexpr double kMaxLambda     = 10.0;   // damping ceiling

/*
 * Orientation error — exact SO(3) logarithmic map.
 *
 *   q_err = q_target * q_current⁻¹     (shortest arc)
 *   rot_err = θ · n                     (axis-angle representation)
 *
 *   θ = 2 · atan2(|v|, w)              (rotation angle)
 *   n = v / |v|                        (rotation axis)
 *
 * The shortest arc is enforced by  q_err.w < 0 ⇒ −q_err.
 * Near-zero rotation (|v| ≈ 0) returns the zero vector.
 */
inline math::Vec3d orientationError(const math::Quatd& current, const math::Quatd& target)
{
    math::Quatd qe = target * current.conj();
    qe.normalize();
    if (qe.w < 0.0) {
        qe.x = -qe.x; qe.y = -qe.y; qe.z = -qe.z; qe.w = -qe.w;
    }
    const double v_norm = std::sqrt(qe.x * qe.x + qe.y * qe.y + qe.z * qe.z);
    if (v_norm < 1e-15) return { 0.0, 0.0, 0.0 };
    const double theta = 2.0 * std::atan2(v_norm, qe.w);
    const double s = theta / v_norm;
    return { s * qe.x, s * qe.y, s * qe.z };
}

/*
 * 6×6 linear system solver — Gaussian elimination with partial pivoting.
 *
 * (J·Jᵀ + λ²I) is SPD, so Cholesky would be ~2× faster.  However, at
 * small λ near convergence the normal matrix may become ill-conditioned
 * (JJᵀ nearly rank-deficient), and partial-pivot GE handles that regime
 * more robustly than unpivoted Cholesky.  For a 6×6 system the absolute
 * cost difference is negligible (~200 flops), so robustness wins.
 */
bool solve6x6(double A[6][6], double b[6], double x[6])
{
    constexpr int N = 6;
    for (int i = 0; i < N; ++i) {
        int    piv = i;
        double pv  = std::abs(A[i][i]);
        for (int r = i + 1; r < N; ++r)
            if (std::abs(A[r][i]) > pv) { pv = std::abs(A[r][i]); piv = r; }
        if (pv < 1e-15) return false;
        if (piv != i) {
            for (int c = i; c < N; ++c) std::swap(A[i][c], A[piv][c]);
            std::swap(b[i], b[piv]);
        }
        const double inv = 1.0 / A[i][i];
        for (int r = i + 1; r < N; ++r) {
            const double f = A[r][i] * inv;
            for (int c = i; c < N; ++c) A[r][c] -= f * A[i][c];
            b[r] -= f * b[i];
        }
    }
    for (int i = N - 1; i >= 0; --i) {
        double s = b[i];
        for (int j = i + 1; j < N; ++j) s -= A[i][j] * x[j];
        x[i] = s / A[i][i];
    }
    return true;
}

/*
 * Single-seed DLS IK iteration.
 *
 * Joint-frame convention
 * ----------------------
 * The FK chain is  T_{i-1}  →  (× origin_i)  →  joint input frame
 * →  (× joint_i(q_i))  →  joint output frame  →  next link.
 *
 * We extract (p_i, z_i) at the joint input frame — before joint_i(q_i)
 * is applied.  For revolute joints the origin position is unchanged by
 * the subsequent rotation, and for prismatic joints the axis direction
 * is unchanged by the subsequent translation, so whether (p_i, z_i) are
 * sampled before or after the joint motion is mathematically equivalent.
 * We sample before to match the textbook geometric-Jacobian convention.
 *
 * DLS for general DOF counts
 * ---------------------------
 *   n < 6 : under-actuated — DLS yields the best-effort least-squares
 *           step.  Unreachable target components are handled gracefully.
 *   n = 6 : standard square-Jacobian DLS.
 *   n > 6 : redundant robot — Δq = Jᵀ·y is the damped minimum-norm
 *           solution among all Δq satisfying J·Δq ≈ e.
 * Secondary objectives (joint-limit avoidance, posture optimisation)
 * would require null-space projection  (I − J⁺J)·q_null  on top of the
 * minimum-norm solution.  This is left as a future extension.
 *
 * Adaptive damping (not true Levenberg-Marquardt)
 * -----------------------------------------------
 * Each step is unconditionally accepted (after joint-limit clamping).
 * λ is adjusted heuristically:  λ ← 0.7·λ when the total error
 * decreased from the previous iteration,  λ ← 2·λ when it increased.
 * True LM would re-evaluate FK after the trial step and reject the
 * update if the error rose — at the cost of a second FK per iteration.
 * The heuristic is lighter and sufficient for well-conditioned chains.
 *
 * `q` holds the seed on entry and is modified in-place during iteration.
 * All other buffers are caller-allocated and reused — zero heap traffic
 * in the hot loop.
 */
bool solveFromSeed(std::span<const DofInfo>        dofs,
                   const math::Isometry3d&          target,
                   std::vector<Q>&                  solutions,
                   std::span<math::Isometry3d>      T_chain,
                   std::span<math::Vec3d>           p_world,
                   std::span<math::Vec3d>           z_world,
                   std::span<double>                Jc,
                   std::span<double>                q,
                   std::span<double>                dq)
{
    const size_t n = dofs.size();
    if (n == 0) return false;

    double lambda   = kInitLambda;
    double prev_err = std::numeric_limits<double>::max();

    for (int iter = 0; iter < kMaxIter; ++iter) {
        // ---- forward kinematics ----
        math::Isometry3d T;
        for (size_t i = 0; i < n; ++i) {
            T = T * dofs[i].origin;                       // joint input frame
            p_world[i] = T.translation.asVector();         //   → position p_i
            z_world[i] = T.rotation * dofs[i].axis;        //   → axis     z_i

            if (dofs[i].type == DofType::PrismaticJoint) {
                const double d = q[i];
                T = T * math::Isometry3d(
                    math::Point3d(dofs[i].axis.x * d, dofs[i].axis.y * d, dofs[i].axis.z * d),
                    math::Quatd(1, 0, 0, 0));
            } else {
                T = T * math::Isometry3d(math::Point3d(0, 0, 0),
                                         math::Quatd(q[i], dofs[i].axis));
            }
            T_chain[i] = T;                                // joint output frame
        }
        const math::Isometry3d& T_ee = T_chain.back();

        // ---- errors (world frame) ----
        const math::Vec3d pos_err = target.translation.asVector() - T_ee.translation.asVector();
        const math::Vec3d rot_err = orientationError(T_ee.rotation, target.rotation);

        const double pos_norm = pos_err.length();
        const double rot_norm = rot_err.length();
        const double err      = kPosWeight * pos_norm + kRotWeight * rot_norm;

        // ---- convergence ----
        if (pos_norm < kPosTol && rot_norm < kRotTol) {
            Q sol;
            for (size_t i = 0; i < n; ++i) sol.push_back(q[i]);
            solutions.push_back(std::move(sol));
            return true;
        }

        // ---- Jacobian columns (weighted, computed once per iteration) ----
        const math::Vec3d p_ee = T_ee.translation.asVector();
        for (size_t i = 0; i < n; ++i) {
            math::Vec3d Jv, Jw;
            if (dofs[i].type == DofType::PrismaticJoint) {
                Jv = z_world[i];
                Jw = { 0, 0, 0 };
            } else {
                Jv = z_world[i].cross(p_ee - p_world[i]);
                Jw = z_world[i];
            }
            double* col = &Jc[i * 6];
            col[0] = kPosWeight * Jv.x; col[1] = kPosWeight * Jv.y; col[2] = kPosWeight * Jv.z;
            col[3] = kRotWeight * Jw.x; col[4] = kRotWeight * Jw.y; col[5] = kRotWeight * Jw.z;
        }

        // ---- error vector (weighted) ----
        const double e_vec[6] = { kPosWeight * pos_err.x, kPosWeight * pos_err.y, kPosWeight * pos_err.z,
                                  kRotWeight * rot_err.x, kRotWeight * rot_err.y, kRotWeight * rot_err.z };

        // ---- DLS: accumulate J·Jᵀ (6×6) + λ²I ----
        double JJt[6][6] = {};
        for (size_t i = 0; i < n; ++i) {
            const double* col = &Jc[i * 6];
            for (int r = 0; r < 6; ++r)
                for (int c = 0; c < 6; ++c)
                    JJt[r][c] += col[r] * col[c];
        }
        const double lambda2 = lambda * lambda;
        for (int i = 0; i < 6; ++i) JJt[i][i] += lambda2;

        // ---- solve (J·Jᵀ + λ²I)·y = e ----
        double A[6][6], b[6], y[6];
        for (int i = 0; i < 6; ++i) {
            b[i] = e_vec[i];
            for (int j = 0; j < 6; ++j) A[i][j] = JJt[i][j];
        }
        if (!solve6x6(A, b, y)) {
            lambda = std::min(lambda * 2.0, kMaxLambda);
            continue;
        }

        // ---- Δq = Jᵀ·y  (damped minimum-norm, works for all n) ----
        for (size_t i = 0; i < n; ++i) {
            const double* col = &Jc[i * 6];
            dq[i] = col[0] * y[0] + col[1] * y[1] + col[2] * y[2] +
                    col[3] * y[3] + col[4] * y[4] + col[5] * y[5];
        }

        // ---- step clamp (per-joint-type limits), joint limits ----
        double step = 1.0;
        for (size_t i = 0; i < n; ++i) {
            const double limit = (dofs[i].type == DofType::PrismaticJoint)
                                     ? kMaxLinearStep : kMaxAngularStep;
            if (std::abs(dq[i]) > limit)
                step = std::min(step, limit / std::abs(dq[i]));
        }

        for (size_t i = 0; i < n; ++i) {
            q[i] += step * dq[i];
            if (dofs[i].lower < dofs[i].upper)
                q[i] = std::clamp(q[i], dofs[i].lower, dofs[i].upper);
        }

        // ---- adaptive damping (heuristic, not true LM) ----
        if (err < prev_err) {
            lambda = std::max(kMinLambda, lambda * 0.7);
        } else {
            lambda = std::min(lambda * 2.0, kMaxLambda);
        }
        prev_err = err;
    }

    return false;
}

} // anonymous namespace

/*
 * Public entry point.
 *
 * Tries up to kMaxSeeds random-restart configurations.  Returns as soon
 * as the first valid IK solution is found — this is the preferred
 * behaviour for real-time control loops where a single feasible
 * configuration is sufficient.  The `solutions` vector receives at most
 * one entry.
 *
 * All working buffers are allocated once here and reused across seeds
 * and iterations.
 */
bool JacobianIKSolver::solve(const math::Isometry3d& target,
                             std::vector<Q>&          solutions) const
{
    const size_t n = dofs_.size();
    if (n == 0) return false;

    // -------- pre-allocated buffers (reused across all seeds) --------
    std::vector<math::Isometry3d> T_chain(n);
    std::vector<math::Vec3d>      p_world(n);
    std::vector<math::Vec3d>      z_world(n);
    std::vector<double>           Jc(n * 6);   // Jacobian columns, 6 doubles/joint
    std::vector<double>           q(n);        // working joint vector
    std::vector<double>           dq(n);       // joint update

    std::mt19937                           rng(42);
    std::uniform_real_distribution<double> dist(-math::PI, math::PI);

    // -------- Seed 0: zero (clamped to limits) --------
    for (size_t i = 0; i < n; ++i) {
        if (dofs_[i].lower < dofs_[i].upper)
            q[i] = std::clamp(0.0, dofs_[i].lower, dofs_[i].upper);
        else
            q[i] = 0.0;
    }
    if (solveFromSeed(dofs_, target, solutions, T_chain, p_world, z_world, Jc, q, dq))
        return true;

    // -------- Seeds 1..kMaxSeeds-1: uniform random within limits --------
    for (int seed = 1; seed < kMaxSeeds; ++seed) {
        for (size_t i = 0; i < n; ++i) {
            const double lo = dofs_[i].lower, hi = dofs_[i].upper;
            if (lo < hi) {
                std::uniform_real_distribution<double> jdist(lo, hi);
                q[i] = jdist(rng);
            } else {
                q[i] = dist(rng);
            }
        }
        if (solveFromSeed(dofs_, target, solutions, T_chain, p_world, z_world, Jc, q, dq))
            return true;
    }

    return false;
}

VN_ROBOTICS_KINEMATICS_NS_END