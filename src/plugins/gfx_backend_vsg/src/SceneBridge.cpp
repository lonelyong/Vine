#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/SceneBridgeInternals.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/DrawIndexed.h>
#include <vsg/core/Array.h>
#include <vsg/io/Options.h>
#include <vsg/nodes/Geometry.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/material.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderSet.h>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Node.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

namespace
{


/**
 * @brief Derives the render state a command is drawn with.
 *
 * The command carries the state folded from the scene graph (RenderCommand::
 * renderState). A pass additionally declares how its content treats the
 * target's depth (RenderPass::depthMode): TestAndWrite for opaque scene
 * content, TestOnly for translucent content that tests without writing,
 * Disabled for HUD content drawn on top. That policy fills the depth test /
 * write enable bits of every command that did not author depth itself, while
 * an explicit StateNode depth keeps winning (finer-grained intent over the
 * pass default). Folding it here — rather than into the slot's baked shader
 * set — is what makes the policy reach the pipeline and a run-time change
 * detectable as a state change.
 *
 * @param command           Command whose state to derive.
 * @param content_depth_mode The pass' depth policy (see setContentDepthMode).
 * @return The state the command's pipeline must honour.
 */
vine::graphics::ResolvedRenderState effectiveCommandState(
    const vine::graphics::RenderCommand& command,
    vine::graphics::DepthMode            content_depth_mode)
{
    vine::graphics::ResolvedRenderState state = command.renderState;
    if (!command.depthExplicit) {
        state.depth.test  = content_depth_mode != vine::graphics::DepthMode::Disabled;
        state.depth.write = content_depth_mode == vine::graphics::DepthMode::TestAndWrite;
    }
    return state;
}


}  // namespace

SceneBridge::SceneBridge()
{
    // Share layout / pipeline / descriptor-set content across the bridge's
    // geometry so identical (shader, render state, material) resolve to ONE
    // VkPipeline: the pipeline count follows state variants, not the geometry
    // count, which is what keeps hundreds/thousands of drawables cheap to load
    // and run (see GraphicsPipelineConfigurator::copyTo's SharedObjects path).
    shared_objects_ = ::vsg::SharedObjects::create();
}

SceneBridge::~SceneBridge() = default;

void SceneBridge::setShaderSet(::vsg::ref_ptr<::vsg::ShaderSet> shaderSet)
{
    shader_set_ = shaderSet;
}

void SceneBridge::setMaterialManager(vine::raw_ptr<VsgMaterialManager> manager)
{
    material_manager_ = manager;
}

VsgMaterialManager& SceneBridge::materialManager()
{
    return material_manager_ != nullptr ? *material_manager_ : default_manager_;
}

::vsg::ref_ptr<::vsg::ShaderSet> SceneBridge::baseShaderSet()
{
    if (shader_set_ == nullptr) {
        shader_set_ = ::vsg::createPhongShaderSet();
    }
    return shader_set_;
}


