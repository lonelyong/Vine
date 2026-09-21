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

    ::vsg::ShaderCompiler                                       compiler;
    ::vsg::ref_ptr<::vsg::DescriptorSetLayout>                  block_set;
    ::vsg::ShaderStages                                         stages;
    ::vsg::ref_ptr<::vsg::PipelineLayout>                       layout;
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
    // offsets, so a draw's state never reaches this layout again.
    ::vsg::PushConstantRanges push_ranges;
    if (settings.push_bytes != 0U) {
        push_ranges.push_back(VkPushConstantRange{ VK_SHADER_STAGE_VERTEX_BIT, 0U, settings.push_bytes });
    }
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

ContentPipeline::~ContentPipeline() = default;

ContentPipeline::Result ContentPipeline::acquire(core::VariantPool& pool, const core::PipelineKey& key)
{
    const core::VariantPool::Lookup lookup = pool.acquire(key);

    const auto found = d->pipelines.find(lookup.id);
    if (found != d->pipelines.end()) {
        // A reused identity whose object this layer already holds - the common frame path.
        return {lookup.action, lookup.id, found->second};
    }

    auto pipeline = ::vsg::GraphicsPipeline::create(d->layout, d->stages, d->states);
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
