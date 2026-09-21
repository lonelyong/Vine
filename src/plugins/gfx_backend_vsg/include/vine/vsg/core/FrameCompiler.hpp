#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <vine/vsg/core/Diagnostics.hpp>
#include <vine/vsg/core/FrameArena.hpp>
#include <vine/vsg/core/FrameGraph.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/Observe.hpp>
#include <vine/vsg/core/TargetPlan.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The frame's COMPILATION stage: what the host asked for becomes an executable plan, and every
 * default is made explicit.
 *
 * WHAT IT DECIDES. (1) the order the pass scopes run in - the dependency graph's answer, not the call
 * order (see FrameGraph); (2) the resolved form of every default the description is allowed to leave
 * open: a viewport nobody announced becomes the whole target, a clear nobody announced becomes "no clear
 * except where a fresh attachment forces one", and a command with no program of its own becomes the
 * frame's default program; (3) which pass of this frame is the BOOTSTRAP one for each target (the first
 * writer into attachments that were just built or invalidated), and what each target's plan and depth plan
 * are (planTarget / depthPlan, the pure functions of the target layer).
 *
 * WHAT IT DOES NOT DECIDE. Which GPU objects exist, which pipeline a draw uses, what anything costs, and
 * whether a target should be rebuilt (that is planTarget's answer, read here, not invented here). It
 * touches no API type: the facts it needs arrive as plain values (FrameFacts) and the plan it produces is
 * plain values too, so a phase can drive the whole stage without a device.
 *
 * WHY IT NEEDS FACTS AT ALL. The description is what the host declared this frame; it cannot know that an
 * off-screen target is currently 0x103 in size, that its depth was promoted, or that another target
 * borrows it. Those are the backend's own account of its resources, so they arrive from the layer that
 * owns them (the API layer's registry) as TargetFacts - borrowed for the call, because the compiler only
 * reads them once, immediately.
 *
 * THE PLAN LIVES IN THE FRAME ARENA, and it is built from values only: no span points at a host container,
 * at a target's description, or at the API layer's registry - anything needed later was copied (see
 * FrameRecorder's file note for why that is the rule, and §2.5 P0-1 for the review that pinned it).
 */

V_VSG_NS_BEGIN

namespace core
{

/** @brief What the compiler knows about one target: the API layer's own account of it. */
struct TargetFacts
{
    const void*    target{nullptr};  ///< Identity; nullptr is the default framebuffer.
    TargetDesc     wanted{};         ///< The extent and shape THIS frame asks for.
    TargetInstance current{};        ///< What the backend currently has for it.
    DepthFacts     depth{};          ///< Promotion / borrowing / preservation facts.
};

/** @brief Everything the description cannot carry: the facts about what the frame draws into. */
struct FrameFacts
{
    std::span<const TargetFacts> targets{};  ///< One entry per target the frame draws into, window included.
};

/** @brief One instance of one command, resolved: identity, plus the state to deliver with it. */
struct CompiledCommand
{
    const void*   geometry{nullptr};      ///< Leaf geometry identity.
    std::uint64_t geometry_revision{0};   ///< `Geometry::revision()` at collection time.
    ProgramRef    program{};              ///< The command's own program, or the frame's default (resolved).
    const void*   material{nullptr};      ///< Material identity, or nullptr.
    vine::math::Mat4d model{};            ///< World-space model matrix.
    float         opacity{1.0F};          ///< Effective opacity.
    DynamicState  dynamic{};              ///< The dynamic layer, resolved (see resolveDynamicState).
};

/** @brief One drawing call, resolved. */
struct CompiledDraw
{
    DrawKind         kind{DrawKind::Content};  ///< Content drawing or a full-screen program.
    CameraSnapshot   camera{};                 ///< The camera announced with the call.
    vine::graphics::Viewport viewport{};       ///< Resolved: the announced rectangle, or the whole target.
    std::span<const LightRef> lights{};        ///< The consumed lights; empty = the backend default.
    const void*      source{nullptr};          ///< Screen draws: the target whose attachments are sampled.
    ProgramRef       program{};                ///< Screen draws: the fragment program to draw with.
    std::span<const CompiledCommand> commands{};  ///< Content draws: the instances to render.
};

/** @brief One pass scope, resolved: its scope attributes are final and every default is explicit. */
struct CompiledPass
{
    PassId        pass{0};                    ///< Pass identity.
    std::uint32_t schedule_index{0};          ///< Position in the execution order (0 = first).
    std::uint32_t target_index{0};            ///< Index into CompiledFrame::targets (the pass' target).
    vine::graphics::Viewport viewport{};      ///< Whole-target rectangle, for the draw calls that announced none.
    vine::graphics::DepthMode depth{vine::graphics::DepthMode::TestAndWrite};  ///< The pass' depth policy.
    ClearPolicy   clear{};                    ///< The effective clear policy (announced, or the default).
    bool          bootstrap{false};           ///< First writer of freshly built attachments: it must clear.
    bool          depth_preserved{false};     ///< A later pass reads the depth this one writes: never clear it.
    std::uint32_t color_attachments{0};       ///< Colour attachments of the pass' target (part of a pipeline's identity).
    bool          depth_sampleable{false};    ///< The pass' target offers a sampleable depth (part of the identity).
    std::span<const CompiledDraw> draws{};    ///< Drawing calls, in the order they were collected.
};

/** @brief One target the frame draws into, with what has to happen to it (see planTarget / depthPlan). */
struct CompiledTarget
{
    const void*    target{nullptr};      ///< Identity; nullptr is the default framebuffer.
    TargetDecision decision{};           ///< None / Repair / ResizeInPlace / Rebuild, and why.
    DepthPlan      depth{};              ///< Whether the depth is sampleable, borrowed or preserved.
};

/** @brief One frame, compiled: what the executor walks, in the order it walks it. */
struct CompiledFrame
{
    FrameToken  token{};                          ///< The frame this plan belongs to.
    ProgramRef  default_program{};                ///< What content without a program of its own draws with.
    std::span<const CompiledPass>   passes{};     ///< In EXECUTION order, not call order.
    std::span<const CompiledTarget> targets{};    ///< Every target the frame draws into, in first-use order.
    std::uint32_t cycles{0};                      ///< Cyclic components skipped this frame (0 = a valid schedule).
};

/**
 * @brief The compilation stage (see the file note for what it decides and what it refuses to).
 */
class FrameCompiler
{
  public:
    /** @brief Creates a compiler writing into @p arena and reporting through @p diagnostics.
     *
     * The arena is the same one the recorder used for this frame (the recorder resets it in beginFrame),
     * so compile() must be called between endFrame() and the next beginFrame().
     *
     * @param arena       Storage the plan is built in.
     * @param diagnostics The backend's one diagnostic route.
     * @param observe     The counters a phase gates on.
     */
    FrameCompiler(FrameArena& arena, Diagnostics& diagnostics, Observe& observe) noexcept;

    FrameCompiler(const FrameCompiler&)            = delete;
    FrameCompiler& operator=(const FrameCompiler&) = delete;

    /** @brief Turns one collected frame into an executable plan.
     *
     * @param description What the recorder collected this frame.
     * @param facts       The backend's own account of the targets the frame draws into.
     * @return The compiled plan, valid until the next beginFrame().
     */
    const CompiledFrame& compile(const FrameDescription& description, const FrameFacts& facts);

    /** @brief Gets the plan published by the last compile(). */
    [[nodiscard]] const CompiledFrame& frame() const noexcept;

    /** @brief Gets the dependency graph of the last compile() (for tests and diagnostics). */
    [[nodiscard]] const FrameGraph& graph() const noexcept;


  private:
    /** @brief Finds the facts for @p target, or nullptr when the backend does not know it. */
    [[nodiscard]] static const TargetFacts* findTarget(const FrameFacts& facts, const void* target) noexcept;

    /** @brief Resolves one pass' drawing calls into the arena (viewports and programs made explicit). */
    [[nodiscard]] std::span<const CompiledDraw> resolveDraws(const CollectedPass& pass,
                                                             const vine::graphics::Viewport& whole,
                                                             const ProgramRef& default_program);

    /** @brief Resolves one drawing call's instances (program and dynamic state made explicit). */
    [[nodiscard]] std::span<const CompiledCommand> resolveCommands(const CollectedDraw& draw,
                                                                   const ProgramRef& default_program,
                                                                   vine::graphics::DepthMode pass_depth);

    /** @brief Reports one condition through the one route. */
    void report(vine::graphics::DiagnosticCategory category, const std::string& message);


  private:
    /** @brief One target the frame draws into, as the compiler works it out (first use owns the slot). */
    struct TargetSlot
    {
        CompiledTarget compiled{};  ///< What the executor will read.
        int            width{0};    ///< Extent the frame asked for (0 = not usable).
        int            height{0};   ///< Extent the frame asked for (0 = not usable).
        std::uint32_t  color_attachments{0};  ///< Colour attachments the frame asked for.
    };

    /// Sentinel for "no target slot" in the per-pass table.
    static constexpr std::uint32_t kNoTarget = 0xFFFFFFFFU;

    FrameArena&  arena_;        ///< The frame's storage (shared with the recorder).
    Diagnostics& diagnostics_;  ///< The one diagnostic route.
    Observe&     observe_;      ///< The counters a phase gates on.

    FrameGraph    graph_;       ///< The pass-dependency graph of the frame being compiled.
    CompiledFrame frame_;       ///< The plan last published.

    // Working memory, one frame at a time, capacity kept across frames (see the file note: the PLAN is
    // arena-backed, these are the tables it is built from).
    std::vector<std::uint8_t> servable_;      ///< Per pass: whether it can be served this frame.
    std::vector<std::uint32_t> target_index_; ///< Per pass: its TargetSlot, or kNoTarget.
    std::vector<std::uint8_t>  bootstrapped_; ///< Per slot: whether its first writer has run.
    std::vector<TargetSlot>    targets_;      ///< The target table of the frame being compiled.
};

}  // namespace core

V_VSG_NS_END
