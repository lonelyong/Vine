#include <vine/graphics/Plan.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "PassScheduler.hpp"

VN_GRAPHICS_NS_BEGIN

namespace
{

/**
 * @brief Converts an SDK-built ASCII number to the string type a report is assembled from.
 *
 * @param text ASCII text (the SDK's reports carry every number as digits).
 * @return The same text as a u8 string.
 */
String toU8(const std::string& text)
{
    return String(std::u8string(text.begin(), text.end()));
}

/** @brief Spells a count for a report (see toU8). */
String digits(std::size_t value)
{
    return toU8(std::to_string(value));
}

} // namespace

std::vector<PlanProblem> validatePlan(const Plan& plan, std::span<const String> external_outputs)
{
    // THE WHOLE CHECK IS ONE PASS over the plan plus one pass over the names it publishes: no device, no
    // frame, no engine state, so a refused shape is reproducible in a test and in a bug report alike.
    std::vector<PlanProblem> problems;

    if (plan.passes.size() > kMaxPlanPasses) {
        problems.push_back(
            PlanProblem{ PlanProblemCode::TooManyPasses,
                         String(),
                         String(u8"the plan declares ") + digits(plan.passes.size()) + String(u8" passes; the limit is ") + digits(kMaxPlanPasses) });
    }

    // WHICH PASS ANSWERS WHICH NAME. Two maps, because two different questions are asked: a pass NAME is the
    // identity the diff uses (so it must be unique), while an OUTPUT name is what a consumer reads (so it is
    // the edge's producer). The first declaration of a name wins its slot (a later duplicate is reported
    // instead of replacing it), because the wiring edges have to point somewhere well defined even in a
    // refused plan - the report then names the duplicate, not a half-built graph.
    std::map<String, std::size_t> producer_of;
    std::map<String, std::size_t> published_by;
    for (std::size_t index = 0; index < plan.passes.size(); ++index) {
        const PassDesc& pass = plan.passes[index];
        const String&   name = pass.name;
        // THE DECLARATION'S OWN SHAPE: a Target or an Output that states two forms at once is a value defect,
        // refused here instead of letting one of them win at compile time (see Target::isValid / Output::isValid).
        if (!pass.target.isValid()) {
            problems.push_back(PlanProblem{ PlanProblemCode::InvalidTarget,
                                            name,
                                            String(u8"pass '") + name +
                                                String(u8"' states a target that mixes its forms: a Target is the "
                                                       u8"window, one explicit target, or one transient name - not several") });
        }
        if (!pass.output.isValid()) {
            problems.push_back(PlanProblem{ PlanProblemCode::InvalidOutput,
                                            name,
                                            String(u8"pass '") + name +
                                                String(u8"' promises both a whole target and one image for its output; "
                                                       u8"one name covers one claim") });
        }
        // A KIND READS ITS OWN FIELDS. The values below are documented as belonging to one execution, so stating
        // one on the other kind is a declaration nothing consumes - refused here instead of being left to look like
        // a declaration at the call site (see DrawKind).
        //
        // `content` is NOT one of them for a Fullscreen pass: FullscreenPass reads it for the scene's LIGHT SET, so a
        // composite pass names the scene whose lights its program shades with. The fields below are the ones the
        // other execution truly never reads.
        if (pass.kind == DrawKind::Fullscreen) {
            if (pass.program_override != nullptr) {
                problems.push_back(PlanProblem{ PlanProblemCode::FieldUnusedByKind,
                                                name,
                                                String(u8"pass '") + name +
                                                    String(u8"' is a Fullscreen pass and states program_override; its "
                                                           u8"program is `program`") });
            }
            if (pass.layer_mask != kAllLayers) {
                problems.push_back(PlanProblem{ PlanProblemCode::FieldUnusedByKind,
                                                name,
                                                String(u8"pass '") + name +
                                                    String(u8"' is a Fullscreen pass and states a layer mask; it draws "
                                                           u8"no content, so the mask would select nothing") });
            }
        }
        else if (pass.program != nullptr) {
            problems.push_back(PlanProblem{ PlanProblemCode::FieldUnusedByKind,
                                            name,
                                            String(u8"pass '") + name +
                                                String(u8"' is a Scene pass and states `program`; a scene pass shades "
                                                       u8"its content through program_override and its per-geometry "
                                                       u8"programs") });
        }
        if (name.empty()) {
            problems.push_back(PlanProblem{ PlanProblemCode::EmptyPassName,
                                            String(),
                                            String(u8"pass #") + digits(index) +
                                                String(u8" has no name; a plan identifies a pass by its name, so an unnamed pass "
                                                       u8"cannot be diffed against the shape it replaces") });
            continue;
        }
        if (!producer_of.emplace(name, index).second) {
            problems.push_back(PlanProblem{ PlanProblemCode::DuplicatePassName,
                                            name,
                                            String(u8"two passes answer the name '") + name +
                                                String(u8"'; one plan is one pass per name, so the diff has one node to update") });
        }
        const String& output = plan.passes[index].output.name;
        if (!output.empty()) {
            // Two publishers of one name is the compile's business (the latest publication serves the name);
            // for the EDGE it is enough that the name is served, so the first publisher answers it.
            published_by.emplace(output, index);
        }
    }

    // THE EDGES A PLAN CAN SEE WITHOUT AN ENGINE ARE THE NAMED ONES: a dependency is "this pass reads the
    // name that pass publishes", which is what the compile resolves. A target/image identity is a handle,
    // and "was it produced" is a fact of the frame, so the compile that resolves it judges it (see
    // validatePlan). Both walk the same PassDesc::inputs list, so no input is visible to one and not the
    // other.
    detail::PassScheduler  scheduler;
    std::vector<int>       orders;
    orders.reserve(plan.passes.size());
    for (const PassDesc& pass : plan.passes) {
        orders.push_back(pass.order.value_or(passStageOrder(pass.stage)));
    }
    scheduler.reset(orders);

    for (std::size_t consumer = 0; consumer < plan.passes.size(); ++consumer) {
        for (const ImageInput& input : plan.passes[consumer].inputs) {
            if (input.kind() != ImageInput::Kind::Name) {
                continue;
            }
            const String& name     = input.name();
            const auto    producer = published_by.find(name);
            if (producer == published_by.end()) {
                if (std::find(external_outputs.begin(), external_outputs.end(), name) == external_outputs.end()) {
                    problems.push_back(PlanProblem{ PlanProblemCode::UnknownInput,
                                                    plan.passes[consumer].name,
                                                    String(u8"pass '") + plan.passes[consumer].name + String(u8"' reads '") + name +
                                                        String(u8"', which no pass of the plan publishes and no host binding serves") });
                }
                continue;
            }
            // A pass reading its own output is the FEEDBACK pattern (read what you wrote), not a cycle: the
            // compile applies the same rule, so validation must not refuse what a frame can run.
            if (producer->second == consumer) {
                continue;
            }
            scheduler.addEdge(static_cast<std::uint32_t>(producer->second), static_cast<std::uint32_t>(consumer));
        }
    }

    const detail::PassSchedule& schedule = scheduler.schedule();
    for (const std::vector<std::uint32_t>& component : schedule.cycles) {
        String members;
        for (const std::uint32_t node : component) {
            if (!members.empty()) {
                members += String(u8", ");
            }
            members += plan.passes[node].name;
        }
        problems.push_back(PlanProblem{ PlanProblemCode::DependencyCycle,
                                        component.empty() ? String() : plan.passes[component.front()].name,
                                        String(u8"passes ") + members +
                                            String(u8" declare a dependency cycle (each reads what another publishes), which no "
                                                   u8"execution order can satisfy") });
    }

    return problems;
}

