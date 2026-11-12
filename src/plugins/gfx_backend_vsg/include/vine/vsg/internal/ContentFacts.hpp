#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Sampler.hpp>
#include <vine/graphics/ShaderAbi.hpp>
#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/Keys.hpp>
#include <vine/graphics/backend/Streams.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/vsg/internal/ContentPipeline.hpp>
#include <vine/vsg/internal/FactResult.hpp>
#include <vine/vsg/internal/ProgramAbi.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief What the content layer is GIVEN: the three tables a compiled command cannot carry, and the rules
 * that make a table entry usable.
 *
 * WHY TABLES AND NOT MORE PLAN. The plan names a program, a geometry and a material by identity - that is
 * all the frame can afford to copy per command, and all it should: the GPU objects and their source data
 * belong to the layer that owns them, are shared between frames, and must never be re-decided per frame.
 * What the plan CANNOT say is "this identity is these vertex streams" or "these material bytes", because
 * that is content, not frame intent. So the content layer answers those questions from its own tables, keyed
 * by the same identities the plan carries.
 *
 * REVISION IS PART OF THE KEY, and that is what makes a stale answer impossible rather than unlikely: a
 * program edited after the plan was compiled, a geometry whose bytes moved, a material that was rewritten -
 * each is a DIFFERENT entry, so the lookup misses instead of silently drawing the old thing. A miss is then
 * a report, not a fallback: the content layer says which identity and which revision it could not answer,
 * and the draw is skipped (see FactMiss for the three ways to miss).
 *
 * THE TABLES ARE BORROWED FOR THE FRAME. Their entries - and the arrays the channel's data points at - must
 * outlive the recording; a provider that rebuilds a geometry must keep the old revision alive for as long as
 * a plan that names it may still be recorded (the same rule as every other borrowed argument in this
 * backend, and the same reason StreamUploads keys its binds by revision).
 *
 * WHY A LOOKUP IS NOT A SCAN. Each lookup runs several times per drawing call per frame (`api/ContentHalves`
 * and `api/ContentPass` both resolve the same three identities), and the tables only GROW: a linear scan is
 * therefore O(rows) per lookup, i.e. O(drawables x rows) per frame - invisible at the content sizes this
 * backend was written for (tens of rows) and a wall at a thousand (measured 2026-09-24: see the design log's
 * §11.16bz, which is also where the index's own cost is stated). So a table may carry a ROW ORDER - the same
 * rows, sorted by the identity the lookup names - and a lookup that has one narrows to the run of rows that
 * share that identity by bisection instead of reading all of them. The order is DERIVED and is therefore
 * never the table's source of truth: the rows are, and a table without an order still answers correctly (by
 * scanning). What it buys is a lookup whose cost follows the LOGARITHM of the table rather than the table.
 */
namespace vn::graphics
{

class Texture;

}

VN_VSG_NS_BEGIN

/** @brief Layout -> how many live geometry rows feed it (the map @ref ContentDeclarations borrows). */
using LayoutDeclarations = std::unordered_map<core::VertexLayoutKey, std::uint32_t, core::VertexLayoutKeyHash>;

/**
 * @brief The declaration indexes one table publishes (see @ref ContentDeclarations).
 *
 * @details
 * - ONE NAMED FIELD PER KIND, and that is the whole reason this type exists: the buffer and texture indexes have
 *   the same C++ type (`address -> row count`), so an argument list let a caller swap them and compile - a table
 *   whose buffers were asked as textures answers "not declared" for every entry, which retires GPU copies that
 *   content still names. A designated initializer names the half it fills, so the mistake is not spellable.
 * - A NULL FIELD IS "THIS TABLE PUBLISHES NO SUCH INDEX", which is what a hand-built table (a test, an alternative
 *   provider) has. Consumers state what that means for them (see @ref ContentDeclarations::hasLayoutIndex).
 * - BORROWED, NEVER OWNED: every pointer names one of the publishing table's own maps and stays valid until that
 *   table's rows move, the same lifetime the spans beside `ContentFacts` have.
 */
