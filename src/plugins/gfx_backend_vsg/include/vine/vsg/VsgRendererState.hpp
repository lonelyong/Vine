#pragma once

/**
 * @brief The session state `VsgRenderer` drives its frames from: data, split by LIFETIME.
 *
 * `VsgRenderer` declares no state of its own: it holds one @ref VsgRendererPersistent (the
 * services that outlive every window session) and one @ref VsgRendererState (everything that
 * references a vsg::Window / vsg::Device, replaced wholesale by `shutdown()`), both BY VALUE —
 * there is no d-pointer and no incomplete type anywhere.
 *
 * The split is the point, not a compilation trick: a session-scoped resource can only be left
 * behind by a manual teardown list, and there is no such list to get wrong — the session state
 * is ONE object that is assigned over.
 *
 * WHAT IS NOT HERE (§48 / §50). The concepts its frames are made of live in their own
 * headers, so this type stays the data a frame is driven through instead of the owner of
 * every operation that touches it:
 *
 *   * @ref VsgRenderTargetEntry (VsgRenderTargetEntry.hpp) — one output target: its
 *     attachments, its per-pass GPU objects and its retained slots, plus @ref SlotKey, the key
 *     that indexes them;
 *   * @ref detail::PassPlan / @ref detail::PassAttachments (VsgFramePlan.hpp) — the per-pass
 *     decisions, as plain values;
 *   * the pass materialisation (VsgPassMaterialiser.hpp) — turning a decided pass into its
 *     render pass / framebuffer / render graph;
 *   * the command graph's record order (VsgRecordOrder.hpp) — the plan and its three phases;
 *   * the readback paths (VsgReadback.hpp) — the colour / depth reads a host asks for;
 *   * the retire ring (VsgRetireRing.hpp) — parking replaced GPU objects instead of stopping
 *     the device;
 *   * the diagnostic route (VsgDiagnostics.hpp) — where a failing path reports through.
 *
 * The per-frame protocol is documented in .ai/design/vsg-pass-lifecycle.md (§28 for the
 * per-pass model, §44 / §47 / §48 / §49 / §50 for the state's shape and what left it).
 */

#include <vine/vsg/vsg_global.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec4.h>

#include <vine/vsg/VsgDrawBlockPool.hpp>
#include <vine/vsg/VsgFwd.hpp>
#include <vine/vsg/VsgMeshResourceCache.hpp>
#include <vine/vsg/VsgTextureCache.hpp>

#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Viewport.hpp>

#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgFramePlan.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>
#include <vine/vsg/VsgRenderTargetEntry.hpp>
#include <vine/vsg/VsgRetireRing.hpp>

V_VSG_NS_BEGIN

struct VsgRendererPersistent {
    CameraBridge                        cameraBridge;
    VsgMaterialManager                  materialManager;
    // The program content without its own program is shaded with. NO default on purpose: the backend
    // never invents a shading, so a session that was never handed one draws no program-less content
    // (reported) instead of guessing. RenderEngine supplies forwardProgram() by default.
    vine::intrusive_ptr<const vine::graphics::ShaderProgram> default_content_program;
    void*                               bound_handle = nullptr;
};

