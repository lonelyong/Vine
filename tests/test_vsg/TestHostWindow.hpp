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
    }

    TestHostWindow(const TestHostWindow&)            = delete;
    TestHostWindow& operator=(const TestHostWindow&) = delete;

    ~TestHostWindow()
    {
        xcb_destroy_window(connection_, window_);
        xcb_flush(connection_);
    }

    /** @brief The handle in the form the backend passes it around. */
    /// @brief How long @ref resize waits for the server to report the requested size before giving up.
    static constexpr std::chrono::milliseconds kResizeDeadline{ 500 };

    [[nodiscard]] void* handle() const noexcept
    {
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(window_));
    }

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

    /** @brief Reads one pixel back out of the window (XGetImage's server-side equivalent).
     *
     * @param x Column to read.
     * @param y Row to read.
     * @return The first three bytes of the pixel; black when the server refuses the read.
     */
    [[nodiscard]] std::array<std::uint8_t, 3> pixel(int x, int y) const
    {
        const xcb_get_image_cookie_t cookie =
            xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, static_cast<std::int16_t>(x),
                          static_cast<std::int16_t>(y), 1U, 1U, ~0U);
        xcb_get_image_reply_t* reply = xcb_get_image_reply(connection_, cookie, nullptr);
        if (reply == nullptr)
        {
            return { 0U, 0U, 0U };
        }
        const std::uint8_t*               data = xcb_get_image_data(reply);
        const std::array<std::uint8_t, 3> rgb{ data[0], data[1], data[2] };
        std::free(reply);
        return rgb;
    }

  private:
    xcb_connection_t* connection_;
    xcb_window_t      window_{ 0 };
};

#endif  // !_WIN32
