#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/Diagnostics.hpp>
#include <vine/graphics/FrameTable.hpp>
#include <vine/graphics/ImageInput.hpp>
#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/ReportOnce.hpp>
#include <vine/graphics/graphics_global.hpp>

#include "EngineFrame.hpp"
#include "WiringReport.hpp"

VN_GRAPHICS_NS_BEGIN

class ImageRef;
class RenderBackend;
class RenderPass;
class RenderTarget;
class Scene;
class RenderEngine;

namespace detail
{

class FrameResources;
class PassScheduler;
class PublicationStore;

/**
 * @brief THE SESSION SHAPE: the pass table, the plan diff, and the compile that turns declarations into a frame.
 *
 * @details
 * - WHY IT IS NOT IN `RenderEngine` (`G5` / `G12`): the engine is the COMPOSITION ROOT (graphics-architecture §2.8) -
 *   it owns the pump, the surface ledger, the publications and the diagnostics route. The pass table and everything
 *   derived from it (ordering, wiring checks, transient allocation, the compiled frame value) are MECHANISM, and they
 *   lived in the composition root: `RenderEngine.hpp` carried six nested value types and eleven working tables for
 *   them. Here they are one unit whose contract is "what does this session run, in what order, and what is wrong with
 *   it".
 * - WHAT IT OWNS: the registered passes (`Slot`), the compile's reused tables (`PlanRow`, the scheduler, the wiring
 *   checks' scratch), the episode state of every compile-time report, and the transients the declarations asked for.
 *   The declarations themselves are NOT copied: a pass IS its `PassDesc` (see RenderPass), so the table stores nodes
 *   and reads one copy of every fact.
 * - WHAT IT DOES NOT OWN: the frame value it fills (the pump's), the image bookkeeping (`FrameResources`), the
 *   publication channel (`PublicationStore`), the diagnostics route or the device. Those are borrowed for a call, so
 *   the shape has no lifetime of its own beyond the engine's.
 * - ONE COMPILE PER FRAME, ON THE CONTENT HALF'S THREAD: reading the declarations IS the compile (nothing tells the
 *   engine a declaration moved), and every table below is reused so a steady frame allocates nothing.
 *
 * @warning A shape change (applyPlan / removePass) is refused while a frame is being recorded - the ENGINE decides
 *          that (it owns the pump), which is why `applyPlan` here expects the caller to have asked.
 */
class SessionShape {
  public:
    /** @brief One registered pass: the node, where it sits in the frame, and what a frame produced for it.
     *
     * @details
     * - THE DECLARATION IS NOT HERE. It is the pass' own (RenderPass::declaration()), so a descriptor-declared pass and
     *   the node it became cannot drift apart, and the compile reads one copy of every fact instead of two.
     * - What a slot adds is what only the ENGINE knows: the order the pass was registered at and the transient target
     *   the compile resolved for it. A disabled pass is skipped at run time; its declaration stays, because the wiring
     *   checks judge declarations.
     * - WHAT IT DELIBERATELY DOES NOT HOLD IS THE COST (`G13`): a record cost is a fact of one FRAME, measured by both
     *   halves on their own threads, so it lives on the frame value (`detail::PassCost`) and reaches this table only
     *   through `frameTable`'s answer. A session-level cell here would be written by two threads at two moments.
     */
    struct Slot {
        intrusive_ptr<RenderPass> pass;
        int                       order = 0;
        /// The object that registered this pass and takes it back when it dies (null = registered without an owner,
        /// e.g. straight on the engine). Opaque on purpose: the engine never calls into it, only compares it.
        const void* owner{ nullptr };
        /// Monotonic ticket taken when the row was inserted: a take-back that had to wait removes only the rows that
        /// were already here when it was asked for, so an address a later object reuses keeps its own rows.
        std::uint64_t registration{ 0 };
        /// The role that owner gave this pass (empty = anonymous).
        String role;
        /// The transient the compile allocated for this pass (null when it declares none).
        intrusive_ptr<RenderTarget> transient;
    };

  public:
    /**
     * @brief Creates an empty shape (no passes).
     *
     * @param diagnostics  The engine's one diagnostic route (borrowed; every compile report goes through it).
     * @param publications The publication channel the compile binds each scene draw to (borrowed).
     * @param resources    This frame's image tables (borrowed; the transients and the host bindings live there).
     */
    SessionShape(Diagnostics& diagnostics, PublicationStore& publications, FrameResources& resources) noexcept;

    ~SessionShape();

    SessionShape(const SessionShape&)            = delete;
    SessionShape& operator=(const SessionShape&) = delete;

  public:
    /** @brief Gets the registered passes, in engine order. */
    [[nodiscard]] std::span<const Slot> slots() const noexcept;

