#pragma once
#include <vine/vsg/vsg_global.hpp>

// Internal header: the vsg object factories this backend composes a frame out
// of. Split out of VsgRenderer.cpp so the renderer's translation units (session
// lifecycle, pass protocol + slots, off-screen targets, overlay draws) share
// one factory layer instead of all living in a single 3.5k-line file. Not
// installed: the plugin's own sources are its only users.
//
// Everything here is a free function of its explicit arguments — nothing reads
// the renderer's state — so a factory can be reasoned about (and reused)
// without knowing which pass asked for the object.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/RenderPass.h>

#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderPreset.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/raw_ptr.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief Converts a Vine colour attachment format to the matching Vulkan
 * format.
 *
 * @param f Vine colour attachment format.
 * @return The matching VkFormat.
 */
VkFormat toColorFormat(vine::graphics::RenderTarget::ColorFormat f);

/**
 * @brief Converts a Vine depth attachment format to the matching Vulkan
 * format.
 *
 * @param f Vine depth attachment format.
 * @return The matching VkFormat.
 */
VkFormat toDepthFormat(vine::graphics::RenderTarget::DepthFormat f);

/**
 * @brief Why an overlay/program node could not be built.
 *
 * Returned to the caller instead of reported here: only the calling member
 * knows which pass asked, and it owns the sink and the diagnostic counters.
 */
enum class ProgramNodeFailure
{
    None,           ///< Built successfully.
    NoCompiler,     ///< The runtime GLSL compiler is unavailable (no shaderc).
    NoFragmentStage,///< The user program carries no fragment stage.
    CompileFailed,  ///< GLSL compilation failed.
};

/**
 * @brief CPU mirror of the deferred-lighting push-constant block.
 *
 * All members are vec4-sized (std140/std430 offsets coincide). 8 x 16 = 128
 * bytes == the declared push range: ambient + projection params + up to three
 * directional lights (dir/colour pairs). Layout matches the fragment shader
 * ABI the fullscreen program pass expects (see makeFullscreenProgramNode).
 */
struct alignas(16) LightPushBlock
{
    std::array<float, 4> ambient{};   // ambient rgb + intensity
    // Perspective projection params for view-position reconstruction from the
    // G-buffer depth: x = near, y = far, z = proj[0][0], w = proj[1][1].
    std::array<float, 4> projparms{};
    std::array<std::array<float, 4>, 3> dirs{};   // directional lights: view-space directions
    std::array<std::array<float, 4>, 3> cols{};   // directional lights: rgb + intensity
};

// The block is copied verbatim into the shader's push-constant range, so its
// size IS the shader ABI: a field added here without updating the range must
// fail the build, not silently under-push (or over-read) at run time.

static_assert(sizeof(LightPushBlock) == 128, "LightPushBlock must match the 128-byte push-constant range");
static_assert(alignof(LightPushBlock) == 16, "LightPushBlock must stay std140-aligned");

/**
 * @brief Builds the shader set for the given shading preset with complete
 * pipeline states.
 *
 * vsg's built-in shader sets (createPhongShaderSet / createFlatShadedShaderSet)
 * arrive without default pipeline states, so pipelines built by
 * GraphicsPipelineConfigurator would lack a ViewportState and nothing would
 * rasterize. Declare the canonical states here so every SceneBridge-built
 * geometry pipeline is complete. The baked viewport matches the window size at
 * attach; when the window drives a dynamic viewport it is overridden at record
 * time anyway. Both Phong and flat presets bind a "material" descriptor of
 * type vsg::PhongMaterialValue, so the shared Vine material path (SceneBridge
 * assigns that value) works unchanged for either.
 *
 * @param preset      Shading-model preset to build for.
 * @param extent      Window extent for the baked static viewport.
 * @param depth_test  When false, depth test/write are disabled so the geometry
 *                    always draws on top of previously rendered content (used
 *                    for HUD overlays such as the axis gizmo).
 * @param color_count Colour attachment count of the target this set renders
 *                    into. Vulkan requires the pipeline's color-blend
 *                    attachment count to equal the subpass's colour count, so
 *                    MRT targets (color_count > 1) get a matching default
 *                    ColorBlendState; single-colour targets keep one (the
 *                    default).
 * @return Configured shader set.
 */
