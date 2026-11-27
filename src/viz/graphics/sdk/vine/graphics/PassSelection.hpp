#pragma once
#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Which of a packet's commands a pass records (see `PassDesc::selection`).
 *
 * @details
 * - A PASS SAYS WHAT IT DRAWS FROM, not only what it draws WITH. A colour pass wants everything its view collects;
 *   a pass that writes DEPTH for somebody else to read wants the surfaces that actually OCCLUDE, and a surface
 *   whose coverage is a fraction of its fragment does not: a blended leaf drawn into a shadow map would leave the
 *   depth of its whole quad behind it and shadow everything under the parts that are almost transparent.
 * - ONE QUESTION, TWO READERS (graphics-material.md §2.7 / graphics-rendering.md §2.7): the criterion is the very
 *   one the sort already answers (`DrawItem::isTransparent`), so the queue a drawable lands in, the blend the
 *   backend enables for it and whether a depth map records it are three readings of one value. A second predicate
 *   would let "blended" and "casts" disagree, and nothing would report it.
 * - `Mask` IS A CASTER: its alpha cuts fragments out instead of fading them, so the drawable is opaque to the sort
 *   AND its cut-out reaches a depth pass through the same threshold the colour pass applies (`vine_alpha.glsl`).
 *   Selecting the drawable and cutting its silhouette are two different steps, which is why this value is not
 *   enough on its own.
 * - THE BACKEND APPLIES IT, the way it applies the pass' program override and its state: the pass states the
 *   selection ONCE and the command list stays the shared, immutable list the frame collected - filtering the list
 *   instead would cost a command-sized copy per pass per frame (G10).
 */
enum class PassSelection : std::uint8_t
{
    /// Every command the packet carries: what a pass that writes COLOUR draws.
    All = 0,
    /// Only the commands the sort leaves in the opaque queue (full coverage): what a DEPTH pass can occlude with.
    Opaque = 1,
};

VN_GRAPHICS_NS_END
