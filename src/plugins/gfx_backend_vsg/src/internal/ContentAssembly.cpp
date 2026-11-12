#include <vine/vsg/internal/ContentAssembly.hpp>

#include <vsg/nodes/Group.h>

#include <vine/graphics/backend/StateRegistry.hpp>

VN_VSG_NS_BEGIN

struct ContentAssembly::Data {
    ContentStore*                 store{ nullptr };
    ::vsg::ref_ptr<::vsg::Device> device{};
    core::VariantPool*            pool{ nullptr };
    BlockStorage*                 storage{ nullptr };
    MaterialImages*               images{ nullptr };
    vn::graphics::Diagnostics*    diagnostics{ nullptr };

    std::unique_ptr<ContentHalves> halves{};
    std::unique_ptr<ContentSets>   sets{};
    StreamUploads                  uploads{}; // Stream sharing spans passes and frames (keyed by revision).
    /// The sampled-input sets the frames share (see api/ContentPass:InputSetCache): one per distinct set of
    /// images, which is what keeps a steady frame from writing a descriptor a pending frame still reads.
    InputSetCache input_sets{};

    /// The device's widest LINE width (see ContentPass::Scope::max_line_width): read once here, because this is
    /// the object that owns the device and the recorder that clamps the value is created per pass.
    float max_line_width{ 1.0F };

    const ContentFacts*    facts{ nullptr };
    core::FrameTimeline*   timeline{ nullptr };
    core::RetirementQueue* retirement{ nullptr };
    /// Reused per frame: the textures the passes import (the content that names a texture without a row).
    std::vector<const vn::graphics::Texture*> imported_textures;

    /// Episode state of the light-drop sentence, PER PASS: the sentence is about a pass ("the lights the
    /// host announced do not fit the block"), and the frame path builds a scope per pass per frame - so
    /// the state has to outlive the frame or the same sentence repeats every frame (registered
    /// 2026-09-25, M11m). One row per pass id ever recorded; ids are never re-issued (see api/PassRegistry),
    /// so this grows with the passes a host announces, not with frames.
    std::vector<std::pair<core::PassId, vn::graphics::ReportOnce>> lights_dropped;
    /// Episode state of "a host whose render area is not laid out yet": one fact about the SESSION, said
    /// once however many frames (and passes) hit it.
    vn::graphics::ReportOnce empty_rectangle;

    /** @brief Gets the episode state of @p pass ' light-drop report (a row is created on first use).
     *
     * @param pass The pass the report is about.
     * @return The ReportOnce that pass' episode lives in.
     */
    [[nodiscard]] vn::graphics::ReportOnce& lightsDroppedFor(core::PassId pass) noexcept
    {
        for (std::pair<core::PassId, vn::graphics::ReportOnce>& row : lights_dropped) {
            if (row.first == pass) {
                return row.second;
            }
        }
        lights_dropped.emplace_back(pass, vn::graphics::ReportOnce{});
        return lights_dropped.back().second;
    }
};

ContentAssembly::ContentAssembly(ContentStore&                 store,
                                 ::vsg::ref_ptr<::vsg::Device> device,
                                 core::VariantPool&            pool,
                                 BlockStorage&                 storage,
                                 MaterialImages&               images,
                                 vn::graphics::Diagnostics&    diagnostics)
  : d(std::make_unique<Data>())
{
    d->store       = &store;
    d->device      = std::move(device);
    d->pool        = &pool;
    d->storage     = &storage;
    d->images      = &images;
    d->diagnostics = &diagnostics;
    // The halves' recorders issue the dynamic state of every draw, so they need the device's entry points -
    // the calls cannot be named by link (see api/StateCommands). A device-free caller of api/ContentHalves
    // leaves them empty and does not record; THIS caller has the device and is about to record.
    d->halves      = std::make_unique<ContentHalves>(pool, detail::fetchDynamicStateEntryPoints(d->device->vk(), d->device->getInstance()->vk()));
    d->sets        = std::make_unique<ContentSets>(d->device, storage, images);
    // How wide a rasterised LINE may be is the device's answer, and the API REFUSES a width above it - so it is
    // read here, once, and handed to every pass' recorder (see ContentPass::Scope::max_line_width).
    if (const ::vsg::PhysicalDevice* physical = d->device->getPhysicalDevice()) {
        d->max_line_width = physical->getProperties().limits.lineWidthRange[1];
    }
}

