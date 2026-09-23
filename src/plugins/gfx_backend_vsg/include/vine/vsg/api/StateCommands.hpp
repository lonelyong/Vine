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
 * BLENDING, AND THE ONE PLACE IT IS NOT ENABLED. For a SINGLE colour attachment blending is always enabled:
 * opacity rides the per-vertex colour alpha and may drop below 1 at any time, so the engine's
 * `blend.enabled` selects the FACTORS rather than turning blending off (see RenderStateMapper's note), and a
 * state that does not opt in gets the standard SrcAlpha / OneMinusSrcAlpha pair. A pass with SEVERAL
 * attachments is written UNBLENDED, whatever the state says: those attachments carry DATA - the engine's
 * G-buffer writes the material's shininess into its normal attachment's alpha (clamp(shininess / 256, 0, 1),
 * so a shininess of 32 attenuates it to 12.5% and a shininess of 0 erases it) - and the L2 measured exactly
 * that before writing its own G-buffer unblended (applyOpaqueBlendForAttachments, VsgSceneRules.hpp). The
 * rule is re-applied at THIS end because the enable travels with the command, not with the pipeline.
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

/** @brief Maps the engine's primitive topology onto the API's enum.
 *
 * The ONE spelling of the mapping: the pipeline bakes it into its create-info (the value a dynamic
 * `vkCmdSetPrimitiveTopology` may only move within the CLASS of - see core::PipelineKey) and the dynamic
 * command delivers it per draw, so the two cannot disagree about what "Triangles" means.
 *
 * @param topology Engine topology.
 * @return The corresponding `VkPrimitiveTopology` (triangle list for an unrecognised value).
 */
[[nodiscard]] VkPrimitiveTopology mapTopology(vine::graphics::Topology topology) noexcept;

/** @brief Maps the engine's dynamic state onto the set command that delivers it.
 *
 * @param state            The state the next draws use.
 * @param color_attachments Colour attachments of the pass (0 = a depth-only pass: no blend entries).
 * @param entry_points     The three extension entry points (see DynamicStateEntryPoints); a device-free
 *                         caller leaves them empty and record() skips those calls.
 * @param draws_content    Whether this is the CONTENT path (see below); a full-screen draw passes false.
 * @return The command (never null).
 *
 * WHICH KIND OF DRAW DECIDES WHETHER THE PICTURE BLENDS, and the two kinds want opposite things:
 *
 *   * a CONTENT draw is a surface, and the engine's per-drawable opacity can drop below 1 without a
 *     pipeline rebuild (`vine_draw.params.x`), so a single-colour-attachment picture keeps the standard
 *     pair - the pipeline cannot know, the command has to say;
 *   * a FULL-SCREEN draw is a WRITE: a copy or a post-process overwrites the rectangle it covers, and the
 *     engine's screen ABI carries no opacity at all. The previous implementation's full-screen pipelines
 *     stated exactly that (blending off), and blending one is not a subtle difference: a copy of an
 *     attachment whose alpha is 0 - the G-buffer's extra attachments clear to transparent black - blends
 *     the destination away and the pass shows NOTHING (measured: three of the demo's four G-buffer previews
 *     were invisible while the clear colour of the pass stayed on screen).
 */
[[nodiscard]] ::vsg::ref_ptr<detail::SetDynamicState> makeDynamicStateCommand(
    const core::DynamicState& state, std::uint32_t color_attachments,
    const detail::DynamicStateEntryPoints& entry_points, bool draws_content = true);

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
