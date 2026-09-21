#include <vine/vsg/api/VsgExecutor.hpp>

#include <cstddef>
#include <string>

V_VSG_NS_BEGIN

namespace
{

/// @brief Builds a `vine::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

}  // namespace

VsgExecutor::VsgExecutor(core::Diagnostics& diagnostics) noexcept
    : diagnostics_(diagnostics)
{
}

void VsgExecutor::addTarget(const void* identity, OffscreenTarget* target) noexcept
{
    for (Entry& entry : targets_)
    {
        if (entry.identity == identity)
        {
            entry.target = target;
            return;
        }
    }
    targets_.push_back(Entry{ identity, target });
}

void VsgExecutor::clearTargets() noexcept
{
    targets_.clear();
}

bool VsgExecutor::record(const core::CompiledFrame& frame, ::vsg::ref_ptr<::vsg::CommandGraph> command_graph,
                         std::span<const PassContent> content)
{
    recorded_.clear();
    skipped_ = 0;

    for (const core::CompiledPass& pass : frame.passes)
    {
        if (pass.target_index >= frame.targets.size())
        {
            ++skipped_;
            continue;
        }
        const core::CompiledTarget& compiled_target = frame.targets[pass.target_index];

        OffscreenTarget* target = resolve(compiled_target);
        if (target == nullptr)
        {
            reportSkipped(compiled_target, "this executor was not told about it");
            continue;
        }

        // The plan and the resource world must agree about the SHAPE of what the pass draws into: the plan's
        // colour-attachment count and depth sampleability come from the facts it was compiled with, and the
        // target answers for what it really has. A disagreement means the plan describes a different target
        // than the one it resolved to - and a pipeline built against the wrong shape is a picture with no
        // relationship to what the host asked for. It is found HERE, at the one place the two meet.
        if (pass.color_attachments != target->colorAttachmentCount() ||
            pass.depth_sampleable != target->depth().sampleable)
        {
            reportSkipped(compiled_target, "the plan and the target disagree about its shape (colour "
                                           "attachments or depth sampleability)");
            continue;
        }

        ::vsg::ref_ptr<::vsg::RenderGraph> graph = target->passGraph(pass.clear);
        if (graph == nullptr)
        {
            reportSkipped(compiled_target, "the target has no attachments to draw into");
            continue;
        }

        // Whatever the content layer recorded for THIS pass goes inside this pass - after the plan's clear,
        // before the pass ends. The executor does not look at it: which draws it holds, and what they bind,
        // was decided where the content lives.
        for (const PassContent& packet : content)
        {
            if (packet.pass == pass.pass && packet.content != nullptr)
            {
                graph->addChild(packet.content);
            }
        }

        // One pass scope, one render pass instance: what the pass clears comes from the plan, and the order
        // the graphs are added in IS the execution order (see the file note).
        command_graph->addChild(graph);
        recorded_.push_back(pass.pass);
    }

    // Content for a pass the frame does not contain would silently disappear - and the pass it was meant for
    // is missing for a reason the caller has to hear (a cycle was skipped, a target could not be served).
    for (const PassContent& packet : content)
    {
        const bool known = std::any_of(frame.passes.begin(), frame.passes.end(),
                                       [&packet](const core::CompiledPass& pass) noexcept {
                                           return pass.pass == packet.pass;
                                       });
        if (!known && packet.content != nullptr)
        {
            ++skipped_;
            diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                                vine::graphics::DiagnosticCategory::ContentSkipped,
                                asString("recorded content was not placed: the plan has no pass " +
                                         std::to_string(packet.pass) +
                                         " (its target could not be served, or it sits on a dependency "
                                         "cycle that was skipped)"));
        }
    }

    // The copies go after every pass, per target: a probe reads the frame's final picture, and a copy
    // recorded between two passes would read one that is not final.
    for (const core::CompiledTarget& compiled_target : frame.targets)
    {
        OffscreenTarget* target = resolve(compiled_target);
        if (target != nullptr)
        {
            command_graph->addChild(target->capture());
        }
    }

    return skipped_ == 0;
}

std::span<const core::PassId> VsgExecutor::recorded() const noexcept
{
    return recorded_;
}

std::uint64_t VsgExecutor::skipped() const noexcept
{
    return skipped_;
}

OffscreenTarget* VsgExecutor::resolve(const core::CompiledTarget& target) const noexcept
{
    for (const Entry& entry : targets_)
    {
        if (entry.identity == target.target)
        {
            return entry.target;
        }
    }
    return nullptr;
}

void VsgExecutor::reportSkipped(const core::CompiledTarget& target, const char* why)
{
    ++skipped_;
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped,
                        asString(std::string("a compiled pass is not recorded: ") + why +
                                 (target.target == nullptr ? " (the pass targets the default framebuffer, which "
                                                             "this executor does not serve yet)"
                                                           : "")));
}

V_VSG_NS_END
