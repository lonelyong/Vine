/**
 * @brief Device-free tests of the material image cache's decisions.
 *
 * The cache builds vsg create-infos (image, image view, sampler) and never touches a device, so every rule
 * it follows is assertable here: when it falls back, when it rebuilds, when it lets go. The UPLOAD and the
 * sampling are covered by the device cases in ContentPassTest — but those cannot reach the DECISIONS,
 * because a cache that rebuilt on every acquire and one that never rebuilt draw exactly the same picture.
 * That is the gap this suite exists to close.
 *
 * The cube's IMAGE is where a wrong build is silent rather than loud: the layers can be staged in an order
 * that every copy region accepts (the byte count is the same) and that leaves each face holding another
 * face's texels. The order and the view type are therefore asserted here (the data's depth and view type
 * are what vsg derives the copy regions and the view from), and the FACE COLORS are asserted by pixels in
 * the device case.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>

#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>

#include <vine/vsg/api/MaterialImages.hpp>

using vine::graphics::CubeMap;
using vine::graphics::Texture;
using vine::graphics::Texture2D;
using vine::imaging::Image;
using vine::imaging::PixelFormat;
using vine::vsg::detail::TextureReject;
using vine::vsg::MaterialImages;
using vine::vsg::SamplerImage;

namespace
{

/// @brief Builds a filled, single-level 2D texture: the shape the cache accepts.
vine::intrusive_ptr<Texture2D> readyTexture(int width = 4, int height = 4)
{
    auto texture = vine::intrusive_ptr<Texture2D>(new Texture2D(width, height, PixelFormat::Rgba8Unorm));
    texture->setImage(vine::intrusive_ptr<const Image>(new Image(width, height, PixelFormat::Rgba8Unorm)));
    return texture;
}

/// @brief Builds a cube with every face filled, one texel each.
vine::intrusive_ptr<CubeMap> readyCube()
{
    auto cube = vine::intrusive_ptr<CubeMap>(new CubeMap(1, PixelFormat::Rgba8Unorm));
    for (int face = 0; face < cube->faceCount(); ++face) {
        cube->setSource(face, vine::intrusive_ptr<const Image>(new Image(1, 1, PixelFormat::Rgba8Unorm)));
    }
    return cube;
}

/// @brief Gets the view type a fallback or an uploaded texture is declared with.
VkImageViewType viewTypeOf(const SamplerImage& images)
{
    return images.view != nullptr ? images.view->viewType : VK_IMAGE_VIEW_TYPE_MAX_ENUM;
}

}  // namespace

TEST(MaterialImagesTest, AMissingTextureYieldsTheSharedWhiteFallback)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    TextureReject reason = TextureReject::Ok;
    const SamplerImage images = cache->acquire(nullptr, reason);

    // Absence is not an error: the descriptor a shader samples must be bound to something, so a material
    // without a texture samples white and the caller is told why.
    EXPECT_EQ(reason, TextureReject::Absent);
    ASSERT_NE(images.view, nullptr);
    ASSERT_NE(images.sampler, nullptr);
    EXPECT_EQ(images.view.get(), cache->white().view.get()) << "one shared fallback, not one per call";
    EXPECT_EQ(cache->count(), 0U) << "the fallback is not a cached texture";
}

TEST(MaterialImagesTest, AnUnfinishedTextureNeverCachesItsImages)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    // Described but not filled: the backend cannot upload what is not there, and the texture is still a
    // normal intermediate state rather than a caller's mistake.
    const vine::intrusive_ptr<Texture2D> texture(new Texture2D(4, 4, PixelFormat::Rgba8Unorm));

    TextureReject      reason = TextureReject::Ok;
    const SamplerImage images = cache->acquire(texture.get(), reason);

    EXPECT_EQ(reason, TextureReject::Incomplete);
    EXPECT_EQ(images.view.get(), cache->white().view.get());
    EXPECT_EQ(cache->count(), 0U) << "an incomplete texture is not an entry";
}

TEST(MaterialImagesTest, AFallbackFollowsTheTextureKind)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    // A cube sampler cannot bind a 2D view: a 2D view where the text declares `samplerCube` is an invalid
    // descriptor, not an untextured draw. The fallback therefore follows the texture's own kind.
    const vine::intrusive_ptr<CubeMap> unfinished_cube(new CubeMap(1, PixelFormat::Rgba8Unorm));
    TextureReject                      reason = TextureReject::Ok;
    const SamplerImage                 cube   = cache->acquire(unfinished_cube.get(), reason);
    EXPECT_EQ(reason, TextureReject::Incomplete);
    EXPECT_EQ(viewTypeOf(cube), VK_IMAGE_VIEW_TYPE_CUBE);
    EXPECT_EQ(cube.view.get(), cache->whiteCube().view.get());
    EXPECT_NE(cube.view.get(), cache->white().view.get()) << "the two fallbacks are different images";

    const vine::intrusive_ptr<Texture2D> unfinished_2d(new Texture2D(1, 1, PixelFormat::Rgba8Unorm));
    const SamplerImage                   flat = cache->acquire(unfinished_2d.get(), reason);
    EXPECT_EQ(viewTypeOf(flat), VK_IMAGE_VIEW_TYPE_2D);
}

TEST(MaterialImagesTest, AFormatWithoutAVulkanCounterpartFallsBack)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    // Three channels have no Vulkan format: the caller chose a pixel layout this backend cannot sample, and
    // the answer is the fallback plus the reason, never a descriptor over an image nothing can read.
    auto texture = vine::intrusive_ptr<Texture2D>(new Texture2D(2, 2, PixelFormat::Rgb8Unorm));
    texture->setImage(vine::intrusive_ptr<const Image>(new Image(2, 2, PixelFormat::Rgb8Unorm)));

    TextureReject      reason = TextureReject::Ok;
    const SamplerImage images = cache->acquire(texture.get(), reason);

    EXPECT_EQ(reason, TextureReject::UnsupportedFormat);
    EXPECT_EQ(images.view.get(), cache->white().view.get());
    EXPECT_EQ(cache->count(), 0U);
}

TEST(MaterialImagesTest, TheSameUnchangedTextureIsBuiltOnce)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    const vine::intrusive_ptr<Texture2D> texture = readyTexture();

    TextureReject reason = TextureReject::Ok;
    const auto    first  = cache->acquire(texture.get(), reason);
    ASSERT_EQ(reason, TextureReject::Ok);
    ASSERT_NE(first.view, nullptr);
    EXPECT_TRUE(cache->has(texture.get()));
    EXPECT_EQ(cache->count(), 1U);

    const auto second = cache->acquire(texture.get(), reason);
    EXPECT_EQ(second.view.get(), first.view.get()) << "the same texture is not rebuilt";
    EXPECT_EQ(second.sampler.get(), first.sampler.get());
    EXPECT_EQ(cache->count(), 1U);
}

TEST(MaterialImagesTest, RefillingATextureRebuildsItsImages)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    const vine::intrusive_ptr<Texture2D> texture = readyTexture();
    TextureReject                        reason  = TextureReject::Ok;
    const auto                           before  = cache->acquire(texture.get(), reason);
    ASSERT_NE(before.view, nullptr);

    // The address is the same, so only the revision says the pixels changed: without it the entry would
    // keep serving the image built from the old texels.
    texture->setImage(vine::intrusive_ptr<const Image>(new Image(4, 4, PixelFormat::Rgba8Unorm)));
    const auto after = cache->acquire(texture.get(), reason);
    EXPECT_EQ(reason, TextureReject::Ok);
    ASSERT_NE(after.view, nullptr);
    EXPECT_NE(after.view.get(), before.view.get()) << "a re-filled texture is rebuilt";
    EXPECT_EQ(cache->count(), 1U) << "and it replaces its entry rather than joining it";
}

TEST(MaterialImagesTest, AnEntryKeepsItsTextureAliveUntilItIsAbandoned)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    auto          texture = readyTexture();  // the app's own reference, dropped below
    TextureReject reason  = TextureReject::Ok;
    const auto    images  = cache->acquire(texture.get(), reason);
    ASSERT_NE(images.view, nullptr);
    ASSERT_EQ(cache->count(), 1U);

    // While the app holds the texture the entry is a retained share of a live object.
    EXPECT_EQ(cache->releaseAbandoned(), 0U);
    EXPECT_TRUE(cache->has(texture.get()));

    const Texture* address = texture.get();
    texture.reset();  // the cache is the only owner left

    EXPECT_EQ(cache->releaseAbandoned(), 1U) << "nothing can look the entry up again";
    EXPECT_FALSE(cache->has(address));
    EXPECT_EQ(cache->count(), 0U);
}

TEST(MaterialImagesTest, TheDeviceLimitIsWhatMipChainedTexturesAreFilteredWith)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    // A cache that was never told filters isotropically. That is the safe answer AND the one a missing piece
    // of wiring leaves in place for the whole session: the sampler asks for anisotropy with a factor of 1,
    // which is the same picture as not asking at all (see MaterialImages::makeSampler).
    EXPECT_FLOAT_EQ(cache->maxAnisotropy(), 1.0F);

    cache->setMaxAnisotropy(16.0F);  // what a device may report
    EXPECT_FLOAT_EQ(cache->maxAnisotropy(), 16.0F);
    cache->setMaxAnisotropy(64.0F);  // more than this backend asks for: clamped, never an illegal request
    EXPECT_FLOAT_EQ(cache->maxAnisotropy(), 16.0F);
    cache->setMaxAnisotropy(0.5F);  // a device that offers less than 1: 1 is legal on every device
    EXPECT_FLOAT_EQ(cache->maxAnisotropy(), 1.0F);

    cache->setMaxAnisotropy(16.0F);

    // Anisotropy is only meaningful with a mip chain to choose between, which is the texture this case needs:
    // the same 4x4 picture with a second level.
    const auto texture = vine::intrusive_ptr<Texture2D>(new Texture2D(4, 4, PixelFormat::Rgba8Unorm, 2));
    texture->setImage(vine::intrusive_ptr<const Image>(new Image(4, 4, PixelFormat::Rgba8Unorm, 2)));

    TextureReject      reason = TextureReject::Ok;
    const SamplerImage images = cache->acquire(texture.get(), reason);
    ASSERT_EQ(reason, TextureReject::Ok);
    ASSERT_NE(images.sampler, nullptr);
    EXPECT_EQ(images.sampler->anisotropyEnable, VK_TRUE);
    EXPECT_FLOAT_EQ(images.sampler->maxAnisotropy, 16.0F) << "the device's own limit, not the 1x default";

    // And the contrast: a single-level texture must NOT ask for it - there is nothing to interpolate between,
    // and leaving the flag on would make the driver filter across levels that do not exist.
    const auto flat_texture = readyTexture();
    const auto flat         = cache->acquire(flat_texture.get(), reason);
    ASSERT_EQ(reason, TextureReject::Ok);
    ASSERT_NE(flat.sampler, nullptr);
    EXPECT_EQ(flat.sampler->anisotropyEnable, VK_FALSE);
    EXPECT_FLOAT_EQ(flat.sampler->maxAnisotropy, 1.0F);
}

TEST(MaterialImagesTest, ClearDropsEveryEntryAndTheFallbacks)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    const vine::intrusive_ptr<Texture2D> texture = readyTexture();
    TextureReject                        reason  = TextureReject::Ok;
    const auto                           images  = cache->acquire(texture.get(), reason);
    ASSERT_NE(images.view, nullptr);
    const auto white_before = cache->white().view;

    cache->clear();

    EXPECT_EQ(cache->count(), 0U);
    EXPECT_FALSE(cache->has(texture.get()));

    const auto rebuilt = cache->acquire(texture.get(), reason);
    EXPECT_EQ(reason, TextureReject::Ok);
    ASSERT_NE(rebuilt.view, nullptr);
    // The images held above keep the old objects alive, so "built again" is a different object rather than
    // an address the allocator happened to hand back.
    EXPECT_NE(rebuilt.view.get(), images.view.get()) << "the texture is built again on its next acquire";
    EXPECT_NE(cache->white().view.get(), white_before.get()) << "the fallback is rebuilt too";
}

TEST(MaterialImagesTest, TheOldestEntryIsTrimmedAtCapacity)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    std::vector<vine::intrusive_ptr<Texture2D>> textures;
    textures.reserve(MaterialImages::kMaxEntries + 1U);
    for (std::size_t index = 0; index < MaterialImages::kMaxEntries + 1U; ++index) {
        textures.push_back(readyTexture(1, 1));
        TextureReject reason = TextureReject::Ok;
        ASSERT_NE(cache->acquire(textures.back().get(), reason).view, nullptr);
    }

    // Nothing tells the backend a texture is gone, so the cache is bounded: the OLDEST entry goes, because
    // the newest are the ones a live scene is using - and an evicted texture rebuilds on its next acquire.
    EXPECT_EQ(cache->count(), MaterialImages::kMaxEntries);
    EXPECT_FALSE(cache->has(textures.front().get()));
    EXPECT_TRUE(cache->has(textures.back().get()));
}

TEST(MaterialImagesTest, ACubeBecomesACubeViewOverItsSixLayers)
{
    const std::shared_ptr<MaterialImages> cache = MaterialImages::create();
    ASSERT_NE(cache, nullptr);

    const vine::intrusive_ptr<CubeMap> cube = readyCube();
    TextureReject                      reason = TextureReject::Ok;
    const SamplerImage                 images = cache->acquire(cube.get(), reason);

    ASSERT_EQ(reason, TextureReject::Ok);
    ASSERT_NE(images.view, nullptr);
    EXPECT_EQ(viewTypeOf(images), VK_IMAGE_VIEW_TYPE_CUBE);

    const ::vsg::ref_ptr<::vsg::Image> image = images.view->image;
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->arrayLayers, 6U);
    EXPECT_EQ(image->mipLevels, 1U);
    EXPECT_TRUE((image->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U)
        << "a cube view over an image without the flag is invalid";

    // The layer count vsg's copy regions use comes from the DATA's depth and view type, so both are the
    // image's own declaration - a six-layer texture described by a 2D array would get one copy region.
    ASSERT_NE(image->data, nullptr);
    EXPECT_EQ(image->data->depth(), 6U) << "six layers, declared as a depth";
    EXPECT_EQ(image->data->properties.imageViewType, VK_IMAGE_VIEW_TYPE_CUBE);
}
