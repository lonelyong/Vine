#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <vine/intrusive_ptr.hpp>

#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>
#include <vine/imageio/ImageCodec.hpp>

using vine::imaging::bytesPerPixel;
using vine::imaging::Image;
using vine::imaging::PixelFormat;
using vine::imageio::canRead;
using vine::imageio::canWrite;
using vine::imageio::decodeImage;
using vine::imageio::encodeImage;
using vine::imageio::formatFromPath;
using vine::imageio::formatName;
using vine::imageio::ImageFileFormat;
using vine::imageio::loadImage;
using vine::imageio::saveImage;
using vine::intrusive_ptr;

namespace
{

/// Every layout a container can hold, including the sRGB-encoded ones (whose bytes are the same).
///
/// `Rg8Unorm` is deliberately not here: a container's two channels mean grey + alpha, and refusing the
/// mismatch is the codec's documented behaviour (see the test that pins it).
const PixelFormat kLayouts[] = {
    PixelFormat::R8Unorm,    PixelFormat::R8Srgb,     PixelFormat::Rgb8Unorm,  PixelFormat::Rgb8Srgb,
    PixelFormat::Rgba8Unorm, PixelFormat::Rgba8Srgb,  PixelFormat::Bgra8Unorm, PixelFormat::Bgra8Srgb,
};

/// Every container this module writes.
const ImageFileFormat kContainers[] = { ImageFileFormat::Png, ImageFileFormat::Bmp, ImageFileFormat::Tga };

/**
 * @brief Gets the path of an asset under the source tree's test_data/.
 *
 * @param relative Path below test_data/.
 * @return The absolute path of the asset.
 */
std::filesystem::path assetPath(const std::string& relative)
{
    return std::filesystem::path(VINE_TEST_DATA_DIR) / relative;
}

/**
 * @brief Creates an image and fills it with a deterministic pattern.
 *
 * Every channel of every pixel gets a different value, so a codec that shuffles or drops channels (BGRA vs
 * RGBA, a collapsed alpha) cannot round-trip by accident.
 *
 * @param width     Width in pixels.
 * @param height    Height in pixels.
 * @param format    Pixel layout.
 * @param mip_count Number of mip levels.
 * @return The filled image.
 */
intrusive_ptr<Image> patternImage(int width, int height, PixelFormat format, int mip_count = 1)
{
    auto image = intrusive_ptr<Image>(new Image(width, height, format, mip_count));

    const int channels = static_cast<int>(bytesPerPixel(format));

    for (int mip = 0; mip < mip_count; ++mip) {
        std::span<std::byte> pixels = image->mipData(mip);
        for (std::size_t p = 0; p + static_cast<std::size_t>(channels) <= pixels.size(); p += static_cast<std::size_t>(channels)) {
            for (int c = 0; c < channels; ++c) {
                // Varies with the pixel (so rows differ) and with the channel (so a swap is visible).
                pixels[p + static_cast<std::size_t>(c)] = std::byte{ static_cast<unsigned char>((p * 7 + static_cast<std::size_t>(c) * 101 + mip * 13) & 0xFFu) };
            }
        }
    }

    return image;
}

/**
 * @brief Copies a mip level's bytes out of an image.
 *
 * @param image Image to read.
 * @param mip   Mip level.
 * @return The level's bytes.
 */
std::vector<std::byte> bytesOf(const Image& image, int mip = 0)
{
    const std::span<const std::byte> level = image.mipData(mip);
    return std::vector<std::byte>(level.begin(), level.end());
}

} // namespace

// ============ Containers ============

TEST(ImageFileFormatTest, NamesItselfForDiagnostics)
{
    EXPECT_STREQ(formatName(ImageFileFormat::Png), "PNG");
    EXPECT_STREQ(formatName(ImageFileFormat::Jpeg), "JPEG");
    EXPECT_STREQ(formatName(ImageFileFormat::Bmp), "BMP");
    EXPECT_STREQ(formatName(ImageFileFormat::Tga), "TGA");
    EXPECT_STREQ(formatName(ImageFileFormat::Unknown), "Unknown");
    EXPECT_STREQ(formatName(static_cast<ImageFileFormat>(200)), "Unknown");
}

