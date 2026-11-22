#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <dwmapi.h>
#else
#    include <xcb/xcb.h>
#endif

#if defined(_WIN32)

/**
 * @brief The desktop a host window is created on, in RAII form.
 *
 * X11 needs a connection and a screen before a window exists, and the connection has to be closed AFTER every
 * window built on it (see the X11 branch's note); Win32 needs neither - every process has the desktop it runs
 * on. This is the object a case declares first and TestHostWindow takes, so a case's setup reads the same on
 * both platforms.
 */
class TestDisplay {
  public:
    /**
     * @brief Measures whether this process can create a window at all.
     *
     * Win32 has no connection to open; the fact that can be missing is a desktop (a service session has none),
     * so the answer is measured by creating the cheapest possible window and destroying it again - the same
     * failure CreateWindowExW answers with when a case builds its host window.
     *
     * @return true when a window can be created.
     */
    bool open()
    {
        const HWND probe = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (probe == nullptr) {
            return false;
        }
        DestroyWindow(probe);
        open_ = true;
        return true;
    }

    explicit operator bool() const noexcept
    {
        return open_;
    }

    /** @brief The reason @ref open answered false, in the words a SKIP message wants. */
    [[nodiscard]] const char* skipReason() const noexcept
    {
        return "no desktop to create a host window on";
    }

  private:
    bool open_{ false };
};

/**
 * @brief A window the TEST owns, so a session can adopt it as the host's and the test can read it back.
 *
 * The Win32 counterpart of the XCB window below: the same contract, the same member names and the same
 * constructor, with one fact to know about the read.
 *
 *   * it is a POPUP window, so its CLIENT area is exactly the size asked for - a decorated window would shrink
 *     the client area by its frame, and every pixel assertion would be written against the wrong extent;
 *   * `handle()` is the platform spelling the backend passes around (an `HWND` in a `void*`), so a test never
 *     has to restate the cast;
 *   * it can be read back (`pixel`) and asked whether it still exists (`alive`), which is how a phase tells
 *     "the backend drew this" from "the backend destroyed the host's window".
 *
 * HOW A WIN32 PIXEL IS READ. `PrintWindow` with `PW_RENDERFULLCONTENT` asks the DESKTOP COMPOSITOR for the
 * window's content, which is the only path that sees a flip-model swapchain's pixels: the window's own device
 * context answers what GDI last drew, and a Vulkan present does not go through GDI at all. That device context
 * is still the FALLBACK when the compositor refuses, so the fixture can measure itself with plain GDI (see
 * HostWindowReadTest) before a device is involved.
 *
 * A refused read is reported the way the X11 fixture reports an X error: @ref readError is 0 when the read was
 * served and non-zero when it was refused, so "the picture is black" and "nobody answered" stay different
 * answers. Win32 has no X error code; the two refusals this fixture can tell apart are named constants.
 */