struct ContentDeclarationIndexes {
    const std::unordered_map<const void*, std::uint32_t>*   buffers{ nullptr };   // Geometry rows: buffers their streams read.
    const std::unordered_map<const void*, std::uint32_t>*   textures{ nullptr };  // Material rows: maps their slots sample.
    const std::unordered_map<std::uint64_t, std::uint32_t>* materials{ nullptr }; // Material rows: the identities they answer for.
    const LayoutDeclarations*                               layouts{ nullptr };   // Geometry rows: the vertex layouts they feed.
};

/**
 * @brief What the live content still DECLARES, as an index a residency table can ask in O(1).
 *
 * @details
 * - WHY AN INDEX AND NOT A WALK. A residency table asks one question per entry - "does anything alive still
 *   name these bytes / this layout?" - and the answer is a property of the content rows. Answering it by walking
 *   the rows would make a frame's residency bookkeeping cost O(live content), which is the wrong side of the pin
 *   model: culled content KEEPS its rows, so the cost would be paid on frames that draw nothing. The content
 *   store therefore keeps these indexes beside the rows (each table maintains its own as rows move) and publishes
 *   them here; asking is one hash lookup.
 * - FOUR KINDS, ONE QUESTION. A geometry row declares the BUFFERS its streams read and the vertex LAYOUT it feeds;
 *   a material row declares the TEXTURES its slots sample and its own IDENTITY (what the block arena holds a slot
 *   under). The first three are maps keyed by address or by the layout value, the last by a handle - but one type
 *   answers all four instead of four lookalikes, and every residency table that keys on any of them
 *   (api/StreamUploads, api/MaterialImages, the material arena, api/ContentHalves) asks the same way.
 * - WHAT IT MEANS, AND WHAT IT DOES NOT. Membership says an address is still declared by some live row, at the
 *   moment that row was built. It says NOTHING about whether the bytes at that address are still the ones a
 *   residency entry was built from: `Buffer::edit()` / a texture refill move the object's revision without
 *   touching any row. That half is answered by the entry itself, which holds the handle it uploaded from and
 *   compares its revision (see `ResourceTable::retireUnpinned`): declaration membership + own-revision equality
 *   - two O(1) facts.
 * - BORROWED, LIKE THE ROWS. The object points into the store's indexes and stays valid until the next row
 *   mutation - the same lifetime the spans beside it have.
 */
class ContentDeclarations {
  public:
    /**
     * @brief Points at the indexes @p indexes names.
     *
     * @param indexes The table's own indexes (borrowed, never owned); an omitted kind answers "not declared".
     */
    explicit ContentDeclarations(ContentDeclarationIndexes indexes = {}) noexcept
      : indexes_(indexes)
    {}

    /**
     * @brief Gets whether a live geometry row still feeds @p layout.
     *
     * @details
     * - THE PIN OF A COMPILED HALF, together with `program@rev` (see api/ContentHalves): a half is kept while
     *   the program revision answers AND some live geometry still feeds the layout it was compiled for - so a
     *   layout whose last geometry left the tree retires its halves without any per-frame walk of the rows.
     *
     * @param layout The vertex layout (canonical mask + custom locations).
     * @return true when some live geometry row still feeds it.
     */
    [[nodiscard]] bool declaresLayout(const core::VertexLayoutKey& layout) const noexcept
    {
        return indexes_.layouts != nullptr && indexes_.layouts->find(layout) != indexes_.layouts->end();
    }

    /**
     * @brief Gets whether a layout declaration index was published at all.
     *
     * @details
     * - The index is OPTIONAL in the same sense the row orders are: a table built by hand (tests, an alternative
     *   provider) may leave it empty, and a consumer that needs the question then answers it by scanning the
     *   geometry rows - the same answer at a linear price. Production's `ContentStore` always publishes it, so
     *   the frame path never takes that fallback.
     *
     * @return true when @ref declaresLayout can answer from an index.
     */
    [[nodiscard]] bool hasLayoutIndex() const noexcept
    {
        return indexes_.layouts != nullptr;
    }

    /**
     * @brief Gets whether a live geometry row still declares @p address.
     *
     * @param address Buffer address (the `StreamKey::buffer` spelling); null answers false.
     * @return true when some live geometry row still names that buffer.
     */
    [[nodiscard]] bool declaresBuffer(const void* address) const noexcept
    {
        return address != nullptr && indexes_.buffers != nullptr && indexes_.buffers->find(address) != indexes_.buffers->end();
    }

