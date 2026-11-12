#include <vine/vsg/internal/DrawNeeds.hpp>

VN_VSG_NS_BEGIN

namespace
{

/**
 * @brief Whether a program reads any sampler from THIS layer's input set.
 *
 * WHY IT MATTERS, AND WHAT IT COST TO MISS IT: the pass' declared inputs are what this backend binds at set 1
 * (see ContentPipeline::kInputSet) FOR THE PROGRAMS THAT READ THEM THERE, while the engine's own programs
 * keep every map outside that set - the material block and the diffuse map at set 0, the per-drawable block
 * at set 1 - and are served by name instead (see api/ContentImages: a texture slot from the material,
 * `vn_shadow_map` from the pass' resolved images). Compiling such a program against an input set is a LAYOUT
 * question rather than a size one: its per-drawable block lives in set 1, and a sampled set in that slot is
 * exactly the mismatch ContentPipeline::acquire's guard refuses - measured: every command of the engine's
 * forward path was refused ("its pipeline identity has no compiled pipeline"), 42 lines per frame, with the
 * whole scene invisible while the passes, the plan and the pixels that were left all looked fine.
 *
 * @param abi The program's declarations.
 * @return true when the text declares at least one sampler in the input set.
 */
bool programSamplesInputSet(const ProgramAbi& abi) noexcept
{
    for (const AbiBinding& binding : abi.bindings) {
        if (binding.kind != AbiDescriptorKind::UniformBlock && binding.set == ContentPipeline::kInputSet) {
            return true;
        }
    }
    return false;
}

} // namespace

std::uint32_t sampledColorCount(const core::CompiledPass& pass) noexcept
{
    std::uint32_t total = 0;
    for (const core::CompiledInput& input : pass.inputs) {
        total += input.color_attachments;
    }
    return total;
}

std::uint32_t sampledDepthCount(const core::CompiledPass& pass) noexcept
{
    std::uint32_t total = 0;
    for (const core::CompiledInput& input : pass.inputs) {
        total += input.depth_sampleable ? 1U : 0U;
    }
    return total;
}

core::PipelineKey DrawNeeds::keyFor(const core::RenderPassCompatibility& compatibility) const
{
    core::PipelineKey key;
    key.kind                = kind;
    key.program             = program;
    key.vertex_layout       = layout;
    key.compatibility       = compatibility;
    key.depth_sampleable    = depth_sampleable;
    key.sampled_color_count = key_colors;
    key.sampled_depth_count = key_depths;
    key.variant             = variant.bits();
    key.topology            = topology;
    return key;
}

DrawNeeds drawNeeds(const core::CompiledPass& pass, const core::CompiledCommand& command, const ContentFacts& facts)
{
    DrawNeeds out;
    out.kind              = core::DrawKind::Content;
    out.program           = command.program;
    out.topology          = command.dynamic.topology;
    out.color_attachments = pass.color_attachments;
    out.depth_sampleable  = pass.depth_sampleable;
    out.set_colors        = sampledColorCount(pass);
    out.set_depths        = sampledDepthCount(pass);
    // The instance stream is a property of the COMMAND (the visible prefix travels in the draw), so the variant
    // knows about it before the program entry is asked for - the engine's own rule, see api/ProgramVariant.
    out.instanced         = command.instance_buffer != nullptr;

    const FactResult<GeometryFacts> geometry = findGeometry(facts, command.geometry);
    if (!geometry.found()) {
        out.missed = DrawNeeds::Miss::Geometry;
        out.miss   = geometry.miss;
        return out;
    }
    out.geometry_entry = geometry.entry;

    const FactResult<MaterialFacts> material = findMaterial(facts, command.material);
    if (!material.found()) {
        out.missed = DrawNeeds::Miss::Material;
        out.miss   = material.miss;
        return out;
    }
    out.material_entry = material.entry;

    out.variant           = variantOf(*material.entry, *geometry.entry);
    out.variant.instanced = out.instanced;

    const FactResult<ProgramFacts> program = findProgram(facts, command.program, out.variant, core::DrawKind::Content);
    if (!program.found()) {
        out.missed = DrawNeeds::Miss::Program;
        out.miss   = program.miss;
        return out;
    }

    out.program_entry = program.entry;
    // The ROW's spelling of the identity, not the plan's: the lookup proved them equal, and the half and the
    // pipeline must be keyed by one of them rather than by whichever a caller happened to hold.
    out.program       = program.entry->program;
    out.layout        = geometry.entry->layout;
    out.reads_inputs  = programSamplesInputSet(program.entry->abi);
    // A program that does not sample the input set has a pipeline compiled WITHOUT its layout (see
    // programSamplesInputSet): the key states no counts for it, while the pass' set is still bound by whoever
    // reads it - two facts, and the pair above is the one rule that relates them.
    out.key_colors    = out.reads_inputs ? out.set_colors : 0U;
    out.key_depths    = out.reads_inputs ? out.set_depths : 0U;
    return out;
}

DrawNeeds drawNeeds(const core::CompiledPass& pass, const core::CompiledDraw& draw, std::span<const InputImages> inputs)
{
    DrawNeeds out;
    out.kind              = core::DrawKind::Screen;
    out.program           = draw.program;
    out.topology          = draw.dynamic.topology;
    out.color_attachments = pass.color_attachments;
    // A full-screen key states no `depth_sampleable`: the full-screen ABI's set is its TEXT's, and what it
    // samples is already described by the counts below (the key's other half is the content ABI's switch).
    out.depth_sampleable  = false;

    // WHICH OF THE PASS' INPUTS THIS CALL READS: named by IDENTITY (the plan's `draw.source`), never by position -
    // the pass' other inputs are its own business, and a call whose source is not among them has nothing to draw
    // from. An offer of no inputs therefore answers a call with no source, which is the shape a producer that
    // only asks for the half's key needs (see the header).
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (index < pass.inputs.size() && pass.inputs[index].target == draw.source) {
            out.source_index = index;
            break;
        }
    }

    if (out.hasSource()) {
        const InputImages& source                                      = inputs[out.source_index];
        // THE FULL-SCREEN ABI'S SOURCE IS THE WHOLE TARGET: binding N reads the source's attachment N, so a call
        // whose pass declared the source as ONE image still binds - and is compiled for - the whole target.
        // `colors` is the fallback for a ProducedTarget input, where the two lists are the same.
        const std::span<const ::vsg::ref_ptr<::vsg::ImageView>> colors = source.screen_colors.empty() ? source.colors : source.screen_colors;
        out.set_colors                                                 = static_cast<std::uint32_t>(colors.size());
        out.set_depths                                                 = source.depth != nullptr ? 1U : 0U;
    }

    // The pipeline is compiled against exactly what its set binds (there is no "reads the input set" question for
    // this ABI: the set IS the call's interface), so the two pairs agree by construction.
    out.key_colors = out.set_colors;
    out.key_depths = out.set_depths;
    return out;
}

VN_VSG_NS_END
