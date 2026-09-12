/**
 * @brief Device-free tests of the texture cache's decisions.
 *
 * The cache builds vsg descriptions (image, image view, sampler) and never touches a device, so every rule it
 * follows is assertable here: when it falls back, when it rebuilds, when it lets go. The upload itself is
 * covered by the device self-test's pixel assertions — but those cannot reach the DECISIONS, because a cache
 * that rebuilt on every call and one that never rebuilt draw exactly the same picture. That is the gap this
 * suite exists to close.
 */

#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/vsg/VsgTextureCache.hpp>

#include <gtest/gtest.h>

#include <cstddef>

using vine::graphics::CubeMap;
using vine::graphics::Texture;
using vine::graphics::Texture2D;
using vine::imaging::Image;
using vine::imaging::PixelFormat;
using vine::vsg::detail::TextureReject;
using vine::vsg::VsgTextureCache;

namespace
{

/// @brief Builds a filled single-level 2D texture, the shape the cache accepts.
vine::intrusive_ptr<Texture2D> readyTexture()
{
    auto texture = vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, PixelFormat::Rgba8Unorm));
    texture->setImage(vine::intrusive_ptr<const Image>(new Image(4, 4, PixelFormat::Rgba8Unorm)));
    return texture;
}

/// @brief Builds an image that matches @p texture's description.
vine::intrusive_ptr<const Image> sourceFor(const Texture& texture)
{
    return vine::intrusive_ptr<const Image>(
        new Image(texture.width(), texture.height(), texture.format()));
}

} // namespace

TEST(TextureCacheTest, AMissingTextureYieldsTheSharedWhiteFallback)
{
    VsgTextureCache cache;
    TextureReject   reason = TextureReject::Ok;

    const auto info = cache.getOrCreate(nullptr, reason);

    // Absence is not an error: the descriptor a shader samples must be bound to something, so a material
    // without a texture samples white and the caller is told why.
    EXPECT_EQ(reason, TextureReject::Absent);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info.get(), cache.whiteFallback().get()) << "one shared fallback, not one per call";
    EXPECT_EQ(cache.count(), 0u) << "the fallback is not a cached texture";
}

TEST(TextureCacheTest, AnUnfinishedOrUnrepresentableTextureFallsBack)
{
    VsgTextureCache cache;

    auto half_cube = vine::intrusive_ptr<CubeMap>(new CubeMap(4, PixelFormat::Rgba8Unorm));
    half_cube->setFaceImage(CubeMap::Face::PosX, sourceFor(*half_cube));

    TextureReject reason = TextureReject::Ok;
    EXPECT_EQ(cache.getOrCreate(half_cube.get(), reason).get(), cache.whiteFallback().get());
    EXPECT_EQ(reason, TextureReject::Incomplete);

    // A three-channel layout has no Vulkan format, so it is refused for its format rather than uploaded.
    auto three_channel =
        vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, PixelFormat::Rgb8Unorm));
    three_channel->setImage(vine::intrusive_ptr<const Image>(new Image(4, 4, PixelFormat::Rgb8Unorm)));
    EXPECT_EQ(cache.getOrCreate(three_channel.get(), reason).get(), cache.whiteFallback().get());
    EXPECT_EQ(reason, TextureReject::UnsupportedFormat);
}

TEST(TextureCacheTest, TheSameUnchangedTextureIsBuiltOnce)
{
    VsgTextureCache cache;
    auto            texture = readyTexture();
    TextureReject   reason  = TextureReject::Ok;

    const auto first  = cache.getOrCreate(texture.get(), reason);
    const auto second = cache.getOrCreate(texture.get(), reason);

    EXPECT_EQ(reason, TextureReject::Ok);
    // Identity, not equality: rebuilding an identical image every call would look the same on screen and
    // would re-upload the texture every frame.
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(cache.count(), 1u);
    EXPECT_TRUE(cache.has(texture.get()));
}

TEST(TextureCacheTest, RefillingATextureRebuildsItsResources)
{
    VsgTextureCache cache;
    auto            texture = readyTexture();

    TextureReject reason = TextureReject::Ok;
    const auto    before = cache.getOrCreate(texture.get(), reason);

    // The address is the same, so without the revision the cache would keep serving the OLD pixels.
    texture->setImage(sourceFor(*texture));
    const auto after = cache.getOrCreate(texture.get(), reason);

    EXPECT_NE(before.get(), after.get());
    EXPECT_EQ(cache.count(), 1u) << "a rebuild replaces the entry rather than adding one";
}

TEST(TextureCacheTest, AnEntryKeepsItsTextureAliveUntilItIsAbandoned)
{
    VsgTextureCache cache;
    auto            texture = readyTexture();

    TextureReject reason = TextureReject::Ok;
    (void)cache.getOrCreate(texture.get(), reason);

    // Dropping the app's reference must not dangle: the entry holds the texture it is keyed by, which is what
    // makes the address usable as a key in the first place.
    const auto* address = texture.get();
    texture             = nullptr;
    EXPECT_TRUE(cache.has(address));
    EXPECT_EQ(cache.releaseAbandoned(), 1u) << "the cache was the only owner left";
    EXPECT_EQ(cache.count(), 0u);
    EXPECT_FALSE(cache.has(address));
}

TEST(TextureCacheTest, ATextureStillInUseIsNotAbandoned)
{
    VsgTextureCache cache;
    auto            texture = readyTexture();

    TextureReject reason = TextureReject::Ok;
    (void)cache.getOrCreate(texture.get(), reason);

    EXPECT_EQ(cache.releaseAbandoned(), 0u) << "the app still holds a reference";
    EXPECT_EQ(cache.count(), 1u);
}

TEST(TextureCacheTest, ClearDropsEveryEntryAndTheFallback)
{
    VsgTextureCache cache;
    auto            texture = readyTexture();

    TextureReject reason = TextureReject::Ok;
    (void)cache.getOrCreate(texture.get(), reason);
    const auto fallback = cache.whiteFallback();

    cache.clear();

    EXPECT_EQ(cache.count(), 0u);
    // A new fallback, so nothing the caller kept from the previous session is silently reused.
    EXPECT_NE(cache.whiteFallback().get(), fallback.get());
}

TEST(TextureCacheTest, TheOldestEntryIsTrimmedAtCapacity)
{
    // The bound is what stops a long session from growing: the cap is a constant of the cache's own making
    // (kMaxEntries), restated here so a change to it has to be a deliberate act rather than a silent one.
    constexpr std::size_t kCap      = 256u;
    constexpr std::size_t kOverflow = kCap + 1u;

    VsgTextureCache                          cache;
    std::vector<vine::intrusive_ptr<Texture>> textures;
    textures.reserve(kOverflow);
    for (std::size_t i = 0; i < kOverflow; ++i) {
        textures.push_back(readyTexture());
    }

    TextureReject reason = TextureReject::Ok;
    for (const auto& texture : textures) {
        EXPECT_NE(cache.getOrCreate(texture.get(), reason), nullptr);
    }

    EXPECT_EQ(cache.count(), kCap) << "the cap is a bound, not a target";
    EXPECT_FALSE(cache.has(textures.front().get())) << "the oldest went first";
    EXPECT_TRUE(cache.has(textures.back().get()));
}