TEST(ImageFileFormatTest, ReadsEveryContainerItAdvertisesAndWritesThree)
{
    EXPECT_TRUE(canRead(ImageFileFormat::Png));
    EXPECT_TRUE(canRead(ImageFileFormat::Jpeg));
    EXPECT_TRUE(canRead(ImageFileFormat::Bmp));
    EXPECT_TRUE(canRead(ImageFileFormat::Tga));
    EXPECT_FALSE(canRead(ImageFileFormat::Unknown));

    EXPECT_TRUE(canWrite(ImageFileFormat::Png));
    EXPECT_TRUE(canWrite(ImageFileFormat::Bmp));
    EXPECT_TRUE(canWrite(ImageFileFormat::Tga));
    EXPECT_FALSE(canWrite(ImageFileFormat::Jpeg)) << "writing JPEG is lossy and needs a quality parameter";
    EXPECT_FALSE(canWrite(ImageFileFormat::Unknown));
}

TEST(ImageFileFormatTest, ReadsTheFormatOutOfAPathsExtension)
{
    EXPECT_EQ(formatFromPath("frame.png"), ImageFileFormat::Png);
    EXPECT_EQ(formatFromPath("frame.PNG"), ImageFileFormat::Png);
    EXPECT_EQ(formatFromPath("/a/b/frame.PnG"), ImageFileFormat::Png);
    EXPECT_EQ(formatFromPath("frame.jpg"), ImageFileFormat::Jpeg);
    EXPECT_EQ(formatFromPath("frame.jpeg"), ImageFileFormat::Jpeg);
    EXPECT_EQ(formatFromPath("frame.JPEG"), ImageFileFormat::Jpeg);
    EXPECT_EQ(formatFromPath("frame.bmp"), ImageFileFormat::Bmp);
    EXPECT_EQ(formatFromPath("frame.tga"), ImageFileFormat::Tga);
}

TEST(ImageFileFormatTest, AnUnrecognisedExtensionIsUnknown)
{
    EXPECT_EQ(formatFromPath("frame.tiff"), ImageFileFormat::Unknown);
    EXPECT_EQ(formatFromPath("frame"), ImageFileFormat::Unknown);
    EXPECT_EQ(formatFromPath(""), ImageFileFormat::Unknown);
}

TEST(ImageFileFormatTest, APathWithNoExtensionIsNotMistakenForOne)
{
    // A directory named "shots.png" holding "frame" must not make "frame" a PNG.
    EXPECT_EQ(formatFromPath("/shots.png/frame"), ImageFileFormat::Unknown);
    EXPECT_EQ(formatFromPath(".png"), ImageFileFormat::Unknown) << "a dotfile has no extension";
}

// ============ Decoding the real assets ============

TEST(ImageDecodeTest, DecodesARealJpegCubeFace)
{
    const auto image = loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgba8Unorm);

    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->width(), 2048);
    EXPECT_EQ(image->height(), 2048);
    EXPECT_EQ(image->format(), PixelFormat::Rgba8Unorm);
    EXPECT_EQ(image->mipCount(), 1) << "a container holds one image; the chain above it is not generated here";
    EXPECT_EQ(image->totalByteSize(), std::size_t{ 2048 } * 2048 * 4);
}

TEST(ImageDecodeTest, DecodesEveryAssetUnderBothNamingSets)
{
    // The point is that the whole asset set is usable, not just the file one test happens to pick: a
    // cubemap test will need all six faces of a set to decode.
    const char* const files[] = {
        "images/posx.jpg", "images/negx.jpg", "images/posy.jpg",
        "images/negy.jpg", "images/posz.jpg", "images/negz.jpg",
        "images/front.jpg", "images/back.jpg", "images/left.jpg",
        "images/right.jpg", "images/top.jpg", "images/bottom.jpg",
    };

    for (const char* file : files) {
        SCOPED_TRACE(file);

        const std::filesystem::path path = assetPath(file);
        ASSERT_TRUE(std::filesystem::exists(path)) << "missing test asset: " << path.string();
        EXPECT_EQ(formatFromPath(path), ImageFileFormat::Jpeg);

        const auto image = loadImage(path, PixelFormat::Rgba8Unorm);
        EXPECT_EQ(image->width(), 2048);
        EXPECT_EQ(image->height(), 2048);
    }
}

TEST(ImageDecodeTest, TheRequestedLayoutDecidesTheChannelCountNotTheFile)
{
    // A JPEG stores three channels, but the caller asks for what it needs: that is how decoded pixels
    // become a layout a GPU actually has.
    EXPECT_EQ(loadImage(assetPath("images/posx.jpg"), PixelFormat::R8Unorm)->totalByteSize(), std::size_t{ 2048 } * 2048);
    EXPECT_EQ(loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgb8Unorm)->totalByteSize(), std::size_t{ 2048 } * 2048 * 3);
    EXPECT_EQ(loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgba8Unorm)->totalByteSize(), std::size_t{ 2048 } * 2048 * 4);
    EXPECT_EQ(loadImage(assetPath("images/posx.jpg"), PixelFormat::Bgra8Unorm)->totalByteSize(), std::size_t{ 2048 } * 2048 * 4);
}

