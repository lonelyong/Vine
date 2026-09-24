#include <gtest/gtest.h>

#include <vine/math/Matrix3x3.hpp>
#include <vine/math/Transform2.hpp>

using namespace vn::math;

TEST(Matrix3x3, setToIdentity) {
    Mat3d m;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (i == j) {
                ASSERT_EQ(m(i, j), 1.0);
            }
            else {
                ASSERT_EQ(m(i, j), 0.0);
            }
        }
    }
}

TEST(Matrix3x3, isIdentity) {
    Mat3d m;
    ASSERT_TRUE(m.isIdentity());
}

TEST(Matrix3x3, scalarConstructor) {
    Mat3d m(1, 2, 3,
            4, 5, 6,
            7, 8, 9);
    EXPECT_EQ(m(0, 0), 1);
    EXPECT_EQ(m(0, 1), 2);
    EXPECT_EQ(m(0, 2), 3);
    EXPECT_EQ(m(1, 0), 4);
    EXPECT_EQ(m(1, 1), 5);
    EXPECT_EQ(m(1, 2), 6);
    EXPECT_EQ(m(2, 0), 7);
    EXPECT_EQ(m(2, 1), 8);
    EXPECT_EQ(m(2, 2), 9);
}

TEST(Matrix3x3, transpose) {
    Mat3d m;
    m(0, 1) = 5;
    m(2, 0) = 7;
    m.transpose();
    EXPECT_EQ(m(1, 0), 5);
    EXPECT_EQ(m(0, 2), 7);
}

TEST(Matrix3x3, determinant) {
    Mat3d m(1, 0, 0,
            0, 2, 0,
            0, 0, 3);
    EXPECT_DOUBLE_EQ(m.determinant(), 6.0);
}

TEST(Matrix3x3, invertRoundTrip) {
    Mat3d m(2, 0, 0,
            0, 3, 0,
            0, 0, 4);
    Mat3d inv = m.inverted();
    Mat3d id = m * inv;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (i == j)
                EXPECT_NEAR(id(i, j), 1.0, 1e-12);
            else
                EXPECT_NEAR(id(i, j), 0.0, 1e-12);
}

TEST(Matrix3x3, multiply) {
    Mat3d a(1, 2, 3,
            4, 5, 6,
            7, 8, 9);
    Mat3d b(9, 8, 7,
            6, 5, 4,
            3, 2, 1);
    Mat3d c = a * b;
    EXPECT_EQ(c(0, 0), 30);
    EXPECT_EQ(c(0, 1), 24);
    EXPECT_EQ(c(0, 2), 18);
    EXPECT_EQ(c(1, 0), 84);
    EXPECT_EQ(c(1, 1), 69);
    EXPECT_EQ(c(1, 2), 54);
    EXPECT_EQ(c(2, 0), 138);
    EXPECT_EQ(c(2, 1), 114);
    EXPECT_EQ(c(2, 2), 90);
}

TEST(Matrix3x3, isAffine) {
    Mat3d m;
    ASSERT_TRUE(m.isAffine());

    m(2, 0) = 0.5;
    ASSERT_FALSE(m.isAffine());
}

TEST(Matrix3x3, rowMajorStorage) {
    Matrix3x3<double, RowMajor> m(1, 2, 3,
                                  4, 5, 6,
                                  7, 8, 9);
    EXPECT_EQ(m(0, 0), 1);
    EXPECT_EQ(m(1, 2), 6);
    EXPECT_EQ(m(2, 0), 7);

    Matrix3x3<double, ColMajor> cm(1, 2, 3,
                                    4, 5, 6,
                                    7, 8, 9);
    EXPECT_DOUBLE_EQ(m.determinant(), cm.determinant());
}
