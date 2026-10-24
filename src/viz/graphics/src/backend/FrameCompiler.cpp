#include <vine/graphics/backend/FrameCompiler.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

VN_GRAPHICSBACKEND_NS_BEGIN

namespace
{

/// @brief Whether the target's attachments are being (re)built this frame, so the first pass in must clear.
bool freshAttachments(const TargetDecision& decision) noexcept
{
    if (decision.action == TargetAction::Rebuild || decision.action == TargetAction::ResizeInPlace)
    {
        return true;
    }
    return decision.action == TargetAction::Repair && decision.reason == RepairReason::Bootstrap;
}

/// @brief Whether nothing may be drawn into the target yet (an extent that is not usable).
bool unservable(const TargetDecision& decision) noexcept
{
    return decision.action == TargetAction::Repair && decision.reason == RepairReason::SizeUnknown;
}

/// @brief Whether a pass writes the target it announced: it drew something, or it announced a clear.
bool writes(const CollectedPass& pass) noexcept
{
    return !pass.draws.empty() || pass.has_clear;
}

/// @brief Copies the shape a target's facts state into the plan's own storage (see CompiledShape).
CompiledShape copiedShape(FrameArena& arena, const TargetShape& shape)
{
    CompiledShape copy;
    copy.color_formats = arena.copy(std::span<const vn::graphics::RenderTarget::ColorFormat>{ shape.color_formats });
    copy.depth_format  = shape.depth_format;
    copy.device_color_formats =
        arena.copy(std::span<const std::uint32_t>{ shape.device_color_formats });
    copy.device_depth_format = shape.device_depth_format;
    copy.samples             = shape.samples;
    copy.subpass             = shape.subpass;
    return copy;
}

}  // namespace

bool statedShapeAgrees(const CompiledShape& stated, const TargetShape& actual) noexcept
{
    // The engine's half is stated by every plan (a depth-only target states an empty color_formats), so it
    // is compared as it stands.
    if (!std::equal(stated.color_formats.begin(), stated.color_formats.end(), actual.color_formats.begin(),
                    actual.color_formats.end()) ||
        stated.depth_format != actual.depth_format || stated.samples != actual.samples ||
        stated.subpass != actual.subpass)
    {
        return false;
    }

    // The device's half is compared only where BOTH sides state it: a shape that never learned the device's
    // spellings says nothing about them, and "not known" must not read as "absent" (see
    // RenderPassCompatibility). What this leaves unchecked is two unknowns - they may differ, and nothing
    // here could tell.
    if (!stated.device_color_formats.empty() && !actual.device_color_formats.empty() &&
        !std::equal(stated.device_color_formats.begin(), stated.device_color_formats.end(),
                    actual.device_color_formats.begin(), actual.device_color_formats.end()))
    {
        return false;
    }
    return stated.device_depth_format == 0U || actual.device_depth_format == 0U ||
           stated.device_depth_format == actual.device_depth_format;
}

FrameCompiler::FrameCompiler(FrameArena& arena, Diagnostics& diagnostics, Observe& observe) noexcept
    : arena_(arena)
    , diagnostics_(diagnostics)
    , observe_(observe)
{
}

