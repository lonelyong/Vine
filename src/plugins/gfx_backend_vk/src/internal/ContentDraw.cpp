#include <vine/backend_vk/internal/ContentDraw.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vine/backend_vk/internal/ShaderCompiler.hpp>
#include <vine/backend_vk/internal/StateMapping.hpp>
#include <vine/backend_vk/internal/ContentImages.hpp>
#include <vine/backend_vk/internal/LightBlock.hpp>
#include <vine/backend_vk/internal/ProgramVariant.hpp>
#include <vine/Buffer.hpp>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/MaterialBase.hpp>
#include <vine/graphics/Primitive.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/StateResolution.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/// @brief The alignment used when the device reports none (Vulkan's minimum for uniform-buffer offsets).
constexpr std::uint32_t kDefaultUniformAlignment = 256U;

/// @brief Descriptor sets (and therefore draws) one frame may record before the pool is exhausted.
constexpr std::uint32_t kDescriptorsPerFrame = 256U;

/// @brief The content ABI's texture-slot count (see `TextureSlot`), i.e. set 0 bindings 1..5.
constexpr std::uint32_t kTextureSlots = vn::graphics::kTextureSlotCount;

/// @brief The content set's SHADOW bindings: the engine's forward ABI puts the map at 7 and its block at 8.
///
/// The engine's `forwardFragmentSource()` declares both UNCONDITIONALLY (its `vn_shadow.params.x` is the runtime
/// switch), so a backend that wants its own forward programs must offer the binding even while it casts no shadow.
constexpr std::uint32_t kShadowMapBinding   = 7U;
constexpr std::uint32_t kShadowBlockBinding = 8U;

/// @brief How a shadow map is read: nearest texels, clamped to the edge, no mip interpolation.
///
/// @details
/// - THE ENGINE'S TERM COMPARES ONE TEXEL (see BuiltinShaders' shadow term): filtering would average a caster's depth
///   with the surface behind it and move every silhouette, so the map is read exactly. The reference backend states the
///   same three fields for the same reason (`ContentPipeline::depthSampler`), which is what makes the two backends
///   sample the same value.
/// - Clamp rather than repeat is the other half: map coordinates are clamped into the rectangle by the shader's own
///   range test, and a wrapped read would sample the opposite edge if that test ever disagreed with the sampler.
constexpr vn::graphics::Sampler kShadowSampler{
    .min_filter   = vn::graphics::Sampler::Filter::Nearest,
    .mag_filter   = vn::graphics::Sampler::Filter::Nearest,
    .mipmap_mode  = vn::graphics::Sampler::MipmapMode::Nearest,
    .address_u    = vn::graphics::Sampler::AddressMode::ClampToEdge,
    .address_v    = vn::graphics::Sampler::AddressMode::ClampToEdge,
    .address_w    = vn::graphics::Sampler::AddressMode::ClampToEdge,
};

/// @brief The shadow block's bytes, in the engine ABI's shape (`VineShadowBlock` in builtin_forward.frag).
struct VineShadowBlock {
    std::array<float, 16> view_to_light{}; // View space -> light clip (the SDK's clip convention).
    std::array<float, 4>  params{};        // x = enabled, y = depth bias, z = strength, w = the light it belongs to.
};
static_assert(sizeof(VineShadowBlock) == 80U, "the shadow block is a mat4 row plus one vec4 row");

/// @brief The full-screen ABI's sampler bindings: bindings 0..4 (four colour attachments and depth).
constexpr std::uint32_t kScreenSamplerBindings = 5U;

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
 * @brief Copies a column-major double matrix into a column-major float array.
 *
 * @param source      The matrix (element (row, column) access).
 * @param destination The sixteen floats, column-major.
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
 * @brief Folds the SDK's clip space into the device's: y down, and z into the [0, 1] reverse-Z range.
 *
 * @details
 * - The SDK's camera builds an OpenGL-style projection (y up, near at clip z = -1, far at +1) - it is
 *   backend-neutral - while Vulkan defines framebuffer coordinates with the origin at the TOP-left and a depth
 *   range of [0, 1] that this backend reads with the GREATER comparison (see StateMapping::compareOp): a nearer
 *   fragment must own the LARGER stored value.
 * - One matrix carries both parts, exactly as the reference backend's `foldToDeviceClip` does: y is negated
 *   (diag(1, -1, 1, 1) keeps the picture and the front-face rule right, see StateMapping::kFrontFace) and z is
 *   mapped to `0.5 - 0.5 z` (near -1 -> 1, far +1 -> 0, matching the depth the pass clears to).
 * - FOLDING ONLY Y WAS A DEPTH BUG: the fragments' stored depths kept the OpenGL direction, so with GREATER the
 *   FARTHER fragment won whenever two overlapped. This backend's own tests had stated their depths in the folded
 *   direction, which is why they could not see it; the parity suite, which hands the SAME frame to both backends,
 *   did (M7).
 *
 * @param projection The camera's clip-space projection.
 * @return The same projection in the device's clip convention.
 */
[[nodiscard]] vn::math::Mat4d foldToDeviceClip(const vn::math::Mat4d& projection) noexcept
{
    vn::math::Mat4d fold{}; // identity: x and w are the same in both conventions
    fold(1, 1) = -1.0;      // the device's NDC is y-down
    fold(2, 2) = -0.5;      // reverse-Z: the SDK's near (-1) becomes 1, its far (+1) becomes 0
    fold(2, 3) = 0.5;
    return fold * projection;
}

/**
 * @brief The 128-byte push range the engine's content programs read (`vine_params.glsl`).
 *
 * @details
 * - `pc.projection == VineViewBlock.proj` (the pass' projection, folded into the device's clip convention) and
 *   `pc.modelView == VineViewBlock.view * VineDrawBlock.model`: the same two values a block-reading program finds in
 *   the view/draw pair, spelled for the range the built-in stages declare.
 */
struct ContentPushConstants {
    std::array<float, 16> projection{}; // View -> clip, column-major, in the device's clip convention.
    std::array<float, 16> model_view{}; // World -> view, column-major (the pass' view times the drawable's model).
};
static_assert(sizeof(ContentPushConstants) == 128U, "the content push range is two mat4");

/**
 * @brief Fills the push range from a pass' camera and one drawable's model matrix.
 *
 * @param camera The pass' camera snapshot (its projection is folded, see foldToDeviceClip).
 * @param model  The drawable's world transform.
 * @return The range's bytes.
 */
[[nodiscard]] ContentPushConstants pushConstantsOf(const vn::graphics::CameraSnapshot& camera, const vn::math::Mat4d& model) noexcept
{
    ContentPushConstants push{};
    copyMatrix(foldToDeviceClip(camera.projection), push.projection);
    copyMatrix(camera.view * model, push.model_view);
    return push;
}

/**
 * @brief Packs a material's values into the shading block (see ShaderAbi.hpp).
 *
 * @details
 * - A command that names no material uses the block's DEFAULTS, which are the default material's values - content
 *   without a material must look like content with a default one.
 * - `texture_flags` states which slots the caller could actually bind (a complete texture was uploaded); those are the
 *   bits the shading reads to decide whether a slot is sampled or falls back to its constant.
 *
 * @param values        The resolved values, or null for the default material.
 * @param texture_flags Bit 0-4 = the slots that carry an uploaded map.
 * @return The block.
 */
[[nodiscard]] vn::graphics::VineMaterialBlock buildMaterialBlock(const vn::graphics::MaterialParameters* values, std::uint32_t texture_flags) noexcept
{
    vn::graphics::VineMaterialBlock block{}; // its member defaults are the default material
    if (values != nullptr) {
        block.base_color = { values->base_color.r, values->base_color.g, values->base_color.b, values->base_color.a };
        block.emissive   = { values->emissive.r, values->emissive.g, values->emissive.b, values->emissive.a };
        block.params     = { values->metallic, values->roughness, values->occlusion, values->alpha_cutoff };
        block.flags[0]   = texture_flags | (static_cast<std::uint32_t>(values->alpha_mode) << vn::graphics::kMaterialAlphaModeShift);
    }
    return block;
}

