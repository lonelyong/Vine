#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <vine/graphics/backend/FrameCompiler.hpp>
#include <vine/graphics/backend/Keys.hpp>
#include <vine/vsg/internal/ContentFacts.hpp>
#include <vine/vsg/internal/ContentImages.hpp>
#include <vine/vsg/internal/FactResult.hpp>
#include <vine/vsg/internal/ProgramVariant.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief What one drawing call NEEDS: the identity a pipeline is compiled for, answered by exactly one builder.
 *
 * WHY IT EXISTS. Two layers need the same answer and must never disagree about it: the layer that BUILDS what a
 * frame needs ahead of time (api/ContentHalves, and the `prepare()` entry that will drive it) and the layer that
 * RECORDS the frame (api/ContentPass). Before this type both spelled the tuple themselves - the geometry's
 * layout, the variant both facts imply, the program row for that variant, the counts the pipeline key carries -
 * and a frame that compiled one identity while the answer promised another is a pipeline compiled for a picture
 * nobody draws (a wasted compile at best, the wrong text at worst). `drawNeeds` is the single spelling of that
 * tuple, so "what the warm-up answers" and "what the frame builds" are the same value by construction (`G1`).
 *
 * WHAT IS IN IT, AND WHAT IS DELIBERATELY NOT. In: everything a pipeline is IDENTITY by (see core::PipelineKey) -
 * the drawing kind, the program's identity and revision, the vertex layout, the variant, the topology, the
 * pass' attachment shape, and the counts of the sampled textures the pipeline's set carries - plus the table
 * rows the answer was read from, so a consumer does not look them up a second time. Not in: anything delivered
 * with a set command or written into a buffer (cull, blend, depth policy, matrices, material bytes) - those are
 * dynamic state or data, and `keyFor` is the only way this answer becomes an identity.
 *
 * THE TWO COUNT PAIRS, AND WHY THERE ARE TWO. `set_colors` / `set_depths` are what the pass' SAMPLED-INPUT SET
 * binds (they include a program that reads the set, and one that does not: the set is a property of the pass).
 * `key_colors` / `key_depths` are what the PIPELINE KEY states - zero for a program that samples nothing in that
 * set, because the pipeline is compiled against the layout named there (see `programSamplesInputSet`). Two
 * numbers, one rule, and the rule lives here rather than in each caller.
 *
 * THE MISS ORDER IS PART OF THE ANSWER. A drawable is answered geometry, then material, then program - the order
 * the recorder reports refusals in - so `missed` names the FIRST identity that could not be answered and `miss`
 * says why. A consumer reads both instead of re-deriving which lookup failed.
 *
 * NO DEVICE IS NEEDED: the answer is read from borrowed tables and the plan, and `keyFor` copies a value. A
 * `DrawNeeds` that is not `answerable()` still carries what WAS answered (the geometry row when the program
 * misses, for instance), which is what lets the refusal name the identity it could not serve.
 */
VN_VSG_NS_BEGIN

/** @brief One drawing call's needs: the identity it is compiled for, and the table rows that answered it. */
struct DrawNeeds {
    /** @brief Which identity the tables could not answer (the FIRST one, in the order the answer is built). */
    enum class Miss : std::uint8_t
    {
        None,     // The answer is complete: every row was found and the variant agrees with the program's entry.
        Geometry, // The command's geometry row misses; nothing after it was asked.
        Material, // The command's material row misses; the variant and the program were not asked.
        Program,  // No program row for the drawable's VARIANT (the text is not described, or at another revision).
    };

    /** @brief `source_index` when a full-screen call reads none of the pass' inputs. */
    static constexpr std::size_t kNoSource = static_cast<std::size_t>(-1);

    core::DrawKind           kind{ core::DrawKind::Content };               // Which of the two drawing calls this is.
    vn::graphics::ContentRef program{};                                     // Identity AND revision the key names.
    core::VertexLayoutKey    layout{};                                      // The vertex layout the pipeline is built against
                                                                            // (empty for a full-screen call).
    ProgramVariant           variant{};                                     // The spelled variant (the key carries its bits).
    vn::graphics::Topology   topology{ vn::graphics::Topology::Triangles }; // The class the pipeline bakes.
    std::uint32_t            color_attachments{ 0 };                        // The pass' colour count (the blend state's shape).
    bool                     depth_sampleable{ false };                     // The pass' depth being sampleable (content only).
    bool                     reads_inputs{ false };                         // The program samples the pass' input set.
    bool                     instanced{ false };                            // The drawable reads an instance stream.
    std::uint32_t            set_colors{ 0 };                               // Colour textures the pass' input set binds.
    std::uint32_t            set_depths{ 0 };                               // Depth textures the pass' input set binds.
    std::uint32_t            key_colors{ 0 };                               // Colour textures the PIPELINE KEY states.
    std::uint32_t            key_depths{ 0 };                               // Depth textures the PIPELINE KEY states.
    std::size_t              source_index{ kNoSource };                     // Full-screen: which pass input the call reads.
    const ProgramFacts*      program_entry{ nullptr };                      // The row the program was answered from; null on a miss.
    const GeometryFacts*     geometry_entry{ nullptr };                     // The geometry row; null for a full-screen call or a miss.
    const MaterialFacts*     material_entry{ nullptr };                     // The material row; null for a full-screen call or a miss.
    Miss                     missed{ Miss::None };                          // Which lookup failed first.
    FactMiss                 miss{ FactMiss::None };                        // Why it failed (only meaningful for `missed`).

