#include <vine/vsg/SceneBridge.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
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
#include <vine/graphics/ShaderAbi.hpp>
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

// Deliberately default: the destructor must NOT touch the injected caches (see flushDrawSlots).
SceneBridge::~SceneBridge() = default;

void SceneBridge::setShaderSet(::vsg::ref_ptr<::vsg::ShaderSet> shaderSet)
{
    shader_set_ = shaderSet;
    // Our own forward set is the only one that reads per-drawable values from a draw
    // block (`vine_draw`); a set without one has no per-drawable slot at all, so the
    // slot is only reserved, bound and written for the set that reads it.
    forward_draw_block_ =
        shader_set_ != nullptr && static_cast<bool>(shader_set_->getDescriptorBinding("vine_draw"));
    // A new set re-arms the "I have nothing to shade with" report (see buildStateGroup): a slot
    // given one again must not stay silent if it loses it a second time.
    no_shader_set_reported_ = false;
    // The retained STATE wrappers were built against the OLD set (their pipelines, descriptor sets
    // and attribute bindings are the old set's), so they have to go: the next sync rebuilds them
    // from the new one. The vertex data is untouched — a set change costs pipelines, not uploads
    // (see invalidateState).
    invalidateState();
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
    // Whatever was injected, and NOTHING else. A bridge with no set cannot shade: it reports that
    // once (see buildStateGroup) and draws no content, instead of inventing a default — the set
    // carries the declarations, the attribute locations and the light source a slot's content is
    // drawn with, so guessing one is guessing what the picture means.
    return shader_set_;
}

void SceneBridge::setTextureAnisotropy(float device_limit)
{
    // Through the accessor, so this always reaches the very cache the samplers are built from.
    textureCache().setMaxAnisotropy(device_limit);
}

std::size_t SceneBridge::textureCount() const noexcept
{
    // The same choice textureCache() makes, spelled with the members so a const
    // accessor does not need a const overload of the accessor itself.
    return (texture_cache_ != nullptr ? *texture_cache_ : default_texture_cache_).count();
}

void SceneBridge::setTextureCache(vine::raw_ptr<VsgTextureCache> cache)
{
    texture_cache_ = cache;
}

void SceneBridge::setMeshResourceCache(vine::raw_ptr<VsgMeshResourceCache> cache)
{
    mesh_cache_ = cache;
}

void SceneBridge::setLightsData(::vsg::ref_ptr<::vsg::Data> data)
{
    // Not a cache: the block BELONGS to the slot, which refreshes it every frame,
    // so the bridge only holds a reference for the descriptor sets it builds.
    lights_data_ = std::move(data);
}

void SceneBridge::setShadowMap(::vsg::ref_ptr<::vsg::ImageInfo> map, bool declared)
{
    shadow_map_      = std::move(map);
    shadow_declared_ = declared;
}

void SceneBridge::setShadowData(::vsg::ref_ptr<::vsg::Data> data)
{
    // Same contract as the lights block: the slot owns it and rewrites it per frame.
    shadow_data_ = std::move(data);
}

VsgMeshResourceCache& SceneBridge::meshResources()
{
    return mesh_cache_ != nullptr ? *mesh_cache_ : default_mesh_cache_;
}

void SceneBridge::setDrawBlockPool(vine::raw_ptr<VsgDrawBlockPool> pool)
{
    draw_block_pool_ = pool;
}

VsgDrawBlockPool* SceneBridge::drawBlockPool()
{
    return draw_block_pool_;
}

void SceneBridge::releaseDrawSlot(VsgDrawBlockPool::Slot slot)
{
    if (draw_block_pool_ == nullptr || !slot.valid()) {
        return;
    }
    // Deferred by one ring depth: the frames in flight may still bind this slot's offset,
    // so it must not be handed to another drawable until the retire ring has advanced past
    // them (the same rule retireNode follows for the nodes that bound it).
    pending_draw_slots_.push_back(PendingDrawSlot{ slot, kDrawSlotRetireFrames });
}

void SceneBridge::flushDrawSlots()
{
    if (draw_block_pool_ == nullptr || pending_draw_slots_.empty()) {
        pending_draw_slots_.clear();
        return;
    }
    const auto remaining = std::remove_if(pending_draw_slots_.begin(), pending_draw_slots_.end(),
                                          [this](PendingDrawSlot& pending) {
                                              if (pending.frames_remaining > 0u) {
                                                  --pending.frames_remaining;
                                                  return false;
                                              }
                                              draw_block_pool_->release(pending.slot);
                                              return true;
                                          });
    pending_draw_slots_.erase(remaining, pending_draw_slots_.end());
}

