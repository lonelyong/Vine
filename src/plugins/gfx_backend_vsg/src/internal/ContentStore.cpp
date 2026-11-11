#include <vine/vsg/internal/ContentStore.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include <vine/vsg/internal/ContentSources.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>

VN_VSG_NS_BEGIN

struct ContentStore::Data {
    struct LiveGeometry {
        /// The object the identity names, kept ONLY to reserve its address (ADR-2026-11-09): the rows answer by
        /// address, so an allocator handing that address to a later geometry would serve the dead one's facts. Null
        /// when the identity came from a value producer, whose owner is the one that keeps it alive.
        vn::intrusive_ptr<vn::graphics::Geometry> object{};
        /// The shape the rows are built from. Immutable, so a new revision is a NEW value here (never an in-place edit).
        vn::graphics::GeometryDescriptionPtr shape{};
        bool                                 described{ false };
        std::uint64_t                        described_revision{ 0 };   // The revision a row was (attempted to be) built at.
        std::uint64_t                        release_token{ 0 };         // Bumped by release and by reuse.
        bool                                 release_requested{ false }; // A release wait is parked.
    };

    struct LiveProgram {
        vn::intrusive_ptr<vn::graphics::ShaderProgram> object{};
        bool                                           described{ false };
        std::uint64_t                                  described_revision{ 0 };
        std::vector<std::uint32_t>                     variants;              // Variants built at that revision (their bits()).
        bool                                           screen_built{ false }; // The full-screen entry, at that revision.
        std::uint64_t                                  release_token{ 0 };
        bool                                           release_requested{ false };
    };

    struct LiveMaterial {
        vn::intrusive_ptr<vn::graphics::MaterialBase> object{};
        /// The VALUES this entry answers for: what a frame published (see the file note on geometry's `shape`), or
        /// null for a material nobody published - a hand-built command, answered from the object's own cache.
        vn::graphics::MaterialDescriptionPtr values{};
        /// Which of its texture slots have already been reported in THIS episode (a bit per slot, see
        /// claimSlotReport). A new description - i.e. the material's values changed - re-arms it, because a
        /// host that fixes a map and breaks another one has a new thing to say.
        std::uint32_t slots_reported{ 0 };
        bool          described{ false };
        std::uint64_t described_revision{ 0 }; // The `values` revision the row was built at.
        std::uint64_t release_token{ 0 };
        bool          release_requested{ false };
    };

    struct GeometryStorage {
        std::vector<ChannelFacts>   channels;   // What the row's `GeometryFacts::channels` points at.
        std::vector<PrimitiveFacts> primitives; // What the row's `GeometryFacts::primitives` points at.
    };

    struct MaterialStorage {
        std::vector<std::byte> block; // What the row's `MaterialFacts::block` points at.
    };

    /// A replaced material's value and its storage, kept together for the parking window (see the file note).
    struct RetiredMaterial {
        MaterialFacts                    facts{};
        std::unique_ptr<MaterialStorage> storage{};
    };

    /// A program row's per-row storage: none. The type is here because the three tables move their rows the
    /// same way, and an EMPTY slot (a null pointer) costs nothing to carry beside the other two.
    struct NoStorage {};

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
    class Table {
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
        [[nodiscard]] std::unique_ptr<Storage> replace(std::size_t index, const Row& row, std::unique_ptr<Storage> cell)
        {
            std::unique_ptr<Storage> replaced = std::move(storage_[index]);
            rows_[index]                      = row;
            storage_[index]                   = std::move(cell);
            refresh();
            return replaced;
        }

        /// @brief Erases every row @p drop names, running @p before_erase on each one first.
        ///
        /// The callback is what lets a wrapper keep a INDEX beside the rows (see GeometryTable): it sees the row
        /// that is about to disappear, in the same single pass that removes it, so the two cannot drift and the
        /// order is refreshed once. `eraseIf` is this with nothing to do.
        template <typename Predicate, typename OnErase>
        void eraseWhere(Predicate drop, OnErase before_erase)
        {
            for (std::size_t index = 0U; index < rows_.size();) {
                if (!drop(rows_[index])) {
                    ++index;
                    continue;
                }
                before_erase(rows_[index]);
                rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
                storage_.erase(storage_.begin() + static_cast<std::ptrdiff_t>(index));
            }
            refresh();
        }

