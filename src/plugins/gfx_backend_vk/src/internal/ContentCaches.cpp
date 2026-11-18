#include <vine/backend_vk/internal/ContentCaches.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/ShaderCompiler.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/DrawAbi.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/LightAbi.hpp>
#include <vine/graphics/MaterialAbi.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/ShaderAbi.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

using detail::alignedUp;
using detail::kDefaultUniformAlignment;
using detail::kShadowBlockBinding;
using detail::kShadowMapBinding;
using detail::kTextureSlots;
using detail::vertexFormatOf;

} // namespace

struct ContentCaches::Impl {
    /**
     * @brief ONE LOCK FOR THE WHOLE CACHE (`G10`): the tables are edited at a frame head or by a host verb, never on the
     *        recording path - `Lock` (see the header) is the proof a caller holds it.
     *
     * @details
     * - IT GUARDS THE TABLES, THE BOOKS, THE SHARED LAYOUTS' LIFETIME AND THE IMAGE CACHE'S TABLES. The immutable
     *   projections (`table()`, the layouts, the line width) are set by `create` and read without it: they never change
     *   while the device is up, and the recording path reads them on every draw.
     * - THE COUNTERS ARE ATOMIC SO READING THEM DOES NOT NEED THE LOCK: a test asks `lockAcquisitions()` while no frame
     *   is in flight, and the number must not change the mechanism it measures. `waits` is counted the way the frame
     *   gate counts its own (try the lock, count a failure, then block), so "a steady frame never waited" is measured
     *   identically at both places.
     */
    mutable std::mutex          lock{};
    std::atomic<std::uint64_t>  acquisitions{ 0 };
    std::atomic<std::uint64_t>  waits{ 0 };

    /// @brief The program cache's key: one TEXT (its identity and revision are the pointer) under one VARIANT.
    struct ProgramKey {
        const vn::graphics::ShaderProgramDescription* description{ nullptr };
        std::uint32_t                                 variant{ 0 }; // see ProgramVariant::bits

        /**
         * @brief Compares two keys (everything the cache distinguishes).
         *
         * @param other Key to compare against.
         * @return true when both name the same text under the same variant.
         */
        [[nodiscard]] bool operator==(const ProgramKey& other) const noexcept
        {
            return description == other.description && variant == other.variant;
        }
    };

    /// @brief The hash a program cache needs (pointer and variant, mixed once).
    struct ProgramKeyHash {
        /**
         * @brief Hashes @p key.
         *
         * @param key The key to hash.
         * @return A stable hash of the pair.
         */
        [[nodiscard]] std::size_t operator()(const ProgramKey& key) const noexcept
        {
            return std::hash<const void*>{}(key.description) ^ (static_cast<std::size_t>(key.variant) << 4U);
        }
    };

    /// @brief The pipeline-relevant half of a resolved state (dynamic state recompiles nothing, so it is not in here).
    struct PipelineState {
        VkPolygonMode   polygon_mode{ VK_POLYGON_MODE_FILL };
        VkCullModeFlags cull_mode{ VK_CULL_MODE_NONE };
        VkBool32        depth_test{ VK_FALSE };
        VkBool32        depth_write{ VK_FALSE };
        VkCompareOp     depth_compare{ VK_COMPARE_OP_ALWAYS };
        VkBool32        blend_enable{ VK_FALSE };
        VkBlendFactor   blend_src{ VK_BLEND_FACTOR_ONE };
        VkBlendFactor   blend_dst{ VK_BLEND_FACTOR_ZERO };

        /**
         * @brief Orders states so the pipeline cache is a stable map.
         *
         * @param other State to compare against.
         * @return true when this state sorts before @p other.
         */
        [[nodiscard]] bool operator<(const PipelineState& other) const
        {
            return std::tie(polygon_mode, cull_mode, depth_test, depth_write, depth_compare, blend_enable, blend_src, blend_dst) <
                   std::tie(other.polygon_mode, other.cull_mode, other.depth_test, other.depth_write, other.depth_compare, other.blend_enable,
                            other.blend_src, other.blend_dst);
        }
    };

    /// @brief What one pipeline is built for: program revision, VARIANT, target compatibility, state and the vertex layout.
    struct PipelineKey {
        /// true for a FULL-SCREEN program: `program` is then the FRAGMENT stage's description, and the geometry's
        /// channels play no part (the vertex stage is the engine's full-screen triangle and reads no buffer).
        bool                                                   screen{ false };
        const vn::graphics::ShaderProgramDescription*          program{ nullptr };
        /// The VARIANT's bits (see ProgramVariant): the same text compiled under different names is a different
        /// program, so two pipelines that differ only there are two pipelines.
        std::uint32_t                                          variant{ 0 };
        vn::graphics::RenderPassCompatibility         compatibility{};
        VkPrimitiveTopology                                    topology{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        PipelineState                                          state{};
        std::vector<std::pair<std::uint32_t, std::uint32_t>>   channels; // (location, components), in description order

        /**
         * @brief Orders keys so the cache is a stable map.
         *
         * @details
         * - The DEVICE spelling of the attachment set decides compatibility; the engine spelling is a projection of it,
         *   so two render passes that Vulkan considers compatible share one pipeline.
         *
         * @param other Key to compare against.
         * @return true when this key sorts before @p other.
         */
        [[nodiscard]] bool operator<(const PipelineKey& other) const
        {
            if (screen != other.screen) {
                return screen < other.screen;
            }
            if (program != other.program) {
                return program < other.program;
            }
            if (variant != other.variant) {
                return variant < other.variant;
            }
            if (compatibility.device_color_formats != other.compatibility.device_color_formats) {
                return std::lexicographical_compare(compatibility.device_color_formats.begin(),
                                                    compatibility.device_color_formats.end(),
                                                    other.compatibility.device_color_formats.begin(),
                                                    other.compatibility.device_color_formats.end());
            }
            if (compatibility.device_depth_format != other.compatibility.device_depth_format) {
                return compatibility.device_depth_format < other.compatibility.device_depth_format;
            }
            if (compatibility.samples != other.compatibility.samples) {
                return compatibility.samples < other.compatibility.samples;
            }
            if (compatibility.render_pass_key != other.compatibility.render_pass_key) {
                return compatibility.render_pass_key < other.compatibility.render_pass_key;
            }
            if (topology != other.topology) {
                return topology < other.topology;
            }
            if (state < other.state) {
                return true;
            }
            if (other.state < state) {
                return false;
            }
            return std::lexicographical_compare(channels.begin(), channels.end(), other.channels.begin(), other.channels.end());
        }
    };

    const VolkDeviceTable*          table{ nullptr };
    /// The table `table()` answers before `create` (and after `destroy`): no entry point is a valid answer, a null
    /// dereference is not.
    VolkDeviceTable                 empty_table{};
    VkDevice                        device{ VK_NULL_HANDLE };

    /// The widest line the device accepts, in pixels (see setLineWidthLimit): the number every `vkCmdSetLineWidth` in
    /// this recorder is clamped to, and the same one the capabilities report as `max_line_width`.
    float max_line_width{ 1.0F };
    /// The device's memory pool (borrowed: the device owns it and outlives this recorder).
    MemoryAllocator*                allocator{ nullptr };
    std::uint32_t                   uniform_alignment{ kDefaultUniformAlignment };

    /**
     * @brief What the device does with an object a frame may still name (see ContentImages::ParkFn).
     *
     * @details
     * - EMPTY DESTROYS AT ONCE, which is correct at both ends of a session: before `initialize` nothing has been
     *   submitted, and during `shutdown` the device has already idled its queue and flushed it.
     */
    ContentImages::ParkFn park{};

    /// The set layouts and pipeline layouts the two ABIs use: the content pair (set 0 material/slots/lights, set 1
    /// draw) and the full-screen set (the source's attachments at 0..N-1, lights at 6).
    VkDescriptorSetLayout content_set_layout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout draw_set_layout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout screen_set_layout{ VK_NULL_HANDLE };
    VkPipelineLayout      content_pipeline_layout{ VK_NULL_HANDLE };
    VkPipelineLayout      screen_pipeline_layout{ VK_NULL_HANDLE };
    /**
     * @brief The session's pipeline cache (see create).
     *
     * @details
     * - WHY IT EXISTS AT ALL: the recorder already keeps the pipelines it built, so this buys little WITHIN one session -
     *   what it buys is the driver's own deduplication and the place a serialised blob would go. The industry rule is
     *   "always create one"; persisting it to disk (keyed by driver and device) is the half that pays on cold start
     *   and is registered as backlog T44.
     */
    VkPipelineCache pipeline_cache{ VK_NULL_HANDLE };

    /// The engine's full-screen vertex stage (the fragment stage of a screen draw is the draw's own program).
    vn::graphics::ShaderProgramDescriptionPtr screen_vertex{};
    /// The sampled images and samplers the material slots read.
    ContentImages images{};

    std::unordered_map<ProgramKey, ProgramEntry, ProgramKeyHash>                     programs;
    std::unordered_map<const vn::graphics::GeometryDescription*, GeometryEntry>     geometries;
    /// One entry per INSTANCE buffer a command read (keyed by the buffer's address, see InstanceStream).
    std::unordered_map<const vn::Buffer<float>*, InstanceStream>                    instance_streams;
    std::map<PipelineKey, VkPipeline>                                               pipelines;
    /**
     * @brief WHO MAY BUILD a geometry: the device table two frame drivers share (F4, see `ResidenceTable`).
     *
     * @details
     * - IT AGREES WITH @c geometries AT ALL TIMES: an entry exists there while its key is `Resident` (published), and
     *   a key leaves the table when its entry does (`releaseGeometry` forgets, `destroyAll` clears). The map stays the
     *   TRUTH - a table that names a key this map does not hold is repaired, never trusted (see `ensureGeometry`).
     * - ITS OWN LOCK, SO THE ADMISSION DOES NOT NEED THIS CACHE'S: a competitor asks before it needs the tables. The
     *   order is always cache-lock-then-table-lock (see the class note).
     */
    ResidenceTable                                                                 residence;
    /// How many pipelines were CREATED (a resize must leave this unmoved; see pipelinesCreated).
    std::uint64_t created_{ 0 };

    /// M6's books (see Progress): cumulative builds and releases; the live totals are counted from the tables, so a
    /// hole a release left is never reported as resident.
    std::uint64_t stream_builds_{ 0 };
    std::uint64_t stream_bytes_{ 0 };
    std::uint64_t stream_releases_{ 0 };
    std::uint64_t program_builds_{ 0 };
    std::uint64_t pipeline_releases_{ 0 };
    /// Pipelines the driver reported as served from the cache (see setCreationFeedback): the persisted cache's witness.
    std::uint64_t cache_hits_{ 0 };
    /// Whether this device answers pipeline creation feedback (see setCreationFeedback).
    bool          creation_feedback{ false };
    /// Whether the driver refused the cache handed to it (see pipelineCachePreloadRefused).
    bool          preload_refused_{ false };
    /// Whether a cache was handed to the driver at all (see pipelineCachePreloadHanded).
    bool          preload_handed_{ false };

