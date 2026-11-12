#include <vine/vsg/internal/ContentHalves.hpp>

#include <optional>
#include <unordered_set>
#include <utility>

#include <vine/graphics/backend/ResourceTable.hpp>
#include <vine/vsg/internal/ContentDraw.hpp>
#include <vine/vsg/internal/ContentPipeline.hpp>
#include <vine/vsg/internal/DrawNeeds.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>

VN_VSG_NS_BEGIN

struct ContentHalves::Data {
    /** @brief What a compiled half is: everything the layer depends on (see the file note). */
    struct Key {
        core::DrawKind           kind{ core::DrawKind::Content };
        vn::graphics::ContentRef program{}; // Identity AND revision: the text this half was compiled from.
        core::VertexLayoutKey    layout{};
        ProgramVariant           variant{};
        std::uint32_t            color_attachments{ 1 };
        vn::graphics::Topology   topology{ vn::graphics::Topology::Triangles };

        [[nodiscard]] friend bool operator==(const Key&, const Key&) noexcept = default;
    };

    /** @brief Hashes a half key over exactly the fields equality reads. */
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept
        {
            std::size_t seed = 0;
            const auto  mix  = [&seed](std::uint64_t value) noexcept {
                seed ^= static_cast<std::size_t>(value) + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
            };
            mix(key.program.id.bits());
            mix(key.program.revision);
            mix(static_cast<std::uint64_t>(key.kind));
            mix(key.layout.canonical_mask);
            for (const std::uint32_t location : key.layout.custom_locations) {
                mix(location);
            }
            mix(key.variant.bits());
            mix(key.color_attachments);
            mix(static_cast<std::uint64_t>(key.topology));
            return seed;
        }
    };

    /** @brief One compiled half: a layer and its recorder, and the key that says when they are this one. */
    struct Half {
        Key                              key{};
        std::unique_ptr<ContentPipeline> layer{};
        std::unique_ptr<ContentDraw>     draws{};
        /// Episode state of the two per-half reports (see ContentPass::Scope::Entry): they live WITH the
        /// half, so a half that cannot be served says so once per session, not once per frame - and a
        /// second pass drawing through the same half does not repeat it.
        vn::graphics::ReportOnce reported{};
        vn::graphics::ReportOnce shadow_reported{};
    };

    core::VariantPool*                                      pool{ nullptr };
    detail::DynamicStateEntryPoints                         entry_points{};
    core::ResourceTable<Key, std::shared_ptr<Half>, KeyHash> halves;
    std::vector<ContentPass::Scope::Entry>                  entries;
    /// Keys whose build was REFUSED. They have no resident value (no GPU object exists), so they are not in the
    /// table: the set is only the "do not retry" memory, and it leaves when its program revision does.
    std::unordered_set<Key, KeyHash> refused_keys;
    std::uint64_t                    builds{ 0 };
    std::size_t                      refused{ 0 };

    /** @brief Adds @p half's entry to this call's list, once. */
    void serve(Half& half)
    {
        for (const ContentPass::Scope::Entry& entry : entries) {
            if (entry.pipelines == half.layer.get()) {
                return; // the same half, drawn twice in one pass
            }
        }
        entries.push_back(ContentPass::Scope::Entry{ half.key.kind,
                                                     half.key.program,
                                                     half.key.layout,
                                                     half.layer.get(),
                                                     half.draws.get(),
                                                     half.key.variant,
                                                     half.key.topology,
                                                     &half.reported,
                                                     &half.shadow_reported });
    }

    /** @brief Ensures the half @p needs names, building it from @p program (and @p geometry, for content). */
    void ensure(const DrawNeeds& needs, const ProgramFacts& program, const GeometryFacts* geometry)
    {
        // The half's key comes from the SAME answer the recorder reads (see api/DrawNeeds), so the halves this
        // producer builds are exactly the tuples a pass asks it for - and what a half is keyed by is spelled in
        // one place rather than here and again in the recording path.
        Key key;
        key.kind              = needs.kind;
        key.program           = needs.program;
        key.layout            = needs.layout;
        key.variant           = needs.variant;
        key.color_attachments = needs.color_attachments;
        key.topology          = needs.topology;

        if (refused_keys.find(key) != refused_keys.end()) {
            return; // a refused key is remembered, not retried: a fix arrives with a new revision, another key
        }

        const auto acquired = halves.acquire(key, [&]() -> std::optional<std::shared_ptr<Half>> {
            ContentPipeline::Settings settings;
            settings.color_attachments             = key.color_attachments;
            // The topology the layer BAKES is the key's: the API restricts a dynamic set to the class the
            // pipeline was created with (see core::PipelineKey::topology), so a layer built for one topology can
            // only ever serve the draws that state it.
            settings.topology                      = key.topology;
            std::unique_ptr<ContentPipeline> layer = key.kind == core::DrawKind::Screen
                                                         ? ContentPipeline::createScreen(program.abi, program.shaders, settings)
                                                         : ContentPipeline::create(program.abi, *geometry, program.shaders, settings);

            ++builds;
            if (layer == nullptr) {
                // A program whose GLSL did not compile, or a layout this backend cannot declare: no half, and
                // the key is REMEMBERED so nobody tries again every frame (a fix arrives with a new revision,
                // which is another key).
                return std::nullopt;
            }

            std::shared_ptr<Half> half = std::make_shared<Half>();
            half->key                  = key;
            half->layer                = std::move(layer);
            half->draws                = std::make_unique<ContentDraw>(*half->layer, *pool, entry_points);
            return half;
        });
        if (acquired.value == nullptr) {
            ++refused;
            refused_keys.insert(key);
            return;
        }
        serve(**acquired.value);
    }
};

