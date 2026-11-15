#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

#include <volk.h>

#include <vine/Buffer.hpp>
#include <vine/String.hpp>
#include <vine/backend_vk/internal/ContentAbi.hpp>
#include <vine/backend_vk/internal/ContentRefusal.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/backend_vk/internal/ProgramVariant.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/MaterialBase.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/backend/Keys.hpp>
#include <vine/graphics/backend/RetirementQueue.hpp>

VN_BACKEND_VK_NS_BEGIN

class ContentImages;

/**
 * @brief What the device builds for content and keeps between frames: compiled programs, uploaded streams, pipelines,
 *        the shared layouts and the sampled-image cache.
 *
 * @details
 * - WHY THIS IS THE DEVICE'S. Everything here is keyed by an identity the host owns (a program revision, a geometry
 *   revision, a `Buffer` address) and costs real time to rebuild, so a second frame driver must FIND it rather than
 *   duplicate it: one compile per program revision, one upload per geometry revision, one pipeline per
 *   (program, variant, target shape, state, vertex layout). That is what makes "two sessions" cost two frames' worth
 *   of work instead of two assets' worth.
 * - WHAT IT IS NOT. Nothing of the frame: no ring, no descriptor pool, no per-frame counters - those are
 *   `FrameScratch`'s, one per driver, because mixing two frames' slices is a correctness problem while sharing a
 *   compiled pipeline is the point (see `.ai/design/graphics-sessions.md` §3.1.3).
 * - THE PLANNER IS HERE, NOT IN THE RECORDER. `planCommand` / `planScreen` answer what a drawing call builds with
 *   (program, streams, pipeline, resolved state) and nothing else: the warm-up (`prepare`) and the recording
 *   (`record`) run the SAME resolution, so a frame cannot ask for something the warm-up would not have built.
 * - RELEASE IS A TWO-PART ANSWER. An announced release forgets the entry NOW (so residency numbers answer the
 *   question they are asked and the next frame uploads again) while the Vulkan objects park on the device's
 *   completion watermark (`setRetirement`) until no submitted frame can still name them. Without a queue they are
 *   destroyed immediately, which is correct before `initialize()` and during `shutdown` (both sit on an idle queue).
 * - MEMORY MODEL. Streams and staging copies live in host-visible coherent memory; the residency step (M6) is what
 *   moves streams to device-local memory behind one copy.
 *
 * @warning Before step S6 (public `createSession()`) this type is driven by ONE session at a time: there is no lock
 *          yet, because there is no second reader yet. `Progress` is a snapshot, not a synchronisation point.
 *
 * @see `ContentDraw` (the recorder that consumes the answers), `FrameScratch` (the frame's own half).
 */
class VN_BACKEND_VK_API ContentCaches final {
  public:
    /// @brief One program revision's compiled modules (the description is held so the key cannot be reused).
    struct ProgramEntry {
        vn::graphics::ShaderProgramDescriptionPtr description;
        VkShaderModule                            vertex{ VK_NULL_HANDLE };
        VkShaderModule                            fragment{ VK_NULL_HANDLE };
        std::string                               vertex_entry{ "main" };
        std::string                               fragment_entry{ "main" };
        bool                                      failed{ false };
    };

    /// @brief One uploaded vertex channel.
    struct VertexStream {
        VkBuffer      buffer{ VK_NULL_HANDLE };
        MemorySlice   memory{};
        std::uint32_t location{ 0 };
        std::uint32_t components{ 0 };
        std::uint64_t bytes{ 0 };       // Bytes copied in (the entry's residency weight).
        const void*   source{ nullptr }; // The `Buffer` this stream was uploaded from (RVALUE identity only).
    };

    /// @brief One uploaded index stream (per primitive).
    struct IndexStream {
        VkBuffer      buffer{ VK_NULL_HANDLE };
        MemorySlice   memory{};
        std::uint32_t count{ 0 };
        std::uint64_t bytes{ 0 };       // Bytes copied in (the entry's residency weight).
        const void*   source{ nullptr }; // The `Buffer` this stream was uploaded from (identity only).
    };

    /// @brief One geometry revision's uploaded streams.
    ///
    /// A HOLE IS "NOT UPLOADED": a stream a host released keeps its slot with a null buffer, and the next
    /// `ensureGeometry` uploads exactly the holes - which is what makes releasing one `Buffer` cost one upload.
    struct GeometryEntry {
        vn::graphics::GeometryDescriptionPtr description;
        std::vector<VertexStream>            streams;
        std::vector<IndexStream>             indices;
        std::uint32_t                        vertex_count{ 0 };
        bool                                 failed{ false };
    };