ContentAssembly::~ContentAssembly() = default;

void ContentAssembly::repoint(BlockStorage& storage, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    d->storage = &storage;
    if (d->sets != nullptr) {
        (void)d->sets->repoint(storage, timeline, retirement);
    }
}

const ContentFacts& ContentAssembly::beginFrame(const core::CompiledFrame& frame, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    return openTables(frame, timeline, retirement, /*blocks*/ true);
}

const ContentFacts& ContentAssembly::prepare(const core::CompiledFrame& frame, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    // THE SAME OPENING, MINUS THE BLOCK STORAGE'S FRAME: see the header - the ring's rotation is the frame's, and a
    // warm-up that consumed one would shift it under the oldest frame still in flight.
    return openTables(frame, timeline, retirement, /*blocks*/ false);
}

const ContentFacts& ContentAssembly::openTables(const core::CompiledFrame& frame, core::FrameTimeline& timeline, core::RetirementQueue& retirement, bool blocks)
{
    d->timeline   = &timeline;
    d->retirement = &retirement;
    if (blocks) {
        d->storage->beginFrame();
    }
    d->facts = &d->store->tablesFor(frame, timeline);
    // The streams' frame opens ON THE FACTS THAT WALK JUST PUBLISHED (see api/StreamUploads): the published
    // declaration index says which buffers live content still names, and each entry answers the other half (its
    // own buffer's revision). Culling keeps a stream (its row still declares the buffer); leaving the tree lets
    // it go through the same parking window every replaced GPU object gets.
    (void)d->uploads.beginFrame(*d->facts, timeline, retirement);

    // The textures' frame opens on the SAME facts plus the frame's own imports (see api/MaterialImages): a material
    // row declares the maps its slots sample, and a pass' imported input declares a texture with no row at all - so a
    // map that is only ever imported would otherwise leave the cache between two frames that both sample it.
    d->imported_textures.clear();
    for (const core::CompiledPass& pass : frame.passes) {
        for (const core::CompiledInput& input : pass.inputs) {
            if (input.kind == vn::graphics::ImageSource::Kind::ImportedTexture && input.texture != nullptr) {
                d->imported_textures.push_back(static_cast<const vn::graphics::Texture*>(input.texture));
            }
        }
    }
    (void)d->images->beginFrame(d->facts->declarations, d->imported_textures);

    // The MATERIAL BLOCKS follow the same pin, one kind down (see MaterialArena::retireUnpinned): an arena slot is
    // kept while a live material row names its identity, and returned the frame nothing does. The release is
    // immediate because the arena's own copy rotation is what protects an in-flight frame's bytes - so what comes
    // back here is the right to reuse a slot, never bytes anybody may still read. The walk above published the
    // index this asks, so the sweep costs one lookup per resident slot rather than a pass over the rows.
    (void)d->storage->retireUnpinnedMaterials(d->facts->declarations);

    // The sampled-input sets the previous frame did not ask for are no longer this session's to keep: the
    // images they name were replaced (a resize builds new ones) or the pass that sampled them is gone. They
    // are PARKED rather than dropped, because a command buffer in flight may still name their VkDescriptorSet
    // (the same window every replaced object gets), and a frame that builds a set under a stale entry's key
    // would otherwise be the second writer of a set a pending command buffer reads.
    ++d->input_sets.frame;
    for (auto it = d->input_sets.entries.begin(); it != d->input_sets.entries.end();) {
        if (it->frame + 1U >= d->input_sets.frame) {
            ++it;
            continue;
        }
        if (it->set != nullptr) {
            const ::vsg::ref_ptr<::vsg::DescriptorSet> set = it->set;
            if (!retirement.retire(timeline, [set]() { (void)set; })) {
                ++it; // no window to park through yet: kept, which costs memory and never correctness
                continue;
            }
        }
        it = d->input_sets.entries.erase(it);
    }
    return *d->facts;
}