/** @brief Retained vsg node for one drawn geometry. */
struct SceneBridge::Item {
    // Rejection record: true when this geometry's data could not be built at
    // @ref rejected_revision (malformed attributes / out-of-range indices).
    // Kept so the diagnostic prints once per revision instead of every frame,
    // and cleared when the geometry leaves the frame so a fixed geometry is
    // re-evaluated on its next appearance.
    bool rejected = false;
    std::uint64_t rejected_revision = 0;
    // Last translated identity, used to detect geometry/material/state changes.
    // The material and the program are HELD, not merely compared: the address is
    // the identity here, so a released material or program could be replaced at
    // the same address and the comparisons below would then report "nothing
    // changed" while the retained pipeline / descriptor still belongs to the
    // dead one (wrong colours / wrong shader, silently). The cached variants own
    // their keys for the same reason (see OwnedPairCacheEntry).
    vine::intrusive_ptr<vine::graphics::Material> material;
    vine::intrusive_ptr<const vine::graphics::ShaderProgram> program;
    std::uint64_t revision = ~std::uint64_t{0};
    // Last resolved render state the retained pipeline was built with.
    vine::graphics::ResolvedRenderState render_state;
    // Content revision of @ref program the retained pipeline was built with
    // (D10: editing a retained program's GLSL must invalidate it).
    std::uint64_t program_revision = ~std::uint64_t{0};
    // Root of the retained subtree: matrix transform -> state_node -> data_node.
    ::vsg::ref_ptr<::vsg::MatrixTransform> transform;
    // Pipeline/descriptor wrapper (state group) for the current
    // (material, state, program) variant; its child is @ref data_node.
    ::vsg::ref_ptr<::vsg::StateGroup> state_node;
    // Geometry vertex/index draw commands. Kept stable across state-only
    // rebuilds so a material/state/program edit never re-materialises or
    // re-uploads the mesh data.
    ::vsg::ref_ptr<::vsg::Commands> data_node;
    // Per-vertex color array; its alpha carries the effective per-drawable
    // opacity and is rewritten only when the opacity actually changed.
    ::vsg::ref_ptr<::vsg::vec4Array> colors;
    // Forwarded custom vertex channels (locations >= 3) bound after the three
    // canonical arrays, in binding order (see buildGeometryData). Drives the
    // per-layout ShaderSet / variant identity for state-only rebuilds (a
    // program / material edit reuses this without re-uploading the mesh).
    std::vector<VertexChannel> extra_channels;
    // Custom channels the retained state wrapper was built for (the layout
    // identity). Tracked separately so a data rebuild that changes the channel
    // SET (locations >= 3 added/removed live) forces the state wrapper to be
    // rebuilt too — its per-layout shader set / pipeline must follow the bound
    // vertex arrays.
    std::vector<VertexChannel> state_channels;
    // Primitive topology the retained data was built for: it decides whether
    // automatic normals are derived (Triangles) or defaulted (Points / Lines),
    // so a topology change is a DATA change (part of the node's identity).
    vine::graphics::Topology topology = vine::graphics::Topology::Triangles;
    // Cached write state so steady-state frames skip redundant work.
    ::vsg::dmat4 last_matrix;
    bool matrix_valid = false;
    float last_opacity = -1.0f;  // sentinel forces the first write
    // Consecutive frames this geometry was absent (hidden/culled/removed).
    std::uint32_t absent_frames = 0;
};


void SceneBridge::setDiagnosticSink(vine::graphics::DiagnosticSink sink)
{
    diagnostic_sink_ = std::move(sink);
}

std::size_t SceneBridge::diagnosticCount(vine::graphics::DiagnosticCategory category) const noexcept
{
    const auto index = static_cast<std::size_t>(category);
    return index < diagnostic_counts_.size() ? diagnostic_counts_[index] : 0u;
}

void SceneBridge::report(vine::graphics::DiagnosticSeverity severity, vine::graphics::DiagnosticCategory category,
                         const vine::String& message)
{
    ++diagnostic_count_;
    const auto index = static_cast<std::size_t>(category);
    if (index < diagnostic_counts_.size()) {
        ++diagnostic_counts_[index];
    }
    // The sink is the renderer's route in the real backend (it adds the stderr
    // trace, the backend-wide counters and the host's sink), so this bridge does
    // not write out of band itself: one reporting authority, no double traces.
    if (diagnostic_sink_) {
        diagnostic_sink_(vine::graphics::RenderDiagnostic{ severity, category, message });
    }
}

void SceneBridge::retireNode(::vsg::ref_ptr<::vsg::Node> node)
{
    // A Node IS an Object: this bridge parks on the same ring the session parks its replaced
    // render passes / framebuffers on (see VsgRetireRing), so the depth and the advance point
    // have one definition instead of two that had to be kept in step.
    retire_ring_.park(std::move(node));
}

void SceneBridge::advanceRetireRing()
{
    // One advance per SUBMITTED frame: the bucket entered now was filled kRetireRingDepth
    // submits ago, so the command-buffer slot that could have referenced its objects has been
    // re-recorded since (start() waits on the slot's fence before re-recording it) and the GPU
    // no longer executes them.
    retire_ring_.advance();
}

void SceneBridge::clearCache()
{
    cache_.clear();
    program_shader_sets_.clear();
    program_stages_.clear();
    variant_cache_.clear();
    // Forget the shared-object registry (releases the registered pipeline /
    // layout / descriptor-set objects) when the slot's content is released.
    // Retained geometry nodes still hold ref_ptr to the shared objects until
    // they are destroyed, so clearing only drops the registry, never leaves a
    // dangling reference.
    if (shared_objects_ != nullptr) {
        shared_objects_->clear();
    }
    pipeline_variants_ = 0;
    variant_reuses_   = 0;
    program_stage_compiles_ = 0;
    shared_prune_count_     = 0;
    pending_evictions_      = 0;
}

