#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
#include <vine/graphics/ImageSource.hpp>
#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/ReportOnce.hpp>
#include <vine/graphics/graphics_global.hpp>

#include "EngineFrame.hpp"

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
class FrameScheduler;
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
     * - What a slot adds is what only the ENGINE knows: the order the pass was registered at, the transient target the
     *   compile resolved for it, and how long its last record took. A disabled pass is skipped at run time; its
     *   declaration stays, because the wiring checks judge declarations.
     */
    struct Slot {
        intrusive_ptr<RenderPass> pass;
        int                       order = 0;
        /// The object that registered this pass and takes it back when it dies (null = registered without an owner,
        /// e.g. straight on the engine). Opaque on purpose: the engine never calls into it, only compares it.
        const void* owner{ nullptr };
        /// The role that owner gave this pass (empty = anonymous).
        String role;
        /// How long the last frame's record of this pass took, in microseconds (0: it did not run then).
        /// Measured by the compile, where that record happens (see buildFrameValue); reported through the frame
        /// plan (see PassRow::record_micros).
        std::uint64_t record_micros = 0;
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

    /** @brief Gets what a frame of this engine consists of: the passes it runs, in order. */
    [[nodiscard]] FrameTable frameTable() const;

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

    /** @brief Removes every pass whose slot is owned by @p owner (each one as removePass). */
    void removePassesOwnedBy(const void* owner, RenderBackend* device) noexcept;

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

    /** @brief Adds the engine half's per-pass cost to the plan a host reads. */
    void addEngineMicros(std::uint32_t slot, std::uint64_t micros) noexcept;

  private:
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

    /** @brief WHICH image a declared output is, as the wiring check compares it (design §14.4). */
    struct OutputIdentity {
        raw_ptr<const RenderTarget> target{ nullptr };   // set once the image is bound
        int                         attachment = 0;      // colour index; kWholeTarget = the whole target
        bool                        depth      = false;  // a depth attachment is NOT attachment 0
        raw_ptr<const ImageRef>     declared{ nullptr }; // identity while unbound

        /// @brief The coarse form: a whole target, as a bundle declaration names it.
        static constexpr int kWholeTarget = -1;

        [[nodiscard]] bool operator==(const OutputIdentity& other) const noexcept;
        [[nodiscard]] static OutputIdentity of(const ImageRef& image) noexcept;
        [[nodiscard]] static OutputIdentity colorOf(const RenderTarget& target, int attachment) noexcept;
        [[nodiscard]] static OutputIdentity depthOf(const RenderTarget& target) noexcept;
        [[nodiscard]] static OutputIdentity targetOf(const RenderTarget& target) noexcept;
        [[nodiscard]] bool operator<(const OutputIdentity& other) const noexcept;
    };

    /** @brief What one pass declared about one image or target, as the wiring checks compare it. */
    struct Declaration {
        raw_ptr<const ImageRef>     image{ nullptr };
        raw_ptr<const RenderTarget> target{ nullptr };
        raw_ptr<const RenderPass>   pass{ nullptr };
        std::size_t                 index{ 0 };
        OutputIdentity              identity{};
    };

    /** @brief A declared image nobody bound to a target, and who declared it. */
    struct UnboundDeclaration {
        raw_ptr<const ImageRef>   image{ nullptr };
        raw_ptr<const RenderPass> pass{ nullptr };
        bool                      as_output{ false };
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

    /** @brief Answers the names the HOST publishes (see RenderEngine::publish). */
    [[nodiscard]] std::vector<String> hostOutputNames() const;

  private:
    Diagnostics&       diagnostics_;
    PublicationStore&  publications_;
    FrameResources&    resources_;

    // THE COMPILE'S TWO BORROWED FACTS, set for the length of one compile() call: the frame value being filled and
    // the device transients are released through. Neither outlives the call.
    EngineFrame*   frame_{ nullptr };
    RenderBackend* device_{ nullptr };

    std::vector<Slot> slots_; // uniform ordered pass registry

    // The wiring checks' tables (see collectPlanProblems). MEMBERS, cleared every compile: the pass count is small, a
    // flat scan beats a tree, and a vector keeps its capacity - so a steady frame allocates nothing here.
    std::vector<Declaration>        wiring_filled_;
    std::vector<Declaration>        wiring_claimed_;
    std::vector<OutputIdentity>     wiring_colliding_;
    std::vector<UnboundDeclaration> wiring_unbound_;
    std::vector<OutputIdentity>     wiring_promised_images_;

    /// This frame's compiled rows, in the order they run (reused; one per slot).
    std::vector<PlanRow> plan_rows_;
    /// The execution order of plan_rows_ (indices), decided by scheduleRows(); also what buildFrameValue walks.
    std::vector<std::uint32_t> frame_order_;
    /// The announced sort key of each plan row, rebuilt by scheduleRows() (the scheduler reads a contiguous span).
    std::vector<int> frame_order_keys_;
    /// The scheduler's working memory (see FrameScheduler): one per engine, reused every frame.
    std::unique_ptr<FrameScheduler> scheduler_;
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
