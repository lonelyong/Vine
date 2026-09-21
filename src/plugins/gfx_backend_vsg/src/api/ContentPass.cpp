#include <vine/vsg/api/ContentPass.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include <vsg/core/Array.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/PushConstants.h>

#include <vine/vsg/api/DrawBlock.hpp>
#include <vine/vsg/api/LightBlock.hpp>
#include <vine/vsg/api/ShadowBlock.hpp>

V_VSG_NS_BEGIN

namespace
{

/// @brief Builds a `vine::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

/// @brief Names a table miss the way the message needs it.
const char* missText(FactMiss miss) noexcept
{
    switch (miss)
    {
    case FactMiss::None:
        return "none";
    case FactMiss::Unknown:
        return "the content layer was never told about this identity";
    case FactMiss::Revision:
        return "the table knows this identity at a different revision";
    case FactMiss::Malformed:
        return "the table's entry cannot be drawn (see api/ContentFacts.hpp)";
    }
    return "unknown";
}

/// @brief The bytes of one block, as the storage takes them.
template <typename Block>
std::span<const std::byte> bytesOf(const Block& block) noexcept
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&block), sizeof(Block));
}

/// @brief How many colour textures the pass' inputs offer, as the plan states it (the key's count).
std::uint32_t sampledColorCount(const core::CompiledPass& pass) noexcept
{
    std::uint32_t total = 0;
    for (const core::CompiledInput& input : pass.inputs)
    {
        total += input.color_attachments;
    }
    return total;
}

/// @brief How many DEPTH textures the pass' inputs offer, as the plan states it (the key's count).
std::uint32_t sampledDepthCount(const core::CompiledPass& pass) noexcept
{
    std::uint32_t total = 0;
    for (const core::CompiledInput& input : pass.inputs)
    {
        total += input.depth_sampleable ? 1U : 0U;
    }
    return total;
}

/// @brief The full-screen ABI's push range: the layout the SDK's screen programs declare (see LightPushBlock).
constexpr std::size_t kFullscreenPushBytes = sizeof(vine::vsg::LightPushBlock);
static_assert(kFullscreenPushBytes == 128U, "the full-screen push range is the ABI's 128 bytes");

}  // namespace

ContentPass::ContentPass(const Scope& scope, core::Diagnostics& diagnostics) noexcept
    : scope_(scope)
    , diagnostics_(diagnostics)
{
}