::vsg::ref_ptr<::vsg::ShaderSet> buildShaderSet(vine::graphics::ShaderPreset preset, const VkExtent2D& extent, bool depth_test, bool depth_write, int color_count = 1);

/**
 * @brief Builds an off-screen render pass whose colour attachments end in
 * SHADER_READ_ONLY_OPTIMAL so a later pass can sample them as textures.
 *
 * vsg::createRenderPass() leaves the colour attachment in PRESENT_SRC_KHR
 * (correct for swapchain output, wrong for a texture sampled by a later
 * pass). The dependency on subpass-external fragment-shader reads makes the
 * colour writes visible to the sampling pass without an extra barrier. A
 * target with several colour attachments (MRT / G-buffer) gets one attachment
 * per entry, all sampleable on their own; fragment output @p i writes
 * attachment @p i.
 *
 * @param device       Device the render pass is created on.
 * @param color_formats Colour attachment formats, one per attachment (in
 *                      attachment order).
 * @param depth_format Depth attachment format, or VK_FORMAT_UNDEFINED for a
 *                     colour-only pass.
 * @param promote_depth When true the depth attachment ends in
 *                     SHADER_READ_ONLY_OPTIMAL (sampleable); false leaves it a
 *                     plain depth attachment another target can borrow.
 * @param color_clear  When true (the default) the colour attachments are
 *                     CLEARed at pass start; false LOADs the previous pass'
 *                     colour (a pass that composites over earlier content
 *                     without clearing it). A LOAD assumes the colour is
 *                     already defined (an earlier pass wrote it this frame).
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeSampleableRenderPass(
    ::vsg::Device*                    device,
    const std::vector<VkFormat>&      color_formats,
    VkFormat                          depth_format,
    bool                              promote_depth = true,
    bool                              color_clear = true);

/**
 * @brief Builds a depth-only off-screen render pass (shadow maps).
 *
 * The depth attachment is stored and left in SHADER_READ_ONLY_OPTIMAL so a
 * later pass can sample it as a shadow map. The subpass-external fragment-read
 * dependency makes the depth writes visible to the sampling pass.
 *
 * @param device      Device the render pass is created on.
 * @param depth_format Depth attachment format.
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeDepthOnlyRenderPass(::vsg::Device* device, VkFormat depth_format);

/**
 * @brief Builds a colour + depth render pass that PRESERVES depth (LOAD).
 *
 * Same attachment set / colour handling as makeSampleableRenderPass, but the
 * depth attachment keeps its previous contents across frames instead of being
 * cleared at pass start (loadOp = LOAD, staying in the depth-attachment
 * layout). Used when an engine clear(color, clearDepth=false) targets an
 * off-screen RenderTarget, so a pass keeps drawing against depth a previous
 * pass wrote. Because depth is never promoted to a sampled texture here, such
 * targets must not be sampled for depth (the deferred G-buffer always clears
 * depth, so it never selects this pass).
 *
 * @param device        Device the render pass is created on.
 * @param color_formats Colour attachment formats.
 * @param depth_format  Depth format (VK_FORMAT_UNDEFINED when no depth).
 * @param initial_clear When true, the depth attachment is CLEARED on this
 *                      (first) pass — transitioning a freshly created image
 *                      from UNDEFINED into DEPTH_STENCIL_ATTACHMENT_OPTIMAL —
 *                      instead of being LOADed. Use it for exactly the first
 *                      frame after (re)building a depth-LOAD target.
 * @param color_clear  When true (the default) the colour attachments are
 *                     CLEARed at pass start; false LOADs the previous pass'
 *                     colour (a pass that composites over earlier content
 *                     without clearing it). A LOAD assumes the colour is
 *                     already defined (an earlier pass wrote it this frame).
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeDepthLoadRenderPass(
    ::vsg::Device*               device,
    const std::vector<VkFormat>& color_formats,
    VkFormat                     depth_format,
    bool                         initial_clear = false,
    bool                         color_clear = true);

/**
 * @brief The render-pass variant ONE pass under an off-screen target needs (§28).
 *
 * Pure value: it encodes the load-op / layout / promotion decision for a pass
 * without touching a device, so the §28 invariants are unit-testable:
 *
 *  - a pass that clears gets its own CLEAR pass; a pass that preserves gets a
 *    LOAD pass — that is what removes the "one target, one baked load-op" model
 *    (and its ClearAttachments patch);
 *  - **LOAD and promotion are mutually exclusive**: once any pass of the target
 *    LOADs depth, no pass may leave the depth in SHADER_READ_ONLY, or the next
 *    LOAD would read a layout it cannot attach;
 *  - a LOAD requires the depth image to be DEFINED already; an UNDEFINED image
 *    must be CLEARed once first, which the caller records with
 *    @ref seed_required (the pass' `render_pass_seed` variant);
 *  - a BORROWED depth belongs to its source's policy: always LOAD, never
 *    promoted, and never seeded (the source defined it this frame).
 */
