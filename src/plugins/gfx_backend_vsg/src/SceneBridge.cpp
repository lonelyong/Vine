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
        // ONE derivation of what the policy means to a pipeline (see detail::depthTestWrite), shared
        // with the slot that bakes it into the shader set: two copies is how a fourth DepthMode ends
        // up honoured in one of them.
        const detail::DepthTestWrite depth = detail::depthTestWrite(content_depth_mode);
        state.depth.test                   = depth.test;
        state.depth.write                  = depth.write;
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

// The destructor drains the retained cache so that every item leaves while the retire ring still
// exists (see the body), and deliberately touches nothing else: the injected caches are the
// session's, and each item's per-draw slot goes back to the pool through its lease, retired for the
// frames that could still bind its offset (see VsgDrawBlockPool::Lease).
SceneBridge::~SceneBridge()
{
    // The items are dropped HERE rather than by the member destructors: an item parks its retained
    // subtree on the retire ring when it goes (Item::~Item), and the ring is declared AFTER this
    // cache — the member destructors run in reverse order, so they would park into a destroyed ring.
    // Emptying the cache in the body makes every item leave while the ring is still there; whatever
    // they park is then released with the bridge, which is safe at this point: nothing is in flight
    // (an explicit teardown path waits for the device first, and a bridge that goes with its session
    // goes with the device).
    cache_.clear();
}

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
    no_shader_set_reported_.rearm();
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

VsgTextureCache& SceneBridge::textureCache()
{
    return texture_cache_ != nullptr ? *texture_cache_ : default_texture_cache_;
}


/** @brief Retained vsg node for one drawn geometry.
 *
 * An item owns the GPU state a dropped drawable must not destroy outright: its per-draw slot (through
 * the LEASE, which retires it back to the pool) and its retained subtree (parked on the bridge's
 * retire ring by the destructor). Both are consequences of BEING DROPPED rather than steps for the
 * code path that does the dropping, so no erase site can forget one — which is exactly what the one
 * path that did (a state wrapper that failed to build) used to do: it leaked a per-draw slot on every
 * frame it failed in, and destroyed a subtree a submitted command buffer could still name.
 */
struct SceneBridge::Item {
    /** @brief Creates an item whose drops go through @p bridge (its retire ring).
     *
     * @param bridge Bridge the item belongs to (it must outlive the item).
     */
    explicit Item(SceneBridge& bridge) noexcept :
        owner(bridge)
    {
    }

    /** @brief Parks the retained subtree and returns the per-draw slot.
     *
     * A dropped subtree may still be named by a submitted command buffer, so it is PARKED
     * (SceneBridge::retireNode) instead of destroyed — the rule the data and state rebuild paths
     * already followed, applied here so that every way out of the cache follows it. Parking
     * @ref transform covers the whole subtree (the state wrapper and the data node are its
     * descendants); a data node that never reached the graph needs no park, because nothing ever
     * recorded it.
     */
    ~Item()
    {
        if (transform != nullptr) {
            owner.retireNode(std::move(transform));
        }
    }

    SceneBridge& owner;

