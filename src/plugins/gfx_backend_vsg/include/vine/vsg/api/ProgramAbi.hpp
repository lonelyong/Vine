#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vine/vsg/api/FactResult.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief What a program's TEXT declares: the bindings and the push ranges, per variant (see
 * `.ai/design/vsg-reimplementation.md` §11.16ae).
 *
 * WHY THE TEXT HAS TO BE READ AT ALL. A backend cannot choose where a shader's blocks live: the program's
 * source carries the `layout(set = ..., binding = ...)` qualifiers, and a pipeline whose layout disagrees
 * with them is not "a bit off" - the shader reads a binding nobody wrote, and the pipeline expects one the
 * shader never names. The engine ships its programs WITH the arrangement they were written against
 * (`BuiltinShaders::forwardProgram` declares the material at set 0 / binding 0, the optional diffuse map at
 * 1, the lights at 2, and the shadow map and its block at 3 and 4, with the per-drawable block in set 1),
 * while this backend's own programs may declare the L1 blocks wherever they like. Serving either one means
 * knowing what the text says, so the text is read - a SCAN, not a parser: the GLSL qualifier grammar it
 * understands is the small, fixed subset the engine's programs use, and what it cannot classify is reported
 * instead of guessed.
 *
 * FACTS HERE, POLICY LATER. This file answers what is declared: which (set, binding) pairs exist, of what
 * kind, which stages read them, which of the five L1 blocks each uniform block is (the GLSL block type name
 * IS the L1 name - see `.ai/design/graphics-shader.md` §11.3), how large each block is, and which push ranges
 * the stages declare. It does NOT decide which bytes or which image go to which binding: that is the
 * serving layer's policy, and the same facts are served differently by the content path and the full-screen
 * path. An unknown block type is therefore a FACT (`AbiBlockRole::Foreign`) rather than a rejection: the
 * text is fine, the backend just cannot fill it - and the layer that binds is where that becomes a refusal.
 *
 * A VARIANT, NOT A PROGRAM, IS WHAT HAS FACTS. The shipped programs gate declarations with `#ifdef`
 * (`VINE_DIFFUSE_MAP`, `VINE_TEXCOORD_CUBE`, ...), so "the bindings of this program" has no answer until the
 * defines in effect are known. The scan therefore takes the defines and answers for that variant, and it
 * evaluates them READ-ONLY: a `#define` the source carries itself (the flat forward program's `VINE_FLAT`)
 * is honoured, and a `#error` inside a taken branch makes the variant Malformed - the text itself says it
 * cannot compile, which is exactly what a scan can see and a driver would only say later.
 *
 * `import_defines` records the names the source ALLOWS to be defined (the SDK's `#pragma import_defines`).
 * It is part of the facts because the compiler drops a define the source never asks for, silently: a name
 * that is set but not imported changes nothing, and only a reader of the text can say so.
 *
 * WHAT THE SCAN UNDERSTANDS (and what makes it refuse): `#ifdef` / `#ifndef` / `#if defined(X)` (with an
 * optional `!`), `#elif` of the same forms, `#else`, `#endif`, `#define NAME ...`, `#undef NAME`, `#error`
 * and `#pragma import_defines (...)`. Any other conditional expression, an unparseable layout value, or a
 * declaration form it cannot classify is reported as Malformed - a text nobody can read is not a text whose
 * bindings may be guessed, because guessing here is guessing which memory a shader reads.
 */
VN_VSG_NS_BEGIN

/** @brief The stage a binding is read by (this backend's two graphics stages). */
enum class AbiStage : std::uint32_t
{
    Vertex   = 1U,  ///< The vertex stage.
    Fragment = 2U,  ///< The fragment stage.
};

/** @brief What a declared binding is. */
enum class AbiDescriptorKind : std::uint8_t
{
    UniformBlock,  ///< A `std140` uniform block (`VineMaterialBlock` and friends).
    Sampler2D,     ///< A `sampler2D`.
    SamplerCube,   ///< A `samplerCube`.
    OtherSampler,  ///< Any other sampler type (recorded by name; the serving layer decides).
};

/** @brief Which of the L1 blocks a uniform block is, by its GLSL TYPE NAME (the L1 name, §11.3). */
enum class AbiBlockRole : std::uint8_t
{
    NotABlock,     ///< A sampler (or a push block, which does not appear in `bindings`).
    View,          ///< `VineViewBlock` (the per-pass view block).
    Draw,          ///< `VineDrawBlock` (the per-draw model matrix and opacity).
    Material,      ///< `VineMaterialBlock` (the shading material).
    Lights,        ///< `VineLightsBlock` (one ambient plus up to three directional lights, in view space).
    ShadowBlock,   ///< `VineShadowBlock` (the map's placement and comparison scalars).
    Foreign,       ///< A uniform block whose type name is none of the L1 names: the backend cannot fill it.
};

/** @brief The layout qualifier a block declares (a role block must be `std140` for the bytes to be the ABI's). */
enum class AbiBlockLayout : std::uint8_t
{
    Unspecified,  ///< No layout qualifier: the layout is the compiler's, so the block's size is not known here.
    Std140,       ///< `layout(std140)`.
    Std430,       ///< `layout(std430)` (legal on the push-constant blocks; the scan sizes those with it).
};

/** @brief One binding a stage text declares. */
struct AbiBinding
{
    std::uint32_t    set{0};          ///< Descriptor set.
    std::uint32_t    binding{0};      ///< Binding index inside that set.
    std::uint32_t    count{1};        ///< Descriptors at that binding (an array suffix; 1 for a single one).
    AbiDescriptorKind kind{AbiDescriptorKind::UniformBlock};  ///< What is bound there.
    AbiBlockRole     role{AbiBlockRole::NotABlock};  ///< The L1 block it is (blocks only; see AbiBlockRole).
    std::uint32_t    stages{0};       ///< Union of the stages that declare it (AbiStage bits).
    AbiBlockLayout   layout{AbiBlockLayout::Unspecified};  ///< The block's declared layout qualifier.
    std::uint32_t    block_size{0};   ///< Its std140 size in bytes; 0 when the layout is Unspecified or the
                                      ///< members could not be sized (a sampler has no size either).
    std::string      type_name{};     ///< The declared type (`VineMaterialBlock`, `samplerCube`, ...).
    std::string      name{};          ///< The declared instance name (`material`, `diffuseMap`, ...).
};

/** @brief One member of a declared push block: what it is called, and where its bytes sit. */
struct AbiPushMember
{
    std::string   name{};    ///< The declared member name (`projection`, `modelView`, ...).
    std::uint32_t offset{0}; ///< Its byte offset inside the push range (from the range's start).
    std::uint32_t size{0};   ///< Its size in bytes (an array counts all its elements).
};

/** @brief One push-constant range a stage declares. */
struct AbiPushRange
{
    std::uint32_t offset{0};     ///< Byte offset inside the push range (from `layout(offset = ...)`, else 0).
    std::uint32_t size{0};       ///< Its size in bytes (std430 rules); 0 when the members could not be sized.
    std::uint32_t stages{0};     ///< Union of the stages that declare it (AbiStage bits).
    std::string   type_name{};   ///< The declared block type name (for diagnostics).
    /// The members, in declaration order, each with its own offset - what the serving layer fills BY NAME.
    /// A push block is the one range whose bytes are ASSEMBLED rather than copied from an L1 struct (the
    /// engine's `pc` is the L2 realization of the `VineViewBlock` / `VineDrawBlock` pair), so the names are
    /// what says which value goes where: a member nobody recognizes is refused by name, never zero-filled.
    std::vector<AbiPushMember> members{};
};

/** @brief The bindings and push ranges a program's two stages declare FOR ONE VARIANT. */
struct ProgramAbi
{
    std::vector<AbiBinding>  bindings{};        ///< Sorted by (set, binding); one entry per declared binding.
    std::vector<AbiPushRange> pushes{};         ///< Sorted by (offset, size); one entry per distinct range.
    std::vector<std::string> import_defines{};  ///< The names the text allows to be defined, sorted.
};

/**
 * @brief Reads the bindings @p vertex and @p fragment declare under @p defines.
 *
 * The two texts are the ones a program entry carries (see `ProgramFacts::shaders`), and the defines are the
 * VARIANT's - the names the caller intends to compile the pair with. A declaration inside a branch those
 * defines do not take is not part of the answer; a `#define` the text carries itself is.
 *
 * @param vertex  Vertex stage source.
 * @param fragment Fragment stage source.
 * @param defines Names the caller defines for this variant (may be empty).
 * @param out     Receives the facts. Its previous contents are discarded.
 * @return None when the texts were read; Malformed when a conditional expression, a layout value or a
 *         declaration form could not be classified, when the two stages disagree about one binding, or when
 *         an active `#error` says the variant cannot compile.
 */
[[nodiscard]] FactMiss scanProgramAbi(std::string_view vertex, std::string_view fragment,
                                      std::span<const std::string_view> defines, ProgramAbi& out);

/**
 * @brief Gets the L1 block a GLSL block type name names, or Foreign when it names none of them.
 *
 * The type name IS the role (`.ai/design/graphics-shader.md` §11.3 keeps the GLSL block type name and the L1
 * name identical on purpose), which is what lets a program declare the blocks at any (set, binding) it likes
 * and still be served.
 *
 * @param type_name The declared block type name.
 * @return The role, or Foreign when the name is not one of the L1 block names.
 */
[[nodiscard]] AbiBlockRole blockRoleOf(std::string_view type_name) noexcept;

/**
 * @brief Gets a role's name, for diagnostics and evidence.
 *
 * @param role Role to name.
 * @return A static string; never null.
 */
[[nodiscard]] const char* abiBlockRoleName(AbiBlockRole role) noexcept;

VN_VSG_NS_END
