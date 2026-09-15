#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/OwnedCache.hpp>

// The definitions below are the moved bodies: their documentation and default
// arguments live on the declarations in the header.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/ClearAttachments.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/commands/Draw.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/VertexIndexDraw.h>
#include <vsg/core/Array.h>
#include <vsg/core/Exception.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PushConstants.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/ViewDependentState.h>
#include <vsg/state/ViewportState.h>
#include <vsg/state/material.h>
#include <vsg/utils/Builder.h>
#include <vsg/app/CompileManager.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderCompiler.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>
#include <vsg/vk/ResourceRequirements.h>
#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

namespace detail
{

/**
 * @brief Builds the default pipeline states every scene pipeline shares.
 *
 * Every content set this backend builds carries the same list, so a pass can
 * switch between them freely: any difference in the states would show up as a
 * different picture rather than as an error.
 *
 * @param extent      Target extent for the baked static viewport.
 * @param depth_test  Enable depth test.
 * @param depth_write Enable depth write.
 * @param color_count Colour attachment count (0 for a depth-only pass).
 * @return The default states.
 */
::vsg::GraphicsPipelineStates makeScenePipelineStates(const VkExtent2D& extent, bool depth_test, bool depth_write, int color_count)
{
    auto raster_state      = ::vsg::RasterizationState::create();
    raster_state->cullMode = VK_CULL_MODE_NONE; // tolerate either winding order
    auto depth_state       = ::vsg::DepthStencilState::create();
    depth_state->depthTestEnable  = depth_test ? VK_TRUE : VK_FALSE;
    depth_state->depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    // One colour-blend attachment per COLOUR ATTACHMENT this pass has, blending
    // off and writing every channel. The count must match the render pass'
    // colourAttachmentCount or pipeline creation fails
    // (VUID-VkGraphicsPipelineCreateInfo-renderPass-06055), so a DEPTH-ONLY pass
    // (color_count == 0) declares NONE — declaring vsg's default single
    // attachment would make the pipeline unbuildable against a depth-only
    // render pass. One code path covers 0 / 1 / N.
    ::vsg::ColorBlendState::ColorBlendAttachments blend_attachments;
    blend_attachments.reserve(static_cast<std::size_t>(color_count));
    for (int i = 0; i < color_count; ++i) {
        VkPipelineColorBlendAttachmentState attachment = {};
        attachment.blendEnable         = VK_FALSE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.colorBlendOp        = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp        = VK_BLEND_OP_ADD;
        attachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blend_attachments.push_back(attachment);
    }
    auto blend_state = ::vsg::ColorBlendState::create(blend_attachments);
    return ::vsg::GraphicsPipelineStates{
        depth_state,
        raster_state,
        blend_state,
        ::vsg::InputAssemblyState::create(),
        ::vsg::MultisampleState::create(),
        ::vsg::ViewportState::create(extent),
    };
}


namespace
{

/**
 * @brief The process-wide table of compiled stages (its bound lives on kMaxCompiledStageEntries).
 *
 * A function-local static reached through a call, so the table and the diagnostic that reports its
 * size are the same object: no session owns it (it is shared by every session, which is the point),
 * so the bound is the only thing keeping it from growing, and the bound needs a witness.
 */
struct CompiledStageTable
{
    /// One compiled program: the program it came from (OWNED: the key is its address), the revision
    /// it was compiled at, and its SPIR-V stages.
    struct Entry
    {
        vine::intrusive_ptr<const vine::graphics::ShaderProgram> owner;
        std::uint64_t                                            revision = 0;
        std::uint64_t                                            inserted = 0;
        ::vsg::ShaderStages                                      stages;

        /** @brief Gets the insertion sequence (FIFO order for the capacity trim). */
        std::uint64_t sequence() const noexcept { return inserted; }
    };

