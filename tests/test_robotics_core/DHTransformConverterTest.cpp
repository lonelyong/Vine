#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include <vine/math/Isometry3.hpp>
#include <vine/math/Math.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/robotics/kinematics/DHParameter.hpp>
#include <vine/robotics/kinematics/DHTransformConverter.hpp>

using namespace vn;
using namespace vn::math;
using namespace vn::robotics::kinematics;

namespace
{

constexpr double kTol   = 1e-10; /* structural / validity checks */
constexpr double kRtTol = 1e-7;  /* roundtrip error (trace-method quaternion → ~3e-8) */

/* Generate a random angle in [-π, π]. */
double randomAngle(std::mt19937& rng)
{
    std::uniform_real_distribution<double> dist(-math::PI, math::PI);
    return dist(rng);
}

/* Generate a random value in [lo, hi]. */
double randomRange(std::mt19937& rng, double lo, double hi)
{
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(rng);
}

} // namespace

/* ========================================================================= */
/*  Forward transform consistency                                            */
/* ========================================================================= */

TEST(MdhTransformTest, Identity)
{
    DHParameter dh; /* all zeros */
    Isometry3d  T = mdhToTransform(dh);
    EXPECT_NEAR(T.translation.x, 0.0, kTol);
    EXPECT_NEAR(T.translation.y, 0.0, kTol);
    EXPECT_NEAR(T.translation.z, 0.0, kTol);
    EXPECT_NEAR(T.rotation.x, 0.0, kTol);
    EXPECT_NEAR(T.rotation.y, 0.0, kTol);
    EXPECT_NEAR(T.rotation.z, 0.0, kTol);
    EXPECT_NEAR(T.rotation.w, 1.0, kTol);
}

TEST(SdhTransformTest, Identity)
{
    DHParameter dh;
    Isometry3d  T = sdhToTransform(dh);
    EXPECT_NEAR(T.translation.x, 0.0, kTol);
    EXPECT_NEAR(T.translation.y, 0.0, kTol);
    EXPECT_NEAR(T.translation.z, 0.0, kTol);
    EXPECT_NEAR(T.rotation.x, 0.0, kTol);
    EXPECT_NEAR(T.rotation.y, 0.0, kTol);
    EXPECT_NEAR(T.rotation.z, 0.0, kTol);
    EXPECT_NEAR(T.rotation.w, 1.0, kTol);
}

/* ========================================================================= */
/*  MDH roundtrip:  DH → T → tryMdhFromTransform → DH  (should succeed)     */
/* ========================================================================= */

TEST(MdhRoundtripTest, RandomParams)
{
    std::mt19937 rng(42);

    for (int trial = 0; trial < 100; ++trial) {
        DHParameter dh_in;
        dh_in.alpha = randomAngle(rng);
        dh_in.theta = randomAngle(rng);
        dh_in.a     = randomRange(rng, 0.0, 2.0);
        dh_in.d     = randomRange(rng, -1.0, 1.0);

        Isometry3d T = mdhToTransform(dh_in);

        /* Must be representable as MDH. */
        EXPECT_TRUE(isMdhRepresentable(T, kTol));

        auto dh_opt = tryMdhFromTransform(T, kTol);
        ASSERT_TRUE(dh_opt.has_value());

        const DHParameter& dh_out = *dh_opt;

        /* Verify roundtrip via forward transform. */
        Isometry3d T2 = mdhToTransform(dh_out);

        double pos_err = (T2.translation.x - T.translation.x) * (T2.translation.x - T.translation.x) +
                         (T2.translation.y - T.translation.y) * (T2.translation.y - T.translation.y) +
                         (T2.translation.z - T.translation.z) * (T2.translation.z - T.translation.z);

        /* Rotation angular error (2·acos(|q1·q2|)). */
        double qdot    = std::abs(T.rotation.x * T2.rotation.x + T.rotation.y * T2.rotation.y + T.rotation.z * T2.rotation.z + T.rotation.w * T2.rotation.w);
        double rot_err = 2.0 * std::acos(std::clamp(qdot, 0.0, 1.0));

        EXPECT_LT(pos_err, kTol * kTol);
        EXPECT_LT(rot_err, kRtTol);
    }
}

/* ========================================================================= */
/*  SDH roundtrip                                                            */
/* ========================================================================= */

