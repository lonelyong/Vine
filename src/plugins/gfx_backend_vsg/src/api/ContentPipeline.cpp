#include <vine/vsg/api/ContentPipeline.hpp>

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

V_VSG_NS_BEGIN

namespace
{

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
    ::vsg::ref_ptr<::vsg::DescriptorSetLayout>                  block_set;
    ::vsg::PushConstantRanges                                   push_ranges;  ///< Kept for the per-count layouts.
    ::vsg::ShaderStages                                         stages;
    ::vsg::ref_ptr<::vsg::PipelineLayout>                       layout;
    std::unordered_map<std::uint32_t, Sampled>                  sampled;  ///< One entry per distinct count.
    ::vsg::ref_ptr<::vsg::Sampler>                              input_sampler;
    ::vsg::GraphicsPipelineStates                               states;
    std::unordered_map<std::uint64_t, ::vsg::ref_ptr<::vsg::GraphicsPipeline>> pipelines;
    std::uint64_t                                               compiles{0};
    std::uint64_t                                               failures{0};
};

ContentPipeline::ContentPipeline() : d(std::make_unique<Data>())
{
}

std::unique_ptr<ContentPipeline> ContentPipeline::create(const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set,
                                                         std::span<const VertexBinding>  bindings,
                                                         std::span<const VertexAttribute> attributes,
                                                         const Shaders& shaders)
{
    return create(block_set, bindings, attributes, shaders, Settings{});
}

std::unique_ptr<ContentPipeline> ContentPipeline::create(const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& block_set,
                                                         std::span<const VertexBinding>  bindings,
                                                         std::span<const VertexAttribute> attributes,
                                                         const Shaders& shaders, const Settings& settings)
{
    auto layer = std::unique_ptr<ContentPipeline>(new ContentPipeline());
    if (block_set == nullptr) {
        return nullptr;
    }

    ::vsg::ref_ptr<::vsg::ShaderStage> vertex =
        layer->d->compileStage(VK_SHADER_STAGE_VERTEX_BIT, shaders.vertex, shaders.entry);
    ::vsg::ref_ptr<::vsg::ShaderStage> fragment =
        layer->d->compileStage(VK_SHADER_STAGE_FRAGMENT_BIT, shaders.fragment, shaders.entry);
    if (vertex == nullptr || fragment == nullptr) {
        return nullptr;  // nothing to shade with: the caller reports it instead of drawing nothing
    }
    layer->d->stages = ::vsg::ShaderStages{ vertex, fragment };
    layer->d->block_set = block_set;

    // The layout binds the block set (set 0) and the ABI's push budget. The blocks arrive through dynamic
    // offsets, so a draw's state never reaches this layout again. A layout with sampled inputs (set 1) is
    // built on demand, one per count (see sampledSetLayout): the key names the count, so a pass never gets a
    // pipeline compiled against another pass' sampled-input shape.
    ::vsg::PushConstantRanges push_ranges;
    if (settings.push_bytes != 0U) {
        push_ranges.push_back(VkPushConstantRange{ VK_SHADER_STAGE_VERTEX_BIT, 0U, settings.push_bytes });
    }
    layer->d->push_ranges = push_ranges;
    layer->d->layout = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ block_set }, push_ranges);
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

    const core::VariantPool::Lookup lookup = pool.acquire(key);

    const auto found = d->pipelines.find(lookup.id);
    if (found != d->pipelines.end()) {
        // A reused identity whose object this layer already holds - the common frame path.
        return {lookup.action, lookup.id, found->second};
    }

    // The key names how many sampled colour textures this pass binds, and the pipeline layout has to be the
    // one built for exactly that count: a pipeline is compiled against one descriptor set layout, so the count
    // is identity rather than runtime state.
    const ::vsg::ref_ptr<::vsg::PipelineLayout> layout = layoutFor(key.sampled_color_count);
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

::vsg::ref_ptr<::vsg::DescriptorSetLayout> ContentPipeline::sampledSetLayout(std::uint32_t color_bindings)
{
    if (color_bindings == 0U) {
        return {};  // nothing to sample: a content layer has only its block set, a full-screen one has nothing
    }
    const auto found = d->sampled.find(color_bindings);
    if (found != d->sampled.end()) {
        return found->second.set;
    }

    // One combined image sampler per colour texture, readable from either shading stage: the sampled inputs
    // of a pass ARE the picture it reads, and which stage reads it is the shader's business.
    auto set = ::vsg::DescriptorSetLayout::create();
    if (set == nullptr) {
        return {};
    }
    for (std::uint32_t binding = 0; binding < color_bindings; ++binding) {
        set->addBinding(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1U,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    // Where the set sits is the kind's, and it is the whole difference between the two descriptor ABIs: a
    // content layer binds its blocks at 0 and the samplers at 1, a full-screen layer binds the samplers at 0
    // and nothing else (see the file note - the engine's screen programs declare `layout(binding = i)`).
    ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline;
    if (d->kind == core::DrawKind::Screen) {
        pipeline = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ set }, d->push_ranges);
    }
    else {
        pipeline = ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ d->block_set, set }, d->push_ranges);
    }
    if (pipeline == nullptr) {
        return {};
    }
    d->sampled.emplace(color_bindings, Data::Sampled{ set, pipeline });
    return set;
}

::vsg::ref_ptr<::vsg::PipelineLayout> ContentPipeline::layoutFor(std::uint32_t sampled_color_bindings)
{
    if (sampled_color_bindings == 0U) {
        return d->layout;  // the content layer's "blocks only" layout; nothing for a full-screen layer
    }
    if (sampledSetLayout(sampled_color_bindings) == nullptr) {
        return {};
    }
    return d->sampled.at(sampled_color_bindings).pipeline;
}

core::DrawKind ContentPipeline::kind() const noexcept
{
    return d->kind;
}

::vsg::ref_ptr<::vsg::Sampler> ContentPipeline::inputSampler()
{
    if (d->input_sampler == nullptr) {
        d->input_sampler = ::vsg::Sampler::create();
    }
    return d->input_sampler;
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