    std::map<std::pair<const void*, std::uint64_t>, Entry> entries; ///< Keyed by (program, revision).
    InsertionClock                                         clock;   ///< FIFO order for the capacity trim.
};

/**
 * @brief Gets the table (see @ref CompiledStageTable).
 *
 * @return The process-wide compiled-stage table.
 */
CompiledStageTable& compiledStageTable()
{
    static CompiledStageTable table;
    return table;
}

/**
 * @brief Compiles a shading program into vsg stages, once per (program, revision).
 *
 * glslang is the expensive part of building the set, and every pass/depth-mode
 * variant of one program uses the same stages, so the compiled SPIR-V is shared
 * (the ShaderSet only adds interface declarations on top).
 *
 * The stages come from the PROGRAM the caller named (the engine's own texts live in the SDK,
 * BuiltinShaders.hpp): the SDK owns the shading TEXT, this backend only compiles it and declares the
 * ABI it is bound through. A program with no stages — or one glslang refuses — caches an EMPTY list,
 * and buildVineShaderSet then declines it: the caller reports it and nothing is drawn, rather than
 * shading with a program the host did not name. A null program declines the same way.
 *
 * The cache OWNS the program it is keyed by, and keys on (program, revision): a key that was only an
 * address could serve a dead program's stages to a new one allocated at the same address, and a
 * program edited in place announces itself through its revision (the rule every cache here follows).
 *
 * ONE table, PROCESS-wide, and BOUNDED (unlike the bridges' caches, which are per session and are
 * released with it): the programs a session shades by default are the ENGINE's own — process-wide
 * singletons — and a second session should not pay for compiling them again. What it may not do is
 * grow without end: an entry holds the program and its SPIR-V, so a host that churns programs (a
 * shader editor makes a new revision per edit) would otherwise leave one entry per revision for the
 * life of the process. The bound is the module's own capacity rule (trimToCapacity): the OLDEST
 * entry goes first, which only costs a recompile — the same "never correctness, only the fast path"
 * bargain the bridges' caches make — and a null/absent program recompiles into an empty list rather
 * than being remembered, so the table never has to remember "this one failed".
 *
 * @param program Program to compile (null yields an empty list).
 * @return Compiled stages, or an empty list when the program cannot be used.
 */
::vsg::ShaderStages compiledStages(const vine::intrusive_ptr<const vine::graphics::ShaderProgram>& program)
{
    // Keyed by (address, revision) with the entry owning the program. The capacity trim below needs
    // one accessor name on the entry (the same shape OwnedCache's entries expose), and the returned
    // stages are COPIED out rather than referenced into the table: an entry can be evicted by a
    // later call, so a reference into it would be a dangling one waiting for the next program.
    auto& cache = compiledStageTable().entries;
    auto& clock = compiledStageTable().clock;

    const auto revision = program != nullptr ? program->revision() : 0u;
    const auto key      = std::make_pair(static_cast<const void*>(program.get()), revision);
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second.stages;
    }

    ::vsg::ShaderStages stages;
    if (program != nullptr) {
        auto compiler = ::vsg::ShaderCompiler::create();
        if (compiler != nullptr && compiler->supported()) {
            for (std::size_t i = 0; i < program->stageCount(); ++i) {
                const auto* stage_spec = program->stage(i);
                if (stage_spec == nullptr) {
                    stages.clear();
                    break;
                }
                const auto flag = stage_spec->type == vine::graphics::ShaderStageType::Vertex
                                      ? VK_SHADER_STAGE_VERTEX_BIT
                                      : VK_SHADER_STAGE_FRAGMENT_BIT;
                auto       stage = ::vsg::ShaderStage::create(flag, stage_spec->entryPoint.stdstr(),
                                                              stage_spec->source.stdstr());
                if (!compiler->compile(stage)) {
                    stages.clear();
                    break;
                }
                stages.push_back(stage);
            }
        }
    }
    cache.emplace(key, CompiledStageTable::Entry{ program, revision, clock.tick(), stages });
    // The newest entry is never the one this trims: trimToCapacity removes by the smallest sequence,
    // and the entry just inserted has the largest one.
    trimToCapacity(cache, kMaxCompiledStageEntries);
    return stages;
}

}  // namespace

std::size_t compiledStageCacheCount() noexcept
{
    // The table is process-wide, so it has no session to ask and no owner to count it: this is the
    // only witness of the bound (see kMaxCompiledStageEntries) that the table holds to.
    return compiledStageTable().entries.size();
}

DrawBlockSetBinding::DrawBlockSetBinding() :
    Inherit(1u) // set 1
{
}

bool DrawBlockSetBinding::compatibleDescriptorSetLayout(const ::vsg::DescriptorSetLayout& dsl) const
{
    // Exactly one dynamic uniform buffer at binding 0: anything else is a different set 1,
    // so the caller must not reuse a pipeline layout built against it.
    if (dsl.bindings.size() != 1u) {
        return false;
    }
    const auto& binding = dsl.bindings.front();
    return binding.binding == 0u && binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
           binding.descriptorCount == 1u;
}

::vsg::ref_ptr<::vsg::DescriptorSetLayout> DrawBlockSetBinding::createDescriptorSetLayout()
{
    auto layout = ::vsg::DescriptorSetLayout::create();
    // The draw block is read by the FRAGMENT stage (the opacity scales the fragment alpha).
    layout->addBinding(0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1u, VK_SHADER_STAGE_FRAGMENT_BIT);
    return layout;
}

::vsg::ref_ptr<::vsg::StateCommand> DrawBlockSetBinding::createStateCommand(::vsg::ref_ptr<::vsg::PipelineLayout>)
{
    // Deliberately empty: the bind command carries a per-drawable dynamic offset, so it
    // cannot be one shared command per variant (see the header).
    return {};
}