TEST(SdhRoundtripTest, RandomParams)
{
    std::mt19937 rng(99);

    for (int trial = 0; trial < 100; ++trial) {
        DHParameter dh_in;
        dh_in.alpha = randomAngle(rng);
        dh_in.theta = randomAngle(rng);
        dh_in.a     = randomRange(rng, 0.0, 2.0);
        dh_in.d     = randomRange(rng, -1.0, 1.0);

        Isometry3d T = sdhToTransform(dh_in);

        EXPECT_TRUE(isSdhRepresentable(T, kTol));

        auto dh_opt = trySdhFromTransform(T, kTol);
        ASSERT_TRUE(dh_opt.has_value());

        Isometry3d T2 = sdhToTransform(*dh_opt);

        double pos_err = (T2.translation.x - T.translation.x) * (T2.translation.x - T.translation.x) +
                         (T2.translation.y - T.translation.y) * (T2.translation.y - T.translation.y) +
                         (T2.translation.z - T.translation.z) * (T2.translation.z - T.translation.z);

        double qdot    = std::abs(T.rotation.x * T2.rotation.x + T.rotation.y * T2.rotation.y + T.rotation.z * T2.rotation.z + T.rotation.w * T2.rotation.w);
        double rot_err = 2.0 * std::acos(std::clamp(qdot, 0.0, 1.0));

        EXPECT_LT(pos_err, kTol * kTol);
        EXPECT_LT(rot_err, kRtTol);
    }
}

/* ========================================================================= */
/*  Unrepresentable transforms                                               */
/* ========================================================================= */

TEST(UnrepresentableTest, MdhRejectsNonMdhRotation)
{
    /* Rot_y(0.5) has R(0,2) ≠ 0, so not MDH-representable. */
    double angle = 0.5;
    double c = std::cos(angle), s = std::sin(angle);
    /* Rot_y(θ) =
     *   |  c, 0, s |
     *   |  0, 1, 0 |
     *   | -s, 0, c |
     *
     * Build as quaternion:  q = (0, sin(θ/2), 0, cos(θ/2))
     */
    Quatd      q_ry(0.0, std::sin(angle * 0.5), 0.0, std::cos(angle * 0.5));
    Isometry3d T(Point3d(0.0, 0.0, 0.0), q_ry);

    EXPECT_FALSE(isMdhRepresentable(T, kTol));
    EXPECT_EQ(tryMdhFromTransform(T, kTol), std::nullopt);
}

TEST(UnrepresentableTest, SdhRejectsNonSdhRotation)
{
    /* Rot_y(0.5) has R(2,0) ≠ 0, so not SDH-representable either. */
    double     angle = 0.5;
    Quatd      q_ry(0.0, std::sin(angle * 0.5), 0.0, std::cos(angle * 0.5));
    Isometry3d T(Point3d(0.0, 0.0, 0.0), q_ry);

    EXPECT_FALSE(isSdhRepresentable(T, kTol));
    EXPECT_EQ(trySdhFromTransform(T, kTol), std::nullopt);
}

TEST(UnrepresentableTest, RandomRotationRejected)
{
    /* Random SO(3) rotation is unlikely to satisfy MDH/SDH constraints. */
    std::mt19937 rng(123);

    for (int trial = 0; trial < 50; ++trial) {
        /* Build random rotation via random axis-angle. */
        double angle = randomAngle(rng);
        double ax    = randomRange(rng, -1.0, 1.0);
        double ay    = randomRange(rng, -1.0, 1.0);
        double az    = randomRange(rng, -1.0, 1.0);
        double nrm   = std::sqrt(ax * ax + ay * ay + az * az);
        if (nrm < 1e-10) {
            --trial;
            continue;
        }
        ax /= nrm;
        ay /= nrm;
        az /= nrm;

        double     half = angle * 0.5;
        double     s    = std::sin(half);
        Quatd      q(ax * s, ay * s, az * s, std::cos(half));
        Isometry3d T(Point3d(1.0, 2.0, 3.0), q);

        /* Most random rotations fail both constraints. */
        bool mdh_ok = isMdhRepresentable(T, kTol);
        bool sdh_ok = isSdhRepresentable(T, kTol);

        if (mdh_ok) {
            auto r = tryMdhFromTransform(T, kTol);
            ASSERT_TRUE(r.has_value());
        }
        else {
            EXPECT_EQ(tryMdhFromTransform(T, kTol), std::nullopt);
        }

        if (sdh_ok) {
            auto r = trySdhFromTransform(T, kTol);
            ASSERT_TRUE(r.has_value());
        }
        else {
            EXPECT_EQ(trySdhFromTransform(T, kTol), std::nullopt);
        }
    }
}

/* ========================================================================= */
/*  Cross-convention: MDH transform rejected by SDH extraction               */
/* ========================================================================= */