struct PassRenderPassPlan
{
    VkAttachmentLoadOp color_load    = VK_ATTACHMENT_LOAD_OP_CLEAR; ///< CLEAR, or LOAD for a pass that composites.
    VkAttachmentLoadOp depth_load    = VK_ATTACHMENT_LOAD_OP_CLEAR; ///< CLEAR, or LOAD for a preserving pass.
    VkImageLayout      depth_initial = VK_IMAGE_LAYOUT_UNDEFINED;   ///< The depth layout the pass starts from.
    bool               promote_depth = false;                       ///< Depth ends SHADER_READ_ONLY (sampleable).
    bool               seed_required = false;                       ///< Record the CLEAR variant once before LOADing.
};

/**
 * @brief Computes the render-pass variant for one pass under a target.
 *
 * @param color_clear       The pass clears colour (false composites over it).
 * @param want_depth_clear  The pass clears depth (false preserves it).
 * @param has_depth         The target has a depth attachment.
 * @param promote_requested The target asked for a sampleable depth.
 * @param any_load_pass     Another pass of this target already LOADs depth.
 * @param depth_seeded      The target's depth image has been defined already.
 * @param borrowed_depth    The depth belongs to another target (shareDepth).
 * @return The variant to build (see PassRenderPassPlan).
 */
PassRenderPassPlan planPassRenderPass(bool color_clear,
                                      bool want_depth_clear,
                                      bool has_depth,
                                      bool promote_requested,
                                      bool any_load_pass,
                                      bool depth_seeded,
                                      bool borrowed_depth);

/**
 * @brief Vertex shader source shared by the full-screen overlay passes.
 *
 * Generates the full-screen triangle from gl_VertexIndex alone, so the draws
 * need no vertex buffers or camera matrices. Both overlay builders (PiP
 * screen sampling and fullscreen user-program lighting) use this identical
 * vertex stage.
 *
 * @return The GLSL vertex source.
 */
const std::string& fullscreenVertexSource();

/**
 * @brief Builds the default pipeline states for the full-screen overlay
 * passes.
 *
 * Both overlay draws (PiP screen sampling, fullscreen program lighting)
 * composite over already-rendered content: depth test/write stay off and the
 * rasterizer culls nothing; blending stays at the opaque default because each
 * pass replaces the sub-viewport it owns.
 *
 * @param extent Surface extent for the baked static viewport.
 * @return The default GraphicsPipelineStates.
 */
::vsg::GraphicsPipelineStates makeOverlayPipelineStates(const VkExtent2D& extent);

/**
 * @brief Builds a state-group that draws a full-screen textured triangle
 * sampling @p image_view into the current target.
 *
 * The vertex shader generates the full-screen triangle from gl_VertexIndex
 * (no vertex buffers / camera matrices involved); the fragment shader samples
 * the passed image. Depth test/write are disabled so the textured triangle
 * composites over previously rendered content (used by the PiP screen pass).
 *
 * @param image_view Image to sample (the off-screen target's colour view).
 * @param extent     Surface extent for the baked static viewport.
 * @return The drawable state-group, or null when shader compilation failed.
 */