    // Rejection record: true when this geometry's data could not be built at
    // @ref rejected_revision (malformed attributes / out-of-range indices).
    // Kept so the diagnostic prints once per revision instead of every frame,
    // and cleared when the geometry leaves the frame so a fixed geometry is
    // re-evaluated on its next appearance.
    bool rejected = false;
    std::uint64_t rejected_revision = 0;
    // State-build record: true when the LAST attempt to build this drawable's state wrapper failed for the
    // identity stored above (a program the backend cannot compile, a pipeline it cannot create). Like the
    // rejection record, it is what keeps such a drawable from being re-attempted every frame: the attempt is
    // skipped until one of its inputs changes, and the next attempt clears it. The item itself is KEPT (with
    // its uploaded data node and its per-draw slot), so a drawable the backend cannot build costs a lookup
    // per frame instead of an item churn — and no longer depends on the covered cases happening not to draw
    // the failing drawable again, which is the only reason the erasure this replaced was invisible.
    bool state_failed = false;
    // Last translated identity, used to detect geometry/material/state changes.
    // The material and the program are HELD, not merely compared: the address is
    // the identity here, so a released material or program could be replaced at
    // the same address and the comparisons below would then report "nothing
    // changed" while the retained pipeline / descriptor still belongs to the
    // dead one (wrong colours / wrong shader, silently). The cached variants own
    // their keys for the same reason (see OwnedPairCacheEntry).
    //
    // CONSEQUENCE, and why it is bounded rather than a leak: those references are NOT shares (the
    // counts count CACHE entries only), so a material / program / texture the app has dropped stays
    // alive until no live item holds it -- i.e. until this geometry is released too. The reuse
    // window that used to break that tie by evicting the item is gone (see
    // releaseAbandonedGeometries), so a geometry the app keeps but never draws pins its material's
    // entry for as long as the app keeps it. Each cache's capacity trim is what bounds that
    // (VsgMaterialManager::kMaxEntries, the texture cache's, 64 / 64 / 256 for the program caches);
    // releasing the geometry (or clearCache()) frees it sooner. Pinned by
    // SceneBridgeCacheOwnershipTest.AHeldUndrawnGeometryPinsItsDroppedMaterial.
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
    // This drawable's LEASE on a session per-draw block slot (see setDrawBlockPool), or an empty one
    // when the bridge has no pool or the reservation failed. The slot's VALUES are what a translucent
    // drawable costs per frame — four floats written in place instead of a pass over its vertices —
    // and the slot's OFFSET is what its state wrapper binds. The lease is what hands the slot back
    // when this item goes, so no drop path releases it by hand.
    VsgDrawBlockPool::Lease draw_slot;
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

void SceneBridge::advanceRetireRing(FrameCommit commit)
{
    // One advance per COMMITTED frame: the bucket entered now was filled kRetireRingDepth
    // submits ago, so the command-buffer slot that could have referenced its objects has been
    // re-recorded since (start() waits on the slot's fence before re-recording it) and the GPU
    // no longer executes them. The token in the signature is the evidence that this advance
    // accounts for a committed frame (see FrameCommit).
    //
    // The per-draw slots follow the same clock, but their queue is the POOL's (the session's), not
    // this bridge's: a bridge can be destroyed by a teardown while its slots still have frames to
    // wait out, and a queue that died with it would lose the pool's capacity for good (see
    // VsgDrawBlockPool::retire). VsgRenderer::settleSubmittedFrame advances that one.
    retire_ring_.advance(commit);
}

void SceneBridge::clearCache()
{
    // Every retained item goes, so every per-draw slot it held goes with it: the slot's lease retires
    // it back to the pool, and the item's destructor parks its retained subtree (see Item), as the
    // item is destroyed. Neither is spelled out here, so this path cannot disagree with the live ones
    // about what dropping a drawable costs.
    cache_.clear();
    // The candidate lists describe that cache (the keys it holds), so they go with it: a list left
    // behind would keep raw geometry addresses alive-looking for a cache that no longer has them.
    undrawn_.clear();
    undrawn_set_.clear();
    drawn_.clear();
    program_shader_sets_.clear();
    program_stages_.clear();
    variant_cache_.clear();
    // Forget the shared-object registry (the registered pipeline / layout / descriptor-set objects)
    // when the slot's content is released. PARKED, never cleared in place: this path runs MID-FRAME
    // (a slot rebuilt after a policy change, a shader set swapped, a shadow seed that changed), so a
    // command buffer that was submitted before it may still name what the registry holds — and
    // clearing in place destroyed them while in flight. Measured in the self-test's pass-protocol
    // phase with the validation layer on: `VUID-vkDestroyPipeline-pipeline-00765` together with the
    // render pass / framebuffer pair beside it (`-vkDestroyRenderPass-00873`, `-vkDestroyFramebuffer-00892`),
    // none of which the runs without the layer could see. The registry OWNS what it holds, so parking
    // it keeps all of it alive for exactly the frames that may still reference it (see VsgRetireRing),
    // and a fresh registry takes its place for the slot's next build (which is what the in-place
    // `clear()` used to leave behind, now as a new object).
    if (shared_objects_ != nullptr) {
        retire_ring_.park(shared_objects_);
        shared_objects_ = ::vsg::SharedObjects::create();
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
        // And re-arm the build: this call exists to say "the wrappers were built
        // against something that is no longer true", so a recorded failure (a set
        // or a pipeline that could not be built) must not suppress the new attempt.
        item->state_failed = false;
    }
}

bool SceneBridge::syncRenderCommands(
    const std::vector<vine::graphics::RenderCommand>& commands,
    ::vsg::Group* root,
    std::vector<::vsg::ref_ptr<::vsg::Node>>* created,
    const OwnedShareCounts* session_shares)
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
            auto entry = std::make_unique<Item>(*this);
            item = entry.get();
            // The lookup key is the geometry as given, while the ENTRY holds the
            // owning reference that keeps that address unique (OwnedCacheEntry).
            it = cache_.emplace(
                     geometry,
                     GeometryCacheEntry(vine::intrusive_ptr<const vine::graphics::Geometry>(geometry),
                                        std::move(entry), 0u))
                     .first;
            changed = true;
            if (!reserveDrawSlot(*item)) {
                cache_.erase(it);
                continue;
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
        // "No data yet" is a property of the DATA node, not of the transform. A drawable whose state could not
        // be built keeps its data node and its per-draw slot but has no transform, so asking `!had_node` marked
        // it dirty for ever: its whole mesh was re-materialised and re-uploaded on every frame it was drawn
        // (measured on the self-test: 163 -> 160 builds per run, small because its covered failing drawables
        // are drawn on a single frame — an application loop that keeps drawing one would pay it every frame).
        // What decides a rebuild is the data IDENTITY below, the same rule the state attempt follows.
        const bool          data_dirty =
            item->data_node == nullptr || item->revision != geometry->revision() ||
            item->topology != state.topology ||
            (has_loc2 && (item->program.get() == nullptr) != (cmd.program.get() == nullptr));
        // Opacity is a per-drawable VALUE, never a state input: it rides the draw block
        // on our forward set and the colour carrier's alpha on the built-in one, and
        // neither changes which vertex inputs the pipeline feeds. An opacity edit is
        // therefore always a value write, with no rebuild in between.
        //
        // The five inputs a wrapper is built from, kept apart from `state_dirty` below because "there is no
        // wrapper yet" is not one of them (see the attempt rule).
        const bool state_inputs_changed = item->material.get() != cmd.material.get() ||
                                          item->texture.get() != texture ||
                                          item->texture_revision != texture_revision ||
                                          item->render_state != state ||
                                          item->program.get() != cmd.program.get() ||
                                          item->program_revision != program_rev;
        const bool state_dirty = !had_node || state_inputs_changed;
        // The identity block records what the retained nodes were built FOR, so it runs when one of those
        // inputs changed — `!had_node` is not one of them, and a failing drawable neither has a transform nor
        // new inputs (it keeps its data, its slot and its recorded failure until something real changes).
        if (data_dirty || state_inputs_changed) {
            item->revision                = geometry->revision();
            item->topology                = state.topology;
            item->material                = cmd.material;
            item->texture                 = vine::intrusive_ptr<const vine::graphics::Texture>(texture);
            item->texture_revision        = texture_revision;
            item->render_state            = state;
            item->program                 = cmd.program;
            item->program_revision        = program_rev;
            changed                       = true;
            if (data_dirty) {
                // Fresh data is a fresh attempt: a failure recorded for the previous data says nothing about
                // this one (the shape is part of what a variant can fail to be built for — a channel set is
                // the obvious case, which state_channels_changed below also covers).
                item->state_failed = false;
            }
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
            std::vector<ChannelKey> keys_now  = channelKeysOf(*geometry);
            const ChannelKey        index_now = indexKeyOf(*geometry);
            if (refreshChangedStreams(*geometry, *item, state, keys_now, index_now)) {
                // Each refreshed channel was served through its own bind, so vsg re-creates and copies
                // exactly those channels: nothing here is new, but the replaced BufferInfos still need
                // this frame's compile pass, which the created queue below already covers.
                changed = true;
            }
            else if (!rebuildDataNode(*geometry, *item, state, keys_now, index_now)) {
                continue; // refused and recorded for this revision (see rebuildDataNode)
            }
            else {
                ++data_edit_stats_.data_nodes_built;
                state_channels_changed = item->state_channels != item->extra_channels;
            }
        }

        // World-space placement comes from the command stream; the transform write at the
        // end of this iteration is skipped when the node did not move.
        const ::vsg::dmat4 world       = detail::toVsg(cmd.modelMatrix);
        const bool        matrix_moved = !item->matrix_valid || item->last_matrix != world;

        // Whether this frame BUILT a new wrapper (as opposed to reusing the retained one): a new wrapper is a new
        // subtree, so it has to reach the compile queue below whoever asked for the rebuild — an identity change,
        // a channel-set change, or invalidateState() (which replaces the wrapper while changing none of its
        // inputs at all: a pass whose commands all author their own depth gets a wrapper rebuilt by a depth-policy
        // flip that then compares equal, and the queue used to stay empty for it — the D22 shape, an
        // uncompiled pipeline about to be recorded).
        bool state_rebuilt = false;
        if (state_inputs_changed || state_channels_changed ||
            (item->state_node == nullptr && !item->state_failed)) {
            if (!rebuildStateWrapper(*item)) {
                continue; // recorded failure: nothing to attach, nothing to draw
            }
            state_rebuilt = true;
        }
        if (item->state_node == nullptr) {
            continue; // still unbuildable for this identity: nothing to attach, nothing to draw
        }

        attachRetainedNodes(*item, had_node);

        if ((data_dirty || state_dirty || state_rebuilt) && created != nullptr) {
            // New/rebuild subtrees must be GPU-compiled before recording.
            created->emplace_back(item->transform);
        }

        writePerDrawValues(*item, cmd, world, matrix_moved);
        visible.emplace_back(item->transform);
    }

