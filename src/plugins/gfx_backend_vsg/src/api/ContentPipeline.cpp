#include <vine/vsg/api/ContentPipeline.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>
#include <vsg/utils/ShaderCompiler.h>

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/vsg/api/ContentImages.hpp>
#include <vine/vsg/api/ContentPush.hpp>
#include <vine/vsg/api/LightBlock.hpp>

V_VSG_NS_BEGIN

namespace
{

/** @brief The descriptor set a CONTENT program's sampled inputs live in (see the file note). */
constexpr std::uint32_t kContentInputSet = 1U;

/** @brief The bytes one L1 block role's ABI struct carries (0 for a role that is not a block). */
std::uint32_t abiSizeOfRole(AbiBlockRole role) noexcept
{
    switch (role)
    {
    case AbiBlockRole::View: return static_cast<std::uint32_t>(sizeof(vine::graphics::VineViewBlock));
    case AbiBlockRole::Draw: return static_cast<std::uint32_t>(sizeof(vine::graphics::VineDrawBlock));
    case AbiBlockRole::Material: return static_cast<std::uint32_t>(sizeof(vine::graphics::VineMaterialBlock));
    case AbiBlockRole::Lights: return static_cast<std::uint32_t>(sizeof(VineLightsBlock));
    case AbiBlockRole::ShadowBlock: return static_cast<std::uint32_t>(sizeof(vine::graphics::VineShadowBlock));
    case AbiBlockRole::NotABlock:
    case AbiBlockRole::Foreign: return 0U;
    }
    return 0U;
}

/**
 * @brief Reads @p abi into @p sets (one per set index) and the shapes they declare.
 *
 * Refuses what this backend cannot describe at all - a foreign block, a block that is not std140 or that
 * declares MORE bytes than its L1 struct, an unsupported sampler, an array binding, a sampler outside the
 * input set - because a pipeline built against a layout nobody can feed is worse than no pipeline: its
 * draws read whatever happens to be bound.
 *
 * @param abi   The program's declared bindings.
 * @param sets  Receives one layout per set index the facts mention (empty layouts included: the API wants a
 *              contiguous range, and a shader may declare further sets than it uses).
 * @param shapes Receives the shape of each entry of @p sets.
 * @return true when every declaration can be described.
 */
bool describeAbi(const ProgramAbi& abi, std::vector<::vsg::ref_ptr<::vsg::DescriptorSetLayout>>& sets,
                 std::vector<std::vector<BlockDescriptors::Binding>>& shapes,
                 std::vector<std::vector<std::uint32_t>>& sampler_shapes)
{
    std::uint32_t set_count = 0U;
    for (const AbiBinding& binding : abi.bindings)
    {
        if (binding.kind == AbiDescriptorKind::UniformBlock)
        {
            if (binding.role == AbiBlockRole::Foreign || binding.role == AbiBlockRole::NotABlock)
            {
                return false;
            }
            if (binding.layout != AbiBlockLayout::Std140 || binding.count != 1U ||
                binding.block_size > abiSizeOfRole(binding.role))
            {
                return false;
            }
        }
        else
        {
            // A sampler may sit in ANY set the text names: the engine's own set 0 carries the material block
            // and the diffuse map side by side, and a set is a set whatever kind of binding it holds. What
            // this layer refuses is a KIND it has no view for (a cube map arrives with the texture work) and
            // an array of samplers (each binding carries one image here) - plus ONE name: `skyMap` is the
            // frame's environment, and the environment image has not landed, so a program that samples it is
            // better refused than compiled against a stand-in that would show something nobody authored
            // (see api/ContentImages).
            if (binding.kind == AbiDescriptorKind::OtherSampler || binding.kind == AbiDescriptorKind::SamplerCube ||
                binding.count != 1U || imageOriginOf(binding.name) == ImageOrigin::Environment)
            {
                return false;
            }
        }
        set_count = std::max(set_count, binding.set + 1U);
    }

    // One (set, binding) cannot be two things: a block and a sampler at the same binding would need one
    // descriptor to be a buffer and an image at once.
    for (const AbiBinding& left : abi.bindings)
    {
        for (const AbiBinding& right : abi.bindings)
        {
            if (&left != &right && left.set == right.set && left.binding == right.binding &&
                left.kind != right.kind)
            {
                return false;
            }
        }
    }

    sets.resize(set_count);
    shapes.resize(set_count);
    sampler_shapes.resize(set_count);
    for (std::uint32_t index = 0U; index < set_count; ++index)
    {
        shapes[index] = blockShapeOf(abi, index);
        for (const AbiBinding& binding : abi.bindings)
        {
            if (binding.set == index && binding.kind != AbiDescriptorKind::UniformBlock)
            {
                sampler_shapes[index].push_back(binding.binding);
            }
        }
        sets[index] = BlockDescriptors::layoutOfShape(shapes[index], sampler_shapes[index]);
        if (sets[index] == nullptr)
        {
            return false;
        }
    }
    return true;
}

/// @brief The baked values the create-info carries (Vulkan requires the structs; set commands win).
///
/// They are the constants the dynamic declaration starts from, so every pipeline built here is
/// content-identical and a state change costs commands instead of a compile. The compare operator and the
/// front face are the ENGINE's conventions, not choices: the depth comparison is inverted for reverse-Z,
/// and front faces arrive clockwise because vsg's projection inverts Y (see RenderStateMapper's note -
/// declaring counter-clockwise made every cull mode act on the wrong faces, silently).
constexpr VkCullModeFlags kBakedCullMode         = VK_CULL_MODE_BACK_BIT;
constexpr VkFrontFace     kBakedFrontFace        = VK_FRONT_FACE_CLOCKWISE;
constexpr VkPrimitiveTopology kBakedTopology     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
constexpr VkBool32        kBakedDepthTestEnable  = VK_TRUE;
constexpr VkBool32        kBakedDepthWriteEnable = VK_TRUE;
constexpr VkCompareOp     kBakedCompareOp        = VK_COMPARE_OP_GREATER;

/// @brief The viewport/scissor the create-info carries; the real rectangle is set per draw.
constexpr VkExtent2D kBakedViewportExtent{ 1U, 1U };

/**
 * @brief The dynamic states every content pipeline declares.
 *
 * The nine a `StateNode` edits while a scene runs (depth test / write / compare, cull, front face, polygon
 * mode, topology, blend enable and equation) plus the VIEWPORT and the SCISSOR. The last two are what keeps
 * an extent out of the pipeline's identity: a resize changes a rectangle, and a rectangle is a command.
 */
::vsg::ref_ptr<::vsg::DynamicState> makeDynamicStateDeclaration()
{
    return ::vsg::DynamicState::create(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_CULL_MODE,
                                       VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
                                       VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR);
}

/// @brief The blend state's attachment list for @p color_count attachments (0 = a depth-only pass).
::vsg::ref_ptr<::vsg::ColorBlendState> makeColorBlendState(std::uint32_t color_count)
{
    ::vsg::ColorBlendState::ColorBlendAttachments attachments;
    attachments.reserve(color_count);
    for (std::uint32_t index = 0; index < color_count; ++index) {
        VkPipelineColorBlendAttachmentState attachment = {};
        attachment.blendEnable         = VK_FALSE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        attachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachments.push_back(attachment);
    }
    return ::vsg::ColorBlendState::create(attachments);
}

}  // namespace

