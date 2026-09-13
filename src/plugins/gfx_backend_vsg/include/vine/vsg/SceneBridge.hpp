#pragma once
#include "vsg_global.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Command.h>
#include <vsg/commands/Commands.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/utils/ShaderSet.h>
#include <vsg/utils/SharedObjects.h>

#include <vine/raw_ptr.hpp>
#include <vine/graphics/DepthMode.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/vsg/OwnedCache.hpp>
#include <vine/vsg/VsgMaterialManager.hpp>
#include <vine/vsg/VsgRetireRing.hpp>
#include <vine/vsg/VsgDrawBlockPool.hpp>
#include <vine/vsg/VsgMeshResourceCache.hpp>
#include <vine/vsg/VsgTextureCache.hpp>

namespace vine::graphics
{
class Geometry;
class Material;
class Scene;
class Node;
class ShaderProgram;
struct RenderCommand;
struct ResolvedRenderState;
}

V_VSG_NS_BEGIN

/**
 * @brief Translates Vine render commands into a retained vsg scene graph.
 *
 * SceneBridge is the bridge between Vine's platform-independent scene graph
 * (Scene/Node/Drawable) and VulkanSceneGraph. Rendering is driven by the
 * per-frame render command list produced by Scene::collectRenderCommands:
 * syncRenderCommands() reconciles a stable vsg::Group against the commands so
 * drawables that move or change color only update their matrix / material in
 * place, and a vsg node (matrix transform -> state group -> vertex draw) is
 * only (re)built when a geometry first appears, disappears, or changes its
 * shape or material binding. This keeps runtime scene edits visible without
 * re-initializing the backend.
 */
class V_VSG_API SceneBridge {
  public:
    SceneBridge();
    ~SceneBridge();

    /** @brief Sets the shader set used to build per-geometry pipelines.
     *
     * @param shaderSet Shader set to use; defaults to flat shaded when unset.
     */
    void setShaderSet(::vsg::ref_ptr<::vsg::ShaderSet> shaderSet);

    /** @brief Whether the bridge's shader set reads the slot's own light block.
     *
     * The lights a slot must FEED depend on the set that draws it, not on the session:
     *  - our forward set takes the per-view `vine_lights` block the slot fills, so the slot
     *    builds no vsg light nodes for it;
     *  - every other set — the built-in vsg sets and user programs — shades from vsg's
     *    view-dependent light data, which only exists if the slot puts vsg light nodes under
     *    its view.
     *
     * The distinction matters exactly when a preset has no Vine program: the session's forward
     * switch is on, but THIS slot's set is the built-in one (see makeContentShaderSet), so the
     * slot must build the vsg lights after all — a session-level answer would leave that slot
     * unlit.
     *
     * @return true when the set reads the slot's `vine_lights` block.
     */
    bool hasOwnLightsBlock() const noexcept;

    /** @brief Sets the material manager used to obtain Phong resources.
     *
     * Must outlive the bridge. When unset, a default VsgMaterialManager is
     * created lazily.
     *
     * @param manager Material manager to use.
     */
    void setMaterialManager(vine::raw_ptr<VsgMaterialManager> manager);

    /** @brief Injects the texture-resource cache this bridge uploads through.
     *
     * Must outlive the bridge. The cache is keyed by the texture's ADDRESS and its entries OWN the
     * texture, so handing every content slot of a session the session's cache (see
     * `VsgRendererState::texture_cache`) means one texture sampled by N slots is staged ONCE and sampled
     * from one image, instead of each slot staging its own copy of the same pixels.
     *
     * When unset, the bridge creates (and owns) its own cache and uploads stay per slot.
     *
     * @param cache Texture cache to use.
     */
    void setTextureCache(vine::raw_ptr<VsgTextureCache> cache);

    /** @brief Injects the cache of SHARED mesh binds (see VsgMeshResourceCache).
     *
     * Must outlive the bridge. Handing every content slot of a session the session's cache means the streams
     * whose bytes are the model's own — positions, authored normals / texcoords / colours, custom channels
     * and the index stream — are bound through ONE command per stream, so N drawables reading them share one
     * device buffer and one upload instead of staging the same bytes N times.
     *
     * When unset, the bridge creates (and owns) its own cache: sharing then stays inside this bridge.
     *
     * @param cache Mesh-resource cache to use.
     */
    void setMeshResourceCache(vine::raw_ptr<VsgMeshResourceCache> cache);

    /** @brief Injects the session's pool of per-draw uniform slots (see VsgDrawBlockPool).
     *
     * Must outlive the bridge. Our forward set reads per-drawable values (`VineDrawBlock`:
     * the opacity today) from set 1, and the slots they live in come from this ONE pool, so
     * a scene's drawables share a handful of buffers and descriptor sets instead of owning
     * one each. Every slot's lifetime is the drawable's: the bridge reserves one when it
     * retains a geometry and returns it — deferred past the retire ring, because a frame in
     * flight may still bind the offset — when the geometry is evicted.
     *
     * Left unset, the bridge's drawables get NO per-draw block: our forward set would then
     * read the block's zeroed memory and draw nothing, so a caller that uses that set must
     * inject the pool (the session creates it next to its other device-backed caches).
     *
     * @param pool Slot pool to use.
     */
    void setDrawBlockPool(vine::raw_ptr<VsgDrawBlockPool> pool);

    /** @brief Counts one retained share per cache entry this bridge holds.
     *
     * Feeds the session's OwnedShareCounts (see releaseAbandonedCaches): a program
     * is held by up to three of this bridge's caches and a material by a variant
     * template of EVERY slot that draws it, so the number of retained shares is
     * data dependent and has to be counted rather than assumed.
     *
     * @param shares Counts to add to.
     */
    void collectOwnedShares(OwnedShareCounts& shares) const;

    /** @brief Ages the cached geometries that were not drawn and evicts the ones past the window.
     *
     * The CANDIDATES, not the whole cache: what can be absent is known — the geometries this slot
     * drew earlier and did not draw since (see updateAbsentCandidates) — so this walks that list
     * instead of every entry the slot has ever cached. See the notes in the .cpp for why that is
     * the difference between O(entries ever seen) and O(drawn + absent) per frame.
     *
     * Called once per frame by the session with the UNION of every slot's drawings (a geometry any
     * pass drew is not absent), and from syncRenderCommands with this slot's own drawings when no
     * session is driving — a test, or a driver that never opens a frame.
     *
     * @param drawn  Geometries drawn this frame (the session's union, or this slot's own).
     * @param shares Retained shares counted for the geometries this sweep judges.
     * @return true when anything was evicted.
     */
    bool ageAbsentItems(const std::unordered_set<const vine::graphics::Geometry*>& drawn,
                        const OwnedShareCounts& shares);