    /// @brief One uploaded instance stream: the per-instance model matrices a command reads at locations 12-15.
    ///
    /// Keyed by the BUFFER's address (the identity the SDK releases by, see releaseContent(Buffer)), and re-uploaded
    /// when the buffer's revision moves - the bytes are announced through it, not through a geometry revision.
    struct InstanceStream {
        VkBuffer      buffer{ VK_NULL_HANDLE };
        MemorySlice   memory{};
        std::uint64_t bytes{ 0 };    // Bytes copied in (the entry's residency weight).
        std::uint64_t revision{ 0 }; // The Buffer revision the bytes came from.
    };

    /// @brief What one scene drawing call needs before any command can be recorded.
    ///
    /// @details
    /// - THE ONE RESOLUTION the recorder and the warm-up share: "what does this command draw with" is answered by
    ///   `planCommand` once, so a warm-up cannot build something the frame would not ask for (and vice versa).
    struct CommandPlan {
        ProgramEntry*                        program{ nullptr };
        GeometryEntry*                       streams{ nullptr };
        vn::graphics::MaterialDescriptionPtr material{};        // the material it is shaded with, or null (defaults)
        InstanceStream*                      instances{ nullptr }; // the instance stream it reads, or null
        ProgramVariant                       variant{};          // what the program was compiled under
        VkPipeline                           pipeline{ VK_NULL_HANDLE };
        vn::graphics::ResolvedCommandState   resolved{};
    };

    /// @brief What one screen drawing call needs before it can be recorded.
    struct ScreenPlan {
        ProgramEntry*                    fragment{ nullptr };
        VkPipeline                       pipeline{ VK_NULL_HANDLE };
        vn::graphics::ResolvedCommandState resolved{};
    };

    /**
     * @brief What the caches have built, what they let go and what they hold now.
     *
     * @details
     * - CUMULATIVE, EXCEPT THE TWO `live` FIELDS: a frame's own numbers are the difference of two cumulative ones
     *   (see FrameUploads), while residency is a question about NOW (see ResidencyLedger).
     * - `streams_live` / `stream_bytes_live` count the UPLOADED BUFFERS, not the geometries: one geometry may own a
     *   vertex stream per channel plus one index stream per primitive, and a released `Buffer` takes only the
     *   streams uploaded from it.
     * - `pipelines_live` is what the residency ledger calls the compiled halves - the pipelines ARE that kind here.
     *
     * @note The frame scratch's numbers (the ring and the descriptor sets) are NOT here: they belong to the driver,
     *       and `Device::contentCacheCounters()` composes both halves into one answer.
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

  public:
    /** @brief Builds empty caches; nothing exists until @ref create. */
    ContentCaches();

    /** @brief Releases every Vulkan object these caches own (see @ref destroy). */
    ~ContentCaches();

    ContentCaches(const ContentCaches&)            = delete;
    ContentCaches& operator=(const ContentCaches&) = delete;

  public:
    /**
     * @brief Creates the layouts, the compiled-program cache's pipeline cache and the image cache over @p device.
     *
     * @details
     * - Called by the device once its logical device and memory pool exist; on failure nothing is left behind, so the
     *   caller may simply refuse to come up. The descriptor pool and the uniform ring are NOT created here: they are
     *   the frame's (`FrameScratch::create`), which is what lets a second driver exist without a second asset cache.
     *
     * @param table               The device's entry points (volk table).
     * @param device              The logical device.
     * @param allocator           The device's memory pool (every stream is a slice of it).
     * @param uniform_alignment   `minUniformBufferOffsetAlignment` of the device (0 selects 256, the Vulkan minimum).
     * @param pipeline_cache_data A cache a previous session got out of @ref pipelineCacheBytes (empty starts cold).
     * @return true when every layout, the pipeline cache and the image cache exist.
     */
    [[nodiscard]] bool create(const VolkDeviceTable&     table,
                              VkDevice                   device,
                              MemoryAllocator&           allocator,
                              std::uint32_t              uniform_alignment,
                              std::span<const std::byte> pipeline_cache_data = {});