bool ContentPass::record(const core::CompiledPass& pass, const ContentFacts& facts,
                         const core::RenderPassCompatibility& compatibility,
                         std::span<const InputImages> inputs, std::span<const std::byte> view_block,
                         ::vsg::ref_ptr<::vsg::Node>& out)
{
    auto group = ::vsg::Group::create();

    // The plan says what the pass READS; the caller says what those images ARE. The two have to agree before
    // anything is bound - the same discipline the executor applies to a pass' target shape - because a pass
    // that sampled images nobody described would put a picture on screen that no plan explains. A disagreement
    // refuses the WHOLE pass (its inputs are one fact, not a per-draw one), and says which entry moved.
    if (inputs.size() != pass.inputs.size())
    {
        diagnostics_.report(
            vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
            asString("the pass is not drawn: it declares " + std::to_string(pass.inputs.size()) +
                     " input(s) and the caller offered " + std::to_string(inputs.size()) +
                     " (one entry per declared input, in declaration order)"));
        out = group;
        return false;
    }
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        // The DEPTH half of the offer: the plan says whether the input offers a sampleable depth (it is the
        // same fact the shader's binding count comes from), so "the shader declares a depth sampler" and "the
        // caller offered one" cannot disagree silently.
        const bool offers_depth = inputs[index].depth != nullptr;
        if (offers_depth != pass.inputs[index].depth_sampleable)
        {
            diagnostics_.report(
                vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                asString("the pass is not drawn: input " + std::to_string(index) +
                         (offers_depth ? " offers a depth texture where the plan says its depth is not "
                                         "sampleable (a depth a pass preserves, or a lender's, cannot be sampled)"
                                       : " offers no depth texture where the plan says its depth IS sampleable "
                                         "(the shader's binding would have nothing to read)")));
            out = group;
            return false;
        }

        if (inputs[index].colors.size() != pass.inputs[index].color_attachments)
        {
            diagnostics_.report(
                vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                asString("the pass is not drawn: input " + std::to_string(index) + " offers " +
                         std::to_string(inputs[index].colors.size()) +
                         " colour texture(s) where the plan says " +
                         std::to_string(pass.inputs[index].color_attachments)));
            out = group;
            return false;
        }
    }

    // The count the pipeline identity carries comes from the PLAN. The set the draws bind is built per KIND:
    // a content draw binds it at set 1 (after the blocks), a full-screen draw at set 0 (its own ABI), so each
    // kind's set is built from the layer that kind's pipelines were compiled in - and each kind's set is
    // bound once, because the inputs are a property of the pass.
    const std::uint32_t sampled_color_count = sampledColorCount(pass);
    const std::uint32_t sampled_depth_count = sampledDepthCount(pass);

    const Scope::Entry* content_half  = nullptr;
    const Scope::Entry* screen_half   = nullptr;
    bool                has_content   = false;
    for (const Scope::Entry& entry : scope_.entries)
    {
        if (entry.kind == core::DrawKind::Screen)
        {
            screen_half = screen_half == nullptr ? &entry : screen_half;
        }
        else
        {
            content_half = content_half == nullptr ? &entry : content_half;
        }
    }
    for (const core::CompiledDraw& draw : pass.draws)
    {
        has_content = has_content || draw.kind == core::DrawKind::Content;
    }

    ::vsg::ref_ptr<::vsg::BindDescriptorSet> input_set;
    if (has_content)
    {
        if (content_half == nullptr || content_half->pipelines == nullptr)
        {
            reportRefused("the pass' sampled inputs", "no compiled content half was built for this pass");
            out = group;
            return false;
        }
        input_set = makeInputSet(pass, inputs, *content_half->pipelines, 1U);
        if ((sampled_color_count != 0U || sampled_depth_count != 0U) && input_set == nullptr)
        {
            out = group;  // makeInputSet reported why
            return false;
        }
    }

    // One view block per pass: it describes the view, and the pass has one camera. A full-screen draw does not
    // read it (the full-screen ABI binds no blocks), so a pass with nothing but those still pays for one row.
    const BlockStorage::Block view = scope_.storage->writeView(view_block);
    if (!view.valid)
    {
        reportRefused("the pass' view block", "the frame's block budget is full");
        out = group;
        return false;
    }

    // The full-screen halves' images, built on demand: one set per half (its layer's set layout, set 0), and a
    // second screen half of the same pass gets a set of its own over the same images - the same recipe builds
    // their layouts, so the two are compatible with one another.
    const Scope::Entry*                            screen_set_for = nullptr;
    ::vsg::ref_ptr<::vsg::BindDescriptorSet>       screen_set;

    bool complete = true;
    for (const core::CompiledDraw& draw : pass.draws)
    {
        if (draw.kind == core::DrawKind::Screen)
        {
            // The half is found by the program the PLAN names (identity plus revision): the stages are already
            // compiled in the layer the entry names, so nothing here needs the facts table - the geometry-shaped
            // half of the content lookup has no counterpart in a call that draws no geometry.
            const Scope::Entry* half = nullptr;
            bool                same_program = false;
            for (const Scope::Entry& entry : scope_.entries)
            {
                if (entry.kind != core::DrawKind::Screen || entry.program != draw.program.program)
                {
                    continue;
                }
                same_program = true;
                if (entry.revision == draw.program.revision)
                {
                    half = &entry;
                    break;
                }
            }
            if (half == nullptr || half->pipelines == nullptr || half->draws == nullptr)
            {
                reportRefused("a full-screen drawing call",
                              same_program ? "the pass' program is compiled at a different revision"
                                           : "no compiled full-screen half was built for the pass' program");
                complete = false;
                continue;
            }

            if (screen_set_for != half)
            {
                screen_set     = makeInputSet(pass, inputs, *half->pipelines, 0U);
                screen_set_for = half;
                if ((sampled_color_count != 0U || sampled_depth_count != 0U) && screen_set == nullptr)
                {
                    complete = false;  // makeInputSet reported why
                    continue;
                }
            }

            if (!recordScreenDraw(draw, *half, pass, compatibility, screen_set, *group))
            {
                complete = false;
            }
            continue;
        }

        // The packing of the lights is the drawing call's, not the command's: the contract announces lights for
        // ONE call (every command of the call shares them), so one block serves the whole call and the block
        // bytes are written before the first command is recorded - the same "one announcement, one call" rule
        // the viewport follows.
        vine::vsg::VineLightsBlock lights_block;
        const std::size_t          represented = packLightBlock(draw.lights, draw.camera, lights_block);
        const BlockStorage::Block  lights      = scope_.storage->writeLights(bytesOf(lights_block));
        reportLightsDropped(draw.lights.size(), represented, draw.camera.present);
        if (!lights.valid)
        {
            reportRefused("the drawing call's light block", "the frame's block budget is full");
            complete = false;
            continue;
        }

        // The shadow block is written for EVERY call, switch on or off: the same shader text serves a shadowed and
        // an unshadowed pass (the ABI's `params.x` is that switch), so the binding must never be left to chance -
        // an unbound block a shader reads is undefined behaviour, not "no shadow".
        vine::graphics::VineShadowBlock shadow_block;
        const bool                      shadow_on    = packShadowBlock(pass.shadow, draw, shadow_block);
        const BlockStorage::Block       shadow       = scope_.storage->writeShadows(bytesOf(shadow_block));
        (void)shadow_on;
        if (!shadow.valid)
        {
            reportRefused("the drawing call's shadow block", "the frame's block budget is full");
            complete = false;
            continue;
        }

        for (const core::CompiledCommand& command : draw.commands)
        {
            if (!recordCommand(command, draw, pass, facts, compatibility, view.offset, lights.offset, shadow.offset,
                               input_set, sampled_color_count, sampled_depth_count, *group))
            {
                complete = false;
            }
        }
    }

    out = group;
    return complete;
}