    /** @brief Provides the set this bridge reports the geometries it draws into (frame-scoped).
     *
     * The session clears it at the start of a frame, every slot's sync adds what it drew, and the
     * session then ages every slot by that union (ageAbsentItems). Set for the duration of one
     * frame and cleared with it, like the share counts.
     *
     * @param drawn Set to report into, or null to age within this bridge's own syncs.
     */
    void setFrameGeometrySet(std::unordered_set<const vine::graphics::Geometry*>* drawn) noexcept
    {
        frame_drawn_ = drawn;
    }

    /** @brief Provides the retained-share counts this bridge's sweep judges by.
     *
     * Set for the duration of one frame by the session that can count every slot's
     * shares, and cleared again before the frame returns, so the pointer never
     * outlives its counts. Unset (the default, and what a caller driving one bridge
     * directly gets) means the bridge counts what it can see itself — see
     * releaseAbandonedCaches.
     *
     * @param shares Session's counts, or null to count locally.
     */
    void setRetainedShares(const OwnedShareCounts* shares) noexcept { retained_shares_ = shares; }

    /** @brief Injects the per-view light block this bridge binds for its own forward shader set.
     *
     * Must outlive the bridge. The block is the slot's (not the session's): the
     * lights are per view, so a shared block would light the HUD's ambient-only
     * slot with the scene's sun. The slot writes it every frame and this bridge
     * only declares it in the pipeline layout / descriptor set of the variants
     * built from a ShaderSet that asks for `vine_lights` (our forward set); the
     * built-in vsg set does not, so nothing changes while it draws.
     *
     * When unset, a set declaring `vine_lights` gets no lights bound: the shader
     * would read an unbound descriptor, which is why the renderer always injects
     * one together with the forward set.
     *
     * @param data Uniform block holding a VineLightsBlock (see fillVineLightsBlock), or null.
     */
    void setLightsData(::vsg::ref_ptr<::vsg::Data> data);

    /** @brief Reconciles the retained scene under root against the commands.
     *
     * Each render command contributes one retained child (a vsg::MatrixTransform
     * holding the geometry state group). Existing children are updated in place;
     * a child is (re)built when its geometry has no cached node or its shape /
     * material binding changed, and children whose geometry is no longer drawn
     * are dropped.
     *
     * @param commands Render commands for the current frame.
     * @param root     Stable vsg root group the retained children live under.
     * @param created  Optional: receives the subtrees newly built this frame
     *                 (they still need GPU compilation before recording).
     * @return true when the graph changed structurally; only newly built
     *         subtrees in @p created require compilation.
     */
    bool syncRenderCommands(
        const std::vector<vine::graphics::RenderCommand>& commands,
        ::vsg::Group* root,
        std::vector<::vsg::ref_ptr<::vsg::Node>>* created = nullptr);
    /** @brief Releases all retained per-geometry vsg nodes. */
    void clearCache();

    /** @brief Sets the pass-level depth policy for content that does not set
     * depth itself.
     *
     * A pass declares how its content treats the target's depth
     * (RenderPass::depthMode): TestAndWrite for opaque scene content, TestOnly
     * for translucent content that tests without writing, Disabled for HUD
     * content drawn on top. The policy fills the depth item of every command
     * that did not author one (RenderCommand::depthExplicit false); a
     * StateNode that sets depth explicitly keeps winning, as it is the
     * finer-grained intent.
     *
     * Changing the policy re-derives the depth state of every command, so call
     * invalidateState() afterwards to rebuild the retained state wrappers
     * (the vertex data is untouched).
     *
     * @param mode Depth handling for this slot's content.
     */
    void setContentDepthMode(vine::graphics::DepthMode mode);

    /** @brief Gets the pass-level depth policy (see setContentDepthMode). */
    [[nodiscard]] vine::graphics::DepthMode contentDepthMode() const noexcept { return content_depth_mode_; }

    /** @brief Drops the retained state wrappers so the next sync rebuilds them.
     *
     * The retained subtree is split into vertex DATA (arrays + bind/draw
     * commands) and STATE (pipeline + descriptor binds). Dropping only the
     * state wrappers makes the next syncRenderCommands() rebuild the pipelines
     * for every geometry while reusing the uploaded mesh, which is what a
     * pass-level state change (a new depth policy, a swapped shader set) needs.
     */
    void invalidateState();

    /** @brief Installs the sink that receives this bridge's diagnostics.
     *
     * A bridge reports every request it could not serve — a rejected geometry,
     * a dropped channel, a program that fell back to the built-in shader —
     * instead of degrading silently, so the renderer can forward it to the
     * host's RenderBackend sink. The bridge also keeps its stderr tracing, so
     * installing no sink loses nothing but the programmatic channel.
     *
     * @param sink Callback invoked for every diagnostic, or empty to clear.
     */
    void setDiagnosticSink(vine::graphics::DiagnosticSink sink);

    /** @brief Gets how many diagnostics this bridge has reported. */
    std::size_t diagnosticCount() const noexcept { return diagnostic_count_; }

    /** @brief Gets how many diagnostics of @p category were reported.
     *
     * @param category Category to count.
     * @return Number of reported diagnostics in that category.
     */
    std::size_t diagnosticCount(vine::graphics::DiagnosticCategory category) const noexcept;

    /** @brief Parks a node whose Vulkan objects may still be in flight.
     *
     * Replacing a retained data/state node drops the old one, but the GPU may
     * still be executing command buffers that reference its pipeline, buffers
     * or descriptor sets (the viewer keeps several command-buffer slots in
     * flight). Destroying them then is a validation error
     * (VUID-vkDestroyPipeline-00765 and friends) and can fault the device, so
     * the node is parked here and released by advanceRetireRing() once every
     * slot that could reference it has been re-recorded (which waits on its
     * fence first).
     *
     * @param node Node to park (null is ignored).
     */
    void retireNode(::vsg::ref_ptr<::vsg::Node> node);

    /** @brief Releases the nodes retired one ring cycle ago.
     *
     * Must be called exactly once per SUBMITTED frame — after that frame's
     * recordAndSubmit() — because one advance is what accounts for one
     * submission's fence wait (see retireNode). The ring itself, its depth and its policy are
     * VsgRetireRing.
     */
    void advanceRetireRing();

    /** @brief Gets the number of distinct compiled pipeline variants.
     *
     * Counts how many genuinely distinct vsg::GraphicsPipeline objects this
     * bridge registered with its shared-objects cache. Geometries that
     * resolve to the same (shader, render state, subpass) variant share one
     * pipeline, so loading N geometries whose variant count stays far below N
     * confirms pipeline sharing is collapsing duplicates (pipeline count
     * follows state variants, not geometry count).
     *
     * @return Number of distinct pipeline variants built so far.
     */
    std::size_t pipelineVariantCount() const noexcept { return pipeline_variants_; }

