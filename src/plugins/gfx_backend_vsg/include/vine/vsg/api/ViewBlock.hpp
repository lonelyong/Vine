#pragma once

#include <cstdint>

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The per-view ABI block a pass' draws read: the ONE place the conventions of `VineViewBlock` are
 * applied.
 *
 * WHY A BUILDER AND NOT "fill it in where you record". The block is bound for every pass (the block set's
 * first binding), and four of its contents are conventions rather than values a caller happens to have:
 *
 *   * the MATRICES are column-major, written from the math module's accessors as `column * 4 + row` - the
 *     same spelling DrawBlock uses for the model matrix, so the two cannot drift into different orders;
 *   * the two CLIP-space matrices (`proj`, `view_proj`) carry the DEVICE's clip convention, folded here from
 *     the SDK's. The SDK's matrices are x right, y up, z in [-1, 1] with -1 at the near plane; this backend's
 *     device is reverse-Z and y-down (near maps to depth 1, far to 0, world up renders up) - the same
 *     convention vsg's own cameras live in, and the one its clear value, its compare op and the pipelines'
 *     front-face declaration already assume (the design's "four sides of one fact"). A host program writes
 *     `gl_Position = view_proj * model * pos` and must not have to know any of that, so the fold - x and w
 *     unchanged, y negated, z mapped as `0.5 - 0.5 * z` - belongs HERE and nowhere else. `view`/`inv_view`
 *     are view-space and stay in the module's convention (a lighting term in view space is orthogonal to
 *     the clip convention);
 *   * `view_proj` is the composition `proj * view` (world -> clip), and `inv_view` the inverse of the view:
 *     a shading stage that reconstructs world positions from a G-buffer reads the second one, and a wrong
 *     composition there is a scene that looks subtly displaced rather than an error;
 *   * `frame` carries the frame's TIME (x), the picture's EXTENT (y, z) and a reserved flags slot (w), and
 *     `cam_pos.w` is reserved: the reserved slots are packed as zero, because "there is a place for it" is
 *     how per-view values silently stop reaching shaders.
 *
 * WHY THE FOLD IS NOT OPTIONAL, MEASURED: an unfolded matrix put the test's triangle at clip z = -0.714 on a
 * window whose depth attachment is cleared to 0.0 under `VK_COMPARE_OP_GREATER` (reverse-Z). Every fragment
 * was then rejected - the window showed its clear colour and the draws were still recorded, the plan was
 * still correct, zero validation messages were produced and no diagnostic was reported: a picture without its
 * content. It is the same trap the backend's `SceneBridgePipeline` note records for hand-written
 * `gl_Position`, one layer up (the matrices, not the program).
 *
 * WHAT "THE PICTURE'S EXTENT" MEANS, and why not the pass' rectangle: `frame.y/z` describes the ATTACHMENT
 * the shading reconstructs screen-space data from (the G-buffer's size, the window's size). A pass that
 * announced a sub-rectangle draws inside a larger picture, and a screen-space effect that used the rectangle
 * would reconstruct against a size no attachment has. The sub-rectangle is the viewport command's business.
 *
 * THE TIME comes from the session (it owns the frame clock): `Session::frameSeconds()`, the same value for
 * every pass of a frame - a frame is one moment, and two passes that disagreed about "now" would animate
 * against each other.
 *
 * A SNAPSHOT WITH NO CAMERA has nothing to view: the matrices are written as zero and the frame facts are
 * still carried. A pass with no camera has no content to shade (the engine passes a camera for every pass
 * that draws), so this is the honest "there is no view" rather than a made-up identity - and the caller that
 * builds the block decides what to do about it.
 */
V_VSG_NS_BEGIN

/** @brief Builds the view block for one pass (see the file note for the conventions it applies).
 *
 * @param camera       The pass' camera snapshot, as the plan carries it (its matrices and eye).
 * @param time_seconds The frame's time, in seconds since the session opened (see Session::frameSeconds).
 * @param width        Width of the target the pass draws into, in device pixels.
 * @param height       Height of the target the pass draws into, in device pixels.
 * @return The block, in the ABI's shape (`VineViewBlock`), ready for BlockStorage::writeView.
 */
[[nodiscard]] vine::graphics::VineViewBlock buildViewBlock(const core::CameraSnapshot& camera, float time_seconds,
                                                           std::uint32_t width, std::uint32_t height) noexcept;

V_VSG_NS_END