    // One share count for both sweeps of this sync (P11): the geometry sweep judges "the app let
    // go of this geometry" by it and the cache sweep judges its programs and materials by it, so
    // building it once keeps the two from disagreeing about what the scene still holds.
    OwnedShareCounts sweep_shares;
    const OwnedShareCounts* shares = session_shares;
    if (shares == nullptr) {
        collectSweepShares(sweep_shares);
        shares = &sweep_shares;
    }
    updateUndrawnCandidates(seen);
    // The sweep runs here for session-driven slots too: its predicate is the app's own reference
    // count (see releaseAbandonedGeometries), so it does not have to wait for the other slots to
    // have synced, and the session sweeps every slot again at the frame's end — which is what
    // covers the slots whose pass did not run at all this frame.
    changed = releaseAbandonedGeometries(*shares) || changed;
    publishRetainedChildren(*root, visible, commands);
    releaseAbandonedCaches(*shares);
    return changed;
}

bool SceneBridge::reserveDrawSlot(Item& item)
{
    if (!forward_draw_block_ || item.draw_slot.valid() || draw_block_pool_ == nullptr) {
        return true;
    }
    item.draw_slot = draw_block_pool_->acquire();
    if (item.draw_slot.valid()) {
        return true;
    }
    report(vine::graphics::DiagnosticSeverity::Error, vine::graphics::DiagnosticCategory::ContentSkipped,
           u8"the per-draw block pool could not provide a slot (out of device memory); the "
           u8"drawable is dropped this frame");
    return false;
}

