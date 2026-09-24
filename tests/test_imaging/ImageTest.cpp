#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <span>
#include <stdexcept>
#include <string>

#include <vine/intrusive_ptr.hpp>

#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>

using vn::imaging::bytesPerPixel;
using vn::imaging::channelCount;
using vn::imaging::formatName;
using vn::imaging::Image;
using vn::imaging::isDepthFormat;
using vn::imaging::isSrgbFormat;
using vn::imaging::PixelFormat;
using vn::intrusive_ptr;

namespace
{

/**
 * @brief The facts the free functions must report for one real format.
 *
 * Held as a table so that adding a format to the enum forces a decision here rather than
 * leaving its accessors untested.
 */
struct FormatFact {
    PixelFormat format;
    int         channels;
    std::size_t bytes;
    bool        depth;
    bool        srgb;
    const char* name;
};

const FormatFact kFacts[] = {
    { PixelFormat::R8Unorm, 1, 1, false, false, "R8Unorm" },
    { PixelFormat::R8Srgb, 1, 1, false, true, "R8Srgb" },
    { PixelFormat::Rg8Unorm, 2, 2, false, false, "Rg8Unorm" },
    { PixelFormat::Rgb8Unorm, 3, 3, false, false, "Rgb8Unorm" },
    { PixelFormat::Rgb8Srgb, 3, 3, false, true, "Rgb8Srgb" },
    { PixelFormat::Rgba8Unorm, 4, 4, false, false, "Rgba8Unorm" },
    { PixelFormat::Rgba8Srgb, 4, 4, false, true, "Rgba8Srgb" },
    { PixelFormat::Bgra8Unorm, 4, 4, false, false, "Bgra8Unorm" },
    { PixelFormat::Bgra8Srgb, 4, 4, false, true, "Bgra8Srgb" },
    { PixelFormat::R16Float, 1, 2, false, false, "R16Float" },
    { PixelFormat::Rg16Float, 2, 4, false, false, "Rg16Float" },
    { PixelFormat::Rgba16Float, 4, 8, false, false, "Rgba16Float" },
    { PixelFormat::R32Float, 1, 4, false, false, "R32Float" },
    { PixelFormat::Rg32Float, 2, 8, false, false, "Rg32Float" },
    { PixelFormat::Rgba32Float, 4, 16, false, false, "Rgba32Float" },
    { PixelFormat::D16Unorm, 1, 2, true, false, "D16Unorm" },
    { PixelFormat::D24UnormS8Uint, 2, 4, true, false, "D24UnormS8Uint" },
    { PixelFormat::D32Float, 1, 4, true, false, "D32Float" },
};

/// A value that cannot be any enumerator, used to probe the out-of-range path.
const PixelFormat kBogusFormat = static_cast<PixelFormat>(200);

} // namespace

TEST(PixelFormatTest, ReportsTheFactsOfEveryFormat)
{
    for (const FormatFact& fact : kFacts) {
        SCOPED_TRACE(fact.name);

        EXPECT_EQ(channelCount(fact.format), fact.channels);
        EXPECT_EQ(bytesPerPixel(fact.format), fact.bytes);
        EXPECT_EQ(isDepthFormat(fact.format), fact.depth);
        EXPECT_EQ(isSrgbFormat(fact.format), fact.srgb);
        EXPECT_STREQ(formatName(fact.format), fact.name);
    }
}

TEST(PixelFormatTest, UnknownIsNotAFormat)
{
    EXPECT_EQ(channelCount(PixelFormat::Unknown), 0);
    EXPECT_EQ(bytesPerPixel(PixelFormat::Unknown), 0);
    EXPECT_FALSE(isDepthFormat(PixelFormat::Unknown));
    EXPECT_FALSE(isSrgbFormat(PixelFormat::Unknown));
    EXPECT_STREQ(formatName(PixelFormat::Unknown), "Unknown");
}

TEST(PixelFormatTest, AnOutOfRangeValueReadsAsUnknown)
{
    EXPECT_EQ(channelCount(kBogusFormat), 0);
    EXPECT_EQ(bytesPerPixel(kBogusFormat), 0);
    EXPECT_FALSE(isDepthFormat(kBogusFormat));
    EXPECT_FALSE(isSrgbFormat(kBogusFormat));
    EXPECT_STREQ(formatName(kBogusFormat), "Unknown");
}

TEST(PixelFormatTest, EveryFormatHasItsOwnName)
{
    // A duplicated name would make a diagnostic point at the wrong format, and it is the kind
    // of mistake a copy-pasted table row causes.
    std::set<std::string> names;
    names.insert("Unknown");

    for (const FormatFact& fact : kFacts) {
        ASSERT_NE(formatName(fact.format), nullptr);
        EXPECT_TRUE(names.insert(formatName(fact.format)).second) << "duplicate name: " << fact.name;
    }
}