    /**
     * @brief Resolves the pipeline-relevant state of one drawing call (the dynamic half recompiles nothing).
     *
     * @param resolved          The state `resolveRenderState` produced.
     * @param color_attachments How many colour attachments the pass writes (a multi-attachment target never blends).
     * @return The state a pipeline is keyed by.
     */
    [[nodiscard]] PipelineState pipelineStateOf(const vn::graphics::ResolvedCommandState& resolved,
                                                std::uint32_t                             color_attachments) const noexcept
    {
        PipelineState state;
        state.polygon_mode  = StateMapping::polygonMode(resolved.state.polygonMode);
        state.cull_mode     = StateMapping::cullMode(resolved.state.cullMode);
        state.depth_test    = resolved.state.depth.test ? VK_TRUE : VK_FALSE;
        state.depth_write   = resolved.state.depth.write ? VK_TRUE : VK_FALSE;
        state.depth_compare = StateMapping::compareOp(resolved.state.depth.compare);
        // A multi-attachment target never blends: its attachments hold data, not a surface (see BlendState).
        state.blend_enable  = color_attachments == 1U && resolved.state.blend.enabled;
        state.blend_src     = StateMapping::blendFactor(resolved.state.blend.src);
        state.blend_dst     = StateMapping::blendFactor(resolved.state.blend.dst);
        return state;
    }


    [[nodiscard]] bool createHostBuffer(const VkDeviceSize size,
                                        const VkBufferUsageFlags usage,
                                        VkBuffer&                buffer,
                                        MemorySlice&             memory) const
    {
        return detail::createHostBuffer(*table, device, *allocator, size, usage, buffer, memory);
    }

    void destroyBuffer(VkBuffer& buffer, MemorySlice& memory) const noexcept
    {
        detail::destroyBuffer(*table, device, *allocator, buffer, memory);
    }

    /**
     * @brief Runs @p destroy now, or parks it with the device until no submitted frame can still name the object.
     *
     * @details
     * - THE DEVICE DECIDES *WHEN*; THIS RECORDER ONLY SAYS *WHAT*. A released entry leaves the table immediately, so
     *   the residency numbers keep answering the question they are asked ("is this content resident?"), while the
     *   Vulkan objects behind it live until the device's completion watermark has passed the frame that could still
     *   read them.
     * - WITHOUT A QUEUE IT RUNS AT ONCE, which is correct at both ends of a session: before `initialize` nothing has
     *   been submitted, and during `shutdown` the device has already idled its queue and flushed it (see
     *   Device::shutdown).
     *
     * @param destroy What to run when the object may go. Capture handles and slices BY VALUE - the closure outlives
     *                this call, and the table entry it came from is already gone.
     */
    void releaseLater(std::function<void()> destroy)
    {
        if (park) {
            park(std::move(destroy));
            return;
        }
        destroy();
    }

    /** @brief Gets how many of @p entry's stream buffers are live (the count `progress()` reports as released). */
    [[nodiscard]] static std::uint32_t liveStreams(const GeometryEntry& entry) noexcept
    {
        std::uint32_t live = 0;
        for (const VertexStream& stream : entry.streams) {
            live += stream.buffer != VK_NULL_HANDLE ? 1U : 0U;
        }
        for (const IndexStream& stream : entry.indices) {
            live += stream.buffer != VK_NULL_HANDLE ? 1U : 0U;
        }
        return live;
    }

    /**
     * @brief Creates a buffer and copies @p bytes into it (host-visible coherent memory).
     *
     * @param bytes  Bytes to upload (must be non-empty).
     * @param usage  Buffer usage flags.
     * @param buffer Receives the buffer.
     * @param memory Receives the pool range the buffer is bound into.
     * @return true when the bytes are in the buffer.
     */
    [[nodiscard]] bool upload(std::span<const std::byte> bytes, VkBufferUsageFlags usage, VkBuffer& buffer, MemorySlice& memory) const
    {
        if (bytes.empty()) {
            return false;
        }
        if (!createHostBuffer(static_cast<VkDeviceSize>(bytes.size()), usage, buffer, memory)) {
            return false;
        }
        // THE POOL'S MAPPING IS PERSISTENT (see MemoryAllocator::map): coherent memory needs no flush, so the bytes are
        // written and forgotten instead of mapped and unmapped around one memcpy.
        void* const mapped = allocator->map(memory);
        if (mapped == nullptr) {
            destroyBuffer(buffer, memory);
            return false;
        }
        std::memcpy(mapped, bytes.data(), bytes.size());
        return true;
    }

    /**
     * @brief Gets (or uploads) one instance stream: the per-instance model matrices a command reads.
     *
     * @details
     * - KEYED BY THE BUFFER'S ADDRESS, the identity the SDK releases by (see RenderDevice::releaseContent(Buffer)),
     *   and re-uploaded when the buffer's revision moves - a command's instance data is announced through the
     *   buffer, not through a geometry revision.
     * - The bytes are the WHOLE buffer: `first_instance` / `instance_count` are DRAW parameters, so one upload
     *   serves every draw that reads the same buffer.
     *
     * @param buffer The instance buffer (its floats are the matrices, four vec4 columns per instance).
     * @param why    Receives the reason when the bytes could not be uploaded.
     * @return The stream, or null when it could not be uploaded.
     */
    [[nodiscard]] InstanceStream* ensureInstanceStream(const vn::Buffer<float>& buffer, vn::String& why)
    {
        InstanceStream&      entry    = instance_streams[&buffer];
        const std::uint64_t revision = buffer.revision();
        if (entry.buffer != VK_NULL_HANDLE && entry.revision == revision) {
            return &entry;
        }
        // A NEW REVISION REPLACES THE BYTES, AND THE OLD BUFFER IS PARKED (see releaseLater): a frame that resolved the
        // previous revision may still be recording, so destroying the buffer here would pull the bytes out from under
        // a command buffer that has already named it. The plan carries the handle by value, and the park is what keeps
        // it valid until the device's watermark.
        if (entry.buffer != VK_NULL_HANDLE) {
            const VkBuffer    replaced       = entry.buffer;
            const MemorySlice replaced_memory = entry.memory;
            entry.buffer = VK_NULL_HANDLE;
            entry.memory = MemorySlice{};
            releaseLater([target = this, replaced, replaced_memory]() mutable {
                VkBuffer    buffer = replaced;
                MemorySlice memory = replaced_memory;
                target->destroyBuffer(buffer, memory);
            });
        }
        const std::span<const float> floats = buffer.view();
        if (floats.empty()) {
            why = vn::String(u8"vk: an instance stream carries no transforms");
            return nullptr;
        }
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(floats.data()), floats.size() * sizeof(float));
        if (!upload(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, entry.buffer, entry.memory)) {
            why = vn::String(u8"vk: an instance stream could not be uploaded");
            return nullptr;
        }
        entry.bytes    = bytes.size();
        entry.revision = revision;
        ++stream_builds_;
        stream_bytes_ += entry.bytes;
        return &entry;
    }

    /**
     * @brief Gets (or compiles) one program text under one VARIANT.
     *
     * @details
     * - THE TEXT IS ONE THING; WHAT IT MEANS IS THE VARIANT (see ProgramVariant). A cache that keyed only the text
     *   would answer the textured variant with the untextured compile - the exact silence the engine's
     *   `#pragma import_defines` line exists to prevent.
     *
     * @param description The program's text as a value (not null; the entry keeps it alive).
     * @param variant     The defines the text is compiled under.
     * @param why         Receives the first failure's reason.
     * @return The entry, or null when the program could not be compiled.
     */
    [[nodiscard]] ProgramEntry* ensureProgram(const vn::graphics::ShaderProgramDescription* description, const ProgramVariant& variant, vn::String& why)
    {
        const ProgramKey key{ description, variant.bits() };
        const auto       found = programs.find(key);
        if (found != programs.end()) {
            return found->second.failed ? nullptr : &found->second;
        }

        ProgramEntry entry;
        entry.description = vn::graphics::ShaderProgramDescriptionPtr(description);

        const std::vector<std::string> defines = variant.defines();
        for (const vn::graphics::ShaderStage& stage : description->stages()) {
            if (stage.type == vn::graphics::ShaderStageType::Compute) {
                continue; // M2 has no compute path
            }
            SpirvModule spirv;
            if (!compileStage(stage, defines, spirv, why)) {
                entry.failed = true;
                break;
            }

            VkShaderModuleCreateInfo info{};
            info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = spirv.words.size() * sizeof(std::uint32_t);
            info.pCode    = spirv.words.data();
            VkShaderModule module = VK_NULL_HANDLE;
            if (table->vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
                entry.failed = true;
                why          = vn::String(u8"vk: vkCreateShaderModule refused the compiled stage");
                break;
            }
            if (stage.type == vn::graphics::ShaderStageType::Vertex) {
                entry.vertex       = module;
                entry.vertex_entry = stage.entryPoint.as_std_str();
            }
            else {
                entry.fragment       = module;
                entry.fragment_entry = stage.entryPoint.as_std_str();
            }
        }

        if (entry.vertex_entry.empty()) {
            entry.vertex_entry = "main";
        }
        if (entry.fragment_entry.empty()) {
            entry.fragment_entry = "main";
        }

        const auto inserted = programs.emplace(key, std::move(entry));
        if (inserted.first->second.failed) {
            return nullptr;
        }
        ++program_builds_;
        return &inserted.first->second;
    }

