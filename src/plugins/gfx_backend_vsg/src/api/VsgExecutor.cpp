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

void VsgExecutor::setWindow(WindowTarget* window) noexcept
{
    window_ = window;
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
    // The wrapped passes of the PREVIOUS frame name its graphs; this frame's intervals must not be
    // attributed to them (see profileEntries).
    profile_entries_.clear();
    recorded_.clear();
    skipped_          = 0;
    window_recorded_  = false;

    for (const core::CompiledPass& pass : frame.passes)
    {
        if (pass.target_index >= frame.targets.size())
        {
            ++skipped_;
            continue;
        }
        const core::CompiledTarget& compiled_target = frame.targets[pass.target_index];

        // The default framebuffer (a null identity) is the window's, everything else is an off-screen target
        // this executor was told about.
        const bool recorded = compiled_target.target == nullptr
                                  ? recordWindow(pass, command_graph, content)
                                  : recordOffscreen(pass, compiled_target, command_graph, content);
        if (!recorded)
        {
            continue;
        }
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
            // A depth-only target (a shadow map) copies its DEPTH back; a colour one copies attachment 0.
            // Adding the null a colour-less target would answer with a colour capture is a vsg Group with a
            // null child, which traverses into a crash the first time the frame is recorded.
            if (auto readback = target->readback())
            {
                command_graph->addChild(readback);
            }
        }
    }

    return skipped_ == 0;
}

void VsgExecutor::setProfiling(bool enabled) noexcept
{
    profiling_ = enabled;
}

bool VsgExecutor::profiling() const noexcept
{
    return profiling_;
}

std::span<const VsgExecutor::ProfileEntry> VsgExecutor::profileEntries() const noexcept
{
    return profile_entries_;
}

const VsgExecutor::ProfileEntry* VsgExecutor::profileOf(const ::vsg::RenderGraph& graph) const noexcept
{
    for (const ProfileEntry& entry : profile_entries_)
    {
        if (entry.graph == &graph)
        {
            return &entry;
        }
    }
    return nullptr;
}

::vsg::ref_ptr<::vsg::Node> VsgExecutor::recordedChild(const ::vsg::ref_ptr<::vsg::RenderGraph>& graph,
                                                       const core::CompiledPass&                  pass,
                                                       bool                                       window)
{
    if (!profiling_ || graph == nullptr)
    {
        return graph;
    }
    // The name is for a human reading a capture; a READER matches the graph (see setProfiling).
    ProfileEntry entry;
    entry.graph    = graph.get();
    entry.pass     = pass.pass;
    entry.schedule = pass.schedule_index;
    entry.window   = window;
    entry.name     = (window ? std::string("window@")
                             : std::string("target") + std::to_string(pass.target_index) + "@")
                     + std::to_string(pass.schedule_index);

    auto wrapper = ::vsg::InstrumentationNode::create(graph);
    wrapper->setName(entry.name);
    profile_entries_.push_back(std::move(entry));
    return wrapper;
}

bool VsgExecutor::recordOffscreen(const core::CompiledPass& pass, const core::CompiledTarget& compiled_target,
                                  const ::vsg::ref_ptr<::vsg::CommandGraph>& command_graph,
                                  std::span<const PassContent> content)
{
    OffscreenTarget* target = resolve(compiled_target);
    if (target == nullptr)
    {
        reportSkipped(compiled_target, "this executor was not told about it");
        return false;
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
        return false;
    }

    // What the pass does to its attachments is the PLAN's decision, and both halves of it are facts the
    // executor cannot derive: whether this is the target's first writer (a fresh image cannot be loaded) and
    // whether a later pass reads the depth this one writes (never cleared). Handing them on is what makes a
    // second pass over one target LOAD 
    // what the first one wrote instead of erasing it - the target builds the variant the plan asks for.
    ::vsg::ref_ptr<::vsg::RenderGraph> graph = target->passGraph(pass.clear, pass.bootstrap, pass.depth_preserved);
    if (graph == nullptr)
    {
        reportSkipped(compiled_target, "the target has no attachments to draw into");
        return false;
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
    // the graphs are added in IS the execution order (see the file note). A measuring session records the
    // pass through its wrapper (see setProfiling); everything else is unchanged.
    command_graph->addChild(recordedChild(graph, pass, /*window*/ false));
    return true;
}

bool VsgExecutor::recordWindow(const core::CompiledPass& pass,
                               const ::vsg::ref_ptr<::vsg::CommandGraph>& command_graph,
                               std::span<const PassContent> content)
{
    if (window_ == nullptr)
    {
        reportWindowSkipped("no window target was registered with this executor");
        return false;
    }

    // The same plan/world agreement an off-screen target gets, asked of the window: the plan resolved the
    // default framebuffer's facts (one colour attachment, no sampleable depth), and the window answers for
    // what the swapchain and its render pass really are.
    if (pass.color_attachments != window_->colorAttachmentCount() ||
        pass.depth_sampleable != window_->depthSampleable())
    {
        reportWindowSkipped("the plan and the window disagree about its shape (colour attachments or depth "
                            "sampleability)");
        return false;
    }

    // ONE graph, ONE clear: the first window pass of the frame brings the graph up to date (its render area
    // follows the window's live extent, and its clear values are that pass' clear policy) and puts the graph
    // into the command graph at the position that pass has in the plan. Later window passes stack on what it
    // left - the swapchain's render pass cannot express a second clear (see WindowTarget).
    if (!window_recorded_)
    {
        window_->prepare(pass.clear);
        // The window graph is wrapped when it is added - the first window pass of the frame - and its
        // interval covers the graph's whole record traversal, i.e. every window pass' content: the window
        // sample is the PRESENT PATH as a whole, not one view of the swapchain (see WindowTarget).
        command_graph->addChild(recordedChild(window_->graph(), pass, /*window*/ true));
        window_recorded_ = true;
    }

    for (const PassContent& packet : content)
    {
        if (packet.pass == pass.pass && packet.content != nullptr)
        {
            window_->addContent(packet.content);
        }
    }
    return true;
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
                                 (target.target == nullptr ? " (the pass targets the default framebuffer)"
                                                           : "")));
}

void VsgExecutor::reportWindowSkipped(const char* why)
{
    ++skipped_;
    diagnostics_.report(vine::graphics::DiagnosticSeverity::Warning,
                        vine::graphics::DiagnosticCategory::ContentSkipped,
                        asString(std::string("a compiled pass is not recorded: ") + why +
                                 " (the pass targets the default framebuffer)"));
}

V_VSG_NS_END
