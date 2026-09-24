#include <gtest/gtest.h>

#include <vine/math/Math.hpp>
#include <vine/math/Quaternion.hpp>

using namespace vn::math;

TEST(Quaternion, multiplication)
{
    constexpr double EPS = 1e-9;

    // Test basic unit quaternions: i, j, k
    // i = (1, 0, 0, 0), j = (0, 1, 0, 0), k = (0, 0, 1, 0), 1 = (0, 0, 0, 1)

    // Test i * j = k
    {
        Quatd i(1.0, 0.0, 0.0, 0.0); // i
        Quatd j(0.0, 1.0, 0.0, 0.0); // j
        Quatd result = i * j;

        EXPECT_NEAR(result.x, 0.0, EPS);
        EXPECT_NEAR(result.y, 0.0, EPS);
        EXPECT_NEAR(result.z, 1.0, EPS); // k
        EXPECT_NEAR(result.w, 0.0, EPS);
    }

    // Test j * k = i
    {
        Quatd j(0.0, 1.0, 0.0, 0.0); // j
        Quatd k(0.0, 0.0, 1.0, 0.0); // k
        Quatd result = j * k;

        EXPECT_NEAR(result.x, 1.0, EPS); // i
        EXPECT_NEAR(result.y, 0.0, EPS);
        EXPECT_NEAR(result.z, 0.0, EPS);
        EXPECT_NEAR(result.w, 0.0, EPS);
    }

    // Test k * i = j
    {
        Quatd k(0.0, 0.0, 1.0, 0.0); // k
        Quatd i(1.0, 0.0, 0.0, 0.0); // i
        Quatd result = k * i;

        EXPECT_NEAR(result.x, 0.0, EPS);
        EXPECT_NEAR(result.y, 1.0, EPS); // j
        EXPECT_NEAR(result.z, 0.0, EPS);
        EXPECT_NEAR(result.w, 0.0, EPS);
    }

    // Test i * i = -1
    {
        Quatd i(1.0, 0.0, 0.0, 0.0); // i
        Quatd result = i * i;

        EXPECT_NEAR(result.x, 0.0, EPS);
        EXPECT_NEAR(result.y, 0.0, EPS);
        EXPECT_NEAR(result.z, 0.0, EPS);
        EXPECT_NEAR(result.w, -1.0, EPS); // -1
    }

    // Test j * i = -k (anti-commutative)
    {
        Quatd j(0.0, 1.0, 0.0, 0.0); // j
        Quatd i(1.0, 0.0, 0.0, 0.0); // i
        Quatd result = j * i;

        EXPECT_NEAR(result.x, 0.0, EPS);
        EXPECT_NEAR(result.y, 0.0, EPS);
        EXPECT_NEAR(result.z, -1.0, EPS); // -k
        EXPECT_NEAR(result.w, 0.0, EPS);
    }

    // Test identity: q * (0,0,0,1) = q
    {
        Quatd q(1.0, 2.0, 3.0, 4.0);
        Quatd identity(0.0, 0.0, 0.0, 1.0);
        Quatd result = q * identity;

        EXPECT_NEAR(result.x, q.x, EPS);
        EXPECT_NEAR(result.y, q.y, EPS);
        EXPECT_NEAR(result.z, q.z, EPS);
        EXPECT_NEAR(result.w, q.w, EPS);
    }

    // Test operator*=
    {
        Quatd i(1.0, 0.0, 0.0, 0.0); // i
        Quatd j(0.0, 1.0, 0.0, 0.0); // j
        i *= j;

        EXPECT_NEAR(i.x, 0.0, EPS);
        EXPECT_NEAR(i.y, 0.0, EPS);
        EXPECT_NEAR(i.z, 1.0, EPS); // k
        EXPECT_NEAR(i.w, 0.0, EPS);
    }
}