// ---- Pass scope (RenderBackend::beginPass / endPass) ----
//
// ONE structure holds everything the engine announced for the pass being
// executed. It used to be a handful of separate pending_* fields that had to
// be reset in step and were read from three different entry points
// (render() and both drawScreen*() calls) — the shape that made "which call
// means what" depend on the call order. Now the request IS the state:
//
//   * scope attributes (pass identity, target, order, depth mode,
//     presenting) stay valid for every draw call of the scope and are
//     dropped by endPass() — a pass that draws twice keeps its stacking
//     position and depth policy for both calls;
//   * per-draw-call attributes (viewport, lights) are consumed by the draw
//     call that follows them (takeViewport / takeLights).
//
// The scope is the ONLY way to drive this backend: a request exists between beginPass() and
// endPass(), and a drawing call that finds none is refused (see VsgRenderer::refuseNoPassAnnounced)
// instead of drawing with state no pass announced. There used to be a second way — a direct driver
// that never opened a scope, which kept the request alive across frames — and every rule that
// existed only to make THAT safe (the (camera, order) / (source) fallback identities, a sticky
// "protocol used" flag, an episode spanning frames) is gone with it.
struct VsgPassRequest
{
    /// The pass announced by beginPass(): the identity of every slot the request draws into
    /// (null while no scope is open — see VsgRenderer::refuseNoPassAnnounced).
    const vine::graphics::RenderPass* pass = nullptr;
    /// Target announced by setRenderTarget() (null = the window).
    vine::graphics::RenderTarget* target = nullptr;
    /// Pipeline order announced by setPassOrder (stacking position).
    int order = 0;
    /// Depth handling announced by setDepthMode(); explicit per pass.
    vine::graphics::DepthMode depth_mode = vine::graphics::DepthMode::TestAndWrite;
    /// Set by clear(): this pass fills the target (the "presenting" pass
    /// that seeds the window's default headlight). Independent of depth.
    bool presenting = false;
    /// Depth-clear request of the clear() call above — a scope attribute
    /// like presenting, so every draw call of the scope keeps it. Each pass
    /// has its OWN render pass, so the request is honoured exactly for the
    /// pass that made it whatever the target's other passes asked for.
    bool clear_depth = true;
    /// Colour the clear() call above asked for. Only consumed by an
    /// off-screen pass (the window clears from the viewer's own record).
    ::vsg::vec4 clear_color{ kDefaultClearColor };
    /// Sub-viewport announced by setViewport(), per draw call.
    std::optional<vine::graphics::Viewport> viewport;
    /// Lights announced by setLights(), per draw call. Empty keeps the
    /// slot's current/default lights (RenderBackend::clearLights() drops
    /// them), so an announcement and an empty announcement are equivalent.
    std::vector<const vine::graphics::Light*> lights;
    // The pass' resolved input targets, in its declaration order (RenderBackend::setPassInputs).
    // A content slot reads them when it builds/updates its state: the shadow map is an input, and
    // the slot is what binds it (see VsgContentSlot).
    std::vector<vine::raw_ptr<vine::graphics::RenderTarget>> inputs;
    /// The announced target was released while this scope was still using it
    /// (RenderBackend::releaseRenderTarget). The pass announced by beginPass() is
    /// borrowed for its scope, so a drawing call that would still use the dead
    /// pointer cannot be honoured: it is skipped rather than redirected to the
    /// window, which would put the content somewhere the caller never asked for.
    /// A new setRenderTarget() clears it (and so does the next scope, which starts
    /// from an empty request).
    bool target_released = false;
    /// True once the dead announcement above was reported: the report is an
    /// EPISODE, and the episode is the rest of THIS scope (beginPass() starts from
    /// an empty request), so a pass that keeps drawing on it is told once.
    /// Cleared together with target_released.
    bool target_release_reported = false;

    /** @brief Consumes a dead target announcement, telling the caller to report it once.
     *
     * The announced target was released while the announcement was still queued
     * (see target_released): the call that would have used it cannot be honoured, so its
     * caller must skip it — drawing into the window instead would put the content
     * somewhere the host never asked for. The refusal is an EPISODE, one report per
     * release: the first refusal says so (@p report), the rest of the episode is silent but
     * still refused, and the next setRenderTarget() (or a new pass scope) re-arms it.
     *
     * @param report Receives whether this is the episode's first refusal, i.e. whether the
     *               caller reports now. Untouched when the announcement is usable.
     * @return true when the announcement is dead and the caller must skip the call.
     */
    [[nodiscard]] bool takeDeadTargetAnnouncement(bool& report)
    {
        if (!target_released) {
            return false;
        }
        report                  = !target_release_reported;
        target_release_reported = true;
        return true;
    }