    /**
     * @brief States the anisotropy every sampler this bridge's cache creates may request.
     *
     * A device limit AND a device feature, so the side that owns the device announces it instead of this
     * bridge assuming a number: a request above what the device reports is a validation error. Forwarded
     * rather than exposing the cache, because the cache is an implementation detail of the bridge.
     *
     * @param device_limit The device's reported maxSamplerAnisotropy.
     */
    void setTextureAnisotropy(float device_limit);

    /** @brief Gets how many times the shared-objects table was pruned.
     *
     * A variant registered with the shared-objects cache is HELD by that table,
     * so a variant the retained caches evicted left its pipeline (and layout,
     * descriptor sets) behind: the table only ever grew, which is what let it
     * outlive the caches whose bounds are supposed to cap memory. The table is
     * now pruned on the frames that evicted something, with vsg's own rule
     * (drop the entries nothing else references — the same useCount() <= 1 test
     * the caches use). This counts those prunes, so a test can tell a table that
     * still pinned an evicted variant's pipeline from one that let it go.
     *
     * @return Number of shared-objects prunes so far.
     */
    std::size_t sharedPruneCount() const noexcept { return shared_prune_count_; }

    /** @brief Records that a capacity trim of a program-keyed cache evicted an
     * entry, so the next releaseAbandonedCaches() prunes the shared table.
     *
     * The table holds what it registers, so an evicted variant leaves its
     * pipeline behind; the FIFO trims happen at the insert sites (they cannot
     * wait for the end-of-pass sweep, which is what keeps the caches bounded),
     * so they report here instead of pruning on their own — pruning belongs to
     * the one place that can see every cache of this bridge.
     */
    void noteEviction() noexcept { ++pending_evictions_; }

    /** @brief Gets how many times geometry reused a cached pipeline variant.
     *
     * Incremented whenever buildGeometry reuses an already-built (program,
     * material, render-state) template instead of running a fresh
     * GraphicsPipelineConfigurator. A load whose reuse count is close to its
     * geometry count (minus the distinct variants) confirms the L2 fast path
     * is collapsing repeated variant setup.
     *
     * @return Number of variant-template reuses so far.
     */
    std::size_t variantReuseCount() const noexcept { return variant_reuses_; }

    /** @brief Gets how many times this bridge ran the glslang stage compiler.
     *
     * The compiler runs once per (program, content revision) — not per vertex
     * layout — so one program used with several custom-channel layouts counts
     * a single compile, proving the L1a stage cache is shared across its
     * layouts (only the per-layout ShaderSet assembly is repeated).
     *
     * @return Number of glslang compile passes started.
     */
    std::size_t programStageCompileCount() const noexcept { return program_stage_compiles_; }

    /** @brief Gets the number of retained geometry entries.
     *
     * The observable half of this bridge's geometry retention policy: an entry exists from the sync
     * that first drew its geometry until the app releases it (released at once) or the geometry
     * has been absent for the whole reuse window. It is also what makes the candidate list
     * assertable — the list has to cover exactly these entries, and an entry it missed would never
     * be evicted, so this count would never fall.
     *
     * @return Number of cached geometries.
     */
    std::size_t retainedGeometryCount() const noexcept { return cache_.size(); }

    /** @brief Gets the number of textures with cached GPU resources.
     *
     * The observable half of this bridge's texture cache (the session's when one was injected, see
     * setTextureCache): a texture the scene stopped sampling — or one whose last retained entry just left
     * the frame — must drop out of this count, instead of keeping its GPU image until 256 later textures
     * push it out of the FIFO or the slot is destroyed. It is what makes that sweep assertable without a
     * device.
     *
     * @return Number of cached textures (the shared white fallback is not counted).
     */
    std::size_t textureCount() const noexcept;

  private:
    /** @brief One forwarded custom vertex channel (location >= 3).
     *
     * Describes an extra per-vertex attribute carried past the canonical
     * 0=position / 1=normal / 2=colour arrays: its geometry location and its
     * AttributeBuffer components (the stride its packed floats use). */
    struct VertexChannel
    {
        std::uint32_t location = 0;
        std::uint32_t components = 0;
        bool operator==(const VertexChannel& o) const
        {
            return location == o.location && components == o.components;
        }
    };

    /** @brief The bind commands a built data node holds, kept so an edit can refresh ONE stream.
     *
     * A data node binds one Canonical channel per command (binding 0..3) plus one more for the custom
     * channels, instead of one command holding every array. The reason is vsg's re-copy granularity:
     * `BindVertexBuffers::compile()` re-creates and re-copies EVERY array of a command whose ANY array is
     * stale, so a single command can only ever re-upload the whole mesh. One command per channel makes the
     * unit of re-upload one channel — the index stream already worked that way.
     *
     * The binding numbers stay the explicit `firstBinding` of each command (0..3 canonical, 4+ custom), so
     * what the shader sees does not change with the split.
     */
    struct RetainedBinds
    {
        /** @brief Canonical binding index of @p location, or kNoBinding when it is not a canonical one. */
        static std::size_t canonicalBindingOf(std::uint32_t location) noexcept
        {
            switch (location)
            {
                case 0u: return 0u;                                                                     // positions
                case 1u: return 1u;                                                                     // normals
                case vine::graphics::Geometry::kTexCoordLocation: return 2u;                             // texcoords
                case 2u: return 3u;                                                                     // loc2 colour
                default: return kNoBinding;
            }
        }

        /** @brief Sentinel: the location is not one of the four canonical ones. */
        static constexpr std::size_t kNoBinding = ~std::size_t{ 0 };
        /** @brief Sentinel: the bind is not in @ref commands (nothing to swap). */
        static constexpr std::size_t kNoChild = ~std::size_t{ 0 };
        /** @brief Number of canonical vertex bindings (positions, normals, texcoords, loc2 colour). */
        static constexpr std::size_t kCanonicalCount = 4u;

        // The command list the binds below are children of, so a refresh can swap ONE of them in place
        // instead of re-pointing the command every geometry shares.
        ::vsg::ref_ptr<::vsg::Commands> commands;
        // One bind per canonical binding index (see canonicalBindingOf).
        std::array<::vsg::ref_ptr<::vsg::BindVertexBuffers>, kCanonicalCount> canonical;
        // Where each canonical bind sits in @ref commands, so the swap keeps the command order (and with it
        // the binding numbers) intact.
        std::array<std::size_t, kCanonicalCount> canonical_child{ kNoChild, kNoChild, kNoChild, kNoChild };
        // Whether each canonical bind came from the shared mesh cache. A SHARED bind must never be mutated:
        // an in-place refresh routes the new bytes through the cache again instead, so the other geometries
        // keep reading what they bound (see the refresh path in syncRenderCommands).
        std::array<bool, kCanonicalCount> canonical_shared{};
        // The index stream's bind (its own command: the in-place path swaps it).
        ::vsg::ref_ptr<::vsg::BindIndexBuffer> index;
        // Where @ref index sits in @ref commands (same reason as canonical_child).
        std::size_t index_child = kNoChild;
        // Whether @ref index came from the shared mesh cache (same rule as above).
        bool index_shared = false;
    };

