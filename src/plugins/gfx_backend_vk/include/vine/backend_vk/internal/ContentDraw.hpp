#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <span>
#include <vector>

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
    /**
     * @brief One full-screen draw packet's source: the views a fragment stage samples, in attachment order.
     *
     * @details
     * - VIEWS ONLY. The source's IMAGES are not here because a recorder never moves them: the transitions a sampled
     *   picture needs are recorded in pass order, against the per-image records the source target owns (`G1`: one fact
     *   one owner, and the source is the owner). An empty span is a source this device never built, which makes the
     *   call report and skip instead of binding nothing.
     * - ONE CONTIGUOUS SPAN because the set binds 0..N-1 as one call (`vkUpdateDescriptorSets` over the same array),
     *   which is also why `PassRole` keeps its views in their own span.
     */
    struct FullscreenSource {
        std::span<const VkImageView> views{}; // The source's attachment views, bound at set 0 bindings 0..N-1.
    };

    /**
     * @brief The SHADOW MAP a content pass samples (defined in `FrameHead.hpp`, where its rules are documented).
     *
     * @details
     * - IT IS ONE TYPE, NOT TWO (`G1`): the CLAIM resolves the map with the rest of the frame's read set, so the value
     *   is a `FrameHead::Pass` member; the recorder names the same type instead of defining a second value that would
     *   have to be copied field by field.
     */
    using ShadowSource = ::vn::backend_vk::ShadowSource;

    /**
     * @brief WHAT ONE PASS RECORDS WITH: its command buffer, its rectangle, the frame's head and the shadow it resolved.
     *
     * @details
     * - ONE VALUE FOR THE PASS-VARYING FACTS (`G1` / `G4`). Every call of a pass shares the command buffer, the pass'
     *   rectangle and the frame head, and every call of the pass samples the same map - so those are stated once per
     *   pass instead of in every draw call's argument list (which used to be nine arguments, four of them the same
     *   value for the whole pass).
     * - THE RECTANGLE IS A PASS FACT AND IS COMPUTED ONCE (`T54`): the viewport is the pass' own rectangle or the whole
     *   target, its depth band comes from `DepthRange` under reverse-Z, and the scissor is the same rectangle. The
     *   recorder states them only when they differ from what the command buffer already holds (see RecordedState).
     * - IT IS A VALUE, NOT AN OWNER: the head and the command buffer are borrowed for the pass; `forPass` copies what
     *   it needs and writes nothing.
     */
    struct DrawScope {
        VkCommandBuffer  commands{ VK_NULL_HANDLE }; ///< The command buffer being recorded (inside the rendering instance).
        const FrameHead* head{ nullptr };            ///< The frame's head (stand-in and shadow sampler).
        ShadowSource     shadow{};                   ///< The map this pass samples, or an empty one.
        VkViewport       viewport{};                 ///< The pass' rectangle, translated for Vulkan.
        VkRect2D         scissor{};                  ///< The same rectangle, as a scissor.

        /**
         * @brief Builds the scope of one pass: the rectangle from @p pass (or the whole of @p extent) and the rest as given.
         *
         * @param commands The command buffer the pass is being recorded into.
         * @param extent   The attachment set's extent (the whole-rectangle fallback).
         * @param head     The frame's head (borrowed for the pass).
         * @param shadow   The shadow map the pass resolved (see ShadowSource), or an empty one.
         * @param pass     The pass being recorded (its viewport and depth band are read).
         * @return The scope, with the viewport and the scissor already resolved.
         */
        [[nodiscard]] static DrawScope forPass(VkCommandBuffer               commands,
                                              VkExtent2D                    extent,
                                              const FrameHead&              head,
                                              ShadowSource                  shadow,
                                              const vn::graphics::PassDraw& pass) noexcept;
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
     * @brief Opens one recording run: forgets what the last frame left in its command buffer.
     *
     * @details
     * - DYNAMIC STATE IS COMMAND BUFFER STATE, and every frame begins a NEW command buffer (see `Device::FrameDriver`),
     *   so what the last frame set is not a fact about this one. This call is where that boundary is stated, and it is
     *   why the recorder can skip a state command that has not changed.
     */
    void beginFrame() noexcept;

    /**
     * @brief Gets how many `vkCmdSet*` commands this recorder issued over its lifetime.
     *
     * @details
     * - THE WITNESS FOR "A STATE CHANGE IS ONE COMMAND, A REPEATED STATE IS NONE": the number grows with distinct state
     *   changes, not with the draws that share one - which is what makes `T54`'s "state is a command" a cost model
     *   rather than a comment. `Device::recordingNumbers` publishes it.
     *
     * @return The cumulative count (0 before the first draw).
     */
    [[nodiscard]] std::uint64_t stateCommands() const noexcept;
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
     * @param scope   The pass' recording scope (see DrawScope): command buffer, rectangle, head and shadow.
     * @param draw    The draw to record (borrowed for the call).
     * @param plans   One resolved plan per command of @p draw, from the frame head (same order).
     * @param refusal Receives the first reason a command was skipped (cleared on entry; empty when every
     *                command was recorded), or null.
     */
    void record(const DrawScope&                         scope,
                const vn::graphics::DrawPacket&          draw,
                std::span<const FrameHead::DrawPlan>      plans,
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
     * @param scope   The pass' recording scope (see DrawScope): command buffer, rectangle, head and shadow.
     * @param draw    The full-screen draw (its fragment program and camera are read; borrowed).
     * @param plan    The resolved program and state, from the frame head.
     * @param source  The source target's views (see FullscreenSource), in attachment order.
     * @param refusal Receives the reason when the draw was skipped (cleared on entry), or null.
     */
    void recordFullscreen(const DrawScope&                scope,
                          const vn::graphics::DrawPacket& draw,
                          const FrameHead::DrawPlan&      plan,
                          FullscreenSource                source,
                          ContentRefusal*                 refusal = nullptr);

  private:
    /**
     * @brief WHAT THE LAST COMMANDS LEFT BOUND in this frame's command buffer: the shadow behind "state is a command
     *        WHEN IT CHANGES".
     *
     * @details
     * - `T54` made every state item a COMMAND rather than a pipeline variant, which is right - but a command that
     *   states what is already stated costs a driver call per draw. The viewport and the scissor are PASS facts (they
     *   are computed from the pass' rectangle), so a pass with ten draws used to state the same rectangle ten times.
     * - IT SURVIVES A PIPELINE BIND, and that is a property of THIS BACKEND, not of Vulkan: a bind resets no dynamic
     *   state, and every pipeline here comes from one builder that declares the same eleven dynamic states
     *   (`buildPipeline`), so a value the buffer already holds is still one the new pipeline reads. A second builder
     *   with a narrower list would have to invalidate the shadow where it binds. Nothing else invalidates it either -
     *   dynamic state is command buffer state, and every other boundary (a new pass) states the same values anyway.
     * - The stored values are the VULKAN ones, not the seam's: this shadow answers "would the call be redundant", not
     *   "what did the host ask for".
     */
    struct RecordedState {
        VkPipeline pipeline{ VK_NULL_HANDLE };
        bool       viewport_set{ false };
        VkViewport viewport{};
        bool       scissor_set{ false };
        VkRect2D   scissor{};
        bool       line_width_set{ false };
        float      line_width{ 0.0F };
        bool       depth_bias_set{ false };
        float      depth_bias_units{ 0.0F };
        float      depth_bias_factor{ 0.0F };
        bool       raster_set{ false };
        VkPolygonMode      polygon{ VK_POLYGON_MODE_FILL };
        VkCullModeFlags    cull{ VK_CULL_MODE_NONE };
        VkBool32           depth_test{ VK_FALSE };
        VkBool32           depth_write{ VK_FALSE };
        VkCompareOp        depth_compare{ VK_COMPARE_OP_ALWAYS };
        bool               blend_set{ false };
        VkBool32           blend_enabled{ VK_FALSE };
        VkBlendFactor      blend_src{ VK_BLEND_FACTOR_ONE };
        VkBlendFactor      blend_dst{ VK_BLEND_FACTOR_ZERO };
        std::uint32_t      blend_attachments{ 0U };
    };

    /**
     * @brief Binds @p pipeline unless it is the one already bound; the state shadow is KEPT.
     *
     * @details
     * - Keeping it is sound because every pipeline this backend builds declares the same dynamic set (see
     *   `RecordedState`): Vulkan keeps dynamic state across `vkCmdBindPipeline`, so re-stating viewport, scissor and
     *   the raster half after every bind would be a command per item with no new meaning.
     *
     * @param commands The command buffer being recorded.
     * @param pipeline The pipeline this draw needs.
     */
    void bindPipeline(VkCommandBuffer commands, VkPipeline pipeline) noexcept;

    /**
     * @brief States the ONE viewport, scissor, line width, depth bias and raster state a draw needs - skipping what the
     *        command buffer already holds.
     *
     * @param commands          The command buffer being recorded.
     * @param viewport          The pass' rectangle (or the whole target).
     * @param scissor           The same rectangle as a scissor.
     * @param resolved          The command's resolved state (cull, polygon, depth, blend, dynamic half).
     * @param color_attachments How many colour attachments the pass writes (0 = depth-only: no blend).
     * @param depth_bias_units  The depth bias this call states (`DynamicState`'s, or zero for a full-screen draw).
     * @param depth_bias_factor The factor that goes with it.
     */
    void recordCallState(VkCommandBuffer                            commands,
                         const VkViewport&                          viewport,
                         const VkRect2D&                            scissor,
                         const vn::graphics::ResolvedCommandState&  resolved,
                         std::uint32_t                              color_attachments,
                         float                                      depth_bias_units,
                         float                                      depth_bias_factor) noexcept;

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
    /// What the last commands left bound (see RecordedState); reset by @ref beginFrame, and only there.
    RecordedState  recorded{};
    /// The vertex bind lists of the command being recorded: cleared and refilled per call, so their storage is
    /// allocated once instead of twice per command (see the class note on the recording cost).
    std::vector<VkBuffer>     bind_buffers_{};
    /// The offsets that go with `bind_buffers_`: every stream is bound at its first byte, so every entry is 0.
    std::vector<VkDeviceSize> bind_offsets_{};
    /// How many state commands this recorder issued (see @ref stateCommands).
    std::uint64_t  state_commands_{ 0U };
};

VN_BACKEND_VK_NS_END