    /**
     * @brief Gets whether a live material row still samples the texture at @p address.
     *
     * @param address Texture address (the image cache's key); null answers false.
     * @return true when some live material row still names that texture.
     */
    [[nodiscard]] bool declaresTexture(const void* address) const noexcept
    {
        return address != nullptr && indexes_.textures != nullptr && indexes_.textures->find(address) != indexes_.textures->end();
    }

    /**
     * @brief Gets whether a live material row still names the identity @p material.
     *
     * @details
     * - THE PIN OF A MATERIAL BLOCK. The arena keeps one slot per identity, so "is this block still wanted?" is
     *   "does any row of that identity answer?" - and a superseded revision that a recorded plan may still name
     *   keeps the slot exactly as long as it answers, because the block's bytes ARE that row's.
     * - COUNTED BY THE TABLE ITSELF: the material table already keeps one row count per identity (it is what
     *   decides when an entry may leave), so this borrows that count instead of copying it - one fact, one home.
     *
     * @param material Material identity (the key the arena holds a slot under). The null id asks about the default
     *                 material, which is an entry like any other.
     * @return true when some live material row still names that identity.
     */
    [[nodiscard]] bool declaresMaterial(vn::graphics::MaterialId material) const noexcept
    {
        return indexes_.materials != nullptr && indexes_.materials->find(material.handle().bits()) != indexes_.materials->end();
    }

  private:
    ContentDeclarationIndexes indexes_{}; // The publishing table's own indexes (see the type's note).
};

/** @brief One vertex channel of a geometry: what the stream is shared by, and where its bytes live.
 *
 * @details
 * - THE ENTRY BORROWS THE SOURCE, IT DOES NOT COPY IT. It keeps the buffer handle and the slice, so the
 *   bytes are read when a stream is actually UPLOADED (a sharing hit reads nothing) and are always the ones
 *   the host wrote - a table built before an edit cannot hand out the old bytes.
 * - THE KEY'S REVISIONS ARE PER FRAME, not facts of the entry: `keyFor()` fills them from the two live
 *   announcements - the geometry's (batch: every channel re-keys) and the buffer's own (fine: only the
 *   streams reading it re-key).
 */
struct ChannelFacts {
    core::StreamKey                            key{};  // Kind, location, components, buffer, slice (and the
                                                       // revisions it had when the entry was built).
    vn::intrusive_ptr<const vn::Buffer<float>> values; // The source scalars; read only when uploading.

    /** @brief Gets the stream's identity as the frame that is being recorded sees it.
     *
     * @details
     * - The revision is the BUFFER's, read now: an edit the host announced on the buffer re-keys this
     *   stream, and an edit nobody announced leaves it keyed exactly as it was (see StreamKey).
     *
     * @return The key, with the buffer's revision read live.
     */
    [[nodiscard]] core::StreamKey streamKey() const noexcept
    {
        core::StreamKey out = key;
        out.revision        = values != nullptr ? values->revision() : 0U;
        return out;
    }

    /** @brief Gets this channel's scalars as they are right now (the slice's own range).
     * @return A view over the source buffer; empty when the channel holds nothing.
     */
    [[nodiscard]] std::span<const float> scalars() const noexcept
    {
        if (values == nullptr) {
            return {};
        }
        const std::span<const float> all   = values->view();
        const std::size_t            begin = std::min<std::size_t>(key.offset, all.size());
        return all.subspan(begin, std::min<std::size_t>(key.count, all.size() - begin));
    }
};

/** @brief The index stream of a geometry: the same shape as one vertex channel, over indices. */
struct IndexFacts {
    core::StreamKey                                    key{};  // Kind Index, the whole buffer, no slice.
    vn::intrusive_ptr<const vn::Buffer<std::uint32_t>> values; // The source indices; read only when uploading.

    /** @brief Gets the stream's identity as the frame that is being recorded sees it (see ChannelFacts::streamKey).
     * @return The key, with the buffer's revision read live.
     */
    [[nodiscard]] core::StreamKey streamKey() const noexcept
    {
        core::StreamKey out = key;
        out.revision        = values != nullptr ? values->revision() : 0U;
        return out;
    }

    /** @brief Gets the whole index buffer as it is right now (the bind aliases all of it; the draw states the span).
     * @return A view over the index buffer; empty when it holds nothing.
     */
    [[nodiscard]] std::span<const std::uint32_t> scalars() const noexcept
    {
        return values != nullptr ? values->view() : std::span<const std::uint32_t>{};
    }
};