TEST(ImageDecodeTest, ASingleChannelIsLumaAndNotTheSourcesRed)
{
    // Pinned on a LOSSLESS container holding content this test owns, so the numbers have to agree exactly.
    //
    // Not on the JPEG assets: over JPEG the decoder computes luma from its own intermediate components
    // before they are clamped, so recomputing luma out of the decoded RGB bytes can differ from it by a
    // step. That is a property of the decoder, not of this API, and asserting it would pin the wrong thing.
    const auto colour_image = patternImage(16, 4, PixelFormat::Rgb8Unorm);
    const std::vector<std::byte> png = encodeImage(*colour_image, ImageFileFormat::Png);

    const auto grey = decodeImage(png, PixelFormat::R8Unorm);

    const std::span<const std::byte> colour = colour_image->mipData(0);
    const std::span<const std::byte> luma   = grey->mipData(0);

    // Rec.601 with the decoder's own weights, spelled out here so the numbers have to agree exactly.
    const auto expectedLuma = [](unsigned r, unsigned g, unsigned b) noexcept { return (r * 77 + g * 150 + b * 29) >> 8; };

    bool found_colour = false;
    for (std::size_t p = 0; p * 3 + 2 < colour.size(); ++p) {
        const unsigned r = std::to_integer<unsigned>(colour[p * 3 + 0]);
        const unsigned g = std::to_integer<unsigned>(colour[p * 3 + 1]);
        const unsigned b = std::to_integer<unsigned>(colour[p * 3 + 2]);

        EXPECT_EQ(std::to_integer<unsigned>(luma[p]), expectedLuma(r, g, b)) << "pixel " << p;

        found_colour = found_colour || (r != g) || (g != b);
    }

    EXPECT_TRUE(found_colour) << "the sample must hold a non-grey pixel, or luma and red are indistinguishable";
}

TEST(ImageDecodeTest, ASingleChannelOfARealJpegIsNotACopyOfItsRedChannel)
{
    const auto rgb  = loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgb8Unorm);
    const auto grey = loadImage(assetPath("images/posx.jpg"), PixelFormat::R8Unorm);

    const std::span<const std::byte> colour = rgb->mipData(0);
    const std::span<const std::byte> luma   = grey->mipData(0);

    bool differs = false;
    for (std::size_t p = 0; p < 4096 && !differs; ++p) {
        differs = luma[p] != colour[p * 3 + 0];
    }

    EXPECT_TRUE(differs) << "greyscale conversion of a colour image must not equal its red channel";
}

TEST(ImageDecodeTest, RefusesTwoChannelLayoutsBecauseTwoChannelsInAFileMeanGreyPlusAlpha)
{
    const std::vector<std::byte> encoded = encodeImage(*patternImage(4, 4, PixelFormat::Rgba8Unorm), ImageFileFormat::Png);

    EXPECT_THROW(decodeImage(encoded, PixelFormat::Rg8Unorm), std::invalid_argument)
        << "Rg8Unorm means red + green; answering it with luma and alpha would be a lie";
}

TEST(ImageDecodeTest, DecodingIntoThreeOrFourChannelsGivesTheSameColourChannels)
{
    const auto rgb  = loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgb8Unorm);
    const auto rgba = loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgba8Unorm);

    const std::span<const std::byte> packed = rgb->mipData(0);
    const std::span<const std::byte> wide   = rgba->mipData(0);

    // Sampled rather than compared whole: enough to pin the channel order without walking 12 MB.
    for (const std::size_t pixel : { std::size_t{ 0 }, std::size_t{ 1 }, std::size_t{ 4095 } }) {
        for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_EQ(packed[pixel * 3 + c], wide[pixel * 4 + c]) << "pixel " << pixel << " channel " << c;
        }
        EXPECT_EQ(std::to_integer<unsigned>(wide[pixel * 4 + 3]), 255u) << "the source has no alpha to preserve";
    }
}

