#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <memory>

#include <volk.h>

#include <vine/String.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/backend/Keys.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief M2's content path: the draws of one pass become indexed Vulkan drawing calls.
 *
 * @details
 * - WHAT IT OWNS. Compiled shader modules per program REVISION (keyed by the immutable `ShaderProgramDescription`),
 *   uploaded vertex/index streams per geometry REVISION (keyed by `GeometryDescription`), the descriptor and pipeline
 *   layouts every pipeline shares, the pipelines themselves, and one small uniform buffer per recorded draw holding
 *   the `VineViewBlock` + `VineDrawBlock` pair (std140, see ShaderAbi.hpp).
 * - A PIPELINE IS KEYED BY COMPATIBILITY, NOT BY A RENDER-PASS HANDLE. `RenderPassCompatibility` names the attachment
 *   set (formats, depth, samples, subpass), which is what Vulkan compares a pipeline against; an EXTENT change creates
 *   new images and a new framebuffer but no new render pass, so every pipeline stays valid and is REUSED. That is the
 *   observable half of M3's "a resize must not recompile" rule, pinned by `pipelinesCreated()`.
 * - WHAT IT DOES NOT OWN. Nothing of the frame: every pointer inside a `DrawDescription` is borrowed for the call, and
 *   a description it keeps as a cache key is retained by an intrusive pointer - so a later edit of the live object is
 *   a NEW description and a new key, never a silent reuse of old bytes.
 * - THE CONTENT ABI IT SERVES. This backend's own arrangement, not vsg's: set 0 binding 0 is `VineViewBlock` and set 0
 *   binding 1 is `VineDrawBlock`. A program written for this backend declares those two blocks and nothing else (M4
 *   adds the material, light and texture blocks together with the shading that reads them). The arrangement is stated
 *   here because a shader cannot read a binding nobody wrote.
 * - MEMORY MODEL. Streams and the per-draw block live in host-visible coherent memory. M2's own words are "channel ->
 *   buffer", and a staging copy would add a synchronisation story this step does not teach; the residency step (M6)
 *   is what moves both to device-local memory behind a copy.
 * - FRAME BOUNDARY. `beginFrame()` resets the descriptor pool and `endFrame()` releases the per-draw uniform buffers.
 *   The device calls both around ONE command buffer, after the queue is idle, which is what makes the reuse safe
 *   without a ring of frames.
 * - WHAT IS REPORTED, NOT DRAWN. A draw with no camera issues nothing (see `DrawDescription::camera`), one with no
 *   program is skipped and reported, and a full-screen draw is M4's. Every skip leaves the rest of the pass drawn.
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
     * @brief Resets the per-frame descriptor pool.
     *
     * @details
     * - Called once per frame, before any draw is recorded: a descriptor set is allocated per draw and the pool is
     *   what makes that cheap. The previous frame must be complete (the device submits synchronously), or the reset
     *   would free a set a pending command buffer still names.
     */
    void beginFrame();

    /** @brief Releases the per-draw uniform buffers recorded since @ref beginFrame (the queue is idle). */
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
     * @brief Records one draw description's commands into @p commands.
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

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
