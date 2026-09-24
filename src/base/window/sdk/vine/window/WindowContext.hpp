#pragma once

#include "window_global.hpp"

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/Signal.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>

#include "InputEvent.hpp"
#include "WindowEvent.hpp"

VN_WINDOW_NS_BEGIN

/**
 * @brief Read-only window data context shared between a host and the render layer.
 *
 * WindowContext is an abstract interface, NOT a window manager and NOT a data
 * store: it does not create, destroy, own or mutate a native window. A derived
 * class holds a pointer to the real window (vsg::Window, a Qt wrapper, ...)
 * and overrides the accessors below to query that window's live state. The
 * render layer (RenderBackend, RenderEngine, camera manipulator) consumes the
 * context through this interface — reading the native handle for surface
 * creation and subscribing to the signals for interaction.
 *
 * Events are delivered via the Signal members. The host (or the derived
 * window wrapper) injects them through postEvent(), which is the single entry
 * point used both by native event translation and by synthetic event
 * injection (tests).
 */
class VN_WINDOW_API WindowContext : public Object, public RefCounted<WindowContext> {
    VN_OBJECT_META_DECL;

  public:
    ~WindowContext() override = default;

  public:
    // ---- Read-only queries (overridden by the derived window wrapper) ----

    /** @brief Gets the window title. */
    virtual String title() const = 0;

    /** @brief Gets the client area width in pixels. */
    virtual int width() const = 0;

    /** @brief Gets the client area height in pixels. */
    virtual int height() const = 0;

    /** @brief Gets whether the window is visible. */
    virtual bool isVisible() const = 0;

    /** @brief Gets the native window handle (HWND, X11 Window, ...).
     *
     * @return Opaque native handle, or nullptr when not yet available.
     */
    virtual void* nativeHandle() const = 0;

  public:
    // ---- Events ----

    /** @brief Fired when the client area is resized. Args: width, height. */
    Signal<int, int> resized;

    /** @brief Fired when the window is moved on screen. Args: x, y. */
    Signal<int, int> moved;

    /** @brief Fired when the user requests to close the window. */
    Signal<> closed;

    /** @brief Fired when the window gains keyboard focus. */
    Signal<> focusIn;

    /** @brief Fired when the window loses keyboard focus. */
    Signal<> focusOut;

    /** @brief Fired when the native surface becomes ready for rendering.
     *
     * The host window wrapper fires this once the underlying window is
     * exposed (shown with a valid size), which is when a render backend can
     * safely attach to its native surface.
     */
    Signal<> exposed;

    /** @brief Fired on key press/release. */
    Signal<const KeyEvent&> key;

    /** @brief Fired on mouse button/motion events. */
    Signal<const MouseEvent&> mouse;

    /** @brief Fired on mouse wheel scroll. */
    Signal<const ScrollEvent&> scroll;

    /** @brief Fired on window lifecycle events (minimize/restore, ...). */
    Signal<const WindowEvent&> windowEvent;

    /**
     * @brief Injects a typed event, dispatching it to the matching signal.
     *
     * This is the single entry point used both by the host's native event
     * translation and by synthetic event injection (tests). The host decides
     * when events are delivered; WindowContext never polls.
     *
     * @tparam TEvent One of KeyEvent, MouseEvent, ScrollEvent or WindowEvent.
     * @param event   Event to dispatch.
     */
    template <typename TEvent>
    void postEvent(const TEvent& event);

  protected:
    WindowContext() = default;
};

template <typename TEvent>
void WindowContext::postEvent(const TEvent& event)
{
    if constexpr (std::is_same_v<TEvent, KeyEvent>) {
        key.trigger(event);
    } else if constexpr (std::is_same_v<TEvent, MouseEvent>) {
        mouse.trigger(event);
    } else if constexpr (std::is_same_v<TEvent, ScrollEvent>) {
        scroll.trigger(event);
    } else if constexpr (std::is_same_v<TEvent, WindowEvent>) {
        windowEvent.trigger(event);
    } else {
        static_assert(std::is_same_v<TEvent, KeyEvent> || std::is_same_v<TEvent, MouseEvent> ||
                          std::is_same_v<TEvent, ScrollEvent> || std::is_same_v<TEvent, WindowEvent>,
                      "postEvent() supports KeyEvent, MouseEvent, ScrollEvent and WindowEvent");
    }
}

VN_WINDOW_NS_END