/**
 * @brief Fills the per-draw block a content program reads (see ShaderAbi.hpp).
 *
 * @param command   The draw command.
 * @param point_size The resolved point size (`params.y`; the content stages write it to `gl_PointSize`).
 * @return The block: the model matrix, the effective opacity and the point size.
 */
[[nodiscard]] vn::graphics::VineDrawBlock buildDrawBlock(const vn::graphics::RenderCommand& command, float point_size) noexcept
{
    vn::graphics::VineDrawBlock block{};
    copyMatrix(command.model_matrix, block.model);
    block.params = { command.opacity, point_size, 0.0F, 0.0F };
    return block;
}

/**
 * @brief Packs the shadow block one drawing call shades with (see ShaderAbi.hpp `VineShadowBlock`).
 *
 * @details
 * - THE MAP'S IDENTITY IS SOMEBODY ELSE'S STATEMENT: the target says whose shadow it is (`RenderTarget::shadowOf`) and
 *   the producer published how to read it (`setProducerViewProjection`). This function only checks whether that map can
 *   shade THIS call - it never infers a map from an input's depth.
 * - FOUR FACTS DECIDE THE SWITCH, and each of them can turn it off on its own: no map, no published matrix, no view to
 *   map from, or a map whose light this call did not announce as an enabled caster of a kind that has a map
 *   (directional or spot). The light must also have taken a slot in the light block, because `params.w` names the
 *   light whose term the map scales - a slot the block cannot carry would scale somebody else's.
 * - THE MATRIX IS `light clip <- world` TIMES `world <- this view`: a fragment's view position is what the shading
 *   has, and the map's space is where it must be compared. It stays in the SDK's clip convention (y up, z in [-1, 1]):
 *   the shader's own conversion to the map's texel space and reverse-Z depth is the ABI's statement, and a matrix
 *   folded here would be a second one.
 * - `params` is {enabled, the casting light's OWN bias, strength 1, its slot}: the bias comes from `ShadowSettings`
 *   (a private default would be the second answer this ABI exists to prevent), and the SDK has no strength knob yet.
 *
 * @param shadow The map this pass samples (see ContentDraw::ShadowSource), or an empty one for none.
 * @param draw   The drawing call: its camera (the view space to map from) and the lights it announced.
 * @return The block; every field is written and the switch is off when the map cannot shade this call.
 */