/** @brief ONE of a geometry's draws: how it assembles, and the index span it reads.
 *
 * @details
 * - A geometry is a set of vertex channels plus N primitives (see the SDK's Primitive.hpp), and a
 *   primitive IS a draw: the plan names one by its index (CompiledCommand::primitive), so this row is
 *   what a command's `(geometry, primitive)` pair resolves to.
 * - The index stream is optional HERE rather than on the geometry: an unindexed primitive assembles from
 *   the vertex streams themselves and is a different COMMAND (`vkCmdDraw`) than an indexed one
 *   (`vkCmdDrawIndexed`), which is why the absence has to be a fact of the DRAW.
 * - The stream is normalized to its WHOLE buffer (the bind aliases all of it, the draw states the span),
 *   so primitives slicing one arena share a single upload.
 */
struct PrimitiveFacts {
    vn::graphics::Topology    topology{ vn::graphics::Topology::Triangles }; // How it assembles.
    std::optional<IndexFacts> indices{};                                     // The index stream when this draw is indexed.
    std::uint32_t             index_count{ 0 };                              // Indices it reads (indexed only).
    std::uint32_t             first_index{ 0 };                              // First index of its span (indexed only).

    /// The instance stream this draw reads (16 scalars per instance, column major), or null for a plain draw.
    ///
    /// It is CONTENT, like the index buffer: the buffer is the primitive's own, and only the VISIBLE PREFIX is a
    /// draw parameter (it travels in the plan). Holding the handle here is what lets a stream's pin and its upload
    /// key come from the primitive that declares its bytes - so a culled instanced draw keeps its instance upload
    /// exactly as a culled plain draw keeps its vertex streams.
    vn::intrusive_ptr<const vn::Buffer<float>> instances{};
};

/** @brief What a geometry identity answers with: its layout, its vertex channels, and its draws. */
struct GeometryFacts {
    /// The identity AND the revision the plan names (see `ContentId.hpp`).
    ///
    /// THE REVISION IS PART OF THE KEY, the same rule the program and material tables follow: a frame in flight
    /// that named revision 3 must be served revision 3's streams after the geometry moved to 4, so a table keyed
    /// by the identity alone would answer the new shape to the old frame.
    ///
    /// THE IDENTITY IS THE GEOMETRY'S OWN NODE ID (`Geometry::id()`, inherited from `Node`), not an address and
    /// not a separate geometry handle: a geometry IS a node (see ADR-2026-11-11-content-identity-is-a-handle),
    /// so `ref.as<NodeTag>()` is the typed view and no `GeometryId` allocator exists.
    vn::graphics::ContentRef      geometry{};
    core::VertexLayoutKey         layout{};   // Which channels this geometry feeds (the pipeline key's half).
    std::span<const ChannelFacts> channels{}; // One per fed channel, in binding order.

    /// The draws, in the geometry's own order: `CompiledCommand::primitive` indexes this row. Empty is a
    /// fact ("this geometry draws nothing"), never a default - a geometry with data always has one.
    std::span<const PrimitiveFacts> primitives{};

    /// Vertices a NON-indexed draw reads: the position stream's own vertex count (the SDK's definition of
    /// `Geometry::vertexCount()`), because positions are the one required attribute and the only stream the
    /// count can be derived from.
    std::uint32_t vertex_count{ 0 };
    std::uint32_t vertex_offset{ 0 }; // Added to every index before fetching (indexed only).
};

/** @brief What a program identity answers with: the two stages a content pipeline compiles, and the
 *         bindings those stages declare (see api/ProgramAbi.hpp).
 *
 * The ABI is part of the entry because a pipeline cannot be built without it: what the text declares is
 * what the layout must be, and the layer that compiles the pair is the one that needs both halves of the
 * answer together (see api/ContentPipeline). It is scanned for the variant the layer compiles with - today
 * the source as written, so the two can never disagree about which declarations are in effect. */
