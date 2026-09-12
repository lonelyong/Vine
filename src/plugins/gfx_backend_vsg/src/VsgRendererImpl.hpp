#pragma once

// Internal header: the renderer's PImpl state, split out of VsgRenderer.cpp so
// the implementation can be spread over several translation units (session
// lifecycle, pass protocol + slots, off-screen targets, overlay draws, Vulkan
// object factories) instead of one 3.5k-line file. Not installed: the plugin's
// own sources are its only users, and the session state it describes is private
// to this backend.
//
// The rule for what belongs here is the lifetime rule from Persistent/Impl: a
// window session owns everything that references a vsg::Window / vsg::Device,
// so session-scoped state (window, viewer, command graph, targets and their
// retained slots) can never be left behind by a manual teardown list — it is
// simply replaced wholesale on shutdown()/initialize().

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec4.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/Viewport.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>
#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>
#include <vine/vsg/VsgRenderer.hpp>

#include "VsgPipelineFactory.hpp"

V_VSG_NS_BEGIN

/** @brief Renderer state that outlives any window session.
 *
 * These objects are created once with the renderer and stay valid for its
 * lifetime (MaterialManager contract). Each window session — everything that
 * references a vsg::Window / vsg::Device — lives in @ref Impl and is replaced
 * wholesale on shutdown()/initialize(), so a session-scoped resource can never
 * be forgotten in a manual teardown list.
 */
struct VsgRenderer::Persistent {
    CameraBridge                        cameraBridge;
    VsgMaterialManager                  materialManager;
    vine::graphics::ShaderPreset        shader_preset{ vine::graphics::ShaderPreset::StandardPhong };
    void*                               bound_handle = nullptr;
};

struct VsgRenderer::Impl {
    ::vsg::ref_ptr<::vsg::Window>       window;
    ::vsg::ref_ptr<::vsg::Viewer>       viewer;
    ::vsg::ref_ptr<::vsg::CommandGraph> command_graph;
    // Window-target shader sets shared by its content slots' bridges: one per
    // DepthMode (TestAndWrite / TestOnly / Disabled) so each slot bakes the
    // right depth test/write state. Per-geometry pipelines are compiled per
    // view (vsg compiles per viewID), so every content slot carries its own
    // SceneBridge; off-screen targets bake their own per-size sets (see Target).
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_on_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_testonly_shader_set;
    ::vsg::ref_ptr<::vsg::ShaderSet>    depth_off_shader_set;
    bool                                initialized = false;
    // The device report is logged once, from the first submitted frame: the
    // window's Vulkan device / swapchain only materialises when it is first
    // used, so querying it during initialize() returns nothing.
    bool                                device_reported = false;