struct ContentPipeline::Data
{
    /** @brief The set layout and pipeline layout one sampled-input count is compiled against. */
    struct Sampled
    {
        ::vsg::ref_ptr<::vsg::DescriptorSetLayout> set;       ///< The samplers (set 1 for content, set 0 for screen).
        ::vsg::ref_ptr<::vsg::PipelineLayout>      pipeline;  ///< That set, in its place among the layer's sets.
    };

    /// @brief The pair a sampled set is built for: colour bindings and depth bindings.
    struct SampledKey
    {
        std::uint32_t colors{0};  ///< Colour textures the pass binds.
        std::uint32_t depths{0};  ///< DEPTH textures the pass binds (they follow the colours).

        /** @brief Compares the pair. */
        [[nodiscard]] bool operator==(const SampledKey& other) const noexcept
        {
            return colors == other.colors && depths == other.depths;
        }
    };

    /** @brief Compiles one GLSL stage, or returns an empty pointer when the compiler refuses it. */
    ::vsg::ref_ptr<::vsg::ShaderStage> compileStage(VkShaderStageFlagBits stage, const std::string& source,
                                                    const std::string& entry)
    {
        if (!compiler.supported()) {
            return {};
        }
        auto shader_stage = ::vsg::ShaderStage::create(stage, entry, source);
        if (shader_stage == nullptr || !compiler.compile(shader_stage) || shader_stage->module == nullptr ||
            shader_stage->module->code.empty()) {
            return {};
        }
        return shader_stage;
    }

