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

using namespace vn;

namespace
{

/// @brief A trivially copyable element, standing in for a vertex.
struct Vec3 {
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
    {
        auto edit = buffer->edit();
        edit[0]   = Vec3{ 1.0f, 2.0f, 3.0f };
        edit[1]   = Vec3{ 4.0f, 5.0f, 6.0f };
    }

    const auto bytes = buffer->bytes();

    EXPECT_EQ(bytes.size(), 2u * sizeof(Vec3));
    EXPECT_EQ(reinterpret_cast<const std::byte*>(buffer->data()), bytes.data()) << "the byte view must alias the elements, not a packed copy of them";

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
    {
        auto edit = first->edit();
        edit[0]   = 7;
    }

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
    // Writing contents is what lets a loader or a mesh builder fill a buffer it just created - and the scope
    // is what makes the buffer hear about it, which a raw pointer never could (see the class note).
    auto buffer = intrusive_ptr<Buffer<Vec3>>(new Buffer<Vec3>(1));
    {
        auto edit = buffer->edit();
        auto span = edit.view();
        span[0]   = Vec3{ 9.0f, 8.0f, 7.0f };
    }

    EXPECT_FLOAT_EQ((*buffer)[0].x, 9.0f);
    EXPECT_FLOAT_EQ(buffer->view()[0].z, 7.0f);
}

TEST(BufferTest, AnEditAnnouncesItselfExactlyOnce)
{
    // The alternative this replaced was "no write announces, the writer says so afterwards": a rule that is
    // correct only if nobody forgets, and forgetting is invisible. The scope is the missing fact - where the
    // edit ends - so the announcement is the scope's, and it happens once whatever the edit did.
    auto buffer = intrusive_ptr<Buffer<int>>(new Buffer<int>());
    ASSERT_EQ(buffer->revision(), 0u);

    {
        auto edit = buffer->edit();
        edit.push_back(1);
        edit.append(std::vector<int>{ 2, 3 });
        edit.reserve(64);
        EXPECT_EQ(buffer->revision(), 0u) << "the edit is not announced until it ends";
        EXPECT_EQ(edit.size(), 3u);
    }

    EXPECT_EQ(buffer->revision(), 1u) << "one edit, one announcement (not one per element, not one per call)";
    EXPECT_EQ(buffer->size(), 3u);

    {
        auto edit = buffer->edit();
        edit.clear();
    }
    EXPECT_EQ(buffer->revision(), 2u) << "and emptying it is an edit like any other";
    EXPECT_TRUE(buffer->empty());
}

#ifndef NDEBUG
TEST(BufferTest, ASecondWriterCannotOpenWhileOneIsLive)
{
    // Two live writers would each announce the end of "the" edit, and the second one's end would tell a
    // consumer nothing. It is a call-site bug, so it asserts where it is made.
    auto buffer = intrusive_ptr<Buffer<int>>(new Buffer<int>(1));

    auto outer = buffer->edit();
    EXPECT_DEATH(
        {
            auto inner = buffer->edit();
            (void)inner;
        },
        "one writer at a time");
    (void)outer;
}
#endif

TEST(BufferTest, SetRevisionStaysForBytesWrittenOutsideTheBuffer)
{
    // The escape hatch, not the ordinary path: a file mapped elsewhere, a device write, a producer with its own
    // version number. It moves the counter like an edit does, so a cached consumer sees "new contents".
    auto buffer = intrusive_ptr<Buffer<int>>(new Buffer<int>(1));
    {
        auto edit = buffer->edit();
        edit[0]   = 5;
    }
    ASSERT_EQ(buffer->revision(), 1u);

    buffer->setRevision(41u); // a writer that has a revision of its own reports it
    EXPECT_EQ(buffer->revision(), 41u);

    buffer->bumpRevision(); // the safe step, which cannot lower it
    EXPECT_EQ(buffer->revision(), 42u);

    // The counter says nothing about the data; it only orders changes.
    EXPECT_EQ((*buffer)[0], 5);
}
