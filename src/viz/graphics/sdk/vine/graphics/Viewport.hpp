#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief A rectangular draw region in device pixels (top-left origin).
 *
 * A pass without a viewport draws the full surface / target it renders into;
 * a pass that carries a Viewport restricts its draw to that rectangle (used
 * by picture-in-picture, minimaps, previews and corner HUD boxes). The
 * viewport is maintained explicitly by the pass's owner; it is never derived
 * automatically.
 */
struct VN_GRAPHICS_API Viewport {
    /** @brief Origin x in device pixels. */
    int x = 0;
    /** @brief Origin y in device pixels (top-left origin). */
    int y = 0;
    /** @brief Width in device pixels. */
    int width = 0;
    /** @brief Height in device pixels. */
    int height = 0;

    /** @brief Returns whether this viewport describes a positive draw area. */
    bool isValid() const
    {
        return width > 0 && height > 0;
    }
};

VN_GRAPHICS_NS_END
