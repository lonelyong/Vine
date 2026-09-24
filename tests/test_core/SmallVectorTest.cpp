#include <gtest/gtest.h>

#include <vine/SmallVector.hpp>

using namespace vn;

TEST(SmallVector, defaultConstructEmpty) {
    SmallVector<int, 8> v;
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0u);
    EXPECT_GE(v.capacity(), 8u);
}

TEST(SmallVector, pushBackAndAccess) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);

    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
    EXPECT_EQ(v[2], 3);
    EXPECT_EQ(v.front(), 1);
    EXPECT_EQ(v.back(), 3);
}

TEST(SmallVector, pushBackBeyondInline) {
    SmallVector<int, 2> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3); // triggers heap allocation

    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
    EXPECT_EQ(v[2], 3);
    EXPECT_GT(v.capacity(), 2u);
}

TEST(SmallVector, popBack) {
    SmallVector<int, 8> v;
    v.push_back(1);
    v.push_back(2);
    v.pop_back();

    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(v.back(), 1);
}

TEST(SmallVector, clear) {
    SmallVector<int, 8> v;
    v.push_back(1);
    v.push_back(2);
    v.clear();

    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0u);
}

TEST(SmallVector, resizeGrow) {
    SmallVector<int, 8> v;
    v.resize(5);

    EXPECT_EQ(v.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_EQ(v[i], 0); // default-constructed ints
}

TEST(SmallVector, resizeShrink) {
    SmallVector<int, 8> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.resize(1);

    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 1);
}

TEST(SmallVector, copyConstructor) {
    SmallVector<int, 4> a;
    a.push_back(10);
    a.push_back(20);

    SmallVector<int, 4> b(a);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 10);
    EXPECT_EQ(b[1], 20);
}

TEST(SmallVector, copyAssignment) {
    SmallVector<int, 4> a;
    a.push_back(1);
    a.push_back(2);

    SmallVector<int, 4> b;
    b.push_back(99);
    b = a;

    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
}

TEST(SmallVector, moveConstructor) {
    SmallVector<int, 4> a;
    a.push_back(5);
    a.push_back(6);

    SmallVector<int, 4> b(std::move(a));
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 5);
    EXPECT_EQ(b[1], 6);
    EXPECT_TRUE(a.empty()); // moved-from
}

TEST(SmallVector, moveAssignment) {
    SmallVector<int, 4> a;
    a.push_back(7);
    a.push_back(8);

    SmallVector<int, 4> b;
    b.push_back(99);
    b = std::move(a);

    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 7);
    EXPECT_EQ(b[1], 8);
}

TEST(SmallVector, emplaceBack) {
    SmallVector<std::string, 4> v;
    v.emplace_back(3, 'x');

    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], "xxx");
}

TEST(SmallVector, atBoundsCheck) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);

    EXPECT_EQ(v.at(0), 1);
    EXPECT_EQ(v.at(1), 2);
    EXPECT_THROW(v.at(2), std::out_of_range);
}

TEST(SmallVector, iteratorLoop) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);

    int sum = 0;
    for (auto x : v)
        sum += x;
    EXPECT_EQ(sum, 6);
}

TEST(SmallVector, constIterator) {
    SmallVector<int, 4> v;
    v.push_back(10);
    v.push_back(20);

    const auto& cv = v;
    int sum = 0;
    for (auto it = cv.cbegin(); it != cv.cend(); ++it)
        sum += *it;
    EXPECT_EQ(sum, 30);
}

TEST(SmallVector, initializerList) {
    SmallVector<int, 8> v = {1, 2, 3, 4, 5};
    EXPECT_EQ(v.size(), 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(v[i], i + 1);
}

TEST(SmallVector, nonTrivialType) {
    SmallVector<std::string, 3> v;
    v.push_back("hello");
    v.push_back("world");

    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], "hello");
    EXPECT_EQ(v[1], "world");
}

TEST(SmallVector, dataAccess) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);

    int* p = v.data();
    EXPECT_EQ(p[0], 1);
    EXPECT_EQ(p[1], 2);
    EXPECT_EQ(p[2], 3);
}

TEST(SmallVector, reserve) {
    SmallVector<int, 2> v;
    v.reserve(10);
    EXPECT_GE(v.capacity(), 10u);
    EXPECT_TRUE(v.empty());
}

TEST(SmallVector, explicitCountConstructor) {
    SmallVector<int, 4> v(3);
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 0);
    EXPECT_EQ(v[1], 0);
    EXPECT_EQ(v[2], 0);
}

TEST(SmallVector, popBackOnEmpty) {
    SmallVector<int, 4> v;
    v.pop_back(); // should be a no-op
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0u);
}

TEST(SmallVector, selfAssignment) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v = v;
    EXPECT_EQ(v.size(), 2u);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
}

TEST(SmallVector, copyConstructorHeapSource) {
    SmallVector<int, 2> a;
    a.push_back(1);
    a.push_back(2);
    a.push_back(3); // now on heap

    SmallVector<int, 2> b(a);
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
    EXPECT_EQ(b[2], 3);
    // b should also be on heap (copied all 3 elements)
    EXPECT_GE(b.capacity(), 3u);
}

TEST(SmallVector, moveConstructorStealHeap) {
    SmallVector<int, 2> a;
    a.push_back(1);
    a.push_back(2);
    a.push_back(3); // heap
    const auto cap_a = a.capacity();

    SmallVector<int, 2> b(std::move(a));
    EXPECT_EQ(b.size(), 3u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
    EXPECT_EQ(b[2], 3);
    EXPECT_EQ(b.capacity(), cap_a); // stole the buffer
    EXPECT_TRUE(a.empty());
    EXPECT_GE(a.capacity(), 2u); // back to inline capacity
}

TEST(SmallVector, moveAssignmentHeapToInline) {
    SmallVector<int, 2> a;
    a.push_back(1);
    a.push_back(2);
    EXPECT_LE(a.capacity(), 2u); // inline

    SmallVector<int, 2> b;
    b.push_back(10);
    b.push_back(20);
    b.push_back(30); // heap

    b = std::move(a);
    EXPECT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
    EXPECT_LE(b.capacity(), 2u); // moved inline
}

TEST(SmallVector, shrinkToFitBackToInline) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.push_back(4);
    v.push_back(5); // heap, capacity = 8
    EXPECT_GT(v.capacity(), 4u);

    v.pop_back();
    v.pop_back(); // size = 3 < 4 (InlineCapacity)
    v.shrink_to_fit();

    EXPECT_EQ(v.size(), 3u);
    EXPECT_LE(v.capacity(), 4u); // back to inline
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
    EXPECT_EQ(v[2], 3);
}

TEST(SmallVector, shrinkToFitHeapReduce) {
    SmallVector<int, 3> v;
    for (int i = 0; i < 10; ++i)
        v.push_back(i); // heap, capacity = 12

    v.resize(6);       // size = 6, capacity = 12
    v.shrink_to_fit();

    EXPECT_EQ(v.size(), 6u);
    EXPECT_EQ(v.capacity(), 6u); // heap shrunk to size
    for (int i = 0; i < 6; ++i)
        EXPECT_EQ(v[i], i);
}
