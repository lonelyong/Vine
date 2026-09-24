#pragma once

#include <algorithm>
#include <cmath>

#include <vine/graphics/Viewport.hpp>

/**
 * @brief The rectangle a preview draws its source into: the slot's box, letterboxed to the source's aspect.
 *
 * WHY THIS EXISTS. A fullscreen program maps `vine_uv` over the DESTINATION rectangle (see
 * BuiltinShaders::fullscreenVertexProgram), so copying a whole attachment into a rectangle of another
 * aspect squeezes it. The G-buffer follows the window while a preview slot is a fixed box, so widening the
 * window made the four previews look squashed and narrowing it stretched them - the copy is isotropic only
 * while the two aspects agree. Fitting the SOURCE's aspect inside the box keeps the preview undistorted and
 * keeps the slot's size and place, so the four previews stay a row whatever the window does.
 *
 * It lives here, not in AppShellDemo.cpp, so the arithmetic has a unit test of its own (it is the only part
 * of the preview strip whose failure is a PICTURE nobody can grep for).
 */
namespace vn::app_shell
{

/**
 * @brief Fits an @p source_width x @p source_height picture inside a slot, centred.
 *
 * @param source_width  Source attachment width in pixels (the window's aspect is these two).
 * @param source_height Source attachment height in pixels.
 * @param slot_x        Left edge of the slot.
 * @param slot_y        Top edge of the slot.
 * @param slot_width    Width of the slot (the box the preview fits inside).
 * @param slot_height   Height of the slot.
 * @return The rectangle to draw into, centred in the slot, at least one pixel a side (the slot itself when
 *         either extent is not positive: there is no aspect to fit).
 */
inline vn::graphics::Viewport fitPreviewRect(int source_width, int source_height, int slot_x, int slot_y,
                                               int slot_width, int slot_height)
{
    vn::graphics::Viewport rect{ slot_x, slot_y, slot_width, slot_height };
    if (source_width <= 0 || source_height <= 0 || slot_width <= 0 || slot_height <= 0) {
        return rect;
    }
    const double scale = std::min(static_cast<double>(slot_width) / static_cast<double>(source_width),
                                  static_cast<double>(slot_height) / static_cast<double>(source_height));
    const int    width  = std::max(1, static_cast<int>(std::lround(static_cast<double>(source_width) * scale)));
    const int    height = std::max(1, static_cast<int>(std::lround(static_cast<double>(source_height) * scale)));
    rect.x              = slot_x + (slot_width - width) / 2;
    rect.y              = slot_y + (slot_height - height) / 2;
    rect.width          = width;
    rect.height         = height;
    return rect;
}

}  // namespace vn::app_shell