class TestHostWindow {
  public:
    TestHostWindow(const TestDisplay& display, int width, int height)
    {
        // Win32 needs nothing from the display: there is one desktop per process (see TestDisplay), and the
        // constructor below is the same on both platforms so a case's setup reads the same.
        (void)display;
        registerWindowClass();
        const auto began = std::chrono::steady_clock::now();
        // WS_VISIBLE in the create call: a Win32 window is on the screen when CreateWindowEx returns it, so
        // unlike X11 there is no asynchronous "viewable" report to wait for. The fact is still MEASURED rather
        // than assumed, and the wait is reported (it is the class construction itself).
        // WS_EX_TOPMOST keeps the window COMPOSED: a fully occluded window's presents are skipped by the
        // compositor (measured 2026-11-23: reads of an occluded test window answered black with a success code,
        // and the same case passed whenever the window happened to be visible).
        window_ = CreateWindowExW(WS_EX_TOPMOST,
                                  kWindowClassName,
                                  L"vine host-window test",
                                  WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                  100,
                                  100,
                                  width,
                                  height,
                                  nullptr,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr);
        if (window_ != nullptr) {
            UpdateWindow(window_);
            SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        viewable_at_creation_ = window_ != nullptr && IsWindowVisible(window_) != FALSE;
        view_wait_            = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    }

    TestHostWindow(const TestHostWindow&)            = delete;
    TestHostWindow& operator=(const TestHostWindow&) = delete;

    ~TestHostWindow()
    {
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    /** @brief The handle in the form the backend passes it around (an HWND as a `void*`). */
    [[nodiscard]] void* handle() const noexcept
    {
        return window_;
    }

    /** @brief Whether the window was visible when the constructor returned (see the constructor). */
    [[nodiscard]] bool wasViewableAtCreation() const noexcept
    {
        return viewable_at_creation_;
    }

    /** @brief How long the constructor actually took to make the window visible. */
    [[nodiscard]] std::chrono::milliseconds viewWait() const noexcept
    {
        return view_wait_;
    }

    /** @brief How long @ref resize waits for the client area to report the requested size before giving up. */
    static constexpr std::chrono::milliseconds kResizeDeadline{ 500 };

    /**
     * @brief Resizes the window the way a host widget would, and WAITS until the client area reports the size.
     *
     * The wait is the same contract as the X11 fixture's: the backend follows the surface by reading the
     * platform's geometry once, so a host that announces a size its window does not have yet hands it a window
     * that answers the previous size.
     *
     * @param width  New width in pixels.
     * @param height New height in pixels.
     * @return true when the client area reports the requested size before the deadline (see kResizeDeadline).
     */
    [[nodiscard]] bool resize(int width, int height)
    {
        if (window_ == nullptr) {
            return false;
        }
        SetWindowPos(window_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const auto deadline = std::chrono::steady_clock::now() + kResizeDeadline;
        for (;;) {
            RECT client{};
            if (GetClientRect(window_, &client) != FALSE && client.right - client.left == width && client.bottom - client.top == height) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /** @brief Whether the window still exists (the session must not have destroyed the host's own window). */
    [[nodiscard]] bool alive() const noexcept
    {
        return window_ != nullptr && IsWindow(window_) != FALSE;
    }

    /** @brief How long a case may wait for the window to become visible (Win32 answers at once; the name is shared). */
    static constexpr std::chrono::milliseconds kViewDeadline{ 10000 };

    /** @brief How long @ref pixel may wait for the window to be visible before reading (Win32 answers at once). */
    static constexpr std::chrono::milliseconds kReadDeadline{ 50 };

    /**
     * @brief Whether the window is visible (Win32 has no asynchronous map report; see the constructor).
     *
     * @param deadline Unused on Win32: the answer is a fact of the window, not of a server's queue.
     * @return True when the window is visible.
     */
    [[nodiscard]] bool waitUntilViewable(std::chrono::milliseconds deadline) const noexcept
    {
        (void)deadline;
        return window_ != nullptr && IsWindowVisible(window_) != FALSE;
    }

    /** @brief @ref readError value: the read was served (the bytes are the window's content). */
    static constexpr std::uint8_t kReadServed{ 0 };
    /** @brief @ref readError value: the request is outside the window's client area (X11's BadMatch counterpart). */
    static constexpr std::uint8_t kReadOutsideTheWindow{ 1 };
    /** @brief @ref readError value: the system could not produce a picture at all (no display context, no compositor). */
    static constexpr std::uint8_t kReadRefused{ 2 };

    /**
     * @brief Reads one pixel back out of the window (see the class note for which path answers).
     *
     * THE FIRST READ OF A WINDOW'S FIRST CONTENT IS WARMED UP, not answered: measured 2026-11-23 on Windows 11,
     * the compositor's first capture after a new window's first real content change (a GDI fill) answered BLACK
     * with a success code, and the next capture - microseconds later - answered the fill. The fixture therefore
     * spends one capture the first time a caller reads, and answers the second; every later read is one capture.
     *
     * @param x Column to read, in client-area pixels.
     * @param y Row to read, in client-area pixels.
     * @return The window's first three bytes for that pixel, in the SURFACE's own order (see @ref capture), and
     *         black when the read is refused (see @ref readError for the refusal's own fact - the two are
     *         different answers and only one of them is a picture).
     */
    [[nodiscard]] std::array<std::uint8_t, 3> pixel(int x, int y) const
    {
        if (!compositor_warmed_) {
            compositor_warmed_ = true;
            (void)capture(x, y);
        }
        return capture(x, y);
    }

    /** @brief Gets the refusal the last @ref pixel read answered with (kReadServed = it succeeded). */
    [[nodiscard]] std::uint8_t readError() const noexcept
    {
        return last_read_error_;
    }

    /** @brief How long @ref waitForPixel waits between two reads (the X11 fixture's measurement, kept in step). */
    static constexpr std::chrono::milliseconds kProbeInterval{ 50 };

    /**
     * @brief Reads (x, y) until @p accept accepts it, and returns the LAST read either way.
     *
     * The same contract as the X11 fixture's: a session's presents are answered before the compositor has the
     * picture, so a read that follows can answer the window's background; the wait is bounded and it never
     * turns "never arrived" into "passed" (the caller's own assertion does that).
     *
     * @param x        Column to read.
     * @param y        Row to read.
     * @param accept   Predicate the pixel has to satisfy.
     * @param deadline How long to keep reading before giving up.
     * @return The last pixel read (the first one @p accept accepted, when it did).
     */
    template <typename Accept>
    [[nodiscard]] std::array<std::uint8_t, 3> waitForPixel(int x, int y, Accept accept, std::chrono::milliseconds deadline) const
    {
        const auto deadline_at = std::chrono::steady_clock::now() + deadline;
        for (;;) {
            const std::array<std::uint8_t, 3> pixel = this->pixel(x, y);
            if (accept(pixel)) {
                return pixel;
            }
            if (std::chrono::steady_clock::now() >= deadline_at) {
                return pixel;
            }
            std::this_thread::sleep_for(kProbeInterval);
        }
    }

  private:
    static constexpr const wchar_t* kWindowClassName              = L"VineTestHostWindow";
    static constexpr UINT           kPrintWindowRenderFullContent = 0x00000002U;

    /** @brief One compositor capture of the window's client area, and the pixel @p x, @p y of it. */
    [[nodiscard]] std::array<std::uint8_t, 3> capture(int x, int y) const
    {
        last_read_error_ = kReadServed;
        if (window_ == nullptr) {
            last_read_error_ = kReadRefused;
            return { 0U, 0U, 0U };
        }
        RECT client{};
        if (GetClientRect(window_, &client) == FALSE) {
            last_read_error_ = kReadRefused;
            return { 0U, 0U, 0U };
        }
        const int width  = client.right - client.left;
        const int height = client.bottom - client.top;
        if (x < 0 || y < 0 || x >= width || y >= height) {
            last_read_error_ = kReadOutsideTheWindow;
            return { 0U, 0U, 0U };
        }
        const HDC screen = GetDC(nullptr);
        if (screen == nullptr) {
            last_read_error_ = kReadRefused;
            return { 0U, 0U, 0U };
        }
        const HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth       = width;
        info.bmiHeader.biHeight      = -height; // Top-down: row 0 is the top row, matching the y asked for.
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void*         bits   = nullptr;
        const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (memory == nullptr || bitmap == nullptr || bits == nullptr) {
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
            if (memory != nullptr) {
                DeleteDC(memory);
            }
            ReleaseDC(nullptr, screen);
            last_read_error_ = kReadRefused;
            return { 0U, 0U, 0U };
        }

        const HGDIOBJ previous = SelectObject(memory, bitmap);
        // THE FIXTURE PUMPS THE THREAD'S MESSAGES, like the host a case models: Win32 marks a thread that never
        // pumps as not responding, and the compositor stops delivering new content to such a window (measured
        // 2026-11-23: reads of a window created late in a long run answered its black background while the same
        // case passed alone). Pumping is what a host's own loop does and costs nothing when the queue is empty.
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != FALSE) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        // DWM COMPOSES THE WINDOW'S PIXELS, SO A READ WAITS FOR THE NEXT COMPOSITION PASS (measured 2026-11-23:
        // a read right after a present answered the window's black background, and the same case passed when the
        // read followed a composition). DwmFlush is that pass's own sync point - the Win32 counterpart of the X11
        // fixture's viewable/round-trip waits - and a failed flush (composition disabled) simply costs nothing.
        (void)::DwmFlush();
        // PW_RENDERFULLCONTENT (= 2, Windows 8.1+) is not a named constant on every SDK, so the value is named
        // here: it is what makes PrintWindow ask the COMPOSITOR instead of sending WM_PRINT to an application
        // that has no handler for it.
        BOOL served = PrintWindow(window_, memory, kPrintWindowRenderFullContent);
        if (served == FALSE) {
            // The fallback for content GDI itself drew (the instrument's own case): copy the window's device
            // context, which is also what a window without a composited surface can answer.
            const HDC source = GetDC(window_);
            if (source != nullptr) {
                served = BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY);
                ReleaseDC(window_, source);
            }
        }
        std::array<std::uint8_t, 3> rgb{ 0U, 0U, 0U };
        if (served == FALSE) {
            last_read_error_ = kReadRefused;
        }
        else {
            // THE BYTES COME BACK IN THE SURFACE'S OWN ORDER, like the X11 fixture's server order: a GDI-painted
            // window answers blue-green-red (GDI's 32-bit DIB order) and a Vulkan window answers in its
            // swapchain's order (measured 2026-11-23 on Windows 11), so an assertion is written against a
            // single channel (a pure colour) or against both encodings - see the callers.
            const auto* bytes  = static_cast<const std::uint8_t*>(bits);
            const auto  offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4U;
            rgb                = { bytes[offset + 0U], bytes[offset + 1U], bytes[offset + 2U] };
        }

        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return rgb;
    }

    /** @brief Registers the fixture's window class once per process (a window cannot be created without it). */
    static void registerWindowClass()
    {
        static const ATOM registered = []() -> ATOM {
            WNDCLASSEXW description{};
            description.cbSize        = sizeof(description);
            description.style         = CS_HREDRAW | CS_VREDRAW;
            description.lpfnWndProc   = ::DefWindowProcW;
            description.hInstance     = ::GetModuleHandleW(nullptr);
            description.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
            description.lpszClassName = kWindowClassName;
            return ::RegisterClassExW(&description);
        }();
        // A failure here (other than the class already existing) leaves CreateWindowExW answering null, which
        // every case reports through wasViewableAtCreation() / alive() - the fixture has nothing else to say.
        (void)registered;
    }

    HWND                      window_{ nullptr };
    bool                      viewable_at_creation_{ false };           // See wasViewableAtCreation.
    std::chrono::milliseconds view_wait_{ 0 };                           // See viewWait.
    mutable std::uint8_t      last_read_error_{ kReadServed };           // See readError: written by the const reads.
    mutable bool              compositor_warmed_{ false };               // See pixel: the first read's warm-up capture.
};

#else

/**
 * @brief The display a host window is created on, in RAII form: the X connection and the screen it names.
 *
 * `xcb_connect` has no RAII of its own, and the device cases connected and never disconnected: the WHOLE
 * strict-leak ASan/LSan report of test_vsg was these connections ("Direct leak of 21176 byte(s)" x22, the
 * connection's buffer, measured 2026-09-25). Declare one of these right AFTER opening the display and make
 * sure it outlives every window built on it: C++ destroys in reverse declaration order, so the windows go
 * first and this closes the connection last.
 *
 * Win32 needs neither (see the Win32 branch), which is why a case takes this object rather than the two X
 * values directly.
 */
class TestDisplay {
  public:
    /**
     * @brief Connects to the default X display and takes its first screen.
     *
     * @return true when a connection was opened and it has a screen; false is the case's SKIP signal.
     */
    bool open()
    {
        int screen_index = 0;
        connection_      = xcb_connect(nullptr, &screen_index);
        if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0) {
            return false;
        }
        screen_ = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
        return screen_ != nullptr;
    }

    ~TestDisplay()
    {
        if (connection_ != nullptr) {
            xcb_disconnect(connection_);
        }
    }

    TestDisplay(const TestDisplay&)            = delete;
    TestDisplay& operator=(const TestDisplay&) = delete;

    explicit operator bool() const noexcept
    {
        return screen_ != nullptr;
    }

    /** @brief The reason @ref open answered false, in the words a SKIP message wants. */
    [[nodiscard]] const char* skipReason() const noexcept
    {
        return "no X display to create a host window on";
    }

    /** @brief The connection a host window is created on (valid after open() answered true). */
    [[nodiscard]] xcb_connection_t* connection() const noexcept
    {
        return connection_;
    }

    /** @brief The screen a host window is created on (valid after open() answered true). */
    [[nodiscard]] xcb_screen_t* screen() const noexcept
    {
        return screen_;
    }

  private:
    xcb_connection_t* connection_{ nullptr };
    xcb_screen_t*     screen_{ nullptr };
};

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
 * X11 or Win32, like the cases that use it: the class exists on both platforms behind one contract (the Win32
 * counterpart above), and the callers SKIP instead of failing where there is no display. The connection and the
 * screen it names come from @ref TestDisplay, so a case's setup reads the same on both platforms.
 */
class TestHostWindow {
  public:
    TestHostWindow(const TestDisplay& display, int width, int height)
      : connection_(display.connection())
    {
        xcb_screen_t* const screen = display.screen();
        window_                      = xcb_generate_id(connection_);
        const std::uint32_t mask     = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
        const std::uint32_t values[] = { screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(connection_,
                          XCB_COPY_FROM_PARENT,
                          window_,
                          screen->root,
                          0,
                          0,
                          static_cast<std::uint16_t>(width),
                          static_cast<std::uint16_t>(height),
                          0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen->root_visual,
                          mask,
                          values);
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
        view_wait_               = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - map_requested);
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
        const std::uint32_t values[] = { static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
        xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
        xcb_flush(connection_);
        const auto deadline = std::chrono::steady_clock::now() + kResizeDeadline;
        for (;;) {
            if (auto* reply = xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, window_), nullptr)) {
                const bool applied = reply->width == width && reply->height == height;
                std::free(reply);
                if (applied) {
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /** @brief Whether the window still exists (the session must not have destroyed the host's own window). */
    [[nodiscard]] bool alive() const
    {
        const auto* reply = xcb_get_window_attributes_reply(connection_, xcb_get_window_attributes(connection_, window_), nullptr);
        const bool  alive = reply != nullptr;
        if (reply != nullptr) {
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
        for (;;) {
            const auto* reply    = xcb_get_window_attributes_reply(connection_, xcb_get_window_attributes(connection_, window_), nullptr);
            const bool  viewable = reply != nullptr && reply->map_state == XCB_MAP_STATE_VIEWABLE;
            std::free(const_cast<xcb_get_window_attributes_reply_t*>(reply));
            if (viewable) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= give_up) {
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
            xcb_get_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), 1U, 1U, ~0U);
        xcb_generic_error_t*   error = nullptr;
        xcb_get_image_reply_t* reply = xcb_get_image_reply(connection_, cookie, &error);
        last_read_error_             = error != nullptr ? static_cast<std::uint8_t>(error->error_code) : 0U;
        std::free(error);
        if (reply == nullptr) {
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
    [[nodiscard]] std::array<std::uint8_t, 3> waitForPixel(int x, int y, Accept accept, std::chrono::milliseconds deadline) const
    {
        const auto deadline_at = std::chrono::steady_clock::now() + deadline;
        for (;;) {
            const std::array<std::uint8_t, 3> pixel = this->pixel(x, y);
            if (accept(pixel)) {
                return pixel;
            }
            if (std::chrono::steady_clock::now() >= deadline_at) {
                return pixel;
            }
            std::this_thread::sleep_for(kProbeInterval);
        }
    }

  private:
    xcb_connection_t*         connection_;
    xcb_window_t              window_{ 0 };
    bool                      viewable_at_creation_{ false }; // See wasViewableAtCreation.
    std::chrono::milliseconds view_wait_{ 0 };                // See viewWait.
    mutable std::uint8_t      last_read_error_{ 0 };          // See readError: written by the const reads above.
};

#endif // _WIN32