ContentHalves::ContentHalves(core::VariantPool& pool, detail::DynamicStateEntryPoints entry_points)
  : d(std::make_shared<Data>())
{
    d->pool         = &pool;
    d->entry_points = entry_points;
}

ContentHalves::~ContentHalves() = default;

std::span<const ContentPass::Scope::Entry>
ContentHalves::halvesFor(const core::CompiledPass& pass, const ContentFacts& facts, core::FrameTimeline& /*timeline*/, core::RetirementQueue& /*retirement*/)
{
    d->entries.clear();

    for (const core::CompiledDraw& draw : pass.draws) {
        if (draw.kind == core::DrawKind::Screen) {
            // The full-screen ABI is the engine's and carries no variant: a screen program is one text, and
            // it draws without a vertex layout (see api/ContentSources).
            const FactResult<ProgramFacts> program = findProgram(facts, draw.program, ProgramVariant{}, core::DrawKind::Screen);
            if (!program.found()) {
                continue; // the recorder reports the miss; no half is invented for it
            }
            // An empty offer: a half's key states no sampled counts, and the counts a full-screen pipeline is
            // compiled for are the CALLER's offer (see api/DrawNeeds) - which the producer, asking for shapes,
            // does not have.
            d->ensure(drawNeeds(pass, draw, {}), *program.entry, nullptr);
            continue;
        }

        for (const core::CompiledCommand& command : draw.commands) {
            // The same answer the recorder reads (see api/DrawNeeds), so the halves produced here are exactly
            // the tuples it will ask for.
            const DrawNeeds needs = drawNeeds(pass, command, facts);
            if (!needs.answerable()) {
                continue; // the recorder reports the miss; no half is invented for it
            }
            d->ensure(needs, *needs.program_entry, needs.geometry_entry);
        }
    }

    return d->entries;
}

void ContentHalves::sweep(const ContentFacts& facts, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    // A half is PINNED BY ITS PROGRAM REVISION AND ITS LAYOUT. While the tables still answer the
    // (identity, revision, variant, kind) it was compiled from AND some live geometry row still feeds its layout,
    // the half stays. In production both halves are O(1) questions - the program table's index and the layout
    // declaration index (see ContentDeclarations::declaresLayout). A hand-built table that published no layout
    // index answers the layout question by scanning its rows (the same optional-index rule as the row order);
    // the frame path always has the index, so it never pays the scan.
    const auto layout_is_live = [&facts](const Data::Key& key) {
        if (facts.declarations.hasLayoutIndex()) {
            return facts.declarations.declaresLayout(key.layout);
        }
        for (const GeometryFacts& geometry : facts.geometries) {
            if (geometry.layout == key.layout) {
                return true;
            }
        }
        return false;
    };
    const auto pinned = [&facts, &layout_is_live](const Data::Key& key) {
        if (!findProgram(facts, key.program, key.variant, key.kind).found()) {
            return false;
        }
        // A full-screen half has no vertex layout: the program is its whole pin (see api/ContentSources).
        return key.kind == core::DrawKind::Screen || layout_is_live(key);
    };
    (void)d->halves.retireUnpinned([&pinned](const Data::Key& key, const std::shared_ptr<Data::Half>&) { return pinned(key); }, timeline, retirement);

    // A refused build has no GPU object to park: its key is simply forgotten once its pins are gone (the same
    // program revision and layout the resident halves are held by), which is the ONE rule above asked of the keys
    // that never became a resident value.
    for (auto it = d->refused_keys.begin(); it != d->refused_keys.end();) {
        if (pinned(*it)) {
            ++it;
        }
        else {
            it = d->refused_keys.erase(it);
        }
    }
}

std::size_t ContentHalves::halves() const noexcept
{
    return d->halves.live();
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
    d->refused_keys.clear();
    d->entries.clear();
    d->refused = 0U;
}

VN_VSG_NS_END