    core::DrawKind                                              kind{core::DrawKind::Content};
    ::vsg::ShaderCompiler                                       compiler;
    ProgramAbi                                                  abi;          ///< The facts the layer was built from.
    /// @brief One layout per set index the DECLARED BLOCKS reach (empty layouts included, see describeAbi).
    std::vector<::vsg::ref_ptr<::vsg::DescriptorSetLayout>>     sets;
    /// @brief One shape per entry of @ref sets: which block role sits at which binding.
    std::vector<std::vector<BlockDescriptors::Binding>>         shapes;
    /// @brief One sampled-binding list per entry of @ref sets: the images the set declares (the other half of
    ///        a declared set - the engine's set 0 carries blocks AND the diffuse map).
    std::vector<std::vector<std::uint32_t>>                     sampler_shapes;
    /// @brief The set indices that declare at least one block, ascending.
    std::vector<std::uint32_t>                                  declared_sets;
    /// @brief The layout that fills a set index the declarations leave empty (the API wants a CONTIGUOUS
    ///        range of set layouts, and a gap is not a null entry - it is a set with no bindings).
    ::vsg::ref_ptr<::vsg::DescriptorSetLayout>                  empty_set;
    ::vsg::PushConstantRanges                                   push_ranges;  ///< The declared push ranges.
    ::vsg::ShaderStages                                         stages;
    ::vsg::ref_ptr<::vsg::PipelineLayout>                       layout;
    /// @brief Hash for the (colours, depths) pair (a set layout is built per distinct pair).
    struct SampledKeyHash
    {
        /** @brief Hashes the pair (the same mix the other keys use). */
        [[nodiscard]] std::size_t operator()(const SampledKey& key) const noexcept
        {
            std::size_t hash = 0;
            const auto  mix  = [&hash](std::uint64_t value) noexcept {
                hash ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
            };
            mix(key.colors);
            mix(key.depths);
            return hash;
        }
    };

    std::unordered_map<SampledKey, Sampled, SampledKeyHash>     sampled;  ///< One entry per distinct pair.
    ::vsg::ref_ptr<::vsg::Sampler>                              input_sampler;
    ::vsg::GraphicsPipelineStates                               states;
    ::vsg::ref_ptr<::vsg::Sampler> depth_sampler;  ///< NEAREST: a depth compare reads exact texels.
    std::unordered_map<std::uint64_t, ::vsg::ref_ptr<::vsg::GraphicsPipeline>> pipelines;
    std::uint64_t                                               compiles{0};
    std::uint64_t                                               failures{0};
};

ContentPipeline::ContentPipeline() : d(std::make_unique<Data>())
{
}

std::unique_ptr<ContentPipeline> ContentPipeline::create(const ProgramAbi& abi,
                                                        std::span<const VertexBinding>  bindings,
                                                        std::span<const VertexAttribute> attributes,
                                                        const Shaders& shaders)
{
    return create(abi, bindings, attributes, shaders, Settings{});
}

