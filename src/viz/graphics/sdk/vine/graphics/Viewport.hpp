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

    /**
     * @brief A rectangular draw region in NORMALIZED surface coordinates (top-left origin).
     *
     * The same idea as Viewport, expressed as fractions of the render area, which is what a view sharing a
     * surface with other views knows: a side-by-side layout is {0,0,0.5,1} and {0.5,0,0.5,1} whatever the
     * surface size is, whatever the display's device pixel ratio is, and it survives every resize. The device
     * pixels a render target or a pass viewport needs are DERIVED from it when they are needed
     * (see SceneView::setViewport).
     *
     * The values are not clamped: a viewport is a statement about where the view draws, and a statement that
     * reaches outside the surface is the caller's to fix (the device rectangle is what gets clamped).
     */
    struct VN_GRAPHICS_API NormalizedViewport {
        /** @brief Origin x as a fraction of the render area width. */
        double x = 0.0;
        /** @brief Origin y as a fraction of the render area height. */
        double y = 0.0;
        /** @brief Width as a fraction of the render area width. */
        double width = 1.0;
        /** @brief Height as a fraction of the render area height. */
        double height = 1.0;

        /** @brief Returns whether this viewport describes a positive area. */
        bool isValid() const
        {
            return width > 0.0 && height > 0.0;
        }

        /** @brief Returns whether this viewport is the whole render area (the default). */
        bool isFull() const
        {
            return x == 0.0 && y == 0.0 && width == 1.0 && height == 1.0;
        }

        /** @brief Returns whether a normalized point lies inside this viewport.
         *
         * The hit test a host needs to route an input event to the view that owns the area under the cursor
         * (the upper/left edge is inside, the lower/right edge is not, so neighbouring areas never both claim
         * a point).
         *
         * @param nx Point x as a fraction of the render area width.
         * @param ny Point y as a fraction of the render area height.
         * @return true when the point is inside.
         */
        bool contains(double nx, double ny) const
        {
            return nx >= x && nx < x + width && ny >= y && ny < y + height;
        }
    };

VN_GRAPHICS_NS_END
