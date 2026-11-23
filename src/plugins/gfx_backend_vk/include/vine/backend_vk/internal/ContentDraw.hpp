#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <span>

#include <volk.h>

#include <vine/backend_vk/internal/ContentAbi.hpp>
#include <vine/backend_vk/internal/ContentCaches.hpp>
#include <vine/backend_vk/internal/ContentRefusal.hpp>
#include <vine/backend_vk/internal/FrameHead.hpp>
#include <vine/graphics/DrawAbi.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/LightAbi.hpp>
#include <vine/graphics/MaterialAbi.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/DrawState.hpp>
#include <vine/math/Matrix4x4.hpp>

VN_BACKEND_VK_NS_BEGIN

class ContentCaches;
class FrameScratch;

/**
 * @brief The content path's RECORDER: the draw packets of one pass become indexed Vulkan draws, using what the
 *        device's caches built and the driver's own frame scratch.
 *
 * @details
 * - TWO OWNERS, ONE JOB EACH (see `.ai/design/graphics-sessions.md` §3.1.3): `ContentCaches` (the device's) answers
 *   WHAT a command draws with - compiled programs, uploaded streams, pipelines - and `FrameScratch` (the driver's)
 *   answers WHERE this frame's constants and descriptor sets live. This type owns neither; it is the value that turns
 *   one `DrawPacket` into commands, which is why a second driver needs a second scratch and NOT a second cache.
 * - IT HOLDS a borrowed reference to the device's caches for their IMMUTABLE projections only (`table()`, the pipeline
 *   layouts, the line-width limit): what a draw packet draws with arrives as the frame head's resolved values
 *   (`FrameHead::DrawPlan`), so the recording path takes no cache lock and names no cache entry.
 * - WHAT IT DOES NOT DO. It does not compile, upload or build pipelines: the frame head resolves every command (and
 *   `prepareFrame` runs the SAME resolution), so a frame cannot ask for something the preparation would not have built
 *   (`R23`) - and the recorder cannot change what the caches hold.
 * - THE CONTENT ABI IS THE ENGINE'S, NOT THIS BACKEND'S (M4): set 0 is the content set - material at binding 0, the
 *   five texture slots at 1-5, the light block at 6, the shadow pair at 7/8 - and set 1 binding 0 is the draw block.
 *   The camera matrices are the 128-byte push range (`projection`, `modelView`), exactly the L2 realization
 *   `vine_params.glsl` describes. A full-screen draw uses the OTHER ABI: the source target's attachments at set 0
 *   bindings 0..N-1.
 * - THE SHADOW ABI IS PART OF THAT SET (M9): binding 7 is the map and binding 8 is `VineShadowBlock`, because the
 *   engine's forward text declares both unconditionally. A pass that samples no map binds the 1x1 stand-in and zeroes
 *   the block (`params.x = 0` is the switch the one text takes its unshadowed path with), so a shadow is an
 *   ADDITIONAL fact about a pass rather than a second pipeline.
 * - WHAT IS REPORTED, NOT DRAWN. A draw with no camera issues nothing (see `DrawPacket::camera`), one with no
 *   program is skipped and reported, and a full-screen draw with no source target is skipped and reported. Every skip
 *   leaves the rest of the pass drawn, and the FIRST reason travels in a `ContentRefusal` so the device's report
 *   carries the code of the cause rather than a sentence about a symptom.
 *
 * @note The name says what it records, and it is deliberately the CONTENT half: the clear-only pass M1 records stays in
 *       the device, because a pass' scope is the device's, not this object's.
 *
 * @warning Not thread-safe: one frame at a time, from the thread that drives its scratch (the SDK's per-driver
 *          contract). Two drivers are concurrent because each owns its own recorder and scratch.
 */
class VN_BACKEND_VK_API ContentDraw final {
  public:
    /** @brief One full-screen draw packet's source: the target's colour images and the views a fragment stage samples. */
    struct ScreenSource {
        std::span<const VkImage>     images{}; // The source's colour images (for layout transitions; may be empty).
        std::span<const VkImageView> views{};  // The same attachments' views, bound at set 0 bindings 0..N-1.
    };

