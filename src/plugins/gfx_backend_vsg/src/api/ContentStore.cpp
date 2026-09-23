#include <vine/vsg/api/ContentStore.hpp>

#include <algorithm>
#include <utility>

#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>

V_VSG_NS_BEGIN

struct ContentStore::Data
{
    struct LiveGeometry
    {
        vine::intrusive_ptr<vine::graphics::Geometry> object{};  ///< The share that keeps the address alive.
        bool                                          described{false};
        std::uint64_t                                 described_revision{0};
    };

    struct LiveProgram
    {
        vine::intrusive_ptr<vine::graphics::ShaderProgram> object{};
        bool                                               described{false};
        std::uint64_t                                      described_revision{0};
        std::vector<std::uint32_t> variants;  ///< Variants built at that revision (their bits()).
        bool                       screen_built{false};  ///< The full-screen entry, at that revision.
    };

    struct LiveMaterial
    {
        vine::intrusive_ptr<vine::graphics::Material> object{};
        std::uint64_t                                 revision{0};  ///< Edit counter; only updateMaterial() moves it.
        bool                                          described{false};
        std::uint64_t                                 described_revision{0};
    };

    struct GeometryStorage
    {
        std::vector<ChannelFacts> channels;  ///< What the row's `GeometryFacts::channels` points at.
    };

    struct MaterialStorage
    {
        std::vector<std::byte> block;  ///< What the row's `MaterialFacts::block` points at.
    };

    /// A replaced material's value and its storage, kept together for the parking window (see the file note).
    struct RetiredMaterial
    {
        MaterialFacts                    facts{};
        std::unique_ptr<MaterialStorage> storage{};
    };

    // The live set: the objects the host tracked, held so the address key cannot be recycled.
    std::unordered_map<const vine::graphics::Geometry*, LiveGeometry>     geometries;
    std::unordered_map<const vine::graphics::ShaderProgram*, LiveProgram> programs;
    std::unordered_map<const vine::graphics::Material*, LiveMaterial>     materials;

    // The tables: contiguous BECAUSE `ContentFacts` hands out spans of them, with per-entry storage beside
    // each row so a span inside a row (its channels, its block) points at that row's own memory.
    std::vector<GeometryFacts>                    geometry_table;
    std::vector<std::unique_ptr<GeometryStorage>> geometry_storage;
    std::vector<ProgramFacts>                     program_table;
    std::vector<MaterialFacts>                    material_table;
    std::vector<std::unique_ptr<MaterialStorage>> material_storage;

    ContentFacts  facts{};
    std::uint64_t builds{0};
    std::size_t   retained{0};
    /// Cells a refused park left: no parking window means "keep", never "free early".
    std::vector<std::shared_ptr<void>> kept;