std::unique_ptr<ContentPipeline> ContentPipeline::create(const ProgramAbi& abi,
                                                        std::span<const VertexBinding>  bindings,
                                                        std::span<const VertexAttribute> attributes,
                                                        const Shaders& shaders, const Settings& settings)
{
    auto layer = std::unique_ptr<ContentPipeline>(new ContentPipeline());
    layer->d->abi = abi;
    layer->d->empty_set = ::vsg::DescriptorSetLayout::create();
    if (layer->d->empty_set == nullptr ||
        !describeAbi(abi, layer->d->sets, layer->d->shapes, layer->d->sampler_shapes)) {
        return nullptr;  // a declaration this backend cannot describe: reported, never compiled against a guess
    }
    // A pipeline layout's set layouts are a CONTIGUOUS range from 0 and a gap is a set with no bindings - not
    // a null entry, which is an invalid handle. describeAbi is where that range is built (one entry per set
    // index, empty layouts included), so nothing is left to patch up here.
    //
    // Which sets a CALLER builds (and a pass binds): every set that declares blocks, plus every set that
    // declares sampled images OTHER than the input set - the input set's images are the pass' own (it binds
    // what an earlier pass produced, in declaration order), so its set is built from the key's counts rather
    // than by the caller (see Data::sampled). A set that declares both is refused above (the draw block and
    // the pass' images would have to be one descriptor set).
    for (std::uint32_t set = 0U; set < layer->d->shapes.size(); ++set) {
        const bool has_blocks  = !layer->d->shapes[set].empty();
        const bool has_samplers = !layer->d->sampler_shapes[set].empty();
        if (has_blocks || (has_samplers && set != kContentInputSet)) {
            layer->d->declared_sets.push_back(set);
        }
    }

    ::vsg::ref_ptr<::vsg::ShaderStage> vertex =
        layer->d->compileStage(VK_SHADER_STAGE_VERTEX_BIT, shaders.vertex, shaders.entry);
    ::vsg::ref_ptr<::vsg::ShaderStage> fragment =
        layer->d->compileStage(VK_SHADER_STAGE_FRAGMENT_BIT, shaders.fragment, shaders.entry);
    if (vertex == nullptr || fragment == nullptr) {
        return nullptr;  // nothing to shade with: the caller reports it instead of drawing nothing
    }
    layer->d->stages = ::vsg::ShaderStages{ vertex, fragment };

    // The push ranges ARE the declarations (offset, size and the stage that reads them): a content program
    // that reads its camera matrices from a push block gets exactly the range its text declares, and one that
    // declares none gets none - pushing into a range that does not exist is an API violation, not a no-op.
    // The MEMBERS are checked here too: a range whose members are not the L2 realization of the L1 camera
    // pair (see api/ContentPush) is one no pass can fill, so the layer that would compile against it refuses
    // the program instead of drawing with zeros.
    ::vsg::PushConstantRanges push_ranges;
    for (const AbiPushRange& range : abi.pushes) {
        if (range.size == 0U) {
            return nullptr;  // a block whose members could not be sized is not a range to declare
        }
        for (const AbiPushMember& member : range.members) {
            if (!canFillPushMember(member)) {
                return nullptr;  // a member this backend cannot fill: the pass would draw with zero bytes
            }
        }
        push_ranges.push_back(
            VkPushConstantRange{ static_cast<VkShaderStageFlags>(pushStagesOf(range.stages)), range.offset,
                                 range.size });
    }
    layer->d->push_ranges = push_ranges;
    layer->d->layout     = ::vsg::PipelineLayout::create(layer->d->sets, push_ranges);
    if (layer->d->layout == nullptr) {
        return nullptr;
    }

    ::vsg::VertexInputState::Bindings declared_bindings;
    declared_bindings.reserve(bindings.size());
    for (const VertexBinding& binding : bindings) {
        VkVertexInputBindingDescription declared = {};
        declared.binding   = binding.binding;
        declared.stride    = binding.stride;
        declared.inputRate = binding.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        declared_bindings.push_back(declared);
    }
    ::vsg::VertexInputState::Attributes declared_attributes;
    declared_attributes.reserve(attributes.size());
    for (const VertexAttribute& attribute : attributes) {
        VkVertexInputAttributeDescription declared = {};
        declared.location = attribute.location;
        declared.binding  = attribute.binding;
        declared.format   = static_cast<VkFormat>(attribute.format);
        declared.offset   = attribute.offset;
        declared_attributes.push_back(declared);
    }

    auto raster_state       = ::vsg::RasterizationState::create();
    raster_state->cullMode  = kBakedCullMode;
    raster_state->frontFace = kBakedFrontFace;
    auto depth_state              = ::vsg::DepthStencilState::create();
    depth_state->depthTestEnable  = kBakedDepthTestEnable;
    depth_state->depthWriteEnable = kBakedDepthWriteEnable;
    depth_state->depthCompareOp   = kBakedCompareOp;
    auto input_assembly     = ::vsg::InputAssemblyState::create();
    input_assembly->topology = kBakedTopology;

    layer->d->states = ::vsg::GraphicsPipelineStates{
        ::vsg::VertexInputState::create(declared_bindings, declared_attributes),
        input_assembly,
        ::vsg::ViewportState::create(kBakedViewportExtent),
        raster_state,
        depth_state,
        makeColorBlendState(settings.color_attachments),
        ::vsg::MultisampleState::create(),
        makeDynamicStateDeclaration(),
    };
    return layer;
}