void SceneBridge::setContentDepthMode(vine::graphics::DepthMode mode)
{
    content_depth_mode_ = mode;
}

void SceneBridge::invalidateState()
{
    // Keep the per-geometry DATA (arrays + bind/draw commands, the uploaded
    // mesh) and drop only the STATE wrapper: the next syncRenderCommands()
    // rebuilds every pipeline / descriptor bind while reusing the vertex data.
    // The dropped wrapper is retired rather than destroyed: its pipeline may
    // still be referenced by a submitted command buffer (the caller normally
    // waits first, but parking it here keeps the bridge correct on its own).
    for (auto& entry : cache_) {
        Item* item = entry.second.payload().get();
        if (item == nullptr) {
            continue;
        }
        retireNode(std::move(item->state_node));
        // Force the per-layout rebuild path too: the channel set tracked by the
        // dropped wrapper is meaningless once the wrapper itself is gone.
        item->state_channels.clear();
    }
}

bool SceneBridge::syncRenderCommands(
    const std::vector<vine::graphics::RenderCommand>& commands,
    ::vsg::Group* root,
    std::vector<::vsg::ref_ptr<::vsg::Node>>* created)
{
    if (root == nullptr) {
        return false;
    }
    bool changed = false;
    std::vector<::vsg::ref_ptr<::vsg::Node>> visible;
    visible.reserve(commands.size());
    std::unordered_set<const vine::graphics::Geometry*> seen;
    seen.reserve(commands.size());

    for (const auto& cmd : commands) {
        const auto* geometry = cmd.geometry.get();
        if (geometry == nullptr) {
            continue;
        }
        seen.insert(geometry);

        // A geometry whose data was rejected earlier (malformed attributes /
        // out-of-range indices) is skipped until its data revision changes, so
        // its rejection diagnostic is emitted once per revision instead of
        // spamming the log on every frame while the bad mesh is still drawn.
        Item* item = nullptr;
        auto it = cache_.find(geometry);
        if (it == cache_.end()) {
            auto entry = std::make_unique<Item>();
            item = entry.get();
            // The lookup key is the geometry as given, while the ENTRY holds the
            // owning reference that keeps that address unique (OwnedCacheEntry).
            it = cache_.emplace(
                     geometry,
                     GeometryCacheEntry(vine::intrusive_ptr<const vine::graphics::Geometry>(geometry),
                                        std::move(entry), 0u))
                     .first;
            changed = true;
        }
        item = it->second.payload().get();
        if (item->rejected) {
            if (item->rejected_revision == geometry->revision()) {
                continue;
            }
            item->rejected = false; // data changed: allow a rebuild below
        }

        // Rebuild the retained subtree when any of its inputs changed. The
        // mesh DATA and the STATE (pipeline + descriptor) are decoupled: a
        // revision (vertex/index data) change rebuilds only the data node; a
        // material / resolved-state / program change rebuilds only the state
        // wrapper and reuses the retained data node, so material/state edits
        // never re-materialise or re-upload the mesh.
        const bool had_node = item->transform != nullptr;
        const auto program_rev =
            cmd.program.get() != nullptr ? cmd.program.get()->revision() : std::uint64_t{0};
        // The state the pipeline must honour: the command's folded state plus
        // the pass-level depth policy for content that did not author depth
        // (see effectiveCommandState). Deriving it here means a pass that
        // changes its depth mode is detected as a state change and rebuilt,
        // instead of serving a pipeline built for the previous policy.
        const vine::graphics::ResolvedRenderState state =
            effectiveCommandState(cmd, content_depth_mode_);
        // The DATA identity is the vertex/index payload plus the two draw
        // inputs that change how it is materialised: the primitive topology
        // (drives normal derivation) and, for a mesh that carries an authored
        // loc2 colour, whether the built-in (white opacity carrier) or the
        // custom (authored colour) path binds binding 2.
        const bool has_loc2 =
            geometry->buffer(2) != nullptr && !geometry->buffer(2)->empty();
        const bool data_dirty =
            !had_node || item->revision != geometry->revision() ||
            item->topology != state.topology ||
            (has_loc2 && (item->program.get() == nullptr) != (cmd.program.get() == nullptr));
        const bool state_dirty = !had_node || item->material.get() != cmd.material.get() ||
                                 item->render_state != state ||
                                 item->program.get() != cmd.program.get() ||
                                 item->program_revision != program_rev;
        if (data_dirty || state_dirty) {
            item->revision         = geometry->revision();
            item->topology         = state.topology;
            item->material         = cmd.material;
            item->render_state     = state;
            item->program          = cmd.program;
            item->program_revision = program_rev;
            changed                = true;
        }

        // A data rebuild may have changed the forwarded custom-channel SET
        // (locations >= 3 added/removed), which the state wrapper's per-layout
        // shader set / variant identity depends on. Detect that here so the
        // wrapper is rebuilt even when material / state / program did not
        // change: otherwise a live-added channel would stay unbound (and a
        // removed one leave a stale binding / layout) until some state change.
        bool state_channels_changed = false;
        if (data_dirty) {
            // Fresh vertex data: rebuild the data node; the previous opacity
            // carrier is dropped with it and rewritten on the next frames. The
            // replaced node is parked (its buffers may still be in flight).
            item->extra_channels.clear();
            retireNode(std::move(item->data_node));
            item->data_node = buildGeometryData(geometry, item->program.get() == nullptr,
                                                state.topology, item->colors,
                                                item->extra_channels);
            if (item->data_node == nullptr) {
                // Unsupported shape / malformed vertex data (unusable attribute
                // strides, out-of-range indices, ...): nothing drawable. The
                // rejection is recorded (once per data revision, so the
                // diagnostic is not retried on every frame) and the state
                // wrapper goes with the data it wrapped.
                item->rejected          = true;
                item->rejected_revision = geometry->revision();
                retireNode(std::move(item->state_node));
                item->state_channels.clear();
                item->matrix_valid = false;
                continue;
            }
            state_channels_changed = item->state_channels != item->extra_channels;
            item->matrix_valid     = false;
            item->last_opacity     = -1.0f;
        }

        if (state_dirty || item->state_node == nullptr || state_channels_changed) {
            // The replaced wrapper (and the pipeline it holds) may still be
            // referenced by an in-flight command buffer: park it.
            retireNode(std::move(item->state_node));
            item->state_node = buildStateGroup(item->data_node, item->material.get(),
                                               item->render_state, item->program.get(),
                                               item->extra_channels);
            if (item->state_node == nullptr) {
                cache_.erase(it);
                continue;
            }
            item->state_channels = item->extra_channels;
        }

        // Attach: the wrapper's child is the current data node and the
        // retained transform's child is the current wrapper.
        if (item->state_node->children.empty() ||
            item->state_node->children.front().get() != item->data_node.get()) {
            item->state_node->children.clear();
            item->state_node->addChild(item->data_node);
        }
        if (!had_node) {
            item->transform = ::vsg::MatrixTransform::create();
            item->transform->addChild(item->state_node);
        }
        else if (item->transform->children.empty() ||
                 item->transform->children.front().get() != item->state_node.get()) {
            item->transform->children.clear();
            item->transform->addChild(item->state_node);
        }

        if ((data_dirty || state_dirty) && created != nullptr) {
            // New/rebuild subtrees must be GPU-compiled before recording.
            created->emplace_back(item->transform);
        }

        // Effective opacity (scene x nodes x leaf geometry) rides the
        // per-vertex alpha. Rewriting O(vertices) only when it actually
        // changed keeps the steady-state per-frame cost independent of mesh
        // size, while opacity edits still apply live.
        if (item->colors != nullptr && item->last_opacity != cmd.opacity) {
            const float opacity = cmd.opacity;
            for (auto& color : *item->colors) {
                color.a = opacity;
            }
            // The colour array is DYNAMIC (see buildGeometry): mark it dirty so
            // vsg's per-frame TransferTask re-copies it this frame; unchanged
            // frames issue no transfer.
            item->colors->dirty();
            item->last_opacity = opacity;
        }

        // World-space placement comes from the command stream; the matrix
        // write is skipped when the node did not move this frame.
        const ::vsg::dmat4 world = detail::toVsg(cmd.modelMatrix);
        if (!item->matrix_valid || item->last_matrix != world) {
            item->transform->matrix = world;
            item->last_matrix = world;
            item->matrix_valid = true;
        }
        visible.emplace_back(item->transform);
    }

    changed = evictAbsentItems(seen) || changed;
    publishRetainedChildren(*root, visible, commands);
    releaseAbandonedCaches();
    return changed;
}

