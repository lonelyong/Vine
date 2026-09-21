#include <vine/vsg/core/FrameCompiler.hpp>

#include <cstddef>
#include <string>

V_VSG_NS_BEGIN

namespace core
{
namespace
{

/// @brief Builds a `vine::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

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

}  // namespace

FrameCompiler::FrameCompiler(FrameArena& arena, Diagnostics& diagnostics, Observe& observe) noexcept
    : arena_(arena)
    , diagnostics_(diagnostics)
    , observe_(observe)
{
}

const CompiledFrame& FrameCompiler::compile(const FrameDescription& description, const FrameFacts& facts)
{
    frame_                  = CompiledFrame{};
    frame_.token            = description.token;
    frame_.default_program  = description.default_program;

    const std::size_t pass_count = description.passes.size();
    graph_.reset(description.passes);

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
            graph_.exclude(index);  // no target, no pass: it neither runs nor orders anything
            report(vine::graphics::DiagnosticCategory::ContentSkipped,
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
            graph_.exclude(index);
            continue;
        }
        servable_[index]     = 1;
        target_index_[index] = slot;
    }

    // 2. The dependency edges. A sampled input is produced by a pass that writes the target it names - and
    //    a pass that samples the target it draws into is the feedback pattern, not a cycle, so an edge from
    //    a pass to itself is never added here.
    for (std::uint32_t consumer = 0; consumer < pass_count; ++consumer)
    {
        if (servable_[consumer] == 0)
        {
            continue;
        }
        for (const InputRef& input : description.passes[consumer].inputs)
        {
            if (input.target == nullptr)
            {
                continue;  // nothing produced it this frame (the engine resolved it to null)
            }
            for (std::uint32_t producer = 0; producer < pass_count; ++producer)
            {
                if (producer == consumer || servable_[producer] == 0 ||
                    description.passes[producer].target != input.target || !writes(description.passes[producer]))
                {
                    continue;
                }
                graph_.addEdge(producer, consumer);
            }
        }

        // A borrowed depth is another target's attachment: the pass that owns it must have written before the
        // borrower reads it (the second kind of edge the schedule exists for).
        const CompiledTarget& compiled_target = targets_[target_index_[consumer]].compiled;
        if (!compiled_target.depth.borrowed || compiled_target.depth.source == nullptr)
        {
            continue;
        }
        for (std::uint32_t producer = 0; producer < pass_count; ++producer)
        {
            if (producer == consumer || servable_[producer] == 0 ||
                description.passes[producer].target != compiled_target.depth.source ||
                !writes(description.passes[producer]))
            {
                continue;
            }
            graph_.addEdge(producer, consumer);
        }
    }

    // 3. The order, and the cycles that have none.
    const FrameSchedule& schedule = graph_.schedule();
    frame_.cycles = static_cast<std::uint32_t>(schedule.cycleCount());
    observe_.counters().invalid_schedules += frame_.cycles;
    for (const std::vector<std::uint32_t>& component : schedule.cycles)
    {
        std::string members;
        for (const std::uint32_t member : component)
        {
            if (!members.empty())
            {
                members += ", ";
            }
            members += std::to_string(description.passes[member].pass);
        }
        report(vine::graphics::DiagnosticCategory::ContentSkipped,
               "frame " + std::to_string(frame_.token.frame) + ": passes " + members +
                   " declare a dependency cycle (each reads what the other writes), which no execution order "
                   "can satisfy: those passes are skipped this frame, and the rest of the frame still runs");
    }

    // 4. The passes, in execution order, with every default made explicit.
    const std::span<CompiledPass> compiled = arena_.makeArray<CompiledPass>(schedule.order.size());
    bootstrapped_.assign(targets_.size(), 0);

    std::uint32_t schedule_index = 0;
    for (const std::uint32_t node : schedule.order)
    {
        if (servable_[node] == 0)
        {
            continue;  // unreachable while the loop above excludes every unservable pass (see there)
        }
        const CollectedPass&  source = description.passes[node];
        const std::uint32_t   slot   = target_index_[node];
        const CompiledTarget& target = targets_[slot].compiled;

        CompiledPass& pass   = compiled[schedule_index];
        pass.pass            = source.pass;
        pass.schedule_index  = schedule_index;
        pass.target_index    = slot;
        pass.depth           = source.depth;
        pass.clear           = source.clear;
        pass.viewport        = vine::graphics::Viewport{ 0, 0, targets_[slot].width, targets_[slot].height };
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

const CompiledFrame& FrameCompiler::frame() const noexcept
{
    return frame_;
}

const FrameGraph& FrameCompiler::graph() const noexcept
{
    return graph_;
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

std::span<const CompiledDraw> FrameCompiler::resolveDraws(const CollectedPass&            pass,
                                                         const vine::graphics::Viewport& whole,
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
        draw.commands = resolveCommands(source, default_program, pass.depth);
    }
    return draws;
}

std::span<const CompiledCommand> FrameCompiler::resolveCommands(const CollectedDraw& source,
                                                                const ProgramRef&    default_program,
                                                                vine::graphics::DepthMode pass_depth)
{
    const std::span<CompiledCommand> commands = arena_.makeArray<CompiledCommand>(source.commands.size());
    for (std::size_t index = 0; index < source.commands.size(); ++index)
    {
        const CollectedCommand& command = source.commands[index];
        CompiledCommand&        out     = commands[index];

        out.geometry          = command.geometry;
        out.geometry_revision = command.geometry_revision;
        out.material          = command.material;
        out.model             = command.model;
        out.opacity           = command.opacity;
        out.dynamic           = resolveDynamicState(command.state, command.depth_explicit, pass_depth);

        // A command that names no program of its own is shaded by the frame's default - resolved here so the
        // executor never has to ask "and what if it is null?" (see the design's D2 table).
        out.program = command.program.program != nullptr ? command.program : default_program;
    }
    return commands;
}

void FrameCompiler::report(vine::graphics::DiagnosticCategory category, const std::string& message)
{
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Error, category, asString(message));
}

}  // namespace core

V_VSG_NS_END