    /**
     * @brief Gets (or uploads) one geometry revision's streams, ASKING THE RESIDENCE TABLE WHO MAY BUILD IT (F4).
     *
     * @details
     * - THREE ANSWERS, THREE FILLS (see `ResidenceTable::admit`). `Use` returns the resident entry. `Build` fills the
     *   cache's entry and keeps the claim for the frame to publish. `Copy` fills a DETACHED entry the caller's frame
     *   owns and throws away - another driver is building this key, and waiting is the one shape in which two drivers
     *   can deadlock each other (`P2`).
     * - A REPEAT CALL PAYS NOTHING when every stream is resident, and repairs the HOLES a host release left: a
     *   release drops the buffer of the streams it named while the entry - and its other streams - stay, so the
     *   next frame uploads exactly what left (`RenderDevice::releaseContent(Buffer)`).
     * - A REFUSED BUILD IS REMEMBERED: an entry that could not be uploaded answers null from then on, so a broken
     *   geometry is reported once instead of failing the same way every frame. Its claim is NOT published - `Resident`
     *   has to mean "usable" - so the key returns to `Absent` and the next frame may claim it again (it then finds the
     *   remembered refusal and answers null without uploading).
     * - THE FRAME ASKS ONCE per description: `builds` answers every later command of the same frame, so a frame with
     *   a thousand draws of one geometry costs one admission, not a thousand.
     *
     * @param description The geometry's shape as a value (the entry keeps it alive).
     * @param builds      This frame's admissions (see `FrameBuilds`).
     * @param why         Receives the first failure's reason.
     * @return The entry this frame names (the cache's, or this frame's temporary copy), or null when a stream could not
     *         be uploaded.
     */
    [[nodiscard]] GeometryEntry* ensureGeometry(const vn::graphics::GeometryDescription& description, FrameBuilds& builds, vn::String& why)
    {
        if (GeometryEntry* const known = builds.entryOf(&description)) {
            return known;
        }

        ResidenceTable::Decision decision = residence.admit(&description, builds.session());
        if (decision.kind == ResidenceTable::Decision::Kind::Use) {
            const auto found = geometries.find(&description);
            if (found != geometries.end() && !found->second.failed) {
                // A RELEASED STREAM LEAVES A HOLE IN A RESIDENT ENTRY, and filling it is this path's job: the release
                // names one `Buffer`, and the next frame that needs the geometry uploads exactly what left (see
                // `releaseStreams`). A complete entry pays nothing here.
                GeometryEntry& entry = found->second;
                if (!fillStreams(entry, description, why)) {
                    return failGeometry(entry, std::move(why));
                }
                builds.noteUsed(description, entry);
                return &entry;
            }
            // THE TABLE AND THE TABLES DISAGREE: a key the table called usable that this cache does not hold cannot be
            // used, and refusing the draw would charge the host for a bookkeeping slip. The cache is the truth, so
            // the key is forgotten and asked again - one turn, which answers Build and builds it.
            (void)residence.forget(&description);
            return ensureGeometry(description, builds, why);
        }

        if (decision.kind == ResidenceTable::Decision::Kind::Build) {
            const auto found = geometries.find(&description);
            if (found != geometries.end() && found->second.failed) {
                // THE REFUSAL WAS REPORTED WHEN IT HAPPENED: the claim is left to `decision`, whose destructor returns
                // the key to Absent, so the answer is "broken" without pretending the key is resident.
                return nullptr;
            }
            GeometryEntry* const entry = found != geometries.end()
                                             ? &found->second
                                             : &geometries.emplace(&description, shapedFor(description)).first->second;
            // THE FILL IS THE CLAIM'S. A failure here leaves `decision` owning the ticket, so the key returns to
            // Absent at once - the next frame may try again instead of finding it Uploading forever (`P1`, `R4`).
            if (!fillStreams(*entry, description, why)) {
                return failGeometry(*entry, std::move(why));
            }
            builds.noteUsed(description, *entry);
            builds.noteClaim(std::move(decision.claim));
            return entry;
        }

        // COPY: another driver is building this key, so this frame gets its own streams and keeps none of them (see
        // the class note and `FrameBuilds`). The copy is NOT inserted into the tables: a second entry under one key
        // would be a second thing every later frame could find.
        GeometryEntry copy = shapedFor(description);
        if (!fillStreams(copy, description, why)) {
            return failGeometry(copy, std::move(why));
        }
        return builds.noteCopy(description, std::move(copy));
    }

    /**
     * @brief Builds one entry's SLOT SHAPE: a stream per non-empty channel (binding order) and a slot per primitive.
     *
     * @param description The geometry's shape.
     * @return The entry with every stream still missing (a null buffer) and its description held.
     */
    [[nodiscard]] static GeometryEntry shapedFor(const vn::graphics::GeometryDescription& description)
    {
        GeometryEntry fresh;
        fresh.description = vn::graphics::GeometryDescriptionPtr(&description);
        // One slot per NON-EMPTY channel, in channel order: that is the vertex-input binding order the pipeline
        // states, so a hole and its binding stay aligned.
        for (const vn::graphics::GeometryChannel& channel : description.channels()) {
            if (!channel.channel.empty()) {
                fresh.streams.emplace_back();
            }
        }
        fresh.indices.resize(description.primitives().size());
        return fresh;
    }

