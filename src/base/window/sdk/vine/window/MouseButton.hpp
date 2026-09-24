#pragma once

#include "window_global.hpp"

#include <cstdint>

VN_WINDOW_NS_BEGIN

/**
 * @brief Mouse buttons reported by a pointing device.
 */
enum class MouseButton : std::uint32_t {
    None = 0,
    Left,
    Right,
    Middle,
    XButton1,  ///< Forward / thumb button 1.
    XButton2,  ///< Back / thumb button 2.
};

/**
 * @brief Bit flags describing the modifier keys held when an event occurred.
 *
 * Use the VN_ENABLE_ENUM_FLAGS bitwise operators, e.g. `Shift | Control`, and
 * test with vn::testFlag().
 */
enum class ModifierKey : std::uint32_t {
    None    = 0,
    Shift   = 1 << 0,
    Control = 1 << 1,
    Alt     = 1 << 2,
    Super   = 1 << 3,  ///< Windows key / Command key.
};

VN_ENABLE_ENUM_FLAGS(ModifierKey)

VN_WINDOW_NS_END