::vsg::ref_ptr<::vsg::ShaderSet> buildVineShaderSet(vine::intrusive_ptr<const vine::graphics::ShaderProgram> program, const VkExtent2D& extent, bool depth_test, bool depth_write, int color_count)
{
    // The stages come from the program the caller named (the engine's own texts live in the SDK,
    // BuiltinShaders.hpp). A program that has no stages, or that glslang refuses, DECLINES here: the
    // caller reports it and the drawable is not drawn — nothing is shaded with a program the host did
    // not name, which is what makes the shading side free of hidden defaults.
    // By VALUE: the table the stages come from is bounded and trims its oldest entry, so a
    // reference into it would be a dangling one waiting for the next program to be compiled.
    const ::vsg::ShaderStages stages = compiledStages(program);
    if (stages.empty()) {
        return {};
    }
    // The canonical shader LOCATIONS are the SDK's ABI (ShaderAbi.hpp), not a
    // backend choice; the vsg_* names are only this backend's binding aliases.
    using vine::graphics::attributeLocation;
    using vine::graphics::VertexAttribute;

    auto shader_set = ::vsg::ShaderSet::create(stages);
    // Attributes: the canonical four, in the BINDING ORDER the data node binds
    // them (positions, normals, texcoords, colours).
    shader_set->addAttributeBinding("vine_Vertex", "", attributeLocation(VertexAttribute::Position),
                                    VK_FORMAT_R32G32B32_SFLOAT, ::vsg::vec3Array::create(1));
    shader_set->addAttributeBinding("vine_Normal", "", attributeLocation(VertexAttribute::Normal),
                                    VK_FORMAT_R32G32B32_SFLOAT, ::vsg::vec3Array::create(1));
    // The two optional attributes carry the define that gates them in the GLSL:
    // assigning an array enables the define (vsg's assignArray does that), which
    // selects the compiled variant that declares the attribute. Geometry without
    // an authored colour therefore draws the variant without vine_Color instead of
    // being padded with a white carrier.
    shader_set->addAttributeBinding("vine_TexCoord0", "VINE_DIFFUSE_MAP", attributeLocation(VertexAttribute::TexCoord0),
                                    VK_FORMAT_R32G32_SFLOAT, ::vsg::vec2Array::create(1));
    shader_set->addAttributeBinding("vine_Color", "VINE_VERTEX_COLOR", attributeLocation(VertexAttribute::Color),
                                    VK_FORMAT_R32G32B32A32_SFLOAT, ::vsg::vec4Array::create(1));
    // Material: the ENGINE's block (ShaderAbi.hpp VineMaterialBlock), declared with the bytes the
    // material manager fills — our own ABI, not a vsg material type. The sample only states the size
    // and layout vsg must expect, exactly as the lights / per-drawable bindings below do.
    shader_set->addDescriptorBinding("material", "", 0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT,
                                     ::vsg::ubyteArray::create(
                                         static_cast<uint32_t>(sizeof(vine::graphics::VineMaterialBlock))));
    // Texture: the resolved diffuse map (the material's own, or the cache's white
    // fallback), gated so geometry without UVs compiles without the sampler.
    shader_set->addDescriptorBinding("diffuseMap", "VINE_DIFFUSE_MAP", 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, {});
    // Per-view lights: a UBO, because the 128-byte push-constant range is
    // already spoken for by the camera matrices vsg pushes per drawable (see
    // VineLightsBlock). The host binds this set's own lights buffer here; vsg
    // assigns nothing to it, which is why it is declared with an empty sample.
    shader_set->addDescriptorBinding("vine_lights", "", 0, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, ::vsg::ubyteArray::create(static_cast<uint32_t>(sizeof(VineLightsBlock))));
    // The shadow ABI (ShaderAbi.hpp): the map the pass declared as an input, and the block that
    // places its fragments in it. DECLARED ALWAYS, unlike the fullscreen path's per-pass variant:
    // this set is shared per (target, depth mode) — the program is picked once for a session and a
    // content slot reuses it — so a shadowed variant would double that cache for every target and a
    // host program would still have no shadowed twin to pick. One text, switched at runtime by
    // `params.x`: the slot binds the real pair when the pass declared a shadow input and a valid
    // stand-in with the block disabled when it did not (see VsgContentSlot).
    shader_set->addDescriptorBinding("shadow_map", "", 0, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT, {});
    shader_set->addDescriptorBinding("vine_shadow", "", 0, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT,
                                     ::vsg::ubyteArray::create(
                                         static_cast<uint32_t>(sizeof(vine::graphics::VineShadowBlock))));
    // Per-DRAWABLE values (VineDrawBlock): one slot per drawn command, selected by a dynamic
    // offset from a buffer the whole scene shares. It is a CUSTOM set (1) rather than an ordinary
    // binding because the bind command is per drawable — see DrawBlockSetBinding — so this
    // contributes the layout and the bridge binds it.
    //
    // The declaration below it is what puts set 1 in the pipeline layout AT ALL: vsg derives the
    // range of sets from `descriptorBindings` (ShaderSet::descriptorSetRange), and a shader that
    // declares a set the layout does not have is an invalid pipeline — the driver resolves a set
    // layout that does not exist. The declaration is never assigned a descriptor, because
    // DescriptorConfigurator::assignDefaults skips a set a custom binding owns (that is what
    // makes the two agree: the custom binding owns set 1's LAYOUT, this owns its RANGE).
    shader_set->addDescriptorBinding("vine_draw", "", 1, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                     VK_SHADER_STAGE_FRAGMENT_BIT,
                                     ::vsg::ubyteArray::create(
                                         static_cast<uint32_t>(sizeof(vine::graphics::VineDrawBlock))));
    shader_set->customDescriptorSetBindings.push_back(DrawBlockSetBinding::create());
    // Camera matrices. The L1 contract is the SDK's VineViewBlock/VineDrawBlock
    // (ShaderAbi.hpp); on this backend the 128-byte push range is their L2
    // realization:
    //   pc.projection == VineViewBlock.proj
    //   pc.modelView  == VineViewBlock.view * VineDrawBlock.model
    // vsg fills it from its own matrix stacks (the name "pc" and this range are its
    // convention). VineViewBlock is far larger than the range, so the push is an
    // IMPLEMENTATION of the L1 pair, not the contract itself: a backend without a
    // push range binds the blocks.
    shader_set->addPushConstantRange("pc", "", VK_SHADER_STAGE_VERTEX_BIT, 0, 128);
    shader_set->defaultGraphicsPipelineStates = makeScenePipelineStates(extent, depth_test, depth_write, color_count);
    return shader_set;
}

