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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
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
        ::vsg::ref_ptr<::vsg::Camera>    camera;     // carries the sub-rect viewport
        ::vsg::ref_ptr<::vsg::View>      view;       // extra View of this target's render graph
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
            ::vsg::ref_ptr<::vsg::RenderPass>  render_pass;      ///< The pass' own colour/depth load-op variant.
            ::vsg::ref_ptr<::vsg::RenderPass>  render_pass_seed; ///< CLEAR variant recorded once for an unseeded LOAD pass.
            ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer;      ///< Framebuffer for @ref render_pass.
            ::vsg::ref_ptr<::vsg::RenderGraph> graph;            ///< Render graph (one per pass).
            /// The pass' explicit pipeline order (setPassOrder). A target's pass
            /// graphs record in this order — the position the pass' content would
            /// have occupied as a View of a single target-wide render pass.
            int         order       = std::numeric_limits<int>::max();
            /// True when this pass preserves (LOADs) depth instead of clearing it.
            bool        load_depth  = false;
            /// True when this pass clears colour at its start; false LOADs the
            /// previous pass' colour (see makeSampleableRenderPass).
            bool        color_clear = true;
            /// This pass' clear colour (its own clear() request).
            ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
            /// True while @ref graph records @ref render_pass_seed (the
            /// one-frame CLEAR that defines a fresh depth image before LOADing).
            bool        seeded      = false;
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
        ::vsg::ref_ptr<::vsg::Node>      depth_share_barrier;
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
        // Depth value a pass of this target clears to (colour targets: 0.0, the
        // reverse-Z far plane; depth-only targets: 1.0). Recorded when the
        // attachments are created, so every pass of the target clears to the
        // value the geometry path expects.
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
        bool        depth_borrow_pending_reported = false;        // ---- content slots (retained Views under graph), keyed by owning pass ----
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
