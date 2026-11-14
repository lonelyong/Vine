#pragma once
#include "graphics_global.hpp"

#include <vine/math/Matrix4x4.hpp>

#include "ShaderAbi.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Packs the per-drawable block a content program reads.
 *
 * @details
 * - THE ONE PACKING RULE for `VineDrawBlock`, and a rule rather than a `memcpy` because three of its facts fail
 *   silently:
 *   - the matrix is COLUMN-MAJOR (the math module's convention): element (row, column) lands at `column * 4 + row`.
 *     A transposed packing is invisible with an identity, a scale or any other symmetric matrix - most test content -
 *     and shows up later as "the model rotates the wrong way", which reads as a modelling bug;
 *   - TRANSLATION therefore lives at indices 12..14, the same fact seen from the bytes;
 *   - `params.x` is the effective opacity and `params.y` the point size in framebuffer pixels; `z`/`w` are the user
 *     slot and pack as zero, never as a leftover.
 * - Every field is written: a block left as whatever the previous owner of these bytes wrote is a shader reading
 *   another drawable's values.
 * - This header, not `ShaderAbi.hpp`: the block's SHAPE is the ABI (there), but filling it needs the math module, and
 *   the shape header stays dependency-free (see that header's note; the same split as `LightAbi.hpp`).
 *
 * @param model      Object -> world matrix, column-major expected (see above).
 * @param opacity    The drawable's EFFECTIVE opacity (`DrawDescription::opacity` after resolution).
 * @param point_size Point size in framebuffer pixels (`ResolvedRenderState::pointSize`; 1.0 means nobody authored one).
 * @param out        Receives the block; every field is written.
 */
VN_GRAPHICS_API void packDrawBlock(const vn::math::Mat4d& model, float opacity, float point_size, VineDrawBlock& out) noexcept;

VN_GRAPHICS_NS_END