::vsg::ref_ptr<::vsg::ShaderSet> makeContentShaderSet(vine::intrusive_ptr<const vine::graphics::ShaderProgram> program, const VkExtent2D& extent, bool depth_test, bool depth_write, int color_count)
{
    // EVERY content set this backend builds is ours. vsg's built-in sets
    // (createPhongShaderSet / createFlatShadedShaderSet) are deliberately not used
    // at all: a set of theirs carries their declarations, their attribute
    // locations and their light source, so mixing the two would mean two shading
    // ABIs to keep in step — and the engine owns the shading text now
    // (BuiltinShaders.hpp).
    //
    // A program that cannot be used (none at all, no stages, a failed compile) is DECLINED — null, no
    // substitution. The caller reports it and draws nothing: shading it with another program (ours or
    // a library's) would show the host a picture it did not ask for and cannot tell apart from the one
    // it did, which is worse than an empty frame it can see the reason for.
    return buildVineShaderSet(std::move(program), extent, depth_test, depth_write, color_count);
}

VkFormat toColorFormat(vine::graphics::RenderTarget::ColorFormat f)
{
    switch (f) {
    case vine::graphics::RenderTarget::ColorFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case vine::graphics::RenderTarget::ColorFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case vine::graphics::RenderTarget::ColorFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

VkFormat toDepthFormat(vine::graphics::RenderTarget::DepthFormat f)
{
    switch (f) {
    case vine::graphics::RenderTarget::DepthFormat::D16: return VK_FORMAT_D16_UNORM;
    case vine::graphics::RenderTarget::DepthFormat::D24: return VK_FORMAT_D24_UNORM_S8_UINT;
    case vine::graphics::RenderTarget::DepthFormat::D32:
    case vine::graphics::RenderTarget::DepthFormat::D32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_D32_SFLOAT;
}

/**
 * @brief The two subpass dependencies every COLOUR+DEPTH variant shares.
 *
 * They are built once and reused by every variant of a pass because the Vulkan
 * spec's Render Pass Compatibility rules exempt initial/final layouts and
 * load/store ops, but NOT subpass dependencies: two render passes are only
 * compatible (and a pass may therefore swap between them at run time, keeping its
 * pipelines) when their dependencies match field for field. Reusing this helper
 * is what makes that structural — the masks are deliberately independent of the
 * load ops, since the pass lifecycle picks them later.
 *
 * @return The subpass-external dependencies (writes in, reads out).
 */
::vsg::RenderPass::Dependencies makeColorDepthDependencies()
{
    constexpr VkPipelineStageFlags k_attachments =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags k_attachment_writes =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    ::vsg::RenderPass::Dependencies dependencies;

    // External -> subpass: the previous frames' attachment writes are made
    // available before this subpass CLEARs or LOADs the attachments.
    ::vsg::SubpassDependency ext_to_sub = {};
    ext_to_sub.srcSubpass               = VK_SUBPASS_EXTERNAL;
    ext_to_sub.dstSubpass               = 0;
    ext_to_sub.srcStageMask             = k_attachments;
    ext_to_sub.dstStageMask             = k_attachments;
    ext_to_sub.srcAccessMask            = k_attachment_writes;
    ext_to_sub.dstAccessMask            = k_attachment_writes;
    dependencies.push_back(ext_to_sub);

    // Subpass -> external: the colour (and depth) writes become visible to a
    // later pass that samples them.
    ::vsg::SubpassDependency sub_to_ext = {};
    sub_to_ext.srcSubpass               = 0;
    sub_to_ext.dstSubpass               = VK_SUBPASS_EXTERNAL;
    sub_to_ext.srcStageMask             = k_attachments;
    sub_to_ext.dstStageMask             = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sub_to_ext.srcAccessMask            = k_attachment_writes;
    sub_to_ext.dstAccessMask            = VK_ACCESS_SHADER_READ_BIT;
    dependencies.push_back(sub_to_ext);

    return dependencies;
}

::vsg::ref_ptr<::vsg::RenderPass> makeColorDepthRenderPass(
    ::vsg::Device*                    device,
    const std::vector<VkFormat>&      color_formats,
    VkFormat                          depth_format,
    VkImageLayout                     depth_initial,
    bool                              promote_depth,
    bool                              color_clear)
{
    const bool has_depth = depth_format != VK_FORMAT_UNDEFINED;

    ::vsg::RenderPass::Attachments attachments;
    attachments.reserve(color_formats.size() + (has_depth ? 1u : 0u));

    ::vsg::SubpassDescription subpass = {};
    subpass.pipelineBindPoint         = VK_PIPELINE_BIND_POINT_GRAPHICS;

    uint32_t attachment_index = 0;
    for (const VkFormat color_format : color_formats) {
        ::vsg::AttachmentDescription color = {};
        color.format                       = color_format;
        color.samples                      = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp                       = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // A CLEAR may start from an undefined image; a LOAD reads what the
        // previous pass left, which this backend always ends in
        // SHADER_READ_ONLY (see the subpass-external dependency below).
        color.initialLayout = color_clear ? VK_IMAGE_LAYOUT_UNDEFINED
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        color.finalLayout                  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments.push_back(color);

        ::vsg::AttachmentReference color_ref = {};
        color_ref.attachment                 = attachment_index++;
        color_ref.layout                     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        subpass.colorAttachments.emplace_back(color_ref);
    }

    if (has_depth) {
        ::vsg::AttachmentDescription depth = {};
        depth.format                       = depth_format;
        depth.samples                      = VK_SAMPLE_COUNT_1_BIT;
        // An UNDEFINED image may only be CLEARed, so the load-op follows from the
        // layout the caller names; a LOAD of an image in an unexpected layout is
        // the mismatch VUID-vkCmdDraw-None-09600 reports.
        depth.loadOp = depth_initial == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                 : VK_ATTACHMENT_LOAD_OP_LOAD;
        // Stored: when @p promote_depth the depth is left sampleable (for
        // reconstruction / SSAO); otherwise it stays a plain depth attachment so
        // ANOTHER target can borrow it for a later depth test in the same frame
        // (see RenderTarget::shareDepth).
        depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE;
        depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout                = depth_initial;
        depth.finalLayout = promote_depth ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depth);

        ::vsg::AttachmentReference depth_ref = {};
        depth_ref.attachment                 = attachment_index;
        depth_ref.layout                     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.depthStencilAttachments.emplace_back(depth_ref);
    }

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass },
                                     makeColorDepthDependencies());
}

