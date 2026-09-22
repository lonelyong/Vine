#include <vine/vsg/api/ContentPass.hpp>

#include <array>
#include <cstddef>
#include <cstring>

#include <vsg/core/Array.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/PushConstants.h>

#include <vine/vsg/api/ContentPush.hpp>
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

/// @brief Whether two block shapes declare the same roles at the same bindings, in the same order.
bool sameShape(std::span<const BlockDescriptors::Binding> left,
               std::span<const BlockDescriptors::Binding> right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        if (left[index].binding != right[index].binding || left[index].role != right[index].role)
        {
            return false;
        }
    }
    return true;
}

/** @brief Whether two sampled-binding lists name the same bindings, in the same order. */
bool sameSamplers(std::span<const BlockDescriptors::SampledBinding> left,
                  std::span<const std::uint32_t> right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        if (left[index].binding != right[index])
        {
            return false;
        }
    }
    return true;
}

}  // namespace

ContentPass::ContentPass(const Scope& scope, core::Diagnostics& diagnostics) noexcept
    : scope_(scope)
    , diagnostics_(diagnostics)
{
    half_reported_   = std::vector<core::ReportOnce>(scope.entries.size());
    shadow_reported_ = std::vector<core::ReportOnce>(scope.entries.size());
}

bool ContentPass::serveHalf(const Scope::Entry& entry, std::uint32_t input_count)
{
    half_block_count_ = 0U;

    if (entry.pipelines == nullptr)
    {
        return false;
    }
    const ProgramAbi& abi = entry.pipelines->abi();
    if (abi.bindings.empty())
    {
        return true;   // a full-screen half, or a content program that declares nothing: nothing to fill
    }

    const auto refuse = [&](const char* why) {
        const std::size_t index = static_cast<std::size_t>(&entry - scope_.entries.data());
        if (index < half_reported_.size() && half_reported_[index].shouldReport())
        {
            reportRefused("the pass' content half", why);
        }
        return false;
    };

    // A push range is now FILLED, member by member, by every command that draws through this half (see
    // api/ContentPush): the camera matrices the text names come from the pass' camera and the drawable's
    // model matrix, and the range's offset, size and stages are the declaration's. What is still refused is a
    // declaration this backend cannot fill at all - that is decided where the layout is built
    // (ContentPipeline::create refuses an unfillable member), so nothing is left to check here.

    // The declared bindings have to be exactly what the pass can put there: the images the INPUT set's
    // declarations name come from the pass' inputs (one per binding, in declaration order), and every other
    // set the program declares is a set the CALLER built - its blocks and its sampled images both, because a
    // set the layer compiled a layout for is only bindable when what it carries is what the text declared.
    std::uint32_t input_samplers = 0U;
    for (const AbiBinding& binding : abi.bindings)
    {
        if (binding.kind != AbiDescriptorKind::UniformBlock && binding.set == ContentPipeline::kInputSet)
        {
            if (binding.binding >= input_count)
            {
                return refuse("its program samples an input texture this pass does not declare");
            }
            ++input_samplers;
        }
    }
    if (input_samplers > input_count)
    {
        return refuse("its program samples more inputs than this pass binds");
    }
    for (const std::uint32_t set : entry.pipelines->declaredSets())
    {
        if (half_block_count_ == half_blocks_.size())
        {
            return refuse("its program declares more sets than this pass can bind");
        }
        BlockDescriptors* found = nullptr;
        for (BlockDescriptors* candidate : scope_.block_sets)
        {
            if (candidate != nullptr && candidate->setIndex() == set &&
                sameShape(candidate->shape(), entry.pipelines->blockShape(set)) &&
                sameSamplers(candidate->samplers(), entry.pipelines->samplerBindings(set)))
            {
                found = candidate;
                break;
            }
        }
        if (found == nullptr)
        {
            return refuse("its program declares a set the caller built no matching set for");
        }
        half_blocks_[half_block_count_++] = found;
    }
    return true;
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
        // What the pass cannot fill is refused BEFORE any pipeline is acquired or any set is built (see
        // serveHalf): a program whose blocks live in a set nobody built for it would otherwise be drawn with
        // another set's bytes.
        if (!serveHalf(*content_half, sampled_color_count + sampled_depth_count))
        {
            out = group;
            return false;
        }
        input_set = makeInputSet(pass, inputs, *content_half->pipelines);
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

            // The set this call binds is the PASS' (the source's attachments, the map, the call's block), so
            // it is built per call: a full-screen call has ONE block and it is the call's own (see
            // makeScreenSet). A pass whose call is refused says why - the rest of the pass still draws.
            if (!recordScreenDraw(draw, *half, pass, compatibility, inputs, *group))
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
                                                                  ContentPipeline& layer)
{
    const std::uint32_t texture_count  = sampledColorCount(pass);
    const std::uint32_t depth_count    = sampledDepthCount(pass);
    if (texture_count == 0U && depth_count == 0U)
    {
        return {};  // nothing declared (or nothing produced): there is no sampled set to bind
    }

    // One set per pass, laid out by the layer that compiles the pipelines the content half's draws bind: the
    // set layout object they were compiled against, so the set is exactly the shape they expect. The halves
    // of one kind build their sampled layouts from the same recipe, so they are compatible with one another
    // as well (a pass may switch halves with this set bound).
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
    // is the content ABI's: 1, after the declared block sets (see ContentPipeline).
    return ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            layer.layoutFor(texture_count, depth_count),
                                            ContentPipeline::kInputSet, set);
}