    /** @brief Gets the number of registered passes. */
    [[nodiscard]] std::size_t passCount() const noexcept;

    /** @brief Finds the registered pass whose name is @p name (the first row with that name). */
    [[nodiscard]] raw_ptr<RenderPass> pass(const String& name) const;

    /** @brief Finds the pass @p owner stated under @p name (null owner: any row, as pass(name)). */
    [[nodiscard]] raw_ptr<RenderPass> passFor(const String& name, const void* owner) const;

    /** @brief Gets the order a registered pass runs at, or nullopt when no such pass is registered. */
    [[nodiscard]] std::optional<int> passOrder(raw_ptr<RenderPass> pass) const;

    /** @brief Answers the session shape this table runs, as a value (the inverse of applyPlan). */
    [[nodiscard]] Plan plan() const;

    /** @brief Answers the rows one contributor stated (the scoped inverse of applyPlan(plan, owner)). */
    [[nodiscard]] Plan planFor(const void* owner) const;

    /** @brief Gets what a frame of this engine consists of: the passes it runs, in order, with what each cost.
     *
     * @details
     * - THE ROWS ARE THE DECLARATIONS AS THEY STAND; THE NUMBERS ARE THE ANSWERED FRAME'S. The two are joined BY PASS
     *   IDENTITY (see detail::PassCost), so a pass registered, removed or re-ordered after that frame answers 0 instead
     *   of inheriting a neighbour's cost - a positional join would hand it one.
     *
     * @param answered The last answered frame's per-pass cost (borrowed for the call; empty before the first answer).
     * @return The table, in announcement order; empty when nothing is registered.
     */
    [[nodiscard]] FrameTable frameTable(std::span<const PassCost> answered) const;

    /**
     * @brief Applies @p plan as this session's shape, diffing it by pass name.
     *
     * @details
     * - A name the shape does not have is added, a name it has is updated IN PLACE (the node - and every backend
     *   state keyed by it - survives), and a whole-session plan also removes every name it no longer states. Every
     *   field a PassDesc states is adopted as the pass' new declaration.
     * - A pass whose KIND changed cannot become the other object: that name is remove + added.
     * - THE WHOLE PLAN IS CHECKED FIRST and a plan with ANY problem is applied entirely or not at all (see
     *   validatePlan).
     * - The caller has already asked whether a shape change is allowed (the engine owns that rule, because the pump
     *   knows whether a frame is recording).
     *
     * @param plan   The shape to apply.
     * @param owner  The contributor the rows belong to (null: the whole session, which may remove).
     * @param device Device to tell about released backend state / an owned target (may be null).
     * @return What the call did (applied, invalid with every problem).
     */
    [[nodiscard]] PlanResult applyPlan(const Plan& plan, const void* owner, RenderBackend* device);

    /** @brief Rebinds which scene a registered pass renders (the declaration's one storage). */
    bool bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

    /** @brief Gets the content scene a registered pass renders, or null. */
    [[nodiscard]] raw_ptr<Scene> contentOf(raw_ptr<RenderPass> pass) const;

    /**
     * @brief Removes every pass whose slot is owned by @p owner (each one as removePass).
     *
     * @details
     * - Ownership is ONE SLOT TABLE with an owner pointer per row (see PassChain): a row is removed by its own name
     *   inside that owner's scope, and taking a whole owner back is one walk.
     * - THE CALLER HOLDS THE INTERVAL: this mutates the table a compile walks, so when it may run is the ENGINE's
     *   decision (`RenderEngine::removePassesOwnedBy`); a take-back that meets a frame is accepted through
     *   `deferTakeBack` instead of being refused (see `graphics-threading.md` 2.19).
     *
     * @param owner  Owner whose passes are taken back (null is a no-op: unowned passes belong to whoever added them).
     * @param device Device to tell about the state the removed passes retained (may be null).
     */
    void removePassesOwnedBy(const void* owner, RenderBackend* device) noexcept;

    /**
     * @brief Accepts a take-back the frame interval would not allow now, to be applied at the next frame head.
     *
     * @details
     * - A TAKE-BACK IS NEVER REFUSED (see `graphics-threading.md` 2.19): its caller is a destructor (`~PassChain`,
     *   `~SceneView`) and has no later call to retry with, so a refusal would mean silently dropping the rows - and
     *   the backend release each removal performs with them.
     * - THE REQUEST NAMES ITS ROWS BY THE TICKET OF THIS INSTANT. The owner address alone is not an identity across
     *   time (a host may destroy a view and get the same address back for the next one), so the record keeps the
     *   current ticket and `applyDeferredTakeBacks` removes only the rows registered before it.
     * - Called with the engine's phase gate held - the same gate every registration takes - so the ticket it reads
     *   cannot move under it, and no row can slip between the refusal and this record.
     *
     * @param owner The scope whose rows are taken back (null is a no-op, as removePassesOwnedBy).
     */
    void deferTakeBack(const void* owner) noexcept;

