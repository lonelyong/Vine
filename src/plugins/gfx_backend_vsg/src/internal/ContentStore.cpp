#include <vine/vsg/internal/ContentStore.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include <vine/vsg/internal/ContentSources.hpp>
#include <vine/vsg/internal/GeometryFacts.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>

VN_VSG_NS_BEGIN

namespace
{

/// @brief The identity a geometry row is counted under: an entry asks "how many rows still name me?".
[[nodiscard]] std::uint64_t geometryRowKey(const GeometryFacts& row) noexcept
{
    return row.geometry.id.bits();
}

/// @brief The identity a program row is counted under (see geometryRowKey).
[[nodiscard]] std::uint64_t programRowKey(const ProgramFacts& row) noexcept
{
    return row.program.id.bits();
}

/// @brief The identity a material row is counted under (see geometryRowKey).
[[nodiscard]] std::uint64_t materialRowKey(const MaterialFacts& row) noexcept
{
    return row.material.id.bits();
}

} // namespace

struct ContentStore::Data {
    struct LiveGeometry {
        /// The object the identity names, held so a row that borrows from it stays readable AND so "nobody else holds
        /// it" (@ref releaseAbandoned) can be asked at all. Null when the identity came from a value producer, whose
        /// owner is the one that keeps it alive.
        vn::intrusive_ptr<vn::graphics::Geometry> object{};
        /// The shape the rows are built from. Immutable, so a new revision is a NEW value here (never an in-place edit).
        vn::graphics::GeometryDescriptionPtr shape{};
        /// Whether a build was attempted at @ref described_revision. A shape that FAILED is not retried until it moves.
        bool described{ false };
        /// Whether that attempt produced a row. Cleared when the row ages out, so the identity is rebuilt when a plan
        /// names it again instead of looking up a row that is gone.
        bool          built{ false };
        std::uint64_t described_revision{ 0 }; // The revision a row was (attempted to be) built at.
        /// The host is done with this identity (@ref releaseGeometry). track() / ensureGeometry() clear it again, and
        /// the entry itself leaves once no row names it (see the file note).
        bool released{ false };
    };

    struct LiveProgram {
        vn::intrusive_ptr<vn::graphics::ShaderProgram> object{};
        bool                                           described{ false };
        std::uint64_t                                  described_revision{ 0 };
        std::vector<std::uint32_t>                     variants;              // Variants built at that revision (their bits()).
        bool                                           screen_built{ false }; // The full-screen entry, at that revision.
        bool                                           released{ false };
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
        /// Whether that attempt produced a row (see LiveGeometry::built).
        bool          built{ false };
        std::uint64_t described_revision{ 0 }; // The `values` revision the row was built at.
        bool          released{ false };
    };

    struct GeometryStorage {
        std::vector<ChannelFacts>   channels;   // What the row's `GeometryFacts::channels` points at.
        std::vector<PrimitiveFacts> primitives; // What the row's `GeometryFacts::primitives` points at.
    };

    struct MaterialStorage {
        std::vector<std::byte> block; // What the row's `MaterialFacts::block` points at.
    };

    /// A program row's per-row storage: none. The type is here because the three tables move their rows the
    /// same way, and an EMPTY slot (a null pointer) costs nothing to carry beside the other two.
    struct NoStorage {};

    /// One table: its rows, the per-row storage their spans point at, the row order its lookups search, and the
    /// EPOCH each row was last named by.
    ///
    /// WHY THEY ARE ONE TYPE. The rows must be contiguous (`ContentFacts` hands out spans of them), every
    /// row's spans point into that row's OWN storage, the order must stay a permutation of the rows sorted
    /// by the lookup's key, and a row's age must move with it - so a row appearing or leaving moves all four.
    /// Before the order existed, the rows and their storage were two vectors kept in step BY HAND at five
    /// erasure sites; a slip there is a row whose spans point at its neighbour's bytes, i.e. a wrong picture
    /// with no refusal anywhere. Here the only mutators are the operations below, and each of them moves all
    /// four in one place, so "the order is a permutation" and "the stamp belongs to its row" are true by
    /// construction rather than by remembering to set a flag.
    ///
    /// The order is refreshed on EVERY mutation (a content change, not a frame) and never per lookup: a frame
    /// that changes nothing touches none of this (see tablesFor).
    template <typename Row, typename Storage, void (*MakeOrder)(std::span<const Row>, std::vector<std::uint32_t>&), std::uint64_t (*KeyOf)(const Row&)>
    class Table {
      public:
        /// @brief Appends one row and its storage, stamped with @p epoch (the frame whose plan names it).
        void append(const Row& row, std::unique_ptr<Storage> cell, std::uint64_t epoch)
        {
            rows_.push_back(row);
            storage_.push_back(std::move(cell));
            epochs_.push_back(epoch);
            ++rows_of_[KeyOf(row)];
            refresh();
        }