bool SceneBridge::evictAbsentItems(const std::unordered_set<const vine::graphics::Geometry*>& seen)
{
    bool changed = false;
    // A geometry missing from the frame is not dropped immediately: hiding a
    // node/drawable or a frustum-culled object must stay cheap (its compiled
    // node is simply detached from the root and reused when it reappears, with
    // no rebuild or recompile). Only a long-running absence — a drawable truly
    // removed from the scene — evicts the retained node.
    constexpr std::uint32_t kAbsentEvictFrames = 600;
    for (auto it = cache_.begin(); it != cache_.end();) {
        Item* item = it->second.payload().get();
        if (seen.count(it->first) != 0) {
            item->absent_frames = 0;
            ++it;
            continue;
        }
        // The geometry left the frame, so its rejection record no longer
        // applies: a fix that bumps the revision is re-evaluated on the next
        // appearance instead of being skipped by a stale record.
        item->rejected = false;
        // The entry owns the geometry, which is what makes the pointer key
        // valid. Once the app itself stops referencing it, nothing can ever
        // look the entry up again, so release it — and the geometry with it —
        // right away instead of pinning both for the whole reuse window.
        if (it->second.abandoned() || ++item->absent_frames > kAbsentEvictFrames) {
            // The retained subtree may still be referenced by an in-flight
            // command buffer, so park it instead of destroying it here.
            retireNode(std::move(item->transform));
            changed = true;
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    return changed;
}

void SceneBridge::publishRetainedChildren(
    ::vsg::Group& root,
    const std::vector<::vsg::ref_ptr<::vsg::Node>>& visible,
    const std::vector<vine::graphics::RenderCommand>& commands)
{
    // Reparent the retained children to match the (already sorted) command
    // stream; a no-op when the order did not change.
    const bool same = [&] {
        if (root.children.size() != visible.size()) {
            return false;
        }
        for (std::size_t i = 0; i < visible.size(); ++i) {
            if (root.children[i] != visible[i]) {
                return false;
            }
        }
        return true;
    }();
    if (!same) {
        root.children.clear();
        for (auto& node : visible) {
            root.children.emplace_back(node);
        }
    }

    // Refresh bound material values so property edits show up live (the
    // descriptor already points at these cached Phong values). Each distinct
    // material is visited once per frame; the MANAGER owns the compare-and-write
    // decision, so a steady frame writes and transfers nothing while an edit
    // lands immediately (VsgMaterialManager::updateMaterial is the single
    // refresh path — before, this loop duplicated it, D19).
    if (!commands.empty()) {
        auto& manager = materialManager();
        std::unordered_set<const vine::graphics::Material*> refreshed;
        refreshed.reserve(commands.size());
        for (const auto& cmd : commands) {
            if (cmd.material == nullptr || !refreshed.insert(cmd.material.get()).second) {
                continue;
            }
            manager.updateMaterial(cmd.material.get());
        }
    }
}

std::size_t SceneBridge::releaseAbandonedCaches()
{
    // Capacity trims happen on insert (the FIFO half of the bargain), which is
    // what bounds the chain of caches sharing one program; this is the prompt
    // half, shared by every program-keyed cache through OwnedCache.hpp so the
    // three of them cannot drift apart.
    const std::size_t erased = eraseAbandoned(program_stages_) + eraseAbandoned(program_shader_sets_) +
                               eraseAbandoned(variant_cache_);
    // A registered variant is HELD BY shared_objects_ (registering is what the
    // table does), so evicting its cache entries freed nothing: the table still
    // referenced the pipeline, layout and descriptor sets, and it only ever grew
    // — the caches stopped bounding memory for as long as the slot lived. vsg's
    // prune() drops exactly the entries nothing else references (this codebase's
    // own useCount() <= 1 rule), so live variants keep theirs through their
    // cached bind commands and the evicted ones go. It runs only on a frame that
    // evicted something — the abandonment sweep above or a capacity trim — which
    // is the only way the table can have gained an unreferenced entry, so its
    // O(entries) cost is paid once per eviction batch, not per frame.
    const bool evicted = erased != 0u || pending_evictions_ != 0u;
    pending_evictions_ = 0;
    if (evicted && shared_objects_ != nullptr) {
        shared_objects_->prune();
        ++shared_prune_count_;
    }
    return erased;
}


V_VSG_NS_END
