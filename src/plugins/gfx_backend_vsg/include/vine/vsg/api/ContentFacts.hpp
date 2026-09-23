#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/FactResult.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/api/ProgramVariant.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/Keys.hpp>
#include <vine/vsg/core/Streams.hpp>
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
 */
namespace vine::graphics
{
class Texture;
}

V_VSG_NS_BEGIN

/** @brief One vertex channel of a geometry: what the stream is shared by, and the bytes to upload. */
struct ChannelFacts
{
    core::StreamKey             key{};   ///< Identity of the stream (kind, location, components, buffer, revision, slice).
    ::vsg::ref_ptr<::vsg::Data> data;    ///< The source array; borrowed for as long as the entry is.
};

/** @brief What a geometry identity answers with: its layout, its vertex channels, and how it is assembled. */
struct GeometryFacts
{
    const void*    geometry{nullptr};   ///< The identity the plan names.
    std::uint64_t  revision{0};         ///< The revision the plan names.
    core::VertexLayoutKey layout{};     ///< Which channels this geometry feeds (the pipeline key's half).
    std::span<const ChannelFacts> channels{};  ///< One per fed channel, in binding order.

    /// The index stream, when the geometry draws INDEXED. Its absence is a fact, not a default: an
    /// unindexed draw assembles its primitives from the vertex streams themselves and is a different
    /// COMMAND (`vkCmdDraw`) than an indexed one (`vkCmdDrawIndexed`) - the same way
    /// `core::GeometryStreams` treats an absent index stream, and the reason the two modes cannot be
    /// told apart by the vertex layout alone.
    std::optional<ChannelFacts> indices{};
    std::uint32_t  index_count{0};      ///< Indices of this geometry (indexed only).
    std::uint32_t  first_index{0};      ///< First index of the span the geometry states (indexed only).
    /// Vertices a NON-indexed draw reads: the position stream's own vertex count (the SDK's definition of
    /// `Geometry::vertexCount()`), because positions are the one required attribute and the only stream the
    /// count can be derived from.
    std::uint32_t  vertex_count{0};
    std::uint32_t  vertex_offset{0};    ///< Added to every index before fetching (indexed only).
};

/** @brief What a program identity answers with: the two stages a content pipeline compiles, and the
 *         bindings those stages declare (see api/ProgramAbi.hpp).
 *
 * The ABI is part of the entry because a pipeline cannot be built without it: what the text declares is
 * what the layout must be, and the layer that compiles the pair is the one that needs both halves of the
 * answer together (see api/ContentPipeline). It is scanned for the variant the layer compiles with - today
 * the source as written, so the two can never disagree about which declarations are in effect. */
struct ProgramFacts
{
    const void*              program{nullptr};  ///< The identity the plan names.
    std::uint64_t            revision{0};       ///< The revision the plan names.
    ProgramVariant           variant{};         ///< The variant (which of the program's texts) this entry is.
    ContentPipeline::Shaders shaders{};         ///< The GLSL those two stages consist of.
    ProgramAbi               abi{};             ///< The bindings and push ranges those two texts declare.
};

/** @brief What a material identity answers with: the block bytes the shading reads. */
struct MaterialFacts
{
    const void*                material{nullptr};  ///< The identity (nullptr = the default material).
    std::uint64_t              revision{0};        ///< The revision its owner tracks its bytes at.
    std::span<const std::byte> block{};            ///< The block; its size must be the ABI's.

    /// The texture the material samples, or null when it has none. The BLOCK does not carry it (a map is
    /// an image, not a texel of material state), and it is a fact the content layer needs: it is what
    /// decides whether a program's `VINE_DIFFUSE_MAP` variant applies to this drawable (see
    /// api/ProgramVariant). The pointer is borrowed with the entry - the material owns it.
    vine::raw_ptr<const vine::graphics::Texture> texture{};
};

/** @brief Every table a frame's content needs, borrowed for the recording. */
struct ContentFacts
{
    std::span<const ProgramFacts>  programs{};    ///< Program identities → their GLSL.
    std::span<const GeometryFacts> geometries{};  ///< Geometry identities → their streams and layout.
    std::span<const MaterialFacts> materials{};   ///< Material identities → their block bytes.
};

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
 * @return true when the block is exactly `sizeof(vine::graphics::VineMaterialBlock)` bytes.
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
 * @return The entry, or the reason there is none.
 */
[[nodiscard]] FactResult<ProgramFacts> findProgram(const ContentFacts& facts, const core::ProgramRef& program,
                                                   const ProgramVariant& variant) noexcept;

/** @brief Finds the geometry a compiled command names.
 *
 * A TABLE MAY CARRY SUPERSEDED REVISIONS (see findProgram): the scan looks for the revision asked for
 * instead of stopping at the first entry of that identity.
 *
 * @param facts    The tables.
 * @param geometry Geometry identity (from the plan).
 * @param revision Geometry revision the plan was collected at.
 * @return The entry, or the reason there is none (Malformed when its channels do not match its layout).
 */
[[nodiscard]] FactResult<GeometryFacts> findGeometry(const ContentFacts& facts, const void* geometry,
                                                     std::uint64_t revision) noexcept;

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
[[nodiscard]] FactResult<MaterialFacts> findMaterial(const ContentFacts& facts, const void* material) noexcept;

V_VSG_NS_END
