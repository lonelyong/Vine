#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#    include <xcb/xcb.h>
#endif

#if !defined(_WIN32)

/**
 * @brief A window the TEST owns, so a session can adopt it as the host's and the test can read it back.
 *
 * Three things about it are the point:
 *
 *   * it is created with the SAME attributes the backend's own tests use (a mapped input-output window on the
 *     screen's root, with structure-notify and exposure events selected), because a session that adopts a host
 *     window asks it for exactly those;
 *   * `handle()` is the platform spelling the backend passes around (an XCB window id in a `void*`), so a test
 *     never has to restate the cast;
 *   * it can be read back (`pixel`) and asked whether it still exists (`alive`), which is how a phase tells
 *     "the backend drew this" from "the backend destroyed the host's window".
 *
 * The bytes of a TrueColor pixel come back in the SERVER's order, so a caller's assertions should be written
 * either against a single channel (a pure colour) or against the value's two encodings (see the callers).
 *
 * X11 only, like the cases that use it: this class IS an XCB window, so it does not exist where XCB does not.
 * The callers guard their bodies the same way and SKIP instead of failing where there is no display.
 */
class TestHostWindow
{
  public:
    TestHostWindow(xcb_connection_t* connection, xcb_screen_t* screen, int width, int height)
      : connection_(connection)
    {
        window_                      = xcb_generate_id(connection_);
        const std::uint32_t mask     = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const std::uint32_t values[] = { screen->black_pixel,
                                         XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, screen->root, 0, 0,
                          static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
        xcb_map_window(connection_, window_);
        xcb_flush(connection_);
        // THE WINDOW IS VIEWABLE BEFORE THE FIRST FRAME IS PRESENTED, and that is a measured requirement,
        // not politeness: a session's presents into a window the server has not reported viewable yet never
        // reach it. The window stays on its untouched backing store - black - no matter how many times the
        // picture is presented again afterwards, and the reads made meanwhile are refused with `BadMatch`
        // (X error 8, visible through @ref readError). Measured 2026-09-24 with a 20-run probe around a case
        // that then failed one run in three: viewability arrived 500-900 ms after the map request, i.e.
        // AFTER the case had already presented its picture - and in two runs of twenty it did not arrive at
        // all within 2 s, while the SAME server reported ordinary windows viewable immediately.
        const auto map_requested = std::chrono::steady_clock::now();
        viewable_at_creation_    = waitUntilViewable(kViewDeadline);
        view_wait_               = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - map_requested);
    }

    TestHostWindow(const TestHostWindow&)            = delete;
    TestHostWindow& operator=(const TestHostWindow&) = delete;

    ~TestHostWindow()
    {
        xcb_destroy_window(connection_, window_);
        xcb_flush(connection_);
    }