TEST(PixelFormatTest, NameIsAStableLiteral)
{
    // Documented as a static literal, so a caller may keep the pointer instead of copying it.
    EXPECT_EQ(formatName(PixelFormat::Rgba8Unorm), formatName(PixelFormat::Rgba8Unorm));
}

TEST(ImageMipCapacityTest, CountsLevelsDownToOnePixel)
{
    EXPECT_EQ(Image::mipCapacity(1, 1), 1);
    EXPECT_EQ(Image::mipCapacity(2, 2), 2);
    EXPECT_EQ(Image::mipCapacity(8, 8), 4);
    EXPECT_EQ(Image::mipCapacity(16, 8), 5);
    EXPECT_EQ(Image::mipCapacity(7, 3), 3);
    EXPECT_EQ(Image::mipCapacity(8, 2), 4);
}

TEST(ImageMipCapacityTest, RejectsANonPositiveExtent)
{
    EXPECT_EQ(Image::mipCapacity(0, 8), 0);
    EXPECT_EQ(Image::mipCapacity(8, 0), 0);
    EXPECT_EQ(Image::mipCapacity(-1, 8), 0);
    EXPECT_EQ(Image::mipCapacity(0, 0), 0);
}

TEST(ImageTest, SingleMipIsOnePackedGrid)
{
    Image image(4, 3, PixelFormat::Rgba8Unorm);

    EXPECT_EQ(image.width(), 4);
    EXPECT_EQ(image.height(), 3);
    EXPECT_EQ(image.format(), PixelFormat::Rgba8Unorm);
    EXPECT_EQ(image.mipCount(), 1);
    EXPECT_EQ(image.mipWidth(0), 4);
    EXPECT_EQ(image.mipHeight(0), 3);
    EXPECT_EQ(image.mipByteSize(0), std::size_t{ 48 });
    EXPECT_EQ(image.totalByteSize(), std::size_t{ 48 });
    EXPECT_EQ(image.mipData(0).size(), std::size_t{ 48 });
}

TEST(ImageTest, MipLevelsHalveAndStopAtOnePixel)
{
    Image image(8, 4, PixelFormat::Rgba8Unorm, Image::mipCapacity(8, 4));

    ASSERT_EQ(image.mipCount(), 4);

    EXPECT_EQ(image.mipWidth(0), 8);
    EXPECT_EQ(image.mipHeight(0), 4);
    EXPECT_EQ(image.mipWidth(1), 4);
    EXPECT_EQ(image.mipHeight(1), 2);
    EXPECT_EQ(image.mipWidth(2), 2);
    EXPECT_EQ(image.mipHeight(2), 1);
    EXPECT_EQ(image.mipWidth(3), 1);
    EXPECT_EQ(image.mipHeight(3), 1);

    EXPECT_EQ(image.mipByteSize(0), std::size_t{ 128 });
    EXPECT_EQ(image.mipByteSize(1), std::size_t{ 32 });
    EXPECT_EQ(image.mipByteSize(2), std::size_t{ 8 });
    EXPECT_EQ(image.mipByteSize(3), std::size_t{ 4 });
    EXPECT_EQ(image.totalByteSize(), std::size_t{ 172 });
}

TEST(ImageTest, ANarrowImageClampsItsWidthToOnePixel)
{
    // 1x8 halves vertically only: the width cannot halve past a single pixel, and the level
    // below it is still one pixel rather than nothing.
    Image image(1, 8, PixelFormat::R8Unorm, 4);

    EXPECT_EQ(image.mipWidth(0), 1);
    EXPECT_EQ(image.mipHeight(0), 8);
    EXPECT_EQ(image.mipWidth(3), 1);
    EXPECT_EQ(image.mipHeight(3), 1);
    EXPECT_EQ(image.mipByteSize(3), std::size_t{ 1 });
    EXPECT_EQ(image.totalByteSize(), std::size_t{ 15 });
}

TEST(ImageTest, NonSquareExtentsHalveIndependently)
{
    Image image(5, 3, PixelFormat::R8Unorm, 2);

    EXPECT_EQ(image.mipWidth(1), 2);
    EXPECT_EQ(image.mipHeight(1), 1);
    EXPECT_EQ(image.mipByteSize(0), std::size_t{ 15 });
    EXPECT_EQ(image.mipByteSize(1), std::size_t{ 2 });
    EXPECT_EQ(image.totalByteSize(), std::size_t{ 17 });
}

