#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/String.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Rect3.hpp>

#include "RenderTarget.hpp"
#include "Viewport.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class RenderEngine;
class Scene;
class ScreenPass;
class ShaderProgram;

/** @brief Builds the default G-buffer geometry program (scene -> MRT).
 *
 * @details
 * - The canonical four outputs the Deferred preset's G-buffer declares: albedo (0), view-space normal + shininess
 *   (1), specular (2) and view-space position (3). It matches the backend's per-material block and view-space light
 *   ABI.
 * - The program - and the GLSL it is built from - is owned by the SDK (see BuiltinShaders.hpp); callers may still
 *   override it through ChainDesc::gbuffer_program.
 *
 * @return The geometry program.
 */
[[nodiscard]] VN_GRAPHICS_API intrusive_ptr<ShaderProgram> defaultGbufferGeometryProgram();

/** @brief Builds the default deferred-lighting fragment program (fullscreen).
 *
 * @details
 * - Samples the G-buffer's albedo / normal / specular / view-position attachments (binding 0..3) and shades ambient
 *   plus up to three directional lights whose parameters arrive in the backend's view-space push block.
 * - The program - and the GLSL it is built from - is owned by the SDK (see BuiltinShaders.hpp); callers may still
 *   override it through ChainDesc::lighting_program.
 *
 * @return The lighting program (fragment stage only).
 */
[[nodiscard]] VN_GRAPHICS_API intrusive_ptr<ShaderProgram> defaultDeferredLightProgram();

/** @brief Builds the canonical G-buffer MRT target of the Deferred preset.
 *
 * @details
 * - Four colour attachments - albedo (RGBA16F), view normal + shininess (RGBA16F), specular (RGBA8), view position
 *   (RGBA16F) - plus depth (D24).
 *
 * @param width  Target width in pixels (<= 0 uses 640).
 * @param height Target height in pixels (<= 0 uses 360).
 * @return A fresh target with the canonical colour + depth attachments.
 */
[[nodiscard]] VN_GRAPHICS_API intrusive_ptr<RenderTarget> defaultGbufferTarget(int width, int height);

/** @brief The light camera a directional shadow is rendered with.
 *
 * @details
 * - Orthographic, looking along @p light's direction, framing @p bounds: the eye is pulled back past the box along
 *   the light, the window covers the box's diagonal (a sphere of that radius fits every orientation, so the fit does
 *   not have to know which way the light comes from), and the depth range reaches from in front of the box to well
 *   behind it.
 * - It has to be the ONLY one: the pass that renders the map draws through this camera, the target STATES its
 *   view-projection (RenderTarget::setProducerViewProjection), and the shading maps its fragments through that
 *   matrix. A second derivation anywhere is a second light camera that agrees only until one of them is touched.
 *
 * @param light  Directional light the map is rendered along (the light shines ALONG its direction, so the eye is
 *               pulled back against it).
 * @param bounds Content bounds the map must cover (a box with an inverted axis maps a unit sphere at the origin
 *               instead).
 * @param camera Receives the light camera (also usable as the shadow pass' camera).
 * @return The light camera's projection * view matrix.
 */
[[nodiscard]] VN_GRAPHICS_API vn::math::Mat4d directionalShadowMatrix(const Light& light,
                                                                     const vn::math::Aabbd& bounds,
                                                                     Camera& camera);

/** @brief The light camera a SPOT shadow is rendered with.
 *
 * @details
 * - Perspective, from the light's POSITION along its direction, with the vertical field of view taken from the spot's
 *   OUTER cone angle (a small margin keeps the map edge off the cone edge, where the light is already zero). The map
 *   is square and the shadow pass draws the whole target, so the aspect is 1 and the horizontal cone matches.
 * - Near and far come from the content bounds PROJECTED onto the light axis (not from a fitted sphere): a spot has a
 *   position, so what matters is how far the content is from it.
 * - The same contract as @ref directionalShadowMatrix: the pass renders through this camera and the target STATES its
 *   view-projection, so the shading maps its fragments through the one matrix - never a second fit.
 *
 * @param light  Spot light the map is rendered from (its position is the eye, its direction the axis).
 * @param bounds Content bounds the map must cover.
 * @param camera Receives the light camera (also usable as the shadow pass' camera).
 * @return The light camera's projection * view matrix.
 */
[[nodiscard]] VN_GRAPHICS_API vn::math::Mat4d spotShadowMatrix(const Light& light, const vn::math::Aabbd& bounds,
                                                              Camera& camera);

/** @brief Recipe: render content into an off-screen target and composite it back as a picture-in-picture screen pass.
 *
 * @details
 * - Creates an order < 0 scene pass rendering into a @p rt_width x @p rt_height RenderTarget (publishing its colour as
 *   @p output_slot), and an order > 0 ScreenPass that samples @p output_slot into the @p pip sub-viewport.
 * - OWNERSHIP: both passes are registered on the engine IMMEDIATELY and the engine keeps them alive - they are NOT
 *   owned by any PassChain, so only RenderEngine::clearPasses() removes them (removePass(screen) alone would leave the
 *   off-screen pass drawing into a target nobody samples).
 * - The caller re-anchors the PiP viewport once the surface size is known (see RenderPass::setViewport).
 *
 * @param engine        Engine the passes are registered on (null answers null).
 * @param camera        View camera the preview scene pass draws through (null answers null).
 * @param content       Scene the preview pass draws (may be null: the pass then draws nothing).
 * @param output_slot   Name the off-screen target is published under and the screen pass resolves.
 * @param rt_width      Off-screen target width.
 * @param rt_height     Off-screen target height.
 * @param color_format  Off-screen colour format.
 * @param depth_format  Off-screen depth format.
 * @param pip           PiP sub-viewport on the output surface (device px).
 * @return The created ScreenPass (owned by the engine; do not delete), or null when the recipe was refused.
 */
[[nodiscard]] VN_GRAPHICS_API raw_ptr<ScreenPass> addOffscreenToScreen(raw_ptr<RenderEngine> engine,
                                                                      raw_ptr<Camera> camera,
                                                                      intrusive_ptr<Scene> content,
                                                                      const String& output_slot,
                                                                      int rt_width,
                                                                      int rt_height,
                                                                      RenderTarget::ColorFormat color_format,
                                                                      RenderTarget::DepthFormat depth_format,
                                                                      const Viewport& pip);

VN_GRAPHICS_NS_END