::vsg::ref_ptr<::vsg::BindDescriptorSet> ContentPass::makeInputSet(const core::CompiledPass& pass,
                                                                  std::span<const InputImages> inputs,
                                                                  ContentPipeline& layer,
                                                                  std::uint32_t    first_set)
{
    const std::uint32_t texture_count  = sampledColorCount(pass);
    const std::uint32_t depth_count    = sampledDepthCount(pass);
    if (texture_count == 0U && depth_count == 0U)
    {
        return {};  // nothing declared (or nothing produced): there is no sampled set to bind
    }

    // One set per pass and kind, laid out by the layer that compiles the pipelines that kind's draws bind: the
    // set layout object they were compiled against, so the set is exactly the shape they expect. The halves of
    // one kind build their sampled layouts from the same recipe, so they are compatible with one another as
    // well (a pass may switch halves with this set bound).
    const auto set_layout = layer.sampledSetLayout(texture_count, depth_count);
    const auto sampler    = layer.inputSampler();
    if (set_layout == nullptr || sampler == nullptr)
    {
        reportRefused("the pass' sampled inputs", "the sampled-input set could not be built");
        return {};
    }

    // The bindings read the inputs in DECLARATION order: input by input, and inside one input its colour
    // attachments in attachment order and then its depth. The image layouts are the ones the producers leave
    // behind (a colour attachment ends sampleable, and a depth a shader may sample ends sampleable too - see
    // OffscreenTarget), so the descriptors name the layouts the producers' passes really left.
    ::vsg::Descriptors descriptors;
    descriptors.reserve(texture_count + depth_count);
    std::uint32_t binding = 0;
    for (const InputImages& input : inputs)
    {
        for (const ::vsg::ref_ptr<::vsg::ImageView>& view : input.colors)
        {
            if (view == nullptr)
            {
                reportRefused("the pass' sampled inputs", "an entry offers no image view");
                return {};
            }
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(sampler, view,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            ++binding;
        }
        if (input.depth != nullptr)
        {
            // A DEPTH texture gets the NEAREST sampler (see ContentPipeline::depthSampler): the shader compares
            // exact depths, so an interpolated one would be a depth nobody rasterised.
            const auto depth_sampler = layer.depthSampler();
            if (depth_sampler == nullptr)
            {
                reportRefused("the pass' sampled inputs", "the depth sampler could not be created");
                return {};
            }
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(depth_sampler, input.depth,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            ++binding;
        }
    }

    auto set = ::vsg::DescriptorSet::create(set_layout, descriptors);
    if (set == nullptr)
    {
        reportRefused("the pass' sampled inputs", "the sampled-input set could not be created");
        return {};
    }
    // The pipeline layout this command names is the one built for the same pair of counts, and the set index
    // is the ABI's: 1 after the block set for a content half, 0 for a full-screen one (see ContentPipeline).
    return ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            layer.layoutFor(texture_count, depth_count), first_set, set);
}

bool ContentPass::recordScreenDraw(const core::CompiledDraw& draw, const Scope::Entry& entry,
                                   const core::CompiledPass& pass,
                                   const core::RenderPassCompatibility& compatibility,
                                   const ::vsg::ref_ptr<::vsg::BindDescriptorSet>& samples, ::vsg::Group& into)
{
    ContentDraw::ScreenDraw full_screen;
    full_screen.key.kind                  = core::DrawKind::Screen;
    full_screen.key.program               = draw.program.program;
    full_screen.key.revision              = draw.program.revision;
    full_screen.key.compatibility         = compatibility;
    full_screen.key.sampled_color_count   = sampledColorCount(pass);
    full_screen.key.sampled_depth_count   = sampledDepthCount(pass);
    full_screen.dynamic                   = draw.dynamic;
    full_screen.samplers                  = samples;
    // The push the SDK's full-screen programs declare: the lights the call announced, packed into the 128-byte
    // range (see LightPushBlock). An empty list leaves the ambient fill, so a full-screen pass with no lights still
    // shades its albedo instead of rendering black. The count of represented lights is deliberately NOT reported
    // here: the drop report is the content path's (the reference behaviour), and a full-screen call packs what fits
    // and says nothing.
    vine::vsg::LightPushBlock push_block;
    (void)packLightPushBlock(draw.lights, draw.camera, push_block);
    ::vsg::ref_ptr<::vsg::ubyteArray> push_bytes =
        ::vsg::ubyteArray::create(static_cast<std::uint32_t>(sizeof(push_block)));
    std::memcpy(push_bytes->data(), &push_block, sizeof(push_block));
    full_screen.push = ::vsg::PushConstants::create(VK_SHADER_STAGE_FRAGMENT_BIT, 0U, push_bytes);
    full_screen.viewport = ViewportRect{ static_cast<float>(draw.viewport.x), static_cast<float>(draw.viewport.y),
                                         static_cast<float>(draw.viewport.width),
                                         static_cast<float>(draw.viewport.height) };
    full_screen.color_attachments = pass.color_attachments;

    const auto group = entry.draws->recordScreen(*scope_.registry, full_screen);
    if (group == nullptr)
    {
        reportRefused("a full-screen drawing call", "its pipeline for this target could not be built");
        return false;
    }
    into.addChild(group);
    return true;
}

bool ContentPass::recordCommand(const core::CompiledCommand& command, const core::CompiledDraw& draw,
                                const core::CompiledPass& pass, const ContentFacts& facts,
                                const core::RenderPassCompatibility& compatibility, std::uint64_t view_offset,
                                std::uint64_t lights_offset, std::uint64_t shadow_offset,
                                const ::vsg::ref_ptr<::vsg::BindDescriptorSet>& inputs,
                                std::uint32_t sampled_color_count, std::uint32_t sampled_depth_count,
                                ::vsg::Group& into)
{
    const FactResult<ProgramFacts> program = findProgram(facts, command.program);
    if (!program.found())
    {
        reportRefused("the command's program", program.miss);
        return false;
    }

    const FactResult<GeometryFacts> geometry = findGeometry(facts, command.geometry, command.geometry_revision);
    if (!geometry.found())
    {
        reportRefused("the command's geometry", geometry.miss);
        return false;
    }

    // The compiled half the pair names: one program's stages against one vertex layout (see the file note).
    // A half carries its own program and revision, so a command that names another program - or the same one at
    // a revision the half was not compiled from - is refused: borrowing a half's stages would draw a picture
    // nobody authored, and the pool would file it under the key the plan named.
    const Scope::Entry* entry         = nullptr;
    bool                program_known = false;
    for (const Scope::Entry& candidate : scope_.entries)
    {
        if (candidate.program != command.program.program || candidate.revision != command.program.revision)
        {
            continue;
        }
        program_known = true;
        if (candidate.layout == geometry.entry->layout)
        {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr)
    {
        // Which of the two did not match matters: the fixes are different ones (compile the program, or the
        // layout), and "not built for" alone would send the reader to the wrong half of the pipeline key.
        if (program_known)
        {
            reportRefused("the command's geometry", "its vertex layout is not one this pass was built for");
        }
        else
        {
            reportRefused("the command", "its program is not one this pass was built for (or not at that revision)");
        }
        return false;
    }

    const FactResult<MaterialFacts> material = findMaterial(facts, command.material);
    if (!material.found())
    {
        reportRefused("the command's material", material.miss);
        return false;
    }

    // The blocks: this draw's identity and data (the view's bytes came in with the pass).
    vine::graphics::VineDrawBlock draw_block;
    packDrawBlock(command, draw_block);
    const BlockStorage::Block block = scope_.storage->writeDraw(bytesOf(draw_block));
    const BlockStorage::MaterialWrite material_write =
        scope_.storage->writeMaterial(material.entry->material, material.entry->revision, material.entry->block);
    if (!block.valid)
    {
        reportRefused("the command's draw block", "the frame's block budget is full");
        return false;
    }

    // The streams: one bind per channel, in the entry's order (which is the binding order).
    std::array<::vsg::ref_ptr<::vsg::BindVertexBuffers>, kMaxChannels> binds;
    std::size_t                                                        bound = 0;
    for (const ChannelFacts& channel : geometry.entry->channels)
    {
        if (bound == binds.size())
        {
            reportRefused("the command's geometry", "it feeds more channels than this layer binds");
            return false;
        }
        const StreamUploads::VertexResult acquired = scope_.uploads->acquireVertex(channel.key, channel.data);
        if (acquired.bind == nullptr)
        {
            reportRefused("the command's geometry", "one of its channels could not be uploaded");
            return false;
        }
        binds[bound] = acquired.bind;
        ++bound;
    }

    const StreamUploads::IndexResult indices =
        scope_.uploads->acquireIndex(geometry.entry->indices.key, geometry.entry->indices.data);
    if (indices.bind == nullptr)
    {
        reportRefused("the command's geometry", "its index stream could not be uploaded");
        return false;
    }

    ContentDraw::Draw record;
    record.key.program            = program.entry->program;
    record.key.revision           = program.entry->revision;
    record.key.vertex_layout      = entry->layout;
    record.key.compatibility      = compatibility;
    record.key.depth_sampleable   = pass.depth_sampleable;
    // How many colour textures this pass binds as samplers: a fact of the plan's input table, carried into the
    // identity so the pipeline is compiled against the sampled shape its pass really binds.
    record.key.sampled_color_count = sampled_color_count;
    // ... and how many DEPTH textures: the same table's other half (see core::CompiledInput), identity for the
    // same reason - the pipeline's sampled set is compiled against how many of each the pass binds.
    record.key.sampled_depth_count = sampled_depth_count;
    record.dynamic                = command.dynamic;
    record.blocks                 = scope_.descriptors->bind(
        entry->pipelines->layoutFor(sampled_color_count, sampled_depth_count),
        BlockDescriptors::Offsets{ view_offset, block.offset, material_write.offset, lights_offset,
                                      shadow_offset });
    record.inputs       = inputs;
    record.vertex_binds = std::span<const ::vsg::ref_ptr<::vsg::BindVertexBuffers>>(binds.data(), bound);
    record.index        = indices.bind;
    record.viewport     = ViewportRect{ static_cast<float>(draw.viewport.x), static_cast<float>(draw.viewport.y),
                                        static_cast<float>(draw.viewport.width),
                                        static_cast<float>(draw.viewport.height) };
    record.index_count       = geometry.entry->index_count;
    record.first_index       = geometry.entry->first_index;
    record.vertex_offset     = geometry.entry->vertex_offset;
    record.color_attachments = pass.color_attachments;

    ::vsg::ref_ptr<::vsg::Node> node = entry->draws->record(*scope_.registry, record);
    if (node == nullptr)
    {
        // The recorder refuses when the identity has no compiled pipeline: a state group without a pipeline bind
        // would draw with whatever was bound last.
        reportRefused("the command", "its pipeline identity has no compiled pipeline");
        return false;
    }
    into.addChild(node);
    return true;
}

void ContentPass::reportRefused(const char* what, FactMiss miss)
{
    const std::string message = std::string(what) + " is not drawn: " + missText(miss);
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped, asString(message));
}

void ContentPass::reportRefused(const char* what, const char* why)
{
    const std::string message = std::string(what) + " is not drawn: " + why;
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped, asString(message));
}

