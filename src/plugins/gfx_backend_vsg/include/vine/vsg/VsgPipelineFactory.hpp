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
    /// The fragment stage declares a descriptor binding this pass cannot
    /// provide (only the source's colour attachments are bound, plus its depth
    /// when that one really is sampleable): a pipeline whose layout lacks the
    /// binding would fail at DRAW time, so the pass is refused up front.
    MissingDescriptorBinding,
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
 * @brief Builds the colour(+depth) render pass ONE pass records into.
 *
 * vsg::createRenderPass() leaves the colour attachment in PRESENT_SRC_KHR
 * (correct for swapchain output, wrong for a texture sampled by a later pass).
 * Here the colour attachments always end in SHADER_READ_ONLY_OPTIMAL and the
 * subpass-external fragment-read dependency makes their writes visible to the
 * pass that samples them without an extra barrier. A target with several colour
 * attachments (MRT / G-buffer) gets one attachment per entry, all sampleable on
 * their own; fragment output @p i writes attachment @p i.
 *
 * Every colour+depth variant of a pass is this function with different
 * arguments: the variants differ only in load-ops and in the depth attachment's
 * initial layout — the two things the Vulkan spec's Render Pass Compatibility
 * rules exempt — while the attachment set, the subpass and (critically) the
 * subpass DEPENDENCIES stay identical, which is what lets a pass swap its render
 * pass at run time and keep the pipelines it already compiled (see
 * VsgRenderer::passGraph and makeColorDepthDependencies()).
 *
 * @param device        Device the render pass is created on.
 * @param color_formats Colour attachment formats, one per attachment (in
 *                      attachment order).
 * @param depth_format  Depth attachment format, or VK_FORMAT_UNDEFINED for a
 *                      colour-only pass.
 * @param depth_initial The layout the depth image is in when the pass starts:
 *                      VK_IMAGE_LAYOUT_UNDEFINED CLEARs it (a freshly created
 *                      image no pass has defined yet),
 *                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL LOADs
 *                      it from a previous pass' attachment use, and
 *                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL LOADs it from an
 *                      image a previous pass left PROMOTED to a sampled depth
 *                      (the pass then hands it back in the layout its consumers
 *                      expect). An UNDEFINED image may only be CLEARed, so a
 *                      LOAD pass has to name the layout its image really is in.
 * @param promote_depth When true the depth attachment ends in
 *                      SHADER_READ_ONLY_OPTIMAL (sampleable); false leaves it a
 *                      plain depth attachment another target can borrow. Only a
 *                      pass that CLEARs depth may promote it (a LOAD pass must
 *                      hand the image back in the attachment layout, see
 *                      planPassVariant()).
 * @param color_clear   When true (the default) the colour attachments are
 *                      CLEARed at pass start; false LOADs the previous pass'
 *                      colour (a pass that composites over earlier content
 *                      without clearing it). A LOAD assumes the colour is
 *                      already defined (an earlier pass wrote it this frame).
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeColorDepthRenderPass(
    ::vsg::Device*                    device,
    const std::vector<VkFormat>&      color_formats,
    VkFormat                          depth_format,
    VkImageLayout                     depth_initial = VK_IMAGE_LAYOUT_UNDEFINED,
    bool                              promote_depth = true,
    bool                              color_clear = true);

/**
 * @brief Builds a depth-only off-screen render pass (shadow maps).
 *
 * The depth attachment is stored and left in SHADER_READ_ONLY_OPTIMAL so a
 * later pass can sample it as a shadow map; the subpass-external fragment-read
 * dependency makes the depth writes visible to the sampling pass.
 *
 * A depth-only target keeps its depth sampleable no matter which pass runs
 * (that is what the target is for), so its two variants — CLEAR and LOAD —
 * differ only in the load-op and the initial layout, exactly like the
 * colour+depth pass above.
 *
 * @param device       Device the render pass is created on.
 * @param depth_format Depth attachment format.
 * @param depth_initial Layout the depth image is in when the pass starts:
 *                     VK_IMAGE_LAYOUT_UNDEFINED CLEARs it (a freshly created
 *                     image no pass has defined yet), any other layout LOADs
 *                     it — in practice
 *                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, which is where
 *                     a depth-only target's image always ends.
 * @return The configured render pass.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeDepthOnlyRenderPass(
    ::vsg::Device* device, VkFormat depth_format, VkImageLayout depth_initial = VK_IMAGE_LAYOUT_UNDEFINED);

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
 *  - a LOAD requires the depth image to be in the layout the pass names; a
 *    freshly created image no pass has defined yet is UNDEFINED and must be
 *    CLEARed once first, which the caller records with @ref seed_required (the
 *    pass' one-frame `render_pass_transient`);
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
 * @brief The render-pass variant ONE pass records this frame (§28).
 *
 * Values only: the load-ops and layouts a render pass is built from, plus whether
 * the pass records a ONE-FRAME variant that VsgRenderer::submitFrame() swaps for
 * the steady one. Split out of PassRenderPassPlan so "which pass records what" is
 * device-free AND fully covered by tests — every invariant this backend needs is
 * expressed here (see planPassVariant()).
 */
