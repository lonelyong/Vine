#include <vine/vsg/api/ContentAssembly.hpp>

#include <vsg/nodes/Group.h>

#include <vine/vsg/core/StateRegistry.hpp>

V_VSG_NS_BEGIN

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

    const ContentFacts*  facts{nullptr};
    core::FrameTimeline* timeline{nullptr};
    core::RetirementQueue* retirement{nullptr};
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

const ContentFacts& ContentAssembly::beginFrame(const core::CompiledFrame& frame, core::FrameTimeline& timeline,
                                                core::RetirementQueue& retirement)
{
    d->timeline   = &timeline;
    d->retirement = &retirement;
    d->storage->beginFrame();
    d->facts = &d->store->tablesFor(frame, timeline, retirement);
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

V_VSG_NS_END