void ContentPass::reportLightsDropped(std::size_t announced, std::size_t represented, bool has_camera)
{
    // The episode rule the SDK states for dropped lights: a drawing call whose lights ALL fit (or which announced
    // none) ends the episode, and a call that lost some is reported once - not once per call, because a scene has
    // one light list and would otherwise fill the log with the same sentence per drawable.
    if (announced == 0U || represented >= announced)
    {
        scope_.lights_dropped.rearm();
        return;
    }
    if (!scope_.lights_dropped.shouldReport())
    {
        return;
    }

    // Three branches, and each says what the pass shades with INSTEAD: "dropped" alone would leave the reader
    // guessing whether the pass went dark, lit by a fill, or lit by a partial list.
    std::string message;
    if (!has_camera)
    {
        message = std::to_string(announced) + " announced light(s) are not lit: the drawing call announced no";
        message += " camera, so there is no view space to light in (the pass draws unlit)";
    }
    else if (represented == 0U)
    {
        message = std::to_string(announced) + " announced light(s) are not lit (disabled, or a kind the light";
        message += " block does not carry); the pass falls back to the block's ambient fill";
    }
    else
    {
        message = std::to_string(announced - represented) + " of " + std::to_string(announced) + " announced";
        message += " light(s) are not lit (disabled, not ambient or directional, or beyond the block's three";
        message += " directional slots)";
    }
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ChannelIgnored, asString(message));
}

V_VSG_NS_END
