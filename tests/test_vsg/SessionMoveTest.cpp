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
 * X11 only, and it SKIPS rather than fails when there is no display or no usable device: what it checks is
 * this backend's behaviour, not the machine's configuration.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include "TestHostWindow.hpp"

#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/core/Diagnostics.hpp>

using vine::vsg::api::Session;
using vine::vsg::api::SessionOptions;
using vine::vsg::api::probePhysicalDevices;

#if !defined(_WIN32)

namespace
{

}  // namespace

TEST(SessionMoveTest, ASecondHostWindowMovesTheSessionAndTheSameOneKeepsIt)
{
    if (std::getenv("DISPLAY") == nullptr)
    {
        GTEST_SKIP() << "no window system";
    }
    const auto probed_devices = probePhysicalDevices();
    if (!probed_devices.ok || probed_devices.usableCount() == 0)
    {
        GTEST_SKIP() << "no device satisfies the requirements";
    }

    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        GTEST_SKIP() << "no X display to create host windows on";
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    ASSERT_NE(screens.rem, 0) << "an X display without a screen";
    xcb_screen_t* screen = screens.data;

    TestHostWindow first(connection, screen, 320, 240);
    TestHostWindow second(connection, screen, 320, 240);

    vine::vsg::core::Diagnostics diagnostics;
    // A session that NEVER pumps the host's events (the host owns the message loop) and reports nothing.
    diagnostics.setSink([](const vine::graphics::RenderDiagnostic&) {});

    Session session;
    SessionOptions options;
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
    // point: the session must treat the host's window as something that outlives it. The X connection is left
    // to the process rather than closed here, because those destructors still need it.
}

#endif  // !_WIN32
