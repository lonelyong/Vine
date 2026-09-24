#include <vine/vsg/api/ContentStore.hpp>

#include <algorithm>
#include <utility>

#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>

VN_VSG_NS_BEGIN

struct ContentStore::Data
{
    struct LiveGeometry
    {
        vn::intrusive_ptr<vn::graphics::Geometry> object{};  ///< The share that keeps the address alive.
        bool                                          described{false};
        std::uint64_t                                 described_revision{0};
    };

    struct LiveProgram
    {
        vn::intrusive_ptr<vn::graphics::ShaderProgram> object{};
        bool                                               described{false};
        std::uint64_t                                      described_revision{0};
        std::vector<std::uint32_t> variants;  ///< Variants built at that revision (their bits()).
        bool                       screen_built{false};  ///< The full-screen entry, at that revision.
    };

    struct LiveMaterial
    {
        vn::intrusive_ptr<vn::graphics::Material> object{};
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

    /// A program row's per-row storage: none. The type is here because the three tables move their rows the
    /// same way, and an EMPTY slot (a null pointer) costs nothing to carry beside the other two.
    struct NoStorage
    {
    };

    /// One table: its rows, the per-row storage their spans point at, and the ROW ORDER its lookups search.
    ///
    /// WHY THE THREE ARE ONE TYPE. The rows must be contiguous (`ContentFacts` hands out spans of them), every
    /// row's spans point into that row's OWN storage, and the order must stay a permutation of the rows sorted
    /// by the lookup's key - so a row appearing or leaving moves all three. Before the order existed, the rows
    /// and their storage were two vectors kept in step BY HAND at five erasure sites; a slip there is a row
    /// whose spans point at its neighbour's bytes, i.e. a wrong picture with no refusal anywhere, and the order
    /// would have added a third vector to the same discipline. Here the only mutators are the four operations
    /// below, and each of them refreshes the order, so "the order is a permutation of the rows" is true by
    /// construction rather than by remembering to set a flag.
    ///
    /// The order is refreshed on EVERY mutation (a content change, not a frame) and never per lookup: a frame
    /// that changes nothing touches none of this (see tablesFor).
    template <typename Row, typename Storage, void (*MakeOrder)(std::span<const Row>, std::vector<std::uint32_t>&)>
    class Table
    {
      public:
        /// @brief Appends one row and its storage. A null storage is allowed and keeps its empty slot.
        void append(const Row& row, std::unique_ptr<Storage> cell)
        {
            rows_.push_back(row);
            storage_.push_back(std::move(cell));
            refresh();
        }

        /// @brief Replaces one row and its storage, giving the replaced storage back to the caller.
        ///
        /// The storage comes back because a row's spans point INTO it: the material table parks the value it
        /// replaced, and that parked row has to keep pointing at the bytes it described (see the file note).
        [[nodiscard]] std::unique_ptr<Storage> replace(std::size_t index, const Row& row,
                                                      std::unique_ptr<Storage> cell)
        {
            std::unique_ptr<Storage> replaced = std::move(storage_[index]);
            rows_[index]                      = row;
            storage_[index]                   = std::move(cell);
            refresh();
            return replaced;
        }

        /// @brief Erases every row @p drop names, with its storage.
        template <typename Predicate>
        void eraseIf(Predicate drop)
        {
            for (std::size_t index = 0U; index < rows_.size();)
            {
                if (!drop(rows_[index]))
                {
                    ++index;
                    continue;
                }
                rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
                storage_.erase(storage_.begin() + static_cast<std::ptrdiff_t>(index));
            }
            refresh();
        }

        /// @brief Forgets every row, its storage and the order (the session-teardown path).
        void clear()
        {
            rows_.clear();
            storage_.clear();
            order_.clear();
        }

        /** @brief Gets the rows (contiguous: `ContentFacts` hands out spans of them). */
        [[nodiscard]] std::span<const Row> rows() const noexcept { return rows_; }