TEST(Quaternion, makeRotate_from_to)
{
    constexpr double EPS = 1e-6;
    constexpr double PI = 3.14159265358979323846;

    // Test 90 deg rotation from X to Y
    {
        Vec3d from(1.0, 0.0, 0.0);
        Vec3d to(0.0, 1.0, 0.0);
        Quatd q;
        q.makeRotate(from, to);

        // Quaternion should represent 90 deg rotation around Z axis
        // q = (0, 0, sin(45 deg), cos(45 deg))
        EXPECT_NEAR(q.x, 0.0, EPS);
        EXPECT_NEAR(q.y, 0.0, EPS);
        EXPECT_NEAR(q.z, std::sin(PI / 4.0), EPS);
        EXPECT_NEAR(q.w, std::cos(PI / 4.0), EPS);

        // Verify quaternion is normalized
        double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        EXPECT_NEAR(len, 1.0, EPS);
    }

    // Test same direction (identity)
    {
        Vec3d from(1.0, 2.0, 3.0);
        Vec3d to(2.0, 4.0, 6.0); // Parallel, same direction
        Quatd q;
        q.makeRotate(from, to);

        // Should produce identity quaternion
        EXPECT_NEAR(q.x, 0.0, EPS);
        EXPECT_NEAR(q.y, 0.0, EPS);
        EXPECT_NEAR(q.z, 0.0, EPS);
        EXPECT_NEAR(q.w, 1.0, EPS);
    }

    // Test opposite direction (180 deg rotation)
    {
        Vec3d from(1.0, 0.0, 0.0);
        Vec3d to(-1.0, 0.0, 0.0);
        Quatd q;
        q.makeRotate(from, to);

        // Should produce 180 deg rotation (w = 0)
        EXPECT_NEAR(q.w, 0.0, EPS);

        // Rotation axis should be perpendicular to from
        double dot = q.x * from.x + q.y * from.y + q.z * from.z;
        EXPECT_NEAR(dot, 0.0, EPS);

        // Quaternion should be normalized
        double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        EXPECT_NEAR(len, 1.0, EPS);
    }

    // Test arbitrary rotation
    {
        Vec3d from(1.0, 0.0, 0.0);
        Vec3d to(1.0, 1.0, 0.0); // 45 deg from X towards Y
        Quatd q;
        q.makeRotate(from, to);

        // Verify quaternion is normalized
        double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        EXPECT_NEAR(len, 1.0, EPS);

        // Rotation axis should be Z (perpendicular to XY plane)
        EXPECT_NEAR(q.x, 0.0, EPS);
        EXPECT_NEAR(q.y, 0.0, EPS);
        // Angle is 45 deg, so half-angle is 22.5 deg
        EXPECT_NEAR(q.z, std::sin(PI / 8.0), EPS);
        EXPECT_NEAR(q.w, std::cos(PI / 8.0), EPS);
    }

    // Test with non-unit vectors
    {
        Vec3d from(3.0, 0.0, 0.0);
        Vec3d to(0.0, 5.0, 0.0);
        Quatd q;
        q.makeRotate(from, to);

        // Should still produce correct rotation (90 deg around Z)
        EXPECT_NEAR(q.x, 0.0, EPS);
        EXPECT_NEAR(q.y, 0.0, EPS);
        EXPECT_NEAR(q.z, std::sin(PI / 4.0), EPS);
        EXPECT_NEAR(q.w, std::cos(PI / 4.0), EPS);
    }

    // Test zero vector (edge case)
    {
        Vec3d from(0.0, 0.0, 0.0);
        Vec3d to(1.0, 0.0, 0.0);
        Quatd q;
        q.makeRotate(from, to);

        // Should produce identity quaternion
        EXPECT_NEAR(q.x, 0.0, EPS);
        EXPECT_NEAR(q.y, 0.0, EPS);
        EXPECT_NEAR(q.z, 0.0, EPS);
        EXPECT_NEAR(q.w, 1.0, EPS);
    }
}