    /**
     * @brief Uploads every stream of @p entry that is missing, and computes the entry's vertex count.
     *
     * @details
     * - A FRESH ENTRY HAS EVERY STREAM MISSING; an entry a release punched holes in uploads exactly those, which is
     *   what makes releasing one `Buffer` cost one upload.
     * - A FAILED FILL IS THE CALLER'S TO SETTLE (`failGeometry`): this only says which stream failed and why.
     *
     * @param entry       The entry to fill (its slot shape must already match @p description).
     * @param description The geometry's shape.
     * @param why         Receives the first failure's reason.
     * @return true when every named stream is resident.
     */
    [[nodiscard]] bool fillStreams(GeometryEntry& entry, const vn::graphics::GeometryDescription& description, vn::String& why)
    {
        std::size_t slot = 0;
        for (const vn::graphics::GeometryChannel& channel : description.channels()) {
            const std::span<const float> scalars = channel.channel.scalars();
            if (scalars.empty()) {
                continue;
            }
            VertexStream& stream = entry.streams[slot];
            if (stream.buffer == VK_NULL_HANDLE) {
                const std::uint32_t components = channel.channel.components;
                if (vertexFormatOf(components) == VK_FORMAT_UNDEFINED) {
                    why = vn::String(u8"vk: an attribute channel states a component count Vulkan has no format for");
                    return false;
                }
                stream.location   = channel.location;
                stream.components = components;
                stream.source     = channel.channel.values.get();
                if (!upload(std::as_bytes(scalars), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, stream.buffer, stream.memory)) {
                    why = vn::String(u8"vk: a vertex channel could not be uploaded");
                    return false;
                }
                stream.bytes = static_cast<std::uint64_t>(scalars.size_bytes());
                ++stream_builds_;
                stream_bytes_ += stream.bytes;
            }
            ++slot;
        }

        // The draw count follows the SMALLEST stream: a vertex index that is in range for one attribute and out of
        // range for another is undefined behaviour, and the smallest stream is the only count all of them serve.
        std::uint32_t smallest = std::numeric_limits<std::uint32_t>::max();
        for (const vn::graphics::GeometryChannel& channel : description.channels()) {
            if (channel.channel.empty()) {
                continue;
            }
            smallest = std::min(smallest, static_cast<std::uint32_t>(channel.channel.vertexCount()));
        }
        entry.vertex_count = smallest == std::numeric_limits<std::uint32_t>::max() ? 0U : smallest;

        const std::span<const vn::graphics::Primitive> primitives = description.primitives();
        for (std::size_t index = 0; index < primitives.size(); ++index) {
            const std::span<const std::uint32_t> indices = primitives[index].indices.span();
            if (indices.empty()) {
                continue;
            }
            IndexStream& stream = entry.indices[index];
            if (stream.buffer != VK_NULL_HANDLE) {
                continue;
            }
            stream.source = primitives[index].indices.values.get();
            if (!upload(std::as_bytes(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, stream.buffer, stream.memory)) {
                why = vn::String(u8"vk: an index stream could not be uploaded");
                return false;
            }
            stream.count = static_cast<std::uint32_t>(indices.size());
            stream.bytes = static_cast<std::uint64_t>(indices.size_bytes());
            ++stream_builds_;
            stream_bytes_ += stream.bytes;
        }
        return true;
    }

    /**
     * @brief Marks one geometry entry refused and destroys every stream it had.
     *
     * @param entry The entry that could not be completed.
     * @param why   The reason, passed through to the caller.
     * @return Always null, so a caller can write `return failGeometry(...)`.
     */
    GeometryEntry* failGeometry(GeometryEntry& entry, vn::String why)
    {
        entry.failed = true;
        // THE STREAMS ARE PARKED, NOT DESTROYED: an entry that failed halfway may already have been resolved by a
        // frame that is still recording (see releaseLater), so the buffers it would name live until the watermark.
        std::vector<VkBuffer>    buffers;
        std::vector<MemorySlice> memories;
        for (VertexStream& stream : entry.streams) {
            if (stream.buffer != VK_NULL_HANDLE) {
                buffers.push_back(stream.buffer);
                memories.push_back(stream.memory);
                stream.buffer = VK_NULL_HANDLE;
                stream.memory = MemorySlice{};
            }
        }
        for (IndexStream& stream : entry.indices) {
            if (stream.buffer != VK_NULL_HANDLE) {
                buffers.push_back(stream.buffer);
                memories.push_back(stream.memory);
                stream.buffer = VK_NULL_HANDLE;
                stream.memory = MemorySlice{};
            }
        }
        stream_releases_ += static_cast<std::uint32_t>(buffers.size());
        if (!buffers.empty()) {
            releaseLater([target = this, buffers = std::move(buffers), memories = std::move(memories)]() mutable {
                for (std::size_t index = 0; index < buffers.size(); ++index) {
                    target->destroyBuffer(buffers[index], memories[index]);
                }
            });
        }
        return nullptr;
    }

    /**
     * @brief Destroys every stream of @p entry (the entry itself stays) and answers how many buffers it let go.
     *
     * @note THE CALLER COUNTS: a teardown is not a release, so only the paths that *are* announcements add the number
     *       to `stream_releases_` (see releaseGeometry / failGeometry).
     *
     * @param entry The entry whose buffers go.
     * @return Stream buffers destroyed.
     */
    [[nodiscard]] std::uint32_t destroyGeometryStreams(GeometryEntry& entry) noexcept
    {
        std::uint32_t destroyed = 0;
        for (VertexStream& stream : entry.streams) {
            if (stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            destroyBuffer(stream.buffer, stream.memory);
            ++destroyed;
        }
        for (IndexStream& stream : entry.indices) {
            if (stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            destroyBuffer(stream.buffer, stream.memory);
            ++destroyed;
        }
        return destroyed;
    }

    /**
     * @brief Gets (or builds) one graphics pipeline from a fully stated key and its shader/vertex-input data.
     *
     * @param key               The cache key (program revision, compatibility, topology, state and vertex layout).
     * @param vertex            The compiled vertex module.
     * @param vertex_entry      The vertex stage's entry point.
     * @param fragment          The compiled fragment module.
     * @param fragment_entry    The fragment stage's entry point.
     * @param bindings          The vertex input bindings (empty for the full-screen triangle).
     * @param attributes        The vertex input attributes (empty for the full-screen triangle).
     * @param layout            The pipeline layout the program's descriptor sets agree with.
     * @param render_pass       A render pass with the key's compatibility (used only when the pipeline is CREATED).
     * @param topology          The pipeline's primitive topology.
     * @param state             The resolved pipeline state (dynamic state is not part of it).
     * @param color_attachments How many colour attachments the pass writes.
     * @param why               Receives the reason when the pipeline could not be built.
     * @return The pipeline, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkPipeline buildPipeline(PipelineKey                                          key,
                                           VkShaderModule                                       vertex,
                                           const std::string&                                   vertex_entry,
                                           VkShaderModule                                       fragment,
                                           const std::string&                                   fragment_entry,
                                           const std::vector<VkVertexInputBindingDescription>&  bindings,
                                           const std::vector<VkVertexInputAttributeDescription>& attributes,
                                           VkPipelineLayout                                     layout,
                                           VkRenderPass                                         render_pass,
                                           VkPrimitiveTopology                                  topology,
                                           const PipelineState&                                 state,
                                           std::uint32_t                                        color_attachments,
                                           vn::String&                                          why)
    {
        const auto found = pipelines.find(key);
        if (found != pipelines.end()) {
            return found->second;
        }

        std::vector<VkVertexInputBindingDescription>   input_bindings = bindings;
        std::vector<VkVertexInputAttributeDescription> input_attributes = attributes;

        // WHAT THE PIPELINE STATES. The resolved state selects the pipeline variant; values that are DYNAMIC state
        // (viewport, scissor, line width, depth-bias values) are set per draw and never recompile anything.
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName  = vertex_entry.c_str();
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName  = fragment_entry.c_str();

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount   = static_cast<std::uint32_t>(input_bindings.size());
        vertex_input.pVertexBindingDescriptions      = input_bindings.empty() ? nullptr : input_bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(input_attributes.size());
        vertex_input.pVertexAttributeDescriptions    = input_attributes.empty() ? nullptr : input_attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = key.topology;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1U;
        viewport.scissorCount  = 1U;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode             = state.polygon_mode;
        raster.cullMode                = state.cull_mode;
        raster.frontFace               = StateMapping::kFrontFace;
        raster.depthBiasEnable         = VK_TRUE; // the bias VALUES are dynamic (vkCmdSetDepthBias)
        raster.depthBiasConstantFactor = 0.0F;
        raster.depthBiasSlopeFactor    = 0.0F;
        raster.lineWidth               = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        // THE SAMPLE COUNT COMES FROM THE TARGET'S SHAPE, not from this file: a pipeline's rasterisation sample count
        // must equal the subpass' colour attachment count, and `compatibility` is the shape half of the key - so the
        // same value the render pass was built with reaches the pipeline. The key compares it, which is why a
        // single-sample and a multisampled target never share one compiled pipeline.
        multisample.rasterizationSamples = StateMapping::sampleCount(key.compatibility.samples);

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable  = state.depth_test;
        depth.depthWriteEnable = state.depth_write;
        depth.depthCompareOp   = state.depth_compare;

        // EVERY COLOUR ATTACHMENT GETS THE SAME BLEND: one object's fragment writes them all, and an MRT target states
        // blendEnable FALSE (its attachments hold data, not a surface - see BlendState).
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments(color_attachments);
        for (VkPipelineColorBlendAttachmentState& attachment : blend_attachments) {
            attachment.blendEnable         = state.blend_enable;
            attachment.srcColorBlendFactor = state.blend_src;
            attachment.dstColorBlendFactor = state.blend_dst;
            attachment.colorBlendOp        = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = state.blend_src;
            attachment.dstAlphaBlendFactor = state.blend_dst;
            attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
            attachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = static_cast<std::uint32_t>(blend_attachments.size());
        // A DEPTH-ONLY PASS has no attachment to blend into; the array is no more valid than an empty one (the fragment's
        // writes are discarded, which is what a shadow pass states by writing no colour).
        blend.pAttachments    = blend_attachments.empty() ? nullptr : blend_attachments.data();

        const VkDynamicState dynamic_states[4]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 4U;
        dynamic.pDynamicStates    = dynamic_states;

        VkGraphicsPipelineCreateInfo info{};
        info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount          = 2U;
        info.pStages             = stages;
        info.pVertexInputState   = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState      = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisample;
        info.pDepthStencilState  = &depth;
        info.pColorBlendState    = &blend;
        info.pDynamicState       = &dynamic;
        info.layout              = layout;
        info.renderPass          = render_pass;
        info.subpass             = 0U;

        // THE CACHE'S ONLY WITNESS (see setCreationFeedback): with feedback enabled the driver answers, per creation,
        // whether it served this pipeline out of the cache. The structure is chained when the device offers it; a
        // device without it builds exactly the same pipelines and reports no hit at all.
        VkPipelineCreationFeedback         overall_feedback{};
        VkPipelineCreationFeedbackCreateInfo feedback_info{};
        if (creation_feedback) {
            feedback_info.sType                            = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO;
            feedback_info.pPipelineCreationFeedback        = &overall_feedback;
            feedback_info.pipelineStageCreationFeedbackCount = 0U;
            info.pNext                                     = &feedback_info;
        }

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (table->vkCreateGraphicsPipelines(device, pipeline_cache, 1U, &info, nullptr, &pipeline) != VK_SUCCESS) {
            why = vn::String(u8"vk: the content pipeline could not be built (see the loader's report)");
            return VK_NULL_HANDLE;
        }
        ++created_;
        if ((overall_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0U) {
            ++cache_hits_;
        }
        pipelines.emplace(std::move(key), pipeline);
        return pipeline;
    }

    /**
     * @brief Gets (or builds) the pipeline of a CONTENT draw.
     *
     * @details
     * - The vertex layout is the geometry's channel list: one binding per channel, at the channel's location and with
     *   its component count - the same arrangement `ensureGeometry` uploaded.
     *
     * @param program           The compiled program (both stages required).
     * @param geometry          The geometry whose channels state the vertex layout.
     * @param variant           The variant the program was compiled under (see ProgramVariant).
     * @param instanced         Whether the command reads an instance stream (one more binding at 12-15).
     * @param compatibility     The target's pipeline compatibility (the key's shape half).
     * @param render_pass       A render pass with that compatibility (used only when the pipeline is CREATED).
     * @param topology          The draw's effective topology.
     * @param state             The resolved pipeline state (dynamic state is not part of it).
     * @param color_attachments How many colour attachments the pass writes.
     * @param why               Receives the reason when the pipeline could not be built.
     * @return The pipeline, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkPipeline ensureContentPipeline(const ProgramEntry&                                   program,
                                                   const vn::graphics::GeometryDescription&              geometry,
                                                   const ProgramVariant&                                 variant,
                                                   bool                                                  instanced,
                                                   const vn::graphics::RenderPassCompatibility& compatibility,
                                                   VkRenderPass                                          render_pass,
                                                   VkPrimitiveTopology                                   topology,
                                                   const PipelineState&                                  state,
                                                   std::uint32_t                                         color_attachments,
                                                   vn::String&                                           why)
    {
        if (program.vertex == VK_NULL_HANDLE || program.fragment == VK_NULL_HANDLE) {
            why = vn::String(u8"vk: a content program needs a vertex and a fragment stage");
            return VK_NULL_HANDLE;
        }

        PipelineKey key;
        key.program       = program.description.get();
        key.variant       = variant.bits();
        key.compatibility = compatibility;
        key.topology      = topology;
        key.state         = state;
        for (const vn::graphics::GeometryChannel& channel : geometry.channels()) {
            if (channel.channel.empty()) {
                continue;
            }
            key.channels.emplace_back(channel.location, channel.channel.components);
        }
        const std::size_t geometry_channels = key.channels.size();
        if (instanced) {
            // THE INSTANCE MATRIX IS FOUR vec4 COLUMNS at locations 12-15 (see ShaderAbi's InstanceModel), supplied by
            // ONE binding whose stride is the matrix - the same shape the engine's vertex stage declares under
            // `VN_INSTANCED`. Putting the entries in the key's channel list is what makes two layouts different keys.
            const std::uint32_t first = vn::graphics::attributeLocation(vn::graphics::VertexAttribute::InstanceModel);
            for (std::uint32_t column = 0; column < vn::graphics::kInstanceMatrixColumns; ++column) {
                key.channels.emplace_back(first + column, 4U);
            }
        }

        std::vector<VkVertexInputBindingDescription>   bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        bindings.reserve(geometry_channels + 1U);
        attributes.reserve(key.channels.size());
        for (std::size_t index = 0; index < geometry_channels; ++index) {
            const std::uint32_t binding    = static_cast<std::uint32_t>(index);
            const std::uint32_t location   = key.channels[index].first;
            const std::uint32_t components = key.channels[index].second;
            VkVertexInputBindingDescription binding_description{};
            binding_description.binding   = binding;
            binding_description.stride    = components * static_cast<std::uint32_t>(sizeof(float));
            binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            bindings.push_back(binding_description);

            VkVertexInputAttributeDescription attribute{};
            attribute.location = location;
            attribute.binding  = binding;
            attribute.format   = vertexFormatOf(components);
            attribute.offset   = 0U;
            attributes.push_back(attribute);
        }
        if (instanced) {
            VkVertexInputBindingDescription binding_description{};
            binding_description.binding   = static_cast<std::uint32_t>(geometry_channels);
            binding_description.stride    = vn::graphics::kInstanceMatrixColumns * 4U * static_cast<std::uint32_t>(sizeof(float));
            binding_description.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            bindings.push_back(binding_description);
            for (std::uint32_t column = 0; column < vn::graphics::kInstanceMatrixColumns; ++column) {
                VkVertexInputAttributeDescription attribute{};
                attribute.location = key.channels[geometry_channels + column].first;
                attribute.binding  = binding_description.binding;
                attribute.format   = vertexFormatOf(4U);
                attribute.offset   = column * 4U * static_cast<std::uint32_t>(sizeof(float));
                attributes.push_back(attribute);
            }
        }

        return buildPipeline(std::move(key),
                             program.vertex,
                             program.vertex_entry,
                             program.fragment,
                             program.fragment_entry,
                             bindings,
                             attributes,
                             content_pipeline_layout,
                             render_pass,
                             topology,
                             state,
                             color_attachments,
                             why);
    }

    /**
     * @brief Gets (or builds) the pipeline of a SCREEN draw: the engine's full-screen vertex stage + the draw's
     *        fragment stage, with no vertex input.
     *
     * @param fragment          The compiled FRAGMENT program (its description is the key's program half).
     * @param vertex            The compiled full-screen vertex stage (see `screen_vertex`).
     * @param compatibility     The destination's pipeline compatibility.
     * @param render_pass       A render pass with that compatibility (used only when the pipeline is CREATED).
     * @param state             The resolved pipeline state (dynamic state is not part of it).
     * @param color_attachments How many colour attachments the destination writes.
     * @param why               Receives the reason when the pipeline could not be built.
     * @return The pipeline, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkPipeline ensureScreenPipeline(const ProgramEntry&                                   fragment,
                                                  const ProgramEntry&                                   vertex,
                                                  const vn::graphics::RenderPassCompatibility& compatibility,
                                                  VkRenderPass                                          render_pass,
                                                  const PipelineState&                                  state,
                                                  std::uint32_t                                         color_attachments,
                                                  vn::String&                                           why)
    {
        if (vertex.vertex == VK_NULL_HANDLE) {
            why = vn::String(u8"vk: the full-screen vertex stage could not be compiled");
            return VK_NULL_HANDLE;
        }
        if (fragment.fragment == VK_NULL_HANDLE) {
            why = vn::String(u8"vk: a screen program needs a fragment stage");
            return VK_NULL_HANDLE;
        }

        PipelineKey key;
        key.screen        = true;
        key.program       = fragment.description.get();
        key.compatibility = compatibility;
        key.topology      = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        key.state         = state;

        return buildPipeline(std::move(key),
                             vertex.vertex,
                             vertex.vertex_entry,
                             fragment.fragment,
                             fragment.fragment_entry,
                             {},
                             {},
                             screen_pipeline_layout,
                             render_pass,
                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                             state,
                             color_attachments,
                             why);
    }

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
     * @param builds            This frame's admissions (see FrameBuilds).
     * @return true when the command can be recorded.
     */
    [[nodiscard]] bool planDraw(const vn::graphics::RenderCommand&                     command,
                                const vn::graphics::PassDraw&                          pass,
                                const vn::graphics::ShaderProgramDescription*          default_program,
                                const vn::graphics::RenderPassCompatibility&  compatibility,
                                VkRenderPass                                           render_pass,
                                std::uint32_t                                          color_attachments,
                                FrameHead::DrawPlan&                                   plan,
                                ContentRefusal&                                        refusal,
                                FrameBuilds&                                           builds)
    {
        // A FAILURE LEAVES AN EMPTY PLAN (`pipeline == VK_NULL_HANDLE` is what the recorder skips) - via `clear()`, NOT
        // a fresh DrawPlan: the plan's stream vector lives in the frame head's reused storage, and assigning over it
        // freed and reallocated that storage on every command of every frame (the exact defect the head's file note
        // promises cannot happen).
        plan.clear();
        const vn::graphics::ShaderProgramDescriptionPtr program =
            pass.program_override_description != nullptr ? pass.program_override_description : command.programDescription();
        const vn::graphics::ShaderProgramDescription* program_description = program != nullptr ? program.get() : default_program;
        if (program_description == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                           contentRefusalCode(ContentRefusalCause::NoProgram),
                           vn::String(u8"vk: a content draw named no program and the device has no default content program"));
            return false;
        }

        const vn::graphics::GeometryDescriptionPtr geometry = command.geometryDescription();
        if (geometry == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                           contentRefusalCode(ContentRefusalCause::NoGeometry),
                           vn::String(u8"vk: a content draw carried no geometry description"));
            return false;
        }
        const std::span<const vn::graphics::Primitive> primitives = geometry->primitives();
        if (command.primitive_index >= primitives.size()) {
            refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                           contentRefusalCode(ContentRefusalCause::PrimitiveAbsent),
                           vn::String(u8"vk: a content draw named a primitive its geometry does not have"));
            return false;
        }

        // THE MATERIAL AND THE VARIANT, before anything is compiled: the texture slots select `VN_TEXTURED` and the
        // geometry's channels select the colour and texcoord names (see ProgramVariant::variantOf). Resolved HERE so
        // a warm-up and the frame that follows it cannot compile different programs for one drawable (G1).
        vn::graphics::MaterialDescriptionPtr material = command.material_description;
        if (material == nullptr && command.material != nullptr) {
            material = command.material->description();
        }
        const bool           instanced = command.instance_transforms.values != nullptr && command.instance_count != 0U;
        const ProgramVariant variant   = variantOf(*geometry, material.get(), instanced);

        vn::String          build_why;
        ProgramEntry* const compiled = ensureProgram(program_description, variant, build_why);
        if (compiled == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::CompileFailed,
                           contentRefusalCode(ContentRefusalCause::ProgramRefused),
                           build_why.empty() ? vn::String(u8"vk: the content program could not be compiled") : std::move(build_why));
            return false;
        }
        GeometryEntry* const streams = ensureGeometry(*geometry, builds, build_why);
        if (streams == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                           contentRefusalCode(ContentRefusalCause::GeometryRefused),
                           build_why.empty() ? vn::String(u8"vk: the geometry's streams could not be uploaded") : std::move(build_why));
            return false;
        }
        InstanceStream* instances = nullptr;
        if (instanced) {
            instances = ensureInstanceStream(*command.instance_transforms.values, build_why);
            if (instances == nullptr) {
                refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                               contentRefusalCode(ContentRefusalCause::InstanceStreamRefused),
                               build_why.empty() ? vn::String(u8"vk: the instance stream could not be uploaded") : std::move(build_why));
                return false;
            }
        }

        // THE STATE, RESOLVED ONCE PER COMMAND (pass override > content > defaults; see StateResolution.hpp). The
        // pipeline-relevant half selects the variant; the dynamic half is set when the draw is recorded.
        plan.resolved = vn::graphics::resolveRenderState(command.authored_state,
                                                         pass.state,
                                                         primitives[command.primitive_index].topology);
        vn::String pipeline_why;
        plan.pipeline = ensureContentPipeline(*compiled,
                                              *geometry,
                                              variant,
                                              instanced,
                                              compatibility,
                                              render_pass,
                                              StateMapping::topology(plan.resolved.state.topology),
                                              pipelineStateOf(plan.resolved, color_attachments),
                                              color_attachments,
                                              pipeline_why);
        if (plan.pipeline == VK_NULL_HANDLE) {
            refusal.refuse(vn::graphics::DiagnosticCategory::CompileFailed,
                           contentRefusalCode(ContentRefusalCause::PipelineRefused),
                           pipeline_why.empty() ? vn::String(u8"vk: the content pipeline could not be built") : std::move(pipeline_why));
            return false;
        }
        plan.material    = std::move(material);
        plan.variant     = variant;
        plan.vertex_count = streams->vertex_count;
        plan.vertex_streams.reserve(streams->streams.size());
        for (const VertexStream& stream : streams->streams) {
            plan.vertex_streams.push_back(stream.buffer);
        }
        plan.instance_buffer = instances != nullptr ? instances->buffer : VK_NULL_HANDLE;
        if (command.primitive_index < streams->indices.size()) {
            plan.index_buffer = streams->indices[command.primitive_index].buffer;
            plan.index_count  = streams->indices[command.primitive_index].count;
        }
        return true;
    }

    /**
     * @brief Resolves one screen draw's fragment program and pipeline (see @ref planDraw for the shared idea).
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
    [[nodiscard]] bool planDraw(const vn::graphics::DrawDescription&                  draw,
                                const vn::graphics::PassDraw&                          pass,
                                const vn::graphics::RenderPassCompatibility&  compatibility,
                                VkRenderPass                                           render_pass,
                                std::uint32_t                                          color_attachments,
                                FrameHead::DrawPlan&                                   plan,
                                ContentRefusal&                                        refusal)
    {
        plan      = FrameHead::DrawPlan{}; // see planDraw(RenderCommand): an empty plan is "this draw records nothing"
        plan.kind = FrameHead::Kind::Screen;
        // THE PROGRAM'S TEXT: the value the frame carries, else the live program's cached description.
        vn::graphics::ShaderProgramDescriptionPtr fragment_description = draw.program_description;
        if (fragment_description == nullptr && draw.program != nullptr) {
            fragment_description = draw.program->description();
        }
        if (fragment_description == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                           contentRefusalCode(ContentRefusalCause::ScreenNoProgram),
                           vn::String(u8"vk: a screen draw named no fragment program"));
            return false;
        }

        vn::String                build_why;
        ProgramEntry* const       fragment = ensureProgram(fragment_description.get(), ProgramVariant{}, build_why);
        if (fragment == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::CompileFailed,
                           contentRefusalCode(ContentRefusalCause::ScreenProgramRefused),
                           build_why.empty() ? vn::String(u8"vk: the screen fragment program could not be compiled") : std::move(build_why));
            return false;
        }
        ProgramEntry* const vertex = ensureProgram(screen_vertex.get(), ProgramVariant{}, build_why);
        if (vertex == nullptr) {
            refusal.refuse(vn::graphics::DiagnosticCategory::CompileFailed,
                           contentRefusalCode(ContentRefusalCause::ScreenProgramRefused),
                           build_why.empty() ? vn::String(u8"vk: the full-screen vertex program could not be compiled") : std::move(build_why));
            return false;
        }

        // THE STATE, resolved from the pass (a full-screen call has no per-command state of its own) - WITH THE DEPTH
        // POLICY FORCED OFF. A full-screen call is the canonical triangle at z = 0.0, which under this engine's
        // reverse-Z convention is the FAR plane: any depth test against what the destination carries discards every
        // fragment. The destinations that expose this are exactly the ones the reference backend's M4b rule names: a
        // window pass' render pass clears D24 to 0 (far), and the deferred chain's composite BORROWS the G-buffer's
        // depth. The pass' depth policy is about the CONTENT it draws into that target, not about this call.
        vn::graphics::PassRenderState screen_state = pass.state;
        screen_state.depth                         = vn::graphics::DepthMode::Disabled;
        plan.resolved = vn::graphics::resolveRenderState(vn::graphics::RenderState{}, screen_state, vn::graphics::Topology::Triangles);
        vn::String pipeline_why;
        plan.pipeline = ensureScreenPipeline(*fragment,
                                             *vertex,
                                             compatibility,
                                             render_pass,
                                             pipelineStateOf(plan.resolved, color_attachments),
                                             color_attachments,
                                             pipeline_why);
        if (plan.pipeline == VK_NULL_HANDLE) {
            refusal.refuse(vn::graphics::DiagnosticCategory::CompileFailed,
                           contentRefusalCode(ContentRefusalCause::ScreenPipelineRefused),
                           pipeline_why.empty() ? vn::String(u8"vk: the screen pipeline could not be built") : std::move(pipeline_why));
            return false;
        }
        return true;
    }

    /** @brief Destroys every compiled program (and its modules). */
    void destroyPrograms() noexcept
    {
        for (auto& [description, entry] : programs) {
            (void)description;
            if (entry.vertex != VK_NULL_HANDLE) {
                table->vkDestroyShaderModule(device, entry.vertex, nullptr);
            }
            if (entry.fragment != VK_NULL_HANDLE) {
                table->vkDestroyShaderModule(device, entry.fragment, nullptr);
            }
        }
        programs.clear();
    }

    /** @brief Destroys every uploaded geometry stream. */
    void destroyGeometries() noexcept
    {
        for (auto& [description, entry] : geometries) {
            (void)description;
            (void)destroyGeometryStreams(entry); // a teardown is not a release: it moves no counter
        }
        geometries.clear();
    }

    /** @brief Destroys every uploaded instance stream. */
    void destroyInstanceStreams() noexcept
    {
        for (auto& [buffer, entry] : instance_streams) {
            (void)buffer;
            destroyBuffer(entry.buffer, entry.memory);
        }
        instance_streams.clear();
    }

    /** @brief Destroys every pipeline. */
    void destroyPipelines() noexcept
    {
        for (auto& [key, pipeline] : pipelines) {
            (void)key;
            table->vkDestroyPipeline(device, pipeline, nullptr);
        }
        pipelines.clear();
    }

    /**
     * @brief The content numbers, computed WITH the lock held (the caller owns it; see @c lock).
     *
     * @return The numbers (see Progress).
     */
    [[nodiscard]] Progress progressLocked() const noexcept
    {
        Progress out;
        out.stream_builds     = stream_builds_;
        out.stream_bytes      = stream_bytes_;
        out.stream_releases   = stream_releases_;
        out.program_builds    = program_builds_;
        out.pipeline_builds   = created_;
        out.pipeline_releases = pipeline_releases_;
        out.pipelines_live    = static_cast<std::uint32_t>(pipelines.size());
        // LIVE, FROM THE TABLES: a hole a release left has a null buffer and is not resident.
        for (const auto& [description, entry] : geometries) {
            (void)description;
            for (const VertexStream& stream : entry.streams) {
                if (stream.buffer != VK_NULL_HANDLE) {
                    ++out.streams_live;
                    out.stream_bytes_live += stream.bytes;
                }
            }
            for (const IndexStream& stream : entry.indices) {
                if (stream.buffer != VK_NULL_HANDLE) {
                    ++out.streams_live;
                    out.stream_bytes_live += stream.bytes;
                }
            }
        }
        // The instance streams are streams too: a command that read one paid for its bytes and fills a row of the same
        // ledger (see ResidencyKind::Streams), so a host release that drops one is visible there like any other.
        for (const auto& [buffer, entry] : instance_streams) {
            (void)buffer;
            if (entry.buffer != VK_NULL_HANDLE) {
                ++out.streams_live;
                out.stream_bytes_live += entry.bytes;
            }
        }
        return out;
    }

    /**
     * @brief Destroys every Vulkan object and blanks the books (the body of the public @c destroy, called with the lock).
     *
     * @details
     * - ONE IMPLEMENTATION, TWO CALLERS: `create` needs the same teardown at its start and on every failed step, and it
     *   already holds the lock - calling the public verb there would deadlock a non-recursive mutex (see `Lock`).
     */
    void destroyAll() noexcept
    {
        // A TEARDOWN DESTROYS, IT DOES NOT PARK: the device has idled its queue before calling this, so nothing can
        // still name what is about to go - and a park here would outlive the queue that would have run it.
        park = {};
        images.setRetirement({});
        // THE RESIDENCE TABLE GOES WITH THE ENTRIES: nothing is resident after a teardown, and a claim that outlives
        // one (a frame destroyed with the device) becomes stale rather than resurrecting what the device let go.
        residence.clear();
        if (device != VK_NULL_HANDLE && table != nullptr) {
            images.destroy();
            destroyPipelines();
            destroyPrograms();
            destroyGeometries();
            destroyInstanceStreams();
            if (pipeline_cache != VK_NULL_HANDLE) {
                table->vkDestroyPipelineCache(device, pipeline_cache, nullptr);
                pipeline_cache = VK_NULL_HANDLE;
            }
            if (content_pipeline_layout != VK_NULL_HANDLE) {
                table->vkDestroyPipelineLayout(device, content_pipeline_layout, nullptr);
                content_pipeline_layout = VK_NULL_HANDLE;
            }
            if (screen_pipeline_layout != VK_NULL_HANDLE) {
                table->vkDestroyPipelineLayout(device, screen_pipeline_layout, nullptr);
                screen_pipeline_layout = VK_NULL_HANDLE;
            }
            for (VkDescriptorSetLayout* layout : { &content_set_layout, &draw_set_layout, &screen_set_layout }) {
                if (*layout != VK_NULL_HANDLE) {
                    table->vkDestroyDescriptorSetLayout(device, *layout, nullptr);
                    *layout = VK_NULL_HANDLE;
                }
            }
        }
        table             = nullptr;
        device            = VK_NULL_HANDLE;
        allocator         = nullptr;
        screen_vertex     = nullptr;
        stream_builds_      = 0;
        stream_bytes_       = 0;
        stream_releases_    = 0;
        program_builds_     = 0;
        pipeline_releases_  = 0;
        created_            = 0;
        cache_hits_         = 0;
        preload_refused_    = false;
        preload_handed_     = false;
    }

};

