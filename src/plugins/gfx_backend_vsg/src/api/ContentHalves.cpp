#include <vine/vsg/api/ContentHalves.hpp>

#include <utility>

#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>

V_VSG_NS_BEGIN

struct ContentHalves::Data
{
    /** @brief What a compiled half is: everything the layer depends on (see the file note). */
    struct Key
    {
        core::DrawKind        kind{core::DrawKind::Content};
        const void*           program{nullptr};
        std::uint64_t         revision{0};
        core::VertexLayoutKey layout{};
        ProgramVariant        variant{};
        std::uint32_t         color_attachments{1};
        vine::graphics::Topology topology{vine::graphics::Topology::Triangles};
    };

    /** @brief One compiled half: a layer and its recorder, and the key that says when they are this one. */
    struct Half
    {
        Key key{};
        std::unique_ptr<ContentPipeline> layer{};  ///< Null when the layer was REFUSED (never retried).
        std::unique_ptr<ContentDraw>     draws{};
    };

    core::VariantPool*              pool{nullptr};
    detail::DynamicStateEntryPoints entry_points{};
    std::vector<std::shared_ptr<Half>>     halves;
    std::vector<ContentPass::Scope::Entry> entries;
    std::uint64_t                          builds{0};
    std::size_t                            refused{0};

    /** @brief Gets the half one key already has (a refused one is a hit too: it must not be retried). */
    [[nodiscard]] Half* find(const Key& key) noexcept
    {
        for (const std::shared_ptr<Half>& half : halves)
        {
            if (half->key.kind == key.kind && half->key.program == key.program &&
                half->key.revision == key.revision && half->key.layout == key.layout &&
                half->key.variant == key.variant && half->key.color_attachments == key.color_attachments &&
                half->key.topology == key.topology)
            {
                return half.get();
            }
        }
        return nullptr;
    }

    /** @brief Adds @p half's entry to this call's list, once. */
    void serve(const Half& half)
    {
        for (const ContentPass::Scope::Entry& entry : entries)
        {
            if (entry.pipelines == half.layer.get())
            {
                return;  // the same half, drawn twice in one pass
            }
        }
        entries.push_back(ContentPass::Scope::Entry{ half.key.kind, half.key.program, half.key.revision,
                                                     half.key.layout, half.layer.get(), half.draws.get(),
                                                     half.key.variant, half.key.topology });
    }

    /** @brief Whether the tables still answer the key of @p half (see the sweep in halvesFor). */
    [[nodiscard]] bool answerable(const Half& half, const ContentFacts& facts) const noexcept
    {
        if (!findProgram(facts, core::ProgramRef{ half.key.program, half.key.revision }, half.key.variant,
                         half.key.kind)
                 .found())
        {
            return false;
        }
        if (half.key.kind == core::DrawKind::Screen)
        {
            return true;
        }
        for (const GeometryFacts& geometry : facts.geometries)
        {
            if (geometry.layout == half.key.layout)
            {
                return true;  // a geometry that would ask for this half's layout is still described
            }
        }
        return false;
    }

    /** @brief Ensures the half one key names, building it from @p program (and @p geometry, for content). */
    void ensure(const Key& key, const ProgramFacts& program, const GeometryFacts* geometry)
    {
        if (Half* existing = find(key))
        {
            if (existing->layer != nullptr)
            {
                serve(*existing);
            }
            return;
        }

        ContentPipeline::Settings settings;
        settings.color_attachments = key.color_attachments;
        // The topology the layer BAKES is the key's: the API restricts a dynamic set to the class the
        // pipeline was created with (see core::PipelineKey::topology), so a layer built for one topology can
        // only ever serve the draws that state it.
        settings.topology          = key.topology;
        std::unique_ptr<ContentPipeline> layer =
            key.kind == core::DrawKind::Screen
                ? ContentPipeline::createScreen(program.abi, program.shaders, settings)
                : ContentPipeline::create(program.abi, *geometry, program.shaders, settings);

        ++builds;
        std::shared_ptr<Half> half = std::make_shared<Half>();
        half->key                  = key;
        if (layer == nullptr)
        {
            // A program whose GLSL did not compile, or a layout this backend cannot declare: no half, and
            // the key is REMEMBERED so nobody tries again every frame (a fix arrives with a new revision,
            // which is another key).
            ++refused;
            halves.push_back(std::move(half));
            return;
        }

        half->layer = std::move(layer);
        half->draws = std::make_unique<ContentDraw>(*half->layer, *pool, entry_points);
        halves.push_back(half);
        serve(*half);
    }
};

