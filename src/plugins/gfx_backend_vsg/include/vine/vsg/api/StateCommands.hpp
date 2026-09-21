#pragma once

#include <cstdint>

#include <vsg/commands/SetScissor.h>
#include <vsg/commands/SetViewport.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/DepthMode.hpp>
#include <vine/vsg/VsgDynamicState.hpp>
#include <vine/vsg/VsgVulkanEntryPoints.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The API's spelling of the dynamic state: the engine's vocabulary mapped onto set commands.
 *
 * WHY THE MAPPING IS A FUNCTION. The engine's state (`core::DynamicState`) is backend-neutral - a depth
 * POLICY, a cull side, a polygon mode, a topology, a blend choice - while the API wants concrete enums and
 * two conventions that are easy to get silently wrong:
 *
 *   * the depth comparison is INVERTED for reverse-Z (closer is greater), so a "TestAndWrite" depth policy is
 *     `VK_COMPARE_OP_GREATER`;
 *   * a front face is CLOCKWISE, because vsg's projection inverts Y: a triangle the engine calls
 *     counter-clockwise in world space arrives at the rasteriser clockwise. Declaring the intuitive
 *     counter-clockwise made every cull mode act on the wrong faces - silently, with no validation error.
 *
 * Both conventions live HERE, once, so a command and a pipeline's create-info cannot drift apart.
 *
 * BLENDING IS ALWAYS ENABLED. Opacity rides the per-vertex colour alpha and may drop below 1 at any time, so
 * the engine's `blend.enabled` selects the FACTORS rather than turning blending off (see
 * RenderStateMapper's note); a state that does not opt in gets the standard SrcAlpha / OneMinusSrcAlpha pair.
 *
 * The commands are the existing backend's own (`detail::SetDynamicState` and `vsg::SetViewport` /
 * `vsg::SetScissor`): reusing a platform command that already learned its slots is not the same as reusing a
 * policy. The dynamic command carries the three extension entry points, because those three calls have no
 * linker symbol on Linux (see DynamicStateEntryPoints).
 */
V_VSG_NS_BEGIN

/** @brief The rectangle a draw covers: the viewport and the scissor are the same rectangle. */
struct ViewportRect
{
    float x{0.0F};          ///< Left edge in framebuffer coordinates.
    float y{0.0F};          ///< Top edge in framebuffer coordinates.
    float width{0.0F};      ///< Width in pixels.
    float height{0.0F};     ///< Height in pixels.
};

/** @brief Maps the engine's dynamic state onto the set command that delivers it.
 *
 * @param state            The state the next draws use.
 * @param color_attachments Colour attachments of the pass (0 = a depth-only pass: no blend entries).
 * @param entry_points     The three extension entry points (see DynamicStateEntryPoints); a device-free
 *                         caller leaves them empty and record() skips those calls.
 * @return The command (never null).
 */
[[nodiscard]] ::vsg::ref_ptr<detail::SetDynamicState> makeDynamicStateCommand(
    const core::DynamicState& state, std::uint32_t color_attachments,
    const detail::DynamicStateEntryPoints& entry_points);

/** @brief Builds the viewport command for @p rect.
 *
 * The depth range is [0, 1] and stays that way: reverse-Z lives in the projection, not in the viewport.
 *
 * @param rect The rectangle the draw covers.
 * @return The command (never null).
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::SetViewport> makeViewportCommand(const ViewportRect& rect);

/** @brief Builds the scissor command for @p rect.
 *
 * A scissor is an integer rectangle and must lie inside the framebuffer, so negative-origin and zero-area
 * rectangles are clamped rather than passed through (the API treats an out-of-bounds scissor as a validation
 * error).
 *
 * @param rect The rectangle the draw covers.
 * @return The command (never null).
 */
[[nodiscard]] ::vsg::ref_ptr<::vsg::SetScissor> makeScissorCommand(const ViewportRect& rect);

V_VSG_NS_END
