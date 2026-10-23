/**
 * @brief Layered render targets, the backend-contract half: the shape a plan is keyed on and the
 * readback classifier.
 *
 * These are backend headers (a host never includes them), so they live in `test_vsg` where the suite is
 * a declared backend consumer. The device-level half - array images, per-layer framebuffers and a real
 * readback of two layers - is `VsgBackendTest.TwoPassesWriteTwoLayersOfOneArrayTargetAndReadEachLayerBack`.
 */

#include <gtest/gtest.h>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/backend/Keys.hpp>
#include <vine/graphics/backend/Readback.hpp>
#include <vine/graphics/backend/TargetPlan.hpp>

using namespace vn::graphics;
using namespace vn::graphics::backend;

TEST(LayeredTargetTest, TheLayerCountIsPartOfTheShapeButNotOfRenderPassCompatibility)
{
    TargetShape plain;
    plain.color_formats = { RenderTarget::ColorFormat::RGBA8 };
    TargetShape layered = plain;
    layered.layers      = 2;

    // Equality (and therefore planTarget's Rebuild decision) sees the layers: the views and the per-layer
    // framebuffers really change.
    EXPECT_FALSE(plain == layered);

    // Vulkan's render-pass compatibility does not depend on how many array layers the images have, so a
    // pipeline compiled for the plain target is still compatible with the layered one. Keeping layers out
    // of this half is what stops a layer count change from recompiling every pipeline.
    EXPECT_TRUE(plain.compatibility() == layered.compatibility());
}

TEST(LayeredTargetTest, ReadbackRefusesAnOutOfRangeLayerBeforeItLooksAtTheAttachment)
{
    ReadbackState state;
    state.color_attachments = 1;
    state.layers            = 2;
    state.color_format      = RenderTarget::ColorFormat::RGBA8;

    // Layer 2 of a two-layer target is an unknown LAYER, not an unknown attachment: the request's address
    // has two parts, and the diagnostic should name the one that is wrong.
    EXPECT_EQ(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U, 2U }).refusal,
              ReadbackRefusal::UnknownLayer);

    // Layer 1 exists but nothing has copied it back yet.
    EXPECT_EQ(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U, 1U }).refusal,
              ReadbackRefusal::NotCaptured);

    state.color_captured = true;
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Color, 0U, 1U }).ok);

    // The depth attachment follows the colour layers.
    state.depth_format   = RenderTarget::DepthFormat::D32F;
    state.depth_captured = true;
    EXPECT_TRUE(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U, 1U }).ok);
    EXPECT_EQ(readbackOf(state, ReadbackRequest{ ReadbackKind::Depth, 0U, 2U }).refusal,
              ReadbackRefusal::UnknownLayer);
}
