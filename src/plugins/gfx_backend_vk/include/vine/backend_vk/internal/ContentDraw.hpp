#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <memory>
#include <span>

#include <volk.h>

#include <vine/String.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/backend/Keys.hpp>
#include <vine/math/Matrix4x4.hpp>

VN_BACKEND_VK_NS_BEGIN

class ContentImages;

/**
 * @brief The content path (M2-M6): the draws of one pass become indexed Vulkan drawing calls, and what they built is
 *        accounted for, can be prepared ahead of time, and can be let go when the host says the content is done.
 *
 * @details
 * - WHAT IT OWNS. Compiled shader modules per program REVISION (keyed by the immutable `ShaderProgramDescription`),
 *   uploaded vertex/index streams per geometry REVISION (keyed by `GeometryDescription`), the descriptor and pipeline
 *   layouts every pipeline shares, the pipelines themselves, and one small uniform buffer per recorded draw holding
 *   the `VineMaterialBlock`, `VineLightsBlock` and `VineDrawBlock` (std140; the first and the third in ShaderAbi.hpp,
 *   the light block in LightAbi.hpp).
 * - A PIPELINE IS KEYED BY COMPATIBILITY, NOT BY A RENDER-PASS HANDLE. `RenderPassCompatibility` names the attachment
 *   set (formats, depth, samples, subpass), which is what Vulkan compares a pipeline against; an EXTENT change creates
 *   new images and a new framebuffer but no new render pass, so every pipeline stays valid and is REUSED. That is the
 *   observable half of M3's "a resize must not recompile" rule, pinned by `pipelinesCreated()`.
 * - WHAT IT DOES NOT OWN. Nothing of the frame: every pointer inside a `DrawDescription` is borrowed for the call, and
 *   a description it keeps as a cache key is retained by an intrusive pointer - so a later edit of the live object is
 *   a NEW description and a new key, never a silent reuse of old bytes.
 * - THE CONTENT ABI IS THE ENGINE'S, NOT THIS BACKEND'S (M4): set 0 is the content set - material at binding 0, the
 *   five texture slots at 1-5, the light block at 6 - and set 1 binding 0 is the draw block. The camera matrices are
 *   the 128-byte push range (`projection`, `modelView`), exactly the L2 realization `vine_params.glsl` describes.
 *   A full-screen draw uses the OTHER ABI: the source target's attachments at set 0 bindings 0..N-1.
 * - THE SHADOW ABI IS PART OF THAT SET (M9): binding 7 is the map and binding 8 is `VineShadowBlock`, because the
 *   engine's forward text declares both unconditionally. A pass that samples no map binds the 1x1 stand-in and zeroes
 *   the block (`params.x = 0` is the switch the one text takes its unshadowed path with), so a shadow is an
 *   ADDITIONAL fact about a pass rather than a second pipeline.
 * - MEMORY MODEL. Streams, the per-draw block and the texture staging copies live in host-visible coherent memory. A
 *   staging copy for geometry would add a synchronisation story this step does not teach; the residency step (M6) is
 *   what moves streams to device-local memory behind one copy.
 * - FRAME BOUNDARY. `beginFrame()` resets the descriptor pool and `endFrame()` releases the per-draw uniform buffers,
 *   the retired texture uploads and their staging buffers. The device calls both around ONE command buffer, after the
 *   queue is idle, which is what makes the reuse safe without a ring of frames.
 * - WHAT IS REPORTED, NOT DRAWN. A draw with no camera issues nothing (see `DrawDescription::camera`), one with no
 *   program is skipped and reported, and a screen draw with no source target is skipped and reported. Every skip leaves
 *   the rest of the pass drawn.
 * - WHAT IS RELEASED, AND BY WHOM. Nothing here decides that a cache entry is no longer wanted: the host says so
 *   (`RenderDevice::releaseContent`), and this object drops exactly what the announcement names - a geometry's
 *   streams, a program's modules and the pipelines compiled from them, the streams one `Buffer` was uploaded from,
 *   or a texture's image (see `ContentImages`). The next frame that names the content builds it again, which is what
 *   the release costs; `progress()` is where the numbers that must go down (residency) and the ones that only grow
 *   (builds) are read.
 * - PREPARE, NOT EXECUTE. `prepare()` / `prepareScreen()` run the SAME resolution a recorded draw runs - program
 *   compiled, geometry uploaded, pipeline selected - and stop before any command is recorded, so a host pays the
 *   first frame's cost at load time and a later frame of the same declarations builds nothing (`R23`).
 *
 * @note The name says what it records, and it is deliberately the CONTENT half: the clear-only pass M1 records stays in
 *       the device, because a pass' scope is the device's, not this object's.
 *
 * @warning Not thread-safe: one frame at a time, from the thread that records it (the SDK's contract).
 */