    /** @brief Removes the geometry rows of one identity at one revision (a parked supersession). */
    void eraseGeometryRows(const void* identity, std::uint64_t revision) noexcept
    {
        for (std::size_t i = 0U; i < geometry_table.size();)
        {
            if (geometry_table[i].geometry == identity && geometry_table[i].revision == revision)
            {
                geometry_table.erase(geometry_table.begin() + static_cast<std::ptrdiff_t>(i));
                geometry_storage.erase(geometry_storage.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    /** @brief Removes every geometry row of one identity (an abandoned object's rows). */
    void eraseGeometryOf(const void* identity) noexcept
    {
        for (std::size_t i = 0U; i < geometry_table.size();)
        {
            if (geometry_table[i].geometry == identity)
            {
                geometry_table.erase(geometry_table.begin() + static_cast<std::ptrdiff_t>(i));
                geometry_storage.erase(geometry_storage.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    /** @brief Removes the program rows of one identity at one revision (a parked supersession). */
    void eraseProgramRows(const void* identity, std::uint64_t revision) noexcept
    {
        program_table.erase(std::remove_if(program_table.begin(), program_table.end(),
                                           [identity, revision](const ProgramFacts& entry) {
                                               return entry.program == identity && entry.revision == revision;
                                           }),
                            program_table.end());
    }

    /** @brief Removes every program row of one identity (an abandoned object's rows). */
    void eraseProgramOf(const void* identity) noexcept
    {
        program_table.erase(std::remove_if(program_table.begin(), program_table.end(),
                                           [identity](const ProgramFacts& entry) {
                                               return entry.program == identity;
                                           }),
                            program_table.end());
    }

    /** @brief Removes the material row of one identity (an abandoned object's row). */
    void eraseMaterialOf(const void* identity) noexcept
    {
        for (std::size_t i = 0U; i < material_table.size();)
        {
            if (material_table[i].material == identity)
            {
                material_table.erase(material_table.begin() + static_cast<std::ptrdiff_t>(i));
                material_storage.erase(material_storage.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }
    }

    /** @brief Gets the row that answers for a geometry right now (the one built at its live revision). */
    [[nodiscard]] const GeometryFacts* liveGeometry(const void* identity, std::uint64_t revision) const noexcept
    {
        for (const GeometryFacts& entry : geometry_table)
        {
            if (entry.geometry == identity && entry.revision == revision)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    /** @brief Gets the row that answers for a material right now (identity is the whole key). */
    [[nodiscard]] const MaterialFacts* liveMaterial(const void* identity) const noexcept
    {
        for (const MaterialFacts& entry : material_table)
        {
            if (entry.material == identity)
            {
                return &entry;
            }
        }
        return nullptr;
    }
};

ContentStore::ContentStore() :
    d(std::make_shared<Data>())
{
}

ContentStore::~ContentStore() = default;

void ContentStore::track(const vine::intrusive_ptr<vine::graphics::Geometry>& geometry)
{
    if (geometry == nullptr)
    {
        return;
    }
    const auto [entry, inserted] = d->geometries.try_emplace(geometry.get());
    if (inserted)
    {
        entry->second.object = geometry;
    }
}

void ContentStore::track(const vine::intrusive_ptr<vine::graphics::ShaderProgram>& program)
{
    if (program == nullptr)
    {
        return;
    }
    const auto [entry, inserted] = d->programs.try_emplace(program.get());
    if (inserted)
    {
        entry->second.object = program;
    }
}

void ContentStore::track(const vine::intrusive_ptr<vine::graphics::Material>& material)
{
    if (material == nullptr)
    {
        return;
    }
    const auto [entry, inserted] = d->materials.try_emplace(material.get());
    if (inserted)
    {
        entry->second.object = material;
    }
}

void ContentStore::updateMaterial(vine::raw_ptr<vine::graphics::Material> material)
{
    if (material == nullptr)
    {
        return;
    }
    const auto entry = d->materials.find(material);
    if (entry == d->materials.end())
    {
        return;  // untracked: nothing draws with it, so nothing can be asked about it
    }

    // THE TOUCH IS A COMPARE-AND-WRITE (see the header note): the material's CURRENT values are compared with
    // the row the table answers for it - member-wise, the ABI's way (see materialBlockAgreesWith: the block's
    // tail padding is deliberately not part of the comparison) - and only a difference moves the revision,
    // which is what makes the next tablesFor() rebuild that row and park the value it had. A blind bump would
    // rebuild every commanded material's row every frame, which is exactly what this avoids; and it does not
    // build anything to compare, so a steady frame allocates nothing.
    Data::LiveMaterial& live = entry->second;
    if (!live.described)
    {
        return;  // nothing is described yet: the first description is taken when a frame records
    }
    const MaterialFacts* row = d->liveMaterial(material);
    if (row != nullptr && materialBlockAgreesWith(*material, row->block) && row->texture == material->texture())
    {
        return;  // touched and unchanged: nothing to build, nothing to park, nothing allocated
    }
    ++live.revision;
}

const ContentFacts& ContentStore::tablesFor(const core::CompiledFrame& frame, core::FrameTimeline& timeline,
                                            core::RetirementQueue& retirement)
{
    for (const core::CompiledPass& pass : frame.passes)
    {
        for (const core::CompiledDraw& draw : pass.draws)
        {
            if (draw.kind == core::DrawKind::Screen)
            {
                // The full-screen ABI is the engine's and carries no variant: a screen program is one text
                // (see api/ContentSources).
                ensureProgram(static_cast<const vine::graphics::ShaderProgram*>(draw.program.program),
                              ProgramVariant{}, true, timeline, retirement);
                continue;
            }

            for (const core::CompiledCommand& command : draw.commands)
            {
                const auto* geometry = static_cast<const vine::graphics::Geometry*>(command.geometry);
                const auto* material = static_cast<const vine::graphics::Material*>(command.material);

                ensureGeometry(geometry, timeline, retirement);
                ensureMaterial(material, timeline, retirement);

                const auto* program = static_cast<const vine::graphics::ShaderProgram*>(command.program.program);
                if (program == nullptr)
                {
                    continue;
                }

                // The variant is a fact of the DRAWABLE, so it is computed from the entries the same walk
                // just produced. A drawable whose geometry or material got no entry is skipped here: the
                // recorder refuses it on those two, and the program's text never decides anything for it.
                const GeometryFacts* live_geometry =
                    geometry != nullptr ? d->liveGeometry(geometry, geometry->revision()) : nullptr;
                const MaterialFacts* live_material = d->liveMaterial(material);
                if (live_geometry == nullptr || live_material == nullptr)
                {
                    continue;
                }
                ensureProgram(program, variantOf(*live_material, *live_geometry), false, timeline, retirement);
            }
        }
    }

    d->facts.programs   = d->program_table;
    d->facts.geometries = d->geometry_table;
    d->facts.materials  = d->material_table;
    return d->facts;
}

void ContentStore::ensureGeometry(const vine::graphics::Geometry* geometry, core::FrameTimeline& timeline,
                                  core::RetirementQueue& retirement)
{
    if (geometry == nullptr)
    {
        return;
    }
    const auto entry = d->geometries.find(geometry);
    if (entry == d->geometries.end())
    {
        // Untracked: the tables answer nothing for it, and the recorder reports the miss. Building facts for
        // an object nobody handed over is impossible anyway - the plan carries only the address.
        return;
    }

    Data::LiveGeometry& live     = entry->second;
    const std::uint64_t revision = geometry->revision();
    if (live.described && live.described_revision == revision)
    {
        return;  // the steady frame: nothing to build, nothing to park
    }

    if (live.described)
    {
        // The revision moved: what the table answers for the old one stays answerable until the frames that
        // may still record it are past (see the file note), and then leaves.
        const void* const         identity = geometry;
        const std::uint64_t       retired  = live.described_revision;
        const std::weak_ptr<Data> weak     = d;
        const bool parked = retirement.retire(timeline, [weak, identity, retired]() {
            if (const std::shared_ptr<Data> data = weak.lock())
            {
                data->eraseGeometryRows(identity, retired);
            }
        });
        if (!parked)
        {
            ++d->retained;  // no window: the rows are kept, which costs memory and never correctness
        }
    }

    auto           storage = std::make_unique<Data::GeometryStorage>();
    GeometryFacts  fresh;
    const FactMiss miss                   = buildGeometryFacts(*geometry, fresh, storage->channels);
    live.described                        = true;
    // The ATTEMPT, not the success: a description that failed is not retried until the revision moves.
    live.described_revision = revision;
    if (miss != FactMiss::None)
    {
        return;
    }

    d->geometry_table.push_back(fresh);
    d->geometry_storage.push_back(std::move(storage));
    ++d->builds;
}

void ContentStore::ensureMaterial(const vine::graphics::Material* material, core::FrameTimeline& timeline,
                                  core::RetirementQueue& retirement)
{
    auto entry = d->materials.find(material);
    if (entry == d->materials.end())
    {
        if (material != nullptr)
        {
            return;  // untracked (see ensureGeometry)
        }
        // The default material is an entry like any other (see api/ContentSources) and needs no tracking:
        // content without a material can be described at any time.
        entry = d->materials.try_emplace(nullptr).first;
    }

    Data::LiveMaterial& live = entry->second;
    if (live.described && live.described_revision == live.revision)
    {
        return;
    }

    auto                       storage = std::make_unique<Data::MaterialStorage>();
    MaterialFacts              fresh;
    const FactMiss             miss             = buildMaterialFacts(material, live.revision, fresh, storage->block);
    live.described                              = true;
    live.described_revision                     = live.revision;
    if (miss != FactMiss::None)
    {
        return;
    }

    // The material's lookup is by identity alone, so the table has to answer "the material now": the row is
    // REPLACED, and only the value it had is parked (see the file note).
    std::size_t row = d->material_table.size();
    for (std::size_t i = 0U; i < d->material_table.size(); ++i)
    {
        if (d->material_table[i].material == material)
        {
            row = i;
            break;
        }
    }

    if (row == d->material_table.size())
    {
        d->material_table.push_back(fresh);
        d->material_storage.push_back(std::move(storage));
        ++d->builds;
        return;
    }

    Data::RetiredMaterial retired;
    retired.facts                 = d->material_table[row];
    retired.storage               = std::move(d->material_storage[row]);
    const std::shared_ptr<Data::RetiredMaterial> cell =
        std::make_shared<Data::RetiredMaterial>(std::move(retired));

    d->material_table[row]   = std::move(fresh);
    d->material_storage[row] = std::move(storage);
    ++d->builds;

    const bool parked = retirement.retire(timeline, [cell]() { (void)cell; });
    if (!parked)
    {
        // The value stays reachable for the session: `kept` owns it, so a recorded frame that still points
        // at the old block bytes never dangles.
        const std::shared_ptr<void> held = cell;
        d->kept.push_back(held);
        ++d->retained;
    }
}

void ContentStore::ensureProgram(const vine::graphics::ShaderProgram* program, const ProgramVariant& variant,
                                 bool screen, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    if (program == nullptr)
    {
        return;
    }
    const auto entry = d->programs.find(program);
    if (entry == d->programs.end())
    {
        return;  // untracked (see ensureGeometry)
    }

    Data::LiveProgram&  live     = entry->second;
    const std::uint64_t revision = program->revision();
    if (live.described && live.described_revision == revision)
    {
        const bool built = screen ? live.screen_built
                                  : std::find(live.variants.begin(), live.variants.end(), variant.bits()) !=
                                        live.variants.end();
        if (built)
        {
            return;  // this text of this revision is already in the tables
        }
    }
    else
    {
        if (live.described)
        {
            const void* const         identity = program;
            const std::uint64_t       retired  = live.described_revision;
            const std::weak_ptr<Data> weak     = d;
            const bool parked = retirement.retire(timeline, [weak, identity, retired]() {
                if (const std::shared_ptr<Data> data = weak.lock())
                {
                    data->eraseProgramRows(identity, retired);
                }
            });
            if (!parked)
            {
                ++d->retained;
            }
        }
        live.described          = true;
        live.described_revision = revision;
        // Another revision is another set of texts: none of the old answers is this one's. (The rows stay in
        // the tables until their park comes due.)
        live.variants.clear();
        live.screen_built = false;
    }

    ProgramFacts   fresh;
    const FactMiss miss = screen ? buildScreenProgramFacts(*program, fresh)
                                 : buildProgramFacts(*program, variant, fresh);
    if (miss != FactMiss::None)
    {
        return;  // a program that is not one content pipeline: no entry, and the recorder says so
    }

    if (screen)
    {
        live.screen_built = true;
    }
    else
    {
        live.variants.push_back(variant.bits());
    }
    d->program_table.push_back(std::move(fresh));
    ++d->builds;
}

std::size_t ContentStore::releaseAbandoned(core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    const std::weak_ptr<Data> weak     = d;
    std::size_t               released = 0U;

    for (auto it = d->geometries.begin(); it != d->geometries.end();)
    {
        if (it->second.object != nullptr && it->second.object->useCount() > 1UL)
        {
            ++it;
            continue;
        }
        const void* const identity = it->first;
        const bool parked = retirement.retire(timeline, [weak, identity]() {
            if (const std::shared_ptr<Data> data = weak.lock())
            {
                data->eraseGeometryOf(identity);
            }
        });
        if (!parked)
        {
            ++d->retained;
        }
        it = d->geometries.erase(it);
        ++released;
    }

    for (auto it = d->programs.begin(); it != d->programs.end();)
    {
        if (it->second.object != nullptr && it->second.object->useCount() > 1UL)
        {
            ++it;
            continue;
        }
        const void* const identity = it->first;
        const bool parked = retirement.retire(timeline, [weak, identity]() {
            if (const std::shared_ptr<Data> data = weak.lock())
            {
                data->eraseProgramOf(identity);
            }
        });
        if (!parked)
        {
            ++d->retained;
        }
        it = d->programs.erase(it);
        ++released;
    }

    for (auto it = d->materials.begin(); it != d->materials.end();)
    {
        if (it->first == nullptr)
        {
            ++it;  // the default material is not an object anyone can abandon
            continue;
        }
        if (it->second.object != nullptr && it->second.object->useCount() > 1UL)
        {
            ++it;
            continue;
        }
        const void* const identity = it->first;
        const bool parked = retirement.retire(timeline, [weak, identity]() {
            if (const std::shared_ptr<Data> data = weak.lock())
            {
                data->eraseMaterialOf(identity);
            }
        });
        if (!parked)
        {
            ++d->retained;
        }
        it = d->materials.erase(it);
        ++released;
    }

    return released;
}

void ContentStore::clear()
{
    d->geometries.clear();
    d->programs.clear();
    d->materials.clear();
    d->geometry_table.clear();
    d->geometry_storage.clear();
    d->program_table.clear();
    d->material_table.clear();
    d->material_storage.clear();
    d->facts    = ContentFacts{};
    d->retained = 0U;
    d->kept.clear();
}

std::size_t ContentStore::geometryEntries() const noexcept
{
    return d->geometry_table.size();
}

std::size_t ContentStore::programEntries() const noexcept
{
    return d->program_table.size();
}

std::size_t ContentStore::materialEntries() const noexcept
{
    return d->material_table.size();
}

std::uint64_t ContentStore::builds() const noexcept
{
    return d->builds;
}

std::size_t ContentStore::retained() const noexcept
{
    return d->retained;
}

V_VSG_NS_END