::vsg::ref_ptr<::vsg::RenderPass> makeDepthOnlyRenderPass(::vsg::Device* device, VkFormat depth_format,
                                                          VkImageLayout depth_initial)
{
    ::vsg::AttachmentDescription depth = {};
    depth.format                       = depth_format;
    depth.samples                      = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp                       = depth_initial == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                                      : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp                      = VK_ATTACHMENT_STORE_OP_STORE; // sampled later as a shadow map
    depth.stencilLoadOp                = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp               = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // A depth-only target's image always ends sampleable (that is what the pass
    // exists for), so a preserving pass names that layout and hands the image
    // back in it; a clearing pass may start from UNDEFINED.
    depth.initialLayout                = depth_initial;
    depth.finalLayout                  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    ::vsg::RenderPass::Attachments attachments{ depth };

    ::vsg::AttachmentReference depth_ref = {};
    depth_ref.attachment                 = 0;
    depth_ref.layout                     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    ::vsg::SubpassDescription subpass = {};
    subpass.pipelineBindPoint         = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.depthStencilAttachments.emplace_back(depth_ref);

    ::vsg::RenderPass::Dependencies dependencies;
    // UNDEFINED -> DEPTH_STENCIL_ATTACHMENT before the subpass.
    ::vsg::SubpassDependency ext_to_sub = {};
    ext_to_sub.srcSubpass               = VK_SUBPASS_EXTERNAL;
    ext_to_sub.dstSubpass               = 0;
    ext_to_sub.srcStageMask             = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstStageMask             = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    ext_to_sub.dstAccessMask            = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies.push_back(ext_to_sub);

    // After the subpass, transition to SHADER_READ_ONLY and make the depth
    // writes visible to a later pass that samples the shadow map.
    ::vsg::SubpassDependency sub_to_ext = {};
    sub_to_ext.srcSubpass               = 0;
    sub_to_ext.dstSubpass               = VK_SUBPASS_EXTERNAL;
    sub_to_ext.srcStageMask             = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    sub_to_ext.dstStageMask             = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    sub_to_ext.srcAccessMask            = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    sub_to_ext.dstAccessMask            = VK_ACCESS_SHADER_READ_BIT;
    dependencies.push_back(sub_to_ext);

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass }, dependencies);
}

// The body is the moved definition: its documentation lives on the declaration.