bool SceneBridge::refreshChangedStreams(const vine::graphics::Geometry& geometry, Item& item,
                                        const vine::graphics::ResolvedRenderState& state,
                                        const std::vector<ChannelKey>& keys_now, const ChannelKey& index_now)
{
    // The fast path must not skip what a rebuild would check. Two checks gate it: the layout has to
    // be the one the alias path reads (three components per vertex — otherwise the vertex count the
    // bounds check needs is a different number), and every index has to be in range, because an
    // out-of-range index reads OOB on the GPU and the builder's rejection (reported once per
    // revision) is what tells the caller. A stream that fails either takes the rebuild.
    const auto* const position_channel =
        geometry.buffer(attributeLocation(vine::graphics::VertexAttribute::Position));
    const bool        layout_is_aliased =
        position_channel != nullptr && !position_channel->empty() && position_channel->components == 3u;
    bool indices_in_range = false;
    if (layout_is_aliased && index_now.buffer != nullptr) {
        const std::size_t vertex_count = position_channel->vertexCount();
        const auto        src_indices  = geometry.indices();
        indices_in_range = std::all_of(src_indices.begin(), src_indices.end(),
                                       [vertex_count](std::uint32_t index) { return index < vertex_count; });
    }
    // The index stream is a SLICE of its buffer (see Geometry::setIndices), and the bind aliases the
    // whole buffer while the DRAW states first index / count. So replacing the bind in place is only
    // valid when the same span is drawn from a different buffer: a changed span changes the draw
    // command, which only a rebuild rewrites.
    const bool index_buffer_changed =
        index_now.buffer != nullptr && item.index_key.buffer != nullptr && index_now.buffer != item.index_key.buffer;
    const bool index_span_changed =
        index_now.offset != item.index_key.offset || index_now.count != item.index_key.count;
    const bool index_changed = index_buffer_changed || index_span_changed;
    // One refreshed channel: the bind to re-point, the array it now reads, and — when that bind is
    // SHARED — the stream identity the cache has to serve it under (see the apply block below).
    struct RefreshedChannel
    {
        std::size_t                      binding = 0u;
        ::vsg::ref_ptr<::vsg::Data>      array;
        VsgMeshResourceCache::ChannelKey shared_key{};
        bool                             shared = false;
    };
    std::vector<RefreshedChannel> refreshed;
    const bool refresh_ok = item.data_node != nullptr && item.binds.index != nullptr &&
                            shapesMatch(item.channel_keys, keys_now) &&
                            (!index_changed || (index_buffer_changed && !index_span_changed && indices_in_range));
    if (refresh_ok) {
        const std::size_t vertex_count = position_channel != nullptr ? position_channel->vertexCount() : 0u;
        const ChannelKey* const positions_before = keyAt(item.channel_keys, 0u);
        const ChannelKey* const positions_after  = keyAt(keys_now, 0u);
        const bool positions_changed =
            positions_before != nullptr && positions_after != nullptr && !(*positions_before == *positions_after);
        for (const std::uint32_t location : { attributeLocation(vine::graphics::VertexAttribute::Position),
                                              attributeLocation(vine::graphics::VertexAttribute::Normal),
                                              attributeLocation(vine::graphics::VertexAttribute::Color),
                                              attributeLocation(vine::graphics::VertexAttribute::TexCoord0) }) {
            const std::size_t binding = RetainedBinds::canonicalBindingOf(location);
            if (binding == RetainedBinds::kNoBinding || item.binds.canonical[binding] == nullptr) {
                continue;
            }
            const ChannelKey* before = keyAt(item.channel_keys, location);
            const ChannelKey* after  = keyAt(keys_now, location);
            // A derived channel has no channel key at all (the geometry authors none), yet it is what
            // the positions are folded into: new positions invalidate it, so it is refreshed too.
            const bool derived_normals_invalidated = location == 1u && before == nullptr && after == nullptr &&
                                                     positions_changed && item.derived.derived_normals != nullptr;
            if (!derived_normals_invalidated && (before == nullptr || after == nullptr || *before == *after)) {
                continue; // absent both times, or byte-for-byte the same stream
            }
            auto array = refreshCanonicalChannel(&geometry, location, vertex_count, state.topology, item.derived);
            if (array == nullptr) {
                return false; // this channel needs the builder (and its diagnostics)
            }
            RefreshedChannel entry;
            entry.binding = binding;
            entry.array   = std::move(array);
            // A refresh only ever produces one of the VERBATIM views the builder aliases (a packed or
            // derived array needs the builder), so a shared bind may be refreshed through the cache:
            // the new key says "same buffer, new revision", which is exactly the stream the new bytes
            // are, and the next geometry to refresh the same stream joins this entry (one upload).
            if (item.binds.canonical_shared[binding] && after != nullptr) {
                entry.shared                = true;
                entry.shared_key.binding    = static_cast<std::uint32_t>(binding);
                entry.shared_key.components = after->components;
                entry.shared_key.buffer     = after->buffer;
                entry.shared_key.revision   = after->revision;
                entry.shared_key.offset     = after->offset;
                entry.shared_key.count      = after->count;
            }
            refreshed.push_back(std::move(entry));
        }
    }

    // The refresh has to be EXPLAINED by the snapshots: a revision the per-stream identities do not
    // account for (a buffer mutated in place without bumping its own revision, say) must not be
    // answered with "nothing to do" — that would leave the previous bytes on the GPU, silently. Such
    // a revision falls back to the rebuild the caller runs instead, which re-reads everything.
    if (!refresh_ok || (refreshed.empty() && !index_changed)) {
        return false;
    }
    for (RefreshedChannel& entry : refreshed) {
        if (entry.shared) {
            // A shared bind belongs to EVERY geometry reading that stream: re-pointing it here
            // would hand them this geometry's array, so this drawable gets the bind the cache
            // holds for the NEW stream instead and swaps it in at the same child slot (keeping
            // the command order, and therefore the binding numbers, intact).
            const auto bind = meshResources().getOrCreateVertexBind(entry.shared_key, entry.array);
            swapRetainedChild(item.binds, item.binds.canonical_child[entry.binding], bind.get());
            item.binds.canonical[entry.binding] = bind;
            continue;
        }
        item.binds.canonical[entry.binding]->assignArrays(::vsg::DataList{ entry.array });
    }
    if (index_changed) {
        auto indices = boundIndexArray(geometry);
        if (item.binds.index_shared && indices != nullptr) {
            // Same rule as a shared vertex channel: the index stream's bind is not ours alone.
            const auto key  = indexBindKeyOf(geometry);
            const auto bind = meshResources().getOrCreateIndexBind(key, indices);
            swapRetainedChild(item.binds, item.binds.index_child, bind.get());
            item.binds.index = bind;
        }
        else {
            item.binds.index->assignIndices(indices);
        }
        item.index_key = index_now;
    }
    item.channel_keys = keys_now;
    ++data_edit_stats_.streams_refreshed;
    return true;
}