struct ProgramFacts {
    /// The identity AND the revision the plan names (see `ContentId.hpp`).
    ///
    /// THE REVISION IS PART OF THE KEY, the same rule the geometry and material tables follow: a frame in flight
    /// that named revision 3 must be served revision 3's texts after the program moved to 4, so a table keyed by
    /// the identity alone would answer the new text to the old frame.
    ///
    /// THE IDENTITY IS THE PROGRAM'S OWN HANDLE (`ShaderProgram::id()`), not an address. An address is reused:
    /// two different programs can be described at one address at two different times, at the SAME revision (an
    /// engine preset is built from the same stage count every time it is created), and a compiled half keyed on
    /// an address and a revision would be served for the other program - the previous mode's pipeline, drawn for
    /// the mode just asked for. A handle moves its generation on release, so that class of collision cannot
    /// happen, and no per-description counter has to stand in for it.
    vn::graphics::ContentRef program{};
    ProgramVariant           variant{}; // The variant (which of the program's texts) this entry is.

    /// Which of the engine's two drawing CALLS this entry describes. It is part of the entry's key and not a
    /// decoration: ONE program object can be described twice at one revision - as content (both stages, the
    /// content ABI) and as a full-screen call (the engine brings the vertex stage, so only the fragment stage
    /// is read, and the ABI is the screen one) - and the two entries carry different texts, different
    /// declarations and different pipelines. A lookup that did not name the kind would hand whichever row the
    /// table happened to carry first to a caller expecting the other, which is a wrong picture with no refusal
    /// anywhere: the ambiguity this field closes (measured reachable - a host that also draws its post-process
    /// program as content is the ordinary way to hit it).
    core::DrawKind kind{ core::DrawKind::Content };

    ContentPipeline::Shaders shaders{}; // The GLSL those two stages consist of.
    ProgramAbi               abi{};     // The bindings and push ranges those two texts declare.
};

/** @brief One program identity's NAME: what a sentence about a miss can call it.
 *
 * @details
 * - A NAME IS NOT IDENTITY (see `ShaderProgram::name()`): it never moves a revision, may be shared, and may be
 *   empty. It is here because the one sentence a host gets when its program cannot be turned into a pipeline has
 *   to say WHICH program - and a plan carries only the handle, while a program whose text cannot be described
 *   has no `ProgramFacts` row to read a name from.
 * - THE VALUE IS THE ONE THE DESCRIPTION SAW. A rename does not move the revision, so a table that was never
 *   asked to describe the program again may still answer the previous name; that is a label, not a fact about
 *   the picture, and it is bounded by the next description.
 */
struct ProgramNameFacts {
    vn::graphics::ProgramId program{}; // The identity.
    std::string             name{};    // `ShaderProgram::name()`; empty when the program was never named.
};

/** @brief One material texture slot as the CONTENT layer sees it: the map and how it is read. */
struct MaterialSlotFacts {
    vn::raw_ptr<const vn::graphics::Texture> texture{}; // The map; null = the slot is absent.
    vn::graphics::Sampler                    sampler{}; // Read state, by value (the image cache keys on it).
};

/** @brief What a material identity answers with: the block bytes the shading reads, and its texture set. */
struct MaterialFacts {
    /// The identity AND the revision the plan names (a null id = the default material).
    ///
    /// THE REVISION IS PART OF THE KEY, the same rule the geometry table follows: a frame in flight that named revision
    /// 3 must be served revision 3's bytes after the material moved to 4, so a table keyed by the identity alone would
    /// answer the new values to the old frame.
    vn::graphics::ContentRef   material{};
    std::span<const std::byte> block{}; // The block; its size must be the ABI's.

    /// The five texture slots, in `TextureSlot` order. A null map means the slot is absent - which is what the
    /// block's flag bits say as well (this list is what the CONTENT layer resolves images from, the flags are
    /// what the shader reads). It is a fact the content layer needs: any present slot is what puts the drawable
    /// in the program's `VN_TEXTURED` variant (see api/ProgramVariant). Each pointer is borrowed with the
    /// entry - the material owns it.
    std::array<MaterialSlotFacts, vn::graphics::kTextureSlotCount> slots{};
};

/** @brief Every table a frame's content needs, borrowed for the recording. */
struct ContentFacts {
    std::span<const ProgramFacts>  programs{};   // Program identities → their GLSL.
    std::span<const GeometryFacts> geometries{}; // Geometry identities → their streams and layout.
    std::span<const MaterialFacts> materials{};  // Material identities → their block bytes.