PassRenderPassPlan planPassRenderPass(bool color_clear,
                                      bool want_depth_clear,
                                      bool has_depth,
                                      bool promote_requested,
                                      bool any_load_pass,
                                      bool depth_seeded,
                                      bool borrowed_depth)
{
    PassRenderPassPlan plan;
    plan.color_load = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    if (!has_depth) {
        return plan; // colour-only target: the depth fields stay inert
    }
    if (borrowed_depth) {
        // The source's pass defined this depth earlier in the frame and owns
        // its layout; this pass only tests against it.
        plan.depth_load    = VK_ATTACHMENT_LOAD_OP_LOAD;
        plan.depth_initial = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        plan.promote_depth = false;
        plan.seed_required = false;
        return plan;
    }
    if (want_depth_clear) {
        plan.depth_load    = VK_ATTACHMENT_LOAD_OP_CLEAR;
        plan.depth_initial = VK_IMAGE_LAYOUT_UNDEFINED;
        // Promotion is only legal while no pass of the target LOADs depth: a
        // LOAD pass must find the image in the attachment layout.
        plan.promote_depth = promote_requested && !any_load_pass;
        plan.seed_required = false;
        return plan;
    }
    // Preserving pass: LOAD the depth the previous frame / pass left. An
    // UNDEFINED image cannot be loaded, so an unseeded target needs the CLEAR
    // (seed) variant recorded once first (see PassObjects::render_pass_transient).
    plan.depth_load    = VK_ATTACHMENT_LOAD_OP_LOAD;
    plan.depth_initial = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    plan.promote_depth = false; // LOAD and promotion are mutually exclusive
    plan.seed_required = !depth_seeded;
    return plan;
}

PassVariant planPassVariant(bool has_color, bool has_depth, bool borrowed, bool promote_requested, bool any_load_pass,
                            bool depth_seeded, bool color_seeded, bool want_color_clear, bool want_depth_clear,
                            bool depth_left_promoted)
{
    // The colour bootstrap: a target whose colour image is still UNDEFINED may not
    // LOAD it, so the pass that first renders it CLEARs — for ONE frame only (a
    // transient variant), or it would wipe what its siblings drew every frame.
    const bool bootstrap_color_clear = has_color && !color_seeded;
    const bool color_clear           = has_color ? (want_color_clear || bootstrap_color_clear) : true;

    const PassRenderPassPlan plan =
        planPassRenderPass(color_clear, want_depth_clear, has_depth, promote_requested, any_load_pass, depth_seeded,
                           borrowed);

    PassVariant variant;
    variant.color_load    = color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    variant.depth_load    = plan.depth_load;
    variant.promote_depth = plan.promote_depth;
    // The colour and depth regimes of a target differ: its colour attachments
    // always end sampleable, while its depth ends sampleable only for a
    // depth-only target (that is what such a target is for) or after a pass that
    // promoted it.
    variant.steady_depth_initial =
        has_color ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    variant.steady_color_load = want_color_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    if (!has_depth) {
        variant.transient = bootstrap_color_clear;
        return variant;
    }

    const bool depth_load = plan.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD;
    // A preserving pass may find the depth in SHADER_READ_ONLY: an earlier frame's
    // promoting pass left it there and nothing has replaced that layout yet. It
    // then names THAT layout for one frame and hands the image back in the layout
    // its consumers expect. A depth-only target needs no such frame — its steady
    // layout IS SHADER_READ_ONLY.
    const bool needs_revoke = has_color && depth_load && depth_left_promoted;
    // The layout the RECORDED pass declares: a CLEAR starts from an undefined
    // image whatever it held before, so only a LOAD has to name the real one.
    variant.depth_initial = !depth_load         ? VK_IMAGE_LAYOUT_UNDEFINED
                            : needs_revoke      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : plan.seed_required ? VK_IMAGE_LAYOUT_UNDEFINED
                                                 : variant.steady_depth_initial;
    variant.transient     = plan.seed_required || needs_revoke || bootstrap_color_clear;
    return variant;
}

bool passVariantIsStale(bool recorded_want_color_clear, bool recorded_want_depth_clear, bool want_color_clear,
                        bool want_depth_clear)
{
    return recorded_want_color_clear != want_color_clear || recorded_want_depth_clear != want_depth_clear;
}

const std::string& fullscreenVertexSource()
{
    // The SDK owns the text (BuiltinShaders::fullscreenVertexProgram); this backend only serves it
    // in the shape vsg wants (a source string to fuse with a fragment stage). Reading the stage from
    // the program — rather than embedding a second copy — is what makes it impossible for a
    // full-screen program to be compiled against a triangle the engine did not state.
    static const std::string source = [] {
        const auto program = vine::graphics::fullscreenVertexProgram();
        return program->stage(0)->source.stdstr();
    }();
    return source;
}

::vsg::GraphicsPipelineStates makeOverlayPipelineStates(const VkExtent2D& extent)
{
    auto raster      = ::vsg::RasterizationState::create();
    raster->cullMode = VK_CULL_MODE_NONE;
    auto depth_state = ::vsg::DepthStencilState::create();
    depth_state->depthTestEnable  = VK_FALSE;
    depth_state->depthWriteEnable = VK_FALSE;
    return ::vsg::GraphicsPipelineStates{
        depth_state,
        raster,
        ::vsg::ColorBlendState::create(),
        ::vsg::InputAssemblyState::create(),
        ::vsg::MultisampleState::create(),
        ::vsg::ViewportState::create(extent),
    };
}

/**
 * @brief Compiles an overlay drawable's stages into a ShaderSet ready to configure.
 *
 * A full-screen draw is a fullscreen triangle (the SDK's canonical vertex stage), depth test and
 * write off, blending off, the pass' own viewport, its samples bound to set 0 and its shader
 * modules compiled at run time. This is the half that does not depend on WHAT is sampled; the
 * caller adds the descriptor bindings and textures.
 *
 * @param vertex_source   Vertex stage source (see fullscreenVertexSource).
 * @param fragment_source Fragment stage source.
 * @param fragment_entry  Fragment entry point name ("main" for the built-in one).
 * @param extent          Surface extent for the baked static viewport.
 * @param failure         Receives why it failed (NoCompiler / CompileFailed).
 * @return The shader set, or null when the compiler is unavailable or a stage fails.
 */
