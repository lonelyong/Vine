#pragma once

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief Packing the per-draw ABI block from a compiled command - the bytes the shading actually reads.
 *
 * WHY PACKING IS A RULE AND NOT A memcpy. The block is the contract between the engine and the shaders
 * (`VineDrawBlock`, pinned by ShaderAbi's static_asserts), and three of its facts are the kind that fail
 * silently:
 *
 *   * the MATRICES ARE COLUMN-MAJOR, matching the math module. A transposed packing of a symmetric matrix -
 *     an identity, a scale, most test content - is indistinguishable from a correct one, and a rotation or a
 *     translation would be wrong in a way that looks like a modelling mistake;
 *   * TRANSLATION LIVES AT indices 12..14 of the flat array (the fourth column), which is the same fact seen
 *     from the bytes rather than from the math;
 *   * OPACITY IS `params.x` and nothing else. `params.yzw` is the reserved user slot and packs as zero: a
 *     value that "looks like it should go somewhere" is how a per-draw parameter quietly stops working.
 *
 * So the packing lives in one function, and the case that pins it uses a NON-SYMMETRIC matrix - the only kind
 * that can tell a correct packing from a transposed one.
 *
 * WHAT IS NOT HERE YET: the view block (`VineViewBlock`). Its ABI is pinned (`view` / `inv_view` / `proj` /
 * `view_proj` / `cam_pos` / `frame`), but two of its fields need conventions nothing has established yet -
 * `frame` carries the time and the viewport size, and `cam_pos.w` is reserved. Those arrive with the session
 * side (a frame number and an extent both belong there), and inventing them here would be a second answer to
 * a question the session already owns.
 */
V_VSG_NS_BEGIN

/**
 * @brief Packs one compiled command into the per-draw block.
 *
 * @param command The resolved command (its model matrix and opacity are the frame's facts).
 * @param out     Receives the block; every field is written (the reserved slot included).
 */
void packDrawBlock(const core::CompiledCommand& command, vine::graphics::VineDrawBlock& out) noexcept;

V_VSG_NS_END
