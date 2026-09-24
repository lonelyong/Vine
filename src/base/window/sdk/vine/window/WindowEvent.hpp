#pragma once

#include "window_global.hpp"

VN_WINDOW_NS_BEGIN

/**
 * @brief Window lifecycle / state event.
 *
 * Published when the window is resized, moved, closed, gains or loses focus,
 * or changes its minimize/restore state.
 */
struct VN_WINDOW_API WindowEvent {
    enum class Type {
        Resize,     ///< Width or height changed; see width/height.
        Move,       ///< Window moved on screen.
        Close,      ///< User requested to close the window.
        FocusIn,    ///< Window gained keyboard focus.
        FocusOut,   ///< Window lost keyboard focus.
        Minimize,   ///< Window was minimized.
        Restore,    ///< Window was restored to normal size.
    };

    Type type = Type::Resize;
    int  width = 0;   ///< Current client width (pixels); valid for Resize.
    int  height = 0;  ///< Current client height (pixels); valid for Resize.
    int  x = 0;       ///< Window position on screen; valid for Move.
    int  y = 0;       ///< Window position on screen; valid for Move.
};

VN_WINDOW_NS_END