namespace detail
{

String planCodeText(PlanProblemCode code)
{
    switch (code) {
    case PlanProblemCode::EmptyPassName: return String(u8"empty-pass-name");
    case PlanProblemCode::DuplicatePassName: return String(u8"duplicate-pass-name");
    case PlanProblemCode::UnknownInput: return String(u8"unknown-input");
    case PlanProblemCode::DependencyCycle: return String(u8"dependency-cycle");
    case PlanProblemCode::TooManyPasses: return String(u8"too-many-passes");
    case PlanProblemCode::InvalidTarget: return String(u8"invalid-target");
    case PlanProblemCode::InvalidOutput: return String(u8"invalid-output");
    case PlanProblemCode::FieldUnusedByKind: return String(u8"field-unused-by-kind");
    }
    return String(u8"unknown");
}

} // namespace detail

String Plan::describe() const
{
    if (passes.empty()) {
        return String(u8"(empty plan)");
    }
    String text;
    for (std::size_t index = 0; index < passes.size(); ++index) {
        const PassDesc& pass = passes[index];
        if (!text.empty()) {
            text += String(u8"\n");
        }
        text += String(u8"pass[") + digits(index) + String(u8"] name='") + pass.name + String(u8"' kind=") +
                (pass.kind == DrawKind::Fullscreen ? String(u8"Fullscreen") : String(u8"Scene")) + String(u8" order=") +
                digits(static_cast<std::size_t>(pass.order.value_or(passStageOrder(pass.stage)))) + String(u8" target=") +
                (pass.target.isExplicit() ? String(u8"explicit")
                                          : (pass.target.isTransient() ? String(u8"transient") : String(u8"window"))) +
                String(u8" inputs=[");
        for (std::size_t input = 0; input < pass.inputs.size(); ++input) {
            if (input != 0) {
                text += String(u8", ");
            }
            text += pass.inputs[input].description();
        }
        text += String(u8"] output=") + (pass.output.name.empty() ? String(u8"-") : pass.output.name);
    }
    return text;
}

VN_GRAPHICS_NS_END