::vsg::ref_ptr<::vsg::ShaderSet> makeOverlayShaderSet(const std::string& vertex_source,
                                                     const std::string& fragment_source,
                                                     const std::string& fragment_entry, const VkExtent2D& extent,
                                                     ProgramNodeFailure* failure)
{
    auto vs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", vertex_source);
    auto fs = ::vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, fragment_entry, fragment_source);

    auto compiler = ::vsg::ShaderCompiler::create();
    if (compiler == nullptr || !compiler->supported()) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::NoCompiler;
        }
        return {};
    }
    if (!compiler->compile(vs) || !compiler->compile(fs)) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::CompileFailed;
        }
        return {};
    }
    auto shaderSet    = ::vsg::ShaderSet::create();
    shaderSet->stages = ::vsg::ShaderStages{ vs, fs };
    shaderSet->defaultGraphicsPipelineStates = makeOverlayPipelineStates(extent);
    return shaderSet;
}

/**
 * @brief Wraps a configured overlay pipeline into the drawable an overlay View records.
 *
 * @param config    Configured pipeline (its textures assigned; init() runs here).
 * @param push_data Per-frame push-constant bytes (every full-screen draw reads the pass' view
 *                  and lights; a program that uses neither still gets the block).
 * @return The state group holding the pipeline and the fullscreen triangle draw.
 */
::vsg::ref_ptr<::vsg::StateGroup> makeOverlayStateGroup(const ::vsg::ref_ptr<::vsg::GraphicsPipelineConfigurator>& config,
                                                       const ::vsg::ref_ptr<::vsg::Data>& push_data)
{
    config->init();
    auto state_group = ::vsg::StateGroup::create();
    config->copyTo(state_group, ::vsg::ref_ptr<::vsg::SharedObjects>());
    auto draw_commands = ::vsg::Commands::create();
    if (push_data != nullptr) {
        // The per-frame block is recorded from push_data's CURRENT bytes, so the
        // caller mutates and re-records it every frame.
        draw_commands->addChild(::vsg::PushConstants::create(VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_data.get()));
    }
    draw_commands->addChild(::vsg::Draw::create(3, 1, 0, 0));
    state_group->addChild(draw_commands);
    return state_group;
}


/**
 * @brief Collects the descriptor bindings a GLSL source DECLARES.
 *
 * The full-screen program ABI (see makeFullscreenProgramNode) gives every source
 * texture a binding index, so what a program NEEDS is knowable before anything is
 * created — but vsg 1.1.16 exposes no shader reflection, and this backend already
 * compiles the user's GLSL itself. Read the bindings from the source: every
 * `layout(...)` qualifier that mentions `binding`, with `set` defaulting to 0 (the
 * only set this pass uses). It is a scan, not a parser — the ABI's qualifier
 * grammar is small and fixed — and it fails SAFE: what it cannot classify is
 * reported to the caller as unsupported rather than silently ignored.
 *
 * @param source GLSL source of one stage.
 * @return The declared (set, binding) pairs, in source order.
 */
bool programSamplesDepth(vine::raw_ptr<const vine::graphics::ShaderProgram> program, std::size_t color_count)
{
    if (program == nullptr) {
        return false;
    }
    const std::uint32_t depth_binding = static_cast<std::uint32_t>(color_count);
    for (const auto& stage : program->stages()) {
        if (stage.type != vine::graphics::ShaderStageType::Fragment) {
            continue;
        }
        for (const auto& [set, binding] : declaredBindings(stage.source.stdstr())) {
            if (set == 0u && binding == depth_binding) {
                return true;
            }
        }
    }
    return false;
}

