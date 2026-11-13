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

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief M2-M4's content path: the draws of one pass become indexed Vulkan drawing calls.
 *
 * @details
 * - WHAT IT OWNS. Compiled shader modules per program REVISION (keyed by the immutable `ShaderProgramDescription`),
 *   uploaded vertex/index streams per geometry REVISION (keyed by `GeometryDescription`), the descriptor and pipeline
 *   layouts every pipeline shares, the pipelines themselves, and one small uniform buffer per recorded draw holding
 *   the `VineMaterialBlock`, `VineLightsBlock` and `VineDrawBlock` (std140, see ShaderAbi.hpp).
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
 * - MEMORY MODEL. Streams, the per-draw block and the texture staging copies live in host-visible coherent memory. A
 *   staging copy for geometry would add a synchronisation story this step does not teach; the residency step (M6) is
 *   what moves streams to device-local memory behind one copy.
 * - FRAME BOUNDARY. `beginFrame()` resets the descriptor pool and `endFrame()` releases the per-draw uniform buffers,
 *   the retired texture uploads and their staging buffers. The device calls both around ONE command buffer, after the
 *   queue is idle, which is what makes the reuse safe without a ring of frames.
 * - WHAT IS REPORTED, NOT DRAWN. A draw with no camera issues nothing (see `DrawDescription::camera`), one with no
 *   program is skipped and reported, and a screen draw with no source target is skipped and reported. Every skip leaves
 *   the rest of the pass drawn.
 * - WHAT IS NOT RELEASED YET. The caches are keyed by an immutable description per revision and held until
 *   `destroy()`: a geometry edited many times keeps every revision it was drawn at. Releasing content is M6's
 *   residency step (`RenderDevice::releaseContent`), and until then the cost is bounded by the edits a session made.
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
     *   calls this once per pass, before @ref record / @ref recordScreen: the copies land on the frame's command
     *   buffer outside the pass, and the draws find the views cached.
     * - It also creates the 1x1 white fallback every absent slot is bound to, on the first frame that needs it.
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
     *
     * @param commands        The command buffer being recorded (inside a render pass).
     * @param render_pass     The render pass @p commands is in (used only when a pipeline must be CREATED; reuse is
     *                        keyed by @p compatibility).
     * @param compatibility   The target's pipeline compatibility (see TargetImages::compatibility).
     * @param color_attachments How many colour attachments the target has.
     * @param width           The target's width in pixels.
     * @param height          The target's height in pixels.
     * @param draw            The draw to record (borrowed for the call).
     * @param pass            The pass drawing it (its override, state and viewport are read; borrowed).
     * @param default_program The program content without one of its own is drawn with, or null.
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
                               vn::String*                                why,
                               vn::graphics::DiagnosticCategory*          category = nullptr);

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