const CompiledFrame& FrameCompiler::compile(const CollectedFrame& description, const FrameFacts& facts)
{
    frame_                  = CompiledFrame{};
    frame_.token            = description.token;
    frame_.default_program  = description.default_program;

    const std::size_t pass_count = description.passes.size();

    // 1. The facts: which passes can be served at all, and the target table they draw into. A target the
    //    backend does not know cannot be drawn into, and saying so is the only honest answer - the
    //    alternative (an empty compatibility, an unbuilt framebuffer) is a silent skip.
    servable_.assign(pass_count, 0);
    target_index_.assign(pass_count, kNoTarget);
    targets_.clear();

    for (std::size_t index = 0; index < pass_count; ++index)
    {
        const CollectedPass& pass  = description.passes[index];
        const TargetFacts*   found = findTarget(facts, pass.target);
        if (found == nullptr)
        {
            report(vn::graphics::DiagnosticCategory::ContentSkipped,
                   "frame " + std::to_string(frame_.token.frame) + ": pass " + std::to_string(pass.pass) +
                       " draws into a render target this backend does not know about: nothing is drawn for it "
                       "this frame");
            continue;
        }

        std::uint32_t slot = kNoTarget;
        for (std::uint32_t candidate = 0; candidate < targets_.size(); ++candidate)
        {
            if (targets_[candidate].compiled.target == pass.target)
            {
                slot = candidate;
                break;
            }
        }
        if (slot == kNoTarget)
        {
            TargetSlot entry;
            entry.compiled.target    = pass.target;
            entry.compiled.decision  = planTarget(found->current, found->wanted);
            entry.compiled.depth     = depthPlan(found->depth);
            entry.compiled.shape     = copiedShape(arena_, found->wanted.shape);
            entry.width              = found->wanted.width;
            entry.height             = found->wanted.height;
            entry.color_attachments  = static_cast<std::uint32_t>(found->wanted.shape.color_formats.size());
            slot                     = static_cast<std::uint32_t>(targets_.size());
            targets_.push_back(entry);
        }

        if (unservable(targets_[slot].compiled.decision))
        {
            // Nothing may be drawn into it yet: not a mistake, and not a report either. It leaves the
            // schedule with the passes that have no target at all.
            continue;
        }
        servable_[index]     = 1;
        target_index_[index] = slot;
    }

    // 2. The passes, in the order the ENGINE announced: the frame value already lists them by dependency
    //    (dependencies first, announced order as the tie-break), and a cyclic component was removed before it
    //    reached this compiler. This stage materializes that order and nothing else - a second schedule here is
    //    a second picture.
    std::size_t servable_count = 0;
    for (const std::uint8_t servable : servable_)
    {
        servable_count += (servable != 0) ? 1U : 0U;
    }
    const std::span<CompiledPass> compiled = arena_.makeArray<CompiledPass>(servable_count);
    bootstrapped_.assign(targets_.size(), 0);

    std::uint32_t schedule_index = 0;
    for (std::size_t node = 0; node < pass_count; ++node)
    {
        if (servable_[node] == 0)
        {
            continue;
        }
        const CollectedPass&  source = description.passes[node];
        const std::uint32_t   slot   = target_index_[node];
        const CompiledTarget& target = targets_[slot].compiled;

        CompiledPass& pass   = compiled[schedule_index];
        pass.pass            = source.pass;
        pass.schedule_index  = schedule_index;
        pass.target_index    = slot;
        pass.state           = source.state;
        pass.depth_range     = source.depth_range;
        pass.clear           = source.clear;
        pass.viewport        = vn::graphics::Viewport{ 0, 0, targets_[slot].width, targets_[slot].height };
        pass.depth_preserved = target.depth.preserve;

        // The other two facts a pipeline's identity needs come from the PASS' side of the plan: how many
        // colour attachments the pass writes into, and whether its target offers a sampleable depth. Both
        // are answered here from the same facts the target table was built from, so a caller building a
        // pipeline key does not have to ask the target registry the same question twice (and cannot get a
        // different answer).
        pass.color_attachments = targets_[slot].color_attachments;
        pass.depth_sampleable  = target.depth.sampleable;

        // The bootstrap pass is the FIRST writer into attachments that were just built or invalidated: an
        // UNDEFINED image cannot be loaded, so that pass must clear (see ClearPlan's rules 1 and 3). Later
        // passes into the same target keep what it wrote.
        if (writes(source) && bootstrapped_[slot] == 0 && freshAttachments(target.decision))
        {
            pass.bootstrap    = true;
            bootstrapped_[slot] = 1;
        }

        pass.draws = resolveDraws(source, pass.viewport, description.default_program);
        pass.inputs = resolveInputs(source, facts, frame_.token);
        pass.shadow = resolveShadow(pass.inputs);
        ++schedule_index;
    }
    frame_.passes = compiled;

    // 5. The target table, published for the executor (identity, what to do, what the depth is).
    const std::span<CompiledTarget> published = arena_.makeArray<CompiledTarget>(targets_.size());
    for (std::size_t index = 0; index < targets_.size(); ++index)
    {
        published[index] = targets_[index].compiled;
    }
    frame_.targets = published;

    return frame_;
}