ContentCaches::FrameBuilds::FrameBuilds() noexcept = default;

ContentCaches::FrameBuilds::FrameBuilds(ContentCaches& owner, const ResidenceTable::FrameDriverId session) noexcept
  : owner_(&owner)
  , session_(session)
{
}

ContentCaches::FrameBuilds::~FrameBuilds()
{
    reset();
}

ContentCaches::FrameBuilds::FrameBuilds(FrameBuilds&& other) noexcept
  : owner_(std::exchange(other.owner_, nullptr))
  , session_(other.session_)
  , resolved_(std::move(other.resolved_))
  , claimed_(std::move(other.claimed_))
  , copies_(std::move(other.copies_))
{
}

ContentCaches::FrameBuilds& ContentCaches::FrameBuilds::operator=(FrameBuilds&& other) noexcept
{
    if (this != &other) {
        reset();
        owner_    = std::exchange(other.owner_, nullptr);
        session_  = other.session_;
        resolved_ = std::move(other.resolved_);
        claimed_  = std::move(other.claimed_);
        copies_   = std::move(other.copies_);
    }
    return *this;
}

ResidenceTable::FrameDriverId ContentCaches::FrameBuilds::session() const noexcept
{
    return session_;
}

ContentCaches::GeometryEntry* ContentCaches::FrameBuilds::entryOf(const vn::graphics::GeometryDescription* description) const noexcept
{
    const auto found = resolved_.find(description);
    return found != resolved_.end() ? found->second : nullptr;
}

