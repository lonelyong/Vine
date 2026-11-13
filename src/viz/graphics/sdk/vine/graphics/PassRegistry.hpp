#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <optional>

#include <vine/Lease.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Plan.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderPass;
class Scene;
struct PassDesc;
struct Plan;

/**
 * @brief The engine's pass registry - the ONE table a frame runs from - as a narrow handle.
 *
 * @details
 * - ONE STORAGE, ONE CONTRACT: the engine owns the ordered slot table (order, owner, transient, timing), and this
 *   handle is a view of it. Content is not in that table: it is a field of the pass' own declaration
 *   (`PassDesc::content`), which is what `bindContent()` writes. There is no second list here - `count()` and
 *   `orderOf()` walk the engine's table.
 * - THE SHAPE IS A PLAN, HERE TOO: a pass is registered by applying a `Plan` scoped to its owner (`apply`), so the one
 *   way a table row appears is the same value-shaped diff a host uses (see RenderEngine::applyPlan). A `PassChain`
 *   builds its plan and hands it over through this handle, which is what lets a recipe depend on the registry instead
 *   of the whole engine.
 * - THE ENGINE MAY OUTLIVE OR PRECEDE THE HANDLE, and the handle knows it (a `Lease`): once the engine is destroyed
 *   `isValid()` answers false and every call is a no-op (a lookup answers null) instead of reaching into freed
 *   memory. That is what lets a chain outlive its engine safely.
 */
class VN_GRAPHICS_API PassRegistry {
  public:
    /** @brief Constructs an empty handle that refers to no engine (see isValid). */
    PassRegistry() noexcept = default;

  public:
    /** @brief Gets whether the engine this handle reads is still alive.
     *
     * @return true when the engine is there and the calls below reach it.
     */
    [[nodiscard]] bool isValid() const noexcept;

    /**
     * @brief Applies @p plan as @p owner's rows (see RenderEngine::applyPlan(plan, owner)).
     *
     * @param plan  The rows this contributor states.
     * @param owner The contributor the rows belong to.
     * @return What the call did (see PlanResult); a handle whose engine is gone answers
     *         `Refused` with `PlanRefusal::EngineGone`.
     */
    [[nodiscard]] PlanResult apply(const Plan& plan, const void* owner);

    /**
     * @brief Finds the registered pass whose name is @p name.
     *
     * @param name Pass name (a plan's identity).
     * @return The registered node, or null when no row carries that name.
     */
    [[nodiscard]] raw_ptr<RenderPass> find(const String& name) const;

    /**
     * @brief Finds the pass @p owner stated under @p name (see RenderEngine::passFor).
     *
     * @param name  Pass name.
     * @param owner The scope that stated it.
     * @return The node, or null.
     */
    [[nodiscard]] raw_ptr<RenderPass> findFor(const String& name, const void* owner) const;

    /** @brief Gets how many passes are registered.
     *
     * @return The number of rows in the engine's table.
     */
    [[nodiscard]] std::size_t count() const;

    /** @brief Gets the order a registered pass runs at.
     *
     * @param pass Pass to look up (null answers nothing).
     * @return The pass' order, or nullopt when it is not registered (which is also its liveness answer).
     */
    [[nodiscard]] std::optional<int> orderOf(raw_ptr<RenderPass> pass) const;

    /** @brief Rebinds which scene a registered pass renders.
     *
     * @details
     * - The binding is written into the pass' declaration (`PassDesc::content`); the registry keeps no copy.
     *
     * @param pass    Pass registered through this handle (by pointer).
     * @param content New content scene; null clears the binding.
     * @return true when @p pass is registered and its declaration now states @p content; false when there is no such
     *         pass (or the engine this handle refers to is gone), so nothing changed.
     */
    bool bindContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

    /** @brief Gets the content scene a registered pass renders.
     *
     * @param pass Pass registered through this handle (by pointer).
     * @return The bound content, or nullptr when the pass has none or is not registered.
     */
    [[nodiscard]] raw_ptr<Scene> contentOf(raw_ptr<RenderPass> pass) const;

  private:
    friend class RenderEngine;
    friend class PassChain;

    /** @brief Mints a handle over @p engine's table (only the engine does this).
     *
     * @param engine Engine whose table this handle reads.
     */
    explicit PassRegistry(Lease<RenderEngine> engine) noexcept;

  public:
    /**
     * @brief Removes every pass filed under @p owner (the scope's teardown; null is a no-op).
     *
     * @details
     * - A contributor's rows leave with the contributor (see RenderEngine::applyPlan(plan, owner)): a view drops its
     *   chain, a chain that owns its own scope drops itself.
     *
     * @param owner The scope whose rows are removed (an identity).
     */
    void clearOwnedBy(const void* owner) noexcept;

  private:

  private:
    Lease<RenderEngine> engine_;
};

VN_GRAPHICS_NS_END