::vsg::ref_ptr<::vsg::Node> makeScreenTextureNode(::vsg::ref_ptr<::vsg::ImageView> image_view, const VkExtent2D& extent,
                                                  ProgramNodeFailure* failure = nullptr);

/**
 * @brief Builds a full-screen textured node running a user fragment program.
 *
 * The vertex shader generates the full-screen triangle from gl_VertexIndex
 * (no vertex buffers / camera matrices); the fragment shader is @p program's
 * fragment stage, sampling @p image_views (one per colour attachment of the
 * source MRT target, descriptor binding i = attachment i) plus, when @p
 * depth_view is set, the source's depth attachment (next binding), and reads a
 * per-frame @p push_data block (see LightPushBlock). Depth test/write are
 * disabled and blending is off: the draw overwrites the sub-viewport it owns.
 * The retained node is drawn as its own View (viewport = the PiP rectangle),
 * so it composites over previously rendered content.
 *
 * Fragment-shader ABI the user program must follow:
 *   layout(location = 0) in vec2 v_uv;
 *   layout(binding = i) uniform sampler2D <any>;   // i-th source colour attachment
 *   layout(binding = N) uniform sampler2D depth;    // source depth (N = colour count)
 *   layout(push_constant) uniform PushConstants { vec4 ... } pc;  // LightPushBlock
 *   layout(location = 0) out vec4 out_color;
 *
 * @param program    User program supplying the fragment stage (any vertex
 *                   stage is ignored; the backend provides the fullscreen VS).
 * @param image_views Source MRT colour-attachment views to sample.
 * @param depth_view  Source depth-attachment view to sample (may be null).
 * @param extent     Surface extent for the baked static viewport.
 * @param push_data  Per-frame push-constant bytes (mutated before each record).
 * @return The drawable state-group, or null when shader compilation failed.
 */
::vsg::ref_ptr<::vsg::Node> makeFullscreenProgramNode(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const ::vsg::ImageViews&                          image_views,
    ::vsg::ref_ptr<::vsg::ImageView>                  depth_view,
    const VkExtent2D&                                 extent,
    ::vsg::ref_ptr<::vsg::Data>                       push_data,
    ProgramNodeFailure*                               failure = nullptr);

/**
 * @brief Replaces a view's light-group children with the given Vine lights.
 *
 * Light nodes carry no GPU resources (they are collected into the per-view
 * lightData uniform at record time), so rebuilding them each frame is cheap
 * and needs no recompile.
 *
 * A view must always be lit by SOMETHING: vsg's Phong accumulates the view's
 * light set, so a view with no light shades every surface to black, and a host
 * that disables a light is toggling it off, not asking for an unlit scene. The
 * group is therefore left untouched — the caller's seeded default light
 * survives — unless the announced list yields at least one usable light node.
 * "No usable node" covers an empty list, every entry disabled, and every entry
 * of a kind this backend does not translate.
 *
 * @param group  The view's light group (null is ignored).
 * @param lights Vine lights to attach (borrowed for the call).
 * @return Number of light nodes attached; 0 means the group was left as-is
 *         (the caller keeps its default light and may report the fallback).
 */
std::size_t setGroupLights(::vsg::Group* group, const std::vector<const vine::graphics::Light*>& lights);

/**
 * @brief Builds the flat white ambient light used to seed non-scene content
 * slots (HUD overlays and off-screen main slots).
 *
 * Ambient-only lighting makes Phong's colour independent of surface
 * orientation, which is what keeps HUD/axis content readable from any angle.
 *
 * @param name Node name (distinguishes the HUD seed from the off-screen one).
 * @return The ambient light node.
 */
::vsg::ref_ptr<::vsg::Node> makeAmbientLight(const char* name);

} // namespace detail

V_VSG_NS_END
