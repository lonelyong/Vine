/**
 * @brief The session's answer to "the host announced a window": keep it, move it, or rebuild.
 *
 * This is the case the whole "the host gives the timing, the control maintains the session" contract exists
 * for, so it is exercised against real host windows and a real device: a session adopts one X window, then
 * is handed a second one and must FOLLOW it - the device and every compiled pipeline survive - rather than
 * start over. The three outcomes are all checked, because the expensive one must not be reachable by
 * accident: re-announcing the SAME window keeps the session (a show or a resize is not a reason to rebuild),
 * a DIFFERENT one moves it, and the test also pins that the host's windows outlive the session.
 *
 * X11 or Win32, and it SKIPS rather than fails when there is no display or no usable device: what it checks is
 * this backend's behaviour, not the machine's configuration.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include "TestHostWindow.hpp"

#include <vine/graphics/Diagnostics.hpp>
#include <vine/backend_vsg/internal/DeviceProbe.hpp>
#include <vine/backend_vsg/internal/WindowSession.hpp>

using vn::backend_vsg::api::probePhysicalDevices;
using vn::backend_vsg::api::WindowSession;
using vn::backend_vsg::api::WindowSessionOptions;

namespace
{} // namespace

TEST(SessionMoveTest, ASecondHostWindowMovesTheSessionAndTheSameOneKeepsIt)
{
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0) {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    TestDisplay display;
    if (!display.open()) {
        GTEST_SKIP() << display.skipReason();
    }

    TestHostWindow first(display, 320, 240);
    TestHostWindow second(display, 320, 240);

    vn::graphics::Diagnostics diagnostics;
    // A session that NEVER pumps the host's events (the host owns the message loop) and reports nothing.
    diagnostics.setSink([](const vn::graphics::RenderDiagnostic&) {});

    WindowSession        session;
    WindowSessionOptions options;
    options.native_handle = first.handle();

    ASSERT_TRUE(session.initialize(options, diagnostics));
    EXPECT_EQ(session.hostHandle(), first.handle());
    EXPECT_EQ(session.moves(), 0U);
    EXPECT_EQ(session.rebuilds(), 0U);
    EXPECT_EQ(session.keeps(), 0U);
    EXPECT_EQ(session.generation(), 1U) << "a built session has its first surface generation";

    const auto frame = [&session] {
        EXPECT_TRUE(session.beginFrame());
        EXPECT_TRUE(session.commitFrame());
    };
    frame();
    frame();
    EXPECT_EQ(session.framesPresented(), 2U);

    // The SAME window again: a show or a resize is not a reason to rebuild anything.
    ASSERT_TRUE(session.initialize(options, diagnostics));
    EXPECT_EQ(session.keeps(), 1U);
    EXPECT_EQ(session.moves(), 0U);
    EXPECT_EQ(session.rebuilds(), 0U);
    EXPECT_EQ(session.generation(), 1U) << "keeping the session does not change its surface";

    // A DIFFERENT window: the session follows it, keeping the device and every compiled pipeline.
    options.native_handle = second.handle();
    ASSERT_TRUE(session.initialize(options, diagnostics));
    EXPECT_EQ(session.moves(), 1U) << "the session must move, not rebuild";
    EXPECT_EQ(session.rebuilds(), 0U);
    EXPECT_EQ(session.keeps(), 1U) << "the earlier keep is still counted; a move is not a keep";
    EXPECT_EQ(session.generation(), 2U) << "a moved session has a new surface generation";
    EXPECT_EQ(session.hostHandle(), second.handle());

    // ... and it keeps rendering on the new surface, with exactly the one device stop the move needed (the
    // swapchain being replaced may still be named by work in flight).
    frame();
    frame();
    EXPECT_EQ(session.framesPresented(), 4U);
    EXPECT_EQ(session.deviceWaits(), 1U) << "the move is the only counted stop so far";

    session.shutdown();
    EXPECT_FALSE(session.initialized());
    EXPECT_TRUE(first.alive()) << "the session must never destroy the host's first window";
    EXPECT_TRUE(second.alive()) << "the session must never destroy the host's second window";

    // The windows are destroyed by THIS test (their destructors run at the end of this scope), which is the
    // point: the session must treat the host's window as something that outlives it. The display outlives them
    // too, by declaration order (see TestDisplay): the connection closes after the last window built on it.
}