    /** @brief Consumes the queued sub-viewport.
     *
     * @return The viewport announced for the next draw call, or empty.
     */
    std::optional<vine::graphics::Viewport> takeViewport()
    {
        std::optional<vine::graphics::Viewport> queued = viewport;
        viewport.reset();
        return queued;
    }

    /** @brief Gets the scope attributes as the one value a content slot remembers.
     *
     * The single conversion from "what the pass announced" to "what a slot applies", so the two cannot
     * disagree about which attributes exist (see PassAttributes).
     *
     * @return The depth policy, stacking order and presenting role of this request.
     */
    [[nodiscard]] PassAttributes attributes() const noexcept
    {
        return PassAttributes{ depth_mode, order, presenting };
    }

    /** @brief Gets the slot identity every draw call of this request belongs to.
     *
     * The announced pass, and nothing else: the pass IS the identity of the state this backend
     * retains, so two passes never alias and a slot follows its pass when its camera / target /
     * program changes. Living on the request (rather than being rebuilt at each draw entry point)
     * means the rule has ONE home, and a request with no pass in it is refused before it gets here
     * (see VsgRenderer::refuseNoPassAnnounced).
     *
     * @return The key of the slots this request draws into.
     */
    [[nodiscard]] SlotKey slotKey() const noexcept
    {
        return SlotKey::ownerPass(pass);
    }

    /** @brief Consumes the queued lights.
     *
     * @return The lights announced for the next draw call (empty = keep).
     */
    std::vector<const vine::graphics::Light*> takeLights()
    {
        std::vector<const vine::graphics::Light*> queued = std::move(lights);
        lights.clear();
        return queued;
    }
};

/**
 * @brief One content-slot view queued for the frame's incremental compile, and where it is recorded.
 *
 * The queue has ONE producer (VsgContentSlot::renderContentSlot, the call that built the subtree), so
 * the entry records the slot the view belongs to instead of making the compiler search: it used to
 * walk every target's slot table per queued view looking for a matching view pointer, and to hand
 * the whole frame over to vsg's full compile when the search found nothing (a view that had just
 * been dropped, or one that belongs to no content slot). The record answers that question directly,
 * and "not found" becomes "nothing records this view any more" rather than a whole-scene recompile.
 *
 * It is a SHORTCUT, not a promise: a slot can be dropped between the queue push and the compile, and
 * a new target allocated at the recorded address must not have this view compiled against ITS
 * framebuffer — so the compiler still checks that the recorded slot holds THIS view (see
 * detail::incrementalCompileViews).
 */
struct PendingCompileView
{
    ::vsg::ref_ptr<::vsg::View>   view;          ///< The view to compile (never null).
    vine::graphics::RenderTarget* target = nullptr; ///< Entry that holds the slot (nullptr = the window).
    SlotKey                       slot;          ///< Slot the view is a child of.
};

/**
 * @brief The passes announced since the last submitted frame.
 *
 * A REUSED vector rather than a `std::set`, for the same reason the engine's publication registry is one
 * (RenderEngine::WiringState::outputs_): it holds one entry per announced pass (a handful), it is rebuilt
 * every frame, and the set paid a node allocation per pass per frame for three questions that are all a
 * linear scan of four entries. The de-duplication the set gave for free is what the methods below keep —
 * the RULE lives here, next to the storage, rather than at each call site.
 *
 * WHAT IT MEANS is documented on @ref VsgRendererState::passes_active_this_frame (an event this frame,
 * not the slots' persistent @ref SlotKey::owner).
 */
struct AnnouncedPasses
{
    /** @brief Announces @p pass for this frame (idempotent: a pass announced twice is one entry).
     *
     * @param pass Pass that began a scope, or null (ignored).
     */
    void mark(const vine::graphics::RenderPass* pass)
    {
        if (pass != nullptr && !contains(pass)) {
            entries.push_back(pass);
        }
    }