    /**
     * @brief Applies every take-back `deferTakeBack` accepted, then forgets them.
     *
     * @details
     * - CALLED WHERE NOTHING RECORDS: the head of a compile (the engine holds the interval and the previous frame is
     *   answered) and `RenderEngine::shutdown` after the pump stopped. Each removal runs the same walk an immediate
     *   take-back uses, so the backend release is not skipped by the deferral.
     * - A ROW ANOTHER CONTRIBUTOR REGISTERED AFTER THE ASK IS LEFT ALONE (see deferTakeBack): the ticket decides, not
     *   the address.
     *
     * @param device Device to tell about the state the removed passes retained (may be null).
     */
    void applyDeferredTakeBacks(RenderBackend* device);

    /** @brief Gets the first enabled pass that names a render target, or null (the engine's capability check). */
    [[nodiscard]] raw_ptr<const RenderPass> firstEnabledPassWithATarget() const noexcept;

    /**
     * @brief Compiles this frame's declarations into @p frame (the ONE place a frame is decided).
     *
     * @details
     * - The compile reads the declarations AS THEY STAND: which pass runs when (dependencies first, announced order as
     *   the tie-break), what each declared input resolves to, which transient targets exist, and what is wrong with
     *   the set. It ends with the frame being a complete value (see EngineFrame) and with this compile's problems
     *   reported once per episode.
     * - The tables are reused, so a steady frame allocates nothing beyond what the compile's own bookkeeping already
     *   has capacity for.
     *
     * @param frame      The frame value to fill (borrowed for the call; the pump owns the storage).
     * @param device     Device to tell about transients it should release (may be null).
     */
    void compile(EngineFrame& frame, RenderBackend* device);

  private:
    /** @brief One take-back that met a recording frame: whose rows, and the ticket that was current when it asked. */
    struct DeferredTakeBack {
        const void*   owner{ nullptr };
        std::uint64_t registration{ 0 };
    };

    /** @brief One row of the compiled frame: what a pass will do, decided before the frame runs. */
    struct PlanRow {
        raw_ptr<RenderPass> pass{ nullptr };
        raw_ptr<Scene>      content{ nullptr };
        int                 order{ 0 };
        /// The declared inputs, resolved for this frame: one entry per declaration, in declaration order,
        /// Kind::None where nothing produced it (exactly what the pass and the backend are handed).
        std::vector<ImageSource> inputs;
    };

    /** @brief One problem a compile found, as the diagnostic a report would carry (its message is its identity). */
    struct CompileProblem {
        vn::graphics::RenderDiagnostic diagnostic;
    };

    /** @brief One pass whose rectangle and camera disagreed, and whether that episode was said. */
    struct ShapeMismatchEpisode {
        raw_ptr<const RenderPass> pass = nullptr;
        ReportOnce                reported;
    };

  private:
    /** @brief Compiles the declarations into the table the frame runs from (see compile). */
    void compileFrame();

    /** @brief Orders this frame's rows: dependencies first, announced order as the tie-break. */
    void scheduleRows();

    /** @brief Assembles the frame's value from this compile's rows (see FrameDescription). */
    void buildFrameValue();

    /** @brief Collects the problems this frame's declarations carry (see CompileProblem). */
    void collectPlanProblems();

    /** @brief Phase 1: what each pass fills and claims, and the declaration mistakes a pass states itself. */
    void collectOutputClaims();

    /** @brief Phase 2: every declared input its consumer cannot draw from (nobody fills it, or no depth exists). */
    void collectUnproducedInputs();

    /** @brief Phase 3: a FullscreenPass whose own declaration says it can never draw. */
    void collectFullscreenPassProblems();

    /** @brief Phase 4: a declared image nobody bound to a target (one report per image). */
    void collectUnboundImages();

    /** @brief Collects this frame's declared picture shapes against the cameras that project them. */
    void collectPictureShapeMismatches();

    /** @brief Makes sure the transient target a slot declares exists, at the shape it declares. */
    void resolveTransientTarget(Slot& slot);

    /** @brief Resolves one pass' declared inputs into @p out, for this frame. */
    void resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<ImageSource>& out);

    /** @brief Publishes a pass' output name for the rest of this frame. */
    void publishRowOutput(raw_ptr<RenderPass> pass);

    /** @brief Resolves one declared input image: the target it addresses, or null when nothing produced it. */
    raw_ptr<RenderTarget> resolveDeclaredImage(raw_ptr<RenderPass> pass, const ImageRef& image);

