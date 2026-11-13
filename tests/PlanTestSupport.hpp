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

#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPass.hpp>
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

/** @brief States @p pass (its declared content) at @p order and answers the registered node. */
inline vn::intrusive_ptr<vn::graphics::RenderPass>
applyPassForTest(vn::graphics::RenderEngine& engine, const vn::intrusive_ptr<vn::graphics::RenderPass>& pass, int order)
{
    vn::graphics::PassDesc desc = pass->declaration();
    desc.order                  = order;
    return applyPassForTest(engine, std::move(desc));
}

/** @brief States @p pass drawing @p content at @p order and answers the registered node. */
inline vn::intrusive_ptr<vn::graphics::RenderPass>
applyPassForTest(vn::graphics::RenderEngine&                        engine,
                 const vn::intrusive_ptr<vn::graphics::RenderPass>& pass,
                 vn::intrusive_ptr<vn::graphics::Scene>             content,
                 int                                                order)
{
    vn::graphics::PassDesc desc = pass->declaration();
    desc.content                = std::move(content);
    desc.order                  = order;
    return applyPassForTest(engine, std::move(desc));
}

/** @brief States @p pass at @p order, replacing @p pass with the engine's node. */
inline void registerPassForTest(vn::graphics::RenderEngine& engine, vn::intrusive_ptr<vn::graphics::RenderPass>& pass, int order)
{
    pass = applyPassForTest(engine, pass, order);
}

/**
 * @brief The TYPED-handle spelling (`intrusive_ptr<ScreenPass>`): the node the engine builds is that kind.
 *
 * @details
 * - A plan states a `PassKind`, and the node the engine builds for it is the built-in of that kind; a handle typed to
 *   `ScreenPass` is therefore handed the `ScreenPass` the engine built (not the template it configured).
 *
 * @param engine Engine to state the pass on.
 * @param pass   Configured pass whose declaration is stated; receives the engine's node.
 * @param order  Execution order.
 */
template <typename PassT>
    requires std::derived_from<PassT, vn::graphics::RenderPass>
inline void registerPassForTest(vn::graphics::RenderEngine& engine, vn::intrusive_ptr<PassT>& pass, int order)
{
    vn::graphics::PassDesc desc = pass->declaration();
    desc.order                  = order;
    pass = vn::intrusive_ptr<PassT>(static_cast<PassT*>(applyPassForTest(engine, std::move(desc)).get()));
}

/** @brief States @p pass drawing @p content at @p order, replacing @p pass with the engine's node. */
inline void registerPassForTest(vn::graphics::RenderEngine&                  engine,
                                vn::intrusive_ptr<vn::graphics::RenderPass>& pass,
                                vn::intrusive_ptr<vn::graphics::Scene>       content,
                                int                                          order)
{
    pass = applyPassForTest(engine, pass, std::move(content), order);
}

/** @brief The `addPass(pass, nullptr, order)` spelling: a pass that names no content. */
inline void registerPassForTest(vn::graphics::RenderEngine&                  engine,
                                vn::intrusive_ptr<vn::graphics::RenderPass>& pass,
                                std::nullptr_t,
                                int                                          order)
{
    registerPassForTest(engine, pass, vn::intrusive_ptr<vn::graphics::Scene>{}, order);
}

/** @brief The const-handle spelling: states @p pass and hands no node back. */
inline void registerPassForTest(vn::graphics::RenderEngine& engine, const vn::intrusive_ptr<vn::graphics::RenderPass>& pass, int order)
{
    (void)applyPassForTest(engine, pass, order);
}

/** @brief The const-handle spelling with explicit content (see above). */
inline void registerPassForTest(vn::graphics::RenderEngine&                        engine,
                                const vn::intrusive_ptr<vn::graphics::RenderPass>& pass,
                                vn::intrusive_ptr<vn::graphics::Scene>             content,
                                int                                                order)
{
    (void)applyPassForTest(engine, pass, std::move(content), order);
}

/** @brief The const-handle spelling for a pass that names no content (see above). */
inline void registerPassForTest(vn::graphics::RenderEngine&                        engine,
                                const vn::intrusive_ptr<vn::graphics::RenderPass>& pass,
                                std::nullptr_t,
                                int                                                order)
{
    registerPassForTest(engine, pass, vn::intrusive_ptr<vn::graphics::Scene>{}, order);
}

/** @brief States one declaration and keeps no node (the `addPass(desc)` spelling). */
inline void registerPassForTest(vn::graphics::RenderEngine& engine, vn::graphics::PassDesc desc)
{
    (void)applyPassForTest(engine, std::move(desc));
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
