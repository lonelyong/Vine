#include <gtest/gtest.h>

#include <vine/math/Math.hpp>
#include <vine/math/Vector4.hpp>

using namespace vn::math;

TEST(Vector, cross)
{
    // Vec3f v1(1, 9, 10);
    // Vec3f v2(-1, 1, 0);
    // Vec3f v3(-1, -1, 0);
    // Vec3i ref1(0, 0, 1);
    // Vec3i ref2(0, 0, -1);

    // Vector2<bool> vb1;

    Vec4i v1(1, 2, 3, 4);
    Vec4i v2(10, 11, 12, 13);

    auto v3 = v1 + v2;
    auto v4 = v1 - v2;
    auto v6 = v1 * 10;

    Vec4b b1(true, false, true, false);
    Vec4b b2(false, false, true, true);

    auto b3 = b2 / true;
    auto b4 = b1 * true;
    auto b5 = b1 + b2;
    auto b6 = b1 - b2;

}

TEST(Vector4, angleTo)
{
    constexpr double PI = 3.14159265358979323846;
    constexpr double EPS = 1e-6;

    // 测试正交向量（90度）
    {
        Vec4d v1(1.0, 0.0, 0.0, 0.0);
        Vec4d v2(0.0, 1.0, 0.0, 0.0);
        double angle = v1.angleTo(v2);
        EXPECT_NEAR(angle, PI / 2.0, EPS);
    }

    // 测试同向向量（0度）
    {
        Vec4d v1(1.0, 2.0, 3.0, 4.0);
        Vec4d v2(2.0, 4.0, 6.0, 8.0);
        double angle = v1.angleTo(v2);
        EXPECT_NEAR(angle, 0.0, EPS);
    }

    // 测试反向向量（180度）
    {
        Vec4d v1(1.0, 2.0, 3.0, 4.0);
        Vec4d v2(-1.0, -2.0, -3.0, -4.0);
        double angle = v1.angleTo(v2);
        EXPECT_NEAR(angle, PI, EPS);
    }

    // 测试任意角度
    {
        Vec4d v1(1.0, 0.0, 0.0, 0.0);
        Vec4d v2(1.0, 1.0, 0.0, 0.0);
        double angle = v1.angleTo(v2);
        EXPECT_NEAR(angle, PI / 4.0, EPS); // 45度
    }

    // 测试回归案例：之前错误实现的反例
    {
        Vec4d v1(1.0, 0.0, 0.0, 0.0);
        Vec4d v2(1.0, 1.0, 0.0, 1.0);
        // 正确角度：acos(1/sqrt(3)) ≈ 0.9553
        double expected = std::acos(1.0 / std::sqrt(3.0));
        double angle = v1.angleTo(v2);
        EXPECT_NEAR(angle, expected, EPS);
    }

    // 测试零向量（边界情况，返回 atan2(0, 0) = 0）
    {
        Vec4d v1(0.0, 0.0, 0.0, 0.0);
        Vec4d v2(1.0, 2.0, 3.0, 4.0);
        double angle = v1.angleTo(v2);
        // 零向量夹角定义为 0
        EXPECT_NEAR(angle, 0.0, EPS);
    }

    // 测试整数类型
    {
        Vec4i v1(1, 0, 0, 0);
        Vec4i v2(0, 1, 0, 0);
        double angle = v1.angleTo(v2);
        EXPECT_NEAR(angle, PI / 2.0, EPS);
    }
}
