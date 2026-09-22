#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/Viewer.h>
#include <vsg/nodes/InstrumentationNode.h>

#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/Session.hpp>
#include <vine/vsg/api/WindowTarget.hpp>
#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameTimeline.hpp>
#include <vine/vsg/core/RetirementQueue.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame's EXECUTION stage: it walks the compiled plan and records it, deciding nothing.
 *
 * WHAT "MECHANICAL" MEANS HERE, and why it is the whole point of the three-stage split: the executor never
 * asks whether a target should be rebuilt, which pipeline a draw uses, what order the passes run in, or what
 * anything costs. The plan already answered all of that (see FrameCompiler), and the executor's only freedom
 * is WHICH API OBJECT it happens to walk - which is exactly what makes the plan worth compiling in the first
 * place.
 *
 * THE RECORD ORDER IS THE PLAN'S ORDER, and that is a claim a picture can check: the passes of a
 * CompiledFrame arrive in execution order, so a frame whose passes were announced in one order and scheduled
 * in another is recorded the other way round. If this type walked the host's call order instead, the last
 * pass recorded would be the last one ANNOUNCED - a different clear on screen, which is what
 * tests/test_vsg/ExecutorTest.cpp asserts.
 *
 * ONE PASS SCOPE IS ONE RENDER PASS. A compiled pass becomes one render graph over its target's attachments,
 * carrying that pass' clear values; the target owns the render pass, the framebuffer and the images. Two
 * passes into the same target are two graphs, in the plan's order - "a pass is a render pass instance" is
 * not a coincidence of this backend, it is what the load-op variant design assumes.
 *
 * WHAT IT DOES NOT SERVE YET (and says so instead of drawing somewhere else): nothing - every pass of a
 * plan is recorded, either into an off-screen target this executor was told about or into the window it was
 * told about. A pass whose target it does not know is reported rather than dropped in silence.
 *
 * THE WINDOW IS ONE PASS' WORTH OF STATE ACROSS PASSES. The window's graph is created once (by the
 * session, see WindowTarget) and every window pass adds its content to it in the plan's order, because the
 * swapchain's render pass has one clear and one begin/end: the first window pass in execution order owns
 * that clear, and later ones stack on top. The graph is added to the command graph where its first pass
 * sits in the plan, so the order the host sees on screen is the plan's order like everywhere else.
 *
 * The CONTENT of a pass arrives already recorded (see PassContent): the layer that owns the content world -
 * pipelines, streams, blocks, materials - records it, because that is where those objects live, and the
 * executor places it inside the pass the plan asked for. What the executor decides about content is that it
 * goes into the RIGHT pass, and that content for a pass the frame does not contain is reported instead of
 * vanishing.
 */
V_VSG_NS_BEGIN

/**
 * @brief The content of one pass, as the layer that owns the content world recorded it.
 *
 * The seam, stated as data: the executor does not know how a draw becomes GPU work - it knows that whatever
 * the content layer recorded for this pass belongs INSIDE this pass (after the plan's clear, before the pass
 * ends). Building it is the content layer's job, and it builds it from the same plan (`CompiledPass::draws`,
 * whose viewports and dynamic state were already resolved), which is what keeps the two halves from drifting.
 */
struct PassContent
{
    core::PassId                 pass{0};   ///< Which pass of the plan this content belongs to.
    ::vsg::ref_ptr<::vsg::Node>  content;   ///< The nodes to draw inside that pass' render graph.
};

/**
 * @brief The execution stage (see the file note for what it decides, which is nothing).
 */
class V_VSG_API VsgExecutor
{
  public:
    /** @brief Creates an executor reporting what it cannot serve through @p diagnostics.
     *
     * @param diagnostics The backend's one diagnostic route.
     */
    explicit VsgExecutor(core::Diagnostics& diagnostics) noexcept;

    VsgExecutor(const VsgExecutor&)            = delete;
    VsgExecutor& operator=(const VsgExecutor&) = delete;

    /** @brief Registers the window that passes targeting the default framebuffer are recorded into.
     *
     * Borrowed: the session owns the window (and the target wrapping it) and must keep it alive while it is
     * registered - the same rule as every other borrowed argument in this backend. One window per executor,
     * because a frame has one swapchain to present.
     *
     * @param window The window target, or nullptr to unregister it (default-framebuffer passes are then
     *               reported as unserved instead of being recorded somewhere else).
     */
    void setWindow(WindowTarget* window) noexcept;

