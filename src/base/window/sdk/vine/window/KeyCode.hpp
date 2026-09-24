#pragma once

#include "window_global.hpp"

#include <cstdint>

VN_WINDOW_NS_BEGIN

/**
 * @brief Platform-independent key codes.
 *
 * Window backends (Win32, X11, VulkanSceneGraph, ...) translate their native
 * key identifiers into these codes before publishing input events. The enum
 * intentionally does not cover every physical key of every keyboard layout;
 * unknown keys map to KeyCode::Unknown.
 */
enum class KeyCode : std::uint32_t {
    Unknown = 0,

    // Letters (US layout).
    A = 1, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Digits (top row).
    D0, D1, D2, D3, D4, D5, D6, D7, D8, D9,

    // Function keys.
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // Whitespace and editing.
    Space,
    Enter,
    Tab,
    Backspace,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,

    // Cursor / navigation.
    Left,
    Right,
    Up,
    Down,

    // Modifiers.
    Shift,
    Control,
    Alt,
    Super,  ///< Windows key / Command key.

    // Punctuation (US layout).
    Minus,
    Equal,
    BracketLeft,
    BracketRight,
    Backslash,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    Grave,  ///< Backtick `.

    // Numpad.
    Numpad0, Numpad1, Numpad2, Numpad3, Numpad4,
    Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
    NumpadDecimal,
    NumpadDivide,
    NumpadMultiply,
    NumpadSubtract,
    NumpadAdd,
    NumpadEnter,

    // Lock keys.
    CapsLock,
    NumLock,
    ScrollLock,

    // Other.
    Escape,
    PrintScreen,
    Pause,
    Menu,
    ContextMenu,
};

VN_WINDOW_NS_END