    /** @brief Retained per-geometry render node (defined in the .cpp). */
    struct Item;

    /** @brief Identity of one vertex channel, as the retained data was built from it.
     *
     * A rebuild has to know WHICH stream changed, not just that something did: a channel whose buffer, byte
     * revision, location, component count and SLICE are the same IS the same data, so the array the retained
     * node holds for it is still correct. The slice matters because one arena buffer may hold several
     * geometries' vertices (see AttributeBuffer::offset): two segments of it are two streams. The revision is
     * `vine::Buffer::revision()` — the contract a consumer that cached bytes compares against (the same rule
     * Texture and ShaderProgram follow).
     */
    struct ChannelKey
    {
        std::uint32_t location = 0;
        std::uint32_t components = 0;
        const void*   buffer = nullptr;
        std::uint64_t revision = 0;
        std::size_t   offset = 0;
        std::size_t   count = 0;

        bool operator==(const ChannelKey& other) const
        {
            return location == other.location && components == other.components && buffer == other.buffer &&
                   revision == other.revision && offset == other.offset && count == other.count;
        }
    };

    /** @brief Snapshots the identity of every vertex channel @p geometry carries.
     *
     * The walk is the one the data builder makes — ascending location, canonical and custom channels alike —
     * so two equal snapshots mean every stream the builder read is byte-for-byte the same one. A channel the
     * builder rejects still appears here: a snapshot that cannot tell that case apart only costs a rebuild,
     * never a stale reuse.
     *
     * @param geometry Geometry to snapshot.
     * @return One key per channel, in ascending location order.
     */
    static std::vector<ChannelKey> channelKeysOf(const vine::graphics::Geometry& geometry);

    /** @brief Whether two channel snapshots describe the same channels with the same SHAPE.
     *
     * Shape is location, component count and element count — what decides how the node is assembled (the
     * bindings, the array types, the derived channels' sizes). The bytes (buffer pointer and revision) are
     * deliberately NOT compared: two snapshots that differ only there are exactly the case an in-place
     * refresh serves.
     *
     * @param before Snapshot the retained node was built from.
     * @param after  Current snapshot.
     * @return true when every channel is present in both with the same shape.
     */
    static bool shapesMatch(const std::vector<ChannelKey>& before, const std::vector<ChannelKey>& after);

    /** @brief Whether the two snapshots describe the SAME streams.
     *
     * Identity, not shape: every channel in @p before must be present in @p after reading the same buffer at
     * the same revision with the same element count and the same component count. This is what tells an
     * announcement a stream can account for (the caller replaced a buffer, or refilled one and bumped its
     * revision) from one no stream explains — a buffer whose bytes moved without its own revision moving,
     * which only the geometry-level revision reports (see Geometry::setRevision).
     *
     * @param before Snapshot the retained node was built from (empty when nothing was built yet).
     * @param after  Current snapshot.
     * @return true when both describe the same streams.
     */
    static bool streamsMatch(const std::vector<ChannelKey>& before, const std::vector<ChannelKey>& after);

    /** @brief Finds the snapshot entry for @p location.
     *
     * @param keys     Snapshot to search (ascending location order).
     * @param location Channel location to find.
     * @return The entry, or null when the snapshot has no such channel.
     */
    static const ChannelKey* keyAt(const std::vector<ChannelKey>& keys, std::uint32_t location);

    /** @brief Snapshots the identity of the geometry's index stream.
     *
     * A geometry without indices yields a default-constructed key (null buffer), which is what distinguishes
     * "no index stream" from "an index stream whose bytes changed". The key covers the DRAWN span (first
     * index and count), not the buffer's length: an index arena holds several geometries' indices, and a
     * geometry that draws a different span draws different triangles.
     *
     * @param geometry Geometry to snapshot.
     * @return Key of the drawn index range, or a null-buffer key when there is none.
     */
    static ChannelKey indexKeyOf(const vine::graphics::Geometry& geometry);

    /** @brief The shared-cache key of the geometry's index stream: the WHOLE buffer the bind aliases.
     *
     * Not to be confused with @ref indexKeyOf, which describes what this geometry DRAWS. The bind holds the
     * whole buffer (the draw states the span), so every geometry slicing one index arena resolves to one
     * entry — which is what makes the arena share one index upload.
     *
     * @param geometry Geometry whose index buffer to key.
     * @return Key of the bound index stream (a null-buffer key when there are no indices).
     */
    static VsgMeshResourceCache::ChannelKey indexBindKeyOf(const vine::graphics::Geometry& geometry);

    /** @brief The index array a retained node binds: the model's WHOLE index buffer, aliased in place.
     *
     * The slice is the DRAW's business (first index / count, see Geometry::setIndices), which keeps the
     * bound array — and therefore the device copy — the same for every geometry reading that arena.
     *
     * @param geometry Geometry whose index buffer to alias.
     * @return Array reading the buffer in place, or null when the geometry has no index buffer.
     */
    static ::vsg::ref_ptr<::vsg::uintArray> boundIndexArray(const vine::graphics::Geometry& geometry);

    /** @brief Puts @p replacement where a retained bind command sits in its own command list.
     *
     * Refreshing a SHARED stream must not re-point the bind the drawable was built with: that command
     * belongs to every geometry reading the same bytes, so the fresh one goes in at the SAME child slot
     * instead (see RetainedBinds). Keeping the slot preserves the order the builder wrote, which is the order
     * the shader contract's binding numbers were checked against.
     *
     * @param binds       Retained binds whose commands list holds the slot.
     * @param child       Child slot to replace (kNoChild when the bind is not in a list).
     * @param replacement Bind (a Command) to put there, or null to leave the list unchanged.
     */
    static void swapRetainedChild(RetainedBinds& binds, std::size_t child, ::vsg::Command* replacement);

    /** @brief The BUILD vertex channels a data rebuild reuses instead of recomputing.
     *
     * A rebuild re-materialises the whole data node, but three of its channels do not come from the model:
     * the white colour carrier (the built-in path's opacity carrier), the zero UV array a mesh without UVs
     * binds, and the normals DERIVED when the geometry authors none. Each costs a pass over the vertices
     * (plus a fresh allocation) on every rebuild — even a rebuild that changed none of its inputs. This
     * remembers them per retained item, keyed by exactly the inputs they were derived from:
     *
     *   * white colours / zero UVs: the vertex count alone;
     *   * derived normals: the positions and index streams they were read from — buffer, revision and slice
     *     (`vine::Buffer::revision()` is the contract a consumer that cached derived bytes compares against,
     *     the same rule Texture and ShaderProgram follow; the slice is what makes two segments of one arena
     *     two different input sets).
     *
     * Reusing the ARRAY OBJECT does not avoid re-uploading it (a new data node builds new vsg BufferInfos,
     * and vsg re-copies whatever a command binds); what it avoids is the CPU work and the allocation. Only
     * the bridge's own rebuild path sees this, because it is the only one that knows which inputs changed.
     */
    struct DerivedChannels
    {
        /** @brief Sentinel for "no count remembered yet". */
        static constexpr std::size_t kUnsetCount = ~std::size_t{ 0 };
        /** @brief Sentinel for "no revision remembered yet". */
        static constexpr std::uint64_t kUnsetRevision = ~std::uint64_t{ 0 };

