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
        vine::graphics::ShaderProgram*   program = nullptr; // program the node was built with
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
        // ---- off-screen GPU attachments (window target: unused) ----
        // One image + view per colour attachment (MRT / G-buffer targets carry
        // several sampleable textures; single-colour targets keep one entry).
        std::vector<::vsg::ref_ptr<::vsg::Image>>     color_images;
        std::vector<::vsg::ref_ptr<::vsg::ImageView>> color_views;
        ::vsg::ref_ptr<::vsg::Image>       depth_image;
        ::vsg::ref_ptr<::vsg::ImageView>   depth_view;
        ::vsg::ref_ptr<::vsg::RenderPass>  render_pass;
        ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer;
        ::vsg::ref_ptr<::vsg::RenderGraph> graph; // off-screen: owned here; window: the shared swapchain graph
        // Depth sharing (see RenderTarget::shareDepth): the target whose depth
        // this framebuffer borrows (null = owns its depth) plus the command
        // barrier that makes that depth visible between the two render graphs.
        vine::graphics::RenderTarget* depth_source = nullptr;
        ::vsg::ref_ptr<::vsg::Node>   depth_share_barrier;
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
        // Engine clear() request for this target, persisted so an off-screen
        // graph (re)built later reapplies the last requested clear values
        // instead of a hard-coded default. clear_depth also selects the depth
        // policy of the off-screen pass (CLEAR vs depth-LOAD) at (re)build.
        bool        clear_seen  = false;
        ::vsg::vec4 clear_color{ 0.2f, 0.2f, 0.2f, 1.0f };
        bool        clear_depth = true;
        bool        depth_load  = false; // off-screen pass LOADs (preserves) depth
        // Depth-LOAD targets keep two compatible render passes over the same
        // attachments: the first-frame pass CLEARs the freshly created
        // (UNDEFINED) depth image so its layout becomes
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL, and the steady pass LOADs it every
        // later frame. depth_ready tracks that one-time initialisation.
        ::vsg::ref_ptr<::vsg::RenderPass> render_pass_load;
        bool        depth_ready = false;
        // ---- content slots (retained Views under graph), keyed by owning pass ----
        std::map<SlotKey, ContentSlot> content_slots;
        // ---- PiP views sampling other targets (drawn under this graph) ----
        std::map<SlotKey, ScreenSlot> screen_slots;
        // ---- fullscreen-program views (deferred lighting), keyed by owning pass ----
        std::map<SlotKey, ProgramSlot> program_slots;
    };
    std::map<vine::graphics::RenderTarget*, Target> targets; // nullptr key == window
};

V_VSG_NS_END