        /// @brief Erases every row @p drop names, with its storage.
        template <typename Predicate>
        void eraseIf(Predicate drop)
        {
            eraseWhere(drop, [](const Row&) {});
        }

        /// @brief Forgets every row, its storage and the order (the session-teardown path).
        void clear()
        {
            rows_.clear();
            storage_.clear();
            order_.clear();
        }

        /** @brief Gets the rows (contiguous: `ContentFacts` hands out spans of them). */
        [[nodiscard]] std::span<const Row> rows() const noexcept
        {
            return rows_;
        }

        /** @brief Gets the row order the lookups of this table search (see api/ContentFacts). */
        [[nodiscard]] std::span<const std::uint32_t> order() const noexcept
        {
            return order_;
        }

        /** @brief Gets how many rows the table carries. */
        [[nodiscard]] std::size_t size() const noexcept
        {
            return rows_.size();
        }

      private:
        /// @brief Rebuilds the order from the rows (the only thing that ever writes it).
        void refresh()
        {
            MakeOrder(rows_, order_);
        }

        std::vector<Row>                      rows_;
        std::vector<std::unique_ptr<Storage>> storage_; // One per row, in row order.
        std::vector<std::uint32_t>            order_;   // A permutation of `rows_`, sorted by the lookup key.
    };

    /// @brief The geometry rows AND the buffers they declare: one object, so a row cannot move without the
    ///        declaration index following it.
    ///
    /// WHY A WRAPPER. A residency table asks "does anything alive still name this buffer?" once per entry, and
    /// the answer is this index. Letting the rows and the index be two independent things would be the classic
    /// one-fact-two-places defect: every erase path would have to remember to decrement. Here there are two
    /// mutation paths (`append`, `eraseIf`) plus `clear`, and each keeps both sides in step in one method.
    ///
    /// WHAT THE COUNT IS FOR. One buffer may be declared many times (a channel per attribute, the index stream,
    /// the instance stream, and several rows sharing one arena), so membership is a REFCOUNT rather than a bit:
    /// an address leaves the index only when its last declaration goes, which is exactly what lets one shared
    /// upload outlive the first geometry that used it.
    class GeometryTable {
      public:
        /// @brief Appends one row and its storage, declaring the buffers it names.
        void append(const GeometryFacts& row, std::unique_ptr<GeometryStorage> cell)
        {
            table_.append(row, std::move(cell));
            declare(row, +1);
        }

        /// @brief Erases every row @p drop names, withdrawing the buffers those rows declared.
        template <typename Predicate>
        void eraseIf(Predicate drop)
        {
            table_.eraseWhere(drop, [this](const GeometryFacts& row) { declare(row, -1); });
        }

        /// @brief Forgets every row and every declaration (the session-teardown path).
        void clear()
        {
            table_.clear();
            declared_.clear();
        }

        /** @brief Gets the rows (contiguous: `ContentFacts` hands out spans of them). */
        [[nodiscard]] std::span<const GeometryFacts> rows() const noexcept
        {
            return table_.rows();
        }

        /** @brief Gets the row order the lookups search (see api/ContentFacts). */
        [[nodiscard]] std::span<const std::uint32_t> order() const noexcept
        {
            return table_.order();
        }

        /** @brief Gets how many rows the table carries. */
        [[nodiscard]] std::size_t size() const noexcept
        {
            return table_.size();
        }

        /** @brief Gets the declaration index the facts publish (borrowed for as long as those facts are). */
        [[nodiscard]] const std::unordered_map<const void*, std::uint32_t>& declaredBuffers() const noexcept
        {
            return declared_;
        }

      private:
        /// @brief Adds (@p direction +1) or withdraws (-1) every buffer one row names.
        void declare(const GeometryFacts& row, int direction)
        {
            for (const ChannelFacts& channel : row.channels) {
                declareOne(channel.values.get(), direction);
            }
            for (const PrimitiveFacts& primitive : row.primitives) {
                if (primitive.indices.has_value()) {
                    declareOne(primitive.indices->values.get(), direction);
                }
                declareOne(primitive.instances.get(), direction);
            }
        }