std::unique_ptr<ContentPipeline> ContentPipeline::createScreen(const Shaders& shaders)
{
    return createScreen(shaders, Settings{});
}

std::unique_ptr<ContentPipeline> ContentPipeline::createScreen(const Shaders& shaders, const Settings& settings)
{
    auto layer = std::unique_ptr<ContentPipeline>(new ContentPipeline());
    layer->d->kind = core::DrawKind::Screen;

    ::vsg::ref_ptr<::vsg::ShaderStage> vertex =
        layer->d->compileStage(VK_SHADER_STAGE_VERTEX_BIT, shaders.vertex, shaders.entry);
    ::vsg::ref_ptr<::vsg::ShaderStage> fragment =
        layer->d->compileStage(VK_SHADER_STAGE_FRAGMENT_BIT, shaders.fragment, shaders.entry);
    if (vertex == nullptr || fragment == nullptr) {
        return nullptr;  // nothing to shade with: the caller reports it instead of drawing nothing
    }
    layer->d->stages = ::vsg::ShaderStages{ vertex, fragment };

    // The full-screen ABI's constants are the FRAGMENT stage's: the engine's canonical vertex stage generates
    // the triangle from gl_VertexIndex and declares none, while a screen program reads its light block.
    ::vsg::PushConstantRanges push_ranges;
    if (settings.push_bytes != 0U) {
        push_ranges.push_back(VkPushConstantRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0U, settings.push_bytes });
    }
    layer->d->push_ranges = push_ranges;

    // The layout a pass with NO declared inputs binds. `layoutFor(0, 0)` answers with this one, and a screen
    // program that reads nothing but its push block (a lighting pass that reconstructs positions from a depth
    // attachment it samples as an INPUT, say) needs it: without it the layer would have no layout to hand the
    // pipeline and every such draw would be refused as "its pipeline could not be built" - which is a pipeline
    // that was never asked for, not one that failed.
    layer->d->layout = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{}, push_ranges);
    if (layer->d->layout == nullptr) {
        return nullptr;
    }

    // No blocks and no vertex streams: every set of this layer is the sampled one, and the triangle's vertices
    // are generated. The create-info's states are the LEGACY full-screen shape (the previous implementation's
    // overlay pipelines): no culling and no depth test, because a full-screen triangle's winding is the
    // engine's own and cutting it out is only ever a way to lose the whole picture. They are the dynamic
    // declaration's starting point - the plan's resolved state is what a draw commands.
    auto raster_state       = ::vsg::RasterizationState::create();
    raster_state->cullMode  = VK_CULL_MODE_NONE;
    raster_state->frontFace = kBakedFrontFace;
    auto depth_state              = ::vsg::DepthStencilState::create();
    depth_state->depthTestEnable  = VK_FALSE;
    depth_state->depthWriteEnable = VK_FALSE;
    depth_state->depthCompareOp   = kBakedCompareOp;
    auto input_assembly           = ::vsg::InputAssemblyState::create();
    input_assembly->topology      = kBakedTopology;

    layer->d->states = ::vsg::GraphicsPipelineStates{
        ::vsg::VertexInputState::create(::vsg::VertexInputState::Bindings{}, ::vsg::VertexInputState::Attributes{}),
        input_assembly,
        ::vsg::ViewportState::create(kBakedViewportExtent),
        raster_state,
        depth_state,
        makeColorBlendState(settings.color_attachments),
        ::vsg::MultisampleState::create(),
        makeDynamicStateDeclaration(),
    };
    return layer;
}