        /** @brief Gets the row order the lookups of this table search (see api/ContentFacts). */
        [[nodiscard]] std::span<const std::uint32_t> order() const noexcept { return order_; }

        /** @brief Gets how many rows the table carries. */
        [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }

      private:
        /// @brief Rebuilds the order from the rows (the only thing that ever writes it).
        void refresh() { MakeOrder(rows_, order_); }

        std::vector<Row>                      rows_;
        std::vector<std::unique_ptr<Storage>> storage_;  ///< One per row, in row order.
        std::vector<std::uint32_t>            order_;    ///< A permutation of `rows_`, sorted by the lookup key.
    };

    // The live set: the objects the host tracked, held so the address key cannot be recycled.
    std::unordered_map<const vn::graphics::Geometry*, LiveGeometry>     geometries;
    std::unordered_map<const vn::graphics::ShaderProgram*, LiveProgram> programs;
    std::unordered_map<const vn::graphics::Material*, LiveMaterial>     materials;

    // The tables (see Table: rows, their storage and their row order together).
    Table<GeometryFacts, GeometryStorage, &orderGeometryRows> geometry;
    Table<ProgramFacts, NoStorage, &orderProgramRows>         program;
    Table<MaterialFacts, MaterialStorage, &orderMaterialRows> material;

    ContentFacts  facts{};
    std::uint64_t builds{0};
    std::size_t   retained{0};
    /// Cells a refused park left: no parking window means "keep", never "free early".
    std::vector<std::shared_ptr<void>> kept;

    /**
     * @brief Gets the tables as the lookups see them: the rows, and the row order each is searched by.
     *
     * Built on demand rather than cached, because the walk MUTATES the tables while it runs: a cached view
     * would hold spans into a vector that has since reallocated (the arrays live in the tables, the view does
     * not own them). Six spans are cheap, and the answer it produces is always the table's current one.
     *
     * @return The tables and their orders.
     */
    [[nodiscard]] ContentFacts view() const noexcept
    {
        ContentFacts tables;
        tables.programs   = program.rows();
        tables.geometries = geometry.rows();
        tables.materials  = material.rows();
        tables.program_order  = program.order();
        tables.geometry_order = geometry.order();
        tables.material_order = material.order();
        return tables;
    }

    /** @brief Removes the geometry rows of one identity at one revision (a parked supersession). */
    void eraseGeometryRows(const void* identity, std::uint64_t revision) noexcept
    {
        geometry.eraseIf([identity, revision](const GeometryFacts& row) {
            return row.geometry == identity && row.revision == revision;
        });
    }

    /** @brief Removes every geometry row of one identity (an abandoned object's rows). */
    void eraseGeometryOf(const void* identity) noexcept
    {
        geometry.eraseIf([identity](const GeometryFacts& row) { return row.geometry == identity; });
    }

    /** @brief Removes the program rows of one identity at one revision (a parked supersession). */
    void eraseProgramRows(const void* identity, std::uint64_t revision) noexcept
    {
        program.eraseIf([identity, revision](const ProgramFacts& row) {
            return row.program == identity && row.revision == revision;
        });
    }

    /** @brief Removes every program row of one identity (an abandoned object's rows). */
    void eraseProgramOf(const void* identity) noexcept
    {
        program.eraseIf([identity](const ProgramFacts& row) { return row.program == identity; });
    }

    /** @brief Removes every material row of one identity (an abandoned object's rows). */
    void eraseMaterialOf(const void* identity) noexcept
    {
        material.eraseIf([identity](const MaterialFacts& row) { return row.material == identity; });
    }

    /** @brief Gets the row that answers for a geometry right now (the one built at its live revision).
     *
     * It asks through the SAME lookup the recording asks with (`findGeometry`), so what this walk believes and
     * what the recording will find cannot drift apart - and the question is a bisection of the row order
     * rather than a walk of the table (the walk asks it once per command, see api/ContentFacts).
     */
    [[nodiscard]] const GeometryFacts* liveGeometry(const void* identity, std::uint64_t revision) const noexcept
    {
        return findGeometry(view(), identity, revision).entry;
    }