        /// @brief Adds or withdraws ONE declaration; the address leaves when its count reaches zero.
        void declareOne(const void* buffer, int direction)
        {
            if (buffer == nullptr) {
                return;
            }
            const auto found = declared_.find(buffer);
            if (direction > 0) {
                if (found == declared_.end()) {
                    declared_.emplace(buffer, 1U);
                }
                else {
                    ++found->second;
                }
                return;
            }
            if (found == declared_.end()) {
                return; // cannot happen for a row this table appended; never erase what is not there
            }
            if (--found->second == 0U) {
                declared_.erase(found);
            }
        }

        Table<GeometryFacts, GeometryStorage, &orderGeometryRows> table_;
        std::unordered_map<const void*, std::uint32_t>             declared_;
    };

    // The live set: the identities the host tracked, each holding the SHAPE it answers with. The description is held
    // so the value a row was built from cannot die under the table, and the identity is the address the content side
    // names (a node's), so an edit re-uses the entry instead of creating a second one.
    std::unordered_map<const void*, LiveGeometry>                       geometries;
    std::unordered_map<const vn::graphics::ShaderProgram*, LiveProgram> programs;
    std::unordered_map<const vn::graphics::MaterialBase*, LiveMaterial> materials;

    /** @brief Cancels a pending release because the object is being used again.
     *
     * The token moves even when no release was requested, so a callback from a previous release can never
     * match after the object has been tracked again (see releaseGeometry).
     */
    template <typename Live>
    static void markUsed(Live& live) noexcept
    {
        live.release_requested = false;
        ++live.release_token;
    }

    // The tables (see Table: rows, their storage and their row order together).
    GeometryTable                                            geometry;
    Table<ProgramFacts, NoStorage, &orderProgramRows>         program;
    Table<MaterialFacts, MaterialStorage, &orderMaterialRows> material;

    ContentFacts  facts{};
    std::uint64_t builds{ 0 };
    /// Raised for every program row appended: what tells two descriptions of one address apart (see
    /// ProgramFacts::generation - the compiled halves key on it).
    std::uint64_t descriptions{ 0 };
    std::size_t   retained{ 0 };
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
        tables.programs       = program.rows();
        tables.geometries     = geometry.rows();
        tables.materials      = material.rows();
        tables.program_order  = program.order();
        tables.geometry_order = geometry.order();
        tables.material_order = material.order();
        // The declaration index travels WITH the rows (see ContentDeclarations): a residency table asks it
        // instead of walking the rows, so its per-frame cost does not follow the size of the content.
        tables.declarations = ContentDeclarations{ &geometry.declaredBuffers() };
        return tables;
    }

    /** @brief Removes the geometry rows of one identity at one revision (a superseded shape). */
    void eraseGeometryRows(const void* identity, std::uint64_t revision) noexcept
    {
        geometry.eraseIf([identity, revision](const GeometryFacts& row) { return row.geometry == identity && row.revision == revision; });
    }

    /** @brief Removes every geometry row of one identity (an abandoned object's rows). */
    void eraseGeometryOf(const void* identity) noexcept
    {
        geometry.eraseIf([identity](const GeometryFacts& row) { return row.geometry == identity; });
    }

    /** @brief Removes the program rows of one identity at one revision (a parked supersession). */
    void eraseProgramRows(const void* identity, std::uint64_t revision) noexcept
    {
        program.eraseIf([identity, revision](const ProgramFacts& row) { return row.program == identity && row.revision == revision; });
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
    /** @brief Gets the row that answers for a TRACKED geometry right now (the one built at its live revision).
     *
     * It asks through the SAME lookup the recording asks with (`findGeometry`), so what this walk believes and
     * what the recording will find cannot drift apart - and the question is a bisection of the row order
     * rather than a walk of the table (the walk asks it once per command, see api/ContentFacts).
     *
     * The membership check comes FIRST, and it is load-bearing: the revision has to be read from the shape the entry
     * holds, and a plan may name one the host has already let go of - a parked row still answers for it (see
     * releaseAbandoned), but the address itself may be freed memory, so it must not be read. A row that
     * EXISTS holds a share (LiveGeometry::object), and that share is the license to touch the shape; a
     * row-less address answers nullptr without being read.
     */
    [[nodiscard]] const GeometryFacts* liveGeometry(const void* identity) const noexcept
    {
        if (identity == nullptr) {
            return nullptr;
        }
        const auto entry = geometries.find(identity);
        if (entry == geometries.end() || entry->second.shape == nullptr) {
            return nullptr;
        }
        return findGeometry(view(), identity, entry->second.shape->revision()).entry;
    }

    /** @brief Gets the row that answers for a material right now (identity is the whole key). */
    [[nodiscard]] const MaterialFacts* liveMaterial(const void* identity) const noexcept
    {
        return findMaterial(view(), identity).entry;
    }
};