bool ContentAssembly::record(const core::CompiledPass&            pass,
                             const core::RenderPassCompatibility& compatibility,
                             std::span<const InputImages>         inputs,
                             std::span<const std::byte>           view_block,
                             ::vsg::ref_ptr<::vsg::Node>&         out)
{
    if (d->facts == nullptr) {
        // A frame recorded before beginFrame() is a caller bug, not a content condition: the answer is an
        // empty node and false, with nothing reported (see the file note).
        out = ::vsg::Group::create();
        return false;
    }

    core::StateRegistry registry(*d->pool);
    ContentPass         recorder(scopeFor(pass, inputs, registry), *d->diagnostics);
    return recorder.record(pass, *d->facts, compatibility, inputs, view_block, out);
}

bool ContentAssembly::prepare(const core::CompiledPass& pass, const core::RenderPassCompatibility& compatibility, std::span<const InputImages> inputs)
{
    if (d->facts == nullptr) {
        // The same caller bug `record` answers with an empty node: here there is no node to answer with, so the
        // false IS the answer (nothing was ensured, and the next frame will record what this call could not).
        return false;
    }

    core::StateRegistry registry(*d->pool);
    ContentPass         recorder(scopeFor(pass, inputs, registry), *d->diagnostics);
    return recorder.prepare(pass, *d->facts, compatibility, inputs);
}

ContentPass::Scope ContentAssembly::scopeFor(const core::CompiledPass& pass, std::span<const InputImages> inputs, core::StateRegistry& registry)
{
    const std::span<const ContentPass::Scope::Entry> entries    = d->halves->halvesFor(pass, *d->facts, *d->timeline, *d->retirement);
    const std::span<BlockDescriptors* const>         candidates = d->sets->setsFor(pass, *d->facts, entries, inputs, *d->timeline, *d->retirement);

    // One registry PER PASS: the registry's contract is "this pass' state memory" (see its header), because
    // what a recording issues is the pass' - its view block's offsets among them - and a second pass must
    // not inherit it.
    ContentPass::Scope scope;
    scope.entries                 = entries;
    scope.registry                = &registry;
    scope.storage                 = d->storage;
    scope.block_sets              = candidates;
    scope.uploads                 = &d->uploads;
    scope.input_sets              = &d->input_sets;
    scope.max_line_width          = d->max_line_width;
    // The two reports that are NOT about this frame: the light-drop sentence is about the PASS and the
    // empty-rectangle one is about the SESSION, so their episodes belong to the content world (see
    // ContentPass::Scope and the note on the halves' own state).
    scope.lights_dropped_episode  = &d->lightsDroppedFor(pass.pass);
    scope.empty_rectangle_episode = &d->empty_rectangle;
    // The material-slot reports belong to the MATERIAL, so their episodes live in the store's per-material
    // entries (see ContentStore::claimSlotReport) - the pass itself is per frame.
    scope.store                   = d->store;
    return scope;
}

const ContentFacts& ContentAssembly::facts() const noexcept
{
    static const ContentFacts empty{};
    return d->facts != nullptr ? *d->facts : empty;
}

ContentHalves& ContentAssembly::halves() noexcept
{
    return *d->halves;
}

ContentSets& ContentAssembly::sets() noexcept
{
    return *d->sets;
}

StreamUploads& ContentAssembly::uploads() noexcept
{
    return d->uploads;
}

const StreamUploads& ContentAssembly::uploads() const noexcept
{
    return d->uploads;
}

std::uint64_t ContentAssembly::inputSetBuilds() const noexcept
{
    return d->input_sets.builds;
}

VN_VSG_NS_END
