#pragma once
#include "graphics_global.hpp"

#include <cmath>
#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief ONE frame's context, as a value: which frame it is, what the previous one cost, and what surface it lands on.
 *
 * @details
 * - THE ONE VALUE THE PER-FRAME WORK SHARES (see FrameUpdate): every object doing its own per-frame work gets the
 *   same `FrameContext` for the same frame, so a gizmo and a readout cannot disagree about which frame they are in or
 *   how big the surface is.
 * - IT IS A VALUE, NOT A HANDLE: a copy is what a `FrameUpdate::update()` call receives, and it says nothing about
 *   the engine - an object that needs more asks the object that owns it.
 * - THE SIZE IS THE HOST'S ANNOUNCEMENT (`RenderArea::resize`), in LOGICAL pixels, with the ratio beside it, so
 *   `renderWidth()` / `renderHeight()` answer device pixels the same way the engine lays a frame out
 *   (`std::lround(logical * ratio)` - one rounding rule, not two).
 * - THE TIME IS THE ENGINE'S MEASUREMENT of the interval between the two most recent frames, not a per-object clock
 *   read: one clock in the system, so two overlays cannot window the same frame differently. It is WALL-CLOCK and it
 *   describes the frame BEFORE the one being prepared - a host that needs a fixed step or a replay keeps its own
 *   clock and reads the other fields (see `graphics-frame-pipeline.md` 2.13).
 *
 * @note
 * - A first frame has no interval to report: `elapsed_seconds` stays 0 until two frames have run, which is also what
 *   an object taken from "nothing was measured" answers.
 * - 0 for a size means "no host announced one yet", the same convention as `RenderEngine::areaWidth()`; it is not a
 *   usable surface, and `hasArea()` says so in one call.
 */
struct VN_GRAPHICS_API FrameContext {
    /// The frame this context is for: the engine's next frame id (see RenderEngine::frameNumber).
    std::uint64_t frame_number{ 0 };
    /// Wall-clock seconds between the two most recent frames; 0 until two have run.
    double elapsed_seconds{ 0.0 };
    /// Surface width in LOGICAL pixels as the host announced it; 0 until it announced one.
    int area_width{ 0 };
    /// Surface height in LOGICAL pixels as the host announced it; 0 until it announced one.
    int area_height{ 0 };
    /// Device pixels per logical pixel (1.0 until a host announced one).
    double pixel_ratio{ 1.0 };

    /**
     * @brief Gets the surface width in DEVICE pixels (the value a viewport or a target uses).
     *
     * @return Width in device pixels; 0 when no area was announced.
     */
    [[nodiscard]] int renderWidth() const noexcept
    {
        return static_cast<int>(std::lround(static_cast<double>(area_width) * pixel_ratio));
    }

    /**
     * @brief Gets the surface height in DEVICE pixels (see renderWidth).
     *
     * @return Height in device pixels; 0 when no area was announced.
     */
    [[nodiscard]] int renderHeight() const noexcept
    {
        return static_cast<int>(std::lround(static_cast<double>(area_height) * pixel_ratio));
    }

    /**
     * @brief Gets whether a host announced a usable surface size.
     *
     * @return true when both dimensions are positive.
     */
    [[nodiscard]] bool hasArea() const noexcept
    {
        return area_width > 0 && area_height > 0;
    }
};

VN_GRAPHICS_NS_END