ContentPipeline::~ContentPipeline() = default;

ContentPipeline::Result ContentPipeline::acquire(core::VariantPool& pool, const core::PipelineKey& key)
{
    if (key.kind != d->kind) {
        // The two kinds compile against different descriptor ABIs (blocks at set 0 versus the samplers there),
        // so a key of the other kind is a call-site bug: compiling it here would hand the draw a pipeline bound
        // to a layout its own bindings do not match. Refused, counted, and never entered into the pool.
        ++d->failures;
        return { core::VariantPool::Action::Created, 0U, {} };
    }

    // The declarations have to cover the key, or the pipeline would be compiled against a layout the pass'
    // bindings do not fit. Two rules, and they are about different sets:
    //
    //   * a sampler in the INPUT set (this backend's own arrangement: a content pass that samples what an
    //     earlier pass produced binds those images there) needs an input for every binding the key promises
    //     - `sampled_color_count + sampled_depth_count` of them, in declaration order;
    //   * a sampler OUTSIDE it (the engine's own set 0, where the material block and the diffuse map share a
    //     set) is a MAP rather than a pass input: it is served from what the caller has (a material's
    //     texture, the white fallback), so the input count has nothing to say about it.
    const std::uint32_t input_count = key.sampled_color_count + key.sampled_depth_count;
    for (const AbiBinding& binding : d->abi.bindings)
    {
        if (binding.kind == AbiDescriptorKind::UniformBlock)
        {
            if (input_count != 0U && binding.set == kContentInputSet) {
                ++d->failures;
                return { core::VariantPool::Action::Created, 0U, {} };
            }
            continue;
        }
        if (binding.set == kContentInputSet && binding.binding >= input_count) {
            ++d->failures;
            return { core::VariantPool::Action::Created, 0U, {} };
        }
    }

    const core::VariantPool::Lookup lookup = pool.acquire(key);

    const auto found = d->pipelines.find(lookup.id);
    if (found != d->pipelines.end()) {
        // A reused identity whose object this layer already holds - the common frame path.
        return {lookup.action, lookup.id, found->second};
    }

    // The key names how many sampled colour textures this pass binds, and the pipeline layout has to be the
    // one built for exactly that count: a pipeline is compiled against one descriptor set layout, so the count
    // is identity rather than runtime state.
    const ::vsg::ref_ptr<::vsg::PipelineLayout> layout =
        layoutFor(key.sampled_color_count, key.sampled_depth_count);
    if (layout == nullptr) {
        ++d->failures;
        return {lookup.action, lookup.id, {}};
    }

    auto pipeline = ::vsg::GraphicsPipeline::create(layout, d->stages, d->states);
    if (pipeline == nullptr) {
        ++d->failures;
        return {lookup.action, lookup.id, {}};
    }
    d->pipelines.emplace(lookup.id, pipeline);
    ++d->compiles;
    // Drop the objects whose id the pool evicted: this layer is a keep-alive, and keeping a pipeline nothing
    // can look up again is how a session leaks one per generated program (see the file note).
    for (auto it = d->pipelines.begin(); it != d->pipelines.end();) {
        it = pool.contains(it->first) ? std::next(it) : d->pipelines.erase(it);
    }
    return {lookup.action, lookup.id, std::move(pipeline)};
}

::vsg::ref_ptr<::vsg::PipelineLayout> ContentPipeline::layout() const noexcept
{
    return d->layout;
}

