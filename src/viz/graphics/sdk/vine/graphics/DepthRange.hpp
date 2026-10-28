#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief The depth-buffer interval a pass' draws are mapped into.
 *
 * @details
 * - The values are DEPTH-BUFFER values, not clip-space z: `max` is the end of the interval the viewer is on
 *   and `min` the far end, which is what a readback or a sampled depth texture shows (the backend renders
 *   reverse-Z, so 1 is near and 0 is the value a depth buffer is cleared to).
 * - The default {0, 1} is the whole buffer, i.e. no remapping at all, so a pass that states nothing behaves
 *   exactly as it did before this type existed.
 * - A narrower interval forces the pass' content into that BAND, which is how a view puts one semantic layer
 *   in front of another regardless of the geometry's real distance - the "depth banding" a layered renderer
 *   does, expressed as one number pair instead of a second mechanism.
 * - The mapping is AFFINE AND ORDER-PRESERVING: relationships inside the band, and between the band and
 *   whatever the default mapping wrote, stay as they were. What it does NOT preserve is DEPTH-AS-DISTANCE: a
 *   remapped value is no longer a distance, so a target whose writers use different intervals can never be
 *   read back as one picture's geometry.
 *
 * @note
 * - Stated per PASS, not per node: a StateNode cannot carry it, because two subtrees mapping one framebuffer
 *   differently leave a pixel's depth value meaningless - nothing can unmap it again without a per-pixel tag,
 *   and "which object is that I just clicked" stops having one answer. Every draw of one pass shares the band.
 * - The pass is the right owner rather than the camera: one camera can be drawn by a main pass, a shadow pass
 *   and an overlay pass, and the overlay is exactly the one that wants its own band.
 */
struct VN_GRAPHICS_API DepthRange {
    float min = 0.0F; // Depth-buffer value at the FAR end of the band (the clear value when default).
    float max = 1.0F; // Depth-buffer value at the NEAR end of the band.

    /**
     * @brief Returns whether this is the whole-buffer interval, i.e. no remapping.
     *
     * @return True when the band is {0, 1}.
     */
    [[nodiscard]] bool isDefault() const noexcept
    {
        return min == 0.0F && max == 1.0F;
    }

    /**
     * @brief Returns whether the band is inside the buffer and non-inverted.
     *
     * @return True when 0 <= @ref min <= @ref max <= 1.
     */
    [[nodiscard]] bool isValid() const noexcept
    {
        return min >= 0.0F && max <= 1.0F && min <= max;
    }
};

/** @brief Returns whether two depth ranges are equal. */
inline bool operator==(const DepthRange& lhs, const DepthRange& rhs)
{
    return lhs.min == rhs.min && lhs.max == rhs.max;
}

/** @brief Returns whether two depth ranges differ. */
inline bool operator!=(const DepthRange& lhs, const DepthRange& rhs)
{
    return !(lhs == rhs);
}

VN_GRAPHICS_NS_END
