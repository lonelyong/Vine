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
#include <vine/vsg/VsgReportOnce.hpp>
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
     * Held as a non-owning pointer: the LEASES the bridge takes hold the pool (see VsgDrawBlockPool::Lease),
     * so what this pointer needs is only to be alive when a slot is acquired — the slots already handed out
     * stay valid (and keep the pool alive) however the session tears down. Our forward set reads per-drawable
     * values (`VineDrawBlock`: the opacity today) from set 1, and the slots they live in come from this ONE
     * pool, so a scene's drawables share a handful of buffers and descriptor sets instead of owning one each.
     * Every slot's lifetime is the drawable's: the bridge acquires one when it retains a geometry and the
     * lease returns it — deferred through the pool's retired queue, because a frame in flight may still bind
     * the offset — when the geometry is dropped.
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

    /** @brief Releases the cached geometries the app has let go of.
     *
     * WHAT RELEASES AN ENTRY IS THE APP RELEASING THE OBJECT, not how long ago it was last drawn.
     * The entry OWNS the geometry it is keyed by, so `useCount() <= shares` means every reference
     * left IS one of this bridge's entries — nothing outside the caches holds it any more.
     *
     * That is what tells a removed object from merely an invisible one, and it is why a geometry a
     * tree still holds is KEPT however long it goes undrawn: culled, hidden and moved are all
     * "still held", and the scene graph's own child references are the outside holders that make
     * them read that way in useCount(). A culled object is not a removed one — that is the whole
     * point of this cache (see the reuse note in the .cpp).
     *
     * The CANDIDATES, not the whole cache: only a geometry this slot did not draw in its last sync
     * can have lost its last outside holder (one that still has a holder is drawn by it, or sits in
     * a parked tree), so this walks that list instead of every entry the slot has ever cached. See
     * the notes in the .cpp for why that is the difference between O(entries ever seen) and
     * O(drawn + undrawn) per frame.
     *
     * Called once per frame by the session for EVERY slot — including the slots whose pass did not
     * run, so a disabled pass still releases what it kept — and from syncRenderCommands with this
     * slot's own counts when no session drives (a test, or a driver that never opens a frame).
     *
     * @param shares Retained shares counted for the geometries this sweep judges.
     * @return true when anything was released.
     */
    bool releaseAbandonedGeometries(const OwnedShareCounts& shares);

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

    /** @brief Sets the shadow map the pass' content set samples (set 0 / binding 3).
     *
     * Both this and @ref setShadowData are the shadow ABI (ShaderAbi.hpp). They are DECLARED by
     * every content set this backend builds, because the set is shared per (target, depth mode)
     * rather than per pass and the program is picked once for a session: the slot therefore always
     * binds SOMETHING valid here — the real map when the pass declared a shadow input, and a
     * stand-in with the block disabled when it did not (see VsgContentSlot), which is what keeps one
     * shader text able to take both paths.
     *
     * A set that declares neither (an injected foreign set, e.g. another library's in a test) is left
     * alone: the assignment is guarded by the declaration, so such a set keeps the pipeline it had.
     *
     * @param map      Image view + sampler for the map (null: bind nothing).
     * @param declared Whether the pass really declared a shadow INPUT (the map came from a
     *                 producer). False means @p map is the stand-in a pass without a shadow binds,
     *                 and it is what keeps the "this program cannot shade the shadow" report about
     *                 the passes that HAVE one.
     */
    void setShadowMap(::vsg::ref_ptr<::vsg::ImageInfo> map, bool declared);

    /** @brief Sets the shadow block the pass' content set reads (set 0 / binding 4).
     *
     * Like the lights block: the slot owns the buffer and refreshes it per frame, the bridge only
     * holds it for the descriptor sets it builds. `params.x == 0` means the block is disabled (no
     * shadow reaches this pass), which the shader takes as "no shadow" without sampling the map.
     *
     * @param data Uniform block holding a VineShadowBlock, or null.
     */
    void setShadowData(::vsg::ref_ptr<::vsg::Data> data);

    /** @brief Reconciles the retained scene under root against the commands.
     *
     * Each render command contributes one retained child (a vsg::MatrixTransform
     * holding the geometry state group). Existing children are updated in place;
     * a child is (re)built when its geometry has no cached node or its shape /
     * material binding changed, and children whose geometry is no longer drawn
     * are dropped.
     *
     * @param commands       Render commands for the current frame.
     * @param root           Stable vsg root group the retained children live under.
     * @param created        Optional: receives the subtrees newly built this frame
     *                       (they still need GPU compilation before recording).
     * @param session_shares The frame's ownership picture — how many retained entries hold each
     *                       object, counted across every slot and the material manager (see
     *                       OwnedShareCounts). Passed in rather than held: this bridge has no
     *                       pointer to the session, so there is no lifetime protocol to keep. A null
     *                       pointer means "no session" (a device-free caller driving one bridge):
     *                       this bridge then counts its own shares plus the material manager's, the
     *                       conservative direction (an under-count keeps an entry a frame longer).
     * @return true when the graph changed structurally; only newly built
     *         subtrees in @p created require compilation.
     */
    bool syncRenderCommands(
        const std::vector<vine::graphics::RenderCommand>& commands,
        ::vsg::Group* root,
        std::vector<::vsg::ref_ptr<::vsg::Node>>* created                  = nullptr,
        const OwnedShareCounts*                 session_shares         = nullptr);
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
     * Must be called exactly once per COMMITTED frame — after that frame's recordAndSubmit() —
     * because one advance is what accounts for one submission's fence wait (see retireNode). The
     * ring itself, its depth and its policy are VsgRetireRing.
     *
     * @param commit Evidence that the frame this advance accounts for was committed (see
     *               @ref FrameCommit: the advance is only legal after the submit).
     */
    void advanceRetireRing(FrameCommit commit);

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
     * entry, so the next releaseAbandonedCaches(shares) prunes the shared table.
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
     * that first drew its geometry until the app releases it (released at once) — being undrawn never evicts it, however long
     * it stays undrawn. It is also what makes the candidate list
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
     * AttributeChannel components (the stride its packed floats use). */
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
            // The CASES are the shader ABI's locations (ShaderAbi.hpp), the VALUES are this backend's
            // binding numbers — vsg's own order for its canonical arrays (texcoords at 2, colour at 3),
            // which is a spelling of the ABI here, not a definition of it.
            switch (location)
            {
                case vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Position): return 0u;
                case vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Normal): return 1u;
                case vine::graphics::attributeLocation(vine::graphics::VertexAttribute::TexCoord0): return 2u;
                case vine::graphics::attributeLocation(vine::graphics::VertexAttribute::Color): return 3u;
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
     * geometries' vertices (see AttributeChannel::offset): two segments of it are two streams. The revision is
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
     * A rebuild re-materialises the whole data node, but two of its channels do not come from the model:
     * the zero UV array a mesh without UVs binds, and the normals DERIVED when the geometry authors none.
     * Each costs a pass over the vertices (plus a fresh allocation) on every rebuild — even a rebuild that
     * changed none of its inputs. This remembers them per retained item, keyed by exactly the inputs they
     * were derived from:
     *
     *   * zero UVs: the vertex count alone;
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

        // White fallback colour and zero UVs: a function of the vertex count alone, so a count change
        // drops both rather than letting either be reused at the wrong size.
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
     * The sink is the renderer's route (it adds the stderr trace, the backend-wide counters and the
     * host's own sink), so this bridge writes nothing out of band itself: one reporting authority,
     * no double traces. Without a sink the message is still counted, so a host can gate on
     * diagnosticCount() without listening (see setDiagnosticSink). Failing to draw something must
     * never be silent.
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
     * primitive assembly is the topology's job, not the data builder's.
     *
     * @param geometry        Geometry to build.
     * @param topology        Primitive topology the geometry is drawn with:
     *                        automatic normal derivation runs for Triangles
     *                        only; Points / Lines fall back to authored
     *                        normals or a constant default.
     * @param extra_channels  Receives one entry per forwarded custom channel
     *                        (location >= 3), in binding order after the three
     *                        canonical arrays (ascending location).
     * @param derived         Cache of the channels this builder DERIVES (white colour fallback, zero UVs,
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
        vine::graphics::Topology topology,
        std::vector<VertexChannel>& extra_channels,
        DerivedChannels& derived,
        RetainedBinds& out_binds,
        vine::raw_ptr<VsgMeshResourceCache> mesh_cache);

    /** @brief The array one canonical vertex binding reads, and the model channel it aliases.
     *
     * @p aliased is null when the array was BUILT here (the white colour carrier, the zero UVs, derived
     * normals, a packed colour) rather than being a verbatim view of a model buffer — which is also
     * what decides whether the bind may be served from the shared cache (see VsgMeshResourceCache).
     */
    struct CanonicalStream
    {
        ::vsg::ref_ptr<::vsg::Data>             array;   ///< The array the binding reads.
        const vine::graphics::AttributeChannel* aliased = nullptr; ///< The model channel it views, or null.
    };

    /** @brief Builds the loc0 (positions) stream, or reports why the geometry cannot be drawn.
     *
     * The one stage that can REJECT the geometry: location 0 is mandatory, and a channel that is neither
     * three scalars per vertex nor a whole number of xyz elements is refused with the reason (rather than
     * drawn wrong). Every other stream falls back.
     *
     * @param geometry      Geometry to read.
     * @param out           Receives the array and, when it views the model, the channel it views.
     * @param out_positions Receives the positions as the CPU sees them (the model's own scalars, or the
     *                      unpacked copy held by @p out_unpacked).
     * @param out_unpacked  Storage the unpacked copy lives in: it has to outlive every later stage that
     *                      reads @p out_positions (the derivation of normals does), so the CALLER owns it
     *                      rather than this function.
     * @param derived       Derived-channel cache: the fallbacks are sized by the vertex count, so a mesh
     *                      whose count changed drops the ones built for the previous size.
     * @return true when the stream was built (false = reported and the geometry is not drawable).
     */
    bool buildPositionStream(const vine::graphics::Geometry& geometry, CanonicalStream& out,
                             std::span<const vine::math::Vec3f>& out_positions,
                             vine::geometry::Vec3fArray& out_unpacked, DerivedChannels& derived);

    /** @brief Builds the loc1 (normals) stream: authored, derived (Triangles) or defaulted.
     *
     * A bad optional channel is REPORTED and treated as absent (it must not reject an otherwise drawable
     * mesh), and a derived channel is reused verbatim while the streams it was derived from are the ones
     * it was derived from.
     *
     * @param geometry     Geometry to read.
     * @param positions    Positions as the CPU sees them (normal derivation input).
     * @param indices      Index stream of this build.
     * @param indexed      Whether @p indices came from the model (drives indexed derivation).
     * @param is_triangles Whether the topology has a surface to derive from.
     * @param derived      Derived-channel cache (read for reuse, written for a fresh derivation).
     * @return The stream the loc1 binding reads.
     */
    CanonicalStream buildNormalStream(const vine::graphics::Geometry& geometry,
                                      std::span<const vine::math::Vec3f> positions, const ::vsg::uintArray& indices,
                                      bool indexed, bool is_triangles, DerivedChannels& derived);

    /** @brief Builds the loc2 (colour) stream: the authored channel verbatim, or the white carrier.
     *
     * @param geometry     Geometry to read.
     * @param vertex_count Vertices the mesh holds (the fallback is sized by it).
     * @param derived      Derived-channel cache (holds the white carrier).
     * @return The stream the loc2 binding reads.
     */
    CanonicalStream buildColorStream(const vine::graphics::Geometry& geometry, std::size_t vertex_count,
                                     DerivedChannels& derived);

    /** @brief Builds the texture-coordinate stream: a UV pair or a direction, or zeros.
     *
     * The array states which of the two it is (see detail::texCoordArray), and it is emitted whether or
     * not the mesh authors one, because the canonical binding ORDER is what the custom channels after it
     * depend on.
     *
     * @param geometry     Geometry to read.
     * @param vertex_count Vertices the mesh holds (the zero fallback is sized by it).
     * @param derived      Derived-channel cache (holds the zero UVs).
     * @return The stream the texcoord binding reads.
     */
    CanonicalStream buildTexCoordStream(const vine::graphics::Geometry& geometry, std::size_t vertex_count,
                                        DerivedChannels& derived);

    /** @brief Collects the forwarded custom channels (locations >= 3) in ascending location order.
     *
     * A malformed channel is reported and SKIPPED: it must not misread the mesh, and it must not reject a
     * mesh the rest of which is drawable.
     *
     * @param geometry       Geometry to walk.
     * @param vertex_count   Vertices the mesh holds (a channel must cover them).
     * @param extra_channels Receives one entry per accepted channel, in binding order.
     * @return The arrays to bind after the canonical prefix.
     */
    ::vsg::DataList collectCustomChannels(const vine::graphics::Geometry& geometry, std::size_t vertex_count,
                                          std::vector<VertexChannel>& extra_channels);

    /** @brief Assembles the per-channel binds, the index bind and the draw command.
     *
     * One BindVertexBuffers PER CHANNEL, each stating its own firstBinding: vsg re-creates and re-copies
     * every array of a command whose any array is stale, so one command could only ever re-upload the
     * whole mesh (see RetainedBinds). The child ORDER is the binding order, which is why a refresh swaps a
     * bind in place rather than rebuilding the list.
     *
     * @param geometry          Geometry the commands are built for (index-bind keying and the nil checks).
     * @param canonical         The four canonical streams, in binding order 0..3.
     * @param custom_arrays     Arrays bound after the canonical prefix (one shared command for all of them).
     * @param indices           The index stream to bind.
     * @param drawn_first_index First index the draw states (a slice of a shared buffer).
     * @param drawn_index_count Indices the draw states.
     * @param indexed           Whether the model supplied the indices (decides bind sharing).
     * @param mesh_cache        Cache for the streams whose bytes are the model's own (null builds privately).
     * @param out_binds         Receives the binds, so a later edit can refresh one channel in place.
     * @return The data commands node.
     */
    ::vsg::ref_ptr<::vsg::Commands> assembleDrawCommands(
        const vine::graphics::Geometry& geometry,
        const std::array<CanonicalStream, RetainedBinds::kCanonicalCount>& canonical,
        const ::vsg::DataList& custom_arrays, const ::vsg::ref_ptr<::vsg::uintArray>& indices,
        std::size_t drawn_first_index, std::size_t drawn_index_count, bool indexed,
        vine::raw_ptr<VsgMeshResourceCache> mesh_cache, RetainedBinds& out_binds);

    /** @brief What the variant being built will sample, and which canonical attributes it feeds.
     *
     * One value rather than five locals because the two decisions are entangled: the UV attribute and
     * the sampler share `VINE_DIFFUSE_MAP`, so dropping one drops the other, and the texcoord width
     * selects the sampler KIND — which belongs to the variant identity for the same reason the drops do.
     */
    struct VariantSampling
    {
        ::vsg::ref_ptr<::vsg::ImageInfo> info;  ///< The resource the diffuse descriptor binds.
        /// Why the material's own texture is not @ref info (Ok = it is; Absent = it has none).
        detail::TextureReject reason = detail::TextureReject::Absent;
        bool        forward_set = false;             ///< Our own forward set (the one that can drop attributes).
        bool        drop_color  = false;             ///< The derived white carrier is not fed.
        bool        drop_uv     = false;             ///< The zero UVs (and with them the sampler) are not fed.
        bool        three_scalar_texcoords = false;  ///< The width the data node bound at the texcoord slot.
        const char* kind_define = nullptr;           ///< The sampler-kind define this variant compiles with.
    };

    /** @brief Gets the ShaderSet this drawable is shaded with, or reports that there is none.
     *
     * A program's own set (per (program, vertex layout)) wins; the slot's set is the fallback. A slot
     * with no set at all is reported ONCE PER BRIDGE and returns null, which the caller turns into "not
     * drawn" — shading it with something else would put a picture on screen the host did not ask for.
     *
     * @param program        User program, or null for the slot's own set.
     * @param extra_channels Custom channels the set must declare (part of the set's identity).
     * @return The set to build the pipeline from, or null when this drawable cannot be shaded.
     */
    ::vsg::ref_ptr<::vsg::ShaderSet> shadingSetFor(vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                                  const std::vector<VertexChannel>& extra_channels);

    /** @brief Decides what the variant samples and which canonical attributes it feeds.
     *
     * Resolves the material's texture first (the descriptor binds the RESOLVED resource, so the variant
     * key must be computed from it), reports an unusable one once per variant build, decides whether the
     * DERIVED colour / UV arrays may be dropped, and picks the sampler kind the pipeline compiles with —
     * substituting the kind's own white fallback when the material's texture is of the other kind, which
     * costs the map rather than the drawable.
     *
     * @param texture    Material's texture (may be null).
     * @param arrays     Vertex arrays the data node bound, in binding order. IN/OUT: the entries the
     *                   variant does not feed are CLEARED here, because the assignment stage that runs
     *                   next reads the same list (a dropped array must not be assigned).
     * @param shaderSet  Set the pipeline is built from (decides whether a program handed the kind rule).
     * @param program    User program, or null for our forward set.
     * @param derived    Channels the data builder derived, or null when the caller cannot say.
     * @return The decision, as one value.
     */
    VariantSampling resolveVariantSampling(vine::raw_ptr<const vine::graphics::Texture> texture, ::vsg::DataList& arrays,
                                           const ::vsg::ShaderSet& shaderSet,
                                           vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                                           const DerivedChannels* derived);

    /** @brief Registers the vertex arrays and the set-0 descriptors the pipeline is built from.
     *
     * Every canonical role is matched by NAME (that is how vsg matches an array against a ShaderSet),
     * and a declared name an array could not match is REPORTED: the drawable would otherwise read an
     * attribute the pipeline never enables — a degenerate picture with a clean validation log, which is
     * the failure mode this report exists to make visible.
     *
     * @param config         Configurator assembling the pipeline.
     * @param shaderSet      Set whose declared bindings the arrays are matched against.
     * @param arrays         Vertex arrays in binding order (entries dropped by @p sampling are null).
     * @param extra_channels Custom channels, bound after the canonical prefix by name.
     * @param material_data  The material's uniform bytes.
     * @param sampling       What the variant samples (decides whether the sampler is assigned at all).
     */
    void assignVariantBindings(::vsg::GraphicsPipelineConfigurator& config, ::vsg::ShaderSet& shaderSet,
                               const ::vsg::DataList& arrays, const std::vector<VertexChannel>& extra_channels,
                               ::vsg::ref_ptr<::vsg::Data> material_data, const VariantSampling& sampling);

    /** @brief Registers the descriptors the SLOT provides (lights, shadow map and shadow block).
     *
     * Each is assigned only when the set declares it: a binding nothing declares must not appear in the
     * pipeline layout, and a declared-but-unwritten descriptor is an invalid set rather than a harmless
     * one. A program that shades a drawable whose pass declared a shadow but never reads the map is
     * reported here (once per variant, not once per frame).
     *
     * @param config    Configurator assembling the pipeline.
     * @param shaderSet Set whose declared bindings decide what is registered.
     * @param program   User program, or null when the slot's own set shades the drawable.
     */
    void assignSlotDescriptors(::vsg::GraphicsPipelineConfigurator& config, ::vsg::ShaderSet& shaderSet,
                               vine::raw_ptr<const vine::graphics::ShaderProgram> program);

    /** @brief Caches a built variant's reusable pieces for later identical geometry.
     *
     * The entry owns both key objects (see OwnedPairCacheEntry), so a released program or material
     * cannot be replaced at the same address while the template it keyed is cached. Growth is bounded
     * by the same FIFO trim the geometry and material caches use; the prompt half (an entry whose
     * program AND material the app released) is releaseAbandonedCaches().
     *
     * @param hash_key        Variant key the entry is stored under.
     * @param program         Key object 1 (may be null).
     * @param material        Key object 2 (may be null).
     * @param state           Resolved state the template was built for.
     * @param layout          Vertex-layout hash (custom channels + the sampling decisions).
     * @param state_group     Wrapper the reusable state commands are read from.
     * @param pipeline_layout The variant's pipeline layout (what a reuse binds the per-draw block with).
     */
    void cacheStateVariant(std::uint64_t hash_key, vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                           vine::raw_ptr<vine::graphics::Material> material,
                           const vine::graphics::ResolvedRenderState& state, std::uint64_t layout,
                           const ::vsg::StateGroup& state_group, ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout);

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
     * @param draw_slot      The drawable's lease on a slot in the per-draw block pool, or an empty
     *                       one when it has none. The slot is NOT part of the variant identity (its
     *                       values are rewritten in place), so what the wrapper records is the slot's
     *                       OFFSET: one shared descriptor set per pool chunk, bound with this
     *                       drawable's dynamic offset.
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
        const VsgDrawBlockPool::Lease& draw_slot = {});

    /** @brief Gets (and caches) the run-time compiled ShaderSet for a program.
     *
     * Compiles the program's stages and assembles a ShaderSet once per
     * (program, vertex layout) instead of once per geometry: N geometry bound
     * to the same program AND carrying the same set of custom channels share a
     * single glslang compile and ShaderSet. Besides the canonical
     * vine_Vertex/Normal/Color bindings (the locations `attributeLocation()` assigns them), the set declares one
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

    /** @brief Reserves the item's per-draw block slot when it needs one, or says why it could not.
     *
     * The slot is what a translucent drawable's opacity costs per frame (four bytes written in place),
     * and its offset is what the item's state wrapper binds, so it is reserved with the ITEM rather
     * than with either node: a data rebuild must not lose it, and both nodes may come and go.
     *
     * Only the bridge's OWN forward set reads the block — the built-in fallback carries opacity in the
     * vertex colour — so a bridge without that set needs no slot at all.
     *
     * A refusal is not "draw it without the block": the shader would read zeros, scale the fragment
     * alpha by 0 and the drawable would silently vanish, so the failure is REPORTED and the caller
     * drops the drawable for this frame.
     *
     * @param item Item to give a slot to.
     * @return true when the item can be drawn (it has a slot, or needs none).
     */
    bool reserveDrawSlot(Item& item);

    /** @brief Refreshes the streams whose bytes changed, leaving the retained nodes where they are.
     *
     * The cheap half of a data edit: each channel has its own bind command, so a stream that changed
     * bytes is served by re-pointing (or swapping in) that one bind, and vsg re-creates and copies
     * exactly that channel instead of the whole mesh. The index stream takes the same path — it is one
     * more channel — but only while the SPAN it draws does not change, because the span lives in the
     * draw command, which only a rebuild rewrites.
     *
     * The refresh has to be EXPLAINED by the stream identities: a revision none of them accounts for
     * (a buffer mutated in place without bumping its own revision) must not be answered with "nothing
     * to do", so it returns false and the caller re-reads the model instead.
     *
     * @param geometry Geometry whose streams to check.
     * @param item     Item holding the retained node, the binds and the identities to compare against.
     * @param state    Resolved render state of this frame (its topology drives normal derivation).
     * @param keys_now Channel identities as the geometry is NOW.
     * @param index_now Index stream identity as the geometry is NOW.
     * @return true when the changed streams were refreshed in place (false = the caller must rebuild).
     */
    bool refreshChangedStreams(const vine::graphics::Geometry& geometry, Item& item,
                               const vine::graphics::ResolvedRenderState& state,
                               const std::vector<ChannelKey>& keys_now, const ChannelKey& index_now);

    /** @brief Re-materialises the item's data node from the geometry.
     *
     * The expensive half: a fresh node built from the model, with the previous one parked (its buffers
     * may still be in flight) and its binds replaced. The item's channel/index identities are taken
     * over, so the next revision can tell which streams changed — and only the streams a rebuild can
     * ACCOUNT for are served from the shared-bind cache.
     *
     * A geometry the builder refuses (unusable strides, out-of-range indices) is RECORDED once per
     * revision: the item keeps its per-draw slot, loses the wrapper that no longer has data to wrap,
     * and costs one lookup per frame instead of a rebuild per frame.
     *
     * @param geometry Geometry to materialise.
     * @param item     Item to rebuild (its data node, binds and identities are replaced).
     * @param state    Resolved render state of this frame.
     * @param keys_now Channel identities as the geometry is NOW (moved into the item).
     * @param index_now Index identity as the geometry is NOW.
     * @return true when the data node was built, false when the geometry was refused (recorded).
     */
    bool rebuildDataNode(const vine::graphics::Geometry& geometry, Item& item,
                         const vine::graphics::ResolvedRenderState& state, std::vector<ChannelKey>& keys_now,
                         const ChannelKey& index_now);

    /** @brief Builds the item's state wrapper, or records that the attempt failed.
     *
     * The wrapper carries the pipeline and the descriptor binds, so it is rebuilt whenever one of its
     * inputs changed, whenever the forwarded channel SET changed (its per-layout shader set follows the
     * bound arrays), and whenever it is missing while no failure is recorded for the current identity —
     * which is what makes a pass' depth-policy flip reach a wrapper whose inputs compare equal (see
     * invalidateState).
     *
     * On failure the item KEEPS its data node and its per-draw slot, and the attempt is recorded: the
     * next frame skips it until something real changes, so a drawable the backend cannot build costs a
     * lookup per frame instead of a rebuilt subtree per frame.
     *
     * @param item Item to rebuild the wrapper of.
     * @return true when a wrapper was built (false = recorded failure, nothing to draw).
     */
    bool rebuildStateWrapper(Item& item);

    /** @brief Puts the item's current nodes into the retained subtree.
     *
     * The retained shape is fixed — transform -> state wrapper -> data node — and the children are only
     * touched when they do NOT already hold the current pair, so a steady frame writes nothing. The
     * transform is created on first sight; a rebuild of either node reuses it, which is what keeps a
     * material or data edit from moving the drawable.
     *
     * @param item     Item whose nodes to attach.
     * @param had_node Whether the item already had a retained subtree before this frame.
     */
    void attachRetainedNodes(Item& item, bool had_node);

    /** @brief Writes the frame's per-drawable values into the item's nodes.
     *
     * Opacity and placement are VALUES, never part of any identity: the opacity goes into the pooled
     * per-draw block and the placement into the item's transform, each only when it differs from what
     * the item last wrote (an unchanged value costs one comparison; a moved camera costs one store per
     * drawable).
     *
     * @param item         Item to write into.
     * @param cmd          Command carrying this frame's opacity.
     * @param world        World matrix of this draw.
     * @param matrix_moved Whether @p world differs from the one the item last wrote.
     */
    void writePerDrawValues(Item& item, const vine::graphics::RenderCommand& cmd, const ::vsg::dmat4& world,
                            bool matrix_moved);

    /** @brief Gets the cache that uploads the textures the scene samples.
     *
     * @return The texture cache (always non-null / usable).
     */
    VsgTextureCache& textureCache();

    /** @brief Gets the mesh-resource cache in use (the injected one, or this bridge's own). */
    VsgMeshResourceCache& meshResources();

    /** @brief Gets the slot's base shader set: whatever was injected, and nothing else.
     *
     * The ENGINE builds its own sets lazily — one per (target size, depth policy), cached by the
     * renderer and handed to each content slot (see detail::makeContentShaderSet) — so a bridge never
     * pays for a fresh build per geometry and never reaches for another library's set. A user program
     * path builds on top of the injected set's default pipeline states (the baked viewport /
     * blending), keeping both paths on one material descriptor ABI.
     *
     * A caller may INJECT any set (setShaderSet), including a foreign one: the bridge is generic on
     * purpose and reads whatever that set declares. The ENGINE never does — every set it hands a slot
     * is one of its own (see detail::makeContentShaderSet) — so a foreign set that shades from another
     * library's own light data draws without the lights this bridge feeds.
     *
     * @return The injected set, or null when none was set (a bridge with no set cannot shade: it
     *         reports that once and draws no content rather than inventing a default).
     */
    ::vsg::ref_ptr<::vsg::ShaderSet> baseShaderSet();

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
     * @param draw_slot       The drawable's lease on a slot (an empty lease appends nothing).
     */
    void appendDrawBlockBind(::vsg::StateGroup& state_group,
                             ::vsg::ref_ptr<::vsg::PipelineLayout> pipeline_layout,
                             const VsgDrawBlockPool::Lease& draw_slot);

    /** @brief Rebuilds the undrawn candidate list from this sync's drawings.
     *
     * The list is what releaseAbandonedGeometries walks and (with drawn_) the keys of the geometry
     * cache, so it is maintained on every sync whether or not this bridge sweeps itself.
     *
     * @param seen Geometries drawn by this sync.
     */
    void updateUndrawnCandidates(const std::unordered_set<const vine::graphics::Geometry*>& seen);

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
     * session hands its counts to the sync instead (see syncRenderCommands).
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
    // The geometries this slot has cached but did not draw in its LAST sync (the sweep's
    // candidates; see releaseAbandonedGeometries), and the geometries it drew in that sync.
    // Together they are the keys of `cache_` — every entry was created by a sync that drew its
    // geometry and is dropped when nothing outside the caches holds the geometry any more, or when
    // the bridge is cleared — which is what lets a frame find the candidates without walking the
    // cache.
    std::vector<const vine::graphics::Geometry*> undrawn_;
    std::unordered_set<const vine::graphics::Geometry*> undrawn_set_;
    std::vector<const vine::graphics::Geometry*> drawn_;
    // Whether this bridge already reported that it has no shader set to shade with (see
    // setShaderSet / buildStateGroup). Once per set: injecting one re-arms the report, so a slot
    // that loses its set again says so again instead of going quiet.
    ReportOnce no_shader_set_reported_;
    // The slot's per-view light block (setLightsData): declared in the pipeline
    // layout and descriptor set of the variants built from a ShaderSet that asks
    // for `vine_lights` (our forward set). Null while the built-in set draws.
    ::vsg::ref_ptr<::vsg::Data>         lights_data_;
    // The slot's shadow pair (setShadowMap / setShadowData): declared in the pipeline layout and
    // descriptor set of the variants built from a ShaderSet that asks for `shadow_map` /
    // `vine_shadow` — every set this backend builds does (see buildVineShaderSet).
    ::vsg::ref_ptr<::vsg::ImageInfo>    shadow_map_;
    bool                                shadow_declared_ = false;
    ::vsg::ref_ptr<::vsg::Data>         shadow_data_;
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
    // being pinned. What IS this cache's own policy is that a geometry the app still holds is
    // kept however long it goes undrawn (a culled object must stay cheap to bring back), so
    // unlike the program caches this one is deliberately NOT capacity-trimmed.
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
