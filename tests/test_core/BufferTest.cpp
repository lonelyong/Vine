/**
 * @brief Tests for the reference-counted buffer.
 *
 * The buffer exists to hold ONE allocation that two consumers describe differently — a model that wants
 * typed access and a renderer that wants bytes — so the properties worth asserting are exactly those that
 * make that safe: the storage is handed over (not copied), the byte view is the typed elements themselves,
 * and sharing is by reference count rather than by copy.
 */

#include <vine/Buffer.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

using namespace vine;

namespace
{

/// @brief A trivially copyable element, standing in for a vertex.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

} // namespace

TEST(BufferTest, ValueInitialisesItsElements)
{
    auto buffer = intrusive_ptr<Buffer<int>>(new Buffer<int>(4));

    EXPECT_EQ(buffer->size(), 4u);
    EXPECT_FALSE(buffer->empty());
    for (std::size_t index = 0; index < buffer->size(); ++index) {
        EXPECT_EQ((*buffer)[index], 0) << index;
    }
}

TEST(BufferTest, AdoptsAVectorInsteadOfCopyingIt)
{
    std::vector<int> values{ 1, 2, 3 };
    const int* const storage = values.data();

    auto buffer = intrusive_ptr<Buffer<int>>(new Buffer<int>(std::move(values)));

    // Identity, not equality: the entire point is that the caller hands its storage over. A copy would show
    // up as a different address while the contents still matched.
    EXPECT_EQ(buffer->data(), storage);
    EXPECT_EQ((*buffer)[1], 2);
}

TEST(BufferTest, TheByteViewIsTheTypedElementsThemselves)
{
    auto buffer = intrusive_ptr<Buffer<Vec3>>(new Buffer<Vec3>(2));
    (*buffer)[0] = Vec3{ 1.0f, 2.0f, 3.0f };
    (*buffer)[1] = Vec3{ 4.0f, 5.0f, 6.0f };

    const auto bytes = buffer->bytes();

    EXPECT_EQ(bytes.size(), 2u * sizeof(Vec3));
    EXPECT_EQ(reinterpret_cast<const std::byte*>(buffer->data()), bytes.data())
        << "the byte view must alias the elements, not a packed copy of them";

    // And the bytes really are the floats, in order — which is why no repacking is needed for a layout the
    // device happens to accept (3 x float32 here).
    float read_back[6] = {};
    std::memcpy(read_back, bytes.data(), bytes.size());
    EXPECT_FLOAT_EQ(read_back[0], 1.0f);
    EXPECT_FLOAT_EQ(read_back[2], 3.0f);
    EXPECT_FLOAT_EQ(read_back[5], 6.0f);
}

TEST(BufferTest, IsSharedRatherThanCopied)
{
    auto first = intrusive_ptr<Buffer<int>>(new Buffer<int>(2));
    (*first)[0] = 7;

    auto second = first;
    EXPECT_EQ(second->useCount(), 2u);
    EXPECT_EQ((*second)[0], 7) << "one allocation, seen through two owners";

    first = nullptr;
    EXPECT_EQ(second->useCount(), 1u);
    EXPECT_EQ((*second)[0], 7) << "the data outlives the owner that let go";
}

TEST(BufferTest, AnEmptyBufferHasNoStorage)
{
    auto buffer = intrusive_ptr<Buffer<int>>(new Buffer<int>(0));

    EXPECT_TRUE(buffer->empty());
    EXPECT_EQ(buffer->size(), 0u);
    EXPECT_TRUE(buffer->view().empty());
    EXPECT_TRUE(buffer->bytes().empty());
}

TEST(BufferTest, WriteAccessReachesTheElements)
{
    // Only the LENGTH is fixed (so views stay valid); writing contents is allowed, which is what lets a
    // loader or a mesh builder fill a buffer it just created.
    auto buffer = intrusive_ptr<Buffer<Vec3>>(new Buffer<Vec3>(1));

    auto values = buffer->view();
    values[0]  = Vec3{ 9.0f, 8.0f, 7.0f };

    EXPECT_FLOAT_EQ((*buffer)[0].x, 9.0f);
    EXPECT_FLOAT_EQ(buffer->view()[0].z, 7.0f);
}
