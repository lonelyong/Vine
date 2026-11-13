#include <vine/backend_vk/internal/ContentDraw.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/backend_vk/internal/ShaderCompiler.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/ShaderAbi.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The alignment used when the device reports none (Vulkan's minimum for uniform-buffer offsets).
constexpr std::uint32_t kDefaultUniformAlignment = 256U;

/// @brief Descriptor sets (and therefore draws) one frame may record before the pool is exhausted.
constexpr std::uint32_t kDescriptorsPerFrame = 256U;

/**
 * @brief Finds a memory type that serves @p bits and has every property in @p wanted.
 *
 * @param memory The device's memory properties.
 * @param bits   The type bits the allocation requires.
 * @param wanted The property flags the memory must have.
 * @param out    Receives the type index.
 * @return true when a matching type exists.
 */
[[nodiscard]] bool findMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                                  std::uint32_t                          bits,
                                  VkMemoryPropertyFlags                  wanted,
                                  std::uint32_t&                         out) noexcept
{
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            out = index;
            return true;
        }
    }
    return false;
}

/**
 * @brief Rounds @p value up to the next multiple of @p alignment.
 *
 * @param value     Value to round.
 * @param alignment Multiple to round to (0 is treated as 1).
 * @return The rounded value.
 */
[[nodiscard]] VkDeviceSize alignedUp(VkDeviceSize value, VkDeviceSize alignment) noexcept
{
    const VkDeviceSize step = alignment != 0U ? alignment : 1U;
    return ((value + step - 1U) / step) * step;
}

/**
 * @brief Gets the vertex format for a channel's component count.
 *
 * @param components Scalars per vertex (1..4).
 * @return The `VkFormat`, or `VK_FORMAT_UNDEFINED` for a count this backend cannot serve.
 */