        // White colour carrier and zero UVs: a function of the vertex count alone, so a count change drops
        // both rather than letting either be reused at the wrong size.
        std::size_t                      count = kUnsetCount;
        ::vsg::ref_ptr<::vsg::vec4Array> white_colors;
        ::vsg::ref_ptr<::vsg::vec2Array> zero_texcoords;
        // Derived normals: the positions and index streams they were computed from, plus the revisions that
        // announce those bytes changed. The slices are part of the identity: one arena holds several
        // geometries' vertices, so the same buffer at a different offset is a different input set.
        const vine::Buffer<float>*         normal_positions = nullptr;
        std::uint64_t                      normal_positions_revision = kUnsetRevision;
        std::size_t                        normal_positions_offset = kUnsetCount;
        const vine::Buffer<std::uint32_t>* normal_indices = nullptr;
        std::uint64_t                      normal_indices_revision = kUnsetRevision;
        std::size_t                        normal_indices_first = kUnsetCount;
        std::size_t                        normal_indices_count = kUnsetCount;
        std::size_t                        normal_vertex_count = kUnsetCount;
        ::vsg::ref_ptr<::vsg::vec3Array>   derived_normals;
    };

    /** @brief Reports one diagnostic to the installed sink and counts it.
     *
     * Also writes the message to stderr: that is this backend's built-in
     * tracing (the validation harness reads it), so routing a rejection site
     * through here keeps today's out-of-band behaviour and adds the
     * programmatic channel. Failing to draw something must never be silent.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, with the numbers involved.
     */
    void report(vine::graphics::DiagnosticSeverity severity,
                vine::graphics::DiagnosticCategory category, const vine::String& message);

    /** @brief Builds ONE canonical channel's array for an in-place refresh, or null when it cannot.
     *
     * The counterpart of buildGeometryData() for a single channel: it reproduces exactly what that builder
     * would bind for @p location — aliasing the model's bytes, or re-deriving the normals a positions edit
     * invalidated — but only when the node's SHAPE did not change. Returning null means "this edit needs the
     * full rebuild" (an unpacked layout, an unusable channel, a different vertex count), which is also what
     * keeps the builder the single authority for the general case, diagnostics included.
     *
     * @param geometry     Geometry the channel belongs to.
     * @param location     Channel location (0, 1, 2, or Geometry::kTexCoordLocation).
     * @param vertex_count Vertices the retained node was built with (the shape is unchanged).
     * @param topology     Topology the node was built with (decides whether normals are derived).
     * @param derived      Derived-channel cache to keep in step when normals are re-derived.
     * @return The array to bind now, or null when the caller must rebuild the node.
     */
    static ::vsg::ref_ptr<::vsg::Data> refreshCanonicalChannel(
        vine::raw_ptr<const vine::graphics::Geometry> geometry,
        std::uint32_t                                 location,
        std::size_t                                   vertex_count,
        vine::graphics::Topology                      topology,
        DerivedChannels&                              derived);

    /** @brief Builds (or rebuilds) the retained vertex-data node of a geometry.
     *
     * Materialises the geometry's attribute buffers into vsg arrays and wraps
     * them in bind/draw commands. The node is geometry-data only: it carries
     * no pipeline, so it is reused verbatim across material / render-state /
     * program changes (only the state wrapper is rebuilt then), and it stays
     * stable so a later geometry-only edit never re-uploads unchanged meshes.
     * The index stream is kept verbatim (bounds-checked, never truncated):
     * primitive assembly is the topology's job, not the data builder's. When
     * @p opacity_carrier is true (built-in path), the per-vertex colour array
     * is marked DYNAMIC and returned via @p out_colors so per-drawable
     * opacity edits after upload are re-transferred on dirty().
     *
     * @param geometry        Geometry to build.
     * @param opacity_carrier True when the caller drives per-drawable opacity through the vertex-colour
     *                        alpha. That is the BUILT-IN path: its shader reads the vertex colour's alpha
     *                        and there is no per-draw block to carry the opacity. Our forward set passes
     *                        false — its `vine_draw` block holds the opacity, so its colour array is a
     *                        static authored-or-white channel that is uploaded once.
     * @param topology        Primitive topology the geometry is drawn with:
     *                        automatic normal derivation runs for Triangles
     *                        only; Points / Lines fall back to authored
     *                        normals or a constant default.
     * @param out_colors      Receives the per-vertex colour array the caller
     *                        keeps to drive opacity each frame (null when
     *                        @p opacity_carrier is false).
     * @param extra_channels  Receives one entry per forwarded custom channel
     *                        (location >= 3), in binding order after the three
     *                        canonical arrays (ascending location).
     * @param derived         Cache of the channels this builder DERIVES (white colour carrier, zero UVs,
     *                        derived normals): reused when the inputs they were derived from did not
     *                        change, so an unrelated edit does not pay for them again. See DerivedChannels.
     * @param out_index_bind  Receives the index bind command the node holds, so a later index-only edit can
     *                        replace that stream IN PLACE — its own BufferInfo is what vsg re-creates and
     *                        copies, one channel instead of the whole mesh (P6).
     * @param out_binds       Receives the per-channel vertex binds (and the index bind) the node holds, so a
     *                        later edit can refresh ONE channel through its own command.
     * @param mesh_cache      Cache of shared binds for the channels whose bytes are the model's own, or null
     *                        to build every bind privately. Null is what a REBUILD announces when its
     *                        revision is not explained by any stream's identity: the retained shared bind was
     *                        copied from the bytes as of ITS insertion, so a build that cannot attribute
     *                        what moved has to take its own copies (see VsgMeshResourceCache).
     * @return Data commands node, or null when not buildable.
     */
    ::vsg::ref_ptr<::vsg::Commands> buildGeometryData(
        vine::raw_ptr<const vine::graphics::Geometry> geometry,
        bool opacity_carrier,
        vine::graphics::Topology topology,
        ::vsg::ref_ptr<::vsg::vec4Array>& out_colors,
        std::vector<VertexChannel>& extra_channels,
        DerivedChannels& derived,
        RetainedBinds& out_binds,
        vine::raw_ptr<VsgMeshResourceCache> mesh_cache);

