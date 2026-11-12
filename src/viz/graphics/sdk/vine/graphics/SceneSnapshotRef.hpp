#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <memory>

VN_GRAPHICS_NS_BEGIN

namespace detail
{
struct SceneSnapshot; // The immutable value itself (src/SceneSnapshot.hpp): no caller names it.
class FrameWorker;    // The engine half that hands a publication to its per-content source (src/FrameWorker.hpp).
} // namespace detail

/**
 * @brief A handle to ONE immutable scene publication: what `Scene::publish()` posted, as a value.
 *
 * @details
 * - WHAT IT IS. A publication is a scene's rows frozen at one moment (see `Scene::publish`). It is immutable, so it
 *   can be read from any thread, and it is the thing a frame draws from - which is why the publication has to travel
 *   as a VALUE rather than as "whatever the scene is now": a frame that lands while the content keeps editing must
 *   draw the rows it was published with.
 * - WHY IT IS OPAQUE. The rows, their chunking and the selector over them are implementation (see
 *   `detail::SceneSnapshot`); a host never reads them, it carries the handle and states which publication a frame
 *   should draw (see `RenderEngine::bindSnapshot`). That keeps the handle cheap to pass and free to change.
 * - WHAT IT DOES NOT OWN. Nothing: it shares the publication with whoever else holds it (the scene keeps the one it
 *   last posted, the engine keeps the one it was bound to, an in-flight frame keeps the one it started with). The
 *   rows leave when the last holder does.
 * - AN EMPTY HANDLE IS A VALUE, NOT AN ERROR: it means "no publication" (a scene that has never published, or a bind
 *   that clears one), and every view then answers with an empty list.
 *
 * @note
 * - `revision()` is the publication's identity and is what tells two publications apart: it starts at 1 for the first
 *   posted snapshot and moves by one per POSTED publication (`Unchanged` and `InsideBatch` answer with the handle
 *   they were called on, so the revision is unchanged with it).
 */
class VN_GRAPHICS_API SceneSnapshotRef {
  public:
    /** @brief Constructs an empty handle (see the class note: it means "no publication"). */
    SceneSnapshotRef() noexcept = default;

  public:
    /**
     * @brief Gets whether this handle names a publication.
     *
     * @return true when the handle came from a `Posted` or `Unchanged` answer, false when it is empty.
     */
    [[nodiscard]] bool valid() const noexcept;

    /**
     * @brief Gets the same answer as @ref valid, for a caller that tests the handle directly.
     *
     * @return true when this handle names a publication.
     */
    [[nodiscard]] explicit operator bool() const noexcept;

    /**
     * @brief Gets the publication's identity (see the class note).
     *
     * @return The revision, or 0 for an empty handle.
     */
    [[nodiscard]] std::uint64_t revision() const noexcept;

  private:
    friend class Scene;
    friend class RenderEngine;
    friend class detail::FrameWorker;

    /**
     * @brief Wraps the value a publication posted (owner side; see the class note).
     *
     * @param value The immutable publication; null builds an empty handle.
     */
    explicit SceneSnapshotRef(std::shared_ptr<const detail::SceneSnapshot> value) noexcept;

    /**
     * @brief Gets the value this handle names, for the engine's bind step.
     *
     * @return The publication, or null for an empty handle.
     */
    [[nodiscard]] const std::shared_ptr<const detail::SceneSnapshot>& published() const noexcept;

  private:
    std::shared_ptr<const detail::SceneSnapshot> value_{}; // The publication; null when empty.
};

VN_GRAPHICS_NS_END