    /** @brief Gets whether @p pass was announced this frame.
     *
     * @param pass Pass to test, or null.
     * @return true when this frame announced @p pass.
     */
    [[nodiscard]] bool contains(const vine::graphics::RenderPass* pass) const
    {
        return std::find(entries.begin(), entries.end(), pass) != entries.end();
    }

    /** @brief Forgets @p pass: a released pass is not announced any more.
     *
     * @param pass Pass that was released, or null (ignored).
     */
    void drop(const vine::graphics::RenderPass* pass)
    {
        entries.erase(std::remove(entries.begin(), entries.end(), pass), entries.end());
    }

    /** @brief Forgets every announcement: the next frame starts from none. */
    void clear() noexcept
    {
        entries.clear();
    }

  private:
    // The announced passes, in announcement order. Kept across frames so the buffer is reused (see the
    // type's documentation): clear() empties it without giving the memory back.
    std::vector<const vine::graphics::RenderPass*> entries;
};

struct VsgRendererState {
    /** @brief Releases the ref-counted vsg objects this session holds.
     *
     * Declared here and defined in VsgRenderer.cpp, which is what lets the members above name vsg types
     * this header does not include (see VsgFwd.hpp): a `ref_ptr` needs its pointee complete only where it
     * is DESTROYED, and the destructor is the one place that happens. `noexcept` is spelled out for the
     * same reason: the IMPLICIT exception specification would have the compiler instantiate every member's
     * destructor right here, in every translation unit, to find out whether it can throw — which is the
     * cost this arrangement exists to avoid, and the answer is known (releasing a ref_ptr cannot throw).
     */
    ~VsgRendererState() noexcept;

    /** @brief Starts an empty session: no window, no viewer, no target table.
     *
     * Defined out of line with the rest of the special members: a defaulted-in-the-header one would have
     * its exception specification computed HERE, and computing it instantiates every member's destructor
     * (see the destructor's note above).
     */
    VsgRendererState() noexcept;

    /** @brief Takes over @p other's session.
     *
     * Declared alongside the destructor and the move assignment, because a user-declared destructor or move
     * assignment stops the compiler from generating the move operations — and this type is BUILT AND
     * RETURNED BY VALUE (the tests' session fixtures), so it needs them. All three are defined out of line
     * for the reason VsgFwd.hpp gives: releasing ref-counted vsg members needs those types complete in one
     * .cpp rather than in every translation unit that includes this header.
     *
     * @param other State to move from.
     */
    VsgRendererState(VsgRendererState&& other) noexcept;

    /** @brief Replaces this session wholesale with @p other's (see shutdown()).
     *
     * @param other State to move from.
     * @return This state.
     */
    VsgRendererState& operator=(VsgRendererState&& other) noexcept;

