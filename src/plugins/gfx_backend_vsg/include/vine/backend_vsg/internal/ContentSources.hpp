#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/backend_vsg/internal/ContentFacts.hpp>
#include <vine/backend_vsg/internal/ProgramVariant.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

/**
 * @brief Building the PROGRAM and MATERIAL entries of the content tables, from the SDK objects the host
 * authored (the second and third source; the geometry's walk is `api/GeometryFacts.hpp`).
 *
 * WHY THESE TWO ARE RULES RATHER THAN COPIES.
 *
 *   * A CONTENT PIPELINE IS EXACTLY TWO STAGES with ONE entry point (`ContentPipeline::Shaders`): a program
 *     that names a compute stage, two fragment stages, or different entry points per stage cannot be turned
 *     into one. Picking "the first one of each kind" would compile something the host did not write, and the
 *     picture would differ from its intent with no counter to show it - so those programs are reported as
 *     undescribable instead.
 *   * EVERY COMMAND HAS A MATERIAL, INCLUDING WHEN IT HAS NONE. The engine's material is optional, and the
 *     ABI's own defaults ARE the default material (grey, no shininess), which is what the existing
 *     implementation hands to a drawable that names no material. So "no material" is an ENTRY with a null
 *     identity rather than a lookup miss: a table that carries one answers for such content, and a table that
 *     does not says Unknown - the content layer then decides whether that is a refusal or a fallback, instead
 *     of a lookup deciding for it.
 *
 * The material's REVISION comes from the caller, not from the SDK object: `Material` has no revision accessor,
 * so the layer that tracks material edits (the material manager) is the one that knows. That is the same rule
 * the geometry follows - a revision is a fact the plan names, and only its owner can report it.
 *
 * THE ENTRY ALSO CARRIES THE BINDINGS THE TEXT DECLARES (`ProgramFacts::abi`, see api/ProgramAbi.hpp): a
 * pipeline is built against THE TEXT's own `layout(set = ..., binding = ...)` qualifiers, so "what this program
 * is" is not just its two stages but also where its blocks and samplers live. The scan runs on the same two
 * texts the entry hands the layer, for the same variant the layer compiles them with - today, the sources as
 * they are written - so the layout the layer builds and the module it compiles cannot disagree about which
 * declarations are in effect.
 */
VN_BACKEND_VSG_NS_BEGIN

/** @brief Builds the program table entry for @p program.
 *
 * @param program SDK program text to describe (borrowed; the entry copies its sources).
 * @param out     Receives the entry.
 * @return None when the entry was built; Unknown when the program has no stages; Malformed when it cannot be
 *         one content pipeline (not exactly one vertex and one fragment stage, an empty source, an entry
 *         point the two stages disagree about, or a text whose declared bindings cannot be read - see
 *         api/ProgramAbi).
 */
[[nodiscard]] FactMiss buildProgramFacts(const vn::graphics::ShaderProgramDescription& program, ProgramFacts& out);

/**
 * @brief The convenience spelling over a live @p program: converts with `ShaderProgram::description()` and delegates.
 *
 * @note For a hand-built caller or a test that still holds the object. The FRAME path never takes this door: the
 *       store describes the value the frame carried (see ContentStore::ensureProgram), so recording never reads a
 *       program's text on the thread that records it.
 *
 * @param program SDK program to describe (borrowed; the entry copies its sources).
 * @param out     Receives the entry.
 * @return None / Unknown / Malformed exactly as the value overload says.
 */
[[nodiscard]] FactMiss buildProgramFacts(const vn::graphics::ShaderProgram& program, ProgramFacts& out);

/**
 * @brief Builds the program table entry for ONE variant of a content program.
 *
 * A program's text is several programs (see api/ProgramVariant): its stages gate declarations and code on
 * `#pragma import_defines` names, so the ABI a pipeline is built against depends on which of those names
 * the variant defines. This overload is the one spelling of that pairing - the scan and the compile read
 * the SAME list (`out.shaders.defines`), so a layout and a module can never disagree about what is in
 * effect, which is the failure a text with an `#error` in the wrong branch would otherwise report as a
 * compile error at draw time (or not at all).
 *
 * @param program  The program text whose stages are described.
 * @param variant  The variant to describe it for.
 * @param out      Receives the entry (its `shaders.defines` are the variant's).
 * @return None when the entry was built; Unknown/Malformed exactly as the untagged overload says.
 */
[[nodiscard]] FactMiss buildProgramFacts(const vn::graphics::ShaderProgramDescription& program, const ProgramVariant& variant, ProgramFacts& out);

/** @brief The convenience spelling over a live @p program (see the untagged overload). */
[[nodiscard]] FactMiss buildProgramFacts(const vn::graphics::ShaderProgram& program, const ProgramVariant& variant, ProgramFacts& out);

/** @brief Builds the program table entry for a FULL-SCREEN program (see core::DrawKind::Screen).
 *
 * The full-screen ABI is the engine's, not the host's: the fragment stage is the program's own, and the
 * VERTEX stage is the engine's canonical full-screen triangle (`BuiltinShaders::fullscreenVertexProgram` -
 * three vertices generated from `gl_VertexIndex`, `vn_uv` at location 0, no vertex buffer and no
 * vertex-side constants). A vertex stage the program happens to carry is IGNORED, because that is what the
 * contract says (`DrawDescription` with `Kind::Screen`: the draw's `program` is its FRAGMENT stage - the
 * backend provides the fullscreen vertex stage): honouring one would compile a triangle the engine's fragment
 * stages are not written against.
 *
 * The entry's identity is the program and its revision, so the plan's `program` names it exactly as a content
 * program does.
 *
 * @param program SDK screen program to describe (borrowed; the entry copies the fragment source).
 * @param out     Receives the entry (both GLSL texts are the pair a full-screen pipeline compiles, and the
 *                declared bindings are scanned from that same pair - the engine's triangle declares none).
 * @return None when the entry was built; Unknown when the program has no stages at all; Malformed when it has
 *         no fragment stage (or several), a compute or a second fragment stage, an empty fragment source, a
 *         fragment entry point that is not the canonical vertex stage's (the pipeline carries one entry point
 *         for both stages), or a fragment text whose declared bindings cannot be read.
 */
[[nodiscard]] FactMiss buildScreenProgramFacts(const vn::graphics::ShaderProgramDescription& program, ProgramFacts& out);

/** @brief The convenience spelling over a live @p program (see the value overload). */
[[nodiscard]] FactMiss buildScreenProgramFacts(const vn::graphics::ShaderProgram& program, ProgramFacts& out);

/** @brief Builds the material table entry for @p material from the VALUES @p values, or for the DEFAULT material.
 *
 * @details
 * - It packs the block from the value, so an asset and an instance over one are both describable (the value is the
 *   resolved set - graphics-material.md 2.2) and, more importantly, describing a row never reads the live material:
 *   what the frame published is what the row says (see `RenderCommand::materialDescription`).
 *
 * @param material The identity AND revision the row answers for; a null id is the engine's default material.
 * @param values   The values to pack, or null for the default material.
 * @param out      Receives the entry (its block span points into @p storage).
 * @param storage  Caller-owned storage the entry borrows; resized to the block's size and must outlive it.
 * @return None always: an absent material is a describable thing (see the file note), not a miss.
 */
[[nodiscard]] FactMiss buildMaterialFacts(vn::graphics::ContentRef                        material,
                                          const vn::graphics::MaterialDescriptionPtr& values,
                                          MaterialFacts&                               out,
                                          std::vector<std::byte>&                      storage);

VN_BACKEND_VSG_NS_END
