/**
 * @brief The TEST HOST WINDOW as a measuring instrument: what a window read answers, and what a REFUSAL is.
 *
 * WHY THIS SUITE EXISTS. Every window case in this directory concludes with "the pixels say so", and those
 * pixels come from `TestHostWindow::pixel` - an `XGetImage` on the window the session adopted. That call has
 * TWO failure shapes that used to be one: the server can REFUSE the read (a NULL reply, e.g. `BadMatch` for a
 * rectangle outside the drawable, or for a window it does not consider viewable yet), and the pixel can
 * legitimately BE black. Both answered `{0, 0, 0}`, so a caller could not tell "the picture is black" from
 * "nobody answered" - measured 2026-09-24 in a full-suite run: a probe inside a drawn triangle answered
 * (0,0,0) while its neighbours answered the picture, and the reply had simply been NULL.
 *
 * The instrument now has two halves, and this file pins both WITHOUT a device (the window here is drawn by
 * plain X, so the cases run wherever a display does):
 *
 *   * a read of what the window shows answers those bytes and reports NO error (`readError() == 0`);
 *   * a read the server cannot serve answers black AND says it was refused, so a caller that asserts on the
 *     picture can print WHICH of the two happened;
 *   * a window is VIEWABLE before its constructor returns (`wasViewableAtCreation`), which is the one moment
 *     that matters for a present: the frames a session presents into a window the server has not reported
 *     viewable yet never reach it - the window stays on its untouched backing store however long the caller
 *     waits afterwards (measured 2026-09-24, see TestHostWindow's constructor).
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>

#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#include "TestHostWindow.hpp"

#if !defined(_WIN32)

namespace
{

constexpr int kWidth  = 64;
constexpr int kHeight = 48;

/// @brief One connection, one mapped 64x48 window on the root, and the screen it was created on.
struct Fixture
{
    xcb_connection_t* connection{nullptr};
    xcb_screen_t*     screen{nullptr};

    bool open()
    {
        int screen_index = 0;
        connection       = xcb_connect(nullptr, &screen_index);
        if (connection == nullptr || xcb_connection_has_error(connection) != 0)
        {
            return false;
        }
        screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
        return screen != nullptr;
    }

    ~Fixture()
    {
        if (connection != nullptr)
        {
            xcb_disconnect(connection);
        }
    }
};

}  // namespace

TEST(HostWindowReadTest, AReadOfWhatTheWindowShowsComesBackAndReportsNoError)
{
    Fixture fixture;
    if (!fixture.open())
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    TestHostWindow host(fixture.connection, fixture.screen, kWidth, kHeight);

    // The constructor's own wait is the sync point, and it is reported: a window that was just created is
    // mapped by the server at its own pace, and the frames presented before it is viewable are lost (see the
    // file note). This assertion is what makes the wait a contract instead of a hope.
    EXPECT_TRUE(host.wasViewableAtCreation())
        << "the constructor waits for the server's own viewable report before a case presents anything "
        << "(waited " << host.viewWait().count() << " ms)";
    if (host.viewWait() > std::chrono::milliseconds{ 50 })
    {
        // Not a failure: the wait exists for exactly this, and the cost is what it is. It is PRINTED because
        // a server that is late on a map is the environment fact behind the window suites' old flakes.
        std::printf("[host-window] the server reported the window viewable after %lld ms (usually it is "
                    "already viewable)\n",
                    static_cast<long long>(host.viewWait().count()));
    }

    // Paint the window with plain X - no device, no backend: this case is about the INSTRUMENT.
    const auto          window = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(host.handle()));
    const xcb_gcontext_t gc    = xcb_generate_id(fixture.connection);
    const std::uint32_t  mask  = XCB_GC_FOREGROUND;
    const std::uint32_t  values[] = { fixture.screen->white_pixel };
    xcb_create_gc(fixture.connection, gc, window, mask, values);
    const xcb_rectangle_t rectangle{ 0, 0, static_cast<std::uint16_t>(kWidth),
                                     static_cast<std::uint16_t>(kHeight) };
    xcb_poly_fill_rectangle(fixture.connection, window, gc, 1, &rectangle);
    xcb_flush(fixture.connection);

    const std::array<std::uint8_t, 3> pixel = host.pixel(5, 5);
    EXPECT_EQ(static_cast<int>(host.readError()), 0) << "the read was served, so it has no error to report";
    EXPECT_EQ(static_cast<int>(pixel[0]), 255);
    EXPECT_EQ(static_cast<int>(pixel[1]), 255);
    EXPECT_EQ(static_cast<int>(pixel[2]), 255) << "the bytes the window shows are the bytes that come back";

    xcb_free_gc(fixture.connection, gc);
    xcb_flush(fixture.connection);
}

TEST(HostWindowReadTest, AReadTheServerCannotServeIsRefusedAndSaysSoInsteadOfLookingBlack)
{
    Fixture fixture;
    if (!fixture.open())
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    TestHostWindow host(fixture.connection, fixture.screen, kWidth, kHeight);
    ASSERT_TRUE(host.wasViewableAtCreation());

    // A rectangle far outside the drawable: the server refuses it with BadMatch, deterministically - which is
    // what makes this the guard for the two facts a caller must not conflate.
    const std::array<std::uint8_t, 3> refused = host.pixel(10000, 5);
    EXPECT_NE(static_cast<int>(host.readError()), 0)
        << "a refused read reports the X error it was refused with (0 would make it indistinguishable from a "
           "black pixel)";
    EXPECT_EQ(static_cast<int>(refused[0]) + static_cast<int>(refused[1]) + static_cast<int>(refused[2]), 0)
        << "and the answer is still black - the point is that the caller can tell WHICH of the two it was";

    // The same window answers a legal read right afterwards: the refusal is a property of the request, not of
    // the window or of the connection.
    (void)host.pixel(5, 5);
    EXPECT_EQ(static_cast<int>(host.readError()), 0) << "the window itself is fine";
}

TEST(HostWindowReadTest, TheWaitReturnsWhenThePictureArrivesAndGivesUpAtItsDeadline)
{
    Fixture fixture;
    if (!fixture.open())
    {
        GTEST_SKIP() << "no X display to create a host window on";
    }
    TestHostWindow host(fixture.connection, fixture.screen, kWidth, kHeight);
    ASSERT_TRUE(host.wasViewableAtCreation());

    // A predicate the window already satisfies answers at once: the wait must not cost a deadline where there
    // is nothing to wait for (every window case's first read goes through it).
    const auto began = std::chrono::steady_clock::now();
    (void)host.waitForPixel(5, 5, [](const std::array<std::uint8_t, 3>&) { return true; },
                            std::chrono::milliseconds{ 200 });
    const auto immediate = std::chrono::steady_clock::now() - began;
    EXPECT_LT(immediate, std::chrono::milliseconds{ 100 }) << "an already-satisfied predicate does not wait";

    // A predicate nothing satisfies uses the deadline and ANSWERS ANYWAY (the caller's own assertion is what
    // fails then - the wait never turns "never arrived" into "passed").
    const auto nothing = std::chrono::steady_clock::now();
    const auto pixel   = host.waitForPixel(5, 5, [](const std::array<std::uint8_t, 3>&) { return false; },
                                           std::chrono::milliseconds{ 60 });
    const auto spent   = std::chrono::steady_clock::now() - nothing;
    EXPECT_GE(spent, std::chrono::milliseconds{ 60 }) << "the wait is bounded, not infinite";
    EXPECT_LT(spent, std::chrono::milliseconds{ 600 }) << "and it gives up at its deadline";
    EXPECT_EQ(static_cast<int>(pixel[0]) + static_cast<int>(pixel[1]) + static_cast<int>(pixel[2]), 0)
        << "the last read is what comes back (the window never painted anything here)";
}

#else  // _WIN32

TEST(HostWindowReadTest, TheHostWindowInstrumentIsX11Only)
{
    GTEST_SKIP() << "TestHostWindow is an XCB window; the Windows path has no equivalent readback yet";
}

#endif  // _WIN32