    /** @brief Builds (or rebuilds) the state wrapper around a data node.
     *
     * The wrapper is a vsg::StateGroup carrying the pipeline + descriptor-set
     * binds for one (program, material, resolved-state, vertex-layout) variant;
     * @p data is attached as its child by the caller. Pipelines are resolved
     * through the per-(program, layout) ShaderSet cache and the per-variant L2
     * template cache, so repeated variants skip the configurator entirely. The
     * forwarded custom channels (locations >= 3) are bound after the canonical
     * arrays and named vine_Attribute{location}.
     *
     * @param data           The retained vertex-data node to wrap (non-null).
     * @param material       Bound material (may be null).
     * @param texture        Texture the material samples, or null when it has none (the shared white
     *                       fallback is bound instead, so the shader always samples something).
     * @param state          Resolved render state the pipeline must honour.
     * @param program        User shader program, or null for the built-in
     *                       default.
     * @param extra_channels Custom channels carried by @p data (locations >= 3),
     *                       in binding order after the canonical arrays.
     * @param derived        Channels the data builder DERIVED (the white colour carrier, the zero UVs),
     *                       or null when the caller cannot say. Used only to decide whether OUR forward
     *                       set may take the variant WITHOUT a canonical attribute: a derived array (the
     *                       geometry authored none) is dropped, an authored one is never.
     * @param draw_slot      The drawable's slot in the per-draw block pool, or an invalid Slot when it has
     *                       none. The slot is NOT part of the variant identity (its values are rewritten in
     *                       place), so what the wrapper records is the slot's OFFSET: one shared
     *                       descriptor set per pool chunk, bound with this drawable's dynamic offset.
     * @return State wrapper, or null when not buildable.
     */
    ::vsg::ref_ptr<::vsg::StateGroup> buildStateGroup(
        ::vsg::ref_ptr<::vsg::Node> data,
        vine::raw_ptr<vine::graphics::Material> material,
        vine::raw_ptr<const vine::graphics::Texture> texture,
        const vine::graphics::ResolvedRenderState& state,
        vine::raw_ptr<const vine::graphics::ShaderProgram> program,
        const std::vector<VertexChannel>& extra_channels,
        const DerivedChannels* derived = nullptr,
        VsgDrawBlockPool::Slot draw_slot = {});

    /** @brief Gets (and caches) the run-time compiled ShaderSet for a program.
     *
     * Compiles the program's stages and assembles a ShaderSet once per
     * (program, vertex layout) instead of once per geometry: N geometry bound
     * to the same program AND carrying the same set of custom channels share a
     * single glslang compile and ShaderSet. Besides the canonical
     * vsg_Vertex/Normal/Color bindings (locations 0/1/2), the set declares one
     * vine_Attribute{location} binding per forwarded custom channel, whose
     * format follows its components. A compile/assembly failure is cached too
     * (null), so later geometry does not retry the failed compile each time.
     *
     * @param program        User program (non-null).
     * @param extra_channels Custom channels (locations >= 3) the set must
     *                       declare, in binding order.
     * @return Compiled shader set, or null when it could not be built.
     */
    ::vsg::ref_ptr<::vsg::ShaderSet> getProgramShaderSet(
        vine::raw_ptr<const vine::graphics::ShaderProgram> program,
        const std::vector<VertexChannel>& extra_channels);

    /** @brief Gets the material manager used to obtain Phong resources.
     *
     * Falls back to the bridge-owned default manager when the renderer never
     * injected one (setMaterialManager). Both callers that need material
     * resources route through here so the fallback is decided once.
     *
     * @return The active material manager (always non-null).
     */
    VsgMaterialManager& materialManager();

    /** @brief Gets the cache that uploads the textures the scene samples.
     *
     * @return The texture cache (always non-null / usable).
     */
    VsgTextureCache& textureCache();

    /** @brief Gets the mesh-resource cache in use (the injected one, or this bridge's own). */
    VsgMeshResourceCache& meshResources();

    /** @brief Gets the slot pool per-drawable values are written to (see setDrawBlockPool).
     *
     * @return The injected pool, or null when the caller injected none.
     */
    VsgDrawBlockPool* drawBlockPool();

    /** @brief Gets the slot's base shader set (the built-in default when unset).
     *
     * The built-in Phong set is created lazily on first use and cached, so
     * the bridge never pays for a fresh createPhongShaderSet() per geometry.
     * A user program path builds on top of this set's default pipeline states
     * (the baked viewport / blending), keeping both paths on one material
     * descriptor ABI.
     *
     * @return The base shader set (always non-null).
     */
    ::vsg::ref_ptr<::vsg::ShaderSet> baseShaderSet();


    /** @brief Drops retained cache entries nothing but their own cache holds.
     *
     * Every retained cache here owns the object it keys on (see OwnedCache.hpp),
     * so an entry the app has let go of is released instead of pinning that
     * program's SPIR-V, its assembled ShaderSet and the cached bind commands for
     * the rest of the session. Because several caches may share one program
     * (the stage cache, the per-layout ShaderSet cache and a variant template),
     * an entry may only go when the ONLY references left to its key are the
     * retained entries that hold it — which is a number, not a guess: see
     * OwnedShareCounts and P11.
     *
     * Judging happens by the shares the sweep was handed (setRetainedShares), or
     * by the shares this bridge can count itself when the renderer handed none:
     * its own caches plus the material manager's, which is complete for a bridge
     * whose objects no other slot also holds. A SESSION handed
     * collectOwnedShares() over every slot is what makes the judgement exact
     * when two slots draw the same material.
     *
     * The per-geometry cache has its own sweep inline (it also applies the reuse
     * window), so it is not part of this.
     *
     * The TEXTURE cache is swept here as well: it is this sweep's only caller, and a texture the scene
     * stopped sampling — or one whose last retained entry just left the frame — must not keep its GPU image
     * until 256 later textures push it out of the FIFO.
     *
     * @return Number of erased entries.
     */
    std::size_t releaseAbandonedCaches();

    /** @brief Evicts retained items the frame no longer draws.
     *
     * The tail half of syncRenderCommands(): hiding a node / frustum culling
     * stays cheap (the retained node is detached from the root and reused when
     * it reappears), while an entry the app itself released — or one absent
     * past the reuse window — is dropped, its subtree parked on the retire ring
     * because an in-flight command buffer may still reference it.
     *
     * @param seen Geometries drawn this frame.
     * @return true when anything was evicted.
     */
    /**
     * @brief Parks @p slot for release once the frames that could bind it are accounted for.
     *
     * @param slot Slot whose drawable is gone (an invalid slot or a bridge without a pool
     *             is a no-op).
     */
    /**
     * @brief Appends the drawable's per-draw bind of set 1 (see VsgDrawBlockPool).
     *
     * The variant's shared commands cannot carry it: the bind names the drawable's slot as a
     * dynamic offset, so it is per drawable while the descriptor set it selects from is one per
     * (pool chunk, set layout). Called for both the fresh-build and the cached-template path.
     *
     * @param state_group     Wrapper being assembled (the bind is appended last).
     * @param pipeline_layout The variant's pipeline layout (null or set-less for the sets that
     *                        declare no per-draw block: nothing is appended then).
     * @param draw_slot       The drawable's slot (an invalid slot appends nothing).
     */
    void appendDrawBlockBind(::vsg::StateGroup& state_group,
                             ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout,
                             VsgDrawBlockPool::Slot draw_slot);

