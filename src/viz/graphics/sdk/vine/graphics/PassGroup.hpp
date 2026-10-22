#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <vector>

#include <vine/Lease.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderPass;
struct PassDesc;

/**
 * @brief A registration handle: the passes added through it are removed when it goes away.
 *
 * @details
 * - THE REGISTRY IS THE ENGINE'S, and it stays that way: the engine owns the one ordered slot table, the order a pass
 *   runs at, its content binding and its resources. A group is a TOKEN over that table - an id the slots carry - so
 *   "which passes does this picture own" has exactly one storage, and asking for them walks the engine's table
 *   instead of a second list.
 * - WHY IT EXISTS. Registering a pass and forgetting to remove it is the defect this type makes impossible: a
 *   recipe (a picture, a preview, a test) puts its passes in a group and the group's destructor is the removal. The
 *   symmetric operation lives with the registration, so nothing has to remember anything.
 * - A ROLE is a short name a group gives one of its passes (a HUD overlay registers its pass as "gizmo"); it is
 *   stored on the slot, so a reader asks the group for a pass by name instead of a picture keeping a field per pass -
 *   adding a third overlay is a registration with a new role, not a new field.
 * - THE ENGINE MAY OUTLIVE OR PRECEDE THE GROUP, and the handle knows it (a Lease): after the engine is destroyed
 *   `isValid()` answers false and the destructor is a no-op - the slot table died with the engine, so there is
 *   nothing left to remove.
 *
 * @note
 * - Move-only: two owners of one registration set would each think they are the one that removes it.
 * - `passes()` returns a SNAPSHOT (a vector of handles). It is a query result, not a storage: the name/order/content
 *   of each pass still comes from the engine.
 */
class VN_GRAPHICS_API PassGroup
{
  public:
    /** @brief Constructs an empty group that refers to no engine (see isValid). */
    PassGroup() noexcept = default;

    /** @brief Takes over @p other's registration set; @p other becomes empty.
     *
     * @param other Group to take the registration from (left empty, so it will not remove anything).
     */
    PassGroup(PassGroup&& other) noexcept;

    /** @brief Takes over @p other's registration set, removing whatever this group still owned.
     *
     * @details
     * - The removal is NOT optional and is why this is hand-written: the defaulted move assignment would overwrite
     *   the id and leak the passes this handle registered - exactly the defect the type exists to prevent.
     *
     * @param other Group to take the registration from (left empty, so it will not remove anything).
     * @return This group.
     */
    PassGroup& operator=(PassGroup&& other) noexcept;

    PassGroup(const PassGroup&) = delete;
    PassGroup& operator=(const PassGroup&) = delete;

    /** @brief Removes every pass this group registered, while its engine is still alive. */
    ~PassGroup();

  public:
    /** @brief Returns whether this group still has an engine to talk to. */
    [[nodiscard]] bool isValid() const noexcept;

    /**
     * @brief Registers a declared pass in this group.
     *
     * @details
     * - The engine turns the declaration into its execution node (see RenderEngine::addPass) and files it under this
     *   group's id; the returned handle is the node, for the caller that wants to change what it does later.
     * - A null answer means the engine is gone, the declaration is empty, or that pass is already registered.
     *
     * @param desc Declaration of the pass to register.
     * @param role Name this group gives the pass, so a reader can ask for it by name instead of holding a handle
     *             (empty for an anonymous pass). Roles are not enforced unique - pass() answers the first match.
     * @return The registered pass, or null (see above).
     */
    intrusive_ptr<RenderPass> add(const PassDesc& desc, const String& role = String());

    /**
     * @brief Answers the passes this group registered, as a snapshot in engine order.
     *
     * @return The handles; empty when the engine is gone or the group registered nothing.
     */
    [[nodiscard]] std::vector<intrusive_ptr<RenderPass>> passes() const;

    /**
     * @brief Looks one of this group's passes up by the role it was registered with.
     *
     * @param role Role the pass was registered under (see add).
     * @return The pass, or null when this group registered no pass with that role.
     */
    [[nodiscard]] raw_ptr<RenderPass> pass(const String& role) const;

  private:
    friend class RenderEngine;

    /** @brief Mints a group handle over @p engine's slot table (only the engine does this).
     *
     * @param engine Engine whose slot table the group's passes are filed in.
     * @param id Group id the slots carry (0 means "no registration", which the engine never mints).
     */
    PassGroup(Lease<RenderEngine> engine, std::uint64_t id) noexcept;

    /** @brief Removes this group's passes and forgets them; a no-op once the engine is gone. */
    void release() noexcept;

  private:
    Lease<RenderEngine> engine_;
    std::uint64_t       id_{ 0 };
};

VN_GRAPHICS_NS_END