TEST(Quaternion, slerp)
{
    constexpr double EPS = 1e-6;
    constexpr double PI = 3.14159265358979323846;

    // Test boundary: t = 0 should return 'from'
    {
        // Use normalized quaternions
        Quatd from(0.0, 0.0, 0.0, 1.0); // Identity
        Quatd to(0.0, 0.0, std::sin(PI / 4.0), std::cos(PI / 4.0)); // 90 deg rotation around Z
        Quatd result = Quatd::slerp(from, to, 0.0);

        EXPECT_NEAR(result.x, from.x, EPS);
        EXPECT_NEAR(result.y, from.y, EPS);
        EXPECT_NEAR(result.z, from.z, EPS);
        EXPECT_NEAR(result.w, from.w, EPS);
    }

    // Test boundary: t = 1 should return 'to'
    {
        Quatd from(0.0, 0.0, 0.0, 1.0); // Identity
        Quatd to(0.0, 0.0, std::sin(PI / 4.0), std::cos(PI / 4.0)); // 90 deg rotation around Z
        Quatd result = Quatd::slerp(from, to, 1.0);

        EXPECT_NEAR(result.x, to.x, EPS);
        EXPECT_NEAR(result.y, to.y, EPS);
        EXPECT_NEAR(result.z, to.z, EPS);
        EXPECT_NEAR(result.w, to.w, EPS);
    }

    // Test midpoint interpolation between orthogonal unit quaternions
    {
        // 90 deg rotation around Z-axis
        Quatd from(0.0, 0.0, std::sin(PI / 4.0), std::cos(PI / 4.0));
        // 180 deg rotation around Z-axis
        Quatd to(0.0, 0.0, std::sin(PI / 2.0), std::cos(PI / 2.0));

        Quatd result = Quatd::slerp(from, to, 0.5);

        // Result should be 135 deg rotation around Z-axis
        // q = (0, 0, sin(67.5 deg), cos(67.5 deg))
        EXPECT_NEAR(result.x, 0.0, EPS);
        EXPECT_NEAR(result.y, 0.0, EPS);
        EXPECT_NEAR(result.z, std::sin(3.0 * PI / 8.0), EPS);
        EXPECT_NEAR(result.w, std::cos(3.0 * PI / 8.0), EPS);

        // Verify result is normalized
        double len = std::sqrt(result.x * result.x + result.y * result.y +
                               result.z * result.z + result.w * result.w);
        EXPECT_NEAR(len, 1.0, EPS);
    }

    // Test shortest path selection (negative dot product)
    {
        // Two quaternions representing similar rotations but on opposite hemispheres
        Quatd from(0.0, 0.0, 0.1, 0.995);  // Small rotation
        Quatd to(0.0, 0.0, -0.1, -0.995);  // Same rotation but negated (equivalent)

        Quatd result = Quatd::slerp(from, to, 0.5);

        // Result should be close to identity or one of the inputs
        // since they represent nearly the same rotation
        double len = std::sqrt(result.x * result.x + result.y * result.y +
                               result.z * result.z + result.w * result.w);
        EXPECT_NEAR(len, 1.0, EPS);
    }

    // Test linear interpolation fallback (very close quaternions)
    {
        Quatd from(0.0, 0.0, 0.0, 1.0);         // Identity
        Quatd to(0.0, 0.0, 0.001, 0.9999995);   // Nearly identity

        Quatd result = Quatd::slerp(from, to, 0.5);

        // Result should be normalized
        double len = std::sqrt(result.x * result.x + result.y * result.y +
                               result.z * result.z + result.w * result.w);
        EXPECT_NEAR(len, 1.0, EPS);

        // Should be between from and to
        EXPECT_GT(result.z, from.z);
        EXPECT_LT(result.z, to.z);
    }

    // Test interpolation produces unit quaternions
    {
        Quatd from(0.0, 0.0, std::sin(PI / 6.0), std::cos(PI / 6.0));
        Quatd to(0.0, 0.0, std::sin(PI / 3.0), std::cos(PI / 3.0));

        for (double t = 0.0; t <= 1.0; t += 0.1) {
            Quatd result = Quatd::slerp(from, to, t);
            double len = std::sqrt(result.x * result.x + result.y * result.y +
                                   result.z * result.z + result.w * result.w);
            EXPECT_NEAR(len, 1.0, EPS);
        }
    }

    // Test identity interpolation
    {
        Quatd identity(0.0, 0.0, 0.0, 1.0);
        Quatd rotation(0.0, 0.0, std::sin(PI / 4.0), std::cos(PI / 4.0));

        Quatd result = Quatd::slerp(identity, rotation, 0.5);

        // Should be halfway rotation
        EXPECT_NEAR(result.x, 0.0, EPS);
        EXPECT_NEAR(result.y, 0.0, EPS);
        EXPECT_NEAR(result.z, std::sin(PI / 8.0), EPS);
        EXPECT_NEAR(result.w, std::cos(PI / 8.0), EPS);
    }
}
