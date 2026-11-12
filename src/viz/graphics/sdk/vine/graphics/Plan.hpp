#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vine/String.hpp>

#include "PassDesc.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Why a plan was refused, as ONE stable code per cause.
 *
 * @details
 * - A CODE, not a sentence: the answer a host acts on is "which of these five things is wrong", and a
 *   sentence is what changes the day someone rewords it. The sentence accompanies the code (see
 *   PlanProblem::detail), the code is what a test and a host switch on.
 * - `SessionBusy` is the only code that is not about the plan's own content: it says the SHAPE cannot be
 *   changed right now, because a frame is being recorded against the table it would replace.
 *
 * @note
 * - The set is closed on purpose (`G8`): a new failure of a plan is a new code added here, next to its
 *   check in validatePlan(), not a new string somewhere in a report.
 */
enum class PlanProblemCode : std::uint8_t
{
    EmptyPassName,     // A pass has no name, so a plan cannot diff it against the shape it replaces.
    DuplicatePassName, // Two passes answer the same name: one plan, one pass of that name.
    UnknownInput,      // A named input no pass of the plan (and no host binding) publishes.
    DependencyCycle,   // The named inputs form a cycle: no execution order satisfies it.
    TooManyPasses,     // More passes than kMaxPlanPasses (the plan is refused, not truncated).
    SessionBusy,       // A frame is being recorded; apply the plan between frames.
};

/**
 * @brief One problem a plan has, as a value (see validatePlan).
 *
 * @details
 * - `pass` names the subject when the problem is about one pass (empty when it is about the plan as a
 *   whole, or when the pass has no name to name it by).
 * - `detail` is for a human reading the report; it is NOT a contract, and a test asserts on `code`.
 */
struct VN_GRAPHICS_API PlanProblem {
    /// What is wrong (see PlanProblemCode).
    PlanProblemCode code{ PlanProblemCode::EmptyPassName };
    /// The pass the problem is about, or empty when it is about the plan.
    String pass;
    /// The sentence a report carries (human text, not a contract).
    String detail;
};

/**
 * @brief The most passes one plan may declare.
 *
 * @details
 * - A cap rather than a growth policy: a frame with more than this many passes is a mistake (a plan built
 *   in a loop over content, an effect that registered itself per view), and refusing it keeps every table
 *   the compile builds bounded - which is what lets the plan diff be a flat, allocation-free walk.
 */
inline constexpr std::size_t kMaxPlanPasses = 64;

/**
 * @brief A session's shape, as a value: the passes of a frame, in declaration order.
 *
 * @details
 * - WHAT IT IS. The frame graph a host DECLARES - one `PassDesc` per pass, in the order the host wants
 *   equal orders resolved by. `RenderEngine::applyPlan()` turns it into the shape the engine runs, by
 *   diffing it against what is registered: a name that appears is added, a name present in both is updated
 *   IN PLACE (the node, and therefore the backend state keyed by it, survives), a name that disappears is
 *   removed. Nothing calls add/remove, and a pass that keeps its name keeps its GPU state.
 * - A PASS' IDENTITY IS ITS NAME (`PassDesc::name`), which is why an empty name and a duplicate name are
 *   refused rather than guessed at: the diff has nothing else to compare, and a plan that cannot be diffed
 *   would be applied as "remove everything, add everything" - the churn this type exists to remove.
 * - WHAT IT DOES NOT CARRY: measured facts (the record time of the last frame) and the engine's own
 *   bookkeeping (owner/role, the transient a compile resolved). Those belong to the compiled frame, which
 *   is a different value (`FramePlan`) - one describes what the frame SHOULD be, the other what the engine
 *   compiled, and neither can be mistaken for the other.
 * - It holds references (a camera, a scene, a target), so copying a plan copies handles, not pictures:
 *   cheap, and not a snapshot of the content behind them.
 *
 * @note
 * - `G12` is the whole point: with the shape stated as a value, "edit the session's shape" is one
 *   assignment, and the shapes a host cannot express (a pass added while its table is being walked) stop
 *   existing instead of being refused at run time.
 */
struct VN_GRAPHICS_API Plan {
    /// The passes, in declaration order; the pass' NAME is its identity within the plan (see PassDesc::name).
    std::vector<PassDesc> passes;

    /**
     * @brief Renders the plan as the table a human reads.
     *
     * @details
     * - One line per pass: its index, name, kind, effective order, target and named inputs - enough to see
     *   WHY two shapes differ when a diff does something unexpected. An empty plan says so in one line.
     *
     * @note
     * - The text is for humans and is not a parsing contract; `passes` is what a machine reads.
     *
     * @return The table, UTF-8, lines separated by '\n' (no trailing newline).
     */
    [[nodiscard]] String describe() const;
};

/**
 * @brief Checks a plan against itself, without a device, a frame or an engine.
 *
 * @details
 * - Pure: same plan (and same externally published names) answers the same problems, which is what makes a
 *   refused plan reproducible in a test and in a bug report.
 * - What it checks, in the order a host reads them: `TooManyPasses`, `EmptyPassName`, `DuplicatePassName`,
 *   `UnknownInput`, `DependencyCycle`. It returns EVERY problem, not the first, so one call tells a host
 *   everything that has to change.
 * - Inputs are matched BY NAME (`PassDesc::input_names` against `PassDesc::output_name`); the object-typed
 *   inputs are identities, not names, so they are checked by the compile that resolves them (see
 *   RenderEngine::framePlan). A pass that names its own output is feedback, not a cycle, and is allowed -
 *   the same rule the compile applies.
 *
 * @param plan              The shape to check.
 * @param external_outputs  Names published by someone other than the plan's passes (a host binding, see
 *                          RenderEngine::publish). An input naming one of these is served, not unknown.
 *
 * @return Every problem found, empty when the plan is applicable.
 */
[[nodiscard]] VN_GRAPHICS_API std::vector<PlanProblem> validatePlan(const Plan& plan, std::span<const String> external_outputs = {});

/**
 * @brief Spells a plan problem's code as the stable token a report and a log line carry.
 *
 * @details
 * - One spelling per code ("duplicate-pass-name", not "duplicate"), greppable and asserted by a test, so
 *   a host that logs the code and a human reading the log agree about which failure it was.
 *
 * @param code Code to spell.
 *
 * @return The token, e.g. "unknown-input".
 */
[[nodiscard]] VN_GRAPHICS_API String planCodeText(PlanProblemCode code);

VN_GRAPHICS_NS_END
