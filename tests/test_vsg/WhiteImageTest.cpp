/**
 * @brief The 1x1 white image a material without a texture samples (see `.ai/design/vsg-reimplementation.md`
 * §11.16ai, `api/WhiteImage.hpp`).
 *
 * Device-free by construction: `vsg::Image`, `vsg::ImageView` and `vsg::Sampler` are create-infos until a
 * `vsg::Context` compiles them, so what is checked here is that the fallback IS what a declared set needs
 * (an image view, a sampler, and a node that makes the image white and leaves it readable). The pixel half
 * - a program whose `diffuseMap` reads white, so the shading is the material's own colour - lives in
 * ContentPassTest.
 */

#include <gtest/gtest.h>

#include <cstddef>

#include <vsg/commands/ClearImage.h>
#include <vsg/commands/Commands.h>
#include <vsg/state/Sampler.h>

#include <vine/vsg/api/WhiteImage.hpp>

using vine::vsg::WhiteImage;

TEST(WhiteImageTest, TheFallbackCarriesAViewASamplerAndItsFill)
{
    const std::shared_ptr<WhiteImage> white = WhiteImage::create();
    ASSERT_NE(white, nullptr);
    ASSERT_NE(white->view(), nullptr) << "a declared set binds an image view";
    ASSERT_NE(white->sampler(), nullptr) << "and reads it through a sampler";
    EXPECT_EQ(white->view()->viewType, VK_IMAGE_VIEW_TYPE_2D);

    // The fill is what makes it white: a clear of the whole 1x1 image between the two barriers that leave it
    // in the layout a sampled descriptor declares (SHADER_READ_ONLY - the same a written image ends in).
    const auto commands = white->fill().cast<::vsg::Commands>();
    ASSERT_NE(commands, nullptr);
    ASSERT_EQ(commands->children.size(), 3U) << "a barrier, the clear, and the barrier that leaves it readable";

    const auto clear = commands->children[1].cast<::vsg::ClearColorImage>();
    ASSERT_NE(clear, nullptr) << "the middle command is the clear";
    EXPECT_EQ(clear->imageLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    EXPECT_FLOAT_EQ(clear->color.float32[0], 1.0F) << "white, all four channels";
    EXPECT_FLOAT_EQ(clear->color.float32[1], 1.0F);
    EXPECT_FLOAT_EQ(clear->color.float32[2], 1.0F);
    EXPECT_FLOAT_EQ(clear->color.float32[3], 1.0F);
    ASSERT_EQ(clear->ranges.size(), 1U);
    EXPECT_EQ(clear->ranges[0].levelCount, 1U);

    // Two DISTINCT nodes: building the fallback twice must not hand the same image to two callers (each set
    // owns what it binds).
    const std::shared_ptr<WhiteImage> second = WhiteImage::create();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(white->view(), second->view());
}