    /** @brief The handle in the form the backend passes it around. */
    [[nodiscard]] void* handle() const noexcept
    {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window_));
    }

    /** @brief Whether the server had reported the window viewable when the constructor returned.
     *
     * The constructor waits (see @ref kViewDeadline), so this is false only when the server took longer than
     * that - the one state in which a case must not present: its frames would never reach the window.
     */
    [[nodiscard]] bool wasViewableAtCreation() const noexcept
    {
        return viewable_at_creation_;
    }

    /** @brief How long the constructor actually waited for the server's viewable report. */
    [[nodiscard]] std::chrono::milliseconds viewWait() const noexcept
    {
        return view_wait_;
    }

    /** @brief How long @ref resize waits for the server to report the requested size before giving up. */
    static constexpr std::chrono::milliseconds kResizeDeadline{ 500 };

    /** @brief Resizes the window the way a host widget would, and WAITS until the server reports the size.
     *
     * THE WAIT IS THE CONTRACT, not politeness: the backend FOLLOWS the surface by reading the platform's
     * geometry once (see SessionContentAccess::followResizedSurface - one read, no second guess), so a host
     * that announces a size its surface does not have yet hands it a window that answers the previous size,
     * and the session goes on serving images of the old one (a stretched picture, and black once the frames
     * in flight run out). The round trip below is what makes "the surface has it" true before the caller
     * announces: `xcb_configure_window` is unchecked, so the request could fail unnoticed without it, and the
     * geometry reply is the proof - which is why it is READ here instead of thrown away.
     *
     * @param width  New width in pixels.
     * @param height New height in pixels.
     * @return true when the server reports the requested size before the deadline (see kResizeDeadline).
     */
    [[nodiscard]] bool resize(int width, int height)
    {
        const std::uint32_t values[] = { static_cast<std::uint32_t>(width),
                                         static_cast<std::uint32_t>(height) };
        xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
        xcb_flush(connection_);
        const auto deadline = std::chrono::steady_clock::now() + kResizeDeadline;
        for (;;)
        {
            if (auto* reply = xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window_), nullptr))
            {
                const bool applied = reply->width == width && reply->height == height;
                std::free(reply);
                if (applied)
                {
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /** @brief Whether the window still exists (the session must not have destroyed the host's own window). */
    [[nodiscard]] bool alive() const
    {
        const auto* reply =
            xcb_get_window_attributes_reply(connection_, xcb_get_window_attributes(connection_, window_), nullptr);
        const bool alive = reply != nullptr;
        if (reply != nullptr)
        {
            std::free(const_cast<xcb_get_window_attributes_reply_t*>(reply));
        }
        return alive;
    }

    /** @brief How long the constructor waits for the server to report the window viewable.
     *
     * Sized from a measurement, not from taste: viewability arrived 500-900 ms after the map request in the
     * runs where it was late at all, and the frames a case presents before it are lost (see the constructor).
     * The deadline is deliberately generous, because the cost of waiting is paid ONLY by the runs in which
     * the server is late (an already-viewable window costs one round trip), while the cost of giving up is a
     * case that cannot possibly pass: its presents never reach the window.
     */
    static constexpr std::chrono::milliseconds kViewDeadline{ 10000 };

    /** @brief How long @ref pixel waits for the server to report the window viewable before reading.
     *
     * Short on purpose: the constructor already waited (see @ref kViewDeadline), so the poll here is a round
     * trip in the common case and only covers a window that was unmapped again meanwhile. A refused read is
     * reported through @ref readError instead of being waited out.
     */
    static constexpr std::chrono::milliseconds kReadDeadline{ 50 };

    /** @brief Waits until the server reports the window VIEWABLE (mapped, and every ancestor mapped).
     *
     * WHY A READ WAITS AT ALL. `GetImage` is refused with `BadMatch` for a window the server does not consider
     * viewable yet, and a refused read looks exactly like a black pixel in @ref pixel's return value (see
     * @ref readError). Waiting for the server's own report is the sync point a reader needs - measured
     * 2026-09-24, before the constructor waited as well, when a probe inside a drawn triangle answered
     * (0,0,0) while its neighbours answered the picture.
     *
     * @param deadline How long to keep asking before giving up.
     * @return true when the server reported VIEWABLE before the deadline (already true is the common case).
     */
    [[nodiscard]] bool waitUntilViewable(std::chrono::milliseconds deadline) const
    {
        const auto give_up = std::chrono::steady_clock::now() + deadline;
        for (;;)
        {
            const auto* reply = xcb_get_window_attributes_reply(
                connection_, xcb_get_window_attributes(connection_, window_), nullptr);
            const bool viewable = reply != nullptr && reply->map_state == XCB_MAP_STATE_VIEWABLE;
            std::free(const_cast<xcb_get_window_attributes_reply_t*>(reply));
            if (viewable)
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= give_up)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /** @brief Reads one pixel back out of the window (XGetImage's server-side equivalent).
     *
     * The read WAITS for the server to report the window viewable first (see @ref waitUntilViewable), so a
     * window the server has not finished mapping answers its real content instead of a refusal.
     *
     * @param x Column to read.
     * @param y Row to read.
     * @return The first three bytes of the pixel; black when the server refuses the read (see @ref readError
     *         for the refusal's own fact - the two are different answers and only one of them is a picture).
     */
    [[nodiscard]] std::array<std::uint8_t, 3> pixel(int x, int y) const
    {
        (void)waitUntilViewable(kReadDeadline);
        const xcb_get_image_cookie_t cookie =
            xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, static_cast<std::int16_t>(x),
                          static_cast<std::int16_t>(y), 1U, 1U, ~0U);
        xcb_generic_error_t*   error = nullptr;
        xcb_get_image_reply_t* reply = xcb_get_image_reply(connection_, cookie, &error);
        last_read_error_             = error != nullptr ? static_cast<std::uint8_t>(error->error_code) : 0U;
        std::free(error);
        if (reply == nullptr)
        {
            return { 0U, 0U, 0U };
        }
        const std::uint8_t*               data = xcb_get_image_data(reply);
        const std::array<std::uint8_t, 3> rgb{ data[0], data[1], data[2] };
        std::free(reply);
        return rgb;
    }

    /** @brief Gets the X error the last @ref pixel read was refused with (0 = it succeeded).
     *
     * WHY IT IS A FACT OF ITS OWN: "the server refused the read" and "the pixel is black" look identical
     * in @ref pixel's return value, and a caller that cannot tell them apart reads a race as a picture.
     * Measured 2026-09-24 in a full-suite run: a probe inside a drawn triangle answered (0,0,0) while its
     * neighbours answered the picture, and the reply had simply been NULL.
     */
    [[nodiscard]] std::uint8_t readError() const noexcept
    {
        return last_read_error_;
    }

    /** @brief How long @ref waitForPixel waits between two reads.
     *
     * NOT A POLLING INTERVAL, and it was raised from 2 ms after a measurement: a tight poll makes its own
     * round trips, and a sequence of them competes with the display path's copy of the very picture it waits
     * for (measured 2026-09-24: a 2 ms poll over a 250 ms budget - ~125 `GetImage` round trips - failed in
     * five of six full-suite runs, where the same cases had passed with a single read).
     */
    static constexpr std::chrono::milliseconds kProbeInterval{ 50 };

    /** @brief Reads (x, y) until @p accept accepts it, and returns the LAST read either way.
     *
     * WHY A WINDOW READ MAY NEED TO WAIT. A session's presents are answered before the DISPLAY PATH has the
     * picture (see VsgBackend::framesPresented, which moves when swapBuffers returns), so the read that
     * follows can answer the window's own background. Measured 2026-09-24 with a 20-run probe: a run read
     * (0,0,0) with `readError() == 8` (`BadMatch`, the window not viewable - the constructor now waits for
     * that) and then (0,0,0) with `readError() == 0` for four more reads over ≈ 1.3 s, and answered the
     * picture (0,137,0) on the read AFTER the case presented its graph once more. So two different facts
     * live here: the presents made before the window was viewable were LOST (they never arrive, however
     * long the caller waits), and a present made after it does arrive - which is why the window cases
     * "settle" by re-presenting the PICTURE and not with plan-free frames: a plan-free frame repaints the
     * window with the session's own graph, so the read that catches it answers the window's background.
     *
     * WHAT THIS IS NOT: an assertion device. It waits for the picture to ARRIVE; a picture that is
     * permanently wrong never satisfies @p accept and the caller's own assertion still fails. What it does
     * not catch is a picture that is transiently wrong and then right - the price of reading an asynchronous
     * display, which is why the wait is bounded, and why it is named after what it waits for.
     *
     * @param x        Column to read.
     * @param y        Row to read.
     * @param accept   Predicate the pixel has to satisfy (e.g. `isRed`).
     * @param deadline How long to keep reading before giving up.
     * @return The last pixel read (the first one @p accept accepted, when it did).
     */
    template <typename Accept>
    [[nodiscard]] std::array<std::uint8_t, 3> waitForPixel(int x, int y, Accept accept,
                                                           std::chrono::milliseconds deadline) const
    {
        const auto deadline_at = std::chrono::steady_clock::now() + deadline;
        for (;;)
        {
            const std::array<std::uint8_t, 3> pixel = this->pixel(x, y);
            if (accept(pixel))
            {
                return pixel;
            }
            if (std::chrono::steady_clock::now() >= deadline_at)
            {
                return pixel;
            }
            std::this_thread::sleep_for(kProbeInterval);
        }
    }

  private:
    xcb_connection_t*        connection_;
    xcb_window_t             window_{ 0 };
    bool                     viewable_at_creation_{ false };  ///< See wasViewableAtCreation.
    std::chrono::milliseconds view_wait_{ 0 };                 ///< See viewWait.
    mutable std::uint8_t     last_read_error_{ 0 };            ///< See readError: written by the const reads above.
};

/**
 * @brief Closes the test's X connection when the scope ends.
 *
 * `xcb_connect` has no RAII of its own, and the device cases connected and never disconnected: the WHOLE
 * strict-leak ASan/LSan report of test_vsg was these connections ("Direct leak of 21176 byte(s)" x22, the
 * connection's buffer, measured 2026-09-25). Declare one of these right AFTER the connection and make sure
 * it is declared BEFORE the objects built on it: C++ destroys in reverse declaration order, so the windows
 * go first and this closes the connection last.
 */
class TestXConnection
{
  public:
    explicit TestXConnection(xcb_connection_t* connection) noexcept : connection_(connection) {}

    ~TestXConnection()
    {
        if (connection_ != nullptr)
        {
            xcb_disconnect(connection_);
        }
    }

    TestXConnection(const TestXConnection&)            = delete;
    TestXConnection& operator=(const TestXConnection&) = delete;

  private:
    xcb_connection_t* connection_;
};

#endif  // !_WIN32