bool ContentPass::recordScreenDraw(const core::CompiledDraw& draw, const Scope::Entry& entry,
                                   const core::CompiledPass& pass,
                                   const core::RenderPassCompatibility& compatibility,
                                   std::span<const InputImages> inputs, ::vsg::Group& into)
{
    ContentPipeline* const layer = entry.pipelines;
    if (layer == nullptr)
    {
        reportRefused("a full-screen drawing call", "no compiled full-screen half was built for the pass' program");
        return false;
    }

    // WHICH OF THE PASS' INPUTS THIS CALL READS. The full-screen ABI reads ONE input - the call's source - plus
    // the shadow map (by name, wherever the text declares it). The source is named by IDENTITY (the plan's
    // `draw.source`), not by position: the pass' other inputs are its own business, and a call whose source is
    // not among them has nothing to draw from.
    std::size_t source_index = inputs.size();
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        if (index < pass.inputs.size() && pass.inputs[index].target == draw.source)
        {
            source_index = index;
            break;
        }
    }
    const bool         has_source = source_index < inputs.size();
    const InputImages* source     = has_source ? &inputs[source_index] : nullptr;

    // The map: the input the plan RESOLVED (by the light its target states), never "the first one with a
    // depth" - a G-buffer has a depth too, and binding that one as the sun's map is the measured defect the
    // plan's own resolution exists to avoid (see api/ContentImages).
    const std::size_t map_index = vine::vsg::shadowInputIndexOf(pass, inputs);

    // A text that declares a sampler OTHER than the map's reads the SOURCE's attachments, so a call that names
    // no source among the pass' inputs has nothing to fill them from - and every input that is neither the
    // source nor the map is one the ABI has no binding for at all. Both are refused by name: a binding filled
    // with somebody else's picture (or left unread) is a picture nobody asked for.
    const bool declares_source_textures = [&] {
        for (const AbiBinding& binding : layer->abi().bindings)
        {
            if (binding.kind != AbiDescriptorKind::UniformBlock &&
                vine::vsg::imageOriginOf(binding.name) != ImageOrigin::Shadow)
            {
                return true;
            }
        }
        return false;
    }();
    if (declares_source_textures && !has_source)
    {
        reportRefused("a full-screen drawing call",
                      "its program samples the source's textures and the call named no source among the pass' "
                      "inputs");
        return false;
    }
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        if (index == source_index)
        {
            continue;
        }
        if (index != map_index)
        {
            reportRefused("a full-screen drawing call",
                          "one of the pass' inputs is neither the call's source nor the shadow map, and the "
                          "full-screen ABI has no binding for its textures");
            return false;
        }
        if (!inputs[index].colors.empty())
        {
            reportRefused("a full-screen drawing call",
                          "the shadow map's input offers colour textures the full-screen ABI has no binding "
                          "for (the map is its depth)");
            return false;
        }
    }

    // The text's own declaration of the map (the engine's shadow ABI names it) and of its block. A text that
    // declares the map is a text that shades a shadow - the engine picks its shadowed lighting variant only
    // when a shadow exists - so a pass without a readable map REFUSES the call instead of shading it through a
    // stand-in: the picture would be one nobody asked for, and the reason (the map's depth is not sampleable,
    // or its producer published no matrix) is exactly what the refusal has to say.
    std::uint32_t map_binding = 0U;
    const bool    declares_map = vine::vsg::shadowBindingOf(layer->abi(), 0U, map_binding);
    vine::vsg::SamplerImage map_image;
    if (declares_map && !vine::vsg::shadowImageOf(pass, inputs, layer->depthSampler(), map_image))
    {
        reportRefused("a full-screen drawing call",
                      "its program declares `shadow_map` and this pass resolved no readable map (the depth is "
                      "not sampleable, or the producer published no matrix)");
        return false;
    }
    // ... and the other direction, once per half: the pass resolved a map and the program cannot read it.
    reportShadowNotSampled(entry, pass);

    // The block is the CALL's (its lights and camera are), so it is written per call - for every call, switch
    // on or off: the same text serves both, and an unbound block a shader reads is undefined behaviour rather
    // than "no shadow".
    const std::span<const BlockDescriptors::Binding> shape = layer->blockShape(0U);
    vine::graphics::VineShadowBlock                  shadow_block;
    std::uint64_t                                    shadow_offset = 0U;
    if (!shape.empty())
    {
        const bool                shadow_on = packShadowBlock(pass.shadow, draw, shadow_block);
        const BlockStorage::Block written   = scope_.storage->writeShadows(bytesOf(shadow_block));
        (void)shadow_on;
        if (!written.valid)
        {
            reportRefused("the drawing call's shadow block", "the frame's block budget is full");
            return false;
        }
        shadow_offset = written.offset;
    }

    // The set: the source's colour attachments, the source's own depth, the map (at the binding its text
    // names) and the call's block - laid out by the layer, filled here. A depth the text declares is the
    // SOURCE's depth; the map never takes a depth slot (it has a binding of its own), which is what keeps the
    // engine's hard-coded 5/6 right whether or not the source's depth is sampleable.
    //
    // A text that declares NOTHING and a call with no source need no set at all: the draw's whole interface is
    // its push block (the engine's own screen programs always name a source - theirs shade its picture).
    const std::uint32_t colours       = source != nullptr ? static_cast<std::uint32_t>(source->colors.size()) : 0U;
    const std::uint32_t depths        = source != nullptr && source->depth != nullptr ? 1U : 0U;
    const bool          needs_set     = !layer->abi().bindings.empty() || colours != 0U || depths != 0U;
    const auto          set_layout    = needs_set ? layer->sampledSetLayout(colours, depths) : nullptr;
    const auto          sampler       = layer->inputSampler();
    const auto          depth_sampler = layer->depthSampler();
    if (needs_set &&
        (set_layout == nullptr || sampler == nullptr || depth_sampler == nullptr || scope_.storage->buffer() == nullptr))
    {
        reportRefused("a full-screen drawing call", "its sampled set could not be built");
        return false;
    }
    ::vsg::Descriptors descriptors;
    bool               source_depth_used = false;
    const std::span<const VkDescriptorSetLayoutBinding> declared_bindings =
        set_layout != nullptr ? std::span<const VkDescriptorSetLayoutBinding>(set_layout->bindings)
                              : std::span<const VkDescriptorSetLayoutBinding>{};
    descriptors.reserve(declared_bindings.size());
    for (const VkDescriptorSetLayoutBinding& declared : declared_bindings)
    {
        if (declared.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        {
            auto buffer_info = ::vsg::BufferInfo::create(scope_.storage->buffer(), shadow_offset,
                                                         static_cast<VkDeviceSize>(sizeof(shadow_block)));
            descriptors.push_back(::vsg::DescriptorBuffer::create(
                ::vsg::BufferInfoList{ buffer_info }, declared.binding, 0U, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
            continue;
        }
        if (declares_map && declared.binding == map_binding)
        {
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(map_image.sampler, map_image.view,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                declared.binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            continue;
        }
        if (source != nullptr && declared.binding < colours)
        {
            const ::vsg::ref_ptr<::vsg::ImageView>& view = source->colors[declared.binding];
            if (view == nullptr)
            {
                reportRefused("a full-screen drawing call", "the source offers no attachment at one of its bindings");
                return false;
            }
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                declared.binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            continue;
        }
        if (!source_depth_used && source != nullptr && source->depth != nullptr)
        {
            source_depth_used = true;
            descriptors.push_back(::vsg::DescriptorImage::create(
                ::vsg::ImageInfoList{ ::vsg::ImageInfo::create(depth_sampler, source->depth,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) },
                declared.binding, 0U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER));
            continue;
        }
        // A binding the layout declares (the text names it) that neither the source nor the map fills: the
        // text samples something this pass does not offer, said by NAME rather than left to read undefined
        // data (a material's map and the environment are the two names this can be).
        reportRefused("a full-screen drawing call", "it samples a texture this pass does not offer at the "
                                                    "binding its text declares");
        return false;
    }

    ::vsg::ref_ptr<::vsg::BindDescriptorSet> samples;
    if (needs_set)
    {
        auto set = ::vsg::DescriptorSet::create(set_layout, descriptors);
        if (set == nullptr)
        {
            reportRefused("a full-screen drawing call", "its sampled set could not be created");
            return false;
        }
        samples = ::vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                   layer->layoutFor(colours, depths), 0U, set);
        if (samples == nullptr)
        {
            reportRefused("a full-screen drawing call", "its sampled set could not be bound");
            return false;
        }
    }

    ContentDraw::ScreenDraw full_screen;
    full_screen.key.kind                  = core::DrawKind::Screen;
    full_screen.key.program               = draw.program.program;
    full_screen.key.revision              = draw.program.revision;
    full_screen.key.compatibility         = compatibility;
    // The identity says how many textures the set the draw binds carries: the source's colours and its own
    // depth (the map and the block are the text's own declarations, so they cannot vary with the pass).
    full_screen.key.sampled_color_count   = colours;
    full_screen.key.sampled_depth_count   = depths;
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

    // The shadow the pass declared reaches a program only through the name its text uses for the map
    // (api/ContentImages): a program that never declares it shades unshadowed, which is a picture the host
    // cannot tell from "the light does not cast" - so it is said once per half rather than left to the eye.
    reportShadowNotSampled(*entry, pass);

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
    // The blocks: one bind per set the PROGRAM declares (its own `layout(set = ..., binding = ...)` qualifiers,
    // see api/ProgramAbi). Which sets those are came from `serveHalf`, which also refused the half when the
    // caller built no set for one of them.
    const ::vsg::ref_ptr<::vsg::PipelineLayout> block_layout =
        entry->pipelines->layoutFor(sampled_color_count, sampled_depth_count);
    const BlockDescriptors::Offsets block_offsets{ view_offset, block.offset, material_write.offset, lights_offset,
                                                   shadow_offset };
    std::array<::vsg::ref_ptr<::vsg::BindDescriptorSet>, kMaxBlockSets> block_binds;
    std::size_t                                                        bound_sets = 0U;
    for (std::size_t index = 0U; index < half_block_count_; ++index)
    {
        block_binds[bound_sets] = half_blocks_[index]->bind(block_layout, block_offsets);
        if (block_binds[bound_sets] == nullptr)
        {
            reportRefused("the command's block set", "one of its dynamic offsets is not one the device accepts");
            return false;
        }
        ++bound_sets;
    }
    record.blocks       = std::span<const ::vsg::ref_ptr<::vsg::BindDescriptorSet>>(block_binds.data(), bound_sets);
    // The pushes: one command per range the PROGRAM declares, filled from this pass' camera and THIS drawable's
    // model matrix (api/ContentPush) - a declared push is re-written for every drawable, because `modelView`
    // carries the drawable's model matrix, and an unwritten one would carry the last drawable's. The range's
    // offset, size and stages are the declaration's, so the words and the pipeline layout cannot disagree.
    std::array<::vsg::ref_ptr<::vsg::PushConstants>, kMaxPushRanges> push_commands;
    std::size_t                                                      push_count = 0U;
    for (const AbiPushRange& range : program.entry->abi.pushes)
    {
        if (push_count == push_commands.size())
        {
            reportRefused("the command's push range", "its program declares more push ranges than this pass writes");
            return false;
        }
        if (!packContentPush(range, draw.camera, command.model, push_bytes_, unhandled_member_))
        {
            // A member the layer accepted but the packer cannot fill cannot happen (ContentPipeline refuses
            // those); it is reported rather than pushed as zeros because the bytes here ARE the shader's input.
            reportRefused("the command's push range", unhandled_member_.empty()
                                                            ? "its program declares a push range with no size"
                                                            : "one of its push members is not one this backend fills");
            return false;
        }
        ::vsg::ref_ptr<::vsg::ubyteArray> bytes =
            ::vsg::ubyteArray::create(static_cast<std::uint32_t>(push_bytes_.size()));
        std::memcpy(bytes->data(), push_bytes_.data(), push_bytes_.size());
        push_commands[push_count] = ::vsg::PushConstants::create(
            static_cast<VkShaderStageFlags>(pushStagesOf(range.stages)), range.offset, bytes);
        if (push_commands[push_count] == nullptr)
        {
            reportRefused("the command's push range", "its bytes could not be prepared");
            return false;
        }
        ++push_count;
    }
    record.pushes       = std::span<const ::vsg::ref_ptr<::vsg::PushConstants>>(push_commands.data(), push_count);
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

void ContentPass::reportShadowNotSampled(const Scope::Entry& entry, const core::CompiledPass& pass)
{
    if (pass.shadow.light == nullptr || entry.pipelines == nullptr)
    {
        return;  // the pass samples no shadow (or the half declares nothing): nothing to say
    }
    if (samplesShadowMap(entry.pipelines->abi()))
    {
        return;  // the text names the map: the caller fills that binding (see api/ContentImages)
    }
    const std::size_t index = static_cast<std::size_t>(&entry - scope_.entries.data());
    if (index >= shadow_reported_.size() || !shadow_reported_[index].shouldReport())
    {
        return;
    }
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::UnsupportedRequest,
                        asString("the pass declared a shadow, but the program shading it declares no `shadow_map` "
                                 "sampler, so the map does not reach its drawables: they are shaded unshadowed"));
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
