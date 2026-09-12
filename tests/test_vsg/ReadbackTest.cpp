/**
 * @brief The readback guards and their refusal classification (design §50 / §58 D60, device-free).
 *
 * A readback is synchronous — it stops the device — so its guards are the part worth
 * pinning: a call that cannot be served must refuse BEFORE the wait and say why on the
 * diagnostics route, not return an empty buffer silently. The prologue therefore CLASSIFIES
 * its refusal (ReadbackRefusal) and the entry points report it, so a host can tell a state a
 * later call can succeed in (not initialized yet / not built yet / empty) from one this
 * backend will never serve (a target it never rendered).
 *
 * The session flag and the target table are enough for all of it: no window, no device and no
 * built attachments are needed, which is exactly the point of the extracted layer.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/vsg/VsgReadback.hpp>

using vine::graphics::RenderDiagnostic;
using vine::graphics::RenderTarget;
using vine::graphics::RenderTargetPtr;
using vine::vsg::VsgDiagnostics;
using vine::vsg::VsgRendererState;
using vine::vsg::detail::readbackRefusalMessage;
using vine::vsg::detail::ReadbackRefusal;
using vine::vsg::detail::readbackTarget;
using vine::vsg::detail::readColorBuffer;
using vine::vsg::detail::readDepthBuffer;

namespace
{

/// Collects what a readback route reports (the entry points report through it).
struct Captured
{
    std::vector<RenderDiagnostic> items;

    /// Installs this collector on @p diagnostics.
    void installOn(VsgDiagnostics& diagnostics)
    {
        diagnostics.setDownstream([this](const RenderDiagnostic& diagnostic) { items.push_back(diagnostic); });
    }
};

/// A state whose session is initialized, but with no window / device: all a refusal needs.
VsgRendererState initializedSession()
{
    VsgRendererState state;
    state.initialized = true;
    return state;
}

} // namespace

TEST(Readback, TargetLookupRefusesWithoutATargetOrASession)
{
    VsgRendererState state; // not initialized: the session has rendered nothing
    RenderTargetPtr  target(new RenderTarget());
    ReadbackRefusal  refusal = ReadbackRefusal::None;

    // No target at all is a caller error, not a backend limitation.
    EXPECT_EQ(readbackTarget(state, nullptr, refusal), nullptr);
    EXPECT_EQ(refusal, ReadbackRefusal::NoTarget);

    // A target without a session: unsupported for now, and a later call can succeed.
    EXPECT_EQ(readbackTarget(state, target.get(), refusal), nullptr);
    EXPECT_EQ(refusal, ReadbackRefusal::NoSession);
}

TEST(Readback, TargetLookupClassifiesTheTargetsState)
{
    VsgRendererState state = initializedSession();
    RenderTargetPtr  target(new RenderTarget());
    ReadbackRefusal  refusal = ReadbackRefusal::None;

    // Unknown to this session: never rendered into, which this backend will never serve.
    EXPECT_EQ(readbackTarget(state, target.get(), refusal), nullptr);
    EXPECT_EQ(refusal, ReadbackRefusal::NotRendered);

    // Registered, but its attachments were never built (a refused or not-yet-run build).
    auto& entry = state.entryFor(target.get());
    EXPECT_EQ(readbackTarget(state, target.get(), refusal), nullptr);
    EXPECT_EQ(refusal, ReadbackRefusal::NotBuilt);

    // Built, but with no usable size.
    entry.attachments_built = true;
    EXPECT_EQ(readbackTarget(state, target.get(), refusal), nullptr);
    EXPECT_EQ(refusal, ReadbackRefusal::Empty);

    // Built with a size: readable, and the classification says nothing was refused.
    entry.width  = 4;
    entry.height = 4;
    EXPECT_NE(readbackTarget(state, target.get(), refusal), nullptr);
    EXPECT_EQ(refusal, ReadbackRefusal::None);
}

TEST(Readback, ColourReadbackRefusesWithoutABuiltTargetAndSaysWhy)
{
    VsgRendererState          state = initializedSession();
    VsgDiagnostics            diagnostics;
    Captured                  captured;
    std::vector<std::uint8_t> pixels;
    RenderTargetPtr           target(new RenderTarget());
    captured.installOn(diagnostics);
    // Known to the session but with no built attachments: the refusal that says a later call
    // (after the target renders) can succeed.
    state.entryFor(target.get());

    EXPECT_FALSE(readColorBuffer(state, diagnostics, target.get(), 0, pixels));
    EXPECT_TRUE(pixels.empty());
    // A refused readback says why (the header's promise): the classification reaches the sink.
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_NE(captured.items[0].message.stdstr().find("no built attachments"), std::string::npos);
    // ...and it refused BEFORE the wait: a refusal never stops the device.
    EXPECT_EQ(state.retireRing.waits, 0u);
}

TEST(Readback, DepthReadbackRefusesWithoutABuiltTargetAndSaysWhy)
{
    VsgRendererState   state = initializedSession();
    VsgDiagnostics     diagnostics;
    Captured           captured;
    std::vector<float> depths;
    RenderTargetPtr    target(new RenderTarget());
    captured.installOn(diagnostics);
    // Known to the session but not built yet (see the colour case).
    state.entryFor(target.get());

    EXPECT_FALSE(readDepthBuffer(state, diagnostics, target.get(), depths));
    EXPECT_TRUE(depths.empty());
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_NE(captured.items[0].message.stdstr().find("no built attachments"), std::string::npos);
    EXPECT_EQ(state.retireRing.waits, 0u);
}

TEST(Readback, ReadbackRefusesANullTargetAndSaysWhy)
{
    VsgRendererState   state = initializedSession();
    VsgDiagnostics     diagnostics;
    Captured           captured;
    std::vector<float> depths;
    captured.installOn(diagnostics);

    EXPECT_FALSE(readDepthBuffer(state, diagnostics, nullptr, depths));
    EXPECT_TRUE(depths.empty());
    ASSERT_EQ(captured.items.size(), 1u);
    EXPECT_NE(captured.items[0].message.stdstr().find("no target was given"), std::string::npos);
}

TEST(Readback, EachRefusalKeepsItsOwnWording)
{
    RenderTargetPtr target(new RenderTarget());
    const auto      message = [&target](ReadbackRefusal refusal) {
        return readbackRefusalMessage(refusal, "readDepthBuffer", target.get()).stdstr();
    };
    const std::string no_target    = message(ReadbackRefusal::NoTarget);
    const std::string no_session   = message(ReadbackRefusal::NoSession);
    const std::string not_rendered = message(ReadbackRefusal::NotRendered);
    const std::string not_built    = message(ReadbackRefusal::NotBuilt);
    const std::string empty        = message(ReadbackRefusal::Empty);
    const std::string no_device    = message(ReadbackRefusal::NoDevice);

    // The entry point that refused is named in every message.
    for (const std::string& text : { no_target, no_session, not_rendered, not_built, empty, no_device }) {
        EXPECT_NE(text.find("readDepthBuffer"), std::string::npos);
    }
    // Each refusal carries its own wording: one shared format string is how a branch ends up
    // printing another branch's number (§54), so no two branches may share a phrase.
    EXPECT_NE(no_target.find("no target was given"), std::string::npos);
    EXPECT_NE(no_session.find("not initialized yet"), std::string::npos);
    EXPECT_NE(not_rendered.find("never rendered"), std::string::npos);
    EXPECT_NE(not_built.find("no built attachments yet"), std::string::npos);
    EXPECT_NE(empty.find("no usable size"), std::string::npos);
    EXPECT_NE(no_device.find("no usable device"), std::string::npos);
    EXPECT_EQ(not_built.find("no usable size"), std::string::npos);
    EXPECT_EQ(empty.find("no built attachments"), std::string::npos);
    // Nothing refused: no message to report.
    EXPECT_TRUE(readbackRefusalMessage(ReadbackRefusal::None, "readDepthBuffer", target.get()).empty());
}
