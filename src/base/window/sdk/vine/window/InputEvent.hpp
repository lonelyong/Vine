#pragma once

#include "window_global.hpp"

#include "KeyCode.hpp"
#include "MouseButton.hpp"

VN_WINDOW_NS_BEGIN

/**
 * @brief Keyboard key event.
 *
 * Published on key press and release. A repeat is a key press generated while
 * the key is held down.
 */
struct VN_WINDOW_API KeyEvent {
    KeyCode     code = KeyCode::Unknown;  ///< Physical key that changed.
    ModifierKey modifiers = ModifierKey::None;
    bool        pressed = false;          ///< false when the key was released.
    bool        repeat = false;           ///< true for auto-repeat presses.
};

/**
 * @brief Mouse button / motion event.
 *
 * x/y are in window client coordinates (origin at the top-left), in device
 * (pixel) units. dx/dy are the deltas relative to the previous event.
 * When only the pointer moved (no button involved) button is MouseButton::None.
 */
struct VN_WINDOW_API MouseEvent {
    MouseButton button = MouseButton::None;
    ModifierKey modifiers = ModifierKey::None;
    double      x = 0.0;
    double      y = 0.0;
    double      dx = 0.0;
    double      dy = 0.0;
    bool        pressed = false;  ///< true for a button press, false for a release.
};

/**
 * @brief Mouse wheel / scroll event.
 *
 * deltaX/deltaY are scroll amounts; a positive deltaY scrolls up/away, a
 * positive deltaX scrolls right. Units are lines (or notches) by default.
 */
struct VN_WINDOW_API ScrollEvent {
    double      deltaX = 0.0;
    double      deltaY = 0.0;
    ModifierKey modifiers = ModifierKey::None;
};

/**
 * @brief Window surface resize event.
 *
 * Carries the new client-area size in device (pixel) units, matching the
 * extent the render backend should rebuild its swapchain to.
 */
struct VN_WINDOW_API ResizeEvent {
    int width = 0;   ///< New client width in pixels.
    int height = 0;  ///< New client height in pixels.
};

VN_WINDOW_NS_END