    /** @brief Identifies one content slot.
     *
     * Slots are keyed by SlotKey: the pass announced in beginPass() owns its
     * slot, so the retained state follows the pass (its camera / render target
     * may change without orphaning it) and two passes never alias. A direct
     * driver that skips the pass protocol falls back to the historical
     * (camera, explicit pass order) identity.
     */    struct ContentSlot {
        int                           order  = 0;   // explicit pipeline order (stacking)
        vine::graphics::DepthMode     depth_mode = vine::graphics::DepthMode::TestAndWrite;
        bool                          presenting = false; // this slot cleared the target (full-target main pass)
        bool                          headlight_seed = false; // its default light is the headlight (presenting window slot)
        ::vsg::ref_ptr<::vsg::Camera> vsg_camera;
        ::vsg::ref_ptr<::vsg::Group>  root;        // retained content root
        ::vsg::ref_ptr<::vsg::Group>  light_group; // lights under this slot's view
        ::vsg::ref_ptr<::vsg::View>   view;
        SceneBridge                   bridge;      // per-view pipelines (vsg compiles per viewID)
        // D22: true once this slot's (window/framebuffer render pass + view)
        // context has been registered into the viewer's CompileManager pool
        // (incrementalCompileViews()). Each slot is registered once, so the
        // pool gains exactly one context per slot View.
        bool                          compile_context_registered = false;
        // True while this slot's view is DETACHED from its target's graph
        // because the pass did not execute in the last submitted frame (see
        // retireInactivePassSlots): the retained data / pipelines are kept, so
        // re-enabling the pass simply re-attaches the view instead of
        // re-uploading the mesh and recompiling.
        bool                          detached = false;
        bool                          ready = false;
        // True once this slot has reported that its announced lights were all
        // unusable and it therefore keeps the seeded default light (see
        // setGroupLights). Re-armed when usable lights arrive, so each episode
        // reports once instead of every frame.
        bool                          light_fallback_reported = false;
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
    struct PassRequest
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

    /// The request in progress: the open pass scope, or the direct driver's queue.
    PassRequest request;
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

    // Renderer-owned objects whose Vulkan handles a SUBMITTED command buffer may
    // still reference after they were replaced (a pass' render pass / framebuffer
    // swapped for another load-op variant) or dropped (a fullscreen program slot
    // whose source revoked its depth promotion mid-frame). They are parked here
    // and released kRetireRingDepth frame advances later instead of holding the
    // frame with a device-wide idle: the depth and the "advance after the submit"
    // point are the ones the per-slot node ring already established
    // (SceneBridge::retireNode) — a command-buffer slot re-records the frame, it
    // never reuses a recording, kRetireRingDepth frames after it recorded an
    // object.
    //
    // Parking is not a nicety: destroying a VkRenderPass / VkFramebuffer /
    // VkPipeline whose last recording is still pending is undefined behaviour,
    // and the validation layer does not necessarily see it (the recording that
    // references the object may be several frames old).
    std::array<std::vector<::vsg::ref_ptr<::vsg::Object>>, SceneBridge::kRetireRingDepth> retire_ring;
    std::size_t retire_head = 0;
    // Objects released by the ring so far (diagnostic; see
    // VsgRenderer::retiredObjectCount()): a ring that never released would grow
    // without bound, so the policy-churn check asserts it advances.
    std::size_t retired_object_count = 0;
    // Device-wide idles taken so far (diagnostic; see
    // VsgRenderer::deviceWaitCount()). Avoiding them on the frame-assembly paths
    // is what the ring is for, so this is the judge of that change: no
    // policy-changing frame may raise it.
    std::size_t device_wait_count = 0;

    /** @brief Parks an object until every command buffer that could reference
     * it has been re-recorded.
     *
     * @param object Object to release later (null is ignored).
     */
    void retireObject(::vsg::ref_ptr<::vsg::Object> object);

    /** @brief Releases the objects parked kRetireRingDepth frame advances ago.
     *
     * Called once per submitted frame (submitFrame), beside the per-slot node
     * rings.
     */
    void advanceRetireRing();

    /** @brief Stops the device and counts the stop (see device_wait_count).
     *
     * Used by the DESTRUCTIVE teardown paths, which are exactly the ones that drop
     * a bridge's cache: SceneBridge::clearCache() releases the shared object
     * registry, and a pipeline / sampler in it needs no retained node to own it, so
     * those paths cannot be made wait-free by parking the slot's view (measured:
     * vkDestroyPipeline-00765 / vkDestroySampler-01082). Everything else — a
     * variant swap, the promotion cascade, a dropped program slot, an inactive
     * pass' view, a depth-mode state rebuild — PARKS instead (retireObject), and
     * the policy-churn check asserts that the two kinds stay apart.
     */
    void waitForIdle();

    // Content-slot VIEWs that gained new/rebuild subtrees this frame. D22
    // incremental compile: submitFrame() recompiles ONLY these views (not the
    // whole scene). The view (not a detached subtree) is the compile unit
    // because vsg assigns the per-View viewID only while traversing the View
    // node — compiling a detached subtree always uses viewID 0 and crashes at
    // record for any other slot's viewID.
    std::vector<::vsg::ref_ptr<::vsg::View>> pending_compile_views;

    // ---- Output targets: the window (nullptr key) + off-screen (RT* key) ----

    /** @brief One picture-in-picture view sampling another target's colour
     * attachment.
     *
     * Owned by the pass that draws it (see SlotKey); the sampled source and
     * attachment are slot ATTRIBUTES compared each frame, so a pass that
     * switches its input or destination is rebuilt instead of silently
     * sampling the old texture. */
    struct ScreenSlot {
        int                              order      = std::numeric_limits<int>::max(); // stacking order (engine pass order); PiP last by default
        const vine::graphics::RenderTarget* source_target = nullptr; // sampled target the slot was built for
        int                              attachment = 0;             // sampled colour attachment
        ::vsg::ref_ptr<::vsg::Camera>    camera;      // carries the sub-rect viewport
        ::vsg::ref_ptr<::vsg::View>      view;        // extra View of this target's render graph
        ::vsg::ref_ptr<::vsg::ImageView> source_view; // keeps the sampled attachment alive
        int                              source_w = 0;
        int                              source_h = 0;
        int                              dest_w   = 0; // destination surface the node was built for
        int                              dest_h   = 0;
        // See ContentSlot::detached: a retired slot keeps its node / pipeline
        // so re-enabling the pass re-attaches instead of rebuilding.
        bool                             detached = false;
        bool                             ready    = false;
    };

    /** @brief One retained fullscreen-program view sampling another target's
     * colour attachments through a user fragment program (deferred lighting).
     *
     * Owned by the pass that draws it (see SlotKey); rebuilt when the sampled
     * source, its size, the destination size or the program changes. The
     * per-frame push block (view-space lights, see LightPushBlock) is written
     * into @p push_data before each record.
     */
    struct ProgramSlot {
        int                              order      = std::numeric_limits<int>::min(); // stacking order (engine pass order); fullscreen first by default
        const vine::graphics::RenderTarget* source_target = nullptr; // sampled target the slot was built for
        // Whether the source's depth really ended in SHADER_READ_ONLY when this
        // node was built: the `gbuffer_depth` binding is only declared then (a
        // pass of the source that PRESERVES depth revokes the target's
        // promotion, so the image stays an attachment and cannot be sampled).
        // Part of the rebuild identity, so a policy change rebuilds the node.
        bool                             source_depth_sampleable = false;
        // Whether the node really BOUND the source's depth: the binding is only
        // declared when the shader samples it (the ABI gives the depth the
        // binding index the colour count sets), so a colour-only program never
        // puts the depth in its pipeline layout or its descriptor set and
        // records nothing that names the depth's layout. Only a slot that BINDS
        // it has to be dropped when the source revokes the promotion in the
        // same frame (see the promotion cascade in VsgRendererTargets.cpp).
        bool                             binds_source_depth = false;
        ::vsg::ref_ptr<::vsg::Camera>    camera;     // carries the sub-rect viewport
        ::vsg::ref_ptr<::vsg::View>      view;       // extra View of this target's render graph
        // The graph @p view is a child of. Kept so a slot can be taken out of the
        // frame it was built for (the depth-promotion revoke does that without a
        // host call: the slot was built while the promotion still stood).
        ::vsg::ref_ptr<::vsg::Group>     dest_graph;
        ::vsg::ref_ptr<::vsg::Node>      node;       // the fullscreen program drawable
        ::vsg::ref_ptr<::vsg::Data>      push_data;  // per-frame push-constant bytes
        // The program the node was compiled from, HELD (not merely compared):
        // the address is the slot's identity, so a released program replaced at
        // the same address must not read as "unchanged" (the ownership rule
        // SceneBridge's caches follow). Its content revision is part of the
        // rebuild identity too, so editing the program's GLSL in place
        // (ShaderProgram::replaceStages / setStage) rebuilds the node on the
        // next frame — the scene-geometry path keys its compiled state by the
        // revision the same way (D10).
        vine::intrusive_ptr<const vine::graphics::ShaderProgram> program;
        std::uint64_t                    program_revision = 0;
        int                              source_w = 0;
        int                              source_h = 0;
        int                              dest_w   = 0; // destination surface the node was built for
        int                              dest_h   = 0;
        // See ContentSlot::detached.
        bool                             detached = false;
        bool                             ready    = false;
    };

    /** @brief One output target (window = nullptr key, off-screen = RT* key).
     *
     * Unified (C6.4 / C6.5): window and off-screen targets are the SAME
     * shape — a RenderGraph whose children are content-slot Views (per
     * (camera, pass order)) plus optional PiP views (screen_slots). The
     * window target's graph is the shared swapchain graph created in
     * initialize(); each off-screen target owns its
     * own graph + attachments (images / views / render pass / framebuffer)
     * and lazily builds per-size shader sets, so one RT can bake several
     * content slots (different program / content / depth policy) the same
     * way the window does.
     */
    struct Target {
        /** @brief The parts of a RenderTarget's description that shape this
         * target's off-screen attachments and render pass.
         *
         * buildOffscreenTarget bakes all of them into the images, render pass
         * and framebuffer it creates, and a host may change any of them between
         * frames (attachColor / attachDepth / setDepthPromotion) — so a change
         * must rebuild. Comparing one key instead of listing the properties at
         * the rebuild predicate is what keeps a newly supported property from
         * being silently ignored: before this, an attachment added or a depth
         * promotion turned on after the first frame kept the old framebuffer
         * for the life of the target (readColorBuffer then reported the new
         * attachment as "out of range", and the promotion never happened).
         *
         * Size is tracked by the width / height fields below instead of here:
         * the borrow validation reads the built size, and releaseRenderTarget()
         * forces a rebuild by clearing them.
         */
        struct BuildKey {
            int                                                    color_count = 0;
            std::vector<vine::graphics::RenderTarget::ColorFormat> color_formats;
            bool                                                   has_depth = false;
            vine::graphics::RenderTarget::DepthFormat              depth_format{};
            bool                                                   depth_promotion = false;

            /** @brief Builds the key of @p target as it reads right now.
             *
             * @param target Render target to describe.
             * @return The key of the target's current attachment / pass shape.
             */
            [[nodiscard]] static BuildKey of(const vine::graphics::RenderTarget& target)
            {
                BuildKey key;
                key.color_count = target.colorCount();
                key.color_formats.reserve(key.color_count > 0 ? static_cast<std::size_t>(key.color_count) : 0u);
                for (int i = 0; i < key.color_count; ++i) {
                    key.color_formats.push_back(target.colorFormat(i));
                }
                key.has_depth       = target.hasDepth();
                key.depth_format    = target.depthFormat();
                key.depth_promotion = target.depthPromotion();
                return key;
            }

            /** @brief Returns whether this key still describes @p target.
             *
             * The rebuild predicate runs for every pass into this target on
             * every frame, so it compares against the target instead of
             * building a key to compare with: the unchanged case allocates
             * nothing and returns on the first difference.
             *
             * @param target Render target to compare against.
             * @return true when every property this key watches still matches.
             */
            [[nodiscard]] bool matches(const vine::graphics::RenderTarget& target) const
            {
                if (color_count != target.colorCount() || has_depth != target.hasDepth() ||
                    depth_promotion != target.depthPromotion()) {
                    return false;
                }
                if (has_depth && depth_format != target.depthFormat()) {
                    return false;
                }
                for (int i = 0; i < color_count; ++i) {
                    if (color_formats[static_cast<std::size_t>(i)] != target.colorFormat(i)) {
                        return false;
                    }
                }
                return true;
            }
        };

        /** @brief Per-pass GPU objects for one pass under this target (§28).
         *
         * A render pass bakes ONE pair of attachment load-ops, so a pass that
         * clears and a pass that preserves cannot share one: each pass owns its
         * render pass + framebuffer + RenderGraph over the target's SHARED
         * attachments. The graph is a direct child of the command graph and
         * records in the passes' explicit pipeline order (see
         * reconcileOffscreenOrder). The window target is the exception — its
         * render pass is the swapchain's, so every window pass shares the
         * session graph and creates no entry here.
         */
        struct PassObjects {
            ::vsg::ref_ptr<::vsg::RenderPass>  render_pass;          ///< The pass' own colour/depth load-op variant.
            ::vsg::ref_ptr<::vsg::RenderPass>  render_pass_transient; ///< The one-frame variant of a pass whose depth image is in a transitional layout.
            ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer;         ///< Framebuffer for @ref render_pass.
            ::vsg::ref_ptr<::vsg::RenderGraph> graph;               ///< Render graph (one per pass).
            /// The pass' explicit pipeline order (setPassOrder). A target's pass
            /// graphs record in this order — the position the pass' content would
            /// have occupied as a View of a single target-wide render pass.
            int         order       = std::numeric_limits<int>::max();
            /// True when this pass preserves (LOADs) depth instead of clearing it.
            bool        load_depth  = false;
            /// True when this pass clears colour at its start; false LOADs the
            /// previous pass' colour (see planPassVariant). This is the
            /// STEADY variant's load-op, i.e. what @ref want_color_clear asks for.
            bool        color_clear = true;
            /// This pass' own clear requests (what the host asked), the input
            /// @ref color_clear / @ref load_depth were derived from. A run-time
            /// change of THESE is what makes a pass rebuild its variant
            /// (RenderPass::setClearEnabled / setShouldClearDepth): the
            /// materialised load-ops also carry the one-frame bootstrap, and the
            /// same frame builds one pass twice (setupContentSlot + render).
            bool        want_color_clear = false;
            /// True when this pass asked to clear depth (@ref load_depth is its
            /// materialised counterpart: a pass that asked for no clear LOADs).
            bool        want_depth_clear = false;
            /// This pass' clear colour (its own clear() request).
            ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
            /// True while @ref graph records @ref render_pass_transient instead
            /// of @ref render_pass, i.e. for the ONE frame in which this pass
            /// consumes a depth image that is in a transitional layout because
            /// no pass of the target has left it in the attachment layout yet:
            ///
            ///  - the image is still UNDEFINED (nothing defined it: the pass
            ///    records the CLEAR seed variant), or
            ///  - the image was PROMOTED to SHADER_READ_ONLY by an earlier
            ///    frame's pass and this pass is the first of this frame to use
            ///    it (the pass records the LOAD variant that names that layout).
            ///
            /// submitFrame() swaps the graph to the steady variant afterwards,
            /// once that frame has made the depth attachment-optimal again.
            bool        transient   = false;
        };

        // Per-pass objects, keyed by the owning pass (address-stable map so a
        // pass' objects keep their address). Empty until the following steps
        // create them.
        std::map<SlotKey, PassObjects> passes;
        // True once this target's images / views exist: the per-pass model's
        // "target is built" test (there is no single target-level graph).
        bool attachments_built = false;
        // True once this target's depth image has been defined (cleared) at
        // least once, so a LOAD pass may load it — an UNDEFINED image cannot be
        // loaded. Target-level, because the image is shared by every pass.
        bool depth_seeded = false;
        // True once any pass of this target LOADs depth: the depth must then
        // stay in the attachment layout, so NO pass of this target may promote
        // it to a sampleable texture (see the §28 invariants).
        bool any_load_pass = false;

        // ---- Retained slots, by kind ------------------------------------------

        /** @brief The three kinds of retained slot a target can hold.
         *
         * They share their LIFECYCLE (built by a pass, re-checked every frame,
         * detached when the pass stops executing, erased when the pass moves or
         * is released) and live in their own tables because what they RETAIN
         * differs: a content slot owns a SceneBridge, a screen / program slot
         * owns a sampling edge (source_target + attachment). Most walks do not
         * care which kind they are looking at — see forEachSlot() / visitSlot().
         */
        enum class SlotKind { Content, Screen, Program };

        /** @brief Visits every retained slot of this target, whatever its kind.
         *
         * @param visitor Called as visitor(key, slot, kind) for each slot.
         */
        template <class Visitor> void forEachSlot(Visitor&& visitor)
        {
            for (auto& entry : content_slots) {
                visitor(entry.first, entry.second, SlotKind::Content);
            }
            for (auto& entry : screen_slots) {
                visitor(entry.first, entry.second, SlotKind::Screen);
            }
            for (auto& entry : program_slots) {
                visitor(entry.first, entry.second, SlotKind::Program);
            }
        }

        /** @brief Visits the ONE slot this target holds under @p key.
         *
         * The tables are keyed identically (a pass owns one slot of one kind per
         * target), so at most one of them holds @p key.
         *
         * @param key     Slot key to look for.
         * @param visitor Called as visitor(slot, kind) when the key is present.
         * @return true when a slot was visited.
         */
        template <class Visitor> bool visitSlot(const SlotKey& key, Visitor&& visitor)
        {
            if (const auto it = content_slots.find(key); it != content_slots.end()) {
                visitor(it->second, SlotKind::Content);
                return true;
            }
            if (const auto it = screen_slots.find(key); it != screen_slots.end()) {
                visitor(it->second, SlotKind::Screen);
                return true;
            }
            if (const auto it = program_slots.find(key); it != program_slots.end()) {
                visitor(it->second, SlotKind::Program);
                return true;
            }
            return false;
        }

        /** @brief Whether this target holds a slot of ANY kind under @p key.
         *
         * The early-out of every per-pass path: a pass that drew into another
         * target owns nothing here, and the teardown paths must not pay a device
         * wait for it.
         *
         * @param key Slot key to look for.
         * @return true when any of the slot tables holds @p key.
         */
        [[nodiscard]] bool hasSlot(const SlotKey& key) const
        {
            return content_slots.count(key) != 0u || screen_slots.count(key) != 0u || program_slots.count(key) != 0u;
        }

        /** @brief Erases the slot @p kind holds under @p key.
         *
         * @param kind Table the slot came from (see forEachSlot / visitSlot).
         * @param key  Slot key whose entry is erased.
         */
        void eraseSlot(SlotKind kind, const SlotKey& key)
        {
            switch (kind) {
            case SlotKind::Content: content_slots.erase(key); return;
            case SlotKind::Screen: screen_slots.erase(key); return;
            case SlotKind::Program: program_slots.erase(key); return;
            }
        }

        /** @brief The render graph a slot's view records into (the §28 model).
         *
         * The window target has ONE graph (the shared swapchain graph, created
         * with the window); an off-screen target has one per pass. A slot whose
         * pass has no entry yet — or an off-screen target that failed to build —
         * has none.
         *
         * @param owner     Target entry that holds the slot.
         * @param owner_key The key @p owner is registered under (nullptr = window).
         * @param key       Slot key (its owner pass indexes the pass table).
         * @return The graph, or null when the slot has none.
         */
        [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> slotGraph(Target& owner,
                                                                  vine::graphics::RenderTarget* owner_key,
                                                                  const SlotKey& key)
        {
            if (owner_key == nullptr) {
                return owner.graph;
            }
            const auto pass = owner.passes.find(key);
            return pass == owner.passes.end() ? ::vsg::ref_ptr<::vsg::RenderGraph>() : pass->second.graph;
        }

        // ---- off-screen GPU attachments (window target: unused) ----
        // One image + view per colour attachment (MRT / G-buffer targets carry
        // several sampleable textures; single-colour targets keep one entry).
        // The render pass and framebuffer are PER PASS (@ref PassObjects): one
        // render pass bakes ONE pair of attachment load-ops, so a pass that
        // clears and a pass that preserves materialise their own variant over
        // these shared images. Only the window keeps a target-level graph, and
        // only because its render pass is the swapchain's, which cannot be
        // re-created per pass.
        std::vector<::vsg::ref_ptr<::vsg::Image>>     color_images;
        std::vector<::vsg::ref_ptr<::vsg::ImageView>> color_views;
        ::vsg::ref_ptr<::vsg::Image>       depth_image;
        ::vsg::ref_ptr<::vsg::ImageView>   depth_view;
        ::vsg::ref_ptr<::vsg::RenderGraph> graph; // window: the shared swapchain graph (off-screen: see passes)
        // Depth sharing (see RenderTarget::shareDepth): the target whose depth
        // this framebuffer borrows (null = owns its depth) plus the command
        // barrier that makes that depth visible between the two render graphs.
        vine::graphics::RenderTarget*    depth_source = nullptr;
        // The source's depth VIEW this framebuffer was baked with. A source
        // that is rebuilt (size or depth-policy change) replaces its depth
        // image, and the baked framebuffer would go on testing the replaced
        // image — which nobody writes any more — so render() rebuilds this
        // target as soon as the two differ and runs the borrow validation again.
        ::vsg::ref_ptr<::vsg::ImageView> depth_source_view;
        ::vsg::ref_ptr<::vsg::PipelineBarrier> depth_share_barrier;
        // Set when the borrowed source above was RELEASED while still borrowed:
        // its VkImage is gone, so the borrow cannot be honoured and this target
        // builds with its own depth instead (a later shareDepth() with a live
        // source clears the condition by being a different pointer).
        const vine::graphics::RenderTarget* unusable_depth_source = nullptr;
        // Per-size shader sets for off-screen slots (window slots share
        // impl->depth_on / depth_testonly / depth_off shader sets). Built lazily.
        ::vsg::ref_ptr<::vsg::ShaderSet> depth_on_shader_set;
        ::vsg::ref_ptr<::vsg::ShaderSet> depth_testonly_shader_set;
        ::vsg::ref_ptr<::vsg::ShaderSet> depth_off_shader_set;
        int width  = 0; // off-screen logical size
        int height = 0;
        // The attachment / pass shape these attachments were built from (see
        // BuildKey): a change means the images / render pass / framebuffer no
        // longer match the target's description and must be rebuilt.
        BuildKey build_key;
        // Engine clear() request for this target, persisted so a pass graph
        // created later clears to the last requested colour instead of a
        // hard-coded default. A pass' OWN clear request (the open scope) takes
        // precedence; this is the fallback for a pass that never asked.
        bool        clear_seen  = false;
        ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
        // Depth value a pass of this target clears to: the reverse-Z FAR plane
        // (0.0) for every target, colour or depth-only. The depth compare is
        // VK_COMPARE_OP_GREATER, so the buffer must start at the far plane for
        // any fragment to pass — a depth-only target cleared to the near plane
        // would reject everything (see buildOffscreenTarget).
        float       depth_clear_value = 0.0f;
        // True when a pass of this target leaves its depth in
        // SHADER_READ_ONLY_OPTIMAL because it promoted it to a sampled texture:
        // that image can no longer serve as ANOTHER target's depth attachment,
        // so a later shareDepth() of this target cannot be honoured (see the
        // borrow validation in buildOffscreenTarget). Recorded when the
        // ATTACHMENTS are built, from the target's own description
        // (RenderTarget::depthPromotion) — a consumer that borrows this depth is
        // validated in the same frame, before any of this target's passes
        // exists, so recording it only when a pass is created would be too late.
        bool        depth_sampleable = false;
        // True once a pass of this target has CLEARed its colour. A freshly
        // created colour image is UNDEFINED, and a render pass may not LOAD an
        // UNDEFINED image (VUID-VkRenderPassBeginInfo-image-...), so the FIRST
        // pass into a new target always clears it whatever that pass asked for
        // — the colour counterpart of @ref depth_seeded.
        bool        color_seeded = false;
        // True while a requested depth borrow could not be honoured YET because
        // the source had no depth image at build time, and the report for that
        // episode was already emitted. Transient: the borrow is retried as soon
        // as the source exists (see render()'s rebuild predicate) and this flag
        // is cleared when it is honoured, so a source that arrives late is
        // reported once, not every frame.
        bool        depth_borrow_pending_reported = false;

        // ---- content slots (retained Views under graph), keyed by owning pass ----
        std::map<SlotKey, ContentSlot> content_slots;
        // ---- PiP views sampling other targets (drawn under this graph) ----
        std::map<SlotKey, ScreenSlot> screen_slots;
        // ---- fullscreen-program views (deferred lighting), keyed by owning pass ----
        std::map<SlotKey, ProgramSlot> program_slots;

        // The target this entry belongs to. The entry OWNS it, exactly like the
        // content caches own the geometry they are keyed by (see the ownership
        // rule in the design notes): a raw pointer key whose entry does not hold
        // the object can outlive it, and then a NEW target allocated at the same
        // address silently inherits the dead one's attachments — its size, its
        // colour formats and its depth format included. Owning it makes that
        // address unreusable while the entry exists. Engine targets are released
        // through releaseRenderTarget(); a target the host dropped without
        // announcing it is swept in submitFrame().
        vine::intrusive_ptr<vine::graphics::RenderTarget> owner;
    };

    /** @brief Gets the output-target entry for @p target, pinning its address.
     *
     * Every path that touches the target table goes through here, so an entry can
     * never exist without owning the target it is keyed by (see Target::owner).
     *
     * @param target Target key (nullptr = the window).
     * @return The entry, created on first use.
     */
    /** @brief Stops a target's passes being recorded and makes their release safe.
     *
     * The destructive unhook both teardown paths need: a target about to be rebuilt
     * (buildOffscreenTarget) and a target about to be released
     * (releaseRenderTarget). Each pass graph is removed from the command graph, the
     * device is waited on, and every content slot's bridge cache is dropped together
     * with its queued compile view.
     *
     * The wait is REQUIRED here and is the counted one (Impl::waitForIdle), not a
     * park: clearCache() releases the bridge's shared object registry, whose
     * pipelines / samplers the retained nodes do not necessarily keep alive as the
     * only owner — parking the views instead was measured to trip
     * vkDestroyPipeline-00765 / vkDestroySampler-01082 (see the policy-churn notes).
     * Non-destructive paths (a replaced render pass, a dropped program slot) park
     * their objects with Impl::retireObject instead.
     *
     * @param t Target entry whose passes stop being recorded.
     */
    void unhookTargetPasses(Target& t);

    /** @brief Creates the barrier that orders a borrowed depth image's writes before
     * the borrower's pass reads / tests it.
     *
     * A depth borrow (RenderTarget::shareDepth) makes two targets share ONE depth
     * image in the attachment layout. The image is written by the source's passes and
     * then LOADed by the borrower's, so the write must be made visible before the
     * read: reconcileOffscreenOrder() inserts this barrier right after the source's
     * last pass graph (Target::depth_share_barrier).
     *
     * The subresource range covers both aspects when the source's depth format is a
     * COMBINED depth/stencil one (D24 / D32S8 / D16S8): with separateDepthStencilLayouts
     * disabled a barrier may not name only one aspect of such a format
     * (VUID-VkImageMemoryBarrier-image-03320).
     *
     * @param source Target whose depth image is borrowed.
     * @return The barrier, or null when @p source has no depth image to share.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::PipelineBarrier> makeDepthShareBarrier(vine::graphics::RenderTarget* source) const;

    /** @brief Detaches one slot's view from the graph it records into.
     *
     * The single place that knows how a slot stops being recorded: the view is
     * removed from the target's (or the pass') render graph and dropped from the
     * pending incremental-compile queue. Every path that retires, moves or erases
     * a slot goes through it — a slot whose view is left attached keeps drawing.
     *
     * @param owner     Target entry that holds the slot.
     * @param owner_key Key @p owner is registered under (nullptr = window).
     * @param key       Slot key whose graph the view was attached to.
     * @param view      The slot's retained view (null is a no-op).
     */
    void detachSlotView(Target& owner, vine::graphics::RenderTarget* owner_key, const SlotKey& key,
                        const ::vsg::ref_ptr<::vsg::View>& view);

    /** @brief Forgets everything a previous build of a target's attachments produced.
     *
     * The second half of a rebuild (the first is unhookTargetPasses, which stops the old
     * passes being recorded and makes the release safe): every image / view / slot table and
     * every flag the build set goes back to its initial value, while the target's own entry
     * stays. Written as ONE list because it is exactly what a build OWNS — a Target field
     * added later and forgotten here would survive a rebuild as a stale image, a stale
     * "already built" flag or a stale borrow source, and nothing would report it.
     *
     * @param t Target entry being emptied (its key stays registered).
     */
    void resetTargetAttachments(Target& t);

    /** @brief Creates a target's GPU attachments: one colour image + view per attachment, plus
     * its depth (owned or borrowed from an earlier target this frame).
     *
     * The USAGE flags are the reason this lives in one place — they are what makes a colour
     * attachment also usable as a sampled texture (PiP / fullscreen-program sources) or as a
     * blit source (readColorBuffer), and what lets a depth image be copied out
     * (readDepthBuffer): without TRANSFER_SRC the depth cannot even be transitioned to
     * TRANSFER_SRC_OPTIMAL (VUID-VkImageMemoryBarrier-oldLayout-01212).
     *
     * Every view is created through createImageView(), which compiles the Image (creates the
     * VkImage and allocates / binds its memory) AND the ImageView: without it both handles stay
     * VK_NULL_HANDLE and a framebuffer built from them holds corrupt handles, which only shows
     * up as a crash in vkCmdBeginRenderPass.
     *
     * A BORROWED depth (see RenderTarget::shareDepth) attaches the SOURCE's image, so what this
     * target records is which source VIEW its framebuffer was baked with — the source replacing
     * that image invalidates the framebuffer and render() rebuilds this target by comparing the
     * two.
     *
     * @param t        Target entry whose images / views are set (it is being built).
     * @param device   Device that compiles the images and creates the views.
     * @param target   Render target description (attachment count / formats / depth).
     * @param w        Width to create the images with.
     * @param h        Height to create the images with.
     * @param depth_src Source whose depth is borrowed, or null to create this target's own.
     */
    void createTargetAttachments(Target& t, ::vsg::Device* device, const vine::graphics::RenderTarget& target,
                                 uint32_t w, uint32_t h, vine::graphics::RenderTarget* depth_src);

    /** @brief Drops every slot that SAMPLES @p target, which was just (re)built.
     *
     * A rebuild creates FRESH colour views, and a consumer's stale check only watches the
     * source's SIZE — which a same-size rebuild does not change — so a PiP / fullscreen-program
     * slot built against the old views would go on sampling an image nothing draws into any
     * more. Dropping the slot makes its owner's next drawScreenTexture / drawScreenProgram call
     * reattach against the new attachments.
     *
     * Consumers are found by inspecting the slot ATTRIBUTE (source_target), because a slot's
     * key is the pass that OWNS it, not the target it samples.
     *
     * @param target Target whose attachments were just rebuilt (the sampled source).
     */
    void dropConsumersSampling(const vine::graphics::RenderTarget* target);

    /** @brief Records @p commands into a fresh command buffer and waits for it.
     *
     * The readback paths (readColorBuffer / readDepthBuffer) are synchronous by
     * contract: they submit ONE immediate transfer — barriers plus a blit / a copy —
     * and may not return before the GPU is finished with it. Owning the submission
     * here also keeps the do-nothing guards, the queue choice and the (generous)
     * fence timeout in one place, instead of repeating them at every readback.
     *
     * @param commands Command list to record and complete.
     * @return false when the session has no usable device (nothing to submit to).
     */
    [[nodiscard]] bool submitOneShot(const ::vsg::ref_ptr<::vsg::Commands>& commands) const;

    /** @brief Allocates the memory a readback destination has to live in.
     *
     * Host-visible and host-coherent on purpose: the CPU reads the staging buffer
     * (or the LINEAR image a colour readback blits into) the moment the submission
     * above returns.
     *
     * @param device       Device to allocate on.
     * @param requirements Requirements of the resource it will be bound to.
     * @return The allocation; the caller binds it to its resource.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::DeviceMemory> hostVisibleMemory(::vsg::Device* device,
                                                                      const VkMemoryRequirements& requirements) const;

    /** @brief The built target entry a readback reads from, or null.
     *
     * The shared prologue of readColorBuffer / readDepthBuffer: the window session
     * and viewer must exist, the target must have been built (its attachments exist)
     * and have a usable size. It deliberately does NOT stop the device: both callers
     * check the format (and report why) before paying for the wait.
     *
     * @param target Target to read from (null = unsupported).
     * @return The entry, or null when this readback is unsupported.
     */
    [[nodiscard]] const Target* readbackTarget(vine::graphics::RenderTarget* target) const;

    /** @brief The Vulkan-side description of one target's attachment set.
     *
     * Every pass of a target attaches exactly the same images and only differs in
     * its load-ops, so this is computed once per pass build and then passed around:
     * the device the objects are created on, the attachment formats the render
     * pass must declare, and which attachments exist (owned or BORROWED — see
     * RenderTarget::shareDepth).
     */
    struct PassAttachments {
        ::vsg::ref_ptr<::vsg::Device> device;
        std::vector<VkFormat>         color_formats;
        VkFormat                      depth_format = VK_FORMAT_UNDEFINED;
        bool                          has_color    = false;
        bool                          has_depth    = false;
        bool                          borrowed     = false;
    };

    /** @brief Describes the attachment set passes into @p target will attach.
     *
     * @param t      Target entry the pass builds objects for.
     * @param target Render target whose description names the formats.
     * @return The description; @c device is null before the window session exists.
     */
    [[nodiscard]] PassAttachments passAttachments(const Target& t, const vine::graphics::RenderTarget& target) const;

    /** @brief What one pass of a target needs THIS frame (§28, decided in one place).
     *
     * Values only: everything the apply side needs, read at plan time — because applying
     * the plan (revoking a depth promotion, publishing the new objects) is exactly what
     * changes them. @ref current in particular points INTO the target's pass table, so it
     * has to be taken before the pass is published under its key.
     */
    struct PassPlan {
        /// The target's attachment set (owned by buildOffscreenTarget).
        PassAttachments att;
        /// The variant this pass has to record (load-ops, promotion, transient bootstrap).
        detail::PassVariant variant;
        /// The pass' recorded objects, or null when this is a NEW pass.
        const Target::PassObjects* current = nullptr;
        /// Whether the target has a colour / a depth attachment.
        bool has_color = false;
        /// Whether the target has a depth attachment (owned or borrowed).
        bool has_depth = false;
        /// This pass' own clear requests (what the host asked). The variant's materialised
        /// load-ops come from these — never from a materialised comparison (see planPass).
        bool want_color_clear = false;
        bool want_depth_clear = false;
        /// The variant's colour load-op is CLEAR (the pass clears rather than loads).
        bool color_clear = false;
        /// The variant's depth load-op is LOAD (the pass preserves the depth it finds).
        bool depth_load = false;
    };

    /** @brief Decides what one pass records this frame — device-free, no side effects.
     *
     * The whole per-pass decision: the attachment set, the load-op variant, the pass' own
     * clear requests, and whether its depth image still carries the layout a PROMOTING pass
     * left behind.
     *
     * THE LOAD-OP POLICY. A pass' OWN clear request (the open pass scope) decides its
     * load-ops — nothing else. A pass that never asked for a clear must LOAD what an earlier
     * pass left, or the stacked-pass pipelines break: the engine's deferred +
     * forward-composite pipeline stacks fullscreen lighting and the forward transparent
     * content on ONE off-screen target whose passes all set clearEnabled=false, so "clear it
     * anyway" wipes the lit result the next pass was meant to composite over. A pass that DID
     * ask for a clear clears, whatever its siblings asked for. The one exception is the
     * bootstrap: a target whose colour image has never been defined holds an UNDEFINED image,
     * and a render pass may not LOAD an UNDEFINED image, so the first pass into a NEW target
     * clears it ONCE (planPassVariant()'s transient bootstrap variant, swapped for the steady
     * one at the end of the frame like the depth seed) — otherwise the bootstrap would turn
     * into "this pass clears colour for ever", wiping what an earlier pass of the target drew
     * every frame.
     *
     * PROMOTION STATE. A pass that LOADs depth has to name the layout its image really is in,
     * and promotion is the only way the depth ends anywhere but the attachment layout. It is
     * in force while no pass of this target LOADs depth (@ref Target::depth_sampleable — the
     * cascade in passGraph revokes it) and the image is defined (@ref Target::depth_seeded);
     * it can then only have been replaced by a pass that RAN EARLIER in this frame. Passes
     * announce themselves in passes_active_this_frame as they render and record in their
     * explicit order, so a pass of this target that is announced AND ordered before this one
     * has already run (see depthStillPromoted).
     *
     * The variant itself is the device-free planPassVariant(), and the steady-frame guard is
     * passVariantIsStale(), which compares the pass' REQUESTS — never the materialised
     * load-ops: those carry the bootstrap too, and the very same frame builds one pass twice
     * (setupContentSlot() and render()), so a materialised comparison would rebuild the second
     * build into a LOAD against images nothing has defined yet.
     *
     * @param t      Target entry the pass belongs to.
     * @param key    Slot key of the pass.
     * @param target The render target itself (its description names the formats).
     * @return The plan; @ref PassPlan::current is null for a pass that has not recorded yet.
     */
    [[nodiscard]] PassPlan planPass(const Target& t, const SlotKey& key,
                                    const vine::graphics::RenderTarget& target) const;

    /** @brief Creates the render pass + framebuffer for ONE load-op combination.
     *
     * A render pass bakes one pair of attachment load-ops, so a pass that clears and
     * a pass that preserves cannot share one: this is where a variant becomes
     * objects. The pair is returned together because the framebuffer names the
     * attachments the render pass declares — they are replaced together or not at
     * all.
     *
     * @param t                Target entry the pass belongs to.
     * @param att              Its attachment description (see passAttachments).
     * @param pass_color_clear Whether this pass clears (rather than loads) colour.
     * @param depth_load       Whether the depth attachment is loaded, not cleared.
     * @param promote          Whether the pass may leave the depth sampleable.
     * @param depth_initial    Layout the depth image is in when this pass starts. A
     *                         CLEAR pass starts from UNDEFINED whatever its image
     *                         held, so this only matters for a depth-LOAD pass,
     *                         which must name the layout its image really is in.
     * @return The render pass and its framebuffer.
     */
    [[nodiscard]] std::pair<::vsg::ref_ptr<::vsg::RenderPass>, ::vsg::ref_ptr<::vsg::Framebuffer>>
    makePassObjects(const Target& t, const PassAttachments& att, bool pass_color_clear, bool depth_load, bool promote,
                    VkImageLayout depth_initial) const;

    /** @brief Creates the render graph of a NEW pass, with its clear values.
     *
     * One graph per pass (§28): a pass owns the load-ops of its own scope and therefore
     * cannot record into its target's graph. Everything the graph needs is derived from
     * the target's built attachments.
     *
     * The clear values follow the ATTACHMENT ORDER the framebuffer was built with —
     * colour attachments in order, then depth — as VkRenderPassBeginInfo requires:
     * attachment 0 carries THIS pass' clear colour, the extra MRT attachments stay
     * transparent black (their regions stay black until a fragment writes them), and the
     * depth entry carries the target's depth clear value, the reverse-Z far plane for
     * every target (see buildOffscreenTarget).
     *
     * @param t           Target entry whose attachments the graph renders into.
     * @param has_depth   Whether the framebuffer has a depth attachment (its clear value
     *                    is appended last).
     * @param clear_color Colour attachment 0 clears to (the pass' own request).
     * @return The graph, with a null render pass / framebuffer to be set by the caller.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> makePassGraph(const Target& t, bool has_depth,
                                                                  const ::vsg::vec4& clear_color) const;

    /** @brief Reuses a pass' recorded variant when its clear policy did not change.
     *
     * The whole steady-frame cost of a pass: its objects already encode the load-ops
     * its requests ask for, so only the clear VALUE can have changed — two map lookups,
     * no device call, no allocation. A pass that re-requests a clear updates ITS OWN
     * graph and never its siblings': a pass clears to its own request (§28), so one
     * pass' clear must not become another pass' background colour.
     *
     * The caller has already moved the pass to its record position when its explicit
     * pipeline order changed (VsgRenderer::reconcileOffscreenOrder).
     *
     * @param objects           The pass' recorded objects (its variant).
     * @param want_color_clear  Whether the pass asks to clear colour this frame.
     * @param want_depth_clear  Whether the pass asks to clear depth this frame.
     * @param has_color         Whether the target has a colour attachment (clear value 0
     *                          is the COLOUR entry only then: a depth-only target's single
     *                          entry is its DEPTH value, and VkClearValue is a union, so
     *                          writing .color there would clear the depth to a colour's
     *                          bit pattern).
     * @param clear_color       The colour the pass clears to (its own request).
     * @return The pass' graph, or null when the pass changed its clear policy and its
     *         variant has to be rebuilt.
     */
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> reuseSteadyPass(Target::PassObjects& objects, bool want_color_clear,
                                                                   bool want_depth_clear, bool has_color,
                                                                   const ::vsg::vec4& clear_color);

    /** @brief Records a (re)built pass and what it establishes for its target.
     *
     * The pass now exists and will record: its objects are stored under its slot key, the
     * target learns that its attachments have been written (a later pass may LOAD them) and
     * a pass that LOADs depth withdraws the target's depth promotion, because from here on
     * the image ends in the attachment layout (readDepthBuffer and a later borrow
     * validation are told by the same flag).
     *
     * Adding the pass' graph to the command graph and ordering it is the caller's job
     * (VsgRenderer::passGraph): only VsgRenderer owns the command graph.
     *
     * @param t         Target entry that owns the pass.
     * @param key       Slot key of the pass.
     * @param objects   The pass' materialised objects.
     * @param has_color Whether the target has a colour attachment.
     */
    void publishPass(Target& t, const SlotKey& key, const Target::PassObjects& objects, bool has_color);

    /** @brief Whether the target's depth still carries the layout a promoting pass left.
     *
     * A pass that LOADs depth has to name the layout its image really is in, and
     * promotion is the only way the depth ends anywhere but the attachment layout.
     * Promotion is in force while no pass of the target LOADs depth
     * (@ref Target::depth_sampleable — revokeDepthPromotion() withdraws it) and the
     * image is defined (@ref Target::depth_seeded); it can then only have been
     * replaced by a pass that RAN EARLIER in this frame — passes announce
     * themselves in passes_active_this_frame as they render and record in their
     * explicit order, so a pass of the target that is announced AND ordered before
     * this one has already run.
     *
     * @param t       Target entry the pass belongs to.
     * @param current The pass being built (skipped; null when it has no objects yet).
     * @param order   This pass' explicit record order.
     * @return true when the depth is still promoted, so a LOAD must name that layout.
     */
    [[nodiscard]] bool depthStillPromoted(const Target& t, const Target::PassObjects* current, int order) const;

    /** @brief Whether a target's recorded attachments have to be (re)built because of its
     * DEPTH BORROW.
     *
     * Two separate ways a borrowed depth outlives its usefulness, answered together because
     * they mean the same thing to the caller: the framebuffer recorded for this target no
     * longer matches the source it has to test against.
     *
     *  - PENDING: the requested borrow could not be honoured yet (the source had no depth
     *    image when this target was built), so the baked borrow differs from the requested one
     *    and is retried as soon as the source has an image. A source that is permanently
     *    unusable is remembered as such (@ref Target::unusable_depth_source), so this retries
     *    only while the borrow is merely WAITING — a disabled or never-built producer costs one
     *    map lookup per frame, not a rebuild loop.
     *  - STALE: an honoured borrow attaches the source's depth VIEW, and a source that is
     *    rebuilt (a size change, or the depth-policy change this same predicate watches for its
     *    own targets) replaces its depth image. The borrower's framebuffer would go on testing
     *    the replaced image, which nobody writes any more: the borrowed depth silently freezes
     *    while the old image stays alive. Comparing the source's current view against the one
     *    this target was baked with detects that, and the rebuild re-runs the borrow validation
     *    against the new image.
     *
     * @param t          Target entry to inspect.
     * @param target_key The target itself (nullptr = the window, which never borrows).
     * @return true when the caller has to rebuild the target's attachments.
     */
    [[nodiscard]] bool borrowNeedsRebuild(const Target& t, const vine::graphics::RenderTarget* target_key) const;

    /** @brief Re-creates every pass of @p t WITHOUT depth promotion.
     *
     * A pass that LOADs depth must find the image in a layout it named, so no pass
     * of the target may promote it any more: passes that were allowed to promote
     * (the first pass cleared depth and nothing loaded it) are rebuilt without it
     * and the target's @ref Target::depth_sampleable is withdrawn. A depth-only
     * target is exempt — promotion is not a choice there (its depth always ends
     * sampleable), so nothing has to be revoked.
     *
     * The pass being (re)built is skipped: its own variant is decided by the caller,
     * and rebuilding it here would be thrown away — and would hide the layout its
     * image is in (a pass that STOPS promoting is exactly the one whose depth may
     * still carry the promoted layout).
     *
     * @param t                    Target entry being revoked.
     * @param current              The pass being built, skipped (may be null).
     * @param steady_depth_initial Layout a depth-LOAD pass of this target expects.
     */
    void revokeDepthPromotion(Target& t, const Target::PassObjects* current, VkImageLayout steady_depth_initial);

    /** @brief The command graph's record plan for one frame (reconcileOffscreenOrder).
     *
     * The engine can build a target's graph out of dependency order — a producer
     * (re)built after its consumers existed, a consumer wired to a producer built
     * later — and a consumer only samples its source's CURRENT content when the
     * producer's graph is recorded first. So the plan is: which graphs each target
     * records this frame (in the target's own pass order), the targets' CURRENT
     * record order (the stable tie-break seed), and the dependency-valid order the
     * children end up in.
     */
    struct RecordPlan {
        ::vsg::ref_ptr<::vsg::RenderGraph> window_graph;
        std::map<vine::graphics::RenderTarget*, std::vector<::vsg::ref_ptr<::vsg::RenderGraph>>> graphs_of;
        std::vector<vine::graphics::RenderTarget*> present; ///< Targets recorded now, in current child order.
        std::vector<vine::graphics::RenderTarget*> order;   ///< Targets in dependency-valid order (see orderRecordPlan).
    };

    /** @brief Fills @p plan's graph map and current record order (phase 1).
     *
     * Collects, per off-screen target, the pass graphs that must record this frame
     * (skipping RETIRED ones: a pass whose slot is detached records nothing) in the
     * target's explicit pass order, seeding ties from the order the graphs are
     * recorded in right now.
     *
     * @param plan Plan to fill (@ref RecordPlan::window_graph must be set).
     */
    void fillRecordPlan(RecordPlan& plan) const;

    /** @brief Turns @p plan's current order into a dependency-valid one (phase 2).
     *
     * Edges: a target's off-screen SAMPLING sources (its non-detached screen /
     * program slots' source_target) and its DEPTH BORROW source (a borrower's pass
     * LOADs the depth its source writes this frame). The order itself is the pure
     * stableTopologicalOrder(), so unrelated targets keep their relative position.
     *
     * @param plan Plan whose graphs_of / present are filled.
     */
    void orderRecordPlan(RecordPlan& plan) const;

    /** @brief Rewrites the command graph's children from @p plan (phase 3).
     *
     * Each target's graphs in dependency order, the depth-share barrier of every
     * target borrowing THIS one right after its last graph (so the borrower's LOAD
     * sees the writes), and the window swapchain graph last (it may itself sample
     * off-screen targets). Reordering render-graph children only changes the
     * per-frame record order.
     *
     * @param plan Plan whose order / graphs_of are filled.
     */
    void applyRecordPlan(const RecordPlan& plan);

    Target& entryFor(vine::graphics::RenderTarget* target)
    {
        Target& entry = targets[target];
        if (target != nullptr && entry.owner.get() != target) {
            entry.owner = target;
        }
        return entry;
    }

    std::map<vine::graphics::RenderTarget*, Target> targets; // nullptr key == window
};

V_VSG_NS_END