    /// Program identities → their names (see ProgramNameFacts): what a sentence about a missing program calls it.
    ///
    /// SEPARATE FROM `programs` ON PURPOSE. That table answers "what a pipeline is compiled from", so a program
    /// whose text cannot be described has no row - and that is exactly the case whose sentence needs a name. One
    /// entry per identity (a name does not move with the revision), found by a linear scan on a failure path.
    std::span<const ProgramNameFacts> program_names{};

    /// WHAT THOSE ROWS DECLARE, as an O(1) index (see ContentDeclarations). A residency table asks this per
    /// entry instead of walking the rows - so a frame's residency bookkeeping costs O(entries), not O(content),
    /// and a frame that draws nothing (every geometry culled) pays the same as a busy one.
    ContentDeclarations declarations{};

    /// The ROW ORDER of each table, as the lookups search it: a permutation of that table's row indices,
    /// sorted by the identity the lookup names (see @ref orderProgramRows and the file note).
    ///
    /// EMPTY IS ALLOWED AND MEANS "NOT INDEXED": every lookup then answers by scanning its whole table, which
    /// is the same answer at a linear price - the path a table built by hand takes (tests do that), and the
    /// reason the two spellings can live side by side without one of them being wrong. An order that does not
    /// cover exactly the table's rows is treated the same way (a SCAN), so a table that moved without its
    /// order being rebuilt still answers with its own rows rather than with an index it does not hold; the
    /// builders below are the only supported way to make one, and `api/ContentStore`'s own tables keep the
    /// order current through every mutation.
    std::span<const std::uint32_t> program_order{};
    std::span<const std::uint32_t> geometry_order{};
    std::span<const std::uint32_t> material_order{};
};

/** @brief Replaces @p order with the row order @ref findProgram searches the program table by.
 *
 * The order is (identity, revision, KIND) - the three fields the lookup can compare in constant time. The
 * VARIANT is deliberately not part of it: comparing two of them is comparing their define lists, which is
 * work the search would then pay per comparison. The rows that share those three are therefore contiguous
 * and IN TABLE ORDER inside the order (the sort is stable), and the lookup scans that short run for the
 * variant - which is also what keeps "the first entry of that identity and revision" meaning the same thing
 * it means today.
 *
 * @param rows  The table's rows.
 * @param order Receives the order; its previous contents are replaced and its capacity is kept.
 */
void orderProgramRows(std::span<const ProgramFacts> rows, std::vector<std::uint32_t>& order);

/** @brief Replaces @p order with the row order @ref findGeometry searches the geometry table by.
 *
 * The order is the ref's (identity, revision) pair: rows that share both are contiguous and in table order, so
 * the geometry the scan would have answered with is the first row of that run.
 *
 * @param rows  The table's rows.
 * @param order Receives the order; its previous contents are replaced and its capacity is kept.
 */
void orderGeometryRows(std::span<const GeometryFacts> rows, std::vector<std::uint32_t>& order);

/** @brief Replaces @p order with the row order @ref findMaterial searches the material table by.
 *
 * The order is the identity alone (see findMaterial: the material lookup has no revision), and rows that
 * name the same identity keep their table order - the material table answers "the material now", so which
 * of two rows of one identity is the answer is decided by row order, not by the sort.
 *
 * @param rows  The table's rows.
 * @param order Receives the order; its previous contents are replaced and its capacity is kept.
 */
void orderMaterialRows(std::span<const MaterialFacts> rows, std::vector<std::uint32_t>& order);

/** @brief Gets whether a geometry's channels are exactly what its layout declares, and how it is drawn.
 *
 * The two must agree or the picture is wrong in a way no counter sees: a layout that declares an attribute
 * nothing feeds is a pipeline the driver refuses (or, worse, one that reads unbound memory), and a channel
 * with no declaration is data nothing consumes. The rule counts BOTH halves - the canonical mask and the
 * custom locations - against the streams, so a geometry whose channels are merely reordered is fine (binding
 * order is the channel order) while one that is short a channel is not.
 *
 * It also checks the STREAMS THE DRAW IS ASSEMBLED FROM: an indexed geometry needs its index stream, and a
 * non-indexed one needs vertices to assemble (its count comes from the position stream). A geometry that
 * offers neither is described but not drawable, which is a distinction the report has to keep.
 *
 * @param facts Geometry entry to check.
 * @return true when the entry's channel count is what its layout declares and its streams can be drawn.
 */