    /** @brief Registers the target that a compiled pass' identity resolves to.
     *
     * Borrowed: the caller owns the target and must keep it alive while it is registered (the same rule as
     * every other borrowed argument in this backend).
     *
     * @param identity The identity the plan's CompiledTarget carries.
     * @param target   The target to draw into, or nullptr to unregister.
     */
    void addTarget(const void* identity, OffscreenTarget* target) noexcept;

    /** @brief Forgets every registered target. */
    void clearTargets() noexcept;

    /** @brief Records @p frame into @p command_graph, in the plan's order.
     *
     * The copy-back nodes of every registered target are appended LAST, after every pass: a probe reads what
     * the LAST pass into that target left, and a copy recorded between two passes would read a picture that
     * is not the frame's.
     *
     * @param frame         The compiled frame (its passes are already in execution order).
     * @param command_graph Graph the pass graphs are appended to.
     * @param content       Already-recorded content, one entry per pass that has any (may be empty).
     * @return true when every pass was recorded; false when at least one could not be (it was reported).
     */
    bool record(const core::CompiledFrame& frame, ::vsg::ref_ptr<::vsg::CommandGraph> command_graph,
                std::span<const PassContent> content = {});

    /** @brief Gets the passes it recorded, in the order it recorded them. */
    [[nodiscard]] std::span<const core::PassId> recorded() const noexcept;

    /** @brief What applying a plan's target answers did (see @ref applyTargetPlans). */
    struct TargetApplications
    {
        std::uint64_t resized{0};  ///< Targets whose extent was replaced (ResizeInPlace).
        std::uint64_t rebuilt{0};  ///< Targets whose SHAPE was replaced (Rebuild).
        std::uint64_t refused{0};  ///< A depth lease blocked it: nothing was replaced.
        std::uint64_t failed{0};   ///< The description could not be built: the target keeps what it had.
    };

    /**
     * @brief Applies the plan's target answers (resize / rebuild) to the targets this executor holds.
     *
     * WHY THIS IS THE EXECUTOR'S STEP, and not the host's: the plan already answered what has to happen to
     * every target this frame draws into (see core::planTarget), the targets are the ones THIS executor
     * records into, and the caller that owns the frame has no way to tell a "the extent moved" from a "the
     * pass graph must be rebuilt" except by reading the plan it just compiled. Here the answers become real
     * in one call: `compile`, @ref applyTargetPlans, @ref record, @ref submit is the whole frame drive, and
     * a target the plan asked to rebuild is not drawn into with the pipelines of its old shape.
     *
     * WHAT IT WALKS, AND WHAT IT LEAVES ALONE. One application per target of @p frame (the plan's own list,
     * so a target this frame does not touch is not touched here either), matched to its entry in @p facts -
     * which are the same facts the plan was compiled from, because the wanted extent and shape live THERE
     * (the compiled plan carries the answer, not the description). A target this executor was not told
     * about is skipped: @ref record reports the pass that needed it. The default framebuffer is skipped too
     * (its extent belongs to the surface, not to this table). The modes that need nothing - None, and the
     * repair arms, which the recording answers with a bootstrap clear - do not appear in the counts, so a
     * caller can gate on "exactly one replacement happened".
     *
     * @param frame      The compiled plan whose targets are applied.
     * @param facts      The target facts the plan was compiled from (the wanted descriptions live here).
     * @param timeline   The frame timeline a replaced set is parked against (the caller's own clock).
     * @param retirement Where replaced objects go (the caller owns it, like its device waits).
     * @return What was applied, refused and could not be built, per mode.
     */
    TargetApplications applyTargetPlans(const core::CompiledFrame& frame, std::span<const core::TargetFacts> facts,
                                        const core::FrameTimeline& timeline, core::RetirementQueue& retirement);

    /**
     * @brief Records that @p frame's submission did not happen: what its passes wrote is not there.
     *
     * A submission that fails - or a frame the host dropped after recording it - leaves the attachments of
     * every OFF-SCREEN target this frame's passes wrote in a state nobody can trust: the writes were
     * recorded, not performed. Marking them is what turns "contents unknown" back into a state the plan can
     * answer (see OffscreenTarget::invalidateAttachments): the next compiled plan says "repair" for them,
     * and the first BOOTSTRAPPING pass into each clears that - exactly once, because only a pass that clears
     * can make contents known again.
     *
     * Only the targets @p frame's passes actually name: a registered target the frame did not write is left
     * alone (nothing was recorded into it), and the default framebuffer has no attachments of ours to
     * distrust.
     *
     * @param frame The frame whose submission was lost (the same compiled frame that was recorded).
     * @return How many targets were marked.
     */
    std::size_t noteLostSubmission(const core::CompiledFrame& frame) noexcept;