void ContentCaches::FrameBuilds::noteUsed(const vn::graphics::GeometryDescription& description, GeometryEntry& entry)
{
    resolved_.emplace(&description, &entry);
}

void ContentCaches::FrameBuilds::noteClaim(ResidenceTable::Claim claim)
{
    claimed_.push_back(std::move(claim));
}

ContentCaches::GeometryEntry* ContentCaches::FrameBuilds::noteCopy(const vn::graphics::GeometryDescription& description, GeometryEntry copy)
{
    // THE COPY LIVES IN THIS OBJECT, NOT IN THE CACHE (see the class note): the cache's tables are keyed by identity,
    // and a second entry under one key would be a second thing every later frame could find.
    copies_.push_back(std::make_unique<GeometryEntry>(std::move(copy)));
    GeometryEntry* const stored = copies_.back().get();
    resolved_.emplace(&description, stored);
    return stored;
}

void ContentCaches::FrameBuilds::publishAll()
{
    // EVERY CLAIM HERE NAMES A USABLE FILL: a failed build never records one (its ticket withdraws where the failure
    // happened). A claim whose key a host released in the meantime answers `stale` - counted, not an error.
    for (ResidenceTable::Claim& claim : claimed_) {
        (void)claim.publish();
    }
}

void ContentCaches::FrameBuilds::reset() noexcept
{
    // WITHDRAW FIRST, DISPOSE SECOND: the claim's destructor puts an unresolved key back to Absent (P1's prevention,
    // not its cleanup), and the copies then leave without a key ever naming them.
    claimed_.clear();
    if (!copies_.empty()) {
        if (owner_ != nullptr) {
            owner_->releaseTemporary(copies_);
        }
        copies_.clear();
    }
    resolved_.clear();
    owner_ = nullptr;
}

ContentCaches::ContentCaches()
  : impl(std::make_unique<Impl>())
{
}

ContentCaches::~ContentCaches()
{
    destroy();
}