    void releaseDrawSlot(VsgDrawBlockPool::Slot slot);

    /**
     * @brief Returns the parked slots whose ring advances have elapsed to the pool.
     *
     * Called once per submitted frame, next to the retire ring's own advance. Nothing here
     * runs from the destructor: the pool is session-scoped and injected, so it OUTLIVES the
     * bridge (the same contract the texture / mesh caches have), and a bridge whose slot is
     * torn down has already returned its slots through clearCache() — which is why the
     * destructor must not touch the pool at all.
     */
    void flushDrawSlots();

    /** @brief Ring advances a released slot waits before the pool may hand it out again. */
    static constexpr std::uint32_t kDrawSlotRetireFrames = static_cast<std::uint32_t>(VsgRetireRing::kRetireRingDepth);

    /** @brief Rebuilds the absent candidate list from this sync's drawings.
     *
     * The list is what ageAbsentItems walks and (with last_seen_) the keys of the geometry cache,
     * so it is maintained on every sync whether or not this bridge ages itself.
     *
     * @param seen Geometries drawn by this sync.
     */
    void updateAbsentCandidates(const std::unordered_set<const vine::graphics::Geometry*>& seen);

    /** @brief The share-aware half of releaseAbandonedCaches (see it for the rule).
     *
     * @param shares Retained shares counted for the objects this sweep judges.
     * @return Number of erased entries.
     */
    std::size_t releaseAbandonedCaches(const OwnedShareCounts& shares);

    /** @brief Counts this bridge's own shares PLUS the material manager's.
     *
     * What a bridge can say on its own: the caches it holds, and the manager it draws through. It
     * is the complete picture only while no OTHER slot holds the same objects, which is why a
     * session hands its counts in instead (see setRetainedShares).
     *
     * @param shares Counts to fill.
     */
    void collectSweepShares(OwnedShareCounts& shares);

    /** @brief Republishes the retained children in command order and refreshes
     * the materials the frame drew.
     *
     * The command list is already sorted by the caller, so the retained
     * children are re-ordered to match it (a no-op when the order is unchanged)
     * and every distinct material the frame draws is refreshed through the
     * material manager's single compare-and-write path (D19), which keeps a
     * steady frame transfer-free while an edit lands immediately.
     *
     * @param root     Stable vsg root the retained children live under.
     * @param visible  Retained transforms in draw order.
     * @param commands Commands drawn this frame (for the material refresh).
     */
    void publishRetainedChildren(::vsg::Group& root,
                                 const std::vector<::vsg::ref_ptr<::vsg::Node>>& visible,
                                 const std::vector<vine::graphics::RenderCommand>& commands);

