#pragma once

/**
 * @brief Key of one retained slot (see VsgRenderTargetEntry).
 *
 * The owning pass, or — for a direct driver that skips the pass protocol — the historical
 * fallback identity named by the factories below. Keying by the OWNING PASS is what lets a
 * slot's camera / target change without orphaning it, and keeps two passes from aliasing.
 */

/**
 * @brief One output target's retained state: the window (nullptr key) or an off-screen
 * target (`RenderTarget*` key).
 *
 * `VsgRendererState` holds a table of these, keyed by the engine's own target pointers;
 * everything a target owns — its attachments, its per-pass GPU objects, its retained slots
 * and its depth-borrow bookkeeping — lives here, so the session state keeps only what
 * outlives the targets themselves.
 *
 * All of these types are namespace-scope on purpose: behaviour over them is written as
 * functions (see VsgFramePlan.hpp, and §48 in .ai/design/vsg-pass-lifecycle.md), instead of
 * every new operation becoming another method on the state.
 */

#include <vine/vsg/vsg_global.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/intrusive_ptr.hpp>

#include <vine/vsg/SceneBridge.hpp>

V_VSG_NS_BEGIN
/** @brief Identity of one retained backend slot.
 *
 * Primary identity: @ref owner, the pass that draws the slot (announced
 * via beginPass). Two passes therefore never alias each other even when
 * they share a camera and a pass order, and the retained state follows the
 * pass when its camera / render target / program changes.
 *
 * A direct backend driver that skips the pass protocol (no beginPass, as
 * the device self-test does) keeps the historical identity instead, carried
 * by @ref scope / @ref index: camera + pass order for a content slot,
 * sampled target + attachment for a picture-in-picture slot, sampled target
 * for a fullscreen-program slot. The two identity schemes never mix in one
 * slot map because a pass-scoped key leaves scope / index at their
 * defaults.
 */


struct SlotKey
{
    const vine::graphics::RenderPass* owner = nullptr; ///< Pass that owns the slot, or null (direct driver).
    const void*                       scope = nullptr; ///< Fallback identity: camera / sampled target.
    int                               index = 0;       ///< Fallback identity: pass order / attachment index.

    bool operator<(const SlotKey& o) const noexcept
    {
        if (owner != o.owner) return owner < o.owner;
        if (scope != o.scope) return scope < o.scope;
        return index < o.index;
    }

    /** @brief Key of the slot owned by a pass. */
    static SlotKey ownerPass(const vine::graphics::RenderPass* pass) noexcept
    {
        return SlotKey{ pass, nullptr, 0 };
    }

    /** @brief Fallback key of a content slot: (camera, pass order). */
    static SlotKey cameraOrder(const vine::graphics::Camera* camera, int order) noexcept
    {
        return SlotKey{ nullptr, camera, order };
    }

    /** @brief Fallback key of a PiP screen slot: (sampled target, attachment). */
    static SlotKey sampledTarget(const vine::graphics::RenderTarget* source, int attachment) noexcept
    {
        return SlotKey{ nullptr, source, attachment };
    }

    /** @brief Fallback key of a fullscreen-program slot: its sampled target. */
    static SlotKey sampledTarget(const vine::graphics::RenderTarget* source) noexcept
    {
        return SlotKey{ nullptr, source, 0 };
    }
};

/** @brief Identifies one content slot.
 *
 * Slots are keyed by SlotKey: the pass announced in beginPass() owns its
 * slot, so the retained state follows the pass (its camera / render target
 * may change without orphaning it) and two passes never alias. A direct
 * driver that skips the pass protocol falls back to the historical
 * (camera, explicit pass order) identity.
 */
struct ContentSlot {
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
    // True while this slot has reported that some announced light was unusable
    // — all of them (it therefore keeps the seeded default light, see
    // setGroupLights) or only some (the rest are lit). Re-armed once every
    // announced light is attached again, so each episode reports once instead
    // of every frame (see beginLightsDroppedEpisode).
    bool                          light_fallback_reported = false;
};

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
struct VsgRenderTargetEntry {
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

    /** @brief Const counterpart of forEachSlot, for the diagnostic walks.
     *
     * Needed because the renderer holds its state BY VALUE (no PImpl): a const
     * renderer makes a const entry, and the read-only counters (detachedSlotCount
     * and friends) must still be able to walk the slot tables.
     *
     * @param visitor Called as visitor(key, slot, kind) for each slot.
     */
    template <class Visitor> void forEachSlot(Visitor&& visitor) const
    {
        for (const auto& entry : content_slots) {
            visitor(entry.first, entry.second, SlotKind::Content);
        }
        for (const auto& entry : screen_slots) {
            visitor(entry.first, entry.second, SlotKind::Screen);
        }
        for (const auto& entry : program_slots) {
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
    [[nodiscard]] ::vsg::ref_ptr<::vsg::RenderGraph> slotGraph(VsgRenderTargetEntry& owner,
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
    // Per-size shader sets for off-screen slots (window slots share the
    // renderer's depth_on / depth_test only / depth_off shader sets). Built lazily.
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

    // True while this target has no size, so a build attempt cannot produce attachments and
    // every pass drawing into it draws nothing — and the report for that episode was already
    // emitted. Cleared as soon as a build attempt finds a usable size, so a target the host
    // sizes later is reported again if it loses the size, and a frame-by-frame build attempt
    // on an unsized target says so once instead of every frame (see
    // detail::beginTargetSizeMissingEpisode).
    bool        size_missing_reported = false;

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

V_VSG_NS_END