bool SceneBridge::rebuildDataNode(const vine::graphics::Geometry& geometry, Item& item,
                                  const vine::graphics::ResolvedRenderState& state,
                                  std::vector<ChannelKey>& keys_now, const ChannelKey& index_now)
{
    // Fresh vertex data: rebuild the data node; the previous opacity
    // carrier is dropped with it and rewritten on the next frames. The
    // replaced node is parked (its buffers may still be in flight).
    item.extra_channels.clear();
    // Whether this rebuild's announcement is one a STREAM accounts for: a fresh node (nothing was
    // built yet), a channel reading a different buffer / revision, or an index stream that moved.
    // A revision nothing explains — the geometry says its data changed while every stream still
    // reads the same bytes, which is what a buffer written through a raw pointer reports — must be
    // answered by RE-READING the model, and a retained shared bind was copied from the bytes as of
    // ITS insertion: it cannot be vouched for here, so this node builds its own binds instead.
    const bool streams_changed = item.data_node == nullptr || !streamsMatch(item.channel_keys, keys_now) ||
                                 !(index_now == item.index_key);
    // The retained node is parked (its buffers may still be in flight), which clears the member the
    // check above reads — hence the order.
    retireNode(std::move(item.data_node));
    item.binds     = RetainedBinds{};
    item.data_node = buildGeometryData(&geometry, state.topology, item.extra_channels, item.derived, item.binds,
                                       streams_changed ? &meshResources() : nullptr);
    if (item.data_node == nullptr) {
        // Unsupported shape / malformed vertex data (unusable attribute
        // strides, out-of-range indices, ...): nothing drawable. The
        // rejection is recorded (once per data revision, so the
        // diagnostic is not retried on every frame) and the state
        // wrapper goes with the data it wrapped.
        item.rejected          = true;
        item.rejected_revision = geometry.revision();
        retireNode(std::move(item.state_node));
        item.state_channels.clear();
        item.matrix_valid = false;
        return false;
    }
    item.matrix_valid = false;
    // Remember what this node was built from, so the next revision can tell which streams changed.
    item.channel_keys = std::move(keys_now);
    item.index_key    = index_now;
    return true;
}