struct PassVariant
{
    VkAttachmentLoadOp color_load    = VK_ATTACHMENT_LOAD_OP_CLEAR; ///< RECORDED colour load-op.
    VkAttachmentLoadOp depth_load    = VK_ATTACHMENT_LOAD_OP_CLEAR; ///< RECORDED depth load-op.
    VkImageLayout      depth_initial = VK_IMAGE_LAYOUT_UNDEFINED;   ///< RECORDED depth initial layout (UNDEFINED when the pass CLEARs).
    bool               promote_depth = false;                       ///< The depth may end SHADER_READ_ONLY.
    /// True when the recorded variant is a ONE-FRAME one and submitFrame() has to
    /// swap the graph to the steady variant afterwards.
    bool               transient   = false;
    /// The layout the STEADY variant (and every later frame's LOAD pass) expects:
    /// SHADER_READ_ONLY for a depth-only target, whose depth is always sampleable,
    /// and the attachment layout otherwise.
    VkImageLayout      steady_depth_initial = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    /// The colour load-op the STEADY variant uses: the pass' own request, without
    /// the one-frame bootstrap that defines a fresh target's colour image.
    VkAttachmentLoadOp steady_color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
};

/**
 * @brief Decides the render-pass variant one pass records (§28).
 *
 * Pure: it wraps planPassRenderPass() (which decides the depth POLICY) and adds
 * the materialisation the caller has to get right:
 *
 *  - the colour bootstrap: the first pass into a target whose colour image is
 *    UNDEFINED clears it (a render pass may not LOAD an UNDEFINED image) — ONCE,
 *    as a transient variant, or that pass would wipe its siblings' draws for ever;
 *  - the depth SEED: a preserving pass on an image no pass has defined yet records
 *    a depth CLEAR for one frame (plan.seed_required);
 *  - the promotion REVOKE: a preserving pass whose image still carries the
 *    SHADER_READ_ONLY layout an earlier frame's promoting pass left behind names
 *    that layout for one frame (see @p depth_left_promoted).
 *
 * @param has_color        The target has colour attachments.
 * @param has_depth        The target has a depth attachment (own or borrowed).
 * @param borrowed         The depth belongs to another target (shareDepth).
 * @param promote_requested The target's description asks for a sampleable depth.
 * @param any_load_pass    A pass of this target already preserves (LOADs) depth.
 * @param depth_seeded     The target's depth image has been defined already.
 * @param color_seeded     The target's colour image has been defined already.
 * @param want_color_clear The pass asked to clear colour (it called clear()).
 * @param want_depth_clear The pass asked to clear depth.
 * @param depth_left_promoted The depth image is still in SHADER_READ_ONLY: a pass
 *                        of this target promoted it and no pass that records
 *                        BEFORE this one has run yet this frame.
 * @return The variant to build (see PassVariant).
 */
PassVariant planPassVariant(bool has_color, bool has_depth, bool borrowed, bool promote_requested,
                            bool any_load_pass, bool depth_seeded, bool color_seeded, bool want_color_clear,
                            bool want_depth_clear, bool depth_left_promoted);

/**
 * @brief Whether a pass has to re-record its render-pass variant.
 *
 * The comparison is on the pass' REQUESTS, never on the load-ops it materialised:
 * those carry the one-frame bootstrap/seed too, and the same frame builds one pass
 * twice (setupContentSlot() and render()), so comparing materialised load-ops would
 * rebuild the second build into a LOAD against images nothing has defined yet.
 *
 * @param recorded_want_color_clear The pass' colour request when it was recorded.
 * @param recorded_want_depth_clear The pass' depth request when it was recorded.
 * @param want_color_clear The pass' colour request now.
 * @param want_depth_clear The pass' depth request now.
 * @return true when the requests differ and the variant has to be rebuilt.
 */
bool passVariantIsStale(bool recorded_want_color_clear, bool recorded_want_depth_clear, bool want_color_clear,
                        bool want_depth_clear);

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
 * @brief Returns whether a full-screen program SAMPLES the source's depth.
 *
 * The full-screen program ABI gives the source's depth the binding index the
 * colour count sets (see makeFullscreenProgramNode), so a program samples the
 * depth exactly when its fragment stage declares that binding. Only such a
 * program ends up with the depth in its pipeline layout and its descriptor set;
 * a colour-only program records nothing that names the depth's layout.
 *
 * @param program     User program supplying the fragment stage (may be null).
 * @param color_count Number of colour attachments of the sampled source.
 * @return true when the fragment stage declares the depth binding.
 */
bool programSamplesDepth(vine::raw_ptr<const vine::graphics::ShaderProgram> program, std::size_t color_count);

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
