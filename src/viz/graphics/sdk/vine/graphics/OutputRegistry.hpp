#pragma once
#include "graphics_global.hpp"

#include <cstddef>

#include <vine/Lease.hpp>
#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

VN_GRAPHICS_NS_BEGIN

class RenderEngine;
class RenderTarget;

/**
 * @brief The engine's named outputs (and the transients it hands out) as a narrow handle.
 *
 * @details
 * - ONE PUBLICATION TABLE: a pass' output is published for the frame it ran in, a host binding lasts until it is
 *   replaced or withdrawn, and a frame's publication wins over a standing binding of the same name. Those rules are
 *   the engine's; this handle only reaches them.
 * - A TRANSIENT is the compile's product, not the build's: `transientTarget()` answers the instance live NOW, and a
 *   handle kept across a resize describes a target the compile has already let go.
 * - The engine may be destroyed first and the handle knows it (a `Lease`): once it is gone every call answers the
 *   empty value instead of reaching into freed memory.
 */
class VN_GRAPHICS_API OutputRegistry {
  public:
    /** @brief Constructs an empty handle that refers to no engine (see isValid). */
    OutputRegistry() noexcept = default;

  public:
    /** @brief Gets whether the engine this handle reads is still alive.
     *
     * @return true when the engine is there and the calls below reach it.
     */
    [[nodiscard]] bool isValid() const noexcept;

    /** @brief Binds a render target to a named output slot until it is replaced or withdrawn.
     *
     * @param name   Slot name consumers resolve against.
     * @param target Target to bind (a reference is kept; null is reported once per name).
     */
    void publish(const String& name, intrusive_ptr<RenderTarget> target);

    /** @brief Looks a published render target up by slot name.
     *
     * @param name Slot name to look up.
     * @return The target published this frame, or the standing binding, or nullptr when nothing serves @p name.
     */
    [[nodiscard]] raw_ptr<RenderTarget> resolve(const String& name) const;

    /** @brief Removes an output slot (a host binding and this frame's publication alike).
     *
     * @param name Slot name to remove (no-op when not published).
     */
    void unpublish(const String& name);

    /** @brief Gets the transient target a declaration asked for, by its declared identity.
     *
     * @param identity Identity a pass declared as its transient (PassDesc::writes_image).
     * @return The instance live now, or null when nothing declares that identity yet.
     */
    [[nodiscard]] raw_ptr<RenderTarget> transientTarget(const String& identity) const;

    /** @brief Gets how many transient targets the current plan hands out.
     *
     * @return The number of DISTINCT live transients (not the frame count).
     */
    [[nodiscard]] std::size_t transientCount() const noexcept;

  private:
    friend class RenderEngine;

    /** @brief Mints a handle over @p engine's publication table (only the engine does this).
     *
     * @param engine Engine whose outputs this handle reads.
     */
    explicit OutputRegistry(Lease<RenderEngine> engine) noexcept;

  private:
    Lease<RenderEngine> engine_;
};

VN_GRAPHICS_NS_END
