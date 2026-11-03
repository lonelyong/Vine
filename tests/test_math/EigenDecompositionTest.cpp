#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include <vine/math/EigenDecomposition.hpp>
#include <vine/math/Matrix3x3.hpp>

using namespace vn::math;

namespace {

template <typename T>
T columnDot(const Matrix3x3<T>& m, int a, int b)
{
    return m(0, a) * m(0, b) + m(1, a) * m(1, b) + m(2, a) * m(2, b);
}

template <typename T>
Matrix3x3<T> reconstruct(const Matrix3x3<T>& eigenvectors, const std::array<T, 3>& eigenvalues)
{
    Matrix3x3<T> rebuilt;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            T sum = T(0);
            for (int k = 0; k < 3; ++k) {
                sum += eigenvectors(i, k) * eigenvalues[k] * eigenvectors(j, k);
            }
            rebuilt(i, j) = sum;
        }
    }
    return rebuilt;
}

template <typename T>
void expectOrthonormalBasis(const Matrix3x3<T>& m, T eps)
{
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const T expected = (i == j) ? T(1) : T(0);
            EXPECT_NEAR(columnDot(m, i, j), expected, eps) << "columns " << i << " and " << j;
        }
    }
}

} // namespace

TEST(EigenDecomposition, DiagonalMatrixIsSortedAscending)
{
    const Matrix3x3<double> m(3.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 2.0);

    std::array<double, 3> values{};
    Matrix3x3<double>     vectors;
    ASSERT_TRUE(symmetricEigen(m, values, &vectors));

    EXPECT_DOUBLE_EQ(values[0], 1.0);
    EXPECT_DOUBLE_EQ(values[1], 2.0);
    EXPECT_DOUBLE_EQ(values[2], 3.0);
    expectOrthonormalBasis(vectors, 1e-12);
}

TEST(EigenDecomposition, KnownSpectrumReconstructsTheMatrix)
{
    // 2×2 block with eigenvalues 1 and 3, plus an isolated 3.
    const Matrix3x3<double> m(2.0, 1.0, 0.0, 1.0, 2.0, 0.0, 0.0, 0.0, 3.0);

    std::array<double, 3> values{};
    Matrix3x3<double>     vectors;
    ASSERT_TRUE(symmetricEigen(m, values, &vectors));

    EXPECT_NEAR(values[0], 1.0, 1e-12);
    EXPECT_NEAR(values[1], 3.0, 1e-12);
    EXPECT_NEAR(values[2], 3.0, 1e-12);

    expectOrthonormalBasis(vectors, 1e-12);

    const Matrix3x3<double> rebuilt = reconstruct(vectors, values);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            EXPECT_NEAR(rebuilt(i, j), m(i, j), 1e-12) << "at (" << i << ", " << j << ")";
        }
    }
}

TEST(EigenDecomposition, FloatAndDoubleAgreeOnTheSpectrum)
{
    const Matrix3x3<float> mf(4.0f, 1.0f, 2.0f, 1.0f, 5.0f, 3.0f, 2.0f, 3.0f, 6.0f);
    const Matrix3x3<double> md(4.0, 1.0, 2.0, 1.0, 5.0, 3.0, 2.0, 3.0, 6.0);

    std::array<float, 3>  values_f{};
    std::array<double, 3> values_d{};
    ASSERT_TRUE(symmetricEigen(mf, values_f));
    ASSERT_TRUE(symmetricEigen(md, values_d));

    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(static_cast<double>(values_f[i]), values_d[i], 1e-4);
        EXPECT_LE(values_d[i], values_d[2]) << "ascending order";
    }
}

TEST(EigenDecomposition, NullMatrixConvergesToAZeroSpectrum)
{
    const Matrix3x3<double> zero(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

    std::array<double, 3> values{};
    Matrix3x3<double>     vectors;
    EXPECT_TRUE(symmetricEigen(zero, values, &vectors));
    EXPECT_DOUBLE_EQ(values[0], 0.0);
    EXPECT_DOUBLE_EQ(values[2], 0.0);
    expectOrthonormalBasis(vectors, 1e-12);
}

TEST(EigenDecomposition, OnlyTheUpperTriangleIsRead)
{
    // The lower triangle holds junk: the contract says the caller need not mirror it.
    const Matrix3x3<double> upper_only(2.0, 1.0, 0.0, 99.0, 2.0, 0.0, 99.0, 99.0, 3.0);

    std::array<double, 3> values{};
    ASSERT_TRUE(symmetricEigen(upper_only, values));

    EXPECT_NEAR(values[0], 1.0, 1e-12);
    EXPECT_NEAR(values[1], 3.0, 1e-12);
    EXPECT_NEAR(values[2], 3.0, 1e-12);
}

TEST(EigenDecomposition, RepeatedEigenvaluesStillGiveAnOrthonormalBasis)
{
    const Matrix3x3<double> m(5.0, 0.0, 0.0, 0.0, 5.0, 0.0, 0.0, 0.0, 1.0);

    std::array<double, 3> values{};
    Matrix3x3<double>     vectors;
    ASSERT_TRUE(symmetricEigen(m, values, &vectors));

    EXPECT_DOUBLE_EQ(values[0], 1.0);
    EXPECT_DOUBLE_EQ(values[1], 5.0);
    EXPECT_DOUBLE_EQ(values[2], 5.0);
    expectOrthonormalBasis(vectors, 1e-12);
}