bool SceneBridge::rebuildStateWrapper(Item& item)
{
    // The replaced wrapper (and the pipeline it holds) may still be
    // referenced by an in-flight command buffer: park it.
    retireNode(std::move(item.state_node));
    item.state_node = buildStateGroup(item.data_node, item.material.get(), item.texture.get(), item.render_state,
                                      item.program.get(), item.extra_channels, &item.derived, item.draw_slot);
    if (item.state_node == nullptr) {
        // Nothing will be drawn for this identity, but the item KEEPS what it holds: its uploaded data
        // node and its per-draw slot. The attempt is recorded as failed so the next frame does not
        // repeat it (see state_failed) — the same "one record per identity" rule the data-rejection
        // path follows. Measured on this build: the self-test takes this branch 4 times per run, and
        // keeping the item changes neither its `buildGeometryData` total (163 either way) nor its
        // evidence, because the covered failing drawables are not drawn again; what the record buys is
        // that this no longer HAS to be true for the cost to stay flat.
        item.state_failed = true;
        return false;
    }
    item.state_failed   = false;
    item.state_channels = item.extra_channels;
    return true;
}

void SceneBridge::attachRetainedNodes(Item& item, bool had_node)
{
    // Attach: the wrapper's child is the current data node and the
    // retained transform's child is the current wrapper.
    if (item.state_node->children.empty() || item.state_node->children.front().get() != item.data_node.get()) {
        item.state_node->children.clear();
        item.state_node->addChild(item.data_node);
    }
    if (!had_node) {
        item.transform = ::vsg::MatrixTransform::create();
        item.transform->addChild(item.state_node);
    }
    else if (item.transform->children.empty() ||
             item.transform->children.front().get() != item.state_node.get()) {
        item.transform->children.clear();
        item.transform->addChild(item.state_node);
    }
}