[[nodiscard]] VineShadowBlock buildShadowBlock(const ContentDraw::ShadowSource& shadow, const vn::graphics::DrawDescription& draw) noexcept
{
    VineShadowBlock block{};
    if (shadow.light == nullptr || !shadow.has_view_projection || shadow.view == VK_NULL_HANDLE || !draw.camera.present ||
        !draw.light_values.has_value())
    {
        return block;
    }

    const vn::graphics::LightValue* caster = nullptr;
    for (const vn::graphics::LightValue& light : *draw.light_values) {
        if (light.identity == shadow.light) {
            caster = &light;
            break;
        }
    }
    if (caster == nullptr || !caster->enabled || !caster->cast_shadow ||
        (caster->type != vn::graphics::LightType::Directional && caster->type != vn::graphics::LightType::Spot))
    {
        return block;
    }

    const std::size_t slot = packedSlotOf(*draw.light_values, shadow.light);
    if (slot >= vn::graphics::kMaxLights) {
        return block;
    }

    const vn::math::Mat4d view_to_light = shadow.view_projection * draw.camera.view.inverted();
    copyMatrix(view_to_light, block.view_to_light);
    block.params = { 1.0F, caster->shadow_bias, 1.0F, static_cast<float>(slot) };
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

    /// @brief One uploaded vertex channel.
    struct VertexStream {
        VkBuffer      buffer{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        std::uint32_t location{ 0 };
        std::uint32_t components{ 0 };
        std::uint64_t bytes{ 0 };          // Bytes copied in (the entry's residency weight).
        const void*   source{ nullptr };    // The `Buffer` this stream was uploaded from (RVALUE identity only).
    };

    /// @brief One uploaded index stream (per primitive).
    struct IndexStream {
        VkBuffer       buffer{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        std::uint32_t  count{ 0 };
        std::uint64_t  bytes{ 0 };       // Bytes copied in (the entry's residency weight).
        const void*    source{ nullptr }; // The `Buffer` this stream was uploaded from (identity only).
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
        VkBuffer       buffer{ VK_NULL_HANDLE };
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        std::uint64_t  bytes{ 0 };    // Bytes copied in (the entry's residency weight).
        std::uint64_t  revision{ 0 }; // The Buffer revision the bytes came from.
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
        vn::graphics::backend::RenderPassCompatibility         compatibility{};
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
            if (compatibility.subpass != other.compatibility.subpass) {
                return compatibility.subpass < other.compatibility.subpass;
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
    VkDevice                        device{ VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::uint32_t                   uniform_alignment{ kDefaultUniformAlignment };

    /// The set layouts and pipeline layouts the two ABIs use: the content pair (set 0 material/slots/lights, set 1
    /// draw) and the full-screen set (the source's attachments at 0..N-1, lights at 6).
    VkDescriptorSetLayout content_set_layout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout draw_set_layout{ VK_NULL_HANDLE };
    VkDescriptorSetLayout screen_set_layout{ VK_NULL_HANDLE };
    VkPipelineLayout      content_pipeline_layout{ VK_NULL_HANDLE };
    VkPipelineLayout      screen_pipeline_layout{ VK_NULL_HANDLE };
    VkDescriptorPool      descriptor_pool{ VK_NULL_HANDLE };

    /// The engine's full-screen vertex stage (the fragment stage of a screen draw is the draw's own program).
    vn::graphics::ShaderProgramDescriptionPtr screen_vertex{};
    /// The sampled images and samplers the material slots read.
    ContentImages images{};

    std::unordered_map<ProgramKey, ProgramEntry, ProgramKeyHash>                     programs;
    std::unordered_map<const vn::graphics::GeometryDescription*, GeometryEntry>     geometries;
    /// One entry per INSTANCE buffer a command read (keyed by the buffer's address, see InstanceStream).
    std::unordered_map<const vn::Buffer<float>*, InstanceStream>                    instance_streams;
    std::map<PipelineKey, VkPipeline>                                               pipelines;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>>                                frame_uniforms;
    /// How many pipelines were CREATED (a resize must leave this unmoved; see pipelinesCreated).
    std::uint64_t created_{ 0 };

    /// M6's books (see Progress): cumulative builds and releases; the live totals are counted from the tables, so a
    /// hole a release left is never reported as resident.
    std::uint64_t stream_builds_{ 0 };
    std::uint64_t stream_bytes_{ 0 };
    std::uint64_t stream_releases_{ 0 };
    std::uint64_t program_builds_{ 0 };
    std::uint64_t pipeline_releases_{ 0 };

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

    /** @brief Byte offset of the light block inside one draw's uniform buffer (material starts at 0). */
    [[nodiscard]] VkDeviceSize lightsOffset() const noexcept
    {
        return alignedUp(sizeof(vn::graphics::VineMaterialBlock), uniform_alignment);
    }

    /** @brief Byte offset of the draw block inside one draw's uniform buffer. */
    [[nodiscard]] VkDeviceSize drawOffset() const noexcept
    {
        return alignedUp(lightsOffset() + sizeof(VineLightsBlock), uniform_alignment);
    }

    /** @brief Byte offset of the shadow block inside one draw's uniform buffer. */
    [[nodiscard]] VkDeviceSize shadowOffset() const noexcept
    {
        return alignedUp(drawOffset() + sizeof(vn::graphics::VineDrawBlock), uniform_alignment);
    }

    /** @brief Size of one draw's uniform buffer: material, lights, draw and the (unused-for-now) shadow block. */
    [[nodiscard]] VkDeviceSize uniformSize() const noexcept
    {
        return shadowOffset() + sizeof(VineShadowBlock);
    }

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
        destroyBuffer(entry.buffer, entry.memory); // a new revision replaces the bytes
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
     * @brief Gets (or uploads) one geometry revision's streams.
     *
     * @details
     * - A REPEAT CALL PAYS NOTHING when every stream is resident, and repairs the HOLES a host release left: a
     *   release drops the buffer of the streams it named while the entry - and its other streams - stay, so the
     *   next frame uploads exactly what left (`RenderDevice::releaseContent(Buffer)`).
     * - A REFUSED BUILD IS REMEMBERED: an entry that could not be uploaded answers null from then on, so a broken
     *   geometry is reported once instead of failing the same way every frame.
     *
     * @param description The geometry's shape as a value (the entry keeps it alive).
     * @param why         Receives the first failure's reason.
     * @return The entry, or null when a stream could not be uploaded.
     */
    [[nodiscard]] GeometryEntry* ensureGeometry(const vn::graphics::GeometryDescription& description, vn::String& why)
    {
        const auto found = geometries.find(&description);
        if (found != geometries.end() && found->second.failed) {
            return nullptr;
        }

        GeometryEntry* entry = nullptr;
        if (found == geometries.end()) {
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
            entry = &geometries.emplace(&description, std::move(fresh)).first->second;
        }
        else {
            entry = &found->second;
        }

        std::size_t slot = 0;
        for (const vn::graphics::GeometryChannel& channel : description.channels()) {
            const std::span<const float> scalars = channel.channel.scalars();
            if (scalars.empty()) {
                continue;
            }
            VertexStream& stream = entry->streams[slot];
            if (stream.buffer == VK_NULL_HANDLE) {
                const std::uint32_t components = channel.channel.components;
                if (vertexFormatOf(components) == VK_FORMAT_UNDEFINED) {
                    why = vn::String(u8"vk: an attribute channel states a component count Vulkan has no format for");
                    return failGeometry(*entry, std::move(why));
                }
                stream.location   = channel.location;
                stream.components = components;
                stream.source     = channel.channel.values.get();
                if (!upload(std::as_bytes(scalars), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, stream.buffer, stream.memory)) {
                    why = vn::String(u8"vk: a vertex channel could not be uploaded");
                    return failGeometry(*entry, std::move(why));
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
        entry->vertex_count = smallest == std::numeric_limits<std::uint32_t>::max() ? 0U : smallest;

        const std::span<const vn::graphics::Primitive> primitives = description.primitives();
        for (std::size_t index = 0; index < primitives.size(); ++index) {
            const std::span<const std::uint32_t> indices = primitives[index].indices.span();
            if (indices.empty()) {
                continue;
            }
            IndexStream& stream = entry->indices[index];
            if (stream.buffer != VK_NULL_HANDLE) {
                continue;
            }
            stream.source = primitives[index].indices.values.get();
            if (!upload(std::as_bytes(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, stream.buffer, stream.memory)) {
                why = vn::String(u8"vk: an index stream could not be uploaded");
                return failGeometry(*entry, std::move(why));
            }
            stream.count = static_cast<std::uint32_t>(indices.size());
            stream.bytes = static_cast<std::uint64_t>(indices.size_bytes());
            ++stream_builds_;
            stream_bytes_ += stream.bytes;
        }
        return entry;
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
        destroyGeometryStreams(entry);
        return nullptr;
    }

    /** @brief Destroys every stream of @p entry and counts the releases (the entry itself stays). */
    void destroyGeometryStreams(GeometryEntry& entry) noexcept
    {
        for (VertexStream& stream : entry.streams) {
            if (stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            destroyBuffer(stream.buffer, stream.memory);
            ++stream_releases_;
        }
        for (IndexStream& stream : entry.indices) {
            if (stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            destroyBuffer(stream.buffer, stream.memory);
            ++stream_releases_;
        }
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

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (table->vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U, &info, nullptr, &pipeline) != VK_SUCCESS) {
            why = vn::String(u8"vk: the content pipeline could not be built (see the loader's report)");
            return VK_NULL_HANDLE;
        }
        ++created_;
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
                                                   const vn::graphics::backend::RenderPassCompatibility& compatibility,
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
                                                  const vn::graphics::backend::RenderPassCompatibility& compatibility,
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
     * @brief What one scene drawing call needs before any command can be recorded: its program, its streams and the
     *        pipeline its state selects.
     *
     * @details
     * - THE ONE RESOLUTION @ref record AND @ref ContentDraw::prepare SHARE: "what does this command draw with" is
     *   answered here once, so a warm-up cannot build something the frame would not ask for (and vice versa).
     */
    struct CommandPlan {
        ProgramEntry*                        program{ nullptr };
        GeometryEntry*                       streams{ nullptr };
        vn::graphics::MaterialDescriptionPtr material{};     // the material it is shaded with, or null (defaults)
        InstanceStream*                      instances{ nullptr }; // the instance stream it reads, or null
        ProgramVariant                       variant{};      // what the program was compiled under
        VkPipeline                           pipeline{ VK_NULL_HANDLE };
        vn::graphics::ResolvedCommandState   resolved{};
    };

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
     * @param why               Receives the reason a command could not be resolved.
     * @param category          Receives that reason's category (CompileFailed for a build, ContentSkipped otherwise).
     * @return true when the command can be recorded.
     */
    [[nodiscard]] bool planCommand(const vn::graphics::RenderCommand&                     command,
                                   const vn::graphics::PassDraw&                          pass,
                                   const vn::graphics::ShaderProgramDescription*          default_program,
                                   const vn::graphics::backend::RenderPassCompatibility&  compatibility,
                                   VkRenderPass                                           render_pass,
                                   std::uint32_t                                          color_attachments,
                                   CommandPlan&                                           plan,
                                   vn::String&                                            why,
                                   vn::graphics::DiagnosticCategory&                       category)
    {
        const vn::graphics::ShaderProgramDescriptionPtr program =
            pass.program_override_description != nullptr ? pass.program_override_description : command.programDescription();
        const vn::graphics::ShaderProgramDescription* program_description = program != nullptr ? program.get() : default_program;
        if (program_description == nullptr) {
            why      = vn::String(u8"vk: a content draw named no program and the device has no default content program");
            category = vn::graphics::DiagnosticCategory::ContentSkipped;
            return false;
        }

        const vn::graphics::GeometryDescriptionPtr geometry = command.geometryDescription();
        if (geometry == nullptr) {
            why      = vn::String(u8"vk: a content draw carried no geometry description");
            category = vn::graphics::DiagnosticCategory::ContentSkipped;
            return false;
        }
        const std::span<const vn::graphics::Primitive> primitives = geometry->primitives();
        if (command.primitive_index >= primitives.size()) {
            why      = vn::String(u8"vk: a content draw named a primitive its geometry does not have");
            category = vn::graphics::DiagnosticCategory::ContentSkipped;
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
            why      = build_why.empty() ? vn::String(u8"vk: the content program could not be compiled") : std::move(build_why);
            category = vn::graphics::DiagnosticCategory::CompileFailed;
            return false;
        }
        GeometryEntry* const streams = ensureGeometry(*geometry, build_why);
        if (streams == nullptr) {
            why      = build_why.empty() ? vn::String(u8"vk: the geometry's streams could not be uploaded") : std::move(build_why);
            category = vn::graphics::DiagnosticCategory::ContentSkipped;
            return false;
        }
        InstanceStream* instances = nullptr;
        if (instanced) {
            instances = ensureInstanceStream(*command.instance_transforms.values, build_why);
            if (instances == nullptr) {
                why      = build_why.empty() ? vn::String(u8"vk: the instance stream could not be uploaded") : std::move(build_why);
                category = vn::graphics::DiagnosticCategory::ContentSkipped;
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
            why      = pipeline_why.empty() ? vn::String(u8"vk: the content pipeline could not be built") : std::move(pipeline_why);
            category = vn::graphics::DiagnosticCategory::CompileFailed;
            return false;
        }
        plan.program   = compiled;
        plan.streams   = streams;
        plan.material  = std::move(material);
        plan.instances = instances;
        plan.variant   = variant;
        return true;
    }

    /**
     * @brief What one screen drawing call needs before it can be recorded: its fragment stage, the composed pipeline
     *        and the state that selected it.
     */
    struct ScreenPlan {
        ProgramEntry*                      fragment{ nullptr };
        VkPipeline                         pipeline{ VK_NULL_HANDLE };
        vn::graphics::ResolvedCommandState resolved{};
    };

    /**
     * @brief Resolves one screen draw's fragment program and pipeline (see @ref planCommand for the shared idea).
     *
     * @param draw              The screen draw (its fragment program is read).
     * @param pass              The pass drawing it (state is read).
     * @param compatibility     The destination's pipeline compatibility.
     * @param render_pass       A render pass with that compatibility (used only when a pipeline must be CREATED).
     * @param color_attachments How many colour attachments the destination writes.
     * @param plan              Receives what was resolved.
     * @param why               Receives the reason the draw could not be resolved.
     * @param category          Receives that reason's category.
     * @return true when the draw can be recorded.
     */
    [[nodiscard]] bool planScreen(const vn::graphics::DrawDescription&                  draw,
                                  const vn::graphics::PassDraw&                          pass,
                                  const vn::graphics::backend::RenderPassCompatibility&  compatibility,
                                  VkRenderPass                                           render_pass,
                                  std::uint32_t                                          color_attachments,
                                  ScreenPlan&                                            plan,
                                  vn::String&                                            why,
                                  vn::graphics::DiagnosticCategory&                       category)
    {
        // THE PROGRAM'S TEXT: the value the frame carries, else the live program's cached description.
        vn::graphics::ShaderProgramDescriptionPtr fragment_description = draw.program_description;
        if (fragment_description == nullptr && draw.program != nullptr) {
            fragment_description = draw.program->description();
        }
        if (fragment_description == nullptr) {
            why      = vn::String(u8"vk: a screen draw named no fragment program");
            category = vn::graphics::DiagnosticCategory::ContentSkipped;
            return false;
        }

        vn::String                build_why;
        ProgramEntry* const       fragment = ensureProgram(fragment_description.get(), ProgramVariant{}, build_why);
        if (fragment == nullptr) {
            why      = build_why.empty() ? vn::String(u8"vk: the screen fragment program could not be compiled") : std::move(build_why);
            category = vn::graphics::DiagnosticCategory::CompileFailed;
            return false;
        }
        ProgramEntry* const vertex = ensureProgram(screen_vertex.get(), ProgramVariant{}, build_why);
        if (vertex == nullptr) {
            why      = build_why.empty() ? vn::String(u8"vk: the full-screen vertex program could not be compiled") : std::move(build_why);
            category = vn::graphics::DiagnosticCategory::CompileFailed;
            return false;
        }

        // THE STATE, resolved from the pass (a full-screen call has no per-command state of its own).
        plan.resolved = vn::graphics::resolveRenderState(vn::graphics::RenderState{}, pass.state, vn::graphics::Topology::Triangles);
        vn::String pipeline_why;
        plan.pipeline = ensureScreenPipeline(*fragment,
                                             *vertex,
                                             compatibility,
                                             render_pass,
                                             pipelineStateOf(plan.resolved, color_attachments),
                                             color_attachments,
                                             pipeline_why);
        if (plan.pipeline == VK_NULL_HANDLE) {
            why      = pipeline_why.empty() ? vn::String(u8"vk: the screen pipeline could not be built") : std::move(pipeline_why);
            category = vn::graphics::DiagnosticCategory::CompileFailed;
            return false;
        }
        plan.fragment = fragment;
        return true;
    }

    /**
     * @brief Allocates one descriptor set from @p layout.
     *
     * @param layout The set layout the set must agree with.
     * @return The set, or `VK_NULL_HANDLE` when the pool is exhausted.
     */
    [[nodiscard]] VkDescriptorSet allocateSet(VkDescriptorSetLayout layout) const
    {
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool     = descriptor_pool;
        allocate.descriptorSetCount = 1U;
        allocate.pSetLayouts        = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (table->vkAllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return set;
    }

    /**
     * @brief Writes a content draw's set 0: material at binding 0, the five slots at 1-5, lights at 6.
     *
     * @param buffer The draw's uniform buffer (material at 0, lights at @ref lightsOffset).
     * @param slots  One image info per texture slot, in `TextureSlot` order.
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet makeContentSet(VkBuffer buffer,
                                                 const std::array<VkDescriptorImageInfo, kTextureSlots>& slots,
                                                 VkImageView shadow_view,
                                                 VkSampler   shadow_sampler) const
    {
        const VkDescriptorSet set = allocateSet(content_set_layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }

        VkDescriptorBufferInfo material_info{};
        material_info.buffer = buffer;
        material_info.offset = 0U;
        material_info.range  = sizeof(vn::graphics::VineMaterialBlock);

        VkDescriptorBufferInfo lights_info{};
        lights_info.buffer = buffer;
        lights_info.offset = lightsOffset();
        lights_info.range  = sizeof(VineLightsBlock);

        // THE SHADOW ABI (see kShadowMapBinding): the engine's forward programs declare both, so every content set binds
        // them - the map the pass samples, or the white stand-in when it samples none (see buildShadowBlock for what
        // turns the map into a shadow).
        VkDescriptorImageInfo shadow_map{};
        shadow_map.sampler     = shadow_sampler;
        shadow_map.imageView   = shadow_view;
        shadow_map.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo shadow_info{};
        shadow_info.buffer = buffer;
        shadow_info.offset = shadowOffset();
        shadow_info.range  = sizeof(VineShadowBlock);

        VkWriteDescriptorSet writes[5]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0U;
        writes[0].descriptorCount = 1U;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo     = &material_info;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1U;
        writes[1].descriptorCount = kTextureSlots;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = slots.data();
        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = set;
        writes[2].dstBinding      = 6U;
        writes[2].descriptorCount = 1U;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo     = &lights_info;
        writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet          = set;
        writes[3].dstBinding      = kShadowMapBinding;
        writes[3].descriptorCount = 1U;
        writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3].pImageInfo      = &shadow_map;
        writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet          = set;
        writes[4].dstBinding      = kShadowBlockBinding;
        writes[4].descriptorCount = 1U;
        writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo     = &shadow_info;
        table->vkUpdateDescriptorSets(device, 5U, writes, 0U, nullptr);
        return set;
    }

    /**
     * @brief Writes a content draw's set 1: the per-draw block at binding 0.
     *
     * @param buffer The draw's uniform buffer (draw at @ref drawOffset).
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet makeDrawSet(VkBuffer buffer) const
    {
        const VkDescriptorSet set = allocateSet(draw_set_layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }

        VkDescriptorBufferInfo draw_info{};
        draw_info.buffer = buffer;
        draw_info.offset = drawOffset();
        draw_info.range  = sizeof(vn::graphics::VineDrawBlock);

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0U;
        write.descriptorCount = 1U;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo     = &draw_info;
        table->vkUpdateDescriptorSets(device, 1U, &write, 0U, nullptr);
        return set;
    }

    /**
     * @brief Writes a screen draw's set 0: the source attachments at 0..4, the lights at 6.
     *
     * @param buffer The draw's uniform buffer (lights at @ref lightsOffset).
     * @param slots  One image info per source/depth binding, in binding order; absent ones carry the fallback.
     * @return The set, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkDescriptorSet makeScreenSet(VkBuffer buffer, const std::array<VkDescriptorImageInfo, kTextureSlots>& slots) const
    {
        const VkDescriptorSet set = allocateSet(screen_set_layout);
        if (set == VK_NULL_HANDLE) {
            return set;
        }

        VkDescriptorBufferInfo lights_info{};
        lights_info.buffer = buffer;
        lights_info.offset = lightsOffset();
        lights_info.range  = sizeof(VineLightsBlock);

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0U;
        writes[0].descriptorCount = kTextureSlots;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = slots.data();
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 6U;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo     = &lights_info;
        table->vkUpdateDescriptorSets(device, 2U, writes, 0U, nullptr);
        return set;
    }

    /**
     * @brief Creates the per-draw uniform buffer and writes the material, lights and draw blocks at their offsets.
     *
     * @param material The material block (offset 0).
     * @param lights   The light block (offset @ref lightsOffset).
     * @param draw     The per-draw block (offset @ref drawOffset).
     * @param buffer   Receives the buffer.
     * @param memory   Receives the memory.
     * @return true when the bytes are in the buffer.
     */
    [[nodiscard]] bool createDrawUniforms(const vn::graphics::VineMaterialBlock& material,
                                          const VineLightsBlock&                 lights,
                                          const vn::graphics::VineDrawBlock&     draw,
                                          const VineShadowBlock&                 shadow,
                                          VkBuffer&                              buffer,
                                          VkDeviceMemory&                        memory) const
    {
        const VkDeviceSize size = uniformSize();
        if (!createHostBuffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, buffer, memory)) {
            return false;
        }
        void* mapped = nullptr;
        if (table->vkMapMemory(device, memory, 0U, size, 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
            destroyBuffer(buffer, memory);
            return false;
        }
        auto* bytes = static_cast<std::byte*>(mapped);
        std::memcpy(bytes, &material, sizeof(material));
        std::memcpy(bytes + lightsOffset(), &lights, sizeof(lights));
        std::memcpy(bytes + drawOffset(), &draw, sizeof(draw));
        // THE SHADOW BLOCK is the call's own statement (see buildShadowBlock); its `params.x = 0` is the engine's "no
        // shadow reached this pass" switch, which is what a pass that samples no map states.
        std::memcpy(bytes + shadowOffset(), &shadow, sizeof(shadow));
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
            destroyGeometryStreams(entry);
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

    // THE CONTENT SET (set 0): material at 0, the five slots at 1-5, lights at 6 - the engine's arrangement, which is
    // also the one vine_params.glsl declares.
    VkDescriptorSetLayoutBinding content_bindings[9]{};
    content_bindings[0].binding         = 0U;
    content_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
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
    content_bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    content_bindings[6].descriptorCount = 1U;
    content_bindings[6].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // THE ENGINE'S SHADOW ABI (see kShadowMapBinding): a forward program declares both, so the layout offers both -
    // fragment stages only, which is where the engine's shading reads them.
    content_bindings[7].binding         = kShadowMapBinding;
    content_bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    content_bindings[7].descriptorCount = 1U;
    content_bindings[7].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    content_bindings[8].binding         = kShadowBlockBinding;
    content_bindings[8].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    content_bindings[8].descriptorCount = 1U;
    content_bindings[8].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 9U;
    layout_info.pBindings    = content_bindings;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->content_set_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    // THE DRAW SET (set 1): the per-draw block at binding 0.
    VkDescriptorSetLayoutBinding draw_binding{};
    draw_binding.binding         = 0U;
    draw_binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    draw_binding.descriptorCount = 1U;
    draw_binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_info.bindingCount = 1U;
    layout_info.pBindings    = &draw_binding;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->draw_set_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    // THE FULL-SCREEN SET (set 0 of a screen program): the source's colour attachments at 0..4 and the lights at 6.
    VkDescriptorSetLayoutBinding screen_bindings[6]{};
    for (std::uint32_t slot = 0; slot < kScreenSamplerBindings; ++slot) {
        screen_bindings[slot].binding         = slot;
        screen_bindings[slot].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        screen_bindings[slot].descriptorCount = 1U;
        screen_bindings[slot].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    screen_bindings[5].binding         = 6U;
    screen_bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    screen_bindings[5].descriptorCount = 1U;
    screen_bindings[5].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    layout_info.bindingCount = 6U;
    layout_info.pBindings    = screen_bindings;
    if (impl->table->vkCreateDescriptorSetLayout(impl->device, &layout_info, nullptr, &impl->screen_set_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    // THE CONTENT LAYOUT: set 0 + set 1, and the 128-byte camera push every built-in content stage reads.
    const VkDescriptorSetLayout content_sets[2]{ impl->content_set_layout, impl->draw_set_layout };
    VkPushConstantRange         push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0U;
    push.size       = sizeof(ContentPushConstants);

    VkPipelineLayoutCreateInfo pipeline_info{};
    pipeline_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_info.setLayoutCount         = 2U;
    pipeline_info.pSetLayouts            = content_sets;
    pipeline_info.pushConstantRangeCount = 1U;
    pipeline_info.pPushConstantRanges    = &push;
    if (impl->table->vkCreatePipelineLayout(impl->device, &pipeline_info, nullptr, &impl->content_pipeline_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    // THE SCREEN LAYOUT: set 0 only (a full-screen program reads no camera push).
    pipeline_info.setLayoutCount         = 1U;
    pipeline_info.pSetLayouts            = &impl->screen_set_layout;
    pipeline_info.pushConstantRangeCount = 0U;
    pipeline_info.pPushConstantRanges    = nullptr;
    if (impl->table->vkCreatePipelineLayout(impl->device, &pipeline_info, nullptr, &impl->screen_pipeline_layout) != VK_SUCCESS) {
        destroy();
        return false;
    }

    // ONE POOL FOR BOTH PATHS: a content draw allocates two sets (3 UBOs + 5 samplers), a screen draw one (1 UBO +
    // 5 samplers).
    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    // material, lights and the shadow block per content set; the draw block per draw set.
    pool_sizes[0].descriptorCount = kDescriptorsPerFrame * 4U;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // five material slots + the shadow map per content set; five sampled inputs per screen set.
    pool_sizes[1].descriptorCount = kDescriptorsPerFrame * (2U * kTextureSlots + 1U);

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = kDescriptorsPerFrame * 2U;
    pool_info.poolSizeCount = 2U;
    pool_info.pPoolSizes    = pool_sizes;
    if (impl->table->vkCreateDescriptorPool(impl->device, &pool_info, nullptr, &impl->descriptor_pool) != VK_SUCCESS) {
        destroy();
        return false;
    }

    if (!impl->images.create(table, device, memory)) {
        destroy();
        return false;
    }

    // The engine's full-screen vertex stage: the interface every screen fragment stage is written against.
    const vn::intrusive_ptr<vn::graphics::ShaderProgram> fullscreen = vn::graphics::fullscreenVertexProgram();
    impl->screen_vertex = fullscreen != nullptr ? fullscreen->description() : vn::graphics::ShaderProgramDescriptionPtr{};
    return impl->screen_vertex != nullptr;
}

void ContentDraw::destroy() noexcept
{
    if (!impl) {
        return;
    }
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        impl->images.destroy();
        impl->destroyFrameUniforms();
        impl->destroyPipelines();
        impl->destroyPrograms();
        impl->destroyGeometries();
        impl->destroyInstanceStreams();
        if (impl->descriptor_pool != VK_NULL_HANDLE) {
            impl->table->vkDestroyDescriptorPool(impl->device, impl->descriptor_pool, nullptr);
            impl->descriptor_pool = VK_NULL_HANDLE;
        }
        if (impl->content_pipeline_layout != VK_NULL_HANDLE) {
            impl->table->vkDestroyPipelineLayout(impl->device, impl->content_pipeline_layout, nullptr);
            impl->content_pipeline_layout = VK_NULL_HANDLE;
        }
        if (impl->screen_pipeline_layout != VK_NULL_HANDLE) {
            impl->table->vkDestroyPipelineLayout(impl->device, impl->screen_pipeline_layout, nullptr);
            impl->screen_pipeline_layout = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout* layout : { &impl->content_set_layout, &impl->draw_set_layout, &impl->screen_set_layout }) {
            if (*layout != VK_NULL_HANDLE) {
                impl->table->vkDestroyDescriptorSetLayout(impl->device, *layout, nullptr);
                *layout = VK_NULL_HANDLE;
            }
        }
    }
    impl->table  = nullptr;
    impl->device = VK_NULL_HANDLE;
    impl->memory_properties = VkPhysicalDeviceMemoryProperties{};
    impl->screen_vertex = nullptr;
    impl->stream_builds_      = 0;
    impl->stream_bytes_       = 0;
    impl->stream_releases_    = 0;
    impl->program_builds_     = 0;
    impl->pipeline_releases_  = 0;
    impl->created_            = 0;
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
        impl->images.releaseFrame();
    }
}

std::uint64_t ContentDraw::pipelinesCreated() const noexcept
{
    return impl->created_;
}

ContentDraw::Progress ContentDraw::progress() const noexcept
{
    Progress out;
    out.stream_builds   = impl->stream_builds_;
    out.stream_bytes    = impl->stream_bytes_;
    out.stream_releases = impl->stream_releases_;
    out.program_builds  = impl->program_builds_;
    out.pipeline_builds = impl->created_;
    out.pipeline_releases = impl->pipeline_releases_;
    out.pipelines_live    = static_cast<std::uint32_t>(impl->pipelines.size());
    // LIVE, FROM THE TABLES: a hole a release left has a null buffer and is not resident.
    for (const auto& [description, entry] : impl->geometries) {
        (void)description;
        for (const Impl::VertexStream& stream : entry.streams) {
            if (stream.buffer != VK_NULL_HANDLE) {
                ++out.streams_live;
                out.stream_bytes_live += stream.bytes;
            }
        }
        for (const Impl::IndexStream& stream : entry.indices) {
            if (stream.buffer != VK_NULL_HANDLE) {
                ++out.streams_live;
                out.stream_bytes_live += stream.bytes;
            }
        }
    }
    // The instance streams are streams too: a command that read one paid for its bytes and fills a row of the same
    // ledger (see ResidencyKind::Streams), so a host release that drops one is visible there like any other.
    for (const auto& [buffer, entry] : impl->instance_streams) {
        (void)buffer;
        if (entry.buffer != VK_NULL_HANDLE) {
            ++out.streams_live;
            out.stream_bytes_live += entry.bytes;
        }
    }
    return out;
}

ContentImages& ContentDraw::images() noexcept
{
    return impl->images;
}

const ContentImages& ContentDraw::images() const noexcept
{
    return impl->images;
}

std::size_t ContentDraw::releaseGeometry(const vn::graphics::GeometryDescription* description)
{
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || description == nullptr) {
        return 0;
    }
    const auto found = impl->geometries.find(description);
    if (found == impl->geometries.end()) {
        return 0; // nothing was resident: already released
    }
    impl->destroyGeometryStreams(found->second);
    impl->geometries.erase(found);
    return 1;
}

std::size_t ContentDraw::releaseProgram(const vn::graphics::ShaderProgramDescription* description)
{
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || description == nullptr) {
        return 0;
    }
    std::size_t dropped = 0;
    // THE PIPELINES FIRST: they are keyed by the program's pointer, so once the entry is gone they could never be
    // looked up again - keeping them alive would be a leak nothing can name.
    for (auto it = impl->pipelines.begin(); it != impl->pipelines.end();) {
        if (it->first.program != description) {
            ++it;
            continue;
        }
        impl->table->vkDestroyPipeline(impl->device, it->second, nullptr);
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
            impl->table->vkDestroyShaderModule(impl->device, it->second.vertex, nullptr);
        }
        if (it->second.fragment != VK_NULL_HANDLE) {
            impl->table->vkDestroyShaderModule(impl->device, it->second.fragment, nullptr);
        }
        it = impl->programs.erase(it);
        ++dropped;
    }
    return dropped;
}

std::size_t ContentDraw::releaseStreams(const void* buffer)
{
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || buffer == nullptr) {
        return 0;
    }
    std::size_t dropped = 0;
    for (auto& [description, entry] : impl->geometries) {
        (void)description;
        for (Impl::VertexStream& stream : entry.streams) {
            if (stream.source != buffer || stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            impl->destroyBuffer(stream.buffer, stream.memory);
            ++impl->stream_releases_;
            ++dropped;
        }
        for (Impl::IndexStream& stream : entry.indices) {
            if (stream.source != buffer || stream.buffer == VK_NULL_HANDLE) {
                continue;
            }
            impl->destroyBuffer(stream.buffer, stream.memory);
            ++impl->stream_releases_;
            ++dropped;
        }
    }
    // AN INSTANCE STREAM IS RELEASED BY THE SAME ANNOUNCEMENT (`releaseContent(Buffer)` names a vertex-or-instance
    // stream, see RenderDevice): the next draw that reads this buffer uploads it again, exactly like a vertex one.
    const auto instance = impl->instance_streams.find(static_cast<const vn::Buffer<float>*>(buffer));
    if (instance != impl->instance_streams.end() && instance->second.buffer != VK_NULL_HANDLE) {
        impl->destroyBuffer(instance->second.buffer, instance->second.memory);
        ++impl->stream_releases_;
        ++dropped;
    }
    return dropped;
}

void ContentDraw::prepare(const vn::graphics::backend::RenderPassCompatibility& compatibility,
                          const VkRenderPass                             render_pass,
                          const std::uint32_t                            color_attachments,
                          const vn::graphics::DrawDescription&           draw,
                          const vn::graphics::PassDraw&                  pass,
                          const vn::graphics::ShaderProgramDescription* default_program,
                          vn::String*                                    why,
                          vn::graphics::DiagnosticCategory*              category)
{
    vn::String                       local;
    vn::String&                      reason        = why != nullptr ? *why : local;
    vn::graphics::DiagnosticCategory local_category = vn::graphics::DiagnosticCategory::ContentSkipped;
    vn::graphics::DiagnosticCategory& category_ref  = category != nullptr ? *category : local_category;
    reason                                          = vn::String();
    category_ref                                    = vn::graphics::DiagnosticCategory::ContentSkipped;

    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || draw.commands == nullptr) {
        return;
    }
    if (!draw.camera.present) {
        return; // no view means no draw, and no draw means nothing to build
    }
    if (draw.kind != vn::graphics::DrawDescription::Kind::Scene) {
        reason = vn::String(u8"vk: a screen draw reached the content path (it has its own recordScreen)");
        return;
    }

    // NO FALLBACK IMAGE, NO DESCRIPTOR SET: a warm-up builds programs, streams and pipelines, and the 1x1 stand-in
    // and the texture copies are recorded afterwards by preparePass, on the transfer batch prepare() opens for them.
    for (const vn::graphics::RenderCommand& command : *draw.commands) {
        Impl::CommandPlan                plan;
        vn::String                       failure;
        vn::graphics::DiagnosticCategory failure_category = vn::graphics::DiagnosticCategory::ContentSkipped;
        if (impl->planCommand(command, pass, default_program, compatibility, render_pass, color_attachments, plan, failure, failure_category)) {
            continue;
        }
        if (reason.empty()) {
            reason       = std::move(failure);
            category_ref = failure_category;
        }
    }
}

void ContentDraw::prepareScreen(const vn::graphics::backend::RenderPassCompatibility& compatibility,
                                const VkRenderPass                             render_pass,
                                const std::uint32_t                            color_attachments,
                                const vn::graphics::DrawDescription&           draw,
                                const vn::graphics::PassDraw&                  pass,
                                vn::String*                                    why,
                                vn::graphics::DiagnosticCategory*              category)
{
    vn::String                       local;
    vn::String&                      reason        = why != nullptr ? *why : local;
    vn::graphics::DiagnosticCategory local_category = vn::graphics::DiagnosticCategory::ContentSkipped;
    vn::graphics::DiagnosticCategory& category_ref  = category != nullptr ? *category : local_category;
    reason                                          = vn::String();
    category_ref                                    = vn::graphics::DiagnosticCategory::ContentSkipped;

    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return;
    }
    if (!draw.camera.present) {
        return;
    }
    Impl::ScreenPlan                 plan;
    vn::String                       failure;
    vn::graphics::DiagnosticCategory failure_category = vn::graphics::DiagnosticCategory::ContentSkipped;
    if (!impl->planScreen(draw, pass, compatibility, render_pass, color_attachments, plan, failure, failure_category)) {
        reason       = std::move(failure);
        category_ref = failure_category;
    }
}

void ContentDraw::preparePass(const VkCommandBuffer commands, const std::span<const vn::graphics::DrawDescription> draws)
{
    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || commands == VK_NULL_HANDLE) {
        return;
    }
    vn::String why;
    if (!impl->images.ensureFallback(commands, &why)) {
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
                    (void)impl->images.viewFor(*slot.texture, commands, &why);
                }
            }
        }
    }
}

std::uint64_t ContentDraw::record(VkCommandBuffer                             commands,
                                  VkRenderPass                                render_pass,
                                  const vn::graphics::backend::RenderPassCompatibility& compatibility,
                                  std::uint32_t                               color_attachments,
                                  int                                         width,
                                  int                                         height,
                                  const vn::graphics::DrawDescription&        draw,
                                  const vn::graphics::PassDraw&               pass,
                                  const vn::graphics::ShaderProgramDescription* default_program,
                                  ShadowSource                                shadow,
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
        reason = vn::String(u8"vk: a screen draw reached the content path (it has its own recordScreen)");
        return 0;
    }

    // THE 1x1 WHITE STAND-IN: prepared outside the render pass by preparePass, so no descriptor a shader might read
    // is left undefined.
    if (!impl->images.ensureFallback(VK_NULL_HANDLE, &reason)) {
        return 0;
    }

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
    if (pass.depth_range.has_value() && pass.depth_range->isValid()) {
        // The pass' depth BAND: min is the far end, max the near end (see DepthRange), which is exactly Vulkan's
        // minDepth/maxDepth pair under reverse-Z.
        const std::pair<float, float> depth = StateMapping::viewportDepth(pass.depth_range);
        viewport.minDepth                  = depth.first;
        viewport.maxDepth                  = depth.second;
    }

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
        // WHAT THIS COMMAND DRAWS WITH - the same resolution a warm-up runs (see planCommand): program compiled,
        // streams uploaded, pipeline selected. Without it there is nothing to record.
        Impl::CommandPlan                 plan;
        vn::String                        failure;
        vn::graphics::DiagnosticCategory  failure_category = vn::graphics::DiagnosticCategory::ContentSkipped;
        if (!impl->planCommand(command, pass, default_program, compatibility, render_pass, color_attachments, plan, failure, failure_category)) {
            fail(std::move(failure), failure_category);
            continue;
        }
        const vn::graphics::ResolvedCommandState& resolved = plan.resolved;

        // THE MATERIAL: resolved by the plan (the same answer the variant was derived from), so the slots bound and
        // the program compiled cannot describe two different materials.
        const vn::graphics::MaterialParameters* material_values = plan.material != nullptr ? &plan.material->values() : nullptr;

        // THE SLOTS: upload each COMPLETE texture, bind the rest to the fallback, and state the presence bits the
        // shading reads. A slot the cache cannot serve is reported and stays absent rather than reading stale bytes.
        std::array<VkDescriptorImageInfo, kTextureSlots> slots{};
        std::uint32_t                                    texture_flags = 0U;
        for (std::uint32_t slot = 0; slot < kTextureSlots; ++slot) {
            VkImageView image_view = impl->images.fallbackView();
            VkSampler   sampler    = impl->images.fallbackSampler();
            if (material_values != nullptr) {
                const vn::graphics::TextureSlotBinding& binding = material_values->textures[slot];
                if (binding.texture != nullptr && binding.texture->isComplete()) {
                    vn::String        upload_why;
                    const VkImageView uploaded = impl->images.viewFor(*binding.texture, VK_NULL_HANDLE, &upload_why);
                    if (uploaded != VK_NULL_HANDLE) {
                        image_view = uploaded;
                        const VkSampler authored = impl->images.samplerFor(binding.sampler);
                        if (authored != VK_NULL_HANDLE) {
                            sampler = authored;
                        }
                        texture_flags |= 1U << slot;
                    }
                    else {
                        note(upload_why.empty() ? u8"vk: a material texture could not be uploaded" : upload_why.c_str());
                    }
                }
            }
            slots[slot].sampler     = sampler;
            slots[slot].imageView   = image_view;
            slots[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        // THE LIGHTS: the draw's announcement, packed in view space (see LightBlock.hpp). No announcement and an
        // empty one both mean the backend default, an ambient-lit scene.
        VineLightsBlock lights_block{};
        if (draw.light_values.has_value()) {
            (void)packLightBlock(*draw.light_values, draw.camera, lights_block);
        }
        else {
            (void)packLightBlock({}, draw.camera, lights_block);
        }

        // THE SHADOW: the map the pass samples becomes a block for THIS call (the caster's slot, its bias and the
        // matrix that maps a fragment's view position into the producer's clip - see buildShadowBlock).
        const VineShadowBlock shadow_block = buildShadowBlock(shadow, draw);

        VkBuffer       uniform_buffer = VK_NULL_HANDLE;
        VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
        if (!impl->createDrawUniforms(buildMaterialBlock(material_values, texture_flags),
                                      lights_block,
                                      buildDrawBlock(command, resolved.state.pointSize),
                                      shadow_block,
                                      uniform_buffer,
                                      uniform_memory)) {
            note(u8"vk: a draw's uniform buffer could not be created");
            continue;
        }
        impl->frame_uniforms.emplace_back(uniform_buffer, uniform_memory);

        const VkImageView shadow_view    = shadow.view != VK_NULL_HANDLE ? shadow.view : impl->images.fallbackView();
        const VkSampler   shadow_sampler = impl->images.samplerFor(kShadowSampler);
        const VkDescriptorSet content_set =
            impl->makeContentSet(uniform_buffer, slots, shadow_view, shadow_sampler != VK_NULL_HANDLE ? shadow_sampler : impl->images.fallbackSampler());
        const VkDescriptorSet draw_set    = content_set != VK_NULL_HANDLE ? impl->makeDrawSet(uniform_buffer) : VK_NULL_HANDLE;
        if (content_set == VK_NULL_HANDLE || draw_set == VK_NULL_HANDLE) {
            note(u8"vk: the descriptor pool ran out of sets for this frame");
            continue;
        }

        if (plan.pipeline != bound_pipeline) {
            impl->table->vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, plan.pipeline);
            bound_pipeline = plan.pipeline;
        }
        impl->table->vkCmdSetViewport(commands, 0U, 1U, &viewport);
        impl->table->vkCmdSetScissor(commands, 0U, 1U, &scissor);
        impl->table->vkCmdSetLineWidth(commands, resolved.state.lineWidth);
        impl->table->vkCmdSetDepthBias(commands, resolved.state.polygonOffset.units, 0.0F, resolved.state.polygonOffset.factor);
        const VkDescriptorSet sets[2]{ content_set, draw_set };
        impl->table->vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, impl->content_pipeline_layout, 0U, 2U, sets, 0U, nullptr);

        const ContentPushConstants push = pushConstantsOf(draw.camera, command.model_matrix);
        impl->table->vkCmdPushConstants(commands,
                                        impl->content_pipeline_layout,
                                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                        0U,
                                        sizeof(push),
                                        &push);

        std::vector<VkBuffer>      vertex_buffers;
        std::vector<VkDeviceSize>  vertex_offsets;
        const std::size_t          binding_count = plan.streams->streams.size() + (plan.instances != nullptr ? 1U : 0U);
        vertex_buffers.reserve(binding_count);
        vertex_offsets.reserve(binding_count);
        for (const Impl::VertexStream& stream : plan.streams->streams) {
            vertex_buffers.push_back(stream.buffer);
            vertex_offsets.push_back(0U);
        }
        if (plan.instances != nullptr) {
            // THE INSTANCE STREAM is the LAST binding, matching the layout `ensureContentPipeline` built for it
            // (one binding, stride of a matrix, input rate INSTANCE).
            vertex_buffers.push_back(plan.instances->buffer);
            vertex_offsets.push_back(0U);
        }
        if (!vertex_buffers.empty()) {
            impl->table->vkCmdBindVertexBuffers(commands, 0U, static_cast<std::uint32_t>(vertex_buffers.size()), vertex_buffers.data(), vertex_offsets.data());
        }

        const std::uint32_t instance_count = plan.instances != nullptr ? command.instance_count : 1U;
        const std::uint32_t first_instance = plan.instances != nullptr ? command.first_instance : 0U;
        const Impl::IndexStream* indices = nullptr;
        if (command.primitive_index < plan.streams->indices.size() && plan.streams->indices[command.primitive_index].buffer != VK_NULL_HANDLE) {
            indices = &plan.streams->indices[command.primitive_index];
        }
        if (indices != nullptr) {
            impl->table->vkCmdBindIndexBuffer(commands, indices->buffer, 0U, VK_INDEX_TYPE_UINT32);
            impl->table->vkCmdDrawIndexed(commands, indices->count, instance_count, 0U, 0, first_instance);
            ++recorded;
        }
        else if (plan.streams->vertex_count > 0U) {
            impl->table->vkCmdDraw(commands, plan.streams->vertex_count, instance_count, 0U, first_instance);
            ++recorded;
        }
        else {
            note(u8"vk: a draw named neither an index stream nor vertices");
        }
    }
    return recorded;
}

std::uint64_t ContentDraw::recordScreen(VkCommandBuffer                             commands,
                                        VkRenderPass                                render_pass,
                                        const vn::graphics::backend::RenderPassCompatibility& compatibility,
                                        std::uint32_t                               color_attachments,
                                        int                                         width,
                                        int                                         height,
                                        const vn::graphics::DrawDescription&        draw,
                                        const vn::graphics::PassDraw&               pass,
                                        ScreenSource                                source,
                                        vn::String*                                 why,
                                        vn::graphics::DiagnosticCategory*           category)
{
    vn::String                       local;
    vn::String&                      reason        = why != nullptr ? *why : local;
    vn::graphics::DiagnosticCategory local_category = vn::graphics::DiagnosticCategory::ContentSkipped;
    vn::graphics::DiagnosticCategory& category_ref  = category != nullptr ? *category : local_category;
    reason                                          = vn::String();
    category_ref                                    = vn::graphics::DiagnosticCategory::ContentSkipped;

    if (impl->device == VK_NULL_HANDLE || impl->table == nullptr || commands == VK_NULL_HANDLE) {
        return 0;
    }
    // The same contract a content draw follows: no camera names no picture.
    if (!draw.camera.present) {
        return 0;
    }
    if (source.views.empty()) {
        reason = vn::String(u8"vk: a screen draw named no source target, so there is nothing to sample");
        return 0;
    }
    if (!impl->images.ensureFallback(VK_NULL_HANDLE, &reason)) {
        return 0;
    }

    // THE PROGRAM AND THE PIPELINE - the same resolution a warm-up runs (see planScreen).
    Impl::ScreenPlan                 plan;
    vn::String                       failure;
    vn::graphics::DiagnosticCategory failure_category = vn::graphics::DiagnosticCategory::ContentSkipped;
    if (!impl->planScreen(draw, pass, compatibility, render_pass, color_attachments, plan, failure, failure_category)) {
        reason       = std::move(failure);
        category_ref = failure_category;
        return 0;
    }
    const vn::graphics::ResolvedCommandState& resolved = plan.resolved;
    const VkPipeline                          pipeline = plan.pipeline;

    // The rectangle the pass states, or the whole destination (a pass without a viewport draws all of it).
    VkViewport viewport{};
    VkRect2D   scissor{};
    if (pass.viewport.has_value() && pass.viewport->isValid()) {
        viewport.x      = static_cast<float>(pass.viewport->x);
        viewport.y      = static_cast<float>(pass.viewport->y);
        viewport.width  = static_cast<float>(pass.viewport->width);
        viewport.height = static_cast<float>(pass.viewport->height);
        scissor.offset  = VkOffset2D{ pass.viewport->x, pass.viewport->y };
        scissor.extent  = VkExtent2D{ static_cast<std::uint32_t>(pass.viewport->width), static_cast<std::uint32_t>(pass.viewport->height) };
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

    // THE SLOTS: binding N is the source's attachment N; a binding past the source's count reads the fallback (a
    // screen program only declares the bindings its own ABI states, so nothing reads them).
    std::array<VkDescriptorImageInfo, kTextureSlots> slots{};
    for (std::uint32_t slot = 0; slot < kTextureSlots; ++slot) {
        slots[slot].sampler     = impl->images.fallbackSampler();
        slots[slot].imageView   = slot < source.views.size() ? source.views[slot] : impl->images.fallbackView();
        slots[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VineLightsBlock lights_block{};
    if (draw.light_values.has_value()) {
        (void)packLightBlock(*draw.light_values, draw.camera, lights_block);
    }
    else {
        (void)packLightBlock({}, draw.camera, lights_block);
    }

    VkBuffer       uniform_buffer = VK_NULL_HANDLE;
    VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
    if (!impl->createDrawUniforms(vn::graphics::VineMaterialBlock{}, lights_block, vn::graphics::VineDrawBlock{}, VineShadowBlock{}, uniform_buffer, uniform_memory)) {
        reason = vn::String(u8"vk: a screen draw's uniform buffer could not be created");
        return 0;
    }
    impl->frame_uniforms.emplace_back(uniform_buffer, uniform_memory);

    const VkDescriptorSet set = impl->makeScreenSet(uniform_buffer, slots);
    if (set == VK_NULL_HANDLE) {
        reason = vn::String(u8"vk: the descriptor pool ran out of sets for this frame");
        return 0;
    }

    impl->table->vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    impl->table->vkCmdSetViewport(commands, 0U, 1U, &viewport);
    impl->table->vkCmdSetScissor(commands, 0U, 1U, &scissor);
    impl->table->vkCmdSetLineWidth(commands, resolved.state.lineWidth);
    // The pipeline states depth bias as DYNAMIC (the content path does the same), so a full-screen draw owes the
    // command buffer the values too - zero is "no offset".
    impl->table->vkCmdSetDepthBias(commands, 0.0F, 0.0F, 0.0F);
    impl->table->vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, impl->screen_pipeline_layout, 0U, 1U, &set, 0U, nullptr);
    // The full-screen triangle: three vertices generated from gl_VertexIndex, no buffer, no index stream.
    impl->table->vkCmdDraw(commands, 3U, 1U, 0U, 0U);
    return 1;
}

VN_BACKEND_VK_NS_END
