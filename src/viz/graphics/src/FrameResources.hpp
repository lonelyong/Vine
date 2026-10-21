#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <vector>

#include <vine/String.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderTarget.hpp>

VN_GRAPHICS_NS_BEGIN

class RenderDevice;
class RenderPass;

namespace detail
{

/**
 * @brief One publication a pass made this frame: the name it serves and the target that serves it.
 *
 * @details
 * - The pass is kept (borrowed) rather than its name copied: `RenderPass::outputName()` is stable while the pass
 *   is alive, and a steady frame must not allocate for a question it already answered. The engine owns the pass
 *   for at least as long as the frame, so the pointer is valid for the compile's lifetime.
 */
struct FramePublication
{
    raw_ptr<const RenderPass> pass{ nullptr };   ///< The pass that published (borrowed; its `outputName()` is the key).
    raw_ptr<RenderTarget>     target{ nullptr }; ///< The target the name serves this frame.
};

/**
 * @brief The image bookkeeping a frame needs, separated from the pass registry that drives it.
 *
 * @details
 * - `RenderEngine` owns passes, ordering and the compile. This type owns the three tables that are about IMAGES:
 *   the host's standing bindings (`publish` / `unpublish`), the frame's production/publication tables, and the
 *   transient targets a declaration asks for. Keeping them here is what lets the engine stay "a scheduler plus a
 *   declaration registry" instead of growing a fourth role when a second host-facing axis arrives.
 * - The boundary is deliberately about STORAGE and IDENTITY, not policy: collision reports, episode rules and
 *   "why did this input not resolve" stay in `RenderEngine`, where the diagnostics route and the compile's problem
 *   list live. This type answers "what exists" and "what was produced"; the engine decides what to say about it.
 * - All methods are called from the engine's single frame thread (see `RenderFrameExecutor`'s threading contract),
 *   so no synchronisation is needed.
 */
class FrameResources
{
  public:
    FrameResources();
    ~FrameResources();

    FrameResources(const FrameResources&) = delete;
    FrameResources& operator=(const FrameResources&) = delete;

  public:
    // ---- Host bindings: standing, across frames ----

    /** @brief Adds or replaces a host binding, returning the report a caller should emit for an unusable one.
     *
     * @details
     * - A null target is an EPISODE keyed by the name: the first one returns a message, a repeat returns nothing,
     *   and a later real target (or `unpublish`) ends it. The engine reports the returned sentence, so the
     *   diagnostics route stays in one place.
     *
     * @param name   Name consumers resolve against.
     * @param target Target to bind, or null to report that the name serves nothing.
     * @return The message to report, or an empty string when the binding was stored or the episode was already said.
     */
    String publish(const String& name, intrusive_ptr<RenderTarget> target);

    /** @brief Removes a host binding and re-arms the "cannot serve it" episode for that name.
     *
     * @param name Name to withdraw.
     */
    void unpublish(const String& name);

    /** @brief Gets the host's standing bindings, as the compile seeds a frame from.
     *
     * @return The bindings; a binding with a null target is never stored.
     */
    [[nodiscard]] const std::map<String, intrusive_ptr<RenderTarget>>& hostBindings() const noexcept;

    // ---- This frame's production and publication tables ----

    /** @brief Clears the per-frame tables: one call at the start of a compile. */
    void beginFrame();

    /** @brief Seeds "what this frame has produced" with the host's standing bindings.
     *
     * @details
     * - A host binding is filled by the HOST, so it is available before any pass runs; an object-typed input
     *   addressing one is answered like one addressing a pass' target.
     */
    void seedFromHostBindings();

    /** @brief Gets whether a target has been produced so far this frame.
     *
     * @param target Target to look up.
     * @return true when a pass (or the host) filled it before the current row.
     */
    [[nodiscard]] bool wasProduced(raw_ptr<const RenderTarget> target) const noexcept;

    /** @brief Records that a target is filled, ignoring a duplicate (one target is one production fact).
     *
     * @param target Target that was filled.
     */
    void recordProduced(raw_ptr<const RenderTarget> target);