TEST(ImageTest, MipLevelsAreContiguousInsideOneAllocation)
{
    Image image(8, 4, PixelFormat::Rgba8Unorm, 4);

    const std::byte* base   = image.mipData(0).data();
    std::size_t      offset = 0;

    for (int mip = 0; mip < image.mipCount(); ++mip) {
        EXPECT_EQ(image.mipData(mip).data(), base + offset) << "mip " << mip;
        EXPECT_EQ(image.mipData(mip).size(), image.mipByteSize(mip)) << "mip " << mip;
        offset += image.mipByteSize(mip);
    }

    EXPECT_EQ(offset, image.totalByteSize());
}

TEST(ImageTest, EveryPixelStartsZeroed)
{
    Image image(3, 2, PixelFormat::R8Unorm, 2);

    const Image& view = image;
    for (int mip = 0; mip < view.mipCount(); ++mip) {
        for (std::byte value : view.mipData(mip)) {
            ASSERT_EQ(std::to_integer<unsigned>(value), 0u) << "mip " << mip;
        }
    }
}

TEST(ImageTest, WritingOneMipDoesNotTouchItsNeighbours)
{
    Image image(8, 4, PixelFormat::Rgba8Unorm, 4);

    std::span<std::byte> level = image.mipData(1);
    ASSERT_EQ(level.size(), std::size_t{ 32 });

    level[0]                 = std::byte{ 0x5A };
    level[level.size() - 1]  = std::byte{ 0x5A };

    const Image& view = image;
    EXPECT_EQ(std::to_integer<unsigned>(view.mipData(1)[0]), 0x5Au);
    EXPECT_EQ(std::to_integer<unsigned>(view.mipData(1)[level.size() - 1]), 0x5Au);

    for (int mip : { 0, 2, 3 }) {
        for (std::byte value : view.mipData(mip)) {
            ASSERT_EQ(std::to_integer<unsigned>(value), 0u) << "mip " << mip;
        }
    }
}

TEST(ImageTest, AMipIndexOutOfRangeClampsToTheLastLevel)
{
    Image image(8, 4, PixelFormat::Rgba8Unorm, 4);

    EXPECT_EQ(image.mipWidth(99), image.mipWidth(3));
    EXPECT_EQ(image.mipHeight(99), image.mipHeight(3));
    EXPECT_EQ(image.mipByteSize(99), image.mipByteSize(3));
    EXPECT_EQ(image.mipData(99).data(), image.mipData(3).data());

    // A negative index clamps to the base level rather than reading before the allocation.
    EXPECT_EQ(image.mipWidth(-5), image.mipWidth(0));
    EXPECT_EQ(image.mipData(-5).data(), image.mipData(0).data());
}

TEST(ImageTest, RejectsANonPositiveSize)
{
    EXPECT_THROW(Image(0, 4, PixelFormat::Rgba8Unorm), std::invalid_argument);
    EXPECT_THROW(Image(4, 0, PixelFormat::Rgba8Unorm), std::invalid_argument);
    EXPECT_THROW(Image(-1, 4, PixelFormat::Rgba8Unorm), std::invalid_argument);
    EXPECT_THROW(Image(4, -1, PixelFormat::Rgba8Unorm), std::invalid_argument);
}

TEST(ImageTest, RejectsAFormatWithoutAPixelSize)
{
    EXPECT_THROW(Image(4, 4, PixelFormat::Unknown), std::invalid_argument);
    EXPECT_THROW(Image(4, 4, kBogusFormat), std::invalid_argument);
}

TEST(ImageTest, RejectsAMipCountOutsideTheChainsCapacity)
{
    // 8x4 holds exactly 4 levels, so 0, a negative, and 5 are all rejected while 1 and 4 are not.
    EXPECT_THROW(Image(8, 4, PixelFormat::Rgba8Unorm, 0), std::invalid_argument);
    EXPECT_THROW(Image(8, 4, PixelFormat::Rgba8Unorm, -1), std::invalid_argument);
    EXPECT_THROW(Image(8, 4, PixelFormat::Rgba8Unorm, 5), std::invalid_argument);

    EXPECT_NO_THROW(Image(8, 4, PixelFormat::Rgba8Unorm, 1));
    EXPECT_NO_THROW(Image(8, 4, PixelFormat::Rgba8Unorm, 4));
}

TEST(ImageTest, IsAReferenceCountedObject)
{
    intrusive_ptr<Image> image(new Image(2, 2, PixelFormat::R8Unorm));

    ASSERT_NE(Image::desc(), nullptr);
    EXPECT_EQ(image->getType(), Image::desc());
    EXPECT_EQ(image->useCount(), 1u);

    {
        intrusive_ptr<Image> second(image);
        EXPECT_EQ(second.get(), image.get());
        EXPECT_EQ(image->useCount(), 2u);
    }

    EXPECT_EQ(image->useCount(), 1u);
}
