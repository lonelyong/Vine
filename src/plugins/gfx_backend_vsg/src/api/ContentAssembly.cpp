#include <vine/vsg/api/ContentAssembly.hpp>

#include <vsg/nodes/Group.h>

#include <vine/vsg/core/StateRegistry.hpp>

VN_VSG_NS_BEGIN

struct ContentAssembly::Data
{
    ContentStore*                 store{nullptr};
    ::vsg::ref_ptr<::vsg::Device> device{};
    core::VariantPool*            pool{nullptr};
    BlockStorage*                 storage{nullptr};
    MaterialImages*               images{nullptr};
    core::Diagnostics*            diagnostics{nullptr};

    std::unique_ptr<ContentHalves> halves{};
    std::unique_ptr<ContentSets>   sets{};
    StreamUploads                  uploads{};  ///< Stream sharing spans passes and frames (keyed by revision).
    /// The sampled-input sets the frames share (see api/ContentPass:InputSetCache): one per distinct set of
    /// images, which is what keeps a steady frame from writing a descriptor a pending frame still reads.
    InputSetCache                  input_sets{};

    const ContentFacts*  facts{nullptr};
    core::FrameTimeline* timeline{nullptr};
    core::RetirementQueue* retirement{nullptr};

    /// Episode state of the light-drop sentence, PER PASS: the sentence is about a pass ("the lights the
    /// host announced do not fit the block"), and the frame path builds a scope per pass per frame - so
    /// the state has to outlive the frame or the same sentence repeats every frame (registered
    /// 2026-09-25, M11m). One row per pass id ever recorded; ids are never re-issued (see api/PassRegistry),
    /// so this grows with the passes a host announces, not with frames.
    std::vector<std::pair<core::PassId, core::ReportOnce>> lights_dropped;
    /// Episode state of "a host whose render area is not laid out yet": one fact about the SESSION, said
    /// once however many frames (and passes) hit it.
    core::ReportOnce empty_rectangle;

    /** @brief Gets the episode state of @p pass ' light-drop report (a row is created on first use).
     *
     * @param pass The pass the report is about.
     * @return The ReportOnce that pass' episode lives in.
     */
    [[nodiscard]] core::ReportOnce& lightsDroppedFor(core::PassId pass) noexcept
    {
        for (std::pair<core::PassId, core::ReportOnce>& row : lights_dropped)
        {
            if (row.first == pass)
            {
                return row.second;
            }
        }
        lights_dropped.emplace_back(pass, core::ReportOnce{});
        return lights_dropped.back().second;
    }
};

ContentAssembly::ContentAssembly(ContentStore& store, ::vsg::ref_ptr<::vsg::Device> device, core::VariantPool& pool,
                                 BlockStorage& storage, MaterialImages& images, core::Diagnostics& diagnostics) :
    d(std::make_unique<Data>())
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
    d->halves      = std::make_unique<ContentHalves>(
        pool, detail::fetchDynamicStateEntryPoints(d->device->vk(), d->device->getInstance()->vk()));
    d->sets        = std::make_unique<ContentSets>(d->device, storage, images);
}

ContentAssembly::~ContentAssembly() = default;

void ContentAssembly::repoint(BlockStorage& storage, core::FrameTimeline& timeline,
                              core::RetirementQueue& retirement)
{
    d->storage = &storage;
    if (d->sets != nullptr)
    {
        (void)d->sets->repoint(storage, timeline, retirement);
    }
}

const ContentFacts& ContentAssembly::beginFrame(const core::CompiledFrame& frame, core::FrameTimeline& timeline,
                                                core::RetirementQueue& retirement)
{
    d->timeline   = &timeline;
    d->retirement = &retirement;
    d->storage->beginFrame();
    // The streams' frame opens with it (see api/StreamUploads): what this frame binds is stamped with it, and
    // the grace window is the parking window itself - a stream is kept for exactly as long as a recorded frame
    // that named it may still be in flight.
    d->uploads.beginFrame(timeline.submittedFrame(), retirement.slots() + 1U);
    d->facts = &d->store->tablesFor(frame, timeline, retirement);

    // The sampled-input sets the previous frame did not ask for are no longer this session's to keep: the
    // images they name were replaced (a resize builds new ones) or the pass that sampled them is gone. They
    // are PARKED rather than dropped, because a command buffer in flight may still name their VkDescriptorSet
    // (the same window every replaced object gets), and a frame that builds a set under a stale entry's key
    // would otherwise be the second writer of a set a pending command buffer reads.
    ++d->input_sets.frame;
    for (auto it = d->input_sets.entries.begin(); it != d->input_sets.entries.end();)
    {
        if (it->frame + 1U >= d->input_sets.frame)
        {
            ++it;
            continue;
        }
        if (it->set != nullptr)
        {
            const ::vsg::ref_ptr<::vsg::DescriptorSet> set = it->set;
            if (!retirement.retire(timeline, [set]() { (void)set; }))
            {
                ++it;  // no window to park through yet: kept, which costs memory and never correctness
                continue;
            }
        }
        it = d->input_sets.entries.erase(it);
    }
    return *d->facts;
}

bool ContentAssembly::record(const core::CompiledPass& pass, const core::RenderPassCompatibility& compatibility,
                             std::span<const InputImages> inputs, std::span<const std::byte> view_block,
                             ::vsg::ref_ptr<::vsg::Node>& out)
{
    if (d->facts == nullptr)
    {
        // A frame recorded before beginFrame() is a caller bug, not a content condition: the answer is an
        // empty node and false, with nothing reported (see the file note).
        out = ::vsg::Group::create();
        return false;
    }

    const std::span<const ContentPass::Scope::Entry> entries =
        d->halves->halvesFor(pass, *d->facts, *d->timeline, *d->retirement);
    const std::span<BlockDescriptors* const> candidates =
        d->sets->setsFor(pass, *d->facts, entries, inputs, *d->timeline, *d->retirement);

    // One registry PER PASS: the registry's contract is "this pass' state memory" (see its header), because
    // what a recording issues is the pass' - its view block's offsets among them - and a second pass must
    // not inherit it.
    core::StateRegistry  registry(*d->pool);
    ContentPass::Scope   scope;
    scope.entries    = entries;
    scope.registry   = &registry;
    scope.storage    = d->storage;
    scope.block_sets = candidates;
    scope.uploads    = &d->uploads;
    scope.input_sets = &d->input_sets;
    // The two reports that are NOT about this frame: the light-drop sentence is about the PASS and the
    // empty-rectangle one is about the SESSION, so their episodes belong to the content world (see
    // ContentPass::Scope and the note on the halves' own state).
    scope.lights_dropped_episode    = &d->lightsDroppedFor(pass.pass);
    scope.empty_rectangle_episode   = &d->empty_rectangle;

    ContentPass recorder(scope, *d->diagnostics);
    return recorder.record(pass, *d->facts, compatibility, inputs, view_block, out);
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

std::uint64_t ContentAssembly::releaseUnusedStreams()
{
    return d->uploads.releaseUnseen();
}

std::uint64_t ContentAssembly::inputSetBuilds() const noexcept
{
    return d->input_sets.builds;
}

VN_VSG_NS_END