std::span<const CompiledInput> FrameCompiler::resolveInputs(const CollectedPass& pass, const FrameFacts& facts,
                                                            const FrameToken& token)
{
    const std::span<CompiledInput> inputs = arena_.makeArray<CompiledInput>(pass.inputs.size());
    for (std::size_t index = 0; index < pass.inputs.size(); ++index)
    {
        CompiledInput&                   out    = inputs[index];
        const vn::graphics::ImageSource& source = pass.inputs[index].source;
        out.kind                                = source.kind;
        if (source.isImported())
        {
            // An imported texture is sampled like any other input: one colour image, no producer, no depth.
            // Its revision is the content revision the cache is keyed by, so a re-upload invalidates binds.
            out.texture           = source.texture;
            out.texture_revision  = source.revision;
            out.sampler           = source.sampler;
            out.color_attachments = 1;
            continue;
        }
        out.target = source.target;
        if (out.target == nullptr)
        {
            continue;  // nothing produced it this frame: the engine reports that, and there is nothing to bind
        }

        // The count comes from the SAME facts the pass' own target was resolved from - so "how many colour
        // textures this pass samples" is answered once, by the table, and a caller that re-derived it from the
        // target object could not give a different answer.
        const TargetFacts* found = findTarget(facts, out.target);
        if (found == nullptr)
        {
            // The backend does not know a target it is being asked to sample. Binding nothing silently would
            // shade the pass as if it had not declared the input at all, so it is said out loud - the fix is to
            // hand the backend that target's facts.
            report(vn::graphics::DiagnosticCategory::ContentSkipped,
                   "frame " + std::to_string(token.frame) + ": pass " + std::to_string(pass.pass) +
                       " declares a sampled input this backend cannot resolve to a target it knows: nothing is "
                       "bound for it this frame");
            continue;
        }
        const std::uint32_t offered_colors =
            static_cast<std::uint32_t>(found->wanted.shape.color_formats.size());
        if (source.kind == vn::graphics::ImageSource::Kind::ProducedImage)
        {
            // A SINGLE image: the attachment is the identity, so the offer is that one view (or the depth),
            // never the whole target. The count follows the image rather than the target's attachment count -
            // that is what a sampled-input set binds, and what a pipeline's input count is compiled for.
            out.attachment = source.attachment;
            out.depth      = source.depth;
            if (source.depth)
            {
                // The depth half, decided by the same function the target's own plan uses (depthPlan): a depth
                // a pass preserves is not sampleable, and neither is a lender's depth - so a shader never samples
                // an image some other pass is still depth-testing against.
                out.depth_sampleable = depthPlan(found->depth).sampleable;
                // WHAT this input is, when the target says so: a shadow map is a depth input whose owner stated
                // whose shadow it is (see ShadowFacts). The statement is copied, never re-derived by the consumer.
                out.shadow = found->shadow;
            }
            else if (source.attachment < 0 ||
                     static_cast<std::uint32_t>(source.attachment) >= offered_colors)
            {
                // The consumer is the one that knows what the target offers, so it is the one that reports the
                // index. Offering nothing makes the binding layer refuse the pass instead of sampling another
                // attachment, which is the only safe reading of an index this target does not have.
                report(vn::graphics::DiagnosticCategory::ContentSkipped,
                       "frame " + std::to_string(token.frame) + ": pass " + std::to_string(pass.pass) +
                           " declares colour attachment " + std::to_string(source.attachment) +
                           " of a sampled target that offers " + std::to_string(offered_colors) +
                           " colour attachment(s): nothing is bound for it this frame");
            }
            else
            {
                out.color_attachments = 1U;
            }
            continue;
        }

        // A whole-target declaration keeps its coarse meaning: every colour attachment, plus the depth while
        // that one is sampleable (see ImageSource::ProducedTarget).
        out.color_attachments = offered_colors;
        out.depth_sampleable  = depthPlan(found->depth).sampleable;
        out.shadow            = found->shadow;
    }
    return inputs;
}

const CompiledFrame& FrameCompiler::frame() const noexcept
{
    return frame_;
}

const TargetFacts* FrameCompiler::findTarget(const FrameFacts& facts, const void* target) noexcept
{
    for (const TargetFacts& entry : facts.targets)
    {
        if (entry.target == target)
        {
            return &entry;
        }
    }
    return nullptr;
}

ShadowFacts FrameCompiler::resolveShadow(std::span<const CompiledInput> inputs) noexcept
{
    // The FIRST input that IS a map, whose depth is really sampleable and whose producer published how to read
    // it: three facts, and any of them missing leaves the shading's switch off (a map nobody can place, or one
    // whose texture would not be bound, is not a shadow this pass can shade with).
    for (const CompiledInput& input : inputs)
    {
        if (input.shadow.light != nullptr && input.depth_sampleable && input.shadow.has_view_projection)
        {
            return input.shadow;
        }
    }
    return {};
}