        /// @brief Stamps @p row (a row of THIS table) with @p epoch: the frame whose plan just named it.
        void stamp(const Row* row, std::uint64_t epoch) noexcept
        {
            if (row != nullptr) {
                epochs_[static_cast<std::size_t>(row - rows_.data())] = epoch;
            }
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
                eraseAt(index);
            }
            refresh();
        }

        /// @brief Erases every row @p drop names, with its storage.
        template <typename Predicate>
        void eraseIf(Predicate drop)
        {
            eraseWhere(drop, [](const Row&) {});
        }

        /// @brief Erases every row no plan named since @p oldest_live that @p keep does not protect.
        ///
        /// THE RECLAMATION PASS (see the file note): `last_epoch < oldest_live` is the age rule, and @p keep is the
        /// other half - a row that is still its entry's LIVE answer stays however long ago a plan last named it (a
        /// culled drawable keeps its facts, and its uploads, for as long as the content exists). There is no
        /// per-entry park and no release token: the row's age and its entry are the whole lifetime.
        template <typename Keep, typename OnErase>
        std::size_t reclaimBefore(std::uint64_t oldest_live, Keep keep, OnErase before_erase)
        {
            std::size_t removed = 0U;
            for (std::size_t index = 0U; index < rows_.size();) {
                if (epochs_[index] >= oldest_live || keep(rows_[index])) {
                    ++index;
                    continue;
                }
                before_erase(rows_[index]);
                eraseAt(index);
                ++removed;
            }
            refresh();
            return removed;
        }

        /// @brief Forgets every row, its storage, its stamp and the order (the session-teardown path).
        void clear()
        {
            rows_.clear();
            storage_.clear();
            epochs_.clear();
            rows_of_.clear();
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

        /** @brief Gets how many rows one identity (a ref's id bits) still has - the entry-drop condition. */
        [[nodiscard]] std::size_t rowsOf(std::uint64_t key) const noexcept
        {
            const auto found = rows_of_.find(key);
            return found != rows_of_.end() ? static_cast<std::size_t>(found->second) : 0U;
        }

      private:
        /// @brief Erases row @p index with its storage and stamp (the one place the four vectors move).
        void eraseAt(std::size_t index)
        {
            withdraw(KeyOf(rows_[index]));
            rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(index));
            storage_.erase(storage_.begin() + static_cast<std::ptrdiff_t>(index));
            epochs_.erase(epochs_.begin() + static_cast<std::ptrdiff_t>(index));
        }

        /// @brief Takes one row off @p key 's count; the entry leaves when its last row does.
        void withdraw(std::uint64_t key) noexcept
        {
            const auto found = rows_of_.find(key);
            if (found == rows_of_.end()) {
                return; // cannot happen for a row this table appended; never erase what is not there
            }
            if (--found->second == 0U) {
                rows_of_.erase(found);
            }
        }

        /// @brief Rebuilds the order from the rows (the only thing that ever writes it).
        void refresh()
        {
            MakeOrder(rows_, order_);
        }