TEST(CrossConventionTest, MdhTransformIsNotSdh)
{
    std::mt19937 rng(77);

    for (int trial = 0; trial < 50; ++trial) {
        DHParameter dh;
        dh.alpha = randomAngle(rng);
        dh.theta = randomAngle(rng);
        dh.a     = randomRange(rng, 0.1, 2.0);
        dh.d     = randomRange(rng, -1.0, 1.0);

        Isometry3d T = mdhToTransform(dh);

        /* MDH transform is always MDH-representable. */
        EXPECT_TRUE(isMdhRepresentable(T, kTol));

        /* But generally NOT SDH-representable (unless α ≈ 0 or π). */
        bool is_alpha_axial = (std::abs(std::sin(dh.alpha)) < kTol);
        if (!is_alpha_axial) {
            EXPECT_FALSE(isSdhRepresentable(T, kTol));
            EXPECT_EQ(trySdhFromTransform(T, kTol), std::nullopt);
        }
    }
}

/* ========================================================================= */
/*  Invalid rotation (non-unit quaternion)                                   */
/* ========================================================================= */

TEST(InvalidRotationTest, NonUnitQuaternion)
{
    /* Non-unit quaternion → extraction fails. */
    Quatd      q(0.2, 0.3, 0.4, 0.5); /* |q| ≠ 1 */
    Isometry3d T(Point3d(0.0, 0.0, 0.0), q);

    EXPECT_EQ(tryMdhFromTransform(T, kTol), std::nullopt);
    EXPECT_EQ(trySdhFromTransform(T, kTol), std::nullopt);
}

TEST(InvalidRotationTest, ZeroQuaternion)
{
    Quatd      q(0.0, 0.0, 0.0, 0.0);
    Isometry3d T(Point3d(0.0, 0.0, 0.0), q);

    EXPECT_EQ(tryMdhFromTransform(T, kTol), std::nullopt);
    EXPECT_EQ(trySdhFromTransform(T, kTol), std::nullopt);
}

/* ========================================================================= */
/*  Tolerance parameter                                                      */
/* ========================================================================= */

TEST(ToleranceTest, LooseTolerancePasses)
{
    /* Build MDH transform, then perturb slightly → fails with tight tol,
     * passes with loose tol. */
    DHParameter dh;
    dh.alpha = 0.8;
    dh.theta = 1.2;
    dh.a     = 1.0;
    dh.d     = 0.3;

    Isometry3d T = mdhToTransform(dh);

    /* Perturb rotation slightly (add a small Ry component). */
    double eps = 1e-6;
    Quatd  q_pert(std::sin(eps * 0.5), 0.0, 0.0, std::cos(eps * 0.5)); /* tiny Rx */
    /* Actually, let's add a tiny Ry to break R(0,2)=0. */
    Quatd      q_ry(0.0, std::sin(eps * 0.5), 0.0, std::cos(eps * 0.5));
    Quatd      q_new = T.rotation * q_ry; /* post-multiply → breaks MDH constraint */
    Isometry3d T2(T.translation, q_new.normalized());

    /* Tight tol → fail. */
    EXPECT_FALSE(isMdhRepresentable(T2, 1e-12));
    EXPECT_EQ(tryMdhFromTransform(T2, 1e-12), std::nullopt);

    /* Loose tol → pass. */
    EXPECT_TRUE(isMdhRepresentable(T2, 1e-4));
    EXPECT_TRUE(tryMdhFromTransform(T2, 1e-4).has_value());
}

/* ========================================================================= */
/*  Edge cases:  α ≈ ±π/2  (cos α ≈ 0)                                     */
/* ========================================================================= */

TEST(EdgeCaseTest, AlphaNearHalfPi)
{
    DHParameter dh;
    dh.alpha = math::PI_HALF; /* 90° */
    dh.theta = 0.3;
    dh.a     = 1.0;
    dh.d     = 0.5;

    Isometry3d T = mdhToTransform(dh);

    EXPECT_TRUE(isMdhRepresentable(T, kTol));

    auto dh_opt = tryMdhFromTransform(T, kTol);
    ASSERT_TRUE(dh_opt.has_value());

    /* α should recover to within tolerance. */
    double alpha_err = std::abs(dh_opt->alpha - dh.alpha);
    /* Account for 2π periodicity. */
    if (alpha_err > math::PI)
        alpha_err = math::PI_TWO - alpha_err;
    EXPECT_LT(alpha_err, kTol);
}

TEST(EdgeCaseTest, AlphaNearMinusHalfPi)
{
    DHParameter dh;
    dh.alpha = -math::PI_HALF;
    dh.theta = -0.7;
    dh.a     = 0.8;
    dh.d     = -0.3;

    Isometry3d T = mdhToTransform(dh);

    auto dh_opt = tryMdhFromTransform(T, kTol);
    ASSERT_TRUE(dh_opt.has_value());

    double alpha_err = std::abs(dh_opt->alpha - dh.alpha);
    if (alpha_err > math::PI)
        alpha_err = math::PI_TWO - alpha_err;
    EXPECT_LT(alpha_err, kTol);
}
