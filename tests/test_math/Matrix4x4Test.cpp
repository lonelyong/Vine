#include <gtest/gtest.h>

#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Transform3.hpp>

using namespace vn::math;

TEST(Matrix4x4, setToIdentity) {
    Mat4d m;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (i == j) {
                ASSERT_TRUE(m(i, j) == 1.0);
            }
            else {
                ASSERT_TRUE(m(i, j) == 0.0);
            }
        }
    }
}

TEST(Matrix4x4, transpose) {
    Mat4d m;
    m(1, 3) = 6;
    m.transpose();
    ASSERT_TRUE(m(3, 1) == 6);
}

TEST(Matrix4x4, isIdentity) {
    Mat4d m;
    ASSERT_TRUE(m.isIdentity());
}

TEST(Matrix4x4, multiplyAssign) {
    Mat4d a = translate(1.0, 2.0, 3.0);

    Mat4d b = scale(2.0, 3.0, 4.0);

    Mat4d expect = a * b;
    a *= b;

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_DOUBLE_EQ(a(r, c), expect(r, c));
        }
    }
}

TEST(Matrix4x4, makeRotationZeroAngleMakesIdentity) {
    Mat4d m;
    m = translate(9.0, 8.0, 7.0);
    m = rotate(Vec3d(1.0, 0.0, 0.0), 0.0);
    EXPECT_TRUE(m.isIdentity());
}

TEST(Matrix4x4, transformVectorAndPoint) {
    Mat4d m = translate(10.0, 20.0, 30.0);

    Vec3d v(1.0, 2.0, 3.0);
    Point3d p(1.0, 2.0, 3.0);

    // Vector should ignore translation.
    Vec3d v2 = m * v;
    EXPECT_DOUBLE_EQ(v2.x, 1.0);
    EXPECT_DOUBLE_EQ(v2.y, 2.0);
    EXPECT_DOUBLE_EQ(v2.z, 3.0);

    // Point should include translation.
    Point3d p2 = m * p;
    EXPECT_DOUBLE_EQ(p2.x, 11.0);
    EXPECT_DOUBLE_EQ(p2.y, 22.0);
    EXPECT_DOUBLE_EQ(p2.z, 33.0);
}

TEST(Matrix4x4, invertAffineRoundTrip) {
    constexpr double pi = 3.14159265358979323846;

    Mat4d t = translate(5.0, -2.0, 1.5);

    Mat4d r = rotate(Vec3d(0.0, 0.0, 1.0), pi * 0.25);

    Mat4d s = scale(2.0, 3.0, 4.0);

    Mat4d m = t * r * s;
    Mat4d inv = m.inverted();
    Mat4d id = m * inv;

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (i == j) {
                EXPECT_NEAR(id(i, j), 1.0, 1e-9);
            }
            else {
                EXPECT_NEAR(id(i, j), 0.0, 1e-9);
            }
        }
    }
}

TEST(Matrix4x4, quaternionRotationMatchesAxisAngle) {
    constexpr double half_pi = 1.5707963267948966;
    Quatd q(half_pi, Vec3d(0.0, 0.0, 1.0));

    Mat4d mq = rotate(q);

    Mat4d ma = rotate(Vec3d(0.0, 0.0, 1.0), half_pi);

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            EXPECT_NEAR(mq(i, j), ma(i, j), 1e-12);
        }
    }
}

TEST(Matrix4x4, isRigidRejectsScaleAndReflection) {
    Mat4d rigid = rotate(Vec3d(0.0, 1.0, 0.0), 0.3);
    rigid(0, 3) = 1.0;
    rigid(1, 3) = 2.0;
    rigid(2, 3) = 3.0;
    EXPECT_TRUE(rigid.isRigid());

    Mat4d scaled = scale(2.0, 1.0, 1.0);
    EXPECT_FALSE(scaled.isRigid());

    Mat4d reflected = scale(-1.0, 1.0, 1.0);
    EXPECT_FALSE(reflected.isRigid());
}

TEST(Matrix4x4, perspectiveMapsPointWithHomogeneousDivide) {
    constexpr double pi = 3.14159265358979323846;
    Mat4d p = perspective<double>(pi / 2.0, 1.0, 1.0, 100.0);

    Point3d view_pt(0.0, 0.0, -2.0);
    Point3d clip_ndc = p * view_pt;

    // Center line should stay centered after perspective divide.
    EXPECT_NEAR(clip_ndc.x, 0.0, 1e-12);
    EXPECT_NEAR(clip_ndc.y, 0.0, 1e-12);

    // z in OpenGL NDC should be in [-1, 1] for point inside frustum.
    EXPECT_GE(clip_ndc.z, -1.0);
    EXPECT_LE(clip_ndc.z, 1.0);
}