[[nodiscard]] bool channelsMatchLayout(const GeometryFacts& facts) noexcept;

/** @brief Gets whether a material entry carries a block of the ABI's size.
 *
 * @param facts Material entry to check.
 * @return true when the block is exactly `sizeof(vn::graphics::VineMaterialBlock)` bytes.
 */
[[nodiscard]] bool blockFitsAbi(const MaterialFacts& facts) noexcept;

/** @brief Finds the program a compiled command names.
 *
 * THE VARIANT IS PART OF THE KEY. A program's text is several texts (see api/ProgramVariant): its stages
 * gate declarations and code on define names, so one (identity, revision) may be answered with several
 * entries that differ in what their `abi` declares. The lookup asks for the one the drawable means, because
 * an entry of another variant would hand the layer bindings and push ranges the module it compiled does not
 * have - and the picture, or the validation layer, would be the only thing to notice.
 *
 * A TABLE MAY CARRY SUPERSEDED REVISIONS. A production table keeps a replaced revision answerable until the
 * frames that may still name it are past (see api/ContentStore), so the scan looks for the entry asked for
 * instead of stopping at the first entry of that identity: reporting Revision on the first mismatch would
 * call a revision missing that is right there.
 *
 * @param facts    The tables.
 * @param program  Program identity and revision (from the plan).
 * @param variant  Which of the program's texts the drawable means.
 * @param kind     Which drawing CALL the entry must describe (see ProgramFacts::kind): the same program may
 *                 be answered for both, and the two answers are different entries.
 * @return The entry, or the reason there is none.
 */
[[nodiscard]] FactResult<ProgramFacts>
findProgram(const ContentFacts& facts, const vn::graphics::ContentRef& program, const ProgramVariant& variant, core::DrawKind kind) noexcept;

/** @brief Gets the name a program identity was described with (see ProgramNameFacts).
 *
 * @details
 * - Used by diagnostics only, and only where the tables cannot name the program otherwise: a program with no
 *   `ProgramFacts` row (its text was refused) still has a name here, because the name is read when the store
 *   starts describing the revision and not when the pipeline is built.
 * - A linear scan: it runs on a reporting path, never per draw.
 *
 * @param facts   The tables.
 * @param program Program identity.
 * @return The name, or an empty view when the identity was never described.
 */
[[nodiscard]] VN_VSG_API std::string_view programName(const ContentFacts& facts, vn::graphics::ProgramId program) noexcept;

/** @brief Finds the geometry a compiled command names.
 *
 * THE REFERENCE IS THE WHOLE KEY (see GeometryFacts::geometry): the plan carries the identity and the revision it
 * was collected at, so a frame in flight is answered with the shape it recorded.
 *
 * A TABLE MAY CARRY SUPERSEDED REVISIONS (see findProgram): the scan looks for the revision asked for
 * instead of stopping at the first entry of that identity.
 *
 * @param facts    The tables.
 * @param geometry Geometry identity and revision (from the plan).
 * @return The entry, or the reason there is none (Malformed when its channels do not match its layout).
 */
[[nodiscard]] FactResult<GeometryFacts> findGeometry(const ContentFacts& facts, vn::graphics::ContentRef geometry) noexcept;

/** @brief Finds the material a compiled command names.
 *
 * IDENTITY IS THE WHOLE KEY, unlike the other two tables: the plan cannot name a material revision, because
 * the SDK's `Material` has no revision accessor (the layer that tracks edits does - see api/ContentSources.hpp).
 * The entry is therefore the table's own account of those bytes, taken in the same frame the plan was collected
 * in, so "the answer" IS "the material now". The entry keeps its revision because the block storage keys its
 * in-flight copies by it.
 *
 * A null identity is looked up like any other: content without a material matches a table's DEFAULT entry
 * (built for nullptr), and a table without one answers Unknown.
 *
 * @param facts    The tables.
 * @param material Material identity (from the plan), or nullptr for "no material".
 * @return The entry, or the reason there is none (Malformed when its block size is not the ABI's).
 */
[[nodiscard]] FactResult<MaterialFacts> findMaterial(const ContentFacts& facts, vn::graphics::ContentRef material) noexcept;

VN_VSG_NS_END
