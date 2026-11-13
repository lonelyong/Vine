#include <vine/graphics/Plan.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "FrameScheduler.hpp"

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
        const String& name = plan.passes[index].name;
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
        const String& output = plan.passes[index].output_name;
        if (!output.empty()) {
            // Two publishers of one name is the compile's business (the latest publication serves the name);
            // for the EDGE it is enough that the name is served, so the first publisher answers it.
            published_by.emplace(output, index);
        }
    }

    // THE EDGES ARE THE NAMED INPUTS, nothing else: a dependency is "this pass reads the name that pass
    // publishes", which is what the compile resolves. The scheduler is the engine's own, so a plan and a
    // compiled frame cannot disagree about what a cycle is.
    detail::FrameScheduler scheduler;
    std::vector<int>       orders;
    orders.reserve(plan.passes.size());
    for (const PassDesc& pass : plan.passes) {
        orders.push_back(pass.order.value_or(passStageOrder(pass.stage)));
    }
    scheduler.reset(orders);

    for (std::size_t consumer = 0; consumer < plan.passes.size(); ++consumer) {
        for (const String& input : plan.passes[consumer].input_names) {
            const auto producer = published_by.find(input);
            if (producer == published_by.end()) {
                const bool served = std::find(external_outputs.begin(), external_outputs.end(), input) != external_outputs.end();
                if (!served) {
                    problems.push_back(PlanProblem{ PlanProblemCode::UnknownInput,
                                                    plan.passes[consumer].name,
                                                    String(u8"pass '") + plan.passes[consumer].name + String(u8"' reads '") + input +
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

    const detail::FrameSchedule& schedule = scheduler.schedule();
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

String planCodeText(PlanProblemCode code)
{
    switch (code) {
    case PlanProblemCode::EmptyPassName: return String(u8"empty-pass-name");
    case PlanProblemCode::DuplicatePassName: return String(u8"duplicate-pass-name");
    case PlanProblemCode::UnknownInput: return String(u8"unknown-input");
    case PlanProblemCode::DependencyCycle: return String(u8"dependency-cycle");
    case PlanProblemCode::TooManyPasses: return String(u8"too-many-passes");
    }
    return String(u8"unknown");
}

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
                (pass.kind == PassKind::Screen ? String(u8"Screen") : String(u8"Scene")) + String(u8" order=") +
                digits(static_cast<std::size_t>(pass.order.value_or(passStageOrder(pass.stage)))) + String(u8" target=") +
                (pass.writes != nullptr ? String(u8"explicit") : String(u8"window")) + String(u8" inputs=[");
        for (std::size_t input = 0; input < pass.input_names.size(); ++input) {
            if (input != 0) {
                text += String(u8", ");
            }
            text += pass.input_names[input];
        }
        text += String(u8"] output=") + (pass.output_name.empty() ? String(u8"-") : pass.output_name);
    }
    return text;
}

VN_GRAPHICS_NS_END