bool ContentCaches::create(const VolkDeviceTable&     table,
                           const VkDevice             device,
                           MemoryAllocator&           allocator,
                           const std::uint32_t        uniform_alignment,
                           const std::span<const std::byte> pipeline_cache_data)
{
    const Lock held = lock();
    impl->destroyAll();
    if (device == VK_NULL_HANDLE) {
        return false;
    }

    impl->table             = &table;
    impl->device            = device;
    impl->allocator         = &allocator;
    impl->uniform_alignment = uniform_alignment != 0U ? uniform_alignment : kDefaultUniformAlignment;

    // THE CONTENT SET (set 0): material at 0, the five slots at 1-5, lights at 6 - the engine's arrangement, which is
    // also the one vine_params.glsl declares.
    VkDescriptorSetLayoutBinding content_bindings[9]{};
    content_bindings[0].binding         = 0U;
    content_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    content_bindings[0].descriptorCount = 1U;
    content_bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (std::uint32_t slot = 0; slot < kTextureSlots; ++slot) {
        VkDescriptorSetLayoutBinding& binding = content_bindings[1U + slot];
        binding.binding                       = 1U + slot;
        binding.descriptorType                = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount               = 1U;
        binding.stageFlags                    = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    content_bindings[6].binding         = 6U;
    content_bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    content_bindings[6].descriptorCount = 1U;
    content_bindings[6].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // THE ENGINE'S SHADOW ABI (see kShadowMapBinding): a forward program declares both, so the layout offers both -
    // fragment stages only, which is where the engine's shading reads them.
    content_bindings[7].binding         = kShadowMapBinding;
    content_bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    content_bindings[7].descriptorCount = 1U;
    content_bindings[7].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    content_bindings[8].binding         = kShadowBlockBinding;
    content_bindings[8].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    content_bindings[8].descriptorCount = 1U;
    content_bindings[8].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 9U;
    layout_info.pBindings    = content_bindings;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->content_set_layout) != VK_SUCCESS) {
        impl->destroyAll();
        return false;
    }

    // THE DRAW SET (set 1): the per-draw block at binding 0.
    VkDescriptorSetLayoutBinding draw_binding{};
    draw_binding.binding         = 0U;
    draw_binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    draw_binding.descriptorCount = 1U;
    draw_binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_info.bindingCount = 1U;
    layout_info.pBindings    = &draw_binding;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->draw_set_layout) != VK_SUCCESS) {
        impl->destroyAll();
        return false;
    }

    // THE FULL-SCREEN SET (set 0 of a screen program): the source's colour attachments at 0..4, the lights at 6, and
    // the SAME shadow pair the content set declares at 7/8 - a full-screen pass whose fragment is the engine's forward
    // shading states both, so the layout has to offer them (see FrameScratch::buildScreenSet for what is bound there).
    VkDescriptorSetLayoutBinding screen_bindings[8]{};
    for (std::uint32_t slot = 0; slot < detail::kScreenSamplerBindings; ++slot) {
        screen_bindings[slot].binding         = slot;
        screen_bindings[slot].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        screen_bindings[slot].descriptorCount = 1U;
        screen_bindings[slot].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    screen_bindings[5].binding         = 6U;
    screen_bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    screen_bindings[5].descriptorCount = 1U;
    screen_bindings[5].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    screen_bindings[6].binding         = kShadowMapBinding;
    screen_bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    screen_bindings[6].descriptorCount = 1U;
    screen_bindings[6].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    screen_bindings[7].binding         = kShadowBlockBinding;
    screen_bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    screen_bindings[7].descriptorCount = 1U;
    screen_bindings[7].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_info.bindingCount = 8U;
    layout_info.pBindings    = screen_bindings;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->screen_set_layout) != VK_SUCCESS) {
        impl->destroyAll();
        return false;
    }

    // THE CONTENT LAYOUT: set 0 + set 1, and the 128-byte camera push every built-in content stage reads.
    const VkDescriptorSetLayout content_sets[2]{ impl->content_set_layout, impl->draw_set_layout };
    VkPushConstantRange         push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0U;
    push.size       = sizeof(detail::PushRangeConstants);

    VkPipelineLayoutCreateInfo pipeline_info{};
    pipeline_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_info.setLayoutCount         = 2U;
    pipeline_info.pSetLayouts            = content_sets;
    pipeline_info.pushConstantRangeCount = 1U;
    pipeline_info.pPushConstantRanges    = &push;
    if (impl->table->vkCreatePipelineLayout(impl->device, &pipeline_info, nullptr, &impl->content_pipeline_layout) != VK_SUCCESS) {
        impl->destroyAll();
        return false;
    }

    // THE SCREEN LAYOUT: set 0 only (a full-screen program reads no camera push).
    pipeline_info.setLayoutCount         = 1U;
    pipeline_info.pSetLayouts            = &impl->screen_set_layout;
    pipeline_info.pushConstantRangeCount = 0U;
    pipeline_info.pPushConstantRanges    = nullptr;
    if (impl->table->vkCreatePipelineLayout(impl->device, &pipeline_info, nullptr, &impl->screen_pipeline_layout) != VK_SUCCESS) {
        impl->destroyAll();
        return false;
    }

    // THE SESSION'S PIPELINE CACHE - the driver's own deduplication, and where a serialised blob would go (backlog
    // T44 persists it). A previous session's blob is handed over here; a driver that REFUSES it gets ONE retry
    // without it, because a stale file is a slower start, never a lost session.
    VkPipelineCacheCreateInfo cache_info{};
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cache_info.initialDataSize = pipeline_cache_data.size();
    cache_info.pInitialData    = pipeline_cache_data.empty() ? nullptr : pipeline_cache_data.data();
    impl->preload_handed_      = cache_info.pInitialData != nullptr;
    if (impl->table->vkCreatePipelineCache(impl->device, &cache_info, nullptr, &impl->pipeline_cache) != VK_SUCCESS) {
        impl->pipeline_cache = VK_NULL_HANDLE;
        if (cache_info.pInitialData != nullptr) {
            impl->preload_refused_ = true;
            VkPipelineCacheCreateInfo cold{};
            cold.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            if (impl->table->vkCreatePipelineCache(impl->device, &cold, nullptr, &impl->pipeline_cache) != VK_SUCCESS) {
                impl->pipeline_cache = VK_NULL_HANDLE;
            }
        }
    }

    if (!impl->images.create(table, device, allocator)) {
        impl->destroyAll();
        return false;
    }

    // The engine's full-screen vertex stage: the interface every screen fragment stage is written against.
    const vn::intrusive_ptr<vn::graphics::ShaderProgram> fullscreen = vn::graphics::fullscreenVertexProgram();
    impl->screen_vertex = fullscreen != nullptr ? fullscreen->description() : vn::graphics::ShaderProgramDescriptionPtr{};
    return impl->screen_vertex != nullptr;
}

void ContentCaches::destroy() noexcept
{
    if (!impl) {
        return;
    }
    const Lock held = lock();
    impl->destroyAll();
}

bool ContentCaches::ready() const noexcept
{
    return impl->device != VK_NULL_HANDLE && impl->table != nullptr;
}

const VolkDeviceTable& ContentCaches::table() const noexcept
{
    return impl->table != nullptr ? *impl->table : impl->empty_table;
}

float ContentCaches::lineWidthLimit() const noexcept
{
    return impl->max_line_width;
}

std::uint32_t ContentCaches::uniformAlignment() const noexcept
{
    return impl->uniform_alignment;
}

VkDescriptorSetLayout ContentCaches::contentSetLayout() const noexcept
{
    return impl->content_set_layout;
}

VkDescriptorSetLayout ContentCaches::drawSetLayout() const noexcept
{
    return impl->draw_set_layout;
}

VkDescriptorSetLayout ContentCaches::screenSetLayout() const noexcept
{
    return impl->screen_set_layout;
}

VkPipelineLayout ContentCaches::contentPipelineLayout() const noexcept
{
    return impl->content_pipeline_layout;
}

VkPipelineLayout ContentCaches::screenPipelineLayout() const noexcept
{
    return impl->screen_pipeline_layout;
}

bool ContentCaches::planDraw(const vn::graphics::RenderCommand&                     command,
                             const vn::graphics::PassDraw&                          pass,
                             const vn::graphics::ShaderProgramDescription*          default_program,
                             const vn::graphics::RenderPassCompatibility&  compatibility,
                             const VkRenderPass                                     render_pass,
                             const std::uint32_t                                    color_attachments,
                             FrameHead::DrawPlan&                                   plan,
                             ContentRefusal&                                        refusal,
                             const Lock&                                            held,
                             FrameBuilds&                                           builds)
{
    (void)held; // the type is the proof; an unheld lock cannot be built outside this class (see Lock)
    return impl->planDraw(command, pass, default_program, compatibility, render_pass, color_attachments, plan, refusal, builds);
}

bool ContentCaches::planDraw(const vn::graphics::DrawDescription&                  draw,
                             const vn::graphics::PassDraw&                          pass,
                             const vn::graphics::RenderPassCompatibility&  compatibility,
                             const VkRenderPass                                     render_pass,
                             const std::uint32_t                                    color_attachments,
                             FrameHead::DrawPlan&                                   plan,
                             ContentRefusal&                                        refusal,
                             const Lock&                                            held)
{
    (void)held; // see the content overload
    return impl->planDraw(draw, pass, compatibility, render_pass, color_attachments, plan, refusal);
}

ContentCaches::Lock ContentCaches::lock() const
{
    // COUNTED BEFORE BLOCKING, so the number says "a caller asked for the caches", not "a caller got in": a test that
    // asserts the count does not grow with drawing calls must not depend on how long a concurrent holder took. A
    // caller that had to WAIT is counted too (see FrameLocking): the try says whether the lock was free, and the count
    // is the witness that a steady frame never waits for another frame's head.
    impl->acquisitions.fetch_add(1U, std::memory_order_relaxed);
    std::unique_lock<std::mutex> held(impl->lock, std::defer_lock);
    if (!held.try_lock()) {
        impl->waits.fetch_add(1U, std::memory_order_relaxed);
        held.lock();
    }
    return Lock(std::move(held));
}

std::uint64_t ContentCaches::lockAcquisitions() const noexcept
{
    return impl->acquisitions.load(std::memory_order_relaxed);
}

std::uint64_t ContentCaches::lockWaits() const noexcept
{
    return impl->waits.load(std::memory_order_relaxed);
}

std::uint64_t ContentCaches::pipelinesCreated() const noexcept

{
    const Lock held = lock();
    return impl->created_;
}

void ContentCaches::setCreationFeedback(const bool on) noexcept
{
    const Lock held = lock();
    impl->creation_feedback = on;
}

void ContentCaches::setRetirement(ContentImages::ParkFn park)
{
    const Lock held = lock();
    impl->park = park;
    // THE IMAGES FOLLOW THE SAME EVIDENCE: a released texture is dated against the same watermark a released stream is
    // (see ContentImages::setRetirement), which is what keeps one clock for "may this object go".
    impl->images.setRetirement(std::move(park));
}

std::uint64_t ContentCaches::pipelineCacheHits() const noexcept
{
    const Lock held = lock();
    return impl->cache_hits_;
}

bool ContentCaches::pipelineCachePreloadRefused() const noexcept
{
    const Lock held = lock();
    return impl->preload_refused_;
}

bool ContentCaches::pipelineCachePreloadHanded() const noexcept
{
    const Lock held = lock();
    return impl->preload_handed_;
}

std::vector<std::byte> ContentCaches::pipelineCacheBytes() const
{
    const Lock held = lock();
    std::vector<std::byte> bytes;
    if (impl->pipeline_cache == VK_NULL_HANDLE || impl->table == nullptr || impl->device == VK_NULL_HANDLE) {
        return bytes;
    }

    // TWO CALLS, the standard Vulkan shape: the first answers the size, the second the bytes. A cache that reports no
    // size has nothing to persist, which is not a failure (a session that built no pipeline).
    std::size_t size = 0U;
    if (impl->table->vkGetPipelineCacheData(impl->device, impl->pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0U) {
        return bytes;
    }
    bytes.resize(size);
    if (impl->table->vkGetPipelineCacheData(impl->device, impl->pipeline_cache, &size, bytes.data()) != VK_SUCCESS) {
        bytes.clear();
        return bytes;
    }
    bytes.resize(size); // the driver may answer fewer bytes than the first call promised
    return bytes;
}

ContentCaches::Progress ContentCaches::progress() const
{
    const Lock held = lock();
    return impl->progressLocked();
}

ContentImages::Progress ContentCaches::imageProgress() const
{
    const Lock held = lock();
    return impl->images.progress();
}

ContentCaches::Numbers ContentCaches::numbers() const
{
    const Lock held = lock();
    Numbers    out;
    out.content = impl->progressLocked();
    out.images  = impl->images.progress();
    return out;
}

void ContentCaches::setMaxAnisotropy(const float max_anisotropy) noexcept
{
    const Lock held = lock();
    impl->images.setMaxAnisotropy(max_anisotropy);
}

bool ContentCaches::hasPendingUploads(const ContentImages::BatchId batch) const
{
    const Lock held = lock();
    return impl->images.hasPendingUploads(batch);
}

void ContentCaches::releaseUploadStaging(const ContentImages::BatchId batch) noexcept
{
    const Lock held = lock();
    impl->images.releaseUploadStaging(batch);
}

bool ContentCaches::releaseTexture(const vn::graphics::Texture& texture)
{
    const Lock held = lock();
    return impl->images.releaseTexture(texture);
}

void ContentCaches::setLineWidthLimit(const float max_line_width) noexcept
{
    const Lock held = lock();
    impl->max_line_width = max_line_width >= 1.0F ? max_line_width : 1.0F;
}

std::size_t ContentCaches::releaseGeometry(const vn::graphics::GeometryDescription* description)
{
    const Lock held = lock();
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || description == nullptr) {
        return 0;
    }
    // THE KEY IS FORGOTTEN EVEN WHEN NOTHING WAS RESIDENT (F4): after an announcement the table must agree with the
    // cache, or the next frame would find a key it may not claim. A claim still in flight over it becomes stale and
    // cannot resurrect what the host let go (see ResidenceTable::forget).
    (void)impl->residence.forget(description);
    const auto found = impl->geometries.find(description);
    if (found == impl->geometries.end()) {
        return 0; // nothing was resident: already released
    }
    // THE ENTRY LEAVES THE CACHE NOW; ITS BUFFERS FOLLOW WHEN THE DEVICE SAYS THEY MAY (see releaseLater). The two
    // questions are different: "is this content resident" is about the table, "may this object be destroyed" is about
    // the queue - and answering both in one place is what used to force release to be a between-frames call.
    GeometryEntry doomed = std::move(found->second);
    impl->geometries.erase(found);
    impl->stream_releases_ += Impl::liveStreams(doomed);
    impl->releaseLater([target = impl.get(), entry = std::move(doomed)]() mutable {
        (void)target->destroyGeometryStreams(entry);
    });
    return 1;
}