::vsg::ref_ptr<::vsg::Node> makeFullscreenProgramNode(
    vine::raw_ptr<const vine::graphics::ShaderProgram> program,
    const ::vsg::ImageViews&                          image_views,
    ::vsg::ref_ptr<::vsg::ImageView>                  depth_view,
    const FullscreenShadowInput&                      shadow,
    const VkExtent2D&                                 extent,
    ::vsg::ref_ptr<::vsg::Data>                       push_data,
    ProgramNodeFailure*                               failure)
{
    // The caller reports (it knows the pass and the sink); this helper only says
    // why it could not build the node.
    if (failure != nullptr) {
        *failure = ProgramNodeFailure::None;
    }
    if (program == nullptr || image_views.empty() || push_data == nullptr) {
        return ::vsg::ref_ptr<::vsg::Node>();
    }
    // Locate the user's fragment stage.
    const vine::graphics::ShaderStage* fs_spec = nullptr;
    for (const auto& stage : program->stages()) {
        if (stage.type == vine::graphics::ShaderStageType::Fragment) {
            fs_spec = &stage;
            break;
        }
    }
    if (fs_spec == nullptr) {
        if (failure != nullptr) {
            *failure = ProgramNodeFailure::NoFragmentStage;
        }
        return ::vsg::ref_ptr<::vsg::Node>();
    }

    const std::string vertex_source = fullscreenVertexSource();

    auto shader_set = makeOverlayShaderSet(vertex_source, fs_spec->source.stdstr(), fs_spec->entryPoint.stdstr(), extent,
                                           failure);
    if (shader_set == nullptr) {
        return ::vsg::ref_ptr<::vsg::Node>();
    }

    // This pass provides ONE descriptor set: binding i = the source's i-th colour
    // attachment, and binding N (= the colour count) = the source's depth — but
    // only when the caller has a sampleable depth to bind. A fragment stage that
    // declares anything else would build a pipeline whose layout lacks that
    // binding and fail at DRAW time (a validation error per frame, with nothing
    // telling the host why), so refuse it here instead, before anything is built.
    // What this node can fill, as a set: the source's colour attachments, its depth WHILE that one
    // is sampleable, and the shadow's two slots while a shadow was handed over. A declaration
    // outside it is refused here rather than left to fail per frame at draw time.
    const std::uint32_t        color_count     = static_cast<std::uint32_t>(image_views.size());
    const std::uint32_t        shadow_map_slot = color_count + 1u;
    const std::uint32_t        shadow_block_slot = color_count + 2u;
    const bool                 has_shadow      = shadow.map != nullptr && shadow.block != nullptr;
    const std::vector<std::uint32_t> fillable  = [&] {
        std::vector<std::uint32_t> slots;
        for (std::uint32_t i = 0; i < color_count; ++i) {
            slots.push_back(i);
        }
        if (depth_view != nullptr) {
            slots.push_back(color_count);
        }
        if (has_shadow) {
            slots.push_back(shadow_map_slot);
            slots.push_back(shadow_block_slot);
        }
        return slots;
    }();
    for (const auto& [set, binding] : declaredBindings(fs_spec->source.stdstr())) {
        if (set != 0u || std::find(fillable.begin(), fillable.end(), binding) == fillable.end()) {
            if (failure != nullptr) {
                *failure = ProgramNodeFailure::MissingDescriptorBinding;
            }
            return ::vsg::ref_ptr<::vsg::Node>();
        }
    }

    for (std::size_t i = 0; i < image_views.size(); ++i) {
        shader_set->addDescriptorBinding("gbuffer" + std::to_string(i), "", 0, static_cast<uint32_t>(i),
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         ::vsg::ref_ptr<::vsg::Data>());
    }
    if (depth_view != nullptr) {
        shader_set->addDescriptorBinding("gbuffer_depth", "", 0, static_cast<uint32_t>(image_views.size()),
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         ::vsg::ref_ptr<::vsg::Data>());
    }
    if (has_shadow) {
        // The map is a raw depth view (no compare sampler): the shader compares it itself, so the
        // filter must not interpolate between casters — nearest keeps the value the rasteriser
        // wrote.
        shader_set->addDescriptorBinding("shadow_map", "", 0, shadow_map_slot,
                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         ::vsg::ref_ptr<::vsg::Data>());
        shader_set->addDescriptorBinding("shadow_block", "", 0, shadow_block_slot, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT,
                                         ::vsg::ubyteArray::create(
                                             static_cast<uint32_t>(sizeof(vine::graphics::VineShadowBlock))));
    }
    // Per-frame light/view parameters (LightPushBlock, the full 128-byte
    // range; its size is asserted against sizeof(LightPushBlock)).
    shader_set->addPushConstantRange("pc_light", "", VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                     static_cast<uint32_t>(sizeof(LightPushBlock)));

    auto config = ::vsg::GraphicsPipelineConfigurator::create(shader_set);
    auto sampler = ::vsg::Sampler::create();
    for (std::size_t i = 0; i < image_views.size(); ++i) {
        auto image_info = ::vsg::ImageInfo::create(sampler, image_views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        config->assignTexture("gbuffer" + std::to_string(i), ::vsg::ImageInfoList{ image_info });
    }
    if (depth_view != nullptr) {
        // Depth is sampled raw (no compare): nearest filter keeps it exact.
        auto depth_sampler   = ::vsg::Sampler::create();
        depth_sampler->magFilter  = VK_FILTER_NEAREST;
        depth_sampler->minFilter  = VK_FILTER_NEAREST;
        depth_sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        auto depth_info = ::vsg::ImageInfo::create(depth_sampler, depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        config->assignTexture("gbuffer_depth", ::vsg::ImageInfoList{ depth_info });
    }
    if (has_shadow) {
        auto shadow_sampler        = ::vsg::Sampler::create();
        shadow_sampler->magFilter  = VK_FILTER_NEAREST;
        shadow_sampler->minFilter  = VK_FILTER_NEAREST;
        shadow_sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        auto shadow_info =
            ::vsg::ImageInfo::create(shadow_sampler, shadow.map, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        config->assignTexture("shadow_map", ::vsg::ImageInfoList{ shadow_info });
        config->assignDescriptor("shadow_block", shadow.block);
    }
    return makeOverlayStateGroup(config, push_data);
}

} // namespace detail

V_VSG_NS_END
