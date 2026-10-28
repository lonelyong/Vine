#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How a pipeline shades its opaque content.
 *
 * @details
 * - The ONE structural choice a chain makes: a forward path draws the content straight into the lit
 *   image (one scene pass), a deferred path first writes a G-buffer off-screen and then shades it
 *   fullscreen.
 * - It is an OPTION rather than a type because the two carry the same behaviour — a list of passes —
 *   and differ only in that data; and because the other things a host wants (shadows, screen-space
 *   effects) are ORTHOGONAL to it: they add passes and inputs to whichever path is chosen (see
 *   PassStage), so making the path a type would need one subclass per combination.
 */
enum class ShadingPath
{
    Forward,  // One window scene pass drawing the content.
    Deferred, // A G-buffer pass (offscreen MRT) + a fullscreen lighting pass.
};

VN_GRAPHICS_NS_END