class VN_BACKEND_VK_API ContentDraw final {
  public:
    /** @brief One screen drawing call's source: the target's colour images and the views a fragment stage samples. */
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
     *   never because it happens to have a sampleable depth (a G-buffer does too - see `ShadowFacts` in
     *   `backend/FrameCompiler.hpp`).
     * - THREE FACTS, each from its owner: the VIEW is the image the producing pass wrote (already in
     *   `SHADER_READ_ONLY_OPTIMAL`, the device transitions it before the pass begins), the MATRIX is the producer's own
     *   (light clip <- world, in the SDK's clip convention), and the LIGHT is the identity the map belongs to - what
     *   the drawing call's announced lights are matched against, because the switch lives on the light.
     * - AN EMPTY ONE MEANS "NO MAP": the content set binds the 1x1 white stand-in and the block's switch stays off, so
     *   the engine's forward text takes its unshadowed path with the same pipeline.
     *
     * @see ShadowBlock: the packing rules, the clip conversion and the reverse-Z comparison.
     */
    struct ShadowSource {
        VkImageView     view{ VK_NULL_HANDLE };         // The map's depth view, bound at set 0 binding 7.
        bool            has_view_projection{ false };   // Whether the producer published how to read the map.
        vn::math::Mat4d view_projection{};              // Light clip <- world, in the SDK's clip convention.
        const void*     light{ nullptr };               // Whose map it is (a `Light`'s identity; nullptr = not a map).
    };

  public:
    /** @brief Builds an empty recorder; nothing exists until @ref create. */
    ContentDraw();

    /** @brief Releases every Vulkan object this recorder owns (see @ref destroy). */
    ~ContentDraw();

    ContentDraw(const ContentDraw&)            = delete;
    ContentDraw& operator=(const ContentDraw&) = delete;

  public:
    /**
     * @brief Creates the layouts, the descriptor pool and the caches over @p device.
     *
     * @details
     * - Called by the device once its logical device and memory properties exist; on failure nothing is left behind,
     *   so the caller may simply refuse to come up.
     *
     * @param table                 The device's entry points (volk table).
     * @param device                The logical device.
     * @param memory                The device's memory properties (for the host-visible memory type search).
     * @param uniform_alignment     `minUniformBufferOffsetAlignment` of the device (0 selects 256, the Vulkan minimum).
     * @return true when every layout and the pool exist.
     */
    [[nodiscard]] bool create(const VolkDeviceTable&                    table,
                              VkDevice                                  device,
                              const VkPhysicalDeviceMemoryProperties&   memory,
                              std::uint32_t                             uniform_alignment);

    /** @brief Destroys every object this recorder owns (idempotent; safe before @ref create). */
    void destroy() noexcept;

    /**
     * @brief Prepares the textures one pass' draws sample, BEFORE the pass' render pass begins.
     *
     * @details
     * - A texture upload records image barriers and buffer-to-image copies, and Vulkan forbids both inside a render
     *   pass instance (`vkCmdCopyBufferToImage` outright, a barrier without a self-dependency). The device therefore
     *   calls this once per pass, before @ref record / @ref recordScreen: the copies land on the command buffer it
     *   passes in - the frame's own, outside the pass, or the ONE transfer-only buffer a warm-up submits of its own
     *   (see RenderFrameExecutor::prepareFrame) - and the draws find the views cached.
     * - It also creates the 1x1 white fallback every absent slot is bound to, on the first recording that needs it.
     *
     * @param commands The command buffer being recorded (outside any render pass).
     * @param draws    The pass' drawing calls (borrowed for the call).
     */
    void preparePass(VkCommandBuffer commands, std::span<const vn::graphics::DrawDescription> draws);

    /**
     * @brief Resets the per-frame descriptor pool.
     *
     * @details
     * - Called once per frame, before any draw is recorded: a descriptor set is allocated per draw and the pool is
     *   what makes that cheap. The previous frame must be complete (the device submits synchronously), or the reset
     *   would free a set a pending command buffer still names.
     */
    void beginFrame();

    /** @brief Releases the per-draw uniform buffers and the retired texture uploads recorded since @ref beginFrame. */
    void endFrame() noexcept;

    /**
     * @brief Gets how many `VkPipeline` objects this recorder built over its lifetime.
     *
     * @details
     * - THE COUNTER THE RESIZE RULE IS OBSERVED WITH: a frame drawn again after an extent change must leave it where
     *   it was (the pipeline is compatible with the new framebuffer), while a shape change or a new program moves it.
     *
     * @return Pipelines created since @ref create (0 after a failure that built none).
     */
    [[nodiscard]] std::uint64_t pipelinesCreated() const noexcept;

    /**
     * @brief What the content path has built, what it let go and what it holds now.
     *
     * @details
     * - CUMULATIVE, EXCEPT THE THREE `live` FIELDS: a frame's own numbers are the difference of two cumulative ones
     *   (see FrameUploads), while residency is a question about NOW (see ResidencyLedger).
     * - `streams_live` / `stream_bytes_live` count the UPLOADED BUFFERS, not the geometries: one geometry may own a
     *   vertex stream per channel plus one index stream per primitive, and a released `Buffer` takes only the
     *   streams uploaded from it.
     * - `pipelines_live` is what the residency ledger calls the compiled halves - the pipelines ARE that kind here.
     */
    struct Progress {
        std::uint64_t stream_builds{ 0 };     ///< Uploaded vertex/index buffers, cumulative.
        std::uint64_t stream_bytes{ 0 };      ///< Bytes those buffers copied, cumulative.
        std::uint64_t stream_releases{ 0 };   ///< Stream buffers a release let go, cumulative.
        std::uint64_t program_builds{ 0 };    ///< Program revisions compiled (both stages), cumulative.
        std::uint64_t pipeline_builds{ 0 };   ///< Pipelines created, cumulative (= pipelinesCreated()).
        std::uint64_t pipeline_releases{ 0 }; ///< Pipelines destroyed by a program's release, cumulative.
        std::uint32_t streams_live{ 0 };      ///< Uploaded buffers resident NOW.
        std::uint64_t stream_bytes_live{ 0 };
        std::uint32_t pipelines_live{ 0 }; ///< Pipelines resident NOW.
    };

    /** @brief Gets the numbers (see Progress). */
    [[nodiscard]] Progress progress() const noexcept;

    /**
     * @brief Gets the sampled-image cache this recorder prepares.
     *
     * @details
     * - Exposed so one report can ask all three tables (streams, images, halves) in ONE place - the device composes
     *   the residency ledger, and it is the only object that should learn the shape of more than one table.
     *
     * @return The image cache (outlives this recorder).
     */
    [[nodiscard]] ContentImages& images() noexcept;
    /** @brief Gets the image cache for a reader that only reports on it (see the mutable overload). */
    [[nodiscard]] const ContentImages& images() const noexcept;
    /**
     * @brief Sets how wide a rasterised line may be (the device's `lineWidthRange[1]`).
     *
     * @details
     * - A pass' line-width override is a viewing switch (see PassRenderState::lineWidth) and the host may ask for
     *   more than the device allows; this is the number the recorder clamps every `vkCmdSetLineWidth` to, so the
     *   answer is the device's best instead of an invalid command. It is the SAME number the capabilities report.
     *
     * @param max_line_width The widest line the device accepts, in pixels (1 when it accepts only 1).
     */
    void setLineWidthLimit(float max_line_width) noexcept;    /**
     * @brief Drops the streams uploaded for @p description, so a later frame uploads them again.
     *
     * @details
     * - The pipelines stay: they are keyed by the program, the target's compatibility and the vertex LAYOUT - never
     *   by the geometry's identity - so a geometry coming back uploads its streams and reuses what it compiled.
     *
     * @param description The geometry's shape as the frame named it (the same value a released `Geometry` answers).
     * @return Entries dropped: 1 when a cache entry left, 0 when nothing was resident (already released).
     */
    std::size_t releaseGeometry(const vn::graphics::GeometryDescription* description);

    /**
     * @brief Drops @p description's compiled modules and every pipeline built from them.
     *
     * @param description The program's text as the frame named it.
     * @return Entries dropped: the program plus the pipelines keyed by it (0 when nothing was resident).
     */
    std::size_t releaseProgram(const vn::graphics::ShaderProgramDescription* description);

    /**
     * @brief Drops the vertex/index streams uploaded from @p buffer (its identity, not its bytes).
     *
     * @details
     * - PER STREAM, NOT PER GEOMETRY: a geometry that reads three buffers keeps the two nobody released, and the
     *   next frame re-uploads only the missing one. The geometry's entry stays (it is still live content).
     *
     * @param buffer The stream buffer's address, as `vn::Buffer` identity spells it.
     * @return Stream buffers dropped (0 when no uploaded stream reads it - already released).
     */
    std::size_t releaseStreams(const void* buffer);

    /**
     * @brief Builds what a SCENE draw's commands need, without recording a single command.
     *
     * @details
     * - THE WARM-UP HALF OF @ref record: the same program resolution, the same stream upload and the same pipeline
     *   selection (one implementation, see `planCommand`), stopping before any drawing command. Nothing is
     *   acquired, submitted or presented.
     * - A command that cannot be built is reported through @p why with @p category, exactly as @ref record reports
     *   it; the rest of the draw is still prepared.
     *
     * @param compatibility     The target's pipeline compatibility (as @ref record).
     * @param render_pass       A render pass with that compatibility (used only when a pipeline must be CREATED).
     * @param color_attachments How many colour attachments the target has.
     * @param draw              The draw to prepare (borrowed).
     * @param pass              The pass drawing it (its override and state are read; borrowed).
     * @param default_program   The program content without one of its own is drawn with, or null.
     * @param why               Receives the first reason a command could not be built (empty when all were).
     * @param category          Receives the category of that reason, or null to ignore.
     */
    void prepare(const vn::graphics::backend::RenderPassCompatibility& compatibility,
                 VkRenderPass                               render_pass,
                 std::uint32_t                              color_attachments,
                 const vn::graphics::DrawDescription&       draw,
                 const vn::graphics::PassDraw&              pass,
                 const vn::graphics::ShaderProgramDescription* default_program,
                 vn::String*                                why,
                 vn::graphics::DiagnosticCategory*          category = nullptr);

    /**
     * @brief Builds what a SCREEN draw needs (its fragment stage and the composed pipeline), recording nothing.
     *
     * @param compatibility     The destination's pipeline compatibility.
     * @param render_pass       A render pass with that compatibility (used only when a pipeline must be CREATED).
     * @param color_attachments How many colour attachments the destination has.
     * @param draw              The screen draw (its fragment program is read; borrowed).
     * @param pass              The pass drawing it (state is read; borrowed).
     * @param why               Receives the reason when the draw could not be prepared (empty when it was).
     * @param category          Receives the category of that reason, or null to ignore.
     */
    void prepareScreen(const vn::graphics::backend::RenderPassCompatibility& compatibility,
                       VkRenderPass                               render_pass,
                       std::uint32_t                              color_attachments,
                       const vn::graphics::DrawDescription&       draw,
                       const vn::graphics::PassDraw&              pass,
                       vn::String*                                why,
                       vn::graphics::DiagnosticCategory*          category = nullptr);

    /**
     * @brief Records one CONTENT draw description's commands into @p commands.
     *
     * @details
     * - Must run INSIDE the render pass that names the target, because it records drawing commands; every resource it
     *   needs is created on demand (shader modules, stream buffers, the pipeline) before the first draw of that shape.
     * - The program is resolved as `PassDraw::program_override_description`, else the command's own
     *   `programDescription()`, else @p default_program; a draw with none is skipped and reported through @p why.
     *   The viewport is the pass' rectangle, or the whole of the target when the pass states none.
     * - The STATE is resolved per command (`resolveRenderState`): topology, cull/front face, polygon mode, depth
     *   test/write/compare (reverse-Z), blend and polygon offset select or fill the pipeline, while line width, depth
     *   bias values and the viewport are dynamic; the point size travels in the draw block. A multi-attachment target
     *   never blends (its attachments hold data, not a surface - see BlendState).
     * - Each command's material values (the value the frame carries, else the live material's description) are packed
     *   into the material block; each texture slot with a complete texture is uploaded and bound, and the other slots
     *   are bound to the 1x1 white fallback with their slot bits clear. The draw's announced lights are packed into
     *   the light block in view space (see LightBlock.hpp).
     * - THE SHADOW MAP, when @p shadow names one: its depth view is bound at the content set's shadow binding and the
     *   `VineShadowBlock` is packed per call from the map's own statement (see ShadowSource and `shadowBlockOf`) - the
     *   block's switch is on only when the map's light is one this call announced AND still casts, so a map nobody can
     *   name shades nothing rather than scaling a light it does not belong to.
     *
     * @param commands        The command buffer being recorded (inside a render pass).
     * @param render_pass     The render pass @p commands is in (used only when a pipeline must be CREATED; reuse is
     *                        keyed by @p compatibility).
     * @param compatibility   The target's pipeline compatibility (see TargetImages::compatibility).
     * @param color_attachments How many colour attachments the target has (0 for a depth-only pass).
     * @param width           The target's width in pixels.
     * @param height          The target's height in pixels.
     * @param draw            The draw to record (borrowed for the call).
     * @param pass            The pass drawing it (its override, state and viewport are read; borrowed).
     * @param default_program The program content without one of its own is drawn with, or null.
     * @param shadow          The shadow map this pass samples (see ShadowSource), or an empty one for none.
     * @param why             Receives the first reason a command was skipped (empty when every command was recorded).
     * @param category        Receives the category that reason belongs to (CompileFailed for a program or pipeline
     *                        that could not be built, ContentSkipped for anything else), or null to ignore.
     * @return How many drawing calls were recorded.
     */
    std::uint64_t record(VkCommandBuffer                            commands,
                         VkRenderPass                               render_pass,
                         const vn::graphics::backend::RenderPassCompatibility& compatibility,
                         std::uint32_t                              color_attachments,
                         int                                        width,
                         int                                        height,
                         const vn::graphics::DrawDescription&       draw,
                         const vn::graphics::PassDraw&              pass,
                         const vn::graphics::ShaderProgramDescription* default_program,
                         ShadowSource                               shadow,
                         vn::String*                                why,
                         vn::graphics::DiagnosticCategory*          category = nullptr);

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
     *   ShadowSource) the REAL map and a block packed for this call are bound - a screen pass that shades is exactly
     *   where the sun's shadow has to arrive; with no map the white stand-in and the zeroed block are bound, which is
     *   the engine's own "no shadow reaches this pass".
     * - The pass' state selects the pipeline the same way a content draw's does (reverse-Z, culling, blend); a
     *   full-screen draw writes its rectangle, and a multi-attachment target never blends (the content rule).
     * - The source's images must already be in `SHADER_READ_ONLY_OPTIMAL`: the device records that transition before
     *   the destination's render pass begins, where an image barrier is unambiguously allowed.
     *
     * @param commands          The command buffer being recorded (inside the destination's render pass).
     * @param render_pass       The destination's render pass (used only when a pipeline must be CREATED).
     * @param compatibility     The destination's pipeline compatibility.
     * @param color_attachments How many colour attachments the destination has.
     * @param width             The destination's width in pixels.
     * @param height            The destination's height in pixels.
     * @param draw              The screen draw (its fragment program and camera are read; borrowed).
     * @param pass              The pass drawing it (state and viewport; borrowed).
     * @param source            The source target's colour images/views (see ScreenSource).
     * @param shadow            The map this pass resolved (see ShadowSource), or an empty one for none.
     * @param why               Receives the reason when the draw was skipped (empty when it was recorded).
     * @param category          Receives the category of that reason, or null to ignore.
     * @return 1 when the full-screen draw was recorded, 0 otherwise.
     */
    std::uint64_t recordScreen(VkCommandBuffer                            commands,
                               VkRenderPass                               render_pass,
                               const vn::graphics::backend::RenderPassCompatibility& compatibility,
                               std::uint32_t                              color_attachments,
                               int                                        width,
                               int                                        height,
                               const vn::graphics::DrawDescription&       draw,
                               const vn::graphics::PassDraw&              pass,
                               ScreenSource                               source,
                               ShadowSource                               shadow,
                               vn::String*                                why,
                               vn::graphics::DiagnosticCategory*          category = nullptr);

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