    /** @brief Destroys every object these caches own (idempotent; safe before @ref create). */
    void destroy() noexcept;

    /** @brief Answers whether the caches are up (a device and its entry points exist). */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief Gets the device's entry points, for a caller that records commands against the same device.
     *
     * @return The table `create` was given (all null before @ref create).
     */
    [[nodiscard]] const VolkDeviceTable& table() const noexcept;

    /**
     * @brief Hands these caches the device's park queue and completion clock, so a release can be deferred.
     *
     * @details
     * - THE DEVICE DECIDES WHEN A RELEASED OBJECT MAY GO; the caches only say what it is. A released entry leaves
     *   the table immediately, while the Vulkan objects behind it live until the device's watermark has passed the
     *   frame that could still name them - which is what makes `releaseContent` legal while another session frames.
     * - Pass nulls during teardown: a device that is going away has idled its queue and must destroy, not park.
     *
     * @param retirement The device's park queue (borrowed; null destroys immediately).
     * @param timeline   The clock the park is dated against (borrowed; null destroys immediately).
     */
    void setRetirement(vn::graphics::backend::RetirementQueue* retirement,
                       const vn::graphics::backend::FrameTimeline* timeline) noexcept;

    /**
     * @brief States whether this device answers `VK_EXT_pipeline_creation_feedback` (core in Vulkan 1.3).
     *
     * @details
     * - THE ONE OBSERVABLE THAT SAYS A CACHE HELPED: with it enabled, every `vkCreateGraphicsPipelines` call reports
     *   whether the driver served the pipeline from the cache; without it, "the cache is not helping" and "the cache is
     *   not wired" are indistinguishable from outside. It is a SETTING because the extension is the device's to offer.
     *
     * @param on Whether creation feedback is enabled for this device.
     */
    void setCreationFeedback(bool on) noexcept;

    /**
     * @brief Sets how wide a rasterised line may be (the device's `lineWidthRange[1]`).
     *
     * @details
     * - A pass' line-width override is a viewing switch (see PassRenderState::lineWidth) and the host may ask for
     *   more than the device allows; this is the number the recorder clamps every `vkCmdSetLineWidth` to.
     *
     * @param max_line_width The widest line the device accepts, in pixels (1 when it accepts only 1).
     */
    void setLineWidthLimit(float max_line_width) noexcept;

    /** @brief Gets the widest line the device accepts, in pixels (see @ref setLineWidthLimit). */
    [[nodiscard]] float lineWidthLimit() const noexcept;

    /** @brief Gets `minUniformBufferOffsetAlignment` as `create` received it (0 became 256). */
    [[nodiscard]] std::uint32_t uniformAlignment() const noexcept;

    /** @brief Gets set 0's layout of a content program (borrowed by the frame scratch, which allocates against it). */
    [[nodiscard]] VkDescriptorSetLayout contentSetLayout() const noexcept;
    /** @brief Gets set 1's layout of a content program. */
    [[nodiscard]] VkDescriptorSetLayout drawSetLayout() const noexcept;
    /** @brief Gets set 0's layout of a full-screen program. */
    [[nodiscard]] VkDescriptorSetLayout screenSetLayout() const noexcept;
    /** @brief Gets the two-set pipeline layout every content pipeline is built with. */
    [[nodiscard]] VkPipelineLayout contentPipelineLayout() const noexcept;
    /** @brief Gets the one-set pipeline layout every full-screen pipeline is built with. */
    [[nodiscard]] VkPipelineLayout screenPipelineLayout() const noexcept;

    /**
     * @brief Gets how many `VkPipeline` objects these caches built over their lifetime.
     *
     * @details
     * - THE COUNTER THE RESIZE RULE IS OBSERVED WITH: a frame drawn again after an extent change must leave it where
     *   it was (the pipeline is compatible with the new framebuffer), while a shape change or a new program moves it.
     *
     * @return Pipelines created since @ref create (0 after a failure that built none).
     */
    [[nodiscard]] std::uint64_t pipelinesCreated() const noexcept;

    /** @brief Gets how many pipelines the driver reported as served from the cache (see @ref setCreationFeedback). */
    [[nodiscard]] std::uint64_t pipelineCacheHits() const noexcept;

    /** @brief Answers whether the driver REFUSED the cache a previous session left (see @ref create). */
    [[nodiscard]] bool pipelineCachePreloadRefused() const noexcept;