TEST(ImageDecodeTest, BgraIsRgbaWithRedAndBlueSwapped)
{
    const auto rgba = loadImage(assetPath("images/posx.jpg"), PixelFormat::Rgba8Unorm);
    const auto bgra = loadImage(assetPath("images/posx.jpg"), PixelFormat::Bgra8Unorm);

    ASSERT_EQ(rgba->totalByteSize(), bgra->totalByteSize());

    const std::span<const std::byte> wide = rgba->mipData(0);
    const std::span<const std::byte> swapped = bgra->mipData(0);

    for (const std::size_t pixel : { std::size_t{ 0 }, std::size_t{ 1234 } }) {
        EXPECT_EQ(swapped[pixel * 4 + 0], wide[pixel * 4 + 2]) << "blue takes red's place";
        EXPECT_EQ(swapped[pixel * 4 + 1], wide[pixel * 4 + 1]) << "green is untouched";
        EXPECT_EQ(swapped[pixel * 4 + 2], wide[pixel * 4 + 0]) << "red takes blue's place";
        EXPECT_EQ(swapped[pixel * 4 + 3], wide[pixel * 4 + 3]) << "alpha is untouched";
    }

    // The swizzle must not have been a no-op, or the assertions above would be vacuous: at least one
    // pixel has to actually have different red and blue.
    bool swizzle_is_observable = false;
    for (std::size_t p = 0; p * 4 + 2 < wide.size(); ++p) {
        if (wide[p * 4 + 0] != wide[p * 4 + 2]) {
            swizzle_is_observable = true;
            break;
        }
    }
    EXPECT_TRUE(swizzle_is_observable) << "red and blue differ somewhere, so the swap is observable";
}

// ============ Round trips ============

TEST(ImageEncodeTest, EveryWritableContainerRoundTripsEveryLayoutExactly)
{
    // PNG, BMP and TGA are all lossless, so an exact byte comparison is the right assertion — anything
    // weaker would let a channel shuffle or a truncated level through.
    for (const ImageFileFormat container : kContainers) {
        for (const PixelFormat layout : kLayouts) {
            const std::string label = std::string(formatName(container)) + "/" + vine::imaging::formatName(layout);
            SCOPED_TRACE(label);

            const auto source = patternImage(7, 5, layout);

            const std::vector<std::byte> encoded = encodeImage(*source, container);
            ASSERT_FALSE(encoded.empty());

            const auto decoded = decodeImage(encoded, layout);

            EXPECT_EQ(decoded->width(), 7);
            EXPECT_EQ(decoded->height(), 5);
            EXPECT_EQ(decoded->format(), layout);
            EXPECT_EQ(decoded->mipCount(), 1);
            EXPECT_EQ(bytesOf(*decoded), bytesOf(*source));
        }
    }
}

TEST(ImageEncodeTest, ABgraRoundTripIsNotSilentlyRebuiltAsRgba)
{
    const auto source = patternImage(4, 2, PixelFormat::Bgra8Unorm);

    const std::vector<std::byte> encoded = encodeImage(*source, ImageFileFormat::Png);

    // Decoded back as RGBA, the channels must come out swapped relative to the BGRA source: that is what
    // proves the encoder really wrote the red and blue the caller put there, rather than swapping twice.
    const auto as_rgba = decodeImage(encoded, PixelFormat::Rgba8Unorm);
    const auto as_bgra = decodeImage(encoded, PixelFormat::Bgra8Unorm);

    const std::vector<std::byte> source_bytes = bytesOf(*source);

    EXPECT_EQ(bytesOf(*as_bgra), source_bytes);
    EXPECT_NE(bytesOf(*as_rgba), bytesOf(*as_bgra)) << "reading it back as RGBA must not hide the swap";

    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        EXPECT_EQ(as_rgba->mipData(0)[pixel * 4 + 0], as_bgra->mipData(0)[pixel * 4 + 2]);
        EXPECT_EQ(as_rgba->mipData(0)[pixel * 4 + 2], as_bgra->mipData(0)[pixel * 4 + 0]);
    }
}

TEST(ImageEncodeTest, OnlyTheBaseLevelIsWritten)
{
    const auto source = patternImage(8, 8, PixelFormat::Rgba8Unorm, 4);
    ASSERT_EQ(source->mipCount(), 4);

    const auto decoded = decodeImage(encodeImage(*source, ImageFileFormat::Png), PixelFormat::Rgba8Unorm);

    EXPECT_EQ(decoded->mipCount(), 1) << "PNG cannot carry a chain, so the base level is a level-1 image";
    EXPECT_EQ(decoded->totalByteSize(), source->mipByteSize(0));
    EXPECT_EQ(bytesOf(*decoded), bytesOf(*source, 0));
}