::vsg::ref_ptr<::vsg::DescriptorSetLayout> ContentPipeline::sampledSetLayout(std::uint32_t color_bindings,
                                                                             std::uint32_t depth_bindings)
{
    if (color_bindings == 0U && depth_bindings == 0U) {
        return {};  // nothing to sample: a content layer has only its block set, a full-screen one has nothing
    }
    const Data::SampledKey key{ color_bindings, depth_bindings };
    const auto             found = d->sampled.find(key);
    if (found != d->sampled.end()) {
        return found->second.set;
    }

    // One combined image sampler per texture, readable from either shading stage: the sampled inputs of a pass
    // ARE the picture it reads, and which stage reads it is the shader's business. The DEPTH textures follow
    // the colour ones (see the declaration), so a pass that samples colours and a depth reads binding 0..N-1
    // for the colours and N for the depth - the same order whatever else the pass declares.
    auto set = ::vsg::DescriptorSetLayout::create();
    if (set == nullptr) {
        return {};
    }
    for (std::uint32_t binding = 0; binding < color_bindings + depth_bindings; ++binding) {
        set->addBinding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    // Where the set sits is the kind's: a content layer's sampled inputs live in set 1 (after the declared
    // block sets), a full-screen layer's in set 0 (its own ABI - the engine's screen programs declare
    // `layout(binding = i)`).
    ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline;
    if (d->kind == core::DrawKind::Screen) {
        pipeline = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ set }, d->push_ranges);
    }
    else {
        ::vsg::DescriptorSetLayouts pipeline_sets = d->sets;
        pipeline_sets.resize(std::max<std::size_t>(pipeline_sets.size(), kContentInputSet + 1U), d->empty_set);
        pipeline_sets[kContentInputSet] = set;
        pipeline                        = ::vsg::PipelineLayout::create(pipeline_sets, d->push_ranges);
    }
    if (pipeline == nullptr) {
        return {};
    }
    d->sampled.emplace(key, Data::Sampled{ set, pipeline });
    return set;
}

::vsg::ref_ptr<::vsg::PipelineLayout> ContentPipeline::layoutFor(std::uint32_t sampled_color_bindings,
                                                                 std::uint32_t sampled_depth_bindings)
{
    if (sampled_color_bindings == 0U && sampled_depth_bindings == 0U) {
        return d->layout;  // the content layer's "blocks only" layout; nothing for a full-screen layer
    }
    if (sampledSetLayout(sampled_color_bindings, sampled_depth_bindings) == nullptr) {
        return {};
    }
    return d->sampled.at(Data::SampledKey{ sampled_color_bindings, sampled_depth_bindings }).pipeline;
}

core::DrawKind ContentPipeline::kind() const noexcept
{
    return d->kind;
}

const ProgramAbi& ContentPipeline::abi() const noexcept
{
    return d->abi;
}

std::span<const BlockDescriptors::Binding> ContentPipeline::blockShape(std::uint32_t set) const noexcept
{
    return set < d->shapes.size() ? std::span<const BlockDescriptors::Binding>(d->shapes[set])
                                  : std::span<const BlockDescriptors::Binding>{};
}

std::span<const std::uint32_t> ContentPipeline::samplerBindings(std::uint32_t set) const noexcept
{
    return set < d->sampler_shapes.size() ? std::span<const std::uint32_t>(d->sampler_shapes[set])
                                          : std::span<const std::uint32_t>{};
}

std::span<const std::uint32_t> ContentPipeline::declaredSets() const noexcept
{
    return d->declared_sets;
}

::vsg::ref_ptr<::vsg::Sampler> ContentPipeline::inputSampler()
{
    if (d->input_sampler == nullptr) {
        d->input_sampler = ::vsg::Sampler::create();
    }
    return d->input_sampler;
}

::vsg::ref_ptr<::vsg::Sampler> ContentPipeline::depthSampler()
{
    if (d->depth_sampler == nullptr) {
        d->depth_sampler               = ::vsg::Sampler::create();
        d->depth_sampler->magFilter    = VK_FILTER_NEAREST;
        d->depth_sampler->minFilter    = VK_FILTER_NEAREST;
        d->depth_sampler->mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    return d->depth_sampler;
}

const ::vsg::ShaderStages& ContentPipeline::stages() const noexcept
{
    return d->stages;
}

std::size_t ContentPipeline::pipelines() const noexcept
{
    return d->pipelines.size();
}

std::uint64_t ContentPipeline::compiles() const noexcept
{
    return d->compiles;
}

std::uint64_t ContentPipeline::failures() const noexcept
{
    return d->failures;
}

bool ContentPipeline::agreesWithPool(const core::VariantPool& pool) const noexcept
{
    for (const auto& [id, pipeline] : d->pipelines) {
        if (!pool.contains(id)) {
            return false;
        }
    }
    return true;
}

V_VSG_NS_END
