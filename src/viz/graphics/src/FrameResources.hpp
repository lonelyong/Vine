#pragma once

#include <cstddef>
#include <map>
#include <mutex>
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
struct FramePublication {
    raw_ptr<const RenderPass> pass{ nullptr };   // The pass that published (borrowed; its `outputName()` is the key).
    raw_ptr<RenderTarget>     target{ nullptr }; // The target the name serves this frame.
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
 * - TWO WORLDS REACH THESE TABLES, so the lock is this type's own rule (`G1` / `G4`; C23): the COMPILE writes the
 *   frame's tables and walks them for the whole build, while the HOST may publish, withdraw, resolve or read a
 *   transient at any time - including while a frame is being built or answered, because a standing binding is
 *   swapped when the host has something new to offer, not when the frame loop happens to be between frames.
 *   `tables_mutex_` therefore guards every table a host verb can reach (the standing bindings, the episode set,
 *   the frame's publications, the transients); it is held for a lookup, an insert or an eviction, and NEVER across
 *   a device call.
 * - THE FRAME READS THE HOST'S BINDINGS AS A VALUE (`G1` / C23): `beginFrame` copies them, and the compile then
 *   seeds, resolves and checks against that ONE copy - so a publish landing mid-compile answers on the next frame
 *   instead of letting one frame see two answers to "what does this name serve". Those two tables (`frame_bindings_`,
 *   `produced_`) belong to the compile alone and take no lock.
 */
class FrameResources {
  public:
    FrameResources();
    ~FrameResources();

    FrameResources(const FrameResources&)            = delete;
    FrameResources& operator=(const FrameResources&) = delete;

  public:
    /** @brief Adds or replaces a host binding, returning the report a caller should emit for an unusable one.
     *
     * @details
     * - A null target is an EPISODE keyed by the name: the first one returns a message, a repeat returns nothing,
     *   and a later real target (or `unpublish`) ends it. The engine reports the returned sentence, so the
     *   diagnostics route stays in one place.
     * - ANY THREAD, INCLUDING DURING A FRAME: the binding is stored under the table's lock and answers on the NEXT
     *   frame (a frame already building reads the bindings its head took, see frameBindings).
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

    /** @brief Gets the host bindings THIS frame took: the copy `beginFrame` made, as the compile seeds from.
     *
     * @details
     * - The compile's own table, so it takes no lock and returns a reference: it is written once, by `beginFrame`,
     *   and read only by the compile of the frame that took it. A host verb must never read it - `resolve` answers
     *   the host, this answers the frame.
     *
     * @return The bindings this frame started with; a binding with a null target is never stored.
     */
    [[nodiscard]] const std::map<String, intrusive_ptr<RenderTarget>>& frameBindings() const noexcept;

    /** @brief Gets the names the host serves RIGHT NOW, as a copy taken under the lock.
     *
     * @details
     * - For a question asked outside a compile: a plan validated between frames must know the names the host hands in,
     *   or it would refuse a row that reads one - and it must see a binding published a moment ago, which the frame's
     *   head copy does not have yet. A frame's own question is `frameBindings`.
     *
     * @return The names of the standing bindings.
     */
    [[nodiscard]] std::vector<String> hostBindingNames() const;

    /** @brief Opens a frame: takes the host's bindings as THIS frame's copy and clears the per-frame tables.
     *
     * @details
     * - The head is where a frame's inputs are frozen (`G1` / C23): the bindings are copied under the table's lock,
     *   so every later question of this frame (seeding, wiring, resolution) answers the same set even when the host
     *   publishes from another thread while the compile runs.
     */
    void beginFrame();

    /** @brief Seeds "what this frame has produced" with the bindings the frame took.
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

    /** @brief Records a pass' publication, answering the target the name served before it.
     *
     * @details
     * - The LATEST publisher serves the name for the rest of the frame, so recording a second pass' target under one
     *   name replaces the first (and keeps the first pass as the row's subject).
     * - The previous target is returned rather than exposed as a row reference: the collision report is the compile's
     *   (it owns the wording and the episode rule), and the table stays inside this type's lock.
     *
     * @param pass   Pass that published (borrowed; its `outputName()` is the key).
     * @param target Target the name serves.
     * @return The different target the name already served this frame, or null when this is the name's first
     *         publication (or it serves the same target again).
     */
    [[nodiscard]] raw_ptr<RenderTarget> recordPublication(raw_ptr<const RenderPass> pass, raw_ptr<RenderTarget> target);

    /** @brief Resolves a name as the HOST asks it now: a pass' publication, else the live standing binding.
     *
     * @details
     * - ANY THREAD, INCLUDING DURING A FRAME: the tables it walks are locked, so a lookup racing a compile or
     *   another host's publish never tears.
     * - The answer is borrowed: the target is kept alive by the binding or by the pass that published it.
     *
     * @param name Name to resolve.
     * @return The target the name serves now, or null when nothing serves it.
     */
    [[nodiscard]] raw_ptr<RenderTarget> resolve(const String& name) const noexcept;

    /** @brief Resolves a name as THIS FRAME asked it at its head: a pass' publication, else the binding it took.
     *
     * @details
     * - The compile's resolution (see `SessionShape::resolveRowInputs`): the same tables `resolve` walks, except
     *   that the standing bindings are the frame's own copy - so a host publish landing mid-compile cannot answer
     *   a row of a frame whose head, seeds and wiring checks saw the previous binding (C23).
     *
     * @param name Name to resolve.
     * @return The target this frame resolves the name to (borrowed), or null when nothing served it.
     */
    [[nodiscard]] raw_ptr<RenderTarget> resolveForFrame(const String& name) const noexcept;

    /** @brief Marks every transient as undeclared, ready for this compile's declarations. */
    void beginTransientCompile();

    /** @brief Makes sure the transient a declaration asks for exists at the requested shape.
     *
     * @details
     * - Same identity and shape = the same target (two passes share it). A shape or extent change REPLACES the
     *   target, because a built target's attachment set cannot change; the old one is released through @p device
     *   when there is one.
     *
     * @param identity Declared identity (`PassDesc::target.name`).
     * @param shape    Attachment set the declaration asks for.
     * @param width    Extent in device pixels (> 0).
     * @param height   Extent in device pixels (> 0).
     * @param device   Device to tell about a replaced target, or null before a device is set.
     * @return The target the declaration now owns.
     */
    [[nodiscard]] intrusive_ptr<RenderTarget>
    resolveTransient(const String& identity, const TransientShape& shape, int width, int height, RenderDevice* device);

    /** @brief Releases every transient no declaration named this compile, then forgets it.
     *
     * @param device Device to tell about the released targets, or null.
     */
    void retireUndeclared(RenderDevice* device);

    /** @brief Gets how many transient targets are live.
     *
     * @details
     * - ANY THREAD, INCLUDING DURING A FRAME: the count is taken under the table's lock, so it is one instant of a
     *   table the compile is rewriting.
     *
     * @return The count.
     */
    [[nodiscard]] std::size_t transientCount() const noexcept;

    /** @brief Gets the live transient a declaration named.
     *
     * @details
     * - ANY THREAD, INCLUDING DURING A FRAME: the lookup takes the table's lock, and the answer is borrowed - it is
     *   the instance live at that instant, so a compile that replaces or retires it afterwards leaves the handle
     *   describing a target that has been let go (re-ask).
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
    struct TransientTarget {
        String                      identity;              // The declared identity (`PassDesc::target.name`).
        TransientShape              shape;                 // The shape it was built at.
        intrusive_ptr<RenderTarget> target;                // The target itself.
        bool                        declared_now{ false }; // Whether this compile saw the declaration.
    };

    /** @brief Builds the target a transient declaration asks for.
     *
     * @param identity Name to give the target (diagnostics and the plan read it).
     * @param width    Extent in device pixels.
     * @param height   Extent in device pixels.
     * @param shape    Attachment set the declaration asked for.
     * @return The target, configured but not yet built.
     */
    [[nodiscard]] intrusive_ptr<RenderTarget> makeTransientTarget(const String& identity, int width, int height, const TransientShape& shape);

    /** @brief Tells @p device a target is done with, when there is a device to tell.
     *
     * @param target Target to release.
     * @param device Device to tell, or null.
     */
    static void release(raw_ptr<RenderTarget> target, RenderDevice* device);

  private:
    /// Guards every table a host verb can reach (see the class note): the lock is taken per verb, never across a
    /// device call.
    mutable std::mutex tables_mutex_;

    std::map<String, intrusive_ptr<RenderTarget>> host_outputs_;
    std::set<String>                              unpublishable_host_names_;
    std::vector<FramePublication>                 publications_;
    std::vector<TransientTarget>                  transients_;

    /// The host bindings THIS frame took: written once by `beginFrame`, then the compile's alone (no lock).
    std::map<String, intrusive_ptr<RenderTarget>> frame_bindings_;
    /// What this frame has filled so far: the compile's alone (no lock).
    std::vector<raw_ptr<const RenderTarget>>      produced_;
};

} // namespace detail

VN_GRAPHICS_NS_END