    ::vsg::ref_ptr<::vsg::Window>       window;
    ::vsg::ref_ptr<::vsg::Viewer>       viewer;
    ::vsg::ref_ptr<::vsg::CommandGraph> command_graph;
    // The session's texture-resource cache, injected into every content slot's bridge (see
    // SceneBridge::setTextureCache): one texture sampled by several slots is staged ONCE instead of once per
    // slot. Held through a pointer because the cache is not copyable and this state is assigned over
    // wholesale by shutdown() — which is also what drops those resources with the device they belong to.
    std::unique_ptr<VsgTextureCache> texture_cache;
    // The session's MESH-stream cache, injected the same way (see SceneBridge::setMeshResourceCache): the
    // streams a geometry ALIASES from a model buffer are bound through it, so N drawables reading the same
    // mesh share one bind — and therefore one device buffer and one upload. Session-scoped for the same
    // reason: the buffers belong to the session's device.
    std::unique_ptr<VsgMeshResourceCache> mesh_cache;
    // The session's pool of per-draw uniform slots, injected the same way (see
    // SceneBridge::setDrawBlockPool): our forward set reads each drawable's opacity from set 1, and
    // the slots those blocks live in are shared by the whole session, so a scene's drawables own
    // slots in a handful of buffers instead of one buffer and one descriptor set each.
    // Session-scoped because the slots' memory belongs to the session's device.
    // SHARED ownership: the leases the pool hands out (VsgDrawBlockPool::Lease) hold it, so a bridge that
    // outlives this session still returns its slots into live memory instead of writing into a destroyed
    // pool — the state is replaced wholesale by shutdown(), whose member assignment order would otherwise
    // have to keep the pool alive past the target table (measured: it did not, and the corruption was a
    // crash at exit in one run out of three).
    std::shared_ptr<VsgDrawBlockPool> draw_block_pool;
    // The frame's ownership picture: the retained shares of every cache that sweeps this frame.
    // Built at the frame's start and handed to each sync (SceneBridge::syncRenderCommands), then
    // rebuilt just before the frame's end-of-frame sweeps (VsgRenderer::releaseAbandonedContent),
    // so a slot dropped while the frame was open cannot leave it over-counted. It lives here, not
    // in a bridge, because "the app dropped this material" can only be told by a count that covers
    // EVERY slot holding it: with two slots drawing one material, a bridge judging by its own
    // shares sees the other's and waits for it (the P11 mutual wait).
    OwnedShareCounts retained_shares;
    // WHICH compile manager the registrations below belong to. A registration is only valid for the
    // manager that was in place when it was made, and such a manager can be replaced wholesale when a
    // teardown has left it holding contexts nothing can use any more (detail::renewCompileContexts),
    // so the session names the current one as a number that replacement bumps: every registration
    // becomes stale in one step, without walking the slots. A slot keeps the value it registered
    // under and compares (see ContentSlot::compile_manager_generation); 0 there means "never".
    // Starts at 1 so that a fresh slot is never "registered".
    std::uint64_t compile_manager_generation = 1;
    // Registrations this backend has made into the manager named above (see
    // VsgRetentionStats::compile_contexts). vsg 1.1.16 has no API to remove one, so a manager keeps
    // what it has been given until it is replaced -- and a teardown replaces it once at least half of
    // what it holds is a registration whose slot is gone (detail::renewCompileContexts). This stays
    // under twice the slots alive, instead of a count that follows the slots ever CREATED.
    std::size_t compile_context_registrations = 0;
    // Window-target shader sets shared by its content slots' bridges: one per
    // DepthMode (TestAndWrite / TestOnly / Disabled) so each slot bakes the
    // right depth test/write state. Per-geometry pipelines are compiled per
    // view (vsg compiles per viewID), so every content slot carries its own
    // SceneBridge; off-screen targets bake their own per-size sets (see VsgRenderTargetEntry).
    // Whether this session already told the host that it has no default content program at all, so content
    // that names none is not drawn (see VsgContentSlot): once per session, so telling the host once
    // is what makes "you named no program" visible without becoming per-slot noise. Session state,
    // so a re-init tells the new session's host as well.
    bool                                no_default_default_content_program_reported = false;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_on_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_testonly_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_off_shader_set;
    bool                                initialized = false;
    // The device report is logged once, from the first submitted frame: the
    // window's Vulkan device / swapchain only materialises when it is first
    // used, so querying it during initialize() returns nothing.
    bool                                device_reported = false;

    // ---- The frame's commit token (see FrameCommit, VsgDeferredRelease.hpp) ----

    // Minted by beginFrame() and consumed by the one submitFrame(): the deferral rings advance on the
    // committed-frame clock, so this is what makes an advance mean "a frame was committed", and it is
    // why a second swapBuffers() without a new beginFrame() cannot advance them a second time (which
    // would release GPU objects a frame too early, while a submitted command buffer may still name them).
    std::optional<FrameCommit> pending_commit;
    // True once a submit that had no open frame was refused: the refusal is an EPISODE — one report per
    // episode, re-armed by the next beginFrame() — so a host looping on swapBuffers() is not flooded.
    bool submit_without_frame_reported = false;

