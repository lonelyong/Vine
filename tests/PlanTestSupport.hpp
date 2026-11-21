#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/PassChain.hpp>
#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/FullscreenPass.hpp>
#include <vine/graphics/Scene.hpp>

/**
 * @brief The plan spelling of "run this pass at this order", for a test.
 *
 * @details
 * - A PLAN IDENTIFIES A PASS BY NAME and the ENGINE builds the node. `applyPassForTest` states the declaration and
 *   answers the ENGINE'S node; `registerPassForTest` does the same and REPLACES the test's handle with it, so a test
 *   that configures the pass after registering keeps driving the node that actually runs.
 * - IT IS TEST SUPPORT, not an API: a production contributor states its own plans (see `PassChain` and the demo's
 *   `addDemoPass`), so the helper exists only where a test wants "one pass at one order" without composing one.
 * - THE OWNER DEFAULTS TO THE ENGINE ITSELF (an `Object` identity), so every helper call is one scoped upsert: a
 *   test's second call adds a row instead of replacing the session's whole shape (which is what the whole-session
 *   form does, on purpose).
 */
namespace vn::test
{

namespace detail
{
/** @brief A fresh, unique name for a test pass declared without one (a plan identifies a pass by name). */
inline vn::String nextTestPassName()
{
    static std::atomic<std::uint64_t> s_next{ 0 };
    const std::string text = "test_pass_" + std::to_string(s_next.fetch_add(1, std::memory_order_relaxed));
    return vn::String(std::u8string(text.begin(), text.end()));
}
} // namespace detail

/**
 * @brief The pass identity a hand-built FrameDescription names (see FrameDescription::Pass::pass).
 *
 * @details
 * - AN ENTRY THAT NAMES NO PASS IS NOTHING TO RUN (see RenderFrameExecutor::executeFrame), so a test that builds a
 *   frame value by hand has to name one - otherwise it is handing the backend a frame with no work in it.
 * - ONE IDENTITY SERVES EVERY FRAME, because a backend keys per-pass retained state by it and these frames are
 *   about the drawing path, not about per-pass retention: the object lives for the whole process, so a frame that
 *   borrows it stays valid for the call that runs it.
 *
 * @return A pass that outlives every frame of the process.
 */
inline vn::raw_ptr<const vn::graphics::RenderPass> framePassForTest()
{
    static const vn::intrusive_ptr<vn::graphics::RenderPass> s_pass(new vn::graphics::RenderPass());
    return s_pass.get();
}

/**
 * @brief States one declaration and answers the registered node.
 *
 * @details
 * - THE PLAN IS THE WHOLE SHAPE: the helper composes the engine's current plan plus this declaration and applies it,
 *   so a pass that reads a name another pass publishes validates no matter which of the two the test stated first
 *   (a one-row scoped apply could not see the other row yet). Every existing row is re-stated as it stands, so
 *   nothing is removed and nothing is rebound.
 *
 * @param engine Engine to state the pass on.
 * @param desc   The pass to state (its name is its identity; an unnamed one gets a test name).
 * @return The registered node, or null when the plan was refused.
 */
inline vn::intrusive_ptr<vn::graphics::RenderPass> applyPassForTest(vn::graphics::RenderEngine& engine, vn::graphics::PassDesc desc)
{
    if (desc.name.empty()) {
        desc.name = detail::nextTestPassName();
    }
    const vn::String name = desc.name; // the plan takes the declaration by value
    vn::graphics::Plan plan = engine.planFor(&engine);
    // Same name, same row: the restatement REPLACES the declaration the plan already carries (the engine updates the
    // node in place, which is what keeps the backend state keyed by it).
    plan.passes.erase(std::remove_if(plan.passes.begin(),
                                     plan.passes.end(),
                                     [&name](const vn::graphics::PassDesc& existing) { return existing.name == name; }),
                      plan.passes.end());
    plan.passes.push_back(std::move(desc));
    if (!engine.applyPlan(plan, &engine).applied()) {
        return nullptr;
    }
    return vn::intrusive_ptr<vn::graphics::RenderPass>(engine.passFor(name, &engine));
}

/** @brief States @p desc at @p order and answers the registered node. */
inline vn::intrusive_ptr<vn::graphics::RenderPass> applyPassForTest(vn::graphics::RenderEngine& engine, vn::graphics::PassDesc desc, int order)
{
    desc.order = order;
    return applyPassForTest(engine, std::move(desc));
}

/** @brief States @p desc at @p order and keeps no node (the `addPass(desc)` spelling). */
inline void registerPassForTest(vn::graphics::RenderEngine& engine, vn::graphics::PassDesc desc, int order)
{
    (void)applyPassForTest(engine, std::move(desc), order);
}

/** @brief States @p desc and keeps no node (the declaration carries its own order). */
inline void registerPassForTest(vn::graphics::RenderEngine& engine, vn::graphics::PassDesc desc)
{
    (void)applyPassForTest(engine, std::move(desc));
}

/**
 * @brief Edits the declaration @p pass IS and states it again (the one explicit diff a live edit is).
 *
 * @details
 * - A pass is READ-ONLY to its holder (see E-04): a test that changes one field copies the declaration, changes it and
 *   hands it back - exactly what a host does (`PassChain::restate` or `applyPlan`). The node survives (the plan updates
 *   it in place), so identity comparisons keep working.
 *
 * @param engine Engine whose table changes.
 * @param pass   Pass to edit (null answers null).
 * @param edit   Receives the declaration to change.
 * @return The same node (its declaration now states the edit), or null when the plan was refused.
 */
template <typename Edit>
inline vn::intrusive_ptr<vn::graphics::RenderPass> updatePassForTest(vn::graphics::RenderEngine& engine, vn::raw_ptr<vn::graphics::RenderPass> pass, Edit&& edit)
{
    if (pass == nullptr) {
        return nullptr;
    }
    vn::graphics::PassDesc desc = pass->declaration();
    std::forward<Edit>(edit)(desc);
    return applyPassForTest(engine, std::move(desc));
}

/** @brief The handle spelling of updatePassForTest (a test's `intrusive_ptr` handle). */
template <typename Edit>
inline vn::intrusive_ptr<vn::graphics::RenderPass> updatePassForTest(vn::graphics::RenderEngine& engine, const vn::intrusive_ptr<vn::graphics::RenderPass>& pass, Edit&& edit)
{
    return updatePassForTest(engine, pass.get(), std::forward<Edit>(edit));
}

/**
 * @brief Restates one of a chain's declarations THROUGH the chain (see PassChain::restate).
 *
 * @details
 * - The chain-owned spelling of a live edit: the chain is the scope that owns the row, so it is the one that hands the
 *   changed declaration back to the engine's plan (a host never edits a chain's pass any other way - see E-04).
 *
 * @param chain Chain that owns the row.
 * @param pass  The pass to edit (null answers false).
 * @param edit  Receives the declaration to change.
 * @return true when the shape was applied; false when the row is not the chain's or the plan was refused.
 */
template <typename Edit>
inline bool restatePassForTest(vn::graphics::PassChain& chain, vn::raw_ptr<vn::graphics::RenderPass> pass, Edit&& edit)
{
    if (pass == nullptr) {
        return false;
    }
    vn::graphics::PassDesc desc = pass->declaration();
    std::forward<Edit>(edit)(desc);
    return chain.restate(desc);
}

/**
 * @brief The TYPED handle spelling of a Screen declaration (`intrusive_ptr<FullscreenPass>`).
 *
 * @details
 * - A plan states a `PassKind`, and the node the engine builds for it is the built-in of that kind; a handle typed to
 *   `FullscreenPass` is therefore the node the engine built, not a template the test configured.
 *
 * @param pass The registered node (a Screen declaration, or null).
 * @return The same node as a FullscreenPass handle (null when @p pass is null).
 */
inline vn::intrusive_ptr<vn::graphics::FullscreenPass> asFullscreenPassForTest(const vn::intrusive_ptr<vn::graphics::RenderPass>& pass)
{
    return vn::intrusive_ptr<vn::graphics::FullscreenPass>(static_cast<vn::graphics::FullscreenPass*>(pass.get()));
}

/**
 * @brief Removes @p pass by name, through the whole-session plan (the shape without that row).
 *
 * @param engine Engine whose table changes.
 * @param pass   Pass to remove (null is a no-op).
 */
inline void removePassForTest(vn::graphics::RenderEngine& engine, vn::raw_ptr<vn::graphics::RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    // A REMOVAL IS A SESSION STATEMENT (a scoped plan never removes, see RenderEngine::applyPlan): the whole shape
    // minus that row.
    vn::graphics::Plan plan = engine.plan();
    for (std::size_t index = 0; index < plan.passes.size(); ++index) {
        if (plan.passes[index].name == pass->name()) {
            plan.passes.erase(plan.passes.begin() + static_cast<std::ptrdiff_t>(index));
            break;
        }
    }
    (void)engine.applyPlan(plan);
}

/** @brief Removes every registered pass (the empty whole-session plan). */
inline void clearPassesForTest(vn::graphics::RenderEngine& engine)
{
    (void)engine.applyPlan(vn::graphics::Plan{});
}

} // namespace vn::test
