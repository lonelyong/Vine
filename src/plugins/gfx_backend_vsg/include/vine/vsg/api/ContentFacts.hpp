#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

#include <vine/graphics/ShaderAbi.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
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
V_VSG_NS_BEGIN

/** @brief One vertex channel of a geometry: what the stream is shared by, and the bytes to upload. */
struct ChannelFacts
{
    core::StreamKey             key{};   ///< Identity of the stream (kind, location, components, buffer, revision, slice).
    ::vsg::ref_ptr<::vsg::Data> data;    ///< The source array; borrowed for as long as the entry is.
};

/** @brief What a geometry identity answers with: its layout, its vertex channels and its index stream. */
struct GeometryFacts
{
    const void*    geometry{nullptr};   ///< The identity the plan names.
    std::uint64_t  revision{0};         ///< The revision the plan names.
    core::VertexLayoutKey layout{};     ///< Which channels this geometry feeds (the pipeline key's half).
    std::span<const ChannelFacts> channels{};  ///< One per fed channel, in binding order.
    ChannelFacts   indices{};           ///< The index stream (draws are indexed).
    std::uint32_t  index_count{0};      ///< Indices of this geometry.
    std::uint32_t  first_index{0};      ///< First index of the span the geometry states.
    std::uint32_t  vertex_offset{0};    ///< Added to every index before fetching.
};

/** @brief What a program identity answers with: the two stages a content pipeline compiles. */
struct ProgramFacts
{
    const void*              program{nullptr};  ///< The identity the plan names.
    std::uint64_t            revision{0};       ///< The revision the plan names.
    ContentPipeline::Shaders shaders{};         ///< The GLSL those two stages consist of.
};

/** @brief What a material identity answers with: the block bytes the shading reads. */
struct MaterialFacts
{
    const void*                material{nullptr};  ///< The identity (nullptr = the default material).
    std::uint64_t              revision{0};        ///< The revision its owner tracks its bytes at.
    std::span<const std::byte> block{};            ///< The block; its size must be the ABI's.
};

/** @brief Why a lookup did not answer with an entry the content layer can record. */
enum class FactMiss : std::uint8_t
{
    None,       ///< Found: the entry can be recorded as it is.
    Unknown,    ///< No entry has this identity: the content layer was never told about it.
    Revision,   ///< The identity is there, at a DIFFERENT revision: the plan describes content that has moved on.
    Malformed,  ///< The entry exists but cannot be drawn (see the rule each lookup applies).
};

/** @brief The answer of one lookup: the entry, and why it is missing when it is. */
template <typename Facts>
struct FactResult
{
    const Facts* entry{nullptr};          ///< The entry, or null (see @ref miss).
    FactMiss     miss{FactMiss::None};    ///< Why it is missing when it is (None when it is not).

    /** @brief Gets whether an entry was found. */
    [[nodiscard]] bool found() const noexcept { return entry != nullptr; }
};

/** @brief Every table a frame's content needs, borrowed for the recording. */
struct ContentFacts
{
    std::span<const ProgramFacts>  programs{};    ///< Program identities → their GLSL.
    std::span<const GeometryFacts> geometries{};  ///< Geometry identities → their streams and layout.
    std::span<const MaterialFacts> materials{};   ///< Material identities → their block bytes.
};

/** @brief Gets whether a geometry's channels are exactly what its layout declares.
 *
 * The two must agree or the picture is wrong in a way no counter sees: a layout that declares an attribute
 * nothing feeds is a pipeline the driver refuses (or, worse, one that reads unbound memory), and a channel
 * with no declaration is data nothing consumes. The rule counts BOTH halves - the canonical mask and the
 * custom locations - against the streams, so a geometry whose channels are merely reordered is fine (binding
 * order is the channel order) while one that is short a channel is not.
 *
 * @param facts Geometry entry to check.
 * @return true when the entry's channel count is what its layout declares.
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
 * @param facts    The tables.
 * @param program  Program identity and revision (from the plan).
 * @return The entry, or the reason there is none.
 */
[[nodiscard]] FactResult<ProgramFacts> findProgram(const ContentFacts& facts, const core::ProgramRef& program) noexcept;

/** @brief Finds the geometry a compiled command names.
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