    /// The request in progress: filled by the open pass scope, dropped by endPass().
    VsgPassRequest request;
    /// True while a beginPass() scope is open (endPass() closes it).
    bool pass_open = false;
    // True once a drawing call that found no announced pass was refused: the refusal is an EPISODE —
    // one report per frame, re-armed by the next beginFrame() — so a host looping on such a call is
    // not flooded (see VsgRenderer::refuseNoPassAnnounced).
    bool scope_refusal_reported = false;
    // Passes announced since the last submitted frame (see
    // retireInactivePassSlots): a pass that did not execute this frame is
    // retired (its view detached) rather than left drawing stale content.
    //
    // Why a separate list of pass pointers next to the slots' own SlotKey::owner — asked
    // once, so the answer is written down: they are two DIFFERENT facts, not two
    // copies of one. `SlotKey::owner` is persistent identity ("which pass owns
    // this slot"); this set is a per-frame EVENT ("which passes were announced
    // this frame"). beginPass() knows the pass but not yet the target, camera or
    // order — those are announced AFTER it — so the slot a pass will end up in
    // cannot be resolved at announcement time, which is the only moment the event
    // happens. Its two readers both go through slots (retireInactivePassSlots
    // asks "was this slot's owner announced?", depthStillPromoted asks the same
    // about an earlier pass of one target), so it is a proxy for a slot fact, and
    // the tempting alternative — a bool on each slot, set when the slot is USED —
    // would not be the same question: it would read "drew this frame" where this
    // asks "was announced this frame", a semantic change hiding inside a
    // refactor. See .ai/design/vsg-pass-lifecycle.md §63 for the survey.
    AnnouncedPasses passes_active_this_frame;

    // Successful off-screen target builds (diagnostic; see
    // VsgRenderer::offscreenBuildCount()).
    std::size_t offscreen_build_count = 0;
    // Successful fullscreen-program slot builds (diagnostic; see
    // VsgRenderer::programSlotBuildCount()). Counted in drawScreenProgram when
    // a slot becomes ready, so a program hot-reload is observable.
    std::size_t program_slot_build_count = 0;

    // ---- Retiring replaced GPU objects without stopping the device ---------

    // Replaced / dropped renderer-owned objects are PARKED for kRetireRingDepth frame
    // advances instead of holding the frame with a device-wide idle. The policy, the ring
    // itself and its two diagnostics live in VsgRetireRing: the state only owns the one ring
    // its frames advance.
    VsgRetireRing retireRing;

    // ---- Content-slot views that gained new/rebuild subtrees this frame ---------
    //
    // D22 incremental compile: submitFrame() recompiles ONLY these views (not the
    // whole scene). The view (not a detached subtree) is the compile unit
    // because vsg assigns the per-View viewID only while traversing the View
    // node — compiling a detached subtree always uses viewID 0 and crashes at
    // record for any other slot's viewID.
    //
    // Each entry says WHERE the view is recorded (see PendingCompileView), so the
    // compiler does not have to search every target's slots for it.
    std::vector<PendingCompileView> pending_compile_views;

    // ---- Output targets: the window (nullptr key) + off-screen (RT* key) ----

    /** @brief Gets the output-target entry for @p target, pinning its address.
     *
     * Every path that touches the target table goes through here, so an entry can
     * never exist without owning the target it is keyed by (see VsgRenderTargetEntry::owner).
     *
     * @param target Target key (nullptr = the window).
     * @return The entry, created on first use.
     */
    VsgRenderTargetEntry& entryFor(vine::graphics::RenderTarget* target)
    {
        VsgRenderTargetEntry& entry = targets[target];
        if (target != nullptr && entry.owner.get() != target) {
            entry.owner = target;
        }
        return entry;
    }

    std::map<vine::graphics::RenderTarget*, VsgRenderTargetEntry> targets; // nullptr key == window
};

V_VSG_NS_END

