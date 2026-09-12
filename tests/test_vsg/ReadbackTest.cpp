/**
 * @brief The readback guards (design §50, device-free).
 *
 * A readback is synchronous — it stops the device — so its guards are the part worth
 * pinning: a call that cannot be served must refuse BEFORE the wait and say why on the
 * diagnostics route, not return an empty buffer silently. None of these cases needs a
 * window, a device or a built target, which is exactly the point of the extracted layer
 * (VsgReadback.hpp).
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/vsg/VsgReadback.hpp>

using vine::graphics::RenderTarget;
using vine::vsg::VsgDiagnostics;
using vine::vsg::VsgRendererState;
using vine::vsg::detail::readbackTarget;
using vine::vsg::detail::readColorBuffer;
using vine::vsg::detail::readDepthBuffer;

TEST(Readback, TargetLookupRefusesWithoutASession)
{
    VsgRendererState       state; // no window / viewer: nothing was ever built

    EXPECT_EQ(readbackTarget(state, nullptr), nullptr);
}

TEST(Readback, ColourReadbackRefusesWithoutABuiltTarget)
{
    VsgRendererState       state;
    VsgDiagnostics         diagnostics;
    std::vector<std::uint8_t> pixels;

    EXPECT_FALSE(readColorBuffer(state, diagnostics, nullptr, 0, pixels));
    EXPECT_TRUE(pixels.empty());
}

TEST(Readback, DepthReadbackRefusesWithoutABuiltTarget)
{
    VsgRendererState       state;
    VsgDiagnostics         diagnostics;
    std::vector<float>     depths;

    EXPECT_FALSE(readDepthBuffer(state, diagnostics, nullptr, depths));
    EXPECT_TRUE(depths.empty());
}

TEST(Readback, ReadbackRefusesATargetThatWasNeverBuilt)
{
    VsgRendererState       state;
    VsgDiagnostics         diagnostics;
    RenderTarget           target; // attached to nothing: the session never built it
    std::vector<float>     depths;

    EXPECT_FALSE(readDepthBuffer(state, diagnostics, &target, depths));
    EXPECT_TRUE(depths.empty());
}