TEST(ImageEncodeTest, SavesAndLoadsThroughARealFile)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vine_imageio_roundtrip.png";
    std::filesystem::remove(path);

    const auto source = patternImage(9, 4, PixelFormat::Rgba8Unorm);
    saveImage(path, *source, ImageFileFormat::Png);

    ASSERT_TRUE(std::filesystem::exists(path));

    const auto loaded = loadImage(path, PixelFormat::Rgba8Unorm);
    EXPECT_EQ(loaded->width(), 9);
    EXPECT_EQ(loaded->height(), 4);
    EXPECT_EQ(bytesOf(*loaded), bytesOf(*source));

    std::filesystem::remove(path);
}

// ============ Refusals ============

TEST(ImageDecodeTest, RefusesALayoutAContainerCannotHold)
{
    const std::vector<std::byte> encoded = encodeImage(*patternImage(4, 4, PixelFormat::Rgba8Unorm), ImageFileFormat::Png);

    EXPECT_THROW(decodeImage(encoded, PixelFormat::Rgba16Float), std::invalid_argument);
    EXPECT_THROW(decodeImage(encoded, PixelFormat::R32Float), std::invalid_argument);
    EXPECT_THROW(decodeImage(encoded, PixelFormat::D32Float), std::invalid_argument);
    EXPECT_THROW(decodeImage(encoded, PixelFormat::Unknown), std::invalid_argument);
}

TEST(ImageDecodeTest, RefusesAnEmptyBuffer)
{
    EXPECT_THROW(decodeImage({}, PixelFormat::Rgba8Unorm), std::invalid_argument);
}

TEST(ImageDecodeTest, ReportsUndecodableBytesAsARuntimeError)
{
    const std::vector<std::byte> garbage(64, std::byte{ 0x7F });
    EXPECT_THROW(decodeImage(garbage, PixelFormat::Rgba8Unorm), std::runtime_error);
}

TEST(ImageDecodeTest, ReportsATruncatedImageAsARuntimeError)
{
    std::vector<std::byte> encoded = encodeImage(*patternImage(16, 16, PixelFormat::Rgba8Unorm), ImageFileFormat::Png);
    ASSERT_GT(encoded.size(), 16u);

    // Keep the header (so the container is recognised) and drop the pixel data.
    encoded.resize(encoded.size() / 3);
    EXPECT_THROW(decodeImage(encoded, PixelFormat::Rgba8Unorm), std::runtime_error);
}

TEST(ImageDecodeTest, ReportsAMissingFileAsARuntimeError)
{
    EXPECT_THROW(loadImage(assetPath("images/does_not_exist.png"), PixelFormat::Rgba8Unorm), std::runtime_error);
}

TEST(ImageDecodeTest, ReportsAnEmptyFileAsARuntimeError)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vine_imageio_empty.png";
    {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(static_cast<bool>(create));
    }

    EXPECT_THROW(loadImage(path, PixelFormat::Rgba8Unorm), std::runtime_error);

    std::filesystem::remove(path);
}

TEST(ImageEncodeTest, RefusesAContainerItCannotWrite)
{
    const auto source = patternImage(4, 4, PixelFormat::Rgba8Unorm);

    EXPECT_THROW(encodeImage(*source, ImageFileFormat::Jpeg), std::invalid_argument);
    EXPECT_THROW(encodeImage(*source, ImageFileFormat::Unknown), std::invalid_argument);
    EXPECT_THROW(encodeImage(*source, static_cast<ImageFileFormat>(200)), std::invalid_argument);
}

TEST(ImageEncodeTest, RefusesAnImageALayoutAContainerCannotHold)
{
    const auto float_image = intrusive_ptr<Image>(new Image(4, 4, PixelFormat::Rgba16Float));
    const auto two_channel = intrusive_ptr<Image>(new Image(4, 4, PixelFormat::Rg8Unorm));

    EXPECT_THROW(encodeImage(*float_image, ImageFileFormat::Png), std::invalid_argument);
    EXPECT_THROW(encodeImage(*two_channel, ImageFileFormat::Png), std::invalid_argument);
}

TEST(ImageEncodeTest, ReportsAnUnwritablePathAsARuntimeError)
{
    const std::filesystem::path path = assetPath("images/this_directory_does_not_exist/out.png");
    const auto                source = patternImage(4, 4, PixelFormat::Rgba8Unorm);

    EXPECT_THROW(saveImage(path, *source, ImageFileFormat::Png), std::runtime_error);
}