void SceneBridge::writePerDrawValues(Item& item, const vine::graphics::RenderCommand& cmd, const ::vsg::dmat4& world,
                                     bool matrix_moved)
{
    // Effective per-drawable opacity: a PER-DRAWABLE VALUE held in the pooled block — four bytes
    // written through the pool's mapping, so a translucent drawable costs one store per frame
    // instead of a pass over its vertices, and the value is in the buffer the frame records FROM
    // rather than one frame behind it.
    if (item.draw_slot.valid() && item.last_slot_opacity != cmd.opacity) {
        item.draw_slot.writeOpacity(cmd.opacity);
        item.last_slot_opacity = cmd.opacity;
    }
    if (matrix_moved) {
        item.transform->matrix = world;
        item.last_matrix       = world;
        item.matrix_valid      = true;
    }
}

void SceneBridge::updateUndrawnCandidates(const std::unordered_set<const vine::graphics::Geometry*>& seen)
{
    // The geometries this slot has cached but did not draw in this sync: the sweep's CANDIDATES.
    // Walking the whole cache to find them (what the sweep used to do) cost O(entries ever seen)
    // per frame per slot — a roaming camera fills the cache with the whole scene, so the frame got
    // slower the more of the scene it had ever visited, for a list that is usually empty. The list
    // is built from what the sync drew instead, so maintaining it costs O(drawn + undrawn).
    //
    // Its two halves also ARE the keys of `cache_`: an entry is created by the sync that draws its
    // geometry, dropped when nothing outside the caches holds the geometry any more, or cleared by
    // clearCache(). That is what lets the ownership pass read the lists instead of the cache (see
    // collectOwnedShares).
    std::vector<const vine::graphics::Geometry*> next;
    next.reserve(undrawn_.size() + drawn_.size());
    for (const auto* geometry : undrawn_) {
        if (seen.count(geometry) == 0) {
            next.push_back(geometry); // still undrawn
        }
    }
    for (const auto* geometry : drawn_) {
        if (seen.count(geometry) == 0 && !undrawn_set_.count(geometry)) {
            next.push_back(geometry); // just stopped being drawn
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
    undrawn_ = std::move(next);
    undrawn_set_.clear();
    undrawn_set_.insert(undrawn_.begin(), undrawn_.end());
    drawn_.assign(seen.begin(), seen.end());
}

bool SceneBridge::releaseAbandonedGeometries(const OwnedShareCounts& shares)
{
    // Judge the CANDIDATES (see updateUndrawnCandidates) by ONE rule: does anything outside the
    // caches still hold this geometry? An entry OWNS its key, so `useCount() <= shares` means every
    // remaining reference IS a retained entry — the app let go.
    //
    // Nothing else releases an entry: how long a geometry has gone undrawn says nothing about
    // whether it is still wanted. A culled, hidden or moved object is still held by its tree, a
    // subtree parked for reuse is still held by whoever parked it, and every one of those holders
    // is an OUTSIDE holder — which is exactly what useCount() counts and the shares do not. That is
    // what makes this cache's promise (a culled object stays cheap to bring back) true instead of
    // self-defeating, and it is why "the window expired" is not a reason to drop anything.
    if (undrawn_.empty()) {
        return false;
    }
    bool                                         changed = false;
    std::vector<const vine::graphics::Geometry*> still_undrawn;
    still_undrawn.reserve(undrawn_.size());
    for (const auto* geometry : undrawn_) {
        const auto it = cache_.find(geometry);
        if (it == cache_.end()) {
            continue; // its entry is already gone (the bridge was cleared)
        }
        if (!it->second.abandoned(shares)) {
            still_undrawn.push_back(geometry); // still held outside the caches
            continue;
        }
        // Dropping the item IS the release: its destructor parks the retained subtree (a submitted
        // command buffer may still name its pipeline) and its slot lease hands the per-draw slot back
        // to the pool's retired queue. Neither is spelled out here, so neither can be forgotten here
        // (see Item).
        cache_.erase(it);
        changed = true;
    }
    undrawn_ = std::move(still_undrawn);
    undrawn_set_.clear();
    undrawn_set_.insert(undrawn_.begin(), undrawn_.end());
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
    // been keeping undrawn since (see drawn_ / undrawn_), so the count does not walk the cache:
    // the frame's ownership pass must not cost more than the frame itself (see
    // releaseAbandonedGeometries).
    for (const auto* geometry : drawn_) {
        shares.add(geometry);
    }
    for (const auto* geometry : undrawn_) {
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

std::size_t SceneBridge::releaseAbandonedCaches(const OwnedShareCounts& shares)
{
    // Capacity trims happen on insert (the FIFO half of the bargain), which is
    // what bounds the chain of caches sharing one program; this is the prompt
    // half, shared by every program-keyed cache through OwnedCache.hpp so the
    // three of them cannot drift apart.
    const std::size_t erased = eraseAbandoned(program_stages_, shares) +
                               eraseAbandoned(program_shader_sets_, shares) +
                               eraseAbandoned(variant_cache_, shares);
    // The SESSION-scoped caches (uploaded textures, shared mesh binds) are NOT swept here: their entries
    // are shared by every slot, so a per-slot sweep multiplied the work by the slot count (and rebuilt a
    // share count per slot) to answer a question that does not change between slots. They are swept once
    // per frame by VsgRenderer::releaseAbandonedContent(), which runs after every slot's geometry sweep
    // — the ordering a texture's "the app dropped it and no retained entry holds it any more" judgement
    // needs, one level up from this sync.
    //
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