    /** @brief Finds this frame's publication for @p name, if any.
     *
     * @param name Name a pass published.
     * @return The publication (mutable, so the latest publisher can replace its target), or null.
     */
    [[nodiscard]] FramePublication* findPublication(const String& name) noexcept;

    /** @brief Records a pass' publication for the rest of the frame.
     *
     * @param pass   Pass that published (borrowed; its `outputName()` is the key).
     * @param target Target the name serves.
     */
    void recordPublication(raw_ptr<const RenderPass> pass, raw_ptr<RenderTarget> target);

    /** @brief Resolves a name for this frame: a pass' publication, else the host's standing binding.
     *
     * @param name Name to resolve.
     * @return The target the name serves, or null when nothing produced it.
     */
    [[nodiscard]] raw_ptr<RenderTarget> resolve(const String& name) const noexcept;

    // ---- Transient targets ----

    /** @brief Marks every transient as undeclared, ready for this compile's declarations. */
    void beginTransientCompile();

    /** @brief Makes sure the transient a declaration asks for exists at the requested shape.
     *
     * @details
     * - Same identity and shape = the same target (two passes share it). A shape or extent change REPLACES the
     *   target, because a built target's attachment set cannot change; the old one is released through @p device
     *   when there is one.
     *
     * @param identity Declared identity (`PassDesc::writes_image`).
     * @param shape    Attachment set the declaration asks for.
     * @param width    Extent in device pixels (> 0).
     * @param height   Extent in device pixels (> 0).
     * @param device   Device to tell about a replaced target, or null before a device is set.
     * @return The target the declaration now owns.
     */
    [[nodiscard]] intrusive_ptr<RenderTarget> resolveTransient(const String& identity, const TransientShape& shape,
                                                               int width, int height, RenderDevice* device);

    /** @brief Releases every transient no declaration named this compile, then forgets it.
     *
     * @param device Device to tell about the released targets, or null.
     */
    void retireUndeclared(RenderDevice* device);

    /** @brief Gets how many transient targets are live.
     *
     * @return The count.
     */
    [[nodiscard]] std::size_t transientCount() const noexcept;

    /** @brief Gets the live transient a declaration named.
     *
     * @param identity Declared identity.
     * @return The target, or null when no declaration names it.
     */
    [[nodiscard]] raw_ptr<RenderTarget> transient(const String& identity) const noexcept;

    /** @brief Gets whether this table owns @p target as a transient (the engine uses it to avoid releasing one twice).
     *
     * @param target Target to look up.
     * @return true when the target is one of this table's transients.
     */
    [[nodiscard]] bool owns(raw_ptr<const RenderTarget> target) const noexcept;

    /** @brief Releases every transient and forgets them (the pass list is going away).
     *
     * @param device Device to tell about the released targets, or null.
     */
    void releaseAll(RenderDevice* device);

  private:
    struct TransientTarget
    {
        String                      identity;          ///< The declared identity (`PassDesc::writes_image`).
        TransientShape              shape;             ///< The shape it was built at.
        intrusive_ptr<RenderTarget> target;            ///< The target itself.
        bool                        declared_now{false}; ///< Whether this compile saw the declaration.
    };

    /** @brief Builds the target a transient declaration asks for.
     *
     * @param identity Name to give the target (diagnostics and the plan read it).
     * @param width    Extent in device pixels.
     * @param height   Extent in device pixels.
     * @param shape    Attachment set the declaration asked for.
     * @return The target, configured but not yet built.
     */
    [[nodiscard]] intrusive_ptr<RenderTarget> makeTransientTarget(const String& identity, int width, int height,
                                                                  const TransientShape& shape);

    /** @brief Tells @p device a target is done with, when there is a device to tell.
     *
     * @param target Target to release.
     * @param device Device to tell, or null.
     */
    static void release(raw_ptr<RenderTarget> target, RenderDevice* device);

  private:
    std::map<String, intrusive_ptr<RenderTarget>> host_outputs_;
    std::set<String>                              unpublishable_host_names_;
    std::vector<raw_ptr<const RenderTarget>>      produced_;
    std::vector<FramePublication>                 publications_;
    std::vector<TransientTarget>                  transients_;
};

}  // namespace detail

VN_GRAPHICS_NS_END