    /**
     * @brief Submits @p frame's recorded graphs through @p viewer, and turns a failed submission into the
     * repair evidence itself (see @ref noteLostSubmission).
     *
     * WHY THE STEP AND ITS FAILURE ARE ONE CALL, and it is the whole reason this method exists: a host
     * that wrote `viewer.recordAndSubmit()` itself would have to know that a failure thrown out of that
     * call means "the writes this frame recorded were never performed" - and the moment it does not know,
     * the next frame draws over contents nobody can vouch for. The host calls ONE method, the failure is
     * never something it has to interpret, and the targets the frame wrote are marked before it ever
     * gets the answer back.
     *
     * WHAT COUNTS AS "FAILED": anything the step throws. vsg's own vocabulary for a submission it cannot
     * make is an exception (`vsg::Exception`, thrown from the command buffer's allocation and nowhere
     * told to the caller of `Viewer::recordAndSubmit`, which returns void) - and an exception means the
     * step did NOT complete, so whether the frame's writes happened is unknown, which is exactly what
     * the mark says. Presenting is not this call: that half stays with whoever owns the window.
     *
     * THE FRAME IS NOT RETRIED HERE. Whether to try again is the host's (or the session's) decision, and
     * the mark is what makes the retry correct either way: the next COMPILED plan answers "repair" for
     * the marked targets, so a frame that never made it is rebuilt from a clear instead of drawn over.
     *
     * @param frame  The frame whose graphs were recorded (the same plan @ref record was given).
     * @param viewer The viewer those graphs are assigned to.
     * @return true when the submission happened; false when it did not (it was reported, and the frame's
     *         written targets were marked).
     */
    bool submit(const core::CompiledFrame& frame, ::vsg::Viewer& viewer);

    /**
     * @brief Commits @p frame through @p session (submit + present), and turns a lost submission into the
     * repair evidence (see @ref noteLostSubmission).
     *
     * THE WINDOW PATH'S TWIN OF @ref submit, and the reason it is a separate call: the window's submission
     * goes through the SESSION (it owns the swapchain, the present and the frame protocol), so the step that
     * knows whether the submission happened is `Session::commitFrame()` - which reports its own failure and
     * answers false (see its declaration). What this executor adds is the half the session cannot know:
     * WHICH of the frame's targets are now holding contents nobody can vouch for. One call, and the caller
     * never has to interpret either answer.
     *
     * A FALSE ANSWER MARKS, INCLUDING "THERE WAS NO OPEN FRAME": both mean this frame's writes were never
     * handed to the queue, which is exactly the fact the mark states. The session is the reporter on this
     * path (its `SubmissionFailed` carries the reason), so this call adds no diagnostic of its own - unlike
     * @ref submit, where the executor is the only layer that saw the failure.
     *
     * @param frame   The frame whose graphs were recorded and assigned to the session.
     * @param session The session that submits and presents them.
     * @return true when the frame was submitted and presented; false when it was not (the frame's written
     *         targets were marked, and the session reported why).
     */
    bool commit(const core::CompiledFrame& frame, api::Session& session);

    /** @brief Sets whether this frame's passes are recorded through measurement wrappers.
     *
     * WHAT THE WRAPPER IS FOR, and why the pass graph cannot be attributed without it: upstream's own
     * per-graph timestamp is written with a NULL object, so a capture (or a profiler reading vsg's log)
     * gets an interval it cannot attach to anything. Recording the pass THROUGH a `vsg::InstrumentationNode`
     * makes the interval carry the GRAPH as its object - and the graph is what this executor already knows
     * the pass of (see @ref profileOf), so attribution needs no second table.
     *
     * THE NAME IS FOR A HUMAN READING A CAPTURE, not for the reader: `"<target>@<schedule>"`, where the
     * target is `"window"` or `"target<i>"` (the index the plan gave it). A reader that matched on the name
     * would break the moment two passes shared one, which is why the legacy backend's own note says the
     * same thing about its names.
     *
     * OFF BY DEFAULT, and off means nothing at all: no wrapper node, no name, no entry - a frame recorded
     * without measurement is byte-for-byte the graph it was before this existed.
     *
     * @param enabled Whether the passes of the frames recorded from now on are wrapped.
     */
    void setProfiling(bool enabled) noexcept;

