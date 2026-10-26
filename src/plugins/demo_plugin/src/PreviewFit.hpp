#pragma once

#include <algorithm>
#include <cmath>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Viewport.hpp>

/**
 * @brief The rectangle a preview draws its source into: the slot's box, letterboxed to the source's aspect.
 *
 * WHY THIS EXISTS. A fullscreen program maps `vn_uv` over the DESTINATION rectangle (see
 * BuiltinShaders::fullscreenVertexProgram), so copying a whole attachment into a rectangle of another
 * aspect squeezes it. The G-buffer follows the window while a preview slot is a fixed box, so widening the
 * window made the four previews look squashed and narrowing it stretched them - the copy is isotropic only
 * while the two aspects agree. Fitting the SOURCE's aspect inside the box keeps the preview undistorted and
 * keeps the slot's size and place, so the four previews stay a row whatever the window does.
 *
 * It lives here, not in DemoScene.cpp, so the arithmetic has a unit test of its own (it is the only part
 * of the preview strip whose failure is a PICTURE nobody can grep for).
 */
namespace vn::demo
{

/**
 * @brief Which channels the preview of G-buffer attachment @p attachment shows.
 *
 * WHY IT IS NOT ALWAYS THE WHOLE TEXEL. Attachment 2 of the canonical G-buffer is emission (rgb) + occlusion
 * (alpha) - graphics-material.md 2.8 - and emission is HDR with no emitters in this demo, so a copy of the
 * whole texel is a rectangle that is BLACK whether the pass wrote it or not. Its alpha is the occlusion
 * factor: 1 on every written, unoccluded pixel and 0 where the clear survived, which is the same "the pass
 * wrote this" evidence the other tiles give by being coloured.
 *
 * WHY IT LIVES HERE, NEXT TO THE FIT. It is the same question as the letterbox arithmetic - what a preview
 * tile SHOWS - and it was answered in two places (the deferred strip and the VN_VSG_GBUFFER strip) once
 * only, so one of them kept drawing the black tile. One function, both callers, one unit test.
 *
 * @param attachment Colour attachment index of the target being previewed.
 * @return The channels the tile shows.
 */
[[nodiscard]] inline vn::graphics::ScreenCopyChannels previewChannelsOf(int attachment) noexcept
{
    return attachment == 2 ? vn::graphics::ScreenCopyChannels::Alpha : vn::graphics::ScreenCopyChannels::All;
}

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

}  // namespace vn::demo
