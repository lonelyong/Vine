#include <vine/vsg/api/VsgExecutor.hpp>

#include <cstddef>
#include <exception>
#include <string>

#include <vsg/core/Exception.h>

VN_VSG_NS_BEGIN

namespace
{

/// @brief Builds a `vn::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vn::String asString(const std::string& text)
{
    return vn::String(reinterpret_cast<const char8_t*>(text.c_str()));
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

void VsgExecutor::addTarget(const void* identity, OffscreenTarget* target, const char* label) noexcept
{
    for (Entry& entry : targets_)
    {
        if (entry.identity == identity)
        {
            entry.target = target;
            entry.label  = label;
            return;
        }
    }
    targets_.push_back(Entry{ identity, target, label });
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
                                  ? recordWindow(pass, compiled_target, command_graph, content)
                                  : recordOffscreen(pass, compiled_target, command_graph, content);
        if (!recorded)
        {
            continue;
        }
        recorded_.push_back(pass.pass);
    }

    // A frame that drew ONLY off-screen still PRESENTS, and the acquired image must have had a render pass
    // run over it before the present: the window's graph is the only place that happens, so it is added here
    // when no window pass did it (measured: presenting an image nobody rendered into is
    // VUID-VkPresentInfoKHR-pImageIndices-01430 - it stays in UNDEFINED). Its content view still holds the
    // last window pass' content, which is the same picture an empty frame presents (see api/Session): a frame
    // with no window pass does not blank the window, it just gives the fresh swapchain image a defined
    // layout. There is no pass to attribute a measurement interval to, so this graph is added UNWRAPPED.
    if (window_ != nullptr && !window_recorded_)
    {
        window_->prepareWithoutClear();
        command_graph->addChild(window_->graph());
        window_recorded_ = true;
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
            diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::ContentSkipped,
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
    //
    // The FORMATS are part of it (see CompiledShape): a shape with the same count and a different format is
    // a different render pass, and the engine's vocabulary cannot tell two of them apart - which is the
    // drift this half of the check exists for.
    if (pass.color_attachments != target->colorAttachmentCount() ||
        pass.depth_sampleable != target->depth().sampleable ||
        !core::statedShapeAgrees(compiled_target.shape, target->shape()))
    {
        reportSkipped(compiled_target, "the plan and the target disagree about its shape (colour attachments, "
                                       "formats, samples or depth)");
        return false;
    }

    // Name the images for the validation layer on the first frame that CAN (see OffscreenTarget::nameImages):
    // a create-info has no Vulkan handle until the first compile allocates it, so this is retried until it
    // sticks and costs one flag check per pass afterwards. The plan's own identity is the name - it is what
    // the host calls this target and what a validation message should say.
    if (compiled_target.target != nullptr) {
        const Entry* entry = entryOf(compiled_target.target);
        if (entry != nullptr && entry->label != nullptr) {
            (void)target->nameImages(entry->label);
        }
    }

    // What the pass does to its attachments is the PLAN's decision, and both halves of it are facts the
    // executor cannot derive: whether this is the target's first writer (a fresh image cannot be loaded) and
    // whether a later pass reads the depth this one writes (never cleared). Handing them on is what makes a
    // second pass over one target LOAD 
    // what the first one wrote instead of erasing it - the target builds the variant the plan asks for.
    ::vsg::ref_ptr<::vsg::RenderGraph> graph = target->passGraph(pass.clear, pass.bootstrap);
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

bool VsgExecutor::recordWindow(const core::CompiledPass& pass, const core::CompiledTarget& compiled_target,
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
    // what the swapchain and its render pass really are - formats and samples included, because a window
    // whose facts state the DEVICE's spelling has to state the swapchain's own (see WindowTarget::facts).
    if (pass.color_attachments != window_->colorAttachmentCount() ||
        pass.depth_sampleable != window_->depthSampleable() ||
        !core::statedShapeAgrees(compiled_target.shape, window_->shape()))
    {
        reportWindowSkipped("the plan and the window disagree about its shape (colour attachments, formats, "
                            "samples or depth)");
        return false;
    }

    // ONE graph, ONE clear: the first window pass of the frame brings the graph up to date (its render area
    // follows the window's live extent, and its clear values are that pass' clear policy) and puts the graph
    // into the command graph at the position that pass has in the plan. Later window passes stack on what it
    // left - the swapchain's render pass cannot express a second clear (see WindowTarget).
    if (!window_recorded_)
    {
        window_->prepare(pass.clear);
        // The window's content is THE FRAME'S: what the last recorded frame put in the retained view is
        // dropped before this frame's lands (a second frame would otherwise draw its predecessor's
        // picture again and never stop growing - see WindowTarget::beginFrame).
        window_->beginFrame();
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
            window_->addFrameContent(packet.content);
        }
    }
    return true;
}

std::size_t VsgExecutor::noteLostSubmission(const core::CompiledFrame& frame) noexcept
{
    std::size_t marked = 0U;
    for (const core::CompiledPass& pass : frame.passes)
    {
        if (pass.target_index >= frame.targets.size())
        {
            continue;  // the plan names a target it does not carry: the recorder refused it on the way in
        }
        const void* const identity = frame.targets[pass.target_index].target;
        if (identity == nullptr)
        {
            continue;  // the default framebuffer: its contents are the presentation's, not ours
        }
        OffscreenTarget* target = nullptr;
        for (const Entry& entry : targets_)
        {
            if (entry.identity == identity)
            {
                target = entry.target;
                break;
            }
        }
        if (target == nullptr || target->instance().attachments_invalidated)
        {
            continue;  // not registered, or already marked by an earlier pass of this frame
        }
        target->invalidateAttachments();
        ++marked;
    }
    return marked;
}

bool VsgExecutor::submit(const core::CompiledFrame& frame, ::vsg::Viewer& viewer)
{
    // Three catches, because the throwers disagree on what an exception is: vsg::Exception is a plain
    // struct (message + VkResult) that is NOT a std::exception, so without the first branch the failure
    // this method exists for would slip past the second - and with it the mark. Anything else is unusual
    // enough to name as such rather than to let it end the frame drive.
    std::string failure;
    try
    {
        viewer.recordAndSubmit();
        return true;
    }
    catch (const ::vsg::Exception& error)
    {
        failure = error.message;
        if (error.result != 0)
        {
            failure += " (VkResult " + std::to_string(error.result) + ")";
        }
    }
    catch (const std::exception& error)
    {
        failure = error.what();
    }
    catch (...)
    {
        failure = "an exception of an unknown type";
    }

    const std::size_t marked = noteLostSubmission(frame);
    diagnostics_.report(vn::graphics::DiagnosticSeverity::Error,
                        vn::graphics::DiagnosticCategory::SubmissionFailed,
                        asString("the frame's submission failed, so what its passes wrote was never "
                                 "performed: " +
                                 std::to_string(marked) +
                                 " off-screen target(s) marked for repair (the next compiled plan bootstraps "
                                 "them) - the failure was: " +
                                 failure));
    return false;
}

bool VsgExecutor::commit(const core::CompiledFrame& frame, api::Session& session)
{
    if (session.commitFrame())
    {
        return true;
    }

    // The submission did not happen (or there was no open frame to commit, which means the same thing for
    // this frame): the session has reported why, and what it cannot know is which of the frame's targets
    // are now holding contents nobody can vouch for - marking them is what makes the next compiled plan
    // repair them (see noteLostSubmission).
    noteLostSubmission(frame);
    return false;
}

std::span<const core::PassId> VsgExecutor::recorded() const noexcept
{
    return recorded_;
}

VsgExecutor::TargetApplications VsgExecutor::applyTargetPlans(const core::CompiledFrame& frame,
                                                              std::span<const core::TargetFacts> facts,
                                                              const core::FrameTimeline& timeline,
                                                              core::RetirementQueue& retirement)
{
    TargetApplications applied;

    // WHAT ORDER THESE ARE APPLIED IN, and why it is a lease question rather than a bookkeeping one: a
    // borrower's framebuffer names its LENDER's depth image, so replacing the lender's attachments first is
    // what makes the borrower's own build name the image the lender now serves (see OffscreenTarget::resize,
    // which refuses in the other order for the VUID a framebuffer smaller than its own attachment is).
    // Applying a borrower first would refuse its resize every frame and leave both targets at the size they
    // were built at - which is exactly what happened to the engine's deferred pipeline, silently.
    const auto factsOf = [&facts](const void* identity) -> const core::TargetFacts* {
        for (const core::TargetFacts& entry : facts)
        {
            if (entry.target == identity)
            {
                return &entry;
            }
        }
        return nullptr;
    };
    const auto indexOf = [&frame](const void* identity) -> std::size_t {
        for (std::size_t index = 0; index < frame.targets.size(); ++index)
        {
            if (frame.targets[index].target == identity)
            {
                return index;
            }
        }
        return frame.targets.size();
    };
    const auto lenderOf = [&factsOf](const core::CompiledTarget& compiled) -> const void* {
        const core::TargetFacts* fact = factsOf(compiled.target);
        return fact != nullptr && fact->depth.borrowed ? fact->depth.source : nullptr;
    };
    // The borrowers of @p identity, as the facts state them: a framebuffer names its lender's depth image, so
    // these are the targets a replacement of @p identity has to answer to.
    const auto borrowersOf = [&facts](const void* identity) -> std::vector<const void*> {
        std::vector<const void*> borrowers;
        for (const core::TargetFacts& entry : facts)
        {
            if (entry.depth.borrowed && entry.depth.source == identity)
            {
                borrowers.push_back(entry.target);
            }
        }
        return borrowers;
    };
    // Whether @p identity can legally back every borrower it has, if it becomes @p width x @p height: a
    // framebuffer attachment must be at least as large as the framebuffer it is attached to
    // (VUID-VkFramebufferCreateInfo-pAttachments-00861), so a lender may not shrink below a borrower that is
    // not shrinking with it. The target itself cannot answer this (it does not know who its borrowers are,
    // which is what the facts are for), and the refusal is the mirror image of the one OffscreenTarget::resize
    // makes for a borrower that grows past its lender.
    //
    // WHAT THE BORROWER WILL BE, not what it is: a borrower with an application of its own is applied AFTER
    // this lender (see the order above), so a pair that shrinks TOGETHER is legal - and the engine's own
    // deferred chain does exactly that on every window resize. Comparing against the borrower's current extent
    // instead refused the lender's shrink while the borrower was about to follow, which left both targets at
    // the old size and reported a lease problem that was not one (measured on the demo, 2026-09-23).
    const auto coversBorrowers = [&frame, &facts, &indexOf](const void* identity, int width, int height) -> bool {
        for (const core::TargetFacts& entry : facts)
        {
            if (!entry.depth.borrowed || entry.depth.source != identity)
            {
                continue;
            }
            int borrower_width  = entry.current.desc.width;
            int borrower_height = entry.current.desc.height;
            const std::size_t index = indexOf(entry.target);
            if (index < frame.targets.size())
            {
                const core::TargetAction borrower_action = frame.targets[index].decision.action;
                if (borrower_action == core::TargetAction::ResizeInPlace ||
                    borrower_action == core::TargetAction::Rebuild)
                {
                    borrower_width  = entry.wanted.width;
                    borrower_height = entry.wanted.height;
                }
            }
            if (borrower_width > width || borrower_height > height)
            {
                return false;
            }
        }
        return true;
    };

    // The few targets of a frame: a selection sort that takes the first target whose lender either has no
    // application of its own or has already been taken. A lender that is itself a borrower is handled by the
    // same rule, and a cycle (which the SDK's contract does not allow) just stops making progress and leaves
    // the rest in their original order.
    std::vector<std::size_t> order;
    std::vector<bool>        taken(frame.targets.size(), false);
    order.reserve(frame.targets.size());
    for (std::size_t placed = 0; placed < frame.targets.size(); ++placed)
    {
        std::size_t chosen = frame.targets.size();
        for (std::size_t index = 0; index < frame.targets.size() && chosen == frame.targets.size(); ++index)
        {
            if (taken[index])
            {
                continue;
            }
            const std::size_t lender = indexOf(lenderOf(frame.targets[index]));
            if (lender >= frame.targets.size() || taken[lender])
            {
                chosen = index;
            }
        }
        if (chosen == frame.targets.size())
        {
            for (std::size_t index = 0; index < frame.targets.size(); ++index)
            {
                if (!taken[index])
                {
                    chosen = index;
                    break;
                }
            }
        }
        taken[chosen] = true;
        order.push_back(chosen);
    }

    // The targets whose attachments were REPLACED by this call: their borrowers still name the depth image
    // they used to serve, and the framebuffer is what names it. Collected here and re-pointed AFTER the loop
    // -
    // not while the lender is being applied - because a borrower with an application of its own replaces its
    // framebuffer too (building it against whatever the lender serves at that moment), and an application can
    // REFUSE or FAIL: "the plan asked it to resize" is not the same fact as "its framebuffer was rebuilt", and
    // only the second one answers whether a re-point is still needed. Re-pointing is idempotent (it returns
    // false when the borrower already names the lender's current image), so doing it once for every replaced
    // lender is enough.
    std::vector<const void*> replaced_identities;

    for (const std::size_t index : order)
    {
        const core::CompiledTarget& compiled = frame.targets[index];
        const core::TargetAction action = compiled.decision.action;

        if (compiled.target == nullptr)
        {
            // The default framebuffer. Its EXTENT belongs to the surface - the platform follows it and vsg
            // rebuilds the swapchain on its own (see WindowTarget::prepare) - so ResizeInPlace is already
            // true and there is nothing here to replace. Its SHAPE is the swapchain's too, and the shape
            // this target's records were keyed on is the target's own account of it, so a plan that says
            // Rebuild is answered by ASKING THE PLATFORM (WindowTarget::refresh) and never by adopting what
            // the plan claims: a claim the swapchain does not back is counted as NOT APPLIED, and the record
            // step refuses the pass that relied on it (see its shape agreement) - which is what keeps a
            // window rebuild from being silently ignored.
            if (action != core::TargetAction::Rebuild)
            {
                // None and the repair arms are the recording's (a bootstrap clear); ResizeInPlace is the
                // surface's, already true.
                continue;
            }
            if (window_ == nullptr)
            {
                continue;  // not registered here: record() reports the pass that needed it
            }
            if (window_->refresh())
            {
                ++applied.rebuilt;
            }
            else
            {
                // NOT reported here: the record step's shape agreement is what refuses the pass that relied on
                // the claim (see recordWindow), so a report from this arm would say one problem twice - and the
                // claim is often STALE rather than false (the plan was compiled from facts sampled before the
                // platform's answer, e.g. a resize the platform has not applied yet).
                ++applied.failed;
            }
            continue;
        }

        if (action != core::TargetAction::ResizeInPlace && action != core::TargetAction::Rebuild)
        {
            // None, and the repair arms (which the recording answers with a bootstrap clear): nothing to
            // replace, so nothing is counted - a caller gates on "exactly one replacement happened".
            continue;
        }

        OffscreenTarget* target = resolve(compiled);
        if (target == nullptr)
        {
            continue;  // not registered here: record() reports the pass that needed it
        }

        // The wanted description lives in the facts, not in the plan (the plan carries the answer). A target
        // the frame names but the table does not is one this call cannot act on: better nothing than a
        // description nobody gave.
        const core::TargetFacts* wanted = factsOf(compiled.target);
        if (wanted == nullptr)
        {
            continue;
        }

        bool replaced = false;
        if (action == core::TargetAction::ResizeInPlace)
        {
            // A LENDER may only shrink while it still covers its borrowers (see coversBorrowers): the depth
            // image is the borrowers' attachment, and a framebuffer is not allowed to be the larger of the
            // two. Refused here rather than inside the target because the borrowers are known to THIS layer
            // (they are rows of the facts table), and refusing keeps the pair legal and reported instead of
            // legal-looking and invalid.
            if (!coversBorrowers(compiled.target, wanted->wanted.width, wanted->wanted.height))
            {
                ++applied.refused;
                reportUnapplied(compiled,
                                "its new extent would leave a borrower's framebuffer larger than the depth "
                                "image it borrows (a lender may not shrink below a borrower that is not "
                                "shrinking with it)");
                continue;
            }
            const OffscreenTarget::Resized resized =
                target->resize(static_cast<std::uint32_t>(wanted->wanted.width),
                               static_cast<std::uint32_t>(wanted->wanted.height), timeline, retirement);
            replaced = resized.replaced;
            if (resized.replaced)
            {
                ++applied.resized;
            }
            else if (resized.refused)
            {
                ++applied.refused;
            }
            else
            {
                ++applied.failed;
            }
        }
        else
        {
            // Rebuild: the plan changed the SHAPE, and everything the target is NOT changing here is its own -
            // the clear policy it was created with and whether its depth was asked to be sampleable (the plan's
            // description carries neither, see OffscreenTarget::layout).
            OffscreenTarget::TargetLayout layout = target->layout();
            layout.width         = static_cast<std::uint32_t>(wanted->wanted.width);
            layout.height        = static_cast<std::uint32_t>(wanted->wanted.height);
            layout.color_formats = wanted->wanted.shape.color_formats;
            layout.depth_format  = wanted->wanted.shape.depth_format;
            const OffscreenTarget::Rebuilt rebuilt = target->rebuild(layout, timeline, retirement);
            replaced = rebuilt.replaced;
            if (rebuilt.replaced)
            {
                ++applied.rebuilt;
            }
            else if (rebuilt.refused)
            {
                ++applied.refused;
            }
            else
            {
                // The plan said Rebuild and nothing was replaced without a lease refusing: the description could
                // not be built, and the target still serves the shape it had (the pass that needed the new one
                // is reported by record()).
                ++applied.failed;
            }
        }

        if (!replaced)
        {
            // A refused or failed application used to be a counter nobody read, and that is how a leased pair
            // sat at the wrong extent for a whole session: the picture was a crop of its own top-left corner
            // and not one line said why. Reported once per EPISODE AND PER TARGET (the answer is a property of
            // the target's description, not of a frame), and deliberately NOT counted as a skipped pass: see
            // reportUnapplied.
            reportUnapplied(compiled, action == core::TargetAction::ResizeInPlace
                                         ? "its new extent could not be applied (a leased depth needs its lender "
                                           "applied first, see the lease rules in OffscreenTarget)"
                                         : "its new shape could not be applied (a leased depth cannot change shape "
                                           "while the lease is in force)");
            continue;
        }

        replaced_identities.push_back(compiled.target);
        if (Entry* entry = entryOf(compiled.target))
        {
            entry->noteApplied();  // the application took effect: a later failure is a new episode
        }
    }

    // Every borrower of a replaced lender follows it, now that the applications have really happened: a
    // borrower that rebuilt its own framebuffer against the lender's current image is already pointing at it
    // (repointBorrowedDepth says so and does nothing), and one whose own application was refused or failed
    // still names the replaced image - which is the case this pass exists for.
    for (const void* lender : replaced_identities)
    {
        for (const void* borrower : borrowersOf(lender))
        {
            if (OffscreenTarget* borrower_target = resolveIdentity(borrower))
            {
                (void)borrower_target->repointBorrowedDepth(timeline, retirement);
            }
        }
    }
    return applied;
}

std::uint64_t VsgExecutor::skipped() const noexcept
{
    return skipped_;
}

OffscreenTarget* VsgExecutor::resolve(const core::CompiledTarget& target) const noexcept
{
    return resolveIdentity(target.target);
}

OffscreenTarget* VsgExecutor::resolveIdentity(const void* identity) const noexcept
{
    for (const Entry& entry : targets_)
    {
        if (entry.identity == identity)
        {
            return entry.target;
        }
    }
    return nullptr;
}

void VsgExecutor::reportUnapplied(const core::CompiledTarget& target, const char* why)
{
    // NOT a skipped pass, and not counted as one: `skipped()` answers "how many PASSES this frame could not
    // record" (see its declaration), and a target that did not follow its description is a different fact -
    // the passes into it WERE recorded, against the attachments it still serves. The refusal is visible in
    // the returned TargetApplications (refused / failed) and in this diagnostic.
    //
    // The episode is PER TARGET (one ReportOnce per registered entry): two targets that cannot follow their
    // descriptions are two problems, and a single episode would report whichever failed first and swallow the
    // other until it recovered.
    Entry* entry = entryOf(target.target);
    if (entry == nullptr || !entry->unapplied.shouldReport())
    {
        return;  // not registered here: record() reports the passes that needed it
    }
    const std::string which = entry->label != nullptr ? std::string(entry->label) : std::string("a target");
    diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::TargetBuildFailed,
                        asString("the target '" + which + "' did not follow its description: " + why));
}

VsgExecutor::Entry* VsgExecutor::entryOf(const void* identity) noexcept
{
    for (Entry& entry : targets_)
    {
        if (entry.identity == identity)
        {
            return &entry;
        }
    }
    return nullptr;
}

void VsgExecutor::reportSkipped(const core::CompiledTarget& target, const char* why)
{
    ++skipped_;
    diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::ContentSkipped,
                        asString(std::string("a compiled pass is not recorded: ") + why +
                                 (target.target == nullptr ? " (the pass targets the default framebuffer)"
                                                           : "")));
}

void VsgExecutor::reportWindowSkipped(const char* why)
{
    ++skipped_;
    diagnostics_.report(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::ContentSkipped,
                        asString(std::string("a compiled pass is not recorded: ") + why +
                                 " (the pass targets the default framebuffer)"));
}

VN_VSG_NS_END