    /** @brief Gets whether pass measurement is on (see @ref setProfiling). */
    [[nodiscard]] bool profiling() const noexcept;

    /** @brief One recorded pass, as the measurement wrapper's attribution key. */
    struct ProfileEntry
    {
        const ::vsg::RenderGraph* graph{nullptr};  ///< The pass graph the measurement interval carries.
        core::PassId              pass{0};         ///< The pass it belongs to.
        std::uint32_t             schedule{0};     ///< Position in the frame's execution order.
        bool                      window{false};   ///< True for the window graph (the present path).
        std::string               name;            ///< The wrapper's name (for a capture, not a reader).
    };

    /** @brief Gets the passes of the last recorded frame, in the order they were wrapped.
     *
     * Empty when measurement is off. Rebuilt per frame: an entry names a graph of THIS frame, and keeping
     * the previous frame's would attribute this frame's intervals to the last one's passes.
     */
    [[nodiscard]] std::span<const ProfileEntry> profileEntries() const noexcept;

    /** @brief Gets the entry of @p graph, or null when it was not a wrapped pass of the last frame.
     *
     * This is the attribution a reader does: the measurement interval carries a graph, and this answers
     * which pass that graph was.
     *
     * @param graph The graph an interval's object points at.
     * @return The entry, or null.
     */
    [[nodiscard]] const ProfileEntry* profileOf(const ::vsg::RenderGraph& graph) const noexcept;

    /** @brief Gets how many passes this frame it could not record. */
    [[nodiscard]] std::uint64_t skipped() const noexcept;


  private:
    /** @brief One registered target: the identity the plan uses, and the target itself. */
    struct Entry
    {
        const void*      identity{nullptr};  ///< What CompiledTarget::target carries.
        OffscreenTarget* target{nullptr};    ///< The target, borrowed.
    };

    /** @brief Records one pass into an off-screen target; false when it could not be served. */
    [[nodiscard]] bool recordOffscreen(const core::CompiledPass& pass, const core::CompiledTarget& compiled_target,
                                       const ::vsg::ref_ptr<::vsg::CommandGraph>& command_graph,
                                       std::span<const PassContent> content);

    /** @brief Records one pass into the window's graph (see the file note for the one-clear rule).
     *
     * @param pass          The compiled pass targeting the default framebuffer.
     * @param command_graph Graph the window's graph is added to, at its first window pass' position.
     * @param content       Already-recorded content, one entry per pass that has any.
     * @return true when the pass was recorded.
     */
    [[nodiscard]] bool recordWindow(const core::CompiledPass& pass,
                                    const ::vsg::ref_ptr<::vsg::CommandGraph>& command_graph,
                                    std::span<const PassContent> content);

    /** @brief Resolves one compiled target to a registered target, or nullptr. */
    [[nodiscard]] OffscreenTarget* resolve(const core::CompiledTarget& target) const noexcept;

    /** @brief The node to append for @p graph: the graph itself, or a named wrapper when measuring.
     *
     * A wrapped pass also records its attribution entry (the graph and the pass it is), which is what a
     * reader of the measurement has to match on.
     *
     * @param graph  The pass' render graph.
     * @param pass   The compiled pass it belongs to.
     * @param window Whether this is the window graph (the present path as a whole).
     * @return What to append to the command graph.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::Node> recordedChild(const ::vsg::ref_ptr<::vsg::RenderGraph>& graph,
                                                            const core::CompiledPass&                  pass,
                                                            bool window);

    /** @brief Reports a pass it could not record. */
    void reportSkipped(const core::CompiledTarget& target, const char* why);

    /** @brief Reports a window pass it could not record (it has no compiled-target entry to name). */
    void reportWindowSkipped(const char* why);


  private:
    core::Diagnostics&        diagnostics_;  ///< The one diagnostic route.
    std::vector<Entry>        targets_;      ///< Registered targets, borrowed.
    WindowTarget*             window_{nullptr};       ///< The registered window (borrowed).
    bool                      profiling_{false};  ///< Wrap each pass for measurement (see setProfiling).
    std::vector<ProfileEntry> profile_entries_;  ///< The wrapped passes of the last recorded frame.
    bool                      window_recorded_{false};  ///< Whether this frame's window graph is in the graph.
    std::vector<core::PassId> recorded_;     ///< Passes recorded, in record order.
    std::uint64_t             skipped_{0};   ///< Passes not recorded this frame.
};

V_VSG_NS_END