    ::vsg::ref_ptr<::vsg::ShaderSet> shader_set_;
    // Whether shader_set_ is OUR forward set (the one that drops a DERIVED canonical
    // attribute behind its define and reads per-drawable values from `vine_draw`).
    // Its per-drawable opacity rides the draw block, not the vertex colour: the colour
    // array it builds is a static authored-or-white channel, so the bridge does not
    // keep one to rewrite and the data builder is told not to make one dynamic.
    bool forward_draw_block_ = false;
    // Pass-level depth policy applied to commands that did not author depth
    // (see setContentDepthMode); part of the retained state identity, so
    // changing it invalidates the state wrappers.
    vine::graphics::DepthMode content_depth_mode_ = vine::graphics::DepthMode::TestAndWrite;
    // Shares layout / pipeline / descriptor-set content across every geometry
    // this bridge builds: GraphicsPipelineConfigurator::copyTo() deduplicates
    // through SharedObjects (content equality), so geometry that resolves to
    // the same pipeline state and material registers ONE vsg::GraphicsPipeline
    // and descriptor set instead of one per geometry.
    ::vsg::ref_ptr<::vsg::SharedObjects> shared_objects_;
    // Distinct pipeline variants registered with shared_objects_ (diagnostic;
    // see pipelineVariantCount()).
    std::size_t pipeline_variants_ = 0;
    // Number of prunes of shared_objects_ (see sharedPruneCount()).
    std::size_t shared_prune_count_ = 0;
    // Evictions reported by the capacity trims since the last prune
    // (see noteEviction()).
    std::size_t pending_evictions_ = 0;
    // Times buildGeometry reused a cached (program, material, state) template
    // instead of running a fresh configurator (diagnostic; see
    // variantReuseCount()).
    std::size_t variant_reuses_ = 0;
    // Fresh glslang stage compiles started (L1a; diagnostic — see
    // programStageCompileCount()).
    std::size_t program_stage_compiles_ = 0;
    vine::raw_ptr<VsgMaterialManager> material_manager_ = nullptr;
    // Default manager used when the renderer does not inject one.
    VsgMaterialManager default_manager_;
    // Texture uploads, keyed by texture. Injected from the session when there is one (setTextureCache):
    // one texture sampled by several slots is then staged once instead of once per slot. Falls back to
    // this bridge's own cache, so a bridge used stand-alone keeps working.
    vine::raw_ptr<VsgTextureCache> texture_cache_ = nullptr;
    // Used when the renderer does not inject one (see textureCache()).
    VsgTextureCache default_texture_cache_;
    // Shared mesh binds. Injected from the session when there is one (setMeshResourceCache): streams whose
    // bytes are the model's own are then bound once for the whole session. Owned privately otherwise.
    vine::raw_ptr<VsgMeshResourceCache> mesh_cache_ = nullptr;
    VsgMeshResourceCache                default_mesh_cache_;
    // Per-draw block slots (see setDrawBlockPool). Injected from the session: the slots' buffers belong to
    // the session's device, so a bridge that owned them would hold device memory past the slot that drew
    // with it. Null when the caller injected none (the drawables then get no per-draw block at all).
    vine::raw_ptr<VsgDrawBlockPool> draw_block_pool_ = nullptr;
    // Slots whose drawable is gone but whose offset a frame in flight may still bind. A slot is returned to
    // the pool only after the retire ring has advanced past the release, which is the same rule the
    // replaced state wrappers / data nodes follow (see retireNode / advanceRetireRing).
    struct PendingDrawSlot
    {
        VsgDrawBlockPool::Slot slot;
        std::uint32_t          frames_remaining = 0; ///< Ring advances to wait before the slot is free.
    };
    std::vector<PendingDrawSlot> pending_draw_slots_;
    // Retained-share counts the sweep judges by, set for the duration of one frame by the session
    // (setRetainedShares). Only ever dereferenced inside this bridge's sweep, so the pointer is
    // live exactly while the session's counts are.
    const OwnedShareCounts* retained_shares_ = nullptr;
    // The geometries this slot has cached but did not draw in its LAST sync (the eviction
    // candidates; see ageAbsentItems), and the geometries it drew in that sync. Together they are
    // the keys of `cache_` — every entry was created by a sync that drew its geometry and is
    // dropped when the window expires or the bridge is cleared — which is what lets a frame find
    // the absent ones without walking the cache.
    std::vector<const vine::graphics::Geometry*> absent_;
    std::unordered_set<const vine::graphics::Geometry*> absent_set_;
    std::vector<const vine::graphics::Geometry*> last_seen_;
    // The frame's set of drawn geometries, injected for one frame (setFrameGeometrySet). Null when
    // no session drives this bridge, which is when the syncs age it themselves.
    std::unordered_set<const vine::graphics::Geometry*>* frame_drawn_ = nullptr;
    // The slot's per-view light block (setLightsData): declared in the pipeline
    // layout and descriptor set of the variants built from a ShaderSet that asks
    // for `vine_lights` (our forward set). Null while the built-in set draws.
    ::vsg::ref_ptr<::vsg::Data>         lights_data_;
    // Retained per-geometry nodes, keyed by geometry pointer for O(1) lookup.
    //
    // The entry OWNS the geometry it is keyed by (OwnedCacheEntry), and that
    // ownership is what makes the pointer key valid: a raw key would outlive
    // the geometry (the map cannot observe destruction) and a later geometry
    // allocated at the same address would be served the dead entry's retained
    // node — drawing the old mesh, or being skipped by a stale rejection
    // record. Holding the reference keeps the address unique, and the sweep
    // drops the entry as soon as the app itself no longer holds the geometry
    // (abandoned()), so an abandoned geometry is released promptly instead of
    // being pinned. The reuse window below is this cache's own policy (a culled
    // object must stay cheap to bring back), so unlike the program caches this
    // one is deliberately NOT capacity-trimmed.
    using GeometryCacheEntry =
        OwnedCacheEntry<vine::graphics::Geometry, std::unique_ptr<Item>>;
    std::unordered_map<const vine::graphics::Geometry*, GeometryCacheEntry> cache_;
    // Cached run-time compiled ShaderSet per (user program, vertex layout) (L1):
    // every geometry bound to the same program with the SAME set of forwarded
    // custom channels shares one glslang compile + ShaderSet instead of
    // recompiling per geometry; a different custom-channel layout (or program)
    // is its own entry. Keyed by a content hash; the entry OWNS the program, so
    // the full (program, layout, program revision) identity it carries is
    // collision-safe and so editing a retained program's GLSL
    // (ShaderProgram::revision) rebuilds the compiled set instead of serving the
    // stale one (D10). Capacity is bounded by a FIFO trim, and an entry whose
    // program the app released is dropped by the per-frame sweep (see
    // releaseAbandonedCaches).
    struct ProgramEntry
    {
        std::uint64_t layout = 0;      // hash over the custom channels
        std::uint64_t revision = ~std::uint64_t{0};
        ::vsg::ref_ptr<::vsg::ShaderSet> shader_set;
    };
    using ProgramShaderSetEntry =
        OwnedCacheEntry<vine::graphics::ShaderProgram, ProgramEntry>;
    std::unordered_map<std::uint64_t, ProgramShaderSetEntry> program_shader_sets_;
    InsertionClock program_shader_sets_clock_;
    // Compiled SPIR-V stages per (user program, content revision) (L1a): the
    // glslang pass runs ONCE per program content; every vertex layout of that
    // program then shares these stages and only the ShaderSet assembly differs
    // (L1b, program_shader_sets_). Editing a program bumps its revision and
    // forces a fresh compile (D10).
    //
    // Like cache_, each entry OWNS the program it is keyed by (OwnedCacheEntry):
    // a raw key would otherwise outlive a destroyed program, and a new program
    // allocated at the same address with the same revision would be served the
    // dead program's SPIR-V (wrong shader, silently). One entry per (program,
    // content revision); capacity is bounded by a FIFO trim and an entry whose
    // program the app released is dropped by the per-frame sweep.
    struct StageEntry
    {
        std::uint64_t revision = ~std::uint64_t{0};
        ::vsg::ShaderStages stages;
    };
    using ProgramStagesEntry =
        OwnedCacheEntry<vine::graphics::ShaderProgram, StageEntry>;
    std::unordered_map<const vine::graphics::ShaderProgram*, ProgramStagesEntry>
        program_stages_;
    InsertionClock program_stages_clock_;
    // Pipeline-template cache (L2), keyed by a content hash of the (program,
    // material, resolved-state) variant; the full key lives in VariantEntry
    // for collision-safe equality. The first geometry of a variant builds its
    // pipeline through the configurator and captures the reusable bind
    // commands; later geometry of that variant reuse them and only attach
    // their own vertex data, keeping pipeline setup cost proportional to the
    // state-variant count rather than the geometry count.
    struct VariantEntry;
    // The entry owns BOTH key objects (see OwnedPairCacheEntry): its equality
    // check compares the program and material addresses, so neither may be
    // replaceable at the same address while the template is cached — a recycled
    // address would make the check report a hit for a different variant and
    // serve the dead one's pipeline and descriptor bind. Capacity is bounded by
    // a FIFO trim and an entry whose program AND material the app released is
    // dropped by the per-frame sweep.
    using VariantCacheEntry =
        OwnedPairCacheEntry<vine::graphics::ShaderProgram, vine::graphics::Material,
                            std::unique_ptr<VariantEntry>>;
    std::unordered_map<std::uint64_t, VariantCacheEntry> variant_cache_;
    InsertionClock variant_cache_clock_;

    // Nodes dropped on a live path, held for VsgRetireRing::kRetireRingDepth frame advances
    // and released by advanceRetireRing(). The ring is the session's too (VsgRendererState
    // parks the renderer-owned objects it replaces on it), so the depth and the "advance after
    // the submit" point have ONE definition.
    VsgRetireRing retire_ring_;

    // Host diagnostics channel (empty when unset) and its counters. Counted
    // whether or not a sink is installed, so a host can gate on
    // diagnosticCount() without listening.
    vine::graphics::DiagnosticSink diagnostic_sink_;
    std::size_t                    diagnostic_count_ = 0;
    std::array<std::size_t, static_cast<std::size_t>(vine::graphics::DiagnosticCategory::Count)>
        diagnostic_counts_{};
};

V_VSG_NS_END