[[nodiscard]] VkFormat vertexFormatOf(std::uint32_t components) noexcept
{
    switch (components) {
    case 1: return VK_FORMAT_R32_SFLOAT;
    case 2: return VK_FORMAT_R32G32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32_SFLOAT;
    case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/**
 * @brief Gets the Vulkan topology for an SDK topology.
 *
 * @param topology The SDK topology.
 * @return The Vulkan topology (triangles for a value this backend does not know).
 */
[[nodiscard]] VkPrimitiveTopology topologyOf(vn::graphics::Topology topology) noexcept
{
    using vn::graphics::Topology;
    switch (topology) {
    case Topology::Triangles: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case Topology::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case Topology::Points: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Topology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

/**
 * @brief Copies a column-major double matrix into a column-major float array.
 *
 * @param source       The matrix (element (row, column) access).
 * @param destination  The sixteen floats, column-major.
 */
void copyMatrix(const vn::math::Mat4d& source, std::array<float, 16>& destination) noexcept
{
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            destination[column * 4 + row] = static_cast<float>(source(row, column));
        }
    }
}

/**
 * @brief Fills the per-view block a content program reads (see ShaderAbi.hpp).
 *
 * @param camera Camera snapshot of the drawing call.
 * @param width  Target width in pixels.
 * @param height Target height in pixels.
 * @return The block.
 */
[[nodiscard]] vn::graphics::VineViewBlock buildViewBlock(const vn::graphics::CameraSnapshot& camera, int width, int height) noexcept
{
    vn::graphics::VineViewBlock block{};
    copyMatrix(camera.view, block.view);
    copyMatrix(camera.view.inverted(), block.inv_view);
    copyMatrix(camera.projection, block.proj);
    copyMatrix(camera.projection * camera.view, block.view_proj);
    block.cam_pos = { static_cast<float>(camera.eye.x), static_cast<float>(camera.eye.y), static_cast<float>(camera.eye.z), 0.0F };
    block.frame   = { 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F };
    return block;
}

/**
 * @brief Fills the per-draw block a content program reads (see ShaderAbi.hpp).
 *
 * @param command The draw command.
 * @return The block: the model matrix and the effective opacity in `params.x`.
 */
[[nodiscard]] vn::graphics::VineDrawBlock buildDrawBlock(const vn::graphics::RenderCommand& command) noexcept
{
    vn::graphics::VineDrawBlock block{};
    copyMatrix(command.model_matrix, block.model);
    // params.y is the point size the resolved state asks for; M2 draws triangles only, so it stays at "nobody authored
    // one" (1.0). z/w are the reserved user parameter slot (P10).
    block.params = { command.opacity, 1.0F, 0.0F, 0.0F };
    return block;
}

} // namespace

struct ContentDraw::Impl {
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
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        std::uint32_t location{ 0 };
        std::uint32_t components{ 0 };
    };

    /// @brief One uploaded index stream (per primitive).
    struct IndexStream {
        VkBuffer       buffer{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        std::uint32_t  count{ 0 };
    };

    /// @brief One geometry revision's uploaded streams.
    struct GeometryEntry {
        vn::graphics::GeometryDescriptionPtr description;
        std::vector<VertexStream>            streams;
        std::vector<IndexStream>             indices;
        std::uint32_t                        vertex_count{ 0 };
        bool                                 failed{ false };
    };

    /// @brief What one pipeline is built for: program revision, render pass and the vertex layout.
    struct PipelineKey {
        const vn::graphics::ShaderProgramDescription*         program{ nullptr };
        VkRenderPass                                          render_pass{ VK_NULL_HANDLE };
        VkPrimitiveTopology                                   topology{ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> channels; // (location, components), in description order

        /**
         * @brief Orders keys so the cache is a stable map (identity first, layout last).
         *
         * @param other Key to compare against.
         * @return true when this key sorts before @p other.
         */
        [[nodiscard]] bool operator<(const PipelineKey& other) const
        {
            if (program != other.program) {
                return program < other.program;
            }
            if (render_pass != other.render_pass) {
                return render_pass < other.render_pass;
            }
            if (topology != other.topology) {
                return topology < other.topology;
            }
            return std::lexicographical_compare(channels.begin(), channels.end(), other.channels.begin(), other.channels.end());
        }
    };

    const VolkDeviceTable*          table{ nullptr };
    VkDevice                        device{ VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::uint32_t                   uniform_alignment{ kDefaultUniformAlignment };

    VkDescriptorSetLayout set_layout{ VK_NULL_HANDLE };
    VkPipelineLayout      pipeline_layout{ VK_NULL_HANDLE };
    VkDescriptorPool      descriptor_pool{ VK_NULL_HANDLE };

    std::unordered_map<const vn::graphics::ShaderProgramDescription*, ProgramEntry> programs;
    std::unordered_map<const vn::graphics::GeometryDescription*, GeometryEntry>     geometries;
    std::map<PipelineKey, VkPipeline>                                               pipelines;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>>                                frame_uniforms;

    /**
     * @brief Creates a buffer and binds host-visible coherent memory to it.
     *
     * @param size   Bytes to allocate (must be > 0).
     * @param usage  Buffer usage flags.
     * @param buffer Receives the buffer.
     * @param memory Receives the bound memory.
     * @return true when both exist.
     */
    [[nodiscard]] bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory) const
    {
        VkBufferCreateInfo info{};
        info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size        = size;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (table->vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements requirements{};
        table->vkGetBufferMemoryRequirements(device, buffer, &requirements);

        std::uint32_t memory_type = 0;
        if (!findMemoryType(memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory_type)) {
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocate{};
        allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize  = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (table->vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS) {
            memory = VK_NULL_HANDLE;
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        if (table->vkBindBufferMemory(device, buffer, memory, 0U) != VK_SUCCESS) {
            table->vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    /**
     * @brief Destroys a buffer and its memory (idempotent).
     *
     * @param buffer Buffer to destroy.
     * @param memory Memory to free.
     */
    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory) const noexcept
    {
        if (buffer != VK_NULL_HANDLE) {
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            table->vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }

    /**
     * @brief Creates a buffer and copies @p bytes into it (host-visible coherent memory).
     *
     * @param bytes  Bytes to upload (must be non-empty).
     * @param usage  Buffer usage flags.
     * @param buffer Receives the buffer.
     * @param memory Receives the bound memory.
     * @return true when the bytes are in the buffer.
     */
    [[nodiscard]] bool upload(std::span<const std::byte> bytes, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory) const
    {
        if (bytes.empty()) {
            return false;
        }
        if (!createHostBuffer(static_cast<VkDeviceSize>(bytes.size()), usage, buffer, memory)) {
            return false;
        }
        void* mapped = nullptr;
        if (table->vkMapMemory(device, memory, 0U, static_cast<VkDeviceSize>(bytes.size()), 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
            destroyBuffer(buffer, memory);
            return false;
        }
        std::memcpy(mapped, bytes.data(), bytes.size());
        table->vkUnmapMemory(device, memory);
        return true;
    }

    /**
     * @brief Gets (or compiles) one program revision's shader modules.
     *
     * @param description The program's text as a value (not null; the entry keeps it alive).
     * @param why         Receives the first failure's reason.
     * @return The entry, or null when the program could not be compiled.
     */
    [[nodiscard]] ProgramEntry* ensureProgram(const vn::graphics::ShaderProgramDescription* description, vn::String& why)
    {
        const auto found = programs.find(description);
        if (found != programs.end()) {
            return found->second.failed ? nullptr : &found->second;
        }

        ProgramEntry entry;
        entry.description = vn::graphics::ShaderProgramDescriptionPtr(description);

        for (const vn::graphics::ShaderStage& stage : description->stages()) {
            if (stage.type == vn::graphics::ShaderStageType::Compute) {
                continue; // M2 has no compute path
            }
            SpirvModule spirv;
            if (!compileStage(stage, spirv, why)) {
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

        if (!entry.failed && (entry.vertex == VK_NULL_HANDLE || entry.fragment == VK_NULL_HANDLE)) {
            entry.failed = true;
            why          = vn::String(u8"vk: a content program needs a vertex and a fragment stage (M2)");
        }
        if (entry.vertex_entry.empty()) {
            entry.vertex_entry = "main";
        }
        if (entry.fragment_entry.empty()) {
            entry.fragment_entry = "main";
        }

        const auto inserted = programs.emplace(description, std::move(entry));
        return inserted.first->second.failed ? nullptr : &inserted.first->second;
    }

    /**
     * @brief Gets (or uploads) one geometry revision's streams.
     *
     * @param description The geometry's shape as a value (the entry keeps it alive).
     * @param why         Receives the first failure's reason.
     * @return The entry, or null when a stream could not be uploaded.
     */
    [[nodiscard]] GeometryEntry* ensureGeometry(const vn::graphics::GeometryDescription& description, vn::String& why)
    {
        const auto found = geometries.find(&description);
        if (found != geometries.end()) {
            return found->second.failed ? nullptr : &found->second;
        }

        GeometryEntry entry;
        entry.description = vn::graphics::GeometryDescriptionPtr(&description);

        for (const vn::graphics::GeometryChannel& channel : description.channels()) {
            const std::span<const float> scalars = channel.channel.scalars();
            if (scalars.empty()) {
                continue;
            }
            const std::uint32_t components = channel.channel.components;
            if (vertexFormatOf(components) == VK_FORMAT_UNDEFINED) {
                entry.failed = true;
                why          = vn::String(u8"vk: an attribute channel states a component count Vulkan has no format for");
                break;
            }
            VertexStream stream;
            stream.location   = channel.location;
            stream.components = components;
            if (!upload(std::as_bytes(scalars), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, stream.buffer, stream.memory)) {
                entry.failed = true;
                why          = vn::String(u8"vk: a vertex channel could not be uploaded");
                break;
            }
            entry.streams.push_back(stream);
        }

        if (!entry.failed) {
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
            entry.indices.resize(primitives.size());
            for (std::size_t index = 0; index < primitives.size(); ++index) {
                const std::span<const std::uint32_t> indices = primitives[index].indices.span();
                if (indices.empty()) {
                    continue;
                }
                IndexStream stream;
                if (!upload(std::as_bytes(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, stream.buffer, stream.memory)) {
                    entry.failed = true;
                    why          = vn::String(u8"vk: an index stream could not be uploaded");
                    break;
                }
                stream.count        = static_cast<std::uint32_t>(indices.size());
                entry.indices[index] = stream;
            }
        }

        if (entry.failed) {
            for (VertexStream& stream : entry.streams) {
                destroyBuffer(stream.buffer, stream.memory);
            }
            for (IndexStream& stream : entry.indices) {
                destroyBuffer(stream.buffer, stream.memory);
            }
            entry.streams.clear();
            entry.indices.clear();
        }

        const auto inserted = geometries.emplace(&description, std::move(entry));
        return inserted.first->second.failed ? nullptr : &inserted.first->second;
    }

    /**
     * @brief Gets (or builds) the pipeline serving one program, render pass and vertex layout.
     *
     * @param program      The compiled program.
     * @param geometry     The geometry whose channels state the vertex layout.
     * @param topology     The draw's topology.
     * @param render_pass  The render pass the pipeline must be compatible with.
     * @param why          Receives the reason when the pipeline could not be built.
     * @return The pipeline, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkPipeline ensurePipeline(const ProgramEntry&                  program,
                                            const vn::graphics::GeometryDescription& geometry,
                                            vn::graphics::Topology               topology,
                                            VkRenderPass                         render_pass,
                                            vn::String&                          why)
    {
        PipelineKey key;
        key.program     = program.description.get();
        key.render_pass = render_pass;
        key.topology    = topologyOf(topology);
        for (const vn::graphics::GeometryChannel& channel : geometry.channels()) {
            if (channel.channel.empty()) {
                continue;
            }
            key.channels.emplace_back(channel.location, channel.channel.components);
        }

        const auto found = pipelines.find(key);
        if (found != pipelines.end()) {
            return found->second;
        }

        std::vector<VkVertexInputBindingDescription>   bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        bindings.reserve(key.channels.size());
        attributes.reserve(key.channels.size());
        for (std::size_t index = 0; index < key.channels.size(); ++index) {
            const std::uint32_t binding   = static_cast<std::uint32_t>(index);
            const std::uint32_t location  = key.channels[index].first;
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

        // WHAT THE PIPELINE STATES. M2's content draw has no material, no depth test and no blend (M3/M4 bring state):
        // a triangle is written as it is. Viewport and scissor are DYNAMIC so a pass' rectangle is one command, not a
        // pipeline (`PassDraw::viewport`).
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = program.vertex;
        stages[0].pName  = program.vertex_entry.c_str();
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = program.fragment;
        stages[1].pName  = program.fragment_entry.c_str();

        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount   = static_cast<std::uint32_t>(bindings.size());
        vertex_input.pVertexBindingDescriptions      = bindings.empty() ? nullptr : bindings.data();
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions    = attributes.empty() ? nullptr : attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = key.topology;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1U;
        viewport.scissorCount  = 1U;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode    = VK_CULL_MODE_NONE; // M3 resolves the state; a picture nobody culled is still the picture
        raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth   = 1.0F;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable  = VK_FALSE; // M3 wires the pass' depth state
        depth.depthWriteEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable    = VK_FALSE;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1U;
        blend.pAttachments    = &blend_attachment;

        const VkDynamicState dynamic_states[2]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2U;
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
        info.layout              = pipeline_layout;
        info.renderPass          = render_pass;
        info.subpass             = 0U;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (table->vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U, &info, nullptr, &pipeline) != VK_SUCCESS) {
            why = vn::String(u8"vk: the content pipeline could not be built (see the loader's report)");
            return VK_NULL_HANDLE;
        }
        pipelines.emplace(std::move(key), pipeline);
        return pipeline;
    }

    /**
     * @brief Allocates the descriptor set of one draw and points its two bindings at @p buffer.
     *
     * @param buffer      The draw's uniform buffer (view block at 0, draw block at @p draw_offset).
     * @param draw_offset Byte offset of the draw block inside @p buffer.
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet makeSet(VkBuffer buffer, VkDeviceSize draw_offset) const
    {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = descriptor_pool;
        allocate.descriptorSetCount = 1U;
        allocate.pSetLayouts        = &set_layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (table->vkAllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }

        VkDescriptorBufferInfo view_info{};
        view_info.buffer = buffer;
        view_info.offset = 0U;
        view_info.range  = sizeof(vn::graphics::VineViewBlock);

        VkDescriptorBufferInfo draw_info{};
        draw_info.buffer = buffer;
        draw_info.offset = draw_offset;
        draw_info.range  = sizeof(vn::graphics::VineDrawBlock);

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0U;
        writes[0].descriptorCount = 1U;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &view_info;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1U;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo     = &draw_info;
        table->vkUpdateDescriptorSets(device, 2U, writes, 0U, nullptr);
        return set;
    }

    /**
     * @brief Creates the per-draw uniform buffer and writes both blocks into it.
     *
     * @param view        The per-view block.
     * @param draw        The per-draw block.
     * @param draw_offset Byte offset the draw block starts at.
     * @param buffer      Receives the buffer.
     * @param memory      Receives the memory.
     * @return true when the bytes are in the buffer.
     */
    [[nodiscard]] bool createUniforms(const vn::graphics::VineViewBlock& view,
                                      const vn::graphics::VineDrawBlock& draw,
                                      VkDeviceSize                       draw_offset,
                                      VkBuffer&                          buffer,
                                      VkDeviceMemory&                    memory) const
    {
        const VkDeviceSize size = draw_offset + sizeof(vn::graphics::VineDrawBlock);
        if (!createHostBuffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer, memory)) {
            return false;
        }
        void* mapped = nullptr;
        if (table->vkMapMemory(device, memory, 0U, size, 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
            destroyBuffer(buffer, memory);
            return false;
        }
        std::memcpy(mapped, &view, sizeof(view));
        std::memcpy(static_cast<std::byte*>(mapped) + draw_offset, &draw, sizeof(draw));
        table->vkUnmapMemory(device, memory);
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
            for (VertexStream& stream : entry.streams) {
                destroyBuffer(stream.buffer, stream.memory);
            }
            for (IndexStream& stream : entry.indices) {
                destroyBuffer(stream.buffer, stream.memory);
            }
        }
        geometries.clear();
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

    /** @brief Destroys the per-frame uniform buffers. */
    void destroyFrameUniforms() noexcept
    {
        for (auto& [buffer, memory] : frame_uniforms) {
            destroyBuffer(buffer, memory);
        }
        frame_uniforms.clear();
    }
};

ContentDraw::ContentDraw()
  : impl(std::make_unique<Impl>())
{
}

ContentDraw::~ContentDraw()
{
    destroy();
}

bool ContentDraw::create(const VolkDeviceTable&                  table,
                         VkDevice                                device,
                         const VkPhysicalDeviceMemoryProperties& memory,
                         std::uint32_t                           uniform_alignment)
{
    destroy();
    if (device == VK_NULL_HANDLE) {
        return false;
    }

    impl->table             = &table;
    impl->device            = device;
    impl->memory_properties = memory;
    impl->uniform_alignment = uniform_alignment != 0U ? uniform_alignment : kDefaultUniformAlignment;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0U;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1U;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1U;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1U;
    bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 2U;
    layout_info.pBindings    = bindings;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->set_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkPipelineLayoutCreateInfo pipeline_info{};
    pipeline_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_info.setLayoutCount = 1U;
    pipeline_info.pSetLayouts    = &impl->set_layout;
    if (impl->table->vkCreatePipelineLayout(impl->device, &pipeline_info, nullptr, &impl->pipeline_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_size.descriptorCount = kDescriptorsPerFrame * 2U;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = kDescriptorsPerFrame;
    pool_info.poolSizeCount = 1U;
    pool_info.pPoolSizes    = &pool_size;
    if (impl->table->vkCreateDescriptorPool(impl->device, &pool_info, nullptr, &impl->descriptor_pool) != VK_SUCCESS) {
        destroy();
        return false;
    }
    return true;
}

void ContentDraw::destroy() noexcept
{
    if (!impl) {
        return;
    }
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        impl->destroyFrameUniforms();
        impl->destroyPipelines();
        impl->destroyPrograms();
        impl->destroyGeometries();
        if (impl->descriptor_pool != VK_NULL_HANDLE) {
            impl->table->vkDestroyDescriptorPool(impl->device, impl->descriptor_pool, nullptr);
            impl->descriptor_pool = VK_NULL_HANDLE;
        }
        if (impl->pipeline_layout != VK_NULL_HANDLE) {
            impl->table->vkDestroyPipelineLayout(impl->device, impl->pipeline_layout, nullptr);
            impl->pipeline_layout = VK_NULL_HANDLE;
        }
        if (impl->set_layout != VK_NULL_HANDLE) {
            impl->table->vkDestroyDescriptorSetLayout(impl->device, impl->set_layout, nullptr);
            impl->set_layout = VK_NULL_HANDLE;
        }
    }
    impl->table  = nullptr;
    impl->device = VK_NULL_HANDLE;
    impl->memory_properties = VkPhysicalDeviceMemoryProperties{};
}

void ContentDraw::beginFrame()
{
    if (impl->device != VK_NULL_HANDLE && impl->descriptor_pool != VK_NULL_HANDLE) {
        (void)impl->table->vkResetDescriptorPool(impl->device, impl->descriptor_pool, 0U);
    }
}

void ContentDraw::endFrame() noexcept
{
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        impl->destroyFrameUniforms();
    }
}

void ContentDraw::releaseRenderPass(VkRenderPass render_pass) noexcept
{
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || render_pass == VK_NULL_HANDLE) {
        return;
    }
    for (auto iterator = impl->pipelines.begin(); iterator != impl->pipelines.end();) {
        if (iterator->first.render_pass == render_pass) {
            impl->table->vkDestroyPipeline(impl->device, iterator->second, nullptr);
            iterator = impl->pipelines.erase(iterator);
        }
        else {
            ++iterator;
        }
    }
}

std::uint64_t ContentDraw::record(VkCommandBuffer                             commands,
                                  VkRenderPass                                render_pass,
                                  int                                         width,
                                  int                                         height,
                                  const vn::graphics::DrawDescription&        draw,
                                  const vn::graphics::PassDraw&               pass,
                                  const vn::graphics::ShaderProgramDescription* default_program,
                                  vn::String*                                 why,
                                  vn::graphics::DiagnosticCategory*           category)
{
    vn::String                       local;
    vn::String&                      reason        = why != nullptr ? *why : local;
    vn::graphics::DiagnosticCategory local_category = vn::graphics::DiagnosticCategory::ContentSkipped;
    vn::graphics::DiagnosticCategory& category_ref  = category != nullptr ? *category : local_category;
    reason                                          = vn::String();
    category_ref                                    = vn::graphics::DiagnosticCategory::ContentSkipped;

    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || draw.commands == nullptr) {
        return 0;
    }
    // NO CAMERA MEANS NO DRAW (see DrawDescription::camera): the pass issues nothing, and that is not a failure.
    if (!draw.camera.present) {
        return 0;
    }
    if (draw.kind != vn::graphics::DrawDescription::Kind::Scene) {
        reason = vn::String(u8"vk: a full-screen draw is M4's; this one was skipped");
        return 0;
    }

    const vn::graphics::VineViewBlock view        = buildViewBlock(draw.camera, width, height);
    const VkDeviceSize                draw_offset = alignedUp(sizeof(vn::graphics::VineViewBlock), impl->uniform_alignment);

    // The rectangle the pass states, or the whole target (a pass without a viewport draws all of it).
    VkViewport viewport{};
    VkRect2D   scissor{};
    if (pass.viewport.has_value() && pass.viewport->isValid()) {
        viewport.x        = static_cast<float>(pass.viewport->x);
        viewport.y        = static_cast<float>(pass.viewport->y);
        viewport.width    = static_cast<float>(pass.viewport->width);
        viewport.height   = static_cast<float>(pass.viewport->height);
        scissor.offset    = VkOffset2D{ pass.viewport->x, pass.viewport->y };
        scissor.extent    = VkExtent2D{ static_cast<std::uint32_t>(pass.viewport->width), static_cast<std::uint32_t>(pass.viewport->height) };
    }
    else {
        viewport.x      = 0.0F;
        viewport.y      = 0.0F;
        viewport.width  = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        scissor.offset  = VkOffset2D{ 0, 0 };
        scissor.extent  = VkExtent2D{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
    }
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;

    std::uint64_t recorded       = 0;
    VkPipeline    bound_pipeline = VK_NULL_HANDLE;

    const auto fail = [&reason, &category_ref](vn::String text, vn::graphics::DiagnosticCategory kind) {
        if (reason.empty()) {
            reason       = std::move(text);
            category_ref = kind;
        }
    };
    const auto note = [&fail](const char8_t* text) { fail(vn::String(text), vn::graphics::DiagnosticCategory::ContentSkipped); };

    for (const vn::graphics::RenderCommand& command : *draw.commands) {
        const vn::graphics::ShaderProgramDescriptionPtr program =
            pass.program_override_description != nullptr ? pass.program_override_description : command.programDescription();
        const vn::graphics::ShaderProgramDescription* program_description = program != nullptr ? program.get() : default_program;
        if (program_description == nullptr) {
            note(u8"vk: a content draw named no program and the device has no default content program");
            continue;
        }
        vn::String          failure;
        Impl::ProgramEntry* compiled = impl->ensureProgram(program_description, failure);
        if (compiled == nullptr) {
            fail(failure.empty() ? vn::String(u8"vk: the content program could not be compiled") : std::move(failure),
                 vn::graphics::DiagnosticCategory::CompileFailed);
            continue;
        }

        const vn::graphics::GeometryDescriptionPtr geometry = command.geometryDescription();
        if (geometry == nullptr) {
            note(u8"vk: a content draw carried no geometry description");
            continue;
        }
        const std::span<const vn::graphics::Primitive> primitives = geometry->primitives();
        if (command.primitive_index >= primitives.size()) {
            note(u8"vk: a content draw named a primitive its geometry does not have");
            continue;
        }
        Impl::GeometryEntry* streams = impl->ensureGeometry(*geometry, failure);
        if (streams == nullptr) {
            fail(failure.empty() ? vn::String(u8"vk: the geometry's streams could not be uploaded") : std::move(failure),
                 vn::graphics::DiagnosticCategory::ContentSkipped);
            continue;
        }

        const VkPipeline pipeline = impl->ensurePipeline(*compiled, *geometry, primitives[command.primitive_index].topology, render_pass, failure);
        if (pipeline == VK_NULL_HANDLE) {
            fail(failure.empty() ? vn::String(u8"vk: the content pipeline could not be built") : std::move(failure),
                 vn::graphics::DiagnosticCategory::CompileFailed);
            continue;
        }

        VkBuffer       uniform_buffer = VK_NULL_HANDLE;
        VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
        if (!impl->createUniforms(view, buildDrawBlock(command), draw_offset, uniform_buffer, uniform_memory)) {
            note(u8"vk: a draw's uniform buffer could not be created");
            continue;
        }
        impl->frame_uniforms.emplace_back(uniform_buffer, uniform_memory);

        const VkDescriptorSet set = impl->makeSet(uniform_buffer, draw_offset);
        if (set == VK_NULL_HANDLE) {
            note(u8"vk: the descriptor pool ran out of sets for this frame");
            continue;
        }

        if (pipeline != bound_pipeline) {
            impl->table->vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound_pipeline = pipeline;
        }
        impl->table->vkCmdSetViewport(commands, 0U, 1U, &viewport);
        impl->table->vkCmdSetScissor(commands, 0U, 1U, &scissor);
        impl->table->vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, impl->pipeline_layout, 0U, 1U, &set, 0U, nullptr);

        std::vector<VkBuffer>      vertex_buffers;
        std::vector<VkDeviceSize>  vertex_offsets;
        vertex_buffers.reserve(streams->streams.size());
        vertex_offsets.reserve(streams->streams.size());
        for (const Impl::VertexStream& stream : streams->streams) {
            vertex_buffers.push_back(stream.buffer);
            vertex_offsets.push_back(0U);
        }
        if (!vertex_buffers.empty()) {
            impl->table->vkCmdBindVertexBuffers(commands, 0U, static_cast<std::uint32_t>(vertex_buffers.size()), vertex_buffers.data(), vertex_offsets.data());
        }

        const Impl::IndexStream* indices = nullptr;
        if (command.primitive_index < streams->indices.size() && streams->indices[command.primitive_index].buffer != VK_NULL_HANDLE) {
            indices = &streams->indices[command.primitive_index];
        }
        if (indices != nullptr) {
            impl->table->vkCmdBindIndexBuffer(commands, indices->buffer, 0U, VK_INDEX_TYPE_UINT32);
            impl->table->vkCmdDrawIndexed(commands, indices->count, 1U, 0U, 0, 0U);
            ++recorded;
        }
        else if (streams->vertex_count > 0U) {
            impl->table->vkCmdDraw(commands, streams->vertex_count, 1U, 0U, 0U);
            ++recorded;
        }
        else {
            note(u8"vk: a draw named neither an index stream nor vertices");
        }
    }
    return recorded;
}

VN_BACKEND_VK_NS_END