    /** @brief Resolves one declared input target, or null when nothing produced it this frame. */
    raw_ptr<RenderTarget> resolveDeclaredTarget(raw_ptr<RenderPass> pass, raw_ptr<RenderTarget> target);

    /** @brief Reports a declared input nothing produced this frame (once per episode). */
    void reportUnproducedInput(raw_ptr<RenderPass> pass, const OutputIdentity& identity, const String& what);

    /** @brief Whether the structural check already spoke about this declared input. */
    [[nodiscard]] bool unusableInputReported(raw_ptr<const RenderPass> pass, const OutputIdentity& identity) const noexcept;

    /** @brief Reports the problems the compile found, once per episode, then remembers them. */
    void reportPlanProblems();

    /** @brief Records a problem the compile found, once per sentence (see CompileProblem). */
    void noteProblem(const vn::graphics::RenderDiagnostic& diagnostic);

    /** @brief Inserts a slot in the pass list, keeping it ascending by order. */
    intrusive_ptr<RenderPass> insertSlot(Slot slot);

    /** @brief Turns a declaration into its execution node, adjudicating the depth edge first. */
    [[nodiscard]] intrusive_ptr<RenderPass> makePassNode(const PassDesc& desc);

    /** @brief Applies a plan that has already been validated (update, add, remove, re-order). */
    void applyPlanRows(const Plan& plan, const void* owner, RenderBackend* device);

    /** @brief Drops one registered pass and releases what the backend retained for it. */
    void removePass(raw_ptr<RenderPass> pass, RenderBackend* device);

    /** @brief Removes every row @p owner registered before ticket @p before (the one walk both take-back forms use). */
    void removeOwnedRows(const void* owner, std::uint64_t before, RenderBackend* device) noexcept;

  private:
    Diagnostics&       diagnostics_;
    PublicationStore&  publications_;
    FrameResources&    resources_;

    // THE COMPILE'S TWO BORROWED FACTS, set for the length of one compile() call: the frame value being filled and
    // the device transients are released through. Neither outlives the call.
    EngineFrame*   frame_{ nullptr };
    RenderBackend* device_{ nullptr };

    std::vector<Slot> slots_; // uniform ordered pass registry

    /// The ticket the next inserted row takes. Monotonic, and moved only under the engine's phase gate (a
    /// registration is a shape change), which is why a take-back may read it there.
    std::uint64_t next_registration_{ 1 };

    /// Take-backs that met a recording frame, in the order they were asked (see deferTakeBack).
    std::vector<DeferredTakeBack> deferred_take_backs_;
    /// The queue's own leaf lock: an asker reaches it from a destructor on any thread, and the drain runs inside a
    /// frame where the phase gate is not this thread's to take. Nothing is called while it is held beyond the vector.
    std::mutex deferred_mutex_;

    /// The wiring checks' tables and their lookups (see WiringReport), cleared every compile.
    WiringReport wiring_;

    /// This frame's compiled rows, in the order they run (reused; one per slot).
    std::vector<PlanRow> plan_rows_;
    /// The execution order of plan_rows_ (indices), decided by scheduleRows(); also what buildFrameValue walks.
    std::vector<std::uint32_t> frame_order_;
    /// The announced sort key of each plan row, rebuilt by scheduleRows() (the scheduler reads a contiguous span).
    std::vector<int> frame_order_keys_;
    /// The scheduler's working memory (see PassScheduler): one per engine, reused every frame.
    std::unique_ptr<PassScheduler> scheduler_;
    /// This frame's problems and the previous frame's, swapped after each compile: the episode rule.
    std::vector<CompileProblem> plan_problems_;
    std::vector<CompileProblem> previous_problems_;
    /// Whether a compile is running: while it is, the checks' reports are COLLECTED rather than emitted.
    bool collecting_problems_ = false;
    /// The declared inputs the structural check spoke about in THIS compile (see unusableInputReported).
    std::set<std::pair<raw_ptr<const RenderPass>, OutputIdentity>> unusable_inputs_this_frame_;
    /// The passes whose picture shape was reported, and this compile's rebuild of that list (the episode rule).
    std::vector<ShapeMismatchEpisode> shape_mismatches_this_frame_;
    std::vector<ShapeMismatchEpisode> shape_mismatches_last_frame_;
    /// "A pass names a layered depth view" is one condition, not a per-pass event (see PassDesc::depth_view).
    ReportOnce layered_depth_reported_{};
    /// The depth-view conflicts already said, keyed by (target, the image it was refused).
    std::set<std::pair<raw_ptr<RenderTarget>, raw_ptr<RenderTarget>>> depth_view_conflicts_{};
};

} // namespace detail

VN_GRAPHICS_NS_END
