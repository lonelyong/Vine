/**
 * @brief The 1x1 white image a material without a texture samples (see `.ai/design/vsg-reimplementation.md`
 * §11.16ai, `api/WhiteImage.hpp`).
 *
 * Device-free by construction: `vsg::Image`, `vsg::ImageView` and `vsg::Sampler` are create-infos until a
 * `vsg::Context` compiles them, so what is checked here is that the fallback IS what a declared set needs
 * (an image view, a sampler, and the white texel as data the compile-time transfer uploads). The pixel half
 * - a program whose `diffuseMap` reads white, so the shading is the material's own colour - lives in
 * ContentPassTest.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <vsg/core/Array3D.h>
#include <vsg/state/Image.h>
#include <vsg/state/Sampler.h>

#include <vine/vsg/api/WhiteImage.hpp>

using vine::vsg::WhiteImage;

TEST(WhiteImageTest, TheFallbackCarriesItsWhiteTexelAsUploadableData)
{
    const std::shared_ptr<WhiteImage> white = WhiteImage::create();
    ASSERT_NE(white, nullptr);
    ASSERT_NE(white->view(), nullptr) << "a declared set binds an image view";
    ASSERT_NE(white->sampler(), nullptr) << "and reads it through a sampler";
    EXPECT_EQ(white->view()->viewType, VK_IMAGE_VIEW_TYPE_2D);

    // The texel travels the way every texture's pixels do: as DATA on the image, which vsg's compile-time
    // transfer copies in and leaves in the layout a sampled descriptor declares (SHADER_READ_ONLY_OPTIMAL).
    // A fallback that instead waited for a frame to record a clear was the demo's last validation error
    // (VUID-vkCmdDraw-None-09600, every submit; see the header note).
    const ::vsg::ref_ptr<::vsg::Image> image = white->view()->image;
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->format, VK_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(image->extent.width, 1U);
    EXPECT_EQ(image->extent.height, 1U);
    EXPECT_EQ(image->arrayLayers, 1U);
    EXPECT_TRUE((image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0U) << "the upload writes it";
    EXPECT_TRUE((image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U) << "the descriptor reads it";
    EXPECT_EQ(image->initialLayout, VK_IMAGE_LAYOUT_UNDEFINED) << "the transfer owns the transition";

    ASSERT_NE(image->data, nullptr) << "nothing to upload means the image is never transitioned";
    EXPECT_EQ(image->data->depth(), 1U) << "one layer, declared as a depth";
    EXPECT_EQ(image->data->properties.imageViewType, VK_IMAGE_VIEW_TYPE_2D);

    const ::vsg::ref_ptr<::vsg::uintArray3D> texels = image->data.cast<::vsg::uintArray3D>();
    ASSERT_NE(texels, nullptr) << "the texel is read the way vsg's copy reads it";
    EXPECT_EQ(texels->at(0U, 0U, 0U), 0xFFFFFFFFU) << "white, all four channels";

    // Two DISTINCT nodes: building the fallback twice must not hand the same image to two callers (each set
    // owns what it binds).
    const std::shared_ptr<WhiteImage> second = WhiteImage::create();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(white->view(), second->view());
}