    /**
     * @brief The SHADOW MAP a content pass samples: whose map it is and how a fragment maps into it.
     *
     * @details
     * - THE MAP ARRIVES AS A PASS INPUT, and this is the device's reading of what the TARGET said about it
     *   (`RenderTarget::shadowOf` + `producerViewProjection`): a target is a map because it states whose shadow it is,
     *   never because it happens to have a sampleable depth (a G-buffer does too).
     * - THREE FACTS, each from its owner: the VIEW is the image the producing pass wrote (already in
     *   `SHADER_READ_ONLY_OPTIMAL`, the device transitions it before the pass begins), the MATRIX is the producer's own
     *   (light clip <- world, in the SDK's clip convention), and the LIGHT is the identity the map belongs to - what
     *   the draw packet's announced lights are matched against, because the switch lives on the light.
     * - AN EMPTY ONE MEANS "NO MAP": the content set binds the 1x1 white stand-in and the block's switch stays off, so
     *   the engine's forward text takes its unshadowed path with the same pipeline.
     *
     * @see ShadowBlock: the packing rules, the clip conversion and the reverse-Z comparison.
     */
    struct ShadowSource {
        VkImageView     view{ VK_NULL_HANDLE };       // The map's depth view, bound at set 0 binding 7.
        bool            has_view_projection{ false }; // Whether the producer published how to read the map.
        vn::math::Mat4d view_projection{};            // Light clip <- world, in the SDK's clip convention.
        const void*     light{ nullptr };             // Whose map it is (a `Light`'s identity; nullptr = not a map).
    };

  public:
    /**
     * @brief Builds a recorder over the device's caches and one frame driver's scratch.
     *
     * @param caches  The device's cache half (borrowed; outlives this recorder). Only its immutable projections are
     *                read (see the class note).
     * @param scratch The driver's frame scratch (borrowed; outlives this recorder).
     */
    ContentDraw(ContentCaches& caches, FrameScratch& scratch) noexcept;

    ContentDraw(const ContentDraw&)            = delete;
    ContentDraw& operator=(const ContentDraw&) = delete;

  public:
    /**
     * @brief Records one CONTENT draw description's commands into @p commands.
     *
     * @details
     * - Must run INSIDE the rendering instance that names the target, because it records drawing commands. Everything
     *   it draws with arrives as the frame head's plans: this call creates nothing, uploads nothing and takes no lock.
     * - The viewport is the pass' rectangle, or the whole of the target when the pass states none.
     * - The STATE was resolved per command (`resolveRenderState`) when the plan was made, and EVERY item of it is a
     *   command here: cull/front face, polygon mode, depth test/write/compare (reverse-Z), blend enable and factors,
     *   line width, depth bias values and the viewport. No item selects a pipeline any more, so a state change
     *   compiles nothing (`T54`); the topology does select it, and the point size travels in the draw block. A
     *   multi-attachment target never blends (its attachments hold data, not a surface - see BlendState).
     * - Each command's material values are packed into the material block; the resolved slots bind their uploaded
     *   views, and an absent one binds the 1x1 white fallback with its slot bit clear. The draw's announced lights are
     *   packed into the light block in view space (see LightBlock.hpp).
     * - THE SHADOW MAP, when @p shadow names one: its depth view is bound at the content set's shadow binding and the
     *   `VineShadowBlock` is packed per call from the map's own statement (see ShadowSource) - the block's switch is
     *   on only when the map's light is one this call announced AND still casts, so a map nobody can name shades
     *   nothing rather than scaling a light it does not belong to.
     * - THE CONSTANTS GO INTO THE DRIVER'S RING (see FrameScratch): one slice per call, bound as dynamic offsets, and
     *   a full ring skips the draw and grows before the next frame rather than growing in place.
     *
     * @param commands        The command buffer being recorded (inside a rendering instance).
     * @param width           The target's width in pixels.
     * @param height          The target's height in pixels.
     * @param draw            The draw to record (borrowed for the call).
     * @param pass            The pass drawing it (its override, state and viewport are read; borrowed).
     * @param plans           One resolved plan per command of @p draw, from the frame head (same order).
     * @param head            The frame's head: the stand-in view/sampler and the shadow sampler every call reads.
     * @param shadow          The shadow map this pass samples (see ShadowSource), or an empty one for none.
     * @param refusal         Receives the first reason a command was skipped (cleared on entry; empty when every
     *                        command was recorded), or null.
     * @return How many draw packets were recorded.
     */
    std::uint64_t record(VkCommandBuffer                           commands,
                         int                                       width,
                         int                                       height,
                         const vn::graphics::DrawPacket&      draw,
                         const vn::graphics::PassDraw&             pass,
                         std::span<const FrameHead::DrawPlan>      plans,
                         const FrameHead&                          head,
                         ShadowSource                              shadow,
                         ContentRefusal*                           refusal = nullptr);

