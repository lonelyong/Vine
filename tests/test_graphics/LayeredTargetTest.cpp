/**
 * @brief Layered render targets, the SDK half: a target's layer count, a pass' layer range, and the
 * image identity a layer gives one attachment.
 *
 * The backend half (shape equality, readback classification, array images and per-layer framebuffers)
 * is evidenced in `test_vsg`; these cases pin the vocabulary the whole path is built from, so a layer
 * cannot quietly become an integer that two consumers read differently:
 *
 *  * `RenderTarget::layerCount` is one target property (0 means 1), and the depth follows it;
 *  * `PassDesc::first_layer` / `layer_count` travel to `RenderPass` as one declaration;
 *  * an `ImageRef` bound to a layer keeps that layer as part of the image's address (and unbinding
 *    resets it).
 */

#include <gtest/gtest.h>

#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>

using namespace vn::graphics;

TEST(LayeredTargetTest, ALayerCountDefaultsToOneAndZeroStillMeansOne)
{
    RenderTarget target;
    EXPECT_EQ(target.layerCount(), 1);

    target.setLayerCount(6);
    EXPECT_EQ(target.layerCount(), 6);

    // Like setSamples: a non-positive count is not "automatic", it is the historical 2D target.
    target.setLayerCount(0);
    EXPECT_EQ(target.layerCount(), 1);
    target.setLayerCount(-3);
    EXPECT_EQ(target.layerCount(), 1);
}

TEST(LayeredTargetTest, TheDeclarationCarriesTheLayerRangeOntoThePass)
{
    PassDesc desc;
    desc.first_layer = 2;
    desc.layer_count = 1;

    RenderPass pass;
    pass.setDesc(desc);
    EXPECT_EQ(pass.firstLayer(), 2U);
    EXPECT_EQ(pass.layerCount(), 1U);

    // A stated 0 is 1, matching the other counts in the API (setSamples, setLayerCount).
    pass.setLayerCount(0);
    EXPECT_EQ(pass.layerCount(), 1U);
    pass.setFirstLayer(5);
    pass.setLayerCount(1);
    EXPECT_EQ(pass.firstLayer(), 5U);
}

TEST(LayeredTargetTest, AnImageRefNamesOneLayerOfOneAttachment)
{
    const vn::intrusive_ptr<RenderTarget> target(new RenderTarget());
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->setSize(16, 16);
    target->setLayerCount(4);

    ImageRef image(u8"layer");
    image.bind(target, 1, 3);
    EXPECT_EQ(image.layer(), 3);
    EXPECT_EQ(image.attachment(), 1);
    ASSERT_EQ(image.target(), target.get());

    // Unbinding resets the address: a layer kept from an unbound image would be a claim about nothing.
    image.bind(nullptr);
    EXPECT_EQ(image.layer(), 0);
    EXPECT_EQ(image.attachment(), 0);
}