ContentHalves::ContentHalves(core::VariantPool& pool, detail::DynamicStateEntryPoints entry_points) :
    d(std::make_shared<Data>())
{
    d->pool         = &pool;
    d->entry_points = entry_points;
}

ContentHalves::~ContentHalves() = default;

std::span<const ContentPass::Scope::Entry> ContentHalves::halvesFor(const core::CompiledPass& pass,
                                                                     const ContentFacts& facts,
                                                                     core::FrameTimeline& timeline,
                                                                     core::RetirementQueue& retirement)
{
    d->entries.clear();

    for (const core::CompiledDraw& draw : pass.draws)
    {
        if (draw.kind == core::DrawKind::Screen)
        {
            // The full-screen ABI is the engine's and carries no variant: a screen program is one text, and
            // it draws without a vertex layout (see api/ContentSources).
            const FactResult<ProgramFacts> program =
                findProgram(facts, draw.program, ProgramVariant{}, core::DrawKind::Screen);
            if (!program.found())
            {
                continue;  // the recorder reports the miss; no half is invented for it
            }
            Data::Key key;
            key.kind              = core::DrawKind::Screen;
            key.program           = draw.program.program;
            key.revision          = draw.program.revision;
            key.color_attachments = pass.color_attachments;
            // A full-screen call's state is the PASS' (its plan field): the topology it draws its generated
            // triangle with travels with it, so a pass drawn as lines gets a line-class pipeline.
            key.topology          = draw.dynamic.topology;
            d->ensure(key, *program.entry, nullptr);
            continue;
        }

        for (const core::CompiledCommand& command : draw.commands)
        {
            // The same lookups the recorder makes (see api/ContentPass::recordCommand), so the halves
            // produced here are exactly the tuples it will ask for.
            const FactResult<GeometryFacts> geometry =
                findGeometry(facts, command.geometry, command.geometry_revision);
            if (!geometry.found())
            {
                continue;
            }
            const FactResult<MaterialFacts> material = findMaterial(facts, command.material);
            if (!material.found())
            {
                continue;
            }
            const ProgramVariant           variant = variantOf(*material.entry, *geometry.entry);
            const FactResult<ProgramFacts> program =
                findProgram(facts, command.program, variant, core::DrawKind::Content);
            if (!program.found())
            {
                continue;
            }

            Data::Key key;
            key.kind              = core::DrawKind::Content;
            key.program           = command.program.program;
            key.revision          = command.program.revision;
            key.layout            = geometry.entry->layout;
            key.variant           = variant;
            key.color_attachments = pass.color_attachments;
            // ... and the topology the command is assembled with: a half is compiled for ONE class, because
            // the API will not accept a dynamic set that leaves it (see core::PipelineKey::topology).
            key.topology          = command.dynamic.topology;
            d->ensure(key, *program.entry, geometry.entry);
        }
    }

    // The sweep: a half whose key the tables no longer answer (their own retirement took the revision away)
    // can never be asked for again - a pass matches halves by the tuples those same tables produce - so it
    // is parked through the same window every other replaced object gets. A half the tables CAN answer
    // stays, even when this pass did not draw it: another pass of this frame may.
    for (auto it = d->halves.begin(); it != d->halves.end();)
    {
        const std::shared_ptr<Data::Half> half = *it;
        if (d->answerable(*half, facts))
        {
            ++it;
            continue;
        }
        if (half->layer != nullptr)
        {
            const bool parked = retirement.retire(timeline, [half]() { (void)half; });
            if (!parked)
            {
                // No window: the half is kept rather than freed while a recorded frame may still name it
                // (memory, never correctness - the same answer api/ContentStore gives).
                ++it;
                continue;
            }
        }
        it = d->halves.erase(it);
    }

    return d->entries;
}

std::size_t ContentHalves::halves() const noexcept
{
    std::size_t alive = 0U;
    for (const std::shared_ptr<Data::Half>& half : d->halves)
    {
        if (half->layer != nullptr)
        {
            ++alive;
        }
    }
    return alive;
}

std::uint64_t ContentHalves::builds() const noexcept
{
    return d->builds;
}

std::size_t ContentHalves::refused() const noexcept
{
    return d->refused;
}

void ContentHalves::clear()
{
    d->halves.clear();
    d->entries.clear();
    d->refused = 0U;
}

V_VSG_NS_END
