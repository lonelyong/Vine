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

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec4.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/graphics/DepthMode.hpp>
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
    vine::graphics::ShaderPreset        shader_preset{ vine::graphics::ShaderPreset::StandardPhong };
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
// A direct driver that never calls beginPass keeps using this as a plain
// request queue: nothing is dropped until it overwrites it with the next
// set* call (the legacy behaviour).
struct VsgPassRequest
{
    /// The pass announced by beginPass() (null for a direct driver).
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
    ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
    /// Sub-viewport announced by setViewport(), per draw call.
    std::optional<vine::graphics::Viewport> viewport;
    /// Lights announced by setLights(), per draw call. Empty keeps the
    /// slot's current/default lights (RenderBackend::clearLights() drops
    /// them), so an announcement and an empty announcement are equivalent.
    std::vector<const vine::graphics::Light*> lights;
    /// Draw calls (render / drawScreen*) this request served (diagnostic).
    std::size_t draws = 0;
    /// The announced target was released while it was still announced
    /// (RenderBackend::releaseRenderTarget). The queued request is the direct
    /// driver's to manage and survives frames (RenderBackend::beginPass), so this
    /// flag is how a call that would still use the dead pointer knows it cannot be
    /// honoured: such a call must be skipped, not redirected to the window. A
    /// pass-scope driven caller never sees it — beginPass() starts from an empty
    /// request — and setRenderTarget() clears it, so it lives exactly as long as
    /// the announcement it invalidates.
    bool target_released = false;
    /// True once the dead announcement above was reported: the report is an
    /// EPISODE, one per release, so a caller looping on it is not flooded.
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

struct VsgRendererState {
    ::vsg::ref_ptr<::vsg::Window>       window;
    ::vsg::ref_ptr<::vsg::Viewer>       viewer;
    ::vsg::ref_ptr<::vsg::CommandGraph> command_graph;
    // Window-target shader sets shared by its content slots' bridges: one per
    // DepthMode (TestAndWrite / TestOnly / Disabled) so each slot bakes the
    // right depth test/write state. Per-geometry pipelines are compiled per
    // view (vsg compiles per viewID), so every content slot carries its own
    // SceneBridge; off-screen targets bake their own per-size sets (see VsgRenderTargetEntry).
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_on_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_testonly_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_off_shader_set;
    bool                                initialized = false;
    // The device report is logged once, from the first submitted frame: the
    // window's Vulkan device / swapchain only materialises when it is first
    // used, so querying it during initialize() returns nothing.
    bool                                device_reported = false;

    /// The request in progress: the open pass scope, or the direct driver's queue.
    VsgPassRequest request;
    /// True while a beginPass() scope is open (endPass() closes it).
    bool pass_open = false;
    // Passes announced since the last submitted frame (see
    // retireInactivePassSlots): a pass that did not execute this frame is
    // retired (its view detached) rather than left drawing stale content.
    std::set<const vine::graphics::RenderPass*> passes_active_this_frame;
    // STICKY: set once any pass is announced, i.e. this backend is being driven
    // through the engine's pass protocol. It is never cleared, so that a frame
    // in which EVERY pass is disabled (nothing announced) still retires the
    // retained views instead of leaving them on screen. A direct driver that
    // never calls beginPass keeps the legacy keying and is never retired.
    bool pass_protocol_used = false;

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

    // Content-slot VIEWs that gained new/rebuild subtrees this frame. D22
    // incremental compile: submitFrame() recompiles ONLY these views (not the
    // whole scene). The view (not a detached subtree) is the compile unit
    // because vsg assigns the per-View viewID only while traversing the View
    // node — compiling a detached subtree always uses viewID 0 and crashes at
    // record for any other slot's viewID.
    std::vector<::vsg::ref_ptr<::vsg::View>> pending_compile_views;

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