ContentStore::ContentStore()
  : d(std::make_shared<Data>())
{}

ContentStore::~ContentStore() = default;

void ContentStore::track(const void* identity, const vn::graphics::GeometryDescriptionPtr& description)
{
    if (identity == nullptr || description == nullptr) {
        return;
    }
    const auto [entry, inserted] = d->geometries.try_emplace(identity);
    (void)inserted;
    Data::LiveGeometry& live = entry->second;
    Data::markUsed(live); // tracked by a frame: a pending release is cancelled
    // A NEW SHAPE AT THE SAME IDENTITY. The rows the tables answer for the old revision stay answerable until the
    // frames that may still record them are past (see ensureGeometry, which parks them when it notices the move), so
    // nothing is dropped here - the value is replaced and the revision compare does the rest.
    live.shape = description;
}

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::Geometry>& geometry)
{
    if (geometry == nullptr) {
        return;
    }
    // ONE CALL, TWO FACTS: the object reserves the address the rows are keyed by (see LiveGeometry::object), and its
    // current shape is what a row is built from. This is the content side's door; a frame with no object names the two
    // separately (see the overload above).
    const auto [entry, inserted] = d->geometries.try_emplace(geometry.get());
    (void)inserted;
    Data::LiveGeometry& live = entry->second;
    Data::markUsed(live);
    live.object = geometry;
    live.shape  = geometry->description();
}

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::ShaderProgram>& program)
{
    if (program == nullptr) {
        return;
    }
    const auto [entry, inserted] = d->programs.try_emplace(program.get());
    if (inserted) {
        entry->second.object = program;
    }
    else {
        Data::markUsed(entry->second);
    }
}

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::MaterialBase>& material)
{
    if (material == nullptr) {
        return;
    }
    const auto [entry, inserted] = d->materials.try_emplace(material.get());
    if (inserted) {
        entry->second.object = material;
    }
    else {
        Data::markUsed(entry->second);
    }
}

void ContentStore::track(const vn::graphics::MaterialBase* identity, const vn::graphics::MaterialDescriptionPtr& values)
{
    if (identity == nullptr || values == nullptr) {
        return;
    }
    const auto entry = d->materials.find(identity);
    if (entry == d->materials.end()) {
        // Untracked: nothing draws with it, so nothing can be asked about it.
        // Unlike a geometry value (which a mirrored row may name alone), a
        // material always arrives with the share that tracked it - so an entry
        // is not created here.
        return;
    }
    Data::LiveMaterial& live = entry->second;
    Data::markUsed(live); // tracked by a frame: a pending release is cancelled
    // A NEW VALUE STARTS A NEW EPISODE for the slot reports: the values changed, so what is wrong with them may
    // have too (see claimSlotReport) - and the report is asked BEFORE the next frame describes the value, which is
    // why it is re-armed here and not in ensureMaterial.
    if (live.described && live.described_revision != values->revision()) {
        live.slots_reported = 0U;
    }
    // A NEW VALUE AT THE SAME IDENTITY. Replacing it is enough: `ensureMaterial` compares the value's revision with the
    // one its row was built at, parks what it replaces and builds the rest, so a steady frame compares an integer and a
    // material edit is one new value at the next publish (see the class note).
    live.values = values;
}

bool ContentStore::claimSlotReport(const void* identity, std::size_t slot) noexcept
{
    const auto entry = d->materials.find(static_cast<const vn::graphics::MaterialBase*>(identity));
    if (entry == d->materials.end() || slot >= vn::graphics::kTextureSlotCount) {
        // Nothing to remember against (an untracked material, or no slot): say "report it", because a caller
        // that says nothing at all would hide a real problem; the recorder's own per-pass state is what keeps
        // that from repeating every frame.
        return true;
    }
    const std::uint32_t bit = 1U << slot;
    if ((entry->second.slots_reported & bit) != 0U) {
        return false; // already said in this episode
    }
    entry->second.slots_reported |= bit;
    return true;
}