    /** @brief Answers whether a previous session's cache was HANDED to the driver (`pInitialData` was set). */
    [[nodiscard]] bool pipelineCachePreloadHanded() const noexcept;

    /**
     * @brief Gets the pipeline cache's bytes as the device's driver would reload them (empty when there is no cache).
     *
     * @details
     * - THE OTHER HALF OF `create`'s `pipeline_cache_data`: what this session accumulated is read out HERE and handed
     *   to whoever persists it (the device's PipelineCacheFile). Asking a driver for its cache is not free, so this is
     *   called once, when a session ends - not per frame.
     *
     * @return The blob (`vkGetPipelineCacheData`'s bytes), or empty when the cache could not be created or is empty.
     */
    [[nodiscard]] std::vector<std::byte> pipelineCacheBytes() const;

    /** @brief Gets the numbers (see Progress). */
    [[nodiscard]] Progress progress() const noexcept;

    /** @brief Gets the sampled-image cache these caches prepare. */
    [[nodiscard]] ContentImages& images() noexcept;
    /** @brief Gets the image cache for a reader that only reports on it (see the mutable overload). */
    [[nodiscard]] const ContentImages& images() const noexcept;

    /**
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
     * @brief Prepares the textures one pass' draws sample, BEFORE the pass' render pass begins.
     *
     * @details
     * - A texture upload records image barriers and buffer-to-image copies, and Vulkan forbids both inside a render
     *   pass instance (`vkCmdCopyBufferToImage` outright, a barrier without a self-dependency). The driver therefore
     *   calls this once per pass, before `record` / `recordScreen`: the copies land on the command buffer it passes
     *   in - the frame's own, outside the pass, or the ONE transfer-only buffer a warm-up submits of its own.
     * - It also creates the 1x1 white fallback every absent slot is bound to, on the first recording that needs it.
     *
     * @param commands The command buffer being recorded (outside any render pass).
     * @param draws    The pass' drawing calls (borrowed for the call).
     */
    void preparePass(VkCommandBuffer commands, std::span<const vn::graphics::DrawDescription> draws);

    /** @brief Releases the retired texture uploads recorded since the frame opened. */
    void endFrame() noexcept;

    /**
     * @brief Resolves one scene command's program, streams and pipeline (building each on a miss).
     *
     * @param command           The drawing call to resolve.
     * @param pass              The pass drawing it (its program override and state are read).
     * @param default_program   The program content without one of its own is drawn with, or null.
     * @param compatibility     The target's pipeline compatibility.
     * @param render_pass       A render pass with that compatibility (used only when a pipeline must be CREATED).
     * @param color_attachments How many colour attachments the pass writes.
     * @param plan              Receives what was resolved (untouched when this answers false).
     * @param refusal           Receives the reason a command could not be resolved, with its code and category.
     * @return true when the command can be recorded.
     */
    [[nodiscard]] bool planCommand(const vn::graphics::RenderCommand&                     command,
                                   const vn::graphics::PassDraw&                          pass,
                                   const vn::graphics::ShaderProgramDescription*          default_program,
                                   const vn::graphics::backend::RenderPassCompatibility&  compatibility,
                                   VkRenderPass                                           render_pass,
                                   std::uint32_t                                          color_attachments,
                                   CommandPlan&                                           plan,
                                   ContentRefusal&                                        refusal);

    /**
     * @brief Resolves one screen draw's fragment program and pipeline (see @ref planCommand for the shared idea).
     *
     * @param draw              The screen draw (its fragment program is read).
     * @param pass              The pass drawing it (state is read).
     * @param compatibility     The destination's pipeline compatibility.
     * @param render_pass       A render pass with that compatibility (used only when a pipeline must be CREATED).
     * @param color_attachments How many colour attachments the destination writes.
     * @param plan              Receives what was resolved.
     * @param refusal           Receives the reason the draw could not be resolved, with its code and category.
     * @return true when the draw can be recorded.
     */
    [[nodiscard]] bool planScreen(const vn::graphics::DrawDescription&                  draw,
                                  const vn::graphics::PassDraw&                          pass,
                                  const vn::graphics::backend::RenderPassCompatibility&  compatibility,
                                  VkRenderPass                                           render_pass,
                                  std::uint32_t                                          color_attachments,
                                  ScreenPlan&                                            plan,
                                  ContentRefusal&                                        refusal);

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