std::span<const CompiledDraw> FrameCompiler::resolveDraws(const CollectedPass&            pass,
                                                         const vn::graphics::Viewport& whole,
                                                         const ProgramRef&               default_program)
{
    const std::span<CompiledDraw> draws = arena_.makeArray<CompiledDraw>(pass.draws.size());
    for (std::size_t index = 0; index < pass.draws.size(); ++index)
    {
        const CollectedDraw& source = pass.draws[index];
        CompiledDraw&        draw   = draws[index];

        draw.kind     = source.kind;
        draw.camera   = source.camera;
        draw.viewport = source.has_viewport ? source.viewport : whole;  // "nobody announced one" is the whole target
        draw.lights   = source.lights;
        draw.source   = source.source;
        draw.program  = source.program;
        draw.commands = resolveCommands(source, default_program, pass.state);

        if (source.kind == DrawKind::Screen)
        {
            // A full-screen draw has no commands to carry per-draw state, so the plan resolves its state here: the
            // defaults a screen program does not edit (no culling, fill, triangles, the engine's blend factors) with
            // the depth policy the call's own definition gives it (see CompiledDraw::dynamic - the canonical
            // triangle lies at the reverse-Z far plane, where any depth test rejects it).
            const vn::graphics::RenderState       screen_authored{};
            const vn::graphics::PassRenderState   screen_pass{ .depth = vn::graphics::DepthMode::Disabled };
            draw.dynamic = resolveDynamicState(
                resolveRenderState(screen_authored, screen_pass, vn::graphics::Topology::Triangles).state);
        }
    }
    return draws;
}

std::span<const CompiledCommand> FrameCompiler::resolveCommands(const CollectedDraw& source,
                                                                const ProgramRef&    default_program,
                                                                const vn::graphics::PassRenderState& pass_state)
{
    const std::span<CompiledCommand> commands = arena_.makeArray<CompiledCommand>(source.commands.size());
    for (std::size_t index = 0; index < source.commands.size(); ++index)
    {
        const CollectedCommand& command = source.commands[index];
        CompiledCommand&        out     = commands[index];

        out.geometry          = command.geometry;
        out.geometry_revision = command.geometry_revision;
        out.primitive         = command.primitive;
        out.material          = command.material;
        out.model             = command.model;
        out.opacity           = command.opacity;
        out.transparent       = command.transparent;
        out.instance_buffer   = command.instance_buffer;
        out.instance_revision = command.instance_revision;
        out.first_instance    = command.first_instance;
        out.instance_count    = command.instance_count;
        // The ONE resolution: what the content authored, weighed against what the pass says
        // (StateResolution.hpp).
        vn::graphics::ResolvedRenderState resolved =
            resolveRenderState(command.authored_state, pass_state, command.topology).state;
        // The item whose destination is the draw block rather than the pipeline (a point's size is a
        // vertex-shader output), so it travels beside the opacity instead of into `dynamic`.
        out.point_size = resolved.pointSize;
        // THE ENGINE'S TRANSPARENCY DECISION IS THE COMMAND'S, made when the typed command was read
        // (`RenderCommand::isTransparent`: the material's resolved alpha mode plus the drawable's opacity), and the
        // plan carried it here as one bool. A translucent command blends with the standard pair even though its
        // content authored none (no StateNode has to say "and blend"), and an opaque one that authored none stays
        // unblended - what `ResolvedRenderState::blend` documents. The order and the blend factor read the same
        // answer because this is the one place that derives one from the other.
        if (out.transparent) {
            resolved.blend.enabled = true;
        }
        out.dynamic = resolveDynamicState(resolved);

        // A command that names no program of its own is shaded by the frame's default - resolved here so the
        // executor never has to ask "and what if it is null?" (see the design's D2 table).
        out.program = command.program.program != nullptr ? command.program : default_program;
    }
    return commands;
}

void FrameCompiler::report(vn::graphics::DiagnosticCategory category, const std::string& message)
{
    diagnostics_.report(vn::graphics::DiagnosticSeverity::Error, category, vn::String::fromUtf8(message));
}

VN_GRAPHICSBACKEND_NS_END
