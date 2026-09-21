/**
 * @brief The off-screen target and its readback: the evidence path itself, on a real device.
 *
 * "It rendered" and "validation was clean" are both satisfiable by a program that draws nothing, so this
 * backend's picture is only ever evidenced by PIXELS. These cases prove the path that produces them: a
 * pass that clears, a render pass that leaves the image where the copy needs it, a copy recorded in the
 * same command graph, and a probe that reads tightly packed RGBA8 rows - all with NO window, so the phase
 * that uses it later does not depend on a display server.
 *
 * The assertions are the two a phase will make: every pixel holds the clear colour (nothing partial, no
 * straggler from a previous frame), and a second frame reads back the same picture (the path is stable,
 * not accidentally correct once).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>

using vine::vsg::OffscreenTarget;
using vine::vsg::createDevice;
using vine::vsg::core::PixelProbe;
using vine::vsg::core::Rgba8;

namespace
{

/// @brief The 8-bit value a clear colour quantises to (UNORM conversion, round to nearest).
std::uint8_t quantise(float value)
{
    return static_cast<std::uint8_t>(std::lround(value * 255.0F));
}

}  // namespace

TEST(OffscreenTargetTest, AClearedTargetReadsBackAsItsClearColour)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements: "
                     << std::string(reinterpret_cast<const char*>(created.error.data()), created.error.size());
    }

    auto target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ 8U, 4U,
                                                                                   { 0.25F, 0.5F, 0.75F, 1.0F } });
    ASSERT_NE(target, nullptr) << "the target (image, pass, framebuffer, copy-back) must be creatable";

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());  // recorded AFTER the pass, on the same queue, in order
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile()) << "the graph must compile";

    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const PixelProbe probe = target->probe();
    ASSERT_TRUE(probe.valid());
    EXPECT_EQ(probe.width(), 8);
    EXPECT_EQ(probe.height(), 4);

    // Every pixel is the clear colour, within the one-step tolerance a UNORM conversion can introduce.
    const Rgba8 expected{ quantise(0.25F), quantise(0.5F), quantise(0.75F), 255U };
    const Rgba8 sampled = probe.pixel(4, 2);
    EXPECT_NEAR(sampled.r, expected.r, 1);
    EXPECT_NEAR(sampled.g, expected.g, 1);
    EXPECT_NEAR(sampled.b, expected.b, 1);
    EXPECT_EQ(sampled.a, expected.a);
    EXPECT_TRUE(probe.wholeImageMatches(sampled))
        << "the clear is uniform: any pixel that differs is a region the pass did not write";
    EXPECT_DOUBLE_EQ(probe.nonBlackFraction(), 1.0) << "a clear colour that is not black is still not 'empty'";
}

TEST(OffscreenTargetTest, ABlackClearReadsBackAsNoContent)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ 4U, 4U, { 0.0F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(target, nullptr);

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());

    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const PixelProbe probe = target->probe();
    ASSERT_TRUE(probe.valid());
    EXPECT_EQ(probe.nonBlackPixels(), 0U) << "a phase that draws nothing must be able to see that it drew nothing";
    EXPECT_TRUE(probe.wholeImageMatches(Rgba8{ 0U, 0U, 0U, 255U }));
}

TEST(OffscreenTargetTest, ASecondFrameReadsBackTheSamePicture)
{
    const auto created = createDevice();
    if (!created.ok) {
        GTEST_SKIP() << "no device satisfies the device-floor requirements";
    }

    auto target = OffscreenTarget::create(created.device, OffscreenTarget::Layout{ 16U, 16U, { 1.0F, 0.0F, 0.0F, 1.0F } });
    ASSERT_NE(target, nullptr);

    auto viewer        = ::vsg::Viewer::create();
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    command_graph->addChild(target->renderGraph());
    command_graph->addChild(target->capture());
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());

    Rgba8 first_pixel{};
    for (int frame = 0; frame < 2; ++frame) {
        viewer->advanceToNextFrame();
        viewer->handleEvents();
        viewer->recordAndSubmit();
        viewer->deviceWaitIdle();

        const PixelProbe probe = target->probe();
        ASSERT_TRUE(probe.valid());
        const Rgba8 center = probe.pixel(8, 8);
        EXPECT_NEAR(center.r, 255, 1) << "frame " << frame;
        EXPECT_NEAR(center.g, 0, 1);
        EXPECT_NEAR(center.b, 0, 1);
        EXPECT_TRUE(probe.wholeImageMatches(center)) << "frame " << frame;
        if (frame == 0) {
            first_pixel = center;
        }
        else {
            EXPECT_EQ(center, first_pixel) << "the readback must be stable frame to frame";
        }
    }
}
