#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

#include <vine/robotics/kinematics/Q.hpp>

using namespace vn::robotics::kinematics;

namespace
{

constexpr double kTol = 1e-12;

} // namespace

TEST(QTest, Constructors)
{
    // Default: empty.
    Q q0;
    EXPECT_EQ(q0.size(), 0u);
    EXPECT_TRUE(q0.empty());

    // Size: zero vector.
    Q q3(3);
    EXPECT_EQ(q3.size(), 3u);
    EXPECT_EQ(q3[0], 0.0);
    EXPECT_EQ(q3[2], 0.0);

    // Initializer list.
    Q ql{ 1.0, 2.0, 3.0 };
    EXPECT_EQ(ql.size(), 3u);
    EXPECT_EQ(ql[1], 2.0);

    // Raw array.
    const double vals[] = { 4.0, 5.0 };
    Q            qa(vals, 2);
    EXPECT_EQ(qa.size(), 2u);
    EXPECT_EQ(qa[0], 4.0);
    EXPECT_EQ(qa[1], 5.0);

    // std::vector.
    const std::vector<double> vec = { 7.0, 8.0, 9.0 };
    Q                         qv(vec);
    EXPECT_EQ(qv.size(), 3u);
    EXPECT_EQ(qv[2], 9.0);
}

TEST(QTest, DotProduct)
{
    Q a{ 1.0, 2.0, 3.0 };
    Q b{ 4.0, 5.0, 6.0 };
    EXPECT_DOUBLE_EQ(a.dot(b), 32.0); // 1*4 + 2*5 + 3*6

    Q c(4); // Different size.
    EXPECT_THROW(a.dot(c), std::logic_error);
}

TEST(QTest, ElemMul)
{
    Q a{ 1.0, 2.0, 3.0 };
    Q b{ 4.0, 5.0, 6.0 };
    EXPECT_EQ(a.elemMul(b), (Q{ 4.0, 10.0, 18.0 }));

    Q c(4);
    EXPECT_THROW(a.elemMul(c), std::logic_error);
}

TEST(QTest, ConcatAndAppend)
{
    Q a{ 1.0, 2.0 };
    Q b{ 3.0 };
    const Q c = a.concat(b);
    EXPECT_EQ(c, (Q{ 1.0, 2.0, 3.0 }));

    Q d = a;
    d.append(4.0);
    EXPECT_EQ(d, (Q{ 1.0, 2.0, 4.0 }));
    d.append(b);
    EXPECT_EQ(d, (Q{ 1.0, 2.0, 4.0, 3.0 }));
}

TEST(QTest, SubQ)
{
    Q a{ 1.0, 2.0, 3.0, 4.0, 5.0 };
    EXPECT_EQ(a.subQ(1, 3), (Q{ 2.0, 3.0, 4.0 }));

    EXPECT_THROW(a.subQ(4, 2), std::out_of_range);
    EXPECT_THROW(a.subQ(0, 6), std::out_of_range);
}

TEST(QTest, Set)
{
    Q           a(5);
    const double vals[] = { 9.0, 8.0 };
    a.set(2, vals, 2);
    EXPECT_EQ(a[2], 9.0);
    EXPECT_EQ(a[3], 8.0);

    EXPECT_THROW(a.set(4, vals, 2), std::out_of_range);
}

TEST(QTest, Norm)
{
    Q a{ 3.0, 4.0 };
    EXPECT_DOUBLE_EQ(a.normSquared(), 25.0);
    EXPECT_DOUBLE_EQ(a.norm(), 5.0);

    Q empty;
    EXPECT_DOUBLE_EQ(empty.norm(), 0.0);
}

TEST(QTest, ToStdVector)
{
    Q                          a{ 1.0, 2.0, 3.0 };
    const std::vector<double> v = a.toStdVector();
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 1.0);
    EXPECT_EQ(v[2], 3.0);
}

TEST(QTest, ComparisonOperators)
{
    Q a{ 1.0, 2.0 };
    Q b{ 1.0, 2.0 };
    Q c{ 1.0, 3.0 };
    Q d(3);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
    EXPECT_FALSE(a != b);
    // Different sizes never compare equal.
    EXPECT_FALSE(a == d);
}

TEST(QTest, ArithmeticOperators)
{
    Q a{ 1.0, 2.0, 3.0 };
    Q b{ 10.0, 20.0, 30.0 };

    EXPECT_EQ(a + b, (Q{ 11.0, 22.0, 33.0 }));
    EXPECT_EQ(b - a, (Q{ 9.0, 18.0, 27.0 }));

    Q s = a;
    s += b;
    EXPECT_EQ(s, (Q{ 11.0, 22.0, 33.0 }));
    s -= a;
    EXPECT_EQ(s, b);

    EXPECT_EQ(a * 2.0, (Q{ 2.0, 4.0, 6.0 }));
    EXPECT_EQ(b / 2.0, (Q{ 5.0, 10.0, 15.0 }));

    Q t = a;
    t *= 2.0;
    EXPECT_EQ(t, (Q{ 2.0, 4.0, 6.0 }));
    t /= 2.0;
    EXPECT_EQ(t, a);

    // Size mismatch throws.
    Q d(4);
    EXPECT_THROW(a + d, std::logic_error);
    EXPECT_THROW(a += d, std::logic_error);
    EXPECT_THROW(a - d, std::logic_error);
    EXPECT_THROW(a -= d, std::logic_error);
}

// Silence unused-constant warning when kTol is not referenced by some builds.
static_assert(kTol > 0.0, "kTol must be positive");