        std::vector<Row>                                 rows_;
        std::vector<std::unique_ptr<Storage>>            storage_; // One per row, in row order.
        std::vector<std::uint64_t>                       epochs_;  // One per row: the last frame whose plan named it.
        std::vector<std::uint32_t>                       order_;   // A permutation of `rows_`, sorted by the lookup key.
        std::unordered_map<std::uint64_t, std::uint32_t> rows_of_; // Rows per identity (see rowsOf).
    };

    /// @brief Address -> how many live rows name it: the index ContentFacts publishes (see ContentDeclarations).
    ///
    /// WHY ONE TYPE FOR BOTH KINDS. A geometry row names the buffers its streams read and a material row names the
    /// textures its slots sample; both are an address, both are asked as membership, and both are refcounted (one
    /// arena is named by several rows). Two lookalike maps is how the two would come to disagree about what "named"
    /// means - so the rule (add one on append, remove one on erase, leave at zero) is written once.
    class Declarations {
      public:
        /// @brief Adds one declaration of @p address (null is not a declaration).
        void add(const void* address)
        {
            if (address != nullptr) {
                ++counts_[address];
            }
        }

        /// @brief Withdraws one declaration of @p address; the address leaves when its count reaches zero.
        void remove(const void* address) noexcept
        {
            if (address == nullptr) {
                return;
            }
            const auto found = counts_.find(address);
            if (found == counts_.end()) {
                return; // cannot happen for a row this index saw; never erase what is not there
            }
            if (--found->second == 0U) {
                counts_.erase(found);
            }
        }

        /// @brief Forgets every declaration (the session-teardown path).
        void clear() noexcept
        {
            counts_.clear();
        }

        /// @brief Gets the index a `ContentDeclarations` borrows.
        [[nodiscard]] const std::unordered_map<const void*, std::uint32_t>& counts() const noexcept
        {
            return counts_;
        }

      private:
        std::unordered_map<const void*, std::uint32_t> counts_;
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
        /// @brief Appends one row and its storage, declaring the buffers it names and stamping it with @p epoch.
        void append(const GeometryFacts& row, std::unique_ptr<GeometryStorage> cell, std::uint64_t epoch)
        {
            table_.append(row, std::move(cell), epoch);
            declare(row, +1);
        }

        /// @brief Stamps @p row with @p epoch (the frame whose plan just named it).
        void stamp(const GeometryFacts* row, std::uint64_t epoch) noexcept
        {
            table_.stamp(row, epoch);
        }

        /// @brief Erases every row @p drop names, withdrawing the buffers those rows declared.
        template <typename Predicate>
        void eraseIf(Predicate drop)
        {
            table_.eraseWhere(drop, [this](const GeometryFacts& row) { declare(row, -1); });
        }

        /// @brief Erases every row no plan named since @p oldest_live that @p keep does not protect, withdrawing its
        ///        declarations.
        template <typename Keep, typename OnErase>
        std::size_t reclaimBefore(std::uint64_t oldest_live, Keep keep, OnErase after_declare)
        {
            return table_.reclaimBefore(oldest_live, keep, [this, &after_declare](const GeometryFacts& row) {
                declare(row, -1);
                after_declare(row);
            });
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

        /** @brief Gets how many rows one identity still has (see Table::rowsOf). */
        [[nodiscard]] std::size_t rowsOf(std::uint64_t key) const noexcept
        {
            return table_.rowsOf(key);
        }

        /** @brief Gets the declaration index the facts publish (borrowed for as long as those facts are). */
        [[nodiscard]] const std::unordered_map<const void*, std::uint32_t>& declaredBuffers() const noexcept
        {
            return declared_.counts();
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

        /// @brief Adds or withdraws ONE declaration.
        void declareOne(const void* buffer, int direction)
        {
            if (direction > 0) {
                declared_.add(buffer);
            }
            else {
                declared_.remove(buffer);
            }
        }

        Table<GeometryFacts, GeometryStorage, &orderGeometryRows, &geometryRowKey> table_;
        Declarations                                                               declared_;
    };

    /// @brief The material rows AND the textures they sample: GeometryTable's rule for the other kind.
    ///
    /// WHY THE TEXTURES NEED ONE. The image cache's residency key is the texture's ADDRESS plus its revision (see
    /// api/MaterialImages), so "is this upload still wanted?" is asked once per cached image and must not cost a
    /// walk over the material rows - a scene with culled draws keeps its rows, so the walk would be paid on frames
    /// that draw nothing. A slot's map is declared for exactly as long as the row that names it answers, which is
    /// the same lifetime the row's bytes have; a material edit replaces the row and the declaration with it.
    class MaterialTable {
      public:
        /// @brief Appends one row and its storage, declaring the textures it samples and stamping it with @p epoch.
        void append(const MaterialFacts& row, std::unique_ptr<MaterialStorage> cell, std::uint64_t epoch)
        {
            table_.append(row, std::move(cell), epoch);
            declare(row, +1);
        }

        /// @brief Stamps @p row with @p epoch (the frame whose plan just named it).
        void stamp(const MaterialFacts* row, std::uint64_t epoch) noexcept
        {
            table_.stamp(row, epoch);
        }

        /// @brief Erases every row no plan named since @p oldest_live that @p keep does not protect, withdrawing its
        ///        declarations.
        template <typename Keep, typename OnErase>
        std::size_t reclaimBefore(std::uint64_t oldest_live, Keep keep, OnErase after_declare)
        {
            return table_.reclaimBefore(oldest_live, keep, [this, &after_declare](const MaterialFacts& row) {
                declare(row, -1);
                after_declare(row);
            });
        }

        /// @brief Forgets every row and every declaration (the session-teardown path).
        void clear()
        {
            table_.clear();
            declared_.clear();
        }

        /** @brief Gets the rows (contiguous: `ContentFacts` hands out spans of them). */
        [[nodiscard]] std::span<const MaterialFacts> rows() const noexcept
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

        /** @brief Gets how many rows one identity still has (see Table::rowsOf). */
        [[nodiscard]] std::size_t rowsOf(std::uint64_t key) const noexcept
        {
            return table_.rowsOf(key);
        }

        /** @brief Gets the texture index the facts publish (borrowed for as long as those facts are). */
        [[nodiscard]] const std::unordered_map<const void*, std::uint32_t>& declaredTextures() const noexcept
        {
            return declared_.counts();
        }

      private:
        /// @brief Adds (@p direction +1) or withdraws (-1) every texture one row's slots name.
        void declare(const MaterialFacts& row, int direction)
        {
            for (const MaterialSlotFacts& slot : row.slots) {
                if (direction > 0) {
                    declared_.add(slot.texture);
                }
                else {
                    declared_.remove(slot.texture);
                }
            }
        }

        Table<MaterialFacts, MaterialStorage, &orderMaterialRows, &materialRowKey> table_;
        Declarations                                                               declared_;
    };

    // The live set: the identities the host tracked, each holding the SHAPE it answers with. The description is held
    // so a row built from it stays readable (a material row borrows its maps, so the entry is what owns them), and the
    // identity is the node handle the content side names, so an edit re-uses the entry instead of creating a second one.
    std::unordered_map<vn::graphics::NodeId, LiveGeometry>     geometries;
    std::unordered_map<vn::graphics::ProgramId, LiveProgram>   programs;
    std::unordered_map<vn::graphics::MaterialId, LiveMaterial> materials;

    /// Material identities whose entry is gone and whose arena slot is nobody's to keep any more (see dropReleased):
    /// the sweep hands them to the block storage.
    std::vector<vn::graphics::MaterialId> material_slots_pending;

    /// Program identity -> the name its description saw (see ProgramNameFacts): the tables answer it separately
    /// from `program` because a program whose text cannot be described still has a name, and that is exactly the
    /// case whose sentence needs one. One entry per identity: a name does not move with the revision.
    std::vector<ProgramNameFacts> program_names;

    // The tables (see Table: rows, their storage, their order and their epoch together).
    GeometryTable                                                     geometry;
    Table<ProgramFacts, NoStorage, &orderProgramRows, &programRowKey> program;
    MaterialTable                                                     material;

    ContentFacts  facts{};
    std::uint64_t builds{ 0 };

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
        tables.program_names  = program_names;
        tables.program_order  = program.order();
        tables.geometry_order = geometry.order();
        tables.material_order = material.order();
        // The declaration indexes travel WITH the rows (see ContentDeclarations): a residency table asks them
        // instead of walking the rows, so its per-frame cost does not follow the size of the content.
        tables.declarations   = ContentDeclarations{ &geometry.declaredBuffers(), &material.declaredTextures() };
        return tables;
    }

    /** @brief Records the name a description saw (see ProgramNameFacts): one entry per identity, updated in place. */
    void recordProgramName(vn::graphics::ProgramId identity, const vn::String& name)
    {
        for (ProgramNameFacts& entry : program_names) {
            if (entry.program == identity) {
                entry.name = name.as_std_str();
                return;
            }
        }
        program_names.push_back(ProgramNameFacts{ identity, name.as_std_str() });
    }

    /** @brief The row-reclamation pass: every row that is no longer its entry's live answer and old enough leaves. */
    void reclaim(std::uint64_t oldest_live)
    {
        geometry.reclaimBefore(
            oldest_live,
            [this](const GeometryFacts& row) { return isCurrent(row); },
            [this](const GeometryFacts& row) { noteGeometryRowGone(row); });
        program.reclaimBefore(
            oldest_live,
            [this](const ProgramFacts& row) { return isCurrent(row); },
            [this](const ProgramFacts& row) { noteProgramRowGone(row); });
        material.reclaimBefore(
            oldest_live,
            [this](const MaterialFacts& row) { return isCurrent(row); },
            [this](const MaterialFacts& row) { noteMaterialRowGone(row); });
        dropReleased();
    }

    /// @brief Says whether @p row is its entry's LIVE answer (and therefore outlives the epoch window; see reclaim).
    [[nodiscard]] bool isCurrent(const GeometryFacts& row) const noexcept
    {
        const auto entry = geometries.find(row.geometry.as<vn::graphics::NodeTag>());
        return entry != geometries.end() && !entry->second.released && entry->second.described_revision == row.geometry.revision;
    }

    /// @brief Says whether @p row is its program's live answer (any variant/kind of the live revision counts).
    [[nodiscard]] bool isCurrent(const ProgramFacts& row) const noexcept
    {
        const auto entry = programs.find(row.program.as<vn::graphics::ProgramTag>());
        return entry != programs.end() && !entry->second.released && entry->second.described_revision == row.program.revision;
    }

    /// @brief Says whether @p row is its material's live answer.
    [[nodiscard]] bool isCurrent(const MaterialFacts& row) const noexcept
    {
        const auto entry = materials.find(row.material.as<vn::graphics::MaterialTag>());
        return entry != materials.end() && !entry->second.released && entry->second.described_revision == row.material.revision;
    }

    /** @brief Forgets the revision a reclaimed geometry row was built at, so the identity is rebuilt when named again. */
    void noteGeometryRowGone(const GeometryFacts& row) noexcept
    {
        const auto entry = geometries.find(row.geometry.as<vn::graphics::NodeTag>());
        if (entry == geometries.end() || entry->second.described_revision != row.geometry.revision) {
            return;
        }
        entry->second.described = false;
        entry->second.built     = false;
    }

    /** @brief Forgets the variant (or the screen entry) a reclaimed program row was built as. */
    void noteProgramRowGone(const ProgramFacts& row) noexcept
    {
        const auto entry = programs.find(row.program.as<vn::graphics::ProgramTag>());
        if (entry == programs.end() || entry->second.described_revision != row.program.revision) {
            return;
        }
        if (row.kind == core::DrawKind::Screen) {
            entry->second.screen_built = false;
        }
        else {
            std::erase(entry->second.variants, row.variant.bits());
        }
        if (!entry->second.screen_built && entry->second.variants.empty()) {
            entry->second.described = false; // nothing of that revision is left: a later ensure describes it again
        }
    }

    /** @brief Forgets the revision a reclaimed material row was built at (see noteGeometryRowGone). */
    void noteMaterialRowGone(const MaterialFacts& row) noexcept
    {
        const auto entry = materials.find(row.material.as<vn::graphics::MaterialTag>());
        if (entry == materials.end() || entry->second.described_revision != row.material.revision) {
            return;
        }
        entry->second.described = false;
        entry->second.built     = false;
    }

    /** @brief Drops the entries the host is done with whose rows are all gone (the epoch rule's other half). */
    void dropReleased()
    {
        for (auto it = geometries.begin(); it != geometries.end();) {
            if (it->second.released && geometry.rowsOf(it->first.handle().bits()) == 0U) {
                it = geometries.erase(it);
            }
            else {
                ++it;
            }
        }
        for (auto it = programs.begin(); it != programs.end();) {
            if (it->second.released && program.rowsOf(it->first.handle().bits()) == 0U) {
                const vn::graphics::ProgramId identity = it->first;
                std::erase_if(program_names, [identity](const ProgramNameFacts& name) { return name.program == identity; });
                it = programs.erase(it);
            }
            else {
                ++it;
            }
        }
        for (auto it = materials.begin(); it != materials.end();) {
            if (it->second.released && material.rowsOf(it->first.handle().bits()) == 0U) {
                // The ARENA SLOT follows the row: the block a recorded plan still reads is the row's, so the id leaves
                // for the block storage only once that row is gone (see releaseMaterial).
                material_slots_pending.push_back(it->first);
                it = materials.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    /** @brief Gets the row that answers for a TRACKED geometry right now (the one built at its live revision).
     *
     * It asks through the SAME lookup the recording asks with (`findGeometry`), so what this walk believes and
     * what the recording will find cannot drift apart - and the question is a bisection of the row order
     * rather than a walk of the table (the walk asks it once per command, see api/ContentFacts).
     *
     * `built` is the membership test: an entry whose row aged out (or whose description was refused) has no row to
     * answer with, and asking the table would walk it for an answer this walk already knows.
     */
    [[nodiscard]] const GeometryFacts* liveGeometry(vn::graphics::NodeId identity) const noexcept
    {
        if (identity.isNull()) {
            return nullptr;
        }
        const auto entry = geometries.find(identity);
        if (entry == geometries.end() || !entry->second.built) {
            return nullptr;
        }
        return findGeometry(view(), vn::graphics::contentRefOf(identity, entry->second.described_revision)).entry;
    }

    /** @brief Gets the row that answers for a material right now (the one built at its live revision). */
    [[nodiscard]] const MaterialFacts* liveMaterial(vn::graphics::MaterialId identity) const noexcept
    {
        const auto entry = materials.find(identity);
        if (entry == materials.end() || !entry->second.built) {
            return nullptr;
        }
        return findMaterial(view(), vn::graphics::ContentRef{ identity.handle(), entry->second.described_revision }).entry;
    }
};

ContentStore::ContentStore()
  : d(std::make_unique<Data>())
{}

ContentStore::~ContentStore() = default;

void ContentStore::track(vn::graphics::NodeId identity, const vn::graphics::GeometryDescriptionPtr& description)
{
    if (identity.isNull() || description == nullptr) {
        return;
    }
    const auto [entry, inserted] = d->geometries.try_emplace(identity);
    (void)inserted;
    Data::LiveGeometry& live = entry->second;
    live.released            = false; // still in use: a release the host asked for is called off
    // A NEW SHAPE AT THE SAME IDENTITY. The rows the tables answer for the old revision stay answerable until no
    // live plan has named them for a window (see tablesFor's reclamation pass), so nothing is dropped here - the value
    // is replaced and the revision compare does the rest.
    live.shape               = description;
}

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::Geometry>& geometry)
{
    if (geometry == nullptr) {
        return;
    }
    // ONE CALL, TWO FACTS: the object is what a row borrows its buffers from (and what "nobody else holds it" asks
    // about), and its current shape is what a row is built from. This is the content side's door; a frame with no
    // object names the two separately (see the overload above).
    const auto [entry, inserted] = d->geometries.try_emplace(geometry->id());
    (void)inserted;
    Data::LiveGeometry& live = entry->second;
    live.released            = false; // still in use: a release the host asked for is called off
    live.object              = geometry;
    live.shape               = geometry->description();
}

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::ShaderProgram>& program)
{
    if (program == nullptr) {
        return;
    }
    const auto [entry, inserted] = d->programs.try_emplace(program->id());
    if (inserted) {
        entry->second.object = program;
    }
    else {
        entry->second.released = false; // still in use: a release the host asked for is called off
    }
}

void ContentStore::track(const vn::intrusive_ptr<vn::graphics::MaterialBase>& material)
{
    if (material == nullptr) {
        return;
    }
    const auto [entry, inserted] = d->materials.try_emplace(material->id());
    if (inserted) {
        entry->second.object = material;
    }
    else {
        entry->second.released = false; // still in use: a release the host asked for is called off
    }
}

void ContentStore::track(const vn::graphics::MaterialBase* identity, const vn::graphics::MaterialDescriptionPtr& values)
{
    if (identity == nullptr || values == nullptr) {
        return;
    }
    const auto entry = d->materials.find(identity->id());
    if (entry == d->materials.end()) {
        // Untracked: nothing draws with it, so nothing can be asked about it.
        // Unlike a geometry value (which a mirrored row may name alone), a
        // material always arrives with the share that tracked it - so an entry
        // is not created here.
        return;
    }
    Data::LiveMaterial& live = entry->second;
    live.released            = false; // still in use: a release the host asked for is called off
    // A NEW VALUE STARTS A NEW EPISODE for the slot reports: the values changed, so what is wrong with them may
    // have too (see claimSlotReport) - and the report is asked BEFORE the next frame describes the value, which is
    // why it is re-armed here and not in ensureMaterial.
    if (live.described && live.described_revision != values->revision()) {
        live.slots_reported = 0U;
    }
    // A NEW VALUE AT THE SAME IDENTITY. Replacing it is enough: `ensureMaterial` compares the value's revision with the
    // one its row was built at and builds what the tables do not answer yet, so a steady frame compares an integer and
    // a material edit is one new value at the next publish (see the class note).
    live.values = values;
}

bool ContentStore::claimSlotReport(vn::graphics::MaterialId identity, std::size_t slot) noexcept
{
    const auto entry = d->materials.find(identity);
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

const ContentFacts& ContentStore::tablesFor(const core::CompiledFrame& frame, const core::FrameTimeline& timeline)
{
    // THE FRAME'S EPOCH, and the window's other end. A row named by this frame's plan is stamped with the frame the
    // plan is being recorded in; a row no plan has named since `completedFrame()` leaves now - the frame whose
    // command buffer could still replay that plan has been recycled (see FrameTimeline). No time window is guessed:
    // before the session has learned its slot count nothing completes, so nothing is reclaimed either.
    const std::uint64_t epoch       = timeline.hasOpenFrame() ? timeline.current().frame : timeline.submittedFrame() + 1U;
    const std::uint64_t oldest_live = timeline.completedFrame() + 1U;
    d->reclaim(oldest_live);

    for (const core::CompiledPass& pass : frame.passes) {
        for (const core::CompiledDraw& draw : pass.draws) {
            if (draw.kind == core::DrawKind::Screen) {
                // The full-screen ABI is the engine's and carries no variant: a screen program is one text
                // (see api/ContentSources).
                ensureProgram(draw.program.as<vn::graphics::ProgramTag>(), ProgramVariant{}, true, epoch);
                continue;
            }

            for (const core::CompiledCommand& command : draw.commands) {
                const vn::graphics::NodeId     identity = command.geometry.as<vn::graphics::NodeTag>();
                const vn::graphics::MaterialId material = command.material.as<vn::graphics::MaterialTag>();

                ensureGeometry(identity, epoch);
                ensureMaterial(command.material, epoch);

                if (command.program.isNull()) {
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
                ensureProgram(command.program.as<vn::graphics::ProgramTag>(), variant, false, epoch);
            }
        }
    }

    d->facts.programs      = d->program.rows();
    d->facts.geometries    = d->geometry.rows();
    d->facts.materials     = d->material.rows();
    d->facts.program_names = d->program_names;

    // The tables are published WITH THEIR ROW ORDER, which is what turns the recording's lookups into
    // bisections instead of whole-table scans (see api/ContentFacts). Nothing is built here: each table owns
    // its order and refreshes it when a row moves, so this is the copy of six spans that the frame reads.
    d->facts.program_order  = d->program.order();
    d->facts.geometry_order = d->geometry.order();
    d->facts.material_order = d->material.order();
    d->facts.declarations   = ContentDeclarations{ &d->geometry.declaredBuffers(), &d->material.declaredTextures() };
    return d->facts;
}

void ContentStore::ensureGeometry(vn::graphics::NodeId identity, std::uint64_t epoch)
{
    if (identity.isNull()) {
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
    live.released            = false; // named by a frame: a release the host asked for is called off
    if (live.shape == nullptr) {
        return;
    }
    const std::uint64_t            revision = live.shape->revision();
    const vn::graphics::ContentRef ref      = vn::graphics::contentRefOf(identity, revision);
    if (!(live.described && live.described_revision == revision)) {
        // A NEW REVISION IS A NEW ATTEMPT: what the next frames ask for is decided by its outcome, and the old
        // revision's row stays answerable until no live plan names it (tablesFor's reclamation pass).
        live.described          = true;
        live.described_revision = revision;
        live.built              = false;
    }
    if (live.built) {
        const FactResult<GeometryFacts> row = findGeometry(d->view(), ref);
        if (row.found()) {
            d->geometry.stamp(row.entry, epoch);
            return; // the steady frame: one bisection, no build
        }
        live.built = false; // the row aged out under us; the description is built again below
    }

    auto           storage = std::make_unique<Data::GeometryStorage>();
    GeometryFacts  fresh;
    const FactMiss miss = buildGeometryFacts(*live.shape, fresh, storage->channels, storage->primitives);
    if (miss != FactMiss::None) {
        // The ATTEMPT, not the success: a shape that failed is not retried until the revision moves.
        return;
    }

    // The row is keyed by the REF the plan names, not by the description it was built from: the description is a
    // value, the ref (identity + revision) is what a command - and therefore a lookup - carries.
    fresh.geometry = ref;
    d->geometry.append(fresh, std::move(storage), epoch);
    live.built = true;
    ++d->builds;
}

void ContentStore::ensureMaterial(vn::graphics::ContentRef material, std::uint64_t epoch)
{
    const vn::graphics::MaterialId identity = material.as<vn::graphics::MaterialTag>();
    auto                           entry    = d->materials.find(identity);
    if (entry == d->materials.end()) {
        if (!identity.isNull()) {
            return; // untracked (see ensureGeometry)
        }
        // The default material is an entry like any other (see api/ContentSources) and needs no tracking:
        // content without a material can be described at any time.
        entry = d->materials.try_emplace(vn::graphics::MaterialId{}).first;
    }

    Data::LiveMaterial& live = entry->second;
    live.released            = false; // named by a frame: a release the host asked for is called off

    // THE VALUES A ROW IS BUILT FROM: the ones the frame published. A material no frame published (a command a host
    // built by hand) is answered from the object's own cache - the same door every hand-built command uses, and the
    // reason that command is the host's to keep still.
    vn::graphics::MaterialDescriptionPtr values = live.values;
    if (values == nullptr && live.object != nullptr) {
        values = live.object->description();
    }
    const std::uint64_t            revision = values != nullptr ? values->revision() : 0;
    const vn::graphics::ContentRef ref{ identity.handle(), revision };
    if (!(live.described && live.described_revision == revision)) {
        // A NEW VALUE IS A NEW ATTEMPT (see ensureGeometry), and it starts a fresh episode for the slot reports
        // (see claimSlotReport).
        live.described          = true;
        live.described_revision = revision;
        live.built              = false;
        live.slots_reported     = 0U;
    }
    if (live.built) {
        const FactResult<MaterialFacts> row = findMaterial(d->view(), ref);
        if (row.found()) {
            d->material.stamp(row.entry, epoch);
            return; // the steady frame: one bisection, no build
        }
        live.built = false;
    }

    auto           storage = std::make_unique<Data::MaterialStorage>();
    MaterialFacts  fresh;
    const FactMiss miss = buildMaterialFacts(ref, values, fresh, storage->block);
    if (miss != FactMiss::None) {
        return; // the ATTEMPT, not the success (see ensureGeometry)
    }

    d->material.append(fresh, std::move(storage), epoch);
    live.built = true;
    ++d->builds;
}

void ContentStore::ensureProgram(vn::graphics::ProgramId program, const ProgramVariant& variant, bool screen, std::uint64_t epoch)
{
    if (program.isNull()) {
        return;
    }
    const auto entry = d->programs.find(program);
    if (entry == d->programs.end()) {
        return; // untracked (see ensureGeometry)
    }

    Data::LiveProgram& live                     = entry->second;
    live.released                               = false; // named by a frame: a release the host asked for is called off
    // THE TRACKED OBJECT IS WHAT THE ROW IS DESCRIBED FROM: the plan names the identity, and the values are the
    // object's current ones - a build never resurrects a revision (see the class note), so the plan's revision
    // is deliberately not used here.
    const vn::graphics::ShaderProgram& source   = *live.object;
    const std::uint64_t                revision = source.revision();
    const vn::graphics::ContentRef     ref{ program.handle(), revision };
    if (!(live.described && live.described_revision == revision)) {
        // Another revision is another set of texts: none of the old answers is this one's (its rows age out by epoch).
        live.described          = true;
        live.described_revision = revision;
        live.variants.clear();
        live.screen_built = false;
        // The NAME travels with the description, not with the row: a program whose text is refused still has a
        // name, and the sentence that says so has no row to read it from (see ProgramNameFacts).
        d->recordProgramName(program, source.name());
    }

    const auto built = [&live, &variant, screen]() {
        return screen ? live.screen_built : std::find(live.variants.begin(), live.variants.end(), variant.bits()) != live.variants.end();
    };
    if (built()) {
        const FactResult<ProgramFacts> row =
            findProgram(d->view(), ref, screen ? ProgramVariant{} : variant, screen ? core::DrawKind::Screen : core::DrawKind::Content);
        if (row.found()) {
            d->program.stamp(row.entry, epoch);
            return; // this text of this revision is already in the tables
        }
        // The row aged out under us: forget the bit and build it again (see ensureGeometry).
        if (screen) {
            live.screen_built = false;
        }
        else {
            std::erase(live.variants, variant.bits());
        }
    }

    ProgramFacts   fresh;
    const FactMiss miss = screen ? buildScreenProgramFacts(source, fresh) : buildProgramFacts(source, variant, fresh);
    if (miss != FactMiss::None) {
        return; // a program that is not one content pipeline: no entry, and the recorder says so
    }

    if (screen) {
        live.screen_built = true;
    }
    else {
        live.variants.push_back(variant.bits());
    }
    d->program.append(fresh, {}, epoch);
    ++d->builds;
}

std::size_t ContentStore::releaseAbandoned(std::vector<vn::graphics::MaterialId>& released_materials)
{
    released_materials.clear();
    std::size_t released = 0U;

    // AN ABANDONED ENTRY IS ONLY MARKED HERE. Its rows may still answer for a recorded plan, and the entry is what
    // keeps a material's maps (and a geometry's shape) alive for them - so the entry leaves in dropReleased() once its
    // last row does, and its arena slot with it.
    for (auto& [identity, live] : d->geometries) {
        if (live.released) {
            continue; // already the host's word
        }
        // WHAT "NOBODY ELSE HOLDS IT" MEANS: the object (when the identity named one) is the content's; a shape with no
        // object came from a value producer, whose owner is the one that keeps it alive. Either way, a share besides
        // the store's own means the thing is still in use.
        const unsigned long held_outside = live.object != nullptr ? live.object->useCount() : (live.shape != nullptr ? live.shape->useCount() : 0UL);
        if (held_outside > 1UL) {
            continue;
        }
        live.released = true;
        ++released;
    }

    for (auto& [identity, live] : d->programs) {
        (void)identity;
        if (live.released || (live.object != nullptr && live.object->useCount() > 1UL)) {
            continue;
        }
        live.released = true;
        ++released;
    }

    for (auto& [identity, live] : d->materials) {
        if (identity.isNull() || live.released) {
            continue; // the default material is not an object anyone can abandon
        }
        if (live.object != nullptr && live.object->useCount() > 1UL) {
            continue;
        }
        live.released = true;
        ++released;
    }

    d->dropReleased();

    // THE ARENA SLOTS come out here: an id whose entry (and therefore every row) is gone is nobody's to keep. The
    // identity is a KEY only (the object may already be destroyed - see the declaration).
    released_materials.swap(d->material_slots_pending);
    d->material_slots_pending.clear();
    return released;
}

bool ContentStore::releaseGeometry(vn::graphics::NodeId identity)
{
    if (identity.isNull()) {
        return false;
    }
    const auto entry = d->geometries.find(identity);
    if (entry == d->geometries.end()) {
        return true; // not resident: the release is already true of this identity
    }
    entry->second.released = true;
    d->dropReleased(); // an identity whose rows are already gone leaves now; otherwise at the next frame boundary
    return true;
}

bool ContentStore::releaseGeometry(vn::raw_ptr<const vn::graphics::Geometry> geometry)
{
    if (geometry == nullptr) {
        return false;
    }
    return releaseGeometry(geometry->id());
}

bool ContentStore::releaseProgram(vn::raw_ptr<const vn::graphics::ShaderProgram> program)
{
    if (program == nullptr) {
        return false;
    }
    const auto entry = d->programs.find(program->id());
    if (entry != d->programs.end()) {
        entry->second.released = true;
        d->dropReleased();
    }
    return true;
}

bool ContentStore::releaseMaterial(vn::raw_ptr<const vn::graphics::MaterialBase> material)
{
    if (material == nullptr) {
        return false;
    }
    const auto entry = d->materials.find(material->id());
    if (entry != d->materials.end()) {
        entry->second.released = true;
        d->dropReleased();
    }
    return true;
}

void ContentStore::clear()
{
    d->geometries.clear();
    d->programs.clear();
    d->materials.clear();
    d->material_slots_pending.clear();
    d->program_names.clear();
    d->geometry.clear();
    d->program.clear();
    d->material.clear();
    d->facts = ContentFacts{};
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
    return (all != 0U && d->materials.find(vn::graphics::MaterialId{}) != d->materials.end()) ? all - 1U : all;
}

std::uint64_t ContentStore::builds() const noexcept
{
    return d->builds;
}

VN_VSG_NS_END