std::size_t ContentCaches::releaseProgram(const vn::graphics::ShaderProgramDescription* description)
{
    const Lock held = lock();
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || description == nullptr) {
        return 0;
    }
    std::size_t dropped = 0;
    // THE PIPELINES FIRST: they are keyed by the program's pointer, so once the entry is gone they could never be
    // looked up again - keeping them alive would be a leak nothing can name.
    std::vector<VkPipeline>     pipelines;
    std::vector<VkShaderModule> modules;
    for (auto it = impl->pipelines.begin(); it != impl->pipelines.end();) {
        if (it->first.program != description) {
            ++it;
            continue;
        }
        pipelines.push_back(it->second);
        it = impl->pipelines.erase(it);
        ++impl->pipeline_releases_;
        ++dropped;
    }

    // ONE TEXT MAY BE SEVERAL VARIANTS (see ProgramVariant): every compile of this description goes, and the entry
    // count says how many. The pipelines above were keyed by the same pointer, so none can survive this.
    for (auto it = impl->programs.begin(); it != impl->programs.end();) {
        if (it->first.description != description) {
            ++it;
            continue;
        }
        if (it->second.vertex != VK_NULL_HANDLE) {
            modules.push_back(it->second.vertex);
        }
        if (it->second.fragment != VK_NULL_HANDLE) {
            modules.push_back(it->second.fragment);
        }
        it = impl->programs.erase(it);
        ++dropped;
    }
    // THE OBJECTS LEAVE WHEN THE DEVICE SAYS THEY MAY (see releaseLater): a submitted frame may still be drawing with
    // the pipelines this announcement forgot, so destroying them here would be a race with the GPU.
    if (!pipelines.empty() || !modules.empty()) {
        impl->releaseLater([table = impl->table,
                            device = impl->device,
                            pipelines = std::move(pipelines),
                            modules   = std::move(modules)]() {
            for (const VkPipeline pipeline : pipelines) {
                table->vkDestroyPipeline(device, pipeline, nullptr);
            }
            for (const VkShaderModule module : modules) {
                table->vkDestroyShaderModule(device, module, nullptr);
            }
        });
    }
    return dropped;
}

std::size_t ContentCaches::releaseStreams(const void* buffer)
{
    const Lock held = lock();
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || buffer == nullptr) {
        return 0;
    }
    // THE STREAMS LEAVE THE CACHE NOW AND THE BUFFERS FOLLOW (see releaseLater): the handles are taken out of the
    // entries so a later frame uploads them again, and the destruction itself waits for the device's watermark.
    struct DoomedBuffer {
        VkBuffer    buffer{ VK_NULL_HANDLE };
        MemorySlice memory{};
    };
    std::vector<DoomedBuffer> doomed;
    const auto                take = [&doomed](VkBuffer& stream_buffer, MemorySlice& stream_memory) {
        if (stream_buffer == VK_NULL_HANDLE) {
            return;
        }
        doomed.push_back(DoomedBuffer{ stream_buffer, stream_memory });
        stream_buffer   = VK_NULL_HANDLE;
        stream_memory   = MemorySlice{};
    };

    std::size_t dropped = 0;
    for (auto& [description, entry] : impl->geometries) {
        (void)description;
        for (VertexStream& stream : entry.streams) {
            if (stream.source != buffer || stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            take(stream.buffer, stream.memory);
            ++impl->stream_releases_;
            ++dropped;
        }
        for (IndexStream& stream : entry.indices) {
            if (stream.source != buffer || stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            take(stream.buffer, stream.memory);
            ++impl->stream_releases_;
            ++dropped;
        }
    }
    // AN INSTANCE STREAM IS RELEASED BY THE SAME ANNOUNCEMENT (`releaseContent(Buffer)` names a vertex-or-instance
    // stream, see RenderDevice): the next draw that reads this buffer uploads it again, exactly like a vertex one.
    const auto instance = impl->instance_streams.find(static_cast<const vn::Buffer<float>*>(buffer));
    if (instance != impl->instance_streams.end() && instance->second.buffer != VK_NULL_HANDLE) {
        take(instance->second.buffer, instance->second.memory);
        ++impl->stream_releases_;
        ++dropped;
    }
    if (!doomed.empty()) {
        impl->releaseLater([target = impl.get(), buffers = std::move(doomed)]() mutable {
            for (DoomedBuffer& one : buffers) {
                target->destroyBuffer(one.buffer, one.memory);
            }
        });
    }
    return dropped;
}

void ContentCaches::preparePass(const VkCommandBuffer commands, const std::span<const vn::graphics::DrawDescription> draws, const Lock& held)
{
    (void)held; // see Lock: the caller's hold is the precondition
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || commands == VK_NULL_HANDLE) {
        return;
    }
    vn::String why;
    if (!impl->images.ensureFallback(commands, 0, &why)) {
        return; // reported when a draw finds no fallback view
    }
    for (const vn::graphics::DrawDescription& draw : draws) {
        if (draw.kind != vn::graphics::DrawDescription::Kind::Scene || draw.commands == nullptr) {
            continue;
        }
        for (const vn::graphics::RenderCommand& command : *draw.commands) {
            vn::graphics::MaterialDescriptionPtr description = command.material_description;
            if (description == nullptr && command.material != nullptr) {
                description = command.material->description();
            }
            if (description == nullptr) {
                continue;
            }
            for (const vn::graphics::TextureSlotBinding& slot : description->values().textures) {
                if (slot.texture != nullptr && slot.texture->isComplete()) {
                    (void)impl->images.viewFor(*slot.texture, commands, 0, &why);
                }
            }
        }
    }
}

bool ContentCaches::ensureFallback(const VkCommandBuffer commands, const Lock& held, vn::String* const why, const ContentImages::BatchId batch)
{
    (void)held;
    return impl->images.ensureFallback(commands, batch, why);
}

void ContentCaches::resolveTextures(const vn::graphics::MaterialDescription* const material,
                                    const VkCommandBuffer                          commands,
                                    FrameHead&                                     head,
                                    FrameHead::DrawPlan&                           plan,
                                    ContentRefusal&                                refusal,
                                    const Lock&                                    held,
                                    const ContentImages::BatchId                   batch)
{
    (void)held;
    plan.slots = {};
    // THE STAND-IN FIRST: it is what an absent slot binds, and its own copy has to be recorded before any render pass
    // begins - which is where this runs (the frame head, or a warm-up's transfer batch for preparePass). The stand-in
    // and the two samplers are the same for every call of one frame, so they land on the HEAD, not on the plan.
    vn::String why;
    if (commands != VK_NULL_HANDLE) {
        (void)impl->images.ensureFallback(commands, batch, &why);
    }
    head.fallback_view    = impl->images.fallbackView();
    head.fallback_sampler = impl->images.fallbackSampler();
    head.shadow_sampler   = impl->images.samplerFor(detail::kShadowSampler);
    if (material == nullptr) {
        for (FrameHead::SlotBinding& binding : plan.slots) {
            binding.view    = head.fallback_view;
            binding.sampler = head.fallback_sampler;
        }
        return;
    }
    const vn::graphics::MaterialParameters& values = material->values();
    for (std::uint32_t slot = 0; slot < kTextureSlots; ++slot) {
        FrameHead::SlotBinding& binding = plan.slots[slot];
        binding.view                    = head.fallback_view;
        binding.sampler                 = head.fallback_sampler;
        const vn::graphics::TextureSlotBinding& named = values.textures[slot];
        if (named.texture == nullptr || !named.texture->isComplete()) {
            continue;
        }
        vn::String        upload_why;
        const VkImageView view = impl->images.viewFor(*named.texture, commands, batch, &upload_why);
        if (view == VK_NULL_HANDLE) {
            refusal.refuse(vn::graphics::DiagnosticCategory::ContentSkipped,
                           contentRefusalCode(ContentRefusalCause::MaterialTextureSkipped),
                           upload_why.empty() ? vn::String(u8"vk: a material texture could not be uploaded") : std::move(upload_why));
            continue;
        }
        binding.view    = view;
        binding.present = true;
        const VkSampler authored = impl->images.samplerFor(named.sampler);
        if (authored != VK_NULL_HANDLE) {
            binding.sampler = authored;
        }
    }
}

void ContentCaches::endFrame(const ContentImages::BatchId batch) noexcept
{
    const Lock held = lock();
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        impl->images.releaseFrame(batch);
    }
}

ResidenceTable& ContentCaches::residence() noexcept
{
    return impl->residence;
}

const ResidenceTable& ContentCaches::residence() const noexcept
{
    return impl->residence;
}

void ContentCaches::releaseTemporary(std::vector<std::unique_ptr<GeometryEntry>>& copies) noexcept
{
    if (copies.empty()) {
        return;
    }
    const Lock held = lock();
    for (const std::unique_ptr<GeometryEntry>& copy : copies) {
        if (copy != nullptr && impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
            // DESTROY NOW, DO NOT PARK: the copy was never in the tables, so only the frame that owned it could name
            // it - and that frame is over (see the header's note). The release counters stay unmoved: a copy is not
            // an announced release, and the live numbers answer what the cache RESIDES, which never included it.
            (void)impl->destroyGeometryStreams(*copy);
        }
    }
    copies.clear();
}

VN_BACKEND_VK_NS_END