    /** @brief Gets the row that answers for a material right now (identity is the whole key). */
    [[nodiscard]] const MaterialFacts* liveMaterial(const void* identity) const noexcept
    {
        return findMaterial(view(), identity).entry;
    }
};

ContentStore::ContentStore() :
    d(std::make_shared<Data>())
{
}

ContentStore::~ContentStore() = default;

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::Geometry>& geometry)
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

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::ShaderProgram>& program)
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

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::Material>& material)
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

void ContentStore::updateMaterial(vn::raw_ptr<vn::graphics::Material> material)
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
                ensureProgram(static_cast<const vn::graphics::ShaderProgram*>(draw.program.program),
                              ProgramVariant{}, true, timeline, retirement);
                continue;
            }

            for (const core::CompiledCommand& command : draw.commands)
            {
                const auto* geometry = static_cast<const vn::graphics::Geometry*>(command.geometry);
                const auto* material = static_cast<const vn::graphics::Material*>(command.material);

                ensureGeometry(geometry, timeline, retirement);
                ensureMaterial(material, timeline, retirement);

                const auto* program = static_cast<const vn::graphics::ShaderProgram*>(command.program.program);
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

    d->facts.programs   = d->program.rows();
    d->facts.geometries = d->geometry.rows();
    d->facts.materials  = d->material.rows();

    // The tables are published WITH THEIR ROW ORDER, which is what turns the recording's lookups into
    // bisections instead of whole-table scans (see api/ContentFacts). Nothing is built here: each table owns
    // its order and refreshes it when a row moves, so this is the copy of six spans that the frame reads.
    d->facts.program_order  = d->program.order();
    d->facts.geometry_order = d->geometry.order();
    d->facts.material_order = d->material.order();
    return d->facts;
}

void ContentStore::ensureGeometry(const vn::graphics::Geometry* geometry, core::FrameTimeline& timeline,
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

    d->geometry.append(fresh, std::move(storage));
    ++d->builds;
}

void ContentStore::ensureMaterial(const vn::graphics::Material* material, core::FrameTimeline& timeline,
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
    // REPLACED, and only the value it had is parked (see the file note). WHICH row is the same question the
    // recording asks, so it is asked the same way - through the lookup that owns the rule.
    const MaterialFacts* const live_row = d->liveMaterial(material);
    if (live_row == nullptr)
    {
        d->material.append(fresh, std::move(storage));
        ++d->builds;
        return;
    }
    const std::size_t row = static_cast<std::size_t>(live_row - d->material.rows().data());

    // The value being replaced goes on living in the parked cell: the row's block span points into the
    // storage, so the storage travels WITH the value rather than being overwritten where it lies.
    const std::shared_ptr<Data::RetiredMaterial> cell = std::make_shared<Data::RetiredMaterial>();
    cell->facts                                      = *live_row;
    cell->storage                                    = d->material.replace(row, fresh, std::move(storage));
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

void ContentStore::ensureProgram(const vn::graphics::ShaderProgram* program, const ProgramVariant& variant,
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
    d->program.append(fresh, {});
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
    d->geometry.clear();
    d->program.clear();
    d->material.clear();
    d->facts    = ContentFacts{};
    d->retained = 0U;
    d->kept.clear();
}

std::size_t ContentStore::geometryEntries() const noexcept
{
    return d->geometry.size();
}

std::size_t ContentStore::programEntries() const noexcept
{
    return d->program.size();
}

std::size_t ContentStore::materialEntries() const noexcept
{
    return d->material.size();
}

std::uint64_t ContentStore::builds() const noexcept
{
    return d->builds;
}

std::size_t ContentStore::retained() const noexcept
{
    return d->retained;
}

VN_VSG_NS_END