    /** @brief Gets whether every identity was answered (and the variant's program row exists).
     * @return true when the answer is complete.
     */
    [[nodiscard]] bool answerable() const noexcept
    {
        return missed == Miss::None;
    }

    /** @brief Gets whether a full-screen call reads one of the pass' declared inputs.
     * @return true when @ref source_index names an entry rather than @ref kNoSource.
     */
    [[nodiscard]] bool hasSource() const noexcept
    {
        return source_index != kNoSource;
    }

    /** @brief Gets the pipeline identity these needs spell for a target of shape @p compatibility.
     *
     * @details
     * - The target's shape is the CALLER's fact, not the plan's (the plan names a target, this layer never sees
     *   it), which is why it is the one input the answer does not carry: two passes over one identity that draw
     *   into differently shaped targets are two pipelines, and the key says so without either pass knowing the
     *   other exists.
     * - It is the ONLY place this answer becomes an identity: a consumer that needs a pipeline calls this, and a
     *   consumer that only needs to know what will be built reads the fields.
     *
     * @param compatibility The target's shape (see core::RenderPassCompatibility).
     * @return The identity a pool variant and a pipeline are keyed by.
     */
    [[nodiscard]] core::PipelineKey keyFor(const core::RenderPassCompatibility& compatibility) const;
};

/** @brief Gets the number of colour textures the pass' inputs offer (the sampled-input set's colour half).
 *
 * @details
 * - The plan states it per input (`CompiledInput::color_attachments`, which a ProducedImage narrows to the one
 *   image it names), so the pass' total is a sum of plan facts rather than a property of the images a caller
 *   happened to offer.
 *
 * @param pass The compiled pass.
 * @return The colour textures every declared input offers, added up.
 */
[[nodiscard]] std::uint32_t sampledColorCount(const core::CompiledPass& pass) noexcept;

/** @brief Gets the number of depth textures the pass' inputs offer (the sampled-input set's depth half).
 *
 * @details
 * - One per input whose plan says its depth is sampleable, which is the same fact the input set's layout is
 *   built from - so "the shader declares a depth sampler" and "the set has a depth binding" cannot disagree.
 *
 * @param pass The compiled pass.
 * @return The depth textures the pass' inputs offer.
 */
[[nodiscard]] std::uint32_t sampledDepthCount(const core::CompiledPass& pass) noexcept;

/** @brief Answers what ONE content drawable needs: the plan's command, read from the three tables.
 *
 * @details
 * - The lookups are the recorder's own, in the recorder's order (geometry, material, then the program entry for
 *   the variant those two imply), so the answer is the tuple the recording path would have derived - now derived
 *   once.
 * - The variant is the ENGINE's rule (`variantOf`), with `instanced` from whether the command names an instance
 *   stream; the program entry is looked up BY that variant, because one program text is several texts.
 *
 * @param pass    The compiled pass the command belongs to (its attachment shape and input counts).
 * @param command The drawable the plan names.
 * @param facts   The tables the identities are answered from.
 * @return The needs, complete or with the first miss named.
 */
[[nodiscard]] DrawNeeds drawNeeds(const core::CompiledPass& pass, const core::CompiledCommand& command, const ContentFacts& facts);

/** @brief Answers what ONE full-screen drawing call needs: the plan's call, read from what the caller offers.
 *
 * @details
 * - A full-screen call's identity carries no variant and no vertex layout, and its counts are the SOURCE's: the
 *   full-screen ABI reads binding N as the source's attachment N (see InputImages::screen_colors), so the count
 *   a caller's offer produces is the count the pipeline is compiled for. That is why the offer is an input here
 *   while the content answer takes it from the plan.
 * - No table is consulted: which program KIND a full-screen call uses is a property of the call, and the
 *   compiled half is found by the caller (the scope). An offer of no inputs answers a call that reads no source,
 *   which is what a producer that only needs the half's shape (`api/ContentHalves`) hands in - the half's key
 *   carries no counts.
 *
 * @param pass   The compiled pass the call belongs to (its colour attachment count).
 * @param draw   The full-screen call.
 * @param inputs The images the caller offers, one entry per `pass.inputs` entry and in the same order.
 * @return The needs (always answerable: no table is asked).
 */
[[nodiscard]] DrawNeeds drawNeeds(const core::CompiledPass& pass, const core::CompiledDraw& draw, std::span<const InputImages> inputs);

VN_VSG_NS_END
