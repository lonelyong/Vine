#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <map>

#include <volk.h>

#include <vine/Buffer.hpp>
#include <vine/String.hpp>
#include <vine/backend_vk/internal/ContentAbi.hpp>
#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/ContentRefusal.hpp>
#include <vine/backend_vk/internal/FrameHead.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/backend_vk/internal/ProgramVariant.hpp>
#include <vine/backend_vk/internal/ResidencyClaims.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/MaterialValues.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateResolution.hpp>
#include <vine/graphics/DrawState.hpp>
#include <vine/graphics/RetirementQueue.hpp>

VN_BACKEND_VK_NS_BEGIN

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
 * - THE PLANNER IS HERE, NOT IN THE RECORDER. `planDraw` answers what a draw packet builds with (program, streams,
 *   pipeline, resolved state, texture slots) and nothing else: the warm-up and the frame head run the SAME
 *   resolution, so a frame cannot ask for something the warm-up would not have built - and the answers CROSS the seam
 *   as values (`FrameHead`), which is what lets the recording path take no lock.
 * - ONE LOCK, ONE OWNER OF THE HOT PATH (`G10`). The tables are edited only at a frame head or by a host verb; a frame
 *   holds `Lock` while it resolves everything it will draw, and `record` / `recordFullscreen` then work from the values in
 *   the plan. The lock's acquisitions are counted (`lockAcquisitions`), which is how a test states "a frame's cache
 *   cost does not grow with its draw packets".
 * - RELEASE IS A TWO-PART ANSWER. An announced release forgets the entry NOW (so residency numbers answer the
 *   question they are asked and the next frame uploads again) while the Vulkan objects park on the device's
 *   completion watermark (`setRetirement`) until no submitted frame can still name them. Without a queue they are
 *   destroyed immediately, which is correct before `initialize()` and during `shutdown` (both sit on an idle queue).
 * - WHO MAY BUILD A GEOMETRY IS THE DEVICE'S TABLE (F4). `ensureGeometry` asks `ResidencyClaims::admit` before it
 *   uploads a stream: a key this driver may build is uploaded into the cache's entry and PUBLISHED when the frame is
 *   proven complete (`FrameBuilds::publishAll`), a key another driver is building is COPIED for this frame and thrown
 *   away with it (`FrameBuilds`), and a resident key is used. So a driver never waits for another driver's FRAME over
 *   one geometry, and the copies are bounded: at most one per competitor, only on the frame in which two drivers
 *   first draw the same thing (`.ai/design/graphics-sessions.md` §2.4, §3.1.4). The frame HEAD is still one hold of
 *   this cache's lock - the admission decides who builds, not how many drivers may hold the tables.
 * - MEMORY MODEL. Streams and staging copies live in host-visible coherent memory; the residency step (M6) is what
 *   moves streams to device-local memory behind one copy.
 *
 * @warning The lock is NOT recursive: a verb that takes `Lock&` must be called with the frame's hold, and a verb that
 *          takes its own lock (a release, `endFrame`, the progress readers) must not be called while a hold is alive.
 *          The rule is enforced by the signatures, not by a runtime check.
 *
 * @note LOCK ORDER. Where a verb holds this cache's lock and asks the residence table, the order is always
 *       cache-then-table; nothing holds the table's lock and asks this cache. `FrameBuilds::publishAll` takes only
 *       the table's, and its destructor takes this cache's without holding the table's, so the order is a property of
 *       the code and not of a comment a later edit may contradict.
 *
 * @see `ContentDraw` (the recorder that consumes the plans), `FrameScratch` (the frame's own half), `ResidencyClaims`
 *      (who may build a key), `FrameBuilds` (one frame's admissions and the copies it owns).
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
        /// The vertex-input locations the compiled vertex stage declares (see SpirvModule::input_locations).
        std::vector<std::uint32_t> vertex_locations;
        /// Whether @c vertex_locations is the module's own answer; false keeps every geometry channel declared.
        bool vertex_locations_known{ false };
        bool failed{ false };

        /**
         * @brief Answers whether the vertex stage reads @p location.
         *
         * @details
         * - UNKNOWN MEANS YES (see SpirvModule::declaresInput): a pipeline narrows its vertex input only when the
         *   stage's own answer says it may.
         *
         * @param location The geometry channel's location.
         * @return true when the stage reads it, or when that is unknown.
         */
        [[nodiscard]] bool vertexStageReads(std::uint32_t location) const
        {
            return !vertex_locations_known || std::binary_search(vertex_locations.begin(), vertex_locations.end(), location);
        }
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

    /**
     * @brief THE CACHE'S ONE LOCK, held while a frame head resolves its content or a host verb edits the caches.
     *
     * @details
     * - WHY A PROOF TYPE AND NOT A PLAIN `std::unique_lock`: every verb that reads or writes the tables takes a
     *   `Lock&`, so "I am holding the cache lock" is enforced by the compiler at the call site instead of by a
     *   comment (`G4`). The lock is acquired through @ref lock, which counts the acquisition - the witness a test uses
     *   to show that a frame's cost does not grow with its draw packets.
     * - A FRAME HOLDS IT ONCE, at its head: `planDraw` / `resolveTextures` run there, and everything
     *   the recording path needs afterwards is a value (see `FrameHead`).
     * - HOST VERBS (release, progress, `endFrame`) take it internally: they are announcements, not frame work.
     *
     * @warning Not recursive. Holding one and calling a verb that takes its own lock (for example @ref releaseGeometry)
     *          is a deadlock, so a verb that needs the caches takes `Lock&` and a verb that owns one does not.
     */
    class Lock final {
      public:
        /** @brief Takes over @p other's hold. */
        Lock(Lock&& other) noexcept = default;

        /** @brief Releases any hold this object had, then takes over @p other's. */
        Lock& operator=(Lock&& other) noexcept = default;

        Lock(const Lock&)            = delete;
        Lock& operator=(const Lock&) = delete;

        /** @brief Answers whether this object is holding the cache. */
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return held_.owns_lock();
        }

      private:
        friend class ContentCaches;

        /** @brief Builds an unheld lock: only the cache itself may hand one out (see @ref lock). */
        Lock() noexcept = default;

        explicit Lock(std::unique_lock<std::mutex> held) noexcept
          : held_(std::move(held))
        {
        }

        std::unique_lock<std::mutex> held_{};
    };

    /**
     * @brief ONE FRAME'S ADMISSIONS: the geometries it resolved, the build claims it must settle, and the temporary
     *        copies it built instead of waiting for another driver.
     *
     * @details
     * - WHY THE FRAME OWNS THIS AND NOT THE CACHE. A claim is a promise that ends at a KNOWN point - the frame's
     *   completion evidence - so its owner must be the frame; and a copy serves exactly one frame's commands and dies
     *   with it. Keeping both here is what lets the recording path work from values and still hand everything back on
     *   every exit path (`G3`): the destructor WITHDRAWS what was not published and DISPOSES the copies, so an
     *   abandoned frame leaves no key `Uploading` (`P1`) and no bytes behind.
     * - THREE ANSWERS, THREE LIFETIMES (see `ResidencyClaims::admit`). `Use` is remembered so the frame's later
     *   commands do not ask the table twice. `Build` is remembered AND its claim kept: `publishAll` publishes it when
     *   the frame completed, and a fill that FAILED never gets here - `ensureGeometry` lets the claim's own destructor
     *   return the key to `Absent`, so a refused fill can never publish itself. `Copy` is a detached entry this object
     *   owns for the rest of the frame.
     * - IT HOLDS CLAIMS, NOT POINTERS INTO THE CACHE: a host may release a geometry while this frame is in flight
     *   (R6 allows it), which moves the entry out of the cache's table. A claim is self-contained (a table, a key and
     *   a token), so publishing it after such a release is a counted `stale` answer instead of a read of freed memory.
     * - A COPY IS FRAME SCRATCH AFTER IT IS BUILT. Its streams are counted in `Progress::stream_builds` (they were
     *   really uploaded) but never in the live fields: those answer "what is resident", and a copy is not. Disposing
     *   one is not a release announcement, so `stream_releases` stays unmoved.
     * - THE PLANNER'S VERBS ARE PUBLIC because `ContentCaches::Impl` is a SIBLING nested type and the two cannot see
     *   each other's private half; the frame body is the caller of `publishAll` and of the destructor, and `entryOf`
     *   is how `ensureGeometry` answers "this frame already resolved it".
     *
     * @warning NOT thread-safe and NOT shared: one frame driver creates it, on the thread that drives that frame.
     *          The table it talks to IS shared (its own lock), which is the point. Its destructor takes the cache's
     *          lock to dispose a copy, so destroy it WITHOUT a `Lock` alive - that is where the frame body declares
     *          it, outside the head's hold.
     *
     * @note Publish on the frame's COMPLETION EVIDENCE, not when the head returns: the design's rule 2 is "not before
     *       the fill is provably complete", and this backend's evidence today is the frame's waited fence (a warm-up
     *       publishes when its transfer-only submit returns). A frame that returns without reaching that point - a
     *       refused command buffer, a failed submit - must let the destructor withdraw instead.
     *
     * @see `.ai/design/graphics-sessions.md` §2.4, §3.1.4 (F4).
     */
    class FrameBuilds final {
      public:
        /** @brief Builds a value that owns nothing and publishes nothing (a caller with no device). */
        FrameBuilds() noexcept;

        /**
         * @brief Builds the admissions of one frame of @p owner, asked in the name of @p session.
         *
         * @param owner   The caches this frame resolves against (borrowed; it outlives the frame).
         * @param session The frame driver's id (see `ResidencyClaims::FrameDriverId`).
         */
        FrameBuilds(ContentCaches& owner, ResidencyClaims::FrameDriverId session) noexcept;

        /** @brief Withdraws every unresolved claim and disposes every copy (see the class note). */
        ~FrameBuilds();

        /** @brief Takes over @p other's admissions; @p other is left empty. */
        FrameBuilds(FrameBuilds&& other) noexcept;

        /** @brief Settles what this object held, then takes over @p other's admissions. */
        FrameBuilds& operator=(FrameBuilds&& other) noexcept;

        FrameBuilds(const FrameBuilds&)            = delete;
        FrameBuilds& operator=(const FrameBuilds&) = delete;

      public:
        /** @brief Gets the id this frame asks the residence table in (see `ResidencyClaims::FrameDriverId`). */
        [[nodiscard]] ResidencyClaims::FrameDriverId session() const noexcept;

        /** @brief Gets what this frame already resolved for @p description, or null (no second table ask). */
        [[nodiscard]] GeometryEntry* entryOf(const vn::graphics::GeometryDescription* description) const noexcept;

        /** @brief Remembers a `Use` answer: @p entry is the cache's resident entry for @p description. */
        void noteUsed(const vn::graphics::GeometryDescription& description, GeometryEntry& entry);

        /**
         * @brief Remembers a `Build` answer: @p claim settles at the frame's completion evidence (see the class note).
         *
         * @param claim The table's ticket (moved in).
         */
        void noteClaim(ResidencyClaims::Claim claim);

        /**
         * @brief Remembers a `Copy` answer: takes ownership of @p copy and answers the entry this frame uses.
         *
         * @param description The identity the copy was built for (the frame's later commands look it up by this).
         * @param copy        The detached entry (its `description` member keeps the identity alive).
         * @return The entry this frame names; owned by this object until it is destroyed.
         */
        [[nodiscard]] GeometryEntry* noteCopy(const vn::graphics::GeometryDescription& description, GeometryEntry copy);

        /**
         * @brief Publishes every claim this frame holds: the fills they name are provably complete.
         *
         * @details
         * - EVERY CLAIM HERE NAMES A USABLE FILL: a build that failed never records one (its ticket returns the key to
         *   `Absent` where the failure happened), so publishing them all cannot make a broken entry `Resident`.
         * - A CLAIM WHOSE KEY A HOST RELEASED in the meantime answers `stale` instead - the documented, counted
         *   outcome, not an error (see `ResidencyClaims::Numbers::stale`).
         * - Idempotent: a published claim is empty, and the destructor's withdrawal of an empty claim does nothing.
         */
        void publishAll();

      private:
        void reset() noexcept;

        ContentCaches*                                              owner_{ nullptr };
        ResidencyClaims::FrameDriverId                               session_{ 0 };
        std::unordered_map<const vn::graphics::GeometryDescription*, GeometryEntry*> resolved_{};
        std::vector<ResidencyClaims::Claim>                          claimed_{};
        std::vector<std::unique_ptr<GeometryEntry>>                 copies_{};
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
     * @note A COMPETITOR'S TEMPORARY COPY COUNTS IN THE CUMULATIVE FIELDS ONLY: it was a real upload
     *       (`stream_builds` / `stream_bytes`), but it never enters these tables, so it is not live - and disposing it
     *       is not a release announcement, so `stream_releases` stays unmoved (see `FrameBuilds`).
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
     * @brief Hands these caches the device's park seam, so a release can be deferred.
     *
     * @details
     * - THE DEVICE DECIDES WHEN A RELEASED OBJECT MAY GO; the caches only say what it is. A released entry leaves
     *   the table immediately, while the Vulkan objects behind it live until the device's watermark has passed the
     *   frame that could still name them - which is what makes `releaseContent` legal while another session frames.
     * - Pass an empty function during teardown: a device that is going away has idled its queue and must destroy, not
     *   park.
     * - IT ALSO HANDS THE SAME SEAM TO THE IMAGE CACHE, so a released texture and a released stream are dated against
     *   the same evidence rather than two clocks that could disagree.
     *
     * @param park What to do with an object a frame may still name (empty destroys immediately).
     */
    void setRetirement(ContentImages::ParkFn park);

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
     *   it was (a pipeline is keyed by the attachment SHAPE and states no extent), while a shape change or a new
     *   program moves it.
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
    [[nodiscard]] Progress progress() const;

    /** @brief Gets the sampled-image cache's numbers (see ContentImages::Progress). */
    [[nodiscard]] ContentImages::Progress imageProgress() const;

    /// @brief Both halves of the content numbers, read under ONE hold (what a frame's books ask for).
    struct Numbers {
        Progress                content{}; ///< Streams, programs and pipelines (see @ref Progress).
        ContentImages::Progress images{};  ///< Sampled images (see ContentImages::Progress).
    };

    /**
     * @brief Gets both halves under one acquisition (see @ref Numbers).
     *
     * @return The numbers; `content` and `images` describe the same instant.
     */
    [[nodiscard]] Numbers numbers() const;

    /**
     * @brief Sets the largest anisotropy factor the device honours (see ContentImages::setMaxAnisotropy).
     *
     * @param max_anisotropy The device's `maxSamplerAnisotropy`, or 1 when the feature is not enabled.
     */
    void setMaxAnisotropy(float max_anisotropy) noexcept;

    /** @brief Gets how many times the cache lock was acquired over this device's lifetime (see @ref Lock). */
    [[nodiscard]] std::uint64_t lockAcquisitions() const noexcept;

    /**
     * @brief Gets how many of those acquisitions had to WAIT for another holder (see @ref Lock and FrameLocking).
     *
     * @details
     * - THE HALF A CONTENTION CLAIM NEEDS: `lockAcquisitions()` says how often the lock was asked for, and this says
     *   how often the asker blocked - a steady single-engine loop is where zero is expected, and a number that grows
     *   there is what a regression would show.
     *
     * @return The waits, cumulative (atomic; reading it takes no lock).
     */
    [[nodiscard]] std::uint64_t lockWaits() const noexcept;

    /**
     * @brief Gets the device's RESIDENCE TABLE: who may build a geometry, who may use it, and who has to copy it.
     *
     * @details
     * - THE DEVICE-LEVEL FACT TWO DRIVERS SHARE, exposed for the same reason `Device::targetTable()` is: a claim is
     *   about the DEVICE, not one frame, and a test that plays "the other driver" must be able to hold one (see
     *   `.ai/design/graphics-sessions.md` §3.1.4). The frame path asks it through `FrameBuilds`; nothing outside this
     *   backend is promised this shape.
     *
     * @return The table (empty before `create`, cleared by `destroy`).
     */
    [[nodiscard]] ResidencyClaims& residence() noexcept;
    /** @brief Gets the residence table for a reader that only reports on it (see the mutable overload). */
    [[nodiscard]] const ResidencyClaims& residence() const noexcept;

    /**
     * @brief Takes the cache's one lock (see @ref Lock): the frame head holds it while it resolves its content.
     *
     * @return The hold; it releases the cache when it goes out of scope.
     */
    [[nodiscard]] Lock lock() const;

    /**
     * @brief Drops the streams uploaded for @p description, so a later frame uploads them again.
     *
     * @details
     * - The pipelines stay: they are keyed by the program, the target's compatibility and the vertex LAYOUT - never
     *   by the geometry's identity - so a geometry coming back uploads its streams and reuses what it compiled.
     * - THE RESIDENCE KEY IS FORGOTTEN TOO (F4): the next frame may claim and build the key again, and a claim still
     *   in flight over it becomes stale and cannot resurrect what the host let go (see `ResidencyClaims::forget`).
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
     * @brief Prepares the textures one pass' draws sample, BEFORE the pass' rendering instance begins.
     *
     * @details
     * - A texture upload records image barriers and buffer-to-image copies, and Vulkan forbids both inside a rendering
     *   instance (`vkCmdCopyBufferToImage` outright, a barrier without a self-dependency). The WARM-UP calls this
     *   once per pass on its transfer-only batch; the frame path does the same work at its head, through
     *   @ref resolveTextures, on the frame's own command buffer.
     * - It also creates the 1x1 white fallback every absent slot is bound to, on the first preparation that needs it.
     *
     * @param commands The command buffer being recorded (outside any rendering instance).
     * @param draws    The pass' draw packets (borrowed for the call).
     * @param held     The cache lock (see @ref Lock).
     */
    void preparePass(VkCommandBuffer commands, std::span<const vn::graphics::DrawPacket> draws, const Lock& held);

    /**
     * @brief Resolves one draw packet's texture bindings into @p plan, recording every upload that is missing on
     *        @p commands; the frame-level stand-in facts land on @p head.
     *
     * @details
     * - THE FRAME HEAD'S TEXTURE HALF: a slot the material names is uploaded here (the copy is legal only outside a
     *   rendering instance, which is where the head runs), and a slot it does not name answers the 1x1 stand-in with
     *   `present == false`. A texture the cache cannot serve is reported through @p refusal and stays absent.
     * - THE FRAME-LEVEL FACTS ARE THE HEAD'S: the stand-in's view and sampler and the shadow sampler are the same for
     *   every call of one frame, so they are resolved onto @p head and the recording reads them from there. The slots
     *   are per call and stay in @p plan.
     *
     * @param material The material to resolve (its slots are read; null means "no material", everything absent).
     * @param commands The frame's command buffer (uploads are recorded before any rendering instance begins).
     * @param head     The frame's head (its stand-in view/sampler and shadow sampler are (re)filled here).
     * @param plan     Receives the slots (cleared on entry).
     * @param refusal  Receives the first reason a texture could not be served (kept when already refused).
     * @param held     The cache lock (see @ref Lock).
     * @param batch    The driver recording the uploads (see @ref ContentImages::BatchId).
     */
    void resolveTextures(const vn::graphics::MaterialDescription* material,
                         VkCommandBuffer                         commands,
                         FrameHead&                              head,
                         FrameHead::DrawPlan&                    plan,
                         ContentRefusal&                         refusal,
                         const Lock&                             held,
                         ContentImages::BatchId                  batch);

    /**
     * @brief Creates (or answers) the 1x1 white image every absent texture slot binds.
     *
     * @param commands The command buffer that records its copy (null answers whether one already exists).
     * @param held     The cache lock (see @ref Lock).
     * @param why      Receives the reason when the stand-in cannot be built, or null to ignore.
     * @param batch    The driver recording the copy (see @ref ContentImages::BatchId).
     * @return true when a fallback view is available.
     */
    [[nodiscard]] bool ensureFallback(VkCommandBuffer commands, const Lock& held, vn::String* why, ContentImages::BatchId batch);

    /** @brief Gets whether @p batch recorded a texture copy into a command buffer that has not been waited for yet. */
    [[nodiscard]] bool hasPendingUploads(ContentImages::BatchId batch) const;

    /** @brief Destroys the staging buffers @p batch recorded so far, leaving the images they fed resident. */
    void releaseUploadStaging(ContentImages::BatchId batch) noexcept;

    /**
     * @brief Drops @p texture's uploaded image (see ContentImages::releaseTexture).
     *
     * @param texture Texture whose image leaves the cache (borrowed; the host keeps the object).
     * @return true when the cache answered.
     */
    [[nodiscard]] bool releaseTexture(const vn::graphics::Texture& texture);

    /** @brief Releases the retired texture uploads recorded by @p batch (see @ref ContentImages::releaseFrame). */
    void endFrame(const ContentImages::BatchId batch) noexcept;

    /**
     * @brief Resolves ONE CONTENT COMMAND into @p plan: its program, streams, pipeline and resolved state, building
     *        each on a miss.
     *
     * @details
     * - ONE VERB, TWO INPUT SHAPES (see the screen overload): both answers are a `FrameHead::DrawPlan`, because the
     *   recording path reads one shape - the plan's `kind` says which payload is filled.
     * - THE ONE RESOLUTION the recorder and the warm-up share: "what does this command draw with" is answered here
     *   once, so a warm-up cannot build something the frame would not ask for (and vice versa).
     *
     * @param command           The command to resolve.
     * @param pass              The pass drawing it (its program override and state are read).
     * @param default_program   The program content without one of its own is drawn with, or null.
     * @param compatibility     The target's pipeline compatibility (the attachment formats and the colour count).
     * @param plan              Receives what was resolved (untouched when this answers false).
     * @param refusal           Receives the reason a command could not be resolved, with its code and category.
     * @param held              The cache lock (see @ref Lock).
     * @param builds            This frame's admissions (see @ref FrameBuilds): the geometry is admitted once per frame
     *                          and a key another driver is building is copied instead of waited for.
     * @return true when the command can be recorded.
     */
    [[nodiscard]] bool planDraw(const vn::graphics::DrawItem&                     command,
                                const vn::graphics::PassDraw&                          pass,
                                const vn::graphics::ShaderProgramDescription*          default_program,
                                const vn::graphics::RenderPassCompatibility&  compatibility,
                                FrameHead::DrawPlan&                                   plan,
                                ContentRefusal&                                        refusal,
                                const Lock&                                            held,
                                FrameBuilds&                                           builds);

    /**
     * @brief Resolves ONE SCREEN DRAW into @p plan: its fragment program, pipeline and resolved state.
     *
     * @details
     * - THE SAME VERB AS THE CONTENT OVERLOAD (one entry, two input shapes): a full-screen packet has no geometry,
     *   material or streams, so its plan fills the shared half (pipeline, resolved state) and leaves the content
     *   payload empty.
     * - THE DEPTH POLICY IS FORCED OFF for this kind (see the implementation): a full-screen triangle is at the far
     *   plane under reverse-Z, and the destination's depth is not the content's business.
     *
     * @param draw              The full-screen draw (its fragment program is read).
     * @param pass              The pass drawing it (state is read).
     * @param compatibility     The destination's pipeline compatibility (the attachment formats and the colour count).
     * @param plan              Receives what was resolved.
     * @param refusal           Receives the reason the draw could not be resolved, with its code and category.
     * @param held              The cache lock (see @ref Lock).
     * @return true when the draw can be recorded.
     */
    [[nodiscard]] bool planDraw(const vn::graphics::DrawPacket&                  draw,
                                const vn::graphics::PassDraw&                          pass,
                                const vn::graphics::RenderPassCompatibility&  compatibility,
                                FrameHead::DrawPlan&                                   plan,
                                ContentRefusal&                                        refusal,
                                const Lock&                                            held);

  private:
    struct Impl;

    /**
     * @brief Destroys the streams of the temporary copies a frame owns (see `FrameBuilds`).
     *
     * @details
     * - IMMEDIATE, NOT PARKED, AND THAT IS THE DIFFERENCE FROM `releaseGeometry`: a copy was never in the cache's
     *   tables, so no OTHER driver's frame can name it - only the frame that owns it could, and this is called when
     *   that frame is over (its fence was waited, or nothing was submitted). Parking would date the destruction
     *   against a watermark the copy does not need.
     * - Takes the cache's lock: the allocator behind it is shared with the frame heads, and this runs on the frame's
     *   thread after the frame body, when another driver's head may be uploading.
     *
     * @param copies The copies to destroy; cleared on return.
     */
    void releaseTemporary(std::vector<std::unique_ptr<GeometryEntry>>& copies) noexcept;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