VsgTextureCache& SceneBridge::textureCache()
{
    return texture_cache_ != nullptr ? *texture_cache_ : default_texture_cache_;
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
    // The texture the material samples, and the content revision it was translated at. Both are needed:
    // the pointer catches "a different texture", the revision catches "the same texture, re-filled" —
    // which a pointer cannot see, and which would otherwise keep sampling the previous upload.
    vine::intrusive_ptr<const vine::graphics::Texture> texture;
    std::uint64_t                                      texture_revision = ~std::uint64_t{0};
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
    // This drawable's slot in the session's per-draw block pool (see setDrawBlockPool), or an
    // invalid slot when the bridge has no pool or the reserve failed. The slot's VALUES are
    // what a translucent drawable costs per frame — four floats written in place instead of a
    // pass over its vertices — and the slot's OFFSET is what its state wrapper binds.
    VsgDrawBlockPool::Slot draw_slot;
    // Identity of the streams the retained data node was built from, per vertex channel and for the index
    // stream (see ChannelKey). A data revision whose VERTEX channels are all unchanged needs only the index
    // stream replaced: a rebuild would re-materialise — and re-upload — every channel with it.
    std::vector<ChannelKey> channel_keys;
    ChannelKey              index_key;
    // The retains of the built data node: one vertex bind per canonical channel and the index bind, so an
    // edit whose shape did not change can refresh the ONE stream that changed (see RetainedBinds).
    RetainedBinds binds;
    // Forwarded custom vertex channels (locations >= 3, except the canonical
    // texcoord location) bound after the canonical prefix, in binding order (see
    // buildGeometryData). Drives the
    // per-layout ShaderSet / variant identity for state-only rebuilds (a
    // program / material edit reuses this without re-uploading the mesh).
    std::vector<VertexChannel> extra_channels;
    // The channels this geometry's data builder DERIVES (white colour fallback, zero UVs, normals derived
    // from the positions): the rebuild reuses them when the inputs they were derived from did not change,
    // so an unrelated edit does not pay a pass over the vertices for them again (see DerivedChannels).
    DerivedChannels derived;
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
    // Last opacity written into @ref draw_slot (the sentinel forces the first write).
    float last_slot_opacity = -1.0f;
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

std::vector<SceneBridge::ChannelKey> SceneBridge::channelKeysOf(const vine::graphics::Geometry& geometry)
{
    std::vector<ChannelKey> keys;
    for (const std::uint32_t location : geometry.bufferLocations()) {
        const vine::graphics::AttributeChannel* const channel = geometry.buffer(location);
        if (channel == nullptr) {
            continue;
        }
        ChannelKey key;
        key.location   = location;
        key.components = channel->components;
        key.buffer     = channel->values.get();
        key.revision   = channel->values != nullptr ? channel->values->revision() : 0u;
        // The channel's OWN range, not the buffer's: an arena holds several geometries' vertices, so the
        // slice is part of what identifies this stream (see AttributeChannel::offset).
        key.offset     = channel->offset;
        key.count      = channel->floatCount();
        keys.push_back(key);
    }
    return keys;
}

bool SceneBridge::streamsMatch(const std::vector<ChannelKey>& before, const std::vector<ChannelKey>& after)
{
    if (before.size() != after.size()) {
        return false; // a channel appeared or disappeared: not the same set of streams
    }
    for (const ChannelKey& key : before) {
        const ChannelKey* const current = keyAt(after, key.location);
        if (current == nullptr || !(*current == key)) {
            return false;
        }
    }
    return true;
}

bool SceneBridge::shapesMatch(const std::vector<ChannelKey>& before, const std::vector<ChannelKey>& after)
{
    if (before.size() != after.size()) {
        return false; // a channel added or removed changes the layout, not just the bytes
    }
    for (const ChannelKey& key : before) {
        const ChannelKey* const current = keyAt(after, key.location);
        if (current == nullptr || current->components != key.components || current->count != key.count) {
            return false;
        }
    }
    return true;
}

const SceneBridge::ChannelKey* SceneBridge::keyAt(const std::vector<ChannelKey>& keys, std::uint32_t location)
{
    for (const ChannelKey& key : keys) {
        if (key.location == location) {
            return &key;
        }
    }
    return nullptr;
}

SceneBridge::ChannelKey SceneBridge::indexKeyOf(const vine::graphics::Geometry& geometry)
{
    ChannelKey key;
    const auto indices = geometry.indicesBuffer();
    if (indices == nullptr) {
        return key; // null buffer == "no index stream", which is not the same as "unchanged indices"
    }
    key.location   = 0u;
    key.components = 1u;
    key.buffer     = indices.get();
    key.revision   = indices->revision();
    // The DRAWN range, not the buffer's: an index arena holds several geometries' indices (see
    // Geometry::setIndices), and a geometry that draws a different span draws different triangles.
    key.offset     = geometry.firstIndex();
    key.count      = geometry.indexCount();
    return key;
}

VsgMeshResourceCache::ChannelKey SceneBridge::indexBindKeyOf(const vine::graphics::Geometry& geometry)
{
    VsgMeshResourceCache::ChannelKey key;
    const auto                      indices = geometry.indicesBuffer();
    if (indices == nullptr) {
        return key;
    }
    key.binding  = 0u;  // the index binds are their own map, so they cannot collide with a vertex binding
    key.buffer   = indices.get();
    key.revision = indices->revision();
    key.count    = indices->size();
    return key;
}

::vsg::ref_ptr<::vsg::uintArray> SceneBridge::boundIndexArray(const vine::graphics::Geometry& geometry)
{
    const auto indices = geometry.indicesBuffer();
    if (indices == nullptr) {
        return {};
    }
    return detail::aliasArray<::vsg::uintArray, std::uint32_t>(indices, indices->size());
}

/**
 * @brief Puts @p replacement where the retained bind command sits in its own command list.
 *
 * Refreshing a SHARED stream cannot re-point the bind it was built with: that command belongs to every
 * geometry reading the same bytes, so the new one goes in at the SAME child slot instead (see
 * RetainedBinds). Keeping the slot is what preserves the order the builder wrote — the order the shader
 * contract's binding numbers were checked against.
 *
 * A bind that is not in a list (nothing to swap: a cached bind served for a stream no drawable bound, say)
 * is simply left alone.
 *
 * @param binds       The drawable's retained binds, whose @ref RetainedBinds::commands is the list.
 * @param child       Child slot of the bind being replaced (kNoChild when it is not in the list).
 * @param replacement The bind to put there, or null to leave the list unchanged.
 */
void SceneBridge::swapRetainedChild(RetainedBinds& binds, std::size_t child, ::vsg::Command* replacement)
{
    if (replacement == nullptr || child == SceneBridge::RetainedBinds::kNoChild || binds.commands == nullptr ||
        child >= binds.commands->children.size()) {
        return;
    }
    binds.commands->children[child] = replacement;
}


void SceneBridge::retireNode(::vsg::ref_ptr<::vsg::Node> node){
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
    // Same one-per-submit bookkeeping for the per-draw slots whose drawable is gone: their
    // offset may still be bound by the frames the ring just accounted for.
    flushDrawSlots();
}

void SceneBridge::clearCache()
{
    // Every retained item goes, so every per-draw slot it held goes with it (deferred: the
    // wrappers that bound those offsets are being dropped in the same breath).
    for (auto& entry : cache_) {
        if (Item* item = entry.second.payload().get()) {
            releaseDrawSlot(item->draw_slot);
            item->draw_slot = {};
        }
    }
    cache_.clear();
    // The candidate lists describe that cache (the keys it holds), so they go with it: a list left
    // behind would keep raw geometry addresses alive-looking for a cache that no longer has them.
    absent_.clear();
    absent_set_.clear();
    last_seen_.clear();
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
            // The drawable's per-draw block slot is reserved here, with the item: the
            // wrapper built below binds its offset, so it has to exist first. Only OUR
            // forward set reads the block — the built-in fallback carries opacity in the
            // vertex colour — so only that set takes a slot. A pool that cannot provide one
            // is an OUT-OF-MEMORY failure, and drawing without a block would read zeros (the
            // shader would scale the fragment alpha by 0 and the drawable would silently
            // vanish), so it is reported and the drawable skipped.
            if (forward_draw_block_ && !item->draw_slot.valid() && draw_block_pool_ != nullptr) {
                item->draw_slot = draw_block_pool_->reserve();
                if (!item->draw_slot.valid()) {
                    report(vine::graphics::DiagnosticSeverity::Error,
                           vine::graphics::DiagnosticCategory::ContentSkipped,
                           u8"the per-draw block pool could not provide a slot (out of device memory); the "
                           u8"drawable is dropped this frame");
                    cache_.erase(it);
                    continue;
                }
            }
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
        // The texture is part of the STATE identity: a different texture, or the same one re-filled (its
        // revision), must rebuild the descriptor bind rather than keep sampling the old upload.
        const vine::graphics::Texture* const texture =
            cmd.material.get() != nullptr ? cmd.material.get()->texture() : nullptr;
        const std::uint64_t texture_revision = texture != nullptr ? texture->revision() : 0u;
        const bool          data_dirty =
            !had_node || item->revision != geometry->revision() ||
            item->topology != state.topology ||
            (has_loc2 && (item->program.get() == nullptr) != (cmd.program.get() == nullptr));
        // Opacity is a per-drawable VALUE, never a state input: it rides the draw block
        // on our forward set and the colour carrier's alpha on the built-in one, and
        // neither changes which vertex inputs the pipeline feeds. An opacity edit is
        // therefore always a value write, with no rebuild in between.
        const bool state_dirty = !had_node || item->material.get() != cmd.material.get() ||
                                 item->texture.get() != texture ||
                                 item->texture_revision != texture_revision ||
                                 item->render_state != state ||
                                 item->program.get() != cmd.program.get() ||
                                 item->program_revision != program_rev;
        if (data_dirty || state_dirty) {
            item->revision                = geometry->revision();
            item->topology                = state.topology;
            item->material                = cmd.material;
            item->texture                 = vine::intrusive_ptr<const vine::graphics::Texture>(texture);
            item->texture_revision        = texture_revision;
            item->render_state            = state;
            item->program                 = cmd.program;
            item->program_revision        = program_rev;
            changed                       = true;
        }

        // A data rebuild may have changed the forwarded custom-channel SET
        // (locations >= 3 added/removed), which the state wrapper's per-layout
        // shader set / variant identity depends on. Detect that here so the
        // wrapper is rebuilt even when material / state / program did not
        // change: otherwise a live-added channel would stay unbound (and a
        // removed one leave a stale binding / layout) until some state change.
        bool state_channels_changed = false;
        if (data_dirty) {
            // The identity of everything the retained node reads, computed BEFORE deciding how to react: an
            // edit that left every vertex channel alone needs only the index stream replaced (see below),
            // while anything else needs the whole node re-materialised.
            const std::vector<ChannelKey> keys_now  = channelKeysOf(*geometry);
            const ChannelKey              index_now = indexKeyOf(*geometry);
            // The fast path must not skip what a rebuild would check. Two checks gate it: the layout has to
            // be the one the alias path reads (three components per vertex — otherwise the vertex count the
            // bounds check needs is a different number), and every index has to be in range, because an
            // out-of-range index reads OOB on the GPU and the builder's rejection (reported once per
            // revision) is what tells the caller. A stream that fails either takes the rebuild.
            const auto* const position_channel =
                geometry->buffer(attributeLocation(vine::graphics::VertexAttribute::Position));
            const bool        layout_is_aliased =
                position_channel != nullptr && !position_channel->empty() && position_channel->components == 3u;
            bool indices_in_range = false;
            if (layout_is_aliased && index_now.buffer != nullptr) {
                const std::size_t vertex_count  = position_channel->vertexCount();
                const auto        src_indices   = geometry->indices();
                indices_in_range = std::all_of(src_indices.begin(), src_indices.end(),
                                               [vertex_count](std::uint32_t index) { return index < vertex_count; });
            }
            // An edit that left the node's SHAPE alone can refresh the streams whose bytes changed instead
            // of re-materialising (and re-uploading) the whole mesh: each channel has its own bind command,
            // so a fresh BufferInfo for one of them copies one channel (see RetainedBinds). This generalises
            // the index-only case — the index stream is simply one more channel that refreshes in place.
            //
            // The index stream is a SLICE of its buffer (see Geometry::setIndices), and the bind aliases the
            // whole buffer while the DRAW states first index / count. So replacing the bind in place is only
            // valid when the same span is drawn from a different buffer: a changed span changes the draw
            // command, which only a rebuild rewrites.
            const bool index_buffer_changed = index_now.buffer != nullptr && item->index_key.buffer != nullptr &&
                                              index_now.buffer != item->index_key.buffer;
            const bool index_span_changed = index_now.offset != item->index_key.offset ||
                                            index_now.count != item->index_key.count;
            const bool index_changed = index_buffer_changed || index_span_changed;
            // One refreshed channel: the bind to re-point, the array it now reads, and — when that bind is
            // SHARED — the stream identity the cache has to serve it under (see the apply block below).
            struct RefreshedChannel
            {
                std::size_t                          binding = 0u;
                ::vsg::ref_ptr<::vsg::Data>          array;
                VsgMeshResourceCache::ChannelKey     shared_key{};
                bool                                 shared = false;
            };
            std::vector<RefreshedChannel> refreshed;
            bool refresh_ok = item->data_node != nullptr && item->binds.index != nullptr &&
                              shapesMatch(item->channel_keys, keys_now) &&
                              (!index_changed ||
                               (index_buffer_changed && !index_span_changed && indices_in_range));
            if (refresh_ok) {
                const std::size_t vertex_count = position_channel != nullptr ? position_channel->vertexCount() : 0u;
                const ChannelKey* const positions_before = keyAt(item->channel_keys, 0u);
                const ChannelKey* const positions_after  = keyAt(keys_now, 0u);
                const bool positions_changed =
                    positions_before != nullptr && positions_after != nullptr && !(*positions_before == *positions_after);
                for (const std::uint32_t location :
                     { attributeLocation(vine::graphics::VertexAttribute::Position),
                       attributeLocation(vine::graphics::VertexAttribute::Normal),
                       attributeLocation(vine::graphics::VertexAttribute::Color),
                       attributeLocation(vine::graphics::VertexAttribute::TexCoord0) })
                {
                    const std::size_t binding = RetainedBinds::canonicalBindingOf(location);
                    if (binding == RetainedBinds::kNoBinding || item->binds.canonical[binding] == nullptr) {
                        continue;
                    }
                    const ChannelKey* before = keyAt(item->channel_keys, location);
                    const ChannelKey* after  = keyAt(keys_now, location);
                    // A derived channel has no channel key at all (the geometry authors none), yet it is what
                    // the positions are folded into: new positions invalidate it, so it is refreshed too.
                    const bool derived_normals_invalidated = location == 1u && before == nullptr && after == nullptr &&
                                                             positions_changed &&
                                                             item->derived.derived_normals != nullptr;
                    if (!derived_normals_invalidated &&
                        (before == nullptr || after == nullptr || *before == *after)) {
                        continue; // absent both times, or byte-for-byte the same stream
                    }
                    auto array =
                        refreshCanonicalChannel(geometry, location, vertex_count, state.topology, item->derived);
                    if (array == nullptr) {
                        refresh_ok = false; // this channel needs the builder (and its diagnostics)
                        break;
                    }
                    RefreshedChannel entry;
                    entry.binding = binding;
                    entry.array   = std::move(array);
                    // A refresh only ever produces one of the VERBATIM views the builder aliases (a packed or
                    // derived array needs the builder), so a shared bind may be refreshed through the cache:
                    // the new key says "same buffer, new revision", which is exactly the stream the new bytes
                    // are, and the next geometry to refresh the same stream joins this entry (one upload).
                    if (item->binds.canonical_shared[binding] && after != nullptr) {
                        entry.shared                 = true;
                        entry.shared_key.binding     = static_cast<std::uint32_t>(binding);
                        entry.shared_key.components  = after->components;
                        entry.shared_key.buffer      = after->buffer;
                        entry.shared_key.revision    = after->revision;
                        entry.shared_key.offset      = after->offset;
                        entry.shared_key.count       = after->count;
                    }
                    refreshed.push_back(std::move(entry));
                }
            }

            // The refresh has to be EXPLAINED by the snapshots: a revision the per-stream identities do not
            // account for (a buffer mutated in place without bumping its own revision, say) must not be
            // answered with "nothing to do" — that would leave the previous bytes on the GPU, silently. Such
            // a revision falls back to the rebuild below, which re-reads everything.
            const bool refresh_applies = refresh_ok && (!refreshed.empty() || index_changed);
            if (refresh_applies) {
                // Every refreshed channel goes through its own bind, so vsg re-creates and copies exactly
                // those channels; the index stream is swapped the same way. Nothing is new here, but the
                // replaced BufferInfos still need this frame's compile pass — which the
                // (data_dirty || state_dirty) block below already queues.
                for (RefreshedChannel& entry : refreshed) {
                    if (entry.shared) {
                        // A shared bind belongs to EVERY geometry reading that stream: re-pointing it here
                        // would hand them this geometry's array, so this drawable gets the bind the cache
                        // holds for the NEW stream instead and swaps it in at the same child slot (keeping
                        // the command order, and therefore the binding numbers, intact).
                        const auto bind = meshResources().getOrCreateVertexBind(entry.shared_key, entry.array);
                        swapRetainedChild(item->binds, item->binds.canonical_child[entry.binding], bind.get());
                        item->binds.canonical[entry.binding] = bind;
                        continue;
                    }
                    item->binds.canonical[entry.binding]->assignArrays(::vsg::DataList{ entry.array });
                }
                if (index_changed) {
                    auto indices = boundIndexArray(*geometry);
                    if (item->binds.index_shared && indices != nullptr) {
                        // Same rule as a shared vertex channel: the index stream's bind is not ours alone.
                        const auto key  = indexBindKeyOf(*geometry);
                        const auto bind = meshResources().getOrCreateIndexBind(key, indices);
                        swapRetainedChild(item->binds, item->binds.index_child, bind.get());
                        item->binds.index = bind;
                    }
                    else {
                        item->binds.index->assignIndices(indices);
                    }
                    item->index_key = index_now;
                }
                item->channel_keys = keys_now;
                changed            = true;
            }
            else {
            // Fresh vertex data: rebuild the data node; the previous opacity
            // carrier is dropped with it and rewritten on the next frames. The
            // replaced node is parked (its buffers may still be in flight).
            item->extra_channels.clear();
            // Whether this rebuild's announcement is one a STREAM accounts for: a fresh node (nothing was
            // built yet), a channel reading a different buffer / revision, or an index stream that moved.
            // A revision nothing explains — the geometry says its data changed while every stream still
            // reads the same bytes, which is what a buffer written through a raw pointer reports — must be
            // answered by RE-READING the model, and a retained shared bind was copied from the bytes as of
            // ITS insertion: it cannot be vouched for here, so this node builds its own binds instead.
            const bool streams_changed = item->data_node == nullptr ||
                                         !streamsMatch(item->channel_keys, keys_now) ||
                                         !(index_now == item->index_key);
            // The retained node is parked (its buffers may still be in flight), which clears the member the
            // check above reads — hence the order.
            retireNode(std::move(item->data_node));
            item->binds     = RetainedBinds{};
            item->data_node = buildGeometryData(geometry, state.topology, item->extra_channels, item->derived,
                                                item->binds, streams_changed ? &meshResources() : nullptr);
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
            // Remember what this node was built from, so the next revision can tell which streams changed.
            item->channel_keys = std::move(keys_now);
            item->index_key    = index_now;
            }
        }

        // World-space placement comes from the command stream; the transform write at the
        // end of this iteration is skipped when the node did not move.
        const ::vsg::dmat4 world       = detail::toVsg(cmd.modelMatrix);
        const bool        matrix_moved = !item->matrix_valid || item->last_matrix != world;

        if (state_dirty || item->state_node == nullptr || state_channels_changed) {
            // The replaced wrapper (and the pipeline it holds) may still be
            // referenced by an in-flight command buffer: park it.
            retireNode(std::move(item->state_node));
            item->state_node = buildStateGroup(item->data_node, item->material.get(),
                                               item->texture.get(), item->render_state,
                                               item->program.get(), item->extra_channels, &item->derived,
                                               item->draw_slot);
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

        // Effective per-drawable opacity: a PER-DRAWABLE VALUE held in the pooled block — four bytes
        // written through the pool's mapping, so a translucent drawable costs one store per frame
        // instead of a pass over its vertices, and the value is in the buffer the frame records FROM
        // rather than one frame behind it.
        if (item->draw_slot.valid() && item->last_slot_opacity != cmd.opacity) {
            draw_block_pool_->writeOpacity(item->draw_slot, cmd.opacity);
            item->last_slot_opacity = cmd.opacity;
        }

        if (matrix_moved) {
            item->transform->matrix = world;
            item->last_matrix       = world;
            item->matrix_valid      = true;
        }
        visible.emplace_back(item->transform);
    }

    // One share count for both sweeps of this sync (P11): the geometry sweep judges "the app let
    // go of this geometry" by it and the cache sweep judges its programs and materials by it, so
    // building it once keeps the two from disagreeing about what the scene still holds.
    OwnedShareCounts sweep_shares;
    const OwnedShareCounts* shares = retained_shares_;
    if (shares == nullptr) {
        collectSweepShares(sweep_shares);
        shares = &sweep_shares;
    }
    updateAbsentCandidates(seen);
    if (frame_drawn_ != nullptr) {
        // Report this slot's drawings to the frame; the session ages every slot by their UNION
        // once per frame (VsgRenderer::submitFrame), so a geometry one pass draws is not absent
        // for the others, and a pass that does not run at all still has its cache aged.
        frame_drawn_->insert(seen.begin(), seen.end());
    } else {
        changed = ageAbsentItems(seen, *shares) || changed;
    }
    publishRetainedChildren(*root, visible, commands);
    releaseAbandonedCaches(*shares);
    return changed;
}

void SceneBridge::updateAbsentCandidates(const std::unordered_set<const vine::graphics::Geometry*>& seen)
{
    // The geometries this slot has cached but did not draw in this sync: the eviction CANDIDATES.
    // Walking the whole cache to find them (what the sweep used to do) cost O(entries ever seen)
    // per frame per slot — a roaming camera fills the cache with the whole scene, so the frame got
    // slower the more of the scene it had ever visited, for a list that is usually empty. The list
    // is built from what the sync drew instead, so maintaining it costs O(drawn + absent).
    //
    // Its two halves also ARE the keys of `cache_`: an entry is created by the sync that draws its
    // geometry, dropped here when the window expires or by the app releasing the geometry, and
    // cleared by clearCache(). That is what lets the ownership pass read the lists instead of the
    // cache (see collectOwnedShares).
    //
    // A geometry that comes back starts the window over: it measures CONSECUTIVE absence, so a
    // scene that alternates would otherwise evict what it is still using.
    for (const auto* geometry : absent_) {
        if (seen.count(geometry) != 0) {
            if (const auto it = cache_.find(geometry); it != cache_.end()) {
                it->second.payload()->absent_frames = 0;
            }
        }
    }
    std::vector<const vine::graphics::Geometry*> next;
    next.reserve(absent_.size() + last_seen_.size());
    for (const auto* geometry : absent_) {
        if (seen.count(geometry) == 0) {
            next.push_back(geometry); // still absent
        }
    }
    for (const auto* geometry : last_seen_) {
        if (seen.count(geometry) == 0 && !absent_set_.count(geometry)) {
            next.push_back(geometry); // just became absent
        }
    }
    // A geometry that left this slot's frame gets its rejection record cleared: the record says
    // "this drawable was refused", and a fix that bumps the revision must be re-evaluated on the
    // next appearance instead of being skipped by a stale record.
    for (const auto* geometry : next) {
        if (const auto it = cache_.find(geometry); it != cache_.end()) {
            it->second.payload()->rejected = false;
        }
    }
    absent_ = std::move(next);
    absent_set_.clear();
    absent_set_.insert(absent_.begin(), absent_.end());
    last_seen_.assign(seen.begin(), seen.end());
}

bool SceneBridge::ageAbsentItems(const std::unordered_set<const vine::graphics::Geometry*>& drawn,
                                 const OwnedShareCounts& shares)
{
    // Age the CANDIDATES (see updateAbsentCandidates), and judge "was it drawn?" by @p drawn: the
    // SESSION hands the union of every slot's drawings once per frame, so a geometry any pass drew
    // this frame is not absent — and a slot whose pass did not run at all still ages what it
    // cached, instead of pinning it for the session.
    //
    // A bridge driven directly (a test, a driver that never opens a frame) ages in its own sync
    // with what IT drew, which is what this looks like from one slot.
    if (absent_.empty()) {
        return false;
    }
    constexpr std::uint32_t kAbsentEvictFrames = 600;
    bool                    changed = false;
    std::vector<const vine::graphics::Geometry*> still_absent;
    still_absent.reserve(absent_.size());
    for (const auto* geometry : absent_) {
        const auto it = cache_.find(geometry);
        if (it == cache_.end()) {
            continue; // its entry is already gone (the app released the geometry)
        }
        Item* item = it->second.payload().get();
        if (drawn.count(geometry) != 0) {
            // Drawn by some pass / by this slot again: the window starts over and the entry stays.
            item->absent_frames = 0;
            still_absent.push_back(geometry);
            continue;
        }
        // The entry owns the geometry, which is what makes the pointer key valid. Once the app
        // itself stops referencing it, nothing can ever look the entry up again, so release it —
        // and the geometry with it — right away instead of pinning both for the whole window.
        if (it->second.abandoned(shares) || ++item->absent_frames > kAbsentEvictFrames) {
            // The retained subtree may still be referenced by an in-flight command buffer, so park
            // it instead of destroying it here.
            retireNode(std::move(item->transform));
            // The draw block's slot goes with it — also deferred, because a frame in flight may
            // still bind the offset this drawable's wrapper recorded.
            releaseDrawSlot(item->draw_slot);
            item->draw_slot = {};
            cache_.erase(it);
            changed = true;
            continue; // the entry is gone, so it leaves the candidate list too
        }
        still_absent.push_back(geometry);
    }
    absent_ = std::move(still_absent);
    absent_set_.clear();
    absent_set_.insert(absent_.begin(), absent_.end());
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

void SceneBridge::collectOwnedShares(OwnedShareCounts& shares) const
{
    // One share per entry of every cache that holds a key object: a program is held by the stage
    // cache, the per-layout ShaderSet cache and one variant template per (program, material) pair
    // this slot draws, so the same program can be held any number of times — the count is what the
    // sweep needs, and assuming a number instead is what left the caches waiting for each other.
    // The geometries with an entry are exactly the ones this slot drew in its last sync or has
    // been keeping absent since (see absent_ / last_seen_), so the count does not walk the cache:
    // the frame's ownership pass must not cost more than the frame itself (see ageAbsentItems).
    for (const auto* geometry : last_seen_) {
        shares.add(geometry);
    }
    for (const auto* geometry : absent_) {
        shares.add(geometry);
    }
    // The two program caches are keyed by a content HASH, so the object they own comes from the
    // entry itself; the variant cache is keyed by the object (its pair).
    for (const auto& entry : program_stages_) {
        shares.add(entry.second.key());
    }
    for (const auto& entry : program_shader_sets_) {
        shares.add(entry.second.key());
    }
    for (const auto& entry : variant_cache_) {
        shares.add(entry.second.firstKey());
        shares.add(entry.second.secondKey());
    }
}

void SceneBridge::collectSweepShares(OwnedShareCounts& shares)
{
    materialManager().collectOwnedShares(shares);
    collectOwnedShares(shares);
}

std::size_t SceneBridge::releaseAbandonedCaches()
{
    // No session counts, so judge by what this bridge can see (see collectSweepShares). The
    // session's counts are the exact answer when several slots hold the same objects.
    if (retained_shares_ != nullptr) {
        return releaseAbandonedCaches(*retained_shares_);
    }
    OwnedShareCounts local;
    collectSweepShares(local);
    return releaseAbandonedCaches(local);
}

std::size_t SceneBridge::releaseAbandonedCaches(const OwnedShareCounts& shares)
{
    // Capacity trims happen on insert (the FIFO half of the bargain), which is
    // what bounds the chain of caches sharing one program; this is the prompt
    // half, shared by every program-keyed cache through OwnedCache.hpp so the
    // three of them cannot drift apart.
    const std::size_t erased = eraseAbandoned(program_stages_, shares) +
                               eraseAbandoned(program_shader_sets_, shares) +
                               eraseAbandoned(variant_cache_, shares);
    // Textures are swept here too, and this is the sweep's ONLY caller:
    // VsgTextureCache::releaseAbandoned() had an implementation and a unit test but no production caller,
    // so a texture the scene stopped sampling kept its GPU image until 256 later textures pushed it out of
    // the FIFO (or the slot was destroyed). Its entry owns the texture it is keyed by, so "the app dropped
    // it and no retained entry holds it any more" is observable exactly here — evictAbsentItems() ran
    // earlier in this same sync, so the geometry that just left the frame has already let go of the
    // texture its entry held.
    //
    // Deliberately NOT part of the eviction gate below: the shared-object table registers pipelines /
    // layouts / descriptor sets, never images, so releasing a texture is no reason to walk it.
    textureCache().releaseAbandoned();
    // Same for the shared mesh binds: their entries hold the arrays (and through them the model's buffers),
    // so an entry whose last geometry is gone must not keep that stream uploaded for the session.
    meshResources().releaseAbandoned();
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