    /**
     * @brief Records one SCREEN draw description: a full-screen triangle sampling @p source.
     *
     * @details
     * - The vertex stage is the engine's `BuiltinShaders::fullscreenVertexProgram()`; the fragment stage is the draw's
     *   own program description, compiled against the full-screen ABI (`vn_uv` at location 0). The source's colour
     *   attachments are bound at set 0 bindings 0..N-1 with the fallback sampler, and the draw's lights at binding 6
     *   (the full-screen deferred program reads them there).
     * - SET 0 ALSO OFFERS THE ENGINE'S SHADOW PAIR (binding 7 the map, 8 `VineShadowBlock`), because the engine's
     *   shadowed deferred-lighting program declares both unconditionally. When the pass resolved a readable map (see
     *   ShadowSource) the REAL map and a block packed for this call are bound - a full-screen pass that shades is exactly
     *   where the sun's shadow has to arrive; with no map the white stand-in and the zeroed block are bound, which is
     *   the engine's own "no shadow reaches this pass".
     * - The pass' state becomes commands exactly as a content draw's does (reverse-Z, culling, blend - all dynamic);
     *   a full-screen draw writes its rectangle, and a multi-attachment target never blends (the content rule).
     * - The source's images must already be in `SHADER_READ_ONLY_OPTIMAL`: the device records that transition before
     *   the destination's rendering instance begins, where an image barrier is unambiguously allowed.
     *
     * @param commands          The command buffer being recorded (inside the destination's rendering instance).
     * @param width             The destination's width in pixels.
     * @param height            The destination's height in pixels.
     * @param draw              The full-screen draw (its fragment program and camera are read; borrowed).
     * @param pass              The pass drawing it (state and viewport; borrowed).
     * @param plan              The resolved program and state, from the frame head.
     * @param head              The frame's head: the fallback view/sampler and the shadow sampler.
     * @param source            The source target's colour images/views (see ScreenSource).
     * @param shadow            The map this pass resolved (see ShadowSource), or an empty one for none.
     * @param refusal           Receives the reason when the draw was skipped (cleared on entry), or null.
     * @return 1 when the full-screen draw was recorded, 0 otherwise.
     */
    std::uint64_t recordFullscreen(VkCommandBuffer                               commands,
                               int                                           width,
                               int                                           height,
                               const vn::graphics::DrawPacket&          draw,
                               const vn::graphics::PassDraw&                 pass,
                               const FrameHead::DrawPlan&                    plan,
                               const FrameHead&                              head,
                               ScreenSource                                  source,
                               ShadowSource                                  shadow,
                               ContentRefusal*                               refusal = nullptr);

  private:
    /**
     * @brief Writes one call's four blocks into @p slice at the offsets the scratch states.
     *
     * @param slice    The slice's byte offset (see FrameScratch::reserveSlice).
     * @param material The material block (offset 0).
     * @param lights   The light block (offset `lightsOffset`).
     * @param draw     The per-draw block (offset `drawOffset`).
     * @param shadow   The shadow block (offset `shadowOffset`).
     */
    void writeSlice(VkDeviceSize                          slice,
                    const vn::graphics::VineMaterialBlock& material,
                    const vn::graphics::VineLightsBlock&   lights,
                    const vn::graphics::VineDrawBlock&     draw,
                    const vn::graphics::VineShadowBlock&   shadow) const noexcept;

  private:
    ContentCaches& caches;
    FrameScratch&  scratch;
};

VN_BACKEND_VK_NS_END
