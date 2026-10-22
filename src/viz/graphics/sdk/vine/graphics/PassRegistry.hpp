#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <optional>
#include <vector>

#include <vine/Lease.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderPass;
class Scene;
struct PassDesc;

/**
 * @brief The engine's pass registry - the ONE table a frame runs from - as a narrow handle.
 *
 * @details
 * - ONE STORAGE, ONE CONTRACT: the engine owns the ordered slot table (order, content binding, transient, timing),
 *   and this handle is a view of it. There is no second list here - `count()` and `orderOf()` walk the engine's
 *   table, and `add()` files a pass into it.
 * - WHO DEPENDS ON IT: a `PassChain` registers its passes through this handle and takes them back through it, so a
 *   recipe depends on the registry instead of the whole engine. Hosts that spell it out use `engine.passes()`.
 * - THE ENGINE MAY OUTLIVE OR PRECEDE THE HANDLE, and the handle knows it (a `Lease`): once the engine is destroyed
 *   `isValid()` answers false and every call is a no-op (an add answers null) instead of reaching into freed
 *   memory. That is what lets a chain outlive its engine safely.
 *
 * @note
 * - The engine's flat methods (`addPass`, `removePass`, `passCount`, ...) are shorthands for this handle; they exist
 *   because most callers live next to the engine, not because there is a second registry.
 */
class VN_GRAPHICS_API PassRegistry
{
  public:
    /** @brief Constructs an empty handle that refers to no engine (see isValid). */
    PassRegistry() noexcept = default;

  public:
    /** @brief Gets whether the engine this handle reads is still alive.
     *
     * @return true when the engine is there and the calls below reach it.
     */
    [[nodiscard]] bool isValid() const noexcept;

    /** @brief Registers a declared pass (see PassDesc), with no owner and no role.
     *
     * @param desc Declaration of the pass to register.
     * @return The execution node the declaration became, or null when the engine is gone or that pass was already
     *         registered.
     */
    intrusive_ptr<RenderPass> add(const PassDesc& desc);

    /** @brief Registers a hand-built pass at an explicit order.
     *
     * @param pass  Pass to add (the engine keeps a reference).
     * @param order Execution order (ascending; equal orders keep insertion order).
     * @return The pass the engine registered, or null when the engine is gone or it was already registered.
     */
    intrusive_ptr<RenderPass> add(intrusive_ptr<RenderPass> pass, int order);

    /** @brief Registers a hand-built pass bound to explicit content.
     *
     * @param pass    Pass to add (the engine keeps a reference).
     * @param content Scene the pass renders (a reference is kept).
     * @param order   Execution order (ascending).
     * @return The pass the engine registered, or null when the engine is gone or it was already registered.
     */
    intrusive_ptr<RenderPass> add(intrusive_ptr<RenderPass> pass, intrusive_ptr<Scene> content, int order);

    /** @brief Removes a registered pass and releases the backend state it retained.
     *
     * @param pass Pass to remove (by pointer; null or an unregistered pass is ignored).
     */
    void remove(raw_ptr<RenderPass> pass);

    /** @brief Removes every registered pass (each one as remove). */
    void clear();

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
     * @param pass    Pass registered through this handle (by pointer).
     * @param content New content scene; null clears the binding.
     */
    void bindContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content);

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

    /** @brief Registers a declaration on behalf of @p owner under @p role (the chain's registration).
     *
     * @param desc  Declaration of the pass to register.
     * @param owner Object that owns the pass (the chain).
     * @param role  Name the owner gives this pass (empty = anonymous).
     * @return The registered node, or null when the engine is gone or it was already registered.
     */
    intrusive_ptr<RenderPass> addOwnedBy(const PassDesc& desc, raw_ptr<Object> owner, const String& role);

    /** @brief Removes every pass @p owner registered (null is a no-op). */
    void removeOwnedBy(const Object* owner) noexcept;

    /** @brief Answers the passes @p owner registered, in engine order. */
    [[nodiscard]] std::vector<intrusive_ptr<RenderPass>> passesOwnedBy(const Object* owner) const;

    /** @brief Answers the pass @p owner registered as @p role, or null. */
    [[nodiscard]] raw_ptr<RenderPass> passOwnedBy(const Object* owner, const String& role) const;

  private:
    Lease<RenderEngine> engine_;
};

VN_GRAPHICS_NS_END
