#include <gtest/gtest.h>

#include <cmath>

#include <vine/math/Matrix3x3.hpp>
#include <vine/math/Quaternion.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>

using namespace vn::math;

namespace {

template <typename T>
T quaternionDot(const Quaternion<T>& a, const Quaternion<T>& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

template <typename T>
void expectSameRotation(const Quaternion<T>& actual, const Quaternion<T>& expected, T eps)
{
    // q and -q are the same rotation.
    EXPECT_NEAR(std::abs(quaternionDot(actual, expected)), T(1), eps);
}

} // namespace

TEST(Transform3, toQuaternionRoundTripsAxisAngleRotations)
{
    const Vec3d axes[] = { Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0), Vec3d(0.0, 0.0, 1.0), Vec3d(1.0, 2.0, 3.0).normalized() };
    const double   angles[] = { 0.0, 0.3, 1.0, 2.5, 3.0 };

    for (const auto& axis : axes) {
        for (const double angle : angles) {
            const Quatd source(angle, axis);

            const Mat3d matrix = rotate3x3(source);
            const Quatd result = toQuaternion(matrix);

            EXPECT_NEAR(result.length(), 1.0, 1e-12);
            expectSameRotation(result, source, 1e-12);
        }
    }
}

TEST(Transform3, toQuaternionHandlesHalfTurns)
{
    // A 180° turn has trace <= 0, which exercises the diagonal branches instead of the trace branch.
    const Vec3d axes[] = { Vec3d(1.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0), Vec3d(0.0, 0.0, 1.0) };

    for (const auto& axis : axes) {
        const Quatd source(PI, axis);
        const Quatd result = toQuaternion(rotate3x3(source));
        expectSameRotation(result, source, 1e-12);
    }
}

TEST(Transform3, toQuaternionReturnsIdentityForTheIdentityMatrix)
{
    const Quatd result = toQuaternion(Mat3d());
    EXPECT_TRUE(result.isIdentity(1e-12));
}

TEST(Transform3, toQuaternionAgreesAcrossStorageOrders)
{
    const Quatd source(0.7, Vec3d(1.0, -2.0, 0.5).normalized());

    const Mat3d col_major = rotate3x3(source);
    const Matrix3x3<double, RowMajor> row_major(col_major(0, 0),
                                                col_major(0, 1),
                                                col_major(0, 2),
                                                col_major(1, 0),
                                                col_major(1, 1),
                                                col_major(1, 2),
                                                col_major(2, 0),
                                                col_major(2, 1),
                                                col_major(2, 2));

    expectSameRotation(toQuaternion(col_major), source, 1e-12);
    expectSameRotation(toQuaternion(row_major), source, 1e-12);
}