const ContentFacts& ContentStore::tablesFor(const core::CompiledFrame& frame, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    for (const core::CompiledPass& pass : frame.passes) {
        for (const core::CompiledDraw& draw : pass.draws) {
            if (draw.kind == core::DrawKind::Screen) {
                // The full-screen ABI is the engine's and carries no variant: a screen program is one text
                // (see api/ContentSources).
                ensureProgram(static_cast<const vn::graphics::ShaderProgram*>(draw.program.program), ProgramVariant{}, true, timeline, retirement);
                continue;
            }

            for (const core::CompiledCommand& command : draw.commands) {
                const void* identity = command.geometry;
                const auto* material = static_cast<const vn::graphics::MaterialBase*>(command.material);

                ensureGeometry(identity, timeline, retirement);
                ensureMaterial(material, timeline, retirement);

                const auto* program = static_cast<const vn::graphics::ShaderProgram*>(command.program.program);
                if (program == nullptr) {
                    continue;
                }

                // The variant is a fact of the DRAWABLE, so it is computed from the entries the same walk
                // just produced. A drawable whose geometry or material got no entry is skipped here: the
                // recorder refuses it on those two, and the program's text never decides anything for it.
                const GeometryFacts* live_geometry = d->liveGeometry(identity);
                const MaterialFacts* live_material = d->liveMaterial(material);
                if (live_geometry == nullptr || live_material == nullptr) {
                    continue;
                }
                ProgramVariant variant = variantOf(*live_material, *live_geometry);
                variant.instanced      = command.instance_buffer != nullptr;
                ensureProgram(program, variant, false, timeline, retirement);
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
    d->facts.declarations   = ContentDeclarations{ &d->geometry.declaredBuffers() };
    return d->facts;
}

void ContentStore::ensureGeometry(const void* identity, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    if (identity == nullptr) {
        return;
    }
    const auto entry = d->geometries.find(identity);
    if (entry == d->geometries.end()) {
        // Untracked: the tables answer nothing for it, and the recorder reports the miss. The plan carries only the
        // identity, so there is no shape to describe either - a frame that draws something must hand it over first
        // (see track).
        return;
    }

    Data::LiveGeometry& live = entry->second;
    Data::markUsed(live); // ensured by a frame: a pending release is cancelled
    if (live.shape == nullptr) {
        return;
    }
    const std::uint64_t revision = live.shape->revision();
    if (live.described && live.described_revision == revision) {
        return; // the steady frame: nothing to build, nothing to park
    }

    if (live.described) {
        // The shape moved: what the table answers for the old revision stays answerable until the frames that may
        // still record it are past (see the file note), and then leaves.
        const std::uint64_t       retired = live.described_revision;
        const std::weak_ptr<Data> weak    = d;
        const bool                parked  = retirement.retire(timeline, [weak, identity, retired]() {
            if (const std::shared_ptr<Data> data = weak.lock()) {
                data->eraseGeometryRows(identity, retired);
            }
        });
        if (!parked) {
            ++d->retained; // no window: the rows are kept, which costs memory and never correctness
        }
    }

    auto          storage = std::make_unique<Data::GeometryStorage>();
    GeometryFacts fresh;
    const FactMiss miss = buildGeometryFacts(*live.shape, fresh, storage->channels, storage->primitives);
    live.described          = true;
    // The ATTEMPT, not the success: a shape that failed is not retried until the revision moves.
    live.described_revision = revision;
    if (miss != FactMiss::None) {
        return;
    }

    // The row is keyed by the IDENTITY the plan names, not by the description it was built from: the description is a
    // value, the identity is what a command (and therefore a lookup) carries.
    fresh.geometry = identity;
    d->geometry.append(fresh, std::move(storage));
    ++d->builds;
}

void ContentStore::ensureMaterial(const vn::graphics::MaterialBase* material, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    auto entry = d->materials.find(material);
    if (entry == d->materials.end()) {
        if (material != nullptr) {
            return; // untracked (see ensureGeometry)
        }
        // The default material is an entry like any other (see api/ContentSources) and needs no tracking:
        // content without a material can be described at any time.
        entry = d->materials.try_emplace(nullptr).first;
    }

    Data::LiveMaterial& live = entry->second;
    Data::markUsed(live); // ensured by a frame: a pending release is cancelled

    // THE VALUES A ROW IS BUILT FROM: the ones the frame published. A material no frame published (a command a host
    // built by hand) is answered from the object's own cache - the same door every hand-built command uses, and the
    // reason that command is the host's to keep still.
    vn::graphics::MaterialDescriptionPtr values = live.values;
    if (values == nullptr && material != nullptr) {
        values = material->description();
    }
    const std::uint64_t revision = values != nullptr ? values->revision() : 0;
    if (live.described && live.described_revision == revision) {
        return; // the steady frame: nothing to build, nothing to park
    }

    auto           storage = std::make_unique<Data::MaterialStorage>();
    MaterialFacts  fresh;
    const FactMiss miss     = buildMaterialFacts(material, values, revision, fresh, storage->block);
    live.described          = true;
    live.described_revision = revision;
    // A fresh description starts a fresh episode for the material's slot reports (see claimSlotReport).
    live.slots_reported     = 0U;
    if (miss != FactMiss::None) {
        return;
    }

    // The material's lookup is by identity alone, so the table has to answer "the material now": the row is
    // REPLACED, and only the value it had is parked (see the file note). WHICH row is the same question the
    // recording asks, so it is asked the same way - through the lookup that owns the rule.
    const MaterialFacts* const live_row = d->liveMaterial(material);
    if (live_row == nullptr) {
        d->material.append(fresh, std::move(storage));
        ++d->builds;
        return;
    }
    const std::size_t row = static_cast<std::size_t>(live_row - d->material.rows().data());

    // The value being replaced goes on living in the parked cell: the row's block span points into the
    // storage, so the storage travels WITH the value rather than being overwritten where it lies.
    const std::shared_ptr<Data::RetiredMaterial> cell = std::make_shared<Data::RetiredMaterial>();
    cell->facts                                       = *live_row;
    cell->storage                                     = d->material.replace(row, fresh, std::move(storage));
    ++d->builds;

    const bool parked = retirement.retire(timeline, [cell]() { (void)cell; });
    if (!parked) {
        // The value stays reachable for the session: `kept` owns it, so a recorded frame that still points
        // at the old block bytes never dangles.
        const std::shared_ptr<void> held = cell;
        d->kept.push_back(held);
        ++d->retained;
    }
}

void ContentStore::ensureProgram(const vn::graphics::ShaderProgram* program,
                                 const ProgramVariant&              variant,
                                 bool                               screen,
                                 core::FrameTimeline&               timeline,
                                 core::RetirementQueue&             retirement)
{
    if (program == nullptr) {
        return;
    }
    const auto entry = d->programs.find(program);
    if (entry == d->programs.end()) {
        return; // untracked (see ensureGeometry)
    }

    Data::LiveProgram& live = entry->second;
    Data::markUsed(live); // ensured by a frame: a pending release is cancelled
    const std::uint64_t revision = program->revision();
    if (live.described && live.described_revision == revision) {
        const bool built = screen ? live.screen_built : std::find(live.variants.begin(), live.variants.end(), variant.bits()) != live.variants.end();
        if (built) {
            return; // this text of this revision is already in the tables
        }
    }
    else {
        if (live.described) {
            const void* const         identity = program;
            const std::uint64_t       retired  = live.described_revision;
            const std::weak_ptr<Data> weak     = d;
            const bool                parked   = retirement.retire(timeline, [weak, identity, retired]() {
                if (const std::shared_ptr<Data> data = weak.lock()) {
                    data->eraseProgramRows(identity, retired);
                }
            });
            if (!parked) {
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
    const FactMiss miss = screen ? buildScreenProgramFacts(*program, fresh) : buildProgramFacts(*program, variant, fresh);
    if (miss != FactMiss::None) {
        return; // a program that is not one content pipeline: no entry, and the recorder says so
    }

    if (screen) {
        live.screen_built = true;
    }
    else {
        live.variants.push_back(variant.bits());
    }
    fresh.generation = ++d->descriptions; // a new description: a later program row at this address cannot be mistaken for it
    d->program.append(fresh, {});
    ++d->builds;
}

std::size_t ContentStore::releaseAbandoned(core::FrameTimeline& timeline, core::RetirementQueue& retirement, std::vector<const void*>& released_materials)
{
    const std::weak_ptr<Data> weak     = d;
    std::size_t               released = 0U;
    released_materials.clear();

    // THE SHARE GOES WITH THE PARK, for all three tables. A row is keyed by the object's ADDRESS, so the address
    // must stay reserved for exactly as long as the row can be read: releasing it here would let the allocator
    // hand the same address to a later geometry / program / material, and the park that runs afterwards erases
    // rows BY IDENTITY - including the rows the new object just had described. Measured as the failure this fixes:
    // a shading switch builds a fresh program at a just-freed address, its rows are erased by the old object's
    // park at the end of that same frame, `ensureProgram` still believes the program is described, and the pass
    // then has no compiled content half at all - so the whole pass (clear included) stops being drawn, for good,
    // until another program object happens to land elsewhere. The parked release owns the object and drops it
    // with the rows; a park that cannot be taken KEEPS the entry, never frees the address early.
    for (auto it = d->geometries.begin(); it != d->geometries.end();) {
        // WHAT "NOBODY ELSE HOLDS IT" MEANS: the object (when the identity named one) is the content's; a shape with no
        // object came from a value producer, whose owner is the one that keeps it alive. Either way, a share besides the
        // store's own means the thing is still in use.
        const unsigned long held_outside = it->second.object != nullptr ? it->second.object->useCount() : (it->second.shape != nullptr ? it->second.shape->useCount() : 0UL);
        if (held_outside > 1UL) {
            ++it;
            continue;
        }
        const void* const                          identity    = it->first;
        const vn::graphics::GeometryDescriptionPtr held        = it->second.shape;
        const vn::intrusive_ptr<vn::graphics::Geometry> object_held = it->second.object;
        const bool                                 parked      = retirement.retire(timeline, [weak, identity, held, object_held]() {
            (void)held;        // the address stays reserved until this runs, and dies with the rows it guards
            (void)object_held; // and so does the object that names it
            if (const std::shared_ptr<Data> data = weak.lock()) {
                data->eraseGeometryOf(identity);
            }
        });
        if (!parked) {
            ++d->retained; // no window yet: keep the description, its rows and its address; retry on a later frame
            ++it;
            continue;
        }
        it = d->geometries.erase(it);
        ++released;
    }

    for (auto it = d->programs.begin(); it != d->programs.end();) {
        if (it->second.object != nullptr && it->second.object->useCount() > 1UL) {
            ++it;
            continue;
        }
        const void* const                                    identity = it->first;
        const vn::intrusive_ptr<vn::graphics::ShaderProgram> held     = it->second.object;
        const bool                                           parked   = retirement.retire(timeline, [weak, identity, held]() {
            (void)held;
            if (const std::shared_ptr<Data> data = weak.lock()) {
                data->eraseProgramOf(identity);
            }
        });
        if (!parked) {
            ++d->retained;
            ++it;
            continue;
        }
        it = d->programs.erase(it);
        ++released;
    }

    for (auto it = d->materials.begin(); it != d->materials.end();) {
        if (it->first == nullptr) {
            ++it; // the default material is not an object anyone can abandon
            continue;
        }
        if (it->second.object != nullptr && it->second.object->useCount() > 1UL) {
            ++it;
            continue;
        }
        const void* const                                   identity = it->first;
        const vn::intrusive_ptr<vn::graphics::MaterialBase> held     = it->second.object;
        const bool                                          parked   = retirement.retire(timeline, [weak, identity, held]() {
            (void)held;
            if (const std::shared_ptr<Data> data = weak.lock()) {
                data->eraseMaterialOf(identity);
            }
        });
        if (!parked) {
            ++d->retained;
            ++it;
            continue;
        }
        // The arena slot follows the row: the material is gone, so its storage is nobody's to keep. The
        // identity is a KEY only (the object may already be destroyed - see the declaration).
        released_materials.push_back(identity);
        it = d->materials.erase(it);
        ++released;
    }

    return released;
}

bool ContentStore::releaseGeometry(const void* identity, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    if (identity == nullptr) {
        return false;
    }
    const auto entry = d->geometries.find(identity);
    if (entry == d->geometries.end()) {
        return true; // not resident: the release is already true of this identity
    }
    Data::LiveGeometry& live = entry->second;
    if (live.release_requested) {
        return true; // one request is enough; a second callback would only repeat the removal
    }

    const std::uint64_t                        token  = ++live.release_token;
    live.release_requested                             = true;
    const std::weak_ptr<Data>                  weak   = d;
    // The address is what the rows are keyed by, so it must stay reserved until the park runs (see
    // releaseAbandoned): the lambda owns the shape and drops it together with the rows it erases.
    const vn::graphics::GeometryDescriptionPtr held   = live.shape;
    const vn::intrusive_ptr<vn::graphics::Geometry> object_held = live.object;
    const bool                                 parked = retirement.retire(timeline, [weak, identity, token, held, object_held]() {
        (void)held;
        (void)object_held; // the address stays reserved until this runs, and dies with the rows it guards
        const std::shared_ptr<Data> data = weak.lock();
        if (data == nullptr) {
            return;
        }
        const auto found = data->geometries.find(identity);
        if (found == data->geometries.end() || !found->second.release_requested || found->second.release_token != token) {
            return; // the identity was used again: this release was cancelled
        }
        data->geometries.erase(found);
        data->eraseGeometryOf(identity);
    });
    if (!parked) {
        // No in-flight window: keep everything, so the description's rows cannot vanish while a recorded frame
        // still names them. The token moves so a stale callback (there is none) could never match.
        live.release_requested = false;
        ++live.release_token;
        return false;
    }
    return true;
}

bool ContentStore::releaseGeometry(vn::raw_ptr<const vn::graphics::Geometry> geometry, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    return releaseGeometry(static_cast<const void*>(geometry), timeline, retirement);
}

bool ContentStore::releaseProgram(vn::raw_ptr<const vn::graphics::ShaderProgram> program, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    if (program == nullptr) {
        return false;
    }
    const auto entry = d->programs.find(program);
    if (entry == d->programs.end()) {
        return true;
    }
    Data::LiveProgram& live = entry->second;
    if (live.release_requested) {
        return true;
    }

    const std::uint64_t token                                   = ++live.release_token;
    live.release_requested                                      = true;
    const std::weak_ptr<Data> weak                              = d;
    // The address must stay reserved until the rows keyed by it are erased (see releaseAbandoned).
    const vn::intrusive_ptr<vn::graphics::ShaderProgram> held   = live.object;
    const bool                                           parked = retirement.retire(timeline, [weak, program, token, held]() {
        (void)held;
        const std::shared_ptr<Data> data = weak.lock();
        if (data == nullptr) {
            return;
        }
        const auto found = data->programs.find(program);
        if (found == data->programs.end() || !found->second.release_requested || found->second.release_token != token) {
            return;
        }
        data->programs.erase(found);
        data->eraseProgramOf(program);
    });
    if (!parked) {
        live.release_requested = false;
        ++live.release_token;
        return false;
    }
    return true;
}

bool ContentStore::releaseMaterial(vn::raw_ptr<const vn::graphics::MaterialBase> material, core::FrameTimeline& timeline, core::RetirementQueue& retirement)
{
    if (material == nullptr) {
        return false;
    }
    const auto entry = d->materials.find(material);
    if (entry == d->materials.end()) {
        return true;
    }
    Data::LiveMaterial& live = entry->second;
    if (live.release_requested) {
        return true;
    }

    const std::uint64_t token                                  = ++live.release_token;
    live.release_requested                                     = true;
    const std::weak_ptr<Data> weak                             = d;
    // The address must stay reserved until the rows keyed by it are erased (see releaseAbandoned).
    const vn::intrusive_ptr<vn::graphics::MaterialBase> held   = live.object;
    const bool                                          parked = retirement.retire(timeline, [weak, material, token, held]() {
        (void)held;
        const std::shared_ptr<Data> data = weak.lock();
        if (data == nullptr) {
            return;
        }
        const auto found = data->materials.find(material);
        if (found == data->materials.end() || !found->second.release_requested || found->second.release_token != token) {
            return;
        }
        data->materials.erase(found);
        data->eraseMaterialOf(material);
    });
    if (!parked) {
        live.release_requested = false;
        ++live.release_token;
        return false;
    }
    return true;
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

std::size_t ContentStore::materialCount() const noexcept
{
    // The default material (a null identity) is not an object the host holds: it is the fill-in for a command
    // that names none, and it does not get an arena slot of its own until it is noted like any other.
    const std::size_t all = d->materials.size();
    return (all != 0U && d->materials.find(nullptr) != d->materials.end()) ? all - 1U : all;
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
